#pragma once

#include "types.h"
#include <string>
#include <sys/types.h>

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

    std::string now_playing() const;

    static bool is_available();

private:
    pid_t mpv_pid_ = -1;
    std::string current_title_;
    bool playing_ = false;
    bool paused_  = false;
    int  death_pipe_[2] = {-1, -1};

    PlayerOptions opts_;

    void play_piped(const std::string& url, const std::string& title, PlayMode mode);
    void play_direct(const std::string& url, const std::string& title);
    void play_xdg(const std::string& url, const std::string& title);
    void kill_mpv();
    void close_death_pipe();
};

} // namespace ytui
