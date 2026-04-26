#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// compat.h — Cross-platform compatibility layer for ytcui
//
// Handles differences between Linux, FreeBSD, and macOS for:
//   - Parent death signal (prctl / procctl / kqueue+pipe)
//   - pipe2() replacement for macOS
//   - Platform detection macros
//   - macOS version detection at runtime (for < 11 fallback)
//
// ROOT CAUSE FIX (macOS < 11 / Catalina and below):
//   EVFILT_PROC + NOTE_EXIT in kqueue requires that the watching process
//   either spawned the target OR is root. A grandchild watching a grandparent
//   PID (ytcui) fails silently with EPERM or EV_ERROR on macOS < 11.
//   macOS 11 (Big Sur) relaxed this to allow same-UID monitoring.
//
//   FIX: Always use pipe-based watchdog on macOS (all versions).
//   The pipe needs zero permissions — ytcui holds the write end, watchdog
//   blocks on read, EOF fires when ytcui dies. Works on macOS 10.x+, BSD, all.
//   kqueue EVFILT_PROC path is permanently disabled for macOS to prevent the
//   silent-permission-failure footgun.
// ═══════════════════════════════════════════════════════════════════════════════

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// ─── Platform detection ────────────────────────────────────────────────────────

#if defined(__linux__)
    #define YTUI_LINUX 1
    #include <sys/prctl.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    #define YTUI_FREEBSD 1
    #include <sys/procctl.h>
#elif defined(__APPLE__) && defined(__MACH__)
    #define YTUI_MACOS 1
    #include <sys/sysctl.h>
    // sys/event.h kept for completeness but EVFILT_PROC is NOT used (see above)
    #include <sys/event.h>
#elif defined(__NetBSD__) || defined(__OpenBSD__)
    #define YTUI_BSD_GENERIC 1
#endif

namespace ytui {
namespace compat {

// ─── macOS runtime version detection ──────────────────────────────────────────
// Returns macOS major version (e.g. 10 for 10.15, 11 for 11.0, 14 for 14.x).
// Returns 0 if not macOS or detection fails. Cached after first call.

#if defined(YTUI_MACOS)
inline int macos_major_version() {
    static int cached = -1;
    if (cached >= 0) return cached;
    char buf[64] = {0};
    size_t len = sizeof(buf);
    if (sysctlbyname("kern.osproductversion", buf, &len, nullptr, 0) == 0) {
        cached = atoi(buf);
    } else {
        cached = 0;
    }
    return cached;
}
#endif

// ─── Portable pipe creation with close-on-exec ─────────────────────────────────
// pipe2(O_CLOEXEC) is Linux/FreeBSD only. macOS uses pipe() + fcntl().

inline int pipe_cloexec(int pipefd[2]) {
#if defined(YTUI_LINUX) || defined(YTUI_FREEBSD)
    return pipe2(pipefd, O_CLOEXEC);
#else
    if (pipe(pipefd) < 0) return -1;
    if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) < 0) {
        int saved = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        errno = saved;
        return -1;
    }
    return 0;
#endif
}

// ─── Set parent death signal (Linux/FreeBSD only) ──────────────────────────────

inline bool set_pdeathsig(int sig) {
#if defined(YTUI_LINUX)
    return prctl(PR_SET_PDEATHSIG, sig) == 0;
#elif defined(YTUI_FREEBSD)
    return procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &sig) == 0;
#else
    (void)sig;
    return false;
#endif
}

inline bool has_native_pdeathsig() {
#if defined(YTUI_LINUX) || defined(YTUI_FREEBSD)
    return true;
#else
    return false;
#endif
}

// ─── Pipe-based watchdog inner loop ───────────────────────────────────────────
// Blocks on read(). When write end closes (parent died), sends SIGTERM+SIGKILL
// to the entire process group. Zero permissions required.

inline void pipe_watchdog_loop(int read_fd) {
    char buf;
    ssize_t n;
    do { n = read(read_fd, &buf, 1); } while (n < 0 && errno == EINTR);
    close(read_fd);
    kill(0, SIGTERM);
    usleep(200000);
    kill(0, SIGKILL);
    _exit(0);
}

// ─── Fork a watchdog process for parent death detection ───────────────────────
// Used on macOS (ALL versions) and generic BSDs.
// Call this AFTER setpgid(0,0) in the child.

inline void fork_death_watchdog(int death_pipe_read_fd, pid_t ytcui_pid) {
    (void)ytcui_pid;

    pid_t watchdog_pid = fork();
    if (watchdog_pid == 0) {
        pipe_watchdog_loop(death_pipe_read_fd);
        _exit(0);
    }
    if (death_pipe_read_fd >= 0) close(death_pipe_read_fd);
    (void)watchdog_pid;
}

// ─── Debug: dump platform info ────────────────────────────────────────────────

inline void dump_platform_info(int fd) {
    char buf[512];
    int n = 0;
#if defined(YTUI_LINUX)
    n = snprintf(buf, sizeof(buf),
        "  platform       : Linux\n"
        "  pdeathsig      : prctl(PR_SET_PDEATHSIG) [kernel-native]\n"
        "  pipe2          : native\n"
        "  watchdog       : not needed\n");
#elif defined(YTUI_FREEBSD)
    n = snprintf(buf, sizeof(buf),
        "  platform       : FreeBSD/DragonFly\n"
        "  pdeathsig      : procctl(PROC_PDEATHSIG_CTL) [kernel-native]\n"
        "  pipe2          : native\n"
        "  watchdog       : not needed\n");
#elif defined(YTUI_MACOS)
    int v = macos_major_version();
    n = snprintf(buf, sizeof(buf),
        "  platform       : macOS\n"
        "  macos_major    : %d\n"
        "  pdeathsig      : unavailable on macOS\n"
        "  pipe2          : emulated (pipe+fcntl FD_CLOEXEC)\n"
        "  watchdog       : pipe-based [all macOS versions]\n"
        "  kqueue_note    : EVFILT_PROC disabled (EPERM on macOS<11 for grandchild->grandparent)\n",
        v);
#elif defined(YTUI_BSD_GENERIC)
    n = snprintf(buf, sizeof(buf),
        "  platform       : NetBSD/OpenBSD\n"
        "  pdeathsig      : unavailable\n"
        "  pipe2          : emulated\n"
        "  watchdog       : pipe-based\n");
#else
    n = snprintf(buf, sizeof(buf), "  platform       : UNKNOWN\n");
#endif
    if (n > 0) { ssize_t w = write(fd, buf, (size_t)n); (void)w; }
}

} // namespace compat
} // namespace ytui
