#pragma once
#include <string>

namespace ytui {

// Identified terminal family. Determined from runtime queries (XTVERSION,
// XTGETTCAP TN, DA1/DA2/DA3) plus environment ($TERM, $TERM_PROGRAM, etc.).
enum class TermId {
    Unknown,        // no usable identification -> conservative baseline
    XTerm,
    MLterm,
    ITerm2,
    AppleTerminal,
    Kitty,
    Ghostty,
    WezTerm,
    Foot,
    Konsole,
    Alacritty,
    VTE,            // gnome-terminal, xfce4-terminal, sakura, etc.
    Contour,
    WindowsTerminal,
    Terminology,
    Rxvt,
    St,             // suckless st
    Mintty,
    LinuxConsole,
    Uterm,          // refi64/uterm and other libtsm/kmscon-based emulators
    Tmux,
    Screen
};

// Resolved capabilities for the connected terminal. Everything ytcui needs to
// decide what to emit and what to avoid lives here, so rendering code never has
// to sniff $TERM itself. Populated once at startup via TermCaps::detect()
// (before ncurses) and refined by TermCaps::refine_from_ncurses() (after
// start_color()).
struct TermCaps {
    TermId      id    = TermId::Unknown;
    std::string name  = "unknown";   // human readable, e.g. "iTerm2 3.5.0"
    std::string term;                // raw $TERM
    bool        is_tty = false;
    bool        in_multiplexer = false;   // tmux/screen wraps us

    // Colour tier.
    int  colors    = 8;              // 8 / 16 / 256 / 16777216 (truecolor)
    bool truecolor = false;          // 24-bit direct colour usable
    bool can_change_color = false;   // ccc / init_color works

    // Rendering quirks.
    bool bce        = true;          // back-colour-erase reliable (terminfo)
    bool reverse_ok = true;          // A_REVERSE renders sanely with colour
    bool blocks_native = false;      // terminal draws block glyphs itself
                                     // (false -> block art depends on the font)
    bool unicode    = true;          // UTF-8 locale: multibyte glyphs are safe
    std::string codeset;             // nl_langinfo(CODESET), e.g. "UTF-8"

    // Input.
    bool mouse_sgr      = true;      // SGR (1006) mouse reporting
    bool kitty_keyboard = false;     // kitty keyboard disambiguation protocol

    // Graphics protocols.
    bool sixel        = false;
    bool kitty_gfx    = false;
    bool iterm_images = false;
    int  cell_px_w = 0, cell_px_h = 0;   // from CSI 16 t (0 = unknown)

    // Best graphics protocol the terminal can do, as a hint (still opt-in to
    // actually use). 0=none,2=sixel,3=kitty,4=iterm (matches Thumbnails::Gfx).
    int best_graphics() const;

    // ---- API ----
    static TermCaps& get();
    // Run identification queries + env analysis. Call ONCE before ncurses
    // init (it puts the tty in raw mode briefly to read query replies).
    static void detect();
    // Pull colour/flag facts from the initialised terminfo (COLORS, RGB, bce,
    // ccc). Call once from TUI::init() after start_color()/use_default_colors().
    static void refine_from_ncurses();

    // Convenience for non-ncurses contexts (e.g. --diag): loads terminfo via
    // setupterm(), runs detect() and refine_from_ncurses() in one call.
    static void detect_for_diag();

    std::string id_name() const;     // "iTerm2", "mlterm", ...
    std::string summary() const;     // multi-line, for --diag
};

} // namespace ytui
