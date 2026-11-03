#pragma once
#include "types.h"
#include "tui.h"
#include "youtube.h"
#include "player.h"
#include "input.h"
#include "config.h"
#include "library.h"
#include "theme.h"
#include "core/timer.h"
#include "core/queue.h"
#include "core/watchstate.h"

namespace ytui {

class App {
public:
    App(Theme theme);
    ~App();
    int run();
    void force_cleanup();
    void set_player_options(const PlayerOptions& opts);
    // Override the thumbnail graphics mode from the CLI (--gfx). Re-resolves
    // against the terminal. Empty string leaves the config value in place.
    void set_graphics_mode(const std::string& mode);
    void set_ui_mode(const std::string& mode);

private:
    AppState state_;
    TUI      tui_;
    YouTube  youtube_;
    Player   player_;
    InputHandler input_;
    Config   config_;
    Library  library_;

    // ── v5 core features ─────────────────────────────────────────────────────
    ytcui::SleepTimer timer_;
    Queue             queue_;
    ytcui::WatchState watch_state_;

    void build_actions();
    void build_playlist_actions();
    void do_search();
    void execute_action(Action action);
    void execute_playlist_action(Action action);
    void open_in_browser(const std::string& url);
    void copy_to_clipboard(const std::string& text);
    void prefetch_thumbnails();
    void resolve_graphics();
    // Force-disable features the terminal can't actually do (colour, chafa,
    // sixel/kitty/iterm), unless config.force_features overrides. Pass
    // authoritative=true once ncurses has reported the real colour count.
    void apply_capability_overrides(bool authoritative);
    void show_browser_picker();
    void show_playlist_picker();
    void enter_playlist(const std::string& playlist_id);
    void refresh_playlist_names();
    void apply_sort_filter();
    void do_save(int choice);

    // In-app settings UI (Ctrl-S): live theme switching + accelerator rebinding.
    void handle_settings_key(int ch);
    void apply_theme_live(const std::string& theme_name);  // re-theme, no restart

    // ── v5 feature handlers ───────────────────────────────────────────────────
    void on_video_end();            // queue auto-advance + autoplay
    void sync_queue_display();      // copy queue_ to state_.queue_items
    void handle_timer_key();        // open timer prompt (Ctrl-T)
    void apply_timer_input();       // parse and arm the typed duration
    void show_share_menu(const std::string& url);
    void switch_backend(BackendKind k);
    void tick_timer();              // called each frame; pauses mpv when due
};

} // namespace ytui
