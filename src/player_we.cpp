// player_we.cpp — subprocess adapter for the ytcui-we backend.
//
// ODR isolation: this file does NOT include any ytcui-we headers.
// ytcui-we (with its own yj.h) is a separate executable; we communicate
// with it via its --resolve-json interface. The bridge uses nlohmann/json
// to parse a stream descriptor, without including either backend's yj.h.
#include "player_we.h"
#include "core/proc.h"
#include "log.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace ytui {

// ── binary discovery ──────────────────────────────────────────────────────────

std::string find_ytcuiwe_binary() {
    if (const char* override_path = std::getenv("YTCUI_WE_BINARY"))
        return ytcui::proc::which(override_path);
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

// Stable bridge schema, isolated from both backends' yj implementations.
static WeStreamInfo parse_resolve_json(const std::string& text) {
    const auto root = nlohmann::json::parse(text);
    WeStreamInfo info;
    info.video_url = root.at("video_url").get<std::string>();
    info.audio_url = root.value("audio_url", "");
    info.user_agent = root.value("user_agent", "");
    info.is_live = root.value("is_live", false);
    for (const auto& height : root.at("heights")) {
        int h = height.get<int>();
        if (h > 0) info.rungs.emplace_back(h, std::to_string(h) + "p");
    }
    if (info.video_url.empty()) throw std::runtime_error("ytcui-we: no URL in output");
    return info;
}

// ── public resolver ────────────────────────────────────────────────────────────

WeStreamInfo we_resolve(const std::string& youtube_url,
                        int preferred_height,
                        const std::string& cookies_file, bool audio_only) {
    auto binary = find_ytcuiwe_binary();
    if (binary.empty())
        throw std::runtime_error(
            "ytcui-we executable not found — install the ytcui-we companion beside ytcui "
            "or set YTCUI_WE_BINARY");

    Log::write("[ytcui-we] resolver: %s", binary.c_str());

    std::vector<std::string> args{binary, "--resolve-json"};
    if (audio_only) args.push_back("--audio");
    if (preferred_height > 0)
        args.insert(args.end(), {"-q", std::to_string(preferred_height)});
    if (!cookies_file.empty())
        args.insert(args.end(), {"--cookies", cookies_file});
    args.insert(args.end(), {"--", youtube_url});

    std::string invocation;
    for (auto& a : args) { invocation += a; invocation += ' '; }
    Log::write("[ytcui-we] invoking: %s", invocation.c_str());

    // 60-second timeout; output capped at 1 MiB (resolve-json is a single line).
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

    Log::write("[ytcui-we] resolve-json: %s", result.out.substr(0, 200).c_str());
    auto info = parse_resolve_json(result.out);
    Log::write("[ytcui-we] resolved video=%s audio=%s live=%s",
               info.video_url.substr(0, 60).c_str(),
               info.audio_url.empty() ? "(muxed)" : info.audio_url.substr(0, 60).c_str(),
               info.is_live ? "yes" : "no");
    return info;
}

} // namespace ytui
