#pragma once

#include <string>
#include <map>
#include "theme.h"

namespace ytui {

struct Config {
    std::string ytdlp_path  = "yt-dlp";
    std::string mpv_path    = "mpv";
    int  max_results        = 15;
    bool prefer_audio       = false;
    bool grayscale          = false;   // legacy
    bool show_thumbnails    = true;

    std::string theme_name  = "default";

    std::string sort_by     = "";
    std::string filter_type = "";
    std::string filter_dur  = "";

    // Per-element colour overrides applied on top of any base theme.
    // Stored under "colors": {} in config.json.
    // Keys: bg, search_box, title, channel, stats, selected, action,
    //       action_sel, status, border, header, accent, tag,
    //       published, bookmark, desc
    // Values: 256-colour index (0-255) or -1 for terminal default.
    std::map<std::string, int> custom_colors;

    Theme get_theme() const {
        if (grayscale) return Theme::Grayscale;
        return string_to_theme(theme_name);
    }

    // Final ThemeColors = base theme + any custom_colors overrides on top.
    ThemeColors resolve_colors() const {
        ThemeColors tc = get_theme_colors(get_theme());
        for (const auto& [key, val] : custom_colors)
            apply_custom_color(tc, key, val);
        return tc;
    }

    void load();
    void save();
    static std::string config_dir();
};

} // namespace ytui
