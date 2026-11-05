#pragma once
/*
 * ytcui-dl — yt_stream_dl.h
 *
 * Alternate download path for -x/--remux: fetch the resolved format(s) and
 * hand them to ffmpeg to produce a single finished file -- an MP3 for
 * audio-only, an MP4 otherwise -- instead of leaving separate raw files
 * around the way the main download path does.
 *
 * The obvious way to build this is to hand ffmpeg the googlevideo URL
 * directly and let its own HTTP client do the fetch. That turns out to be
 * the wrong call: ffmpeg's http protocol holds one continuous connection for
 * the whole input, and past an initial burst this CDN throttles a sustained
 * read on one connection to roughly realtime speed rather than full
 * bandwidth -- measured at ~1.5x on a stream this project's own chunked
 * engine (yt_download.h) pulls at ~180x by never holding one connection
 * open long enough to trigger it (see Downloader::fetch_span). A 10-minute
 * video would take on the order of 10 minutes to "stream through ffmpeg"
 * directly. So this fetches through Downloader::fetch to a temp file first
 * -- fast, and already proven against this exact throttle -- and only hands
 * ffmpeg local paths, where none of this applies and it just remuxes/
 * transcodes at disk speed.
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include "yt_download.h"

namespace ytfast {

struct StreamDlResult {
    bool        ok = false;
    bool        transcoded = false;  // false = remuxed/copied, no re-encode
    std::string error;
};

class StreamDownloader {
public:
    // audio_only: audio_url must be set (video_url is ignored) -> transcoded
    // to MP3. YouTube never serves MP3 source, so this path always encodes.
    //
    // otherwise: video_url must be set; audio_url may be empty (muxed or
    // video-only fetch) or set (separate adaptive audio track) -> output is
    // MP4, stream-copied when the source codecs are legal in an MP4
    // container (av01/avc1 video, aac audio) and transcoded automatically
    // if a copy remux fails (e.g. vp9 video or opus audio, neither legal in
    // MP4).
    static StreamDlResult fetch(const std::string& video_url,
                                const std::string& audio_url,
                                const std::string& out_path,
                                const std::string& user_agent,
                                bool audio_only,
                                bool quiet = false) {
        StreamDlResult res;
        if (!have_ffmpeg()) { res.error = "ffmpeg not found"; return res; }
        if (audio_only ? audio_url.empty() : video_url.empty()) {
            res.error = "nothing to fetch";
            return res;
        }

        DownloadOptions dopt;
        dopt.user_agent = user_agent;
        dopt.connections = 3;
        dopt.on_progress = nullptr;

        OutputStage stage(out_path, audio_only ? ".mp3" : ".mp4");
        std::string tmp_video, tmp_audio;
        auto cleanup_temps = [&] {
            if (!tmp_video.empty()) ::remove(tmp_video.c_str());
            if (!tmp_audio.empty()) ::remove(tmp_audio.c_str());
        };

        if (!audio_only && !video_url.empty()) {
            tmp_video = stage.directory + "/video.src";
            auto r = Downloader::fetch(video_url, tmp_video, dopt);
            if (!r.ok) {
                res.error = "video fetch: " + (r.error.empty() ? "failed" : r.error);
                cleanup_temps();
                return res;
            }
        }
        if (!audio_url.empty()) {
            tmp_audio = stage.directory + "/audio.src";
            auto r = Downloader::fetch(audio_url, tmp_audio, dopt);
            if (!r.ok) {
                res.error = "audio fetch: " + (r.error.empty() ? "failed" : r.error);
                cleanup_temps();
                return res;
            }
        }

        if (audio_only) {
            int rc = run(build_cmd(tmp_video, tmp_audio, stage.output, true, quiet, false));
            cleanup_temps();
            if (rc != 0) { res.error = "ffmpeg exited " + std::to_string(rc); return res; }
            res.ok = stage.finalize();
            if (!res.ok) res.error = "cannot finalize output without overwriting an existing file";
            res.transcoded = true;
            return res;
        }

        int rc = run(build_cmd(tmp_video, tmp_audio, stage.output, false, quiet, true));
        if (rc != 0) {
            // A stream copy fails outright when the source codec isn't
            // legal in MP4 (vp9 video, opus audio) rather than silently
            // transcoding -- ffmpeg errors and writes nothing usable. Retry
            // once with a real transcode instead of copy.
            ::remove(stage.output.c_str());
            rc = run(build_cmd(tmp_video, tmp_audio, stage.output, false, quiet, false));
            res.transcoded = rc == 0;
        }
        cleanup_temps();
        if (rc != 0) { res.error = "ffmpeg exited " + std::to_string(rc); return res; }
        res.ok = stage.finalize();
        if (!res.ok) res.error = "cannot finalize output without overwriting an existing file";
        return res;
    }

private:
    static int run(const std::vector<std::string>& args) { return run_process(args); }
    static std::vector<std::string> build_cmd(const std::string& video_path, const std::string& audio_path,
                                             const std::string& output, bool audio_only, bool quiet, bool copy) {
        std::vector<std::string> args{"ffmpeg", "-n", "-loglevel", quiet ? "error" : "warning"};
        if (audio_only) {
            args.insert(args.end(), {"-i", audio_path, "-vn", "-map", "0:a:0", "-c:a", "libmp3lame", "-q:a", "2"});
        } else {
            args.insert(args.end(), {"-i", video_path});
            if (!audio_path.empty()) args.insert(args.end(), {"-i", audio_path});
            args.insert(args.end(), {"-map", "0:v:0", "-map", audio_path.empty() ? "0:a:0?" : "1:a:0?"});
            if (copy) args.insert(args.end(), {"-c", "copy"});
            else args.insert(args.end(), {"-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-c:a", "aac", "-b:a", "192k"});
            args.insert(args.end(), {"-movflags", "+faststart"});
        }
        args.push_back(output);
        return args;
    }
};
} // namespace ytfast
