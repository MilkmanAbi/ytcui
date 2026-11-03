#include "config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdlib>
#include <sys/stat.h>

namespace ytui {
using json = nlohmann::json;

std::string Config::key_display(int k) {
    switch (k) {
        case ' ':  return "Space";
        case '\n': case '\r': return "Enter";
        case 27:   return "Esc";
        case '\t': return "Tab";
        case 127:  return "Backspace";
        case 259:  return "Up";
        case 258:  return "Down";
        case 260:  return "Left";
        case 261:  return "Right";
    }
    if (k >= 1 && k <= 26) {           // control chars -> ^A .. ^Z
        std::string s = "^";
        s += (char)('A' + k - 1);
        return s;
    }
    if (k >= 32 && k < 127) {          // printable
        return std::string(1, (char)k);
    }
    return "key#" + std::to_string(k);
}

std::string Config::key_config_name(int k) {
    switch (k) {
        case ' ':  return "space";
        case '\n': case '\r': return "enter";
        case 27:   return "escape";
        case '\t': return "tab";
        case 127:  return "backspace";
        case 330:  return "delete";
        case 259:  return "up";
        case 258:  return "down";
        case 260:  return "left";
        case 261:  return "right";
    }
    if (k >= 32 && k < 127) return std::string(1, (char)k);   // printable ASCII
    // Anything else (control chars, ncurses KEY_ codes, or an invalid -1):
    // don't emit a raw byte — it may not be valid UTF-8 and would break JSON
    // serialization. Encode as a stable token that parse_key_name ignores,
    // leaving the default binding intact on reload.
    return std::string("key") + std::to_string(k);
}



// Parse a key name from config.json: "space" -> ' ', "escape" -> 27, "a" -> 'a'
static int parse_key_name(const std::string& name) {
    if (name == "space")   return ' ';
    if (name == "enter" || name == "return") return '\n';
    if (name == "escape" || name == "esc")   return 27;
    if (name == "tab")     return '\t';
    if (name == "backspace") return 127;
    if (name == "delete")  return 330;  // KEY_DC
    if (name == "up")      return 259;  // KEY_UP
    if (name == "down")    return 258;  // KEY_DOWN
    if (name == "left")    return 260;  // KEY_LEFT
    if (name == "right")   return 261;  // KEY_RIGHT
    if (name.size() == 1)  return (unsigned char)name[0];
    return -1;
}



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
        if (j.contains("no_hardware_accel") && j["no_hardware_accel"].is_boolean())
            no_hardware_accel = j["no_hardware_accel"].get<bool>();
        if (j.contains("sort_by") && j["sort_by"].is_string())
            sort_by = j["sort_by"].get<std::string>();
        if (j.contains("show_thumbnails") && j["show_thumbnails"].is_boolean())
            show_thumbnails = j["show_thumbnails"].get<bool>();
        if (j.contains("graphics") && j["graphics"].is_string())
            graphics = j["graphics"].get<std::string>();
        if (j.contains("mode") && j["mode"].is_string())
            mode = j["mode"].get<std::string>();
        if (j.contains("force_features") && j["force_features"].is_boolean())
            force_features = j["force_features"].get<bool>();

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

        // Keybindings: "keys": { "pause": "space", "volume_up": "=", ... }
        if (j.contains("keys") && j["keys"].is_object()) {
            auto bind = [&](const char* name, int& target) {
                if (j["keys"].contains(name) && j["keys"][name].is_string()) {
                    int k = parse_key_name(j["keys"][name].get<std::string>());
                    if (k >= 0) target = k;
                }
            };
            bind("pause",       keys.pause);
            bind("volume_up",   keys.volume_up);
            bind("volume_down", keys.volume_down);
            bind("seek_fwd",    keys.seek_fwd);
            bind("seek_back",   keys.seek_back);
            bind("quit",        keys.quit);
            bind("search",      keys.search);
            bind("scroll_up",   keys.scroll_up);
            bind("scroll_down", keys.scroll_down);
            bind("select",      keys.select);
            bind("back",        keys.back);
            bind("top",         keys.top);
            bind("bottom",      keys.bottom);
            bind("sort",        keys.sort);
            bind("new_playlist", keys.new_playlist);
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
        {"no_hardware_accel", no_hardware_accel},
        {"sort_by",        sort_by},
        {"show_thumbnails", show_thumbnails},
        {"graphics",       graphics},
        {"mode",           mode},
        {"force_features", force_features}
    };

    // Only write colors section if there are custom overrides
    if (!custom_colors.empty()) {
        json colors_obj = json::object();
        for (const auto& [key, val] : custom_colors)
            colors_obj[key] = val;
        j["colors"] = colors_obj;
    }

    // Keybindings — always written so in-app rebinds (Ctrl-S settings) persist.
    // key_config_name produces the parseable form ("space", "x") that load()
    // understands, not the display form ("Space", "^Q").
    {
        json keys_obj = json::object();
        keys_obj["pause"]        = key_config_name(keys.pause);
        keys_obj["volume_up"]    = key_config_name(keys.volume_up);
        keys_obj["volume_down"]  = key_config_name(keys.volume_down);
        keys_obj["seek_fwd"]     = key_config_name(keys.seek_fwd);
        keys_obj["seek_back"]    = key_config_name(keys.seek_back);
        keys_obj["search"]       = key_config_name(keys.search);
        keys_obj["scroll_up"]    = key_config_name(keys.scroll_up);
        keys_obj["scroll_down"]  = key_config_name(keys.scroll_down);
        keys_obj["top"]          = key_config_name(keys.top);
        keys_obj["bottom"]       = key_config_name(keys.bottom);
        keys_obj["sort"]         = key_config_name(keys.sort);
        keys_obj["new_playlist"] = key_config_name(keys.new_playlist);
        keys_obj["quit"]         = key_config_name(keys.quit);
        j["keys"] = keys_obj;
    }

    f << j.dump(2);
}

} // namespace ytui
