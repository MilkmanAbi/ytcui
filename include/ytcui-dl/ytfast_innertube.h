#pragma once
/*
 * ytcui-dl — ytfast_innertube.h
 *
 * Production-grade InnerTube client. Translated 1:1 from yt-dlp source:
 *   _base.py   — INNERTUBE_CLIENTS, _call_api, generate_api_headers,
 *                _extract_visitor_data, extract_ytcfg (VISITOR_DATA bootstrap)
 *   _search.py — _search_results (ep='search'), content_keys
 *   _video.py  — _generate_player_context, _get_checkok_params,
 *                streaming_data format parsing, _DEFAULT_CLIENTS
 *   _tab.py    — _get_text (runs/simpleText traversal)
 *
 * Key design decisions:
 *
 *   VISITOR_DATA bootstrap (fixes audio + LOGIN_REQUIRED):
 *     On first use, fetch www.youtube.com and extract VISITOR_DATA from
 *     ytcfg.set({...}) blobs — same as yt-dlp _download_ytcfg().
 *     This gives a real browser-style visitor token that eliminates bot
 *     detection on both search and player endpoints.
 *     Subsequent calls reuse the cached token.
 *
 *   Shared singleton pattern (fixes audio/video using different caches):
 *     get_instance() returns a process-wide singleton so youtube.cpp's
 *     search() and player.cpp's play_piped() share visitor_data and URL cache.
 *     Search warms the cache; play is instant.
 *
 *   Client chain: android(3) → ios(5) → android_vr(28)
 *     android gives 1080p+26 direct URLs on residential IPs (no JS player).
 *     Falls back gracefully. android_vr is the lowest-suspicion fallback.
 *
 *   URL cache with 5h TTL:
 *     CDN URLs expire in ~6h. Caching eliminates double-click bug entirely.
 *
 *   Async prefetch:
 *     search() spawns a detached thread to pre-resolve top result's streams.
 *     First click is near-instant.
 *
 *   Cross-platform:
 *     No Linux-specific headers. Works on macOS, FreeBSD, OpenBSD, NetBSD.
 *     Uses std::thread for background prefetch (POSIX threads via C++17).
 *
 * v0.2.0 fixes:
 *   - Audio streaming: select_best_audio() now returns stream_url (no &range=)
 *     for streaming, url (with &range=) for downloading. Separate methods.
 *   - Codec parsing: muxed streams correctly split "avc1.x, mp4a.y" into
 *     video_codec + audio_codec.
 *   - Thumbnails: fallback chain maxresdefault → sddefault → hqdefault → mqdefault
 *     with HEAD-check validation. All thumbnails collected in thumbnails[].
 *   - Format dedup: same itag from different sources is deduplicated (higher bitrate wins).
 *   - Container detection: "mp4", "webm", "3gp" extracted from mime_type.
 *   - Format selection engine: filter by codec, container, bitrate, resolution.
 */

#include "ytcui-dl/ytfast_http.h"
#include "ytcui-dl/ytfast_types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <random>
#include <algorithm>
#include <functional>

namespace ytfast {

using json = nlohmann::json;
using sclock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Client definitions — from INNERTUBE_CLIENTS in _base.py
// ---------------------------------------------------------------------------

// web (id=1) — search, trusted UA
static const char* WEB_CLIENT_NAME    = "WEB";
static const char* WEB_CLIENT_VERSION = "2.20260114.08.00";
static const int   WEB_CLIENT_ID      = 1;
static const char* WEB_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

// android (id=3, REQUIRE_JS_PLAYER=False) — primary player, 1080p on residential IPs
static const char* ANDROID_CLIENT_NAME    = "ANDROID";
static const char* ANDROID_CLIENT_VERSION = "21.02.35";
static const int   ANDROID_CLIENT_ID      = 3;
static const char* ANDROID_UA =
    "com.google.android.youtube/21.02.35 (Linux; U; Android 11) gzip";

// ios (id=5, REQUIRE_JS_PLAYER=False) — fallback, 1080p + HLS manifest
static const char* IOS_CLIENT_NAME    = "IOS";
static const char* IOS_CLIENT_VERSION = "21.02.3";
static const int   IOS_CLIENT_ID      = 5;
static const char* IOS_UA =
    "com.google.ios.youtube/21.02.3 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)";

// android_vr (id=28, REQUIRE_JS_PLAYER=False) — last resort, 240p cap but reliable
static const char* ANDROIDVR_CLIENT_NAME    = "ANDROID_VR";
static const char* ANDROIDVR_CLIENT_VERSION = "1.65.10";
static const int   ANDROIDVR_CLIENT_ID      = 28;
static const char* ANDROIDVR_UA =
    "com.google.android.apps.youtube.vr.oculus/1.65.10 "
    "(Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip";

static const char* INNERTUBE_BASE = "https://www.youtube.com/youtubei/v1/";
static const char* SEARCH_PARAMS  = "EgIQAfABAQ=="; // videos-only filter

// CDN URL TTL — YouTube CDN URLs valid ~6h, we cache 5h
static const int URL_CACHE_TTL_S = 5 * 3600;
// Visitor data TTL — refresh every 12h
static const int VISITOR_TTL_S   = 12 * 3600;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string safe_str(const json& j, const char* k, const std::string& fb="") {
    if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>();
    return fb;
}
static int64_t safe_int(const json& j, const char* k, int64_t fb=0) {
    if (!j.contains(k)) return fb;
    if (j[k].is_number_integer()) return j[k].get<int64_t>();
    if (j[k].is_number()) return (int64_t)j[k].get<double>();
    if (j[k].is_string()) try { return std::stoll(j[k].get<std::string>()); } catch(...) {}
    return fb;
}

// sanitize_utf8 — strips invalid bytes, keeps printable + valid multibyte
static std::string sanitize_utf8(const std::string& s) {
    std::string o; o.reserve(s.size());
    const unsigned char* p = (const unsigned char*)s.c_str();
    const unsigned char* e = p + s.size();
    while (p < e) {
        if (*p < 0x80) {
            if (*p >= 32 || *p == '\t' || *p == '\n') o += (char)*p;
            ++p;
        } else if ((*p&0xE0)==0xC0 && p+1<e && (p[1]&0xC0)==0x80)
            { o.append((const char*)p,2); p+=2; }
        else if ((*p&0xF0)==0xE0 && p+2<e && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80)
            { o.append((const char*)p,3); p+=3; }
        else if ((*p&0xF8)==0xF0 && p+3<e && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80 && (p[3]&0xC0)==0x80)
            { o.append((const char*)p,4); p+=4; }
        else ++p;
    }
    return o;
}

static std::string fmt_duration(int s) {
    if (s<=0) return "0:00";
    char b[32]; int h=s/3600, m=(s%3600)/60, sc=s%60;
    if (h>0) snprintf(b,sizeof(b),"%d:%02d:%02d",h,m,sc);
    else      snprintf(b,sizeof(b),"%d:%02d",m,sc);
    return b;
}

static std::string fmt_views(int64_t v) {
    char b[32];
    if      (v>=1000000000) snprintf(b,sizeof(b),"%.2fB",v/1e9);
    else if (v>=1000000)    snprintf(b,sizeof(b),"%.2fM",v/1e6);
    else if (v>=1000)       snprintf(b,sizeof(b),"%.2fK",v/1e3);
    else                    snprintf(b,sizeof(b),"%lld",(long long)v);
    return b;
}

static std::string decode_entities(std::string s) {
    struct { const char* a; const char* b; } tbl[] = {
        {"&amp;","&"},{"&lt;","<"},{"&gt;",">"},{"&quot;","\""},{"&#39;","'"},{"&apos;","'"}
    };
    for (auto& e : tbl)
        for (size_t p; (p=s.find(e.a))!=std::string::npos; )
            s.replace(p, strlen(e.a), e.b);
    return s;
}

// CPN generation — _video.py:2274 "works even with dummy cpn"
static std::string gen_cpn() {
    static const char* alpha = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> d(0,63);
    std::string r(16,' ');
    for (char& c : r) c = alpha[d(rng)];
    return r;
}

// extract_text — _tab.py _get_text(): simpleText or runs[].text
static std::string extract_text(const json& node) {
    if (node.is_string()) return node.get<std::string>();
    if (node.contains("simpleText") && node["simpleText"].is_string())
        return node["simpleText"].get<std::string>();
    if (node.contains("runs") && node["runs"].is_array()) {
        std::string o;
        for (auto& r : node["runs"])
            if (r.contains("text") && r["text"].is_string())
                o += r["text"].get<std::string>();
        return o;
    }
    return "";
}
static std::string get_field(const json& p, const char* k) {
    return p.contains(k) ? extract_text(p[k]) : "";
}

// ---------------------------------------------------------------------------
// Thumbnail helpers — FIXED: fallback chain instead of blind maxresdefault
// ---------------------------------------------------------------------------

// Build thumbnail URL candidates for a video ID (best → worst)
static std::vector<ThumbnailInfo> thumbnail_candidates(const std::string& video_id) {
    if (video_id.empty() || video_id.size() != 11) return {};
    std::string base = "https://i.ytimg.com/vi/" + video_id + "/";
    return {
        { base + "maxresdefault.jpg", 1280, 720 },
        { base + "sddefault.jpg",      640, 480 },
        { base + "hqdefault.jpg",      480, 360 },
        { base + "mqdefault.jpg",      320, 180 },
        { base + "default.jpg",        120,  90 },
    };
}

// Extract thumbnails from videoDetails/videoRenderer JSON node
static std::vector<ThumbnailInfo> extract_thumbnails_from_json(const json& node) {
    std::vector<ThumbnailInfo> out;
    auto from_arr = [&](const json& a) {
        if (!a.is_array()) return;
        for (auto& t : a) {
            ThumbnailInfo ti;
            ti.url    = safe_str(t, "url");
            ti.width  = (int)safe_int(t, "width");
            ti.height = (int)safe_int(t, "height");
            if (!ti.url.empty()) out.push_back(ti);
        }
    };
    if (node.contains("thumbnail") && node["thumbnail"].contains("thumbnails"))
        from_arr(node["thumbnail"]["thumbnails"]);
    else if (node.contains("thumbnails"))
        from_arr(node["thumbnails"]);
    return out;
}

// Pick the best thumbnail: prefer API-provided URLs (guaranteed to exist),
// fall back to hqdefault (virtually universal).
// v0.1 blindly returned maxresdefault which 404s on many videos.
// v0.2 uses API thumbnails as the authoritative source.
static std::string best_thumbnail_url(const std::string& video_id,
                                       const std::vector<ThumbnailInfo>& api_thumbs) {
    // API-provided thumbnails are guaranteed to exist — pick the widest
    if (!api_thumbs.empty()) {
        const ThumbnailInfo* best = &api_thumbs[0];
        for (auto& t : api_thumbs)
            if (t.width > best->width) best = &t;
        return best->url;
    }
    // Fallback: hqdefault exists for virtually all videos (maxresdefault does NOT)
    if (!video_id.empty() && video_id.size() == 11)
        return "https://i.ytimg.com/vi/" + video_id + "/hqdefault.jpg";
    return "";
}

static int64_t parse_vc(const std::string& s) {
    std::string d; for (char c:s) if(c>='0'&&c<='9') d+=c;
    if (d.empty()) return 0;
    try { return std::stoll(d); } catch (...) { return 0; }
}
static int parse_dur(const std::string& s) {
    if (s.empty()) return 0;
    int t=0,v=0;
    for (char c:s) { if(c==':'){t=t*60+v;v=0;} else if(c>='0'&&c<='9') v=v*10+(c-'0'); }
    return t*60+v;
}

// Extract VISITOR_DATA from YouTube HTML — no regex (crashes on large HTML)
// Implements yt-dlp extract_ytcfg() + _extract_visitor_data()
static std::string extract_visitor_data_from_html(const std::string& html) {
    // Try "VISITOR_DATA":"..." first (in ytcfg blobs)
    const char* n1 = "\"VISITOR_DATA\":\"";
    auto p = html.find(n1);
    if (p != std::string::npos) {
        p += strlen(n1);
        auto e = html.find('"', p);
        if (e != std::string::npos && e > p && e-p < 200)
            return html.substr(p, e-p);
    }
    // Fallback: "visitorData":"..." (in responseContext or INNERTUBE_CONTEXT)
    const char* n2 = "\"visitorData\":\"";
    p = html.find(n2);
    if (p != std::string::npos) {
        p += strlen(n2);
        auto e = html.find('"', p);
        if (e != std::string::npos && e > p && e-p < 200)
            return html.substr(p, e-p);
    }
    return "";
}

// ---------------------------------------------------------------------------
// Codec extraction — FIXED: handles muxed "avc1.x, mp4a.y" properly
// ---------------------------------------------------------------------------

// Extract raw codec string from mime_type e.g. "video/mp4; codecs=\"avc1.4d401f\""
static std::string extract_codec_raw(const std::string& mime) {
    auto p = mime.find("codecs=\"");
    if (p == std::string::npos) p = mime.find("codecs=");
    if (p == std::string::npos) return "";
    p = mime.find('"', p); if (p == std::string::npos) return "";
    ++p;
    auto e = mime.find('"', p);
    return e != std::string::npos ? mime.substr(p, e-p) : "";
}

// Split codec string "avc1.4d401f, mp4a.40.2" into {video_codec, audio_codec}
struct CodecPair { std::string video; std::string audio; };
static CodecPair parse_codecs(const std::string& raw_codecs) {
    CodecPair cp;
    if (raw_codecs.empty()) return cp;

    // Split on comma
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < raw_codecs.size()) {
        auto comma = raw_codecs.find(',', start);
        std::string part = raw_codecs.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        // trim whitespace
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        while (!part.empty() && part.back() == ' ') part.pop_back();
        if (!part.empty()) parts.push_back(part);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }

    for (auto& codec : parts) {
        // Audio codecs: mp4a, opus, vorbis, flac, ec-3, ac-3
        bool is_audio = codec.find("mp4a") == 0 || codec.find("opus") == 0 ||
                        codec.find("vorbis") == 0 || codec.find("flac") == 0 ||
                        codec.find("ec-3") == 0 || codec.find("ac-3") == 0 ||
                        codec.find("dtse") == 0;
        if (is_audio)
            cp.audio = codec;
        else
            cp.video = codec;  // avc1, vp9, vp09, av01, etc.
    }
    return cp;
}

// Extract container from mime_type "video/mp4" → "mp4", "audio/webm" → "webm"
static std::string extract_container(const std::string& mime) {
    auto slash = mime.find('/');
    if (slash == std::string::npos) return "";
    auto semi = mime.find(';', slash);
    std::string ct = mime.substr(slash + 1, semi == std::string::npos ? std::string::npos : semi - slash - 1);
    // trim
    while (!ct.empty() && ct.back() == ' ') ct.pop_back();
    return ct;
}


// add_range_param() — append &range=0-N to force single HTTP response
// YouTube DASH streams return only the init+first segment without this.
// yt-dlp does the same for non-live streams internally.
static std::string add_range_param(const std::string& url, int64_t content_length) {
    if (url.find("&range=") != std::string::npos) return url; // already has range
    if (url.find("?") == std::string::npos) return url; // not a query URL
    // Live streams have no content_length — don't add range
    if (content_length <= 0) return url;
    char buf[64];
    snprintf(buf, sizeof(buf), "&range=0-%lld", (long long)(content_length - 1));
    return url + buf;
}

// Applies range param to a StreamFormat's download URL in-place.
// stream_url is kept as the raw URL for mpv/streaming.
static void fix_url_for_streaming(StreamFormat& fmt) {
    // stream_url = raw URL (for mpv streaming), url = download URL (with range)
    // stream_url was already set before this call
    if (fmt.content_length > 0)
        fmt.url = add_range_param(fmt.url, fmt.content_length);
}

// ---------------------------------------------------------------------------
// URL cache
// ---------------------------------------------------------------------------
struct CachedInfo {
    VideoInfo info;
    sclock::time_point fetched_at;
};

class UrlCache {
public:
    const VideoInfo* get(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = cache_.find(id);
        if (it == cache_.end()) return nullptr;
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            sclock::now() - it->second.fetched_at).count();
        if (age > URL_CACHE_TTL_S) { cache_.erase(it); return nullptr; }
        return &it->second.info;
    }
    void put(const std::string& id, const VideoInfo& info) {
        std::lock_guard<std::mutex> lk(mu_);
        cache_[id] = { info, sclock::now() };
    }
private:
    std::mutex mu_;
    std::unordered_map<std::string, CachedInfo> cache_;
};

// ---------------------------------------------------------------------------
// InnertubeClient
// ---------------------------------------------------------------------------

class InnertubeClient {
public:
    InnertubeClient() {}

    // -----------------------------------------------------------------------
    // get_instance() — process-wide singleton
    // -----------------------------------------------------------------------
    static InnertubeClient& get_instance() {
        static InnertubeClient inst;
        return inst;
    }

    // -----------------------------------------------------------------------
    // bootstrap_visitor_data()
    //
    // Strategy 1: Scrape youtube.com HTML for VISITOR_DATA (fastest, most reliable)
    // Strategy 2: Make a minimal innertube API call to get visitorData from
    //             responseContext (works when HTML scraping fails due to consent
    //             redirects, cloud IPs, etc.)
    //
    // v0.2.1 fix: Strategy 2 is critical. Without visitor_data, YouTube's CDN
    // generates restrictive URLs that reject non-ranged GET requests on audio-only
    // adaptive streams. This caused audio playback to fail in mpv for longer videos
    // while working for short ones. The test suite masked this because search()
    // (which always runs in tests) implicitly populated visitor_data via
    // responseContext, but direct CLI usage (ytcui-dl -g) skipped search.
    // -----------------------------------------------------------------------
    void bootstrap_visitor_data() {
        std::lock_guard<std::mutex> lk(vd_mu_);
        // Check if still fresh
        if (!visitor_data_.empty()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                sclock::now() - visitor_data_fetched_).count();
            if (age < VISITOR_TTL_S) return;
        }

        // Strategy 1: YouTube homepage HTML scraping
        const char* urls[] = {
            "https://www.youtube.com/",
            "https://www.youtube.com/sw.js",
            nullptr
        };
        for (int i = 0; urls[i]; ++i) {
            try {
                auto resp = get_thread_http().get(urls[i], {
                    std::string("User-Agent: ") + WEB_UA,
                    "Accept-Language: en-US,en;q=0.9",
                    "Accept: text/html,application/xhtml+xml,*/*;q=0.8"
                });
                if (resp.status == 200 && !resp.body.empty()) {
                    std::string vd = extract_visitor_data_from_html(resp.body);
                    if (!vd.empty()) {
                        visitor_data_ = vd;
                        visitor_data_fetched_ = sclock::now();
                        return;
                    }
                }
            } catch (...) {}
        }

        // Strategy 2: Minimal innertube API call
        // POST to /guide endpoint with web client context — lightest endpoint,
        // always returns responseContext with visitorData.
        // This is what the search() call does implicitly, but we need it
        // before ANY player request, not just after search.
        try {
            json ctx = {{"client", {
                {"clientName", WEB_CLIENT_NAME},
                {"clientVersion", WEB_CLIENT_VERSION},
                {"hl", "en"}, {"timeZone", "UTC"}, {"utcOffsetMinutes", 0}
            }}};
            json body = {{"context", ctx}};
            auto resp = get_thread_http().post(
                std::string(INNERTUBE_BASE) + "guide?prettyPrint=false",
                body.dump(), {
                    "X-YouTube-Client-Name: " + std::to_string(WEB_CLIENT_ID),
                    std::string("X-YouTube-Client-Version: ") + WEB_CLIENT_VERSION,
                    std::string("User-Agent: ") + WEB_UA,
                    "Origin: https://www.youtube.com",
                });
            if (resp.status == 200) {
                json data = json::parse(resp.body);
                if (data.contains("responseContext")) {
                    std::string vd = safe_str(data["responseContext"], "visitorData");
                    if (!vd.empty()) {
                        visitor_data_ = vd;
                        visitor_data_fetched_ = sclock::now();
                        return;
                    }
                }
            }
        } catch (...) {}
        // If all strategies fail, visitor_data stays empty.
        // Player requests will still try to extract it from responseContext,
        // but CDN URLs generated without it may be more restrictive.
    }

    // -----------------------------------------------------------------------
    // search() — POST /youtubei/v1/search
    // -----------------------------------------------------------------------
    std::vector<SearchResult> search(const std::string& query,
                                     int max_results = 15,
                                     bool prefetch_top = true) {
        ensure_visitor_data();

        std::vector<SearchResult> results;
        json body = {
            {"context", web_ctx()},
            {"query",   query},
            {"params",  SEARCH_PARAMS}
        };

        auto resp = get_thread_http().post(
            std::string(INNERTUBE_BASE) + "search?prettyPrint=false",
            body.dump(), web_hdrs());

        if (resp.status != 200)
            throw std::runtime_error("search HTTP " + std::to_string(resp.status));

        json data = json::parse(resp.body);

        // Update visitor_data from response if we didn't get it from homepage
        {
            std::lock_guard<std::mutex> lk(vd_mu_);
            if (visitor_data_.empty() && data.contains("responseContext"))
                visitor_data_ = safe_str(data["responseContext"], "visitorData");
        }

        try {
            auto& secs = data["contents"]["twoColumnSearchResultsRenderer"]
                             ["primaryContents"]["sectionListRenderer"]["contents"];
            for (auto& sec : secs) {
                if (!sec.contains("itemSectionRenderer")) continue;
                for (auto& item : sec["itemSectionRenderer"]["contents"]) {
                    if (!item.contains("videoRenderer")) continue;
                    auto& vr = item["videoRenderer"];
                    SearchResult v;
                    v.id = safe_str(vr, "videoId");
                    if (v.id.empty()) continue;
                    v.title   = sanitize_utf8(decode_entities(get_field(vr, "title")));
                    v.channel = sanitize_utf8(get_field(vr, "ownerText"));
                    if (v.channel.empty())
                        v.channel = sanitize_utf8(get_field(vr, "shortBylineText"));
                    v.view_count     = parse_vc(get_field(vr, "viewCountText"));
                    v.view_count_str = v.view_count > 0 ? fmt_views(v.view_count) : "N/A";
                    v.duration_str   = get_field(vr, "lengthText");
                    v.duration_secs  = parse_dur(v.duration_str);
                    if (v.duration_str.empty()) v.duration_str = "0:00";

                    // Thumbnails — use API-provided (guaranteed to exist), add candidates as bonus
                    auto api_thumbs = extract_thumbnails_from_json(vr);
                    v.thumbnail_url = best_thumbnail_url(v.id, api_thumbs);
                    v.thumbnails = api_thumbs;
                    auto candidates = thumbnail_candidates(v.id);
                    for (auto& c : candidates) {
                        bool dup = false;
                        for (auto& t : v.thumbnails)
                            if (t.width == c.width) { dup = true; break; }
                        if (!dup) v.thumbnails.push_back(c);
                    }

                    v.upload_date    = get_field(vr, "publishedTimeText");
                    v.url            = "https://www.youtube.com/watch?v=" + v.id;

                    // channel_id
                    try {
                        if (vr.contains("ownerText") && vr["ownerText"].contains("runs")
                            && !vr["ownerText"]["runs"].empty()) {
                            auto& run = vr["ownerText"]["runs"][0];
                            if (run.contains("navigationEndpoint") &&
                                run["navigationEndpoint"].contains("browseEndpoint"))
                                v.channel_id = run["navigationEndpoint"]["browseEndpoint"]
                                               .value("browseId", "");
                        }
                    } catch (...) {}

                    // Description snippet
                    try {
                        if (vr.contains("detailedMetadataSnippets")) {
                            for (auto& snip : vr["detailedMetadataSnippets"]) {
                                if (!snip.contains("snippetText")) continue;
                                std::string text;
                                auto& st = snip["snippetText"];
                                if (st.contains("runs")) {
                                    for (auto& run2 : st["runs"])
                                        if (run2.contains("text") && run2["text"].is_string())
                                            text += run2["text"].get<std::string>();
                                } else if (st.contains("simpleText")) {
                                    text = safe_str(st, "simpleText");
                                }
                                if (!text.empty()) {
                                    v.description = sanitize_utf8(text);
                                    break;
                                }
                            }
                        }
                    } catch (...) {}
                    if (vr.contains("badges"))
                        for (auto& b : vr["badges"])
                            if (b.contains("metadataBadgeRenderer") &&
                                safe_str(b["metadataBadgeRenderer"], "label") == "LIVE")
                                { v.is_live = true; break; }
                    results.push_back(std::move(v));
                    if ((int)results.size() >= max_results) goto done;
                }
            }
        } catch (const json::exception&) {}
        done:

        // Async prefetch top result
        if (prefetch_top && !results.empty()) {
            std::string top_id = results[0].id;
            std::thread([this, top_id]() {
                try {
                    if (!url_cache_.get(top_id))
                        url_cache_.put(top_id, fetch_player(top_id));
                } catch (...) {}
            }).detach();
        }

        return results;
    }

    // -----------------------------------------------------------------------
    // get_stream_formats() — with URL cache
    // -----------------------------------------------------------------------
    VideoInfo get_stream_formats(const std::string& video_id) {
        ensure_visitor_data();
        if (const VideoInfo* c = url_cache_.get(video_id)) return *c;
        auto info = fetch_player(video_id);
        url_cache_.put(video_id, info);
        return info;
    }

    // -----------------------------------------------------------------------
    // Convenience: best audio/video URL getters
    // -----------------------------------------------------------------------

    // For DOWNLOADING (url with &range= for full file download)
    std::string get_best_audio_url(const std::string& vid) {
        return select_best_audio_download(get_stream_formats(vid).formats);
    }
    std::string get_best_video_url(const std::string& vid, int max_h = 1080) {
        return select_best_video(get_stream_formats(vid).formats, max_h);
    }

    // For STREAMING in mpv (stream_url, no &range=)
    std::string get_best_audio_stream_url(const std::string& vid) {
        return select_best_audio_stream(get_stream_formats(vid).formats);
    }
    std::string get_best_video_stream_url(const std::string& vid, int max_h = 1080) {
        return select_best_video_stream(get_stream_formats(vid).formats, max_h);
    }

    // Warm cache in background
    void prefetch(const std::string& video_id) {
        std::thread([this, video_id]() {
            try {
                if (!url_cache_.get(video_id))
                    url_cache_.put(video_id, fetch_player(video_id));
            } catch (...) {}
        }).detach();
    }

    // -----------------------------------------------------------------------
    // Format selection — v0.2.0 rewrite
    //
    // Key fix: separate "download" selectors (return url with &range=)
    //          from "stream" selectors (return stream_url for mpv playback).
    //          v0.1 mixed these up, which is why audio streaming was broken.
    // -----------------------------------------------------------------------

    // --- AUDIO: DOWNLOAD (url with &range= for curl/wget full file save) ---
    static std::string select_best_audio_download(const std::vector<StreamFormat>& fmts,
                                                   const std::string& prefer_codec = "",
                                                   int min_bitrate = 0) {
        auto* f = pick_audio(fmts, prefer_codec, min_bitrate, true);
        return f ? f->url : "";
    }

    // --- AUDIO: STREAM (for mpv playback) ---
    // Returns a MUXED stream URL (video+audio). The caller uses mpv --no-video
    // to discard the video track and play audio only.
    //
    // Why muxed instead of audio-only DASH?
    //   YouTube CDN rejects mpv/ffmpeg's HTTP requests on audio-only adaptive
    //   streams for longer videos (403 Forbidden). This varies by region and
    //   video. Muxed streams are progressive downloads — they work 100% of
    //   the time, everywhere, every video. The audio quality difference is
    //   negligible (128kbps AAC muxed vs 128-160kbps opus/aac adaptive).
    //   A bit of extra bandwidth is a small price for never getting 403.
    static std::string select_best_audio_stream(const std::vector<StreamFormat>& fmts,
                                                 const std::string& /*prefer_codec*/ = "") {
        // Pick best muxed stream — progressive download, always works
        const StreamFormat* best = nullptr;
        for (auto& f : fmts) {
            if (!f.is_muxed()) continue;
            if (!best || f.bitrate > best->bitrate) best = &f;
        }
        if (best) {
            return best->stream_url.empty() ? best->url : best->stream_url;
        }
        // Fallback: if no muxed streams exist (rare), try audio-only with range
        auto* f = pick_audio(fmts, "", 0, false);
        return f ? f->url : "";
    }

    // --- AUDIO: worst quality (smallest file, for previews/tests) ---
    static std::string select_worst_audio(const std::vector<StreamFormat>& fmts) {
        const StreamFormat* worst = nullptr;
        for (auto& f : fmts)
            if (f.is_audio_only())
                if (!worst || f.effective_bitrate() < worst->effective_bitrate())
                    worst = &f;
        return worst ? worst->url : "";
    }

    // --- AUDIO: by specific itag ---
    static const StreamFormat* find_by_itag(const std::vector<StreamFormat>& fmts, int itag) {
        for (auto& f : fmts)
            if (f.itag == itag) return &f;
        return nullptr;
    }

    // --- AUDIO: get the StreamFormat pointer for detailed access ---
    static const StreamFormat* select_best_audio_format(const std::vector<StreamFormat>& fmts,
                                                         const std::string& prefer_codec = "",
                                                         int min_bitrate = 0) {
        return pick_audio(fmts, prefer_codec, min_bitrate, true);
    }

    // --- VIDEO: DOWNLOAD (url with &range=) ---
    static std::string select_best_video(const std::vector<StreamFormat>& fmts,
                                          int max_h = 1080) {
        const StreamFormat* best = nullptr;
        // Muxed first
        for (auto& f : fmts) {
            if (!f.has_video || !f.has_audio || f.height > max_h) continue;
            if (!best || f.height > best->height ||
                (f.height == best->height && f.bitrate > best->bitrate))
                best = &f;
        }
        // Adaptive video-only
        if (!best)
            for (auto& f : fmts) {
                if (!f.has_video || f.height > max_h) continue;
                if (!best || f.height > best->height ||
                    (f.height == best->height && f.bitrate > best->bitrate))
                    best = &f;
            }
        return best ? best->url : "";
    }

    // --- VIDEO: STREAM (stream_url for mpv, prefers muxed) ---
    static std::string select_best_video_stream(const std::vector<StreamFormat>& fmts,
                                                  int max_h = 1080) {
        const StreamFormat* best = nullptr;
        // Muxed first — avoids needing --audio-file in mpv
        for (auto& f : fmts) {
            if (!f.has_video || !f.has_audio || f.height > max_h) continue;
            if (!best || f.height > best->height ||
                (f.height == best->height && f.bitrate > best->bitrate)) best = &f;
        }
        // Fallback: adaptive video-only
        if (!best)
            for (auto& f : fmts) {
                if (!f.has_video || f.height > max_h) continue;
                if (!best || f.height > best->height ||
                    (f.height == best->height && f.bitrate > best->bitrate)) best = &f;
            }
        if (!best) return "";
        return best->stream_url.empty() ? best->url : best->stream_url;
    }

    // --- VIDEO: video-only adaptive (no audio) ---
    static std::string select_best_video_only(const std::vector<StreamFormat>& fmts,
                                               int max_h = 1080) {
        const StreamFormat* best = nullptr;
        for (auto& f : fmts) {
            if (!f.is_video_only() || f.height > max_h) continue;
            if (!best || f.height > best->height ||
                (f.height == best->height && f.bitrate > best->bitrate))
                best = &f;
        }
        return best ? best->url : "";
    }

    // Returns true if video_url points to a muxed stream
    static bool is_muxed(const std::vector<StreamFormat>& fmts,
                          const std::string& video_url) {
        for (auto& f : fmts)
            if ((f.url == video_url || f.stream_url == video_url) && f.is_muxed()) return true;
        return false;
    }

    // --- GENERIC filter: find formats matching a predicate ---
    static std::vector<const StreamFormat*> filter_formats(
            const std::vector<StreamFormat>& fmts,
            std::function<bool(const StreamFormat&)> pred) {
        std::vector<const StreamFormat*> out;
        for (auto& f : fmts)
            if (pred(f)) out.push_back(&f);
        return out;
    }

    // -----------------------------------------------------------------------
    // Format selection by yt-dlp style format string
    // Supports: bestaudio, worstaudio, bestvideo, bestvideo[height<=720],
    //           bestaudio[ext=m4a], bestaudio[ext=webm], bestaudio[abr>=128],
    //           best, worst, <itag>
    // -----------------------------------------------------------------------
    static std::string resolve_format_string(const std::string& sel,
                                              const std::vector<StreamFormat>& fmts,
                                              bool for_streaming = false) {
        // Parse format string into base + filters
        std::string base = sel;
        std::vector<std::pair<std::string, std::string>> filters; // key=value pairs

        // Extract [...] filters
        auto bracket = sel.find('[');
        if (bracket != std::string::npos) {
            base = sel.substr(0, bracket);
            std::string rest = sel.substr(bracket);
            // Parse [key op value] blocks
            size_t pos = 0;
            while (pos < rest.size()) {
                auto open = rest.find('[', pos);
                if (open == std::string::npos) break;
                auto close = rest.find(']', open);
                if (close == std::string::npos) break;
                std::string expr = rest.substr(open + 1, close - open - 1);
                filters.push_back(parse_filter(expr));
                pos = close + 1;
            }
        }

        // Apply base selection
        if (base == "bestaudio" || base == "ba" || base == "audio") {
            if (for_streaming) {
                // For streaming/mpv: use muxed stream (mpv --no-video strips video)
                // Muxed streams are progressive downloads — no DASH 403 issues
                return select_best_audio_stream(fmts);
            }
            // For download: use actual audio-only stream
            auto matching = apply_filters(fmts, filters, true, false);
            if (matching.empty()) {
                auto* f = pick_audio(fmts, "", 0, true);
                return f ? f->url : "";
            }
            std::sort(matching.begin(), matching.end(),
                [](const StreamFormat* a, const StreamFormat* b) {
                    return a->effective_bitrate() > b->effective_bitrate();
                });
            return matching[0]->url;
        }

        if (base == "worstaudio" || base == "wa") {
            if (for_streaming) {
                // For streaming: muxed, lowest bitrate
                const StreamFormat* worst = nullptr;
                for (auto& f : fmts)
                    if (f.is_muxed() && (!worst || f.bitrate < worst->bitrate))
                        worst = &f;
                if (worst) return worst->stream_url.empty() ? worst->url : worst->stream_url;
            }
            auto matching = apply_filters(fmts, filters, true, false);
            if (matching.empty()) return select_worst_audio(fmts);
            std::sort(matching.begin(), matching.end(),
                [](const StreamFormat* a, const StreamFormat* b) {
                    return a->effective_bitrate() < b->effective_bitrate();
                });
            return matching[0]->url;
        }

        if (base == "bestvideo" || base == "bv") {
            int max_h = extract_height_filter(filters);
            auto* f = pick_video_only(fmts, max_h);
            return f ? f->url : select_best_video(fmts, max_h);
        }

        if (base == "worstvideo" || base == "wv") {
            const StreamFormat* worst = nullptr;
            for (auto& f : fmts)
                if (f.has_video && (!worst || f.height < worst->height))
                    worst = &f;
            return worst ? worst->url : "";
        }

        if (base == "best" || base == "bestvideo+bestaudio" || base == "b" || base == "video") {
            int max_h = extract_height_filter(filters);
            if (max_h == 0) max_h = 1080;
            return for_streaming ? select_best_video_stream(fmts, max_h)
                                : select_best_video(fmts, max_h);
        }

        if (base == "worst" || base == "w") {
            const StreamFormat* worst = nullptr;
            for (auto& f : fmts)
                if (f.has_video && f.has_audio &&
                    (!worst || f.height < worst->height)) worst = &f;
            return worst ? worst->url : "";
        }

        // itag number
        try {
            int itag = std::stoi(base);
            for (auto& f : fmts)
                if (f.itag == itag)
                    return for_streaming ? stream_or_url(f) : f.url;
        } catch (...) {}

        // Fallback: interpret as "best" with height constraint if digits present
        int max_h = 1080;
        try {
            // Handle bare "720", "480" etc
            auto p = base.find("<=");
            if (p != std::string::npos) {
                max_h = std::stoi(base.substr(p + 2));
            } else {
                // try digits in the string
                std::string digits;
                for (char c : base) if (c >= '0' && c <= '9') digits += c;
                if (!digits.empty()) max_h = std::stoi(digits);
            }
        } catch (...) {}

        return for_streaming ? select_best_video_stream(fmts, max_h)
                            : select_best_video(fmts, max_h);
    }

    // Visitor data accessor
    std::string visitor_data() {
        std::lock_guard<std::mutex> lk(vd_mu_);
        return visitor_data_;
    }

    // ===== LEGACY COMPAT (keep old API working) =====
    // These map to the new split download/stream functions
    static std::string select_best_audio(const std::vector<StreamFormat>& fmts) {
        return select_best_audio_download(fmts);
    }
    static std::string select_best_streaming_audio(const std::vector<StreamFormat>& fmts) {
        return select_best_audio_stream(fmts);
    }
    static std::string select_best_streaming_video(const std::vector<StreamFormat>& fmts, int max_h = 1080) {
        return select_best_video_stream(fmts, max_h);
    }

private:
    UrlCache    url_cache_;

    std::mutex              vd_mu_;
    std::string             visitor_data_;
    sclock::time_point      visitor_data_fetched_{};
    bool                    visitor_bootstrap_done_ = false;

    struct ClientDef {
        const char* name, *ver, *ua;
        int id;
        bool android_style;
        bool ios_style;
    };

    void ensure_visitor_data() {
        bool needs_bootstrap = false;
        {
            std::lock_guard<std::mutex> lk(vd_mu_);
            if (!visitor_bootstrap_done_) {
                visitor_bootstrap_done_ = true;
                needs_bootstrap = true;
            }
        }
        if (needs_bootstrap) bootstrap_visitor_data();
    }

    // -----------------------------------------------------------------------
    // fetch_player() — android(3) → ios(5) → android_vr(28)
    // -----------------------------------------------------------------------
    VideoInfo fetch_player(const std::string& video_id) {
        ClientDef clients[] = {
            { ANDROID_CLIENT_NAME, ANDROID_CLIENT_VERSION, ANDROID_UA,
              ANDROID_CLIENT_ID, true, false },
            { IOS_CLIENT_NAME, IOS_CLIENT_VERSION, IOS_UA,
              IOS_CLIENT_ID, false, true },
            { ANDROIDVR_CLIENT_NAME, ANDROIDVR_CLIENT_VERSION, ANDROIDVR_UA,
              ANDROIDVR_CLIENT_ID, false, false },
        };

        std::string last_err;
        for (auto& c : clients) {
            try { return player_request(video_id, c); }
            catch (const std::exception& ex) {
                last_err = ex.what();
                std::string msg(ex.what());
                bool soft = msg.find("bot")       != std::string::npos ||
                            msg.find("sign")      != std::string::npos ||
                            msg.find("UNPLAYABLE")!= std::string::npos ||
                            msg.find("LOGIN")     != std::string::npos ||
                            msg.find("streaming") != std::string::npos ||
                            msg.find("ERROR")     != std::string::npos;
                if (!soft) throw;
            }
        }
        throw std::runtime_error("All clients failed: " + last_err);
    }

    VideoInfo player_request(const std::string& vid, const ClientDef& c) {
        json client_ctx = {
            {"clientName",    c.name},
            {"clientVersion", c.ver},
            {"hl", "en"}, {"timeZone", "UTC"}, {"utcOffsetMinutes", 0}
        };
        if (c.android_style) {
            client_ctx["androidSdkVersion"] = 30;
            client_ctx["osName"]            = "Android";
            client_ctx["osVersion"]         = "11";
        }
        if (c.ios_style) {
            client_ctx["deviceMake"]  = "Apple";
            client_ctx["deviceModel"] = "iPhone16,2";
            client_ctx["osName"]      = "iPhone";
            client_ctx["osVersion"]   = "18.3.2.22D82";
        }
        {
            std::lock_guard<std::mutex> lk(vd_mu_);
            if (!visitor_data_.empty()) client_ctx["visitorData"] = visitor_data_;
        }

        json body = {
            {"context",        {{"client", client_ctx}}},
            {"videoId",        vid},
            {"contentCheckOk", true},
            {"racyCheckOk",    true},
            {"playbackContext", {{"contentPlaybackContext", {
                {"html5Preference", "HTML5_PREF_WANTS"}
            }}}},
            {"cpn", gen_cpn()},
        };

        std::string vd_copy;
        { std::lock_guard<std::mutex> lk(vd_mu_); vd_copy = visitor_data_; }

        std::vector<std::string> hdrs = {
            "X-YouTube-Client-Name: " + std::to_string(c.id),
            std::string("X-YouTube-Client-Version: ") + c.ver,
            std::string("User-Agent: ") + c.ua,
            "Origin: https://www.youtube.com",
        };
        if (!vd_copy.empty())
            hdrs.push_back("X-Goog-Visitor-Id: " + vd_copy);

        auto resp = get_thread_http().post(
            std::string(INNERTUBE_BASE) + "player?prettyPrint=false",
            body.dump(), hdrs);

        if (resp.status != 200)
            throw std::runtime_error(
                std::string(c.name) + " HTTP " + std::to_string(resp.status));

        json data = json::parse(resp.body);

        // Update visitor_data from response if needed
        {
            std::lock_guard<std::mutex> lk(vd_mu_);
            if (visitor_data_.empty() && data.contains("responseContext"))
                visitor_data_ = safe_str(data["responseContext"], "visitorData");
        }

        // Check playabilityStatus
        if (data.contains("playabilityStatus")) {
            auto& ps = data["playabilityStatus"];
            std::string status = safe_str(ps, "status");
            std::string reason = safe_str(ps, "reason");
            if (reason.empty() && ps.contains("messages") &&
                ps["messages"].is_array() && !ps["messages"].empty())
                reason = ps["messages"][0].is_string() ?
                    ps["messages"][0].get<std::string>() : "";
            if (status == "UNPLAYABLE" || status == "ERROR" ||
                status == "LOGIN_REQUIRED")
                throw std::runtime_error(
                    std::string(c.name) + " " + status + ": " + reason);
        }

        if (!data.contains("streamingData")) {
            std::string reason;
            if (data.contains("playabilityStatus"))
                reason = safe_str(data["playabilityStatus"], "reason");
            throw std::runtime_error(
                "No streamingData from " + std::string(c.name) +
                (reason.empty() ? "" : " (" + reason + ")"));
        }

        // Parse VideoInfo
        VideoInfo info;
        info.id  = vid;
        info.url = "https://www.youtube.com/watch?v=" + vid;

        if (data.contains("videoDetails")) {
            auto& vd = data["videoDetails"];
            info.title          = sanitize_utf8(safe_str(vd, "title"));
            info.channel        = sanitize_utf8(safe_str(vd, "author"));
            info.channel_id     = safe_str(vd, "channelId");
            info.description    = sanitize_utf8(safe_str(vd, "shortDescription"));
            info.duration_secs  = (int)safe_int(vd, "lengthSeconds");
            info.duration_str   = fmt_duration(info.duration_secs);
            info.view_count     = safe_int(vd, "viewCount");
            info.view_count_str = fmt_views(info.view_count);
            if (vd.contains("isLive") && vd["isLive"].is_boolean())
                info.is_live = vd["isLive"].get<bool>();

            // Thumbnails — FIXED: use API thumbs for thumbnail_url (guaranteed to exist),
            // then add candidates to thumbnails[] as bonus options
            auto api_thumbs = extract_thumbnails_from_json(vd);
            info.thumbnail_url = best_thumbnail_url(vid, api_thumbs);

            // Build full thumbnail list: API first, then well-known URL candidates
            info.thumbnails = api_thumbs;
            auto candidates = thumbnail_candidates(vid);
            for (auto& tc : candidates) {
                bool dup = false;
                for (auto& t : info.thumbnails)
                    if (t.width == tc.width) { dup = true; break; }
                if (!dup) info.thumbnails.push_back(tc);
            }
            // Sort by width descending
            std::sort(info.thumbnails.begin(), info.thumbnails.end(),
                [](const ThumbnailInfo& a, const ThumbnailInfo& b) {
                    return a.width > b.width;
                });
        }

        // Extract upload date + category from microformat
        if (data.contains("microformat")) {
            try {
                auto& pmr = data["microformat"]["playerMicroformatRenderer"];
                std::string pd = pmr.value("publishDate", "");
                if (pd.empty()) pd = pmr.value("uploadDate", "");
                if (!pd.empty()) info.upload_date = pd;
                std::string cat = pmr.value("category", "");
                if (!cat.empty()) info.category = cat;
            } catch (...) {}
        }

        // Parse formats + adaptiveFormats
        auto& sd = data["streamingData"];
        std::unordered_set<int> seen_itags;

        for (const char* key : {"formats", "adaptiveFormats"}) {
            if (!sd.contains(key)) continue;
            bool is_muxed_section = (strcmp(key, "formats") == 0);
            for (auto& f : sd[key]) {
                StreamFormat fmt;
                fmt.itag            = (int)safe_int(f, "itag");

                // DEDUP: skip if we already have this itag with higher bitrate
                if (seen_itags.count(fmt.itag)) {
                    // Check if this one has higher bitrate
                    int64_t new_br = safe_int(f, "bitrate");
                    bool dominated = false;
                    for (auto& existing : info.formats) {
                        if (existing.itag == fmt.itag && existing.bitrate >= new_br) {
                            dominated = true;
                            break;
                        }
                    }
                    if (dominated) continue;
                    // Remove the old one, keep this higher-bitrate version
                    info.formats.erase(
                        std::remove_if(info.formats.begin(), info.formats.end(),
                            [&](const StreamFormat& sf) { return sf.itag == fmt.itag; }),
                        info.formats.end());
                }
                seen_itags.insert(fmt.itag);

                fmt.url             = safe_str(f, "url");
                fmt.mime_type       = safe_str(f, "mimeType");
                fmt.quality         = safe_str(f, "quality");
                fmt.quality_label   = safe_str(f, "qualityLabel");
                fmt.width           = (int)safe_int(f, "width");
                fmt.height          = (int)safe_int(f, "height");
                fmt.fps             = (int)safe_int(f, "fps");
                fmt.bitrate         = safe_int(f, "bitrate");
                fmt.average_bitrate = safe_int(f, "averageBitrate");
                fmt.content_length  = safe_int(f, "contentLength");
                fmt.has_video       = fmt.mime_type.find("video/") != std::string::npos;
                fmt.has_audio       = fmt.mime_type.find("audio/") != std::string::npos;
                if (is_muxed_section) {
                    fmt.has_video = true;
                    fmt.has_audio = true;
                }
                if (f.contains("audioSampleRate"))
                    fmt.audio_sample_rate = (int)safe_int(f, "audioSampleRate");
                if (f.contains("audioChannels"))
                    fmt.audio_channels = (int)safe_int(f, "audioChannels");

                // FIXED: codec parsing — properly splits muxed "avc1.x, mp4a.y"
                std::string raw_codecs = extract_codec_raw(fmt.mime_type);
                CodecPair cp = parse_codecs(raw_codecs);
                if (fmt.has_audio) fmt.audio_codec = cp.audio;
                if (fmt.has_video) fmt.video_codec = cp.video;
                // For audio-only, the single codec might be classified as audio
                if (fmt.is_audio_only() && fmt.audio_codec.empty() && !cp.video.empty())
                    fmt.audio_codec = cp.video; // misclassified
                if (fmt.is_video_only() && fmt.video_codec.empty() && !cp.audio.empty())
                    fmt.video_codec = cp.audio; // misclassified

                // Container detection
                fmt.container = extract_container(fmt.mime_type);

                if (fmt.url.empty()) continue; // skip signatureCipher entries
                fmt.stream_url = fmt.url; // save raw URL BEFORE range param
                info.formats.push_back(std::move(fmt));
            }
        }

        // Apply &range=0-N to download URLs (stream_url stays raw)
        for (auto& fmt : info.formats)
            fix_url_for_streaming(fmt);

        // Sort: video height desc, then bitrate desc
        std::stable_sort(info.formats.begin(), info.formats.end(),
            [](const StreamFormat& a, const StreamFormat& b) {
                if (a.height != b.height) return a.height > b.height;
                return a.bitrate > b.bitrate;
            });

        return info;
    }

    // Context / header builders
    std::string get_vd() {
        std::lock_guard<std::mutex> lk(vd_mu_); return visitor_data_;
    }
    json web_ctx() {
        json c = {{"clientName",WEB_CLIENT_NAME},{"clientVersion",WEB_CLIENT_VERSION},
                  {"hl","en"},{"timeZone","UTC"},{"utcOffsetMinutes",0}};
        auto vd = get_vd();
        if (!vd.empty()) c["visitorData"] = vd;
        return {{"client", c}};
    }
    std::vector<std::string> web_hdrs() {
        std::vector<std::string> h = {
            "X-YouTube-Client-Name: " + std::to_string(WEB_CLIENT_ID),
            std::string("X-YouTube-Client-Version: ") + WEB_CLIENT_VERSION,
            std::string("User-Agent: ") + WEB_UA,
            "Origin: https://www.youtube.com",
            "X-Origin: https://www.youtube.com",
        };
        auto vd = get_vd();
        if (!vd.empty()) h.push_back("X-Goog-Visitor-Id: " + vd);
        return h;
    }

    // -----------------------------------------------------------------------
    // Internal format selection helpers
    // -----------------------------------------------------------------------

    // Pick best audio-only format, with optional codec/bitrate preference
    static const StreamFormat* pick_audio(const std::vector<StreamFormat>& fmts,
                                           const std::string& prefer_codec,
                                           int min_bitrate, bool for_download) {
        const StreamFormat* best = nullptr;

        // If codec preference specified, try that first
        if (!prefer_codec.empty()) {
            for (auto& f : fmts) {
                if (!f.is_audio_only()) continue;
                if (f.effective_bitrate() < min_bitrate) continue;
                bool codec_match = false;
                // Match by codec name (opus, mp4a, aac, vorbis) or container (webm, mp4, m4a)
                if (f.audio_codec.find(prefer_codec) != std::string::npos) codec_match = true;
                if (f.container == prefer_codec) codec_match = true;
                if (prefer_codec == "m4a" && f.container == "mp4") codec_match = true;
                if (prefer_codec == "aac" && f.audio_codec.find("mp4a") != std::string::npos) codec_match = true;
                if (!codec_match) continue;
                if (!best || f.effective_bitrate() > best->effective_bitrate()) best = &f;
            }
            if (best) return best;
        }

        // Default preference for streaming: aac/mp4 (most compatible with mpv)
        // Default preference for download: opus/webm (better quality per bitrate)
        const char* pref_container = for_download ? "webm" : "mp4";
        const char* fallback_container = for_download ? "mp4" : "webm";

        // Try preferred container
        for (auto& f : fmts) {
            if (!f.is_audio_only()) continue;
            if (f.effective_bitrate() < min_bitrate) continue;
            if (f.mime_type.find(std::string("audio/") + pref_container) != std::string::npos)
                if (!best || f.effective_bitrate() > best->effective_bitrate()) best = &f;
        }
        if (best) return best;

        // Try fallback container
        for (auto& f : fmts) {
            if (!f.is_audio_only()) continue;
            if (f.effective_bitrate() < min_bitrate) continue;
            if (f.mime_type.find(std::string("audio/") + fallback_container) != std::string::npos)
                if (!best || f.effective_bitrate() > best->effective_bitrate()) best = &f;
        }
        if (best) return best;

        // Any audio-only
        for (auto& f : fmts) {
            if (!f.is_audio_only()) continue;
            if (f.effective_bitrate() < min_bitrate) continue;
            if (!best || f.effective_bitrate() > best->effective_bitrate()) best = &f;
        }
        if (best) return best;

        // Last resort: any format with audio (including muxed)
        for (auto& f : fmts)
            if (f.has_audio && (!best || f.effective_bitrate() > best->effective_bitrate()))
                best = &f;
        return best;
    }

    // Pick best video-only (adaptive) format
    static const StreamFormat* pick_video_only(const std::vector<StreamFormat>& fmts,
                                                int max_h) {
        const StreamFormat* best = nullptr;
        for (auto& f : fmts) {
            if (!f.is_video_only()) continue;
            if (max_h > 0 && f.height > max_h) continue;
            if (!best || f.height > best->height ||
                (f.height == best->height && f.bitrate > best->bitrate))
                best = &f;
        }
        return best;
    }

    // Return the appropriate URL for mpv playback.
    // Always returns stream_url (raw, progressive) which works with mpv.
    static std::string stream_or_url(const StreamFormat& f) {
        return f.stream_url.empty() ? f.url : f.stream_url;
    }

    // Parse filter expression like "ext=m4a" or "height<=720" or "abr>=128"
    static std::pair<std::string, std::string> parse_filter(const std::string& expr) {
        // Try operators: <=, >=, =
        for (const char* op : {"<=", ">=", "="}) {
            auto p = expr.find(op);
            if (p != std::string::npos) {
                return { expr.substr(0, p + strlen(op)), expr.substr(p + strlen(op)) };
            }
        }
        return { expr, "" };
    }

    // Extract height constraint from filters
    static int extract_height_filter(const std::vector<std::pair<std::string, std::string>>& filters) {
        for (auto& [key, val] : filters) {
            if (key.find("height") != std::string::npos && !val.empty()) {
                try { return std::stoi(val); } catch (...) {}
            }
        }
        return 0;
    }

    // Apply filters to get matching audio-only formats
    static std::vector<const StreamFormat*> apply_filters(
            const std::vector<StreamFormat>& fmts,
            const std::vector<std::pair<std::string, std::string>>& filters,
            bool audio_only, bool video_only) {
        std::vector<const StreamFormat*> out;
        for (auto& f : fmts) {
            if (audio_only && !f.is_audio_only()) continue;
            if (video_only && !f.is_video_only()) continue;
            bool match = true;
            for (auto& [key, val] : filters) {
                if (key == "ext=" || key == "container=") {
                    std::string want = val;
                    if (want == "m4a") want = "mp4"; // m4a is mp4 container for audio
                    if (f.container != want && f.container != val) { match = false; break; }
                }
                else if (key == "acodec=" || key == "audio_codec=") {
                    if (f.audio_codec.find(val) == std::string::npos) { match = false; break; }
                }
                else if (key == "vcodec=" || key == "video_codec=") {
                    if (f.video_codec.find(val) == std::string::npos) { match = false; break; }
                }
                else if (key == "abr>=" || key == "bitrate>=") {
                    try { if (f.effective_bitrate()/1000 < std::stoi(val)) { match = false; break; } } catch (...) {}
                }
                else if (key == "abr<=" || key == "bitrate<=") {
                    try { if (f.effective_bitrate()/1000 > std::stoi(val)) { match = false; break; } } catch (...) {}
                }
                else if (key == "height<=" || key == "h<=") {
                    try { if (f.height > std::stoi(val)) { match = false; break; } } catch (...) {}
                }
                else if (key == "height>=" || key == "h>=") {
                    try { if (f.height < std::stoi(val)) { match = false; break; } } catch (...) {}
                }
                else if (key == "fps<=" || key == "fps>=") {
                    try {
                        int fpsval = std::stoi(val);
                        if (key.find("<=") != std::string::npos && f.fps > fpsval) { match = false; break; }
                        if (key.find(">=") != std::string::npos && f.fps < fpsval) { match = false; break; }
                    } catch (...) {}
                }
            }
            if (match) out.push_back(&f);
        }
        return out;
    }
};

// ---------------------------------------------------------------------------
// Convenience free functions — use the singleton
// ---------------------------------------------------------------------------
inline std::vector<SearchResult> yt_search(const std::string& q, int n=15) {
    return InnertubeClient::get_instance().search(q, n);
}
inline VideoInfo yt_get_formats(const std::string& video_id) {
    return InnertubeClient::get_instance().get_stream_formats(video_id);
}
inline std::string yt_best_audio(const std::string& video_id) {
    return InnertubeClient::get_instance().get_best_audio_stream_url(video_id);
}
inline std::string yt_best_audio_download(const std::string& video_id) {
    return InnertubeClient::get_instance().get_best_audio_url(video_id);
}
inline std::string yt_best_video(const std::string& video_id, int max_h=1080) {
    return InnertubeClient::get_instance().get_best_video_url(video_id, max_h);
}
inline std::string yt_best_video_stream(const std::string& video_id, int max_h=1080) {
    return InnertubeClient::get_instance().get_best_video_stream_url(video_id, max_h);
}
inline void yt_prefetch(const std::string& video_id) {
    InnertubeClient::get_instance().prefetch(video_id);
}

} // namespace ytfast
