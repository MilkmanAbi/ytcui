#pragma once
/*
 * ytcui-dl — yt_itag.h
 *
 * Static itag → metadata table.
 *
 * Everything here is a property of the itag itself, not of the video, so it
 * never needs to be parsed out of a player response or stored per-format.
 * A StreamFormat keeps a pointer into this table instead of ~7 std::strings.
 *
 * Derived from live ANDROID/IOS/ANDROID_VR player responses (July 2026),
 * extended with the legacy itags yt-dlp still lists. Unknown itags fall back
 * to parsing mimeType out of the response, so an unrecognised itag degrades
 * to the old behaviour rather than being dropped.
 */

#include <cstdint>
#include <string_view>

namespace ytfast {

// Bit flags. Kept as a plain uint8 so the whole row stays small.
enum : uint8_t {
    IT_VIDEO = 1u << 0,
    IT_AUDIO = 1u << 1,
    IT_HDR   = 1u << 2,  // 10-bit / HDR profile
    IT_3D    = 1u << 3,
};

struct ItagInfo {
    uint16_t         itag;
    uint8_t          flags;
    uint8_t          fps;       // 0 = audio / unknown
    uint8_t          channels;  // 0 = video-only
    uint16_t         height;    // nominal; real height comes from the response
    std::string_view container; // "mp4" | "webm" | "3gp"
    std::string_view vcodec;    // "" when audio-only
    std::string_view acodec;    // "" when video-only
    std::string_view label;     // "1080p60", "medium" ...
    std::string_view mime;      // full static mimeType, avoids rebuilding it

    bool has_video() const { return flags & IT_VIDEO; }
    bool has_audio() const { return flags & IT_AUDIO; }
    bool muxed()     const { return (flags & (IT_VIDEO | IT_AUDIO)) == (IT_VIDEO | IT_AUDIO); }
};

namespace detail {

// Ordered by itag so lookup is a binary search.
static constexpr ItagInfo kItags[] = {
// itag flags                fps ch    h  container vcodec           acodec        label       mime
{  5, IT_VIDEO|IT_AUDIO,       30, 2, 240, "flv",  "h263",           "mp4a.40.2",  "240p",    "video/x-flv"                                     },
{ 17, IT_VIDEO|IT_AUDIO,       30, 1, 144, "3gp",  "mp4v.20.3",      "mp4a.40.2",  "144p",    "video/3gpp; codecs=\"mp4v.20.3, mp4a.40.2\""     },
{ 18, IT_VIDEO|IT_AUDIO,       30, 2, 360, "mp4",  "avc1.42001E",    "mp4a.40.2",  "360p",    "video/mp4; codecs=\"avc1.42001E, mp4a.40.2\""    },
{ 22, IT_VIDEO|IT_AUDIO,       30, 2, 720, "mp4",  "avc1.64001F",    "mp4a.40.2",  "720p",    "video/mp4; codecs=\"avc1.64001F, mp4a.40.2\""    },
{ 36, IT_VIDEO|IT_AUDIO,       30, 1, 240, "3gp",  "mp4v.20.3",      "mp4a.40.2",  "240p",    "video/3gpp; codecs=\"mp4v.20.3, mp4a.40.2\""     },
{ 43, IT_VIDEO|IT_AUDIO,       30, 2, 360, "webm", "vp8.0",          "vorbis",     "360p",    "video/webm; codecs=\"vp8.0, vorbis\""            },
{133, IT_VIDEO,                30, 0, 240, "mp4",  "avc1.4d4015",    "",           "240p",    "video/mp4; codecs=\"avc1.4d4015\""               },
{134, IT_VIDEO,                30, 0, 360, "mp4",  "avc1.4d401e",    "",           "360p",    "video/mp4; codecs=\"avc1.4d401e\""               },
{135, IT_VIDEO,                30, 0, 480, "mp4",  "avc1.4d401f",    "",           "480p",    "video/mp4; codecs=\"avc1.4d401f\""               },
{136, IT_VIDEO,                30, 0, 720, "mp4",  "avc1.4d401f",    "",           "720p",    "video/mp4; codecs=\"avc1.4d401f\""               },
{137, IT_VIDEO,                30, 0,1080, "mp4",  "avc1.640028",    "",           "1080p",   "video/mp4; codecs=\"avc1.640028\""               },
{138, IT_VIDEO,                30, 0,2160, "mp4",  "avc1.640033",    "",           "2160p",   "video/mp4; codecs=\"avc1.640033\""               },
{139, IT_AUDIO,                 0, 2,   0, "mp4",  "",               "mp4a.40.5",  "48kbps",  "audio/mp4; codecs=\"mp4a.40.5\""                 },
{140, IT_AUDIO,                 0, 2,   0, "mp4",  "",               "mp4a.40.2",  "128kbps", "audio/mp4; codecs=\"mp4a.40.2\""                 },
{141, IT_AUDIO,                 0, 2,   0, "mp4",  "",               "mp4a.40.2",  "256kbps", "audio/mp4; codecs=\"mp4a.40.2\""                 },
{160, IT_VIDEO,                30, 0, 144, "mp4",  "avc1.4d400c",    "",           "144p",    "video/mp4; codecs=\"avc1.4d400c\""               },
{171, IT_AUDIO,                 0, 2,   0, "webm", "",               "vorbis",     "128kbps", "audio/webm; codecs=\"vorbis\""                   },
{172, IT_AUDIO,                 0, 2,   0, "webm", "",               "vorbis",     "256kbps", "audio/webm; codecs=\"vorbis\""                   },
{212, IT_VIDEO,                30, 0, 480, "mp4",  "avc1.4d401f",    "",           "480p",    "video/mp4; codecs=\"avc1.4d401f\""               },
{242, IT_VIDEO,                30, 0, 240, "webm", "vp9",            "",           "240p",    "video/webm; codecs=\"vp9\""                      },
{243, IT_VIDEO,                30, 0, 360, "webm", "vp9",            "",           "360p",    "video/webm; codecs=\"vp9\""                      },
{244, IT_VIDEO,                30, 0, 480, "webm", "vp9",            "",           "480p",    "video/webm; codecs=\"vp9\""                      },
{247, IT_VIDEO,                30, 0, 720, "webm", "vp9",            "",           "720p",    "video/webm; codecs=\"vp9\""                      },
{248, IT_VIDEO,                30, 0,1080, "webm", "vp9",            "",           "1080p",   "video/webm; codecs=\"vp9\""                      },
{249, IT_AUDIO,                 0, 2,   0, "webm", "",               "opus",       "50kbps",  "audio/webm; codecs=\"opus\""                     },
{250, IT_AUDIO,                 0, 2,   0, "webm", "",               "opus",       "70kbps",  "audio/webm; codecs=\"opus\""                     },
{251, IT_AUDIO,                 0, 2,   0, "webm", "",               "opus",       "160kbps", "audio/webm; codecs=\"opus\""                     },
{256, IT_AUDIO,                 0, 6,   0, "mp4",  "",               "mp4a.40.5",  "5.1ch",   "audio/mp4; codecs=\"mp4a.40.5\""                 },
{258, IT_AUDIO,                 0, 6,   0, "mp4",  "",               "mp4a.40.2",  "5.1ch",   "audio/mp4; codecs=\"mp4a.40.2\""                 },
{264, IT_VIDEO,                30, 0,1440, "mp4",  "avc1.640032",    "",           "1440p",   "video/mp4; codecs=\"avc1.640032\""               },
{266, IT_VIDEO,                30, 0,2160, "mp4",  "avc1.640033",    "",           "2160p",   "video/mp4; codecs=\"avc1.640033\""               },
{271, IT_VIDEO,                30, 0,1440, "webm", "vp9",            "",           "1440p",   "video/webm; codecs=\"vp9\""                      },
{278, IT_VIDEO,                30, 0, 144, "webm", "vp9",            "",           "144p",    "video/webm; codecs=\"vp9\""                      },
{298, IT_VIDEO,                60, 0, 720, "mp4",  "avc1.4d4020",    "",           "720p60",  "video/mp4; codecs=\"avc1.4d4020\""               },
{299, IT_VIDEO,                60, 0,1080, "mp4",  "avc1.64002a",    "",           "1080p60", "video/mp4; codecs=\"avc1.64002a\""               },
{302, IT_VIDEO,                60, 0, 720, "webm", "vp9",            "",           "720p60",  "video/webm; codecs=\"vp9\""                      },
{303, IT_VIDEO,                60, 0,1080, "webm", "vp9",            "",           "1080p60", "video/webm; codecs=\"vp9\""                      },
{304, IT_VIDEO,                60, 0,1440, "mp4",  "avc1.64002a",    "",           "1440p60", "video/mp4; codecs=\"avc1.64002a\""               },
{305, IT_VIDEO,                60, 0,2160, "mp4",  "avc1.64002a",    "",           "2160p60", "video/mp4; codecs=\"avc1.64002a\""               },
{308, IT_VIDEO,                60, 0,1440, "webm", "vp9",            "",           "1440p60", "video/webm; codecs=\"vp9\""                      },
{313, IT_VIDEO,                30, 0,2160, "webm", "vp9",            "",           "2160p",   "video/webm; codecs=\"vp9\""                      },
{315, IT_VIDEO,                60, 0,2160, "webm", "vp9",            "",           "2160p60", "video/webm; codecs=\"vp9\""                      },
{327, IT_AUDIO,                 0, 6,   0, "mp4",  "",               "mp4a.40.2",  "5.1ch",   "audio/mp4; codecs=\"mp4a.40.2\""                 },
{328, IT_AUDIO,                 0, 6,   0, "mp4",  "",               "ec-3",       "5.1ch",   "audio/mp4; codecs=\"ec-3\""                      },
{330, IT_VIDEO|IT_HDR,         60, 0, 144, "webm", "vp9.2",          "",           "144p60",  "video/webm; codecs=\"vp9.2\""                    },
{331, IT_VIDEO|IT_HDR,         60, 0, 240, "webm", "vp9.2",          "",           "240p60",  "video/webm; codecs=\"vp9.2\""                    },
{332, IT_VIDEO|IT_HDR,         60, 0, 360, "webm", "vp9.2",          "",           "360p60",  "video/webm; codecs=\"vp9.2\""                    },
{333, IT_VIDEO|IT_HDR,         60, 0, 480, "webm", "vp9.2",          "",           "480p60",  "video/webm; codecs=\"vp9.2\""                    },
{334, IT_VIDEO|IT_HDR,         60, 0, 720, "webm", "vp9.2",          "",           "720p60",  "video/webm; codecs=\"vp9.2\""                    },
{335, IT_VIDEO|IT_HDR,         60, 0,1080, "webm", "vp9.2",          "",           "1080p60", "video/webm; codecs=\"vp9.2\""                    },
{336, IT_VIDEO|IT_HDR,         60, 0,1440, "webm", "vp9.2",          "",           "1440p60", "video/webm; codecs=\"vp9.2\""                    },
{337, IT_VIDEO|IT_HDR,         60, 0,2160, "webm", "vp9.2",          "",           "2160p60", "video/webm; codecs=\"vp9.2\""                    },
{338, IT_AUDIO,                 0, 4,   0, "webm", "",               "opus",       "quad",    "audio/webm; codecs=\"opus\""                     },
{380, IT_AUDIO,                 0, 6,   0, "mp4",  "",               "ac-3",       "5.1ch",   "audio/mp4; codecs=\"ac-3\""                      },
{394, IT_VIDEO,                30, 0, 144, "mp4",  "av01.0.00M.08",  "",           "144p",    "video/mp4; codecs=\"av01.0.00M.08\""             },
{395, IT_VIDEO,                30, 0, 240, "mp4",  "av01.0.00M.08",  "",           "240p",    "video/mp4; codecs=\"av01.0.00M.08\""             },
{396, IT_VIDEO,                30, 0, 360, "mp4",  "av01.0.01M.08",  "",           "360p",    "video/mp4; codecs=\"av01.0.01M.08\""             },
{397, IT_VIDEO,                30, 0, 480, "mp4",  "av01.0.04M.08",  "",           "480p",    "video/mp4; codecs=\"av01.0.04M.08\""             },
{398, IT_VIDEO,                60, 0, 720, "mp4",  "av01.0.08M.08",  "",           "720p60",  "video/mp4; codecs=\"av01.0.08M.08\""             },
{399, IT_VIDEO,                60, 0,1080, "mp4",  "av01.0.09M.08",  "",           "1080p60", "video/mp4; codecs=\"av01.0.09M.08\""             },
{400, IT_VIDEO,                60, 0,1440, "mp4",  "av01.0.12M.08",  "",           "1440p60", "video/mp4; codecs=\"av01.0.12M.08\""             },
{401, IT_VIDEO,                60, 0,2160, "mp4",  "av01.0.13M.08",  "",           "2160p60", "video/mp4; codecs=\"av01.0.13M.08\""             },
{571, IT_VIDEO,                60, 0,4320, "mp4",  "av01.0.16M.08",  "",           "4320p60", "video/mp4; codecs=\"av01.0.16M.08\""             },
{598, IT_VIDEO,                15, 0, 144, "webm", "vp9",            "",           "144p",    "video/webm; codecs=\"vp9\""                      },
{599, IT_AUDIO,                 0, 2,   0, "mp4",  "",               "mp4a.40.5",  "32kbps",  "audio/mp4; codecs=\"mp4a.40.5\""                 },
{600, IT_AUDIO,                 0, 2,   0, "webm", "",               "opus",       "35kbps",  "audio/webm; codecs=\"opus\""                     },
};

static constexpr size_t kItagCount = sizeof(kItags) / sizeof(kItags[0]);

} // namespace detail

// Binary search. Returns nullptr for itags we don't know, in which case the
// caller parses mimeType from the response instead.
inline const ItagInfo* itag_lookup(int itag) {
    size_t lo = 0, hi = detail::kItagCount;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int v = detail::kItags[mid].itag;
        if (v == itag) return &detail::kItags[mid];
        if (v < itag) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

// Rough "how good is this codec" ranking, used to break ties at equal
// resolution. Higher is better compression, not better compatibility.
inline int vcodec_rank(std::string_view c) {
    if (c.rfind("av01", 0) == 0) return 3;
    if (c.rfind("vp9",  0) == 0) return 2;
    if (c.rfind("vp09", 0) == 0) return 2;
    if (c.rfind("avc1", 0) == 0) return 1;
    return 0;
}

inline int acodec_rank(std::string_view c) {
    if (c.rfind("opus", 0) == 0) return 3;
    if (c.rfind("mp4a", 0) == 0) return 2;
    if (c.rfind("vorbis", 0) == 0) return 1;
    return 0;  // ac-3 / ec-3 rank low: bitstream is 5.1 but players vary
}

} // namespace ytfast
