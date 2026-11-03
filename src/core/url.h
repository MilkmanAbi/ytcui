#pragma once
// YouTube link parsing.
//
// This exists so that dropping a link into ytcui just works. Paste anything --
// a watch URL, a youtu.be short link, a Shorts link, a music.youtube.com link,
// a live URL, a bare 11-character id, or a whole sentence with a URL buried in
// it -- and ytcui finds the video and plays it rather than running a text
// search for the URL.
//
// ytcui-dl has its own extract_video_id(), but it only answers "which video".
// ytcui needs more: whether the thing is a video, a playlist or a channel, and
// whether a timestamp came along with it, because `t=90` should start playback
// at 90 seconds rather than being silently dropped.

#include <string>
#include <string_view>

namespace ytcui::url {

enum class Kind {
    None,       // not a link we recognise -- treat the text as a search query
    Video,
    Playlist,   // a list= link with no video attached
    Channel,
};

struct Parsed {
    Kind        kind = Kind::None;
    std::string video_id;      // exactly 11 chars when kind == Video
    std::string playlist_id;   // set for Playlist, and for a Video in a list
    std::string channel;       // "@handle", "UC..." or a legacy /user/ name
    int         start_seconds = 0;
    bool        has_start = false;

    bool ok() const { return kind != Kind::None; }
};

// Parse one link, or text containing one. Returns kind == None when the input
// is not a YouTube link, which the caller should treat as a search query.
Parsed parse(std::string_view text);

// True when `text` looks like a URL at all (any scheme or a bare host). Used
// to decide whether to even attempt link handling on search input.
bool looks_like_url(std::string_view text);

// Pull the first http(s) URL out of free text, or an empty string.
std::string extract_first_url(std::string_view text);

// "90" | "90s" | "1m30s" | "1h2m3s" | "02:15" | "1:02:15"  ->  seconds.
// Returns -1 when it cannot be read as a duration.
int parse_timestamp(std::string_view v);

// Canonical forms.
std::string watch_url(const std::string& video_id);
std::string short_url(const std::string& video_id);     // youtu.be/<id>
std::string playlist_url(const std::string& list_id);
std::string channel_url(const std::string& channel);

// A YouTube video id is 11 characters of [A-Za-z0-9_-]. Checking the alphabet
// matters: without it, an 11-character search query like "hello world" would
// be mistaken for an id.
bool is_video_id(std::string_view s);

}  // namespace ytcui::url
