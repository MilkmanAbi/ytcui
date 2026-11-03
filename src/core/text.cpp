#include "core/text.h"

#include <cstdio>

namespace ytcui::txt {
namespace {

struct Range { uint32_t lo, hi; };

// Zero width: combining marks, variation selectors, and the zero-width joiner
// that holds a composed emoji together. Counting these would make every
// accented or flag-bearing title measure too wide.
const Range kZero[] = {
    {0x0300, 0x036F},   // combining diacritical marks
    {0x0483, 0x0489},
    {0x0591, 0x05BD},
    {0x0610, 0x061A},
    {0x064B, 0x065F},
    {0x0670, 0x0670},
    {0x06D6, 0x06DC},
    {0x0711, 0x0711},
    {0x0730, 0x074A},
    {0x07A6, 0x07B0},
    {0x0900, 0x0903},
    {0x093A, 0x093A},
    {0x093C, 0x093C},
    {0x0941, 0x0948},
    {0x094D, 0x094D},
    {0x0E31, 0x0E31},   // Thai
    {0x0E34, 0x0E3A},
    {0x0E47, 0x0E4E},
    {0x200B, 0x200F},   // zero-width space .. RTL mark (incl. ZWJ at 200D)
    {0x2028, 0x202E},
    {0x20D0, 0x20F0},
    {0xFE00, 0xFE0F},   // variation selectors (emoji presentation)
    {0xFE20, 0xFE2F},
    {0x1AB0, 0x1AFF},
    {0x1DC0, 0x1DFF},
    {0xE0100, 0xE01EF},
};

// Double width: East Asian Wide and Fullwidth, plus the emoji blocks that
// terminals render in two cells.
const Range kWide[] = {
    {0x1100, 0x115F},   // Hangul Jamo initial consonants
    {0x2E80, 0x303E},   // CJK radicals, Kangxi, CJK symbols
    {0x3041, 0x33FF},   // Hiragana, Katakana, Bopomofo, compat
    {0x3400, 0x4DBF},   // CJK ext A
    {0x4E00, 0x9FFF},   // CJK unified
    {0xA000, 0xA4CF},   // Yi
    {0xAC00, 0xD7A3},   // Hangul syllables
    {0xF900, 0xFAFF},   // CJK compatibility ideographs
    {0xFE30, 0xFE6F},   // CJK compatibility forms
    {0xFF00, 0xFF60},   // fullwidth forms
    {0xFFE0, 0xFFE6},
    {0x1F004, 0x1F004},  // mahjong red dragon
    {0x1F0CF, 0x1F0CF},
    {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A},
    {0x1F200, 0x1F2FF},
    {0x1F300, 0x1F5FF},  // misc symbols and pictographs (📚 lives here)
    {0x1F600, 0x1F64F},  // emoticons
    {0x1F680, 0x1F6FF},  // transport and map
    {0x1F900, 0x1F9FF},  // supplemental symbols and pictographs
    {0x1FA70, 0x1FAFF},
    {0x20000, 0x2FFFD},  // CJK ext B+
    {0x30000, 0x3FFFD},
};

bool in(const Range* table, std::size_t n, uint32_t cp) {
    // Linear is fine: these tables are tiny and this is called per character
    // on text that is already bounded by the terminal width.
    for (std::size_t i = 0; i < n; ++i)
        if (cp >= table[i].lo && cp <= table[i].hi) return true;
    return false;
}

}  // namespace

std::vector<uint32_t> decode_utf8(std::string_view s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0;
        int len = 0;
        if (c < 0x80)            { cp = c;        len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { out.push_back(0xFFFD); ++i; continue; }   // stray continuation

        if (i + (std::size_t)len > s.size()) {           // truncated at the end
            out.push_back(0xFFFD);
            break;
        }
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            const unsigned char cc = (unsigned char)s[i + (std::size_t)k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        out.push_back(cp);
        i += (std::size_t)len;
    }
    return out;
}

std::string encode_utf8(uint32_t cp) {
    std::string o;
    if (cp < 0x80) {
        o += (char)cp;
    } else if (cp < 0x800) {
        o += (char)(0xC0 | (cp >> 6));
        o += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += (char)(0xE0 | (cp >> 12));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    } else {
        o += (char)(0xF0 | (cp >> 18));
        o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

int codepoint_width(uint32_t cp) {
    if (cp == 0) return 0;
    // C0/C1 controls have no width here; they should never reach a cell.
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
    if (in(kZero, sizeof(kZero) / sizeof(*kZero), cp)) return 0;
    if (in(kWide, sizeof(kWide) / sizeof(*kWide), cp)) return 2;
    return 1;
}

int display_width(std::string_view s) {
    int w = 0;
    for (uint32_t cp : decode_utf8(s)) w += codepoint_width(cp);
    return w;
}

std::string clip_display(std::string_view s, int cols, std::string_view ellipsis) {
    if (cols <= 0) return {};
    if (display_width(s) <= cols) return std::string(s);

    const int ell = display_width(ellipsis);
    // No room for the text plus a marker: just fill what we can with the
    // marker, or nothing if even that will not fit.
    if (cols <= ell) return cols >= 1 ? std::string(ellipsis).substr(0, 0) : std::string();

    const int budget = cols - ell;
    const std::vector<uint32_t> cps = decode_utf8(s);
    std::string out;
    int w = 0;
    for (uint32_t cp : cps) {
        const int cw = codepoint_width(cp);
        // A zero-width mark belongs to the character before it, so it is kept
        // even at the boundary rather than being orphaned onto the ellipsis.
        if (cw > 0 && w + cw > budget) break;
        out += encode_utf8(cp);
        w += cw;
    }
    out += std::string(ellipsis);
    return out;
}

std::string pad_display(std::string_view s, int cols) {
    if (cols <= 0) return {};
    std::string out = clip_display(s, cols);
    const int w = display_width(out);
    if (w < cols) out.append((std::size_t)(cols - w), ' ');
    return out;
}

std::string views(int64_t n) {
    // Matches ytcui-dl's fmt_views exactly: one decimal place and a K/M/B
    // suffix. Any divergence shows up as the list reflowing when the backend
    // is switched, which looks like a bug in the switch.
    char b[32];
    if (n >= 1000000000)   std::snprintf(b, sizeof b, "%.1fB", (double)n / 1e9);
    else if (n >= 1000000) std::snprintf(b, sizeof b, "%.1fM", (double)n / 1e6);
    else if (n >= 1000)    std::snprintf(b, sizeof b, "%.1fK", (double)n / 1e3);
    else                   std::snprintf(b, sizeof b, "%lld", (long long)n);
    return b;
}

std::string duration(int seconds) {
    if (seconds <= 0) return {};
    const int h = seconds / 3600;
    const int m = (seconds % 3600) / 60;
    const int s = seconds % 60;
    char b[32];
    if (h > 0) std::snprintf(b, sizeof b, "%d:%02d:%02d", h, m, s);
    else       std::snprintf(b, sizeof b, "%d:%02d", m, s);
    return b;
}

}  // namespace ytcui::txt
