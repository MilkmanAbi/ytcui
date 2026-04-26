#pragma once
#include "types.h"
#include "library.h"
#include <curses.h>
#include <string>

namespace ytui {

class TUI {
public:
    TUI();
    ~TUI();

    bool init();
    void shutdown();
    void render(const AppState& state, const Library* lib);
    void get_dimensions(int& w, int& h);

    static int  utf8_display_width(const std::string& s);
    static std::string utf8_truncate(const std::string& s, int max_cols);

private:
    bool initialized_ = false;
    bool thumb_color_ready_ = false; // true when pairs 17-272 are initialised

    void setup_colors(const AppState& state);
    void init_thumb_colors();

    // Renders a cached thumbnail at (x, y) into a region (cols×rows).
    // Uses 256-colour pairs when available, falls back to monochrome.
    // All output goes through ncurses — erase() clears it cleanly.
    void draw_thumb(const std::string& video_id,
                    int x, int y, int cols, int rows, int bot);
    void draw_header(const AppState& state);
    void draw_search_box(const AppState& state);
    void draw_tabs(const AppState& state);
    void draw_home_panels(const AppState& state, const Library* lib);
    void draw_results_panel(const AppState& state);
    void draw_actions_panel(const AppState& state);
    void draw_info_panel(const AppState& state, int px, int py, int pw, int ph, bool with_thumb);
    void draw_message_bar(const AppState& state);
    void draw_browser_popup(const AppState& state);
    void draw_sort_menu(const AppState& state);
    void draw_save_prompt(const AppState& state);

    // Playlist UI
    void draw_playlists_tab(const AppState& state, const Library* lib);
    void draw_playlist_view(const AppState& state, const Library* lib);
    void draw_playlist_actions_panel(const AppState& state, const Library* lib);
    void draw_playlist_picker(const AppState& state);
    void draw_new_playlist_prompt(const AppState& state);

    void draw_box(int y, int x, int h, int w, int color_pair);
};

} // namespace ytui
