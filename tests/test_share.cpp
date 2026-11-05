// Share: QR encoding and the clipboard helpers.
//
// The QR matrix is worth testing properly because a wrong one still *looks*
// like a QR code -- it just never scans, and that failure is invisible until
// someone points a phone at it. The structural invariants below (finder
// patterns, quiet zone, size) are what a scanner actually looks for.

#include "check.h"
#include "core/share.h"
#include "core/url.h"

#include <cstdio>
#include <string>

using namespace ytcui;

static void encodes() {
    const std::string link = url::short_url("dQw4w9WgXcQ");
    const share::QrMatrix m = share::encode(link);

    CHECK(m.size > 0);
    // Every QR version is 21 + 4*(v-1) modules square.
    CHECK((m.size - 21) % 4 == 0);
    CHECK(m.size >= 21);
    CHECK_EQ((int)m.modules.size(), m.size * m.size);
    CHECK_EQ(m.quiet, 4);                       // the spec's minimum
    CHECK_EQ(m.full_size(), m.size + 8);

    // Finder patterns: a 7x7 dark ring with a 3x3 dark core, in three corners.
    // If these are wrong, nothing scans.
    const int corners[3][2] = {{0, 0}, {m.size - 7, 0}, {0, m.size - 7}};
    for (const auto& c : corners) {
        const int ox = c[0], oy = c[1];
        for (int i = 0; i < 7; ++i) {
            CHECK(m.at(ox + i, oy + 0));        // top edge
            CHECK(m.at(ox + i, oy + 6));        // bottom edge
            CHECK(m.at(ox + 0, oy + i));        // left edge
            CHECK(m.at(ox + 6, oy + i));        // right edge
        }
        // The separator ring inside the border must be light.
        CHECK(!m.at(ox + 1, oy + 1));
        CHECK(!m.at(ox + 5, oy + 5));
        // Solid 3x3 core.
        for (int y = 2; y <= 4; ++y)
            for (int x = 2; x <= 4; ++x)
                CHECK(m.at(ox + x, oy + y));
    }

    // The fourth corner is not a finder pattern.
    bool all_dark = true;
    for (int i = 0; i < 7; ++i)
        if (!m.at(m.size - 7 + i, m.size - 1)) { all_dark = false; break; }
    CHECK(!all_dark);

    // The quiet zone must be entirely light, or a scanner cannot find the edge.
    const int n = m.full_size();
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const bool in_quiet = x < m.quiet || y < m.quiet ||
                                  x >= m.quiet + m.size || y >= m.quiet + m.size;
            if (in_quiet && m.at_padded(x, y))
                tst::report(__FILE__, __LINE__,
                            "quiet zone is dark at " + std::to_string(x) + "," +
                                std::to_string(y));
        }
    }

    // Out of bounds reads light rather than crashing or wrapping.
    CHECK(!m.at(-1, 0));
    CHECK(!m.at(0, -1));
    CHECK(!m.at(m.size, 0));
    CHECK(!m.at(0, m.size));
}

static void shorter_link_is_smaller() {
    // The reason short_url() exists: fewer characters means a lower version,
    // which means it still scans when each module is one terminal cell.
    const share::QrMatrix shortm = share::encode(url::short_url("dQw4w9WgXcQ"));
    const share::QrMatrix longm  = share::encode(url::watch_url("dQw4w9WgXcQ"));
    CHECK(shortm.size > 0);
    CHECK(longm.size > 0);
    CHECK(shortm.size <= longm.size);
}

static void terminal_fit() {
    const std::string link = url::short_url("dQw4w9WgXcQ");
    // The whole point of half-block rendering: a standard terminal works.
    CHECK(share::fits_in(link, 80, 24));
    CHECK(!share::fits_in(link, 10, 4));

    const auto rows = share::to_ascii(share::encode(link));
    const share::QrMatrix m = share::encode(link);
    CHECK_EQ((int)rows.size(), m.full_size());
    for (const std::string& r : rows)
        CHECK_EQ((int)r.size(), m.full_size());

    // Only the two expected characters appear, so a renderer can map them.
    for (const std::string& r : rows)
        for (char ch : r)
            if (ch != 'X' && ch != ' ') {
                tst::report(__FILE__, __LINE__, "unexpected char in ascii QR");
                return;
            }
}

static void oversized_input() {
    // Far beyond any QR version's capacity: must report failure, not truncate
    // silently into a code that scans to the wrong URL.
    const std::string huge(8000, 'x');
    const share::QrMatrix m = share::encode(huge);
    CHECK_EQ(m.size, 0);
    CHECK(m.modules.empty());
    CHECK(share::to_ascii(m).empty());
    CHECK(!share::fits_in(huge, 200, 200));
}

static void osc52() {
    // base64 of a known string, so the escape sequence is verified rather
    // than assumed. "hi" -> "aGk=".
    const std::string seq = share::osc52_sequence("hi");
    CHECK(seq.rfind("\033]52;c;", 0) == 0);
    CHECK(seq.find("aGk=") != std::string::npos);
    CHECK(seq.back() == '\a');

    // Padding cases: lengths 1, 2 and 3 mod 3.
    CHECK(share::osc52_sequence("a").find("YQ==") != std::string::npos);
    CHECK(share::osc52_sequence("ab").find("YWI=") != std::string::npos);
    CHECK(share::osc52_sequence("abc").find("YWJj") != std::string::npos);

    const std::string link = url::short_url("dQw4w9WgXcQ");
    CHECK(share::osc52_sequence(link).size() > link.size());

    CHECK_EQ(std::string(share::to_string(share::CopyMethod::Osc52)),
             std::string("OSC 52"));
    CHECK_EQ(std::string(share::to_string(share::CopyMethod::None)),
             std::string("none"));
}

int main() {
    std::printf("share (qr + clipboard)\n");
    encodes();
    shorter_link_is_smaller();
    terminal_fit();
    oversized_input();
    osc52();
    return tst::finish("test_share");
}
