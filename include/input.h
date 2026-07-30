#pragma once
#include "types.h"
#include "config.h"
#include <curses.h>

namespace ytui {

class InputHandler {
public:
    // Point the handler at the live keybindings so configured/rebound keys
    // actually take effect. Call once at startup. If never set, a built-in
    // default set is used so input still works.
    void set_keybindings(const Config::KeyBindings* kb) { keys_ = kb; }

    // Returns true if a search should be triggered
    bool handle(int ch, AppState& state);

private:
    bool handle_search_input(int ch, AppState& state);
    void handle_tabs_input(int ch, AppState& state);
    void handle_results_input(int ch, AppState& state);
    void handle_actions_input(int ch, AppState& state);
    void handle_browser_pick(int ch, AppState& state);
    void handle_sort_menu(int ch, AppState& state);
    void handle_save_prompt(int ch, AppState& state);
    // Playlist handlers
    void handle_playlist_list(int ch, AppState& state);
    void handle_playlist_view(int ch, AppState& state);
    void handle_playlist_actions(int ch, AppState& state);
    void handle_playlist_pick(int ch, AppState& state);
    void handle_new_playlist(int ch, AppState& state);

    // Live keybindings (owned by Config in App). May be null before set.
    const Config::KeyBindings* keys_ = nullptr;
    Config::KeyBindings default_keys_{};   // fallback when keys_ is null
    const Config::KeyBindings& kb() const { return keys_ ? *keys_ : default_keys_; }
};

} // namespace ytui
