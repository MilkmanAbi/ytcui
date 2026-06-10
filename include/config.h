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

    // Thumbnail rendering mode:
    //   "blocks" — Unicode block art via ncurses colour pairs (DEFAULT;
    //              universal, the safe path that works on every terminal)
    //   "sixel" / "kitty" / "iterm" — real raster images. EXPERIMENTAL and
    //              opt-in: thumbnails are encoded by piping through chafa, which
    //              cannot probe the terminal's cell-pixel size through a pipe,
    //              so sizing/placement may be off on some terminals. Only use
    //              if it looks right on yours.
    //   "auto"   — detect the terminal but still render block art (raster is
    //              never auto-enabled, so it can't break a working TUI); the log
    //              hints which raster mode your terminal could try.
    //   "off"    — no thumbnails
    std::string graphics    = "blocks";

    std::string theme_name  = "default";

    // UI mode: auto (switch to the streamlined music-player layout when the
    // terminal is very narrow), normal (always full UI), streamlined (always).
    std::string mode        = "auto";

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
