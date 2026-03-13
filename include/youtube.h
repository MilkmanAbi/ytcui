#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <optional>

namespace ytui {

class YouTube {
public:
    YouTube();
    ~YouTube();

    // Search YouTube, returns list of video results
    std::vector<Video> search(const std::string& query, int max_results = 15,
                               const std::string& cookie_args = "");

    // Get the direct stream URL for a video
    std::string get_stream_url(const std::string& video_id, bool audio_only = false);

    // Get video info (single video details)
    std::optional<Video> get_video_info(const std::string& video_id);

    // Check if yt-dlp is available
    static bool is_available();

private:
    // Execute yt-dlp and capture stdout
    std::string exec_ytdlp(const std::vector<std::string>& args);

    // Parse a JSON object into a Video struct
    Video parse_video_json(const std::string& json_str);

    // Format view count (e.g., 1234567 -> "1.23M")
    static std::string format_views(long long views);

    // Format duration seconds to "H:MM:SS" or "M:SS"
    static std::string format_duration(int seconds);
};

} // namespace ytui
