#include "app.h"
#include "check.h"
#include <future>
#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>
#include <unistd.h>

namespace ytui {
struct AppTestAccess {
    static bool opens_share(App& app) {
        app.state_.focus=Panel::Actions;app.show_share_menu("https://youtu.be/jNQXAC9IVRw");
        return app.state_.show_share && app.state_.focus==Panel::ShareMenu && app.state_.share_return_focus==Panel::Actions;
    }
    static bool start(App& app, const std::string& id) { return app.start_sb_fetch(id); }
    static void video(App& app, const std::string& id) { app.state_.now_playing_id = id; }
    static bool fetching(App& app) { return app.sb_fetching_.load(); }
    static std::string summary(App& app) { app.tick_sponsorblock(); return app.state_.sponsorblock_summary; }
    static std::string loaded(App& app) { std::lock_guard<std::mutex> lock(app.sb_mutex_); return app.sb_video_id_; }
};
}
int main() {
    char temporary[] = "/tmp/ytcui-lifetime-XXXXXX";
    const auto dir = mkdtemp(temporary); if (!dir) return 1;
    for (const auto* key : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) setenv(key, dir, 1);
    setenv("PATH", "/nonexistent", 1); // no external tools during App construction
    std::string cookies = std::string(dir) + "/synthetic-cookies.txt";
    { std::ofstream f(cookies); f << "# Netscape HTTP Cookie File\n.example.invalid\tTRUE\t/\tTRUE\t2147483647\tSAPISID\tfixture\n"; }
    CHECK(!ytui::YouTube::configure_cookies(cookies));
    { std::ofstream f(cookies); f << "# Netscape HTTP Cookie File\n.youtube.com\tTRUE\t/\tTRUE\t2147483647\tSAPISID\tfixture\n"; }
    CHECK(ytui::YouTube::configure_cookies(cookies));
    CHECK(ytui::YouTube::configure_cookies(""));
    std::promise<void> entered_a, release_a, entered_c, release_c;
    auto wait_a = release_a.get_future().share(), wait_c = release_c.get_future().share();
    std::atomic<bool> completed_c{false};
    auto* app = new ytui::App(ytui::Theme::Default, [&](const auto& video, const auto&) {
        if (video == "A") { entered_a.set_value(); wait_a.wait(); }
        if (video == "C") { entered_c.set_value(); wait_c.wait(); completed_c = true; }
        ytcui::sb::Segments segments; segments.fetched = true;
        segments.list.push_back({0, 10, "sponsor", "skip", "fixture", 0, false});
        return segments;
    });
    using Access = ytui::AppTestAccess;
    CHECK(Access::opens_share(*app));
    Access::video(*app, "A"); CHECK(Access::start(*app, "A")); entered_a.get_future().wait();
    Access::video(*app, "B"); CHECK(!Access::start(*app, "B"));
    release_a.set_value();
    for (int i = 0; i < 100 && Access::fetching(*app); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(!Access::fetching(*app)); CHECK(Access::loaded(*app) == "A");
    CHECK(Access::summary(*app).empty()); // A's result is unusable while B is current
    CHECK(Access::start(*app, "B"));
    for (int i = 0; i < 100 && Access::fetching(*app); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(Access::loaded(*app) == "B");
    CHECK(!Access::summary(*app).empty());
    CHECK(Access::start(*app, "C")); entered_c.get_future().wait();
    std::thread release([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); release_c.set_value(); });
    delete app; // must wait for C before App state is freed
    CHECK(completed_c.load()); release.join();
    std::filesystem::remove_all(dir);
    return tst::finish("App worker lifetime");
}
