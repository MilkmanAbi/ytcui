#pragma once

#include "types.h"
#include "tui.h"
#include "youtube.h"
#include "player.h"
#include "input.h"
#include "config.h"
#include "library.h"
#include "theme.h"

namespace ytui {

class App {
public:
    App(Theme theme);
    ~App();
    int run();
    void force_cleanup();
    void set_player_options(const PlayerOptions& opts) { player_.set_options(opts); }

private:
    AppState state_;
    TUI tui_;
    YouTube youtube_;
    Player player_;
    InputHandler input_;
    Config config_;
    Library library_;

    void build_actions();
    void do_search();
    void execute_action(Action action);
    void open_in_browser(const std::string& url);
    void copy_to_clipboard(const std::string& text);
    void prefetch_thumbnails();
    void show_browser_picker();
    void apply_sort_filter();
    void do_save(int choice); // 0=bookmark, 1=dl video, 2=dl audio
};

} // namespace ytui
