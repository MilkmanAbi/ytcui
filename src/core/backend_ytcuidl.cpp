// The in-process backend: ytcui-dl's headers, linked straight in.
//
// No subprocess, no JSON round trip, no Python interpreter -- a search is one
// HTTPS request and a scan. It is also the backend that YouTube breaks first,
// which is the whole reason the interface above exists.
//
// This file is the only place in ytcui that includes ytcui-dl's headers, so
// everything else stays independent of it.

#include "core/backend.h"
#include "core/log.h"
#include "core/url.h"

#include <algorithm>
#include <mutex>

#include "ytfast.h"

namespace ytcui {
namespace {

// Map ytcui-dl's VideoInfo onto ours.
Video to_video(const ytfast::VideoInfo& in) {
    Video v;
    v.id               = in.id;
    v.title            = in.title;
    v.channel          = in.channel;
    v.channel_id       = in.channel_id;
    v.duration         = in.duration_str;
    v.duration_seconds = in.duration_secs;
    v.view_count       = in.view_count_str;
    v.upload_date      = in.upload_date;
    v.thumbnail_url    = in.thumbnail_url;
    v.url              = in.url.empty() ? url::watch_url(in.id) : in.url;
    v.description      = in.description;
    // is_live_now is the one that means "streaming right now"; is_live is
    // isLiveContent, which stays true for the VOD a finished stream leaves
    // behind. Showing LIVE on a two-year-old recording is wrong.
    v.is_live          = in.is_live_now;
    return v;
}

// Work out why nothing came back, so failover can be a decision rather than a
// reflex. ytcui-dl reports these conditions on VideoInfo rather than throwing.
Failure classify(const ytfast::VideoInfo& info, bool had_exception,
                 const std::string& what) {
    Failure f;
    if (had_exception) {
        f.kind = FailKind::Network;
        f.message = what.empty() ? "network or TLS failure" : what;
        return f;
    }
    if (info.formats.empty() && info.hls_url.empty()) {
        if (info.sabr_only) {
            f.kind = FailKind::NeedsToken;
            f.message = "every client returned SABR (no fetchable URLs)";
        } else {
            // Private, removed, region-blocked, or age-gated. ytcui-dl cannot
            // tell us which from here, and no other extractor will do better
            // without cookies, so this is not marked worth_failover.
            f.kind = FailKind::NotAvailable;
            f.message = "no playable formats (private, removed, or age-gated)";
        }
        return f;
    }
    return f;
}

class YtcuiDlBackend final : public Backend {
public:
    YtcuiDlBackend() {
        // One global init for the process, whichever backend is constructed
        // first or how many times the user switches back and forth.
        static ytfast::CurlGlobalInit init;
        (void)init;
    }

    const char* name() const override { return "ytcui-dl"; }

    bool available(std::string* detail) const override {
        // Compiled in, so it is always present. The honest caveat is that
        // "present" is not "working": YouTube can wall it at any time, which
        // is what the failure kinds above are for.
        if (detail) *detail = "built in";
        return true;
    }

    std::vector<Video> search(const std::string& query, int max_results,
                              Failure* fail) override {
        if (fail) fail->clear();
        std::vector<Video> out;
        if (query.empty()) return out;
        try {
            auto& yt = ytfast::InnertubeClient::get_instance();
            const auto results = yt.search(query, max_results);
            out.reserve(results.size());
            for (const auto& r : results) out.push_back(to_video(r));
            if (out.empty() && fail) {
                fail->kind = FailKind::Unknown;
                fail->message = "search returned nothing";
            }
        } catch (const std::exception& e) {
            if (fail) {
                fail->kind = FailKind::Network;
                fail->message = e.what();
            }
            log::warn(std::string("ytcui-dl search failed: ") + e.what());
        }
        return out;
    }

    Video info(const std::string& video_id, Failure* fail) override {
        if (fail) fail->clear();
        try {
            auto& yt = ytfast::InnertubeClient::get_instance();
            const ytfast::VideoInfo vi = yt.get_stream_formats(video_id);
            const Failure f = classify(vi, false, {});
            if (f.failed()) {
                if (fail) *fail = f;
                return {};
            }
            return to_video(vi);
        } catch (const std::exception& e) {
            if (fail) { fail->kind = FailKind::Network; fail->message = e.what(); }
            return {};
        }
    }

    Streams resolve(const std::string& video_id, PlayMode mode, int max_height,
                    Failure* fail) override {
        if (fail) fail->clear();
        Streams s;
        try {
            auto& yt = ytfast::InnertubeClient::get_instance();

            const ytfast::Mode m = (mode == PlayMode::Video)
                                       ? ytfast::Mode::AudioVideo
                                       : ytfast::Mode::AudioOnly;
            ytfast::Quality q = max_height > 0 ? ytfast::Quality::upto(max_height)
                                               : ytfast::Quality::best();
            // Rank by what this machine can decode, not by compression: an AV1
            // stream that stalls the player is worse than a larger one that
            // plays. ytcui-dl's own CLI does the same for --play.
            q.for_playback = true;

            const auto res = yt.resolve(video_id, m, q);
            const ytfast::VideoInfo& info = res.info;

            s.user_agent = info.client_ua ? info.client_ua : "";
            s.is_live    = info.is_live_now;

            // A live stream, or a response that came back with nothing but a
            // manifest, is HLS-only -- its progressive URLs are not fetchable.
            if (info.is_live_now || (!info.hls_url.empty() && info.formats.empty())) {
                if (info.hls_url.empty()) {
                    if (fail) {
                        fail->kind = FailKind::NotAvailable;
                        fail->message = info.is_live_now
                                            ? "live stream with no HLS manifest"
                                            : "no playable formats";
                    }
                    return {};
                }
                s.hls_url = info.hls_url;
                s.label   = info.is_live_now ? "LIVE via HLS" : "HLS";
                return s;
            }

            if (!res.ok()) {
                const Failure f = classify(info, false, {});
                if (fail)
                    *fail = f.failed() ? f
                                       : Failure{FailKind::Unknown,
                                                 "nothing selectable for that request"};
                return {};
            }

            s.muxed = res.sel.muxed;
            s.label = res.sel.describe();

            if (mode == PlayMode::Video) {
                if (res.sel.video) s.video_url = res.sel.video->url;
                if (res.sel.audio && !res.sel.muxed) s.audio_url = res.sel.audio->url;
            } else {
                // Audio-only: a muxed stream carries its own audio, so it goes
                // in the video slot and the player is told not to show video.
                if (res.sel.audio) s.audio_url = res.sel.audio->url;
                else if (res.sel.video && res.sel.muxed) s.video_url = res.sel.video->url;
            }

            if (!s.ok() && fail) {
                fail->kind = FailKind::Unknown;
                fail->message = "resolved but produced no URL";
            }
            return s;
        } catch (const std::exception& e) {
            if (fail) { fail->kind = FailKind::Network; fail->message = e.what(); }
            log::warn(std::string("ytcui-dl resolve failed: ") + e.what());
            return {};
        }
    }

    void shutdown() override {
        try {
            ytfast::InnertubeClient::get_instance().shutdown();
        } catch (...) {
            // Shutdown is best-effort; it joins a prefetch thread and closing
            // down is not a place to propagate a failure from.
        }
    }
};

}  // namespace

const char* to_string(FailKind k) {
    switch (k) {
        case FailKind::Network:        return "network";
        case FailKind::NotAvailable:   return "unavailable";
        case FailKind::NeedsToken:     return "needs a PO token";
        case FailKind::NeedsAuth:      return "needs sign-in";
        case FailKind::BackendMissing: return "backend missing";
        case FailKind::Unknown:        return "unknown";
        case FailKind::None:
        default:                       return "ok";
    }
}

std::unique_ptr<Backend> make_ytcuidl_backend() {
    return std::make_unique<YtcuiDlBackend>();
}

}  // namespace ytcui
