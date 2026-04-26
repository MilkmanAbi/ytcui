#pragma once
#include "types.h"
#include <curses.h>

namespace ytui {

class InputHandler {
public:
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
};

} // namespace ytui
