#pragma once

#include "types.h"

// Same ncursesw requirement as tui.h
#ifdef __has_include
#  if __has_include(<ncursesw/ncurses.h>)
#    include <ncursesw/ncurses.h>
#  else
#    include <ncurses.h>
#  endif
#else
#  include <ncurses.h>
#endif

namespace ytui {

class InputHandler {
public:
    bool handle(int ch, AppState& state);

private:
    bool handle_search_input(int ch, AppState& state);
    void handle_tabs_input(int ch, AppState& state);
    void handle_results_input(int ch, AppState& state);
    void handle_actions_input(int ch, AppState& state);
    void handle_browser_pick(int ch, AppState& state);
    void handle_sort_menu(int ch, AppState& state);
    void handle_save_prompt(int ch, AppState& state);
};

} // namespace ytui
