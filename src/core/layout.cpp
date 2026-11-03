#include "core/layout.h"

namespace ytcui::layout {
namespace {

// Gap between two adjacent visible columns.
constexpr int kGap = 1;

}  // namespace

int ResultColumns::total() const {
    int t = marker + title;
    if (channel  > 0) t += kGap + channel;
    if (duration > 0) t += kGap + duration;
    if (views    > 0) t += kGap + views;
    return t;
}

ResultColumns results_columns(int available) {
    ResultColumns c;
    if (available <= 0) { c.marker = 0; c.title = 0; return c; }

    // Below this there is no point trying to be a table.
    if (available < 12) {
        c.marker = available >= 2 ? 2 : 0;
        c.title  = available - c.marker;
        return c;
    }

    const int body = available - c.marker;

    // Widths that look right when there is room, and the order they are given
    // up in when there is not.
    constexpr int kDuration = 8;    // "6:10:58"
    constexpr int kViews    = 12;   // "58,468,989"
    const int channel_ideal = body / 4 < 24 ? body / 4 : 24;

    int remaining = body;
    // Title is never surrendered, so it is reserved first at a floor that is
    // still readable.
    constexpr int kTitleFloor = 20;

    auto fits = [&](int want) { return remaining - want >= kTitleFloor + kGap; };

    if (fits(channel_ideal + kGap) && channel_ideal >= 8) {
        c.channel = channel_ideal;
        remaining -= channel_ideal + kGap;
    }
    if (fits(kDuration + kGap)) {
        c.duration = kDuration;
        remaining -= kDuration + kGap;
    }
    if (fits(kViews + kGap)) {
        c.views = kViews;
        remaining -= kViews + kGap;
    }

    c.title = remaining;
    if (c.title < 1) c.title = 1;

    // Belt and braces: never hand back more than we were given. A rounding
    // slip here is exactly the bug this file exists to prevent.
    while (c.total() > available) {
        if (c.views > 0) c.views = 0;
        else if (c.channel > 0) c.channel = 0;
        else if (c.duration > 0) c.duration = 0;
        else if (c.title > 1) --c.title;
        else break;
    }
    return c;
}

}  // namespace ytcui::layout
