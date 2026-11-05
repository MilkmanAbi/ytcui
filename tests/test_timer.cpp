// The sleep timer, and the forgiving duration parsing it depends on.
//
// Every spelling below is one a person might actually type at a prompt. A
// timer that rejects "23 min" because it wanted "23m" is a timer nobody sets
// twice, so the accepted set is pinned here rather than left to drift.

#include "check.h"
#include "core/timer.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace ytcui;

static void spellings_people_use() {
    // Compact.
    CHECK_EQ(parse_duration("1h23m"), 3600 + 23 * 60);
    CHECK_EQ(parse_duration("23m"), 23 * 60);
    CHECK_EQ(parse_duration("1h"), 3600);
    CHECK_EQ(parse_duration("30s"), 30);
    CHECK_EQ(parse_duration("1h23m45s"), 3600 + 23 * 60 + 45);

    // Spaced.
    CHECK_EQ(parse_duration("1h 23m"), 3600 + 23 * 60);
    CHECK_EQ(parse_duration("1h 23m 45s"), 3600 + 23 * 60 + 45);
    CHECK_EQ(parse_duration("23 min"), 23 * 60);
    CHECK_EQ(parse_duration("30 sec"), 30);
    CHECK_EQ(parse_duration("30sec"), 30);

    // Written out.
    CHECK_EQ(parse_duration("1 hour"), 3600);
    CHECK_EQ(parse_duration("2 hours"), 7200);
    CHECK_EQ(parse_duration("23 minutes"), 23 * 60);
    CHECK_EQ(parse_duration("45 seconds"), 45);
    CHECK_EQ(parse_duration("1 hr"), 3600);
    CHECK_EQ(parse_duration("90 mins"), 90 * 60);

    // Case and stray punctuation.
    CHECK_EQ(parse_duration("1H23M"), 3600 + 23 * 60);
    CHECK_EQ(parse_duration("  45m  "), 45 * 60);
    CHECK_EQ(parse_duration("1h, 30m"), 3600 + 30 * 60);
}

static void bare_number_means_minutes() {
    // The convention every sleep timer uses: someone typing 45 at a
    // sleep-timer prompt means three quarters of an hour.
    CHECK_EQ(parse_duration("45"), 45 * 60);
    CHECK_EQ(parse_duration("1"), 60);
    CHECK_EQ(parse_duration(" 90 "), 90 * 60);
}

static void colon_form_is_hours_and_minutes() {
    // Also a timer convention: 1:30 is an hour and a half, not 90 seconds.
    CHECK_EQ(parse_duration("1:30"), 3600 + 30 * 60);
    CHECK_EQ(parse_duration("0:45"), 45 * 60);
    CHECK_EQ(parse_duration("2:05"), 2 * 3600 + 5 * 60);
    // 90 minutes past the hour is not a time anyone means.
    CHECK_EQ(parse_duration("1:90"), -1);
    CHECK_EQ(parse_duration("1:"), -1);
    CHECK_EQ(parse_duration(":30"), -1);
}

static void rejects_nonsense() {
    // Rejected rather than guessed: silently starting a 0-second or absurd
    // timer is worse than saying it could not be read.
    for (const char* bad : {"", "   ", "abc", "h", "m", "banana",
                            "23 potatoes", "1h 20", "-5m", "1h2x",
                            "999999999h"}) {
        const int r = parse_duration(bad);
        if (r != -1)
            tst::report(__FILE__, __LINE__,
                        std::string("accepted \"") + bad + "\" as " +
                            std::to_string(r));
    }
}

static void formatting_round_trips() {
    CHECK_EQ(format_duration(3600), std::string("1h"));
    CHECK_EQ(format_duration(3600 + 23 * 60), std::string("1h 23m"));
    CHECK_EQ(format_duration(45 * 60), std::string("45m"));
    CHECK_EQ(format_duration(30), std::string("30s"));
    CHECK_EQ(format_duration(0), std::string("0s"));
    CHECK_EQ(format_duration(-5), std::string("0s"));
    // Seconds show alongside minutes only near the end of a countdown.
    CHECK_EQ(format_duration(2 * 60 + 5), std::string("2m 5s"));
    CHECK_EQ(format_duration(45 * 60 + 5), std::string("45m"));

    // What is printed must be re-readable.
    for (int secs : {30, 60, 90 * 60, 3600, 3600 + 23 * 60}) {
        const int back = parse_duration(format_duration(secs));
        if (back != secs)
            tst::report(__FILE__, __LINE__,
                        "round trip: " + std::to_string(secs) + " -> \"" +
                            format_duration(secs) + "\" -> " +
                            std::to_string(back));
    }
}

static void timer_lifecycle() {
    SleepTimer t;
    CHECK(!t.active());
    CHECK_EQ(t.remaining(), 0);
    CHECK(!t.fire_if_due());
    CHECK(t.describe().empty());

    // Zero and negative are refused rather than firing instantly.
    CHECK(!t.set(0));
    CHECK(!t.set(-10));
    CHECK(!t.active());

    CHECK(t.set(3600));
    CHECK(t.active());
    CHECK_EQ(t.total(), 3600);
    CHECK(t.remaining() > 3590 && t.remaining() <= 3600);
    CHECK(!t.fire_if_due());                 // not yet
    CHECK(t.describe().find("sleep in") != std::string::npos);

    t.cancel();
    CHECK(!t.active());
    CHECK_EQ(t.remaining(), 0);
}

static void fires_exactly_once() {
    SleepTimer t;
    // A timer already due: the steady clock cannot be wound forward in a test,
    // so this is the one-second case.
    CHECK(t.set(1));
    // A real wait. A busy-loop here was the first attempt and it was wrong:
    // it finished in tens of milliseconds and the timer had not come due, so
    // the test failed on its own impatience rather than on the code. One
    // second of suite time is the honest price for testing a one-second timer.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(t.fire_if_due());
    // The contract that matters: having fired, it must not fire again, or the
    // player would be re-paused on every poll and never resume.
    CHECK(!t.fire_if_due());
    CHECK(!t.fire_if_due());
    CHECK_EQ(t.remaining(), 0);

    // Setting it again re-arms cleanly.
    CHECK(t.set(3600));
    CHECK(!t.fire_if_due());
    CHECK(t.remaining() > 3000);
}

int main() {
    std::printf("sleep timer\n");
    spellings_people_use();
    bare_number_means_minutes();
    colon_form_is_hours_and_minutes();
    rejects_nonsense();
    formatting_round_trips();
    timer_lifecycle();
    fires_exactly_once();
    return tst::finish("test_timer");
}
