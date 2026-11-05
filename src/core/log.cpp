#include "core/log.h"

#include "core/paths.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace ytcui::log {
namespace {

struct State {
    std::FILE*  fp = nullptr;
    Level       lvl = Level::Error;
    bool        on = false;
    std::string file;
    std::mutex  mu;
};

State& state() {
    static State s;
    return s;
}

std::string stamp() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto secs = clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec, (int)ms);
    return buf;
}

}  // namespace

const char* to_string(Level l) {
    switch (l) {
        case Level::Error: return "ERROR";
        case Level::Warn:  return "WARN ";
        case Level::Info:  return "INFO ";
        case Level::Debug: return "DEBUG";
        case Level::Trace: return "TRACE";
    }
    return "?????";
}

bool init(Level level, const std::string& path) {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.fp) { std::fclose(s.fp); s.fp = nullptr; }

    s.file = path.empty() ? paths::cache_dir() + "/ytcui.log" : path;
    const auto slash = s.file.rfind('/');
    if (slash != std::string::npos) paths::ensure_dir(s.file.substr(0, slash));

    s.fp = std::fopen(s.file.c_str(), "a");
    if (!s.fp) { s.on = false; return false; }
    s.lvl = level;
    s.on = true;
    std::fprintf(s.fp, "\n=== ytcui %s starting (level %s) ===\n",
                 YTCUI_VERSION, to_string(level));
    std::fflush(s.fp);
    return true;
}

void shutdown() {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.fp) { std::fflush(s.fp); std::fclose(s.fp); s.fp = nullptr; }
    s.on = false;
}

bool enabled() { return state().on; }
Level level()  { return state().lvl; }
const std::string& path() { return state().file; }

void write(Level l, const std::string& msg) {
    State& s = state();
    if (!s.on || (int)l > (int)s.lvl) return;
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.fp) return;
    std::fprintf(s.fp, "%s %s %s\n", stamp().c_str(), to_string(l), msg.c_str());
    // Flushed every line on purpose: the interesting log is the one written
    // immediately before a crash or a hang, and a buffered tail is no log.
    std::fflush(s.fp);
}

}  // namespace ytcui::log
