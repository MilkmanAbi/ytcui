#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include "ytcui-dl/yt_browser.h"

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

    static bool export_session(const std::string& browser, const std::string& path,
                               std::string& error) {
        auto profiles = ytfast::browser::discover();
        std::sort(profiles.begin(), profiles.end(), [](const auto& a, const auto& b) { return a.mtime > b.mtime; });
        for (const auto& profile : profiles) {
            if (profile.browser != browser) continue;
            std::vector<ytfast::browser::Cookie> cookies;
            auto result = ytfast::browser::read_cookies(profile, cookies);
            if (!result.ok || !ytfast::browser::has_auth(cookies)) continue;
            return ytfast::browser::write_netscape(path, cookies, &error);
        }
        error = "No readable signed-in profile; sign in in your browser first";
        return false;
    }

    static void clear_browser() { remove(config_path().c_str()); }

    // Configuring a browser does not verify a signed-in YouTube session.
    static bool is_logged_in() { return false; }

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
            std::error_code ec;
            std::filesystem::create_directories(p.substr(0, pos), ec);
        }
    }
};

} // namespace ytui
