#pragma once

#include <string>
#include <map>
#include <algorithm>

namespace ytui {

// ─── Color element names (for config [colors] section) ───────────────────────
// These are the exact strings users write in config.json under "colors": {}

struct ThemeColors {
    int bg         = -1;  // general text / background
    int search_box = -1;  // search input text & cursor
    int title      = 6;   // video title rows
    int channel    = 2;   // channel name
    int stats      = 3;   // view count, duration, stats
    int selected   = -1;  // selected-row background (usually -1, highlight via A_REVERSE)
    int action     = -1;  // action menu items (unselected)
    int action_sel = -1;  // action menu selected (usually -1, A_REVERSE does the work)
    int status     = -1;  // status bar text
    int border     = 6;   // box / panel borders
    int header     = -1;  // top header bar
    int accent     = 214; // highlight, tabs active border, playlist accent
    int tag        = 7;   // [LIVE] / [4K] tags
    int published  = 5;   // publish date
    int bookmark   = 3;   // bookmark indicator
    int desc       = 7;   // description text
};

// Available built-in themes
enum class Theme {
    Default,
    Grayscale,
    Nord,
    Dracula,
    Solarized,
    Monokai,
    Gruvbox,
    Tokyo,
    // Solid colour themes
    Pink,
    Green,
    Blue,
    Purple,
    Red,
    Amber,
    Ocean,
    Mint,
    Coral,
    Slate,
    Custom,   // user-defined via config [colors] — do not use directly
};

inline std::string theme_to_string(Theme t) {
    switch (t) {
        case Theme::Default:   return "default";
        case Theme::Grayscale: return "grayscale";
        case Theme::Nord:      return "nord";
        case Theme::Dracula:   return "dracula";
        case Theme::Solarized: return "solarized";
        case Theme::Monokai:   return "monokai";
        case Theme::Gruvbox:   return "gruvbox";
        case Theme::Tokyo:     return "tokyo";
        case Theme::Pink:      return "pink";
        case Theme::Green:     return "green";
        case Theme::Blue:      return "blue";
        case Theme::Purple:    return "purple";
        case Theme::Red:       return "red";
        case Theme::Amber:     return "amber";
        case Theme::Ocean:     return "ocean";
        case Theme::Mint:      return "mint";
        case Theme::Coral:     return "coral";
        case Theme::Slate:     return "slate";
        case Theme::Custom:    return "custom";
        default:               return "default";
    }
}

inline Theme string_to_theme(const std::string& s) {
    std::string l = s;
    std::transform(l.begin(), l.end(), l.begin(), ::tolower);
    if (l == "grayscale" || l == "gray"  || l == "grey")         return Theme::Grayscale;
    if (l == "nord")                                              return Theme::Nord;
    if (l == "dracula")                                           return Theme::Dracula;
    if (l == "solarized" || l == "solar")                         return Theme::Solarized;
    if (l == "monokai")                                           return Theme::Monokai;
    if (l == "gruvbox")                                           return Theme::Gruvbox;
    if (l == "tokyo" || l == "tokyonight")                        return Theme::Tokyo;
    if (l == "pink")                                              return Theme::Pink;
    if (l == "green")                                             return Theme::Green;
    if (l == "blue")                                              return Theme::Blue;
    if (l == "purple" || l == "violet")                           return Theme::Purple;
    if (l == "red")                                               return Theme::Red;
    if (l == "amber" || l == "orange")                            return Theme::Amber;
    if (l == "ocean" || l == "teal")                              return Theme::Ocean;
    if (l == "mint")                                              return Theme::Mint;
    if (l == "coral")                                             return Theme::Coral;
    if (l == "slate" || l == "steel")                             return Theme::Slate;
    if (l == "custom")                                            return Theme::Custom;
    return Theme::Default;
}

// ─── 256-colour reference ────────────────────────────────────────────────────
// https://www.ditig.com/256-colors-cheat-sheet
// Standard 16:  0=Black 1=Red 2=Green 3=Yellow 4=Blue 5=Magenta 6=Cyan 7=White
//              8-15: bright versions

inline ThemeColors get_theme_colors(Theme theme) {
    switch (theme) {

    // ─── Established themes ───────────────────────────────────────────────────

    case Theme::Default:
        return { .bg=-1, .search_box=-1, .title=6, .channel=2, .stats=3,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=6, .header=-1, .accent=214, .tag=7,
                 .published=5, .bookmark=3, .desc=7 };

    case Theme::Grayscale:
        return { .bg=-1, .search_box=-1, .title=116, .channel=-1, .stats=-1,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=116, .header=-1, .accent=110, .tag=-1,
                 .published=68, .bookmark=110, .desc=-1 };

    case Theme::Nord:
        return { .bg=-1, .search_box=255, .title=110, .channel=108, .stats=222,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=67, .header=255, .accent=116, .tag=139,
                 .published=139, .bookmark=222, .desc=249 };

    case Theme::Dracula:
        return { .bg=-1, .search_box=255, .title=141, .channel=84, .stats=228,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=212, .header=255, .accent=117, .tag=203,
                 .published=215, .bookmark=228, .desc=248 };

    case Theme::Solarized:
        return { .bg=-1, .search_box=244, .title=33, .channel=64, .stats=136,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=37, .header=245, .accent=166, .tag=125,
                 .published=61, .bookmark=136, .desc=244 };

    case Theme::Monokai:
        return { .bg=-1, .search_box=231, .title=81, .channel=148, .stats=228,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=197, .header=231, .accent=208, .tag=141,
                 .published=141, .bookmark=228, .desc=250 };

    case Theme::Gruvbox:
        return { .bg=-1, .search_box=223, .title=109, .channel=142, .stats=214,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=167, .header=223, .accent=208, .tag=175,
                 .published=175, .bookmark=214, .desc=246 };

    case Theme::Tokyo:
        return { .bg=-1, .search_box=189, .title=111, .channel=120, .stats=221,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=141, .header=189, .accent=80, .tag=204,
                 .published=141, .bookmark=221, .desc=146 };

    // ─── Solid colour themes ──────────────────────────────────────────────────
    // Each one picks a dominant hue and builds a coherent palette around it.
    // accent = bold/bright version, title = main colour, border = mid, etc.

    // PINK — blush, rose, soft petal pinks
    case Theme::Pink:
        return { .bg=-1, .search_box=225, .title=218, .channel=217, .stats=223,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=217, .header=225, .accent=211, .tag=224,
                 .published=182, .bookmark=211, .desc=254 };
        // 225=lavender blush, 218=light pink, 217=misty rose, 211=soft rose
        // 223=peach, 182=thistle, 224=antique white, 254=near-white

    // GREEN — sage, celadon, soft botanical
    case Theme::Green:
        return { .bg=-1, .search_box=194, .title=150, .channel=157, .stats=193,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=114, .header=194, .accent=120, .tag=193,
                 .published=108, .bookmark=157, .desc=254 };
        // 194=honeydew, 150=dark sea green, 157=light sage, 120=pale green
        // 114=medium sea green, 108=sage, 193=tea green

    // BLUE — powder, periwinkle, cornflower pastel
    case Theme::Blue:
        return { .bg=-1, .search_box=195, .title=153, .channel=189, .stats=195,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=117, .header=195, .accent=153, .tag=189,
                 .published=147, .bookmark=153, .desc=254 };
        // 195=alice blue, 153=light blue, 189=periwinkle, 117=cornflower
        // 147=medium periwinkle, 254=near-white

    // PURPLE — lavender, wisteria, soft lilac
    case Theme::Purple:
        return { .bg=-1, .search_box=189, .title=183, .channel=189, .stats=225,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=183, .header=189, .accent=177, .tag=225,
                 .published=182, .bookmark=177, .desc=254 };
        // 189=periwinkle-lavender, 183=soft violet, 177=medium lavender
        // 182=thistle, 225=lavender blush

    // RED — rose, blush, dusty rose
    case Theme::Red:
        return { .bg=-1, .search_box=224, .title=217, .channel=223, .stats=229,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=210, .header=224, .accent=210, .tag=223,
                 .published=181, .bookmark=210, .desc=254 };
        // 224=linen, 217=misty rose, 210=light coral (soft), 223=peach
        // 229=light yellow, 181=rosybrown-light

    // AMBER — champagne, warm cream, soft gold
    case Theme::Amber:
        return { .bg=-1, .search_box=230, .title=222, .channel=229, .stats=230,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=221, .header=230, .accent=222, .tag=229,
                 .published=180, .bookmark=222, .desc=254 };
        // 230=cornsilk, 222=pale goldenrod, 229=light yellow, 221=soft gold
        // 180=light goldenrod, 254=near-white

    // OCEAN — pale turquoise, sky, soft aqua
    case Theme::Ocean:
        return { .bg=-1, .search_box=195, .title=159, .channel=194, .stats=195,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=123, .header=195, .accent=159, .tag=194,
                 .published=152, .bookmark=159, .desc=254 };
        // 195=alice blue, 159=pale turquoise, 194=honeydew, 123=aquamarine-light
        // 152=cadet blue-light, 254=near-white

    // MINT — soft spearmint, foam, pale jade
    case Theme::Mint:
        return { .bg=-1, .search_box=194, .title=158, .channel=194, .stats=229,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=157, .header=194, .accent=158, .tag=229,
                 .published=151, .bookmark=158, .desc=254 };
        // 194=honeydew, 158=pale green-mint, 157=light mint, 151=soft teal-green
        // 229=cream, 254=near-white

    // CORAL — peach, apricot, soft salmon
    case Theme::Coral:
        return { .bg=-1, .search_box=224, .title=216, .channel=223, .stats=230,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=216, .header=224, .accent=215, .tag=223,
                 .published=181, .bookmark=215, .desc=254 };
        // 224=linen, 216=light salmon-peach, 215=sandy peach, 223=peach
        // 181=rosy beige, 230=cornsilk, 254=near-white

    // SLATE — soft steel, cool mist, powder blue-gray
    case Theme::Slate:
        return { .bg=-1, .search_box=189, .title=153, .channel=189, .stats=195,
                 .selected=-1, .action=-1, .action_sel=-1, .status=-1,
                 .border=152, .header=189, .accent=153, .tag=195,
                 .published=146, .bookmark=153, .desc=253 };
        // 189=periwinkle-mist, 153=light blue-gray, 152=cadet blue-light
        // 146=light slate blue, 195=alice blue, 253=light gray

    // Custom: not returned here, handled in app.cpp
    case Theme::Custom:
        return get_theme_colors(Theme::Default);

    default:
        return get_theme_colors(Theme::Default);
    }
}

// ─── Colour element name → ThemeColors field mapping ─────────────────────────
// Used to apply per-element custom overrides from config [colors] section.

inline const char* color_element_names[] = {
    "bg", "search_box", "title", "channel", "stats",
    "selected", "action", "action_sel", "status",
    "border", "header", "accent", "tag", "published",
    "bookmark", "desc"
};
constexpr int COLOR_ELEMENT_COUNT = 16;

inline void apply_custom_color(ThemeColors& tc,
                                const std::string& key, int value) {
    if (key == "bg")         tc.bg         = value;
    else if (key == "search_box")  tc.search_box  = value;
    else if (key == "title")       tc.title       = value;
    else if (key == "channel")     tc.channel     = value;
    else if (key == "stats")       tc.stats       = value;
    else if (key == "selected")    tc.selected    = value;
    else if (key == "action")      tc.action      = value;
    else if (key == "action_sel")  tc.action_sel  = value;
    else if (key == "status")      tc.status      = value;
    else if (key == "border")      tc.border      = value;
    else if (key == "header")      tc.header      = value;
    else if (key == "accent")      tc.accent      = value;
    else if (key == "tag")         tc.tag         = value;
    else if (key == "published")   tc.published   = value;
    else if (key == "bookmark")    tc.bookmark    = value;
    else if (key == "desc")        tc.desc        = value;
}

} // namespace ytui
