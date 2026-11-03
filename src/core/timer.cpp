#include "core/timer.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ytcui {
namespace {

int64_t now_s() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::seconds>(
               clock::now().time_since_epoch()).count();
}

// Which unit a word names, or 0.
int unit_seconds(const std::string& w) {
    if (w.empty()) return 0;
    // Longest first: "m" must not swallow the "m" of "min" before "minute"
    // has been considered, and "s" must not match the "s" in "hours".
    if (w == "h" || w == "hr" || w == "hrs" || w == "hour" || w == "hours")
        return 3600;
    if (w == "m" || w == "min" || w == "mins" || w == "minute" || w == "minutes")
        return 60;
    if (w == "s" || w == "sec" || w == "secs" || w == "second" || w == "seconds")
        return 1;
    return 0;
}

}  // namespace

int parse_duration(std::string_view text) {
    std::string s;
    for (char c : text) s += (char)std::tolower((unsigned char)c);

    // Trim.
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    s = s.substr(b, e - b);
    if (s.empty()) return -1;

    // h:mm form. For a sleep timer "1:30" is an hour and a half.
    if (const auto colon = s.find(':'); colon != std::string::npos) {
        const std::string h = s.substr(0, colon);
        const std::string m = s.substr(colon + 1);
        if (h.empty() || m.empty()) return -1;
        for (char c : h) if (!std::isdigit((unsigned char)c)) return -1;
        for (char c : m) if (!std::isdigit((unsigned char)c)) return -1;
        const int mm = std::atoi(m.c_str());
        if (mm > 59) return -1;
        return std::atoi(h.c_str()) * 3600 + mm * 60;
    }

    int total = 0;
    bool any = false;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (std::isspace((unsigned char)s[i]) || s[i] == ',')) ++i;
        if (i >= s.size()) break;

        if (!std::isdigit((unsigned char)s[i])) return -1;   // "abc", "h"
        int value = 0;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) {
            value = value * 10 + (s[i] - '0');
            ++i;
            if (value > 100000) return -1;                   // absurd
        }

        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;

        std::string word;
        while (i < s.size() && std::isalpha((unsigned char)s[i])) word += s[i++];

        if (word.empty()) {
            // A bare number. Minutes if it is the whole input; otherwise the
            // input is malformed ("1h 20" is not a duration anyone means).
            if (any) return -1;
            // Allow trailing whitespace only.
            while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
            if (i != s.size()) return -1;
            return value * 60;
        }

        const int mult = unit_seconds(word);
        if (mult == 0) return -1;                            // "23 potatoes"
        total += value * mult;
        any = true;
    }

    if (!any) return -1;
    return total;
}

std::string format_duration(int seconds) {
    if (seconds <= 0) return "0s";
    const int h = seconds / 3600;
    const int m = (seconds % 3600) / 60;
    const int s = seconds % 60;
    std::string out;
    char buf[32];
    if (h > 0) {
        std::snprintf(buf, sizeof buf, "%dh", h);
        out += buf;
        if (m > 0) {
            std::snprintf(buf, sizeof buf, " %dm", m);
            out += buf;
        }
        return out;
    }
    if (m > 0) {
        std::snprintf(buf, sizeof buf, "%dm", m);
        out += buf;
        // Seconds alongside minutes only matter near the end of a countdown.
        if (s > 0 && m < 5) {
            std::snprintf(buf, sizeof buf, " %ds", s);
            out += buf;
        }
        return out;
    }
    std::snprintf(buf, sizeof buf, "%ds", s);
    return buf;
}

// ---------------------------------------------------------------------------
bool SleepTimer::set(int seconds) {
    if (seconds <= 0) return false;
    total_ = seconds;
    deadline_ = now_s() + seconds;
    active_ = true;
    fired_ = false;
    return true;
}

void SleepTimer::cancel() {
    active_ = false;
    fired_ = false;
    total_ = 0;
    deadline_ = 0;
}

int SleepTimer::remaining() const {
    if (!active_ || fired_) return 0;
    const int64_t left = deadline_ - now_s();
    return left > 0 ? (int)left : 0;
}

bool SleepTimer::fire_if_due() {
    if (!active_ || fired_) return false;
    if (now_s() < deadline_) return false;
    fired_ = true;
    return true;
}

std::string SleepTimer::describe() const {
    if (!active_) return {};
    if (fired_) return "sleep timer fired";
    return "sleep in " + format_duration(remaining());
}

}  // namespace ytcui
