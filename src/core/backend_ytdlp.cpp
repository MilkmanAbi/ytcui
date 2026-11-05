#include "core/backend_ytdlp.h"

#include "core/backend.h"
#include "core/log.h"
#include "core/proc.h"
#include "core/text.h"
#include "core/url.h"

#include <algorithm>
#include <cstdlib>

#include "yj.h"

namespace ytcui::ytdlp {
namespace {

std::string str_of(yj::Val v) {
    if (!v.valid() || !v.is_str()) return {};
    return yj::unescape(v.raw());
}

int64_t int_of(yj::Val v) {
    if (!v.valid()) return 0;
    if (v.is_str()) {
        const std::string s = yj::unescape(v.raw());
        return std::strtoll(s.c_str(), nullptr, 10);
    }
    return v.i64();
}

// Pick a usable thumbnail. Flat-playlist entries carry a `thumbnails` array
// instead of the single `thumbnail` field, so both shapes are handled.
std::string thumbnail_of(yj::Val node) {
    if (yj::Val t = yj::get(node, "thumbnail"); t.is_str())
        return yj::unescape(t.raw());

    std::string best;
    int best_w = -1;
    yj::each_elem(yj::get(node, "thumbnails"), [&](yj::Val th) {
        const std::string u = str_of(yj::get(th, "url"));
        if (u.empty()) return true;
        const int w = (int)int_of(yj::get(th, "width"));
        // Prefer the widest, but take anything rather than nothing: some
        // entries omit width entirely.
        if (w > best_w) { best_w = w; best = u; }
        else if (best.empty()) best = u;
        return true;
    });
    return best;
}

Video video_from(yj::Val node) {
    Video v;
    v.id = str_of(yj::get(node, "id"));
    if (v.id.empty()) return v;

    v.title = str_of(yj::get(node, "title"));
    // `channel` is the display name; `uploader` is the fallback, and one or
    // the other is missing depending on the extractor path.
    v.channel = str_of(yj::get(node, "channel"));
    if (v.channel.empty()) v.channel = str_of(yj::get(node, "uploader"));
    v.channel_id = str_of(yj::get(node, "channel_id"));
    v.description = str_of(yj::get(node, "description"));

    // live_status, not is_live: in flat-playlist output `is_live` is null even
    // for a stream that is live right now.
    const std::string live_status = str_of(yj::get(node, "live_status"));
    v.is_live = live_status == "is_live" || live_status == "post_live";
    if (!v.is_live) {
        if (yj::Val il = yj::get(node, "is_live"); il.valid() && !il.is_str())
            v.is_live = il.boolean();
    }

    v.duration_seconds = (int)int_of(yj::get(node, "duration"));
    // Formatted here rather than taken from yt-dlp's own duration_string,
    // which renders 19 seconds as "19" instead of "0:19".
    v.duration = v.is_live ? std::string() : txt::duration(v.duration_seconds);

    int64_t views = int_of(yj::get(node, "view_count"));
    if (views <= 0) views = int_of(yj::get(node, "concurrent_view_count"));
    if (views > 0) v.view_count = txt::views(views);

    v.upload_date = str_of(yj::get(node, "upload_date"));
    v.thumbnail_url = thumbnail_of(node);

    v.url = str_of(yj::get(node, "webpage_url"));
    if (v.url.empty()) v.url = str_of(yj::get(node, "url"));
    // A flat entry's `url` can be the media URL rather than the page, so a
    // canonical watch URL is safer for anything that shares or re-resolves it.
    if (v.url.empty() || v.url.find("/watch?v=") == std::string::npos)
        v.url = url::watch_url(v.id);
    return v;
}

}  // namespace

std::vector<Video> parse_search(const std::string& json) {
    std::vector<Video> out;
    yj::Val root = yj::parse(json);
    if (!root.is_obj()) return out;

    yj::each_elem(yj::get(root, "entries"), [&](yj::Val e) {
        Video v = video_from(e);
        if (!v.empty()) out.push_back(std::move(v));
        return true;
    });
    return out;
}

Video parse_info(const std::string& json) {
    yj::Val root = yj::parse(json);
    if (!root.is_obj()) return {};
    return video_from(root);
}

std::string format_selector(int max_height, bool audio_only) {
    if (audio_only) return "bestaudio/best";
    if (max_height <= 0) return {};          // let mpv/yt-dlp choose
    const std::string h = std::to_string(max_height);
    // The trailing fallbacks matter: a video with no separate audio track, or
    // no stream under the ceiling, would otherwise select nothing at all.
    return "bestvideo[height<=" + h + "]+bestaudio/best[height<=" + h + "]/best";
}

std::string first_error_line(const std::string& stderr_text) {
    std::size_t start = 0;
    while (start < stderr_text.size()) {
        std::size_t nl = stderr_text.find('\n', start);
        if (nl == std::string::npos) nl = stderr_text.size();
        std::string line = stderr_text.substr(start, nl - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.rfind("ERROR:", 0) == 0) {
            // Drop yt-dlp's own prefix noise so the status line reads cleanly.
            std::string msg = line.substr(6);
            while (!msg.empty() && msg.front() == ' ') msg.erase(msg.begin());
            const std::string tag = "[youtube] ";
            if (msg.rfind(tag, 0) == 0) msg = msg.substr(tag.size());
            if (const auto colon = msg.find(": "); colon != std::string::npos &&
                colon < 20)
                msg = msg.substr(colon + 2);
            return msg;
        }
        start = nl + 1;
    }
    return {};
}

}  // namespace ytcui::ytdlp

// ---------------------------------------------------------------------------
// The backend itself.
// ---------------------------------------------------------------------------
namespace ytcui {
namespace {

// yt-dlp sometimes exits non-zero having still produced usable JSON (one
// unavailable entry in a search, typically), so output is preferred over the
// exit code when deciding whether a call worked.
FailKind classify_stderr(const std::string& err) {
    auto has = [&](const char* needle) {
        return err.find(needle) != std::string::npos;
    };
    if (has("Private video") || has("Video unavailable") ||
        has("has been removed") || has("members-only") ||
        has("This video is not available"))
        return FailKind::NotAvailable;
    if (has("Sign in to confirm") || has("age") || has("cookies"))
        return FailKind::NeedsAuth;
    if (has("Unable to download") || has("Failed to resolve") ||
        has("Connection") || has("timed out") || has("Temporary failure"))
        return FailKind::Network;
    return FailKind::Unknown;
}

class YtDlpBackend final : public Backend {
public:
    YtDlpBackend() { path_ = pick_best(); }

    // Choose the newest yt-dlp on PATH rather than the first one.
    //
    // yt-dlp versions are dates, so a plain string compare orders them. The
    // first match is frequently the worst: a distro package in /usr/bin can
    // be years behind a pip or manual install in ~/.local/bin, and YouTube
    // breaks old versions hard -- an eight-month-old build resolves through
    // an InnerTube client whose URLs the CDN stopped serving, so playback
    // 403s while search still works. Picking by version makes that a
    // non-event instead of a bug report.
    static std::string pick_best() {
        std::vector<std::string> candidates = proc::which_all("yt-dlp");
        for (const std::string& p : proc::which_all("youtube-dl"))
            candidates.push_back(p);
        if (candidates.empty()) return {};
        if (candidates.size() == 1) return candidates.front();

        std::string best, best_ver;
        for (const std::string& c : candidates) {
            const proc::Result r = proc::run({c, "--version"}, 8000);
            if (!r.ok()) continue;
            std::string v = r.out;
            while (!v.empty() && (v.back() == 0x0A || v.back() == 0x0D ||
                                  v.back() == ' '))
                v.pop_back();
            if (v.empty()) continue;
            if (best.empty() || v > best_ver) { best = c; best_ver = v; }
        }
        if (best.empty()) return candidates.front();   // none answered: take any
        if (best != candidates.front())
            log::info("yt-dlp: using " + best + " (" + best_ver +
                      ") over the earlier " + candidates.front() +
                      " found on PATH");
        return best;
    }

    const char* name() const override { return "yt-dlp"; }

    bool available(std::string* detail) const override {
        if (path_.empty()) {
            if (detail)
                *detail = "yt-dlp not found on PATH (pip install yt-dlp, or "
                          "your package manager)";
            return false;
        }
        if (detail) {
            *detail = path_;
            const std::string v = version();
            if (!v.empty()) {
                *detail += " (" + v + ")";
                // An old yt-dlp is worse than no yt-dlp in one specific way:
                // it still runs, and picks InnerTube clients YouTube has since
                // walled, so playback 403s while everything looks fine.
                // Measured here: a 2026.02 build resolved through ANDROID_VR,
                // whose URLs the CDN stopped serving in 2026-08.
                if (looks_stale(v))
                    *detail += " — this looks old; YouTube breaks older "
                               "yt-dlp releases, so update it if playback fails";
            }
        }
        return true;
    }

    // yt-dlp versions are dates: 2026.02.21. Anything more than ~4 months old
    // is very likely to have been overtaken by a YouTube change.
    static bool looks_stale(const std::string& v) {
        if (v.size() < 7 || v[4] != '.') return false;
        const int year = std::atoi(v.substr(0, 4).c_str());
        const int month = std::atoi(v.substr(5, 2).c_str());
        if (year < 2000 || month < 1 || month > 12) return false;
        // Compared against this binary's own build date rather than a
        // hardcoded "now", so the threshold does not start lying as ytcui
        // itself ages. __DATE__ is "MMM DD YYYY".
        const int build_year = (__DATE__[7] - '0') * 1000 + (__DATE__[8] - '0') * 100 +
                               (__DATE__[9] - '0') * 10 + (__DATE__[10] - '0');
        static const char* kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
        int build_month = 0;
        for (int i = 0; i < 12; ++i) {
            if (__DATE__[0] == kMonths[i * 3] && __DATE__[1] == kMonths[i * 3 + 1] &&
                __DATE__[2] == kMonths[i * 3 + 2]) { build_month = i + 1; break; }
        }
        if (build_month == 0) return false;        // unparseable: say nothing
        const int months = (build_year - year) * 12 + (build_month - month);
        return months > 4;
    }

    std::string version() const {
        if (path_.empty()) return {};
        if (!version_cached_) {
            const proc::Result r = proc::run({path_, "--version"}, 8000);
            version_ = r.ok() ? r.out : std::string();
            while (!version_.empty() && (version_.back() == 0x0A ||
                                         version_.back() == 0x0D ||
                                         version_.back() == ' '))
                version_.pop_back();
            version_cached_ = true;
        }
        return version_;
    }

    std::vector<Video> search(const std::string& query, int max_results,
                              Failure* fail) override {
        if (fail) fail->clear();
        std::vector<Video> out;
        if (query.empty()) return out;
        if (path_.empty()) {
            if (fail) {
                fail->kind = FailKind::BackendMissing;
                fail->message = "yt-dlp not installed";
            }
            return out;
        }

        // --flat-playlist keeps this to one request: without it yt-dlp
        // resolves every result's formats, which takes tens of seconds.
        const std::vector<std::string> argv = {
            path_, "--no-warnings", "--ignore-config", "--flat-playlist",
            "--dump-single-json",
            "ytsearch" + std::to_string(max_results > 0 ? max_results : 15) +
                ":" + query,
        };
        const proc::Result r = proc::run(argv, 45000);
        if (!r.ran) {
            if (fail) {
                fail->kind = FailKind::BackendMissing;
                fail->message = "could not run " + path_;
            }
            return out;
        }
        if (r.timed_out) {
            if (fail) {
                fail->kind = FailKind::Network;
                fail->message = "yt-dlp timed out";
            }
            return out;
        }

        out = ytdlp::parse_search(r.out);
        if (out.empty() && fail) {
            const std::string msg = ytdlp::first_error_line(r.err);
            fail->kind = classify_stderr(r.err);
            fail->message = msg.empty() ? "yt-dlp returned no results" : msg;
        }
        return out;
    }

    Video info(const std::string& video_id, Failure* fail) override {
        if (fail) fail->clear();
        if (path_.empty()) {
            if (fail) {
                fail->kind = FailKind::BackendMissing;
                fail->message = "yt-dlp not installed";
            }
            return {};
        }
        const std::vector<std::string> argv = {
            path_, "--no-warnings", "--ignore-config", "--no-playlist",
            "--dump-single-json", url::watch_url(video_id),
        };
        const proc::Result r = proc::run(argv, 45000);
        if (!r.ran || r.timed_out) {
            if (fail) {
                fail->kind = r.timed_out ? FailKind::Network
                                         : FailKind::BackendMissing;
                fail->message = r.timed_out ? "yt-dlp timed out"
                                            : "could not run yt-dlp";
            }
            return {};
        }
        Video v = ytdlp::parse_info(r.out);
        if (v.empty() && fail) {
            const std::string msg = ytdlp::first_error_line(r.err);
            fail->kind = classify_stderr(r.err);
            fail->message = msg.empty() ? "yt-dlp returned no metadata" : msg;
        }
        return v;
    }

    Streams resolve(const std::string& video_id, PlayMode mode, int max_height,
                    Failure* fail) override {
        if (fail) fail->clear();
        if (path_.empty()) {
            if (fail) {
                fail->kind = FailKind::BackendMissing;
                fail->message = "yt-dlp not installed";
            }
            return {};
        }

        // No extraction here, deliberately. mpv is handed the page URL and
        // pointed at yt-dlp, so mpv's ytdl_hook does the resolve and gets the
        // http_headers with it. Lifting a format URL out of `yt-dlp -j` and
        // playing it bare is what 403s, and resolving twice would be slower
        // for no benefit.
        Streams s;
        s.use_ytdl = true;
        s.page_url = url::watch_url(video_id);
        s.ytdl_path = path_;
        s.ytdl_format = ytdlp::format_selector(max_height, mode != PlayMode::Video);
        s.label = "via yt-dlp";
        return s;
    }

private:
    std::string path_;
    // Running `yt-dlp --version` costs a subprocess, so it is done once and
    // only when something actually asks.
    mutable bool        version_cached_ = false;
    mutable std::string version_;
};

}  // namespace

std::unique_ptr<Backend> make_ytdlp_backend() {
    return std::make_unique<YtDlpBackend>();
}

}  // namespace ytcui
