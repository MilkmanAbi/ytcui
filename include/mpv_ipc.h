#pragma once
// ─── mpv IPC controller ──────────────────────────────────────────────────────
// Talks to mpv over its JSON IPC socket (--input-ipc-server). Replaces the old
// SIGSTOP/SIGCONT pause (which drained/rebuffered audio, a 2-3s stall) with
// codec-level pause, plus volume, seek, and live progress.
//
// DESIGN: strictly non-blocking. Commands are fire-and-forget. Properties are
// pushed by mpv via observe_property; we drain the socket once per frame into a
// cache. Nothing here ever blocks the UI loop — a broken or absent socket just
// means the getters return their last-known (or default) values.

#include <string>

namespace ytui {

class MpvIPC {
public:
    MpvIPC();
    ~MpvIPC();

    // Make a unique socket path (before spawning mpv). Pass as
    // --input-ipc-server=<path>. Returns the path.
    std::string init_socket();
    const std::string& socket_path() const { return path_; }

    // Connect after mpv is up. Non-blocking: one quick attempt, retried by the
    // caller across frames via poll(). Returns true once connected.
    bool try_connect();     // single non-blocking attempt
    bool connected() const { return fd_ >= 0; }
    void disconnect();
    void cleanup();         // unlink the socket file

    // Drain any pending events/replies from mpv and update the property cache.
    // Call once per UI frame. Never blocks. Safe when disconnected (no-op).
    void pump();

    // Commands — fire-and-forget, never block. Return false if not connected.
    bool toggle_pause();
    bool set_pause(bool state);
    bool set_volume(int vol);        // absolute, clamped 0-150
    bool adjust_volume(int delta);   // relative
    bool seek(double seconds);          // relative
    bool seek_absolute(double target);  // absolute, exact (not keyframe-snapped)
    bool quit();

    // Cached properties (updated by pump() from mpv's observe events).
    // Return -1 / defaults when unknown. Never block.
    double position() const { return pos_; }
    double duration() const { return dur_; }
    int    volume()   const { return vol_; }
    bool   paused()   const { return paused_; }
    bool   have_progress() const { return dur_ > 0.0; }

private:
    int fd_ = -1;
    std::string path_;
    std::string rbuf_;     // partial-line read buffer
    int next_id_ = 1;

    // cached, observed properties
    double pos_ = -1.0;
    double dur_ = -1.0;
    int    vol_ = -1;
    bool   paused_ = false;
    int    stall_frames_ = 0;   // frames connected with no position data yet

    bool send_raw(const std::string& data);
    void request_observers();          // ask mpv to push the props we care about
    void handle_line(const std::string& line);
};

} // namespace ytui
