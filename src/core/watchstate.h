#pragma once
// What you have already watched, and where you stopped.
//
// Ranked second in docs/RESEARCH.md, and the thing that turns a search tool
// into something you live in: the better clients all have it and the simple
// ones all lack it.
//
// Two decisions worth stating.
//
// ytcc's rule for "watched" is the sound one and is copied here: a video counts
// as watched when playback actually reached the end, not merely when it was
// opened. Opening something, deciding against it and quitting should not mark
// it off.
//
// Position is tracked by ytcui rather than leaning on mpv's own watch-later.
// mpv only writes that file on a graceful quit, so a kill, a crash or a closed
// terminal loses the position -- and ytcui already polls time-pos over IPC, so
// doing it here is both more reliable and nearly free.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ytcui {

struct WatchEntry {
    std::string id;
    std::string title;
    std::string channel;
    double      position = 0;     // seconds
    double      duration = 0;     // 0 when unknown
    bool        watched = false;
    int64_t     last_played = 0;  // unix seconds

    // Far enough in to be worth offering, and not so near the end that
    // resuming would drop you into the credits.
    bool resumable() const;
    // 0..1, for the progress pip in a results row. 1 when watched.
    double progress() const;
};

class WatchState {
public:
    // <data dir>/watch.json. A missing file is a normal first run.
    bool load();
    bool load_from(const std::string& path);
    bool save() const;
    bool save_to(const std::string& path) const;

    // Record where playback got to. `finished` means it reached the end, which
    // is what marks a video watched.
    void record(const std::string& id, const std::string& title,
                const std::string& channel, double position, double duration,
                bool finished);

    void mark_watched(const std::string& id, bool watched = true);
    void forget(const std::string& id);
    void clear();

    const WatchEntry* find(const std::string& id) const;
    bool is_watched(const std::string& id) const;
    double resume_position(const std::string& id) const;   // 0 if none

    // Most recently played first, for a "continue watching" list.
    std::vector<WatchEntry> recent(std::size_t limit = 50) const;

    std::size_t size() const { return entries_.size(); }
    bool dirty() const { return dirty_; }

    std::string serialize() const;
    bool parse(const std::string& json);

private:
    std::unordered_map<std::string, WatchEntry> entries_;
    mutable bool dirty_ = false;

    // Bounded, because this file grows forever otherwise and is read at every
    // launch. The oldest entries go first.
    static constexpr std::size_t kMaxEntries = 2000;
    void prune();
};

}  // namespace ytcui
