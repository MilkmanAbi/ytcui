#pragma once
#include <string>
#include <vector>
#include <utility>

namespace ytui {

// Stream URLs returned by the ytcui-we subprocess resolver.
// Parsed from the "--resolve-json" output of the ytcui-we executable.
struct WeStreamInfo {
    std::string video_url;   // primary URL (video+audio for muxed, video-only for adaptive)
    std::string audio_url;   // separate audio track (empty for muxed/HLS)
    std::string user_agent;  // HTTP UA string required for playback
    std::vector<std::pair<int, std::string>> rungs;
    bool        is_live = false;  // true when this is an HLS/DASH live manifest
};

// Find the ytcui-we executable.
// Searches: (1) YTCUI_WE_BINARY override, sibling executable, then $PATH.
// Returns empty string when not found.
std::string find_ytcuiwe_binary();

// Invoke ytcui-we --resolve-json and parse its output into WeStreamInfo.
// preferred_height: 0 = best available; otherwise e.g. 1080 for -q 1080.
// cookies_file: path to a Netscape cookies file, or empty.
// Throws std::runtime_error on failure (binary missing, resolve error, etc.).
WeStreamInfo we_resolve(const std::string& youtube_url,
                        int preferred_height,
                        const std::string& cookies_file, bool audio_only = false);

} // namespace ytui
