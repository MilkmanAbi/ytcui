#include "core/watchstate.h"

#include "core/log.h"
#include "core/paths.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include "yj.h"
#include "yjw.h"

namespace ytcui {
namespace {

// Below this, you were sampling rather than watching, and offering to resume
// 12 seconds in is noise.
constexpr double kMinResume = 30.0;
// Within this of the end, you effectively finished; resuming would drop you
// into the credits.
constexpr double kEndSlack = 30.0;
// Reaching this fraction counts as watched even if the last seconds were
// skipped, which is what people do with outros.
constexpr double kWatchedFraction = 0.92;

int64_t now_s() { return (int64_t)std::time(nullptr); }

}  // namespace

bool WatchEntry::resumable() const {
    if (watched) return false;
    if (position < kMinResume) return false;
    if (duration > 0 && position > duration - kEndSlack) return false;
    return true;
}

double WatchEntry::progress() const {
    if (watched) return 1.0;
    if (duration <= 0) return 0.0;
    const double p = position / duration;
    return p < 0 ? 0 : (p > 1 ? 1 : p);
}

// ---------------------------------------------------------------------------
void WatchState::record(const std::string& id, const std::string& title,
                        const std::string& channel, double position,
                        double duration, bool finished) {
    if (id.empty()) return;
    WatchEntry& e = entries_[id];
    e.id = id;
    if (!title.empty()) e.title = title;
    if (!channel.empty()) e.channel = channel;
    if (duration > 0) e.duration = duration;
    e.position = position < 0 ? 0 : position;
    e.last_played = now_s();

    // Watched when playback reached the end, or got close enough that the
    // rest was an outro. Never un-marks: re-watching half of something you
    // have seen should not make it unwatched again.
    if (finished) e.watched = true;
    else if (e.duration > 0 && position >= e.duration * kWatchedFraction)
        e.watched = true;

    dirty_ = true;
    prune();
}

void WatchState::mark_watched(const std::string& id, bool watched) {
    if (id.empty()) return;
    WatchEntry& e = entries_[id];
    e.id = id;
    e.watched = watched;
    if (!watched) e.position = 0;      // "unwatch" means start over
    e.last_played = now_s();
    dirty_ = true;
}

void WatchState::forget(const std::string& id) {
    if (entries_.erase(id)) dirty_ = true;
}

void WatchState::clear() {
    if (!entries_.empty()) dirty_ = true;
    entries_.clear();
}

const WatchEntry* WatchState::find(const std::string& id) const {
    const auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second;
}

bool WatchState::is_watched(const std::string& id) const {
    const WatchEntry* e = find(id);
    return e && e->watched;
}

double WatchState::resume_position(const std::string& id) const {
    const WatchEntry* e = find(id);
    return (e && e->resumable()) ? e->position : 0.0;
}

std::vector<WatchEntry> WatchState::recent(std::size_t limit) const {
    std::vector<WatchEntry> out;
    out.reserve(entries_.size());
    for (const auto& [k, v] : entries_) out.push_back(v);
    std::sort(out.begin(), out.end(),
              [](const WatchEntry& a, const WatchEntry& b) {
                  return a.last_played > b.last_played;
              });
    if (out.size() > limit) out.resize(limit);
    return out;
}

void WatchState::prune() {
    if (entries_.size() <= kMaxEntries) return;
    std::vector<const WatchEntry*> all;
    all.reserve(entries_.size());
    for (const auto& [k, v] : entries_) all.push_back(&v);
    std::sort(all.begin(), all.end(),
              [](const WatchEntry* a, const WatchEntry* b) {
                  return a->last_played > b->last_played;
              });
    std::vector<std::string> doomed;
    for (std::size_t i = kMaxEntries; i < all.size(); ++i)
        doomed.push_back(all[i]->id);
    for (const std::string& id : doomed) entries_.erase(id);
}

// ---------------------------------------------------------------------------
std::string WatchState::serialize() const {
    yjw::Writer w;
    w.obj();
    w.kv("version", 1);
    w.key("videos").arr();
    // Newest first, so a truncated or hand-edited file keeps what matters.
    for (const WatchEntry& e : recent(kMaxEntries)) {
        w.obj();
        w.kv("id", e.id);
        if (!e.title.empty()) w.kv("title", e.title);
        if (!e.channel.empty()) w.kv("channel", e.channel);
        w.kv("position", (int64_t)(e.position + 0.5));
        if (e.duration > 0) w.kv("duration", (int64_t)(e.duration + 0.5));
        if (e.watched) w.kv("watched", true);
        w.kv("last_played", e.last_played);
        w.end();
    }
    w.end();
    w.end();
    return w.str();
}

bool WatchState::parse(const std::string& json) {
    entries_.clear();
    yj::Val root = yj::parse(json);
    if (!root.is_obj()) return false;

    yj::each_elem(yj::get(root, "videos"), [&](yj::Val v) {
        WatchEntry e;
        if (yj::Val x = yj::get(v, "id"); x.is_str()) e.id = yj::unescape(x.raw());
        if (e.id.empty()) return true;
        if (yj::Val x = yj::get(v, "title"); x.is_str()) e.title = yj::unescape(x.raw());
        if (yj::Val x = yj::get(v, "channel"); x.is_str()) e.channel = yj::unescape(x.raw());
        // yj's raw() is string-only, so numbers go through i64().
        if (yj::Val x = yj::get(v, "position"); x.valid() && !x.is_str())
            e.position = (double)x.i64();
        if (yj::Val x = yj::get(v, "duration"); x.valid() && !x.is_str())
            e.duration = (double)x.i64();
        if (yj::Val x = yj::get(v, "watched"); x.valid() && !x.is_str())
            e.watched = x.boolean();
        if (yj::Val x = yj::get(v, "last_played"); x.valid() && !x.is_str())
            e.last_played = x.i64();
        entries_[e.id] = std::move(e);
        return true;
    });
    dirty_ = false;
    return true;
}

bool WatchState::load() { return load_from(paths::data_file("watch.json")); }

bool WatchState::load_from(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;          // first run
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

bool WatchState::save() const {
    return save_to(paths::data_dir() + "/watch.json");
}

bool WatchState::save_to(const std::string& path) const {
    const auto slash = path.rfind('/');
    if (slash != std::string::npos && !paths::ensure_dir(path.substr(0, slash)))
        return false;
    // Temp file then rename: an interrupted save must not leave a half-written
    // file that fails to parse and loses the whole history.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << serialize() << "\n";
        if (!f.good()) return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    dirty_ = false;
    return true;
}

}  // namespace ytcui
