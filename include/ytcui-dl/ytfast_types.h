#pragma once
/*
 * ytcui-dl — ytfast_types.h
 * Core data structures.
 */

#include <string>
#include <vector>
#include <cstdint>

namespace ytfast {

struct StreamFormat {
    int         itag             = 0;
    std::string url;         // for downloading: has &range=0-N appended for DASH streams
    std::string stream_url;  // for streaming (mpv): raw URL without range, use for playback
    std::string mime_type;
    std::string quality;
    std::string quality_label;   // e.g. "1080p", "720p60", "128kbps"
    int         width            = 0;
    int         height           = 0;
    int         fps              = 0;
    int64_t     bitrate          = 0;
    int64_t     average_bitrate  = 0;
    int64_t     content_length   = 0;
    bool        has_video        = false;
    bool        has_audio        = false;
    int         audio_sample_rate= 0;
    int         audio_channels   = 0;
    std::string audio_codec;      // e.g. "mp4a.40.2", "opus"
    std::string video_codec;      // e.g. "avc1.4d401f", "vp9"
    std::string container;        // e.g. "mp4", "webm", "3gp"

    // Convenience
    bool is_audio_only() const { return has_audio && !has_video; }
    bool is_video_only() const { return has_video && !has_audio; }
    bool is_muxed()      const { return has_video && has_audio; }

    // Effective bitrate for sorting (prefer average when available)
    int64_t effective_bitrate() const {
        return average_bitrate > 0 ? average_bitrate : bitrate;
    }
};

struct ThumbnailInfo {
    std::string url;
    int         width  = 0;
    int         height = 0;
};

struct VideoInfo {
    std::string id;
    std::string title;
    std::string channel;
    std::string channel_id;
    std::string description;
    std::string thumbnail_url;         // best available thumbnail
    std::vector<ThumbnailInfo> thumbnails;  // all thumbnails, sorted widest-first
    std::string upload_date;
    std::string duration_str;
    int         duration_secs    = 0;
    int64_t     view_count       = 0;
    std::string view_count_str;
    bool        is_live          = false;
    std::string url;
    std::string category;
    std::vector<StreamFormat> formats;
};

using SearchResult = VideoInfo;

} // namespace ytfast
