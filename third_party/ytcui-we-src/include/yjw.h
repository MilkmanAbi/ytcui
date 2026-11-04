#pragma once
/*
 * ytcui-dl — yjw.h
 *
 * Minimal JSON writer. The CLI only ever emits JSON (--dump-json), it never
 * reads it, so pulling in a DOM library for this was 24k lines to build a few
 * flat objects.
 *
 * Append-only, no tree: you drive the structure and it tracks comma placement
 * and indentation. Misuse (unbalanced open/close) produces malformed output
 * rather than throwing; the call sites are static so that's a compile-time
 * concern, not a runtime one.
 */

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace yjw {

class Writer {
public:
    explicit Writer(bool pretty = true) : pretty_(pretty) { s_.reserve(4096); }

    Writer& obj()  { pre(); s_ += '{'; push(); return *this; }
    Writer& arr()  { pre(); s_ += '['; push(); return *this; }
    Writer& end()  { pop(); nl(); s_ += close_.back(); close_.pop_back(); first_ = false; return *this; }

    // object members
    Writer& key(std::string_view k) {
        pre();
        s_ += '"'; esc(k); s_ += '"'; s_ += ':';
        if (pretty_) s_ += ' ';
        pending_key_ = true;
        return *this;
    }
    Writer& val(std::string_view v) { pre(); s_ += '"'; esc(v); s_ += '"'; return *this; }
    Writer& val(const char* v)      { return val(std::string_view(v ? v : "")); }
    Writer& val(const std::string& v) { return val(std::string_view(v)); }
    Writer& val(int64_t v)  { pre(); s_ += std::to_string(v); return *this; }
    Writer& val(int v)      { return val((int64_t)v); }
    Writer& val(bool v)     { pre(); s_ += v ? "true" : "false"; return *this; }
    Writer& val(double v)   { char b[40]; std::snprintf(b, sizeof b, "%.6g", v); pre(); s_ += b; return *this; }
    Writer& null()          { pre(); s_ += "null"; return *this; }

    // key + value in one call
    template <class T> Writer& kv(std::string_view k, T v) { key(k); return val(v); }
    Writer& knull(std::string_view k) { key(k); return null(); }

    const std::string& str() const { return s_; }

private:
    void push() {
        close_.push_back(s_.back() == '{' ? '}' : ']');
        ++depth_;
        first_ = true;
    }
    void pop() { if (depth_) --depth_; }

    void pre() {
        if (pending_key_) { pending_key_ = false; return; }  // value follows a key
        if (!first_) s_ += ',';
        if (depth_) nl();
        first_ = false;
    }
    void nl() {
        if (!pretty_) return;
        s_ += '\n';
        s_.append(depth_ * 2, ' ');
    }
    void esc(std::string_view v) {
        for (char c : v) {
            switch (c) {
                case '"':  s_ += "\\\""; break;
                case '\\': s_ += "\\\\"; break;
                case '\n': s_ += "\\n";  break;
                case '\r': s_ += "\\r";  break;
                case '\t': s_ += "\\t";  break;
                default:
                    if ((unsigned char)c < 0x20) {
                        char b[8];
                        std::snprintf(b, sizeof b, "\\u%04x", c);
                        s_ += b;
                    } else s_ += c;
            }
        }
    }

    std::string s_;
    std::string close_;
    int  depth_ = 0;
    bool first_ = true;
    bool pretty_ = true;
    bool pending_key_ = false;
};

} // namespace yjw
