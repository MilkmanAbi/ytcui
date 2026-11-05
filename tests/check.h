#pragma once
// Minimal test harness. No framework: these tests must build and run on the
// BSDs in CI with nothing installed beyond a compiler.

#include <cstdio>
#include <string>

namespace tst {

inline int failures = 0;

inline void report(const char* file, int line, const std::string& what) {
    std::printf("FAIL %s:%d  %s\n", file, line, what.c_str());
    ++failures;
}

inline int finish(const char* name) {
    std::printf("\n%s: %s (%d failure%s)\n", name,
                failures ? "FAILED" : "ALL PASS", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

}  // namespace tst

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) tst::report(__FILE__, __LINE__, #cond);                   \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        auto va_ = (a);                                                        \
        auto vb_ = (b);                                                        \
        if (!(va_ == vb_))                                                     \
            tst::report(__FILE__, __LINE__,                                    \
                        std::string(#a " == " #b "  (got: ") +                 \
                            tst::show(va_) + ", want: " + tst::show(vb_) + ")");\
    } while (0)

namespace tst {
inline std::string show(const std::string& s) { return "\"" + s + "\""; }
inline std::string show(const char* s) { return std::string("\"") + (s ? s : "") + "\""; }
inline std::string show(int v) { return std::to_string(v); }
inline std::string show(long v) { return std::to_string(v); }
inline std::string show(unsigned long v) { return std::to_string(v); }
inline std::string show(bool v) { return v ? "true" : "false"; }
}  // namespace tst
