#include "core/sponsorblock.h"

#include "core/log.h"
#include "core/text.h"

#include <algorithm>
#include <cstdio>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "yj.h"
#include "yt_http.h"

namespace ytcui::sb {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    return s;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += (char)c;
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

}  // namespace

std::vector<std::string> split_categories(const std::string& csv) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= csv.size()) {
        const std::size_t comma = csv.find(',', start);
        std::string one = trim(csv.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!one.empty()) out.push_back(lower(one));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

std::string hash_prefix(const std::string& video_id, int chars) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int len = 0;
    // EVP rather than the deprecated SHA256() one-shot: OpenSSL 3 warns on the
    // latter, and -Werror turns that into a build failure.
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};
    std::string out;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, video_id.data(), video_id.size()) == 1 &&
        EVP_DigestFinal_ex(ctx, digest, &len) == 1) {
        static const char* hex = "0123456789abcdef";
        for (unsigned int i = 0; i < len; ++i) {
            out += hex[digest[i] >> 4];
            out += hex[digest[i] & 15];
        }
    }
    EVP_MD_CTX_free(ctx);
    if (chars > 0 && (int)out.size() > chars) out.resize((std::size_t)chars);
    return out;
}

double Segments::total_skippable() const {
    double t = 0;
    for (const Segment& s : list)
        if (s.skippable()) t += s.length();
    return t;
}

std::string Segments::summary() const {
    if (list.empty()) return {};
    int n = 0;
    for (const Segment& s : list)
        if (s.skippable()) ++n;
    if (n == 0) return {};
    return std::to_string(n) + (n == 1 ? " segment · " : " segments · ") +
           txt::duration((int)(total_skippable() + 0.5)) + " skippable";
}

double Segments::skip_target(double pos, double grace) const {
    double best = -1;
    for (const Segment& s : list) {
        if (!s.skippable()) continue;
        // A segment that starts at 0 is an intro; entering at exactly 0 counts.
        if (pos + grace >= s.start && pos < s.end - grace) {
            // Chained segments: take the furthest end so two adjacent sponsor
            // reads are one jump rather than two visible stutters.
            if (s.end > best) best = s.end;
        }
    }
    if (best < 0) return -1;
    // Resolve a chain: a second segment may begin where the first ended.
    bool moved = true;
    int guard = 0;
    while (moved && guard++ < 16) {
        moved = false;
        for (const Segment& s : list) {
            if (!s.skippable()) continue;
            if (best + grace >= s.start && best < s.end - grace) {
                best = s.end;
                moved = true;
            }
        }
    }
    return best;
}

Segments parse(const std::string& json, const std::string& video_id,
               const std::string& wanted_categories) {
    Segments out;
    const std::vector<std::string> wanted = split_categories(wanted_categories);

    yj::Val root = yj::parse(json);
    if (!root.is_arr()) {
        out.error = "unexpected response";
        return out;
    }
    out.fetched = true;

    yj::each_elem(root, [&](yj::Val entry) {
        yj::Val id = yj::get(entry, "videoID");
        if (!id.is_str() || yj::unescape(id.raw()) != video_id) return true;

        yj::each_elem(yj::get(entry, "segments"), [&](yj::Val seg) {
            Segment s;
            if (yj::Val c = yj::get(seg, "category"); c.is_str())
                s.category = lower(yj::unescape(c.raw()));
            if (yj::Val a = yj::get(seg, "actionType"); a.is_str())
                s.action = lower(yj::unescape(a.raw()));
            if (yj::Val u = yj::get(seg, "UUID"); u.is_str())
                s.uuid = yj::unescape(u.raw());
            if (yj::Val v = yj::get(seg, "votes"); v.valid() && !v.is_str())
                s.votes = (int)v.i64();
            if (yj::Val l = yj::get(seg, "locked"); l.valid() && !l.is_str())
                s.locked = l.boolean();

            // "segment" is a two-element [start, end] array of seconds.
            int i = 0;
            yj::each_elem(yj::get(seg, "segment"), [&](yj::Val t) {
                const double v = t.dbl();
                if (i == 0) s.start = v;
                else if (i == 1) s.end = v;
                ++i;
                return true;
            });

            if (i < 2 || s.end <= s.start) return true;          // malformed

            // Keep only what this actually acts on. "full" marks an entire
            // video as sponsor -- seeking to the end of that would skip the
            // video -- and "poi" is a highlight marker, not a segment. They
            // are dropped here rather than stored and filtered at every use,
            // because a list holding things that must never be skipped is a
            // trap for the next person who iterates it.
            if (!s.skippable()) return true;
            if (!wanted.empty() &&
                std::find(wanted.begin(), wanted.end(), s.category) == wanted.end())
                return true;                                     // not wanted
            // Heavily downvoted segments are usually wrong; the extension
            // applies the same kind of filter.
            if (s.votes < -1) return true;
            out.list.push_back(std::move(s));
            return true;
        });
        return true;
    });

    std::sort(out.list.begin(), out.list.end(),
              [](const Segment& a, const Segment& b) { return a.start < b.start; });
    return out;
}

Segments fetch(const std::string& video_id, const std::string& categories,
               int timeout_ms) {
    Segments out;
    if (video_id.empty()) return out;

    const std::string prefix = hash_prefix(video_id, 4);
    if (prefix.empty()) {
        out.error = "could not hash the video id";
        return out;
    }

    // The categories go in as a JSON array, url-encoded.
    std::string cats = "[";
    bool first = true;
    for (const std::string& c : split_categories(categories)) {
        if (!first) cats += ',';
        cats += '"' + c + '"';
        first = false;
    }
    cats += ']';

    const std::string url = "https://sponsor.ajay.app/api/skipSegments/" + prefix +
                            "?categories=" + url_encode(cats);
    try {
        ytfast::HttpClient http;
        http.set_timeouts(timeout_ms, timeout_ms);
        const auto r = http.get(url);
        if (r.status == 404) {
            // Nothing submitted for anything in this bucket. Not an error.
            out.fetched = true;
            return out;
        }
        if (r.status != 200) {
            out.error = "HTTP " + std::to_string(r.status);
            return out;
        }
        out = parse(r.body, video_id, categories);
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

}  // namespace ytcui::sb
