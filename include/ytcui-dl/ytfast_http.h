#pragma once
/*
 * ytcui-dl — ytfast_http.h
 * Persistent HTTP client via libcurl. One handle per instance → TCP keep-alive.
 * Cross-platform: Linux, macOS, FreeBSD, OpenBSD, NetBSD.
 */

#include <string>
#include <vector>
#include <stdexcept>
#include <curl/curl.h>

namespace ytfast {

class HttpClient {
public:
    struct Response {
        long        status = 0;
        std::string body;
        std::string content_type;
    };

    HttpClient() {
        curl_ = curl_easy_init();
        if (!curl_) throw std::runtime_error("curl_easy_init() failed");
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE,  1L);
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPIDLE,   30L);
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPINTVL,  15L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT,        30L);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl_, CURLOPT_MAXREDIRS,      5L);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION,  write_cb);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA,      &buf_);
        curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING,"gzip, deflate");
        // Don't verify SSL on BSD where certs may be in non-standard locations
        // (can be overridden by the caller)
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
    }

    ~HttpClient() { if (curl_) curl_easy_cleanup(curl_); }

    // Non-copyable (owns curl handle)
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    Response post(const std::string& url,
                  const std::string& json_body,
                  const std::vector<std::string>& extra_headers = {}) {
        buf_.clear();
        curl_easy_setopt(curl_, CURLOPT_URL,            url.c_str());
        curl_easy_setopt(curl_, CURLOPT_POST,           1L);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS,     json_body.c_str());
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE,  (long)json_body.size());
        auto* hdrs = build_headers(extra_headers);
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, hdrs);
        CURLcode rc = curl_easy_perform(curl_);
        curl_slist_free_all(hdrs);
        return finish(rc);
    }

    Response get(const std::string& url,
                 const std::vector<std::string>& extra_headers = {}) {
        buf_.clear();
        curl_easy_setopt(curl_, CURLOPT_URL,    url.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
        auto* hdrs = build_headers(extra_headers);
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, hdrs);
        CURLcode rc = curl_easy_perform(curl_);
        curl_slist_free_all(hdrs);
        return finish(rc);
    }

    static std::string url_encode(const std::string& s) {
        CURL* tmp = curl_easy_init();
        char* enc = curl_easy_escape(tmp, s.c_str(), (int)s.size());
        std::string r(enc);
        curl_free(enc);
        curl_easy_cleanup(tmp);
        return r;
    }

private:
    CURL*       curl_ = nullptr;
    std::string buf_;

    static size_t write_cb(char* ptr, size_t sz, size_t n, void* ud) {
        static_cast<std::string*>(ud)->append(ptr, sz*n);
        return sz*n;
    }

    Response finish(CURLcode rc) {
        if (rc != CURLE_OK)
            throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(rc));
        Response resp;
        resp.body = buf_;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &resp.status);
        char* ct = nullptr;
        curl_easy_getinfo(curl_, CURLINFO_CONTENT_TYPE, &ct);
        if (ct) resp.content_type = ct;
        return resp;
    }

    struct curl_slist* build_headers(const std::vector<std::string>& extra) {
        struct curl_slist* h = nullptr;
        h = curl_slist_append(h, "Content-Type: application/json");
        h = curl_slist_append(h, "Accept-Language: en-US,en;q=0.9");
        for (auto& s : extra)
            h = curl_slist_append(h, s.c_str());
        return h;
    }
};

// RAII global init — instantiate once at program start
struct CurlGlobalInit {
    CurlGlobalInit()  { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobalInit() { curl_global_cleanup(); }
};


// Thread-local HttpClient factory — ensures each thread has its own curl handle.
// Solves the race condition when prefetch threads and main thread both call 
// the singleton's http_ simultaneously.
inline HttpClient& get_thread_http() {
    thread_local HttpClient tl_http;
    return tl_http;
}

} // namespace ytfast
