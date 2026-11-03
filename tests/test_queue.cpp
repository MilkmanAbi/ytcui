// The play queue.

#include "check.h"
#include "core/queue.h"

#include <cstdio>

using namespace ytui;

static Video vid(const std::string& id, int secs = 100) {
    Video v;
    v.id = id;
    v.title = "Title " + id;
    v.duration_seconds = secs;
    return v;
}

static void adding_and_taking() {
    Queue q;
    CHECK(q.empty());
    CHECK(q.peek() == nullptr);
    CHECK(q.take().empty());        // taking from empty must be safe

    q.add(vid("a"));
    q.add(vid("b"));
    q.add(vid("c"));
    CHECK_EQ((int)q.size(), 3);
    CHECK(q.peek() && q.peek()->id == "a");

    CHECK_EQ(q.take().id, std::string("a"));
    CHECK_EQ((int)q.size(), 2);
    CHECK(q.peek() && q.peek()->id == "b");

    // An empty video is not queueable.
    q.add(Video{});
    CHECK_EQ((int)q.size(), 2);
}

static void no_accidental_duplicates() {
    Queue q;
    q.add(vid("a"));
    q.add(vid("a"));
    // A second press of the queue key is a double keypress, not a request to
    // watch the same thing twice in a row.
    CHECK_EQ((int)q.size(), 1);
    CHECK(q.contains("a"));
    CHECK(!q.contains("zzz"));
    CHECK(!q.contains(""));
}

static void play_next_jumps_the_line() {
    Queue q;
    q.add(vid("a"));
    q.add(vid("b"));
    q.play_next(vid("c"));
    CHECK(q.peek() && q.peek()->id == "c");
    CHECK_EQ((int)q.size(), 3);

    // Play-next on something already waiting means "move it up", not
    // "add a second copy".
    q.play_next(vid("b"));
    CHECK_EQ((int)q.size(), 3);
    CHECK(q.peek() && q.peek()->id == "b");
}

static void reordering() {
    Queue q;
    q.add(vid("a"));
    q.add(vid("b"));
    q.add(vid("c"));

    CHECK(q.move_down(0));
    CHECK_EQ(q.items()[0].id, std::string("b"));
    CHECK_EQ(q.items()[1].id, std::string("a"));

    CHECK(q.move_up(1));
    CHECK_EQ(q.items()[0].id, std::string("a"));

    // The ends do not wrap: wrapping a move is never what anyone means.
    CHECK(!q.move_up(0));
    CHECK(!q.move_down(q.size() - 1));
    // Out of range is a no-op, not a crash.
    CHECK(!q.move_up(99));
    CHECK(!q.move_down(99));
    q.remove(99);
    CHECK_EQ((int)q.size(), 3);

    q.remove(1);
    CHECK_EQ((int)q.size(), 2);
    q.clear();
    CHECK(q.empty());
}

static void total_runtime() {
    Queue q;
    q.add(vid("a", 100));
    q.add(vid("b", 200));
    CHECK_EQ(q.total_seconds(), 300);

    // A live stream has no length, and the header must not claim one.
    Video live = vid("live", 0);
    live.is_live = true;
    q.add(live);
    CHECK_EQ(q.total_seconds(), 300);
}

int main() {
    std::printf("queue\n");
    adding_and_taking();
    no_accidental_duplicates();
    play_next_jumps_the_line();
    reordering();
    total_runtime();
    return tst::finish("test_queue");
}
