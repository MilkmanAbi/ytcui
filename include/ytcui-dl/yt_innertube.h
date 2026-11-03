#pragma once
/*
 * ytcui-dl — yt_innertube.h
 *
 * InnerTube client. No JSON DOM anywhere: requests are built by appending to
 * a string, responses are scanned in place by yj.
 *
 * Client chain (see PLAYER_CHAIN below, current as of August 2026):
 *   VISIONOS    id 101 leads: exempt from the GVS PO Token requirement today
 *   ANDROID_VR  id 28  full adaptive set, useful when VISIONOS is throttled
 *   ANDROID     id 3   full adaptive set, 5.1 audio
 *   IOS         id 5   no 5.1 audio, so ranked last
 * WEB/MWEB/TVHTML5/embedded all return UNPLAYABLE for the player endpoint and
 * are not worth trying for formats -- WEB is still used for visitorData
 * bootstrap and search, where it works fine.
 *
 * Every request carries X-Goog-FieldMask. On the player endpoint that cuts the
 * response from ~183 KB to ~69 KB with an identical format list, because
 * frameworkUpdates and the various renderer trees are pure overhead here.
 */

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <openssl/evp.h>

#include "yj.h"
#include "yt_cache.h"
#include "yt_select.h"
#include "yt_types.h"
#include "yt_http.h"

namespace ytfast {

// ---------------------------------------------------------------------------
// Client definitions
// ---------------------------------------------------------------------------
struct ClientDef {
    const char* name;
    const char* version;
    int         id;
    const char* ua;
    const char* os_name;
    const char* os_version;
    const char* device_model;   // "" to omit
    const char* extra_ctx;      // raw JSON fragment injected into client{}
};

// Version is pinned deliberately. From roughly 21.x onward the player
// endpoint answers ANDROID with a SABR response: status OK, the full
// adaptiveFormats list, and no `url` on any of them -- only initRange/
// indexRange plus a top-level serverAbrStreamingUrl. Playing that requires
// speaking YouTube's UMP/SABR protocol. On 20.10.38 every adaptive format
// still carries a plain, unciphered URL. The tradeoff is that this version
// returns LOGIN_REQUIRED without visitorData, which is why the bootstrap is
// not optional. Re-check with test/test_live if playback ever regresses.
static constexpr ClientDef ANDROID_CLIENT = {
    "ANDROID", "20.10.38", 3,
    "com.google.android.youtube/20.10.38 (Linux; U; Android 14; en_US) gzip",
    "Android", "14", "", "\"androidSdkVersion\":34,"
};
static constexpr ClientDef ANDROID_VR_CLIENT = {
    "ANDROID_VR", "1.65.10", 28,
    "com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android 12L; en_US) gzip",
    "Android", "12L", "Quest 3", "\"androidSdkVersion\":32,"
};
static constexpr ClientDef IOS_CLIENT = {
    "IOS", "20.10.4", 5,
    "com.google.ios.youtube/20.10.4 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X; en_US)",
    "iOS", "18.3.2.22D82", "iPhone16,2", ""
};
// visionOS (Apple Vision Pro). Per yt-dlp's INNERTUBE_CLIENTS table it has no
// GVS_PO_TOKEN_POLICY override, which defaults to not-required -- unlike
// ANDROID/ANDROID_VR/IOS, which all now require one for GVS media fetch (see
// PLAYER_CHAIN below). Confirmed directly: a VISIONOS-signed URL serves an
// entire multi-hundred-MB stream end to end, where the others 403 everything
// past roughly a minute of it.
static constexpr ClientDef VISIONOS_CLIENT = {
    "VISIONOS", "1.02", 101,
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) "
    "Version/26.0 Safari/605.1.15",
    "visionOS", "26.5.23O471", "RealityDevice17,1", "\"deviceMake\":\"Apple\","
};
static constexpr ClientDef WEB_CLIENT = {
    "WEB", "2.20260114.08.00", 1,
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/131.0.0.0 Safari/537.36",
    "Windows", "10.0", "", ""
};

// Player chain, in order.
//
// VISIONOS leads: as of yt-dlp's current INNERTUBE_CLIENTS table it is the
// client with no GVS PO Token requirement. This is a moving target -- it was
// ANDROID_VR until yt-dlp's own changelog notes it started needing one, dated
// 2026-08-17 ("ALL formats... are 403'd with version 1.65.10"), which lines
// up exactly with what this project's own testing found: ANDROID_VR/ANDROID/
// IOS player requests still succeed and hand back plain, non-SABR URLs, but
// the CDN caps how much of the stream those URLs actually serve to roughly a
// minute's worth (scaled by the format's bitrate) before 403ing everything
// further, regardless of connection count, retries, or how long you wait.
// VISIONOS's URLs have no such cap -- confirmed by pulling an entire
// multi-hundred-MB 2160p60 stream through one to completion.
//
// ANDROID_VR/ANDROID/IOS stay in the chain after it: their player responses
// are still useful (VISIONOS can't serve "made for kids" videos either, and a
// clip short enough to fit under the ~60s cap plays fine from any of them).
// If YouTube closes the VISIONOS gap the way it did ANDROID_VR's, whichever
// client fetch_player() actually used is tracked on VideoInfo::client and its
// matching UA travels with every subsequent request for that video -- so the
// fix, next time, is again just reordering this list.
static constexpr const ClientDef* PLAYER_CHAIN[] = {
    &VISIONOS_CLIENT, &ANDROID_VR_CLIENT, &ANDROID_CLIENT, &IOS_CLIENT
};
static constexpr size_t PLAYER_CHAIN_N =
    sizeof(PLAYER_CHAIN) / sizeof(PLAYER_CHAIN[0]);

static const char* INNERTUBE_BASE = "https://www.youtube.com/youtubei/v1/";
static const char* SEARCH_PARAMS  = "EgIQAfABAQ==";  // videos only

// Field masks. Nested paths are silently ignored by the server on the search
// endpoint (it returns an empty body), so search asks for whole `contents`.
static const char* PLAYER_MASK =
    "streamingData,videoDetails,playabilityStatus,responseContext.visitorData";
static const char* SEARCH_MASK = "contents,responseContext.visitorData";

static const int URL_CACHE_TTL_S = 5 * 3600;
static const int VISITOR_TTL_S   = 12 * 3600;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
inline int64_t now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

inline void json_escape_to(std::string_view s, std::string& out) {
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else out += c;
        }
    }
}

inline std::string fmt_duration(int s) {
    if (s <= 0) return "";
    char b[32];
    int h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    if (h) std::snprintf(b, sizeof b, "%d:%02d:%02d", h, m, sec);
    else   std::snprintf(b, sizeof b, "%d:%02d", m, sec);
    return b;
}

inline std::string fmt_views(int64_t v) {
    char b[32];
    if (v >= 1000000000) std::snprintf(b, sizeof b, "%.1fB", v / 1e9);
    else if (v >= 1000000) std::snprintf(b, sizeof b, "%.1fM", v / 1e6);
    else if (v >= 1000)    std::snprintf(b, sizeof b, "%.1fK", v / 1e3);
    else                   std::snprintf(b, sizeof b, "%lld", (long long)v);
    return b;
}

inline int64_t parse_vc(std::string_view s) {
    int64_t n = 0;
    for (char c : s) if (c >= '0' && c <= '9') n = n * 10 + (c - '0');
    return n;
}

// "1:02:03" / "6:10:58" -> seconds
inline int parse_dur(std::string_view s) {
    int total = 0, cur = 0;
    for (char c : s) {
        if (c >= '0' && c <= '9') cur = cur * 10 + (c - '0');
        else if (c == ':') { total = total * 60 + cur; cur = 0; }
    }
    return total * 60 + cur;
}

// InnerTube text nodes are either {simpleText} or {runs:[{text}...]}.
inline std::string text_of(yj::Val node) {
    if (!node.valid()) return {};
    if (yj::Val st = yj::get(node, "simpleText"); st.is_str())
        return yj::unescape(st.raw());
    yj::Val runs = yj::get(node, "runs");
    if (!runs.is_arr()) return {};
    std::string out;
    yj::each_elem(runs, [&](yj::Val r) {
        if (yj::Val t = yj::get(r, "text"); t.is_str())
            yj::unescape_to(t.raw(), out);
        return true;
    });
    return out;
}

inline std::string gen_cpn() {
    static const char* alpha =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> d(0, 63);
    std::string r(16, ' ');
    for (auto& c : r) c = alpha[d(rng)];
    return r;
}

inline std::vector<ThumbnailInfo> thumbnail_candidates(const std::string& id) {
    static const struct { const char* f; int w, h; } kT[] = {
        {"maxresdefault.jpg", 1280, 720}, {"sddefault.jpg", 640, 480},
        {"hqdefault.jpg", 480, 360},      {"mqdefault.jpg", 320, 180},
        {"default.jpg", 120, 90},
    };
    std::vector<ThumbnailInfo> out;
    out.reserve(5);
    for (auto& t : kT)
        out.push_back({"https://i.ytimg.com/vi/" + id + "/" + t.f, t.w, t.h});
    return out;
}

// ---------------------------------------------------------------------------
// URL cache
// ---------------------------------------------------------------------------
class UrlCache {
public:
    bool get(const std::string& id, VideoInfo& out) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = cache_.find(id);
        if (it == cache_.end()) return false;
        if (now_s() - it->second.t > URL_CACHE_TTL_S) { cache_.erase(it); return false; }
        out = it->second.info;
        return true;
    }
    void put(const std::string& id, const VideoInfo& info) {
        std::lock_guard<std::mutex> lk(mu_);
        cache_[id] = {info, now_s()};
    }
private:
    struct Entry { VideoInfo info; int64_t t; };
    std::mutex mu_;
    std::unordered_map<std::string, Entry> cache_;
};

// ---------------------------------------------------------------------------
// InnertubeClient
// ---------------------------------------------------------------------------
class InnertubeClient {
public:
    InnertubeClient() = default;

    static InnertubeClient& get_instance() {
        static InnertubeClient inst;
        return inst;
    }

    // -----------------------------------------------------------------------
    // visitor data
    // -----------------------------------------------------------------------
    void bootstrap_visitor_data() {
        std::lock_guard<std::mutex> lk(vd_mu_);
        if (!visitor_.empty() && now_s() - vd_t_ < VISITOR_TTL_S) return;

        // Disk first. This value is good for hours and the fetch is a full
        // HTTPS round trip, which for a run-and-exit CLI was ~40% of the time
        // to first stream on every invocation.
        if (use_disk_cache_) {
            std::string cached;
            if (DiskCache::get(kVisitorKey, cached, VISITOR_TTL_S) && !cached.empty()) {
                visitor_ = std::move(cached);
                vd_t_ = now_s();
                return;
            }
        }

        // A minimal /guide call returns visitorData in responseContext and is
        // far cheaper than scraping the homepage HTML, which also tends to hit
        // consent interstitials from datacenter IPs.
        std::string body = "{\"context\":";
        append_context(body, WEB_CLIENT, "");
        body += "}";
        try {
            auto hdrs = headers_for(WEB_CLIENT, "responseContext.visitorData");
            auto extra = auth_headers_locked();  // vd_mu_ already held here
            hdrs.insert(hdrs.end(), extra.begin(), extra.end());
            auto r = get_thread_http().post(
                std::string(INNERTUBE_BASE) + "guide?prettyPrint=false", body, hdrs);
            if (r.status == 200) {
                yj::Val vd = yj::path(yj::parse(r.body), "responseContext", "visitorData");
                if (vd.is_str()) {
                    visitor_ = yj::unescape(vd.raw());
                    vd_t_ = now_s();
                    if (use_disk_cache_ && !visitor_.empty())
                        DiskCache::put(kVisitorKey, visitor_);
                }
            }
        } catch (...) {}
    }

    std::string visitor_data() {
        std::lock_guard<std::mutex> lk(vd_mu_);
        return visitor_;
    }

    // Optional Proof-of-Origin token. We cannot mint one ourselves -- it's a
    // BotGuard JS attestation, and yt-dlp doesn't mint one in-house either
    // (its core repo ships zero token-minting logic; every real provider is
    // an external plugin). Leaving it unset is fine today: the chain leads
    // with VISIONOS, which the current GVS PO Token policy doesn't require.
    // If that gap closes, supply one via set_po_token() (pasted from
    // somewhere that can solve the challenge) or fetch_po_token_from_provider()
    // below (delegates to a local provider, the way yt-dlp itself does).
    // Disable the on-disk visitorData cache (embedded targets with no writable
    // filesystem, or callers that want a guaranteed-fresh token).
    void set_disk_cache(bool on) {
        std::lock_guard<std::mutex> lk(vd_mu_);
        use_disk_cache_ = on;
    }

    void set_po_token(std::string tok) {
        std::lock_guard<std::mutex> lk(vd_mu_);
        po_token_ = std::move(tok);
    }
    std::string po_token() {
        std::lock_guard<std::mutex> lk(vd_mu_);
        return po_token_;
    }

    // Loads a Netscape-format cookies.txt (the convention curl, yt-dlp, and
    // every "export cookies" browser extension already use). Cookies whose
    // domain is youtube.com or google.com are sent as a Cookie header on
    // every InnerTube request from here on; if a SAPISID (or
    // __Secure-3PAPISID) cookie is present, requests are additionally signed
    // with a SAPISIDHASH Authorization header, the same origin-bound hash a
    // real logged-in browser sends. This is real, working authentication --
    // it unlocks age-gated/members-only/private videos and a logged-in
    // WEB session -- but it is *not* a PO token: cookies alone don't produce
    // one (see fetch_po_token_from_provider). Returns false if the file
    // couldn't be read or had no youtube/google cookies in it.
    bool set_cookies_file(const std::string& path) {
        std::ifstream f(path);
        if (!f) return false;
        std::string cookies, sapisid, line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::string_view sv(line);
            if (sv.rfind("#HttpOnly_", 0) == 0) sv.remove_prefix(10);
            else if (sv[0] == '#') continue;
            std::vector<std::string_view> field;
            field.reserve(7);
            size_t start = 0;
            for (size_t i = 0; i <= sv.size(); ++i) {
                if (i == sv.size() || sv[i] == '\t') {
                    field.push_back(sv.substr(start, i - start));
                    start = i + 1;
                }
            }
            if (field.size() < 7) continue;
            std::string_view domain = field[0], name = field[5], value = field[6];
            if (domain.find("youtube.com") == std::string_view::npos &&
                domain.find("google.com") == std::string_view::npos) continue;
            if (!cookies.empty()) cookies += "; ";
            cookies.append(name); cookies += '='; cookies.append(value);
            if (name == "SAPISID" || name == "__Secure-3PAPISID")
                sapisid = std::string(value);
        }
        if (cookies.empty()) return false;
        std::lock_guard<std::mutex> lk(vd_mu_);
        cookie_header_ = std::move(cookies);
        sapisid_ = std::move(sapisid);
        return true;
    }

    // Queries an external PO-Token provider over HTTP for a token bound to
    // our visitorData -- the exact request yt-dlp's own bgutil plugin makes
    // to https://github.com/Brainicism/bgutil-ytdlp-pot-provider running
    // locally (`docker run ... brainicism/bgutil-ytdlp-pot-provider`, or
    // `node build/main.js`). We don't solve the BotGuard challenge ourselves
    // -- nothing does without executing Google's JS, yt-dlp included -- this
    // just talks to something that already can. Returns false (and leaves
    // playback to proceed without a token) if the provider isn't reachable.
    bool fetch_po_token_from_provider(const std::string& base_url = "http://127.0.0.1:4416") {
        ensure_visitor_data();
        std::string vd = get_vd();
        if (vd.empty()) return false;
        std::string body = "{\"content_binding\":\"";
        json_escape_to(vd, body);
        body += "\"}";
        try {
            HttpClient h;
            h.set_timeouts(3000, 8000);
            auto r = h.post(base_url + "/get_pot", body, {"Content-Type: application/json"});
            if (r.status != 200) return false;
            yj::Val tok = yj::path(yj::parse(r.body), "poToken");
            if (!tok.is_str()) return false;
            std::string t(yj::unescape(tok.raw()));

            // The player endpoint decodes this field as raw protobuf bytes:
            // confirmed live that a string that doesn't survive base64
            // decoding doesn't just get ignored, it 400s the *entire* player
            // request -- every client in the chain, not just this one. A real
            // token (base64url, well over 100 chars) is nowhere near this
            // boundary; this only exists to keep a broken or misconfigured
            // provider from silently taking down every client at once.
            const bool plausible = t.size() >= 40 && std::all_of(t.begin(), t.end(),
                [](unsigned char ch) { return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '='; });
            if (!plausible) return false;

            set_po_token(std::move(t));
            return true;
        } catch (...) { return false; }
    }

    // -----------------------------------------------------------------------
    // search
    // -----------------------------------------------------------------------
    std::vector<SearchResult> search(const std::string& query,
                                     int max_results = 15,
                                     bool prefetch_top = true) {
        ensure_visitor_data();

        std::string body = "{\"context\":";
        append_context(body, ANDROID_CLIENT, get_vd());
        body += ",\"query\":\"";
        json_escape_to(query, body);
        body += "\",\"params\":\"";
        body += SEARCH_PARAMS;
        body += "\"}";

        std::vector<SearchResult> out;
        HttpClient::Response r;
        try {
            auto hdrs = headers_for(ANDROID_CLIENT, SEARCH_MASK);
            auto extra = auth_headers();
            hdrs.insert(hdrs.end(), extra.begin(), extra.end());
            r = get_thread_http().post(
                std::string(INNERTUBE_BASE) + "search?prettyPrint=false", body, hdrs);
        } catch (...) { return out; }
        if (r.status != 200) return out;

        out.reserve(max_results > 0 ? max_results : 20);
        parse_search_into(r.body, max_results, out);

        if (prefetch_top && !out.empty()) prefetch(out[0].id);
        return out;
    }

    // Split out so it can be tested against a fixture without a socket.
    static void parse_search_into(std::string_view body, int max_results,
                                  std::vector<SearchResult>& out) {
        // ANDROID uses compactVideoRenderer, WEB uses videoRenderer.
        static const std::string_view kR[] = {"videoRenderer", "compactVideoRenderer"};
        // ~87% of a search response is these subtrees and we read none of it.
        static const std::string_view kSkip[] = {
            "menu", "navigationEndpoint", "trackingParams", "accessibility",
            "thumbnailOverlays", "channelThumbnailSupportedRenderers",
            "badges", "ownerBadges", "richThumbnail", "inlinePlaybackEndpoint",
        };
        yj::find_any(yj::parse(body), kR, 2, [&](std::string_view, yj::Val vr) {
            if (max_results > 0 && (int)out.size() >= max_results) return false;
            SearchResult s;
            yj::each_member(vr, [&](std::string_view k, yj::Val v) {
                if      (yj::keyis(k, "videoId"))            s.id = yj::unescape(v.raw());
                else if (yj::keyis(k, "title"))              s.title = text_of(v);
                else if (yj::keyis(k, "longBylineText"))     s.channel = text_of(v);
                else if (yj::keyis(k, "shortBylineText"))  { if (s.channel.empty()) s.channel = text_of(v); }
                else if (yj::keyis(k, "ownerText"))        { if (s.channel.empty()) s.channel = text_of(v); }
                else if (yj::keyis(k, "lengthText"))        s.duration_str = text_of(v);
                else if (yj::keyis(k, "publishedTimeText"))  s.upload_date = text_of(v);
                else if (yj::keyis(k, "viewCountText"))      s.view_count_str = text_of(v);
                else if (yj::keyis(k, "shortViewCountText")){ if (s.view_count_str.empty()) s.view_count_str = text_of(v); }
                return true;
            });
            if (s.id.empty()) return true;
            s.duration_secs = parse_dur(s.duration_str);
            s.is_live       = s.duration_str.empty();
            s.view_count    = parse_vc(s.view_count_str);
            s.url           = "https://www.youtube.com/watch?v=" + s.id;
            s.thumbnails    = thumbnail_candidates(s.id);
            s.thumbnail_url = s.thumbnails.front().url;
            out.push_back(std::move(s));
            return true;
        }, kSkip, sizeof(kSkip) / sizeof(*kSkip));
    }

    // -----------------------------------------------------------------------
    // player
    // -----------------------------------------------------------------------
    VideoInfo get_stream_formats(const std::string& video_id) {
        VideoInfo cached;
        if (cache_.get(video_id, cached)) return cached;
        VideoInfo info = fetch_player(video_id);
        if (!info.formats.empty()) cache_.put(video_id, info);
        return info;
    }

    // Queue a background warm-up of the URL cache.
    //
    // This used to spawn a detached std::thread per call. Each one paid a
    // fresh ~8 KB stack plus a full TLS handshake, because get_thread_http()
    // is thread_local and a brand new thread always has a cold connection --
    // so the "fast path" was doing the single most expensive thing available.
    // Scrolling a result list spawned a thread per item, unbounded and
    // unjoinable, and detached threads still running at exit race with the
    // static destructors of the cache and the SSL context.
    //
    // Now: one worker thread, started on first use, with a small dedup'd
    // queue. It reuses a single keep-alive connection for every prefetch and
    // is joined in the destructor.
    void prefetch(const std::string& video_id) {
        if (video_id.empty()) return;
        VideoInfo tmp;
        if (cache_.get(video_id, tmp)) return;
        {
            std::lock_guard<std::mutex> lk(q_mu_);
            if (q_stop_) return;
            for (const auto& q : queue_) if (q == video_id) return;  // already queued
            if (queue_.size() >= kMaxQueued) queue_.pop_front();     // drop oldest
            queue_.push_back(video_id);
        }
        start_worker();
        q_cv_.notify_one();
    }

    ~InnertubeClient() { stop_worker(); }

    // Stop and join the background prefetch worker.
    //
    // Callers embedding this in a TUI want a hard barrier before process
    // teardown: a worker still mid-request while static destructors run will
    // touch the URL cache and the SSL context after they are gone. The
    // destructor already does this, but static destruction order across
    // translation units is not guaranteed, so an explicit call from main() is
    // the reliable version. Idempotent.
    void shutdown() { stop_worker(); }

    InnertubeClient(const InnertubeClient&) = delete;
    InnertubeClient& operator=(const InnertubeClient&) = delete;

    // -----------------------------------------------------------------------
    // Selection. These are what actually fixed the quality problem: the old
    // versions preferred muxed streams, and muxed on a modern upload is a
    // single 360p/128k itag 18 no matter what the video really offers.
    // -----------------------------------------------------------------------

    // Best audio, adaptive-only. Non-DRC preferred, then bitrate.
    static const StreamFormat* pick_audio(const std::vector<StreamFormat>& fmts,
                                          std::string_view prefer_codec = {},
                                          bool allow_surround = true) {
        const StreamFormat* best = nullptr;
        for (auto& f : fmts) {
            if (!f.is_audio_only()) continue;
            if (!allow_surround && f.audio_channels > 2) continue;
            if (!prefer_codec.empty() && !codec_matches(f, prefer_codec)) continue;
            if (!best || better_audio(f, *best)) best = &f;
        }
        if (best) return best;
        if (!prefer_codec.empty())  // retry without the codec constraint
            return pick_audio(fmts, {}, allow_surround);
        for (auto& f : fmts)        // last resort: muxed
            if (f.has_audio && (!best || better_audio(f, *best))) best = &f;
        return best;
    }

    static const StreamFormat* pick_video(const std::vector<StreamFormat>& fmts,
                                          int max_h = 0,
                                          std::string_view prefer_codec = {}) {
        const StreamFormat* best = nullptr;
        for (auto& f : fmts) {
            if (!f.is_video_only()) continue;
            if (max_h > 0 && f.height > max_h) continue;
            if (!prefer_codec.empty() && !codec_matches(f, prefer_codec)) continue;
            if (!best || better_video(f, *best)) best = &f;
        }
        if (best) return best;
        if (!prefer_codec.empty()) return pick_video(fmts, max_h, {});
        for (auto& f : fmts) {      // fall back to muxed
            if (!f.has_video) continue;
            if (max_h > 0 && f.height > max_h) continue;
            if (!best || better_video(f, *best)) best = &f;
        }
        return best;
    }

    static const StreamFormat* pick_muxed(const std::vector<StreamFormat>& fmts,
                                          int max_h = 0) {
        const StreamFormat* best = nullptr;
        for (auto& f : fmts) {
            if (!f.is_muxed()) continue;
            if (max_h > 0 && f.height > max_h) continue;
            if (!best || better_video(f, *best)) best = &f;
        }
        return best;
    }

    static const StreamFormat* find_by_itag(const std::vector<StreamFormat>& fmts, int itag) {
        for (auto& f : fmts) if (f.itag == itag) return &f;
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // yt-dlp-style format strings.
    //   best / b            best muxed, else best video+audio pair by height
    //   bestvideo / bv      best video-only
    //   bestaudio / ba      best audio-only
    //   worst*/w*           the same, inverted
    //   137 / 251           a literal itag
    //   bv+ba               pair; returns both urls newline-separated
    //   any of the above with [height<=720] [abr>=128] [ext=webm] [fps=60]
    // Returns "" when nothing matches.
    // -----------------------------------------------------------------------
    static std::string resolve_format_string(const std::string& sel,
                                             const std::vector<StreamFormat>& fmts,
                                             bool /*for_streaming*/ = false) {
        if (fmts.empty()) return {};

        // "a+b" -> resolve each side, join with a newline
        size_t plus = sel.find('+');
        if (plus != std::string::npos && sel.find('[') > plus) {
            std::string l = resolve_format_string(sel.substr(0, plus), fmts);
            std::string r = resolve_format_string(sel.substr(plus + 1), fmts);
            if (l.empty()) return r;
            if (r.empty()) return l;
            return l + "\n" + r;
        }
        // "a/b" -> first that resolves
        size_t slash = sel.find('/');
        if (slash != std::string::npos && sel.find('[') > slash) {
            std::string l = resolve_format_string(sel.substr(0, slash), fmts);
            return l.empty() ? resolve_format_string(sel.substr(slash + 1), fmts) : l;
        }

        std::string base = sel;
        std::vector<Filter> filters;
        size_t br = sel.find('[');
        if (br != std::string::npos) {
            base = sel.substr(0, br);
            size_t pos = br;
            while (pos < sel.size()) {
                size_t o = sel.find('[', pos);
                if (o == std::string::npos) break;
                size_t c = sel.find(']', o);
                if (c == std::string::npos) break;
                Filter f;
                if (parse_filter(sel.substr(o + 1, c - o - 1), f)) filters.push_back(f);
                pos = c + 1;
            }
        }

        // A bare number is an itag.
        if (!base.empty() && base.find_first_not_of("0123456789") == std::string::npos) {
            if (auto* f = find_by_itag(fmts, atoi(base.c_str()))) return f->url;
            return {};
        }

        bool worst = base.rfind("worst", 0) == 0 || base == "w" ||
                     base == "wv" || base == "wa";
        enum { ANY, VIDEO, AUDIO } want = ANY;
        if (base == "bestvideo" || base == "bv" || base == "worstvideo" || base == "wv") want = VIDEO;
        else if (base == "bestaudio" || base == "ba" || base == "worstaudio" || base == "wa") want = AUDIO;

        const StreamFormat* pick = nullptr;
        for (auto& f : fmts) {
            if (want == VIDEO && !f.is_video_only()) continue;
            if (want == AUDIO && !f.is_audio_only()) continue;
            if (!passes(f, filters)) continue;
            if (!pick) { pick = &f; continue; }
            bool b = (want == AUDIO) ? better_audio(f, *pick) : better_video(f, *pick);
            if (worst ? !b : b) pick = &f;
        }
        // "best" with no adaptive match: fall back across the whole set.
        if (!pick && want == ANY) {
            for (auto& f : fmts) {
                if (!passes(f, filters)) continue;
                if (!pick || (worst ? !better_video(f, *pick) : better_video(f, *pick)))
                    pick = &f;
            }
        }
        return pick ? pick->url : std::string{};
    }

private:
    struct Filter { std::string key; int op; int64_t num; std::string str; };
    enum { OP_EQ, OP_NE, OP_LE, OP_GE, OP_LT, OP_GT };

    static bool parse_filter(const std::string& e, Filter& f) {
        static const struct { const char* s; int op; } kOps[] = {
            {"<=", OP_LE}, {">=", OP_GE}, {"!=", OP_NE},
            {"<", OP_LT}, {">", OP_GT}, {"=", OP_EQ},
        };
        for (auto& o : kOps) {
            size_t p = e.find(o.s);
            if (p == std::string::npos) continue;
            f.key = e.substr(0, p);
            f.op  = o.op;
            f.str = e.substr(p + strlen(o.s));
            f.num = atoll(f.str.c_str());
            return !f.key.empty();
        }
        return false;
    }

    static bool cmp(int64_t a, int op, int64_t b) {
        switch (op) {
            case OP_EQ: return a == b; case OP_NE: return a != b;
            case OP_LE: return a <= b; case OP_GE: return a >= b;
            case OP_LT: return a <  b; default:    return a >  b;
        }
    }

    static bool passes(const StreamFormat& f, const std::vector<Filter>& fs) {
        for (auto& fl : fs) {
            if (fl.key == "height")      { if (!cmp(f.height, fl.op, fl.num)) return false; }
            else if (fl.key == "width")  { if (!cmp(f.width, fl.op, fl.num)) return false; }
            else if (fl.key == "fps")    { if (!cmp(f.fps, fl.op, fl.num)) return false; }
            else if (fl.key == "abr" || fl.key == "tbr" || fl.key == "vbr") {
                if (!cmp(f.effective_bitrate() / 1000, fl.op, fl.num)) return false;
            } else if (fl.key == "filesize") {
                if (!cmp(f.content_length, fl.op, fl.num)) return false;
            } else if (fl.key == "ext" || fl.key == "container") {
                bool eq = f.container == fl.str ||
                          (fl.str == "m4a" && f.container == "mp4" && f.is_audio_only());
                if (eq != (fl.op != OP_NE)) return false;
            } else if (fl.key == "acodec") {
                bool eq = f.audio_codec.rfind(fl.str, 0) == 0;
                if (eq != (fl.op != OP_NE)) return false;
            } else if (fl.key == "vcodec") {
                bool eq = f.video_codec.rfind(fl.str, 0) == 0;
                if (eq != (fl.op != OP_NE)) return false;
            }
        }
        return true;
    }

public:
    static bool is_muxed(const std::vector<StreamFormat>& fmts, const std::string& url) {
        for (auto& f : fmts) if (f.url == url) return f.is_muxed();
        return false;
    }
    static std::string select_best_video(const std::vector<StreamFormat>& fmts, int max_h = 0) {
        auto* f = pick_video(fmts, max_h);
        return f ? f->url : std::string{};
    }
    static std::string select_best_audio(const std::vector<StreamFormat>& fmts) {
        auto* f = pick_audio(fmts);
        return f ? f->url : std::string{};
    }
    static std::string select_best_video_stream(const std::vector<StreamFormat>& fmts,
                                                int max_h = 1080) {
        auto* f = pick_video(fmts, max_h);
        return f ? f->url : std::string{};
    }
    static std::string select_best_audio_stream(const std::vector<StreamFormat>& fmts) {
        auto* f = pick_audio(fmts);
        return f ? f->url : std::string{};
    }
    static std::string select_best_audio_download(const std::vector<StreamFormat>& fmts,
                                                  const std::string& prefer = "",
                                                  int = 0, bool = true) {
        auto* f = pick_audio(fmts, prefer);
        return f ? f->url : std::string{};
    }
    static std::string select_best_video_only(const std::vector<StreamFormat>& fmts,
                                              int max_h = 0) {
        auto* f = pick_video(fmts, max_h);
        return f ? f->url : std::string{};
    }
    static std::string select_worst_audio(const std::vector<StreamFormat>& fmts) {
        const StreamFormat* worst = nullptr;
        for (auto& f : fmts) {
            if (!f.is_audio_only()) continue;
            if (!worst || f.effective_bitrate() < worst->effective_bitrate()) worst = &f;
        }
        return worst ? worst->url : std::string{};
    }

    // -----------------------------------------------------------------------
    // Mode-aware resolution. This is the API a player or TUI should use: one
    // call gives back the streams for the requested mode plus everything
    // needed to draw a quality menu.
    // -----------------------------------------------------------------------
    struct Resolved {
        VideoInfo  info;
        Selection  sel;
        std::vector<Rung> ladder;
        bool ok() const { return sel.ok(); }
    };

    // Query one specific client, bypassing the chain and the cache. For
    // diagnostics: the point is to see what each client says, including the
    // ones the chain would have skipped.
    VideoInfo probe_client(const std::string& video_id, const ClientDef& c) {
        ensure_visitor_data();
        try { return player_request(video_id, c); }
        catch (...) { return {}; }
    }

    Resolved resolve(const std::string& video_id, Mode mode, const Quality& q = {}) {
        Resolved r;
        r.info = get_stream_formats(video_id);
        if (r.info.formats.empty()) return r;
        r.sel    = Selector::select(r.info.formats, mode, q);
        r.ladder = Selector::ladder(r.info.formats);
        return r;
    }

    // Convenience wrappers, unchanged signatures.
    std::string get_best_audio_url(const std::string& vid) {
        auto info = get_stream_formats(vid);
        auto* f = pick_audio(info.formats);
        return f ? f->url : std::string{};
    }
    std::string get_best_audio_stream_url(const std::string& vid) {
        return get_best_audio_url(vid);
    }
    std::string get_best_video_url(const std::string& vid, int max_h = 0) {
        auto info = get_stream_formats(vid);
        auto* f = pick_video(info.formats, max_h);
        return f ? f->url : std::string{};
    }
    std::string get_best_video_stream_url(const std::string& vid, int max_h = 0) {
        return get_best_video_url(vid, max_h);
    }

private:
    static constexpr size_t kMaxQueued = 8;

    // Prefetch worker
    std::mutex               q_mu_;
    std::condition_variable  q_cv_;
    std::deque<std::string>  queue_;
    std::thread              worker_;
    bool                     q_stop_    = false;
    bool                     q_started_ = false;

    void start_worker() {
        std::lock_guard<std::mutex> lk(q_mu_);
        if (q_started_ || q_stop_) return;
        q_started_ = true;
        worker_ = std::thread([this] {
            for (;;) {
                std::string id;
                {
                    std::unique_lock<std::mutex> wait_lk(q_mu_);
                    q_cv_.wait(wait_lk, [this] { return q_stop_ || !queue_.empty(); });
                    if (q_stop_ && queue_.empty()) return;
                    if (queue_.empty()) continue;
                    id = std::move(queue_.front());
                    queue_.pop_front();
                }
                // One thread, so one thread_local HttpClient, so one warm
                // TLS session reused across every prefetch.
                try { get_stream_formats(id); } catch (...) {}
            }
        });
    }

    void stop_worker() {
        {
            std::lock_guard<std::mutex> lk(q_mu_);
            if (!q_started_) { q_stop_ = true; return; }
            q_stop_ = true;
            queue_.clear();
        }
        q_cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    UrlCache          cache_;
    std::mutex        vd_mu_;
    std::string       visitor_;
    std::string       po_token_;
    std::string       cookie_header_;
    std::string       sapisid_;
    int64_t           vd_t_ = 0;
    bool              use_disk_cache_ = true;
    static constexpr const char* kVisitorKey = "visitor_data";

    static std::string sha1_hex(const std::string& s) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
        EVP_DigestUpdate(ctx, s.data(), s.size());
        EVP_DigestFinal_ex(ctx, digest, &len);
        EVP_MD_CTX_free(ctx);
        static const char* hexch = "0123456789abcdef";
        std::string out;
        out.reserve(len * 2);
        for (unsigned int i = 0; i < len; ++i) {
            out.push_back(hexch[digest[i] >> 4]);
            out.push_back(hexch[digest[i] & 0xF]);
        }
        return out;
    }

    // Cookie/SAPISIDHASH headers to merge onto every InnerTube request, once
    // set_cookies_file() has loaded something. Empty when no cookies are
    // loaded, so every call site can unconditionally append this vector.
    // Caller must already hold vd_mu_.
    std::vector<std::string> auth_headers_locked() {
        std::vector<std::string> h;
        if (cookie_header_.empty()) return h;
        h.emplace_back("Cookie: " + cookie_header_);
        if (!sapisid_.empty()) {
            const int64_t ts = now_s();
            static constexpr const char* kOrigin = "https://www.youtube.com";
            const std::string hash = sha1_hex(std::to_string(ts) + " " + sapisid_ + " " + kOrigin);
            h.emplace_back("Authorization: SAPISIDHASH " + std::to_string(ts) + "_" + hash);
            h.emplace_back(std::string("X-Origin: ") + kOrigin);
        }
        return h;
    }
    std::vector<std::string> auth_headers() {
        std::lock_guard<std::mutex> lk(vd_mu_);
        return auth_headers_locked();
    }

    void ensure_visitor_data() {
        { std::lock_guard<std::mutex> lk(vd_mu_);
          if (!visitor_.empty() && now_s() - vd_t_ < VISITOR_TTL_S) return; }
        bootstrap_visitor_data();
    }
    std::string get_vd() { std::lock_guard<std::mutex> lk(vd_mu_); return visitor_; }

    // -----------------------------------------------------------------------
    // Request construction — plain string append, no serializer.
    // -----------------------------------------------------------------------
    static void append_context(std::string& b, const ClientDef& c,
                               const std::string& visitor) {
        b += "{\"client\":{\"clientName\":\"";
        b += c.name;
        b += "\",\"clientVersion\":\"";
        b += c.version;
        b += "\",\"osName\":\"";
        b += c.os_name;
        b += "\",\"osVersion\":\"";
        b += c.os_version;
        b += "\",";
        if (c.device_model[0]) {
            b += "\"deviceModel\":\"";
            b += c.device_model;
            b += "\",";
        }
        b += c.extra_ctx;
        b += "\"hl\":\"en\",\"gl\":\"US\",\"userAgent\":\"";
        json_escape_to(c.ua, b);
        b += "\"";
        if (!visitor.empty()) {
            b += ",\"visitorData\":\"";
            json_escape_to(visitor, b);
            b += "\"";
        }
        b += "}}";
    }

    static std::vector<std::string> headers_for(const ClientDef& c, const char* mask) {
        std::vector<std::string> h;
        h.reserve(6);
        h.emplace_back(std::string("X-YouTube-Client-Name: ") + std::to_string(c.id));
        h.emplace_back(std::string("X-YouTube-Client-Version: ") + c.version);
        h.emplace_back(std::string("User-Agent: ") + c.ua);
        h.emplace_back("Origin: https://www.youtube.com");
        if (mask && *mask) h.emplace_back(std::string("X-Goog-FieldMask: ") + mask);
        return h;
    }

    VideoInfo fetch_player(const std::string& video_id) {
        ensure_visitor_data();
        VideoInfo fallback;
        for (size_t i = 0; i < PLAYER_CHAIN_N; ++i) {
            try {
                VideoInfo v = player_request(video_id, *PLAYER_CHAIN[i]);
                // A client that returns adaptive formats we can actually fetch
                // wins outright.
                for (auto& f : v.formats)
                    if (f.is_video_only() || f.is_audio_only()) return v;
                // Muxed-only or SABR-gated: keep it as a last resort but keep
                // looking for a client that gives us the real format list.
                if (!v.formats.empty() && fallback.formats.empty())
                    fallback = std::move(v);
            } catch (...) {}
        }
        return fallback;
    }

    VideoInfo player_request(const std::string& vid, const ClientDef& c) {
        std::string body = "{\"context\":";
        append_context(body, c, get_vd());
        body += ",\"videoId\":\"";
        json_escape_to(vid, body);
        body += "\",\"contentCheckOk\":true,\"racyCheckOk\":true"
                ",\"playbackContext\":{\"contentPlaybackContext\":"
                "{\"html5Preference\":\"HTML5_PREF_WANTS\"}}"
                ",\"cpn\":\"";
        body += gen_cpn();
        body += "\"";
        if (std::string pot = po_token(); !pot.empty()) {
            body += ",\"serviceIntegrityDimensions\":{\"poToken\":\"";
            json_escape_to(pot, body);
            body += "\"}";
        }
        body += "}";

        auto hdrs = headers_for(c, PLAYER_MASK);
        auto extra = auth_headers();
        hdrs.insert(hdrs.end(), extra.begin(), extra.end());
        auto r = get_thread_http().post(
            std::string(INNERTUBE_BASE) + "player?prettyPrint=false", body, hdrs);
        if (r.status != 200) return {};

        VideoInfo info;
        parse_player_into(r.body, vid, info);
        info.client_name = c.name;
        info.client_ua   = c.ua;

        // Player-context PO tokens (above, in serviceIntegrityDimensions) can
        // unlock the format *list*; the GVS-context token that authorizes
        // actually fetching the bytes is a URL query param on each format's
        // own URL, appended here so every caller downstream -- download,
        // --play, --get-url -- gets it for free.
        if (std::string pot = po_token(); !pot.empty()) {
            const std::string suffix = "&pot=" + HttpClient::url_encode(pot);
            for (auto& f : info.formats) if (!f.url.empty()) f.url += suffix;
        }
        return info;
    }

public:
    // Public + static so the test suite can run it on a fixture.
    static void parse_player_into(std::string_view body, const std::string& vid,
                                  VideoInfo& info) {
        yj::Val root = yj::parse(body);
        if (!root.is_obj()) return;

        // Playability gate. LOGIN_REQUIRED / UNPLAYABLE / AGE_VERIFY all mean
        // "try the next client", signalled by leaving formats empty.
        if (yj::Val ps = yj::get(root, "playabilityStatus"); ps.is_obj()) {
            yj::Val st = yj::get(ps, "status");
            if (st.is_str() && st.raw() != "OK" && st.raw() != "LIVE_STREAM_OFFLINE")
                return;
        }

        info.id  = vid;
        info.url = "https://www.youtube.com/watch?v=" + vid;

        if (yj::Val vd = yj::get(root, "videoDetails"); vd.is_obj()) {
            yj::each_member(vd, [&](std::string_view k, yj::Val v) {
                if      (yj::keyis(k, "title"))        info.title       = yj::unescape(v.raw());
                else if (yj::keyis(k, "author"))       info.channel     = yj::unescape(v.raw());
                else if (yj::keyis(k, "channelId"))    info.channel_id  = yj::unescape(v.raw());
                else if (yj::keyis(k, "shortDescription")) info.description = yj::unescape(v.raw());
                else if (yj::keyis(k, "viewCount"))    info.view_count  = v.i64();
                else if (yj::keyis(k, "isLiveContent"))info.is_live     = v.boolean();
                else if (yj::keyis(k, "lengthSeconds"))info.duration_secs = (int)v.i64();
                return true;
            });
        }
        info.duration_str   = fmt_duration(info.duration_secs);
        info.view_count_str = fmt_views(info.view_count);
        info.thumbnails     = thumbnail_candidates(vid);
        info.thumbnail_url  = info.thumbnails.front().url;

        yj::Val sd = yj::get(root, "streamingData");
        if (!sd.is_obj()) return;

        // SABR gate. A response can be perfectly OK and still be useless to a
        // plain HTTP downloader: adaptiveFormats is fully populated but every
        // entry lacks a url. Left unchecked we keep the one muxed 360p format,
        // formats is non-empty, and the client chain never falls through -- so
        // the user silently gets 360p on a 4K video. Count it and bail.
        {
            int adaptive = 0, with_url = 0;
            yj::each_elem(yj::get(sd, "adaptiveFormats"), [&](yj::Val f) {
                ++adaptive;
                if (yj::get(f, "url").is_str()) ++with_url;
                return true;
            });
            if (adaptive > 0 && with_url == 0) { info.sabr_only = true; return; }
        }

        info.formats.reserve(48);
        StrArena& arena = info.ensure_arena();
        for (const char* key : {"formats", "adaptiveFormats"}) {
            yj::Val arr = yj::get(sd, key);
            if (!arr.is_arr()) continue;
            yj::each_elem(arr, [&](yj::Val f) {
                StreamFormat sf;
                if (parse_format(f, arena, sf)) info.formats.push_back(std::move(sf));
                return true;
            });
        }
        dedup_drc(info.formats);
    }

private:
    // Returns false for formats we can't use: ciphered, SABR-only, no URL.
    static bool parse_format(yj::Val f, StrArena& arena, StreamFormat& sf) {
        std::string_view mime_raw, url_raw;
        bool ciphered = false;

        yj::each_member(f, [&](std::string_view k, yj::Val v) {
            if      (yj::keyis(k, "itag"))            sf.itag = (int)v.i64();
            else if (yj::keyis(k, "url"))             url_raw = v.raw();
            else if (yj::keyis(k, "mimeType"))        mime_raw = v.raw();
            else if (yj::keyis(k, "bitrate"))         sf.bitrate = v.i64();
            else if (yj::keyis(k, "averageBitrate"))  sf.average_bitrate = v.i64();
            else if (yj::keyis(k, "contentLength"))   sf.content_length = v.i64();
            else if (yj::keyis(k, "width"))           sf.width = (int)v.i64();
            else if (yj::keyis(k, "height"))          sf.height = (int)v.i64();
            else if (yj::keyis(k, "fps"))             sf.fps = (int)v.i64();
            else if (yj::keyis(k, "audioChannels"))   sf.audio_channels = (int)v.i64();
            else if (yj::keyis(k, "audioSampleRate")) sf.audio_sample_rate = (int)v.i64();
            else if (yj::keyis(k, "isDrc"))           sf.is_drc = v.boolean();
            else if (yj::keyis(k, "loudnessDb"))      sf.loudness_db = (float)v.dbl();
            else if (yj::keyis(k, "signatureCipher")) ciphered = true;
            else if (yj::keyis(k, "cipher"))          ciphered = true;
            else if (yj::keyis(k, "quality"))         sf.quality = arena.intern(v.raw());
            return true;
        });

        // A ciphered URL needs the JS player to unscramble. None of the mobile
        // clients return one; if one ever does, skip it rather than emit a
        // format whose URL 403s.
        if (ciphered || url_raw.empty()) return false;

        sf.url = yj::unescape(url_raw);

        // Fast path: everything else comes from the static table.
        if (const ItagInfo* ii = itag_lookup(sf.itag)) {
            sf.mime_type     = ii->mime;
            sf.container     = ii->container;
            sf.video_codec   = ii->vcodec;
            sf.audio_codec   = ii->acodec;
            sf.quality_label = ii->label;
            sf.has_video     = ii->has_video();
            sf.has_audio     = ii->has_audio();
            if (!sf.fps)            sf.fps = ii->fps;
            if (!sf.audio_channels) sf.audio_channels = ii->channels;
            if (!sf.height)         sf.height = ii->height;
            return true;
        }
        return parse_mime_fallback(mime_raw, arena, sf);
    }

    // Unknown itag: recover what we can from mimeType.
    static bool parse_mime_fallback(std::string_view mime, StrArena& arena,
                                    StreamFormat& sf) {
        if (mime.empty()) return false;
        sf.mime_type = arena.intern(mime);
        sf.has_video = mime.rfind("video/", 0) == 0;
        sf.has_audio = mime.rfind("audio/", 0) == 0;

        size_t slash = mime.find('/');
        size_t semi  = mime.find(';');
        if (slash != std::string_view::npos)
            sf.container = arena.intern(mime.substr(
                slash + 1, (semi == std::string_view::npos ? mime.size() : semi) - slash - 1));

        size_t cp = mime.find("codecs=");
        if (cp != std::string_view::npos) {
            size_t q1 = mime.find('"', cp);
            // The response is escaped JSON, so the quote may be \" -> yj gives
            // us the raw slice; handle both forms.
            if (q1 == std::string_view::npos) q1 = mime.find('\\', cp);
            if (q1 != std::string_view::npos) {
                std::string_view rest = mime.substr(q1 + 1);
                size_t end = rest.find_first_of("\"\\");
                std::string_view codecs = rest.substr(0, end == std::string_view::npos ? rest.size() : end);
                size_t comma = codecs.find(',');
                if (comma != std::string_view::npos) {   // muxed: "vcodec, acodec"
                    sf.video_codec = arena.intern(trim(codecs.substr(0, comma)));
                    sf.audio_codec = arena.intern(trim(codecs.substr(comma + 1)));
                    sf.has_video = sf.has_audio = true;
                } else if (sf.has_video) {
                    sf.video_codec = arena.intern(trim(codecs));
                } else {
                    sf.audio_codec = arena.intern(trim(codecs));
                }
            }
        }
        return sf.has_video || sf.has_audio;
    }

    static std::string_view trim(std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.remove_suffix(1);
        return s;
    }

    // YouTube returns the same itag twice, once with isDrc:true. The DRC copy
    // is loudness-compressed and consistently reports the higher bitrate, so a
    // plain "keep the bigger one" dedup silently picks the worse audio on
    // every video. Prefer the non-DRC copy, and only then compare bitrate.
    static void dedup_drc(std::vector<StreamFormat>& fmts) {
        std::vector<StreamFormat> keep;
        keep.reserve(fmts.size());
        for (auto& f : fmts) {
            bool merged = false;
            for (auto& k : keep) {
                if (k.itag != f.itag) continue;
                merged = true;
                bool replace = (k.is_drc && !f.is_drc) ||
                               (k.is_drc == f.is_drc &&
                                f.effective_bitrate() > k.effective_bitrate());
                if (replace) k = std::move(f);
                break;
            }
            if (!merged) keep.push_back(std::move(f));
        }
        fmts.swap(keep);
    }

    static bool codec_matches(const StreamFormat& f, std::string_view want) {
        if (want == f.container) return true;
        if (want == "m4a" && f.container == "mp4") return true;
        if (want == "aac" && f.audio_codec.rfind("mp4a", 0) == 0) return true;
        if (want == "h264" && f.video_codec.rfind("avc1", 0) == 0) return true;
        if (want == "av1" && f.video_codec.rfind("av01", 0) == 0) return true;
        if (!f.audio_codec.empty() && f.audio_codec.find(want) != std::string_view::npos) return true;
        if (!f.video_codec.empty() && f.video_codec.find(want) != std::string_view::npos) return true;
        return false;
    }

    static bool better_audio(const StreamFormat& a, const StreamFormat& b) {
        if (a.is_drc != b.is_drc)  return !a.is_drc;             // clean audio wins
        if (a.audio_channels != b.audio_channels)
            return a.audio_channels > b.audio_channels;          // 5.1 over stereo
        int64_t ab = a.effective_bitrate(), bb = b.effective_bitrate();
        if (ab != bb) return ab > bb;
        return acodec_rank(a.audio_codec) > acodec_rank(b.audio_codec);
    }

    static bool better_video(const StreamFormat& a, const StreamFormat& b) {
        if (a.height != b.height) return a.height > b.height;
        if (a.fps != b.fps)       return a.fps > b.fps;
        int ar = vcodec_rank(a.video_codec), br = vcodec_rank(b.video_codec);
        if (ar != br)             return ar > br;
        return a.effective_bitrate() > b.effective_bitrate();
    }
};

// ---------------------------------------------------------------------------
// Compatibility layer for the pre-rewrite API.
//
// The old build had separate "download" and "stream" selectors because the
// stream variants deliberately returned muxed URLs -- the belief being that
// adaptive URLs get 403'd for non-browser clients. That is not what was
// happening: the client version in use was receiving SABR responses, where the
// adaptive formats carry no URL at all, so muxed was the only thing left. With
// a non-SABR client the adaptive URLs are plain and fetchable, so the split no
// longer means anything and both variants resolve the same way.
// ---------------------------------------------------------------------------
inline std::string select_best_video_stream_impl(const std::vector<StreamFormat>& f, int max_h) {
    auto* p = InnertubeClient::pick_video(f, max_h);
    return p ? p->url : std::string{};
}

// ---------------------------------------------------------------------------
// Free-function shims
// ---------------------------------------------------------------------------
inline void shutdown() { InnertubeClient::get_instance().shutdown(); }

inline std::string yt_best_video_stream(const std::string& id, int max_h = 1080) {
    return InnertubeClient::get_instance().get_best_video_url(id, max_h);
}
inline std::string yt_best_audio_download(const std::string& id) {
    return InnertubeClient::get_instance().get_best_audio_url(id);
}
inline std::string yt_best_audio_stream(const std::string& id) {
    return InnertubeClient::get_instance().get_best_audio_url(id);
}
inline std::vector<SearchResult> yt_search(const std::string& q, int n = 15) {
    return InnertubeClient::get_instance().search(q, n);
}
inline VideoInfo yt_get_formats(const std::string& id) {
    return InnertubeClient::get_instance().get_stream_formats(id);
}
inline std::string yt_best_audio(const std::string& id) {
    return InnertubeClient::get_instance().get_best_audio_url(id);
}
inline std::string yt_best_video(const std::string& id, int max_h = 0) {
    return InnertubeClient::get_instance().get_best_video_url(id, max_h);
}
inline void yt_prefetch(const std::string& id) {
    InnertubeClient::get_instance().prefetch(id);
}

} // namespace ytfast
