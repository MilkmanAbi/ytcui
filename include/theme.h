#pragma once

#include <string>
#include <map>

namespace ytui {

// Theme color definitions
// Each theme defines 16 color pairs using ncurses 256-color palette
// -1 means use terminal default

struct ThemeColors {
    int bg;           // Background
    int search_box;   // Search input text
    int title;        // Video titles
    int channel;      // Channel names
    int stats;        // View counts, stats
    int selected;     // Selected item
    int action;       // Action menu items
    int action_sel;   // Selected action
    int status;       // Status bar
    int border;       // Box borders
    int header;       // Header text
    int accent;       // Accent/highlight color
    int tag;          // Tags like [LIVE], [Video]
    int published;    // Published dates
    int bookmark;     // Bookmark indicator
    int desc;         // Descriptions
};

// Available themes
enum class Theme {
    Default,
    Grayscale,
    Nord,
    Dracula,
    Solarized,
    Monokai,
    Gruvbox,
    Tokyo
};

inline std::string theme_to_string(Theme t) {
    switch (t) {
        case Theme::Default:    return "default";
        case Theme::Grayscale:  return "grayscale";
        case Theme::Nord:       return "nord";
        case Theme::Dracula:    return "dracula";
        case Theme::Solarized:  return "solarized";
        case Theme::Monokai:    return "monokai";
        case Theme::Gruvbox:    return "gruvbox";
        case Theme::Tokyo:      return "tokyo";
        default:                return "default";
    }
}

inline Theme string_to_theme(const std::string& s) {
    if (s == "grayscale" || s == "gray" || s == "grey")  return Theme::Grayscale;
    if (s == "nord")       return Theme::Nord;
    if (s == "dracula")    return Theme::Dracula;
    if (s == "solarized" || s == "solar")  return Theme::Solarized;
    if (s == "monokai")    return Theme::Monokai;
    if (s == "gruvbox")    return Theme::Gruvbox;
    if (s == "tokyo" || s == "tokyonight") return Theme::Tokyo;
    return Theme::Default;
}

// Theme definitions using 256-color palette
// Reference: https://www.ditig.com/256-colors-cheat-sheet

inline ThemeColors get_theme_colors(Theme theme) {
    switch (theme) {
        
        // ═══════════════════════════════════════════════════════════════════════
        // DEFAULT — Vibrant terminal colors
        // ═══════════════════════════════════════════════════════════════════════
        case Theme::Default:
            return {
                .bg         = -1,           // Terminal default
                .search_box = -1,
                .title      = 6,            // Cyan (COLOR_CYAN)
                .channel    = 2,            // Green (COLOR_GREEN)
                .stats      = 3,            // Yellow (COLOR_YELLOW)
                .selected   = -1,
                .action     = -1,
                .action_sel = -1,
                .status     = -1,
                .border     = 6,            // Cyan
                .header     = -1,
                .accent     = 214,          // Orange (256-color)
                .tag        = 7,            // White
                .published  = 5,            // Magenta
                .bookmark   = 3,            // Yellow
                .desc       = 7,            // White
            };
        
        // ═══════════════════════════════════════════════════════════════════════
        // GRAYSCALE — Minimal, Nord-inspired blue tones
        // ═══════════════════════════════════════════════════════════════════════
        case Theme::Grayscale:
            return {
                .bg         = -1,
                .search_box = -1,
                .title      = 116,          // Nord8 frost cyan #88C0D0
                .channel    = -1,
                .stats      = -1,
                .selected   = -1,
                .action     = -1,
                .action_sel = -1,
                .status     = -1,
                .border     = 116,          // Nord8
                .header     = -1,
                .accent     = 110,          // Nord9 accent blue #81A1C1
                .tag        = -1,
                .published  = 68,           // Nord10 darker blue #5E81AC
                .bookmark   = 110,
                .desc       = -1,
            };
        
        // ═══════════════════════════════════════════════════════════════════════
        // NORD — Arctic, bluish color palette
        // https://www.nordtheme.com/
        // ═══════════════════════════════════════════════════════════════════════
        case Theme::Nord:
            return {
                .bg         = -1,
                .search_box = 255,          // Snow white
                .title      = 110,          // Nord9 blue #81A1C1
                .channel    = 108,          // Nord14 green #A3BE8C
                .stats      = 222,          // Nord13 yellow #EBCB8B
                .selected   = -1,
                .action     = -1,
                .action_sel = -1,
                .status     = -1,
                .border     = 67,           // Nord10 #5E81AC
                .header     = 255,
                .accent     = 116,          // Nord8 frost #88C0D0
                .tag        = 139,          // Nord15 purple #B48EAD
                .published  = 139,          // Nord15
                .bookmark   = 222,          // Nord13 yellow
                .desc       = 249,          // Light gray
            };
        
        // ═══════════════════════════════════════════════════════════════════════
        // DRACULA — Dark purple/pink vampire theme
        // https://draculatheme.com/
        // ═══════════════════════════════════════════════════════════════════════
        case Theme::Dracula:
            return {
                .bg         = -1,
                .search_box = 255,          // Foreground
                .title      = 141,          // Purple #BD93F9
                .channel    = 84,           // Green #50FA7B
                .stats      = 228,          // Yellow #F1FA8C
                .selected   = -1,
                .action     = -1,
                .action_sel = -1,
                .status     = -1,
                .border     = 212,          // Pink #FF79C6
                .header     = 255,
                .accent     = 117,          // Cyan #8BE9FD
                .tag        = 203,          // Red #FF5555
                .published  = 215,          // Orange #FFB86C
                .bookmark   = 228,          // Yellow
                .desc       = 248,          // Comment gray
            };
        
        // ═══════════════════════════════════════════════════════════════════════
        // SOLARIZED — Precision colors for machines and people
        // https://ethanschoonover.com/solarized/
        // ═══════════════════════════════════════════════════════════════════════
        case Theme::Solarized:
            return {
                .bg         = -1,
                .search_box = 244,          // Base0
                .title      = 33,           // Blue #268BD2
                .channel    = 64,           // Green #859900
                .stats      = 136,          // Yellow #B58900
                .selected   = -1,
                .action     = -1,
                .action_sel = -1,
                .status     = -1,
                .border     = 37,           // Cyan #2AA198
                .header     = 245,          // Base1
                .accent     = 166,          // Orange #CB4B16
                .tag        = 125,          // Magenta #D33682
                .published  = 61,           // Violet #6C71C4
                .bookmark   = 136,          // Yellow
                .desc       = 244,          // Base0
            };
        
        // ═══════════════════════════════════════════════════════════════════════
        // MONOKAI — Classic code editor theme
        // ═══════════════════════════════════════════════════════════════════════
        case Theme::Monokai:
            return {
                .bg         = -1,
                .search_box = 231,          // White
                .title      = 81,           // Blue #66D9EF
                .channel    = 148,          // Green #A6E22E
                .stats      = 228,          // Yellow #E6DB74
                .selected   = -1,
                .action     = -1,
                .action_sel = -1,
                .status     = -1,
                .border     = 197,          // Pink #F92672
                .header     = 231,
                .accent     = 208,          // Orange #FD971F
                .tag        = 141,          // Purple #AE81FF
                .published  = 141,          // Purple
                .bookmark   = 228,          // Yellow
                .desc       = 250,          // Gray
            };
        
        // ═══════════════════════════════════════════════════════════════════════
        // GRUVBOX — Retro groove colors
        // https://github.com/morhetz/gruvbox
        // ═══════════════════════════════════════════════════════════════════════
        case Theme::Gruvbox:
            return {
                .bg         = -1,
                .search_box = 223,          // fg1 #EBDBB2
                .title      = 109,          // Blue #83A598
                .channel    = 142,          // Green #B8BB26
                .stats      = 214,          // Yellow #FABD2F
                .selected   = -1,
                .action     = -1,
                .action_sel = -1,
                .status     = -1,
                .border     = 167,          // Red #FB4934
                .header     = 223,
                .accent     = 208,          // Orange #FE8019
                .tag        = 175,          // Purple #D3869B
                .published  = 175,          // Purple
                .bookmark   = 214,          // Yellow
                .desc       = 246,          // Gray
            };
        
        // ═══════════════════════════════════════════════════════════════════════
        // TOKYO NIGHT — A clean dark theme inspired by Tokyo
        // https://github.com/folke/tokyonight.nvim
        // ═══════════════════════════════════════════════════════════════════════
        case Theme::Tokyo:
            return {
                .bg         = -1,
                .search_box = 189,          // Foreground #C0CAF5
                .title      = 111,          // Blue #7AA2F7
                .channel    = 120,          // Green #9ECE6A
                .stats      = 221,          // Yellow #E0AF68
                .selected   = -1,
                .action     = -1,
                .action_sel = -1,
                .status     = -1,
                .border     = 141,          // Purple #BB9AF7
                .header     = 189,
                .accent     = 80,           // Cyan #7DCFFF
                .tag        = 204,          // Red #F7768E
                .published  = 141,          // Purple
                .bookmark   = 221,          // Yellow
                .desc       = 146,          // Comment #565F89
            };
        
        default:
            return get_theme_colors(Theme::Default);
    }
}

} // namespace ytui
