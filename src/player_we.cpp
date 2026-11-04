// player_we.cpp — subprocess adapter for the ytcui-we backend.
//
// ODR isolation: this file does NOT include any ytcui-we headers.
// ytcui-we (with its own yj.h) is a separate executable; we communicate
// with it via its --mpv-args interface. Only ytcui-dl's yj.h is used here
// to parse the JSON array output.
#include "player_we.h"
#include "log.h"

#include "ytcui-dl/yj.h"  // ytcui-dl's yj — NOT ytcui-we's

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ytui {

// ── subprocess capture ────────────────────────────────────────────────────────

struct ProcOut { int code; std::string out; std::string err; };

static ProcOut run_capture(const std::vector<std::string>& args) {
    if (args.empty()) throw std::runtime_error("empty command");

    int out_pipe[2], err_pipe[2];
    if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0)
        throw std::runtime_error("pipe: " + std::string(strerror(errno)));

    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        ::close(err_pipe[0]); ::close(err_pipe[1]);
        throw std::runtime_error("fork: " + std::string(strerror(errno)));
    }
    if (pid == 0) {
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        ::close(err_pipe[0]); ::close(err_pipe[1]);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }

    ::close(out_pipe[1]);
    ::close(err_pipe[1]);

    auto drain = [](int fd) {
        std::string buf;
        char tmp[4096];
        ssize_t n;
        while ((n = ::read(fd, tmp, sizeof tmp)) > 0)
            buf.append(tmp, n);
        ::close(fd);
        return buf;
    };

    // Simple sequential drain (stdout then stderr); works for short outputs.
    // For long stderr interleaved with stdout a select() loop would be needed,
    // but --mpv-json emits a single compact JSON line + newline on stdout.
    ProcOut result;
    result.out = drain(out_pipe[0]);
    result.err = drain(err_pipe[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) break;
    result.code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return result;
}

// ── binary discovery ──────────────────────────────────────────────────────────

std::string find_ytcuiwe_binary() {
    // 1. Same directory as the running ytcui executable (/proc/self/exe).
    char self_buf[4096] = {};
    ssize_t n = ::readlink("/proc/self/exe", self_buf, sizeof(self_buf) - 1);
    if (n > 0) {
        auto sibling = std::filesystem::path(self_buf).parent_path() / "ytcui-we";
        if (std::filesystem::exists(sibling))
            return sibling.string();
    }

    // 2. $PATH — let execvp locate it.
    // Quick existence check using PATH search.
    auto check = run_capture({"which", "ytcui-we"});
    if (check.code == 0 && !check.out.empty()) {
        auto path = check.out;
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
            path.pop_back();
        if (!path.empty()) return path;
    }

    return {};
}

// ── JSON parser for mpv-json array ────────────────────────────────────────────
// Output format: ["mpv","--ytdl=no","--force-media-title=...","--user-agent=UA",
//                 "VIDEO_URL","--audio-file=AUDIO_URL"]
static WeStreamInfo parse_mpv_json(const std::string& json) {
    // Strip trailing whitespace/newline
    std::string j = json;
    while (!j.empty() && (j.back() == '\n' || j.back() == '\r' || j.back() == ' '))
        j.pop_back();

    if (j.empty() || j.front() != '[')
        throw std::runtime_error("ytcui-we --mpv-json: unexpected output: " + j.substr(0, 80));

    auto root = yj::parse(j);
    if (!root.is_arr())
        throw std::runtime_error("ytcui-we --mpv-json: not a JSON array");

    WeStreamInfo info;
    yj::each_elem(root, [&](yj::Val v) {
        std::string s = yj::unescape(v.raw());
        if (s.rfind("--user-agent=", 0) == 0) {
            info.user_agent = s.substr(13);
        } else if (s.rfind("--audio-file=", 0) == 0) {
            info.audio_url = s.substr(13);
        } else if (s.rfind("--", 0) != 0 && s != "mpv") {
            // Bare argument (not a flag) = URL or file path
            if (info.video_url.empty())
                info.video_url = s;
        }
        return true;
    });

    // Detect live / HLS manifest (no audio track, URL contains m3u8 or is a local path)
    if (info.audio_url.empty()) {
        const auto& u = info.video_url;
        if (u.find(".m3u8") != std::string::npos ||
            u.find("manifest") != std::string::npos ||
            (!u.empty() && u.front() == '/'))
            info.is_live = true;
    }

    if (info.video_url.empty())
        throw std::runtime_error("ytcui-we --mpv-json: no URL in output");

    return info;
}

// ── public resolver ────────────────────────────────────────────────────────────

WeStreamInfo we_resolve(const std::string& youtube_url,
                        int preferred_height,
                        const std::string& cookies_file) {
    auto binary = find_ytcuiwe_binary();
    if (binary.empty())
        throw std::runtime_error(
            "ytcui-we executable not found — install ytcui-we or build with "
            "CURL support (cmake -DCMAKE_BUILD_TYPE=Release .)");

    Log::write("[ytcui-we] resolver: %s", binary.c_str());

    std::vector<std::string> args{binary, "--mpv-args"};
    if (preferred_height > 0)
        args.insert(args.end(), {"-q", std::to_string(preferred_height)});
    if (!cookies_file.empty())
        args.insert(args.end(), {"--cookies", cookies_file});
    args.push_back(youtube_url);

    std::string invocation;
    for (auto& a : args) { invocation += a; invocation += ' '; }
    Log::write("[ytcui-we] invoking: %s", invocation.c_str());

    auto result = run_capture(args);

    if (result.code != 0) {
        // Log stderr for diagnosis
        if (!result.err.empty())
            Log::write("[ytcui-we] stderr: %s", result.err.substr(0, 400).c_str());
        throw std::runtime_error(
            "ytcui-we exited " + std::to_string(result.code) +
            (result.err.empty() ? "" : ": " + result.err.substr(0, 200)));
    }

    Log::write("[ytcui-we] mpv-json: %s", result.out.substr(0, 200).c_str());
    auto info = parse_mpv_json(result.out);
    Log::write("[ytcui-we] resolved video=%s audio=%s live=%s",
               info.video_url.substr(0, 60).c_str(),
               info.audio_url.empty() ? "(muxed)" : info.audio_url.substr(0, 60).c_str(),
               info.is_live ? "yes" : "no");
    return info;
}

} // namespace ytui
