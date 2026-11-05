#pragma once
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif
namespace ytwe {
#ifdef _WIN32
inline std::wstring wide(const std::string &s) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                              static_cast<int>(s.size()), nullptr, 0);
  if (!n && !s.empty())
    throw std::runtime_error("invalid UTF-8 argument");
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      w.data(), n);
  return w;
}
inline std::string utf8(const std::wstring &w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                              nullptr, 0, nullptr, nullptr);
  std::string s(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                      s.data(), n, nullptr, nullptr);
  return s;
}
inline std::wstring windows_quote(const std::wstring &s) {
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (wchar_t c : s) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    out.append(slashes * (c == L'"' ? 2 : 1), L'\\');
    slashes = 0;
    if (c == L'"')
      out += L'\\';
    out += c;
  }
  out.append(slashes * 2, L'\\');
  return out + L'"';
}
#endif
struct ProcessResult {
  int code = 0;
  std::string output;
};
// Files, rather than bounded pipes, avoid producer/consumer deadlocks. No shell
// ever interprets media URLs, output names, runtime paths or player JavaScript.
inline ProcessResult process(const std::vector<std::string> &args,
                             const std::string *input = nullptr,
                             bool capture = false, int timeout_ms = 0) {
  if (args.empty())
    throw std::runtime_error("empty process command");
  struct File {
    FILE *f = nullptr;
    ~File() {
      if (f)
        std::fclose(f);
    }
  } in, out;
  if (input) {
    in.f = std::tmpfile();
    if (!in.f ||
        std::fwrite(input->data(), 1, input->size(), in.f) != input->size())
      throw std::runtime_error("cannot prepare process input");
    std::fflush(in.f);
    std::rewind(in.f);
  }
  if (capture) {
    out.f = std::tmpfile();
    if (!out.f)
      throw std::runtime_error("cannot prepare process output");
  }
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  ProcessResult result;
  bool timed_out = false;
#ifdef _WIN32
  std::wstring cmd;
  for (auto &arg : args) {
    if (!cmd.empty())
      cmd += L' ';
    cmd += windows_quote(wide(arg));
  }
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  std::vector<HANDLE> owned;
  auto inherited = [&](HANDLE handle) {
    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
                         &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
      throw std::runtime_error("cannot inherit process handle");
    owned.push_back(copy);
    return copy;
  };
  si.hStdInput =
      inherited(in.f ? reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(in.f)))
                     : GetStdHandle(STD_INPUT_HANDLE));
  si.hStdOutput =
      inherited(out.f ? reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(out.f)))
                      : GetStdHandle(STD_OUTPUT_HANDLE));
  si.hStdError = inherited(GetStdHandle(STD_ERROR_HANDLE));
  PROCESS_INFORMATION pi{};
  BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                capture ? CREATE_NO_WINDOW : 0, nullptr,
                                nullptr, &si, &pi);
  for (auto h : owned)
    CloseHandle(h);
  if (!started)
    throw std::runtime_error("cannot launch " + args[0] + " (Windows error " +
                             std::to_string(GetLastError()) + ")");
  CloseHandle(pi.hThread);
  while (WaitForSingleObject(pi.hProcess, 20) == WAIT_TIMEOUT) {
    if (timeout_ms && std::chrono::steady_clock::now() >= deadline) {
      timed_out = true;
      TerminateProcess(pi.hProcess, 124);
      break;
    }
    if (out.f && _filelengthi64(_fileno(out.f)) > 32 * 1024 * 1024) {
      timed_out = true;
      TerminateProcess(pi.hProcess, 124);
      break;
    }
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  result.code = static_cast<int>(code);
#else
  std::vector<char *> argv;
  for (auto &arg : args)
    argv.push_back(const_cast<char *>(arg.c_str()));
  argv.push_back(nullptr);
  pid_t pid = fork();
  if (pid < 0)
    throw std::runtime_error("cannot fork process");
  if (!pid) {
    if (in.f)
      dup2(fileno(in.f), STDIN_FILENO);
    if (out.f)
      dup2(fileno(out.f), STDOUT_FILENO);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  int status = 0;
  while (true) {
    auto done = waitpid(pid, &status, WNOHANG);
    if (done == pid)
      break;
    if (done < 0 && errno != EINTR)
      throw std::runtime_error("cannot wait for child process");
    if (timeout_ms && std::chrono::steady_clock::now() >= deadline) {
      timed_out = true;
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  result.code =
      WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
  if (timed_out)
    throw std::runtime_error(args[0] + " exceeded process time/output limit");
  if (out.f) {
    std::rewind(out.f);
    char b[16384];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, out.f))) {
      result.output.append(b, n);
      if (result.output.size() > 32 * 1024 * 1024)
        throw std::runtime_error("process output exceeds limit");
    }
  }
  return result;
}
inline std::filesystem::path executable_path(const char *argv0) {
#ifdef _WIN32
  (void)argv0;
  std::wstring p(32768, 0);
  auto n = GetModuleFileNameW(nullptr, p.data(), static_cast<DWORD>(p.size()));
  p.resize(n);
  return p;
#else
  std::error_code ec;
#ifdef __APPLE__
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) == 0)
    return std::filesystem::weakly_canonical(buffer.data(), ec);
#endif
  auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec)
    return p;
  if (std::filesystem::path(argv0).has_parent_path())
    return std::filesystem::absolute(argv0);
  std::string paths = std::getenv("PATH") ? std::getenv("PATH") : "";
  for (size_t start = 0; start <= paths.size();) {
    auto end = paths.find(':', start);
    auto directory = paths.substr(start, end - start);
    auto candidate = std::filesystem::path(directory.empty() ? "." : directory) / argv0;
    if (access(candidate.c_str(), X_OK) == 0)
      return std::filesystem::weakly_canonical(candidate, ec);
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return std::filesystem::absolute(argv0);
#endif
}
} // namespace ytwe
