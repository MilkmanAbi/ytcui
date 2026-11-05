#pragma once
// Logging.
//
// Off by default and writing to a file when on, never to the terminal: a TUI
// owns the screen, and a stray fprintf in the middle of a frame corrupts the
// display in a way that looks like a rendering bug. v4 learned this the hard
// way, which is why its --debug writes to a file too.

#include <string>

namespace ytcui::log {

enum class Level { Error = 0, Warn, Info, Debug, Trace };

// Enable file logging. Path defaults to <cache_dir>/ytcui.log. Returns false
// when the file cannot be opened, in which case logging stays off rather than
// falling back to stderr.
bool init(Level level, const std::string& path = {});

void shutdown();

bool enabled();
Level level();
const std::string& path();

void write(Level l, const std::string& msg);

inline void error(const std::string& m) { write(Level::Error, m); }
inline void warn (const std::string& m) { write(Level::Warn,  m); }
inline void info (const std::string& m) { write(Level::Info,  m); }
inline void debug(const std::string& m) { write(Level::Debug, m); }
inline void trace(const std::string& m) { write(Level::Trace, m); }

const char* to_string(Level l);

}  // namespace ytcui::log
