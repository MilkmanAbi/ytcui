#include "player.h"
#include "compat.h"
#include "log.h"
#include <cstring>
#include <cerrno>
#include <vector>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

namespace ytui {

Player::Player() {
    death_pipe_[0] = -1;
    death_pipe_[1] = -1;
}

Player::~Player() { stop(); }

bool Player::is_available() {
    return system("which mpv > /dev/null 2>&1") == 0;
}

// ─── Public API ───────────────────────────────────────────────────────────────

void Player::play(const std::string& url, const std::string& title, PlayMode mode) {
    stop();
    if (mode == PlayMode::Video)
        play_direct(url, title);
    else
        play_piped(url, title, mode);
}

void Player::stop() {
    kill_mpv();
    close_death_pipe();
    playing_ = false;
    paused_  = false;
    current_title_.clear();
}

void Player::close_death_pipe() {
    if (death_pipe_[0] >= 0) { close(death_pipe_[0]); death_pipe_[0] = -1; }
    if (death_pipe_[1] >= 0) { close(death_pipe_[1]); death_pipe_[1] = -1; }
}

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

// ─── is_playing ───────────────────────────────────────────────────────────────
// FIX: old code only called waitpid(WNOHANG) on the tracked pid. When the user
// closes the mpv window the whole process group dies, but the shell wrapper pid
// may not have been reaped yet so WNOHANG returns 0 (still running) incorrectly,
// leaving "Playing <title>" stuck on screen.
//
// Fix: after waitpid says "still running", also probe the process group with
// kill(-pgid, 0). If errno == ESRCH the group is fully gone — mark stopped.

bool Player::is_playing() const {
    if (!playing_ || mpv_pid_ <= 0) return false;

    int status = 0;
    pid_t r = waitpid(mpv_pid_, &status, WNOHANG);

    if (r == mpv_pid_) {
        const_cast<Player*>(this)->playing_ = false;
        const_cast<Player*>(this)->mpv_pid_ = -1;
        Log::write("is_playing: pid %d exited", r);
        return false;
    }

    if (r < 0) {
        // ECHILD: pid is gone entirely
        const_cast<Player*>(this)->playing_ = false;
        const_cast<Player*>(this)->mpv_pid_ = -1;
        return false;
    }

    // r == 0: pid not reaped yet — probe the process group too.
    // If kill(-pgid, 0) → ESRCH, every process in the group is dead.
    if (kill(-mpv_pid_, 0) < 0 && errno == ESRCH) {
        waitpid(mpv_pid_, &status, WNOHANG);
        const_cast<Player*>(this)->playing_ = false;
        const_cast<Player*>(this)->mpv_pid_ = -1;
        Log::write("is_playing: pgid -%d gone (ESRCH)", mpv_pid_);
        return false;
    }

    return true;
}

std::string Player::now_playing() const {
    if (is_playing()) return current_title_;
    return "";
}

// ─── Internal helpers ─────────────────────────────────────────────────────────

static void child_setup(bool log_to_file,
                        int death_pipe_read_fd,
                        pid_t original_ppid,
                        int sync_pipe_write_fd) {
    setpgid(0, 0);

    if (sync_pipe_write_fd >= 0) {
        char byte = 1;
        ssize_t w = write(sync_pipe_write_fd, &byte, 1); (void)w;
    }

    if (compat::has_native_pdeathsig()) {
        compat::set_pdeathsig(SIGKILL);
        if (death_pipe_read_fd >= 0) close(death_pipe_read_fd);
    } else {
        if (death_pipe_read_fd >= 0)
            compat::fork_death_watchdog(death_pipe_read_fd, original_ppid);
    }

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        if (!log_to_file)
            dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    if (log_to_file) {
        std::string el = Log::get_log_dir() + "/mpv.log";
        int ef = open(el.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (ef >= 0) { dup2(ef, STDERR_FILENO); close(ef); }
    }
}

static void setup_death_pipe(int death_pipe[2]) {
    death_pipe[0] = death_pipe[1] = -1;
    if (!compat::has_native_pdeathsig()) {
        if (compat::pipe_cloexec(death_pipe) < 0)
            Log::write("death pipe_cloexec failed: %s", strerror(errno));
    }
}

static bool wait_for_setpgid(int sync_read_fd, pid_t child_pid) {
    if (sync_read_fd < 0) return false;
    int flags = fcntl(sync_read_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(sync_read_fd, F_SETFL, flags | O_NONBLOCK);
    char byte = 0;
    for (int i = 0; i < 500; i++) {
        ssize_t n = read(sync_read_fd, &byte, 1);
        if (n == 1) return true;
        if (n == 0) return false;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
        int st;
        if (waitpid(child_pid, &st, WNOHANG) == child_pid) return false;
        usleep(1000);
    }
    return false;
}

static std::string vol_flag(int v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "--volume=%d", v);
    return buf;
}

// ─── resolve_stream_urls ──────────────────────────────────────────────────────
// FIX for slow open: the old play_direct passed a youtube.com URL + --ytdl=yes,
// which made mpv invoke yt-dlp internally to extract the stream URL. That entire
// extraction happens after the user clicks play, before the window opens — hence
// the 8-15 second blank wait.
//
// Instead: run yt-dlp -g here, get the raw HTTPS stream URLs, pass them directly
// to mpv with --ytdl=no. mpv gets a plain https:// URL it can start buffering
// immediately and the window opens within ~1 second.
//
// yt-dlp -g with a split format (bestvideo+bestaudio) returns two lines:
//   line 1: video stream URL
//   line 2: audio stream URL
// We pass the video URL as the main file and audio via --audio-file=.
//
// Falls back to --ytdl=yes if yt-dlp -g fails for any reason (private video,
// rate limit, geo-block) so nothing silently breaks.

struct StreamURLs {
    std::string video_url;
    std::string audio_url;
    bool ok = false;
};

static StreamURLs resolve_stream_urls(const std::string& youtube_url) {
    StreamURLs result;

    // Request separate video + audio streams for best quality muxing.
    // yt-dlp -g outputs one URL per line when a split format is selected.
    std::string cmd =
        "yt-dlp -g --no-warnings --no-playlist "
        "-f 'bestvideo[height<=1080]+bestaudio/best[height<=1080]/best' "
        "'" + youtube_url + "' 2>/dev/null";

    Log::write("resolve: %s", cmd.c_str());

    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        Log::write("resolve: popen failed: %s", strerror(errno));
        return result;
    }

    char buf[8192];
    std::string line1, line2;

    if (fgets(buf, sizeof(buf), p)) {
        line1 = buf;
        while (!line1.empty() && (line1.back()=='\n'||line1.back()=='\r'))
            line1.pop_back();
    }
    if (fgets(buf, sizeof(buf), p)) {
        line2 = buf;
        while (!line2.empty() && (line2.back()=='\n'||line2.back()=='\r'))
            line2.pop_back();
    }
    pclose(p);

    if (line1.empty()) {
        Log::write("resolve: no output — private/unavailable/rate-limited?");
        return result;
    }

    if (!line2.empty()) {
        result.video_url = line1;
        result.audio_url = line2;
        Log::write("resolve: video=%.80s...", line1.c_str());
        Log::write("resolve: audio=%.80s...", line2.c_str());
    } else {
        // Combined stream (e.g. live, or fallback format)
        result.video_url = line1;
        result.audio_url = "";
        Log::write("resolve: combined=%.80s...", line1.c_str());
    }

    result.ok = true;
    return result;
}

// ─── play_piped (audio modes) ─────────────────────────────────────────────────

void Player::play_piped(const std::string& url, const std::string& title, PlayMode mode) {
    std::string vol = vol_flag(opts_.volume);

    std::string ytdlp_cmd =
        "yt-dlp --no-warnings --no-playlist "
        "-f 'bestaudio[ext=m4a]/bestaudio[ext=webm]/bestaudio' "
        "--audio-quality 0 "
        "-o - '" + url + "'";

    std::string mpv_cmd = "mpv --no-video --no-terminal " + vol;

    if (!opts_.no_cache) {
        mpv_cmd += " --audio-buffer=2 --cache=yes --demuxer-max-bytes=50M";
    } else {
        mpv_cmd += " --cache=no";
        Log::write("no-cache mode");
    }

    mpv_cmd += " --audio-pitch-correction=yes";

    if (opts_.no_hardware_accel) {
        mpv_cmd += " --hwdec=no";
        Log::write("no-ha mode");
    }

    if (mode == PlayMode::AudioLoop) mpv_cmd += " --loop=inf";
    mpv_cmd += " -";

    std::string cmd = ytdlp_cmd + " | " + mpv_cmd;
    Log::write("Piped play: %s", cmd.c_str());

    close_death_pipe();
    setup_death_pipe(death_pipe_);

    int sync_pipe[2] = {-1, -1};
    if (compat::pipe_cloexec(sync_pipe) < 0) {
        Log::write("sync pipe failed: %s", strerror(errno));
        sync_pipe[0] = sync_pipe[1] = -1;
    }

    pid_t original_ppid = getpid();
    pid_t pid = fork();

    if (pid == 0) {
        if (sync_pipe[0] >= 0) close(sync_pipe[0]);
        if (death_pipe_[1] >= 0) close(death_pipe_[1]);
        child_setup(Log::is_logdump(), death_pipe_[0], original_ppid, sync_pipe[1]);
        execlp("sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    } else if (pid > 0) {
        if (sync_pipe[1] >= 0) { close(sync_pipe[1]); sync_pipe[1] = -1; }
        if (death_pipe_[0] >= 0) { close(death_pipe_[0]); death_pipe_[0] = -1; }
        bool pgid_ok = wait_for_setpgid(sync_pipe[0], pid);
        if (sync_pipe[0] >= 0) { close(sync_pipe[0]); sync_pipe[0] = -1; }
        Log::write("Piped play pid=%d pgid_sync=%s", pid, pgid_ok ? "ok" : "timeout");
        mpv_pid_       = pid;
        playing_       = true;
        current_title_ = title;
    } else {
        Log::write("fork failed: %s", strerror(errno));
        if (sync_pipe[0] >= 0) close(sync_pipe[0]);
        if (sync_pipe[1] >= 0) close(sync_pipe[1]);
        close_death_pipe();
    }
}

// ─── play_direct (video mode) ─────────────────────────────────────────────────

void Player::play_direct(const std::string& url, const std::string& title) {
    std::string vol = vol_flag(opts_.volume);

    // Pre-resolve stream URLs so mpv gets direct https:// and opens immediately.
    // The TUI is already showing "Loading..." at this point.
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
        // Fast path: direct stream URLs, mpv opens window immediately
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
        args.push_back(streams.video_url);
        if (!streams.audio_url.empty())
            args.push_back("--audio-file=" + streams.audio_url);

        Log::write("Direct play (fast): vol=%d hwdec=%s cache=%s",
                   opts_.volume,
                   opts_.no_hardware_accel ? "no" : "auto",
                   opts_.no_cache ? "no" : "yes");
    } else {
        // Slow fallback: let mpv call yt-dlp itself
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

    std::vector<const char*> argv;
    for (auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    int exec_pipe[2] = {-1, -1};
    if (compat::pipe_cloexec(exec_pipe) < 0) {
        Log::write("exec pipe failed: %s", strerror(errno));
        return;
    }

    int sync_pipe[2] = {-1, -1};
    if (compat::pipe_cloexec(sync_pipe) < 0) {
        Log::write("sync pipe failed: %s", strerror(errno));
        sync_pipe[0] = sync_pipe[1] = -1;
    }

    close_death_pipe();
    setup_death_pipe(death_pipe_);

    pid_t original_ppid = getpid();
    pid_t pid = fork();

    if (pid == 0) {
        close(exec_pipe[0]);
        if (sync_pipe[0] >= 0) close(sync_pipe[0]);
        if (death_pipe_[1] >= 0) close(death_pipe_[1]);
        child_setup(Log::is_logdump(), death_pipe_[0], original_ppid, sync_pipe[1]);
        execvp("mpv", (char* const*)argv.data());
        int err = errno;
        ssize_t w = write(exec_pipe[1], &err, sizeof(err)); (void)w;
        _exit(127);
    } else if (pid > 0) {
        close(exec_pipe[1]);
        if (sync_pipe[1] >= 0) { close(sync_pipe[1]); sync_pipe[1] = -1; }
        if (death_pipe_[0] >= 0) { close(death_pipe_[0]); death_pipe_[0] = -1; }

        bool pgid_ok = wait_for_setpgid(sync_pipe[0], pid);
        if (sync_pipe[0] >= 0) { close(sync_pipe[0]); sync_pipe[0] = -1; }

        int child_errno = 0;
        ssize_t n = read(exec_pipe[0], &child_errno, sizeof(child_errno));
        close(exec_pipe[0]);

        if (n > 0) {
            Log::write("mpv exec failed: %s", strerror(child_errno));
            waitpid(pid, nullptr, 0);
            close_death_pipe();
            return;
        }

        Log::write("Direct play pid=%d pgid_sync=%s", pid, pgid_ok ? "ok" : "timeout");
        mpv_pid_       = pid;
        playing_       = true;
        current_title_ = title;
    } else {
        close(exec_pipe[0]); close(exec_pipe[1]);
        if (sync_pipe[0] >= 0) close(sync_pipe[0]);
        if (sync_pipe[1] >= 0) close(sync_pipe[1]);
        close_death_pipe();
        Log::write("fork failed: %s", strerror(errno));
    }
}

void Player::play_xdg(const std::string& url, const std::string& title) {
    Log::write("xdg/open: %s", url.c_str());

    close_death_pipe();
    setup_death_pipe(death_pipe_);

    int sync_pipe[2] = {-1, -1};
    compat::pipe_cloexec(sync_pipe);

    pid_t original_ppid = getpid();
    pid_t pid = fork();

    if (pid == 0) {
        if (sync_pipe[0] >= 0) close(sync_pipe[0]);
        if (death_pipe_[1] >= 0) close(death_pipe_[1]);
        child_setup(false, death_pipe_[0], original_ppid, sync_pipe[1]);
#if defined(YTUI_MACOS)
        execlp("open", "open", url.c_str(), nullptr);
#else
        execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
#endif
        _exit(127);
    } else if (pid > 0) {
        if (sync_pipe[1] >= 0) { close(sync_pipe[1]); sync_pipe[1] = -1; }
        if (death_pipe_[0] >= 0) { close(death_pipe_[0]); death_pipe_[0] = -1; }
        wait_for_setpgid(sync_pipe[0], pid);
        if (sync_pipe[0] >= 0) { close(sync_pipe[0]); sync_pipe[0] = -1; }
        mpv_pid_       = pid;
        playing_       = true;
        current_title_ = title;
    } else {
        if (sync_pipe[0] >= 0) close(sync_pipe[0]);
        if (sync_pipe[1] >= 0) close(sync_pipe[1]);
        close_death_pipe();
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
