#include "termcaps.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <unistd.h>
#include <termios.h>
#include <langinfo.h>
#include <clocale>
#include <sys/select.h>
#include <sys/time.h>
#include <curses.h>
#include <term.h>

namespace ytui {

static std::string env_str(const char* k) {
    const char* v = getenv(k);
    return v ? std::string(v) : std::string();
}

// ── Batched terminal query ────────────────────────────────────────────────────
// Write all identification queries at once, ending with DA1 (Primary Device
// Attributes). Every conformant terminal answers DA1 with "CSI ? ... c", and
// because terminals answer in order, the DA1 reply arriving means all earlier
// replies are already in the buffer. We read until we see DA1's terminator or
// the timeout fires. The timeout is essential: minimal emulators (uterm/libtsm)
// may not answer DA1 at all, and we must not hang.
static std::string run_queries(int timeout_ms) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return "";
    struct termios old{};
    if (tcgetattr(STDIN_FILENO, &old) != 0) return "";
    struct termios raw = old;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    // DA3, DA2, XTVERSION, XTGETTCAP(TN, hex 544E), cell-size (CSI 16 t), DA1.
    static const char* q =
        "\033[=c"            // DA3  -> DCS ! | ... ST   (VTE, foot, Terminology)
        "\033[>c"            // DA2  -> CSI > Pp;Pv;Pc c (Alacritty version, type)
        "\033[>0q"           // XTVERSION -> DCS > | name ST (xterm/wez/contour/iterm/mintty)
        "\033P+q544E\033\\"  // XTGETTCAP TN -> DCS 1+r544E=<hex> ST (kitty, mlterm)
        "\033[16t"           // cell size -> CSI 6 ; h ; w t
        "\033[c";            // DA1 (ANCHOR) -> CSI ? ... c   (sixel = attr 4)
    ssize_t w = write(STDOUT_FILENO, q, strlen(q)); (void)w;

    std::string resp; char c;
    struct timeval start{}, now{}; gettimeofday(&start, nullptr);
    bool saw_da1 = false;
    for (;;) {
        fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO, &rf);
        struct timeval tv{0, 15000};
        if (select(STDIN_FILENO + 1, &rf, nullptr, nullptr, &tv) > 0) {
            if (read(STDIN_FILENO, &c, 1) == 1) {
                resp += c;
                // DA1 reply is "CSI ? ... c". Detect a 'c' that closes a "[?".
                if (c == 'c' && resp.find("[?") != std::string::npos) { saw_da1 = true; break; }
            }
        }
        gettimeofday(&now, nullptr);
        long ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
        if (ms > timeout_ms) break;
    }
    (void)saw_da1;
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    return resp;
}

// Decode a run of ASCII-hex (e.g. XTGETTCAP payload) to text.
static std::string unhex(const std::string& h) {
    std::string out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        auto v = [](char x) -> int {
            if (x >= '0' && x <= '9') return x - '0';
            if (x >= 'a' && x <= 'f') return x - 'a' + 10;
            if (x >= 'A' && x <= 'F') return x - 'A' + 10;
            return -1;
        };
        int hi = v(h[i]), lo = v(h[i + 1]);
        if (hi < 0 || lo < 0) break;
        out += (char)((hi << 4) | lo);
    }
    return out;
}

static bool contains_ci(const std::string& hay, const char* needle) {
    std::string h = hay, n = needle;
    for (auto& ch : h) ch = (char)tolower((unsigned char)ch);
    for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
    return h.find(n) != std::string::npos;
}

// Parse the combined query-reply buffer and update identity + sixel + cell px.
// Factored out so it can be unit-tested with synthetic terminal responses.
static void parse_responses(TermCaps& c, const std::string& r) {
    if (r.empty()) return;
    // XTVERSION payload: DCS > | <text> ST
    if (contains_ci(r, "iterm2"))   { c.id = TermId::ITerm2; }
    else if (contains_ci(r, "wezterm")) { c.id = TermId::WezTerm; }
    else if (contains_ci(r, "contour")) { c.id = TermId::Contour; }
    else if (contains_ci(r, "mintty"))  { c.id = TermId::Mintty; }
    else if (contains_ci(r, "xterm("))  { if (c.id == TermId::Unknown) c.id = TermId::XTerm; }

    // XTGETTCAP TN payload: DCS 1 + r 544E = <hex> ST  -> decode the hex name.
    size_t tn = r.find("544E=");
    if (tn == std::string::npos) tn = r.find("544e=");
    if (tn != std::string::npos) {
        std::string hex;
        for (size_t i = tn + 5; i < r.size() && isxdigit((unsigned char)r[i]); ++i) hex += r[i];
        std::string nm = unhex(hex);
        if (contains_ci(nm, "kitty"))  c.id = TermId::Kitty;
        if (contains_ci(nm, "mlterm")) c.id = TermId::MLterm;
    }

    // DA1: CSI ? a;b;c ... c   -> attribute 4 means sixel.
    // The DA1 '4' is the AUTHORITATIVE sixel signal (per VT spec; confirmed by
    // notcurses/ucs-detect/libsixel). XTSMGRAPHICS is unreliable (xterm answers
    // it even when built without sixel), so we only trust this.
    size_t da1 = r.find("[?");
    if (da1 != std::string::npos) {
        size_t end = r.find('c', da1);
        std::string attrs = r.substr(da1 + 2, end == std::string::npos ? std::string::npos : end - da1 - 2);
        c.da1_seen = true;
        size_t p = 0;
        while (p < attrs.size()) {
            size_t q2 = attrs.find(';', p);
            std::string tok = attrs.substr(p, q2 == std::string::npos ? std::string::npos : q2 - p);
            if (tok == "4") { c.sixel = true; c.da1_has_sixel = true; }
            if (q2 == std::string::npos) break;
            p = q2 + 1;
        }
    }

    // Cell size: CSI 6 ; h ; w t
    size_t cs = r.find("[6;");
    if (cs != std::string::npos) {
        int h = 0, wpx = 0;
        if (sscanf(r.c_str() + cs, "[6;%d;%dt", &h, &wpx) == 2 && h > 0 && wpx > 0) {
            c.cell_px_h = h; c.cell_px_w = wpx;
        }
    }
}

TermCaps& TermCaps::get() { static TermCaps c; return c; }

void TermCaps::detect() {
    TermCaps& c = get();
    c.term = env_str("TERM");
    c.is_tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

    // ── Locale charset ───────────────────────────────────────────────────────
    // If the locale is not UTF-8 (e.g. C/POSIX, Latin-1, or terminals like
    // bobcat / mis-configured mlterm shown in real bug reports), every
    // multibyte glyph we emit — box-drawing borders, block-art thumbnails,
    // music symbols — renders as byte garbage. Detect it once and let all
    // rendering gate on caps.unicode. setlocale here is idempotent with the
    // later setlocale(LC_ALL, "") in main.
    setlocale(LC_CTYPE, "");
    const char* cs = nl_langinfo(CODESET);
    c.codeset = cs ? cs : "";
    {
        std::string up = c.codeset;
        for (auto& ch : up) ch = (char)toupper((unsigned char)ch);
        c.unicode = (up.find("UTF-8") != std::string::npos ||
                     up.find("UTF8")  != std::string::npos);
    }

    std::string lc_term = c.term;
    for (auto& ch : lc_term) ch = (char)tolower((unsigned char)ch);
    std::string tp = env_str("TERM_PROGRAM");

    // Multiplexer first (its escapes are wrapped/limited).
    if (!env_str("TMUX").empty() || lc_term.compare(0, 4, "tmux") == 0) {
        c.in_multiplexer = true; c.id = TermId::Tmux;
    } else if (lc_term.compare(0, 6, "screen") == 0) {
        c.in_multiplexer = true; c.id = TermId::Screen;
    }

    // Cheap, reliable env signals (don't need a tty).
    if (tp == "Apple_Terminal")            { c.id = TermId::AppleTerminal; }
    else if (tp == "iTerm.app")            { c.id = TermId::ITerm2; }
    else if (!env_str("KITTY_WINDOW_ID").empty() || lc_term.find("kitty") != std::string::npos)
                                            c.id = TermId::Kitty;
    else if (!env_str("GHOSTTY_RESOURCES_DIR").empty() || tp == "ghostty")
                                            c.id = TermId::Ghostty;
    else if (!env_str("WEZTERM_EXECUTABLE").empty() || tp == "WezTerm")
                                            c.id = TermId::WezTerm;
    else if (!env_str("KONSOLE_VERSION").empty()) c.id = TermId::Konsole;
    else if (!env_str("VTE_VERSION").empty())     c.id = TermId::VTE;
    else if (!env_str("WT_SESSION").empty())      c.id = TermId::WindowsTerminal;
    else if (lc_term.find("alacritty") != std::string::npos) c.id = TermId::Alacritty;
    else if (lc_term.find("mlterm")    != std::string::npos) c.id = TermId::MLterm;
    else if (lc_term.find("foot")      != std::string::npos) c.id = TermId::Foot;
    else if (lc_term.find("contour")   != std::string::npos) c.id = TermId::Contour;
    else if (lc_term == "linux")                             c.id = TermId::LinuxConsole;
    else if (lc_term.find("rxvt")      != std::string::npos) c.id = TermId::Rxvt;
    else if (lc_term.compare(0, 2, "st") == 0)               c.id = TermId::St;

    // Runtime queries refine/confirm identity and discover sixel + cell size.
    // Skipped inside a multiplexer (replies are unreliable / wrapped).
    std::string r;
    if (c.is_tty && !c.in_multiplexer) r = run_queries(250);
    parse_responses(c, r);

    // ── Capability mapping (env + identity + research-backed per-terminal facts).
    // Defaults: assume a modern 256-colour unicode terminal, then specialise.
    std::string colorterm = env_str("COLORTERM");
    bool ct_truecolor = (colorterm == "truecolor" || colorterm == "24bit");
    // (c.unicode already set from the locale codeset above)

    auto set = [&](int colors, bool tc, bool ccc, bool blocks, bool sixel,
                   bool kgfx, bool iimg, bool bce, bool revok) {
        c.colors = colors; c.truecolor = tc; c.can_change_color = ccc;
        c.blocks_native = blocks; if (sixel) c.sixel = true;
        c.kitty_gfx = kgfx; c.iterm_images = iimg; c.bce = bce; c.reverse_ok = revok;
    };

    switch (c.id) {
        case TermId::ITerm2:
            set(16777216, true, true, true, c.sixel, false, true, true, true); break;
        case TermId::Kitty:
            set(16777216, true, true, true, false, true, false, false, true);
            c.kitty_keyboard = true; break;
        case TermId::Ghostty:
            set(16777216, true, true, true, false, true, false, true, true);
            c.kitty_keyboard = true; break;
        case TermId::WezTerm:
            set(16777216, true, true, true, true, true, true, true, true); break;
        case TermId::Foot:
            set(16777216, true, true, true, true, false, false, true, true); break;
        case TermId::Contour:
            set(16777216, true, true, true, true, false, false, true, true); break;
        case TermId::Konsole:
            set(16777216, true, false, true, c.sixel, false, false, true, true); break;
        case TermId::VTE:
            set(16777216, true, true, true, false, false, false, true, true); break;
        case TermId::Alacritty:
            // No native block glyphs (depends on font); no sixel (mainline).
            set(16777216, true, true, false, false, false, false, true, true); break;
        case TermId::MLterm:
            // RGB works, but ccc does not, and COLORTERM must NOT be forced.
            // Sixel is a BUILD option in mlterm (the MacPorts / SDL / framebuffer
            // builds ship without it), and a non-sixel mlterm dumps the sixel
            // bytes as literal text — so we must trust the DA1 '4' probe here,
            // NOT assume. Blocks depend on the font.
            set(16777216, true, false, false, c.sixel, false, false, true, true); break;
        case TermId::XTerm:
            // Truecolor only with --enable-direct-color; sixel only with build
            // flag (we trust the DA1 probe for sixel). Blocks depend on font.
            set(ct_truecolor ? 16777216 : 256, ct_truecolor, false, false,
                c.sixel, false, false, true, true); break;
        case TermId::Mintty:
            set(16777216, true, true, true, c.sixel, false, true, true, true); break;
        case TermId::WindowsTerminal:
            set(16777216, true, false, true, c.sixel, false, false, true, true); break;
        case TermId::Terminology:
            set(256, false, false, true, false, false, false, true, true); break;
        case TermId::St:
            set(ct_truecolor ? 16777216 : 256, ct_truecolor, true, true,
                c.sixel, false, false, true, true); break;
        case TermId::Rxvt:
            set(256, false, false, false, false, false, false, true, true); break;
        case TermId::AppleTerminal:
            // 256 colours only, no RGB, no ccc, block glyphs depend on font,
            // no graphics protocol of any kind. reverse video is reliable.
            set(256, false, false, false, false, false, false, true, true); break;
        case TermId::LinuxConsole:
            // 16 colours (RGB downsampled), font block glyphs reprogrammable but
            // we don't; treat as 16-colour with no graphics.
            set(16, false, true, true, false, false, false, true, true);
            c.mouse_sgr = false; break;
        case TermId::Uterm:
        case TermId::Unknown:
        default:
            // Conservative baseline for unidentified / minimal terminals
            // (e.g. uterm/libtsm): trust $TERM/$COLORTERM only.
            if (ct_truecolor) { c.colors = 16777216; c.truecolor = true; }
            else if (lc_term.find("256") != std::string::npos) c.colors = 256;
            else if (lc_term.find("16") != std::string::npos)  c.colors = 16;
            else c.colors = 8;
            c.can_change_color = false;
            c.blocks_native = false;       // assume font-dependent
            c.bce = true; c.reverse_ok = true;
            break;
    }

    // Multiplexer: cap to what's reliably tunneled and disable graphics.
    if (c.in_multiplexer) {
        c.sixel = c.kitty_gfx = c.iterm_images = false;
        if (!ct_truecolor && c.colors > 256) c.colors = 256;  // needs Tc override
    }

    // ── Authoritative sixel gate ──────────────────────────────────────────────
    // The DA1 '4' attribute is ground truth. If the terminal answered DA1 but
    // did NOT advertise sixel, force it off regardless of any per-$TERM guess —
    // emitting sixel to a non-sixel build (e.g. a MacPorts mlterm without
    // --with-imagelib) dumps raw escape bytes as on-screen garbage. We only
    // keep a heuristic "true" when the terminal never answered DA1 at all
    // (can't probe → fall back to the per-terminal default for known-good ones).
    if (c.da1_seen && !c.da1_has_sixel) c.sixel = false;

    // $COLORTERM can upgrade an under-reported terminal (errs safe).
    if (ct_truecolor && c.id != TermId::MLterm) { c.truecolor = true; c.colors = 16777216; }

    if (c.name == "unknown") c.name = c.id_name();
}

void TermCaps::refine_from_ncurses() {
    TermCaps& c = get();
    // After start_color(): trust the live terminfo for the colour count and the
    // RGB / bce / ccc flags. These are authoritative once ncurses is up.
    if (COLORS > c.colors) c.colors = COLORS;
    if (tigetflag(const_cast<char*>("RGB")) == 1) { c.truecolor = true; if (c.colors < 256) c.colors = 256; }
    int bce = tigetflag(const_cast<char*>("bce"));
    if (bce == 0) c.bce = false; else if (bce == 1) c.bce = true;
    if (::can_change_color()) c.can_change_color = true;
    // Reverse video is only meaningfully "ok" when we have colour to invert.
    if (c.colors < 8) c.reverse_ok = false;
}

void TermCaps::detect_for_diag() {
    int err = 0;
    setupterm(nullptr, STDOUT_FILENO, &err);  // load terminfo for the flag reads
    detect();
    refine_from_ncurses();
}

int TermCaps::best_graphics() const {
    if (kitty_gfx)    return 3;   // Thumbnails::Gfx::Kitty
    if (sixel)        return 2;   // Thumbnails::Gfx::Sixel
    if (iterm_images) return 4;   // Thumbnails::Gfx::Iterm
    return 0;
}

std::string TermCaps::id_name() const {
    switch (id) {
        case TermId::XTerm:           return "XTerm";
        case TermId::MLterm:          return "mlterm";
        case TermId::ITerm2:          return "iTerm2";
        case TermId::AppleTerminal:   return "Apple Terminal";
        case TermId::Kitty:           return "kitty";
        case TermId::Ghostty:         return "Ghostty";
        case TermId::WezTerm:         return "WezTerm";
        case TermId::Foot:            return "foot";
        case TermId::Konsole:         return "Konsole";
        case TermId::Alacritty:       return "Alacritty";
        case TermId::VTE:             return "VTE (gnome/xfce/etc)";
        case TermId::Contour:         return "Contour";
        case TermId::WindowsTerminal: return "Windows Terminal";
        case TermId::Terminology:     return "Terminology";
        case TermId::Rxvt:            return "rxvt";
        case TermId::St:              return "st";
        case TermId::Mintty:          return "mintty";
        case TermId::LinuxConsole:    return "Linux console";
        case TermId::Uterm:           return "uterm (libtsm)";
        case TermId::Tmux:            return "tmux";
        case TermId::Screen:          return "GNU screen";
        default:                      return "unknown";
    }
}

std::string TermCaps::summary() const {
    char buf[1024];
    auto yn = [](bool b) { return b ? "yes" : "no"; };
    const char* ctier = colors >= 16777216 ? "truecolor"
                       : colors >= 256 ? "256"
                       : colors >= 16  ? "16" : "8";
    snprintf(buf, sizeof(buf),
        "  identified  : %s\n"
        "  TERM         : %s%s\n"
        "  colours      : %s (%d)   ccc:%s  bce:%s  reverse-ok:%s\n"
        "  unicode      : %s (codeset: %s)   native-blocks:%s   mouse(SGR):%s\n"
        "  graphics     : sixel:%s kitty:%s iterm:%s   cell:%dx%d px\n"
        "  sixel-probe  : DA1 seen:%s  DA1 advertised sixel(4):%s\n"
        "  kbd-protocol : %s",
        name.c_str(),
        term.empty() ? "(unset)" : term.c_str(),
        in_multiplexer ? "  [multiplexer]" : "",
        ctier, colors, yn(can_change_color), yn(bce), yn(reverse_ok),
        yn(unicode), codeset.empty() ? "?" : codeset.c_str(), yn(blocks_native), yn(mouse_sgr),
        yn(sixel), yn(kitty_gfx), yn(iterm_images), cell_px_w, cell_px_h,
        yn(da1_seen), yn(da1_has_sixel),
        kitty_keyboard ? "kitty" : "legacy");
    return buf;
}

} // namespace ytui
