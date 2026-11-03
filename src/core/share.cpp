#include "core/share.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include <sys/wait.h>
#include <fcntl.h>

#include "qrcodegen.hpp"

namespace ytcui::share {
namespace {

bool have_cmd(const char* name) {
    const std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

// Pipe `text` into a command's stdin. Used for every external clipboard tool;
// none of them take the payload as an argument, and passing a URL on a command
// line would be a quoting bug waiting to happen.
bool pipe_to(const char* argv0, const std::vector<const char*>& args,
             const std::string& text) {
    int fds[2];
    if (::pipe(fds) != 0) return false;

    const pid_t pid = ::fork();
    if (pid < 0) { ::close(fds[0]); ::close(fds[1]); return false; }
    if (pid == 0) {
        ::close(fds[1]);
        ::dup2(fds[0], STDIN_FILENO);
        ::close(fds[0]);
        const int null = ::open("/dev/null", O_WRONLY);
        if (null >= 0) { ::dup2(null, STDOUT_FILENO); ::dup2(null, STDERR_FILENO); }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(argv0));
        for (const char* a : args) argv.push_back(const_cast<char*>(a));
        argv.push_back(nullptr);
        ::execvp(argv0, argv.data());
        ::_exit(127);
    }
    ::close(fds[0]);
    const ssize_t written = ::write(fds[1], text.data(), text.size());
    ::close(fds[1]);

    int st = 0;
    ::waitpid(pid, &st, 0);
    return written == (ssize_t)text.size() && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

std::string base64(const std::string& in) {
    static const char* t =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < in.size()) {
        const unsigned v = ((unsigned char)in[i] << 16) |
                           ((unsigned char)in[i + 1] << 8) |
                           (unsigned char)in[i + 2];
        out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];  out += t[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        const unsigned v = (unsigned char)in[i] << 16;
        out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = ((unsigned char)in[i] << 16) |
                           ((unsigned char)in[i + 1] << 8);
        out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];  out += '=';
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
bool QrMatrix::at(int x, int y) const {
    if (x < 0 || y < 0 || x >= size || y >= size) return false;
    return modules[(std::size_t)y * size + x];
}

bool QrMatrix::at_padded(int x, int y) const {
    return at(x - quiet, y - quiet);
}

QrMatrix encode(const std::string& text) {
    QrMatrix out;
    try {
        // Medium correction: a terminal QR is usually scanned off a clean
        // screen rather than crumpled paper, and a lower version matters more
        // than extra redundancy when every module is half a character cell.
        const qrcodegen::QrCode qr =
            qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        out.size = qr.getSize();
        out.modules.resize((std::size_t)out.size * out.size);
        for (int y = 0; y < out.size; ++y)
            for (int x = 0; x < out.size; ++x)
                out.modules[(std::size_t)y * out.size + x] = qr.getModule(x, y);
    } catch (...) {
        // Too long for any version, or a bad encode. Reported as size == 0.
        out.size = 0;
        out.modules.clear();
    }
    return out;
}

std::vector<std::string> to_ascii(const QrMatrix& m) {
    std::vector<std::string> rows;
    if (m.size == 0) return rows;
    const int n = m.full_size();
    for (int y = 0; y < n; ++y) {
        std::string row;
        row.reserve((std::size_t)n);
        for (int x = 0; x < n; ++x) row += m.at_padded(x, y) ? 'X' : ' ';
        rows.push_back(std::move(row));
    }
    return rows;
}

bool fits_in(const std::string& text, int cols, int rows) {
    const QrMatrix m = encode(text);
    if (m.size == 0) return false;
    const int n = m.full_size();
    // Half-blocks put two module rows in one text row; each module is one
    // column wide, so a module is half as tall as it is wide. Acceptable for
    // scanning, and it is what makes an 80x24 terminal viable at all.
    return n <= cols && (n + 1) / 2 <= rows;
}

// ---------------------------------------------------------------------------
const char* to_string(CopyMethod m) {
    switch (m) {
        case CopyMethod::Osc52:  return "OSC 52";
        case CopyMethod::WlCopy: return "wl-copy";
        case CopyMethod::Xclip:  return "xclip";
        case CopyMethod::Xsel:   return "xsel";
        case CopyMethod::PbCopy: return "pbcopy";
        case CopyMethod::Termux: return "termux-clipboard-set";
        case CopyMethod::Clip:   return "clip.exe";
        case CopyMethod::None:
        default:                 return "none";
    }
}

std::string osc52_sequence(const std::string& text) {
    // ESC ] 52 ; c ; <base64> BEL
    return "\033]52;c;" + base64(text) + "\a";
}

std::vector<std::string> available_tools() {
    std::vector<std::string> out;
    for (const char* t : {"wl-copy", "xclip", "xsel", "pbcopy",
                          "termux-clipboard-set", "clip.exe"})
        if (have_cmd(t)) out.push_back(t);
    return out;
}

CopyResult copy(const std::string& text, bool allow_osc52) {
    CopyResult r;

    if (allow_osc52 && ::isatty(STDOUT_FILENO)) {
        const std::string seq = osc52_sequence(text);
        if (::write(STDOUT_FILENO, seq.data(), seq.size()) == (ssize_t)seq.size()) {
            // There is no reply to an OSC 52 write, so success here means
            // "the terminal was told", not "the clipboard definitely changed".
            // Said plainly in the detail string rather than claimed outright.
            r.ok = true;
            r.method = CopyMethod::Osc52;
            r.detail = "sent to the terminal via OSC 52";
            return r;
        }
    }

#ifdef YTCUI_MACOS
    if (have_cmd("pbcopy") && pipe_to("pbcopy", {}, text)) {
        r.ok = true; r.method = CopyMethod::PbCopy; r.detail = "pbcopy";
        return r;
    }
#endif
    // Wayland before X11: an Xwayland-backed xclip can appear to work and then
    // not be readable by native Wayland apps.
    if (std::getenv("WAYLAND_DISPLAY") && have_cmd("wl-copy") &&
        pipe_to("wl-copy", {}, text)) {
        r.ok = true; r.method = CopyMethod::WlCopy; r.detail = "wl-copy";
        return r;
    }
    if (std::getenv("DISPLAY")) {
        if (have_cmd("xclip") &&
            pipe_to("xclip", {"-selection", "clipboard"}, text)) {
            r.ok = true; r.method = CopyMethod::Xclip; r.detail = "xclip";
            return r;
        }
        if (have_cmd("xsel") && pipe_to("xsel", {"--clipboard", "--input"}, text)) {
            r.ok = true; r.method = CopyMethod::Xsel; r.detail = "xsel";
            return r;
        }
    }
    if (have_cmd("termux-clipboard-set") &&
        pipe_to("termux-clipboard-set", {}, text)) {
        r.ok = true; r.method = CopyMethod::Termux; r.detail = "termux";
        return r;
    }
    // WSL: the Windows clipboard is the one the user can actually paste from.
    if (have_cmd("clip.exe") && pipe_to("clip.exe", {}, text)) {
        r.ok = true; r.method = CopyMethod::Clip; r.detail = "clip.exe";
        return r;
    }

    r.detail = "no clipboard method available "
               "(tried OSC 52, wl-copy, xclip, xsel, pbcopy)";
    return r;
}

// ---------------------------------------------------------------------------
bool open_in_browser(const std::string& url) {
    const char* openers[] = {
#ifdef YTCUI_MACOS
        "open",
#endif
        "xdg-open", "gio", "x-www-browser", "sensible-browser", "www-browser",
    };
    for (const char* o : openers) {
        if (!have_cmd(o)) continue;
        const pid_t pid = ::fork();
        if (pid < 0) return false;
        if (pid == 0) {
            // Double-fork so the browser is reparented to init rather than
            // becoming a zombie child of a TUI that may outlive it.
            if (::fork() == 0) {
                ::setsid();
                const int null = ::open("/dev/null", O_RDWR);
                if (null >= 0) {
                    ::dup2(null, STDOUT_FILENO);
                    ::dup2(null, STDERR_FILENO);
                }
                if (std::string(o) == "gio")
                    ::execlp("gio", "gio", "open", url.c_str(), (char*)nullptr);
                else
                    ::execlp(o, o, url.c_str(), (char*)nullptr);
                ::_exit(127);
            }
            ::_exit(0);
        }
        int st = 0;
        ::waitpid(pid, &st, 0);
        return true;
    }
    return false;
}

}  // namespace ytcui::share
