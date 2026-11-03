#pragma once
/*
 * ytcui-dl — yt_types.h
 *
 * Core data structures.
 *
 * The old StreamFormat carried eight std::strings. Seven of them
 * (mime_type, quality, quality_label, audio_codec, video_codec, container)
 * are functions of the itag alone, so they now point into the static table in
 * yt_itag.h and cost nothing. Only `url` is genuinely per-request.
 *
 * The string_view members are still comparable to string literals and still
 * support find()/empty()/substr(), so most existing call sites compile
 * unchanged. What breaks is implicit conversion to std::string; those call
 * sites need an explicit std::string(...).
 *
 * Views point either at the static itag table (static lifetime) or at the
 * owning VideoInfo's arena, so a StreamFormat must not outlive its VideoInfo.
 */

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "yt_itag.h"

namespace ytfast {

// ---------------------------------------------------------------------------
// Arena — stable storage for the rare strings we can't get from the itag table
// (unknown itags, unusual codec profiles). Chunked so views never dangle on
// growth, unlike a vector<string>.
// ---------------------------------------------------------------------------
class StrArena {
public:
    std::string_view intern(std::string_view s) {
        if (s.empty()) return {};
        if (s.size() > kChunk) {  // oversized: give it a chunk of its own
            auto p = std::make_unique<char[]>(s.size());
            std::memcpy(p.get(), s.data(), s.size());
            std::string_view v(p.get(), s.size());
            big_.push_back(std::move(p));
            return v;
        }
        if (chunks_.empty() || used_ + s.size() > kChunk) {
            chunks_.push_back(std::make_unique<char[]>(kChunk));
            used_ = 0;
        }
        char* dst = chunks_.back().get() + used_;
        std::memcpy(dst, s.data(), s.size());
        used_ += s.size();
        return std::string_view(dst, s.size());
    }
    void clear() { chunks_.clear(); big_.clear(); used_ = 0; }

private:
    static constexpr size_t kChunk = 4096;
    std::vector<std::unique_ptr<char[]>> chunks_, big_;
    size_t used_ = 0;
};

struct StreamFormat {
    int     itag            = 0;
    int     width           = 0;
    int     height          = 0;
    int     fps             = 0;
    int     audio_sample_rate = 0;
    int     audio_channels  = 0;
    int64_t bitrate         = 0;
    int64_t average_bitrate = 0;
    int64_t content_length  = 0;
    bool    has_video       = false;
    bool    has_audio       = false;
    bool    is_drc          = false;  // loudness-compressed duplicate
    float   loudness_db     = 0.f;

    // The one genuinely dynamic string. ~600 bytes, time-limited, per-request.
    std::string url;

    // All views: static table, or the owning VideoInfo's arena.
    std::string_view mime_type;
    std::string_view quality;
    std::string_view quality_label;   // "1080p60", "160kbps"
    std::string_view audio_codec;     // "mp4a.40.2", "opus"
    std::string_view video_codec;     // "avc1.4d401f", "vp9", "av01..."
    std::string_view container;       // "mp4", "webm"

    bool is_audio_only() const { return has_audio && !has_video; }
    bool is_video_only() const { return has_video && !has_audio; }
    bool is_muxed()      const { return has_video && has_audio; }

    int64_t effective_bitrate() const {
        return average_bitrate > 0 ? average_bitrate : bitrate;
    }

    // Byte-range URL for downloaders that want a single-shot GET. Built on
    // demand: storing a second near-identical copy of every URL doubled the
    // per-video footprint for a string almost nobody read.
    std::string range_url() const {
        if (content_length <= 0 || url.empty()) return url;
        if (url.find("&range=") != std::string::npos) return url;
        std::string r;
        r.reserve(url.size() + 32);
        r += url;
        r += "&range=0-";
        r += std::to_string(content_length);
        return r;
    }
};

struct ThumbnailInfo {
    std::string url;
    int width  = 0;
    int height = 0;
};

struct VideoInfo {
    std::string id;
    std::string title;
    std::string channel;
    std::string channel_id;
    std::string description;
    std::string thumbnail_url;
    std::vector<ThumbnailInfo> thumbnails;
    std::string upload_date;
    std::string duration_str;
    int         duration_secs = 0;
    int64_t     view_count    = 0;
    std::string view_count_str;
    bool        is_live       = false;
    std::string url;
    std::string category;
    std::vector<StreamFormat> formats;

    // Which InnerTube client's player response this came from. A format URL
    // is signed for one specific client, so anything fetching it afterwards
    // (media GET, mpv, a download) must send that client's exact User-Agent
    // or the CDN rejects it -- these point at static ClientDef strings, valid
    // for the process lifetime, so raw pointers are fine to carry around.
    const char* client_name = nullptr;
    const char* client_ua   = nullptr;

    // Set when the player answered OK but every adaptive format was
    // URL-less (SABR). Signals the caller to try the next client rather
    // than settle for whatever muxed leftovers came back.
    bool sabr_only = false;

    // Backing store for any format string not covered by the itag table.
    // Shared so VideoInfo stays copyable and the views stay valid across
    // copies (cache hand-out, prefetch results, etc).
    std::shared_ptr<StrArena> arena;

    StrArena& ensure_arena() {
        if (!arena) arena = std::make_shared<StrArena>();
        return *arena;
    }
};

using SearchResult = VideoInfo;

} // namespace ytfast
