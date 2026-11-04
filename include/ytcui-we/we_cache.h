#pragma once
#include "we_platform.h"
#include <atomic>
#include <openssl/evp.h>
namespace ytwe {
inline std::string digest(std::string_view value) {
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned n = 0;
  if (!EVP_Digest(value.data(), value.size(), bytes, &n, EVP_sha256(), nullptr))
    throw std::runtime_error("SHA256 failed");
  std::string out;
  for (unsigned i = 0; i < n; ++i) {
    out += "0123456789abcdef"[bytes[i] >> 4];
    out += "0123456789abcdef"[bytes[i] & 15];
  }
  return out;
}
class Cache {
public:
  bool enabled = true;
  std::filesystem::path directory = []() {
    if (auto e = std::getenv("YTCUI_CACHE_DIR"))
      return std::filesystem::path(e);
#ifdef _WIN32
    if (auto e = std::getenv("LOCALAPPDATA"))
      return std::filesystem::path(e) / "ytcui-we/cache";
#else
    if (auto e = std::getenv("XDG_CACHE_HOME"))
      return std::filesystem::path(e) / "ytcui-we";
    if (auto e = std::getenv("HOME"))
      return std::filesystem::path(e) / ".cache/ytcui-we";
#endif
    return std::filesystem::temp_directory_path() / "ytcui-we-cache";
  }();
  std::string get(const std::string &key, int age = 604800) const {
    if (!enabled)
      return {};
    try {
      auto p = directory / digest(key);
      if (!std::filesystem::exists(p) ||
          std::filesystem::file_size(p) > 16 * 1024 * 1024)
        return {};
      auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                         std::filesystem::file_time_type::clock::now() -
                         std::filesystem::last_write_time(p))
                         .count();
      if (seconds < 0 || seconds > age)
        return {};
      std::ifstream f(p, std::ios::binary);
      return {std::istreambuf_iterator<char>(f),
              std::istreambuf_iterator<char>()};
    } catch (...) {
      return {};
    }
  }
  void put(const std::string &key, const std::string &value) const {
    if (!enabled || value.empty() || value.size() > 16 * 1024 * 1024)
      return;
    try {
      std::filesystem::create_directories(directory);
      static std::atomic<unsigned> seq{0};
      auto p = directory / digest(key);
      auto temp = p;
      temp += ".tmp-" +
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()) +
              "-" + std::to_string(++seq);
      {
        std::ofstream f(temp, std::ios::binary);
        f.write(value.data(), value.size());
        if (!f)
          return;
      }
#ifdef _WIN32
      if (!MoveFileExW(temp.c_str(), p.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        std::filesystem::remove(temp);
#else
      std::filesystem::rename(temp, p);
#endif
    } catch (...) {
    }
  }
};
} // namespace ytwe
