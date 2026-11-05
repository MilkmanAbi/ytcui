#pragma once
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

namespace ytui {
// Replace only after a complete, durable write in the destination directory.
inline bool atomic_write(const std::string& path, const std::string& data,
                         std::string* error = nullptr) {
    std::vector<char> temp(path.begin(), path.end());
    const std::string suffix = ".tmp.XXXXXX";
    temp.insert(temp.end(), suffix.begin(), suffix.end());
    temp.push_back('\0');
    int fd = ::mkstemp(temp.data());
    if (fd < 0) { if (error) *error = std::strerror(errno); return false; }
    bool ok = true;
    size_t offset = 0;
    while (offset < data.size()) {
        ssize_t n = ::write(fd, data.data() + offset, data.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        offset += static_cast<size_t>(n);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    int saved_errno = errno;
    if (::close(fd) != 0 && ok) { ok = false; saved_errno = errno; }
    if (ok && ::rename(temp.data(), path.c_str()) != 0) {
        ok = false; saved_errno = errno;
    }
    if (!ok) {
        ::unlink(temp.data());
        if (error) *error = std::strerror(saved_errno);
    }
    return ok;
}
} // namespace ytui
