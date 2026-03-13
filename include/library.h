#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <ctime>
#include <cstdlib>

namespace ytui {

using json = nlohmann::json;

struct BookmarkEntry {
    std::string id;
    std::string title;
    std::string channel;
    std::string channel_id;
    std::string type; // "video", "channel"
    long long timestamp = 0;
};

class Library {
public:
    void load() {
        ensure_dir();
        load_file(data_dir() + "/library.json", bookmarks_);
        load_file(data_dir() + "/history.json", history_);
    }

    void save() const {
        ensure_dir();
        save_file(data_dir() + "/library.json", bookmarks_);
        save_file(data_dir() + "/history.json", history_);
    }

    bool is_bookmarked(const std::string& id) const {
        return std::any_of(bookmarks_.begin(), bookmarks_.end(),
            [&](const BookmarkEntry& e) { return e.id == id; });
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

    void add_to_history(const std::string& id, const std::string& title,
                        const std::string& channel, const std::string& channel_id) {
        // Remove if already in history (to move to top)
        history_.erase(std::remove_if(history_.begin(), history_.end(),
            [&](const BookmarkEntry& e) { return e.id == id; }), history_.end());
        history_.push_back({id, title, channel, channel_id, "video", (long long)time(nullptr)});
        // Keep last 100
        if (history_.size() > 100) history_.erase(history_.begin());
        save();
    }

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

    const std::vector<BookmarkEntry>& history() const { return history_; }
    const std::vector<BookmarkEntry>& entries() const { return bookmarks_; }

    static std::string data_dir() {
        const char* xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0] != '\0') return std::string(xdg) + "/ytcui";
        const char* home = getenv("HOME");
        return std::string(home ? home : "/tmp") + "/.local/share/ytcui";
    }

private:
    std::vector<BookmarkEntry> bookmarks_;
    std::vector<BookmarkEntry> history_;

    static void ensure_dir() {
        std::string dir = data_dir();
        std::string cmd = "mkdir -p '" + dir + "' 2>/dev/null";
        int r = system(cmd.c_str()); (void)r;
    }

    static void load_file(const std::string& path, std::vector<BookmarkEntry>& out) {
        std::ifstream f(path);
        if (!f.is_open()) return;
        try {
            auto j = json::parse(f);
            out.clear();
            for (const auto& item : j) {
                BookmarkEntry e;
                e.id = item.value("id", "");
                e.title = item.value("title", "");
                e.channel = item.value("channel", "");
                e.channel_id = item.value("channel_id", "");
                e.type = item.value("type", "video");
                e.timestamp = item.value("timestamp", 0LL);
                if (!e.id.empty()) out.push_back(e);
            }
        } catch (...) {}
    }

    static void save_file(const std::string& path, const std::vector<BookmarkEntry>& data) {
        std::ofstream f(path);
        if (!f.is_open()) return;
        json j = json::array();
        for (const auto& e : data) {
            j.push_back({{"id", e.id}, {"title", e.title}, {"channel", e.channel},
                         {"channel_id", e.channel_id}, {"type", e.type},
                         {"timestamp", e.timestamp}});
        }
        f << j.dump(2);
    }
};

} // namespace ytui
