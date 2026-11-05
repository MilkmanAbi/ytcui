#pragma once
// The video model the UI works with.
//
// Deliberately its own type rather than ytcui-dl's VideoInfo. Two backends
// have to fill it -- ytcui-dl in-process and yt-dlp out of it -- and the UI
// should not know or care which one did. It also means ytcui-dl's headers stay
// out of the UI translation units entirely.
//
// Field names and meanings match v4's Video struct, so the library and history
// files on disk keep their shape.

#include <string>
#include <vector>

namespace ytcui {

struct Video {
    std::string id;
    std::string title;
    std::string channel;
    std::string channel_id;
    std::string duration;          // "3:33" as shown
    int         duration_seconds = 0;
    std::string view_count;        // "1.4B views" as shown
    std::string upload_date;
    std::string thumbnail_url;
    std::string url;
    std::string description;
    bool        is_live = false;

    bool empty() const { return id.empty(); }
};

// What a player is asked to do with a video.
enum class PlayMode { Video, Audio, AudioLoop };

inline const char* to_string(PlayMode m) {
    switch (m) {
        case PlayMode::Audio:     return "audio";
        case PlayMode::AudioLoop: return "audio (loop)";
        case PlayMode::Video:
        default:                  return "video";
    }
}

// Everything a player needs to actually play something.
struct Streams {
    // Either a video/audio pair, or hls_url on its own. Live streams and
    // byte-capped clients come back as HLS; see ytcui-dl's known constraints.
    std::string video_url;
    std::string audio_url;      // empty when muxed, or in audio-only mode
    std::string hls_url;

    // A signed googlevideo URL only works with the User-Agent of the client
    // that signed it, so this travels with the URLs rather than being guessed
    // at the point of use.
    std::string user_agent;

    bool        muxed = false;
    bool        is_live = false;
    std::string label;          // "1080p60 av01 + 129kbps mp4a", for the UI
    int         start_seconds = 0;

    // The yt-dlp path works differently, on purpose. Rather than extracting
    // URLs and handing them to mpv, it hands mpv the *page* URL and points
    // mpv's own ytdl_hook at the yt-dlp binary.
    //
    // The reason is that a yt-dlp format URL is not self-sufficient: it needs
    // the http_headers yt-dlp reports alongside it, and a User-Agent alone is
    // not always enough, so a URL lifted out of `yt-dlp -j` and played
    // directly can 403 in ways that are miserable to diagnose. mpv and yt-dlp
    // already integrate properly, so letting them do it is both more reliable
    // and faster here -- nothing is extracted twice.
    bool        use_ytdl = false;
    std::string page_url;       // what mpv resolves, when use_ytdl
    std::string ytdl_path;      // the yt-dlp binary to point mpv at
    std::string ytdl_format;    // an mpv --ytdl-format selector, if any

    bool ok() const {
        if (use_ytdl) return !page_url.empty();
        return !hls_url.empty() || !video_url.empty() || !audio_url.empty();
    }
    bool via_hls() const { return !hls_url.empty(); }
};

}  // namespace ytcui
