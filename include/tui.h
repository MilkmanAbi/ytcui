#pragma once
#include "types.h"
#include "library.h"
#include <curses.h>
#include <string>
#include <vector>

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
    bool thumb_color_ready_ = false; // true when thumbnail colour pairs are usable
    int  thumb_pairs_ = 0;           // # of usable thumb pairs (256 = full fidelity,
                                     // fewer on old/limited ncurses; 0 = monochrome)

    // ── Raster-graphics thumbnails (Sixel/Kitty/iTerm2) ───────────────────────
    // When a raster protocol is active, draw_thumb() does NOT paint block art;
    // it records the cell region here, and flush_graphics() emits the real
    // image escape straight to the tty AFTER ncurses refresh() (these bytes
    // cannot go through ncurses). 0=None 1=Blocks 2=Sixel 3=Kitty 4=Iterm.
    int gfx_mode_ = 1;
    bool kitty_drawn_ = false;        // a kitty image is currently on screen
    struct GfxRegion { std::string id; int x, y, cols, rows; };
    std::vector<GfxRegion> pending_gfx_;
    void flush_graphics();
    bool gfx_is_raster() const { return gfx_mode_ >= 2; }

    // ── Selection highlight (terminal-robust) ─────────────────────────────────
    // sel_use_color_ is set from TermCaps at init: a real fg/bg pair when the
    // terminal has colour, else A_REVERSE. The bar is always painted as full
    // width via explicit spaces, never via clrtoeol/bkgd (bce is unreliable).
    bool sel_use_color_ = true;
    bool mouse_enabled_ = false;
    chtype sel_attr() const {
        return sel_use_color_ ? COLOR_PAIR(Color::SELECTED_BAR) : (chtype)A_REVERSE;
    }
    void paint_sel_bar(int y, int x, int w);

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
