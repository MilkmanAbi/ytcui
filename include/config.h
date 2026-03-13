#pragma once

#include <string>
#include "theme.h"

namespace ytui {

struct Config {
    std::string ytdlp_path = "yt-dlp";
    std::string mpv_path = "mpv";
    int max_results = 15;
    bool prefer_audio = false;
    bool grayscale = false;  // Legacy, use theme instead
    
    // Theme setting
    std::string theme_name = "default";

    std::string sort_by = "";
    std::string filter_type = "";
    std::string filter_dur = "";
    
    // Get theme enum from config
    Theme get_theme() const {
        if (grayscale) return Theme::Grayscale;
        return string_to_theme(theme_name);
    }

    void load();
    void save();
    static std::string config_dir();
};

} // namespace ytui
