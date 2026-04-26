#include "youtube.h"
#include "log.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <array>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <wchar.h>

// ─── ytcui-dl backend ──────────────────────────────────────────────────────────
#ifdef USE_YTCUIDL
#include "ytcui-dl/ytfast.h"
#endif

using json = nlohmann::json;

namespace ytui {

YouTube::YouTube() {}
YouTube::~YouTube() {}

bool YouTube::is_available() {
#ifdef USE_YTCUIDL
    return true;  // built-in — always available
#else
    return system("which yt-dlp > /dev/null 2>&1") == 0;
#endif
}

static std::string sanitize_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const unsigned char* p = (const unsigned char*)s.c_str();
    const unsigned char* end = p + s.size();
    while (p < end) {
        if (*p < 0x80) {
            if (*p >= 32 || *p == '\t' || *p == '\n') out += (char)*p;
            p++;
        } else if ((*p & 0xE0) == 0xC0 && p+1 < end && (p[1]&0xC0)==0x80) { out.append((const char*)p,2); p+=2; }
        else if ((*p & 0xF0) == 0xE0 && p+2 < end && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80) { out.append((const char*)p,3); p+=3; }
        else if ((*p & 0xF8) == 0xF0 && p+3 < end && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80 && (p[3]&0xC0)==0x80) { out.append((const char*)p,4); p+=4; }
        else p++;
    }
    return out;
}

std::string YouTube::format_views(long long views) {
    if (views >= 1000000000) { char buf[32]; snprintf(buf,sizeof(buf),"%.2fB",views/1e9); return buf; }
    if (views >= 1000000)    { char buf[32]; snprintf(buf,sizeof(buf),"%.2fM",views/1e6); return buf; }
    if (views >= 1000)       { char buf[32]; snprintf(buf,sizeof(buf),"%.2fK",views/1e3); return buf; }
    return std::to_string(views);
}

std::string YouTube::format_duration(int secs) {
    if (secs <= 0) return "0:00";
    int h = secs/3600, m = (secs%3600)/60, s = secs%60;
    char buf[32];
    if (h > 0) snprintf(buf,sizeof(buf),"%d:%02d:%02d",h,m,s);
    else        snprintf(buf,sizeof(buf),"%d:%02d",m,s);
    return buf;
}

// ═════════════════════════════════════════════════════════════════════════════
// BACKEND: ytcui-dl
// ═════════════════════════════════════════════════════════════════════════════
#ifdef USE_YTCUIDL

std::vector<Video> YouTube::search(const std::string& query, int max_results,
                                    const std::string& /* cookie_args */) {
    std::vector<Video> videos;
    Log::write("[ytcui-dl] search: '%s' (max %d)", query.c_str(), max_results);

    try {
        auto results = ytfast::yt_search(query, max_results);
        for (auto& r : results) {
            Video v;
            v.id               = r.id;
            v.title            = sanitize_utf8(r.title);
            v.channel          = sanitize_utf8(r.channel);
            v.channel_id       = r.channel_id;
            v.thumbnail_url    = r.thumbnail_url;
            v.description      = sanitize_utf8(r.description);
            v.duration_seconds = r.duration_secs;
            v.duration         = r.duration_str.empty() ? format_duration(r.duration_secs) : r.duration_str;
            v.url              = r.url.empty() ? "https://www.youtube.com/watch?v=" + r.id : r.url;
            v.is_live          = r.is_live;
            v.view_count       = r.view_count > 0 ? format_views(r.view_count) : "N/A";
            v.upload_date      = r.upload_date;
            if (!v.id.empty()) {
                Log::write("[ytcui-dl]   [%s] %s (%s)", v.id.c_str(), v.title.c_str(), v.duration.c_str());
                videos.push_back(std::move(v));
            }
        }
        Log::write("[ytcui-dl] search returned %zu results", videos.size());
    } catch (const std::exception& e) {
        Log::write("[ytcui-dl] search error: %s", e.what());
    }

    return videos;
}

std::string YouTube::get_stream_url(const std::string& video_id, bool audio_only) {
    try {
        return audio_only ? ytfast::yt_best_audio(video_id)
                          : ytfast::yt_best_video_stream(video_id, 1080);
    } catch (const std::exception& e) {
        Log::write("[ytcui-dl] get_stream_url error: %s", e.what());
        return "";
    }
}

std::optional<Video> YouTube::get_video_info(const std::string& video_id) {
    try {
        auto info = ytfast::yt_get_formats(video_id);
        Video v;
        v.id               = info.id;
        v.title            = sanitize_utf8(info.title);
        v.channel          = sanitize_utf8(info.channel);
        v.channel_id       = info.channel_id;
        v.thumbnail_url    = info.thumbnail_url;
        v.description      = sanitize_utf8(info.description);
        v.duration_seconds = info.duration_secs;
        v.duration         = info.duration_str;
        v.url              = info.url;
        v.is_live          = info.is_live;
        v.view_count       = info.view_count > 0 ? format_views(info.view_count) : "N/A";
        v.upload_date      = info.upload_date;
        return v;
    } catch (const std::exception& e) {
        Log::write("[ytcui-dl] get_video_info error: %s", e.what());
        return std::nullopt;
    }
}

std::string YouTube::exec_ytdlp(const std::vector<std::string>&) { return ""; }
Video       YouTube::parse_video_json(const std::string&) { return {}; }

// ═════════════════════════════════════════════════════════════════════════════
// BACKEND: yt-dlp
// ═════════════════════════════════════════════════════════════════════════════
#else

std::vector<Video> YouTube::search(const std::string& query, int max_results,
                                    const std::string& cookie_args) {
    std::vector<Video> videos;
    Log::write("Searching: '%s' (max %d)%s", query.c_str(), max_results,
               cookie_args.empty() ? "" : " [auth]");

    std::string max_str = std::to_string(max_results);
    std::vector<std::string> args;
    if (!cookie_args.empty()) {
        std::istringstream iss(cookie_args);
        std::string tok;
        while (iss >> tok) args.push_back(tok);
    }
    args.push_back("\"ytsearch" + max_str + ":" + query + "\"");
    args.push_back("--flat-playlist");
    args.push_back("-j");
    args.push_back("--no-warnings");
    args.push_back("--ignore-errors");

    std::string output = exec_ytdlp(args);
    if (output.empty()) { Log::write("yt-dlp returned empty output"); return videos; }

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            auto safe_str = [&](const char* key, const std::string& fallback = "") -> std::string {
                if (j.contains(key) && j[key].is_string()) return sanitize_utf8(j[key].get<std::string>());
                return fallback;
            };
            Video v;
            v.id            = safe_str("id");
            v.title         = safe_str("title", "Unknown");
            v.channel       = safe_str("channel");
            if (v.channel.empty()) v.channel = safe_str("uploader", "Unknown");
            v.channel_id    = safe_str("channel_id");
            v.thumbnail_url = safe_str("thumbnail");
            v.description   = safe_str("description");
            if (j.contains("duration") && j["duration"].is_number())
                v.duration_seconds = j["duration"].get<int>();
            v.duration = format_duration(v.duration_seconds);
            v.url = safe_str("url", "https://www.youtube.com/watch?v=" + v.id);
            if (j.contains("is_live") && j["is_live"].is_boolean())
                v.is_live = j["is_live"].get<bool>();
            if (j.contains("view_count") && j["view_count"].is_number())
                v.view_count = format_views(j["view_count"].get<long long>());
            else v.view_count = "N/A";
            std::string raw_date = safe_str("upload_date");
            if (raw_date.size() == 8)
                v.upload_date = raw_date.substr(0,4)+"-"+raw_date.substr(4,2)+"-"+raw_date.substr(6,2);
            else if (!raw_date.empty()) v.upload_date = raw_date;
            if (!v.id.empty()) {
                Log::write("  [%s] %s (%s)", v.id.c_str(), v.title.c_str(), v.duration.c_str());
                videos.push_back(std::move(v));
            }
        } catch (const std::exception& e) {
            Log::write("JSON error: %s", e.what());
        }
    }
    Log::write("Search returned %zu results", videos.size());
    return videos;
}

std::string YouTube::get_stream_url(const std::string& video_id, bool audio_only) {
    std::vector<std::string> args = {
        "https://www.youtube.com/watch?v=" + video_id,
        "-g", "--no-warnings"
    };
    if (audio_only) args.push_back("-f bestaudio");
    return exec_ytdlp(args);
}

std::optional<Video> YouTube::get_video_info(const std::string& /* video_id */) {
    return std::nullopt;
}

Video YouTube::parse_video_json(const std::string&) { return {}; }

std::string YouTube::exec_ytdlp(const std::vector<std::string>& args) {
    std::string cmd = "yt-dlp";
    for (const auto& arg : args) cmd += " " + arg;
    cmd += " 2>/dev/null";
    Log::write("exec: %s", cmd.c_str());
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
    pclose(pipe);
    return result;
}

#endif  // USE_YTCUIDL

} // namespace ytui
