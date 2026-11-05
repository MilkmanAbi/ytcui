#include "tui.h"
#include "thumbs.h"
#include "theme.h"
#include "termcaps.h"
#include "log.h"
#include "core/share.h"
#include "core/layout.h"
#include "core/text.h"
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <unistd.h>
#include <termios.h>
#include <sstream>
#include <wchar.h>

namespace ytui {

// ─── mlterm bold/dim-as-reverse quirk ────────────────────────────────────────
// On a real mlterm with no distinct bold font configured, ncurses A_BOLD (and
// A_DIM) doesn't change font weight — mlterm substitutes a reverse-video block
// to make the text "stand out", since it has no bold glyphs to draw. Every
// place in this file that uses A_BOLD/A_DIM purely for emphasis (panel headers,
// the active tab, dimmed hints/footers — none of which are a "selected item")
// would turn into an unwanted highlight box. There's no terminfo bit for this,
// so when mlterm is the actual detected/forced terminal we strip those two bits
// from every attron/attroff call below via macro interception. A_REVERSE is
// left alone: TermCaps trusts it on mlterm (reverse_ok=true) and it's the one
// attribute that's supposed to look like a block — that's the selection cursor,
// not decoration. The flag is refreshed each frame from TermCaps (single source
// of truth), so nothing here keeps its own copy of the terminal identity.
static bool g_strip_emphasis = false;
static inline chtype mlterm_fx_filter(chtype a) {
    return g_strip_emphasis ? (a & ~(chtype)(A_BOLD | A_DIM)) : a;
}
#undef attron
#undef attroff
#define attron(a)  wattron(stdscr, mlterm_fx_filter((chtype)(a)))
#define attroff(a) wattroff(stdscr, mlterm_fx_filter((chtype)(a)))

// All text enters curses through a sanitized, column-bounded writer.
int TUI::utf8_display_width(const std::string& text) {
    return ytcui::txt::terminal_width(text, TermCaps::get().unicode);
}
std::string TUI::utf8_truncate(const std::string& text, int cols) {
    return ytcui::txt::terminal_clip(text, cols, TermCaps::get().unicode);
}
int TUI::put_at(int y, int x, const char* text) {
    if (!stdscr || !text) return ERR;
    int h, w; getmaxyx(stdscr, h, w);
    if (y < 0 || x < 0 || y >= h || x >= w) return ERR;
    int ascii = 0;
    while (ascii < w-x && text[ascii] && static_cast<unsigned char>(text[ascii]) >= 32 && static_cast<unsigned char>(text[ascii]) < 127) ++ascii;
    if (ascii == w-x || text[ascii] == 0) return mvwaddnstr(stdscr,y,x,text,ascii);
    auto clipped = ytcui::txt::terminal_clip(text, w-x, TermCaps::get().unicode, false);
    return mvwaddnstr(stdscr, y, x, clipped.c_str(), static_cast<int>(clipped.size()));
}
int TUI::print_at(int y, int x, const char* format, ...) {
    char text[16384];
    va_list args; va_start(args, format);
    vsnprintf(text, sizeof text, format, args); va_end(args);
    return put_at(y, x, text);
}
int TUI::print_here(const char* format, ...) {
    int y, x; getyx(stdscr, y, x);
    char text[16384];
    va_list args; va_start(args, format);
    vsnprintf(text, sizeof text, format, args); va_end(args);
    return put_at(y, x, text);
}

// ─── TUI lifecycle ────────────────────────────────────────────────────────────
TUI::TUI() {}
TUI::~TUI() { shutdown(); }

bool TUI::init() {
    if (initialized_) return true;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;
    have_termios_ = tcgetattr(STDIN_FILENO, &original_termios_) == 0;
    screen_ = newterm(nullptr, stdout, stdin);
    if (!screen_) {
        if (have_termios_) tcsetattr(STDIN_FILENO,TCSANOW,&original_termios_);
        return false;
    }
    set_term(screen_);
    // Belt-and-braces: discard anything ncurses' first read would otherwise
    // pick up as a keypress. TermCaps::detect() (and, rarely, an extra
    // sixel/cell-size probe) briefly put the tty in raw mode before we get
    // here to read terminal-identification replies; each of those already
    // drains its own leftover bytes, but flushinp() closes the small gap
    // between "that probe finished" and "initscr() started reading" — the
    // window where a very slow terminal reply could still land and get
    // delivered as garbage text (e.g. straight into the search box).
    // Do not discard real keys typed while the terminal was being detected.
    cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    nodelay(stdscr, FALSE); timeout(33);
    if (!getenv("ESCDELAY")) set_escdelay(100);

    // Disable software flow control (IXON/IXOFF). Without this, Ctrl-S is
    // eaten by the terminal as XOFF (freezing output) and Ctrl-Q as XON,
    // so the settings shortcut (Ctrl-S) would never reach the app and could
    // appear to hang ytcui. cbreak() leaves these on; we clear them here.
    {
        struct termios t;
        if (tcgetattr(STDIN_FILENO, &t) == 0) {
            t.c_iflag &= ~(IXON | IXOFF | IXANY);
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
        }
    }

    // Now that terminfo is live, pull authoritative colour/flag facts.
    const TermCaps& caps = TermCaps::get();

    // Mouse: only enable when the terminal supports SGR mouse reporting. On
    // terminals that don't (e.g. the Linux console), enabling it spews escape
    // bytes into the input stream as garbage keypresses.
    //
    // We request ONLY button events, never REPORT_MOUSE_POSITION. Movement
    // reporting (xterm mode 1003) floods stdin with an SGR report on every
    // cursor move; at a fast input poll those can arrive split across reads and
    // the tail bytes leak onto the screen as garbage like "0;48;9M". ytcui only
    // uses clicks and the scroll wheel, so movement events are pure downside.
    if (caps.mouse_sgr) {
        mousemask(BUTTON1_PRESSED | BUTTON1_CLICKED |
                  BUTTON4_PRESSED | BUTTON5_PRESSED, nullptr);
        mouseinterval(0);
        mouse_enabled_ = true;
    }

    if (has_colors()) {
        start_color();
        default_bg_ = use_default_colors() == OK ? -1 : COLOR_BLACK;
        TermCaps::refine_from_ncurses();
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

void TUI::clear_graphics() {
    if (kitty_drawn_) {
        static const char erase[] = "\033_Ga=d,d=A\033\\";
        (void)::write(STDOUT_FILENO, erase, sizeof erase - 1);
        kitty_drawn_ = false;
    }
    pending_gfx_.clear();
}
void TUI::shutdown() {
    if (initialized_) {
        clear_graphics();
        mousemask(0, nullptr);
        endwin();
        if (screen_) { delscreen(screen_); screen_ = nullptr; }
        if (have_termios_) tcsetattr(STDIN_FILENO, TCSANOW, &original_termios_);
        initialized_ = false;
    }
}

// Thumbnail pairs occupy 19..255 (or fewer when terminfo/A_COLOR has fewer bits).
// COLOR_PAIR(chtype) cannot represent extended pair numbers on ncurses 6.x either.
// Map all 256 input colors across the available legacy slots; reserve UI/QR pairs.
void TUI::init_thumb_colors() {
    thumb_color_ready_ = false;
    thumb_pairs_ = 0;
    if (COLORS < 256) return;                       // hazard 1 -> monochrome

    int top = COLOR_PAIRS - 1;                      // highest pair index per terminfo
    // COLOR_PAIR(chtype) carries only legacy pair bits even on ncurses 6.x.
    top = std::min(top, std::min(255, static_cast<int>(PAIR_NUMBER(A_COLOR))));             // hazard 2: clamp to 8-bit field

    if (top < THUMB_COLOR_BASE) return;             // no room for any thumb pair
    int n = top - THUMB_COLOR_BASE + 1;
    if (n > 256) n = 256;
    if (n < 16) return;                             // too few to be worth it

    for (int s = 0; s < n; s++) {
        // Even-spaced representative colour for this slot (identity when n==256).
        int color = (n == 256) ? s : (s * 256 / n);
        if (color > 255) color = 255;
        if (init_pair(short(THUMB_COLOR_BASE + s), short(color), default_bg_) == ERR) return;
    }
    thumb_pairs_ = n;
    thumb_color_ready_ = (n > 0);
}

// ── EP1: unified thumbnail renderer ──────────────────────────────────────────
// Tries 256-colour first; falls back to monochrome.
// All output goes through ncurses mvaddstr/attron — erase() owns and clears it.
void TUI::draw_thumb(const std::string& video_id,
                     int x, int y, int cols, int rows, int bot) {
    // Strict-monochrome mode draws no thumbnail at all — not raster, not chafa
    // block art. Emitting either is what garbles a no-imagelib mlterm, so the
    // safest thing is to render nothing and let the layout reclaim the space.
    if (mono_mode_) return;
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
                int drawn_cols = 0;
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
                    const int width = utf8_display_width(cell.glyph);
                    if (drawn_cols + width > cols) { attroff(COLOR_PAIR(pair)); break; }
                    put_at(y + row, x + drawn_cols, cell.glyph.c_str());
                    drawn_cols += width;
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
        put_at(y + tl, x, ytcui::txt::terminal_clip(ln, cols, TermCaps::get().unicode, false).c_str());
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

static short palette_color(int value, int count, short fallback) {
    if (value < 0) return fallback;
    if (value < count) return static_cast<short>(value);
    if (count <= 0) return 0;
    if (value < 16) return static_cast<short>(value % std::min(8, count));
    int red, green, blue;
    if (value >= 232) red = green = blue = 8 + (std::min(value,255)-232)*10;
    else {
        const int cube[] = {0,95,135,175,215,255};
        int v = std::clamp(value-16, 0, 215);
        red = cube[v/36]; green = cube[(v/6)%6]; blue = cube[v%6];
    }
    int basic = (red >= 128 ? 1 : 0) | (green >= 128 ? 2 : 0) | (blue >= 128 ? 4 : 0);
    return static_cast<short>(basic % count);
}

void TUI::setup_colors(const AppState& state) {
    qr_colors_ = false;
    if (!has_colors() || COLORS <= 0) { sel_use_color_ = false; return; }
    auto pair = [&](short id, int fg) {
        if (id < COLOR_PAIRS) init_pair(id, palette_color(fg, COLORS, default_bg_ < 0 ? -1 : COLOR_WHITE), default_bg_);
    };
    // resolved_colors is pre-computed by App: base theme + custom overrides.
    // In strict-monochrome mode (MLterm theme OR a detected/forced mlterm) every
    // element must be the terminal default — otherwise an auto-detected mlterm
    // keeps a non-mlterm theme's colours (e.g. cyan titles) and only the
    // selection bar/thumbnails get hardened, leaking colour the terminal can't
    // render cleanly. Substituting the all-default MLterm palette here makes the
    // colour-forcing independent of which theme happens to be configured.
    const ThemeColors& tc = mono_mode_ ? get_theme_colors(Theme::MLterm)
                                       : state.resolved_colors;

    pair(Color::BG, tc.bg);
    pair(Color::SEARCH_BOX, tc.search_box);
    pair(Color::TITLE, tc.title);
    pair(Color::CHANNEL, tc.channel);
    pair(Color::STATS, tc.stats);
    pair(Color::SELECTED, tc.selected);
    pair(Color::ACTION, tc.action);
    pair(Color::ACTION_SEL, tc.action_sel);
    pair(Color::STATUS, tc.status);
    pair(Color::BORDER, tc.border);
    pair(Color::HEADER, tc.header);
    pair(Color::ACCENT, tc.accent);
    pair(Color::TAG, tc.tag);
    pair(Color::PUBLISHED, tc.published);
    pair(Color::BOOKMARK, tc.bookmark);
    pair(Color::DESC, tc.desc);

    // Selection bar: solid fg-on-bg so the highlight is a real, full-width bar
    // that does NOT depend on reverse-video or back-colour-erase behaving the
    // same across terminals (it doesn't: Linux console, PuTTY, gnome-terminal
    // erase with the default bg, so clrtoeol/bkgd can't be trusted to paint it).
    short bar_bg = tc.selected >= 0 ? tc.selected : tc.accent;
    bar_bg = palette_color(bar_bg, COLORS, COLOR_BLACK);
    if (Color::SELECTED_BAR < COLOR_PAIRS) init_pair(Color::SELECTED_BAR, selbar_fg(bar_bg), bar_bg);
    qr_colors_ = Color::QR < COLOR_PAIRS && init_pair(Color::QR, COLOR_BLACK, COLOR_WHITE) == OK;

    // Strict-monochrome mode: never use a coloured selection bar. sel_attr()
    // falls back to plain A_REVERSE — the universal, colour-free "this row is
    // selected" convention, and the only one guaranteed legible on mlterm.
    if (mono_mode_) sel_use_color_ = false;
    else            sel_use_color_ = has_colors() && TermCaps::get().colors >= 8;
}

void TUI::get_dimensions(int& w, int& h) { getmaxyx(stdscr, h, w); }

// Paint a full-width selection bar background with explicit spaces. Robust on
// every terminal because it does not depend on back-colour-erase or clrtoeol
// honouring the background attribute. Caller then draws text with sel_attr().
void TUI::paint_sel_bar(int y, int x, int w) {
    int rows, cols; getmaxyx(stdscr, rows, cols);
    if (w <= 0 || y < 0 || y >= rows || x < 0 || x >= cols) return;
    w = std::min(w, cols-x);
    chtype a = sel_attr();
    attron(a);
    for (int i = 0; i < w; i++) mvaddch(y, x + i, ' ');
    attroff(a);
}

// ─── Top-level render ─────────────────────────────────────────────────────────
void TUI::render(const AppState& state, const Library* lib) {
    if (!initialized_) return;
    // Strict-monochrome mode is in effect when the user picked the MLterm theme
    // OR the terminal was detected/forced as mlterm. Both converge here so the
    // hardening is identical either way.
    const TermCaps& caps = TermCaps::get();
    mono_mode_ = (state.theme == Theme::MLterm) || caps.mono_hardening;
    // Strip the bold/dim-as-reverse emphasis quirk whenever mono mode is active.
    // On a real mlterm this prevents the reverse-video garble; with the MLterm
    // theme on a colour terminal it simply honours the theme's contract ("no
    // bold/dim emphasis") — flat output is the whole point of choosing it.
    g_strip_emphasis = mono_mode_;
    // Monochrome means NO thumbnails of any kind — not raster, not even chafa
    // block art (its bytes are exactly what garbles a no-imagelib mlterm). The
    // app layer already forces gfx off when hardened; this is the render-side
    // guarantee that no image path runs regardless of state.gfx_mode.
    const int requested_gfx = mono_mode_ ? (int)Thumbnails::Gfx::None : state.gfx_mode;
    if (requested_gfx != gfx_mode_) clear_graphics();
    gfx_mode_ = requested_gfx;

    // Diagnostic: log only when the mono state flips, so a --debug log makes it
    // obvious WHY a terminal is (or isn't) black & white — detection vs theme vs
    // force — without spamming a line every frame.
    {
        static int last_mono = -1;
        int cur = mono_mode_ ? 1 : 0;
        if (cur != last_mono) {
            last_mono = cur;
            if (mono_mode_)
                Log::write("Render: strict B&W mode ON (%s)",
                           caps.mono_hardening
                               ? (state.theme == Theme::MLterm
                                      ? "mlterm theme + detected/forced mlterm"
                                      : "detected/forced mlterm")
                               : "MLterm theme selected");
            else
                Log::write("Render: colour mode (theme honoured, no mlterm)");
        }
    }
    pending_gfx_.clear();
    setup_colors(state);
    attrset(A_NORMAL);
    erase();
    if (state.term_w < 16 || state.term_h < (state.ui_mode == 1 ? 6 : 12)) {
        clear_graphics();
        print_at(0, 0, "Resize terminal");
        if (state.show_share && state.term_w>=16 && state.term_h>=6) draw_share_menu(state);
        refresh(); return;
    }

    // Narrow terminal: hand off to the minimalist music-player layout.
    if (state.ui_mode == 1) {
        render_streamlined(state, lib);
        draw_overlays(state);
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

    if (state.show_status_bar) draw_message_bar(state);

    draw_overlays(state);
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
void TUI::draw_overlays(const AppState& state) {
    if (state.focus == Panel::BrowserPick)  draw_browser_popup(state);
    if (state.focus == Panel::SortMenu)     draw_sort_menu(state);
    if (state.focus == Panel::SavePrompt)   draw_save_prompt(state);
    if (state.focus == Panel::PlaylistPick) draw_playlist_picker(state);
    if (state.focus == Panel::NewPlaylist)  draw_new_playlist_prompt(state);
    if (state.show_shortcuts)              draw_shortcuts(state);
    if (state.show_settings)               draw_settings(state);
    if (state.show_timer_prompt || state.focus == Panel::TimerPrompt) draw_timer_prompt(state);
    if (state.show_queue || state.focus == Panel::QueueView)          draw_queue_view(state);
    if (state.show_share  || state.focus == Panel::ShareMenu)         draw_share_menu(state);
    if (state.show_res_menu   || state.focus == Panel::ResMenu)       draw_res_menu(state);
    if (state.show_share || state.show_queue || state.show_settings || state.show_shortcuts ||
        state.show_timer_prompt || state.show_res_menu || state.focus == Panel::BrowserPick ||
        state.focus == Panel::SavePrompt || state.focus == Panel::SortMenu ||
        state.focus == Panel::PlaylistPick || state.focus == Panel::NewPlaylist) clear_graphics();
}

void TUI::flush_graphics() {
    Thumbnails::Gfx g = (Thumbnails::Gfx)gfx_mode_;
    bool have_kitty_clear = (g == Thumbnails::Gfx::Kitty && kitty_drawn_);
    if (pending_gfx_.empty() && !have_kitty_clear) return;

    auto out = [](const char* s, size_t n) { ssize_t w = write(STDOUT_FILENO, s, n); (void)w; };

    // Save the real cursor position (DECSC) before doing ANY raw positioning,
    // and restore it (DECRC) when we're done. Without this, every absolute
    // "\033[y;xH" we send to place a raster image silently moves the terminal's
    // physical cursor while ncurses' own internal model of "where the cursor
    // is" (set by the refresh() that already ran this frame) never learns
    // about it. The next frame's doupdate() then computes its redraw as a
    // *relative* move from the position ncurses still believes is current —
    // which is now wrong — so every subsequent glyph lands in the wrong cell.
    // That desync compounds frame over frame, which is exactly the "insane
    // corruption" that only appeared once a raster graphics mode (sixel/
    // kitty/iterm) was enabled: block-art thumbnails never touch the raw tty,
    // so they never hit this. Saving/restoring keeps the physical cursor
    // exactly where ncurses left it, so its model is never invalidated.
    out("\0337", 2);   // DECSC — save cursor

    // Clear previously placed kitty images first.
    if (have_kitty_clear) {
        static const char kitty_delete_all[] = "\033_Ga=d,d=A\033\\";
        out(kitty_delete_all, sizeof(kitty_delete_all) - 1);
        kitty_drawn_ = false;
    }

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

    out("\0338", 2);   // DECRC — restore cursor to where ncurses left it

    // Force a full repaint next frame so sixel/iterm images get cleared as the
    // underlying cells are reused.
    if (drew) touchwin(stdscr);
}

// ─── Header ───────────────────────────────────────────────────────────────────
void TUI::draw_header(const AppState& /*state*/) {
    // Intentionally empty — the top row is reclaimed by the search box.
}

// ─── Search box ───────────────────────────────────────────────────────────────
void TUI::draw_search_box(const AppState& state) {
    int y = 0, w = state.term_w;
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
    print_at(y + 1, 1, "%s", d.c_str());
    for (int i = dw; i < iw; i++) addch(' ');
    attroff(COLOR_PAIR(Color::SEARCH_BOX));

    if (f) mvchgat(y + 1, 1 + std::min(dw, iw - 1), 1, A_REVERSE, 0, nullptr);

    // Hamburger button [ ☰ ] — lights up when sort menu is open
    draw_box(y, sb_w, 3, HB_W, hsf ? Color::ACCENT : Color::BORDER);
    attron(COLOR_PAIR(hsf ? Color::ACCENT : Color::STATS) | A_BOLD);
    print_at(y + 1, sb_w + 1, "...");  // ☰ UTF-8: E2 98 B0
    attroff(COLOR_PAIR(hsf ? Color::ACCENT : Color::STATS) | A_BOLD);
}

// ─── Tabs ─────────────────────────────────────────────────────────────────────
void TUI::draw_tabs(const AppState& state) {
    if (state.term_w < 20) return;
    int y = 3;

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
        // In mono mode neither colour nor bold can mark the active tab, so wrap
        // its label in brackets — the active one reads as "[Results]" while the
        // rest stay plain. This is the colour-free "you are here" indicator.
        std::string lbl = lb[i];
        if (mono_mode_ && act) lbl = "[" + lbl + "]";
        int ll = (int)lbl.size();
        int lx = tx + (tw - ll) / 2;
        if (lx < 0 || lx + ll > state.term_w) continue;
        if (act) attron(COLOR_PAIR(Color::STATS) | A_BOLD);
        else if (is_results_tab) attron(COLOR_PAIR(Color::ACCENT));
        else attron(COLOR_PAIR(Color::BG));
        print_at(y + 1, lx, "%s", lbl.c_str());
        if (act) attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
        else if (is_results_tab) attroff(COLOR_PAIR(Color::ACCENT));
        else attroff(COLOR_PAIR(Color::BG));
    }
}

// ─── Home panels (Library / Feed / History tabs) ─────────────────────────────
void TUI::draw_home_panels(const AppState& state, const Library* lib) {
    int sy = 6, ey = state.term_h - (state.show_status_bar ? 4 : 1), ah = ey - sy;
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
            print_at(sy + 1, 2, "%s", utf8_truncate("Subscriptions", lw - 4).c_str());
            attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
        }

        auto subs = lib->subscriptions();
        if (subs.empty()) {
            if (sy + 3 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); print_at(sy + 3, 3, "%s", utf8_truncate("No subscriptions yet (._.)  ", lw - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
            if (sy + 4 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); print_at(sy + 4, 3, "%s", utf8_truncate("Subscribe from action menu!", lw - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
        } else {
            for (int i = 0; i < (int)subs.size() && i < max_items; i++) {
                int row = sy + 2 + i;
                if (row >= ey) break;
                attron(COLOR_PAIR(Color::CHANNEL));
                print_at(row, 3, "%s", utf8_truncate(subs[i].title, lw - 5).c_str());
                attroff(COLOR_PAIR(Color::CHANNEL));
            }
        }

        if (sy + 1 < ey) {
            attron(COLOR_PAIR(Color::STATS) | A_BOLD);
            print_at(sy + 1, lw + 2, "%s", utf8_truncate("Saved Videos", rw - 4).c_str());
            attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
        }

        auto vids = lib->saved_videos();
        if (vids.empty()) {
            if (sy + 3 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); print_at(sy + 3, lw + 3, "%s", utf8_truncate("No saved videos yet :3", rw - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
            if (sy + 4 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); print_at(sy + 4, lw + 3, "%s", utf8_truncate("Bookmark from action menu!", rw - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
        } else {
            for (int i = 0; i < (int)vids.size() && i < max_items; i++) {
                int row = sy + 2 + i;
                if (row >= ey) break;
                attron(COLOR_PAIR(Color::TITLE));
                print_at(row, lw + 3, "%s", utf8_truncate(vids[i].title, rw - 5).c_str());
                attroff(COLOR_PAIR(Color::TITLE));
            }
        }

    // ── History tab ──────────────────────────────────────────────────────────
    } else if (state.active_tab == Tab::History) {
        draw_box(sy, 0, ah + 1, state.term_w, Color::BG);

        if (sy + 1 < ey) {
            attron(COLOR_PAIR(Color::STATS) | A_BOLD);
            print_at(sy + 1, 2, "%s", utf8_truncate("Watch History", state.term_w - 4).c_str());
            attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
        }

        auto hist = lib->history();
        if (hist.empty()) {
            if (sy + 3 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); print_at(sy + 3, 3, "%s", utf8_truncate("Nothing watched yet ('v')", state.term_w - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
            if (sy + 4 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); print_at(sy + 4, 3, "%s", utf8_truncate("Play something and it'll show up here!", state.term_w - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
        } else {
            int full_w = state.term_w - 4;
            int title_w = full_w * 60 / 100;
            int chan_w  = std::max(0, full_w - title_w - 2);

            if (sy + 2 < ey) {
                attron(COLOR_PAIR(Color::BG) | A_DIM);
                print_at(sy + 2, 3, "%s", utf8_truncate("Title", title_w).c_str());
                if (chan_w > 0)
                    print_at(sy + 2, 3 + title_w + 2, "%s", utf8_truncate("Channel", chan_w).c_str());
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
                    print_at(row, 2, " %-*s", title_w - 1, utf8_truncate(hist[idx].title, title_w - 2).c_str());
                    if (chan_w > 0)
                        print_here("  %-*s ", chan_w - 2, utf8_truncate(hist[idx].channel, chan_w - 3).c_str());
                    attroff(sel_attr() | A_BOLD);
                } else {
                    attron(COLOR_PAIR(Color::TITLE));
                    print_at(row, 3, "%s", utf8_truncate(hist[idx].title, title_w).c_str());
                    attroff(COLOR_PAIR(Color::TITLE));
                    if (chan_w > 0 && !hist[idx].channel.empty()) {
                        attron(COLOR_PAIR(Color::CHANNEL) | A_DIM);
                        print_at(row, 3 + title_w + 2, "%s", utf8_truncate(hist[idx].channel, chan_w).c_str());
                        attroff(COLOR_PAIR(Color::CHANNEL) | A_DIM);
                    }
                }
            }
            // Hint
            if (state.show_hints && ey - 1 > sy + 2) {
                attron(COLOR_PAIR(Color::BG) | A_DIM);
                print_at(ey - 1, 2, "j/k=navigate  Enter/click=search & go to results");
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
                    print_at(cy + i, ax, "%s", art[i]);
                    attroff(COLOR_PAIR(Color::BG) | A_DIM);
                }
            }
        } else {
            draw_box(sy, 0, ah + 1, lw, Color::BG);
            draw_box(sy, lw, ah + 1, rw, Color::BG);

            if (sy + 1 < ey) {
                attron(COLOR_PAIR(Color::STATS) | A_BOLD);
                print_at(sy + 1, 2, "%s", utf8_truncate("Recently Played", lw - 4).c_str());
                attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
            }

            if (hist.empty()) {
                if (sy + 3 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); print_at(sy + 3, 3, "%s", utf8_truncate("Nothing played yet ('v')", lw - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
            } else {
                int count = std::min((int)hist.size(), max_items / 2);
                for (int i = 0; i < count; i++) {
                    int idx = (int)hist.size() - 1 - i;
                    int row = sy + 2 + i * 2;
                    if (row + 1 >= ey) break;
                    bool sel = (i == state.home_selected_idx) && (state.focus == Panel::Tabs);
                    if (sel) {
                        attron(sel_attr());
                        print_at(row,     2, " %-*s ", lw - 5, utf8_truncate(hist[idx].title, lw - 7).c_str());
                        if (!hist[idx].channel.empty() && row + 1 < ey)
                            print_at(row + 1, 2, " %-*s ", lw - 5, utf8_truncate("  " + hist[idx].channel, lw - 7).c_str());
                        attroff(sel_attr());
                    } else {
                        attron(COLOR_PAIR(Color::TITLE));
                        print_at(row, 3, "%s", utf8_truncate(hist[idx].title, lw - 5).c_str());
                        attroff(COLOR_PAIR(Color::TITLE));
                        if (!hist[idx].channel.empty() && row + 1 < ey) {
                            attron(COLOR_PAIR(Color::CHANNEL) | A_DIM);
                            print_at(row + 1, 5, "%s", utf8_truncate(hist[idx].channel, lw - 7).c_str());
                            attroff(COLOR_PAIR(Color::CHANNEL) | A_DIM);
                        }
                    }
                }
            }

            if (sy + 1 < ey) {
                attron(COLOR_PAIR(Color::STATS) | A_BOLD);
                print_at(sy + 1, lw + 2, "%s", utf8_truncate("Subscribed Channels", rw - 4).c_str());
                attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
            }

            if (subs.empty()) {
                if (sy + 3 < ey) { attron(COLOR_PAIR(Color::BG) | A_DIM); print_at(sy + 3, lw + 3, "%s", utf8_truncate("No subscriptions yet", rw - 6).c_str()); attroff(COLOR_PAIR(Color::BG) | A_DIM); }
            } else {
                for (int i = 0; i < (int)subs.size() && i < max_items; i++) {
                    int row = sy + 2 + i;
                    if (row >= ey) break;
                    attron(COLOR_PAIR(Color::CHANNEL));
                    print_at(row, lw + 3, "%s", utf8_truncate(subs[i].title, rw - 5).c_str());
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
            print_at(iy, ix, "%s", info.c_str());
            attroff(COLOR_PAIR(Color::STATS) | A_DIM);
        }
    }
}

// ─── Results panel ────────────────────────────────────────────────────────────
void TUI::draw_results_panel(const AppState& state) {
    int sy = 6, ey = state.term_h - (state.show_status_bar ? 4 : 1), ah = ey - sy;
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

    auto col = ytcui::layout::results_columns(iw);

    const int results_scroll = std::clamp(state.results_scroll, 0, std::max(0, static_cast<int>(state.results.size())-mv));
    for (int i = 0; i < mv && (i + results_scroll) < (int)state.results.size(); i++) {
        int idx = i + results_scroll;
        int y   = iy + i;
        if (y >= ey) break;

        bool sel = (idx == state.selected_result);
        const auto& v = state.results[idx];

        // Build the composite result row using computed column widths
        std::string marker = sel ? "\xe2\x80\xba " : "  ";  // "› " or "  "
        std::string title_col = utf8_truncate(v.title, col.title);
        // Pad title to its column width
        int tw = utf8_display_width(title_col);
        while (tw < col.title) { title_col += ' '; tw++; }

        std::string row = marker + title_col;
        if (col.show_channel() && !v.channel.empty()) {
            row += ' ';
            row += utf8_truncate(v.channel, col.channel);
        }
        if (col.show_duration() && v.duration_seconds > 0) {
            int d = v.duration_seconds;
            char dbuf[16];
            if (d >= 3600) snprintf(dbuf, sizeof(dbuf), " %d:%02d:%02d", d/3600, (d%3600)/60, d%60);
            else           snprintf(dbuf, sizeof(dbuf), " %d:%02d", d/60, d%60);
            row += dbuf;
        }

        if (sel) {
            attron(sel_attr());
            print_at(y, 1, "%s", utf8_truncate(row, iw).c_str());
            // Fill remaining width
            int rw2 = utf8_display_width(utf8_truncate(row, iw));
            for (int j = rw2; j < iw; j++) addch(' ');
            attroff(sel_attr());
        } else {
            attron(COLOR_PAIR(Color::BG));
            print_at(y, 1, "%s", utf8_truncate(row, iw).c_str());
            attroff(COLOR_PAIR(Color::BG));
        }
    }

    if (rw > 4)
        draw_info_panel(state, lw, sy, rw, ah + 1, true);
}

// ─── Actions panel ────────────────────────────────────────────────────────────
void TUI::draw_actions_panel(const AppState& state) {
    if (state.results.empty()) return;
    int sy = 6, ey = state.term_h - (state.show_status_bar ? 4 : 1), ah = ey - sy;
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
            print_at(y, lw + 1, " %s", label.c_str());
            for (int j = lw2 + 1; j < iw; j++) addch(' ');
            attroff(sel_attr());
        } else {
            attron(COLOR_PAIR(Color::BG));
            print_at(y, lw + 2, "%s", label.c_str());
            attroff(COLOR_PAIR(Color::BG));
        }
    }
}

// ─── Info panel (details + thumbnail) ────────────────────────────────────────
void TUI::draw_info_panel(const AppState& state, int px, int py, int pw, int ph, bool with_thumb) {
    if (state.results.empty() || (state.selected_result < 0 || state.selected_result >= (int)state.results.size())) return;
    if (pw <= 4 || ph <= 2) return;

    const auto& v = state.results[state.selected_result];
    draw_box(py, px, ph, pw, Color::BORDER);

    int y   = py + 1;
    int x   = px + 1;
    int w   = std::max(0, pw - 2);
    int bot = py + ph - 1;

    // Thumbnail — rendered through ncurses as plain UTF-8 halfblock characters.
    // chafa --colors=none emits zero ANSI escape sequences, so put_at() handles
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
                put_at(y, x, "[loading thumbnail...]");
                attroff(COLOR_PAIR(Color::BG) | A_DIM);
                y++;
            }
        }
    }

    // Helper: print one line and advance y
    auto line = [&](int cp, const std::string& text) {
        if (y >= bot || w <= 0) return;
        attron(COLOR_PAIR(cp));
        print_at(y++, x, "%s", utf8_truncate(text, w).c_str());
        attroff(COLOR_PAIR(cp));
    };

    line(Color::TAG, v.is_live ? "[LIVE]" : "[Video]");

    // Title — word-wrap across multiple lines without chopping chars
    if (y < bot) {
        attron(COLOR_PAIR(Color::TITLE));
        std::string rem = ytcui::txt::terminal_sanitize(v.title, TermCaps::get().unicode);
        if (rem.size() > 500) rem.resize(500); // sanity cap
        int safety = 0;
        while (!rem.empty() && y < bot && safety++ < 20) {
            int rw_disp = utf8_display_width(rem);
            if (rw_disp <= w) {
                // Entire remainder fits
                print_at(y++, x, "%s", rem.c_str());
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
            print_at(y++, x, "%s", chunk.c_str());
        }
        attroff(COLOR_PAIR(Color::TITLE));
    }

    if (!v.view_count.empty()) line(Color::STATS, v.view_count + " views");
    if (!v.duration.empty())   line(Color::TITLE, "Length: " + v.duration);
    if (!v.channel.empty())    line(Color::CHANNEL, "Uploaded by " + v.channel);
    if (!v.upload_date.empty()) line(Color::PUBLISHED, "Published " + v.upload_date);
    if (!state.sponsorblock_summary.empty())
        line(Color::TAG, "SB: " + state.sponsorblock_summary);
}

// ─── Format mm:ss ─────────────────────────────────────────────────────────────
static std::string fmt_time(double secs) {
    if (secs < 0) return "--:--";
    int s = (int)secs;
    int m = s / 60; s %= 60;
    int h = m / 60; m %= 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else       snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

// ─── Message bar (with progress bar when playing) ─────────────────────────────
void TUI::draw_message_bar(const AppState& state) {
    int y = state.term_h - 3, w = state.term_w;
    if (y < 0 || w < 4) return;
    int bc = state.is_playing ? (state.is_paused ? Color::STATS : Color::ACCENT) : Color::BG;
    draw_box(y, 0, 3, w, bc);

    if (state.is_playing && !state.now_playing.empty()) {
        // Line 1: title + status
        std::string label = state.is_paused ? "[||] " : "[>] ";
        std::string title_line = label + state.now_playing;

        // Volume indicator on the right
        char vol_buf[16];
        snprintf(vol_buf, sizeof(vol_buf), " Vol:%d%%", state.playback_vol);
        int title_max = w - 2 - (int)strlen(vol_buf);
        if (title_max < 10) title_max = w - 2;

        attron(COLOR_PAIR(Color::BG));
        print_at(y + 1, 1, "%s", utf8_truncate(title_line, title_max).c_str());

        if (title_max < w - 2) {
            print_at(y + 1, w - 1 - (int)strlen(vol_buf), "%s", vol_buf);
        }
        attroff(COLOR_PAIR(Color::BG));

        // Line 2: progress bar (if we have duration info)
        if (state.playback_dur > 0.5) {
            std::string elapsed = fmt_time(state.playback_pos);
            std::string total   = fmt_time(state.playback_dur);
            std::string times   = elapsed + "/" + total;

            int bar_start = 2;
            int bar_end   = w - 2 - (int)times.size() - 1;
            int bar_w     = bar_end - bar_start;

            if (bar_w >= 10) {
                double frac = state.playback_pos / state.playback_dur;
                if (frac < 0) frac = 0;
                if (frac > 1) frac = 1;
                int filled = (int)(frac * bar_w);

                // Draw the bar
                attron(COLOR_PAIR(Color::ACCENT));
                put_at(y + 2, bar_start - 1, bd_.bl);
                for (int i = 0; i < bar_w; i++) {
                    if (i < filled)
                        mvaddch(y + 2, bar_start + i, '#');
                    else
                        put_at(y + 2, bar_start + i, bd_.h);
                }
                attroff(COLOR_PAIR(Color::ACCENT));

                // Time stamps
                attron(COLOR_PAIR(Color::STATS));
                print_at(y + 2, bar_end + 1, "%s", times.c_str());
                attroff(COLOR_PAIR(Color::STATS));
            }
        }
    } else if (!state.status_message.empty() && state.status_message.size() >= 2
               && state.status_message.substr(0, 2) != "__") {
        attron(COLOR_PAIR(Color::BG));
        // Reserve space for timer indicator on the right
        std::string timer_tag = state.timer_status.empty() ? "" : " \xe2\x8f\xb1 " + state.timer_status;
        int msg_max = w - 2 - (int)timer_tag.size();
        if (msg_max < 4) msg_max = w - 2;
        print_at(y + 1, 1, "%s", utf8_truncate(state.status_message, msg_max).c_str());
        attroff(COLOR_PAIR(Color::BG));
        if (!timer_tag.empty() && msg_max < w - 2) {
            attron(COLOR_PAIR(Color::STATS) | A_DIM);
            print_at(y + 1, w - 1 - (int)timer_tag.size(), "%s", timer_tag.c_str());
            attroff(COLOR_PAIR(Color::STATS) | A_DIM);
        }
    }
    // Queue count indicator (bottom-left when queue is non-empty)
    if (!state.queue_items.empty()) {
        attron(COLOR_PAIR(Color::CHANNEL) | A_DIM);
        std::string qtag = " Q:" + std::to_string(state.queue_items.size()) + " ";
        print_at(y + 2, 1, "%s", qtag.c_str());
        attroff(COLOR_PAIR(Color::CHANNEL) | A_DIM);
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
    print_at(py + 1, px + 2, "Sort by");
    attroff(COLOR_PAIR(Color::ACCENT) | A_BOLD);

    const char* sorts[]   = {"Relevance", "Upload date", "View count", "Rating"};
    const char* filters[] = {"All", "Videos", "Channels", "Playlists", "Short", "Long"};

    for (int i = 0; i < 4; i++) {
        bool sel = (state.sort_col == 0 && state.sort_row == i);
        if (sel) attron(sel_attr());
        else     attron(COLOR_PAIR(Color::BG));
        print_at(py + 2 + i, px + 2, "%-*s", half - 4, utf8_truncate(sorts[i], half - 4).c_str());
        if (sel) attroff(sel_attr());
        else     attroff(COLOR_PAIR(Color::BG));
    }

    draw_box(py, px + half, ph, half, state.sort_col == 1 ? Color::ACCENT : Color::BORDER);
    attron(COLOR_PAIR(Color::ACCENT) | A_BOLD);
    print_at(py + 1, px + half + 2, "Filter");
    attroff(COLOR_PAIR(Color::ACCENT) | A_BOLD);

    for (int i = 0; i < 6 && i < ph - 3; i++) {
        bool sel = (state.sort_col == 1 && state.sort_row == i);
        if (sel) attron(sel_attr());
        else     attron(COLOR_PAIR(Color::BG));
        print_at(py + 2 + i, px + half + 2, "%-*s", half - 4, utf8_truncate(filters[i], half - 4).c_str());
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
    print_at(py, px + std::max(0, (pw - 16) / 2), " Save to Library ");
    attroff(COLOR_PAIR(Color::STATS) | A_BOLD);

    const char* opts[] = {"Bookmark only (web shortcut)", "Download (MP4 + MP3) -> ~/Downloads"};
    for (int i = 0; i < 2; i++) {
        int oy = py + 2 + i;
        if (oy >= state.term_h) break;
        if (i == state.save_prompt_idx) {
            attron(sel_attr());
            print_at(oy, px + 2, " %-*s", pw - 5, utf8_truncate(opts[i], pw - 6).c_str());
            attroff(sel_attr());
        } else {
            attron(COLOR_PAIR(Color::BG));
            print_at(oy, px + 3, "%s", utf8_truncate(opts[i], pw - 6).c_str());
            attroff(COLOR_PAIR(Color::BG));
        }
    }
    attron(COLOR_PAIR(Color::BG) | A_DIM);
    print_at(py + ph - 1, px + 2, "%s", utf8_truncate(" Enter=select  Esc=cancel ", pw - 4).c_str());
    attroff(COLOR_PAIR(Color::BG) | A_DIM);
}

// ─── Browser picker popup ─────────────────────────────────────────────────────
void TUI::draw_browser_popup(const AppState& state) {
    if (state.browser_choices.empty()) return;
    int pw = std::min(40, state.term_w - 4);
    int ph = std::min(static_cast<int>(state.browser_choices.size())+4, state.term_h-2);
    if (pw < 10 || ph < 4) return;
    int px = std::max(0, (state.term_w - pw) / 2);
    int py = std::max(0, (state.term_h - ph) / 2);

    for (int i = 0; i < ph && py + i < state.term_h; i++) mvhline(py + i, px, ' ', pw);
    draw_box(py, px, ph, pw, Color::STATS);

    attron(COLOR_PAIR(Color::STATS) | A_BOLD);
    print_at(py, px + std::max(0, (pw - 16) / 2), " Select Browser ");
    attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
    attron(COLOR_PAIR(Color::BG) | A_DIM);
    print_at(py + 1, px + 2, "%s", utf8_truncate("Pick your YouTube browser:", pw - 4).c_str());
    attroff(COLOR_PAIR(Color::BG) | A_DIM);

    const int first = std::max(0, state.browser_pick_idx-(ph-3)+1);
    for (int i = 0; i < ph-3 && i+first < static_cast<int>(state.browser_choices.size()); ++i) {
        int row = py + 2 + i;
        if (row >= state.term_h) break;
        if (i+first == state.browser_pick_idx) {
            attron(sel_attr());
            print_at(row, px + 2, " %-*s", pw - 5, utf8_truncate(state.browser_choices[i+first], pw - 6).c_str());
            attroff(sel_attr());
        } else {
            attron(COLOR_PAIR(Color::BG));
            print_at(row, px + 3, "%s", utf8_truncate(state.browser_choices[i+first], pw - 6).c_str());
            attroff(COLOR_PAIR(Color::BG));
        }
    }
    attron(COLOR_PAIR(Color::BG) | A_DIM);
    print_at(py + ph - 1, px + 2, "%s", utf8_truncate(" Enter=select  Esc=cancel ", pw - 4).c_str());
    attroff(COLOR_PAIR(Color::BG) | A_DIM);
}

// ─── Box drawing ──────────────────────────────────────────────────────────────
// Uses the Border glyph set (Unicode in UTF-8 locales, ASCII otherwise) —
// never ACS_* (see tui.h).
// Fill a rectangle with blanks in the given colour pair so a popup is OPAQUE —
// otherwise whatever's underneath shows through the "box" (which only draws a
// border) and text from the layer below spills into it.
void TUI::fill_rect(int y, int x, int h, int w, int cp) {
    int rows, cols; getmaxyx(stdscr, rows, cols);
    if (w < 1 || h < 1 || x < 0 || y < 0 || x >= cols || y >= rows) return;
    w = std::min(w, cols-x); h = std::min(h, rows-y);
    attron(COLOR_PAIR(cp));
    for (int r = 0; r < h; ++r) mvhline(y+r, x, ' ', w);
    attroff(COLOR_PAIR(cp));
}

void TUI::draw_box(int y, int x, int h, int w, int cp) {
    int rows, cols; getmaxyx(stdscr, rows, cols);
    w = std::min(w, cols-x); h = std::min(h, rows-y);
    if (w < 2 || h < 2 || x < 0 || y < 0) return;
    attron(COLOR_PAIR(cp));
    put_at(y, x, bd_.tl);
    hline_g(y, x + 1, w - 2);
    put_at(y, x + w - 1, bd_.tr);
    for (int i = 1; i < h - 1; i++) {
        put_at(y + i, x,         bd_.v);
        put_at(y + i, x + w - 1, bd_.v);
    }
    put_at(y + h - 1, x, bd_.bl);
    hline_g(y + h - 1, x + 1, w - 2);
    put_at(y + h - 1, x + w - 1, bd_.br);
    attroff(COLOR_PAIR(cp));
}

// ─── Playlists tab — list of all playlists ────────────────────────────────────
void TUI::draw_playlists_tab(const AppState& state, const Library* lib) {
    int sy = 6, ey = state.term_h - (state.show_status_bar ? 4 : 1), ah = ey - sy;
    if (ah < 3 || state.term_w < 4 || !lib) return;

    const auto& pls = lib->playlists();
    draw_box(sy, 0, ah + 1, state.term_w, Color::BG);

    bool focused = (state.focus == Panel::PlaylistList);

    // Header row
    if (sy + 1 < ey) {
        attron(COLOR_PAIR(Color::STATS) | A_BOLD);
        print_at(sy + 1, 2, "Playlists (%d)", (int)pls.size());
        attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
        if (sy + 1 < ey) {
            attron(COLOR_PAIR(Color::BG) | A_DIM);
            print_at(sy + 1, state.term_w - 18, "[n] new playlist");
            attroff(COLOR_PAIR(Color::BG) | A_DIM);
        }
    }

    if (pls.empty()) {
        if (sy + 3 < ey) {
            attron(COLOR_PAIR(Color::BG) | A_DIM);
            print_at(sy + 3, 4, "No playlists yet (._.)");;
            print_at(sy + 4, 4, "Search for a video, open its actions, and choose \"Add to playlist\".");
            attroff(COLOR_PAIR(Color::BG) | A_DIM);
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
        print_at(sy + 2, 3, "%-*s", name_w, "Name");
        if (count_w > 0) print_at(sy + 2, 3 + name_w + 2, "Videos");
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

        // Color tag dot (● in 256-color, hidden in mono)
        bool has_tag = (!mono_mode_ && pl.color_tag >= 0 && pl.color_tag < 7);
        int  dot_w   = has_tag ? 2 : 0;   // "● " takes 2 columns

        if (has_tag) {
            int cp = Color::ACCENT;
            if (thumb_color_ready_ && thumb_pairs_ > 0) {
                int cidx = kPlaylistTagColors[pl.color_tag];
                int slot = (thumb_pairs_ == 256) ? cidx : cidx * thumb_pairs_ / 256;
                if (slot >= thumb_pairs_) slot = thumb_pairs_ - 1;
                cp = THUMB_COLOR_BASE + slot;
            }
            attron(COLOR_PAIR(cp) | A_BOLD);
            print_at(row, 2, "\xe2\x97\x8f ");  // UTF-8: ●
            attroff(COLOR_PAIR(cp) | A_BOLD);
        }

        int name_x = 2 + dot_w;
        int avail  = name_w - dot_w - 1;

        if (sel) {
            attron(sel_attr());
            print_at(row, name_x, " %-*s", avail, utf8_truncate(pl.name, avail).c_str());
            if (count_w > 0) print_here("  %*d ", count_w - 3, (int)pl.videos.size());
            attroff(sel_attr());
        } else {
            attron(COLOR_PAIR(Color::TITLE));
            print_at(row, name_x + 1, "%s", utf8_truncate(pl.name, avail).c_str());
            attroff(COLOR_PAIR(Color::TITLE));
            if (count_w > 0) {
                attron(COLOR_PAIR(Color::STATS) | A_DIM);
                print_at(row, 3 + name_w + 2, "%d", (int)pl.videos.size());
                attroff(COLOR_PAIR(Color::STATS) | A_DIM);
            }
        }
    }

    // Hint bar
    if (state.show_hints && ey - 1 > sy + 2) {
        attron(COLOR_PAIR(Color::BG) | A_DIM);
        print_at(ey - 1, 2, "Enter=open  j/k=navigate  n=new  c=color");
        attroff(COLOR_PAIR(Color::BG) | A_DIM);
    }
}

// ─── Playlist view — video list inside a playlist ────────────────────────────
void TUI::draw_playlist_view(const AppState& state, const Library* lib) {
    int sy = 6, ey = state.term_h - (state.show_status_bar ? 4 : 1), ah = ey - sy;
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
        print_at(sy + 1, 2, "%s", utf8_truncate(hdr, lw - 4).c_str());
        attroff(COLOR_PAIR(Color::STATS) | A_BOLD);
    }

    if (pl->videos.empty()) {
        if (sy + 3 < ey) {
            attron(COLOR_PAIR(Color::BG) | A_DIM);
            print_at(sy + 3, 3, "%s", utf8_truncate("No videos yet (._.)", lw - 6).c_str());
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
                print_at(row, 1, " %s", title.c_str());
                for (int j = tw2 + 1; j < iw; j++) addch(' ');
                attroff(sel_attr());
            } else {
                attron(COLOR_PAIR(Color::TITLE));
                print_at(row, 2, "%s", title.c_str());
                attroff(COLOR_PAIR(Color::TITLE));
            }
        }
    }

    // Right panel: info for selected video
    if (rw > 4 && !pl->videos.empty() &&
        state.playlist_video_idx >= 0 && state.playlist_video_idx < (int)pl->videos.size()) {
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
            print_at(y++, x, "%s", utf8_truncate(v.title, w2).c_str());
            attroff(COLOR_PAIR(Color::TITLE) | A_BOLD);
        }
        if (y < bot && !v.channel.empty()) {
            attron(COLOR_PAIR(Color::CHANNEL));
            print_at(y++, x, "%s", utf8_truncate(v.channel, w2).c_str());
            attroff(COLOR_PAIR(Color::CHANNEL));
        }
        if (y < bot && v.duration_seconds > 0) {
            attron(COLOR_PAIR(Color::STATS));
            print_at(y++, x, "%d:%02d", v.duration_seconds / 60, v.duration_seconds % 60);
            attroff(COLOR_PAIR(Color::STATS));
        }
        if (state.show_hints && bot - 1 > y) {
            attron(COLOR_PAIR(Color::BG) | A_DIM);
            print_at(bot - 1, x, "%s", utf8_truncate("Enter=actions  Esc=back", w2).c_str());
            attroff(COLOR_PAIR(Color::BG) | A_DIM);
        }
    }
}

// ─── Playlist action panel ────────────────────────────────────────────────────
void TUI::draw_playlist_actions_panel(const AppState& state, const Library* lib) {
    if (!lib) return;
    const Playlist* pl = lib->get_playlist(state.current_playlist_id);
    if (!pl || pl->videos.empty()) return;

    int sy = 6, ey = state.term_h - (state.show_status_bar ? 4 : 1), ah = ey - sy;
    if (ah < 3 || state.term_w < 4) return;

    int lw = state.term_w * 30 / 100;
    if (lw < 15) lw = 15;
    if (lw > state.term_w - 4) lw = state.term_w - 4;
    int rw = state.term_w - lw;

    // Left: info panel for selected video
    draw_box(sy, 0, ah + 1, lw, Color::BORDER);
    if (state.playlist_video_idx >= 0 && state.playlist_video_idx < (int)pl->videos.size()) {
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
            print_at(y++, x, "%s", utf8_truncate(v.title, w2).c_str());
            attroff(COLOR_PAIR(Color::TITLE) | A_BOLD);
        }
        if (y < bot && !v.channel.empty()) {
            attron(COLOR_PAIR(Color::CHANNEL));
            print_at(y++, x, "%s", utf8_truncate(v.channel, w2).c_str());
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
            print_at(row, lw + 1, " %s", lbl.c_str());
            for (int j = llen + 1; j < iw2; j++) addch(' ');
            attroff(sel_attr());
        } else {
            attron(COLOR_PAIR(Color::BG));
            print_at(row, lw + 2, "%s", lbl.c_str());
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
    print_at(py, px + std::max(0, (pw - (int)title.size()) / 2), "%s", title.c_str());
    attroff(COLOR_PAIR(Color::STATS) | A_BOLD);

    int max_items = ph - 3;
    const int first = std::max(0, state.playlist_pick_idx-max_items+1);
    for (int i = 0; i+first < (int)state.playlist_names.size() && i < max_items; i++) {
        int row = py + 1 + i;
        if (row >= state.term_h) break;
        bool sel = (i+first == state.playlist_pick_idx);
        bool is_create = (i+first == 0);

        if (sel) {
            attron(sel_attr());
            print_at(row, px + 1, " %-*s ", pw - 4, utf8_truncate(state.playlist_names[i+first], pw - 5).c_str());
            attroff(sel_attr());
        } else if (is_create) {
            attron(COLOR_PAIR(Color::ACCENT));
            print_at(row, px + 2, "%s", utf8_truncate(state.playlist_names[i+first], pw - 4).c_str());
            attroff(COLOR_PAIR(Color::ACCENT));
        } else {
            attron(COLOR_PAIR(Color::BG));
            print_at(row, px + 2, "%s", utf8_truncate(state.playlist_names[i+first], pw - 4).c_str());
            attroff(COLOR_PAIR(Color::BG));
        }
    }

    attron(COLOR_PAIR(Color::BG) | A_DIM);
    print_at(py + ph - 1, px + 2, "Enter=select  Esc=cancel");
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
    print_at(py, px + std::max(0, (pw - (int)title.size()) / 2), "%s", title.c_str());
    attroff(COLOR_PAIR(Color::STATS) | A_BOLD);

    attron(COLOR_PAIR(Color::BG));
    print_at(py + 1, px + 2, "Name:");
    attroff(COLOR_PAIR(Color::BG));

    // Input field
    int ix = px + 8, iw2 = pw - 10;
    std::string disp = utf8_truncate(state.new_playlist_name, iw2);
    int dw = utf8_display_width(disp);

    attron(COLOR_PAIR(Color::SEARCH_BOX));
    print_at(py + 2, ix, "%s", disp.c_str());
    for (int i = dw; i < iw2; i++) addch('_');
    attroff(COLOR_PAIR(Color::SEARCH_BOX));

    // Cursor blink
    mvchgat(py + 2, ix + std::min(dw, iw2 - 1), 1, A_REVERSE, 0, nullptr);

    attron(COLOR_PAIR(Color::BG) | A_DIM);
    print_at(py + ph - 1, px + 2, "Enter=create  Esc=cancel");
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
    print_at(y, x, "%s", t.c_str());
    if (attr) attroff(attr);
}

// Title row + a hairline rule, in theme colours. Returns nothing; content
// starts at y=2.
void TUI::stream_header(const AppState& state, const std::string& title) {
    int W = state.term_w;
    attron(COLOR_PAIR(Color::ACCENT) | A_BOLD);
    print_at(0, 1, "%s", utf8_truncate("♪ " + title, W - 2).c_str());
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

// ─── In-app settings UI (Ctrl-S) ──────────────────────────────────────────────
void TUI::draw_settings(const AppState& state) {
    // Adapt to the terminal: full-ish width, but never overflow. On very
    // narrow (streamlined) terminals we shrink to fit rather than spilling.
    int pw = 60, ph = 22;
    if (pw > state.term_w - 2) pw = state.term_w - 2;
    if (ph > state.term_h - 2) ph = state.term_h - 2;
    if (pw < 20 || ph < 8) {          // too small to draw a useful panel
        // Minimal fallback: a one-line hint so the user isn't stuck.
        fill_rect(state.term_h / 2, 0, 1, state.term_w, Color::BG);
        attron(COLOR_PAIR(Color::ACCENT) | A_BOLD);
        print_at(state.term_h / 2, 1, "%s",
                 utf8_truncate("Settings need a wider terminal", state.term_w - 2).c_str());
        attroff(COLOR_PAIR(Color::ACCENT) | A_BOLD);
        return;
    }
    int px = std::max(0, (state.term_w - pw) / 2);
    int py = std::max(0, (state.term_h - ph) / 2);

    fill_rect(py, px, ph, pw, Color::BG);   // opaque background
    draw_box(py, px, ph, pw, Color::BG);

    // Title
    attron(COLOR_PAIR(Color::HEADER) | A_BOLD);
    print_at(py, px + (pw - 10) / 2, " Settings ");
    attroff(COLOR_PAIR(Color::HEADER) | A_BOLD);

    // Tab bar  (5 tabs). On narrow panels use a compact single-line form.
    static const char* const kSettingsTabs[]      = {"Theme","Accelerators","General","Timer","About"};
    static const char* const kSettingsTabsShort[] = {"Thm",  "Keys",        "Gen",    "Tmr",  "Abt"};
    static const int kNSettingsTabs = 5;
    if (pw >= 48) {
        // Full tab bar
        int tx = px + 2;
        for (int t = 0; t < kNSettingsTabs; t++) {
            bool active = (state.settings_tab == t);
            if (active) attron(COLOR_PAIR(Color::ACCENT) | A_BOLD | A_REVERSE);
            else        attron(COLOR_PAIR(Color::STATS));
            print_at(py + 1, tx, " %s ", kSettingsTabs[t]);
            if (active) attroff(COLOR_PAIR(Color::ACCENT) | A_BOLD | A_REVERSE);
            else        attroff(COLOR_PAIR(Color::STATS));
            tx += (int)strlen(kSettingsTabs[t]) + 3;
        }
    } else {
        // Compact: short tab names, fit in pw
        int tx = px + 1;
        for (int t = 0; t < kNSettingsTabs && tx + 5 < px + pw - 1; t++) {
            bool active = (state.settings_tab == t);
            if (active) attron(COLOR_PAIR(Color::ACCENT) | A_BOLD | A_REVERSE);
            else        attron(COLOR_PAIR(Color::STATS));
            print_at(py + 1, tx, " %s ", kSettingsTabsShort[t]);
            if (active) attroff(COLOR_PAIR(Color::ACCENT) | A_BOLD | A_REVERSE);
            else        attroff(COLOR_PAIR(Color::STATS));
            tx += (int)strlen(kSettingsTabsShort[t]) + 3;
        }
    }

    int list_top = py + 3;
    int list_rows = ph - 5;   // leave room for title, tabs, footer

    if (state.settings_tab == 0) {
        // ── Theme tab: scrollable list, live-applies on Enter/move ──
        int n = (int)state.settings_theme_names.size();
        int sel = state.settings_sel;
        int first = 0;
        if (sel >= list_rows) first = sel - list_rows + 1;
        if (first < 0) first = 0;

        for (int i = 0; i < list_rows && first + i < n; i++) {
            int idx = first + i;
            int y = list_top + i;
            bool cur = (idx == sel);
            const std::string& name = state.settings_theme_names[idx];
            if (cur) {
                paint_sel_bar(y, px + 1, pw - 2);
                attron(sel_attr());
                print_at(y, px + 2, "%-*s", pw - 4, name.c_str());
                attroff(sel_attr());
            } else {
                attron(COLOR_PAIR(Color::BG));
                print_at(y, px + 3, "%s", name.c_str());
                attroff(COLOR_PAIR(Color::BG));
            }
        }
    } else if (state.settings_tab == 1) {
        // ── Accelerators tab: label ............ key ──
        int n = (int)state.settings_accel_labels.size();
        int sel = state.settings_sel;
        int first = 0;
        if (sel >= list_rows) first = sel - list_rows + 1;
        if (first < 0) first = 0;

        for (int i = 0; i < list_rows && first + i < n; i++) {
            int idx = first + i;
            int y = list_top + i;
            bool cur = (idx == sel);
            const std::string& label = state.settings_accel_labels[idx];
            const std::string& key   = (idx < (int)state.settings_accel_keys.size())
                                        ? state.settings_accel_keys[idx] : "";
            int keycol = px + pw - 3 - (int)key.size();
            if (cur) {
                paint_sel_bar(y, px + 1, pw - 2);
                attron(sel_attr());
                print_at(y, px + 2, "%s", label.c_str());
                print_at(y, keycol, "%s", key.c_str());
                attroff(sel_attr());
            } else {
                attron(COLOR_PAIR(Color::BG));
                print_at(y, px + 3, "%s", label.c_str());
                attroff(COLOR_PAIR(Color::BG));
                attron(COLOR_PAIR(Color::ACCENT) | A_BOLD);
                print_at(y, keycol, "%s", key.c_str());
                attroff(COLOR_PAIR(Color::ACCENT) | A_BOLD);
            }
        }

    } else if (state.settings_tab == 2) {
        // ── General tab: backend, UI mode, SponsorBlock, resume, playlist, loop ─
        struct Row { std::string label; std::string value; };
        Row rows[] = {
            { "Backend",           state.active_backend == BackendKind::YtcuiDl ? "ytcui-dl  (native)"
                                 : state.active_backend == BackendKind::YtcuiWe  ? "ytcui-we"
                                 :                                                  "yt-dlp" },
            { "UI mode",           state.config_mode },
            { "SponsorBlock",      state.sponsorblock_enabled ? "on" : "off" },
            { "Resume playback",   state.resume_playback ? "on" : "off" },
            { "Loop playlist",     state.loop_playlist   ? "on" : "off" },
            { "Streamlined loop",  state.streamlined_loop ? "on" : "off" },
            { "Status bar",        state.show_status_bar ? "on" : "off" },
            { "Hints",             state.show_hints ? "on" : "off" },
        };
        int nrows = (int)(sizeof(rows)/sizeof(rows[0]));
        int sel = state.settings_sel;
        for (int i = 0; i < nrows && i < list_rows; i++) {
            int y = list_top + i;
            bool cur = (i == sel);
            std::string rowtext = rows[i].label;
            while ((int)rowtext.size() < 20) rowtext += '.';
            rowtext += "  " + rows[i].value;
            if (cur) { paint_sel_bar(y, px+1, pw-2); attron(sel_attr()); }
            else attron(COLOR_PAIR(Color::BG));
            print_at(y, px + 2, "%-*s", pw - 4, utf8_truncate(rowtext, pw - 4).c_str());
            if (cur) attroff(sel_attr()); else attroff(COLOR_PAIR(Color::BG));
        }
        attron(COLOR_PAIR(Color::BG) | A_DIM);
        print_at(list_top + nrows + 1, px + 2, "%s",
            utf8_truncate("Enter: cycle/toggle  Ctrl-B: backend  Ctrl-S/q: close", pw-4).c_str());
        attroff(COLOR_PAIR(Color::BG) | A_DIM);

    } else if (state.settings_tab == 3) {
        // ── Timer tab ────────────────────────────────────────────────────────
        int y = list_top;
        auto draw_row = [&](bool sel, const std::string& text) {
            if (sel) { paint_sel_bar(y, px+1, pw-2); attron(sel_attr()); }
            else attron(COLOR_PAIR(Color::BG));
            print_at(y, px + 2, "%-*s", pw - 4, utf8_truncate(text, pw - 4).c_str());
            if (sel) attroff(sel_attr()); else attroff(COLOR_PAIR(Color::BG));
            y++;
        };
        std::string timer_row = "Sleep timer......  ";
        timer_row += state.timer_status.empty() ? "off" : state.timer_status;
        draw_row(state.settings_sel == 0, timer_row);
        std::string loop_row  = std::string("Loop on end......  ") + (state.loop_enabled ? "on" : "off");
        draw_row(state.settings_sel == 1, loop_row);
        attron(COLOR_PAIR(Color::BG) | A_DIM);
        print_at(y + 1, px + 2, "%s",
            utf8_truncate("Enter: set timer / toggle loop   Ctrl-T: timer", pw-4).c_str());
        attroff(COLOR_PAIR(Color::BG) | A_DIM);

    } else if (state.settings_tab == 4) {
        // ── About tab ────────────────────────────────────────────────────────
        int y = list_top;
        attron(COLOR_PAIR(Color::TITLE) | A_BOLD);
        print_at(y++, px + 2, "ytcui %s", VERSION);
        attroff(COLOR_PAIR(Color::TITLE) | A_BOLD);
        attron(COLOR_PAIR(Color::BG));
        print_at(y++, px + 2, "A terminal YouTube client.");
        y++;
        print_at(y++, px + 2, "github.com/MilkmanAbi/ytcui");
        print_at(y++, px + 2, "github.com/MilkmanAbi/ytcui-dl");
        y++;
        print_at(y++, px + 2, "MIT License");
        attroff(COLOR_PAIR(Color::BG));
    }

    // Footer: toast (if any) else the key hints
    attron(COLOR_PAIR(Color::STATS));
    if (!state.settings_toast.empty()) {
        print_at(py + ph - 2, px + 2, "%s",
                 utf8_truncate(state.settings_toast, pw - 4).c_str());
    } else if (state.settings_capturing) {
        print_at(py + ph - 2, px + 2, "press a key to bind  (Esc cancels)");
    }
    static const char* hints[5] = {
        "Tab: switch   j/k: move   Enter: apply   Ctrl-S/q: close",
        "Tab: switch   j/k: move   Enter: rebind  Ctrl-S/q: close",
        "Tab: switch   j/k: move   Enter: cycle   Ctrl-S/q: close",
        "Tab: switch   j/k: move   Enter: toggle  Ctrl-S/q: close",
        "Tab: switch   Ctrl-S/q: close",
    };
    int ti = std::clamp(state.settings_tab, 0, 4);
    if (state.show_hints)
        print_at(py + ph - 1, px + 2, "%s", utf8_truncate(hints[ti], pw - 4).c_str());
    attroff(COLOR_PAIR(Color::STATS));
}

// ─── Shortcuts reference panel ─────────────────────────────────────────────────
void TUI::draw_shortcuts(const AppState& state) {
    struct KB { const char* key; const char* action; };
    static const KB bindings[] = {
        // navigation
        {"j / Down",  "move down"},
        {"k / Up",    "move up"},
        {"l / Right / Enter", "select / open actions"},
        {"h / Left / Backspace", "back"},
        {"g",         "jump to top"},
        {"G",         "jump to bottom"},
        {"Tab",       "cycle focus (search -> tabs -> results)"},
        {"Escape",    "navigate outward / cancel"},
        {"/",         "focus search"},
        // playback
        {"Space / p",     "pause / resume (instant via mpv IPC)"},
        {"+",         "volume up (+5%)"},
        {"-",         "volume down (-5%)"},
        {">",         "seek forward 10s"},
        {"<",         "seek backward 10s"},
        // v5 features
        {"Ctrl-T",    "sleep timer (set / cancel)"},
        {"Ctrl-R",    "toggle loop on end"},
        {"Ctrl-Q",    "queue view"},
        {"Ctrl-B",    "cycle backend (ytcui-dl → yt-dlp → ytcui-we)"},
        {"Ctrl-S",    "settings panel"},
        // actions
        {"r",         "resolution selector"},
        {"s",         "sort menu"},
        {"n",         "new playlist (in playlist view)"},
        {"c",         "cycle playlist color tag (in playlists)"},
        {"q",         "quit"},
        {"?",         "this panel"},
    };
    int n = sizeof(bindings) / sizeof(bindings[0]);
    int pw = 52, ph = n + 4;
    int px = std::max(0, (state.term_w - pw) / 2);
    int py = std::max(0, (state.term_h - ph) / 2);
    if (pw > state.term_w - 2) pw = state.term_w - 2;
    if (ph > state.term_h - 2) ph = state.term_h - 2;

    // Draw border
    fill_rect(py, px, ph, pw, Color::BG);   // opaque background
    draw_box(py, px, ph, pw, Color::BG);
    attron(COLOR_PAIR(Color::HEADER) | A_BOLD);
    print_at(py, px + (pw - 12) / 2, " Shortcuts ");
    attroff(COLOR_PAIR(Color::HEADER) | A_BOLD);

    // Draw bindings
    int max_rows = ph - 3;
    for (int i = 0; i < n && i < max_rows; i++) {
        attron(COLOR_PAIR(Color::ACCENT) | A_BOLD);
        print_at(py + 2 + i, px + 2, "%-22s", bindings[i].key);
        attroff(COLOR_PAIR(Color::ACCENT) | A_BOLD);
        attron(COLOR_PAIR(Color::STATS));
        print_at(py + 2 + i, px + 24, "%s", bindings[i].action);
        attroff(COLOR_PAIR(Color::STATS));
    }

    attron(COLOR_PAIR(Color::STATS));
    print_at(py + ph - 1, px + (pw - 22) / 2, " press any key to close ");
    attroff(COLOR_PAIR(Color::STATS));
}

void TUI::render_streamlined(const AppState& state, const Library* lib) {
    switch ((StreamScreen)state.stream_screen) {
        case StreamScreen::Menu:    stream_menu(state, lib); break;
        case StreamScreen::Search:  stream_search(state);    break;
        case StreamScreen::Browse:  stream_browse(state);    break;
        case StreamScreen::Actions:     stream_actions(state);      break;
        case StreamScreen::Playing:     stream_playing(state);      break;
        case StreamScreen::MoreActions: stream_more_actions(state); break;
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
            print_at(y, 2, "%s", utf8_truncate(row, W - 4).c_str());
            attroff(sel_attr() | A_BOLD);
        } else {
            attron(COLOR_PAIR(Color::TITLE));
            print_at(y, 3, "%s", utf8_truncate(row, W - 5).c_str());
            attroff(COLOR_PAIR(Color::TITLE));
        }
    }
    if (state.show_hints && H - 2 > top + n)
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
    print_at(by + 1, 3, "%s", q.c_str());
    attroff(COLOR_PAIR(Color::TITLE));
    mvchgat(by + 1, 3 + std::min(utf8_display_width(q), iw - 1), 1, A_REVERSE, 0, nullptr);
    if (state.show_hints)
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
                    print_at(y, 2, "%s", utf8_truncate(names[idx], W - 4).c_str());
                    attroff(sel_attr() | A_BOLD);
                } else { attron(COLOR_PAIR(Color::TITLE));
                    print_at(y, 3, "%s", utf8_truncate(names[idx], W - 5).c_str());
                    attroff(COLOR_PAIR(Color::TITLE)); }
            }
        }
        if (state.show_hints)
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
        if (state.show_hints)
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
            print_at(y, 2, "%s", utf8_truncate(v.title, W - 4).c_str());
            attroff(sel_attr() | A_BOLD);
        } else {
            attron(COLOR_PAIR(Color::TITLE));
            print_at(y, 2, "%s", utf8_truncate(v.title, W - 3).c_str());
            attroff(COLOR_PAIR(Color::TITLE));
        }
        if (y + 1 < H - 2) {
            std::string meta = v.channel;
            std::string dur = v.is_live ? "LIVE" : v.duration;
            if (!dur.empty() && dur != "0:00") meta += "  ·  " + dur;
            else if (v.is_live) meta += "  ·  LIVE";
            attron(COLOR_PAIR(Color::CHANNEL) | A_DIM);
            print_at(y + 1, 3, "%s", utf8_truncate(meta, W - 4).c_str());
            attroff(COLOR_PAIR(Color::CHANNEL) | A_DIM);
        }
    }
    if (state.show_hints)
        center_text(H - 2, W, "⏎ play · esc back", COLOR_PAIR(Color::BG) | A_DIM);
    stream_status(state);
}

// Play-mode chooser for the highlighted video.
void TUI::stream_actions(const AppState& state) {
    int W = state.term_w, H = state.term_h;
    stream_header(state, "Play");
    bool uni = TermCaps::get().unicode;

    const Video* v = (!state.results.empty() &&
                      state.selected_result >= 0 && state.selected_result < (int)state.results.size())
                     ? &state.results[state.selected_result] : nullptr;
    int y = std::max(3, H / 2 - 3);
    if (v) { center_text(y, W, v->title, COLOR_PAIR(Color::TITLE) | A_BOLD);
             center_text(y + 1, W, v->channel, COLOR_PAIR(Color::CHANNEL) | A_DIM); }
    y += 3;

    const char* labels[3] = { uni ? "▶  Play video" : "Play video",
                              uni ? "♪  Play audio" : "Play audio",
                              uni ? "⋯  More..."    : "More..." };
    for (int i = 0; i < 3; i++) {
        int ry = y + i;
        bool s = (state.stream_action_sel == i);
        std::string lab = labels[i];
        int lw = utf8_display_width(lab);
        int x = (W - lw) / 2; if (x < 2) x = 2;
        if (s) {
            paint_sel_bar(ry, x - 2, lw + 4);
            attron(sel_attr() | A_BOLD); print_at(ry, x, "%s", lab.c_str()); attroff(sel_attr() | A_BOLD);
        } else {
            attron(COLOR_PAIR(Color::TITLE)); print_at(ry, x, "%s", lab.c_str()); attroff(COLOR_PAIR(Color::TITLE));
        }
    }
    if (state.show_hints)
        center_text(H - 2, W, "j/k · ⏎ select · esc back", COLOR_PAIR(Color::BG) | A_DIM);
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

    // Progress waveform + real time labels. When mpv IPC is live we show the
    // true elapsed/total and fill the bar to match; otherwise we fall back to
    // the static 0:00 / video-duration display.
    if (y < H - 5) {
        static const char* bars[] = {"▁","▂","▃","▄","▅","▆","▇","█"};
        static const int pat[] = {2,3,5,4,6,7,5,4,6,3,2,3,5,7,6,4,3,2,4,6,5,3,2,4,5,3,2,1};
        int wf = W - 6; if (wf < 4) wf = W - 2;
        int wx = (W - wf) / 2; if (wx < 1) wx = 1;

        bool live_prog = state.playback_dur > 0.5;
        double frac = live_prog ? (state.playback_pos / state.playback_dur) : 0.0;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        int filled = (int)(frac * wf);

        for (int i = 0; i < wf; i++) {
            int pidx = pat[i % (int)(sizeof(pat)/sizeof(int))];
            // Elapsed portion is bright accent; upcoming portion is dim.
            if (live_prog && i >= filled) attron(COLOR_PAIR(Color::STATS) | A_DIM);
            else                          attron(COLOR_PAIR(Color::ACCENT));
            print_at(y, wx + i, "%s", uni ? bars[pidx] : (i < filled ? "=" : "-"));
            if (live_prog && i >= filled) attroff(COLOR_PAIR(Color::STATS) | A_DIM);
            else                          attroff(COLOR_PAIR(Color::ACCENT));
        }

        attron(COLOR_PAIR(Color::STATS) | A_DIM);
        std::string elapsed = live_prog ? fmt_time(state.playback_pos) : "0:00";
        print_at(y + 1, wx, "%s", elapsed.c_str());
        std::string dur = live_prog ? fmt_time(state.playback_dur)
                        : (v.is_live ? "LIVE"
                          : (v.duration.empty() || v.duration == "0:00") ? "--:--" : v.duration);
        print_at(y + 1, wx + wf - (int)dur.size(), "%s", dur.c_str());
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
        y++;
    }
    // Timer countdown — shown prominently in the playing screen when active
    if (!state.timer_status.empty() && y < H - 1) {
        center_text(y, W, "⏱ " + state.timer_status, COLOR_PAIR(Color::TAG) | A_DIM);
    }
    if (state.show_hints)
        center_text(H - 1, W, "space·+/-·</>·Ctrl-S·b·q", COLOR_PAIR(Color::BG) | A_DIM);
}

// ─── v5 overlay: sleep timer prompt ───────────────────────────────────────────
void TUI::draw_timer_prompt(const AppState& state) {
    int pw = std::min(46, state.term_w - 4), ph = 7;
    if (pw < 12 || ph > state.term_h - 2) return;
    int px = std::max(0, (state.term_w - pw) / 2);
    int py = std::max(0, (state.term_h - ph) / 2);

    for (int i = 0; i < ph && py + i < state.term_h; i++) mvhline(py + i, px, ' ', pw);
    draw_box(py, px, ph, pw, Color::STATS);

    attron(COLOR_PAIR(Color::STATS) | A_BOLD);
    print_at(py, px + std::max(0, (pw - 14) / 2), " Sleep Timer ");
    attroff(COLOR_PAIR(Color::STATS) | A_BOLD);

    attron(COLOR_PAIR(Color::BG) | A_DIM);
    print_at(py + 1, px + 2, "%s", utf8_truncate("e.g.  30m   1h23m   45s", pw - 4).c_str());
    attroff(COLOR_PAIR(Color::BG) | A_DIM);

    // Input line
    attron(COLOR_PAIR(Color::TITLE) | A_BOLD);
    std::string prompt = "> " + state.timer_prompt_input + "_";
    print_at(py + 3, px + 2, "%s", utf8_truncate(prompt, pw - 4).c_str());
    attroff(COLOR_PAIR(Color::TITLE) | A_BOLD);

    if (state.timer_status.size()) {
        attron(COLOR_PAIR(Color::CHANNEL));
        print_at(py + 4, px + 2, "%s", utf8_truncate("Active: " + state.timer_status, pw - 4).c_str());
        attroff(COLOR_PAIR(Color::CHANNEL));
    }

    attron(COLOR_PAIR(Color::BG) | A_DIM);
    print_at(py + ph - 1, px + 2, "%s", utf8_truncate(" Enter=set  Ctrl-T=cancel  Esc=close ", pw - 4).c_str());
    attroff(COLOR_PAIR(Color::BG) | A_DIM);
}

// ─── v5 overlay: queue view ────────────────────────────────────────────────────
void TUI::draw_queue_view(const AppState& state) {
    int n  = (int)state.queue_items.size();
    int ph = std::min(n + 5, state.term_h - 4);
    int pw = std::min(60, state.term_w - 4);
    if (pw < 12 || ph < 4) return;
    int px = std::max(0, (state.term_w - pw) / 2);
    int py = std::max(0, (state.term_h - ph) / 2);

    for (int i = 0; i < ph && py + i < state.term_h; i++) mvhline(py + i, px, ' ', pw);
    draw_box(py, px, ph, pw, Color::STATS);

    attron(COLOR_PAIR(Color::STATS) | A_BOLD);
    std::string title = n == 0 ? " Queue (empty) " : " Queue (" + std::to_string(n) + ") ";
    print_at(py, px + std::max(0, (pw - (int)title.size()) / 2), "%s", title.c_str());
    attroff(COLOR_PAIR(Color::STATS) | A_BOLD);

    if (n == 0) {
        attron(COLOR_PAIR(Color::BG) | A_DIM);
        print_at(py + 2, px + 2, "Queue is empty.  Use 'Add to queue' from the action menu.");
        attroff(COLOR_PAIR(Color::BG) | A_DIM);
    } else {
        int max_rows = ph - 3;
        int scroll = 0;
        if (state.queue_sel >= scroll + max_rows) scroll = state.queue_sel - max_rows + 1;
        scroll = std::max(0, std::min(scroll, n - max_rows));
        for (int i = 0; i < max_rows && scroll + i < n; i++) {
            int idx = scroll + i;
            int row = py + 1 + i;
            if (row >= state.term_h) break;
            const auto& v = state.queue_items[idx];
            std::string line = std::to_string(idx + 1) + ". " + v.title;
            if (idx == state.queue_sel) {
                attron(sel_attr());
                print_at(row, px + 1, " %-*s", pw - 3, utf8_truncate(line, pw - 4).c_str());
                attroff(sel_attr());
            } else {
                attron(COLOR_PAIR(Color::BG));
                print_at(row, px + 2, "%s", utf8_truncate(line, pw - 4).c_str());
                attroff(COLOR_PAIR(Color::BG));
            }
        }
    }
    attron(COLOR_PAIR(Color::BG) | A_DIM);
    print_at(py + ph - 1, px + 2, "%s", utf8_truncate(" Enter=play  d/Del=remove  Ctrl-Q/Esc=close ", pw - 4).c_str());
    attroff(COLOR_PAIR(Color::BG) | A_DIM);
}

// ─── v5 overlay: share menu (QR code + URL) ───────────────────────────────────
void TUI::draw_share_menu(const AppState& state) {
    if (state.share_url.empty()) return;
    if (qr_url_ != state.share_url) {
        qr_url_ = state.share_url;
        qr_matrix_ = ytcui::share::encode(qr_url_);
    }
    const bool safe_blocks = TermCaps::get().unicode && wcwidth(L'▀') == 1 && wcwidth(L'▄') == 1 && wcwidth(L'█') == 1;
    // Apple Terminal's ambiguous-width profile and font fallback can change block geometry.
    // Spaces with explicit backgrounds work without either font or Unicode assumptions.
    bool half = state.share_mode == 2 || (state.share_mode == 0 &&
        TermCaps::get().id != TermId::AppleTerminal && TermCaps::get().blocks_native && !mono_mode_ && safe_blocks);
    if (!safe_blocks) half = false;
    auto raster = ytcui::share::rasterize(qr_matrix_, half ? ytcui::share::QrStyle::HalfBlocks : ytcui::share::QrStyle::Cells);
    const int max_w = std::max(0, state.term_w-2), max_h = std::max(0, state.term_h);
    bool fits = raster.cols > 0 && raster.cols+4 <= max_w && raster.rows+4 <= max_h;
    int pw = std::min(max_w, std::max(36, fits ? raster.cols+4 : 56));
    int ph = std::min(max_h, fits ? raster.rows+4 : 8);
    if (pw < 12 || ph < 4) return;
    int px = (state.term_w-pw)/2, py = (state.term_h-ph)/2;
    attrset(A_NORMAL);
    fill_rect(py, px, ph, pw, Color::BG);
    draw_box(py, px, ph, pw, Color::BORDER);
    print_at(py, px+2, " Share ");
    int text_y = py+2;
    if (fits) {
        const int qx = px+(pw-raster.cols)/2, qy = py+1;
        for (int y = 0; y < raster.rows; ++y) for (int x = 0; x < raster.cols; ++x) {
            unsigned char cell = raster.cells[static_cast<size_t>(y)*raster.cols+x];
            // Discard inherited bold, dim, theme colours and reverse attributes.
            attrset(qr_colors_ ? COLOR_PAIR(Color::QR) : A_REVERSE);
            if (cell == 3) { attrset(qr_colors_ ? COLOR_PAIR(Color::QR)|A_REVERSE : A_NORMAL); mvaddch(qy+y, qx+x, ' '); }
            else if (cell == 0) mvaddch(qy+y, qx+x, ' ');
            else put_at(qy+y, qx+x, cell == 2 ? "▀" : "▄");
        }
        attrset(A_NORMAL);
        text_y = qy+raster.rows;
    } else {
        print_at(text_y++, px+2, "%s", utf8_truncate(qr_matrix_.size ? "Enlarge terminal or press m for compact QR" : "URL too long for QR", pw-4).c_str());
        if (qr_matrix_.size) print_at(text_y++, px+2, "%s", utf8_truncate("Needs " + std::to_string(raster.cols+6) + " columns x " + std::to_string(raster.rows+4) + " rows", pw-4).c_str());
    }
    print_at(text_y++, px+2, "%s", utf8_truncate(state.share_url, pw-4).c_str());
    if (text_y < py+ph-1) print_at(text_y, px+2, "%s", utf8_truncate(state.share_copy_status, pw-4).c_str());
    print_at(py+ph-1, px+1, "%s", utf8_truncate(" c=copy  m=cells/compact  Esc=close ", pw-2).c_str());
    attrset(A_NORMAL);
}

void TUI::draw_res_menu(const AppState& state) {
    if (state.res_labels.empty()) return;

    int n   = (int)state.res_labels.size();
    int pw  = std::min(32, state.term_w - 4);
    int ph  = std::min(n + 4, state.term_h - 4);
    if (ph < 4 || pw < 12) return;
    int px  = std::max(0, (state.term_w - pw) / 2);
    int py  = std::max(0, (state.term_h - ph) / 2);

    for (int i = 0; i < ph && py + i < state.term_h; i++) mvhline(py + i, px, ' ', pw);
    draw_box(py, px, ph, pw, Color::HEADER);

    attron(COLOR_PAIR(Color::HEADER) | A_BOLD);
    std::string title = " Quality ";
    print_at(py, px + std::max(0, (pw - (int)title.size()) / 2), "%s", title.c_str());
    attroff(COLOR_PAIR(Color::HEADER) | A_BOLD);

    int vis   = ph - 3;
    int scroll = 0;
    if (state.res_sel >= scroll + vis) scroll = state.res_sel - vis + 1;
    if (state.res_sel < scroll)        scroll = state.res_sel;
    scroll = std::max(0, scroll);

    for (int i = 0; i < vis && scroll + i < n; i++) {
        int idx = scroll + i;
        bool sel = (idx == state.res_sel);
        int ry = py + 1 + i;
        bool is_current = idx < (int)state.res_heights.size() &&
                          state.res_heights[idx] == state.res_active_height;
        // Prefix: "> " for the currently-active resolution, "  " otherwise.
        std::string lab = (is_current ? "> " : "  ") + state.res_labels[idx];
        if (sel) {
            paint_sel_bar(ry, px + 1, pw - 2);
            attron(sel_attr() | A_BOLD);
            print_at(ry, px + 2, "%s", utf8_truncate(lab, pw - 4).c_str());
            attroff(sel_attr() | A_BOLD);
        } else {
            attron(COLOR_PAIR(Color::TITLE));
            print_at(ry, px + 2, "%s", utf8_truncate(lab, pw - 4).c_str());
            attroff(COLOR_PAIR(Color::TITLE));
        }
    }

    if (state.show_hints) {
        attron(COLOR_PAIR(Color::BG) | A_DIM);
        print_at(py + ph - 1, px + 2, "%s",
                 utf8_truncate("⏎ apply  esc cancel", pw - 4).c_str());
        attroff(COLOR_PAIR(Color::BG) | A_DIM);
    }
}

void TUI::stream_more_actions(const AppState& state) {
    int W = state.term_w, H = state.term_h;
    stream_header(state, "More");

    if (state.actions.empty()) {
        center_text(H / 2, W, "No actions available", COLOR_PAIR(Color::BG) | A_DIM);
        stream_status(state);
        return;
    }

    int n = (int)state.actions.size();
    int vis = std::max(1, H - 6);
    int scroll = 0;
    if (state.selected_action >= scroll + vis) scroll = state.selected_action - vis + 1;
    if (state.selected_action < scroll)        scroll = state.selected_action;
    scroll = std::max(0, scroll);

    for (int i = 0; i < vis && scroll + i < n; i++) {
        int idx = scroll + i;
        bool sel = (idx == state.selected_action);
        int ry = 3 + i;
        std::string lab = state.actions[idx].label;
        int lw = utf8_display_width(lab);
        int x = std::max(2, (W - lw) / 2);
        if (sel) {
            paint_sel_bar(ry, x - 2, lw + 4);
            attron(sel_attr() | A_BOLD);
            print_at(ry, x, "%s", lab.c_str());
            attroff(sel_attr() | A_BOLD);
        } else {
            attron(COLOR_PAIR(Color::TITLE));
            print_at(ry, x, "%s", lab.c_str());
            attroff(COLOR_PAIR(Color::TITLE));
        }
    }
    if (state.show_hints)
        center_text(H - 2, W, "j/k · ⏎ execute · esc back", COLOR_PAIR(Color::BG) | A_DIM);
    stream_status(state);
}

} // namespace ytui
