#pragma once
// A sleep timer: play for a while, then stop.
//
// It pauses rather than quits, deliberately. Killing the player loses the
// position, the queue and the window; pausing leaves everything exactly where
// it was, so pressing space picks the evening back up. That is the whole
// point of a sleep timer and the thing most implementations get wrong.
//
// The parsing is forgiving on purpose. People type a duration the way they say
// it -- "1h23m", "23 min", "half an hour" is a stretch but "30 sec", "1 hour"
// and a bare "45" are not -- and a timer that rejects "23 min" because it
// wanted "23m" is a timer nobody sets twice.

#include <cstdint>
#include <string>
#include <string_view>

namespace ytcui {

// Parse a human duration into seconds. Returns -1 when it cannot be read.
//
// Accepts, case-insensitively, with or without spaces:
//   1h23m   1h 23m 45s   23m   23 min   23 minutes   1h   1 hour
//   30s     30 sec       30 seconds     90           1:30
//
// A bare number means MINUTES, which is the convention every sleep timer
// uses: someone typing "45" at a sleep-timer prompt means three quarters of
// an hour, not three quarters of a minute.
// A colon form means h:mm, for the same reason -- "1:30" is an hour and a
// half, not ninety seconds.
int parse_duration(std::string_view text);

// Render seconds back as a compact human string: "1h 23m", "45m", "30s".
std::string format_duration(int seconds);

class SleepTimer {
public:
    // `seconds` must be positive. Returns false and changes nothing otherwise.
    bool set(int seconds);
    void cancel();

    bool active() const { return active_; }
    // Seconds left; 0 once it has fired or when inactive.
    int remaining() const;
    int total() const { return total_; }

    // True exactly once, on the first call after the deadline passes. Polled
    // from the playback thread; the once-only contract is what stops it
    // re-pausing every poll after it fires.
    bool fire_if_due();

    // For the UI: "sleep in 23m".
    std::string describe() const;

private:
    bool    active_ = false;
    bool    fired_ = false;
    int     total_ = 0;
    int64_t deadline_ = 0;      // steady-clock seconds
};

}  // namespace ytcui
