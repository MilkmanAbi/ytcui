#include "core/url.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <vector>

namespace ytcui::url {
namespace {

bool id_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

std::string lower(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return o;
}

// Strip the trailing junk that comes with a pasted link: quotes, brackets,
// and sentence punctuation. A trailing '/' is kept, since it is meaningful
// in a path, and handled by the path splitter instead.
std::string_view trim_edges(std::string_view s) {
    const std::string_view lead = " \t\r\n\"'<([{";
    const std::string_view trail = " \t\r\n\"'>)]},.;!?";
    while (!s.empty() && lead.find(s.front()) != std::string_view::npos)
        s.remove_prefix(1);
    while (!s.empty() && trail.find(s.back()) != std::string_view::npos)
        s.remove_suffix(1);
    return s;
}

struct Split {
    std::string host;
    std::string path;
    std::map<std::string, std::string> query;
};

std::string percent_decode(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            std::isxdigit((unsigned char)s[i + 1]) &&
            std::isxdigit((unsigned char)s[i + 2])) {
            o += (char)std::strtol(std::string(s.substr(i + 1, 2)).c_str(), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            o += ' ';
        } else {
            o += s[i];
        }
    }
    return o;
}

Split split_url(std::string_view u) {
    Split out;
    // Scheme, if any.
    if (const auto p = u.find("://"); p != std::string_view::npos) u.remove_prefix(p + 3);
    // Strip credentials and fragment.
    if (const auto at = u.find('@'); at != std::string_view::npos &&
        at < u.find_first_of("/?")) u.remove_prefix(at + 1);
    if (const auto h = u.find('#'); h != std::string_view::npos) u = u.substr(0, h);

    std::string_view qs;
    if (const auto q = u.find('?'); q != std::string_view::npos) {
        qs = u.substr(q + 1);
        u = u.substr(0, q);
    }
    const auto slash = u.find('/');
    if (slash == std::string_view::npos) {
        out.host = lower(u);
    } else {
        out.host = lower(u.substr(0, slash));
        out.path = std::string(u.substr(slash));
    }
    // Drop a port, if present.
    if (const auto colon = out.host.find(':'); colon != std::string::npos)
        out.host = out.host.substr(0, colon);

    while (!qs.empty()) {
        const auto amp = qs.find('&');
        std::string_view pair = amp == std::string_view::npos ? qs : qs.substr(0, amp);
        if (const auto eq = pair.find('='); eq != std::string_view::npos)
            out.query[std::string(pair.substr(0, eq))] =
                percent_decode(pair.substr(eq + 1));
        else if (!pair.empty())
            out.query[std::string(pair)] = "";
        if (amp == std::string_view::npos) break;
        qs.remove_prefix(amp + 1);
    }
    return out;
}

bool youtube_host(const std::string& h) {
    // Covers youtube.com, www./m./music. subdomains, youtu.be, and the
    // no-cookie embed domain. A suffix match would wrongly accept
    // "notyoutube.com", so each is checked as a whole label boundary.
    static const char* hosts[] = {
        "youtube.com", "youtu.be", "youtube-nocookie.com",
    };
    for (const char* base : hosts) {
        const std::string b = base;
        if (h == b) return true;
        if (h.size() > b.size() + 1 &&
            h.compare(h.size() - b.size(), b.size(), b) == 0 &&
            h[h.size() - b.size() - 1] == '.')
            return true;
    }
    return false;
}

std::vector<std::string> path_parts(const std::string& path) {
    std::vector<std::string> parts;
    std::size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        const std::size_t start = i;
        while (i < path.size() && path[i] != '/') ++i;
        if (i > start) parts.push_back(path.substr(start, i - start));
    }
    return parts;
}

}  // namespace

bool is_video_id(std::string_view s) {
    if (s.size() != 11) return false;
    return std::all_of(s.begin(), s.end(), id_char);
}

int parse_timestamp(std::string_view v) {
    if (v.empty()) return -1;

    // Colon form: mm:ss or hh:mm:ss.
    if (v.find(':') != std::string_view::npos) {
        int parts[3] = {0, 0, 0};
        int n = 0;
        std::size_t i = 0;
        while (i <= v.size() && n < 3) {
            std::size_t j = i;
            while (j < v.size() && std::isdigit((unsigned char)v[j])) ++j;
            if (j == i) return -1;
            parts[n++] = std::atoi(std::string(v.substr(i, j - i)).c_str());
            if (j >= v.size()) break;
            if (v[j] != ':') return -1;
            i = j + 1;
        }
        if (n == 2) return parts[0] * 60 + parts[1];
        if (n == 3) return parts[0] * 3600 + parts[1] * 60 + parts[2];
        return -1;
    }

    // Suffixed form: 1h2m3s, 90s, or a bare number of seconds.
    int total = 0, cur = 0;
    bool any_digit = false, any_unit = false;
    for (char c : v) {
        if (std::isdigit((unsigned char)c)) {
            cur = cur * 10 + (c - '0');
            any_digit = true;
            continue;
        }
        const char u = (char)std::tolower((unsigned char)c);
        if (u == 'h')      { total += cur * 3600; cur = 0; any_unit = true; }
        else if (u == 'm') { total += cur * 60;   cur = 0; any_unit = true; }
        else if (u == 's') { total += cur;        cur = 0; any_unit = true; }
        else return -1;
    }
    if (!any_digit) return -1;
    total += cur;                       // trailing bare number = seconds
    (void)any_unit;
    return total;
}

bool looks_like_url(std::string_view text) {
    const std::string_view t = trim_edges(text);
    if (t.find(' ') != std::string_view::npos) return false;
    if (t.rfind("http://", 0) == 0 || t.rfind("https://", 0) == 0) return true;
    // Bare host form, as people paste it: youtu.be/xyz, www.youtube.com/...
    const Split s = split_url(t);
    return youtube_host(s.host);
}

std::string extract_first_url(std::string_view text) {
    for (const char* scheme : {"https://", "http://"}) {
        const auto p = text.find(scheme);
        if (p == std::string_view::npos) continue;
        std::string_view rest = text.substr(p);
        const auto end = rest.find_first_of(" \t\r\n");
        if (end != std::string_view::npos) rest = rest.substr(0, end);
        return std::string(trim_edges(rest));
    }
    return {};
}

Parsed parse(std::string_view raw) {
    Parsed out;
    std::string_view text = trim_edges(raw);
    if (text.empty()) return out;

    // A bare id, typed or pasted on its own.
    if (is_video_id(text)) {
        out.kind = Kind::Video;
        out.video_id = std::string(text);
        return out;
    }

    // A URL buried in a sentence.
    if (text.find(' ') != std::string_view::npos) {
        const std::string found = extract_first_url(text);
        if (found.empty()) return out;
        return parse(found);
    }

    const Split s = split_url(text);
    if (!youtube_host(s.host)) return out;

    const std::vector<std::string> parts = path_parts(s.path);

    auto read_start = [&] {
        for (const char* key : {"t", "start", "time_continue"}) {
            const auto it = s.query.find(key);
            if (it == s.query.end()) continue;
            const int secs = parse_timestamp(it->second);
            if (secs >= 0) { out.start_seconds = secs; out.has_start = true; return; }
        }
    };

    if (const auto it = s.query.find("list"); it != s.query.end() && !it->second.empty())
        out.playlist_id = it->second;

    // youtu.be/<id> — the whole path is the id.
    const bool short_host = s.host == "youtu.be" ||
                            (s.host.size() > 8 &&
                             s.host.compare(s.host.size() - 8, 8, "youtu.be") == 0);
    if (short_host && !parts.empty() && is_video_id(parts[0])) {
        out.kind = Kind::Video;
        out.video_id = parts[0];
        read_start();
        return out;
    }

    // /watch?v=<id>
    if (!parts.empty() && parts[0] == "watch") {
        const auto it = s.query.find("v");
        if (it != s.query.end() && is_video_id(it->second)) {
            out.kind = Kind::Video;
            out.video_id = it->second;
            read_start();
            return out;
        }
    }

    // /shorts/<id>, /embed/<id>, /live/<id>, /v/<id>
    if (parts.size() >= 2 &&
        (parts[0] == "shorts" || parts[0] == "embed" || parts[0] == "live" ||
         parts[0] == "v") &&
        is_video_id(parts[1])) {
        out.kind = Kind::Video;
        out.video_id = parts[1];
        read_start();
        return out;
    }

    // /playlist?list=...
    if (!parts.empty() && parts[0] == "playlist" && !out.playlist_id.empty()) {
        out.kind = Kind::Playlist;
        return out;
    }

    // Channels: /@handle, /channel/UC..., /c/Name, /user/Name
    if (!parts.empty()) {
        if (parts[0].size() > 1 && parts[0][0] == '@') {
            out.kind = Kind::Channel;
            out.channel = parts[0];
            return out;
        }
        if (parts.size() >= 2 &&
            (parts[0] == "channel" || parts[0] == "c" || parts[0] == "user")) {
            out.kind = Kind::Channel;
            out.channel = parts[1];
            return out;
        }
    }

    // A list= with no recognised path still means a playlist.
    if (!out.playlist_id.empty()) {
        out.kind = Kind::Playlist;
        return out;
    }
    return out;
}

std::string watch_url(const std::string& video_id) {
    return "https://www.youtube.com/watch?v=" + video_id;
}
std::string short_url(const std::string& video_id) {
    // The short form is what goes in a QR code: fewer characters means a
    // lower QR version, which means a code that still scans at terminal size.
    return "https://youtu.be/" + video_id;
}
std::string playlist_url(const std::string& list_id) {
    return "https://www.youtube.com/playlist?list=" + list_id;
}
std::string channel_url(const std::string& channel) {
    if (!channel.empty() && channel[0] == '@')
        return "https://www.youtube.com/" + channel;
    if (channel.rfind("UC", 0) == 0)
        return "https://www.youtube.com/channel/" + channel;
    return "https://www.youtube.com/c/" + channel;
}

}  // namespace ytcui::url
