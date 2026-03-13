#pragma once

#include "types.h"
#include "library.h"

// CRITICAL: Must use ncursesw for proper UTF-8 / wide-char support.
// ncurses (non-wide) displays raw UTF-8 bytes as M-x Meta sequences.
#ifdef __has_include
#  if __has_include(<ncursesw/ncurses.h>)
#    include <ncursesw/ncurses.h>
#  else
#    include <ncurses.h>
#  endif
#else
#  include <ncurses.h>
#endif

#include <string>

namespace ytui {

class TUI {
public:
    TUI();
    ~TUI();
    bool init();
    void shutdown();
    void render(const AppState& state, const Library* lib = nullptr);
    void get_dimensions(int& w, int& h);

    static std::string utf8_truncate(const std::string& s, int max_cols);
    static int utf8_display_width(const std::string& s);

private:
    bool initialized_ = false;

    void draw_header(const AppState& state);
    void draw_search_box(const AppState& state);
    void draw_tabs(const AppState& state);
    void draw_home_panels(const AppState& state, const Library* lib);
    void draw_results_panel(const AppState& state);
    void draw_actions_panel(const AppState& state);
    void draw_info_panel(const AppState& state, int x, int y, int w, int h, bool with_thumb);
    void draw_message_bar(const AppState& state);
    void draw_browser_popup(const AppState& state);
    void draw_sort_menu(const AppState& state);
    void draw_save_prompt(const AppState& state);

    void draw_box(int y, int x, int h, int w, int color_pair);
    void setup_colors(const AppState& state);
};

} // namespace ytui
