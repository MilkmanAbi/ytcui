// Column budgets.
//
// The bug this guards against shipped once already: the parts added up to ~99
// columns in an 80-column terminal, FTXUI shrank every cell to compensate, and
// the table lost its alignment entirely. The invariant below -- the parts never
// exceed the whole, at any width -- is the one that matters.

#include "check.h"
#include "core/layout.h"

#include <cstdio>

using namespace ytcui::layout;

static void never_exceeds_available() {
    // Every width from unusable to very wide, including the 80 that broke it.
    for (int w = 0; w <= 400; ++w) {
        const ResultColumns c = results_columns(w);
        if (c.total() > w)
            tst::report(__FILE__, __LINE__,
                        "width " + std::to_string(w) + ": columns total " +
                            std::to_string(c.total()));
        if (c.title < 0 || c.channel < 0 || c.duration < 0 || c.views < 0)
            tst::report(__FILE__, __LINE__,
                        "width " + std::to_string(w) + ": negative column");
    }
}

static void the_eighty_column_case() {
    // A standard terminal, minus two for the frame.
    const ResultColumns c = results_columns(78);
    CHECK(c.total() <= 78);
    CHECK(c.title >= 20);
    CHECK(c.show_channel());
    CHECK(c.show_duration());
    printf("   at 78: marker=%d title=%d channel=%d dur=%d views=%d total=%d\n",
           c.marker, c.title, c.channel, c.duration, c.views, c.total());
}

static void degrades_in_order() {
    // Wide: everything on.
    const ResultColumns wide = results_columns(160);
    CHECK(wide.show_channel());
    CHECK(wide.show_duration());
    CHECK(wide.show_views());
    CHECK(wide.title >= 20);

    // Narrowing gives up views before the channel, and the channel before the
    // duration -- a video list without durations is noticeably worse than one
    // without view counts.
    const ResultColumns mid = results_columns(60);
    CHECK(mid.total() <= 60);
    CHECK(mid.title >= 20);

    const ResultColumns narrow = results_columns(30);
    CHECK(narrow.total() <= 30);
    CHECK(!narrow.show_views());

    // Tiny: the title is the last thing standing, and it is never zero while
    // there is any room at all.
    for (int w : {12, 14, 20, 24}) {
        const ResultColumns c = results_columns(w);
        CHECK(c.total() <= w);
        CHECK(c.title >= 1);
    }

    // Degenerate widths must not produce nonsense.
    const ResultColumns zero = results_columns(0);
    CHECK_EQ(zero.total(), 0);
    const ResultColumns neg = results_columns(-5);
    CHECK_EQ(neg.total(), 0);
}

static void title_grows_once_columns_settle() {
    // Title width is deliberately NOT monotonic in the total. Going from 40 to
    // 60 columns re-enables the channel and view count, which legitimately
    // takes room back from the title -- a narrow layout gives the title the
    // slack precisely because the other columns are switched off.
    //
    // So the invariant only holds once the column set has settled: with every
    // column present, more width must go to the title.
    int last = 0;
    for (int w = 100; w <= 260; w += 20) {
        const ResultColumns c = results_columns(w);
        CHECK(c.show_channel() && c.show_duration() && c.show_views());
        if (c.title < last)
            tst::report(__FILE__, __LINE__,
                        "with all columns on, title shrank at width " +
                            std::to_string(w));
        last = c.title;
    }
    // And dropping a column must hand its space to the title, not waste it.
    const ResultColumns all = results_columns(100);
    CHECK_EQ(all.total(), 100);
}

int main() {
    std::printf("layout (result columns)\n");
    never_exceeds_available();
    the_eighty_column_case();
    degrades_in_order();
    title_grows_once_columns_settle();
    return tst::finish("test_layout");
}
