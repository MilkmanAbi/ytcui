#include "player.h"
#include "player_resolve.h"
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
    rungs_.clear();
    play_direct(url, title, mode);
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
        execvp(full_args.front().c_str(), (char* const*)argv.data());
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

// All modes resolve through the same runtime backend dispatch.
void Player::play_piped(const std::string& url, const std::string& title, PlayMode mode) {
    play_direct(url, title, mode);
}

void Player::play_direct(const std::string& url, const std::string& title, PlayMode mode) {
    ResolvedStreams streams;
    rungs_.clear();
    try { streams = resolve_playback(url, mode, opts_); }
    catch (const std::exception& error) {
        Log::write("Playback failed: %s", error.what());
        return;
    }
    rungs_ = streams.rungs;
    std::vector<std::string> args{opts_.mpv_path, "--no-terminal", vol_flag(opts_.volume), "--ytdl=no",
                                  "--title=" + title};
    if (mode == PlayMode::Video) {
        args.insert(args.end(), {"--force-window=yes", "--geometry=854x480",
                                 "--autofit-larger=70%", "--autofit-smaller=640x360"});
    } else {
        args.insert(args.end(), {"--no-video", "--audio-pitch-correction=yes"});
        if (mode == PlayMode::AudioLoop) args.push_back("--loop=inf");
    }
    if (opts_.no_hardware_accel) args.push_back("--hwdec=no");
    args.push_back(opts_.no_cache ? "--cache=no" : "--cache=yes");
    if (!opts_.no_cache) args.push_back("--demuxer-max-bytes=100M");
    if (!streams.user_agent.empty()) args.push_back("--user-agent=" + streams.user_agent);
    if (!streams.audio_url.empty()) args.push_back("--audio-file=" + streams.audio_url);
    args.push_back("--");
    args.push_back(streams.video_url);
    const char* backend = streams.backend == BackendKind::YtcuiDl ? "ytcui-dl"
        : streams.backend == BackendKind::YtcuiWe ? "ytcui-we" : "yt-dlp";
    Log::write("Playback resolved by %s", backend);
    std::string sock = ipc_.init_socket();
    if (spawn_mpv(args, mpv_pid_, Log::is_logdump(), sock)) {
        playing_ = true;
        current_title_ = title;
        current_volume_ = opts_.volume;
        ipc_.try_connect();
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

std::vector<std::pair<int, std::string>> Player::resolution_rungs() const {
    return rungs_;
}

} // namespace ytui
