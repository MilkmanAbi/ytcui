// Column measurement. v4 shipped two real byte-vs-codepoint truncation bugs,
// and the first version of this slice shipped a third: an emoji counted as one
// column, so every field after it on the row shifted left by one and the
// results table visibly lost its alignment.

#include "check.h"
#include "core/text.h"

#include <cstdio>

using namespace ytcui::txt;

static void widths() {
    CHECK_EQ(display_width(""), 0);
    CHECK_EQ(display_width("hello"), 5);

    // CJK and Hangul are two cells each.
    CHECK_EQ(display_width("値"), 2);
    CHECK_EQ(display_width("日本語"), 6);
    CHECK_EQ(display_width("한국어"), 6);

    // Emoji are two cells. glibc's wcwidth says one, which is why there is a
    // table here instead of a wcwidth call.
    CHECK_EQ(display_width("📚"), 2);
    CHECK_EQ(display_width("🎵"), 2);

    // A combining mark adds nothing to its base.
    CHECK_EQ(display_width("e\xcc\x81"), 1);          // e + U+0301
    // A variation selector is presentation, not a cell.
    CHECK_EQ(display_width("\xe2\x9c\x94\xef\xb8\x8f"), 1 + 0 + 0 == 1 ? 1 : 1);

    // The real-world string that exposed the bug.
    CHECK_EQ(display_width("lofi hip hop radio 📚 beats"),
             (int)std::string("lofi hip hop radio  beats").size() + 2);

    // Mixed script.
    CHECK_EQ(display_width("ünïcødé-値-🎵"), 7 + 1 + 2 + 1 + 2);
}

static void decoding_is_total() {
    // Whatever a title contains, decoding must not crash or drop into an
    // infinite loop.
    CHECK_EQ((int)decode_utf8("abc").size(), 3);
    CHECK_EQ((long)decode_utf8("値")[0], 0x5024L);

    // Malformed input becomes U+FFFD rather than throwing.
    const char* junk[] = {"\xff", "\xc3", "\xe0\x80", "\x80\x80", "a\xff" "b"};
    for (const char* j : junk) {
        const auto cps = decode_utf8(j);
        CHECK(!cps.empty());
        (void)display_width(j);        // must not crash
    }

    // Round trip.
    for (uint32_t cp : {0x41u, 0xE9u, 0x5024u, 0x1F3B5u}) {
        const auto back = decode_utf8(encode_utf8(cp));
        CHECK_EQ((int)back.size(), 1);
        if (!back.empty()) CHECK_EQ((long)back[0], (long)cp);
    }
}

static void clipping_respects_columns() {
    // Nothing to do.
    CHECK_EQ(clip_display("short", 10), std::string("short"));
    CHECK_EQ(clip_display("", 10), std::string(""));
    CHECK_EQ(clip_display("abc", 0), std::string(""));

    // The invariant that actually matters: the result never exceeds the budget.
    const char* samples[] = {
        "lofi hip hop radio 📚 beats to relax/study to",
        "日本語のタイトルはここにあります",
        "ünïcødé-値-🎵 mixed script title",
        "plain ascii title that is quite long indeed",
        "🎵🎵🎵🎵🎵🎵🎵🎵🎵🎵",
        "e\xcc\x81" "combining at the start",
    };
    for (const char* s : samples) {
        for (int cols : {1, 2, 3, 5, 10, 20, 21, 40, 55}) {
            const std::string out = clip_display(s, cols);
            const int w = display_width(out);
            if (w > cols)
                tst::report(__FILE__, __LINE__,
                            std::string("clip to ") + std::to_string(cols) +
                                " produced width " + std::to_string(w) +
                                " for \"" + s + "\"");
            // And it must still be valid UTF-8: no U+FFFD introduced by a cut
            // through the middle of a sequence.
            for (uint32_t cp : decode_utf8(out))
                if (cp == 0xFFFD)
                    tst::report(__FILE__, __LINE__,
                                std::string("clip split a sequence in \"") + s + "\"");
        }
    }

    // An emoji must not be half-included to fill an odd budget.
    const std::string two = clip_display("📚📚📚", 3);
    CHECK(display_width(two) <= 3);
}

static void padding_is_exact() {
    // Fixed-width table columns depend on this being exact, wide chars or not.
    CHECK_EQ(display_width(pad_display("abc", 10)), 10);
    CHECK_EQ(display_width(pad_display("日本語", 10)), 10);
    CHECK_EQ(display_width(pad_display("📚 lofi", 10)), 10);
    CHECK_EQ(display_width(pad_display("way too long to fit at all", 10)), 10);
    CHECK_EQ(pad_display("ab", 5), std::string("ab   "));
    CHECK_EQ(pad_display("", 3), std::string("   "));
}

int main() {
    std::printf("text (terminal columns)\n");
    widths();
    decoding_is_total();
    clipping_respects_columns();
    padding_is_exact();
    return tst::finish("test_text");
}
