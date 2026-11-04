#pragma once
/*
 * ytcui-dl — ytdlp_compat.h
 *
 * yt-dlp compatible command line.
 *
 * Enabled when argv[0] is (or ends with) "yt-dlp" or "youtube-dl", or when the
 * first argument is --yt-dlp. Symlink it and existing scripts work unchanged:
 *
 *     ln -s ytcui-dl yt-dlp
 *
 * A separate mode rather than one merged flag set, because the two interfaces
 * genuinely collide: -q is --quality here and --quiet there, -a is --audio
 * here and --batch-file there, -c is --connections here and --continue there,
 * -s is --search here and --simulate there. Merging them would mean silently
 * doing the wrong thing for one set of users, so the multi-call approach keeps
 * both honest.
 *
 * Not implemented, and deliberately: playlists and channels (this resolves
 * single videos), post-processing beyond audio extraction and muxing,
 * subtitles, SponsorBlock, cookies, archives, authentication.
 */

#include "ytfast.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ytdlp {

using namespace ytfast;

// ---------------------------------------------------------------------------
// Output templates: %(field)s, %(field)d, and the padded numeric forms.
//
// yt-dlp's real template language is much larger; this covers the fields that
// appear in practically every command line anyone has written.
// ---------------------------------------------------------------------------
inline std::string sanitize_component(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        const unsigned char u = (unsigned char)c;
        if (u < 0x20) continue;
        out += std::strchr("/\\:*?\"<>|", c) ? '_' : c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out;
}

inline std::string field_value(const std::string& name, const VideoInfo& info,
                               const StreamFormat* f, const std::string& ext) {
    if (name == "title" || name == "fulltitle") return sanitize_component(info.title);
    if (name == "id")            return info.id;
    if (name == "ext")           return ext;
    if (name == "uploader" || name == "channel" || name == "creator")
        return sanitize_component(info.channel);
    if (name == "uploader_id" || name == "channel_id") return info.channel_id;
    if (name == "upload_date")   return info.upload_date;
    if (name == "duration")      return std::to_string(info.duration_secs);
    if (name == "duration_string") return info.duration_str;
    if (name == "view_count")    return std::to_string(info.view_count);
    if (name == "webpage_url" || name == "original_url" || name == "url") return info.url;
    if (name == "thumbnail")     return info.thumbnail_url;
    if (name == "description")   return sanitize_component(info.description);
    if (name == "extractor")     return "youtube";
    if (name == "extractor_key") return "Youtube";
    if (name == "playlist_index" || name == "autonumber") return "1";
    if (name == "playlist" || name == "playlist_title") return "NA";
    if (name == "epoch")         return std::to_string((long long)::time(nullptr));
    if (!f) return "NA";
    if (name == "format_id")     return std::to_string(f->itag);
    if (name == "width")         return std::to_string(f->width);
    if (name == "height")        return std::to_string(f->height);
    if (name == "fps")           return std::to_string(f->fps);
    if (name == "vcodec")        return f->video_codec.empty() ? "none" : std::string(f->video_codec);
    if (name == "acodec")        return f->audio_codec.empty() ? "none" : std::string(f->audio_codec);
    if (name == "tbr")           return std::to_string(f->effective_bitrate() / 1000);
    if (name == "abr")           return std::to_string(f->is_audio_only() ? f->effective_bitrate() / 1000 : 0);
    if (name == "filesize" || name == "filesize_approx") return std::to_string(f->content_length);
    if (name == "resolution") {
        if (!f->has_video) return "audio only";
        return std::to_string(f->width) + "x" + std::to_string(f->height);
    }
    if (name == "format")
        return std::to_string(f->itag) + " - " +
               (f->has_video ? std::to_string(f->width) + "x" + std::to_string(f->height)
                             : std::string("audio only"));
    return "NA";
}

inline std::string expand_template(const std::string& tmpl, const VideoInfo& info,
                                   const StreamFormat* f, const std::string& ext) {
    std::string out;
    out.reserve(tmpl.size() + 64);
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] != '%' || i + 1 >= tmpl.size() || tmpl[i + 1] != '(') {
            out += tmpl[i];
            continue;
        }
        const size_t close = tmpl.find(')', i + 2);
        if (close == std::string::npos) { out += tmpl[i]; continue; }
        std::string name = tmpl.substr(i + 2, close - i - 2);

        // Strip yt-dlp's field modifiers; the base name is what we resolve.
        for (const char* sep : {">", "+", "-", "|", ":"}) {
            const size_t p = name.find(sep);
            if (p != std::string::npos) name = name.substr(0, p);
        }

        // A conversion char, possibly preceded by a width spec (%(x)05d).
        size_t j = close + 1;
        while (j < tmpl.size() && (std::isdigit((unsigned char)tmpl[j]) ||
                                   tmpl[j] == '0' || tmpl[j] == '-' || tmpl[j] == '.')) ++j;
        const std::string width = tmpl.substr(close + 1, j - close - 1);
        const char conv = j < tmpl.size() ? tmpl[j] : 's';

        std::string v = field_value(name, info, f, ext);
        if (!width.empty() && (conv == 'd' || conv == 'i')) {
            char buf[64];
            std::string spec = "%" + width + "lld";
            std::snprintf(buf, sizeof buf, spec.c_str(), atoll(v.c_str()));
            v = buf;
        }
        out += v;
        i = j;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct Opts {
    std::string format = "bestvideo*+bestaudio/best";
    std::string output = "%(title)s [%(id)s].%(ext)s";
    std::string paths;
    std::string audio_format = "best";
    std::string merge_format;
    std::string print_tmpl;
    std::string user_agent;

    bool extract_audio = false;
    bool list_formats  = false;
    bool dump_json     = false;
    bool get_url       = false;
    bool get_title     = false;
    bool get_id        = false;
    bool get_duration  = false;
    bool get_thumbnail = false;
    bool get_filename  = false;
    bool simulate      = false;
    bool skip_download = false;
    bool quiet         = false;
    bool verbose       = false;
    bool no_overwrites = false;
    bool resume        = true;
    bool no_mtime      = false;

    int     concurrent  = 3;
    int64_t limit_rate  = 0;

    std::vector<std::string> urls;
};

inline void usage() {
    std::printf(
"Usage: yt-dlp [OPTIONS] URL [URL...]\n"
"\n"
"ytcui-dl running in yt-dlp compatible mode. Single videos only: playlists,\n"
"channels, subtitles, cookies and post-processing beyond audio extraction are\n"
"not implemented.\n"
"\n"
"General:\n"
"    -h, --help                       print this help\n"
"        --version                    print version\n"
"    -q, --quiet                      quiet mode\n"
"    -v, --verbose                    verbose output\n"
"    -s, --simulate                   do not download\n"
"        --skip-download              same as --simulate\n"
"    -4, --force-ipv4                 use IPv4 only\n"
"    -6, --force-ipv6                 use IPv6 only\n"
"        --user-agent UA              override the User-Agent\n"
"\n"
"Video selection:\n"
"        --no-playlist                accepted; only single videos are handled\n"
"\n"
"Download:\n"
"    -N, --concurrent-fragments N     parallel connections (default 3)\n"
"    -r, --limit-rate RATE            e.g. 4.2M (accepted; not enforced yet)\n"
"    -c, --continue                   resume partial downloads (default)\n"
"        --no-continue                restart from the beginning\n"
"    -w, --no-overwrites              skip files that already exist\n"
"\n"
"Filesystem:\n"
"    -o, --output TEMPLATE            output template\n"
"    -P, --paths PATH                 output directory\n"
"\n"
"Verbosity / simulation:\n"
"    -g, --get-url                    print the stream URL\n"
"    -e, --get-title                  print the title\n"
"        --get-id                     print the id\n"
"        --get-duration               print the duration\n"
"        --get-thumbnail              print the thumbnail URL\n"
"        --get-filename               print the output filename\n"
"    -j, --dump-json                  print metadata as JSON\n"
"    -J, --dump-single-json           same as --dump-json\n"
"    -F, --list-formats               list available formats\n"
"        --print TEMPLATE             print a template and exit\n"
"\n"
"Format selection:\n"
"    -f, --format FORMAT              e.g. bestvideo[height<=1080]+bestaudio\n"
"    -S, --format-sort SORTORDER      accepted; res/codec hints are honoured\n"
"        --merge-output-format FMT    container for merged output\n"
"\n"
"Post-processing:\n"
"    -x, --extract-audio              keep only the audio track\n"
"        --audio-format FORMAT        best, mp3, m4a, opus, ... (needs ffmpeg)\n"
"        --audio-quality Q            accepted\n"
"\n"
"Anything not listed is accepted and ignored so existing scripts still run.\n");
}

// "4.2M" / "500K" / "1000000" -> bytes per second
inline int64_t parse_rate(const std::string& s) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return 0;
    switch (*end) {
        case 'k': case 'K': return (int64_t)(v * 1024);
        case 'm': case 'M': return (int64_t)(v * 1024 * 1024);
        case 'g': case 'G': return (int64_t)(v * 1024 * 1024 * 1024);
        default:            return (int64_t)v;
    }
}

// ---------------------------------------------------------------------------
// Format string -> Selection.
//
// Handles the shapes people actually write: best / worst, bestvideo+bestaudio,
// bare itags, "/" fallback chains, and [height<=N][ext=mp4][fps=60] filters.
// ---------------------------------------------------------------------------
struct Picked {
    const StreamFormat* video = nullptr;
    const StreamFormat* audio = nullptr;
    bool muxed = false;
    bool ok() const { return video || audio; }
};

inline bool parse_filters(const std::string& spec, Quality& q, int& want_itag) {
    want_itag = 0;
    std::string base = spec;
    const size_t br = spec.find('[');
    if (br != std::string::npos) base = spec.substr(0, br);

    if (!base.empty() && base.find_first_not_of("0123456789") == std::string::npos) {
        want_itag = std::atoi(base.c_str());
        return true;
    }
    static std::string vbuf, abuf, cbuf;
    size_t pos = br;
    while (pos != std::string::npos && pos < spec.size()) {
        const size_t open = spec.find('[', pos);
        if (open == std::string::npos) break;
        const size_t close = spec.find(']', open);
        if (close == std::string::npos) break;
        const std::string e = spec.substr(open + 1, close - open - 1);
        pos = close + 1;

        // Split "key<op>value" once, on the first operator character, so
        // "height<=720" yields key="height", op="<=", val="720". The previous
        // version searched for "<" and then kept the "=" in the value, so
        // every "height<=N" filter parsed as zero and was silently ignored.
        size_t k = e.find_first_of("<>=^*!~");
        if (k == std::string::npos || k == 0) continue;
        const std::string key = e.substr(0, k);
        size_t vstart = k;
        while (vstart < e.size() && std::strchr("<>=^*!~", e[vstart])) ++vstart;
        const std::string op  = e.substr(k, vstart - k);
        const std::string val = e.substr(vstart);
        const int n = std::atoi(val.c_str());

        if (key == "height") {
            if (op == "<=" )     q.max_height = n;
            else if (op == "<")  q.max_height = n > 0 ? n - 1 : 0;
            else if (op == "=")  { q.height = n; q.max_height = n; }
            else if (op == ">=" || op == ">") q.height = 0;   // no floor concept
        } else if (key == "width") {
            // Approximate a width cap as a height cap on 16:9.
            if (op == "<=" || op == "<") q.max_height = n * 9 / 16;
        } else if (key == "fps") {
            if (op == ">=" || op == ">" || op == "=") q.fps = n;
        } else if (key == "ext" || key == "container") {
            cbuf = val; q.container = cbuf;
        } else if (key == "vcodec") {
            vbuf = val; q.vcodec = vbuf;
        } else if (key == "acodec") {
            abuf = val; q.acodec = abuf;
        } else if (key == "tbr" || key == "vbr" || key == "abr") {
            if (op == "<=" || op == "<") q.max_bitrate = (int64_t)n * 1000;
        } else if (key == "filesize") {
            // No direct filter; approximate nothing rather than guess wrong.
        }
    }
    return true;
}

inline const StreamFormat* by_itag(const std::vector<StreamFormat>& fmts, int itag) {
    for (const auto& f : fmts) if (f.itag == itag) return &f;
    return nullptr;
}

// One alternative from a "/" chain, e.g. "bestvideo[height<=720]+bestaudio".
inline Picked pick_one(const std::vector<StreamFormat>& fmts, const std::string& spec) {
    Picked p;

    // Split on '+' outside brackets. Searching for a bare '+' and comparing
    // against the first '[' got this backwards for
    // "bestvideo[height<=720]+bestaudio", where the bracket opens before the
    // plus, so the whole string was treated as one spec and the filter lost.
    {
        int depth = 0;
        size_t plus = std::string::npos;
        for (size_t i = 0; i < spec.size(); ++i) {
            if (spec[i] == '[') ++depth;
            else if (spec[i] == ']') --depth;
            else if (spec[i] == '+' && depth == 0) { plus = i; break; }
        }
        if (plus != std::string::npos) {
            const Picked a = pick_one(fmts, spec.substr(0, plus));
            const Picked b = pick_one(fmts, spec.substr(plus + 1));
            p.video = a.video ? a.video : b.video;
            p.audio = a.audio ? a.audio : b.audio;
            // Both halves resolving to video means the spec was nonsense
            // (e.g. "137+299"); keep the first and report no audio rather
            // than handing a caller a video stream labelled as audio.
            if (a.video && b.video && !a.audio && !b.audio) p.audio = nullptr;
            return p;
        }
    }

    Quality q;
    int itag = 0;
    parse_filters(spec, q, itag);
    if (itag) {
        if (const StreamFormat* f = by_itag(fmts, itag)) {
            if (f->is_audio_only()) p.audio = f;
            else { p.video = f; p.muxed = f->is_muxed(); }
        }
        return p;
    }

    std::string base = spec;
    const size_t br = spec.find('[');
    if (br != std::string::npos) base = spec.substr(0, br);
    // yt-dlp's "*" suffix means "may contain either stream"; we already allow that.
    while (!base.empty() && base.back() == '*') base.pop_back();

    const bool worst = base.rfind("worst", 0) == 0 || base == "w" ||
                       base == "wv" || base == "wa";
    if (worst) q.smallest = true;

    if (base == "bestvideo" || base == "bv" || base == "worstvideo" || base == "wv") {
        p.video = Selector::select(fmts, Mode::VideoOnly, q).video;
    } else if (base == "bestaudio" || base == "ba" || base == "worstaudio" || base == "wa") {
        p.audio = Selector::select(fmts, Mode::AudioOnly, q).audio;
    } else if (base == "best" || base == "b" || base == "worst" || base == "w" ||
               base.empty() || base == "all" || base == "mergeall") {
        // "best" / "worst": yt-dlp means a single file carrying both tracks,
        // which on modern uploads is only the muxed stream. Fall back to a
        // pair when no muxed stream survives the filters.
        const Selection s = Selector::select(fmts, Mode::AudioVideo, q);
        p.video = s.video;
        p.audio = s.audio;
        p.muxed = s.muxed;
    } else {
        // An unrecognised name resolves to nothing, so a "/" chain moves on to
        // the next alternative. Treating it as "best" made every fallback
        // chain succeed on its first element no matter what it said.
        return p;
    }
    return p;
}

inline Picked pick_format(const std::vector<StreamFormat>& fmts, const std::string& spec) {
    size_t start = 0;
    while (start <= spec.size()) {
        // Split on "/" at bracket depth zero.
        int depth = 0;
        size_t cut = std::string::npos;
        for (size_t i = start; i < spec.size(); ++i) {
            if (spec[i] == '[') ++depth;
            else if (spec[i] == ']') --depth;
            else if (spec[i] == '/' && depth == 0) { cut = i; break; }
        }
        const std::string alt = spec.substr(start, cut == std::string::npos
                                                 ? std::string::npos : cut - start);
        if (!alt.empty()) {
            Picked p = pick_one(fmts, alt);
            if (p.ok()) return p;
        }
        if (cut == std::string::npos) break;
        start = cut + 1;
    }
    return {};
}

} // namespace ytdlp
