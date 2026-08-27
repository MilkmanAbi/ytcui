#pragma once

#include "types.h"
#include <string>
#include <sys/types.h>
#include "mpv_ipc.h"

namespace ytui {

// Options passed from CLI flags into the Player for a single session.
// All fields default to "off" — only explicitly passed flags change behaviour.
struct PlayerOptions {
    bool no_hardware_accel = false;  // --no-ha: disables mpv hwdec
    bool no_cache          = false;  // --no-cache: disables mpv demuxer cache (debug)
    bool verbose_mpv       = false;  // --mpv-verbose: don't silence mpv terminal output
    int  volume            = 80;     // default volume (--volume)
};

class Player {
public:
    Player();
    ~Player();

    // Apply session options (call before first play())
    void set_options(const PlayerOptions& opts) { opts_ = opts; }

    void play(const std::string& url, const std::string& title, PlayMode mode);
    void stop();

    bool toggle_pause();
    bool is_paused() const { return paused_; }
    bool is_playing() const;

    // Volume control via IPC (instant, no restart)
    bool volume_up(int step = 5);
    bool volume_down(int step = 5);
    bool set_volume(int vol);
    int  get_volume() const;

    // Seeking via IPC
    bool seek_forward(double secs = 10.0);
    bool seek_backward(double secs = 10.0);
    bool seek_to(double secs);   // absolute, clamped to [0, duration]

    std::string now_playing() const;

    // Progress info (cached from mpv IPC; never blocks). Call tick() once per
    // UI frame to refresh the cache and keep the IPC connection alive.
    void   tick();                  // pump IPC + retry connect; per-frame
    double get_position() const;    // seconds elapsed, -1 if unknown
    double get_duration() const;    // total seconds, -1 if unknown
    bool   have_progress() const;

    static bool is_available();

private:
    pid_t mpv_pid_ = -1;
    std::string current_title_;
    bool playing_ = false;
    bool paused_  = false;
    int  death_pipe_[2] = {-1, -1};

    PlayerOptions opts_;
    MpvIPC ipc_;
    int current_volume_ = 80;

    void play_piped(const std::string& url, const std::string& title, PlayMode mode);
    void play_direct(const std::string& url, const std::string& title);
    void play_xdg(const std::string& url, const std::string& title);
    void kill_mpv();
    void close_death_pipe();
};

} // namespace ytui
