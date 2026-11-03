#pragma once
/*
 * yj.h - zero-allocation JSON scanner
 *
 * Replaces nlohmann/json (24,765 lines, full DOM) for InnerTube parsing.
 *
 * Design:
 *   - No DOM. No allocation. No exceptions. No recursion.
 *   - A Val is a (pointer, end) pair into a buffer you already own.
 *   - Navigation is structural skipping; you only pay for what you read.
 *   - Strings come back as string_view slices of the raw buffer. You only
 *     allocate when you call unescape() on one you actually want to keep.
 *   - Malformed input yields null Vals, never UB and never a throw.
 *
 * The one rule: the buffer must outlive every Val derived from it.
 */

#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace yj {

enum class T : uint8_t { Null, Bool, Num, Str, Arr, Obj };

// ---------------------------------------------------------------------------
// Low-level scanning
// ---------------------------------------------------------------------------

inline const char* skip_ws(const char* p, const char* e) {
    while (p < e && (unsigned char)*p <= ' ') ++p;
    return p;
}

// p points at the opening quote. Returns pointer just past the closing quote.
inline const char* skip_string(const char* p, const char* e) {
    const char* s0 = ++p;
    for (;;) {
        if (p >= e) return e;
        const char* q = (const char*)memchr(p, '"', (size_t)(e - p));
        if (!q) return e;
        // A quote is the terminator unless preceded by an odd run of backslashes.
        const char* b = q;
        int n = 0;
        while (b > s0 && b[-1] == '\\') { --b; ++n; }
        if (!(n & 1)) return q + 1;
        p = q + 1;
    }
}

// Advance past exactly one complete JSON value. Iterative: hostile nesting
// cannot blow the stack.
inline const char* skip_value(const char* p, const char* e) {
    p = skip_ws(p, e);
    if (p >= e) return e;
    int depth = 0;
    do {
        const char c = *p;
        switch (c) {
        case '"': p = skip_string(p, e); break;
        case '{': case '[': ++depth; ++p; break;
        case '}': case ']': --depth; ++p; break;
        case ',': case ':': ++p; break;
        default: {
            const char* s = p;
            while (p < e && *p != ',' && *p != '}' && *p != ']' &&
                   (unsigned char)*p > ' ') ++p;
            if (p == s) ++p;   // never stall on garbage
            break;
        }
        }
        p = skip_ws(p, e);
    } while (depth > 0 && p < e);
    return p;
}

// ---------------------------------------------------------------------------
// Val
// ---------------------------------------------------------------------------

struct Val {
    const char* p = nullptr;   // first char of the value
    const char* e = nullptr;   // end of the enclosing buffer

    bool valid() const { return p && p < e; }
    explicit operator bool() const { return valid(); }

    T type() const {
        if (!valid()) return T::Null;
        switch (*p) {
        case '{': return T::Obj;
        case '[': return T::Arr;
        case '"': return T::Str;
        case 't': case 'f': return T::Bool;
        case 'n': return T::Null;
        default:  return T::Num;
        }
    }
    bool is_obj() const { return valid() && *p == '{'; }
    bool is_arr() const { return valid() && *p == '['; }
    bool is_str() const { return valid() && *p == '"'; }
    bool is_num() const { return type() == T::Num; }

    // Raw slice, still escaped, quotes stripped. No allocation.
    std::string_view raw() const {
        if (!is_str()) return {};
        const char* end = skip_string(p, e);
        if (end <= p + 1) return {};
        return std::string_view(p + 1, (size_t)(end - p - 2));
    }

    int64_t i64(int64_t fb = 0) const {
        if (!valid()) return fb;
        const char* q = p;
        if (*q == '"') { ++q; }              // numeric strings: "12345"
        bool neg = false;
        if (q < e && (*q == '-' || *q == '+')) { neg = (*q == '-'); ++q; }
        if (q >= e || *q < '0' || *q > '9') return fb;
        int64_t v = 0;
        while (q < e && *q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); ++q; }
        return neg ? -v : v;
    }
    double dbl(double fb = 0) const {
        if (!valid()) return fb;
        char buf[48];
        size_t n = 0;
        const char* q = (*p == '"') ? p + 1 : p;
        while (q < e && n < sizeof(buf) - 1 &&
               (( *q >= '0' && *q <= '9') || *q=='-' || *q=='+' || *q=='.' ||
                *q=='e' || *q=='E')) buf[n++] = *q++;
        if (!n) return fb;
        buf[n] = 0;
        return strtod(buf, nullptr);
    }
    bool boolean(bool fb = false) const {
        if (!valid()) return fb;
        if (*p == 't') return true;
        if (*p == 'f') return false;
        return fb;
    }

    // Value that immediately follows this one (used by iterators).
    const char* after() const { return skip_value(p, e); }
};

// ---------------------------------------------------------------------------
// Object access
// ---------------------------------------------------------------------------

// Visit every member of an object exactly once. fn(key_raw, Val).
// Return false from fn to stop early.
template <class F>
inline void each_member(Val obj, F&& fn) {
    if (!obj.is_obj()) return;
    const char* p = skip_ws(obj.p + 1, obj.e);
    while (p < obj.e && *p != '}') {
        if (*p != '"') { p = skip_ws(p + 1, obj.e); continue; }
        const char* kend = skip_string(p, obj.e);
        std::string_view key(p + 1, (size_t)(kend - p - 2));
        p = skip_ws(kend, obj.e);
        if (p < obj.e && *p == ':') p = skip_ws(p + 1, obj.e);
        Val v{p, obj.e};
        if (!fn(key, v)) return;
        p = skip_ws(skip_value(p, obj.e), obj.e);
        if (p < obj.e && *p == ',') p = skip_ws(p + 1, obj.e);
    }
}

// Key comparison with the length baked in by the compiler. Use this instead of
// hand-writing `switch (k.size())` — getting a length wrong there silently
// drops a field, which is exactly the kind of bug that ships.
template <size_t N>
inline bool keyis(std::string_view k, const char (&lit)[N]) {
    return k.size() == N - 1 && memcmp(k.data(), lit, N - 1) == 0;
}

// Single key lookup. O(members) — if you need several fields from the same
// object, use each_member and dispatch, don't call this N times.
inline Val get(Val obj, std::string_view key) {
    Val out;
    each_member(obj, [&](std::string_view k, Val v) {
        if (k == key) { out = v; return false; }
        return true;
    });
    return out;
}

// Chained lookup: path(root, "streamingData", "adaptiveFormats")
template <class... Rest>
inline Val path(Val obj, std::string_view k, Rest... rest) {
    Val v = get(obj, k);
    if constexpr (sizeof...(rest) == 0) return v;
    else return path(v, rest...);
}

// ---------------------------------------------------------------------------
// Array access
// ---------------------------------------------------------------------------

template <class F>
inline void each_elem(Val arr, F&& fn) {
    if (!arr.is_arr()) return;
    const char* p = skip_ws(arr.p + 1, arr.e);
    while (p < arr.e && *p != ']') {
        Val v{p, arr.e};
        if (!fn(v)) return;
        p = skip_ws(skip_value(p, arr.e), arr.e);
        if (p < arr.e && *p == ',') p = skip_ws(p + 1, arr.e);
    }
}

inline size_t count(Val arr) {
    size_t n = 0;
    each_elem(arr, [&](Val) { ++n; return true; });
    return n;
}

// ---------------------------------------------------------------------------
// Recursive search — for search results, where the renderer union types make
// a fixed path unreliable. Finds every object that contains `marker` as a key.
// Bounded by depth to stay predictable.
// ---------------------------------------------------------------------------
// Frames live in a growable stack: a fixed-size one would silently drop
// subtrees when full, which loses search results with no way to notice.
struct Finder {
    struct Frame { const char* p; const char* e; int d; };
    std::vector<Frame> stack;
    Finder() { stack.reserve(128); }

    template <class F>
    void run(Val root, std::string_view marker, F&& fn, int max_depth) {
        if (!root.valid()) return;
        stack.clear();
        stack.push_back({root.p, root.e, 0});
        bool stop = false;
        while (!stack.empty() && !stop) {
            Frame f = stack.back();
            stack.pop_back();
            if (f.d > max_depth) continue;
            Val v{f.p, f.e};
            if (v.is_obj()) {
                each_member(v, [&](std::string_view k, Val child) {
                    if (k == marker) {
                        if (!fn(child)) { stop = true; return false; }
                    } else if (child.is_obj() || child.is_arr()) {
                        stack.push_back({child.p, child.e, f.d + 1});
                    }
                    return true;
                });
            } else if (v.is_arr()) {
                each_elem(v, [&](Val child) {
                    if (child.is_obj() || child.is_arr())
                        stack.push_back({child.p, child.e, f.d + 1});
                    return true;
                });
            }
        }
    }
};

template <class F>
inline void find_all(Val root, std::string_view marker, F&& fn, int max_depth = 64) {
    Finder f;
    f.run(root, marker, static_cast<F&&>(fn), max_depth);
}

// Same walk, but matches any of several markers — one pass instead of N.
// fn(marker, Val).
//
// `prune` names keys whose subtrees are never descended into. In a search
// response `menu` and `navigationEndpoint` are ~87% of the bytes and contain
// nothing we read, so skipping them structurally is most of the win.
//
// Results arrive in document order: children are reversed on push so that
// popping from the back yields a left-to-right DFS. Search results are
// relevance-ranked, so order is not cosmetic.
template <class F>
inline void find_any(Val root, const std::string_view* markers, size_t n, F&& fn,
                     const std::string_view* prune = nullptr, size_t np = 0,
                     int max_depth = 64) {
    if (!root.valid() || !n) return;
    std::vector<Finder::Frame> stack;
    stack.reserve(128);
    stack.push_back({root.p, root.e, 0});
    bool stop = false;
    while (!stack.empty() && !stop) {
        Finder::Frame f = stack.back();
        stack.pop_back();
        if (f.d > max_depth) continue;
        Val v{f.p, f.e};
        const size_t base = stack.size();
        if (v.is_obj()) {
            each_member(v, [&](std::string_view k, Val child) {
                for (size_t i = 0; i < n; ++i)
                    if (k == markers[i]) {
                        if (!fn(markers[i], child)) { stop = true; return false; }
                        return true;
                    }
                for (size_t i = 0; i < np; ++i)
                    if (k == prune[i]) return true;
                if (child.is_obj() || child.is_arr())
                    stack.push_back({child.p, child.e, f.d + 1});
                return true;
            });
        } else if (v.is_arr()) {
            each_elem(v, [&](Val child) {
                if (child.is_obj() || child.is_arr())
                    stack.push_back({child.p, child.e, f.d + 1});
                return true;
            });
        }
        // Reverse the batch we just pushed so DFS visits it in document order.
        for (size_t i = base, j = stack.size(); i + 1 <= j && i < --j; ++i) {
            Finder::Frame t = stack[i]; stack[i] = stack[j]; stack[j] = t;
        }
    }
}

// ---------------------------------------------------------------------------
// Unescaping — the only place that allocates, and only for strings you keep.
// Handles \" \\ \/ \b \f \n \r \t \uXXXX (incl. surrogate pairs) -> UTF-8.
// Also drops control bytes and invalid UTF-8, replacing the old
// sanitize_utf8() pass (one pass instead of two).
// ---------------------------------------------------------------------------

inline void utf8_put(std::string& o, uint32_t cp) {
    if (cp < 0x80) { if (cp >= 32 || cp == '\t' || cp == '\n') o += (char)cp; }
    else if (cp < 0x800) {
        o += (char)(0xC0 | (cp >> 6));
        o += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += (char)(0xE0 | (cp >> 12));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    } else {
        o += (char)(0xF0 | (cp >> 18));
        o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    }
}

inline uint32_t hex4(const char* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        char c = p[i]; uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return 0xFFFFFFFFu;
        v = (v << 4) | d;
    }
    return v;
}

inline void unescape_to(std::string_view in, std::string& o) {
    o.clear();
    o.reserve(in.size());
    const char* p = in.data();
    const char* e = p + in.size();
    while (p < e) {
        // Fast path: bulk-copy the run up to the next backslash.
        const char* bs = (const char*)memchr(p, '\\', (size_t)(e - p));
        const char* stop = bs ? bs : e;
        for (const char* q = p; q < stop; ++q) {
            unsigned char c = (unsigned char)*q;
            if (c < 0x80) { if (c >= 32 || c == '\t' || c == '\n') o += (char)c; }
            else o += (char)c;   // pass multibyte through
        }
        if (!bs) return;
        p = bs + 1;
        if (p >= e) return;
        char c = *p++;
        switch (c) {
        case 'n': o += '\n'; break;
        case 't': o += '\t'; break;
        case 'r': break;                 // CR is noise in a terminal UI
        case 'b': case 'f': break;
        case 'u': {
            if (p + 4 > e) return;
            uint32_t cp = hex4(p);
            if (cp == 0xFFFFFFFFu) { p += 4; break; }
            p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= e &&
                p[0] == '\\' && p[1] == 'u') {
                uint32_t lo = hex4(p + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            utf8_put(o, cp);
            break;
        }
        default: o += c; break;          // \" \\ \/ and anything else literal
        }
    }
}

inline std::string unescape(std::string_view in) {
    std::string o;
    unescape_to(in, o);
    return o;
}

// True if the slice has no escapes — lets callers skip the copy entirely.
inline bool clean(std::string_view s) {
    return memchr(s.data(), '\\', s.size()) == nullptr;
}

inline Val parse(const char* data, size_t len) {
    const char* e = data + len;
    return Val{skip_ws(data, e), e};
}
inline Val parse(std::string_view s) { return parse(s.data(), s.size()); }

} // namespace yj
