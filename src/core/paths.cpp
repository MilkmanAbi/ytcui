#include "core/paths.h"

#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>

namespace ytcui::paths {
namespace {

std::string home() {
    const char* h = std::getenv("HOME");
    // "." rather than "/" so a HOME-less environment writes somewhere harmless
    // instead of attempting the filesystem root.
    return (h && *h) ? std::string(h) : std::string(".");
}

}  // namespace

std::string config_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/ytcui";
    return home() + "/.config/ytcui";
}

std::string cache_dir() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/ytcui";
    return home() + "/.cache/ytcui";
}

std::string data_dir() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/ytcui";
    return home() + "/.local/share/ytcui";
}

std::string legacy_data_dir() {
    return home() + "/.local/share/ytcui";
}

std::string data_file(const std::string& name) {
    const std::string primary = data_dir() + "/" + name;
    if (exists(primary)) return primary;
    const std::string legacy = legacy_data_dir() + "/" + name;
    if (exists(legacy)) return legacy;
    return primary;
}

std::string config_file() { return config_dir() + "/config.json"; }

bool exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

bool ensure_dir(const std::string& dir) {
    if (dir.empty()) return false;
    // One component at a time; mkdir on something that already exists is a
    // harmless EEXIST. 0700 because the config directory can hold a cookie
    // jar, which is a credential.
    for (std::size_t i = 1; i < dir.size(); ++i)
        if (dir[i] == '/') ::mkdir(dir.substr(0, i).c_str(), 0700);
    ::mkdir(dir.c_str(), 0700);
    struct stat st{};
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace ytcui::paths
