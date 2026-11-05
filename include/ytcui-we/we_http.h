#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <curl/curl.h>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace ytwe {
class Http {
public:
  inline static std::string default_proxy;
  inline static long ip_version = CURL_IPRESOLVE_WHATEVER;
  struct Response {
    long status = 0;
    std::string body, content_type, content_range;
    int64_t content_length = -1;
  };
  Http() {
    static const int init = []() {
      if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        throw std::runtime_error("curl initialization failed");
      return 1;
    }();
    (void)init;
    handle_ = curl_easy_init();
    if (!handle_)
      throw std::runtime_error("curl allocation failed");
  }
  ~Http() { curl_easy_cleanup(handle_); }
  Http(const Http &) = delete;
  Http &operator=(const Http &) = delete;
  void set_timeouts(int connect, int read) {
    connect_ = connect;
    timeout_ = read;
  }
  void set_proxy(std::string proxy) { proxy_ = std::move(proxy); }
  Response get(const std::string &url,
               const std::vector<std::string> &headers = {}) {
    return request(url, nullptr, headers, {});
  }
  Response post(const std::string &url, const std::string &body,
                const std::vector<std::string> &headers = {}) {
    return request(url, &body, headers, {});
  }
  Response stream(const std::string &url,
                  const std::vector<std::string> &headers,
                  const std::function<bool(const char *, size_t)> &sink) {
    return request(url, nullptr, headers, sink);
  }
  static std::string url_encode(std::string_view s) {
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
          c == '~')
        o += c;
      else {
        o += '%';
        o += hex[c >> 4];
        o += hex[c & 15];
      }
    }
    return o;
  }

private:
  Response request(const std::string &url, const std::string *body,
                   const std::vector<std::string> &headers,
                   const std::function<bool(const char *, size_t)> &sink) {
    Response response;
    curl_easy_reset(handle_);
    curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(connect_));
    curl_easy_setopt(handle_, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_));
    curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle_, CURLOPT_MAXREDIRS, 5L);
#ifdef _WIN32
    curl_easy_setopt(handle_, CURLOPT_SSL_OPTIONS,
                     static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif
    curl_easy_setopt(handle_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle_, CURLOPT_ACCEPT_ENCODING, "");
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(handle_, CURLOPT_PROTOCOLS_STR, "https,http");
    curl_easy_setopt(handle_, CURLOPT_REDIR_PROTOCOLS_STR, "https,http");
#else
    curl_easy_setopt(handle_, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS | CURLPROTO_HTTP);
    curl_easy_setopt(handle_, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS | CURLPROTO_HTTP);
#endif
    curl_easy_setopt(handle_, CURLOPT_USERAGENT, "Mozilla/5.0 ytcui-we/0.2");
    curl_easy_setopt(handle_, CURLOPT_IPRESOLVE, ip_version);
    const auto &proxy = proxy_.empty() ? default_proxy : proxy_;
    if (!proxy.empty())
      curl_easy_setopt(handle_, CURLOPT_PROXY, proxy.c_str());
    struct Headers {
      curl_slist *p = nullptr;
      ~Headers() { curl_slist_free_all(p); }
    } list;
    bool content_type = false, ranged = false;
    for (auto &h : headers) {
      list.p = curl_slist_append(list.p, h.c_str());
      if (h.rfind("Content-Type:", 0) == 0)
        content_type = true;
      if (h.rfind("Range:", 0) == 0)
        ranged = true;
    }
    if (body && !content_type)
      list.p = curl_slist_append(list.p, "Content-Type: application/json");
    curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, list.p);
    if (body) {
      curl_easy_setopt(handle_, CURLOPT_POST, 1L);
      curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, body->data());
      curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE_LARGE,
                       static_cast<curl_off_t>(body->size()));
    }
    struct Sink {
      Response *response;
      const std::function<bool(const char *, size_t)> *fn;
      bool failed = false;
      size_t max_body = 64 * 1024 * 1024;
    } state{&response, &sink};
    if (ranged)
      state.max_body = 2 * 1024 * 1024;
    curl_easy_setopt(
        handle_, CURLOPT_WRITEFUNCTION,
        +[](char *p, size_t n, size_t m, void *v) -> size_t {
          auto &s = *static_cast<Sink *>(v);
          size_t len = n * m;
          try {
            if (*s.fn) {
              if (s.response->status >= 400)
                return len;
              if (!(*s.fn)(p, len))
                return 0;
            } else {
              if (s.response->body.size() + len > s.max_body) {
                s.failed = true;
                return 0;
              }
              s.response->body.append(p, len);
            }
            return len;
          } catch (...) {
            s.failed = true;
            return 0;
          }
        });
    curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(
        handle_, CURLOPT_HEADERFUNCTION,
        +[](char *p, size_t n, size_t m, void *v) -> size_t {
          auto &r = *static_cast<Response *>(v);
          std::string line(p, n * m);
          if (line.rfind("HTTP/", 0) == 0) {
            auto space = line.find(' ');
            if (space != std::string::npos)
              r.status = std::strtol(line.c_str() + space + 1, nullptr, 10);
            r.content_range.clear();
          }
          auto colon = line.find(':');
          if (colon != std::string::npos) {
            auto key = line.substr(0, colon);
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) {
                             return static_cast<char>(std::tolower(c));
                           });
            auto value = line.substr(colon + 1);
            while (!value.empty() &&
                   std::isspace(static_cast<unsigned char>(value.front())))
              value.erase(0, 1);
            while (!value.empty() &&
                   std::isspace(static_cast<unsigned char>(value.back())))
              value.pop_back();
            if (key == "content-range")
              r.content_range = value;
          }
          return n * m;
        });
    curl_easy_setopt(handle_, CURLOPT_HEADERDATA, &response);
    auto code = curl_easy_perform(handle_);
    curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &response.status);
    char *type = nullptr;
    curl_easy_getinfo(handle_, CURLINFO_CONTENT_TYPE, &type);
    if (type)
      response.content_type = type;
    curl_off_t length = -1;
    curl_easy_getinfo(handle_, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
    response.content_length = length;
    if (ranged && state.failed && response.status == 200 && !sink) {
      response.body.clear();
      return response;
    }
    if (code != CURLE_OK)
      throw std::runtime_error("HTTP transport: " +
                               std::string(curl_easy_strerror(code)));
    return response;
  }
  CURL *handle_;
  int connect_ = 10000, timeout_ = 30000;
  std::string proxy_;
};
} // namespace ytwe
