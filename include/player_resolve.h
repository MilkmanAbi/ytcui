#pragma once
#include "player.h"
#include <functional>

namespace ytui {
struct ResolvedStreams {
    std::string video_url, audio_url, user_agent;
    std::vector<std::pair<int, std::string>> rungs;
    BackendKind backend = BackendKind::YtcuiDl;
};
using NativeResolver = std::function<ResolvedStreams(const std::string&, PlayMode, const PlayerOptions&)>;
// The optional native implementation permits deterministic dispatch tests.
ResolvedStreams resolve_playback(const std::string& url, PlayMode mode,
                                 const PlayerOptions& options,
                                 NativeResolver native = {});
}
