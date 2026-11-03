#pragma once
// Column budgets for the results table.
//
// The first version of this hardcoded 55 columns for the title, 21 for the
// channel and so on. That totals about 99 columns, so in an ordinary 80-column
// terminal FTXUI quietly shrank every fixed-size cell to make the row fit --
// and the result was a table with no alignment at all, which looked like a
// text-measurement bug and was actually an arithmetic one.
//
// So widths are computed from the width actually available, columns are
// dropped when there is no room for them, and the invariant that the parts
// never exceed the whole is asserted by a test rather than assumed.
//
// Pure arithmetic, no FTXUI, so it tests without a terminal.

namespace ytcui::layout {

struct ResultColumns {
    int marker  = 2;    // "› "
    int title   = 0;
    int channel = 0;
    int duration = 0;
    int views   = 0;

    // What the row actually consumes, including the single space between
    // each visible column.
    int total() const;
    bool show_channel() const { return channel > 0; }
    bool show_duration() const { return duration > 0; }
    bool show_views() const { return views > 0; }
};

// `available` is the usable width inside the frame -- borders already removed.
// Degrades in a fixed order as space runs out: views first, then the channel,
// then the duration, leaving the title, because a list of videos with no
// titles is useless and a list with no view counts is merely plainer.
ResultColumns results_columns(int available);

}  // namespace ytcui::layout
