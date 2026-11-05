// SponsorBlock parsing and skip logic.
//
// The fixture is the real response shape, confirmed by querying the live API
// while building this. Two things here are worth pinning: the hash prefix,
// because the whole privacy argument rests on only four characters leaving the
// machine, and the chained-segment case, because two adjacent sponsor reads
// must be one jump rather than two visible stutters.

#include "check.h"
#include "core/sponsorblock.h"

#include <cstdio>

using namespace ytcui::sb;

// Trimmed from a real /api/skipSegments/<prefix> response: a bucket holding
// several videos, only one of which is ours.
static const char* kResponse = R"([
  { "videoID": "someOtherVid",
    "segments": [ { "UUID": "x", "actionType": "skip", "category": "sponsor",
                    "segment": [0, 30.0], "votes": 5, "locked": false } ] },
  { "videoID": "ourVideo123",
    "segments": [
      { "UUID": "a", "actionType": "skip", "category": "intro",
        "segment": [0, 14.827], "votes": 0, "locked": false },
      { "UUID": "b", "actionType": "skip", "category": "sponsor",
        "segment": [60.0, 90.5], "votes": 12, "locked": true },
      { "UUID": "c", "actionType": "skip", "category": "selfpromo",
        "segment": [90.5, 105.0], "votes": 3, "locked": false },
      { "UUID": "d", "actionType": "skip", "category": "music_offtopic",
        "segment": [200.0, 215.0], "votes": 1, "locked": false },
      { "UUID": "e", "actionType": "poi", "category": "poi_highlight",
        "segment": [120.0, 120.0], "votes": 9, "locked": false },
      { "UUID": "f", "actionType": "full", "category": "sponsor",
        "segment": [0, 300.0], "votes": 2, "locked": false },
      { "UUID": "g", "actionType": "skip", "category": "sponsor",
        "segment": [250.0, 260.0], "votes": -5, "locked": false }
    ] }
])";

static void hashing_is_short_and_stable() {
    // The privacy claim: only this much is ever sent.
    const std::string p = hash_prefix("dQw4w9WgXcQ", 4);
    CHECK_EQ((int)p.size(), 4);
    // sha256("dQw4w9WgXcQ") starts 5f6b — checked against sha256sum.
    CHECK_EQ(p, std::string("5f6b"));

    // Stable, lowercase hex, and the prefix really is a prefix of the whole.
    const std::string full = hash_prefix("dQw4w9WgXcQ", 64);
    CHECK_EQ((int)full.size(), 64);
    CHECK_EQ(full.substr(0, 4), p);
    for (char ch : full)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            tst::report(__FILE__, __LINE__, "hash is not lowercase hex");

    CHECK(hash_prefix("", 4).size() == 4);     // must not crash on empty
}

static void parses_only_our_video() {
    const Segments s = parse(kResponse, "ourVideo123",
                             "sponsor,selfpromo,intro,outro,music_offtopic");
    CHECK(s.fetched);
    // The other video in the bucket must be ignored entirely.
    for (const Segment& g : s.list)
        if (g.start == 0 && g.end == 30.0)
            tst::report(__FILE__, __LINE__, "took a segment from another video");

    // intro, sponsor, selfpromo, music_offtopic = 4. The poi, the full-video
    // marker and the downvoted one are all excluded.
    CHECK_EQ((int)s.list.size(), 4);

    bool saw_poi = false, saw_full = false, saw_downvoted = false;
    for (const Segment& g : s.list) {
        if (g.action == "poi") saw_poi = true;
        if (g.action == "full") saw_full = true;
        if (g.votes < -1) saw_downvoted = true;
    }
    // A highlight marker is not a skip; a "full" marker means the whole video
    // is sponsor, and jumping to the end of it would skip the video.
    CHECK(!saw_poi);
    CHECK(!saw_full);
    CHECK(!saw_downvoted);

    // Sorted by start, which the skip logic relies on being sane.
    for (std::size_t i = 1; i < s.list.size(); ++i)
        CHECK(s.list[i - 1].start <= s.list[i].start);
}

static void category_filter() {
    // Asking only for sponsors must not skip the intro.
    const Segments only_sponsor = parse(kResponse, "ourVideo123", "sponsor");
    CHECK_EQ((int)only_sponsor.list.size(), 1);
    if (!only_sponsor.list.empty())
        CHECK_EQ(only_sponsor.list[0].category, std::string("sponsor"));

    const Segments none = parse(kResponse, "ourVideo123", "outro");
    CHECK(none.list.empty());
    CHECK(none.fetched);        // a successful lookup that found nothing
}

static void skipping() {
    const Segments s = parse(kResponse, "ourVideo123",
                             "sponsor,selfpromo,intro,outro,music_offtopic");

    // Inside the intro at 0 -> jump to its end.
    CHECK(s.skip_target(0.0) > 14.0);
    CHECK(s.skip_target(5.0) > 14.0);

    // Outside anything -> no jump. This is the common case and must be cheap
    // and quiet.
    CHECK_EQ((int)s.skip_target(30.0), -1);
    CHECK_EQ((int)s.skip_target(150.0), -1);
    CHECK_EQ((int)s.skip_target(400.0), -1);

    // THE case that matters: sponsor 60-90.5 runs straight into selfpromo
    // 90.5-105. One jump to 105, not two.
    const double t = s.skip_target(70.0);
    CHECK(t > 104.0 && t < 106.0);

    // Entering the second of the pair directly still lands at the far end.
    CHECK(s.skip_target(95.0) > 104.0);

    // A position past the end of a segment is not inside it.
    CHECK_EQ((int)s.skip_target(215.5), -1);
}

static void summary_text() {
    const Segments s = parse(kResponse, "ourVideo123",
                             "sponsor,selfpromo,intro,outro,music_offtopic");
    const std::string sum = s.summary();
    CHECK(!sum.empty());
    CHECK(sum.find("4 segments") != std::string::npos);
    // 14.827 + 30.5 + 14.5 + 15 = 74.8 -> "1:14"
    CHECK(sum.find("1:1") != std::string::npos);

    Segments empty;
    CHECK(empty.summary().empty());
    CHECK_EQ((int)empty.skip_target(10.0), -1);
}

static void malformed_is_survivable() {
    // This arrives from a third-party server, so it gets the same treatment as
    // anything else remote: never trusted to be well formed.
    for (const char* junk : {"", "{}", "null", "not json",
                             R"([{"videoID":"ourVideo123"}])",
                             R"([{"videoID":"ourVideo123","segments":[{}]}])",
                             R"([{"videoID":"ourVideo123","segments":[
                                  {"segment":[5]}]}])",
                             R"([{"videoID":"ourVideo123","segments":[
                                  {"segment":[10,5],"category":"sponsor"}]}])"}) {
        const Segments s = parse(junk, "ourVideo123", "sponsor");
        for (const Segment& g : s.list)
            if (g.end <= g.start)
                tst::report(__FILE__, __LINE__, "kept a backwards segment");
        (void)s.skip_target(5.0);      // must not crash
    }
}

static void category_splitting() {
    const auto c = split_categories("sponsor, selfpromo ,intro");
    CHECK_EQ((int)c.size(), 3);
    if (c.size() == 3) {
        CHECK_EQ(c[0], std::string("sponsor"));
        CHECK_EQ(c[1], std::string("selfpromo"));
        CHECK_EQ(c[2], std::string("intro"));
    }
    CHECK(split_categories("").empty());
    CHECK_EQ((int)split_categories("SPONSOR").size(), 1);
    if (!split_categories("SPONSOR").empty())
        CHECK_EQ(split_categories("SPONSOR")[0], std::string("sponsor"));
}

int main() {
    std::printf("sponsorblock\n");
    hashing_is_short_and_stable();
    parses_only_our_video();
    category_filter();
    skipping();
    summary_text();
    malformed_is_survivable();
    category_splitting();
    return tst::finish("test_sponsorblock");
}
