#pragma once
#include <string_view>
#include <initializer_list>
namespace ytfast {
inline bool youtube_cookie_domain(std::string_view host) {
    if (!host.empty() && host.front() == '.') host.remove_prefix(1);
    for (std::string_view base : {"youtube.com", "google.com"}) {
        if (host == base) return true;
        if (host.size() > base.size() && host[host.size() - base.size() - 1] == '.' &&
            host.substr(host.size() - base.size()) == base) return true;
    }
    return false;
}
inline bool cookie_field_safe(std::string_view field) {
    return field.find_first_of("\r\n\t") == std::string_view::npos &&
           field.find('\0') == std::string_view::npos;
}
} // namespace ytfast
