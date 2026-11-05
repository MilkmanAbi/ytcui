#pragma once

#include <string>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <sys/stat.h>

namespace ytui {

class Log {
public:
    // logdump_path: if non-empty, logdump writes to THIS path instead of
    // ~/.cache/ytcui/. Used so --logdump drops a timestamped file at ~/
    // which is easy to find and hand off for debugging.
    static void init(bool enabled, bool logdump = false,
                     const std::string& logdump_path = "") {
        enabled_ = enabled;
        logdump_ = logdump;
        if (!enabled_) return;

        // ── Main debug log (always ~/.cache/ytcui/debug.log) ──────────────
        std::string dir = get_log_dir();
        mkdir(dir.c_str(), 0755);
        std::string debug_path = dir + "/debug.log";
        file_ = fopen(debug_path.c_str(), "a");

        // ── Logdump file (timestamped, ~/ytcui-YYYYMMDD-HHMMSS.log) ───────
        // Separate file so it's trivial to find, share, or diff.
        if (logdump_) {
            std::string lp = logdump_path;
            if (lp.empty()) {
                lp = get_default_logdump_path();
            }
            dump_file_ = fopen(lp.c_str(), "w");
            logdump_path_ = lp;
        }

        if (file_) {
            write("=== ytcui started ===");
            if (logdump_ && dump_file_)
                write("Logdump → %s", logdump_path_.c_str());
        }
        if (dump_file_) {
            // Header so the file is self-explanatory when opened cold
            time_t now = time(nullptr);
            char tbuf[64];
            strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(dump_file_, "# ytcui logdump — %s\n", tbuf);
            fprintf(dump_file_, "# Contains: all debug events + mpv/yt-dlp stderr\n");
            fprintf(dump_file_, "# ────────────────────────────────────────────────\n\n");
            fflush(dump_file_);
        }
    }

    static void shutdown() {
        if (file_) {
            write("=== ytcui exiting ===");
            fclose(file_);
            file_ = nullptr;
        }
        if (dump_file_) {
            fprintf(dump_file_, "\n# === ytcui exiting ===\n");
            fclose(dump_file_);
            dump_file_ = nullptr;
        }
    }

    static void write(const char* fmt, ...) {
        if (!enabled_) return;

        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", t);

        // Write to debug.log
        if (file_) {
            fprintf(file_, "[%s] ", timebuf);
            va_list args;
            va_start(args, fmt);
            vfprintf(file_, fmt, args);
            va_end(args);
            fprintf(file_, "\n");
            fflush(file_);
        }

        // Mirror to logdump file
        if (dump_file_) {
            fprintf(dump_file_, "[%s] ", timebuf);
            va_list args2;
            va_start(args2, fmt);
            vfprintf(dump_file_, fmt, args2);
            va_end(args2);
            fprintf(dump_file_, "\n");
            fflush(dump_file_);
        }
    }

    // Raw output capture (mpv/yt-dlp stderr) — goes to logdump file only
    static void write_raw(const char* prefix, const std::string& data) {
        if (!enabled_ || !logdump_ || data.empty()) return;

        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", t);

        FILE* targets[] = { file_, dump_file_ };
        for (FILE* f : targets) {
            if (!f) continue;
            fprintf(f, "[%s] [%s]\n%s\n", timebuf, prefix, data.c_str());
            fflush(f);
        }
    }

    static bool is_enabled()  { return enabled_; }
    static bool is_logdump()  { return logdump_; }

    static std::string get_log_path()      { return get_log_dir() + "/debug.log"; }
    static std::string get_logdump_path()  { return logdump_path_; }

    static std::string get_log_dir() {
        const char* xdg = getenv("XDG_CACHE_HOME");
        if (xdg && xdg[0] != '\0') return std::string(xdg) + "/ytcui";
        const char* home = getenv("HOME");
        if (home) return std::string(home) + "/.cache/ytcui";
        return "/tmp/ytcui";
    }

    // ~/ytcui-YYYYMMDD-HHMMSS.log — right in home, easy to find
    static std::string get_default_logdump_path() {
        const char* home = getenv("HOME");
        std::string base = home ? std::string(home) : "/tmp";
        time_t now = time(nullptr);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%Y%m%d-%H%M%S", localtime(&now));
        return base + "/ytcui-" + tbuf + ".log";
    }

private:
    static inline bool        enabled_      = false;
    static inline bool        logdump_      = false;
    static inline FILE*       file_         = nullptr;
    static inline FILE*       dump_file_    = nullptr;
    static inline std::string logdump_path_ = "";
};

} // namespace ytui
