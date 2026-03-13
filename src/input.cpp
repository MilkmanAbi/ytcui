#include "input.h"
#include <algorithm>
#include <wchar.h>

namespace ytui {

static int calc_max_visible(const AppState& s) {
    int start_y = 7, end_y = s.term_h - 4;
    return std::max(1, end_y - start_y - 1);
}

static void clamp_scroll(AppState& s) {
    int mv = calc_max_visible(s);
    if (s.selected_result >= s.results_scroll + mv)
        s.results_scroll = s.selected_result - mv + 1;
    if (s.selected_result < s.results_scroll)
        s.results_scroll = s.selected_result;
    int max_s = (int)s.results.size() - mv;
    if (max_s < 0) max_s = 0;
    s.results_scroll = std::clamp(s.results_scroll, 0, max_s);
}

// Pop one UTF-8 character from the end (handles multi-byte correctly)
static void utf8_pop_back(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    // Walk backwards past continuation bytes (10xxxxxx)
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    s.erase(i);
}

bool InputHandler::handle(int ch, AppState& state) {
    if (ch == ERR) return false;

    // Mouse events
    if (ch == KEY_MOUSE) {
        MEVENT ev;
        if (getmouse(&ev) == OK) {
            if (ev.bstate & BUTTON4_PRESSED) {
                bool on_results = (state.active_tab == Tab::Results && !state.results.empty());
                if (on_results && (state.focus == Panel::Results || state.focus == Panel::Search || state.focus == Panel::Tabs)) {
                    if (state.selected_result > 0) { state.selected_result--; clamp_scroll(state); }
                } else if (state.focus == Panel::Actions) {
                    if (state.selected_action > 0) state.selected_action--;
                } else if (state.focus == Panel::BrowserPick) {
                    if (state.browser_pick_idx > 0) state.browser_pick_idx--;
                }
                return false;
            }
            if (ev.bstate & BUTTON5_PRESSED) {
                bool on_results = (state.active_tab == Tab::Results && !state.results.empty());
                if (on_results && (state.focus == Panel::Results || state.focus == Panel::Search || state.focus == Panel::Tabs)) {
                    if (state.selected_result < (int)state.results.size() - 1) { state.selected_result++; clamp_scroll(state); }
                } else if (state.focus == Panel::Actions) {
                    if (state.selected_action < (int)state.actions.size() - 1) state.selected_action++;
                } else if (state.focus == Panel::BrowserPick) {
                    if (state.browser_pick_idx < (int)state.browser_choices.size() - 1) state.browser_pick_idx++;
                }
                return false;
            }

            if (ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED)) {
                int my = ev.y, mx = ev.x;
                if (my >= 1 && my <= 3) {
                    // Hamburger button is the rightmost 5 cols
                    int hb_x = state.term_w - 5;
                    if (mx >= hb_x) {
                        state.sort_col = 0; state.sort_row = 0;
                        state.focus = Panel::SortMenu;
                    } else {
                        state.focus = Panel::Search;
                    }
                    return false;
                }
                if (my >= 4 && my <= 6) {
                    bool has_results = !state.results.empty();
                    int n_tabs = has_results ? 4 : 3;
                    int tab_w = 12;
                    int total_w = tab_w * n_tabs + 2 * (n_tabs - 1);
                    int start_x = std::max(0, (state.term_w - total_w) / 2);
                    Tab clicked = state.active_tab;
                    if      (mx >= start_x && mx < start_x + tab_w)                              clicked = Tab::Library;
                    else if (mx >= start_x + (tab_w+2) && mx < start_x + 2*(tab_w+2) - 2)       clicked = Tab::Feed;
                    else if (mx >= start_x + 2*(tab_w+2) && mx < start_x + 3*(tab_w+2) - 2)     clicked = Tab::History;
                    else if (has_results && mx >= start_x + 3*(tab_w+2) && mx < start_x + 4*tab_w+6) clicked = Tab::Results;
                    state.active_tab = clicked;
                    if (clicked == Tab::Results && !state.results.empty())
                        state.focus = Panel::Results;
                    else
                        state.focus = Panel::Tabs;
                    return false;
                }
                int start_y = 7, end_y = state.term_h - 4;
                if (my > start_y && my < end_y && !state.results.empty()) {
                    if (state.actions_visible) {
                        int left_w = state.term_w * 30 / 100;
                        if (left_w < 15) left_w = 15;
                        if (mx > left_w) {
                            int action_idx = my - start_y - 1;
                            if (action_idx >= 0 && action_idx < (int)state.actions.size()) {
                                state.selected_action = action_idx;
                                state.status_message = "__EXEC_ACTION__";
                            }
                        }
                    } else {
                        int left_w = state.term_w * 60 / 100;
                        if (mx < left_w) {
                            int result_idx = (my - start_y - 1) + state.results_scroll;
                            if (result_idx >= 0 && result_idx < (int)state.results.size()) {
                                state.focus = Panel::Results;
                                if (state.selected_result == result_idx) {
                                    state.actions_visible = true;
                                    state.focus = Panel::Actions;
                                    state.selected_action = 0;
                                } else {
                                    state.selected_result = result_idx;
                                    clamp_scroll(state);
                                }
                            }
                        }
                    }
                }
                return false;
            }
        }
        return false;
    }

    // Popup modes
    if (state.focus == Panel::BrowserPick) { handle_browser_pick(ch, state); return false; }
    if (state.focus == Panel::SortMenu) { handle_sort_menu(ch, state); return false; }
    if (state.focus == Panel::SavePrompt) { handle_save_prompt(ch, state); return false; }

    // Global: 'p' to pause/resume audio (except in search input)
    if (ch == 'p' && state.focus != Panel::Search && state.is_playing) {
        state.status_message = "__PAUSE_TOGGLE__";
        return false;
    }

    // Quit
    if (ch == 'q' && state.focus != Panel::Search) {
        state.running = false;
        return false;
    }

    // Tab cycles focus
    if (ch == '\t') {
        switch (state.focus) {
            case Panel::Search:  state.focus = Panel::Tabs; break;
            case Panel::Tabs:
                if (state.active_tab == Tab::Results && !state.results.empty())
                    state.focus = Panel::Results;
                else
                    state.focus = Panel::Search;
                break;
            case Panel::Results: state.focus = Panel::Search; break;
            case Panel::Actions: state.focus = Panel::Results; state.actions_visible = false; break;
            default: break;
        }
        return false;
    }

    // Escape
    if (ch == 27) {
        if (state.focus == Panel::Actions) {
            state.focus = Panel::Results; state.actions_visible = false;
        } else if (state.focus == Panel::Results) {
            // Esc from results: go back to a home tab if we came from there,
            // otherwise just go to Search
            state.focus = Panel::Search;
        } else if (state.focus == Panel::Tabs) {
            state.focus = Panel::Search;
        }
        return false;
    }

    switch (state.focus) {
        case Panel::Search:
            return handle_search_input(ch, state);
        case Panel::Tabs:
            handle_tabs_input(ch, state);
            return false;
        case Panel::Results:
            handle_results_input(ch, state);
            return false;
        case Panel::Actions:
            handle_actions_input(ch, state);
            return false;
        default:
            return false;
    }
}

// IMPROVED UTF-8 input handling with better validation
bool InputHandler::handle_search_input(int ch, AppState& state) {
    switch (ch) {
        case KEY_BACKSPACE: case 127: case 8:
            utf8_pop_back(state.search_query);
            return false;
        case '\n': case KEY_ENTER:
            return true;
        case KEY_DC:
            state.search_query.clear();
            return false;
        case KEY_DOWN:
            if (!state.results.empty()) state.focus = Panel::Results;
            else state.focus = Panel::Tabs;
            return false;
        default:
            // CRITICAL FIX: Better UTF-8 input with validation
            if (ch >= 32 && ch < 127) {
                // Plain ASCII - just add it
                state.search_query += (char)ch;
            } else if (ch >= 128) {
                // Multi-byte UTF-8 character
                unsigned char lead = (unsigned char)ch;
                int bytes_needed = 0;
                
                // Determine number of continuation bytes
                if ((lead & 0xE0) == 0xC0) bytes_needed = 1;       // 2-byte (110xxxxx)
                else if ((lead & 0xF0) == 0xE0) bytes_needed = 2;  // 3-byte (1110xxxx)
                else if ((lead & 0xF8) == 0xF0) bytes_needed = 3;  // 4-byte (11110xxx)
                else return false;  // Invalid lead byte
                
                // Store lead byte
                state.search_query += (char)ch;
                
                // Read continuation bytes with validation
                timeout(50);  // 50ms timeout per byte
                
                for (int i = 0; i < bytes_needed; i++) {
                    int b = getch();
                    
                    // Validate continuation byte (must be 10xxxxxx)
                    if (b == ERR || (b & 0xC0) != 0x80) {
                        // Invalid sequence - remove what we added
                        state.search_query.pop_back();
                        break;
                    }
                    
                    state.search_query += (char)b;
                }
                
                timeout(100);  // Restore normal timeout
            }
            return false;
    }
}

void InputHandler::handle_tabs_input(int ch, AppState& state) {
    bool has_results = !state.results.empty();
    switch (ch) {
        case 'h': case KEY_LEFT:
            if      (state.active_tab == Tab::Feed)    state.active_tab = Tab::Library;
            else if (state.active_tab == Tab::History) state.active_tab = Tab::Feed;
            else if (state.active_tab == Tab::Results) state.active_tab = Tab::History;
            break;
        case 'l': case KEY_RIGHT:
            if      (state.active_tab == Tab::Library) state.active_tab = Tab::Feed;
            else if (state.active_tab == Tab::Feed)    state.active_tab = Tab::History;
            else if (state.active_tab == Tab::History && has_results) state.active_tab = Tab::Results;
            break;
        case '\n': case KEY_ENTER:
            if (state.active_tab == Tab::Results && has_results)
                state.focus = Panel::Results;
            break;
        case 'j': case KEY_DOWN:
            if (state.active_tab == Tab::Results && has_results)
                state.focus = Panel::Results;
            break;
        case 'k': case KEY_UP:
            state.focus = Panel::Search;
            break;
    }
}

void InputHandler::handle_results_input(int ch, AppState& state) {
    int total = (int)state.results.size();
    if (total == 0) return;
    switch (ch) {
        case 'j': case KEY_DOWN:
            if (state.selected_result < total - 1) { state.selected_result++; clamp_scroll(state); }
            break;
        case 'k': case KEY_UP:
            if (state.selected_result > 0) { state.selected_result--; clamp_scroll(state); }
            else state.focus = Panel::Tabs;
            break;
        case 'g': state.selected_result = 0; state.results_scroll = 0; break;
        case 'G': state.selected_result = total - 1; clamp_scroll(state); break;
        case '\n': case KEY_ENTER: case 'l': case KEY_RIGHT:
            state.actions_visible = true;
            state.focus = Panel::Actions;
            state.selected_action = 0;
            break;
        case '/': state.focus = Panel::Search; break;
        case 's':
            state.sort_col = 0; state.sort_row = 0;
            state.focus = Panel::SortMenu;
            break;
    }
}

void InputHandler::handle_actions_input(int ch, AppState& state) {
    int total = (int)state.actions.size();
    if (total == 0) return;
    switch (ch) {
        case 'j': case KEY_DOWN:
            if (state.selected_action < total - 1) state.selected_action++;
            break;
        case 'k': case KEY_UP:
            if (state.selected_action > 0) state.selected_action--;
            break;
        case '\n': case KEY_ENTER:
            state.status_message = "__EXEC_ACTION__";
            break;
        case 'h': case KEY_LEFT:
            state.focus = Panel::Results; state.actions_visible = false;
            break;
    }
}

void InputHandler::handle_browser_pick(int ch, AppState& state) {
    int total = (int)state.browser_choices.size();
    switch (ch) {
        case 'j': case KEY_DOWN:
            if (state.browser_pick_idx < total - 1) state.browser_pick_idx++;
            break;
        case 'k': case KEY_UP:
            if (state.browser_pick_idx > 0) state.browser_pick_idx--;
            break;
        case '\n': case KEY_ENTER:
            state.status_message = "__BROWSER_PICKED__";
            break;
        case 27:
            state.focus = Panel::Actions;
            state.browser_choices.clear();
            break;
    }
}

void InputHandler::handle_sort_menu(int ch, AppState& state) {
    int max_rows = (state.sort_col == 0) ? 4 : 6;
    switch (ch) {
        case 'j': case KEY_DOWN:
            if (state.sort_row < max_rows - 1) state.sort_row++;
            break;
        case 'k': case KEY_UP:
            if (state.sort_row > 0) state.sort_row--;
            break;
        case 'h': case KEY_LEFT:
            state.sort_col = 0; state.sort_row = std::min(state.sort_row, 3);
            break;
        case 'l': case KEY_RIGHT:
            state.sort_col = 1; state.sort_row = std::min(state.sort_row, 5);
            break;
        case '\n': case KEY_ENTER:
            state.status_message = "__SORT_APPLIED__";
            state.focus = state.results.empty() ? Panel::Search : Panel::Results;
            break;
        case 27:
            state.focus = state.results.empty() ? Panel::Search : Panel::Results;
            break;
    }
}

void InputHandler::handle_save_prompt(int ch, AppState& state) {
    switch (ch) {
        case 'j': case KEY_DOWN:
            if (state.save_prompt_idx < 2) state.save_prompt_idx++;
            break;
        case 'k': case KEY_UP:
            if (state.save_prompt_idx > 0) state.save_prompt_idx--;
            break;
        case '\n': case KEY_ENTER:
            state.status_message = "__SAVE_PICKED__";
            break;
        case 27:
            state.focus = Panel::Actions;
            break;
    }
}

} // namespace ytui
