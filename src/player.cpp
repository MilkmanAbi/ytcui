#include "player.h"
#include "log.h"
#include <cstring>
#include <algorithm>
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
    if (ipc_.connected()) ipc_.quit();   // best-effort clean exit
    ipc_.disconnect();
    ipc_.cleanup();
    kill_mpv();
    playing_ = false;
    paused_  = false;
    current_title_.clear();
}

void Player::close_death_pipe() {}

bool Player::toggle_pause() {
    if (!playing_ || mpv_pid_ <= 0) return false;
    if (ipc_.connected() && ipc_.toggle_pause()) {
        paused_ = !paused_;   // optimistic; pump() will confirm from mpv
        Log::write("IPC %s pid=%d", paused_ ? "paused" : "resumed", mpv_pid_);
    } else {
        // Fallback: SIGSTOP/SIGCONT (laggy but works without IPC)
        if (paused_) {
            kill(-mpv_pid_, SIGCONT);
            paused_ = false;
        } else {
            kill(-mpv_pid_, SIGSTOP);
            paused_ = true;
        }
        Log::write("Signal %s pgid -%d (IPC unavailable)", paused_ ? "paused" : "resumed", mpv_pid_);
    }
    return paused_;
}

bool Player::volume_up(int step) {
    current_volume_ = std::min(150, current_volume_ + step);
    if (ipc_.connected()) return ipc_.adjust_volume(step);
    return false;
}

bool Player::volume_down(int step) {
    current_volume_ = std::max(0, current_volume_ - step);
    if (ipc_.connected()) return ipc_.adjust_volume(-step);
    return false;
}

bool Player::set_volume(int vol) {
    current_volume_ = std::clamp(vol, 0, 150);
    if (ipc_.connected()) return ipc_.set_volume(current_volume_);
    return false;
}

int Player::get_volume() const {
    // Prefer mpv's reported volume when we have it; else our local shadow.
    if (ipc_.connected() && ipc_.volume() >= 0) return ipc_.volume();
    return current_volume_;
}

void Player::tick() {
    if (!playing_) return;
    if (!ipc_.connected()) ipc_.try_connect();  // one cheap attempt per frame
    ipc_.pump();                                 // drain events, refresh cache
}

// Seeks go through an absolute, position-aware target rather than a bare
// "relative" command. Two reasons: (1) it's deterministic — a relative seek
// fired again before mpv has finished the previous one (a real risk on a
// network stream, where a seek means re-requesting a byte range from the
// CDN and can take noticeably longer than a local file) can't drift or
// compound, since each press recomputes the target from mpv's own
// last-known position; (2) it lets us clamp to [0, duration] here instead of
// however mpv's own "relative" clamping happens to behave at the ends of
// the stream. Falls back to a relative nudge only if we don't have a cached
// position yet (e.g. right after playback starts, before mpv's first
// observe_property push has arrived).
bool Player::seek_forward(double secs) {
    if (!ipc_.connected()) return false;
    double pos = ipc_.position();
    if (pos < 0) return ipc_.seek(secs);
    double dur = ipc_.duration();
    double target = pos + secs;
    if (dur > 0 && target > dur) target = dur;
    return ipc_.seek_absolute(target);
}

bool Player::seek_backward(double secs) {
    if (!ipc_.connected()) return false;
    double pos = ipc_.position();
    if (pos < 0) return ipc_.seek(-secs);
    return ipc_.seek_absolute(pos - secs);
}

// Used by the streamlined-mode waveform click-to-seek: secs is already an
// absolute target (computed from the click's fraction across the bar), just
// clamp to the known duration.
bool Player::seek_to(double secs) {
    if (!ipc_.connected()) return false;
    double dur = ipc_.duration();
    double target = secs;
    if (target < 0) target = 0;
    if (dur > 0 && target > dur) target = dur;
    return ipc_.seek_absolute(target);
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
static bool spawn_mpv(const std::vector<std::string>& args, pid_t& out_pid, bool log_to_file, const std::string& ipc_sock = "") {
    // Inject the IPC socket flag just after argv[0] ("mpv"), BEFORE any
    // positional stream URL. Options placed after the URL can be misparsed
    // by some mpv builds, which is enough to make playback fail to start.
    std::vector<std::string> full_args;
    full_args.reserve(args.size() + 1);
    bool injected = false;
    for (size_t i = 0; i < args.size(); ++i) {
        full_args.push_back(args[i]);
        if (i == 0 && !ipc_sock.empty()) {          // right after "mpv"
            full_args.push_back("--input-ipc-server=" + ipc_sock);
            injected = true;
        }
    }
    if (!injected && !ipc_sock.empty())
        full_args.insert(full_args.begin() + (full_args.empty() ? 0 : 1),
                         "--input-ipc-server=" + ipc_sock);
    std::vector<const char*> argv;
    for (auto& a : full_args) argv.push_back(a.c_str());
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
    std::string user_agent;  // client UA that signed video_url/audio_url (ytcui-dl backend)
    bool ok = false;
};

// ═════════════════════════════════════════════════════════════════════════════
// ytcui-dl resolve_stream_urls
//
// v2 flips this from v1: select_best_video_stream() now prefers the best
// ADAPTIVE video-only track (up to 1080p+), falling back to a muxed
// progressive stream (capped around 360p) only if no adaptive video exists.
// The old "always force muxed" behavior was working around a misdiagnosis —
// v1's client returned SABR responses whose adaptive formats carry no URL at
// all, which looked like "adaptive gets 403'd" but wasn't; v2's client chain
// (VISIONOS/ANDROID_VR/ANDROID/IOS) gets real, fetchable adaptive URLs, so
// forcing muxed was silently capping every video's quality at 360p.
//
// Video mode: adaptive video-only URL to mpv, muxed audio (or a separate
// adaptive audio track) supplied via --audio-file when video_url has no
// audio of its own.
// Audio mode: play_piped() resolves audio independently (see below) — it
// does not call this function.
//
// is_muxed() tells us whether video_url already carries audio; if not, we
// fetch a separate audio_url and mpv muxes them live via --audio-file.
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

        // A format URL is signed for the specific InnerTube client that
        // returned it (VISIONOS / ANDROID_VR / ANDROID / IOS — v2 tries them
        // in a chain and the winner varies per video), so mpv must present
        // that exact client's User-Agent when it fetches the URL or the CDN
        // 403s it. There is no single hardcoded UA that works for every
        // video any more (the old ytfast::ANDROID_UA constant is gone).
        result.user_agent = info.client_ua ? info.client_ua : "";

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
    std::string stream_url, user_agent;

    try {
        // get_stream_formats() uses the same URL cache as resolve_stream_urls()
        // (a hashmap lookup if search/prefetch already warmed it). We need the
        // full VideoInfo, not just yt_best_audio()'s bare URL, because the
        // format URL is signed for one specific InnerTube client and mpv must
        // present that exact client's User-Agent (info.client_ua) or the CDN
        // 403s it — there is no single hardcoded UA that works for every video.
        //
        // select_best_audio_stream() now prefers the best adaptive audio-only
        // track (higher quality than the old muxed-only fallback): v2's
        // client chain (VISIONOS/ANDROID_VR/ANDROID/IOS) returns real,
        // fetchable adaptive URLs — the old "adaptive audio 403s" belief was a
        // misdiagnosis of SABR responses carrying no URL at all, not of
        // adaptive audio being blocked. It still falls back to muxed
        // automatically if no adaptive audio track exists for this video.
        auto info = ytfast::InnertubeClient::get_instance().get_stream_formats(video_id);
        stream_url = ytfast::InnertubeClient::select_best_audio_stream(info.formats);
        user_agent = info.client_ua ? info.client_ua : "";
        Log::write("[ytcui-dl] play_piped: resolved audio stream (len=%zu)", stream_url.size());
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
    // --user-agent:  the exact client UA that signed this URL (see above)
    std::vector<std::string> args = {
        "mpv",
        "--no-video",
        "--no-terminal",
        vol,
        "--ytdl=no",
        std::string("--user-agent=") + user_agent,
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

    std::string sock = ipc_.init_socket();
    if (spawn_mpv(args, mpv_pid_, Log::is_logdump(), sock)) {
        playing_       = true;
        current_title_ = title;
        current_volume_ = opts_.volume;
        ipc_.try_connect();   // may not be ready yet; tick() retries each frame
        Log::write("[ytcui-dl] play_piped pid=%d ipc=%s", mpv_pid_, ipc_.connected() ? "ok" : "pending");
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
        current_volume_ = opts_.volume;
        // yt-dlp pipe path: mpv is a child of sh, IPC socket path
        // would need to be injected into the shell command. Skip IPC
        // for the legacy path — SIGSTOP fallback still works.
        Log::write("Piped play pid=%d", pid);
    } else {
        Log::write("fork failed: %s", strerror(errno));
    }

#endif  // USE_YTCUIDL
}

// ─── play_direct: video mode ──────────────────────────────────────────────────
//
// ytcui-dl path:
//   resolve_stream_urls() returns the best adaptive video URL (or a muxed
//   fallback). mpv receives it directly with --ytdl=no and the exact client
//   UA that signed it. --audio-file supplies a separate audio track unless
//   the video URL is already muxed.
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
            // Disable hardware DECODING only. Do NOT set --vo=libmpv: that is
            // mpv's embedding render API, not a standalone video output, and
            // with --force-window it breaks or blanks the video window. The
            // default vo (gpu) is correct; --hwdec=no alone is the right knob.
            args.push_back("--hwdec=no");
        }
#ifdef USE_YTCUIDL
        // The exact client UA that signed streams.video_url/audio_url (see
        // resolve_stream_urls()) — required for CDN access on both muxed and
        // adaptive streams, and no longer a single constant across all videos.
        if (!streams.user_agent.empty())
            args.push_back("--user-agent=" + streams.user_agent);
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
            // Disable hardware DECODING only. Do NOT set --vo=libmpv: that is
            // mpv's embedding render API, not a standalone video output, and
            // with --force-window it breaks or blanks the video window. The
            // default vo (gpu) is correct; --hwdec=no alone is the right knob.
            args.push_back("--hwdec=no");
        }
        args.push_back(url);
    }

    std::string sock = ipc_.init_socket();
    if (spawn_mpv(args, mpv_pid_, Log::is_logdump(), sock)) {
        playing_       = true;
        current_title_ = title;
        current_volume_ = opts_.volume;
        ipc_.try_connect();
        Log::write("Direct play pid=%d ipc=%s", mpv_pid_, ipc_.connected() ? "ok" : "pending");
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

double Player::get_position() const { return ipc_.position(); }
double Player::get_duration() const { return ipc_.duration(); }
bool   Player::have_progress() const { return ipc_.have_progress(); }

} // namespace ytui
