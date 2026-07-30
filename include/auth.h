#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace ytui {

class Auth {
public:
    static std::vector<std::string> detect_browsers() {
        std::vector<std::string> found;
        const char* names[] = {
            "firefox", "chrome", "chromium", "brave",
            "edge", "opera", "vivaldi", "whale"
        };
        const char* bins[] = {
            "firefox", "google-chrome-stable", "chromium-browser", "brave-browser",
            "microsoft-edge-stable", "opera", "vivaldi-stable", "naver-whale"
        };
        const char* alts[] = {
            "firefox", "google-chrome", "chromium", "brave-browser-stable",
            "microsoft-edge", "opera", "vivaldi", "whale"
        };

        for (int i = 0; i < 8; i++) {
            std::string cmd1 = std::string("which ") + bins[i] + " >/dev/null 2>&1";
            std::string cmd2 = std::string("which ") + alts[i] + " >/dev/null 2>&1";
            if (system(cmd1.c_str()) == 0 || system(cmd2.c_str()) == 0) {
                found.push_back(names[i]);
            }
        }

        std::sort(found.begin(), found.end());
        found.erase(std::unique(found.begin(), found.end()), found.end());

        return found;
    }

    static std::string get_configured_browser() {
        std::string path = config_path();
        std::ifstream f(path);
        if (f.is_open()) {
            std::string browser;
            std::getline(f, browser);
            if (!browser.empty()) return browser;
        }
        return "";
    }

    static void set_browser(const std::string& browser) {
        ensure_dir();
        std::ofstream f(config_path());
        if (f.is_open()) f << browser << "\n";
    }

    static void clear_browser() { remove(config_path().c_str()); }

    static bool is_logged_in() { return !get_configured_browser().empty(); }

    static std::string ytdlp_cookie_args() {
        std::string b = get_configured_browser();
        if (b.empty()) return "";
        return "--cookies-from-browser " + b;
    }

private:
    static std::string config_path() {
        const char* xdg = getenv("XDG_CONFIG_HOME");
        std::string dir;
        if (xdg && xdg[0] != '\0')
            dir = std::string(xdg) + "/ytcui";
        else {
            const char* home = getenv("HOME");
            dir = std::string(home ? home : "/tmp") + "/.config/ytcui";
        }
        return dir + "/browser";
    }

    static void ensure_dir() {
        std::string p = config_path();
        size_t pos = p.rfind('/');
        if (pos != std::string::npos) {
            std::string cmd = "mkdir -p '" + p.substr(0, pos) + "'";
            int r = system(cmd.c_str()); (void)r;
        }
    }
};

} // namespace ytui
