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
    MLterm,   // strictly black & white: no per-element colour, plain
              // reverse-video selection, no bold/dim emphasis, no thumbnails.
              // Selectable anywhere as a pure no-colour mode; also force-applied
              // automatically when ytcui detects it is running inside mlterm.
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
        case Theme::MLterm:    return "mlterm";
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
    if (l == "mlterm" || l == "mono" || l == "monochrome" ||
        l == "bw" || l == "blackwhite" || l == "black-white")     return Theme::MLterm;
    if (l == "custom")                                            return Theme::Custom;
    return Theme::Default;
}

// ─── 256-colour reference ────────────────────────────────────────────────────
// https://www.ditig.com/256-colors-cheat-sheet
// Standard 16:  0=Black 1=Red 2=Green 3=Yellow 4=Blue 5=Magenta 6=Cyan 7=White
//              8-15: bright versions

inline ThemeColors get_theme_colors(Theme theme) {
    ThemeColors colors;
    switch (theme) {

    // ─── Established themes ───────────────────────────────────────────────────

    case Theme::Default:
        colors.bg = -1; colors.search_box = -1; colors.title = 6; colors.channel = 2; colors.stats = 3;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 6; colors.header = -1; colors.accent = 214; colors.tag = 7;
        colors.published = 5; colors.bookmark = 3; colors.desc = 7;
        return colors;

    case Theme::Grayscale:
        colors.bg = -1; colors.search_box = -1; colors.title = 116; colors.channel = -1; colors.stats = -1;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 116; colors.header = -1; colors.accent = 110; colors.tag = -1;
        colors.published = 68; colors.bookmark = 110; colors.desc = -1;
        return colors;

    case Theme::Nord:
        colors.bg = -1; colors.search_box = 255; colors.title = 110; colors.channel = 108; colors.stats = 222;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 67; colors.header = 255; colors.accent = 116; colors.tag = 139;
        colors.published = 139; colors.bookmark = 222; colors.desc = 249;
        return colors;

    case Theme::Dracula:
        colors.bg = -1; colors.search_box = 255; colors.title = 141; colors.channel = 84; colors.stats = 228;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 212; colors.header = 255; colors.accent = 117; colors.tag = 203;
        colors.published = 215; colors.bookmark = 228; colors.desc = 248;
        return colors;

    case Theme::Solarized:
        colors.bg = -1; colors.search_box = 244; colors.title = 33; colors.channel = 64; colors.stats = 136;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 37; colors.header = 245; colors.accent = 166; colors.tag = 125;
        colors.published = 61; colors.bookmark = 136; colors.desc = 244;
        return colors;

    case Theme::Monokai:
        colors.bg = -1; colors.search_box = 231; colors.title = 81; colors.channel = 148; colors.stats = 228;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 197; colors.header = 231; colors.accent = 208; colors.tag = 141;
        colors.published = 141; colors.bookmark = 228; colors.desc = 250;
        return colors;

    case Theme::Gruvbox:
        colors.bg = -1; colors.search_box = 223; colors.title = 109; colors.channel = 142; colors.stats = 214;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 167; colors.header = 223; colors.accent = 208; colors.tag = 175;
        colors.published = 175; colors.bookmark = 214; colors.desc = 246;
        return colors;

    case Theme::Tokyo:
        colors.bg = -1; colors.search_box = 189; colors.title = 111; colors.channel = 120; colors.stats = 221;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 141; colors.header = 189; colors.accent = 80; colors.tag = 204;
        colors.published = 141; colors.bookmark = 221; colors.desc = 146;
        return colors;

    // ─── Solid colour themes ──────────────────────────────────────────────────
    // Each one picks a dominant hue and builds a coherent palette around it.
    // accent = bold/bright version, title = main colour, border = mid, etc.

    // PINK — blush, rose, soft petal pinks
    case Theme::Pink:
        colors.bg = -1; colors.search_box = 225; colors.title = 218; colors.channel = 217; colors.stats = 223;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 217; colors.header = 225; colors.accent = 211; colors.tag = 224;
        colors.published = 182; colors.bookmark = 211; colors.desc = 254;
        return colors;
        // 225=lavender blush, 218=light pink, 217=misty rose, 211=soft rose
        // 223=peach, 182=thistle, 224=antique white, 254=near-white

    // GREEN — sage, celadon, soft botanical
    case Theme::Green:
        colors.bg = -1; colors.search_box = 194; colors.title = 150; colors.channel = 157; colors.stats = 193;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 114; colors.header = 194; colors.accent = 120; colors.tag = 193;
        colors.published = 108; colors.bookmark = 157; colors.desc = 254;
        return colors;
        // 194=honeydew, 150=dark sea green, 157=light sage, 120=pale green
        // 114=medium sea green, 108=sage, 193=tea green

    // BLUE — powder, periwinkle, cornflower pastel
    case Theme::Blue:
        colors.bg = -1; colors.search_box = 195; colors.title = 153; colors.channel = 189; colors.stats = 195;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 117; colors.header = 195; colors.accent = 153; colors.tag = 189;
        colors.published = 147; colors.bookmark = 153; colors.desc = 254;
        return colors;
        // 195=alice blue, 153=light blue, 189=periwinkle, 117=cornflower
        // 147=medium periwinkle, 254=near-white

    // PURPLE — lavender, wisteria, soft lilac
    case Theme::Purple:
        colors.bg = -1; colors.search_box = 189; colors.title = 183; colors.channel = 189; colors.stats = 225;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 183; colors.header = 189; colors.accent = 177; colors.tag = 225;
        colors.published = 182; colors.bookmark = 177; colors.desc = 254;
        return colors;
        // 189=periwinkle-lavender, 183=soft violet, 177=medium lavender
        // 182=thistle, 225=lavender blush

    // RED — rose, blush, dusty rose
    case Theme::Red:
        colors.bg = -1; colors.search_box = 224; colors.title = 217; colors.channel = 223; colors.stats = 229;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 210; colors.header = 224; colors.accent = 210; colors.tag = 223;
        colors.published = 181; colors.bookmark = 210; colors.desc = 254;
        return colors;
        // 224=linen, 217=misty rose, 210=light coral (soft), 223=peach
        // 229=light yellow, 181=rosybrown-light

    // AMBER — champagne, warm cream, soft gold
    case Theme::Amber:
        colors.bg = -1; colors.search_box = 230; colors.title = 222; colors.channel = 229; colors.stats = 230;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 221; colors.header = 230; colors.accent = 222; colors.tag = 229;
        colors.published = 180; colors.bookmark = 222; colors.desc = 254;
        return colors;
        // 230=cornsilk, 222=pale goldenrod, 229=light yellow, 221=soft gold
        // 180=light goldenrod, 254=near-white

    // OCEAN — pale turquoise, sky, soft aqua
    case Theme::Ocean:
        colors.bg = -1; colors.search_box = 195; colors.title = 159; colors.channel = 194; colors.stats = 195;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 123; colors.header = 195; colors.accent = 159; colors.tag = 194;
        colors.published = 152; colors.bookmark = 159; colors.desc = 254;
        return colors;
        // 195=alice blue, 159=pale turquoise, 194=honeydew, 123=aquamarine-light
        // 152=cadet blue-light, 254=near-white

    // MINT — soft spearmint, foam, pale jade
    case Theme::Mint:
        colors.bg = -1; colors.search_box = 194; colors.title = 158; colors.channel = 194; colors.stats = 229;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 157; colors.header = 194; colors.accent = 158; colors.tag = 229;
        colors.published = 151; colors.bookmark = 158; colors.desc = 254;
        return colors;
        // 194=honeydew, 158=pale green-mint, 157=light mint, 151=soft teal-green
        // 229=cream, 254=near-white

    // CORAL — peach, apricot, soft salmon
    case Theme::Coral:
        colors.bg = -1; colors.search_box = 224; colors.title = 216; colors.channel = 223; colors.stats = 230;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 216; colors.header = 224; colors.accent = 215; colors.tag = 223;
        colors.published = 181; colors.bookmark = 215; colors.desc = 254;
        return colors;
        // 224=linen, 216=light salmon-peach, 215=sandy peach, 223=peach
        // 181=rosy beige, 230=cornsilk, 254=near-white

    // SLATE — soft steel, cool mist, powder blue-gray
    case Theme::Slate:
        colors.bg = -1; colors.search_box = 189; colors.title = 153; colors.channel = 189; colors.stats = 195;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = 152; colors.header = 189; colors.accent = 153; colors.tag = 195;
        colors.published = 146; colors.bookmark = 153; colors.desc = 253;
        return colors;
        // 189=periwinkle-mist, 153=light blue-gray, 152=cadet blue-light
        // 146=light slate blue, 195=alice blue, 253=light gray

    // MLTERM — strictly black & white. Every element is -1 (terminal default
    // fg/bg): no title/channel/stats/tag/border/etc colour differentiation at
    // all. Selection is NOT a coloured bar (tui.cpp forces plain reverse-video
    // when mono), and thumbnails are disabled entirely (no raster, no chafa),
    // so nothing on screen ever uses colour.
    case Theme::MLterm:
        colors.bg = -1; colors.search_box = -1; colors.title = -1; colors.channel = -1; colors.stats = -1;
        colors.selected = -1; colors.action = -1; colors.action_sel = -1; colors.status = -1;
        colors.border = -1; colors.header = -1; colors.accent = -1; colors.tag = -1;
        colors.published = -1; colors.bookmark = -1; colors.desc = -1;
        return colors;

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
