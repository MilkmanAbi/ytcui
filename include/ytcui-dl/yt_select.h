#pragma once
/*
 * ytcui-dl — yt_select.h
 *
 * Format selection: three playback modes, a resolution ladder, and switching
 * between resolutions without disturbing playback more than necessary.
 *
 * The design point that matters for a player: in AudioVideo mode the video and
 * audio tracks are selected independently. Changing resolution therefore only
 * swaps the video stream -- the audio track keeps its identity, so a player can
 * reload the video without a gap in sound and without re-buffering audio.
 * switch_video() exists to make that explicit.
 *
 * Nothing here allocates or does I/O. It operates on an already-fetched
 * format list, so it is safe to call from a UI thread on every keystroke.
 */

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "yt_caps.h"
#include "yt_types.h"

namespace ytfast {

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------
enum class Mode {
    AudioOnly,   // audio track only — music playback, radio, background
    VideoOnly,   // video track only — silent preview, thumbnails, scrubbing
    AudioVideo,  // both — normal media playback
};

inline const char* mode_name(Mode m) {
    switch (m) {
        case Mode::AudioOnly:  return "audio-only";
        case Mode::VideoOnly:  return "video-only";
        default:               return "audio+video";
    }
}

// ---------------------------------------------------------------------------
// What the caller wants. Every field is optional; zeroes and empty views mean
// "no constraint", so a default-constructed Quality asks for the best
// available and always resolves to something.
// ---------------------------------------------------------------------------
struct Quality {
    int  height         = 0;      // target height; 0 = best available
    int  max_height     = 0;      // hard ceiling; 0 = none
    int  fps            = 0;      // preferred fps (60 to favour high frame rate)
    bool prefer_hdr     = false;
    bool allow_surround = true;   // false pins audio to stereo
    bool allow_muxed    = true;   // false forbids the muxed fallback

    // Invert every quality comparison: smallest picture, lowest bitrate. For
    // metered or very slow links, where "best" is the wrong default.
    bool smallest       = false;

    // Rank by what this machine can actually play rather than by compression.
    // Codecs with no decoder are excluded outright; software-only codecs are
    // deprioritised above 1080p. Set for playback, leave off for downloading,
    // where the smallest file that decodes *eventually* is what you want.
    bool for_playback   = false;

    // Skip streams above this bitrate (bits/sec). 0 = no limit. Useful on a
    // link that cannot sustain a 13 Mbit 2160p feed regardless of decode.
    int64_t max_bitrate = 0;

    // Playback-oriented defaults: capability-aware ranking, 1080p ceiling.
    static Quality playback(int height = 1080) {
        Quality q;
        q.for_playback = true;
        q.height = height;
        return q;
    }

    // Soft preferences: honoured when something matches, ignored otherwise,
    // so a bad hint degrades to the default pick instead of failing.
    std::string_view vcodec;      // "av01" | "vp9" | "avc1" | "h264"
    std::string_view acodec;      // "opus" | "mp4a" | "aac"
    std::string_view container;   // "mp4" | "webm"

    static Quality best()            { return {}; }
    static Quality at(int h)         { Quality q; q.height = h; return q; }
    static Quality upto(int h)       { Quality q; q.max_height = h; return q; }
    // Small and cheap: lowest resolution, lowest bitrate stereo audio.
    static Quality lowest() {
        Quality q; q.smallest = true; q.allow_surround = false; return q;
    }
};

// ---------------------------------------------------------------------------
// Result of a selection.
//
// Pointers alias into the VideoInfo::formats vector they came from, so the
// VideoInfo must outlive the Selection. Nothing here owns anything.
// ---------------------------------------------------------------------------
struct Selection {
    const StreamFormat* video = nullptr;
    const StreamFormat* audio = nullptr;
    Mode mode = Mode::AudioVideo;

    // True when `video` is a muxed stream carrying its own audio; in that case
    // `audio` is null and a player must not be given a separate audio input.
    bool muxed = false;

    bool ok() const {
        switch (mode) {
            case Mode::AudioOnly: return audio || (video && muxed);
            case Mode::VideoOnly: return video != nullptr;
            default:              return video && (audio || muxed);
        }
    }

    int height() const { return video ? video->height : 0; }
    int fps()    const { return video ? video->fps : 0; }

    int64_t total_bytes() const {
        int64_t n = 0;
        if (video) n += video->content_length;
        if (audio && !muxed) n += audio->content_length;
        return n;
    }

    int64_t total_bitrate() const {
        int64_t n = 0;
        if (video) n += video->effective_bitrate();
        if (audio && !muxed) n += audio->effective_bitrate();
        return n;
    }

    // "mp4a.40.2" -> "mp4a", "av01.0.09M.08" -> "av01". A codec profile is
    // noise in a one-line summary.
    static std::string short_codec(std::string_view c) {
        size_t dot = c.find('.');
        return std::string(dot == std::string_view::npos ? c : c.substr(0, dot));
    }

    // One-line human summary, e.g. "1080p60 av01 + 160kbps opus".
    std::string describe() const {
        std::string s;
        if (mode != Mode::AudioOnly && video) {
            s += std::to_string(video->height) + "p";
            if (video->fps > 30) s += std::to_string(video->fps);
            if (!video->video_codec.empty()) {
                s += " ";
                s += short_codec(video->video_codec);
            }
            if (muxed) { s += " (muxed)"; return s; }
        }
        const StreamFormat* a = audio ? audio : (muxed ? video : nullptr);
        if (a && mode != Mode::VideoOnly) {
            if (!s.empty()) s += " + ";
            s += std::to_string(a->effective_bitrate() / 1000) + "kbps";
            if (!a->audio_codec.empty()) {
                s += " ";
                s += short_codec(a->audio_codec);
            }
            if (a->audio_channels > 2) s += " " + std::to_string(a->audio_channels) + "ch";
        }
        return s.empty() ? "nothing" : s;
    }
};

// ---------------------------------------------------------------------------
// One rung of the resolution ladder — what a UI needs to draw a quality menu.
// ---------------------------------------------------------------------------
struct Rung {
    int     height  = 0;
    int     fps     = 0;
    int     itag    = 0;
    bool    hdr     = false;
    int64_t bitrate = 0;
    int64_t bytes   = 0;         // 0 when the response omitted contentLength
    std::string_view codec;
    std::string_view container;

    std::string label() const {
        std::string s = std::to_string(height) + "p";
        if (fps > 30) s += std::to_string(fps);
        if (hdr) s += " HDR";
        return s;
    }
};

// ---------------------------------------------------------------------------
// Selector
// ---------------------------------------------------------------------------
class Selector {
public:
    // -----------------------------------------------------------------------
    // Main entry point. Always returns the best thing it can find for the
    // requested mode; only an empty or wholly unusable format list yields a
    // selection where ok() is false.
    // -----------------------------------------------------------------------
    static Selection select(const std::vector<StreamFormat>& fmts,
                            Mode mode,
                            const Quality& q = {}) {
        Selection sel;
        sel.mode = mode;
        if (fmts.empty()) return sel;

        if (mode == Mode::AudioOnly) {
            sel.audio = best_audio(fmts, q);
            if (!sel.audio && q.allow_muxed) {
                // No adaptive audio: a muxed stream still carries a track.
                sel.video = best_muxed(fmts, q);
                sel.muxed = sel.video != nullptr;
            }
            return sel;
        }

        sel.video = best_video(fmts, q);

        if (mode == Mode::VideoOnly) {
            if (!sel.video && q.allow_muxed) {
                sel.video = best_muxed(fmts, q);
                sel.muxed = sel.video != nullptr;
            }
            return sel;
        }

        // AudioVideo: independent tracks are strongly preferred, because that
        // is what makes resolution switching cheap and what unlocks quality
        // above the muxed ceiling (muxed tops out at 360p on most uploads).
        sel.audio = best_audio(fmts, q);
        if (sel.video && sel.audio) return sel;

        if (q.allow_muxed) {
            if (const StreamFormat* m = best_muxed(fmts, q)) {
                sel.video = m;
                sel.audio = nullptr;
                sel.muxed = true;
            }
        }
        return sel;
    }

    // -----------------------------------------------------------------------
    // Graceful resolution switch.
    //
    // Returns a new Selection at (or nearest to) `height`, carrying the audio
    // track from `current` unchanged. A player can therefore swap only the
    // video input and leave the audio stream running. If the video track can't
    // be changed the current selection comes back untouched, so a caller can
    // compare pointers to decide whether a reload is even needed.
    // -----------------------------------------------------------------------
    static Selection switch_video(const std::vector<StreamFormat>& fmts,
                                  const Selection& current,
                                  int height,
                                  const Quality& base = {}) {
        if (current.muxed || current.mode == Mode::AudioOnly) {
            Quality q = base;
            q.height = height;
            return select(fmts, current.mode, q);
        }
        Quality q = base;
        q.height = height;
        Selection next = current;
        if (const StreamFormat* v = best_video(fmts, q)) next.video = v;
        return next;
    }

    // -----------------------------------------------------------------------
    // Next thing to try after playback failed.
    //
    // A player that dies seconds in has told us the current pick is unusable on
    // this machine, and nothing in the format list explains why -- it could be
    // a missing decoder, a driver that lies about hardware support, a link that
    // cannot sustain the bitrate, or a container quirk. Rather than guess,
    // step conservatively: same picture, cheaper codec first (that is the most
    // common cause and costs no quality), then drop a rung, and finally accept
    // the muxed stream, which is the most compatible thing YouTube serves.
    //
    // Returns a selection with ok() == false once there is nothing left, so a
    // retry loop terminates.
    // -----------------------------------------------------------------------
    static Selection degrade(const std::vector<StreamFormat>& fmts,
                             const Selection& current,
                             const Quality& base = {}) {
        Selection none;
        none.mode = current.mode;
        if (!current.ok() || fmts.empty()) return none;

        // The muxed stream is the floor: it is the most compatible thing
        // YouTube serves, and there is nothing more conservative to fall back
        // to. Stepping "down" from it would produce a video-only stream still
        // flagged muxed, and a caller would hand a player a silent file.
        if (current.muxed) return none;

        // Audio-only has no picture to trade away; try the next codec down.
        if (current.mode == Mode::AudioOnly || !current.video) {
            const StreamFormat* next = nullptr;
            for (const auto& f : fmts) {
                if (!f.is_audio_only()) continue;
                if (current.audio && f.itag == current.audio->itag) continue;
                const int64_t cur = current.audio ? current.audio->effective_bitrate() : 0;
                if (f.effective_bitrate() >= cur) continue;
                if (!next || f.effective_bitrate() > next->effective_bitrate()) next = &f;
            }
            if (!next) return none;
            Selection s = current;
            s.audio = next;
            return s;
        }

        const int h = current.video->height;

        // 1. Same height, a codec that ranks better for playback here.
        {
            const Caps& c = caps();
            const int cur_rank = playback_rank(current.video->video_codec, h, c);
            const StreamFormat* best = nullptr;
            int best_rank = cur_rank;
            for (const auto& f : fmts) {
                if (!f.is_video_only() || f.height != h) continue;
                if (f.itag == current.video->itag) continue;
                const int r = playback_rank(f.video_codec, f.height, c);
                if (r > best_rank) { best_rank = r; best = &f; }
            }
            if (best) {
                Selection s = current;
                s.video = best;
                return s;
            }
        }

        // 2. Drop one rung, taking the most playable codec at that height.
        {
            std::vector<Rung> l = ladder(fmts, false);
            for (size_t i = 0; i < l.size(); ++i) {
                if (l[i].height > h || (l[i].height == h && l[i].fps >= current.video->fps))
                    continue;
                Quality q = base;
                q.for_playback = true;
                q.height = l[i].height;
                q.vcodec = {};
                if (const StreamFormat* v = best_video(fmts, q)) {
                    if (v->itag == current.video->itag) continue;
                    Selection s = current;
                    s.video = v;
                    return s;
                }
            }
        }

        // 3. The muxed stream: lowest quality, highest compatibility.
        if (!current.muxed) {
            Quality q = base;
            if (const StreamFormat* m = best_muxed(fmts, q)) {
                Selection s;
                s.mode  = current.mode;
                s.video = m;
                s.audio = nullptr;
                s.muxed = true;
                return s;
            }
        }
        return none;
    }

    // Step one rung up or down the ladder from the current height. Returns the
    // same height when already at an end, so repeated calls settle rather than
    // wrap around.
    static int step_height(const std::vector<StreamFormat>& fmts,
                           int current, int direction) {
        std::vector<Rung> l = ladder(fmts);
        if (l.empty()) return current;
        // ladder() is sorted high to low.
        for (size_t i = 0; i < l.size(); ++i) {
            if (l[i].height != current) continue;
            if (direction > 0) return l[i > 0 ? i - 1 : 0].height;
            return l[i + 1 < l.size() ? i + 1 : i].height;
        }
        // Not on a rung: land on the nearest one.
        return nearest_height(fmts, current);
    }

    static int nearest_height(const std::vector<StreamFormat>& fmts, int target) {
        int best = 0, best_d = INT32_MAX;
        for (const auto& f : fmts) {
            if (!f.has_video || f.height <= 0) continue;
            int d = f.height > target ? f.height - target : target - f.height;
            if (d < best_d) { best_d = d; best = f.height; }
        }
        return best;
    }

    // -----------------------------------------------------------------------
    // The resolution ladder, best first. One entry per distinct height+fps+HDR
    // combination, keeping the best-compressing codec for each -- a UI wants
    // "1080p60" once, not the AV1, VP9 and H.264 variants of it.
    // -----------------------------------------------------------------------
    static std::vector<Rung> ladder(const std::vector<StreamFormat>& fmts,
                                    bool include_muxed = true) {
        std::vector<Rung> out;
        out.reserve(12);
        for (const auto& f : fmts) {
            if (!f.has_video || f.height <= 0) continue;
            if (!include_muxed && f.is_muxed()) continue;
            bool hdr = is_hdr(f);
            // A quality menu wants "144p" once, not a 15fps and a 30fps entry
            // that render identically. High frame rate stays its own rung
            // because users do choose between 720p and 720p60.
            const int fps_key = f.fps > 30 ? f.fps : 30;
            Rung* hit = nullptr;
            for (auto& r : out) {
                const int r_key = r.fps > 30 ? r.fps : 30;
                if (r.height == f.height && r_key == fps_key && r.hdr == hdr) { hit = &r; break; }
            }
            if (!hit) {
                out.push_back({f.height, f.fps, f.itag, hdr, f.effective_bitrate(),
                               f.content_length, f.video_codec, f.container});
                continue;
            }
            // Same rung: keep the better codec, then the higher bitrate.
            int a = vcodec_rank(f.video_codec), b = vcodec_rank(hit->codec);
            if (a > b || (a == b && f.effective_bitrate() > hit->bitrate)) {
                hit->itag = f.itag;
                hit->fps = f.fps;
                hit->bitrate = f.effective_bitrate();
                hit->bytes = f.content_length;
                hit->codec = f.video_codec;
                hit->container = f.container;
            }
        }
        std::sort(out.begin(), out.end(), [](const Rung& a, const Rung& b) {
            if (a.height != b.height) return a.height > b.height;
            if (a.fps != b.fps) return a.fps > b.fps;
            return a.hdr && !b.hdr;
        });
        return out;
    }

    // Distinct audio tracks, best first — for an audio quality menu.
    static std::vector<const StreamFormat*> audio_ladder(
            const std::vector<StreamFormat>& fmts) {
        std::vector<const StreamFormat*> out;
        for (const auto& f : fmts) if (f.is_audio_only()) out.push_back(&f);
        std::sort(out.begin(), out.end(),
                  [](const StreamFormat* a, const StreamFormat* b) {
                      if (a->audio_channels != b->audio_channels)
                          return a->audio_channels > b->audio_channels;
                      return a->effective_bitrate() > b->effective_bitrate();
                  });
        return out;
    }

    static bool is_hdr(const StreamFormat& f) {
        // vp9.2 / av01 profile 2 carry the 10-bit HDR variants.
        if (f.video_codec.rfind("vp9.2", 0) == 0) return true;
        if (f.video_codec.rfind("av01", 0) == 0 &&
            f.video_codec.find(".10.") != std::string_view::npos) return true;
        if (const ItagInfo* i = itag_lookup(f.itag)) return (i->flags & IT_HDR) != 0;
        return false;
    }

private:
    // -----------------------------------------------------------------------
    // Video. Constraints are applied in tiers so that an over-specified
    // request degrades instead of returning nothing: first everything, then
    // without the codec hint, then without the container hint, then with only
    // the height ceiling, then anything at all.
    // -----------------------------------------------------------------------
    static const StreamFormat* best_video(const std::vector<StreamFormat>& fmts,
                                          const Quality& q) {
        // Constraints relax one tier at a time so an over-specified request
        // degrades rather than returning nothing.
        for (int tier = 0; tier < 4; ++tier) {
            auto admissible = [&](const StreamFormat& f) {
                if (!f.is_video_only()) return false;
                if (q.max_height > 0 && f.height > q.max_height) return false;
                // A codec with no decoder on this machine is never a candidate,
                // at any tier: a stream that cannot be decoded is not a
                // fallback, it is a failure.
                if (q.for_playback && playback_rank(f.video_codec, f.height, caps()) < 0)
                    return false;
                if (q.max_bitrate > 0 && f.effective_bitrate() > q.max_bitrate) return false;
                if (tier < 1 && !q.vcodec.empty() && !codec_hit(f.video_codec, q.vcodec)) return false;
                if (tier < 2 && !q.container.empty() && f.container != q.container) return false;
                if (tier < 3 && q.prefer_hdr && !is_hdr(f)) return false;
                return true;
            };

            // Pass 1: at or below the target — the normal case.
            const StreamFormat* best = nullptr;
            for (const auto& f : fmts) {
                if (!admissible(f)) continue;
                if (q.height > 0 && f.height > q.height) continue;
                if (!best || better_video(f, *best, q)) best = &f;
            }
            if (best) return best;

            // Pass 2: nothing at or below exists, so take the SMALLEST stream
            // above the target. Asking for 240p on an upload that only has
            // 1080p and 2160p should hand back 1080p; picking "the best
            // available" there would be the worst possible answer to the
            // question that was asked.
            if (q.height > 0) {
                for (const auto& f : fmts) {
                    if (!admissible(f)) continue;
                    if (!best || f.height < best->height ||
                        (f.height == best->height && better_video(f, *best, q)))
                        best = &f;
                }
                if (best) return best;
            }
        }
        return nullptr;
    }

    static const StreamFormat* best_audio(const std::vector<StreamFormat>& fmts,
                                          const Quality& q) {
        for (int tier = 0; tier < 3; ++tier) {
            const StreamFormat* best = nullptr;
            for (const auto& f : fmts) {
                if (!f.is_audio_only()) continue;
                if (!q.allow_surround && f.audio_channels > 2) continue;
                if (tier < 1 && !q.acodec.empty() && !codec_hit(f.audio_codec, q.acodec)) continue;
                if (tier < 2 && !q.container.empty() && f.container != q.container) continue;
                if (!best || better_audio(f, *best, q.smallest)) best = &f;
            }
            if (best) return best;
        }
        return nullptr;
    }

    static const StreamFormat* best_muxed(const std::vector<StreamFormat>& fmts,
                                          const Quality& q) {
        const StreamFormat* best = nullptr;
        for (const auto& f : fmts) {
            if (!f.is_muxed()) continue;
            if (q.max_height > 0 && f.height > q.max_height) continue;
            if (q.height > 0 && f.height > q.height && best) continue;
            if (!best || better_video(f, *best, q)) best = &f;
        }
        return best;
    }

    static bool codec_hit(std::string_view have, std::string_view want) {
        if (have.empty()) return false;
        if (have.rfind(want, 0) == 0) return true;
        if (want == "h264" || want == "avc") return have.rfind("avc1", 0) == 0;
        if (want == "av1")                   return have.rfind("av01", 0) == 0;
        if (want == "aac" || want == "m4a")  return have.rfind("mp4a", 0) == 0;
        if (want == "vp09")                  return have.rfind("vp9", 0) == 0;
        return have.find(want) != std::string_view::npos;
    }

    static bool better_video(const StreamFormat& a, const StreamFormat& b,
                             const Quality& q) {
        if (q.smallest) {
            if (a.height != b.height) return a.height < b.height;
            if (a.fps != b.fps)       return a.fps < b.fps;
            return a.effective_bitrate() < b.effective_bitrate();
        }
        if (a.height != b.height) return a.height > b.height;
        // fps only breaks ties once a preference is expressed; otherwise a
        // 60fps stream at double the bitrate would always beat 30fps.
        if (q.fps > 0 && a.fps != b.fps) {
            bool ah = a.fps >= q.fps, bh = b.fps >= q.fps;
            if (ah != bh) return ah;
        } else if (a.fps != b.fps) {
            return a.fps > b.fps;
        }
        if (q.prefer_hdr) {
            bool ah = is_hdr(a), bh = is_hdr(b);
            if (ah != bh) return ah;
        }
        if (q.for_playback) {
            const Caps& c = caps();
            int ap = playback_rank(a.video_codec, a.height, c);
            int bp = playback_rank(b.video_codec, b.height, c);
            if (ap != bp) return ap > bp;
            // Same playability: take the cheaper stream, not the fatter one.
            return a.effective_bitrate() < b.effective_bitrate();
        }
        int ar = vcodec_rank(a.video_codec), br = vcodec_rank(b.video_codec);
        if (ar != br) return ar > br;
        return a.effective_bitrate() > b.effective_bitrate();
    }

    static bool better_audio(const StreamFormat& a, const StreamFormat& b,
                             bool smallest = false) {
        // Never prefer a loudness-compressed duplicate, in either direction.
        if (a.is_drc != b.is_drc) return !a.is_drc;
        if (smallest) {
            if (a.audio_channels != b.audio_channels)
                return a.audio_channels < b.audio_channels;
            return a.effective_bitrate() < b.effective_bitrate();
        }
        if (a.audio_channels != b.audio_channels)
            return a.audio_channels > b.audio_channels;
        int64_t ab = a.effective_bitrate(), bb = b.effective_bitrate();
        if (ab != bb) return ab > bb;
        return acodec_rank(a.audio_codec) > acodec_rank(b.audio_codec);
    }
};

} // namespace ytfast
