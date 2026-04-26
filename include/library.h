#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <ctime>
#include <cstdlib>
#include <random>

namespace ytui {

using json = nlohmann::json;

// ═══════════════════════════════════════════════════════════════════════════════
// Data structures
// ═══════════════════════════════════════════════════════════════════════════════

struct VideoEntry {
    std::string id;
    std::string title;
    std::string channel;
    std::string channel_id;
    std::string thumbnail_url;
    int duration_seconds = 0;
    long long timestamp = 0;  // When added
};

struct BookmarkEntry {
    std::string id;
    std::string title;
    std::string channel;
    std::string channel_id;
    std::string type; // "video", "channel"
    long long timestamp = 0;
};

struct Playlist {
    std::string id;           // Unique ID (generated)
    std::string name;         // User-visible name
    std::vector<VideoEntry> videos;
    long long created_at = 0;
    long long updated_at = 0;
};

// ═══════════════════════════════════════════════════════════════════════════════
// Library class - manages bookmarks, history, subscriptions, and playlists
// ═══════════════════════════════════════════════════════════════════════════════

class Library {
public:
    void load() {
        ensure_dir();
        load_bookmarks(data_dir() + "/library.json");
        load_history(data_dir() + "/history.json");
        load_playlists(data_dir() + "/playlists.json");
    }

    void save() const {
        ensure_dir();
        save_bookmarks(data_dir() + "/library.json");
        save_history(data_dir() + "/history.json");
        save_playlists(data_dir() + "/playlists.json");
    }

    // ─── Bookmarks ──────────────────────────────────────────────────────────────
    
    bool is_bookmarked(const std::string& id) const {
        return std::any_of(bookmarks_.begin(), bookmarks_.end(),
            [&](const BookmarkEntry& e) { return e.id == id && e.type == "video"; });
    }

    void toggle_bookmark(const std::string& id, const std::string& title,
                         const std::string& channel, const std::string& channel_id,
                         const std::string& type = "video") {
        auto it = std::find_if(bookmarks_.begin(), bookmarks_.end(),
            [&](const BookmarkEntry& e) { return e.id == id; });

        if (it != bookmarks_.end()) {
            bookmarks_.erase(it);
        } else {
            bookmarks_.push_back({id, title, channel, channel_id, type, (long long)time(nullptr)});
        }
        save();
    }

    // ─── Subscriptions ──────────────────────────────────────────────────────────

    bool is_subscribed(const std::string& channel_id) const {
        return std::any_of(bookmarks_.begin(), bookmarks_.end(),
            [&](const BookmarkEntry& e) {
                return e.channel_id == channel_id && e.type == "channel";
            });
    }

    void toggle_subscribe(const std::string& channel_id, const std::string& channel_name) {
        auto it = std::find_if(bookmarks_.begin(), bookmarks_.end(),
            [&](const BookmarkEntry& e) {
                return e.channel_id == channel_id && e.type == "channel";
            });
        if (it != bookmarks_.end()) {
            bookmarks_.erase(it);
        } else {
            bookmarks_.push_back({channel_id, channel_name, channel_name, channel_id,
                                  "channel", (long long)time(nullptr)});
        }
        save();
    }

    // ─── History ────────────────────────────────────────────────────────────────

    void add_to_history(const std::string& id, const std::string& title,
                        const std::string& channel, const std::string& channel_id,
                        const std::string& thumbnail_url = "", int duration = 0) {
        // Remove if already in history (to move to top)
        history_.erase(std::remove_if(history_.begin(), history_.end(),
            [&](const VideoEntry& e) { return e.id == id; }), history_.end());
        
        VideoEntry v;
        v.id = id;
        v.title = title;
        v.channel = channel;
        v.channel_id = channel_id;
        v.thumbnail_url = thumbnail_url;
        v.duration_seconds = duration;
        v.timestamp = (long long)time(nullptr);
        history_.push_back(v);
        
        // Keep last 100
        if (history_.size() > 100) history_.erase(history_.begin());
        save();
    }

    // ─── Playlists ──────────────────────────────────────────────────────────────

    std::string create_playlist(const std::string& name) {
        Playlist pl;
        pl.id = generate_id();
        pl.name = name.empty() ? "Untitled Playlist" : name;
        pl.created_at = (long long)time(nullptr);
        pl.updated_at = pl.created_at;
        playlists_.push_back(pl);
        save();
        return pl.id;
    }

    bool delete_playlist(const std::string& playlist_id) {
        auto it = std::find_if(playlists_.begin(), playlists_.end(),
            [&](const Playlist& p) { return p.id == playlist_id; });
        if (it != playlists_.end()) {
            playlists_.erase(it);
            save();
            return true;
        }
        return false;
    }

    bool rename_playlist(const std::string& playlist_id, const std::string& new_name) {
        Playlist* pl = get_playlist_mut(playlist_id);
        if (pl) {
            pl->name = new_name;
            pl->updated_at = (long long)time(nullptr);
            save();
            return true;
        }
        return false;
    }

    bool add_to_playlist(const std::string& playlist_id, const VideoEntry& video) {
        Playlist* pl = get_playlist_mut(playlist_id);
        if (!pl) return false;
        
        // Check if already in playlist
        auto it = std::find_if(pl->videos.begin(), pl->videos.end(),
            [&](const VideoEntry& v) { return v.id == video.id; });
        if (it != pl->videos.end()) return false;  // Already exists
        
        VideoEntry v = video;
        v.timestamp = (long long)time(nullptr);
        pl->videos.push_back(v);
        pl->updated_at = (long long)time(nullptr);
        save();
        return true;
    }

    bool add_to_playlist(const std::string& playlist_id,
                         const std::string& video_id, const std::string& title,
                         const std::string& channel, const std::string& channel_id,
                         const std::string& thumbnail_url = "", int duration = 0) {
        VideoEntry v;
        v.id = video_id;
        v.title = title;
        v.channel = channel;
        v.channel_id = channel_id;
        v.thumbnail_url = thumbnail_url;
        v.duration_seconds = duration;
        return add_to_playlist(playlist_id, v);
    }

    bool remove_from_playlist(const std::string& playlist_id, const std::string& video_id) {
        Playlist* pl = get_playlist_mut(playlist_id);
        if (!pl) return false;
        
        auto it = std::find_if(pl->videos.begin(), pl->videos.end(),
            [&](const VideoEntry& v) { return v.id == video_id; });
        if (it == pl->videos.end()) return false;
        
        pl->videos.erase(it);
        pl->updated_at = (long long)time(nullptr);
        save();
        return true;
    }

    bool move_in_playlist(const std::string& playlist_id, int from_idx, int to_idx) {
        Playlist* pl = get_playlist_mut(playlist_id);
        if (!pl) return false;
        if (from_idx < 0 || from_idx >= (int)pl->videos.size()) return false;
        if (to_idx < 0 || to_idx >= (int)pl->videos.size()) return false;
        if (from_idx == to_idx) return false;
        
        VideoEntry v = pl->videos[from_idx];
        pl->videos.erase(pl->videos.begin() + from_idx);
        pl->videos.insert(pl->videos.begin() + to_idx, v);
        pl->updated_at = (long long)time(nullptr);
        save();
        return true;
    }

    bool move_up_in_playlist(const std::string& playlist_id, int idx) {
        return move_in_playlist(playlist_id, idx, idx - 1);
    }

    bool move_down_in_playlist(const std::string& playlist_id, int idx) {
        return move_in_playlist(playlist_id, idx, idx + 1);
    }

    // Check if video is in any playlist
    bool is_in_any_playlist(const std::string& video_id) const {
        for (const auto& pl : playlists_) {
            for (const auto& v : pl.videos) {
                if (v.id == video_id) return true;
            }
        }
        return false;
    }

    // Get list of playlists containing a video
    std::vector<std::string> get_playlists_containing(const std::string& video_id) const {
        std::vector<std::string> result;
        for (const auto& pl : playlists_) {
            for (const auto& v : pl.videos) {
                if (v.id == video_id) {
                    result.push_back(pl.id);
                    break;
                }
            }
        }
        return result;
    }

    // ─── Accessors ──────────────────────────────────────────────────────────────

    std::vector<BookmarkEntry> subscriptions() const {
        std::vector<BookmarkEntry> subs;
        for (const auto& e : bookmarks_)
            if (e.type == "channel") subs.push_back(e);
        return subs;
    }

    std::vector<BookmarkEntry> saved_videos() const {
        std::vector<BookmarkEntry> vids;
        for (const auto& e : bookmarks_)
            if (e.type == "video") vids.push_back(e);
        return vids;
    }

    const std::vector<VideoEntry>& history() const { return history_; }
    const std::vector<BookmarkEntry>& entries() const { return bookmarks_; }
    const std::vector<Playlist>& playlists() const { return playlists_; }
    
    const Playlist* get_playlist(const std::string& id) const {
        auto it = std::find_if(playlists_.begin(), playlists_.end(),
            [&](const Playlist& p) { return p.id == id; });
        return (it != playlists_.end()) ? &(*it) : nullptr;
    }

    static std::string data_dir() {
        const char* xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0] != '\0') return std::string(xdg) + "/ytcui";
        const char* home = getenv("HOME");
        return std::string(home ? home : "/tmp") + "/.local/share/ytcui";
    }

private:
    std::vector<BookmarkEntry> bookmarks_;
    std::vector<VideoEntry> history_;
    std::vector<Playlist> playlists_;

    Playlist* get_playlist_mut(const std::string& id) {
        auto it = std::find_if(playlists_.begin(), playlists_.end(),
            [&](const Playlist& p) { return p.id == id; });
        return (it != playlists_.end()) ? &(*it) : nullptr;
    }

    static std::string generate_id() {
        static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dis(0, sizeof(chars) - 2);
        
        std::string id = "pl_";
        for (int i = 0; i < 8; i++) id += chars[dis(gen)];
        return id;
    }

    static void ensure_dir() {
        std::string dir = data_dir();
        std::string cmd = "mkdir -p '" + dir + "' 2>/dev/null";
        int r = system(cmd.c_str()); (void)r;
    }

    // ─── Load/Save ──────────────────────────────────────────────────────────────

    void load_bookmarks(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return;
        try {
            auto j = json::parse(f);
            bookmarks_.clear();
            for (const auto& item : j) {
                BookmarkEntry e;
                e.id = item.value("id", "");
                e.title = item.value("title", "");
                e.channel = item.value("channel", "");
                e.channel_id = item.value("channel_id", "");
                e.type = item.value("type", "video");
                e.timestamp = item.value("timestamp", 0LL);
                if (!e.id.empty()) bookmarks_.push_back(e);
            }
        } catch (...) {}
    }

    void save_bookmarks(const std::string& path) const {
        std::ofstream f(path);
        if (!f.is_open()) return;
        json j = json::array();
        for (const auto& e : bookmarks_) {
            j.push_back({{"id", e.id}, {"title", e.title}, {"channel", e.channel},
                         {"channel_id", e.channel_id}, {"type", e.type},
                         {"timestamp", e.timestamp}});
        }
        f << j.dump(2);
    }

    void load_history(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return;
        try {
            auto j = json::parse(f);
            history_.clear();
            for (const auto& item : j) {
                VideoEntry v;
                v.id = item.value("id", "");
                v.title = item.value("title", "");
                v.channel = item.value("channel", "");
                v.channel_id = item.value("channel_id", "");
                v.thumbnail_url = item.value("thumbnail_url", "");
                v.duration_seconds = item.value("duration_seconds", 0);
                v.timestamp = item.value("timestamp", 0LL);
                if (!v.id.empty()) history_.push_back(v);
            }
        } catch (...) {}
    }

    void save_history(const std::string& path) const {
        std::ofstream f(path);
        if (!f.is_open()) return;
        json j = json::array();
        for (const auto& v : history_) {
            j.push_back({{"id", v.id}, {"title", v.title}, {"channel", v.channel},
                         {"channel_id", v.channel_id}, {"thumbnail_url", v.thumbnail_url},
                         {"duration_seconds", v.duration_seconds}, {"timestamp", v.timestamp}});
        }
        f << j.dump(2);
    }

    void load_playlists(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return;
        try {
            auto j = json::parse(f);
            playlists_.clear();
            for (const auto& item : j) {
                Playlist pl;
                pl.id = item.value("id", "");
                pl.name = item.value("name", "");
                pl.created_at = item.value("created_at", 0LL);
                pl.updated_at = item.value("updated_at", 0LL);
                
                if (item.contains("videos") && item["videos"].is_array()) {
                    for (const auto& vi : item["videos"]) {
                        VideoEntry v;
                        v.id = vi.value("id", "");
                        v.title = vi.value("title", "");
                        v.channel = vi.value("channel", "");
                        v.channel_id = vi.value("channel_id", "");
                        v.thumbnail_url = vi.value("thumbnail_url", "");
                        v.duration_seconds = vi.value("duration_seconds", 0);
                        v.timestamp = vi.value("timestamp", 0LL);
                        if (!v.id.empty()) pl.videos.push_back(v);
                    }
                }
                if (!pl.id.empty()) playlists_.push_back(pl);
            }
        } catch (...) {}
    }

    void save_playlists(const std::string& path) const {
        std::ofstream f(path);
        if (!f.is_open()) return;
        json j = json::array();
        for (const auto& pl : playlists_) {
            json videos = json::array();
            for (const auto& v : pl.videos) {
                videos.push_back({{"id", v.id}, {"title", v.title}, {"channel", v.channel},
                                  {"channel_id", v.channel_id}, {"thumbnail_url", v.thumbnail_url},
                                  {"duration_seconds", v.duration_seconds}, {"timestamp", v.timestamp}});
            }
            j.push_back({{"id", pl.id}, {"name", pl.name}, {"videos", videos},
                         {"created_at", pl.created_at}, {"updated_at", pl.updated_at}});
        }
        f << j.dump(2);
    }
};

} // namespace ytui
