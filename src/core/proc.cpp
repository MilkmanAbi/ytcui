#include "core/proc.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#  include <mach-o/dyld.h>   // _NSGetExecutablePath
#elif defined(__FreeBSD__)
#  include <sys/sysctl.h>    // KERN_PROC_PATHNAME
#  include <sys/types.h>
#endif

namespace ytcui::proc {
namespace {

int64_t now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               clock::now().time_since_epoch()).count();
}

}  // namespace

std::string which(const std::string& program) {
    if (program.empty()) return {};
    if (program.find('/') != std::string::npos) {
        struct stat st{};
        if (::stat(program.c_str(), &st) == 0 && (st.st_mode & S_IXUSR))
            return program;
        return {};
    }
    const char* path = std::getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    std::string p(path);
    std::size_t start = 0;
    while (start <= p.size()) {
        const std::size_t colon = p.find(':', start);
        const std::string dir =
            p.substr(start, colon == std::string::npos ? std::string::npos
                                                       : colon - start);
        if (!dir.empty()) {
            const std::string full = dir + "/" + program;
            struct stat st{};
            if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                (st.st_mode & S_IXUSR))
                return full;
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return {};
}

std::vector<std::string> which_all(const std::string& program) {
    std::vector<std::string> out;
    if (program.empty()) return out;
    if (program.find('/') != std::string::npos) {
        if (!which(program).empty()) out.push_back(program);
        return out;
    }
    const char* path = std::getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    std::string p(path);
    std::size_t start = 0;
    while (start <= p.size()) {
        const std::size_t colon = p.find(':', start);
        const std::string dir =
            p.substr(start, colon == std::string::npos ? std::string::npos
                                                       : colon - start);
        if (!dir.empty()) {
            const std::string full = dir + "/" + program;
            struct stat st{};
            if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                (st.st_mode & S_IXUSR)) {
                // /bin is usually a symlink to /usr/bin, so the same binary
                // turns up twice; comparing device+inode catches that where a
                // string compare would not.
                bool dup = false;
                for (const std::string& seen : out) {
                    struct stat s2{};
                    if (::stat(seen.c_str(), &s2) == 0 &&
                        s2.st_dev == st.st_dev && s2.st_ino == st.st_ino) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) out.push_back(full);
            }
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return out;
}

bool exists(const std::string& program) { return !which(program).empty(); }

Result run(const std::vector<std::string>& argv, int timeout_ms,
           std::size_t max_output) {
    Result r;
    if (argv.empty()) return r;

    int outp[2], errp[2];
    if (::pipe(outp) != 0) return r;
    if (::pipe(errp) != 0) { ::close(outp[0]); ::close(outp[1]); return r; }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(outp[0]); ::close(outp[1]);
        ::close(errp[0]); ::close(errp[1]);
        return r;
    }
    if (pid == 0) {
        ::close(outp[0]);
        ::close(errp[0]);
        ::dup2(outp[1], STDOUT_FILENO);
        ::dup2(errp[1], STDERR_FILENO);
        ::close(outp[1]);
        ::close(errp[1]);
        // No stdin: a child that decides to prompt must fail rather than wait
        // forever behind a TUI that cannot show the prompt.
        const int null = ::open("/dev/null", O_RDONLY);
        if (null >= 0) { ::dup2(null, STDIN_FILENO); ::close(null); }
        // Its own process group, so the timeout below can kill the whole tree
        // -- yt-dlp spawns ffmpeg and friends.
        ::setpgid(0, 0);

        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        ::_exit(127);                       // execvp failed: not on PATH
    }

    ::setpgid(pid, pid); // close the race before a very short timeout
    ::close(outp[1]);
    ::close(errp[1]);
    // Non-blocking, so one stream filling up cannot stall the read of the
    // other and deadlock against a child waiting to write.
    ::fcntl(outp[0], F_SETFL, O_NONBLOCK);
    ::fcntl(errp[0], F_SETFL, O_NONBLOCK);

    const int64_t deadline = timeout_ms > 0 ? now_ms() + timeout_ms : 0;
    bool out_open = true, err_open = true, reaped = false;
    int st = 0;

    while (out_open || err_open || !reaped) {
        if (!reaped) {
            pid_t result = ::waitpid(pid, &st, WNOHANG);
            if (result == pid || (result < 0 && errno == ECHILD)) reaped = true;
        }
        if (!out_open && !err_open && reaped) break;
        pollfd fds[2];
        int n = 0;
        if (out_open) fds[n++] = pollfd{outp[0], POLLIN, 0};
        if (err_open) fds[n++] = pollfd{errp[0], POLLIN, 0};

        int wait_ms = 20;
        if (deadline) {
            const int64_t left = deadline - now_ms();
            if (left <= 0) { r.timed_out = true; break; }
            if (left < wait_ms) wait_ms = (int)left;
        }

        const int pr = ::poll(fds, (nfds_t)n, wait_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;              // just a tick; re-check the clock

        for (int i = 0; i < n; ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const bool is_out = fds[i].fd == outp[0];
            char buf[16384];
            for (;;) {
                const ssize_t got = ::read(fds[i].fd, buf, sizeof buf);
                if (got > 0) {
                    std::string& dst = is_out ? r.out : r.err;
                    // Remote data decides this size, so it is capped.
                    if (dst.size() < max_output)
                        dst.append(buf, (std::size_t)std::min<std::size_t>(
                                            (std::size_t)got, max_output - dst.size()));
                    if (deadline && now_ms() >= deadline) break;
                    continue;
                }
                if (got == 0) {
                    if (is_out) out_open = false; else err_open = false;
                    break;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                if (is_out) out_open = false; else err_open = false;
                break;
            }
        }
    }

    ::close(outp[0]);
    ::close(errp[0]);

    if (r.timed_out) {
        // The whole group: yt-dlp's children would otherwise outlive it.
        ::kill(-pid, SIGKILL);
        if (!reaped) while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        r.ran = true;
        return r;
    }

    if (!reaped) {
        ::kill(-pid, SIGKILL);
        while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    }
    if (WIFEXITED(st)) {
        r.exit_code = WEXITSTATUS(st);
        // 127 is execvp's "not found", not something the program reported.
        r.ran = r.exit_code != 127;
    } else if (WIFSIGNALED(st)) {
        r.exit_code = 128 + WTERMSIG(st);
        r.ran = true;
    }
    return r;
}

std::string self_exe() {
    char buf[4096] = {};
#if defined(__linux__)
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) return std::string(buf, static_cast<std::size_t>(n));
#elif defined(__APPLE__)
    uint32_t size = static_cast<uint32_t>(sizeof(buf));
    if (_NSGetExecutablePath(buf, &size) == 0) return std::string(buf);
#elif defined(__FreeBSD__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    std::size_t size = sizeof(buf);
    if (::sysctl(mib, 4, buf, &size, nullptr, 0) == 0 && size > 1)
        return std::string(buf);
#elif defined(__NetBSD__)
    const ssize_t n = ::readlink("/proc/curproc/exe", buf, sizeof(buf) - 1);
    if (n > 0) return std::string(buf, static_cast<std::size_t>(n));
#endif
    // OpenBSD has no /proc by default; unknown platforms fall through to "".
    (void)buf;
    return {};
}

}  // namespace ytcui::proc
