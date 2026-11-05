#include "core/config.h"

#include "core/paths.h"
#include "core/theme.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "yj.h"
#include "yjw.h"

namespace ytcui::config {
namespace {

std::string lower(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return o;
}

// yj's raw() is deliberately string-only -- it returns an empty view for a
// number -- so a value has to be read through the accessor matching its type.
// Getting this wrong is silent: "accent": 198 parsed as "" and the override
// vanished, which is exactly the bug the config tests caught.
std::string text_of(yj::Val v) {
    if (!v.valid()) return {};
    if (v.is_str()) return yj::unescape(v.raw());
    switch (v.type()) {
        case yj::T::Num: {
            const double d = v.dbl();
            const int64_t i = v.i64();
            // Colour indices and counts are integers; only spell out a
            // fraction when there actually is one.
            if (d == (double)i) return std::to_string(i);
            std::string s = std::to_string(d);
            while (s.size() > 1 && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
            return s;
        }
        case yj::T::Bool: return v.boolean() ? "true" : "false";
        default: return {};
    }
}

bool truthy(yj::Val v, bool fallback) {
    if (!v.valid()) return fallback;
    if (v.is_str()) {
        const std::string s = lower(yj::unescape(v.raw()));
        if (s == "true" || s == "yes" || s == "on" || s == "1") return true;
        if (s == "false" || s == "no" || s == "off" || s == "0") return false;
        return fallback;
    }
    return v.boolean();
}

}  // namespace

const char* to_string(UiMode m) {
    switch (m) {
        case UiMode::Normal:      return "normal";
        case UiMode::Streamlined: return "streamlined";
        case UiMode::Minimal:     return "minimal";
        case UiMode::Auto:
        default:                  return "auto";
    }
}

const char* to_string(Backend b) {
    return b == Backend::YtDlp ? "yt-dlp" : "ytcui-dl";
}

UiMode parse_mode(std::string_view s, bool* ok) {
    const std::string v = lower(s);
    if (ok) *ok = true;
    if (v == "auto" || v.empty())                  return UiMode::Auto;
    if (v == "normal" || v == "full")              return UiMode::Normal;
    if (v == "streamlined" || v == "stream" ||
        v == "compact" || v == "player")           return UiMode::Streamlined;
    if (v == "minimal" || v == "min" ||
        v == "script" || v == "inline")            return UiMode::Minimal;
    if (ok) *ok = false;
    return UiMode::Auto;
}

Backend parse_backend(std::string_view s, bool* ok) {
    const std::string v = lower(s);
    if (ok) *ok = true;
    if (v == "ytcui-dl" || v == "ytcuidl" || v == "ytcui" ||
        v == "builtin" || v == "internal")  return Backend::YtcuiDl;
    if (v == "yt-dlp" || v == "ytdlp" ||
        v == "youtube-dl" || v == "dlp")    return Backend::YtDlp;
    if (ok) *ok = false;
    return Backend::YtcuiDl;
}

// ---------------------------------------------------------------------------
Config parse_string(const std::string& body, const std::string& origin) {
    Config c;
    yj::Val root = yj::parse(body);
    if (!root.is_obj()) {
        c.warnings.push_back(origin + ": not a JSON object; using defaults");
        return c;
    }

    if (yj::Val v = yj::get(root, "theme"); v.valid()) c.theme = text_of(v);
    if (yj::Val v = yj::get(root, "graphics"); v.valid()) c.graphics = lower(text_of(v));
    if (yj::Val v = yj::get(root, "sort_by"); v.valid()) c.sort_by = text_of(v);
    if (yj::Val v = yj::get(root, "mpv"); v.valid()) c.mpv_extra = text_of(v);

    if (yj::Val v = yj::get(root, "mode"); v.valid()) {
        bool ok = false;
        c.mode = parse_mode(text_of(v), &ok);
        if (!ok) c.warnings.push_back(origin + ": unknown mode '" + text_of(v) + "'");
    }
    if (yj::Val v = yj::get(root, "backend"); v.valid()) {
        bool ok = false;
        c.backend = parse_backend(text_of(v), &ok);
        if (!ok) c.warnings.push_back(origin + ": unknown backend '" + text_of(v) + "'");
    }

    if (yj::Val v = yj::get(root, "max_results"); v.valid()) {
        const int n = (int)v.i64();
        // A zero or negative page size would render an empty list forever and
        // look like a broken search rather than a bad setting.
        if (n > 0 && n <= 200) c.max_results = n;
        else c.warnings.push_back(origin + ": max_results out of range (1-200)");
    }
    if (yj::Val v = yj::get(root, "volume"); v.valid()) {
        const int n = (int)v.i64();
        if (n >= 0 && n <= 130) c.volume = n;
        else c.warnings.push_back(origin + ": volume out of range (0-130)");
    }

    c.no_hardware_accel = truthy(yj::get(root, "no_hardware_accel"), c.no_hardware_accel);
    c.show_thumbnails   = truthy(yj::get(root, "show_thumbnails"),   c.show_thumbnails);
    c.force_features    = truthy(yj::get(root, "force_features"),    c.force_features);
    c.backend_auto_failover =
        truthy(yj::get(root, "backend_auto_failover"), c.backend_auto_failover);
    c.qr_light_background =
        truthy(yj::get(root, "qr_light_background"), c.qr_light_background);
    c.sponsorblock = truthy(yj::get(root, "sponsorblock"), c.sponsorblock);
    c.resume_playback = truthy(yj::get(root, "resume_playback"), c.resume_playback);
    c.loop_video = truthy(yj::get(root, "loop_video"), c.loop_video);
    if (yj::Val v = yj::get(root, "autoplay"); v.valid()) {
        const std::string a = lower(text_of(v));
        if (a == "off" || a == "none" || a == "false") c.autoplay = "off";
        else if (a == "list" || a == "next" || a == "true") c.autoplay = "list";
        else if (a == "radio" || a == "endless") c.autoplay = "radio";
        else c.warnings.push_back(origin + ": unknown autoplay '" + a +
                                  "' (off | list | radio)");
    }
    if (yj::Val v = yj::get(root, "sponsorblock_categories"); v.valid())
        c.sponsorblock_categories = text_of(v);
    if (yj::Val v = yj::get(root, "cookies_file"); v.valid())
        c.cookies_file = text_of(v);

    // "colors": element -> 214 | "#8be9fd" | "brightblue"
    if (yj::Val colors = yj::get(root, "colors"); colors.is_obj()) {
        const auto& valid = theme::Palette::element_names();
        yj::each_member(colors, [&](std::string_view k, yj::Val v) {
            const std::string key(k);
            if (std::find(valid.begin(), valid.end(), key) == valid.end()) {
                c.warnings.push_back(origin + ": unknown colour element '" + key + "'");
                return true;
            }
            c.color_overrides[key] = text_of(v);
            return true;
        });
    }

    // "keys": action id -> key text. v4's ids are accepted too; see
    // keymap.cpp's action_for_id.
    if (yj::Val k = yj::get(root, "keys"); k.is_obj()) {
        yj::each_member(k, [&](std::string_view id, yj::Val v) {
            const std::string key_text = text_of(v);
            if (!c.keymap.apply(id, key_text))
                c.warnings.push_back(origin + ": cannot bind '" + std::string(id) +
                                     "' to '" + key_text + "'");
            return true;
        });
    }

    return c;
}

Config load() { return load_from(paths::config_file()); }

Config load_from(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Config{};              // no file is the normal first run
    std::stringstream ss;
    ss << f.rdbuf();
    Config c = parse_string(ss.str(), path);
    c.loaded_from_file = true;
    return c;
}

// ---------------------------------------------------------------------------
std::string serialize(const Config& c) {
    const Config def;
    yjw::Writer w;
    w.obj();

    if (c.theme != def.theme)                 w.kv("theme", c.theme);
    if (c.graphics != def.graphics)           w.kv("graphics", c.graphics);
    if (c.mode != def.mode)                   w.kv("mode", to_string(c.mode));
    if (c.backend != def.backend)             w.kv("backend", to_string(c.backend));
    if (c.max_results != def.max_results)     w.kv("max_results", c.max_results);
    if (c.volume != def.volume)               w.kv("volume", c.volume);
    if (!c.sort_by.empty())                   w.kv("sort_by", c.sort_by);
    if (!c.mpv_extra.empty())                 w.kv("mpv", c.mpv_extra);
    if (c.no_hardware_accel != def.no_hardware_accel)
        w.kv("no_hardware_accel", c.no_hardware_accel);
    if (c.show_thumbnails != def.show_thumbnails)
        w.kv("show_thumbnails", c.show_thumbnails);
    if (c.force_features != def.force_features)
        w.kv("force_features", c.force_features);
    if (c.backend_auto_failover != def.backend_auto_failover)
        w.kv("backend_auto_failover", c.backend_auto_failover);
    if (c.qr_light_background != def.qr_light_background)
        w.kv("qr_light_background", c.qr_light_background);
    if (c.sponsorblock != def.sponsorblock)
        w.kv("sponsorblock", c.sponsorblock);
    if (c.sponsorblock_categories != def.sponsorblock_categories)
        w.kv("sponsorblock_categories", c.sponsorblock_categories);
    if (c.resume_playback != def.resume_playback)
        w.kv("resume_playback", c.resume_playback);
    if (c.loop_video != def.loop_video)
        w.kv("loop_video", c.loop_video);
    if (c.autoplay != def.autoplay)
        w.kv("autoplay", c.autoplay);
    if (!c.cookies_file.empty())
        w.kv("cookies_file", c.cookies_file);

    if (!c.color_overrides.empty()) {
        w.key("colors").obj();
        for (const auto& [k, v] : c.color_overrides) w.kv(k, v);
        w.end();
    }

    const auto rebound = c.keymap.diff_from_defaults();
    if (!rebound.empty()) {
        w.key("keys").obj();
        for (const auto& [id, key] : rebound) w.kv(id, key);
        w.end();
    }

    w.end();
    return w.str();
}

bool save(const Config& c) { return save_to(c, paths::config_file()); }

bool save_to(const Config& c, const std::string& path) {
    const auto slash = path.rfind('/');
    if (slash != std::string::npos && !paths::ensure_dir(path.substr(0, slash)))
        return false;

    // Write a temp file and rename, so an interrupted save cannot leave a
    // half-written config that fails to parse on next launch.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << serialize(c) << "\n";
        if (!f.good()) return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace ytcui::config
