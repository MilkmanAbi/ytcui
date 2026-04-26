#pragma once
// ── EP1: ExperimentalPatch1 ──────────────────────────────────────────────────
// Replaces --colors=none monochrome rendering with 256-colour ncurses pairs.
// Strategy: run chafa --colors=256, parse \033[38;5;Nm sequences, emit each
// glyph via attron(COLOR_PAIR(THUMB_COLOR_BASE + N)) + addstr(glyph).
// ncurses fully owns every character and every colour attribute — erase()
// clears it all correctly. No ghosting. Works on every 256-colour terminal:
// xterm, gnome-terminal, macOS Terminal, blackbox, konsole, etc.
//
// Falls back to --colors=none monochrome if COLORS < 256 or COLOR_PAIRS < 273.
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <vector>
#include <utility>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace ytui {

// Color pairs 1-16 are reserved for UI elements (Color::BG … Color::DESC).
// Pairs 17-272 are reserved for thumbnail 256-color rendering.
// THUMB_COLOR_BASE + N maps color index N (0-255) → pair (17+N).
constexpr int THUMB_COLOR_BASE = 17;

// ─── Structured cell returned by render_color() ───────────────────────────────
struct ThumbCell {
    std::string glyph;    // UTF-8 character(s) — typically one halfblock
    int color_idx = -1;   // 256-colour foreground index, -1 = terminal default
};
using ThumbLine = std::vector<ThumbCell>;
using ThumbData = std::vector<ThumbLine>;

class Thumbnails {
public:
    // ── Cache ────────────────────────────────────────────────────────────────
    static std::string cache_dir() {
        const char* xdg = getenv("XDG_CACHE_HOME");
        std::string dir;
        if (xdg && xdg[0] != '\0') dir = std::string(xdg) + "/ytcui/thumbs";
        else {
            const char* home = getenv("HOME");
            dir = std::string(home ? home : "/tmp") + "/.cache/ytcui/thumbs";
        }
        return dir;
    }

    static std::string thumb_path(const std::string& video_id) {
        return cache_dir() + "/" + video_id + ".jpg";
    }

    static bool is_cached(const std::string& video_id) {
        if (video_id.empty()) return false;
        struct stat st;
        return stat(thumb_path(video_id).c_str(), &st) == 0 && st.st_size > 1024;
    }

    static bool renderer_available() {
        return system("which chafa > /dev/null 2>&1") == 0;
    }

    // ── Download (async, fork+exec) ──────────────────────────────────────────
    static void download_async(const std::string& video_id, const std::string& url) {
        if (url.empty() || video_id.empty() || is_cached(video_id)) return;
        mkdir_p(cache_dir());
        std::string path = thumb_path(video_id);
        std::string hq   = "https://i.ytimg.com/vi/" + video_id + "/hqdefault.jpg";
        pid_t pid = fork();
        if (pid == 0) {
            int dn = open("/dev/null", O_RDWR);
            if (dn >= 0) { dup2(dn,0); dup2(dn,1); dup2(dn,2); close(dn); }
            std::string sh =
                "curl -sL -o '" + path + "' --max-time 8 '" + url + "' && "
                "[ $(stat -c%s '" + path + "' 2>/dev/null || echo 0) -gt 1024 ] || "
                "curl -sL -o '" + path + "' --max-time 8 '" + hq + "'";
            execlp("sh", "sh", "-c", sh.c_str(), nullptr);
            _exit(1);
        }
        (void)pid;
    }

    static void download_batch(const std::vector<std::pair<std::string,std::string>>& items) {
        for (const auto& [id, url] : items) download_async(id, url);
        int st; while (waitpid(-1, &st, WNOHANG) > 0) {}
    }

    // ── ANSI parser ───────────────────────────────────────────────────────────
    // Parses chafa --colors=256 output into ThumbData.
    // Only handles the sequences chafa actually emits:
    //   \033[38;5;Nm   → set fg to colour index N
    //   \033[0m / \033[m → reset fg to default
    //   Everything else → ignored
    static ThumbData parse_ansi(const std::string& raw) {
        ThumbData result;
        ThumbLine line;
        int fg = -1;
        size_t i = 0, len = raw.size();

        while (i < len) {
            unsigned char c = (unsigned char)raw[i];

            if (c == '\n') {
                result.push_back(std::move(line));
                line.clear();
                i++;
                continue;
            }
            if (c == '\r') { i++; continue; }

            // ESC [ … m  — ANSI escape sequence
            if (c == 0x1b && i + 1 < len && raw[i+1] == '[') {
                i += 2;
                std::string seq;
                // Read until a final byte (letters). chafa uses only 'm'.
                while (i < len && raw[i] != 'm' && raw[i] != 'A' && raw[i] != 'B'
                       && raw[i] != 'C' && raw[i] != 'D' && raw[i] != 'H'
                       && raw[i] != 'J' && raw[i] != 'K') {
                    seq += raw[i++];
                }
                if (i < len) i++; // consume terminator

                if (seq.empty() || seq == "0") {
                    fg = -1; // reset
                } else if (seq.size() > 5 && seq.substr(0, 5) == "38;5;") {
                    try { fg = std::stoi(seq.substr(5)); }
                    catch (...) { fg = -1; }
                    if (fg < 0 || fg > 255) fg = -1;
                }
                // 48;5;N (bg) and other sequences — ignore
                continue;
            }

            // UTF-8 codepoint — collect all continuation bytes
            std::string glyph;
            int bytes = 1;
            if      ((c & 0xE0) == 0xC0) bytes = 2;
            else if ((c & 0xF0) == 0xE0) bytes = 3;
            else if ((c & 0xF8) == 0xF0) bytes = 4;
            for (int b = 0; b < bytes && i < len; b++, i++)
                glyph += (char)raw[i];

            // Skip control characters
            if (!glyph.empty() && (unsigned char)glyph[0] >= 32)
                line.push_back({std::move(glyph), fg});
        }
        if (!line.empty()) result.push_back(std::move(line));
        return result;
    }

    // ── render_color: 256-colour ThumbData ───────────────────────────────────
    // Returns structured cell data for rendering via ncurses color pairs.
    // Called only when COLORS >= 256 && COLOR_PAIRS >= THUMB_COLOR_BASE + 256.
    static ThumbData render_color(const std::string& video_id, int cols, int rows) {
        if (!is_cached(video_id) || cols <= 0 || rows <= 0) return {};
        std::string path = thumb_path(video_id);
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
            "chafa -s %dx%d --animate=off --colors=256 --format=symbols '%s' 2>/dev/null",
            cols, rows, path.c_str());
        std::string raw;
        FILE* pipe = popen(cmd, "r");
        if (!pipe) return {};
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe)) raw += buf;
        pclose(pipe);
        return parse_ansi(raw);
    }

    // ── render: monochrome fallback (--colors=none) ───────────────────────────
    // Zero ANSI sequences — safe to feed directly into ncurses addstr().
    static std::string render(const std::string& video_id, int cols, int rows) {
        if (!is_cached(video_id) || cols <= 0 || rows <= 0) return "";
        std::string path = thumb_path(video_id);
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
            "chafa -s %dx%d --animate=off --colors=none '%s' 2>/dev/null",
            cols, rows, path.c_str());
        std::string result;
        FILE* pipe = popen(cmd, "r");
        if (!pipe) return "";
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe)) result += buf;
        pclose(pipe);
        return result;
    }

private:
    static void mkdir_p(const std::string& path) {
        std::string cmd = "mkdir -p '" + path + "'";
        int r = system(cmd.c_str()); (void)r;
    }
};

} // namespace ytui
