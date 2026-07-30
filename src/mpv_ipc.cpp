#include "mpv_ipc.h"
#include "log.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

namespace ytui {

MpvIPC::MpvIPC() {}
MpvIPC::~MpvIPC() { disconnect(); cleanup(); }

std::string MpvIPC::init_socket() {
    const char* rd = std::getenv("XDG_RUNTIME_DIR");
    if (rd && rd[0])
        path_ = std::string(rd) + "/ytcui-mpv-" + std::to_string(getpid()) + ".sock";
    else
        path_ = "/tmp/ytcui-mpv-" + std::to_string(getpid()) + ".sock";
    unlink(path_.c_str());
    return path_;
}

// One non-blocking connect attempt. The caller retries across frames, so we
// never sit in a sleep loop that would freeze the UI.
bool MpvIPC::try_connect() {
    if (fd_ >= 0) return true;
    if (path_.empty()) return false;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);          // socket not there yet — mpv still starting
        return false;
    }
    // Non-blocking reads; writes stay blocking but are tiny and rare.
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    fd_ = fd;
    rbuf_.clear();
    stall_frames_ = 0;
    request_observers();
    Log::write("[ipc] connected %s", path_.c_str());
    return true;
}

void MpvIPC::disconnect() {
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
}

void MpvIPC::cleanup() {
    if (!path_.empty()) unlink(path_.c_str());
}

bool MpvIPC::send_raw(const std::string& data) {
    if (fd_ < 0) return false;
    std::string msg = data;
    msg += '\n';
    // Best-effort write. If it would block or errors, drop the connection;
    // the caller keeps working via the SIGSTOP fallback / last-known cache.
    ssize_t n = write(fd_, msg.c_str(), msg.size());
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true; // rare; skip
        disconnect();
        return false;
    }
    return true;
}

// Ask mpv to push these properties whenever they change. That way the UI never
// has to do a synchronous get_property (which would block waiting for a reply).
void MpvIPC::request_observers() {
    send_raw(R"({"command":["observe_property",1,"time-pos"]})");
    send_raw(R"({"command":["observe_property",2,"duration"]})");
    send_raw(R"({"command":["observe_property",3,"volume"]})");
    send_raw(R"({"command":["observe_property",4,"pause"]})");
}

// Extract "name": VALUE from a flat mpv JSON line. Minimal, allocation-light.
static bool json_field(const std::string& s, const char* key, std::string& out) {
    std::string pat = std::string("\"") + key + "\":";
    auto p = s.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    while (p < s.size() && s[p] == ' ') p++;
    auto e = s.find_first_of(",}", p);
    if (e == std::string::npos) e = s.size();
    out = s.substr(p, e - p);
    // strip quotes
    if (out.size() >= 2 && out.front() == '"' && out.back() == '"')
        out = out.substr(1, out.size() - 2);
    return true;
}

void MpvIPC::handle_line(const std::string& line) {
    if (line.find("\"event\"") == std::string::npos) return;
    if (line.find("property-change") == std::string::npos) return;

    std::string name, data;
    if (!json_field(line, "name", name)) return;
    bool has_data = json_field(line, "data", data);

    if (name == "time-pos") {
        pos_ = has_data ? atof(data.c_str()) : -1.0;
    } else if (name == "duration") {
        dur_ = has_data ? atof(data.c_str()) : -1.0;
    } else if (name == "volume") {
        if (has_data) vol_ = (int)(atof(data.c_str()) + 0.5);
    } else if (name == "pause") {
        paused_ = (data == "true");
    }
}

// Drain everything pending without blocking, feed complete lines to handle_line.
void MpvIPC::pump() {
    if (fd_ < 0) return;

    // Safety net: if we're connected but no time-pos has arrived after a while,
    // the observe registration may have raced mpv's startup. Re-request once.
    if (pos_ < 0.0) {
        if (++stall_frames_ == 30) request_observers();
    } else {
        stall_frames_ = 0;
    }

    char buf[4096];
    for (;;) {
        ssize_t n = read(fd_, buf, sizeof(buf));
        if (n > 0) {
            rbuf_.append(buf, (size_t)n);
            size_t nl;
            while ((nl = rbuf_.find('\n')) != std::string::npos) {
                handle_line(rbuf_.substr(0, nl));
                rbuf_.erase(0, nl + 1);
            }
            continue;               // maybe more waiting
        }
        if (n == 0) { disconnect(); return; }   // mpv closed the socket
        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // nothing left
        if (errno == EINTR) continue;
        disconnect();
        return;
    }
}

bool MpvIPC::toggle_pause() { return send_raw(R"({"command":["cycle","pause"]})"); }

bool MpvIPC::set_pause(bool state) {
    return send_raw(std::string(R"({"command":["set_property","pause",)")
                    + (state ? "true" : "false") + "]}");
}

bool MpvIPC::set_volume(int vol) {
    vol = std::max(0, std::min(150, vol));
    return send_raw(std::string(R"({"command":["set_property","volume",)")
                    + std::to_string(vol) + "]}");
}

bool MpvIPC::adjust_volume(int delta) {
    return send_raw(std::string(R"({"command":["add","volume",)")
                    + std::to_string(delta) + "]}");
}

bool MpvIPC::seek(double seconds) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), R"({"command":["seek",%.1f,"relative"]})", seconds);
    return send_raw(cmd);
}

bool MpvIPC::quit() { return send_raw(R"({"command":["quit"]})"); }

} // namespace ytui
