#include "check.h"
#include "config.h"
#include "library.h"
#include "thumbs.h"
#include "core/proc.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>

int main() {
    char temporary[] = "/tmp/ytcui-runtime-XXXXXX";
    auto directory = ::mkdtemp(temporary);
    if (!directory) return 1;
    std::string dir = std::string(directory) + "/quote ' and spaces";
    std::filesystem::create_directories(dir);
    for (const char* key : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) setenv(key, dir.c_str(), 1);
    auto start = std::chrono::steady_clock::now();
    auto result = ytcui::proc::run({"/bin/sh", "-c", "exec 1>&- 2>&-; sleep 2"}, 100);
    CHECK(result.ran && result.timed_out);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    auto normal = ytcui::proc::run({"/bin/sh", "-c", "printf out; printf err >&2; exit 4"}, 1000);
    CHECK(normal.exit_code == 4 && normal.out == "out" && normal.err == "err");
    ytui::Config a;
    a.backend = ytui::BackendKind::YtcuiWe; a.cookies_file = "/fixture/cookies";
    a.sponsorblock = false; a.resume_playback = false; a.preferred_height = 720;
    a.backend_auto_failover = false; a.loop_video = true; a.streamlined_loop = false;
    a.loop_playlist = false; a.autoplay = "off"; a.sleep_seconds = 34;
    a.keys.timer = 0x15; a.save();
    ytui::Config b; b.load();
    CHECK(b.backend == a.backend && b.cookies_file == a.cookies_file);
    CHECK(!b.sponsorblock && !b.resume_playback && b.preferred_height == 720);
    CHECK(!b.backend_auto_failover && b.loop_video && !b.streamlined_loop);
    CHECK(!b.loop_playlist && b.autoplay == "off" && b.sleep_seconds == 34 && b.keys.timer == 0x15);
    ytui::Library library; library.load(); library.toggle_bookmark("old", "keep", "fixture", "fixture");
    const std::string path = ytui::Library::data_dir() + "/library.json";
    std::ifstream before_file(path); std::string before((std::istreambuf_iterator<char>(before_file)), {});
    pid_t child = fork();
    if (child == 0) {
        signal(SIGXFSZ, SIG_IGN);
        rlimit limit{256,256}; setrlimit(RLIMIT_FSIZE, &limit);
        library.toggle_bookmark("new", std::string(4000, 'X'), "fixture", "fixture");
        _exit(library.take_save_error().empty() ? 1 : 0);
    }
    int status = 0; waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::ifstream after_file(path); std::string after((std::istreambuf_iterator<char>(after_file)), {});
    CHECK(before == after); CHECK(nlohmann::json::parse(after).size() == 1);
    std::string bin = dir + "/bin"; std::filesystem::create_directories(bin);
    std::string marker = std::string(directory) + "/injection-marker";
    { std::ofstream f(bin + "/curl"); f << "#!/bin/sh\nexit 0\n"; }
    chmod((bin + "/curl").c_str(), 0755);
    setenv("PATH", bin.c_str(), 1);
    ytui::Thumbnails::download_async("fixture", "https://fixture.invalid/x'; touch " + marker + "; #");
    while (waitpid(-1, &status, 0) > 0) {}
    CHECK(!std::filesystem::exists(marker));
    CHECK(ytui::Thumbnails::thumb_path("../../escape").empty());
    CHECK(ytui::Thumbnails::thumb_path("fixture").find(dir) == 0);
    std::filesystem::remove_all(directory);
    return tst::finish("runtime regressions");
}
