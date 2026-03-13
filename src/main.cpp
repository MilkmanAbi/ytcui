#include "app.h"
#include "log.h"
#include "player.h"
#include "youtube.h"
#include "types.h"
#include "theme.h"
#include "config.h"
#include "compat.h"
#include <cstdio>
#include <cstring>
#include <csignal>
#include <locale.h>
#include <unistd.h>
#include <sys/stat.h>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

static ytui::App* g_app = nullptr;
static std::atomic<bool> g_update_available{false};
static std::string g_remote_version;

// ─── ANSI helpers (only when stdout is a TTY) ─────────────────────────────────
struct Ansi {
    const char* BOLD;
    const char* CYAN;
    const char* GREEN;
    const char* YELLOW;
    const char* RED;
    const char* PURPLE;
    const char* DIM;
    const char* RESET;

    explicit Ansi(bool use_color) {
        if (use_color) {
            BOLD   = "\033[1m";
            CYAN   = "\033[36m";
            GREEN  = "\033[32m";
            YELLOW = "\033[33m";
            RED    = "\033[31m";
            PURPLE = "\033[35m";
            DIM    = "\033[2m";
            RESET  = "\033[0m";
        } else {
            BOLD = CYAN = GREEN = YELLOW = RED = PURPLE = DIM = RESET = "";
        }
    }
};

// ─── Background update check ──────────────────────────────────────────────────

static int compare_versions(const std::string& v1, const std::string& v2) {
    int maj1=0, min1=0, pat1=0, maj2=0, min2=0, pat2=0;
    sscanf(v1.c_str(), "%d.%d.%d", &maj1, &min1, &pat1);
    sscanf(v2.c_str(), "%d.%d.%d", &maj2, &min2, &pat2);
    if (maj1 != maj2) return (maj1 > maj2) ? 1 : -1;
    if (min1 != min2) return (min1 > min2) ? 1 : -1;
    if (pat1 != pat2) return (pat1 > pat2) ? 1 : -1;
    return 0;
}

static void check_for_updates_async() {
    FILE* pipe = popen("curl -fsSL --max-time 2 "
                       "https://raw.githubusercontent.com/MilkmanAbi/ytcui/main/VERSION "
                       "2>/dev/null | tr -d '[:space:]'", "r");
    if (!pipe) return;
    char buf[32] = {0};
    if (fgets(buf, sizeof(buf), pipe)) {
        std::string remote(buf);
        if (!remote.empty() && remote[0] >= '0' && remote[0] <= '9') {
            if (compare_versions(remote, ytui::VERSION) > 0) {
                g_remote_version = remote;
                g_update_available = true;
            }
        }
    }
    pclose(pipe);
}

// ─── Signal handler ───────────────────────────────────────────────────────────

static void signal_handler(int sig) {
    if (g_app) g_app->force_cleanup();
    fprintf(stderr, "\nCaught signal %d, cleaning up...\n", sig);
    exit(1);
}

// ─── --diag: deep system diagnostic dump ──────────────────────────────────────
//
// Prints everything we need to debug macOS/Linux/BSD compat issues.
// Does NOT start the TUI. Exits after printing.

static void run_diag(const Ansi& C) {
    printf("\n%s%s╭──────────────────────────────────────────────╮%s\n",
           C.BOLD, C.CYAN, C.RESET);
    printf("%s%s│  ytcui v%s — System Diagnostic              │%s\n",
           C.BOLD, C.CYAN, ytui::VERSION, C.RESET);
    printf("%s%s╰──────────────────────────────────────────────╯%s\n\n",
           C.BOLD, C.CYAN, C.RESET);

    // ── Platform / compat layer ───────────────────────────────────────────
    printf("%s%s[COMPAT LAYER]%s\n", C.BOLD, C.YELLOW, C.RESET);
    ytui::compat::dump_platform_info(STDOUT_FILENO);
    printf("\n");

    // ── macOS version (if applicable) ─────────────────────────────────────
#if defined(YTUI_MACOS)
    {
        int mv = ytui::compat::macos_major_version();
        printf("%s%s[macOS VERSION]%s\n", C.BOLD, C.YELLOW, C.RESET);
        printf("  kern.osproductversion major : %d\n", mv);
        if (mv < 11) {
            printf("  %s%sWARNING%s: macOS < 11 — EVFILT_PROC would fail (using pipe watchdog)\n",
                   C.BOLD, C.RED, C.RESET);
        } else {
            printf("  %sOK%s: macOS %d (pipe watchdog active; kqueue option exists but unused)\n",
                   C.GREEN, C.RESET, mv);
        }
        printf("\n");
    }
#endif

    // ── Binary dependencies ───────────────────────────────────────────────
    printf("%s%s[DEPENDENCIES]%s\n", C.BOLD, C.YELLOW, C.RESET);

    struct Dep { const char* name; const char* check_cmd; bool required; };
    Dep deps[] = {
        { "yt-dlp",   "which yt-dlp",       true  },
        { "mpv",      "which mpv",           true  },
        { "curl",     "which curl",          false },
        { "chafa",    "which chafa",         false },
#if defined(YTUI_MACOS)
        { "pbcopy",   "which pbcopy",        true  },
        { "open",     "which open",          true  },
#else
        { "xdg-open", "which xdg-open",     false },
        { "wl-copy",  "which wl-copy",      false },
        { "xclip",    "which xclip",        false },
#endif
    };

    for (auto& d : deps) {
        std::string cmd = std::string(d.check_cmd) + " > /dev/null 2>&1";
        bool found = (system(cmd.c_str()) == 0);
        if (found) {
            printf("  %s✓%s  %-12s", C.GREEN, C.RESET, d.name);
            // Print version if available
            std::string vcmd = std::string(d.check_cmd) +
                               " | xargs -I{} {} --version 2>/dev/null | head -1";
#if defined(YTUI_MACOS)
            if (strcmp(d.name, "yt-dlp") == 0)
                vcmd = "yt-dlp --version 2>/dev/null";
            else if (strcmp(d.name, "mpv") == 0)
                vcmd = "mpv --version 2>/dev/null | head -1";
#else
            if (strcmp(d.name, "yt-dlp") == 0)
                vcmd = "yt-dlp --version 2>/dev/null";
            else if (strcmp(d.name, "mpv") == 0)
                vcmd = "mpv --version 2>/dev/null | head -1";
#endif
            FILE* vp = popen(vcmd.c_str(), "r");
            char vbuf[128] = {0};
            if (vp) { char* _r = fgets(vbuf, sizeof(vbuf), vp); (void)_r; pclose(vp); }
            // Trim newline
            size_t vlen = strlen(vbuf);
            if (vlen > 0 && vbuf[vlen-1] == '\n') vbuf[vlen-1] = '\0';
            if (strlen(vbuf) > 0) printf(" %s%s%s", C.DIM, vbuf, C.RESET);
            printf("\n");
        } else {
            if (d.required)
                printf("  %s✗%s  %-12s %s[MISSING — REQUIRED]%s\n",
                       C.RED, C.RESET, d.name, C.RED, C.RESET);
            else
                printf("  %s-%s  %-12s %s[not found — optional]%s\n",
                       C.DIM, C.RESET, d.name, C.DIM, C.RESET);
        }
    }
    printf("\n");

    // ── Config / paths ────────────────────────────────────────────────────
    printf("%s%s[PATHS]%s\n", C.BOLD, C.YELLOW, C.RESET);
    std::string config_dir = ytui::Config::config_dir();
    std::string config_file = config_dir + "/config.json";
    std::string log_dir = ytui::Log::get_log_dir();
    std::string log_file = ytui::Log::get_log_path();

    auto path_status = [&](const char* label, const std::string& path, bool is_file) {
        struct stat st;
        bool exists = (stat(path.c_str(), &st) == 0);
        bool is_right_type = exists && (is_file ? S_ISREG(st.st_mode) : S_ISDIR(st.st_mode));
        printf("  %-20s %s%s%s%s\n",
               label,
               is_right_type ? C.GREEN : C.DIM,
               path.c_str(),
               exists ? (is_right_type ? " [exists]" : " [wrong type]") : " [not found]",
               C.RESET);
    };

    path_status("config dir",   config_dir,  false);
    path_status("config.json",  config_file, true);
    path_status("log dir",      log_dir,     false);
    path_status("debug.log",    log_file,    true);
    printf("\n");

    // ── Config dump (if exists) ────────────────────────────────────────────
    {
        std::ifstream cf(config_file);
        if (cf.is_open()) {
            printf("%s%s[CONFIG CONTENTS]%s\n", C.BOLD, C.YELLOW, C.RESET);
            std::string line;
            while (std::getline(cf, line))
                printf("  %s\n", line.c_str());
            printf("\n");
        }
    }

    // ── yt-dlp self-test ──────────────────────────────────────────────────
    printf("%s%s[YT-DLP SELF-TEST]%s\n", C.BOLD, C.YELLOW, C.RESET);
    printf("  Testing yt-dlp search (ytsearch1:test)...\n");
    fflush(stdout);
    {
        FILE* tp = popen("yt-dlp --flat-playlist -j --no-warnings "
                         "--ignore-errors 'ytsearch1:test' 2>&1 | head -3", "r");
        if (tp) {
            char tbuf[512];
            bool got_output = false;
            while (fgets(tbuf, sizeof(tbuf), tp)) {
                printf("  %s%s%s", C.DIM, tbuf, C.RESET);
                got_output = true;
            }
            pclose(tp);
            if (!got_output)
                printf("  %s[no output — yt-dlp may be broken or rate-limited]%s\n",
                       C.RED, C.RESET);
        }
    }
    printf("\n");

    // ── mpv self-test ─────────────────────────────────────────────────────
    printf("%s%s[MPV SELF-TEST]%s\n", C.BOLD, C.YELLOW, C.RESET);
    printf("  Testing mpv --version...\n");
    {
        FILE* mp = popen("mpv --version 2>&1 | head -2", "r");
        if (mp) {
            char mbuf[256];
            while (fgets(mbuf, sizeof(mbuf), mp))
                printf("  %s%s%s", C.DIM, mbuf, C.RESET);
            pclose(mp);
        }
    }
    printf("\n");

    // ── Clipboard test ────────────────────────────────────────────────────
    printf("%s%s[CLIPBOARD]%s\n", C.BOLD, C.YELLOW, C.RESET);
#if defined(YTUI_MACOS)
    {
        // pbcopy ships with macOS — if it's missing something is very wrong
        bool found = (access("/usr/bin/pbcopy", X_OK) == 0);
        printf("  pbcopy   : %s%s%s  %s(built into macOS)%s\n",
               found ? C.GREEN : C.RED,
               found ? "OK" : "NOT FOUND — this should never happen",
               C.RESET, C.DIM, C.RESET);
    }
#else
    {
        bool wl = (system("which wl-copy > /dev/null 2>&1") == 0);
        bool xc = (system("which xclip  > /dev/null 2>&1") == 0);
        bool xs = (system("which xsel   > /dev/null 2>&1") == 0);
        bool any = wl || xc || xs;

        printf("  wl-copy  : %s%s%s\n",
               wl ? C.GREEN : C.DIM, wl ? "found" : "not found", C.RESET);
        printf("  xclip    : %s%s%s\n",
               xc ? C.GREEN : C.DIM, xc ? "found" : "not found", C.RESET);
        printf("  xsel     : %s%s%s\n",
               xs ? C.GREEN : C.DIM, xs ? "found" : "not found", C.RESET);

        if (any) {
            const char* which = wl ? "wl-copy" : (xc ? "xclip" : "xsel");
            printf("  %sURL copy will use: %s%s\n", C.GREEN, which, C.RESET);
        } else {
            printf("  %sWARNING: no clipboard tool found — URL copy will not work%s\n",
                   C.RED, C.RESET);
            // Detect session type to give the right install command
            bool wayland = (getenv("WAYLAND_DISPLAY") != nullptr)
                        || (getenv("XDG_SESSION_TYPE") != nullptr &&
                            std::string(getenv("XDG_SESSION_TYPE")) == "wayland");
            if (wayland) {
                printf("  Install fix (Wayland): %ssudo apt install wl-clipboard%s\n",
                       C.YELLOW, C.RESET);
                printf("                         %ssudo pacman -S wl-clipboard%s\n",
                       C.YELLOW, C.RESET);
                printf("                         %ssudo dnf install wl-clipboard%s\n",
                       C.YELLOW, C.RESET);
            } else {
                printf("  Install fix (X11):     %ssudo apt install xclip%s\n",
                       C.YELLOW, C.RESET);
                printf("                         %ssudo pacman -S xclip%s\n",
                       C.YELLOW, C.RESET);
                printf("                         %ssudo dnf install xclip%s\n",
                       C.YELLOW, C.RESET);
            }
        }
    }
#endif
    printf("\n");

    // ── Process/pipe compat test ──────────────────────────────────────────
    printf("%s%s[PROCESS COMPAT TEST]%s\n", C.BOLD, C.YELLOW, C.RESET);
    {
        int pfd[2];
        int rc = ytui::compat::pipe_cloexec(pfd);
        if (rc == 0) {
            printf("  %spipe_cloexec()%s     : %sOK%s\n", C.DIM, C.RESET, C.GREEN, C.RESET);
            close(pfd[0]); close(pfd[1]);
        } else {
            printf("  pipe_cloexec()     : %sFAILED (%s)%s\n", C.RED, strerror(errno), C.RESET);
        }

        bool native = ytui::compat::has_native_pdeathsig();
        printf("  has_native_pdeathsig: %s%s%s\n",
               native ? C.GREEN : C.DIM,
               native ? "yes (kernel-managed)" : "no (pipe watchdog used)",
               C.RESET);

#if defined(YTUI_MACOS)
        // Test a dummy fork+watchdog to verify it doesn't crash
        int test_pipe[2];
        if (ytui::compat::pipe_cloexec(test_pipe) == 0) {
            pid_t tp = fork();
            if (tp == 0) {
                close(test_pipe[1]);
                // Watchdog grandchild would block here; just exit for the test
                char b; read(test_pipe[0], &b, 1);
                close(test_pipe[0]);
                _exit(0);
            } else if (tp > 0) {
                close(test_pipe[0]);
                usleep(5000);
                close(test_pipe[1]); // Triggers EOF → child exits
                waitpid(tp, nullptr, 0);
                printf("  pipe watchdog fork : %sOK (fork+pipe+EOF test passed)%s\n",
                       C.GREEN, C.RESET);
            } else {
                printf("  pipe watchdog fork : %sFAILED (fork error: %s)%s\n",
                       C.RED, strerror(errno), C.RESET);
            }
        }
#endif
    }
    printf("\n");

    // ── Recent debug log tail ─────────────────────────────────────────────
    {
        std::ifstream lf(log_file);
        if (lf.is_open()) {
            printf("%s%s[LAST 20 LOG LINES]%s  (%s)\n",
                   C.BOLD, C.YELLOW, C.RESET, log_file.c_str());
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(lf, line)) {
                lines.push_back(line);
                if (lines.size() > 20) lines.erase(lines.begin());
            }
            for (auto& l : lines)
                printf("  %s%s%s\n", C.DIM, l.c_str(), C.RESET);
            printf("\n");
        }
    }

    printf("%s%s══ Diagnostic complete ══%s\n\n", C.BOLD, C.CYAN, C.RESET);
}

// ─── --injectconfig: write a key=value into config.json ───────────────────────
// Usage: ytcui --injectconfig key=value [key=value ...]
// Supports all Config fields: max_results, theme, grayscale, sort_by,
//   filter_type, filter_dur, ytdlp_path, mpv_path

static int run_injectconfig(const std::vector<std::string>& pairs, const Ansi& C) {
    ytui::Config cfg;
    cfg.load();

    bool any_set = false;

    for (const auto& pair : pairs) {
        size_t eq = pair.find('=');
        if (eq == std::string::npos) {
            fprintf(stderr, "%sError%s: invalid format '%s' — expected key=value\n",
                    C.RED, C.RESET, pair.c_str());
            continue;
        }
        std::string key = pair.substr(0, eq);
        std::string val = pair.substr(eq + 1);

        if (key == "max_results") {
            cfg.max_results = atoi(val.c_str());
            printf("  %sset%s max_results = %d\n", C.GREEN, C.RESET, cfg.max_results);
            any_set = true;
        } else if (key == "theme") {
            cfg.theme_name = val;
            printf("  %sset%s theme = %s\n", C.GREEN, C.RESET, cfg.theme_name.c_str());
            any_set = true;
        } else if (key == "grayscale") {
            cfg.grayscale = (val == "true" || val == "1" || val == "yes");
            printf("  %sset%s grayscale = %s\n", C.GREEN, C.RESET, cfg.grayscale ? "true" : "false");
            any_set = true;
        } else if (key == "sort_by") {
            cfg.sort_by = val;
            printf("  %sset%s sort_by = %s\n", C.GREEN, C.RESET, cfg.sort_by.c_str());
            any_set = true;
        } else if (key == "filter_type") {
            cfg.filter_type = val;
            printf("  %sset%s filter_type = %s\n", C.GREEN, C.RESET, cfg.filter_type.c_str());
            any_set = true;
        } else if (key == "filter_dur") {
            cfg.filter_dur = val;
            printf("  %sset%s filter_dur = %s\n", C.GREEN, C.RESET, cfg.filter_dur.c_str());
            any_set = true;
        } else if (key == "ytdlp_path") {
            cfg.ytdlp_path = val;
            printf("  %sset%s ytdlp_path = %s\n", C.GREEN, C.RESET, cfg.ytdlp_path.c_str());
            any_set = true;
        } else if (key == "mpv_path") {
            cfg.mpv_path = val;
            printf("  %sset%s mpv_path = %s\n", C.GREEN, C.RESET, cfg.mpv_path.c_str());
            any_set = true;
        } else {
            fprintf(stderr, "  %sUnknown key%s: '%s'\n  Valid keys: max_results, theme, "
                    "grayscale, sort_by, filter_type, filter_dur, ytdlp_path, mpv_path\n",
                    C.RED, C.RESET, key.c_str());
        }
    }

    if (any_set) {
        cfg.save();
        printf("  %sSaved%s → %s/config.json\n",
               C.GREEN, C.RESET, ytui::Config::config_dir().c_str());
    }
    return any_set ? 0 : 1;
}

// ─── Help ─────────────────────────────────────────────────────────────────────

static void print_version() {
    printf("ytcui %s\n", ytui::VERSION);
}

static void print_help(const Ansi&) {
    printf(
        "ytcui %s - YouTube terminal UI\n"
        "\n"
        "USAGE\n"
        "    ytcui [options]\n"
        "\n"
        "GENERAL\n"
        "    -h, --help                  show this help\n"
        "    -v, --version               show version\n"
        "    -t, --theme <name>          set color theme\n"
        "    -g, --grayscale             grayscale theme (legacy)\n"
        "\n"
        "DEBUG\n"
        "    -d, --debug                 write debug log to ~/.cache/ytcui/debug.log\n"
        "    --logdump                   write full timestamped log to ~/ytcui-DATE.log\n"
        "                                captures all events and mpv/yt-dlp stderr\n"
        "                                implies --debug, no separate flag needed\n"
        "    --diag                      print full system diagnostic and exit\n"
        "    --injectconfig key=value    write config values without opening the TUI\n"
        "                                keys: max_results, theme, grayscale, sort_by,\n"
        "                                      filter_type, filter_dur, ytdlp_path, mpv_path\n"
        "\n"
        "PLAYBACK  (session only, nothing is saved to config)\n"
        "    --no-ha                     disable mpv hardware acceleration\n"
        "    --no-cache                  disable mpv demuxer cache\n"
        "    --mpv-verbose               do not silence mpv terminal output\n"
        "    --volume <0-130>            set volume for this session (default: 80)\n"
        "\n"
        "OTHER\n"
        "    --no-update-check           skip the startup version check\n"
        "    --upgrade                   upgrade ytcui to the latest version\n"
        "\n"
        "THEMES\n"
        "    default, grayscale, nord, dracula, solarized, monokai, gruvbox, tokyo\n"
        "\n"
        "KEYS\n"
        "    Tab       cycle focus between panels\n"
        "    j / k     move down / up\n"
        "    h / l     move left / right\n"
        "    Enter     search or select\n"
        "    p         pause / resume playback\n"
        "    s         sort and filter menu\n"
        "    /         jump to search bar\n"
        "    Esc       go back\n"
        "    q         quit\n"
        "\n"
        "FILES\n"
        "    config    ~/.config/ytcui/config.json\n"
        "    library   ~/.local/share/ytcui/\n"
        "    cache     ~/.cache/ytcui/\n"
        "    log       ~/.cache/ytcui/debug.log\n"
        "    logdump   ~/ytcui-YYYYMMDD-HHMMSS.log\n"
        "\n"
        "EXAMPLES\n"
        "    ytcui --diag\n"
        "    ytcui --logdump\n"
        "    ytcui --no-ha --no-cache\n"
        "    ytcui --theme dracula\n"
        "    ytcui --volume 50\n"
        "    ytcui --injectconfig max_results=25 theme=nord\n"
        "\n",
        ytui::VERSION
    );
}


// ═══════════════════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    setlocale(LC_ALL, "");

    Ansi C(isatty(STDOUT_FILENO) != 0);

    // ── Pre-scan for --upgrade (must run before anything else) ────────────
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--upgrade") == 0) {
            const char* update_script = "/usr/local/share/ytcui/update.sh";
            if (access(update_script, X_OK) == 0) {
                printf("Launching updater...\n\n");
                execl("/bin/bash", "bash", update_script, nullptr);
                perror("Failed to launch updater");
                return 1;
            } else {
                fprintf(stderr, "Error: Update script not found at %s\n", update_script);
                fprintf(stderr, "Please reinstall ytcui from the repository.\n");
                return 1;
            }
        }
    }

    // ── Pre-scan for --no-update-check ────────────────────────────────────
    bool skip_update_check = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-update-check") == 0) {
            skip_update_check = true;
            break;
        }
    }

    // ── Collect all argv into a vector for easier parsing ─────────────────
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);

    // ── Early-exit commands: --help, --version ─────────────────────────────
    for (auto& a : args) {
        if (a == "--help" || a == "-h") { print_help(C); return 0; }
        if (a == "--version" || a == "-v") { print_version(); return 0; }
    }

    // ── --diag: full system diagnostic (early exit, no TUI) ───────────────
    for (auto& a : args) {
        if (a == "--diag") {
            run_diag(C);
            return 0;
        }
    }

    // ── --injectconfig: write config keys (early exit, no TUI) ───────────
    {
        bool inject_mode = false;
        std::vector<std::string> inject_pairs;
        for (int i = 0; i < (int)args.size(); i++) {
            if (args[i] == "--injectconfig") {
                inject_mode = true;
            } else if (inject_mode) {
                // Collect all subsequent key=value pairs (stop at next -- flag)
                if (args[i][0] == '-' && args[i].size() > 1 && args[i][1] == '-') break;
                inject_pairs.push_back(args[i]);
            }
        }
        if (inject_mode) {
            if (inject_pairs.empty()) {
                fprintf(stderr, "Usage: ytcui --injectconfig key=value [key=value ...]\n");
                fprintf(stderr, "Keys: max_results, theme, grayscale, sort_by, "
                                "filter_type, filter_dur, ytdlp_path, mpv_path\n");
                return 1;
            }
            return run_injectconfig(inject_pairs, C);
        }
    }

    // ── Parse runtime flags ────────────────────────────────────────────────
    ytui::Theme theme = ytui::Theme::Default;
    bool debug        = false;
    bool logdump      = false;
    ytui::PlayerOptions player_opts;  // defaults: vol=80, hwdec=auto, cache=on

    for (int i = 0; i < (int)args.size(); i++) {
        const std::string& a = args[i];

        if ((a == "--theme" || a == "-t") && i + 1 < (int)args.size()) {
            theme = ytui::string_to_theme(args[++i]);
        } else if (a == "--grayscale" || a == "-g") {
            theme = ytui::Theme::Grayscale;
        } else if (a == "--debug" || a == "-d") {
            debug = true;
        } else if (a == "--logdump") {
            logdump = true;
            debug = true;   // logdump implies debug — no separate flag needed
        } else if (a == "--no-ha" || a == "--no-hardware-acceleration") {
            player_opts.no_hardware_accel = true;
        } else if (a == "--no-cache") {
            player_opts.no_cache = true;
        } else if (a == "--mpv-verbose") {
            player_opts.verbose_mpv = true;
        } else if (a == "--volume" && i + 1 < (int)args.size()) {
            int v = atoi(args[++i].c_str());
            if (v < 0 || v > 130) {
                fprintf(stderr, "Warning: --volume %d out of range (0-130), clamping\n", v);
                v = (v < 0) ? 0 : 130;
            }
            player_opts.volume = v;
        } else if (a == "--no-update-check" || a == "--upgrade") {
            // already handled above
        } else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", a.c_str());
            fprintf(stderr, "Try 'ytcui --help' for more information.\n");
            return 1;
        }
    }

    if (debug) ytui::Log::init(true, logdump);
    ytui::Log::write("ytcui %s starting (theme=%s debug=%s logdump=%s vol=%d no_ha=%s no_cache=%s)",
                     ytui::VERSION,
                     ytui::theme_to_string(theme).c_str(),
                     debug   ? "yes" : "no",
                     logdump ? "yes" : "no",
                     player_opts.volume,
                     player_opts.no_hardware_accel ? "yes" : "no",
                     player_opts.no_cache          ? "yes" : "no");

    if (debug) {
        ytui::Log::write("--- compat platform dump ---");
#if defined(YTUI_MACOS)
        ytui::Log::write("macOS major version: %d", ytui::compat::macos_major_version());
#endif
        ytui::Log::write("has_native_pdeathsig: %s",
                         ytui::compat::has_native_pdeathsig() ? "yes" : "no");
    }

    if (logdump) {
        // Tell the user where the dump file landed (printed before TUI starts)
        fprintf(stderr, "Logdump → %s\n", ytui::Log::get_logdump_path().c_str());
    }

    // ── Dependency checks ─────────────────────────────────────────────────
    if (!ytui::YouTube::is_available()) {
        fprintf(stderr, "Error: yt-dlp not found. Install with: pip install yt-dlp\n");
        fprintf(stderr, "Run 'ytcui --diag' for a full system diagnostic.\n");
        return 1;
    }
    if (!ytui::Player::is_available()) {
        fprintf(stderr, "Error: mpv not found. Install with: brew install mpv / apt install mpv\n");
        fprintf(stderr, "Run 'ytcui --diag' for a full system diagnostic.\n");
        return 1;
    }

    // ── Background update check ───────────────────────────────────────────
    if (!skip_update_check) {
        std::thread update_thread(check_for_updates_async);
        update_thread.detach();
    }

    usleep(100000);
    if (g_update_available) {
        fprintf(stderr, "\033[33m⬆ Update available: %s → %s (run: ytcui --upgrade)\033[0m\n\n",
                ytui::VERSION, g_remote_version.c_str());
        usleep(1500000);
    }

    // ── Launch app ────────────────────────────────────────────────────────
    ytui::App app(theme);
    app.set_player_options(player_opts);
    g_app = &app;

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    int ret = app.run();
    g_app = nullptr;

    ytui::Log::shutdown();
    return ret;
}
