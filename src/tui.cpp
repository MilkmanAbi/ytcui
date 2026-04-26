#include "tui.h"
#include "thumbs.h"
#include "theme.h"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <wchar.h>

namespace ytui {

// ─── UTF-8 helpers ───────────────────────────────────────────────────────────
//
// Root cause of the M-hM-... mojibake was twofold:
//  1. Linking against -lncurses (narrow) instead of -lncursesw (wide).
//     Narrow ncurses treats every byte > 127 as a "Meta" character and renders
//     it as M-x. Wide ncurses (ncursesw) understands multibyte locale strings.
//  2. mbtowc() doesn't carry state across calls — mbrtowc() does.
//
// With ncursesw + setlocale(LC_ALL,"") + mbrtowc, mvprintw("%s", utf8_str)
// works correctly for CJK, emoji, and all Unicode.

int TUI::utf8_display_width(const std::string& s) {
    int w = 0;
    const char* p = s.c_str();
    const char* end = p + s.size();
    mbstate_t mbs{};
    while (p < end) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, p, end - p, &mbs);
        if (n == (size_t)-1 || n == (size_t)-2) { p++; w++; continue; }
        if (n == 0) break;
        int cw = wcwidth(wc);
        w += (cw > 0) ? cw : 0;
        p += n;
    }
    return w;
}

// Truncate s so its display width <= max_cols, appending "..." if cut.
std::string TUI::utf8_truncate(const std::string& s, int max_cols) {
    if (max_cols <= 0) return "";
    if (utf8_display_width(s) <= max_cols) return s;
    if (max_cols <= 3) return std::string(max_cols, '.');

    int target = max_cols - 3;
    std::string result;
    int cols = 0;
    const char* p = s.c_str();
    const char* end = p + s.size();
    mbstate_t mbs{};
    while (p < end) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, p, end - p, &mbs);
        if (n == (size_t)-1 || n == (size_t)-2) { p++; continue; }
        if (n == 0) break;
        int cw = wcwidth(wc); if (cw < 0) cw = 1;
        if (cols + cw > target) break;
        result.append(p, n);
        cols += cw;
        p += n;
    }
    result += "...";
    return result;
}

// Advance through s by up to max_display_cols columns, return remainder.
// Used for word-wrapping without chopping multibyte sequences.



// ─── TUI lifecycle ────────────────────────────────────────────────────────────
TUI::TUI() {}
TUI::~TUI() { shutdown(); }

bool TUI::init() {
    if (initialized_) return true;
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    nodelay(stdscr, FALSE); timeout(100); set_escdelay(25);
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
    mouseinterval(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_thumb_colors();
    }
    initialized_ = true;
    return true;
}

void TUI::shutdown() {
    if (initialized_) { endwin(); initialized_ = false; }
}

// ── EP1: initialise one ncurses color pair per 256-colour index ───────────────
// Pairs 17-272: pair(THUMB_COLOR_BASE + N) → fg=N, bg=terminal-default.
// We check COLOR_PAIRS at runtime — if the terminal can't support 273 pairs
// we skip and draw_thumb() falls back to monochrome automatically.
void TUI::init_thumb_colors() {
    thumb_color_ready_ = false;
    if (COLORS < 256) return;
    if (COLOR_PAIRS < THUMB_COLOR_BASE + 256) return;
    for (int n = 0; n < 256; n++)
        init_pair(short(THUMB_COLOR_BASE + n), short(n), -1);
    thumb_color_ready_ = true;
}

// ── EP1: unified thumbnail renderer ──────────────────────────────────────────
// Tries 256-colour first; falls back to monochrome.
// All output goes through ncurses mvaddstr/attron — erase() owns and clears it.
void TUI::draw_thumb(const std::string& video_id,
                     int x, int y, int cols, int rows, int bot) {
    if (thumb_color_ready_) {
        ThumbData td = Thumbnails::render_color(video_id, cols, rows);
        if (!td.empty()) {
            int row = 0;
            for (auto& line : td) {
                if (y + row >= bot || row >= rows) break;
                move(y + row, x);
                for (auto& cell : line) {
                    int pair = (cell.color_idx >= 0 && cell.color_idx < 256)
                                   ? THUMB_COLOR_BASE + cell.color_idx
                                   : Color::BG;
                    attron(COLOR_PAIR(pair));
                    addstr(cell.glyph.c_str());
                    attroff(COLOR_PAIR(pair));
                }
                row++;
            }
            return;
        }
        // render_color returned empty (chafa issue?) — fall through to mono
    }

    // Monochrome fallback
    std::string r = Thumbnails::render(video_id, cols, rows);
    if (r.empty()) return;
    std::istringstream ss(r);
    std::string ln;
    int tl = 0;
    while (std::getline(ss, ln) && y + tl < bot && tl < rows) {
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        move(y + tl, x);
        addstr(ln.c_str());
        tl++;
    }
}

void TUI::setup_colors(const AppState& state) {
    // resolved_colors is pre-computed by App: base theme + custom overrides.
    const ThemeColors& tc = state.resolved_colors;

    init_pair(Color::BG,         tc.bg,         -1);
    init_pair(Color::SEARCH_BOX, tc.search_box, -1);
    init_pair(Color::TITLE,      tc.title,      -1);
    init_pair(Color::CHANNEL,    tc.channel,    -1);
    init_pair(Color::STATS,      tc.stats,      -1);
    init_pair(Color::SELECTED,   tc.selected,   -1);
    init_pair(Color::ACTION,     tc.action,     -1);
    init_pair(Color::ACTION_SEL, tc.action_sel, -1);
    init_pair(Color::STATUS,     tc.status,     -1);
    init_pair(Color::BORDER,     tc.border,     -1);
    init_pair(Color::HEADER,     tc.header,     -1);
    init_pair(Color::ACCENT,     tc.accent,     -1);
    init_pair(Color::TAG,        tc.tag,        -1);
    init_pair(Color::PUBLISHED,  tc.published,  -1);
    init_pair(Color::BOOKMARK,   tc.bookmark,   -1);
    init_pair(Color::DESC,       tc.desc,       -1);
}

void TUI::get_dimensions(int& w, int& h) { getmaxyx(stdscr, h, w); }

// ─── Top-level render ─────────────────────────────────────────────────────────
void TUI::render(const AppState& state, const Library* lib) {
    setup_colors(state);
    erase();
    draw_header(state);
    draw_search_box(state);
    draw_tabs(state);

    bool show_results = (state.active_tab == Tab::Results) && !state.results.empty();

    // ── Playlist panels ───────────────────────────────────────────────────────
    if (state.focus == Panel::PlaylistView || state.focus == Panel::PlaylistActions) {
        // Viewing videos inside a specific playlist
        if (state.actions_visible && state.focus == Panel::PlaylistActions)
            draw_playlist_actions_panel(state, lib);
        else
            draw_playlist_view(state, lib);
    } else if (state.active_tab == Tab::Playlists && state.focus != Panel::Results) {
        draw_playlists_tab(state, lib);
    // ── Results / actions ─────────────────────────────────────────────────────
    } else if (show_results && state.actions_visible) {
        draw_actions_panel(state);
    } else if (show_results) {
        draw_results_panel(state);
    } else {
        draw_home_panels(state, lib);
    }

    draw_message_bar(state);

    if (state.focus == Panel::BrowserPick)  draw_browser_popup(state);
    if (state.focus == Panel::SortMenu)     draw_sort_menu(state);
    if (state.focus == Panel::SavePrompt)   draw_save_prompt(state);
    if (state.focus == Panel::PlaylistPick) draw_playlist_picker(state);
    if (state.focus == Panel::NewPlaylist)  draw_new_playlist_prompt(state);
    refresh();
}

// ─── Header ───────────────────────────────────────────────────────────────────
void TUI::draw_header(const AppState& state) {
    const char* t = "Search YouTube";
    int tlen = (int)strlen(t);
    int x = std::max(0, (state.term_w - tlen) / 2);
    if (x + tlen > state.term_w) return;
    attron(COLOR_PAIR(Color::HEADER));
    mvprintw(0, x, "%s", t);
    attroff(COLOR_PAIR(Color::HEADER));
}

// ─── Search box ───────────────────────────────────────────────────────────────
void TUI::draw_search_box(const AppState& state) {
    int y = 1, w = state.term_w;
    if (w < 8) return;

    // Hamburger button: 5 cols wide on the right [ ☰ ]
    // Search box takes the rest
    const int HB_W = 5;
    int sb_w = w - HB_W;  // search box width
    bool f   = (state.focus == Panel::Search);
    bool hsf = (state.focus == Panel::SortMenu);

    // Search box
    draw_box(y, 0, 3, sb_w, f ? Color::BORDER : Color::BG);

    int iw = sb_w - 2;
    std::string d = utf8_truncate(state.search_query, iw);
    int dw = utf8_display_width(d);

    attron(COLOR_PAIR(Color::SEARCH_BOX));
    mvprintw(y + 1, 1, "%s", d.c_str());
    for (int i = dw; i < iw; i++) addch(' ');
    attroff(COLOR_PAIR(Color::SEARCH_BOX));

    if (f) mvchgat(y + 1, 1 + std::min(dw, iw - 1), 1, A_REVERSE, 0, nullptr);

    // Hamburger button [ ☰ ] — lights up when sort menu is open
    draw_box(y, sb_w, 3, HB_W, hsf ? Color::ACCENT : Color::BORDER);
    attron(COLOR_PAIR(hsf ? Color::ACCENT : Color::STATS) | A_BOLD);
    mvprintw(y + 1, sb_w + 1, "...");  // ☰ UTF-8: E2 98 B0
    attroff(COLOR_PAIR(hsf ? Color::ACCENT : Color::STATS) | A_BOLD);
}

// ─── Tabs ─────────────────────────────────────────────────────────────────────
void TUI::draw_tabs(const AppState& state) {
    if (state.term_w < 20) return;
    int y = 4;

    bool has_results = !state.results.empty();
    const char* lb[] = {"Library", "Playlists", "Feed", "History", "Results"};
    Tab         tb[] = {Tab::Library, Tab::Playlists, Tab::Feed, Tab::History, Tab::Results};
    int n_tabs = has_results ? 5 : 4;

    // Auto-fit tab width to terminal width
    int tw = std::max(8, std::min(14, (state.term_w - 2) / n_tabs - 2));
    int tot = tw * n_tabs + 2 * (n_tabs - 1);
    int sx = std::max(0, (state.term_w - tot) / 2);
    bool tf = (state.focus == Panel::Tabs || state.focus == Panel::PlaylistList);

    for (int i = 0; i < n_tabs; i++) {
        int tx = sx + i * (tw + 2);
        if (tx + tw > state.term_w) break;
        bool act = (state.active_tab == tb[i]);
        bool is_results_tab = (i == n_tabs - 1 && has_results);
        int bc = act ? Color::STATS : Color::BG;
        if (is_results_tab && !act) bc = Color::BORDER;
        if (tf && act) bc = Color::ACCENT;
        draw_box(y, tx, 3, tw, bc);
        int ll = (int)strlen(lb[i]);
        int lx = tx + (tw - ll) / 2;
        if (lx < 0 || lx + ll > state.term_w) continue;
        if (act) attron(COLOR_PAIR(Color::STATS) | A_BOLD);
        else if (is_results_tab) attron(COLOR_PAIR(Color::ACCENT));
        else attron(COLOR_PAIR(Color::BG));
        mvprintw(y + 1, lx, "%s", lb[i]);
        if (act) attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
        else if (is_results_tab) attroff(COLOR_PAIR(Color::ACCENT));
        else attroff(COLOR_PAIR(Color::BG));
    }
}

// ─── Home panels (Library / Feed / History tabs) ─────────────────────────────
void TUI::draw_home_panels(const AppState& state, const Library* lib) {
    int sy = 7, ey = state.term_h - 4, ah = ey - sy;
    if (ah < 3 || state.term_w < 4) return;
    int lw = std::max(10, state.term_w * 50 / 100);
    int rw = state.term_w - lw;
    if (rw < 4) { lw = state.term_w - 4; rw = 4; }

    if (!lib) {
        draw_box(sy, 0, ah + 1, lw, Color::BG);
        draw_box(sy, lw, ah + 1, rw, Color::BG);
        return;
    }

    int max_items = std::max(0, ah - 3); // rows available inside box

    // ── Library tab ──────────────────────────────────────────────────────────
    if (state.active_tab == Tab::Library) {
        draw_box(sy, 0, ah + 1, lw, Color::BG);
        draw_box(sy, lw, ah + 1, rw, Color::BG);

        if (sy + 1 < ey) {
            attron(COLOR_PAIR(Color::STATS) | A_BOLD);
            mvprintw(sy + 1, 2, "%s", utf8_truncate("Subscriptions", lw - 4).c_str());
            attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
        }

        // ── Koala hanging on the border between the two boxes ────────────────
        // Centered vertically in the panel, straddles column lw (the shared border).
        //   ʕ•ᴥ•ʔ style — arms wrap around the box edge
        {
            // Koala ASCII art — each line centered on lw
            // We print chars LEFT of lw and RIGHT of lw so it looks like
            // it's clinging to the border post.
            const char* koala[] = {
                " /\\ /\\",  // ears
                "(^\u1D25^ )", // face (ᴥ = U+1D25)
                "/|   |\\", // arms hugging the border
                " \\ v /",  // legs
                "  ^ ^",    // lil feet
            };
            // Position: horizontally straddle lw, vertically center in panel
            int kh = 5;
            int ky = sy + std::max(1, (ah - kh) / 2);
            int kx = lw - 3; // center the 6-char art on the border

            for (int i = 0; i < kh; i++) {
                int row = ky + i;
                if (row >= ey || row < sy + 1) continue;
                if (kx < 0 || kx + 7 > state.term_w) continue;
                attron(COLOR_PAIR(Color::ACCENT) | A_BOLD);
                mvprintw(row, kx, "%s", koala[i]);
                attroff(COLOR_PAIR(Color::ACCENT) | A_BOLD);
            }
        }

        auto subs = lib->subscriptions();
        if (subs.empty()) {
            if (sy + 3 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); mvprintw(sy + 3, 3, "%s", utf8_truncate("No subscriptions yet (._.)  ", lw - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
            if (sy + 4 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); mvprintw(sy + 4, 3, "%s", utf8_truncate("Subscribe from action menu!", lw - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
        } else {
            for (int i = 0; i < (int)subs.size() && i < max_items; i++) {
                int row = sy + 2 + i;
                if (row >= ey) break;
                attron(COLOR_PAIR(Color::CHANNEL));
                mvprintw(row, 3, "%s", utf8_truncate(subs[i].title, lw - 5).c_str());
                attroff(COLOR_PAIR(Color::CHANNEL));
            }
        }

        if (sy + 1 < ey) {
            attron(COLOR_PAIR(Color::STATS) | A_BOLD);
            mvprintw(sy + 1, lw + 2, "%s", utf8_truncate("Saved Videos", rw - 4).c_str());
            attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
        }

        auto vids = lib->saved_videos();
        if (vids.empty()) {
            if (sy + 3 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); mvprintw(sy + 3, lw + 3, "%s", utf8_truncate("No saved videos yet :3", rw - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
            if (sy + 4 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); mvprintw(sy + 4, lw + 3, "%s", utf8_truncate("Bookmark from action menu!", rw - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
        } else {
            for (int i = 0; i < (int)vids.size() && i < max_items; i++) {
                int row = sy + 2 + i;
                if (row >= ey) break;
                attron(COLOR_PAIR(Color::TITLE));
                mvprintw(row, lw + 3, "%s", utf8_truncate(vids[i].title, rw - 5).c_str());
                attroff(COLOR_PAIR(Color::TITLE));
            }
        }

    // ── History tab ──────────────────────────────────────────────────────────
    } else if (state.active_tab == Tab::History) {
        draw_box(sy, 0, ah + 1, state.term_w, Color::BG);

        if (sy + 1 < ey) {
            attron(COLOR_PAIR(Color::STATS) | A_BOLD);
            mvprintw(sy + 1, 2, "%s", utf8_truncate("Watch History", state.term_w - 4).c_str());
            attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
        }

        auto hist = lib->history();
        if (hist.empty()) {
            if (sy + 3 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); mvprintw(sy + 3, 3, "%s", utf8_truncate("Nothing watched yet ('v')", state.term_w - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
            if (sy + 4 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); mvprintw(sy + 4, 3, "%s", utf8_truncate("Play something and it'll show up here!", state.term_w - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
        } else {
            int full_w = state.term_w - 4;
            int title_w = full_w * 60 / 100;
            int chan_w  = std::max(0, full_w - title_w - 2);

            if (sy + 2 < ey) {
                attron(COLOR_PAIR(Color::BG) | A_DIM);
                mvprintw(sy + 2, 3, "%s", utf8_truncate("Title", title_w).c_str());
                if (chan_w > 0)
                    mvprintw(sy + 2, 3 + title_w + 2, "%s", utf8_truncate("Channel", chan_w).c_str());
                attroff(COLOR_PAIR(Color::BG) | A_DIM);
            }

            int count = std::min((int)hist.size(), max_items - 2);
            for (int i = 0; i < count; i++) {
                int idx = (int)hist.size() - 1 - i;
                int row = sy + 3 + i;
                if (row >= ey) break;
                bool sel = (i == state.home_selected_idx) && (state.focus == Panel::Tabs);
                if (sel) {
                    attron(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
                    mvprintw(row, 2, " %-*s", title_w - 1, utf8_truncate(hist[idx].title, title_w - 2).c_str());
                    if (chan_w > 0)
                        printw("  %-*s ", chan_w - 2, utf8_truncate(hist[idx].channel, chan_w - 3).c_str());
                    attroff(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
                } else {
                    attron(COLOR_PAIR(Color::TITLE));
                    mvprintw(row, 3, "%s", utf8_truncate(hist[idx].title, title_w).c_str());
                    attroff(COLOR_PAIR(Color::TITLE));
                    if (chan_w > 0 && !hist[idx].channel.empty()) {
                        attron(COLOR_PAIR(Color::CHANNEL) | A_DIM);
                        mvprintw(row, 3 + title_w + 2, "%s", utf8_truncate(hist[idx].channel, chan_w).c_str());
                        attroff(COLOR_PAIR(Color::CHANNEL) | A_DIM);
                    }
                }
            }
            // Hint
            if (ey - 1 > sy + 2) {
                attron(COLOR_PAIR(Color::BG) | A_DIM);
                mvprintw(ey - 1, 2, "j/k=navigate  Enter/click=search & go to results");
                attroff(COLOR_PAIR(Color::BG) | A_DIM);
            }
        }
        return;

    // ── Feed tab ─────────────────────────────────────────────────────────────
    } else {
        auto subs = lib->subscriptions();
        auto hist = lib->history();

        if (subs.empty() && hist.empty()) {
            draw_box(sy, 0, ah + 1, state.term_w, Color::BG);
            int cy = std::max(sy + 1, sy + ah / 2 - 2);
            const char* art[] = { "        .__.", "       (o.o)", "       |   |", "       (___)" };
            for (int i = 0; i < 4 && cy + i < ey; i++) {
                int ax = std::max(0, (state.term_w - 16) / 2);
                if (ax + 16 <= state.term_w) {
                    attron(COLOR_PAIR(Color::BG) | A_DIM);
                    mvprintw(cy + i, ax, "%s", art[i]);
                    attroff(COLOR_PAIR(Color::BG) | A_DIM);
                }
            }
        } else {
            draw_box(sy, 0, ah + 1, lw, Color::BG);
            draw_box(sy, lw, ah + 1, rw, Color::BG);

            if (sy + 1 < ey) {
                attron(COLOR_PAIR(Color::STATS) | A_BOLD);
                mvprintw(sy + 1, 2, "%s", utf8_truncate("Recently Played", lw - 4).c_str());
                attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
            }

            if (hist.empty()) {
                if (sy + 3 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); mvprintw(sy + 3, 3, "%s", utf8_truncate("Nothing played yet ('v')", lw - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
            } else {
                int count = std::min((int)hist.size(), max_items / 2);
                for (int i = 0; i < count; i++) {
                    int idx = (int)hist.size() - 1 - i;
                    int row = sy + 2 + i * 2;
                    if (row + 1 >= ey) break;
                    bool sel = (i == state.home_selected_idx) && (state.focus == Panel::Tabs);
                    if (sel) {
                        attron(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
                        mvprintw(row,     2, " %-*s ", lw - 5, utf8_truncate(hist[idx].title, lw - 7).c_str());
                        if (!hist[idx].channel.empty() && row + 1 < ey)
                            mvprintw(row + 1, 2, " %-*s ", lw - 5, utf8_truncate("  " + hist[idx].channel, lw - 7).c_str());
                        attroff(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
                    } else {
                        attron(COLOR_PAIR(Color::TITLE));
                        mvprintw(row, 3, "%s", utf8_truncate(hist[idx].title, lw - 5).c_str());
                        attroff(COLOR_PAIR(Color::TITLE));
                        if (!hist[idx].channel.empty() && row + 1 < ey) {
                            attron(COLOR_PAIR(Color::CHANNEL) | A_DIM);
                            mvprintw(row + 1, 5, "%s", utf8_truncate(hist[idx].channel, lw - 7).c_str());
                            attroff(COLOR_PAIR(Color::CHANNEL) | A_DIM);
                        }
                    }
                }
            }

            if (sy + 1 < ey) {
                attron(COLOR_PAIR(Color::STATS) | A_BOLD);
                mvprintw(sy + 1, lw + 2, "%s", utf8_truncate("Subscribed Channels", rw - 4).c_str());
                attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
            }

            if (subs.empty()) {
                if (sy + 3 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); mvprintw(sy + 3, lw + 3, "%s", utf8_truncate("No subscriptions yet", rw - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
            } else {
                for (int i = 0; i < (int)subs.size() && i < max_items; i++) {
                    int row = sy + 2 + i;
                    if (row >= ey) break;
                    attron(COLOR_PAIR(Color::CHANNEL));
                    mvprintw(row, lw + 3, "%s", utf8_truncate(subs[i].title, rw - 5).c_str());
                    attroff(COLOR_PAIR(Color::CHANNEL));
                }
            }
        }

        // Summary footer
        int iy = ey - 1;
        if (iy > sy + 2) {
            std::string info = std::to_string(subs.size()) + " subscriptions | " +
                               std::to_string(lib->saved_videos().size()) + " saved | " +
                               std::to_string(hist.size()) + " watched";
            info = utf8_truncate(info, state.term_w - 4);
            int ix = std::max(0, (state.term_w - utf8_display_width(info)) / 2);
            attron(COLOR_PAIR(Color::STATS) | A_DIM);
            mvprintw(iy, ix, "%s", info.c_str());
            attroff(COLOR_PAIR(Color::STATS) | A_DIM);
        }
    }
}

// ─── Results panel ────────────────────────────────────────────────────────────
void TUI::draw_results_panel(const AppState& state) {
    int sy = 7, ey = state.term_h - 4, ah = ey - sy;
    if (ah < 3 || state.term_w < 4) return;

    int lw = state.term_w * 60 / 100;
    if (lw < 20) lw = 20;
    if (lw > state.term_w - 4) lw = state.term_w - 4;
    int rw = state.term_w - lw;

    bool f = (state.focus == Panel::Results);
    draw_box(sy, 0, ah + 1, lw, f ? Color::BORDER : Color::BG);

    int iy = sy + 1;
    int iw = std::max(0, lw - 2);
    int mv = ah - 1;

    for (int i = 0; i < mv && (i + state.results_scroll) < (int)state.results.size(); i++) {
        int idx = i + state.results_scroll;
        int y   = iy + i;
        if (y >= ey) break;

        bool sel = (idx == state.selected_result);
        std::string title = utf8_truncate(state.results[idx].title, iw);
        int tw = utf8_display_width(title);

        if (sel) {
            attron(COLOR_PAIR(Color::BG) | A_REVERSE);
            mvprintw(y, 1, "%s", title.c_str());
            for (int j = tw; j < iw; j++) addch(' ');
            attroff(COLOR_PAIR(Color::BG) | A_REVERSE);
        } else {
            attron(COLOR_PAIR(Color::BG));
            mvprintw(y, 1, "%s", title.c_str());
            attroff(COLOR_PAIR(Color::BG));
        }
    }

    if (rw > 4)
        draw_info_panel(state, lw, sy, rw, ah + 1, true);
}

// ─── Actions panel ────────────────────────────────────────────────────────────
void TUI::draw_actions_panel(const AppState& state) {
    if (state.results.empty()) return;
    int sy = 7, ey = state.term_h - 4, ah = ey - sy;
    if (ah < 3 || state.term_w < 4) return;

    int lw = state.term_w * 30 / 100;
    if (lw < 15) lw = 15;
    if (lw > state.term_w - 4) lw = state.term_w - 4;
    int rw = state.term_w - lw;

    if (lw > 4) draw_info_panel(state, 0, sy, lw, ah + 1, true);

    bool f = (state.focus == Panel::Actions);
    draw_box(sy, lw, ah + 1, rw, f ? Color::BORDER : Color::BG);

    int iy = sy + 1;
    int iw = std::max(0, rw - 2);

    for (int i = 0; i < (int)state.actions.size() && i < ah - 1; i++) {
        int y = iy + i;
        if (y >= ey) break;

        bool sel = (i == state.selected_action);
        std::string label = utf8_truncate(state.actions[i].label, iw - 2);
        int lw2 = utf8_display_width(label);

        if (sel && f) {
            attron(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
            mvprintw(y, lw + 1, " %s", label.c_str());
            for (int j = lw2 + 1; j < iw; j++) addch(' ');
            attroff(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
        } else {
            attron(COLOR_PAIR(Color::BG));
            mvprintw(y, lw + 2, "%s", label.c_str());
            attroff(COLOR_PAIR(Color::BG));
        }
    }
}

// ─── Info panel (details + thumbnail) ────────────────────────────────────────
void TUI::draw_info_panel(const AppState& state, int px, int py, int pw, int ph, bool with_thumb) {
    if (state.results.empty() || state.selected_result >= (int)state.results.size()) return;
    if (pw <= 4 || ph <= 2) return;

    const auto& v = state.results[state.selected_result];
    draw_box(py, px, ph, pw, Color::BORDER);

    int y   = py + 1;
    int x   = px + 1;
    int w   = std::max(0, pw - 2);
    int bot = py + ph - 1;

    // Thumbnail — rendered through ncurses as plain UTF-8 halfblock characters.
    // chafa --colors=none emits zero ANSI escape sequences, so mvaddstr() handles
    // it correctly. ncurses owns this region: erase() clears it, highlighting and
    // scrolling work normally. No ghosting, no persistence, no z-order bugs.
    if (with_thumb && state.thumbs_available && !v.id.empty() && w > 4) {
        int tc = std::min(w, 60);
        int tr = std::max(2, tc * 9 / 32);
        if (tr > (bot - y) / 3) tr = std::max(2, (bot - y) / 3);

        if (Thumbnails::is_cached(v.id)) {
            draw_thumb(v.id, x, y, tc, tr, bot);
            y += tr;
            if (y < bot) y++; // blank separator row
        } else {
            if (y < bot) {
                attron(COLOR_PAIR(Color::BG) | A_DIM);
                mvaddstr(y, x, "[loading thumbnail...]");
                attroff(COLOR_PAIR(Color::BG) | A_DIM);
                y++;
            }
        }
    }

    // Helper: print one line and advance y
    auto line = [&](int cp, const std::string& text) {
        if (y >= bot || w <= 0) return;
        attron(COLOR_PAIR(cp));
        mvprintw(y++, x, "%s", utf8_truncate(text, w).c_str());
        attroff(COLOR_PAIR(cp));
    };

    line(Color::TAG, v.is_live ? "[LIVE]" : "[Video]");

    // Title — word-wrap across multiple lines without chopping chars
    if (y < bot) {
        attron(COLOR_PAIR(Color::TITLE));
        std::string rem = v.title;
        if (rem.size() > 500) rem.resize(500); // sanity cap
        int safety = 0;
        while (!rem.empty() && y < bot && safety++ < 20) {
            int rw_disp = utf8_display_width(rem);
            if (rw_disp <= w) {
                // Entire remainder fits
                mvprintw(y++, x, "%s", rem.c_str());
                break;
            }
            // Print one row's worth without the "..." — just wrap cleanly
            std::string chunk;
            {
                const char* p = rem.c_str(), *end = p + rem.size();
                int cols = 0; mbstate_t mbs{};
                while (p < end) {
                    wchar_t wc; size_t n = mbrtowc(&wc, p, end - p, &mbs);
                    if (n == (size_t)-1 || n == (size_t)-2) { p++; cols++; continue; }
                    if (n == 0) break;
                    int cw = wcwidth(wc); if (cw < 0) cw = 1;
                    if (cols + cw > w) break;
                    chunk.append(p, n); cols += cw; p += n;
                }
                rem = std::string(p, end);
            }
            if (chunk.empty()) break; // safety: prevent infinite loop
            mvprintw(y++, x, "%s", chunk.c_str());
        }
        attroff(COLOR_PAIR(Color::TITLE));
    }

    if (!v.view_count.empty()) line(Color::STATS, v.view_count + " views");
    if (!v.duration.empty())   line(Color::TITLE, "Length: " + v.duration);
    if (!v.channel.empty())    line(Color::CHANNEL, "Uploaded by " + v.channel);
    if (!v.upload_date.empty()) line(Color::PUBLISHED, "Published " + v.upload_date);
}

// ─── Message bar ──────────────────────────────────────────────────────────────
void TUI::draw_message_bar(const AppState& state) {
    int y = state.term_h - 3, w = state.term_w;
    if (y < 0 || w < 4) return;
    int bc = state.is_playing ? (state.is_paused ? Color::STATS : Color::ACCENT) : Color::BG;
    draw_box(y, 0, 3, w, bc);

    std::string msg;
    if (state.is_playing && !state.now_playing.empty()) {
        msg = state.is_paused
            ? "[Paused]: " + state.now_playing + "  (p to resume)"
            : "[Now Playing]: " + state.now_playing + "  (p to pause)";
    } else if (!state.status_message.empty() && state.status_message.size() >= 2
               && state.status_message.substr(0, 2) != "__") {
        msg = state.status_message;
    }

    if (!msg.empty()) {
        attron(COLOR_PAIR(Color::BG));
        mvprintw(y + 1, 1, "%s", utf8_truncate(msg, w - 2).c_str());
        attroff(COLOR_PAIR(Color::BG));
    }
}

// ─── Sort menu popup ──────────────────────────────────────────────────────────
void TUI::draw_sort_menu(const AppState& state) {
    int pw = std::min(56, state.term_w - 4), ph = 10;
    if (pw < 10 || ph > state.term_h - 2) return;
    int px = std::max(0, (state.term_w - pw) / 2);
    int py = std::max(0, (state.term_h - ph) / 2);

    for (int i = 0; i < ph && py + i < state.term_h; i++) mvhline(py + i, px, ' ', pw);
    draw_box(py, px, ph, pw, Color::BORDER);

    int half = pw / 2;
    draw_box(py, px, ph, half, state.sort_col == 0 ? Color::ACCENT : Color::BORDER);
    attron(COLOR_PAIR(Color::ACCENT) | A_BOLD);
    mvprintw(py + 1, px + 2, "Sort by");
    attroff(COLOR_PAIR(Color::ACCENT) | A_BOLD);

    const char* sorts[]   = {"Relevance", "Upload date", "View count", "Rating"};
    const char* filters[] = {"All", "Videos", "Channels", "Playlists", "Short", "Long"};

    for (int i = 0; i < 4; i++) {
        bool sel = (state.sort_col == 0 && state.sort_row == i);
        if (sel) attron(A_REVERSE | COLOR_PAIR(Color::ACCENT));
        else     attron(COLOR_PAIR(Color::BG));
        mvprintw(py + 2 + i, px + 2, "%-*s", half - 4, utf8_truncate(sorts[i], half - 4).c_str());
        if (sel) attroff(A_REVERSE | COLOR_PAIR(Color::ACCENT));
        else     attroff(COLOR_PAIR(Color::BG));
    }

    draw_box(py, px + half, ph, half, state.sort_col == 1 ? Color::ACCENT : Color::BORDER);
    attron(COLOR_PAIR(Color::ACCENT) | A_BOLD);
    mvprintw(py + 1, px + half + 2, "Filter");
    attroff(COLOR_PAIR(Color::ACCENT) | A_BOLD);

    for (int i = 0; i < 6 && i < ph - 3; i++) {
        bool sel = (state.sort_col == 1 && state.sort_row == i);
        if (sel) attron(A_REVERSE | COLOR_PAIR(Color::ACCENT));
        else     attron(COLOR_PAIR(Color::BG));
        mvprintw(py + 2 + i, px + half + 2, "%-*s", half - 4, utf8_truncate(filters[i], half - 4).c_str());
        if (sel) attroff(A_REVERSE | COLOR_PAIR(Color::ACCENT));
        else     attroff(COLOR_PAIR(Color::BG));
    }

}

// ─── Save prompt popup ────────────────────────────────────────────────────────
void TUI::draw_save_prompt(const AppState& state) {
    int pw = std::min(44, state.term_w - 4), ph = 7;
    if (pw < 10 || ph > state.term_h - 2) return;
    int px = std::max(0, (state.term_w - pw) / 2);
    int py = std::max(0, (state.term_h - ph) / 2);

    for (int i = 0; i < ph && py + i < state.term_h; i++) mvhline(py + i, px, ' ', pw);
    draw_box(py, px, ph, pw, Color::STATS);

    attron(COLOR_PAIR(Color::STATS) | A_BOLD);
    mvprintw(py, px + std::max(0, (pw - 16) / 2), " Save to Library ");
    attroff(COLOR_PAIR(Color::STATS) | A_BOLD);

    const char* opts[] = {"Bookmark only (web shortcut)", "Download video", "Download audio (mp3)"};
    for (int i = 0; i < 3; i++) {
        int oy = py + 2 + i;
        if (oy >= state.term_h) break;
        if (i == state.save_prompt_idx) {
            attron(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
            mvprintw(oy, px + 2, " %-*s", pw - 5, utf8_truncate(opts[i], pw - 6).c_str());
            attroff(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
        } else {
            attron(COLOR_PAIR(Color::BG));
            mvprintw(oy, px + 3, "%s", utf8_truncate(opts[i], pw - 6).c_str());
            attroff(COLOR_PAIR(Color::BG));
        }
    }
    attron(COLOR_PAIR(Color::BG) | A_DIM);
    mvprintw(py + ph - 1, px + 2, "%s", utf8_truncate(" Enter=select  Esc=cancel ", pw - 4).c_str());
    attroff(COLOR_PAIR(Color::BG) | A_DIM);
}

// ─── Browser picker popup ─────────────────────────────────────────────────────
void TUI::draw_browser_popup(const AppState& state) {
    if (state.browser_choices.empty()) return;
    int pw = std::min(40, state.term_w - 4);
    int ph = (int)state.browser_choices.size() + 4;
    if (pw < 10 || ph > state.term_h - 2) return;
    int px = std::max(0, (state.term_w - pw) / 2);
    int py = std::max(0, (state.term_h - ph) / 2);

    for (int i = 0; i < ph && py + i < state.term_h; i++) mvhline(py + i, px, ' ', pw);
    draw_box(py, px, ph, pw, Color::STATS);

    attron(COLOR_PAIR(Color::STATS) | A_BOLD);
    mvprintw(py, px + std::max(0, (pw - 16) / 2), " Select Browser ");
    attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
    attron(COLOR_PAIR(Color::BG) | A_DIM);
    mvprintw(py + 1, px + 2, "%s", utf8_truncate("Pick your YouTube browser:", pw - 4).c_str());
    attroff(COLOR_PAIR(Color::BG) | A_DIM);

    for (int i = 0; i < (int)state.browser_choices.size(); i++) {
        int row = py + 2 + i;
        if (row >= state.term_h) break;
        if (i == state.browser_pick_idx) {
            attron(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
            mvprintw(row, px + 2, " %-*s", pw - 5, utf8_truncate(state.browser_choices[i], pw - 6).c_str());
            attroff(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
        } else {
            attron(COLOR_PAIR(Color::BG));
            mvprintw(row, px + 3, "%s", utf8_truncate(state.browser_choices[i], pw - 6).c_str());
            attroff(COLOR_PAIR(Color::BG));
        }
    }
    attron(COLOR_PAIR(Color::BG) | A_DIM);
    mvprintw(py + ph - 1, px + 2, "%s", utf8_truncate(" Enter=select  Esc=cancel ", pw - 4).c_str());
    attroff(COLOR_PAIR(Color::BG) | A_DIM);
}

// ─── Box drawing ──────────────────────────────────────────────────────────────
void TUI::draw_box(int y, int x, int h, int w, int cp) {
    if (w < 2 || h < 2 || x < 0 || y < 0) return;
    attron(COLOR_PAIR(cp));
    mvaddch(y, x, ACS_ULCORNER);
    for (int i = 1; i < w - 1; i++) mvaddch(y, x + i, ACS_HLINE);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    for (int i = 1; i < h - 1; i++) {
        mvaddch(y + i, x,         ACS_VLINE);
        mvaddch(y + i, x + w - 1, ACS_VLINE);
    }
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    for (int i = 1; i < w - 1; i++) mvaddch(y + h - 1, x + i, ACS_HLINE);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    attroff(COLOR_PAIR(cp));
}

// ─── Playlists tab — list of all playlists ────────────────────────────────────
void TUI::draw_playlists_tab(const AppState& state, const Library* lib) {
    int sy = 7, ey = state.term_h - 4, ah = ey - sy;
    if (ah < 3 || state.term_w < 4 || !lib) return;

    const auto& pls = lib->playlists();
    draw_box(sy, 0, ah + 1, state.term_w, Color::BG);

    bool focused = (state.focus == Panel::PlaylistList);

    // Header row
    if (sy + 1 < ey) {
        attron(COLOR_PAIR(Color::STATS) | A_BOLD);
        mvprintw(sy + 1, 2, "Playlists (%d)", (int)pls.size());
        attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
        if (sy + 1 < ey) {
            attron(COLOR_PAIR(Color::BG) | A_DIM);
            mvprintw(sy + 1, state.term_w - 18, "[n] new playlist");
            attroff(COLOR_PAIR(Color::BG) | A_DIM);
        }
    }

    if (pls.empty()) {
        if (sy + 3 < ey) {
            attron(COLOR_PAIR(Color::BG) | A_DIM);
            mvprintw(sy + 3, 4, "No playlists yet (._.)");;
            mvprintw(sy + 4, 4, "Search for a video, open its actions, and choose \"Add to playlist\".");
            attroff(COLOR_PAIR(Color::BG) | A_DIM);
        }
        // Cute art centered in the empty space
        int cy = sy + std::max(6, ah / 2 - 2);
        const char* art[] = { "  \xe2\x99\xaa\xe2\x99\xab\xe2\x99\xaa", "  [===]", "  |   |", "  '---'" };
        for (int i = 0; i < 4 && cy + i < ey; i++) {
            int ax = std::max(0, (state.term_w - 10) / 2);
            attron(COLOR_PAIR(Color::ACCENT) | A_DIM);
            mvprintw(cy + i, ax, "%s", art[i]);
            attroff(COLOR_PAIR(Color::ACCENT) | A_DIM);
        }
        return;
    }

    // Column widths
    int fw = state.term_w - 4;
    int name_w  = fw * 65 / 100;
    int count_w = std::max(0, fw - name_w - 2);

    // Column headers
    if (sy + 2 < ey) {
        attron(COLOR_PAIR(Color::BG) | A_DIM);
        mvprintw(sy + 2, 3, "%-*s", name_w, "Name");
        if (count_w > 0) mvprintw(sy + 2, 3 + name_w + 2, "Videos");
        attroff(COLOR_PAIR(Color::BG) | A_DIM);
    }

    int max_items = std::max(0, ah - 3);
    int n = (int)pls.size();
    // Clamp scroll
    int scroll = std::max(0, std::min(state.playlist_scroll, n - max_items));

    for (int i = 0; i < max_items && (i + scroll) < n; i++) {
        int row = sy + 3 + i;
        if (row >= ey) break;
        int idx = i + scroll;
        bool sel = (idx == state.selected_playlist) && focused;
        const auto& pl = pls[idx];

        if (sel) {
            attron(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
            mvprintw(row, 2, " %-*s", name_w - 1, utf8_truncate(pl.name, name_w - 2).c_str());
            if (count_w > 0) printw("  %*d ", count_w - 3, (int)pl.videos.size());
            attroff(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
        } else {
            attron(COLOR_PAIR(Color::TITLE));
            mvprintw(row, 3, "%s", utf8_truncate(pl.name, name_w).c_str());
            attroff(COLOR_PAIR(Color::TITLE));
            if (count_w > 0) {
                attron(COLOR_PAIR(Color::STATS) | A_DIM);
                mvprintw(row, 3 + name_w + 2, "%d", (int)pl.videos.size());
                attroff(COLOR_PAIR(Color::STATS) | A_DIM);
            }
        }
    }

    // Hint bar
    if (ey - 1 > sy + 2) {
        attron(COLOR_PAIR(Color::BG) | A_DIM);
        mvprintw(ey - 1, 2, "Enter=open  j/k=navigate  n=new");
        attroff(COLOR_PAIR(Color::BG) | A_DIM);
    }
}

// ─── Playlist view — video list inside a playlist ────────────────────────────
void TUI::draw_playlist_view(const AppState& state, const Library* lib) {
    int sy = 7, ey = state.term_h - 4, ah = ey - sy;
    if (ah < 3 || state.term_w < 4 || !lib) return;

    const Playlist* pl = lib->get_playlist(state.current_playlist_id);
    if (!pl) return;

    int lw = state.term_w * 60 / 100;
    if (lw < 20) lw = 20;
    if (lw > state.term_w - 4) lw = state.term_w - 4;
    int rw = state.term_w - lw;

    bool focused = (state.focus == Panel::PlaylistView);
    draw_box(sy, 0, ah + 1, lw, focused ? Color::BORDER : Color::BG);

    // Header: playlist name + count
    if (sy + 1 < ey) {
        attron(COLOR_PAIR(Color::STATS) | A_BOLD);
        std::string hdr = pl->name + " (" + std::to_string(pl->videos.size()) + ")";
        mvprintw(sy + 1, 2, "%s", utf8_truncate(hdr, lw - 4).c_str());
        attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
    }

    if (pl->videos.empty()) {
        if (sy + 3 < ey) {
            attron(COLOR_PAIR(Color::BG) | A_DIM);
            mvprintw(sy + 3, 3, "%s", utf8_truncate("No videos yet (._.)", lw - 6).c_str());
            attroff(COLOR_PAIR(Color::BG) | A_DIM);
        }
    } else {
        int iw  = std::max(0, lw - 2);
        int mv  = ah - 2;
        int total = (int)pl->videos.size();
        int scroll = std::clamp(state.playlist_video_scroll, 0, std::max(0, total - mv));

        for (int i = 0; i < mv && (i + scroll) < total; i++) {
            int idx = i + scroll;
            int row = sy + 2 + i;
            if (row >= ey) break;
            bool sel = (idx == state.playlist_video_idx) && focused;
            std::string title = utf8_truncate(pl->videos[idx].title, iw);
            int tw2 = utf8_display_width(title);

            if (sel) {
                attron(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
                mvprintw(row, 1, " %s", title.c_str());
                for (int j = tw2 + 1; j < iw; j++) addch(' ');
                attroff(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
            } else {
                attron(COLOR_PAIR(Color::TITLE));
                mvprintw(row, 2, "%s", title.c_str());
                attroff(COLOR_PAIR(Color::TITLE));
            }
        }
    }

    // Right panel: info for selected video
    if (rw > 4 && !pl->videos.empty() &&
        state.playlist_video_idx < (int)pl->videos.size()) {
        draw_box(sy, lw, ah + 1, rw, Color::BORDER);
        const auto& v = pl->videos[state.playlist_video_idx];
        int x = lw + 1, w2 = std::max(0, rw - 2);
        int y = sy + 1, bot = sy + ah;

        // Thumbnail (same fixed approach as results panel — addstr, no ANSI)
        if (state.thumbs_available && Thumbnails::is_cached(v.id) && w2 > 4) {
            int tc = std::min(w2, 50);
            int tr = std::max(2, tc * 9 / 32);
            if (tr > (bot - y) / 3) tr = std::max(2, (bot - y) / 3);
            draw_thumb(v.id, x, y, tc, tr, bot);
            y += tr;
            if (y < bot) y++;
        }

        if (y < bot) {
            attron(COLOR_PAIR(Color::TITLE) | A_BOLD);
            mvprintw(y++, x, "%s", utf8_truncate(v.title, w2).c_str());
            attroff(COLOR_PAIR(Color::TITLE) | A_BOLD);
        }
        if (y < bot && !v.channel.empty()) {
            attron(COLOR_PAIR(Color::CHANNEL));
            mvprintw(y++, x, "%s", utf8_truncate(v.channel, w2).c_str());
            attroff(COLOR_PAIR(Color::CHANNEL));
        }
        if (y < bot && v.duration_seconds > 0) {
            attron(COLOR_PAIR(Color::STATS));
            mvprintw(y++, x, "%d:%02d", v.duration_seconds / 60, v.duration_seconds % 60);
            attroff(COLOR_PAIR(Color::STATS));
        }
        if (bot - 1 > y) {
            attron(COLOR_PAIR(Color::BG) | A_DIM);
            mvprintw(bot - 1, x, "%s", utf8_truncate("Enter=actions  Esc=back", w2).c_str());
            attroff(COLOR_PAIR(Color::BG) | A_DIM);
        }
    }
}

// ─── Playlist action panel ────────────────────────────────────────────────────
void TUI::draw_playlist_actions_panel(const AppState& state, const Library* lib) {
    if (!lib) return;
    const Playlist* pl = lib->get_playlist(state.current_playlist_id);
    if (!pl || pl->videos.empty()) return;

    int sy = 7, ey = state.term_h - 4, ah = ey - sy;
    if (ah < 3 || state.term_w < 4) return;

    int lw = state.term_w * 30 / 100;
    if (lw < 15) lw = 15;
    if (lw > state.term_w - 4) lw = state.term_w - 4;
    int rw = state.term_w - lw;

    // Left: info panel for selected video
    draw_box(sy, 0, ah + 1, lw, Color::BORDER);
    if (state.playlist_video_idx < (int)pl->videos.size()) {
        const auto& v = pl->videos[state.playlist_video_idx];
        int x = 1, w2 = std::max(0, lw - 2);
        int y = sy + 1, bot = sy + ah;

        if (state.thumbs_available && Thumbnails::is_cached(v.id) && w2 > 4) {
            int tc = std::min(w2, 40);
            int tr = std::max(2, tc * 9 / 32);
            if (tr > (bot - y) / 2) tr = std::max(2, (bot - y) / 2);
            draw_thumb(v.id, x, y, tc, tr, bot);
            y += tr;
            if (y < bot) y++;
        }
        if (y < bot) {
            attron(COLOR_PAIR(Color::TITLE) | A_BOLD);
            mvprintw(y++, x, "%s", utf8_truncate(v.title, w2).c_str());
            attroff(COLOR_PAIR(Color::TITLE) | A_BOLD);
        }
        if (y < bot && !v.channel.empty()) {
            attron(COLOR_PAIR(Color::CHANNEL));
            mvprintw(y++, x, "%s", utf8_truncate(v.channel, w2).c_str());
            attroff(COLOR_PAIR(Color::CHANNEL));
        }
    }

    // Right: action list
    bool f = (state.focus == Panel::PlaylistActions);
    draw_box(sy, lw, ah + 1, rw, f ? Color::BORDER : Color::BG);
    int iw2 = std::max(0, rw - 2);
    for (int i = 0; i < (int)state.actions.size() && i < ah - 1; i++) {
        int row = sy + 1 + i;
        if (row >= ey) break;
        bool sel = (i == state.selected_action) && f;
        std::string lbl = utf8_truncate(state.actions[i].label, iw2 - 2);
        int llen = utf8_display_width(lbl);
        if (sel) {
            attron(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
            mvprintw(row, lw + 1, " %s", lbl.c_str());
            for (int j = llen + 1; j < iw2; j++) addch(' ');
            attroff(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
        } else {
            attron(COLOR_PAIR(Color::BG));
            mvprintw(row, lw + 2, "%s", lbl.c_str());
            attroff(COLOR_PAIR(Color::BG));
        }
    }
}

// ─── Playlist picker popup — "Add to playlist" ───────────────────────────────
void TUI::draw_playlist_picker(const AppState& state) {
    int pw = std::min(52, state.term_w - 4);
    int ph = std::min(16, state.term_h - 6);
    if (pw < 22 || ph < 5) return;

    int px = (state.term_w - pw) / 2;
    int py = (state.term_h - ph) / 2;

    // Clear background
    for (int i = 0; i < ph; i++) mvhline(py + i, px, ' ', pw);
    draw_box(py, px, ph, pw, Color::STATS);

    attron(COLOR_PAIR(Color::STATS) | A_BOLD);
    std::string title = " Add to Playlist ";
    mvprintw(py, px + std::max(0, (pw - (int)title.size()) / 2), "%s", title.c_str());
    attroff(COLOR_PAIR(Color::STATS) | A_BOLD);

    int max_items = ph - 3;
    for (int i = 0; i < (int)state.playlist_names.size() && i < max_items; i++) {
        int row = py + 1 + i;
        if (row >= state.term_h) break;
        bool sel = (i == state.playlist_pick_idx);
        bool is_create = (i == 0);

        if (sel) {
            attron(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
            mvprintw(row, px + 1, " %-*s ", pw - 4, utf8_truncate(state.playlist_names[i], pw - 5).c_str());
            attroff(COLOR_PAIR(Color::ACCENT) | A_REVERSE);
        } else if (is_create) {
            attron(COLOR_PAIR(Color::ACCENT));
            mvprintw(row, px + 2, "%s", utf8_truncate(state.playlist_names[i], pw - 4).c_str());
            attroff(COLOR_PAIR(Color::ACCENT));
        } else {
            attron(COLOR_PAIR(Color::BG));
            mvprintw(row, px + 2, "%s", utf8_truncate(state.playlist_names[i], pw - 4).c_str());
            attroff(COLOR_PAIR(Color::BG));
        }
    }

    attron(COLOR_PAIR(Color::BG) | A_DIM);
    mvprintw(py + ph - 1, px + 2, "Enter=select  Esc=cancel");
    attroff(COLOR_PAIR(Color::BG) | A_DIM);
}

// ─── New playlist name dialog ─────────────────────────────────────────────────
void TUI::draw_new_playlist_prompt(const AppState& state) {
    int pw = std::min(52, state.term_w - 4);
    int ph = 5;
    if (pw < 22) return;

    int px = (state.term_w - pw) / 2;
    int py = (state.term_h - ph) / 2;

    for (int i = 0; i < ph; i++) mvhline(py + i, px, ' ', pw);
    draw_box(py, px, ph, pw, Color::STATS);

    attron(COLOR_PAIR(Color::STATS) | A_BOLD);
    std::string title = " New Playlist ";
    mvprintw(py, px + std::max(0, (pw - (int)title.size()) / 2), "%s", title.c_str());
    attroff(COLOR_PAIR(Color::STATS) | A_BOLD);

    attron(COLOR_PAIR(Color::BG));
    mvprintw(py + 1, px + 2, "Name:");
    attroff(COLOR_PAIR(Color::BG));

    // Input field
    int ix = px + 8, iw2 = pw - 10;
    std::string disp = utf8_truncate(state.new_playlist_name, iw2);
    int dw = utf8_display_width(disp);

    attron(COLOR_PAIR(Color::SEARCH_BOX));
    mvprintw(py + 2, ix, "%s", disp.c_str());
    for (int i = dw; i < iw2; i++) addch('_');
    attroff(COLOR_PAIR(Color::SEARCH_BOX));

    // Cursor blink
    mvchgat(py + 2, ix + std::min(dw, iw2 - 1), 1, A_REVERSE, 0, nullptr);

    attron(COLOR_PAIR(Color::BG) | A_DIM);
    mvprintw(py + ph - 1, px + 2, "Enter=create  Esc=cancel");
    attroff(COLOR_PAIR(Color::BG) | A_DIM);
}

} // namespace ytui
