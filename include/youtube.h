#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <optional>

namespace ytui {

// Result of a combined MP4+MP3 download (see YouTube::download_both).
struct DownloadOutcome {
    bool        ok = false;         // true if at least one file was produced
    std::string mp4_path;           // set iff the MP4 was produced
    std::string mp3_path;           // set iff the MP3 was produced (ytcui-dl backend only)
    std::string error;
};

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

    // Stop any background work (prefetch worker) before process teardown.
    // Safe to call on either backend; a no-op for the yt-dlp backend.
    static void shutdown();

    // Download both an MP4 (best available video+audio, muxed) and an MP3
    // (audio only) for a video into out_dir. BLOCKING — this does real
    // network I/O and, on the ytcui-dl backend, shells out to ffmpeg to mux/
    // transcode; call it from a separate process (see main.cpp's
    // --internal-download), never from the UI thread. cookie_args is only
    // consulted by the yt-dlp backend (matches search()'s existing contract:
    // the native backend does not yet wire up browser-cookie auth).
    static DownloadOutcome download_both(const std::string& video_id,
                                          const std::string& out_dir,
                                          const std::string& cookie_args = "");

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
