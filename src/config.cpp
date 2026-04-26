#include "config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdlib>
#include <sys/stat.h>

namespace ytui {
using json = nlohmann::json;

std::string Config::config_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] != '\0') return std::string(xdg) + "/ytcui";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/ytcui";
}

void Config::load() {
    std::string path = config_dir() + "/config.json";
    std::ifstream f(path);
    if (!f.is_open()) return;
    try {
        auto j = json::parse(f);

        if (j.contains("max_results") && j["max_results"].is_number())
            max_results = j["max_results"].get<int>();
        if (j.contains("grayscale") && j["grayscale"].is_boolean())
            grayscale = j["grayscale"].get<bool>();
        if (j.contains("theme") && j["theme"].is_string())
            theme_name = j["theme"].get<std::string>();
        if (j.contains("sort_by") && j["sort_by"].is_string())
            sort_by = j["sort_by"].get<std::string>();
        if (j.contains("show_thumbnails") && j["show_thumbnails"].is_boolean())
            show_thumbnails = j["show_thumbnails"].get<bool>();

        // Per-element color overrides: "colors": { "accent": 198, "title": 213 }
        if (j.contains("colors") && j["colors"].is_object()) {
            custom_colors.clear();
            for (auto& [key, val] : j["colors"].items()) {
                if (val.is_number_integer()) {
                    int v = val.get<int>();
                    if (v >= -1 && v <= 255)
                        custom_colors[key] = v;
                }
            }
        }
    } catch (...) {}
}

void Config::save() {
    std::string dir  = config_dir();
    std::string cmd  = "mkdir -p '" + dir + "'";
    int r = system(cmd.c_str()); (void)r;

    std::string path = dir + "/config.json";
    std::ofstream f(path);
    if (!f.is_open()) return;

    json j = {
        {"max_results",    max_results},
        {"grayscale",      grayscale},
        {"theme",          theme_name},
        {"sort_by",        sort_by},
        {"show_thumbnails", show_thumbnails}
    };

    // Only write colors section if there are custom overrides
    if (!custom_colors.empty()) {
        json colors_obj = json::object();
        for (const auto& [key, val] : custom_colors)
            colors_obj[key] = val;
        j["colors"] = colors_obj;
    }

    f << j.dump(2);
}

} // namespace ytui
