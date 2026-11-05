// Watch state: what counts as watched, and where to resume.
//
// The rules here are judgement calls, so they are pinned: quitting something
// you decided against must not mark it watched, resuming must not drop you
// into the credits, and re-watching half of something seen before must not
// un-mark it.

#include "check.h"
#include "core/watchstate.h"

#include <cstdio>

using namespace ytcui;

static void watched_rule() {
    WatchState w;

    // Opened, then abandoned after 10 seconds. Not watched.
    w.record("abandoned", "T", "C", 10, 600, /*finished=*/false);
    CHECK(!w.is_watched("abandoned"));

    // Played to the end.
    w.record("finished", "T", "C", 599, 600, /*finished=*/true);
    CHECK(w.is_watched("finished"));

    // Got to 95% and quit through the outro: counts.
    w.record("outro", "T", "C", 580, 600, /*finished=*/false);
    CHECK(w.is_watched("outro"));

    // Half way is not watched.
    w.record("half", "T", "C", 300, 600, /*finished=*/false);
    CHECK(!w.is_watched("half"));

    // Re-watching the start of something already seen must not un-mark it.
    w.record("finished", "T", "C", 5, 600, /*finished=*/false);
    CHECK(w.is_watched("finished"));

    // Unknown duration: only an explicit finish marks it.
    w.record("nodur", "T", "C", 5000, 0, /*finished=*/false);
    CHECK(!w.is_watched("nodur"));
    w.record("nodur", "T", "C", 5000, 0, /*finished=*/true);
    CHECK(w.is_watched("nodur"));
}

static void resume_rule() {
    WatchState w;

    // Too early to be worth resuming: you were sampling.
    w.record("sampled", "T", "C", 12, 600, false);
    CHECK_EQ((int)w.resume_position("sampled"), 0);

    // Properly into it.
    w.record("midway", "T", "C", 300, 600, false);
    CHECK_EQ((int)w.resume_position("midway"), 300);

    // Near the end: resuming would land in the credits.
    w.record("nearend", "T", "C", 595, 600, false);
    CHECK_EQ((int)w.resume_position("nearend"), 0);

    // Watched things do not offer a resume.
    w.record("done", "T", "C", 300, 600, true);
    CHECK_EQ((int)w.resume_position("done"), 0);

    // Unknown id.
    CHECK_EQ((int)w.resume_position("never-seen"), 0);

    // Marking unwatched clears the position, so it starts over.
    w.mark_watched("midway", false);
    CHECK(!w.is_watched("midway"));
    CHECK_EQ((int)w.resume_position("midway"), 0);
}

static void progress_display() {
    WatchState w;
    w.record("a", "T", "C", 150, 600, false);
    const WatchEntry* e = w.find("a");
    CHECK(e != nullptr);
    if (e) CHECK_EQ((int)(e->progress() * 100), 25);

    w.record("b", "T", "C", 600, 600, true);
    const WatchEntry* f = w.find("b");
    if (f) CHECK_EQ((int)(f->progress() * 100), 100);

    // No duration means no meaningful bar, rather than a divide by zero.
    w.record("c", "T", "C", 60, 0, false);
    const WatchEntry* g = w.find("c");
    if (g) CHECK_EQ((int)(g->progress() * 100), 0);
}

static void round_trip() {
    WatchState w;
    w.record("one", "First video", "Chan A", 120, 600, false);
    w.record("two", "Second ✨ emoji", "Chan B", 600, 600, true);
    CHECK(w.dirty());

    const std::string json = w.serialize();
    WatchState back;
    CHECK(back.parse(json));
    CHECK_EQ((int)back.size(), 2);
    CHECK(back.is_watched("two"));
    CHECK(!back.is_watched("one"));
    CHECK_EQ((int)back.resume_position("one"), 120);

    const WatchEntry* e = back.find("two");
    CHECK(e != nullptr);
    if (e) {
        CHECK_EQ(e->title, std::string("Second ✨ emoji"));
        CHECK_EQ(e->channel, std::string("Chan B"));
    }
    CHECK(!back.dirty());        // freshly loaded is not pending a save

    // Malformed input must not throw or invent entries.
    for (const char* junk : {"", "{}", "[]", "null", "not json",
                             R"({"videos":null})", R"({"videos":[{}]})"}) {
        WatchState bad;
        bad.parse(junk);
        CHECK_EQ((int)bad.size(), 0);
    }
}

static void file_io() {
    const std::string path = "/tmp/ytcui-test-watch.json";
    std::remove(path.c_str());

    WatchState w;
    w.record("x", "Title", "Chan", 200, 500, false);
    CHECK(w.save_to(path));
    CHECK(!w.dirty());

    WatchState back;
    CHECK(back.load_from(path));
    CHECK_EQ((int)back.resume_position("x"), 200);

    // A missing file is a normal first run, not an error to report.
    WatchState missing;
    CHECK(!missing.load_from("/tmp/ytcui-definitely-not-here.json"));
    CHECK_EQ((int)missing.size(), 0);

    std::remove(path.c_str());
}

static void recent_order() {
    WatchState w;
    w.record("old", "T", "C", 10, 600, false);
    w.record("mid", "T", "C", 10, 600, false);
    w.record("new", "T", "C", 10, 600, false);
    const auto r = w.recent(10);
    CHECK_EQ((int)r.size(), 3);
    // All three land in the same second, so the only firm requirement is that
    // every one is present exactly once -- asserting an order here would be
    // asserting the clock's resolution.
    bool o = false, m = false, n = false;
    for (const WatchEntry& e : r) {
        o |= e.id == "old";
        m |= e.id == "mid";
        n |= e.id == "new";
    }
    CHECK(o && m && n);

    CHECK_EQ((int)w.recent(2).size(), 2);
    w.forget("mid");
    CHECK_EQ((int)w.size(), 2);
    w.clear();
    CHECK_EQ((int)w.size(), 0);
}

int main() {
    std::printf("watch state\n");
    watched_rule();
    resume_rule();
    progress_display();
    round_trip();
    file_io();
    recent_order();
    return tst::finish("test_watchstate");
}
