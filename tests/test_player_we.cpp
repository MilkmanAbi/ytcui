#include "check.h"
#include "player_we.h"
#include "player_resolve.h"
#include "core/proc.h"
#include "atomic_file.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <unistd.h>

using json = nlohmann::json;
using namespace ytui;
static std::string env(const char* name) { auto p = std::getenv(name); return p ? p : ""; }
static json read(const std::string& path) { std::ifstream f(path); return json::parse(f); }

// The same executable doubles as argv-recording child fixtures. No shell/network.
int main(int argc, char** argv) {
    auto name = std::filesystem::path(argv[0]).filename().string();
    if (name == "fake-we" || name == "fake-dlp" || name == "mpv") {
        json args = json::array();
        for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
        atomic_write(env("FIXTURE_LOG") + (name == "mpv" ? ".mpv" : ""), args.dump());
        if (name == "mpv") return 0;
        if (!env("FIXTURE_FAIL").empty()) { std::fputs("fixture resolver error\n", stderr); return 3; }
        if (!env("FIXTURE_MALFORMED").empty()) { std::puts("[]"); return 0; }
        if (name == "fake-we") std::puts(R"({"video_url":"https://fixture.invalid/video","audio_url":"https://fixture.invalid/audio","user_agent":"fixture UA","is_live":false,"heights":[1080,720]})");
        else std::puts(R"({"requested_formats":[{"url":"https://fixture.invalid/dlp-video","http_headers":{"User-Agent":"dlp UA"}},{"url":"https://fixture.invalid/dlp-audio"}],"formats":[{"height":720},{"height":360}]})");
        return 0;
    }
    std::string built = argc == 2 ? argv[1] : find_ytcuiwe_binary();
    CHECK(std::filesystem::equivalent(built, find_ytcuiwe_binary()));
    CHECK(!built.empty());
    auto smoke = ytcui::proc::run({built, "--help"}, 5000);
    CHECK(smoke.ran && !smoke.timed_out && smoke.exit_code == 0);
    CHECK(smoke.out.find("ytcui-we") != std::string::npos);
    auto invalid = ytcui::proc::run({built, "--resolve-json", "--audio", "-q", "720", "--", "bad-id"}, 5000);
    CHECK(invalid.ran && !invalid.timed_out && invalid.exit_code != 0);
    CHECK(invalid.err.find("invalid YouTube video ID") != std::string::npos);
    CHECK(invalid.err.find("unsupported option") == std::string::npos);
    char temporary[] = "/tmp/ytcui-player-XXXXXX";
    auto directory = ::mkdtemp(temporary);
    if (!directory) return 1;
    std::string dir = directory, self = ytcui::proc::self_exe();
    if (self.empty()) self = std::filesystem::absolute(argv[0]).string();
    for (const auto& file : {"fake-we", "fake-dlp", "mpv"})
        std::filesystem::create_symlink(self, dir + "/" + file);
    std::string log = dir + "/args.json";
    setenv("FIXTURE_LOG", log.c_str(), 1);
    setenv("YTCUI_WE_BINARY", (dir + "/fake-we").c_str(), 1);
    setenv("PATH", (dir + ":" + env("PATH")).c_str(), 1);
    setenv("XDG_CACHE_HOME", dir.c_str(), 1);
    PlayerOptions options;
    options.backend_auto_failover = false;
    options.cookies_file = dir + "/cookies with ' quote.txt";
    options.preferred_height = 720;
    options.ytdlp_path = dir + "/fake-dlp";
    const std::string url = "https://fixture.invalid/watch?v=quoted';$(marker)";
    int native_calls = 0;
    NativeResolver native = [&](const std::string& actual, PlayMode, const PlayerOptions& o) {
        ++native_calls; CHECK(actual == url); CHECK(o.preferred_height == 720);
        ResolvedStreams s; s.video_url = "native-url"; s.rungs = {{480,"480p"}}; return s;
    };
    for (auto backend : {BackendKind::YtcuiDl, BackendKind::YtDlp, BackendKind::YtcuiWe}) {
        options.backend = backend;
        for (auto mode : {PlayMode::Video, PlayMode::Audio, PlayMode::AudioLoop}) {
            auto result = resolve_playback(url, mode, options, native);
            CHECK(result.backend == backend);
            if (backend == BackendKind::YtcuiDl) { CHECK(result.video_url == "native-url"); continue; }
            auto args = read(log);
            if (backend == BackendKind::YtcuiWe) {
                json expected = {"--resolve-json"};
                if (mode != PlayMode::Video) expected.push_back("--audio");
                for (const auto& v : {"-q", "720", "--cookies"}) expected.push_back(v);
                expected.push_back(options.cookies_file); expected.push_back("--"); expected.push_back(url);
                CHECK(args == expected);
                CHECK(result.user_agent == "fixture UA"); CHECK(result.rungs.front().first == 1080);
            } else {
                std::string format = mode == PlayMode::Video
                    ? "bestvideo[height<=720]+bestaudio/best[height<=720]" : "bestaudio/best";
                json expected = {"--dump-single-json", "--no-warnings", "--no-playlist", "-f", format,
                                 "--cookies", options.cookies_file, "--", url};
                CHECK(args == expected);
                CHECK(result.user_agent == "dlp UA" && result.rungs.size() == 2);
            }
            std::filesystem::remove(log + ".mpv");
            Player player; player.set_options(options); player.play(url, "fixture title", mode);
            for (int i = 0; i < 100 && !std::filesystem::exists(log + ".mpv"); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CHECK(std::filesystem::exists(log + ".mpv"));
            if (std::filesystem::exists(log + ".mpv")) {
                auto mpv = read(log + ".mpv"); CHECK(mpv.back() == result.video_url);
                bool no_video = false, loop = false;
                for (const auto& arg : mpv) { no_video |= arg == "--no-video"; loop |= arg == "--loop=inf"; }
                CHECK(no_video == (mode != PlayMode::Video)); CHECK(loop == (mode == PlayMode::AudioLoop));
            }
            CHECK(player.resolution_rungs() == result.rungs);
            setenv("FIXTURE_FAIL", "1", 1);
            player.play(url, "failed second video", mode);
            CHECK(player.resolution_rungs().empty() && !player.is_playing());
            unsetenv("FIXTURE_FAIL");
        }
    }
    CHECK(native_calls == 3);
    options.backend = BackendKind::YtcuiWe;
    setenv("FIXTURE_FAIL", "1", 1);
    bool threw = false;
    try { resolve_playback(url, PlayMode::Video, options, native); } catch (...) { threw = true; }
    CHECK(threw && native_calls == 3);
    options.backend_auto_failover = true;
    CHECK(resolve_playback(url, PlayMode::Video, options, native).backend == BackendKind::YtcuiDl);
    CHECK(native_calls == 4);
    unsetenv("FIXTURE_FAIL"); setenv("FIXTURE_MALFORMED", "1", 1);
    threw = false;
    try { we_resolve(url, 720, ""); } catch (...) { threw = true; }
    CHECK(threw);
    // This directory was created by mkdtemp and contains only this test's fixtures.
    std::filesystem::remove_all(dir);
    return tst::finish("player backend dispatch");
}
