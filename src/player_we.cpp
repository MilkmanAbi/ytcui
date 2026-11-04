// player_we.cpp — subprocess adapter for the ytcui-we backend.
//
// ODR isolation: this file does NOT include any ytcui-we headers.
// ytcui-we (with its own yj.h) is a separate executable; we communicate
// with it via its --mpv-args interface. Only ytcui-dl's yj.h is used here
// to parse the JSON array output.
#include "player_we.h"
#include "core/proc.h"
#include "log.h"

#include "ytcui-dl/yj.h"  // ytcui-dl's yj — NOT ytcui-we's

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace ytui {

// ── binary discovery ──────────────────────────────────────────────────────────

std::string find_ytcuiwe_binary() {
    // 1. Same directory as the running ytcui executable (portable via proc::self_exe).
    const std::string self = ytcui::proc::self_exe();
    if (!self.empty()) {
        auto sibling = std::filesystem::path(self).parent_path() / "ytcui-we";
        if (std::filesystem::exists(sibling))
            return sibling.string();
    }

    // 2. PATH search — no shell invocation needed.
    return ytcui::proc::which("ytcui-we");
}

// ── JSON parser for mpv-json array ────────────────────────────────────────────
// Output format: ["mpv","--ytdl=no","--force-media-title=...","--user-agent=UA",
//                 "VIDEO_URL","--audio-file=AUDIO_URL"]
static WeStreamInfo parse_mpv_json(const std::string& json) {
    std::string j = json;
    while (!j.empty() && (j.back() == '\n' || j.back() == '\r' || j.back() == ' '))
        j.pop_back();

    if (j.empty() || j.front() != '[')
        throw std::runtime_error("ytcui-we --mpv-json: unexpected output: " + j.substr(0, 80));

    auto root = yj::parse(j);
    if (!root.is_arr())
        throw std::runtime_error("ytcui-we --mpv-json: not a JSON array");

    WeStreamInfo info;
    yj::each_elem(root, [&](yj::Val v) {
        std::string s = yj::unescape(v.raw());
        if (s.rfind("--user-agent=", 0) == 0) {
            info.user_agent = s.substr(13);
        } else if (s.rfind("--audio-file=", 0) == 0) {
            info.audio_url = s.substr(13);
        } else if (s.rfind("--", 0) != 0 && s != "mpv") {
            if (info.video_url.empty())
                info.video_url = s;
        }
        return true;
    });

    // Detect live / HLS manifest (no audio track, URL contains m3u8 or is a local path)
    if (info.audio_url.empty()) {
        const auto& u = info.video_url;
        if (u.find(".m3u8") != std::string::npos ||
            u.find("manifest") != std::string::npos ||
            (!u.empty() && u.front() == '/'))
            info.is_live = true;
    }

    if (info.video_url.empty())
        throw std::runtime_error("ytcui-we --mpv-json: no URL in output");

    return info;
}

// ── public resolver ────────────────────────────────────────────────────────────

WeStreamInfo we_resolve(const std::string& youtube_url,
                        int preferred_height,
                        const std::string& cookies_file) {
    auto binary = find_ytcuiwe_binary();
    if (binary.empty())
        throw std::runtime_error(
            "ytcui-we executable not found — install ytcui-we or build with "
            "CURL support (cmake -DCMAKE_BUILD_TYPE=Release .)");

    Log::write("[ytcui-we] resolver: %s", binary.c_str());

    std::vector<std::string> args{binary, "--mpv-args"};
    if (preferred_height > 0)
        args.insert(args.end(), {"-q", std::to_string(preferred_height)});
    if (!cookies_file.empty())
        args.insert(args.end(), {"--cookies", cookies_file});
    args.push_back(youtube_url);

    std::string invocation;
    for (auto& a : args) { invocation += a; invocation += ' '; }
    Log::write("[ytcui-we] invoking: %s", invocation.c_str());

    // 60-second timeout; output capped at 1 MiB (mpv-json is a single line).
    // proc::run uses poll() to drain stdout and stderr concurrently, avoiding
    // the deadlock that sequential drain would cause if ytcui-we fills its
    // stderr pipe while the parent is still reading stdout.
    auto result = ytcui::proc::run(args, 60000, 1u * 1024 * 1024);

    if (!result.ran) {
        throw std::runtime_error("ytcui-we: failed to execute '" + binary + "'");
    }

    if (result.timed_out) {
        throw std::runtime_error("ytcui-we: timed out after 60s");
    }

    if (result.exit_code != 0) {
        if (!result.err.empty())
            Log::write("[ytcui-we] stderr: %s", result.err.substr(0, 400).c_str());
        throw std::runtime_error(
            "ytcui-we exited " + std::to_string(result.exit_code) +
            (result.err.empty() ? "" : ": " + result.err.substr(0, 200)));
    }

    Log::write("[ytcui-we] mpv-json: %s", result.out.substr(0, 200).c_str());
    auto info = parse_mpv_json(result.out);
    Log::write("[ytcui-we] resolved video=%s audio=%s live=%s",
               info.video_url.substr(0, 60).c_str(),
               info.audio_url.empty() ? "(muxed)" : info.audio_url.substr(0, 60).c_str(),
               info.is_live ? "yes" : "no");
    return info;
}

} // namespace ytui
