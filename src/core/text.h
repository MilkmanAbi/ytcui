#pragma once
// Text measured in terminal columns, not bytes and not codepoints.
//
// This is the bug class v4 wrote a whole document about. YouTube titles are
// full of CJK, emoji and ZWJ sequences, and three different lengths are easy
// to confuse:
//
//   "lofi 📚 beats"   13 codepoints, 16 bytes, 14 columns
//
// Truncating by bytes can cut a UTF-8 sequence in half, which renders as a
// replacement glyph. Truncating by codepoints -- which is what a first pass
// here did -- keeps the text valid but still overflows the column, because an
// emoji occupies two cells. The result is every row after it shifted by one,
// which is what "beats to relax/study tLofi Girl" looked like on screen.
//
// wcwidth(3) would answer this, but it depends on the process locale and on
// the C library's tables, and glibc reports width 1 for most emoji. A small
// explicit range table is deterministic, testable, needs no locale, and gives
// the same answer on Linux, macOS and the BSDs -- which matters because the
// layout must not differ per platform.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ytcui::txt {

// Decode UTF-8, replacing malformed bytes with U+FFFD rather than throwing or
// stopping: this runs on whatever a video title happens to contain.
std::vector<uint32_t> decode_utf8(std::string_view s);

std::string encode_utf8(uint32_t cp);

// Terminal cells this codepoint occupies: 0 for combining marks and
// zero-width joiners, 2 for East Asian wide and most emoji, 1 otherwise.
int codepoint_width(uint32_t cp);

// Total columns. Grapheme clusters are respected to the extent that matters
// here: a combining mark adds nothing, so "e" + U+0301 measures 1.
int display_width(std::string_view s);

// Truncate to at most `cols` terminal columns, appending `ellipsis` when
// something was removed. Never splits a UTF-8 sequence, never returns more
// than `cols` columns, and never splits a combining mark from its base.
std::string clip_display(std::string_view s, int cols,
                         std::string_view ellipsis = "…");

// Pad on the right to exactly `cols` columns (clipping first if needed), for
// fixed-width table columns.
std::string pad_display(std::string_view s, int cols);

// Safe, single-line output measured with the same locale tables curses uses.
// Controls, bidi overrides and terminal-dependent joiners/selectors are removed.
std::string terminal_sanitize(std::string_view text, bool unicode = true);
int terminal_width(std::string_view text, bool unicode = true);
std::string terminal_clip(std::string_view text, int cols, bool unicode = true,
                          bool ellipsis = true);

// Display formatting shared by both backends.
//
// These exist so that switching backend does not reformat the list under the
// user. ytcui-dl renders view counts as "447.5M" and durations as "3:33"
// internally; yt-dlp hands back raw integers, so it formats them through these
// to land on exactly the same text.
std::string views(int64_t n);          // 447515632 -> "447.5M"
std::string duration(int seconds);     // 213 -> "3:33", 22258 -> "6:10:58"

}  // namespace ytcui::txt
