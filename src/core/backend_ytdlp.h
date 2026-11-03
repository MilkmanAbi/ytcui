#pragma once
// The yt-dlp backend's JSON parsing, exposed so it can be tested without
// running yt-dlp or touching the network.
//
// Worth testing separately because the field names are not obvious and two of
// them are traps, both confirmed by inspecting real output:
//
//   * In --flat-playlist mode `is_live` is null even for a live stream. Only
//     `live_status` ("is_live") is populated, so reading `is_live` marks every
//     live result as a normal video.
//   * `view_count` is absent in flat mode, and live entries carry
//     `concurrent_view_count` instead.
//
// yt-dlp also reports durations and view counts as raw integers, while
// ytcui-dl hands back display strings. Both go through txt::views and
// txt::duration here so the list does not reformat when the backend switches.

#include <string>
#include <vector>

#include "core/video.h"

namespace ytcui::ytdlp {

// Parse `yt-dlp --flat-playlist --dump-single-json ytsearchN:query` output.
std::vector<Video> parse_search(const std::string& json);

// Parse `yt-dlp --no-playlist --dump-single-json <url>` output. Returns an
// empty Video when the JSON has no id.
Video parse_info(const std::string& json);

// mpv's --ytdl-format selector for a height ceiling. 0 means no ceiling.
std::string format_selector(int max_height, bool audio_only);

// A one-line reason out of yt-dlp's stderr, for the status line. yt-dlp is
// verbose and multi-line; this picks the ERROR line a user can act on.
std::string first_error_line(const std::string& stderr_text);

}  // namespace ytcui::ytdlp
