// Link parsing. This is the feature where a bug is invisible: a URL that is
// not recognised silently becomes a search query, and the user just sees odd
// results rather than an error. So every form is pinned here.

#include "check.h"
#include "core/url.h"

using namespace ytcui::url;

static void video_forms() {
    const char* forms[] = {
        "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
        "http://youtube.com/watch?v=dQw4w9WgXcQ",
        "https://m.youtube.com/watch?v=dQw4w9WgXcQ",
        "https://music.youtube.com/watch?v=dQw4w9WgXcQ",
        "https://youtu.be/dQw4w9WgXcQ",
        "youtu.be/dQw4w9WgXcQ",
        "www.youtube.com/watch?v=dQw4w9WgXcQ",
        "https://www.youtube.com/shorts/dQw4w9WgXcQ",
        "https://www.youtube.com/embed/dQw4w9WgXcQ",
        "https://www.youtube.com/live/dQw4w9WgXcQ",
        "https://www.youtube-nocookie.com/embed/dQw4w9WgXcQ",
        "https://www.youtube.com/watch?app=desktop&v=dQw4w9WgXcQ&feature=share",
        "dQw4w9WgXcQ",
    };
    for (const char* f : forms) {
        const Parsed p = parse(f);
        CHECK(p.kind == Kind::Video);
        CHECK_EQ(p.video_id, std::string("dQw4w9WgXcQ"));
    }
}

static void timestamps() {
    CHECK_EQ(parse_timestamp("90"), 90);
    CHECK_EQ(parse_timestamp("90s"), 90);
    CHECK_EQ(parse_timestamp("1m30s"), 90);
    CHECK_EQ(parse_timestamp("1h2m3s"), 3723);
    CHECK_EQ(parse_timestamp("2m"), 120);
    CHECK_EQ(parse_timestamp("1:30"), 90);
    CHECK_EQ(parse_timestamp("1:02:15"), 3735);
    CHECK_EQ(parse_timestamp("abc"), -1);
    CHECK_EQ(parse_timestamp(""), -1);

    // t= must survive into the parse, because dropping it silently restarts
    // the video from zero -- which looks like the link "worked".
    const Parsed a = parse("https://youtu.be/dQw4w9WgXcQ?t=43");
    CHECK(a.has_start);
    CHECK_EQ(a.start_seconds, 43);

    const Parsed b = parse("https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=1m30s");
    CHECK(b.has_start);
    CHECK_EQ(b.start_seconds, 90);

    const Parsed c = parse("https://youtu.be/dQw4w9WgXcQ");
    CHECK(!c.has_start);
}

static void playlists_and_channels() {
    const Parsed p = parse("https://www.youtube.com/playlist?list=PL1234567890");
    CHECK(p.kind == Kind::Playlist);
    CHECK_EQ(p.playlist_id, std::string("PL1234567890"));

    // A video inside a playlist is still a video, but the list comes along.
    const Parsed v = parse("https://www.youtube.com/watch?v=dQw4w9WgXcQ&list=PLabc");
    CHECK(v.kind == Kind::Video);
    CHECK_EQ(v.video_id, std::string("dQw4w9WgXcQ"));
    CHECK_EQ(v.playlist_id, std::string("PLabc"));

    const Parsed h = parse("https://www.youtube.com/@LofiGirl");
    CHECK(h.kind == Kind::Channel);
    CHECK_EQ(h.channel, std::string("@LofiGirl"));

    const Parsed ch = parse("https://www.youtube.com/channel/UC4QobU6STFB0P71PMvOGN5A");
    CHECK(ch.kind == Kind::Channel);
    CHECK_EQ(ch.channel, std::string("UC4QobU6STFB0P71PMvOGN5A"));

    CHECK(parse("https://www.youtube.com/c/SomeName").kind == Kind::Channel);
    CHECK(parse("https://www.youtube.com/user/jawed").kind == Kind::Channel);
}

static void non_links() {
    // The important negative cases: ordinary searches must not be mistaken
    // for links, or typing a query would start playing something.
    const char* queries[] = {
        "lofi hip hop", "rick astley", "how to cook rice", "",
        "hello world", "   ", "youtube", "music.youtube",
        "https://example.com/watch?v=dQw4w9WgXcQ",
        "https://vimeo.com/123456",
    };
    for (const char* q : queries) {
        const Parsed p = parse(q);
        if (p.ok()) tst::report(__FILE__, __LINE__,
                                std::string("treated as a link: \"") + q + "\"");
    }
    // An 11-character query that is not an id must stay a query.
    CHECK(!parse("hello world").ok());
    CHECK(!is_video_id("hello world"));
    CHECK(is_video_id("dQw4w9WgXcQ"));
    CHECK(!is_video_id("short"));
    CHECK(!is_video_id("twelvecharss"));
}

static void messy_input() {
    // What a paste actually looks like.
    const Parsed a = parse("check this out https://youtu.be/dQw4w9WgXcQ lol");
    CHECK(a.kind == Kind::Video);
    CHECK_EQ(a.video_id, std::string("dQw4w9WgXcQ"));

    const Parsed b = parse("  <https://www.youtube.com/watch?v=dQw4w9WgXcQ>  ");
    CHECK(b.kind == Kind::Video);

    const Parsed c = parse("\"https://youtu.be/dQw4w9WgXcQ\",");
    CHECK(c.kind == Kind::Video);

    const Parsed d = parse("https://youtu.be/dQw4w9WgXcQ#t=10");
    CHECK(d.kind == Kind::Video);

    CHECK(looks_like_url("https://youtu.be/x"));
    CHECK(looks_like_url("youtube.com/watch?v=dQw4w9WgXcQ"));
    CHECK(!looks_like_url("lofi hip hop"));
}

static void canonical_forms() {
    CHECK_EQ(watch_url("dQw4w9WgXcQ"),
             std::string("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
    // The short form is what the QR encoder gets, so it must stay short.
    CHECK_EQ(short_url("dQw4w9WgXcQ"), std::string("https://youtu.be/dQw4w9WgXcQ"));
    CHECK(channel_url("@LofiGirl").find("/@LofiGirl") != std::string::npos);
    CHECK(channel_url("UC4QobU6STFB0P71PMvOGN5A").find("/channel/") != std::string::npos);
}

int main() {
    std::printf("url parsing\n");
    video_forms();
    timestamps();
    playlists_and_channels();
    non_links();
    messy_input();
    canonical_forms();
    return tst::finish("test_url");
}
