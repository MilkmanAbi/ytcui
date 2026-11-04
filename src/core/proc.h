#pragma once
// Running a child process and capturing its output.
//
// Needed because the yt-dlp backend is a subprocess. popen() would be shorter
// but is wrong here in two ways that matter: it runs the command through a
// shell, so a video title or a search query becomes a quoting and injection
// problem, and it has no timeout, so a yt-dlp that hangs on a dead network
// hangs ytcui with it.
//
// So: fork/exec with an argv vector -- no shell, nothing to quote -- and a
// deadline after which the child is killed.

#include <string>
#include <vector>

namespace ytcui::proc {

struct Result {
    bool        ran = false;      // the binary was found and executed
    int         exit_code = -1;
    bool        timed_out = false;
    std::string out;              // stdout
    std::string err;              // stderr, kept for error messages

    bool ok() const { return ran && !timed_out && exit_code == 0; }
};

// Run argv[0] with argv, capturing stdout and stderr. Nothing is passed
// through a shell. `timeout_ms` of 0 waits indefinitely.
//
// `max_output` caps how much is retained: a yt-dlp JSON dump for a long
// playlist can be very large, and an unbounded read is a memory hazard driven
// by remote data.
Result run(const std::vector<std::string>& argv, int timeout_ms = 30000,
           std::size_t max_output = 32u * 1024 * 1024);

// Is this on PATH? Uses execvp's own lookup rather than a shell.
bool exists(const std::string& program);

// Absolute path of a program on PATH, or empty. mpv's ytdl_hook needs a real
// path, not a bare name.
std::string which(const std::string& program);

// Every copy on PATH, in PATH order, deduplicated.
//
// This exists because PATH order is not quality order. A machine can easily
// carry a distro yt-dlp from 2024 in /usr/bin and a current one in
// ~/.local/bin, and which one you get depends on how the shell was started --
// measured on the development machine, where /usr/bin held 2024.04.09 and
// ~/.local/bin held 2026.10.26. Picking the first match means playback
// silently fails for a reason that has nothing to do with ytcui.
std::vector<std::string> which_all(const std::string& program);

// Absolute path of the running executable itself, or empty on failure.
// Used to find companion binaries (e.g. ytcui-we) installed alongside ytcui.
// Portable: Linux uses /proc/self/exe, macOS uses _NSGetExecutablePath,
// FreeBSD uses sysctl KERN_PROC_PATHNAME, NetBSD uses /proc/curproc/exe,
// OpenBSD returns "" (no /proc by default; PATH-based discovery is the
// fallback on that platform).
std::string self_exe();

}  // namespace ytcui::proc
