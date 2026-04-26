#include "player.h"
#include "log.h"
#include <cstring>
#include <cerrno>
#include <vector>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

// ─── ytcui-dl backend ──────────────────────────────────────────────────────────
#ifdef USE_YTCUIDL
#include "ytcui-dl/ytfast.h"

// Extract video ID from a full YouTube URL or bare 11-char ID
static std::string extract_video_id(const std::string& url) {
    if (url.size() == 11 && url.find('/') == std::string::npos
        && url.find('.') == std::string::npos)
        return url;
    auto vp = url.find("v=");
    if (vp != std::string::npos) {
        auto id = url.substr(vp + 2, 11);
        if (id.size() == 11) return id;
    }
    auto bp = url.find("youtu.be/");
    if (bp != std::string::npos) return url.substr(bp + 9, 11);
    return url;
}
#endif  // USE_YTCUIDL

namespace ytui {

Player::Player() {
    death_pipe_[0] = -1;
    death_pipe_[1] = -1;
}

Player::~Player() { stop(); }

bool Player::is_available() {
    return system("which mpv > /dev/null 2>&1") == 0;
}

void Player::play(const std::string& url, const std::string& title, PlayMode mode) {
    stop();
    if (mode == PlayMode::Video)
        play_direct(url, title);
    else
        play_piped(url, title, mode);
}

void Player::stop() {
    kill_mpv();
    playing_ = false;
    paused_  = false;
    current_title_.clear();
}

void Player::close_death_pipe() {}

bool Player::toggle_pause() {
    if (!playing_ || mpv_pid_ <= 0) return false;
    if (paused_) {
        kill(-mpv_pid_, SIGCONT);
        paused_ = false;
        Log::write("Resumed pgid -%d", mpv_pid_);
    } else {
        kill(-mpv_pid_, SIGSTOP);
        paused_ = true;
        Log::write("Paused pgid -%d", mpv_pid_);
    }
    return paused_;
}

bool Player::is_playing() const {
    if (!playing_ || mpv_pid_ <= 0) return false;
    int status = 0;
    pid_t r = waitpid(mpv_pid_, &status, WNOHANG);
    if (r == mpv_pid_) {
        const_cast<Player*>(this)->playing_ = false;
        const_cast<Player*>(this)->mpv_pid_ = -1;
        return false;
    }
    if (r < 0) {
        const_cast<Player*>(this)->playing_ = false;
        const_cast<Player*>(this)->mpv_pid_ = -1;
        return false;
    }
    if (kill(-mpv_pid_, 0) < 0 && errno == ESRCH) {
        waitpid(mpv_pid_, &status, WNOHANG);
        const_cast<Player*>(this)->playing_ = false;
        const_cast<Player*>(this)->mpv_pid_ = -1;
        return false;
    }
    return true;
}

std::string Player::now_playing() const {
    if (is_playing()) return current_title_;
    return "";
}

// ─── helpers ──────────────────────────────────────────────────────────────────

static void child_setup(bool log_to_file) {
    setpgid(0, 0);
#if defined(__linux__)
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        if (!log_to_file) dup2(devnull, STDERR_FILENO);
        close(devnull);
    }
    if (log_to_file) {
        std::string el = Log::get_log_dir() + "/mpv.log";
        int ef = open(el.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (ef >= 0) { dup2(ef, STDERR_FILENO); close(ef); }
    }
}

static std::string vol_flag(int v) {
    char buf[32]; snprintf(buf, sizeof(buf), "--volume=%d", v); return buf;
}

// Exec mpv with an args vector, fork-safe. Returns false on exec failure.
static bool spawn_mpv(const std::vector<std::string>& args, pid_t& out_pid, bool log_to_file) {
    std::vector<const char*> argv;
    for (auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    int exec_pipe[2];
    if (pipe(exec_pipe) < 0) {
        Log::write("exec pipe failed: %s", strerror(errno));
        return false;
    }
    fcntl(exec_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid == 0) {
        close(exec_pipe[0]);
        child_setup(log_to_file);
        execvp("mpv", (char* const*)argv.data());
        int err = errno;
        ssize_t w = write(exec_pipe[1], &err, sizeof(err)); (void)w;
        _exit(127);
    } else if (pid > 0) {
        close(exec_pipe[1]);
        int child_errno = 0;
        ssize_t n = read(exec_pipe[0], &child_errno, sizeof(child_errno));
        close(exec_pipe[0]);
        if (n > 0) {
            Log::write("mpv exec failed: %s", strerror(child_errno));
            waitpid(pid, nullptr, 0);
            return false;
        }
        usleep(10000);
        out_pid = pid;
        return true;
    } else {
        close(exec_pipe[0]); close(exec_pipe[1]);
        Log::write("fork failed: %s", strerror(errno));
        return false;
    }
}

// ─── StreamURLs ───────────────────────────────────────────────────────────────

struct StreamURLs {
    std::string video_url;   // muxed (video+audio) or video-only stream URL
    std::string audio_url;   // only set when video_url is video-only adaptive
    bool ok = false;
};

// ═════════════════════════════════════════════════════════════════════════════
// ytcui-dl resolve_stream_urls
//
// ytcui-dl ALWAYS fetches the full muxed stream (video + audio in one URL).
// This is deliberate — audio-only DASH streams get 403'd by YouTube CDN for
// non-browser clients. Muxed progressive streams work 100% of the time.
//
// Audio mode: pass muxed URL to mpv with --no-video → audio-only playback.
// Video mode: pass muxed URL to mpv normally → full video+audio playback.
//
// select_best_video_stream() prefers muxed first, then falls back to
// video-only adaptive only if no muxed stream exists (very rare).
// In the fallback case we also supply a muxed URL as --audio-file.
// ═════════════════════════════════════════════════════════════════════════════

#ifdef USE_YTCUIDL

static StreamURLs resolve_stream_urls(const std::string& youtube_url) {
    StreamURLs result;
    std::string video_id = extract_video_id(youtube_url);
    Log::write("[ytcui-dl] resolve id=%s", video_id.c_str());

    try {
        // get_stream_formats() uses the URL cache — if search already prefetched
        // this video, this call is a hashmap lookup (~0.005ms). Otherwise ~100ms.
        auto info = ytfast::InnertubeClient::get_instance().get_stream_formats(video_id);

        if (info.formats.empty()) {
            Log::write("[ytcui-dl] resolve: no formats returned");
            return result;
        }

        // Prefer muxed stream (video+audio in one progressive download).
        // select_best_video_stream() returns muxed first, adaptive video-only
        // only as a last resort.
        std::string video_url = ytfast::InnertubeClient::select_best_video_stream(
            info.formats, 1080);

        if (video_url.empty()) {
            Log::write("[ytcui-dl] resolve: no usable video stream");
            return result;
        }

        result.video_url = video_url;

        // If mpv got a video-only adaptive URL (rare fallback), also provide
        // a muxed stream as the audio source. mpv's --audio-file plays the
        // audio track from that URL while discarding the video track.
        bool is_muxed = ytfast::InnertubeClient::is_muxed(info.formats, video_url);
        if (!is_muxed) {
            // Muxed stream as audio source — guaranteed to have audio,
            // guaranteed to work (no DASH 403 risk).
            result.audio_url = ytfast::InnertubeClient::select_best_audio_stream(
                info.formats);
            Log::write("[ytcui-dl] resolve: video-only adaptive + muxed-audio fallback");
        } else {
            Log::write("[ytcui-dl] resolve: muxed stream (video+audio)");
        }

        Log::write("[ytcui-dl] resolve: video=%.80s...", video_url.c_str());
        result.ok = true;

    } catch (const std::exception& e) {
        Log::write("[ytcui-dl] resolve error: %s", e.what());
    }

    return result;
}

#else  // yt-dlp backend

static StreamURLs resolve_stream_urls(const std::string& youtube_url) {
    StreamURLs result;
    std::string cmd =
        "yt-dlp -g --no-warnings --no-playlist "
        "-f 'bestvideo[height<=1080]+bestaudio/best[height<=1080]/best' "
        "'" + youtube_url + "' 2>/dev/null";

    Log::write("resolve: %s", cmd.c_str());
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { Log::write("resolve: popen failed: %s", strerror(errno)); return result; }

    char buf[8192];
    std::string line1, line2;
    if (fgets(buf, sizeof(buf), p)) {
        line1 = buf;
        while (!line1.empty() && (line1.back()=='\n'||line1.back()=='\r')) line1.pop_back();
    }
    if (fgets(buf, sizeof(buf), p)) {
        line2 = buf;
        while (!line2.empty() && (line2.back()=='\n'||line2.back()=='\r')) line2.pop_back();
    }
    pclose(p);

    if (line1.empty()) { Log::write("resolve: no output"); return result; }

    if (!line2.empty()) {
        result.video_url = line1;
        result.audio_url = line2;
        Log::write("resolve: video=%.80s...", line1.c_str());
        Log::write("resolve: audio=%.80s...", line2.c_str());
    } else {
        result.video_url = line1;
        Log::write("resolve: combined=%.80s...", line1.c_str());
    }
    result.ok = true;
    return result;
}

#endif  // USE_YTCUIDL

// ─── play_piped: audio-only modes ────────────────────────────────────────────
//
// ytcui-dl path:
//   Resolves the muxed stream URL directly via the InnerTube singleton
//   (cached <0.01ms if search already prefetched it, cold ~100ms).
//   Hands the URL to mpv with --no-video — mpv decodes the video container
//   but only plays the audio track. No yt-dlp process, no piping, no delay.
//
// yt-dlp path:
//   Spawns yt-dlp, pipes its stdout directly into mpv stdin.

void Player::play_piped(const std::string& url, const std::string& title, PlayMode mode) {
    std::string vol = vol_flag(opts_.volume);

#ifdef USE_YTCUIDL

    std::string video_id = extract_video_id(url);
    std::string stream_url;

    try {
        // yt_best_audio() → select_best_audio_stream() → picks the best MUXED
        // stream and returns its stream_url (raw, no &range= param).
        // Never picks audio-only DASH — those 403 on non-browser clients.
        stream_url = ytfast::yt_best_audio(video_id);
        Log::write("[ytcui-dl] play_piped: resolved muxed stream (len=%zu)", stream_url.size());
    } catch (const std::exception& e) {
        Log::write("[ytcui-dl] play_piped: resolve failed: %s", e.what());
        return;
    }

    if (stream_url.empty()) {
        Log::write("[ytcui-dl] play_piped: empty stream url — aborting");
        return;
    }

    // mpv args for audio-only playback from a muxed (video+audio) stream URL.
    // --no-video:    discard video track, play only audio
    // --ytdl=no:     we provide a direct CDN URL, don't let mpv call yt-dlp
    // --user-agent:  Android UA required — YouTube CDN validates it for muxed
    std::vector<std::string> args = {
        "mpv",
        "--no-video",
        "--no-terminal",
        vol,
        "--ytdl=no",
        std::string("--user-agent=") + ytfast::ANDROID_UA,
    };

    if (!opts_.no_cache) {
        args.push_back("--audio-buffer=2");
        args.push_back("--cache=yes");
        args.push_back("--demuxer-max-bytes=50M");
    } else {
        args.push_back("--cache=no");
    }

    args.push_back("--audio-pitch-correction=yes");
    if (opts_.no_hardware_accel) args.push_back("--hwdec=no");
    if (mode == PlayMode::AudioLoop) args.push_back("--loop=inf");

    args.push_back(stream_url);

    Log::write("[ytcui-dl] play_piped: mpv --no-video (muxed stream, direct URL)");

    if (spawn_mpv(args, mpv_pid_, Log::is_logdump())) {
        playing_       = true;
        current_title_ = title;
        Log::write("[ytcui-dl] play_piped pid=%d", mpv_pid_);
    }

#else  // yt-dlp backend: pipe yt-dlp | mpv

    std::string ytdlp_cmd =
        "yt-dlp --no-warnings --no-playlist "
        "-f 'bestaudio[ext=m4a]/bestaudio[ext=webm]/bestaudio' "
        "--audio-quality 0 "
        "-o - '" + url + "'";

    std::string mpv_cmd = "mpv --no-video --no-terminal " + vol;
    if (!opts_.no_cache)
        mpv_cmd += " --audio-buffer=2 --cache=yes --demuxer-max-bytes=50M";
    else
        mpv_cmd += " --cache=no";
    mpv_cmd += " --audio-pitch-correction=yes";
    if (opts_.no_hardware_accel) mpv_cmd += " --hwdec=no";
    if (mode == PlayMode::AudioLoop) mpv_cmd += " --loop=inf";
    mpv_cmd += " -";

    std::string cmd = ytdlp_cmd + " | " + mpv_cmd;
    Log::write("Piped play: %s", cmd.c_str());

    pid_t pid = fork();
    if (pid == 0) {
        child_setup(Log::is_logdump());
        execlp("sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    } else if (pid > 0) {
        usleep(10000);
        mpv_pid_       = pid;
        playing_       = true;
        current_title_ = title;
        Log::write("Piped play pid=%d", pid);
    } else {
        Log::write("fork failed: %s", strerror(errno));
    }

#endif  // USE_YTCUIDL
}

// ─── play_direct: video mode ──────────────────────────────────────────────────
//
// ytcui-dl path:
//   resolve_stream_urls() returns a muxed URL. mpv receives it directly with
//   --ytdl=no and the Android UA. The muxed stream has both video and audio —
//   no --audio-file needed in the common case. Only adds --audio-file if
//   resolve() fell back to video-only adaptive (very rare).
//
// yt-dlp path:
//   yt-dlp resolves bestvideo+bestaudio, gives us two URLs. Falls back to
//   --ytdl=yes if that fails.

void Player::play_direct(const std::string& url, const std::string& title) {
    std::string vol = vol_flag(opts_.volume);
    StreamURLs streams = resolve_stream_urls(url);

    std::vector<std::string> args = {
        "mpv",
        "--force-window=yes",
        "--no-terminal",
        vol,
        "--geometry=854x480",
        "--autofit-larger=70%",
        "--autofit-smaller=640x360",
        "--title=" + title,
    };

    if (streams.ok) {
        args.push_back("--ytdl=no");
        if (!opts_.no_cache) {
            args.push_back("--cache=yes");
            args.push_back("--demuxer-max-bytes=100M");
        } else {
            args.push_back("--cache=no");
        }
        if (opts_.no_hardware_accel) {
            args.push_back("--hwdec=no");
            args.push_back("--vo=libmpv");
        }
#ifdef USE_YTCUIDL
        // Android UA required for CDN access on both muxed and adaptive streams
        args.push_back(std::string("--user-agent=") + ytfast::ANDROID_UA);
#endif
        args.push_back(streams.video_url);
        // audio_url only set when video_url is video-only adaptive (rare fallback)
        if (!streams.audio_url.empty())
            args.push_back("--audio-file=" + streams.audio_url);

        Log::write("Direct play (fast): vol=%d backend=%s",
                   opts_.volume,
#ifdef USE_YTCUIDL
                   "ytcui-dl"
#else
                   "yt-dlp"
#endif
        );
    } else {
        // Fallback: let mpv use its built-in yt-dlp integration
        Log::write("Direct play (slow fallback --ytdl=yes): %s", url.c_str());
        args.push_back("--ytdl=yes");
        args.push_back("--ytdl-format=bestvideo[height<=1080]+bestaudio/best[height<=1080]/best");
        if (!opts_.no_cache) {
            args.push_back("--cache=yes");
            args.push_back("--demuxer-max-bytes=100M");
        } else {
            args.push_back("--cache=no");
        }
        if (opts_.no_hardware_accel) {
            args.push_back("--hwdec=no");
            args.push_back("--vo=libmpv");
        }
        args.push_back(url);
    }

    if (spawn_mpv(args, mpv_pid_, Log::is_logdump())) {
        playing_       = true;
        current_title_ = title;
        Log::write("Direct play pid=%d", mpv_pid_);
    }
}

void Player::play_xdg(const std::string& url, const std::string& title) {
    Log::write("xdg/open: %s", url.c_str());
    pid_t pid = fork();
    if (pid == 0) {
        child_setup(false);
#if defined(__APPLE__) && defined(__MACH__)
        execlp("open", "open", url.c_str(), nullptr);
#else
        execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
#endif
        _exit(127);
    } else if (pid > 0) {
        usleep(10000);
        mpv_pid_       = pid;
        playing_       = true;
        current_title_ = title;
    } else {
        Log::write("fork failed: %s", strerror(errno));
    }
}

void Player::kill_mpv() {
    if (mpv_pid_ <= 0) return;
    Log::write("Killing pgid -%d", mpv_pid_);
    kill(-mpv_pid_, SIGTERM);
    int status;
    pid_t r = waitpid(mpv_pid_, &status, WNOHANG);
    if (r == 0) {
        usleep(300000);
        r = waitpid(mpv_pid_, &status, WNOHANG);
        if (r == 0) {
            kill(-mpv_pid_, SIGKILL);
            waitpid(mpv_pid_, &status, 0);
        }
    }
    mpv_pid_ = -1;
}

} // namespace ytui
