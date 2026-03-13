#pragma once

#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace ytui {

class Thumbnails {
public:
    // Get the cache directory
    static std::string cache_dir() {
        const char* xdg = getenv("XDG_CACHE_HOME");
        std::string dir;
        if (xdg && xdg[0] != '\0') {
            dir = std::string(xdg) + "/ytcui/thumbs";
        } else {
            const char* home = getenv("HOME");
            dir = std::string(home ? home : "/tmp") + "/.cache/ytcui/thumbs";
        }
        return dir;
    }

    // Check if chafa is available for rendering
    static bool renderer_available() {
        return system("which chafa > /dev/null 2>&1") == 0;
    }

    // Get the local path for a thumbnail
    static std::string thumb_path(const std::string& video_id) {
        return cache_dir() + "/" + video_id + ".jpg";
    }

    // Check if thumbnail is already cached
    static bool is_cached(const std::string& video_id) {
        struct stat st;
        return stat(thumb_path(video_id).c_str(), &st) == 0 && st.st_size > 0;
    }

    // Download a thumbnail in the background (non-blocking)
    // Uses fork+exec so it doesn't block the TUI
    static void download_async(const std::string& video_id, const std::string& url) {
        if (url.empty() || video_id.empty()) return;
        if (is_cached(video_id)) return;

        // Ensure cache dir exists
        std::string dir = cache_dir();
        mkdir_p(dir);

        std::string path = thumb_path(video_id);

        // Fire and forget curl download
        pid_t pid = fork();
        if (pid == 0) {
            // Child: silent curl download
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            execlp("curl", "curl", "-sL", "-o", path.c_str(),
                   "--max-time", "5", url.c_str(), nullptr);
            _exit(1);
        }
        // Parent doesn't wait — we'll check if file exists later
    }

    // Download thumbnails for a batch of results
    static void download_batch(const std::vector<std::pair<std::string, std::string>>& items) {
        for (const auto& [id, url] : items) {
            download_async(id, url);
        }
        // Reap any finished children to avoid zombies
        int status;
        while (waitpid(-1, &status, WNOHANG) > 0) {}
    }

    // Render a thumbnail to a string that can be printed at a position
    // Returns the rendered output as a string, or empty if not available
    // cols/rows control the size in terminal cells
    static std::string render(const std::string& video_id, int cols, int rows) {
        if (!is_cached(video_id) || !renderer_available()) return "";

        std::string path = thumb_path(video_id);

        // Use chafa to render to terminal characters
        // chafa auto-detects best mode (kitty/sixel/halfblock/ascii)
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "chafa -s %dx%d --animate=off '%s' 2>/dev/null",
                 cols, rows, path.c_str());

        std::string result;
        FILE* pipe = popen(cmd, "r");
        if (!pipe) return "";

        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe)) {
            result += buf;
        }
        pclose(pipe);

        return result;
    }

private:
    static void mkdir_p(const std::string& path) {
        std::string cmd = "mkdir -p '" + path + "'";
        int ret = system(cmd.c_str());
        (void)ret;
    }
};

} // namespace ytui
