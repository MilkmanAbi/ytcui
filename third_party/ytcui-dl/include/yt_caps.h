#pragma once
/*
 * ytcui-dl — yt_caps.h
 *
 * What can this machine actually decode?
 *
 * Picking the most efficient codec is the right call for downloading and the
 * wrong one for playback. AV1 compresses best, so a compression-ranked
 * selector reaches for it every time -- but AV1 hardware decode only exists on
 * very recent GPUs, and software AV1 at 2160p will stall or kill a player on
 * most machines. A stream that cannot be decoded is worse than a larger one
 * that can.
 *
 * So we ask mpv what decoders it has (`--vd=help`) and which codecs it can
 * hand to hardware (`--hwdec=help`), then let the selector avoid codecs with
 * no decoder at all and deprioritise software-only codecs at high resolution.
 *
 * The probe spawns mpv, so the result is cached on disk: it only changes when
 * the user's mpv or drivers change, and a week-old answer is still right.
 * Everything degrades -- if mpv is absent or the probe fails we assume the
 * common baseline (H.264 and VP9 decodable, AV1 unknown) rather than refusing
 * to play anything.
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "yt_cache.h"

namespace ytfast {

struct Caps {
    // A decoder exists at all (software counts).
    bool dec_h264 = true;
    bool dec_hevc = false;
    bool dec_vp9  = true;
    bool dec_av1  = false;

    // The codec can be handed to hardware. Matters above 1080p, where software
    // decode of the modern codecs stops being comfortable.
    bool hw_h264 = false;
    bool hw_hevc = false;
    bool hw_vp9  = false;
    bool hw_av1  = false;

    bool probed = false;   // false means these are assumed defaults

    bool can_decode(std::string_view codec) const {
        if (codec.rfind("avc1", 0) == 0 || codec.rfind("h264", 0) == 0) return dec_h264;
        if (codec.rfind("hev1", 0) == 0 || codec.rfind("hvc1", 0) == 0) return dec_hevc;
        if (codec.rfind("vp9",  0) == 0 || codec.rfind("vp09", 0) == 0) return dec_vp9;
        if (codec.rfind("av01", 0) == 0) return dec_av1;
        return true;   // unknown codec: do not second-guess the player
    }

    bool hw_decode(std::string_view codec) const {
        if (codec.rfind("avc1", 0) == 0 || codec.rfind("h264", 0) == 0) return hw_h264;
        if (codec.rfind("hev1", 0) == 0 || codec.rfind("hvc1", 0) == 0) return hw_hevc;
        if (codec.rfind("vp9",  0) == 0 || codec.rfind("vp09", 0) == 0) return hw_vp9;
        if (codec.rfind("av01", 0) == 0) return hw_av1;
        return false;
    }

    std::string describe() const {
        std::string s;
        auto add = [&](const char* n, bool d, bool hw) {
            if (!d) return;
            if (!s.empty()) s += " ";
            s += n;
            if (hw) s += "(hw)";
        };
        add("h264", dec_h264, hw_h264);
        add("hevc", dec_hevc, hw_hevc);
        add("vp9",  dec_vp9,  hw_vp9);
        add("av1",  dec_av1,  hw_av1);
        if (!probed) s += " [assumed]";
        return s.empty() ? "none" : s;
    }

    // Serialised as a short flag string so it can live in the disk cache.
    std::string serialize() const {
        std::string s = "1";
        const bool bits[] = {dec_h264, dec_hevc, dec_vp9, dec_av1,
                             hw_h264, hw_hevc, hw_vp9, hw_av1};
        for (bool b : bits) s += b ? '1' : '0';
        return s;
    }
    static bool deserialize(std::string_view s, Caps& out) {
        if (s.size() < 9 || s[0] != '1') return false;
        const bool* end = nullptr;
        bool* bits[] = {&out.dec_h264, &out.dec_hevc, &out.dec_vp9, &out.dec_av1,
                        &out.hw_h264, &out.hw_hevc, &out.hw_vp9, &out.hw_av1};
        (void)end;
        for (int i = 0; i < 8; ++i) *bits[i] = (s[i + 1] == '1');
        out.probed = true;
        return true;
    }
};

namespace detail {

inline std::string run_capture(const char* cmd) {
    std::string out;
    FILE* p = ::popen(cmd, "r");
    if (!p) return out;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, p)) {
        out += buf;
        if (out.size() > (1u << 20)) break;   // runaway output
    }
    ::pclose(p);
    return out;
}

inline bool mentions(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

inline Caps probe_caps_uncached() {
    Caps c;

    // `mpv --vd=help` lists every video decoder the linked ffmpeg exposes.
    const std::string vd = run_capture("mpv --vd=help 2>/dev/null");
    if (vd.empty()) return c;      // no mpv: keep the assumed defaults

    c.probed   = true;
    c.dec_h264 = mentions(vd, "h264");
    c.dec_hevc = mentions(vd, "hevc");
    c.dec_vp9  = mentions(vd, "vp9");
    // Any AV1 decoder counts: libdav1d, libaom-av1, or the builtin.
    c.dec_av1  = mentions(vd, "av1");

    // `mpv --hwdec=help` names the codecs each method can offload. The entries
    // read like "nvdec (h264-nvdec)", so matching "<codec>-" is enough and
    // stays correct across nvdec / vaapi / videotoolbox / qsv / drm.
    const std::string hw = run_capture("mpv --hwdec=help 2>/dev/null");
    if (!hw.empty()) {
        c.hw_h264 = mentions(hw, "h264-");
        c.hw_hevc = mentions(hw, "hevc-");
        c.hw_vp9  = mentions(hw, "vp9-");
        c.hw_av1  = mentions(hw, "av1-");
    }
    return c;
}

} // namespace detail

// Cached for a week. Only changes when mpv or the GPU drivers change, and the
// probe costs two process spawns.
inline const Caps& caps() {
    static Caps c = [] {
        Caps v;
        std::string cached;
        if (DiskCache::get("decoder_caps", cached, 7 * 24 * 3600) &&
            Caps::deserialize(cached, v))
            return v;
        v = detail::probe_caps_uncached();
        if (v.probed) DiskCache::put("decoder_caps", v.serialize());
        return v;
    }();
    return c;
}

// Force a re-probe (after the user installs a codec, or for --diag).
inline Caps refresh_caps() {
    DiskCache::drop("decoder_caps");
    Caps v = detail::probe_caps_uncached();
    if (v.probed) DiskCache::put("decoder_caps", v.serialize());
    return v;
}

// ---------------------------------------------------------------------------
// How suitable is this codec for *playing* a stream of this height?
//
// Higher is better; a negative result means "do not use". Compression
// efficiency is only the tiebreaker -- decodability comes first, then whether
// the work can go to hardware once the resolution gets demanding.
// ---------------------------------------------------------------------------
inline int playback_rank(std::string_view codec, int height, const Caps& c) {
    if (!c.can_decode(codec)) return -1;

    int r = 0;
    const bool hw = c.hw_decode(codec);
    if (hw) r += 20;

    // Software decode of the modern codecs is fine at 1080p and painful above
    // it. 1440p and 2160p AV1 or VP9 on a CPU is where players stutter, drop
    // frames, or give up entirely.
    if (!hw) {
        if (height > 1440)      r -= 12;
        else if (height > 1080) r -= 6;
    }

    // Tiebreak on decode cost, cheapest first: H.264 is the least work per
    // frame, AV1 the most.
    if (codec.rfind("avc1", 0) == 0) r += 3;
    else if (codec.rfind("vp9", 0) == 0 || codec.rfind("vp09", 0) == 0) r += 2;
    else if (codec.rfind("av01", 0) == 0) r += 1;

    return r;
}

} // namespace ytfast
