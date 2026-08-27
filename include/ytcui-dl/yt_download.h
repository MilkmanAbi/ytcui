#pragma once
/*
 * ytcui-dl — yt_download.h
 *
 * Download engine: parallel ranged chunks, resume, progress, cancellation.
 *
 * Parallelism is not a micro-optimisation here. YouTube's CDN rate-limits an
 * individual connection well below what a link can carry, so a single-stream
 * download of a large video is throttle-bound rather than bandwidth-bound.
 * Issuing several Range requests at once is the same thing aria2 and yt-dlp's
 * external downloaders do, and it is usually a multiple, not a few percent.
 *
 * Writes go through pwrite(), which takes an explicit offset and does not
 * touch the shared file description offset, so N workers can fill their own
 * regions of one file without locking or seeking.
 *
 * Everything degrades: a server that ignores Range, or a response with no
 * Content-Length, falls back to a single sequential stream automatically.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "yt_http.h"
#include "yt_types.h"

namespace ytfast {

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------
struct Progress {
    int64_t downloaded  = 0;
    int64_t total       = 0;   // -1 when the server never said
    double  speed_bps   = 0;
    double  eta_seconds = -1;  // -1 when unknown
    int     connections = 1;
    bool    resumed     = false;

    double percent() const {
        return total > 0 ? 100.0 * (double)downloaded / (double)total : -1.0;
    }
};

// Return false from a progress callback to cancel the transfer.
using ProgressFn = std::function<bool(const Progress&)>;

struct DownloadOptions {
    // Concurrent range requests. 1 disables parallelism. Measured against
    // googlevideo: exactly 3 simultaneous connections against one signed URL
    // are honoured; the 4th+ gets an immediate 403 (the worker pool retries
    // those with backoff, but staying at 3 avoids wasting requests on it).
    int      connections = 3;

    // Below this, parallelism costs more in handshakes than it recovers.
    int64_t  min_parallel_size = 2 * 1024 * 1024;

    bool     resume = true;
    ProgressFn on_progress;
    std::string user_agent;

    // Wall-clock cap for the whole transfer, 0 = none.
    int      timeout_seconds = 0;
};

struct DownloadResult {
    bool        ok = false;
    int64_t     bytes = 0;
    long        status = 0;
    double      seconds = 0;
    bool        cancelled = false;
    std::string error;

    double speed_bps() const { return seconds > 0 ? (double)bytes / seconds : 0; }
};

// ---------------------------------------------------------------------------
// Human-readable helpers, useful enough to a TUI to be worth exposing.
// ---------------------------------------------------------------------------
inline std::string human_bytes(int64_t n) {
    char b[40];
    const double d = (double)n;
    if (n >= 1024LL * 1024 * 1024) std::snprintf(b, sizeof b, "%.2f GB", d / (1024.0 * 1024 * 1024));
    else if (n >= 1024 * 1024)     std::snprintf(b, sizeof b, "%.1f MB", d / (1024.0 * 1024));
    else if (n >= 1024)            std::snprintf(b, sizeof b, "%.1f KB", d / 1024.0);
    else                           std::snprintf(b, sizeof b, "%lld B", (long long)n);
    return b;
}

inline std::string human_duration(double s) {
    if (s < 0) return "--:--";
    int t = (int)(s + 0.5);
    char b[32];
    if (t >= 3600) std::snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, (t % 3600) / 60, t % 60);
    else           std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

// ---------------------------------------------------------------------------
// Downloader
// ---------------------------------------------------------------------------
class Downloader {
public:
    // Fetch `url` into `path`.
    static DownloadResult fetch(const std::string& url,
                                const std::string& path,
                                const DownloadOptions& opt = {}) {
        using clock = std::chrono::steady_clock;
        const auto started = clock::now();
        DownloadResult res;

        int64_t total = probe_size(url, opt);

        // Resume only makes sense against a known total and an existing prefix.
        int64_t have = 0;
        if (opt.resume && total > 0) {
            struct stat st{};
            if (::stat(path.c_str(), &st) == 0 && st.st_size > 0) {
                if (st.st_size >= total) {           // already complete
                    res.ok = true;
                    res.bytes = st.st_size;
                    res.status = 200;
                    res.seconds = elapsed(started);
                    return res;
                }
                have = st.st_size;
            }
        }

        // googlevideo's CDN 403s two kinds of request against these signed
        // URLs: a plain unranged GET, and a Range request that spans too much
        // of the stream (empirically close to a minute of playback at the
        // stream's bitrate -- undocumented, unpredictable, and different per
        // stream). fetch_chunked's per-span retry-and-split handles both, so
        // any server that actually honours Range goes through it even at
        // connections=1; only a server that does not support Range at all
        // falls back to the old single-stream path.
        const bool ranged = total > 0 && supports_range(url, opt);

        if (ranged) {
            const int n_conn = (opt.connections > 1 && total >= opt.min_parallel_size)
                                    ? opt.connections : 1;
            res = fetch_chunked(url, path, opt, n_conn, total, have, started);
        } else {
            res = fetch_serial(url, path, opt, total, have, started);
        }

        // A failed transfer that wrote nothing should not leave an empty file:
        // it is confusing on disk, and a later resume would have to special
        // case it. Anything partially written is kept so resume can use it.
        if (!res.ok && have == 0) {
            struct stat st{};
            if (::stat(path.c_str(), &st) == 0 && st.st_size == 0) ::unlink(path.c_str());
        }

        res.seconds = elapsed(started);
        return res;
    }

    // Truncating a UTF-8 string at a fixed byte count can land mid-sequence
    // (real risk here: YouTube allows 100-character titles, and CJK/emoji
    // run 3-4 bytes each in UTF-8, so a long non-Latin title routinely
    // exceeds a 180-byte cutoff). Back off over any trailing continuation
    // bytes, then drop the lead byte too if its sequence didn't fully fit,
    // so the result is always a whole number of codepoints.
    static void utf8_safe_truncate(std::string& s, size_t max_bytes) {
        if (s.size() <= max_bytes) return;
        s.resize(max_bytes);
        size_t i = s.size();
        while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) --i;
        if (i == 0) { s.clear(); return; }
        const unsigned char lead = static_cast<unsigned char>(s[i - 1]);
        size_t seq_len = 1;
        if      ((lead & 0xE0) == 0xC0) seq_len = 2;
        else if ((lead & 0xF0) == 0xE0) seq_len = 3;
        else if ((lead & 0xF8) == 0xF0) seq_len = 4;
        if (s.size() - (i - 1) < seq_len) s.resize(i - 1);
    }

    // Convenience: derive a filename from video metadata and a format.
    static std::string suggest_filename(const VideoInfo& info, const StreamFormat& f) {
        std::string name = info.title.empty() ? info.id : info.title;
        std::string safe;
        safe.reserve(name.size());
        for (char c : name) {
            unsigned char u = (unsigned char)c;
            if (u < 0x20) continue;
            if (std::strchr("/\\:*?\"<>|", c)) { safe += '_'; continue; }
            safe += c;
        }
        while (!safe.empty() && (safe.back() == ' ' || safe.back() == '.')) safe.pop_back();
        if (safe.empty()) safe = info.id.empty() ? "video" : info.id;
        utf8_safe_truncate(safe, 180);
        safe += " [" + (info.id.empty() ? std::string("x") : info.id) + "]";
        safe += "." + ext_for(f);
        return safe;
    }

    static std::string ext_for(const StreamFormat& f) {
        if (f.is_audio_only()) {
            if (f.container == "mp4")  return "m4a";
            if (f.container == "webm") return "opus";
            return std::string(f.container.empty() ? "bin" : f.container);
        }
        if (f.container.empty()) return "mp4";
        return std::string(f.container);
    }

private:
    static double elapsed(std::chrono::steady_clock::time_point t0) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    static std::vector<std::string> headers(const DownloadOptions& opt) {
        std::vector<std::string> h;
        if (!opt.user_agent.empty()) h.emplace_back("User-Agent: " + opt.user_agent);
        return h;
    }

    // A ranged GET for one byte tells us both the total size and whether the
    // server honours Range, in a single cheap request.
    static int64_t probe_size(const std::string& url, const DownloadOptions& opt) {
        try {
            HttpClient h;
            h.set_timeouts(8000, 15000);
            auto hdr = headers(opt);
            hdr.emplace_back("Range: bytes=0-0");
            int64_t total = -1;
            h.download(url, [](const char*, size_t) { return false; }, 
                       [&](int64_t, int64_t t) { if (t > 0 && total < 0) total = t; },
                       0, hdr);
            if (total == 1) {
                // Content-Length of a 206 is the range length, not the file
                // size; the real total lives in Content-Range.
                total = h.last_range_total();
            }
            if (total <= 0) total = h.last_range_total();
            return total;
        } catch (...) { return -1; }
    }

    static bool supports_range(const std::string& url, const DownloadOptions& opt) {
        try {
            HttpClient h;
            h.set_timeouts(8000, 15000);
            auto hdr = headers(opt);
            hdr.emplace_back("Range: bytes=0-0");
            long st = h.download(url, [](const char*, size_t) { return true; }, {}, 0, hdr);
            return st == 206;
        } catch (...) { return false; }
    }

    // -----------------------------------------------------------------------
    // Single stream. Also the fallback whenever ranges are unavailable.
    // -----------------------------------------------------------------------
    static DownloadResult fetch_serial(const std::string& url, const std::string& path,
                                       const DownloadOptions& opt, int64_t total,
                                       int64_t have,
                                       std::chrono::steady_clock::time_point /*t0*/) {
        DownloadResult res;
        // "r+b" for resume: "wb" truncates, which would leave a hole of zeros
        // at the head of the file after seeking to the offset.
        FILE* fp = std::fopen(path.c_str(), have > 0 ? "r+b" : "wb");
        if (!fp) { res.error = "cannot open " + path + ": " + std::strerror(errno); return res; }
        if (have > 0) std::fseek(fp, 0, SEEK_END);

        int64_t got = 0;
        bool cancelled = false, write_fail = false;
        auto last = std::chrono::steady_clock::now();
        int64_t last_bytes = 0;
        double  speed = 0;

        auto sink = [&](const char* p, size_t n) {
            if (std::fwrite(p, 1, n, fp) != n) { write_fail = true; return false; }
            return true;
        };
        auto prog = [&](int64_t g, int64_t t) {
            got = g;
            if (!opt.on_progress) return;
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - last).count();
            if (dt < 0.1) return;
            double inst = (double)(g - last_bytes) / dt;
            speed = speed > 0 ? speed * 0.7 + inst * 0.3 : inst;   // smoothed
            last = now; last_bytes = g;

            Progress pr;
            pr.downloaded  = have + g;
            pr.total       = t > 0 ? have + t : total;
            pr.speed_bps   = speed;
            pr.connections = 1;
            pr.resumed     = have > 0;
            if (speed > 1 && pr.total > 0)
                pr.eta_seconds = (double)(pr.total - pr.downloaded) / speed;
            if (!opt.on_progress(pr)) cancelled = true;
        };

        try {
            HttpClient h;
            h.set_timeouts(10000, 60000);
            res.status = h.download(url, [&](const char* p, size_t n) {
                if (cancelled) return false;
                return sink(p, n);
            }, prog, have, headers(opt));
        } catch (const std::exception& e) {
            res.error = e.what();
        }
        std::fclose(fp);

        res.bytes     = have + got;
        res.cancelled = cancelled;
        if (write_fail)                   res.error = "write failed";
        else if (cancelled)               res.error = "cancelled";
        else if (res.error.empty() && res.status >= 400)
            res.error = "HTTP " + std::to_string(res.status);
        res.ok = res.error.empty() && (res.status == 200 || res.status == 206);
        return res;
    }

    // -----------------------------------------------------------------------
    // Fetch [off, off+len) of `url` into `fd` at that byte offset.
    //
    // googlevideo's CDN 403s a Range request three independent ways, all
    // indistinguishable at the HTTP level:
    //   - too many simultaneous connections against one signed URL (measured
    //     at exactly 3; the 4th+ gets an immediate 403 rather than queuing)
    //   - a single request that spans too much of the stream, and
    //   - a cumulative budget *per TCP connection*: once roughly a minute of
    //     the stream (at its bitrate) has flowed over one connection, that
    //     same connection 403s every further request on it no matter how
    //     small, even though a fresh connection would serve the same bytes
    //     fine. Confirmed by replaying several small ranged GETs over one
    //     kept-alive curl connection: the first ~6 succeed, the 7th 403s at
    //     the same cumulative point a single big request would have.
    // A fresh HttpClient -- a fresh TCP+TLS connection -- per attempt sidesteps
    // the third case entirely (each attempt starts its own budget from zero),
    // leaves the first case to backoff-and-retry (another worker finishes and
    // frees a slot), and the second to halve-and-recurse once retries are
    // exhausted on a 403.
    // -----------------------------------------------------------------------
    static bool fetch_span(const std::string& url,
                           const std::vector<std::string>& base_hdr,
                           int fd, int64_t off, int64_t len,
                           std::atomic<int64_t>& done_bytes,
                           std::atomic<bool>& stop, std::string& err) {
        if (len <= 0) return true;
        // Kept short: a 403 that survives a fresh connection is, in practice,
        // almost never the transient concurrency cap resolving itself -- it is
        // YouTube declining to serve any more of this stream at all without a
        // Proof-of-Origin token (see set_po_token/--po-token), a wall no amount
        // of retrying or splitting gets past. Burning tens of seconds per
        // chunk on that would make a hopeless download look hung instead of
        // failing promptly.
        static constexpr int     kAttempts = 2;
        static constexpr int64_t kFloor    = 1024 * 1024;

        long last_status = 0;
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            if (stop.load()) return false;
            if (attempt > 0)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(150 * (1 << (attempt - 1))));

            int64_t written = 0;
            bool pwrite_failed = false;
            auto hdr = base_hdr;
            hdr.emplace_back("Range: bytes=" + std::to_string(off) + "-" +
                             std::to_string(off + len - 1));
            try {
                HttpClient h;
                h.set_timeouts(10000, 60000);
                last_status = h.download(url, [&](const char* p, size_t n) {
                    if (stop.load()) return false;
                    size_t left = (size_t)(len - written);
                    size_t take = n < left ? n : left;
                    ssize_t w = ::pwrite(fd, p, take, off + written);
                    if (w <= 0) { pwrite_failed = true; return false; }
                    written += w;
                    done_bytes.fetch_add(w);
                    return written < len;
                }, {}, 0, hdr);
            } catch (const std::exception& e) {
                done_bytes.fetch_sub(written);
                err = e.what();
                continue;
            }
            if (pwrite_failed) { err = "pwrite failed"; return false; }
            if (last_status == 200 || last_status == 206) return true;
            // A retry (or a split) re-fetches these bytes, so undo the credit
            // -- otherwise done_bytes and the final size check double-count.
            done_bytes.fetch_sub(written);
            err = "HTTP " + std::to_string(last_status);
        }

        if (last_status == 403 && len > kFloor) {
            int64_t half = len / 2;
            return fetch_span(url, base_hdr, fd, off, half, done_bytes, stop, err) &&
                   fetch_span(url, base_hdr, fd, off + half, len - half, done_bytes, stop, err);
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // Chunked fetch, 1 or more connections.
    //
    // The file is preallocated to its final size and each worker pwrite()s
    // into its own region. Workers pull chunk indices off a shared counter
    // rather than getting a fixed share, so a slow connection cannot hold up
    // the whole transfer. Every request goes through fetch_span, so this
    // handles connections=1 too -- a bare Range-less GET against these URLs
    // 403s just like an oversized one does.
    // -----------------------------------------------------------------------
    static DownloadResult fetch_chunked(const std::string& url, const std::string& path,
                                        const DownloadOptions& opt, int n_conn,
                                        int64_t total, int64_t have,
                                        std::chrono::steady_clock::time_point t0) {
        DownloadResult res;
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT, 0644);
        if (fd < 0) { res.error = "cannot open " + path + ": " + std::strerror(errno); return res; }
        // ftruncate extends without touching existing bytes, so a resumed
        // prefix survives; it only pads the new tail with zeros to be filled in.
        if (::ftruncate(fd, total) != 0) {
            ::close(fd);
            res.error = std::string("ftruncate: ") + std::strerror(errno);
            return res;
        }

        // Enough chunks that workers can steal work, few enough that per-chunk
        // request overhead stays negligible. fetch_span shrinks further on its
        // own if even this turns out to be more than the CDN allows.
        int64_t chunk = total / (n_conn * 4);
        const int64_t kMinChunk = 1024 * 1024, kMaxChunk = 8 * 1024 * 1024;
        if (chunk < kMinChunk) chunk = kMinChunk;
        if (chunk > kMaxChunk) chunk = kMaxChunk;
        const int64_t n_chunks = (total + chunk - 1) / chunk;
        // Resume: skip chunks fully covered by what is already on disk. A
        // chunk straddling `have` gets refetched whole -- pwrite makes that
        // an overwrite of identical bytes, not corruption.
        const int64_t start_chunk = have > 0 ? have / chunk : 0;

        std::atomic<int64_t> next_chunk{start_chunk};
        std::atomic<int64_t> done_bytes{have};
        std::atomic<bool>    stop{false};
        std::mutex           err_mu;
        std::string          first_error;
        const auto base_hdr = headers(opt);

        auto worker = [&] {
            for (;;) {
                int64_t idx = next_chunk.fetch_add(1);
                if (idx >= n_chunks || stop.load()) return;
                const int64_t off = idx * chunk;
                const int64_t len = (off + chunk > total) ? total - off : chunk;

                std::string err;
                if (!fetch_span(url, base_hdr, fd, off, len, done_bytes, stop, err)) {
                    std::lock_guard<std::mutex> lk(err_mu);
                    if (first_error.empty()) first_error = err.empty() ? "download failed" : err;
                    stop.store(true);
                    return;
                }
            }
        };

        std::vector<std::thread> pool;
        pool.reserve(n_conn);
        for (int i = 0; i < n_conn; ++i) pool.emplace_back(worker);

        // Progress is reported from this thread so the callback never fires
        // concurrently -- a UI can touch its own state without a lock.
        bool cancelled = false;
        {
            auto last = std::chrono::steady_clock::now();
            int64_t last_bytes = have;
            int64_t max_reported = have;
            double  speed = 0;
            while (!stop.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
                // A chunk that fails mid-flight subtracts its partial credit
                // before retrying (see fetch_span), which can transiently dip
                // the raw counter even though the file itself never regresses.
                // Report the high-water mark so progress only ever climbs.
                int64_t got = done_bytes.load();
                if (got > max_reported) max_reported = got;
                got = max_reported;
                bool finished = got >= total || next_chunk.load() >= n_chunks + n_conn;

                if (opt.on_progress) {
                    auto now = std::chrono::steady_clock::now();
                    double dt = std::chrono::duration<double>(now - last).count();
                    if (dt > 0) {
                        double inst = (double)(got - last_bytes) / dt;
                        speed = speed > 0 ? speed * 0.7 + inst * 0.3 : inst;
                        last = now; last_bytes = got;
                    }
                    Progress pr;
                    pr.downloaded  = got;
                    pr.total       = total;
                    pr.speed_bps   = speed;
                    pr.connections = n_conn;
                    pr.resumed     = have > 0;
                    if (speed > 1) pr.eta_seconds = (double)(total - got) / speed;
                    if (!opt.on_progress(pr)) { cancelled = true; stop.store(true); break; }
                }
                if (opt.timeout_seconds > 0 && elapsed(t0) > opt.timeout_seconds) {
                    std::lock_guard<std::mutex> lk(err_mu);
                    if (first_error.empty()) first_error = "timed out";
                    stop.store(true);
                    break;
                }
                if (finished && got >= total) break;
            }
        }

        for (auto& t : pool) if (t.joinable()) t.join();
        ::close(fd);

        res.bytes     = done_bytes.load();
        res.status    = res.bytes >= total ? 206 : 0;
        res.cancelled = cancelled;
        {
            std::lock_guard<std::mutex> lk(err_mu);
            res.error = first_error;
        }
        if (cancelled && res.error.empty()) res.error = "cancelled";
        if (res.error.empty() && res.bytes < total)
            res.error = "short read: " + std::to_string(res.bytes) + "/" + std::to_string(total);
        res.ok = res.error.empty() && res.bytes >= total;
        return res;
    }
};

// ---------------------------------------------------------------------------
// Muxing.
//
// Downloading video and audio separately is what unlocks quality above the
// muxed ceiling, but it leaves two files. ffmpeg can stitch them without
// re-encoding (stream copy), which is close to instant and lossless. If ffmpeg
// is absent the two files simply remain, and the caller can say so.
// ---------------------------------------------------------------------------
inline bool have_ffmpeg() {
    static const bool present = (std::system("ffmpeg -version >/dev/null 2>&1") == 0);
    return present;
}

inline bool mux_av(const std::string& video_path, const std::string& audio_path,
                   const std::string& out_path, std::string* err = nullptr) {
    if (!have_ffmpeg()) {
        if (err) *err = "ffmpeg not found";
        return false;
    }
    auto quote = [](const std::string& s) {
        std::string q = "'";
        for (char c : s) { if (c == '\'') q += "'\\''"; else q += c; }
        return q + "'";
    };
    std::string cmd = "ffmpeg -y -loglevel error -i " + quote(video_path) +
                      " -i " + quote(audio_path) +
                      " -c copy -map 0:v:0 -map 1:a:0 " + quote(out_path) +
                      " >/dev/null 2>&1";
    int rc = std::system(cmd.c_str());
    if (rc != 0 && err) *err = "ffmpeg exited " + std::to_string(rc);
    return rc == 0;
}

} // namespace ytfast
