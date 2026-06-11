#include "tui.h"
#include "thumbs.h"
#include "theme.h"
#include "termcaps.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <unistd.h>
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

// Characters that corrupt terminal layout if drawn: C0/C1 control codes, BiDi
// overrides (can visually reorder a line), and zero-width / soft-hyphen code
// points whose width terminals disagree on. YouTube titles occasionally carry
// these; we drop them so a title can never scramble the UI.
static inline bool wc_problematic(wchar_t wc) {
    if (wc < 0x20) return true;                         // C0 controls
    if (wc >= 0x7f && wc < 0xa0) return true;           // DEL + C1 controls
    if (wc == 0x00ad) return true;                      // soft hyphen
    if (wc == 0x070f || wc == 0x06dd || wc == 0x08e2) return true; // arabic format
    if (wc == 0x200b || wc == 0x200c || wc == 0x200d) return true; // ZW space/joiners
    if (wc >= 0x202a && wc <= 0x202e) return true;      // BiDi embeddings/overrides
    if (wc >= 0x2066 && wc <= 0x2069) return true;      // BiDi isolates
    if (wc == 0xfeff) return true;                      // BOM / ZWNBSP
    return false;
}

int TUI::utf8_display_width(const std::string& s) {
    bool uni = TermCaps::get().unicode;
    int w = 0;
    const char* p = s.c_str();
    const char* end = p + s.size();
    mbstate_t mbs{};
    while (p < end) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, p, end - p, &mbs);
        if (n == (size_t)-1 || n == (size_t)-2) { p++; continue; }
        if (n == 0) break;
        if (wc_problematic(wc) || (!uni && (unsigned long)wc >= 0x80)) { p += n; continue; }
        int cw = wcwidth(wc);
        w += (cw > 0) ? cw : 0;
        p += n;
    }
    return w;
}

// Truncate s to display width <= max_cols (appending "..." if cut) AND strip
// problematic code points, so every string drawn is layout-safe.
std::string TUI::utf8_truncate(const std::string& s, int max_cols) {
    if (max_cols <= 0) return "";
    bool fits = utf8_display_width(s) <= max_cols;
    if (max_cols <= 3 && !fits) return std::string(max_cols, '.');

    int target = fits ? max_cols : max_cols - 3;
    bool uni = TermCaps::get().unicode;
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
        // Drop layout-breakers, and in non-UTF-8 locales drop anything outside
        // printable ASCII: emitting those bytes is what turns the whole UI into
        // garbage on terminals like bobcat or a Latin-1 mlterm.
        if (wc_problematic(wc) || (!uni && (unsigned long)wc >= 0x80)) { p += n; continue; }
        int cw = wcwidth(wc); if (cw < 0) cw = 0;
        if (cols + cw > target) break;
        result.append(p, n);
        cols += cw;
        p += n;
    }
    if (!fits) result += "...";
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

    // Now that terminfo is live, pull authoritative colour/flag facts.
    TermCaps::refine_from_ncurses();
    const TermCaps& caps = TermCaps::get();

    // Mouse: only enable when the terminal supports SGR mouse reporting. On
    // terminals that don't (e.g. the Linux console), enabling it spews escape
    // bytes into the input stream as garbage keypresses.
    if (caps.mouse_sgr) {
        mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
        mouseinterval(0);
        mouse_enabled_ = true;
    }

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_thumb_colors();
    }
    // Selection bar uses a real colour pair when we have >= 8 colours; otherwise
    // fall back to reverse video (still legible on monochrome/8-colour).
    sel_use_color_ = has_colors() && caps.colors >= 8;

    // Borders: Unicode box-drawing only in a UTF-8 locale; ASCII otherwise.
    // (Never ACS — see the Border comment in tui.h.)
    if (caps.unicode) bd_ = {"─", "│", "┌", "┐", "└", "┘"};
    else              bd_ = {"-", "|", "+", "+", "+", "+"};

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
// ── initialise thumbnail colour pairs, safely on any ncurses ──────────────────
// We want one pair per 256-colour index (pairs 17..272). Two hazards:
//   1. COLORS < 256 (8/16-colour terminal)            -> monochrome.
//   2. ncurses < 6.1 (e.g. Apple's system ncurses 5.7) reports a large
//      COLOR_PAIRS from terminfo but the colour-pair number only occupies the
//      8-bit A_COLOR field, so any pair > 255 aliases onto a low pair and, on
//      5.7, init_pair() for those high pairs is unsafe. Extended colour pairs
//      (>256) require ncurses 6.1+.
// So: cap the highest usable pair at 255 unless we positively detect ncurses
// >= 6.1, and allocate as many thumb pairs as actually fit. draw_thumb() maps
// each 0..255 colour index into the available palette, so fewer pairs just
// means a coarser (still correct) thumbnail instead of heap corruption.
static bool ncurses_has_ext_colors() {
#if defined(NCURSES_VERSION_MAJOR)
    // Compile-time floor; refine at runtime below.
    if (NCURSES_VERSION_MAJOR < 6) return false;
#endif
    const char* v = curses_version();              // e.g. "ncurses 6.4.20230114"
    if (!v) return false;
    int major = 0, minor = 0;
    // Skip to first digit, then parse "major.minor".
    while (*v && (*v < '0' || *v > '9')) v++;
    if (sscanf(v, "%d.%d", &major, &minor) < 1) return false;
    return (major > 6) || (major == 6 && minor >= 1);
}

void TUI::init_thumb_colors() {
    thumb_color_ready_ = false;
    thumb_pairs_ = 0;
    if (COLORS < 256) return;                       // hazard 1 -> monochrome

    int top = COLOR_PAIRS - 1;                      // highest pair index per terminfo
    if (!ncurses_has_ext_colors())
        top = (top < 255) ? top : 255;             // hazard 2: clamp to 8-bit field

    if (top < THUMB_COLOR_BASE) return;             // no room for any thumb pair
    int n = top - THUMB_COLOR_BASE + 1;
    if (n > 256) n = 256;
    if (n < 16) return;                             // too few to be worth it

    for (int s = 0; s < n; s++) {
        // Even-spaced representative colour for this slot (identity when n==256).
        int color = (n == 256) ? s : (s * 256 / n);
        if (color > 255) color = 255;
        init_pair(short(THUMB_COLOR_BASE + s), short(color), -1);
    }
    thumb_pairs_ = n;
    thumb_color_ready_ = (n > 0);
}

// ── EP1: unified thumbnail renderer ──────────────────────────────────────────
// Tries 256-colour first; falls back to monochrome.
// All output goes through ncurses mvaddstr/attron — erase() owns and clears it.
void TUI::draw_thumb(const std::string& video_id,
                     int x, int y, int cols, int rows, int bot) {
    // Raster protocols: don't paint block art. Record the region and let
    // flush_graphics() draw the real image after refresh(). Leaving the cells
    // blank means the image sits cleanly in the panel.
    if (gfx_is_raster()) {
        if (!video_id.empty() && cols > 0 && rows > 0)
            pending_gfx_.push_back({ video_id, x, y, cols, rows });
        return;
    }
    if (thumb_color_ready_) {
        ThumbData td = Thumbnails::render_color(video_id, cols, rows);
        if (!td.empty()) {
            int row = 0;
            for (auto& line : td) {
                if (y + row >= bot || row >= rows) break;
                move(y + row, x);
                for (auto& cell : line) {
                    int pair = Color::BG;
                    if (cell.color_idx >= 0 && cell.color_idx < 256 && thumb_pairs_ > 0) {
                        // Map the 0..255 colour index into the slots we could
                        // actually allocate (identity when thumb_pairs_ == 256).
                        int slot = (thumb_pairs_ == 256)
                                       ? cell.color_idx
                                       : cell.color_idx * thumb_pairs_ / 256;
                        if (slot >= thumb_pairs_) slot = thumb_pairs_ - 1;
                        pair = THUMB_COLOR_BASE + slot;
                    }
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

// Pick a readable foreground (black or white) for text drawn on a given
// 256-colour background, so the selection bar is legible whatever the theme's
// accent colour is. Uses a cheap luminance estimate over the 256-colour layout.
static short selbar_fg(short bg) {
    int lum; // 0..15ish
    if (bg >= 232 && bg <= 255)        lum = (bg - 232) * 15 / 23;   // grayscale ramp
    else if (bg >= 16 && bg <= 231) {                                // 6x6x6 cube
        int c = bg - 16, r = c / 36, g = (c / 6) % 6, b = c % 6;
        lum = (r * 3 + g * 6 + b) * 15 / (5 * 10);
    } else {                                                         // 0..15 ANSI
        static const int br[16] = {0,3,5,6,3,4,7,11,5,7,12,13,7,9,14,15};
        lum = (bg >= 0 && bg < 16) ? br[bg] : 8;
    }
    return lum >= 8 ? COLOR_BLACK : COLOR_WHITE;
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

    // Selection bar: solid fg-on-bg so the highlight is a real, full-width bar
    // that does NOT depend on reverse-video or back-colour-erase behaving the
    // same across terminals (it doesn't: Linux console, PuTTY, gnome-terminal
    // erase with the default bg, so clrtoeol/bkgd can't be trusted to paint it).
    short bar_bg = tc.selected >= 0 ? tc.selected : tc.accent;
    init_pair(Color::SELECTED_BAR, selbar_fg(bar_bg), bar_bg);
}

void TUI::get_dimensions(int& w, int& h) { getmaxyx(stdscr, h, w); }

// Paint a full-width selection bar background with explicit spaces. Robust on
// every terminal because it does not depend on back-colour-erase or clrtoeol
// honouring the background attribute. Caller then draws text with sel_attr().
void TUI::paint_sel_bar(int y, int x, int w) {
    if (w <= 0) return;
    chtype a = sel_attr();
    attron(a);
    for (int i = 0; i < w; i++) mvaddch(y, x + i, ' ');
    attroff(a);
}

// ─── Top-level render ─────────────────────────────────────────────────────────
void TUI::render(const AppState& state, const Library* lib) {
    gfx_mode_ = state.gfx_mode;
    pending_gfx_.clear();
    setup_colors(state);
    erase();

    // Narrow terminal: hand off to the minimalist music-player layout.
    if (state.ui_mode == 1) {
        render_streamlined(state, lib);
        refresh();
        flush_graphics();
        return;
    }

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
    flush_graphics();   // emit raster thumbnails (Sixel/Kitty/iTerm) post-refresh
}

// ── flush_graphics ────────────────────────────────────────────────────────────
// Emit recorded raster thumbnails directly to the tty, outside ncurses. Called
// once per frame after refresh(). Per-protocol clearing differs:
//   * Sixel / iTerm2 images live in the cell grid, so ncurses repainting those
//     cells (we touchwin() to force it) erases them on the next frame.
//   * Kitty images are separate placements that text repaint does NOT erase, so
//     we explicitly delete all images each frame before drawing the new set
//     (and also when a frame has no thumbnails, to clear a stale one).
void TUI::flush_graphics() {
    Thumbnails::Gfx g = (Thumbnails::Gfx)gfx_mode_;
    auto out = [](const char* s, size_t n) { ssize_t w = write(STDOUT_FILENO, s, n); (void)w; };

    // Clear previously placed kitty images first.
    if (g == Thumbnails::Gfx::Kitty && kitty_drawn_) {
        static const char kitty_delete_all[] = "\033_Ga=d,d=A\033\\";
        out(kitty_delete_all, sizeof(kitty_delete_all) - 1);
        kitty_drawn_ = false;
    }

    if (pending_gfx_.empty()) return;

    bool drew = false;
    for (const auto& r : pending_gfx_) {
        std::string img = Thumbnails::render_graphics(r.id, g, r.cols, r.rows);
        if (img.empty()) continue;
        char pos[32];
        int n = snprintf(pos, sizeof(pos), "\033[%d;%dH", r.y + 1, r.x + 1);
        out(pos, (size_t)n);
        out(img.data(), img.size());
        drew = true;
    }
    if (g == Thumbnails::Gfx::Kitty) kitty_drawn_ = drew;

    // Force a full repaint next frame so sixel/iterm images get cleared as the
    // underlying cells are reused.
    if (drew) touchwin(stdscr);
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
                    paint_sel_bar(row, 2, full_w + 1);
                    attron(sel_attr() | A_BOLD);
                    mvprintw(row, 2, " %-*s", title_w - 1, utf8_truncate(hist[idx].title, title_w - 2).c_str());
                    if (chan_w > 0)
                        printw("  %-*s ", chan_w - 2, utf8_truncate(hist[idx].channel, chan_w - 3).c_str());
                    attroff(sel_attr() | A_BOLD);
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
                        attron(sel_attr());
                        mvprintw(row,     2, " %-*s ", lw - 5, utf8_truncate(hist[idx].title, lw - 7).c_str());
                        if (!hist[idx].channel.empty() && row + 1 < ey)
                            mvprintw(row + 1, 2, " %-*s ", lw - 5, utf8_truncate("  " + hist[idx].channel, lw - 7).c_str());
                        attroff(sel_attr());
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
            attron(sel_attr());
            mvprintw(y, 1, "%s", title.c_str());
            for (int j = tw; j < iw; j++) addch(' ');
            attroff(sel_attr());
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
            attron(sel_attr());
            mvprintw(y, lw + 1, " %s", label.c_str());
            for (int j = lw2 + 1; j < iw; j++) addch(' ');
            attroff(sel_attr());
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
        if (sel) attron(sel_attr());
        else     attron(COLOR_PAIR(Color::BG));
        mvprintw(py + 2 + i, px + 2, "%-*s", half - 4, utf8_truncate(sorts[i], half - 4).c_str());
        if (sel) attroff(sel_attr());
        else     attroff(COLOR_PAIR(Color::BG));
    }

    draw_box(py, px + half, ph, half, state.sort_col == 1 ? Color::ACCENT : Color::BORDER);
    attron(COLOR_PAIR(Color::ACCENT) | A_BOLD);
    mvprintw(py + 1, px + half + 2, "Filter");
    attroff(COLOR_PAIR(Color::ACCENT) | A_BOLD);

    for (int i = 0; i < 6 && i < ph - 3; i++) {
        bool sel = (state.sort_col == 1 && state.sort_row == i);
        if (sel) attron(sel_attr());
        else     attron(COLOR_PAIR(Color::BG));
        mvprintw(py + 2 + i, px + half + 2, "%-*s", half - 4, utf8_truncate(filters[i], half - 4).c_str());
        if (sel) attroff(sel_attr());
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
            attron(sel_attr());
            mvprintw(oy, px + 2, " %-*s", pw - 5, utf8_truncate(opts[i], pw - 6).c_str());
            attroff(sel_attr());
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
            attron(sel_attr());
            mvprintw(row, px + 2, " %-*s", pw - 5, utf8_truncate(state.browser_choices[i], pw - 6).c_str());
            attroff(sel_attr());
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
// Uses the Border glyph set (Unicode in UTF-8 locales, ASCII otherwise) —
// never ACS_* (see tui.h).
void TUI::draw_box(int y, int x, int h, int w, int cp) {
    if (w < 2 || h < 2 || x < 0 || y < 0) return;
    attron(COLOR_PAIR(cp));
    mvaddstr(y, x, bd_.tl);
    hline_g(y, x + 1, w - 2);
    mvaddstr(y, x + w - 1, bd_.tr);
    for (int i = 1; i < h - 1; i++) {
        mvaddstr(y + i, x,         bd_.v);
        mvaddstr(y + i, x + w - 1, bd_.v);
    }
    mvaddstr(y + h - 1, x, bd_.bl);
    hline_g(y + h - 1, x + 1, w - 2);
    mvaddstr(y + h - 1, x + w - 1, bd_.br);
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
            attron(sel_attr());
            mvprintw(row, 2, " %-*s", name_w - 1, utf8_truncate(pl.name, name_w - 2).c_str());
            if (count_w > 0) printw("  %*d ", count_w - 3, (int)pl.videos.size());
            attroff(sel_attr());
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
                attron(sel_attr());
                mvprintw(row, 1, " %s", title.c_str());
                for (int j = tw2 + 1; j < iw; j++) addch(' ');
                attroff(sel_attr());
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
            attron(sel_attr());
            mvprintw(row, lw + 1, " %s", lbl.c_str());
            for (int j = llen + 1; j < iw2; j++) addch(' ');
            attroff(sel_attr());
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
            attron(sel_attr());
            mvprintw(row, px + 1, " %-*s ", pw - 4, utf8_truncate(state.playlist_names[i], pw - 5).c_str());
            attroff(sel_attr());
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

// ═══════════════════════════════════════════════════════════════════════════
// Streamlined mode — dense, themed, minimalist music-player UI for narrow
// terminals. Every colour comes from the active theme's pairs.
// ═══════════════════════════════════════════════════════════════════════════

void TUI::center_text(int y, int width, const std::string& s, chtype attr) {
    std::string t = utf8_truncate(s, width);
    int x = (width - utf8_display_width(t)) / 2; if (x < 0) x = 0;
    if (attr) attron(attr);
    mvprintw(y, x, "%s", t.c_str());
    if (attr) attroff(attr);
}

// Title row + a hairline rule, in theme colours. Returns nothing; content
// starts at y=2.
void TUI::stream_header(const AppState& state, const std::string& title) {
    int W = state.term_w;
    attron(COLOR_PAIR(Color::ACCENT) | A_BOLD);
    mvprintw(0, 1, "%s", utf8_truncate("♪ " + title, W - 2).c_str());
    attroff(COLOR_PAIR(Color::ACCENT) | A_BOLD);
    attron(COLOR_PAIR(Color::BORDER) | A_DIM);
    hline_g(1, 1, W - 2);
    attroff(COLOR_PAIR(Color::BORDER) | A_DIM);
}

// Cute status line (greetings, "Playing: …") pinned to the last row.
void TUI::stream_status(const AppState& state) {
    if (state.status_message.empty()) return;
    // hide internal magic tokens
    if (state.status_message.rfind("__", 0) == 0) return;
    center_text(state.term_h - 1, state.term_w, state.status_message,
                COLOR_PAIR(Color::STATUS) | A_DIM);
}

void TUI::render_streamlined(const AppState& state, const Library* lib) {
    switch ((StreamScreen)state.stream_screen) {
        case StreamScreen::Menu:    stream_menu(state, lib); break;
        case StreamScreen::Search:  stream_search(state);    break;
        case StreamScreen::Browse:  stream_browse(state);    break;
        case StreamScreen::Actions: stream_actions(state);   break;
        case StreamScreen::Playing: stream_playing(state);   break;
    }
}

// iPod-style section menu — dense (one row per item), theme-coloured.
void TUI::stream_menu(const AppState& state, const Library*) {
    int W = state.term_w, H = state.term_h;
    stream_header(state, "ytcui");

    static const char* icons[] = {"⌕", "★", "≡", "⌂", "↺"};  // search/library/playlists/feed/history
    bool uni = TermCaps::get().unicode;
    auto items = stream_menu_items(state);
    int n = (int)items.size();
    int top = 3;
    for (int i = 0; i < n; i++) {
        int y = top + i;
        if (y >= H - 1) break;
        bool sel = (i == state.stream_menu_sel);
        const char* ic = (i < 5 && uni) ? icons[i] : (uni && items[i] == "Now Playing" ? "▶" : ">");
        std::string row = std::string(ic) + "  " + items[i];
        if (sel) {
            paint_sel_bar(y, 1, W - 2);
            attron(sel_attr() | A_BOLD);
            mvprintw(y, 2, "%s", utf8_truncate(row, W - 4).c_str());
            attroff(sel_attr() | A_BOLD);
        } else {
            attron(COLOR_PAIR(Color::TITLE));
            mvprintw(y, 3, "%s", utf8_truncate(row, W - 5).c_str());
            attroff(COLOR_PAIR(Color::TITLE));
        }
    }
    if (H - 2 > top + n)
        center_text(H - 2, W, "j/k · ⏎ open · q quit", COLOR_PAIR(Color::BG) | A_DIM);
    stream_status(state);
}

void TUI::stream_search(const AppState& state) {
    int W = state.term_w, H = state.term_h;
    stream_header(state, "Search");
    int by = std::max(3, H / 2 - 2);
    draw_box(by, 1, 3, W - 2, Color::SEARCH_BOX);
    int iw = W - 6;
    std::string q = utf8_truncate(state.search_query, iw);
    attron(COLOR_PAIR(Color::TITLE));
    mvprintw(by + 1, 3, "%s", q.c_str());
    attroff(COLOR_PAIR(Color::TITLE));
    mvchgat(by + 1, 3 + std::min(utf8_display_width(q), iw - 1), 1, A_REVERSE, 0, nullptr);
    center_text(H - 2, W, "type · ⏎ search · esc back", COLOR_PAIR(Color::BG) | A_DIM);
    stream_status(state);
}

// Dense list of videos (or playlist names). Two compact rows per item:
// title, then a dim channel·duration line.
void TUI::stream_browse(const AppState& state) {
    int W = state.term_w, H = state.term_h;
    stream_header(state, state.stream_section.empty() ? "Browse" : state.stream_section);

    // Playlist picker variant: one row per playlist name.
    if (state.stream_on_playlists) {
        const auto& names = state.playlist_names;
        if (names.empty()) {
            center_text(H / 2, W, "No playlists yet (._.)", COLOR_PAIR(Color::BG) | A_DIM);
        } else {
            int top = 3, vis = std::max(1, H - 5), sel = state.selected_result, start = 0;
            if (sel >= vis) start = sel - vis + 1;
            for (int i = 0; i < vis && start + i < (int)names.size(); i++) {
                int idx = start + i, y = top + i; bool s = (idx == sel);
                if (s) { paint_sel_bar(y, 1, W - 2); attron(sel_attr() | A_BOLD);
                    mvprintw(y, 2, "%s", utf8_truncate(names[idx], W - 4).c_str());
                    attroff(sel_attr() | A_BOLD);
                } else { attron(COLOR_PAIR(Color::TITLE));
                    mvprintw(y, 3, "%s", utf8_truncate(names[idx], W - 5).c_str());
                    attroff(COLOR_PAIR(Color::TITLE)); }
            }
        }
        center_text(H - 2, W, "⏎ open · esc back", COLOR_PAIR(Color::BG) | A_DIM);
        stream_status(state);
        return;
    }

    const auto& r = state.results;
    if (r.empty()) {
        std::string sec = state.stream_section;
        std::string msg = sec == "Library"  ? "No saved videos yet :3"
                        : sec == "Feed"     ? "Nothing watched yet (._.)"
                        : sec == "History"  ? "No history yet (._.)"
                        : "No results... try again? (>_<)";
        center_text(H / 2, W, msg, COLOR_PAIR(Color::BG) | A_DIM);
        center_text(H - 2, W, "esc back", COLOR_PAIR(Color::BG) | A_DIM);
        stream_status(state);
        return;
    }
    int top = 3, rows_per = 2;
    int vis = std::max(1, (H - 4) / rows_per);
    int sel = state.selected_result, start = 0;
    if (sel >= vis) start = sel - vis + 1;
    for (int i = 0; i < vis && start + i < (int)r.size(); i++) {
        int idx = start + i, y = top + i * rows_per;
        if (y >= H - 2) break;
        bool s = (idx == sel);
        const Video& v = r[idx];
        if (s) {
            paint_sel_bar(y, 1, W - 2);
            attron(sel_attr() | A_BOLD);
            mvprintw(y, 2, "%s", utf8_truncate(v.title, W - 4).c_str());
            attroff(sel_attr() | A_BOLD);
        } else {
            attron(COLOR_PAIR(Color::TITLE));
            mvprintw(y, 2, "%s", utf8_truncate(v.title, W - 3).c_str());
            attroff(COLOR_PAIR(Color::TITLE));
        }
        if (y + 1 < H - 2) {
            std::string meta = v.channel;
            std::string dur = v.is_live ? "LIVE" : v.duration;
            if (!dur.empty() && dur != "0:00") meta += "  ·  " + dur;
            else if (v.is_live) meta += "  ·  LIVE";
            attron(COLOR_PAIR(Color::CHANNEL) | A_DIM);
            mvprintw(y + 1, 3, "%s", utf8_truncate(meta, W - 4).c_str());
            attroff(COLOR_PAIR(Color::CHANNEL) | A_DIM);
        }
    }
    center_text(H - 2, W, "⏎ play · esc back", COLOR_PAIR(Color::BG) | A_DIM);
    stream_status(state);
}

// Play-mode chooser for the highlighted video.
void TUI::stream_actions(const AppState& state) {
    int W = state.term_w, H = state.term_h;
    stream_header(state, "Play");
    bool uni = TermCaps::get().unicode;

    const Video* v = (!state.results.empty() &&
                      state.selected_result < (int)state.results.size())
                     ? &state.results[state.selected_result] : nullptr;
    int y = std::max(3, H / 2 - 3);
    if (v) { center_text(y, W, v->title, COLOR_PAIR(Color::TITLE) | A_BOLD);
             center_text(y + 1, W, v->channel, COLOR_PAIR(Color::CHANNEL) | A_DIM); }
    y += 3;

    const char* labels[2] = { uni ? "▶  Play video" : "Play video",
                              uni ? "♪  Play audio" : "Play audio" };
    for (int i = 0; i < 2; i++) {
        int ry = y + i;
        bool s = (state.stream_action_sel == i);
        std::string lab = labels[i];
        int lw = utf8_display_width(lab);
        int x = (W - lw) / 2; if (x < 2) x = 2;
        if (s) {
            paint_sel_bar(ry, x - 2, lw + 4);
            attron(sel_attr() | A_BOLD); mvprintw(ry, x, "%s", lab.c_str()); attroff(sel_attr() | A_BOLD);
        } else {
            attron(COLOR_PAIR(Color::TITLE)); mvprintw(ry, x, "%s", lab.c_str()); attroff(COLOR_PAIR(Color::TITLE));
        }
    }
    center_text(H - 2, W, "j/k · ⏎ play · esc back", COLOR_PAIR(Color::BG) | A_DIM);
    stream_status(state);
}

// Now-playing card: album art, stylized waveform, time, title/artist, transport.
void TUI::stream_playing(const AppState& state) {
    int W = state.term_w, H = state.term_h;
    const Video& v = state.stream_now;
    bool uni = TermCaps::get().unicode;
    int y = 1;

    // Album art (chafa), centered, kept compact so the card stays dense.
    if (state.thumbs_available && !v.id.empty() && H > 13) {
        int art_w = std::min(W - 4, (H - 10) * 2); if (art_w < 6) art_w = W - 4;
        int art_rows = std::max(3, art_w / 2); if (art_rows > H - 10) art_rows = H - 10;
        int ax = (W - art_w) / 2; if (ax < 0) ax = 0;
        draw_thumb(v.id, ax, y, art_w, art_rows, y + art_rows);
        y += art_rows + 1;
    } else { y = 2; }

    // Stylized waveform + time labels (elapsed unknown → 0:00; right = total/LIVE).
    if (y < H - 5) {
        static const char* bars[] = {"▁","▂","▃","▄","▅","▆","▇","█"};
        static const int pat[] = {2,3,5,4,6,7,5,4,6,3,2,3,5,7,6,4,3,2,4,6,5,3,2,4,5,3,2,1};
        int wf = W - 6; if (wf < 4) wf = W - 2;
        int wx = (W - wf) / 2; if (wx < 1) wx = 1;
        attron(COLOR_PAIR(Color::ACCENT));
        for (int i = 0; i < wf; i++)
            mvprintw(y, wx + i, "%s", uni ? bars[pat[i % (int)(sizeof(pat)/sizeof(int))]] : "-");
        attroff(COLOR_PAIR(Color::ACCENT));
        attron(COLOR_PAIR(Color::STATS) | A_DIM);
        mvprintw(y + 1, wx, "0:00");
        std::string dur = v.is_live ? "LIVE"
                        : (v.duration.empty() || v.duration == "0:00") ? "--:--" : v.duration;
        mvprintw(y + 1, wx + wf - (int)dur.size(), "%s", dur.c_str());
        attroff(COLOR_PAIR(Color::STATS) | A_DIM);
        y += 3;
    }

    if (y < H - 3) { center_text(y, W, v.title.empty() ? "Nothing playing" : v.title,
                                 COLOR_PAIR(Color::TITLE) | A_BOLD); y++; }
    if (y < H - 2 && !v.channel.empty()) { center_text(y, W, v.channel, COLOR_PAIR(Color::CHANNEL)); y++; }
    y++;

    // Transport row, theme-coloured. Center symbol reflects play/pause.
    if (y < H - 1) {
        const char* prev = uni ? "◀◀" : "<<";
        const char* mid  = state.is_paused ? (uni ? "▶" : ">") : (uni ? "❚❚" : "||");
        const char* next = uni ? "▶▶" : ">>";
        center_text(y, W, std::string(prev) + "     " + mid + "     " + next,
                    COLOR_PAIR(Color::ACCENT) | A_BOLD);
    }
    center_text(H - 1, W, "space pause · b back · q quit", COLOR_PAIR(Color::BG) | A_DIM);
}

} // namespace ytui
