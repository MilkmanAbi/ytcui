#pragma once
// SponsorBlock: skip sponsor reads, intros, outros and self-promo.
//
// This was the clearest finding of the survey in docs/RESEARCH.md -- it is the
// feature every comparable tool leads with, and the one most of them only get
// indirectly by telling the user to install an mpv script. Doing it here means
// the segment data is also available to the UI, so a video's sponsor load can
// be shown *before* committing to watching it, which nothing else does.
//
// Privacy is the reason the lookup is shaped the way it is. The request goes
// to /api/skipSegments/<first 4 hex chars of sha256(videoID)> and comes back
// with every video sharing that prefix -- 128 of them in the bucket measured
// while building this -- so the server is never told which video is being
// watched. That is what makes it defensible to have on by default; it can
// still be turned off entirely, in which case no request is made at all.

#include <cstdint>
#include <string>
#include <vector>

namespace ytcui::sb {

struct Segment {
    double      start = 0;
    double      end = 0;
    std::string category;      // sponsor, intro, outro, selfpromo, ...
    std::string action;        // skip, mute, full, poi
    std::string uuid;
    int         votes = 0;
    bool        locked = false;

    double length() const { return end > start ? end - start : 0; }
    // "full" marks a whole video as sponsor and "poi" is a highlight marker;
    // neither should make the player jump.
    bool skippable() const { return action.empty() || action == "skip"; }
};

struct Segments {
    std::vector<Segment> list;
    bool    fetched = false;   // a lookup completed, even if it found nothing
    std::string error;

    bool empty() const { return list.empty(); }
    double total_skippable() const;
    // Human summary for the UI: "2 segments · 1:47 skippable".
    std::string summary() const;

    // The end of the skippable segment containing `pos`, or -1.
    // `grace` keeps a seek landing a hair inside the next segment from
    // immediately re-triggering, which otherwise looks like a stutter.
    double skip_target(double pos, double grace = 0.15) const;
};

// sha256(video_id), lowercase hex, first `chars` characters.
std::string hash_prefix(const std::string& video_id, int chars = 4);

// Parse the API's response. `video_id` selects this video out of the bucket of
// videos sharing the hash prefix.
Segments parse(const std::string& json, const std::string& video_id,
               const std::string& wanted_categories);

// Fetch over HTTPS. Blocking, so callers run it off the UI thread.
// `categories` is the comma-separated config string.
Segments fetch(const std::string& video_id, const std::string& categories,
               int timeout_ms = 8000);

// The default set, and what the API understands.
std::vector<std::string> split_categories(const std::string& csv);

}  // namespace ytcui::sb
