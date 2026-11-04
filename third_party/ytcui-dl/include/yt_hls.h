#pragma once
#include "yt_http.h"
#include "yt_select.h"
#include <map>
#include <sstream>
#include <stdexcept>

namespace ytfast {
inline std::string hls_join(const std::string& base, const std::string& relative) {
    if (relative.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error("invalid HLS URI");
    if (relative.rfind("https://", 0) == 0 || relative.rfind("http://", 0) == 0) return relative;
    Url u;
    if (!Url::parse(base, u)) throw std::runtime_error("invalid HLS base URL");
    const std::string scheme = u.tls ? "https:" : "http:";
    if (relative.rfind("//", 0) == 0) return scheme + relative;
    if (relative.find(':') != std::string::npos) throw std::runtime_error("unsupported HLS URI scheme");
    std::string authority = u.host.find(':') == std::string::npos ? u.host : "[" + u.host + "]";
    if (u.port != (u.tls ? "443" : "80")) authority += ":" + u.port;
    auto path = relative.empty() ? u.path : (relative.front() == '/' ? relative :
        u.path.substr(0, u.path.find_last_of('/') + 1) + relative);
    // Normalize dot segments without touching the signed query string.
    const auto query = path.find('?');
    const auto suffix = query == std::string::npos ? "" : path.substr(query);
    path = path.substr(0, query);
    std::vector<std::string> segments;
    std::istringstream parts(path);
    for (std::string item; std::getline(parts, item, '/');) {
        if (item == "..") { if (!segments.empty()) segments.pop_back(); }
        else if (!item.empty() && item != ".") segments.push_back(item);
    }
    std::string result = scheme + "//" + authority;
    for (const auto& item : segments) result += "/" + item;
    return result + (segments.empty() ? "/" : "") + suffix;
}
inline std::map<std::string, std::string> hls_attributes(const std::string& line) {
    std::map<std::string, std::string> out;
    for (size_t p = 0; p < line.size();) {
        const auto equals = line.find('=', p);
        if (equals == std::string::npos) break;
        auto key = line.substr(p, equals - p);
        p = equals + 1;
        std::string value;
        if (p < line.size() && line[p] == '"') {
            const auto end = line.find('"', ++p);
            if (end == std::string::npos) throw std::runtime_error("unterminated HLS attribute");
            value = line.substr(p, end - p); p = end + 1;
        } else {
            const auto end = line.find(',', p);
            value = line.substr(p, end - p); p = end == std::string::npos ? line.size() : end;
        }
        out[key] = value;
        if (p < line.size() && line[p] == ',') ++p;
    }
    return out;
}
struct HlsVariant {
    std::string url, audio_group, codecs;
    int width = 0, height = 0;
    double fps = 0;
    int64_t bandwidth = 0;
};
struct HlsAudio { std::string url, group; bool preferred = false; };
struct HlsManifest {
    std::vector<HlsVariant> variants;
    std::vector<HlsAudio> audio;
    static HlsManifest parse(const std::string& body, const std::string& base) {
        if (body.rfind("#EXTM3U", 0) != 0) throw std::runtime_error("invalid HLS manifest");
        HlsManifest result;
        std::istringstream input(body);
        std::map<std::string, std::string> pending;
        for (std::string line; std::getline(input, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0) pending = hls_attributes(line.substr(18));
            else if (line.rfind("#EXT-X-MEDIA:", 0) == 0) {
                auto attr = hls_attributes(line.substr(13));
                if (attr["TYPE"] == "AUDIO" && !attr["URI"].empty())
                    result.audio.push_back({hls_join(base, attr["URI"]), attr["GROUP-ID"], attr["DEFAULT"] == "YES"});
            } else if (!line.empty() && line[0] != '#' && !pending.empty()) {
                HlsVariant variant;
                variant.url = hls_join(base, line);
                variant.audio_group = pending["AUDIO"];
                variant.codecs = pending["CODECS"];
                const auto resolution = pending["RESOLUTION"];
                const auto split = resolution.find('x');
                if (split != std::string::npos) {
                    variant.width = std::atoi(resolution.substr(0, split).c_str());
                    variant.height = std::atoi(resolution.substr(split + 1).c_str());
                }
                variant.fps = std::atof(pending["FRAME-RATE"].c_str());
                variant.bandwidth = std::strtoll(pending["BANDWIDTH"].c_str(), nullptr, 10);
                result.variants.push_back(std::move(variant)); pending.clear();
            }
        }
        return result;
    }
    const HlsVariant* select(const Quality& quality) const {
        const HlsVariant* best = nullptr;
        for (int pass = 0; pass != 2; ++pass) {
            for (const auto& variant : variants) {
                if (quality.max_height > 0 && (variant.height <= 0 || variant.height > quality.max_height)) continue;
                if (quality.max_bitrate > 0 && variant.bandwidth > quality.max_bitrate) continue;
                if (quality.height > 0 && ((pass == 0 && variant.height > quality.height) ||
                                          (pass == 1 && variant.height <= quality.height))) continue;
                const bool low = quality.smallest || pass == 1;
                if (!best || (variant.height != best->height ? (low ? variant.height < best->height : variant.height > best->height) :
                    (variant.fps != best->fps ? variant.fps > best->fps : variant.bandwidth > best->bandwidth))) best = &variant;
            }
            if (best || quality.height == 0) break;
        }
        return best;
    }
    std::string audio_for(const HlsVariant& variant) const {
        const HlsAudio* found = nullptr;
        for (const auto& track : audio)
            if (track.group == variant.audio_group && (!found || track.preferred)) found = &track;
        return found ? found->url : "";
    }
};
} // namespace ytfast
