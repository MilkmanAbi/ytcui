#pragma once
// Where ytcui keeps things.
//
// These paths are v4-compatible on purpose: v5 is a rewrite of the program,
// not of the user's data. Anyone upgrading keeps their config, playlists,
// history and saved videos without a migration step.
//
// One wrinkle is inherited rather than chosen. v4's config honoured
// $XDG_CONFIG_HOME, but its library code built the data path straight from
// $HOME, ignoring $XDG_DATA_HOME even though ois.conf claimed otherwise. So a
// user with a non-default $XDG_DATA_HOME has their real data in the legacy
// location. data_dir() prefers the correct XDG path for new writes, and
// data_file() falls back to the legacy one when that is where the file
// actually is -- so nobody's library silently goes missing.

#include <string>

namespace ytcui::paths {

// $XDG_CONFIG_HOME/ytcui, else ~/.config/ytcui
std::string config_dir();

// $XDG_CACHE_HOME/ytcui, else ~/.cache/ytcui
std::string cache_dir();

// $XDG_DATA_HOME/ytcui, else ~/.local/share/ytcui
std::string data_dir();

// What v4 actually used: ~/.local/share/ytcui, regardless of $XDG_DATA_HOME.
std::string legacy_data_dir();

// Resolve a data file for reading: the XDG path if it exists there, otherwise
// the legacy path if it exists there, otherwise the XDG path (for a fresh
// write). `name` is a bare filename such as "library.json".
std::string data_file(const std::string& name);

std::string config_file();          // config_dir() + "/config.json"

// mkdir -p. Returns false if the directory does not exist afterwards.
bool ensure_dir(const std::string& dir);

bool exists(const std::string& path);

}  // namespace ytcui::paths
