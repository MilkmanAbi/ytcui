#pragma once
/*
 * ytcui-dl — yt_cache.h
 *
 * Tiny disk cache for values that stay valid across process lifetimes.
 *
 * This exists for one measured reason: the visitorData bootstrap is a full
 * HTTPS round trip (~50 ms of a ~115 ms cold start, so roughly 40% of the time
 * to first stream) and the value it fetches is good for hours. A command line
 * tool that runs and exits was paying that on every single invocation.
 *
 * Deliberately not a general cache. No eviction policy, no index, no locking
 * beyond an atomic rename -- one small file per key, written to a temp name
 * and renamed into place so a reader never sees a half-written value and two
 * concurrent writers cannot interleave.
 *
 * Failure is always silent and non-fatal: an unwritable cache directory means
 * the value is simply refetched, exactly as before.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace ytfast {

class DiskCache {
public:
    // Honours XDG_CACHE_HOME, else ~/.cache/ytcui-dl. Empty if neither is
    // usable, which disables the cache.
    static const std::string& dir() {
        static const std::string d = compute_dir();
        return d;
    }

    static bool enabled() { return !dir().empty(); }

    // Returns false when absent, unreadable, or older than max_age_seconds.
    static bool get(const std::string& key, std::string& out, int max_age_seconds) {
        if (!enabled()) return false;
        const std::string path = dir() + "/" + sanitize(key);

        struct stat st{};
        if (::stat(path.c_str(), &st) != 0) return false;
        if (max_age_seconds > 0) {
            const time_t age = ::time(nullptr) - st.st_mtime;
            // A negative age means a clock jump, not a fresh file; treat it as
            // stale rather than trusting it indefinitely.
            if (age < 0 || age > max_age_seconds) return false;
        }

        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        char buf[4096];
        size_t n = std::fread(buf, 1, sizeof buf, f);
        std::fclose(f);
        if (n == 0) return false;
        out.assign(buf, n);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return !out.empty();
    }

    static bool put(const std::string& key, const std::string& value) {
        if (!enabled() || value.empty() || value.size() > 4096) return false;
        ensure_dir();
        const std::string path = dir() + "/" + sanitize(key);
        const std::string tmp  = path + ".tmp." + std::to_string((long)::getpid());

        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return false;
        const bool wrote = std::fwrite(value.data(), 1, value.size(), f) == value.size();
        std::fclose(f);
        if (!wrote) { ::unlink(tmp.c_str()); return false; }

        // Atomic on POSIX: readers see either the old file or the new one.
        if (::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
        return true;
    }

    static void drop(const std::string& key) {
        if (!enabled()) return;
        ::unlink((dir() + "/" + sanitize(key)).c_str());
    }

private:
    static std::string compute_dir() {
        if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x)
            return std::string(x) + "/ytcui-dl";
        if (const char* h = std::getenv("HOME"); h && *h)
            return std::string(h) + "/.cache/ytcui-dl";
        return {};
    }

    static void ensure_dir() {
        const std::string& d = dir();
        if (d.empty()) return;
        // Create parents one level at a time; mkdir on an existing path is a
        // harmless EEXIST.
        for (size_t i = 1; i < d.size(); ++i) {
            if (d[i] != '/') continue;
            ::mkdir(d.substr(0, i).c_str(), 0700);
        }
        ::mkdir(d.c_str(), 0700);
    }

    // Keys come from us, not from user input, but a key containing a slash
    // would silently write outside the cache directory.
    static std::string sanitize(const std::string& k) {
        std::string s;
        s.reserve(k.size());
        for (char c : k) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            s += ok ? c : '_';
        }
        return s.empty() ? std::string("_") : s;
    }
};

} // namespace ytfast
