#include "player_resolve.h"
#include "player_we.h"
#include "core/proc.h"
#include "log.h"
#include "ytcui-dl/ytfast.h"
#include <nlohmann/json.hpp>
#include <map>
#include <stdexcept>

namespace ytui {
static ResolvedStreams native_resolve(const std::string& url, PlayMode mode,
                                      const PlayerOptions& options) {
    auto& client = ytfast::InnertubeClient::get_instance();
    client.clear_cookies();
    if (!options.cookies_file.empty() && !client.set_cookies_file(options.cookies_file))
        throw std::runtime_error("cannot load YouTube cookies file");
    auto info = client.get_stream_formats(ytfast::extract_video_id(url));
    ResolvedStreams result;
    result.user_agent = info.client_ua ? info.client_ua : "";
    for (const auto& rung : ytfast::Selector::ladder(info.formats))
        result.rungs.emplace_back(rung.height, rung.label());
    if (info.is_live_now && !info.hls_url.empty()) {
        result.video_url = info.hls_url;
    } else if (mode != PlayMode::Video) {
        result.video_url = client.select_best_audio_stream(info.formats);
        if (result.video_url.empty()) result.video_url = info.hls_url;
    } else {
        result.video_url = client.select_best_video_stream(info.formats, options.preferred_height);
        if (result.video_url.empty()) result.video_url = info.hls_url;
        else if (!client.is_muxed(info.formats, result.video_url))
            result.audio_url = client.select_best_audio_stream(info.formats);
    }
    if (result.video_url.empty()) throw std::runtime_error("ytcui-dl: no playable stream");
    return result;
}

static ResolvedStreams dlp_resolve(const std::string& url, PlayMode mode,
                                   const PlayerOptions& options) {
    std::string cap = options.preferred_height > 0
        ? "[height<=" + std::to_string(options.preferred_height) + "]" : "";
    std::string format = mode == PlayMode::Video
        ? "bestvideo" + cap + "+bestaudio/best" + cap : "bestaudio/best";
    std::vector<std::string> args{options.ytdlp_path, "--dump-single-json", "--no-warnings",
                                  "--no-playlist", "-f", format};
    if (!options.cookies_file.empty()) args.insert(args.end(), {"--cookies", options.cookies_file});
    else if (!options.browser.empty()) args.insert(args.end(), {"--cookies-from-browser", options.browser});
    args.insert(args.end(), {"--", url});
    auto output = ytcui::proc::run(args, 60000, 8u * 1024 * 1024);
    if (!output.ran || output.timed_out || output.exit_code != 0)
        throw std::runtime_error("yt-dlp resolution failed: " + output.err.substr(0, 200));
    auto j = nlohmann::json::parse(output.out);
    ResolvedStreams result;
    auto selected = j.value("requested_formats", nlohmann::json::array());
    if (selected.empty()) selected.push_back(j);
    for (const auto& f : selected) {
        auto stream = f.value("url", std::string{});
        if (stream.empty()) continue;
        if (result.video_url.empty()) {
            result.video_url = stream;
            if (f.contains("http_headers")) result.user_agent = f["http_headers"].value("User-Agent", "");
        } else result.audio_url = stream;
    }
    std::map<int, std::string, std::greater<int>> heights;
    for (const auto& f : j.value("formats", nlohmann::json::array())) {
        if (f.contains("height") && f["height"].is_number_integer() && f["height"].get<int>() > 0) {
            int height = f["height"].get<int>();
            heights[height] = std::to_string(height) + "p";
        }
    }
    result.rungs.assign(heights.begin(), heights.end());
    if (result.user_agent.empty() && j.contains("http_headers"))
        result.user_agent = j["http_headers"].value("User-Agent", "");
    if (result.video_url.empty()) throw std::runtime_error("yt-dlp returned no URL");
    return result;
}

ResolvedStreams resolve_playback(const std::string& url, PlayMode mode,
                                 const PlayerOptions& options, NativeResolver native) {
    if (!native) native = native_resolve;
    auto resolve = [&](BackendKind kind) {
        ResolvedStreams streams;
        if (kind == BackendKind::YtcuiDl) streams = native(url, mode, options);
        else if (kind == BackendKind::YtDlp) streams = dlp_resolve(url, mode, options);
        else {
            auto info = we_resolve(url, options.preferred_height, options.cookies_file,
                                   mode != PlayMode::Video);
            streams.video_url = info.video_url;
            streams.audio_url = info.audio_url;
            streams.user_agent = info.user_agent;
            streams.rungs = std::move(info.rungs);
        }
        if (streams.video_url.empty()) throw std::runtime_error("resolver returned no URL");
        streams.backend = kind;
        return streams;
    };
    try { return resolve(options.backend); }
    catch (const std::exception& error) {
        if (!options.backend_auto_failover) throw;
        BackendKind fallback = options.backend == BackendKind::YtcuiDl
            ? BackendKind::YtDlp : BackendKind::YtcuiDl;
        Log::write("backend %d failed (%s); trying %d", static_cast<int>(options.backend),
                   error.what(), static_cast<int>(fallback));
        return resolve(fallback);
    }
}
}
