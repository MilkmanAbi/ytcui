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
#include <cctype>
#include "termcaps.h"
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>

#ifdef USE_LIBSIXEL
#include <sixel.h>
#endif

namespace ytui {

// Color pairs 1-16 are reserved for UI elements (Color::BG … Color::DESC).
// Pairs 17-272 are reserved for thumbnail 256-color rendering.
// THUMB_COLOR_BASE + N maps color index N (0-255) → pair (17+N).
constexpr int THUMB_COLOR_BASE = 18;

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
        // In non-UTF-8 locales the Unicode block glyphs chafa emits by default
        // arrive as multibyte garbage (seen on bobcat / Latin-1 mlterm), so
        // restrict chafa to plain ASCII symbols there.
        const char* sym = TermCaps::get().unicode ? "" : "--symbols ascii ";
        snprintf(cmd, sizeof(cmd),
            "chafa -s %dx%d --animate=off --colors=256 --format=symbols %s'%s' 2>/dev/null",
            cols, rows, sym, path.c_str());
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
        const char* sym = TermCaps::get().unicode ? "" : "--symbols ascii ";
        snprintf(cmd, sizeof(cmd),
            "chafa -s %dx%d --animate=off --colors=none %s'%s' 2>/dev/null",
            cols, rows, sym, path.c_str());
        std::string result;
        FILE* pipe = popen(cmd, "r");
        if (!pipe) return "";
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe)) result += buf;
        pclose(pipe);
        return result;
    }

    // ── Optional pixel-perfect graphics (Sixel / Kitty / iTerm2) ───────────────
    // These protocols draw a *real raster image* instead of Unicode block art.
    // They cannot pass through ncurses (ncurses owns the cell grid and would
    // corrupt or strip the escape sequence), so callers must:
    //   1. let ncurses lay out the frame and refresh(),
    //   2. then write the graphics escape directly to the tty at the thumbnail
    //      origin via raw cursor positioning (see TUI::flush_graphics_thumb),
    //   3. mark the covered cells dirty so the next ncurses frame repaints them.
    // This is opt-in: block art remains the universal default. The extra cost
    // (encoding + a chafa subprocess per visible thumbnail) is only paid when a
    // graphics mode is active, addressing the "adds runtime overhead" concern.
    enum class Gfx { None, Blocks, Sixel, Kitty, Iterm };

    // Detect the best graphics protocol the *current* terminal supports, and
    // explain the decision (for --diag). Heuristic and env-based (cheap, no
    // terminal round-trip); the caller may still confirm sixel via a DA query.
    // Never returns a protocol that would dump garbage on an incapable terminal
    // (Apple Terminal, Alacritty, plain VTE), and refuses raster protocols
    // inside tmux/screen by default because multiplexers mangle the escapes.
    struct GfxDetect { Gfx mode; std::string reason; };
    static GfxDetect detect_gfx_ex() {
        auto env = [](const char* k) -> std::string {
            const char* v = getenv(k); return v ? std::string(v) : std::string();
        };
        std::string term      = env("TERM");
        std::string term_prog = env("TERM_PROGRAM");
        std::string lc_term   = term;
        for (auto& c : lc_term) c = (char)tolower((unsigned char)c);

        // Inside a multiplexer the inner $TERM is screen*/tmux* and raster
        // escapes are rewritten/stripped unless explicitly configured. Be safe:
        // fall back to block art. (Users can still force --gfx sixel if their
        // tmux has `allow-passthrough on`.)
        if (!env("TMUX").empty()
            || lc_term.compare(0, 6, "screen") == 0
            || lc_term.compare(0, 4, "tmux") == 0)
            return { Gfx::Blocks, "multiplexer (tmux/screen): raster unsafe, using blocks" };

        // Kitty graphics protocol (highest quality): kitty, Ghostty, WezTerm,
        // Konsole all implement it.
        if (!env("KITTY_WINDOW_ID").empty() || lc_term.find("kitty") != std::string::npos)
            return { Gfx::Kitty, "kitty graphics ($KITTY_WINDOW_ID/$TERM)" };
        if (!env("GHOSTTY_RESOURCES_DIR").empty() || term_prog == "ghostty")
            return { Gfx::Kitty, "kitty graphics (Ghostty)" };
        if (!env("KONSOLE_VERSION").empty())
            return { Gfx::Kitty, "kitty graphics (Konsole >= 22.04)" };
        if (!env("WEZTERM_EXECUTABLE").empty() || term_prog == "WezTerm")
            return { Gfx::Kitty, "kitty graphics (WezTerm)" };

        // iTerm2 inline images (3.x+).
        if (term_prog == "iTerm.app")
            return { Gfx::Iterm, "iTerm2 inline images ($TERM_PROGRAM)" };

        // Apple Terminal: 256 colours only, no sixel/kitty ever -> block art.
        if (term_prog == "Apple_Terminal")
            return { Gfx::Blocks, "Apple Terminal has no raster protocol, using blocks" };

        // Sixel: xterm (-ti vt340), mlterm, foot, yaft, st+sixel, contour,
        // Windows Terminal (1.22+), recent VS Code.
        if (lc_term.find("mlterm")  != std::string::npos) return { Gfx::Sixel, "sixel (mlterm)" };
        if (lc_term.find("foot")    != std::string::npos) return { Gfx::Sixel, "sixel (foot)" };
        if (lc_term.find("yaft")    != std::string::npos) return { Gfx::Sixel, "sixel (yaft)" };
        if (lc_term.find("contour") != std::string::npos) return { Gfx::Sixel, "sixel (contour)" };
        if (lc_term.find("st-")     == 0 || lc_term == "st")
            return { Gfx::Sixel, "sixel (st, assuming sixel patch)" };
        if (term_prog == "vscode") return { Gfx::Sixel, "sixel (VS Code terminal)" };
        if (!env("WT_SESSION").empty()) return { Gfx::Sixel, "sixel (Windows Terminal)" };

        // xterm advertises sixel only when built --enable-sixel; env can't tell,
        // so leave it to the caller's runtime DA query (query_sixel_da()).
        if (lc_term.find("xterm") != std::string::npos)
            return { Gfx::Blocks, "xterm: probe sixel via DA query, else blocks" };

        // Apple Terminal, Alacritty, plain gnome-terminal/VTE: block art only.
        return { Gfx::Blocks, "no raster protocol detected, using blocks" };
    }

    static Gfx detect_gfx() { return detect_gfx_ex().mode; }

    // Terminal cell size in pixels, learned once via the CSI 16 t query. Needed
    // to size raster images correctly: through a pipe chafa/libsixel cannot
    // probe this themselves and otherwise guess (which is what made sixel
    // thumbnails overflow their panel). 0 means unknown -> fall back to a guess.
    static int& cell_px_w() { static int v = 0; return v; }
    static int& cell_px_h() { static int v = 0; return v; }

    // Ask the terminal for its cell size in pixels: CSI 16 t -> CSI 6 ; H ; W t.
    // Call once at startup BEFORE ncurses init (touches the raw tty). Safe to
    // call when not a tty (it just returns false). Stores into cell_px_w/h.
    static bool probe_cell_px(int timeout_ms = 250) {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;
        struct termios old{}; if (tcgetattr(STDIN_FILENO, &old) != 0) return false;
        struct termios raw = old;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);

        const char* q = "\033[16t";
        ssize_t w = write(STDOUT_FILENO, q, 5); (void)w;

        std::string resp; char c;
        struct timeval start{}, now{}; gettimeofday(&start, nullptr);
        for (;;) {
            fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO, &rf);
            struct timeval tv{0, 20000};
            if (select(STDIN_FILENO + 1, &rf, nullptr, nullptr, &tv) > 0) {
                if (read(STDIN_FILENO, &c, 1) == 1) { resp += c; if (c == 't') break; }
            }
            gettimeofday(&now, nullptr);
            long ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
            if (ms > timeout_ms) break;
        }
        tcsetattr(STDIN_FILENO, TCSANOW, &old);

        // Parse "\033[6;H;Wt"
        size_t p = resp.find("[6;");
        if (p == std::string::npos) return false;
        int h = 0, wpx = 0;
        if (sscanf(resp.c_str() + p, "[6;%d;%dt", &h, &wpx) == 2 && wpx > 0 && h > 0) {
            cell_px_w() = wpx; cell_px_h() = h;
            return true;
        }
        return false;
    }

    // Parse "auto|blocks|sixel|kitty|iterm|off" (from config/CLI) into a Gfx,
    // resolving "auto" against the detected terminal.
    static Gfx parse_gfx_mode(const std::string& m) {
        if (m == "off")    return Gfx::None;
        if (m == "blocks") return Gfx::Blocks;
        if (m == "sixel")  return Gfx::Sixel;
        if (m == "kitty")  return Gfx::Kitty;
        if (m == "iterm")  return Gfx::Iterm;
        return detect_gfx(); // "auto" or unknown
    }

    // One-shot runtime probe: ask the terminal for its Device Attributes and
    // check whether it reports sixel (attribute "4"). Use this to confirm
    // ambiguous terminals (xterm) before sending sixel. Returns false on any
    // timeout/parse failure so we never blast sixel at a terminal that can't
    // show it. Safe to call once at startup, BEFORE ncurses init.
    static bool query_sixel_da(int timeout_ms = 250) {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;
        struct termios old{}; if (tcgetattr(STDIN_FILENO, &old) != 0) return false;
        struct termios raw = old;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);

        const char* da = "\033[c";              // Primary Device Attributes
        ssize_t w = write(STDOUT_FILENO, da, 3); (void)w;

        std::string resp; char c; bool found = false;
        struct timeval start{}, now{}; gettimeofday(&start, nullptr);
        for (;;) {
            fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO, &rf);
            struct timeval tv{0, 20000};        // 20ms slices
            if (select(STDIN_FILENO + 1, &rf, nullptr, nullptr, &tv) > 0) {
                if (read(STDIN_FILENO, &c, 1) == 1) {
                    resp += c;
                    if (c == 'c') break;        // DA response terminator
                }
            }
            gettimeofday(&now, nullptr);
            long ms = (now.tv_sec - start.tv_sec) * 1000
                    + (now.tv_usec - start.tv_usec) / 1000;
            if (ms > timeout_ms) break;
        }
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        // Response is like ESC[?62;4;6;9;...c  — ";4" / "?...;4;" means sixel.
        if (resp.find(";4;") != std::string::npos ||
            resp.find(";4c") != std::string::npos ||
            resp.find("?4;") != std::string::npos) found = true;
        return found;
    }

    // Produce the raw graphics-protocol bytes for a cached thumbnail sized to
    // cols×rows *character cells*. Returns "" if unavailable. By default this
    // shells out to chafa (already a required dependency) with the right
    // --format; if built with -DUSE_LIBSIXEL it encodes sixel in-process via
    // libsixel for the sixel path (one fewer fork). Block art is handled by
    // render_color()/render(); this is only the raster protocols.
    static std::string render_graphics(const std::string& video_id, Gfx mode,
                                        int cols, int rows) {
        if (mode == Gfx::None || mode == Gfx::Blocks) return "";
        if (!is_cached(video_id) || cols <= 0 || rows <= 0) return "";
        std::string path = thumb_path(video_id);

#ifdef USE_LIBSIXEL
        if (mode == Gfx::Sixel) {
            std::string s = sixel_encode_file(path, cols, rows);
            if (!s.empty()) return s;
            // fall through to chafa if libsixel failed
        }
#endif
        const char* fmt = "symbols";
        switch (mode) {
            case Gfx::Sixel: fmt = "sixel";  break;
            case Gfx::Kitty: fmt = "kitty";  break;
            case Gfx::Iterm: fmt = "iterm";  break;
            default:         return "";
        }
        // If we learned the real cell size, pass it as --font-ratio so chafa's
        // pixel sizing matches the terminal instead of its piped-mode guess.
        char ratio[48] = "";
        if (cell_px_w() > 0 && cell_px_h() > 0)
            snprintf(ratio, sizeof(ratio), "--font-ratio=%d/%d ", cell_px_w(), cell_px_h());
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
            "chafa -s %dx%d --animate=off %s--format=%s '%s' 2>/dev/null",
            cols, rows, ratio, fmt, path.c_str());
        std::string out;
        FILE* pipe = popen(cmd, "r");
        if (!pipe) return "";
        char buf[8192];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), pipe)) > 0) out.append(buf, got);
        pclose(pipe);
        return out;
    }

    static const char* gfx_name(Gfx g) {
        switch (g) {
            case Gfx::None:   return "off";
            case Gfx::Blocks: return "blocks";
            case Gfx::Sixel:  return "sixel";
            case Gfx::Kitty:  return "kitty";
            case Gfx::Iterm:  return "iterm";
        }
        return "blocks";
    }

private:
#ifdef USE_LIBSIXEL
    // Encode a cached image file to a SIXEL byte-string using libsixel's
    // high-level encoder (the same path img2sixel uses). cols/rows are cell
    // dimensions; we let libsixel pick pixel size from the source and rely on
    // the terminal to scale, or pass a width hint. One fewer fork than chafa.
    static std::string sixel_encode_file(const std::string& path, int cols, int rows) {
        (void)rows;
        sixel_encoder_t* enc = nullptr;
        if (SIXEL_FAILED(sixel_encoder_new(&enc, nullptr))) return "";
        std::string out;
        // Capture output into our string via a temp file (libsixel writes to a
        // file descriptor; a pipe-backed FILE keeps this simple and portable).
        char tmpl[] = "/tmp/ytcui_sixel_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0) { sixel_encoder_unref(enc); return ""; }
        // Pixel-exact sizing from the probed cell size (the accurate path).
        // Falls back to a conservative guess if the probe failed.
        int cw = cell_px_w() > 0 ? cell_px_w() : 8;
        int ch = cell_px_h() > 0 ? cell_px_h() : 16;
        char wbuf[16]; snprintf(wbuf, sizeof(wbuf), "%d", cols * cw);
        char hbuf[16]; snprintf(hbuf, sizeof(hbuf), "%d", rows * ch);
        sixel_encoder_setopt(enc, SIXEL_OPTFLAG_WIDTH,  wbuf);
        sixel_encoder_setopt(enc, SIXEL_OPTFLAG_HEIGHT, hbuf);
        sixel_encoder_setopt(enc, SIXEL_OPTFLAG_OUTFILE, tmpl);
        SIXELSTATUS st = sixel_encoder_encode(enc, path.c_str());
        sixel_encoder_unref(enc);
        if (!SIXEL_FAILED(st)) {
            lseek(fd, 0, SEEK_SET);
            char buf[8192]; ssize_t n;
            while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
        }
        ::close(fd); ::unlink(tmpl);
        return out;
    }
#endif

    static void mkdir_p(const std::string& path) {
        std::string cmd = "mkdir -p '" + path + "'";
        int r = system(cmd.c_str()); (void)r;
    }
};

} // namespace ytui
