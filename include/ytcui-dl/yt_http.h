#pragma once
/*
 * ytcui-dl — yt_http.h
 *
 * Minimal HTTP/1.1 client over OpenSSL. Replaces libcurl.
 *
 * libcurl is a general-purpose transfer library: it links krb5, ldap, rtmp,
 * ssh2, sasl, nghttp2, brotli, zstd, psl and more, none of which matter when
 * the entire workload is "POST JSON to one host, occasionally GET a file".
 * Dependencies here are libssl, libcrypto, libz, libpthread.
 *
 * Implemented: TLS 1.2+ with SNI and real certificate + hostname verification,
 * connection reuse keyed by host:port, Content-Length and chunked bodies,
 * gzip/deflate, redirects, connect/read timeouts, HEAD, and ranged GET.
 *
 * Not implemented, deliberately: HTTP/2, proxies, cookies, auth, multipart.
 */

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <zlib.h>

namespace ytfast {

// ---------------------------------------------------------------------------
// URL splitting
// ---------------------------------------------------------------------------
struct Url {
    std::string host, port, path;
    bool tls = true;

    static bool parse(std::string_view u, Url& out) {
        if (u.rfind("https://", 0) == 0)      { out.tls = true;  u.remove_prefix(8); }
        else if (u.rfind("http://", 0) == 0)  { out.tls = false; u.remove_prefix(7); }
        else return false;

        size_t slash = u.find('/');
        std::string_view auth = slash == std::string_view::npos ? u : u.substr(0, slash);
        out.path = slash == std::string_view::npos ? "/" : std::string(u.substr(slash));

        size_t colon = auth.rfind(':');
        // Guard against IPv6 literals like [::1] having no port.
        if (colon != std::string_view::npos && auth.find(']', colon) == std::string_view::npos) {
            out.host = std::string(auth.substr(0, colon));
            out.port = std::string(auth.substr(colon + 1));
        } else {
            out.host = std::string(auth);
            out.port = out.tls ? "443" : "80";
        }
        if (out.host.size() > 1 && out.host.front() == '[' && out.host.back() == ']')
            out.host = out.host.substr(1, out.host.size() - 2);
        return !out.host.empty();
    }
};

// ---------------------------------------------------------------------------
// Address family selection.
//
// YouTube signs each media URL to the IP that requested it, and the CDN
// rejects a fetch from any other address. On a dual-stack host the player
// request and the media fetch can independently pick IPv4 or IPv6, so the two
// disagree and every stream 403s while the API itself looks perfectly healthy.
// Pinning one family makes both requests take the same path. This is why
// yt-dlp ships -4/-6, and it is the first thing to try against an otherwise
// inexplicable 403.
// ---------------------------------------------------------------------------
enum class IpFamily { Any, V4, V6 };

inline IpFamily& ip_family() {
    static IpFamily f = IpFamily::Any;
    return f;
}
inline void set_ip_family(IpFamily f) { ip_family() = f; }

// ---------------------------------------------------------------------------
// TLS session cache
// ---------------------------------------------------------------------------
namespace detail {

// Cached TLS sessions, keyed by host:port. A resumed handshake skips the
// key exchange and certificate verification round trip, which is the most
// expensive single thing a short-lived process does.
//
// Capture goes through SSL_CTX_sess_set_new_cb rather than a
// SSL_get1_session() call after SSL_connect(). Under TLS 1.3 the session
// ticket is delivered in a NewSessionTicket message *after* the handshake
// finishes -- often only once application data has flowed -- so a session
// grabbed at connect time has no ticket in it and can never be resumed.
// The callback fires when the ticket actually lands.
struct SessionStore {
    std::mutex mu;
    std::unordered_map<std::string, SSL_SESSION*> map;

    static SessionStore& get() { static SessionStore s; return s; }

    // on_new_session returns 1, which means we took ownership of the session
    // and OpenSSL will not free it. Without this destructor those references
    // are never released -- bounded (one per host) but a real leak, and
    // LeakSanitizer reports it against SSL_read where the ticket arrived.
    //
    // Destruction order is safe: TlsCtx::get() runs during the first
    // Conn::open(), before anything reaches this store, so the SSL_CTX static
    // is constructed first and therefore destroyed last. Sessions are released
    // while libssl and the context are still alive.
    ~SessionStore() { clear(); }

    void clear() {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& kv : map) if (kv.second) SSL_SESSION_free(kv.second);
        map.clear();
    }

    SessionStore() = default;
    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;

    void put(const std::string& key, SSL_SESSION* sess) {
        std::lock_guard<std::mutex> lk(mu);
        SSL_SESSION*& slot = map[key];
        if (slot) SSL_SESSION_free(slot);
        slot = sess;                       // takes ownership
    }
    // Returns a reference the caller must free.
    SSL_SESSION* take(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = map.find(key);
        if (it == map.end() || !it->second) return nullptr;
        SSL_SESSION_up_ref(it->second);
        return it->second;
    }
};

// The key is stashed on the SSL object so the callback knows which host
// the arriving ticket belongs to.
inline int session_index() {
    static int idx = SSL_get_ex_new_index(0, (void*)"ytfast-host", nullptr, nullptr, nullptr);
    return idx;
}

inline int on_new_session(SSL* ssl, SSL_SESSION* sess) {
    if (const char* key = (const char*)SSL_get_ex_data(ssl, session_index()))
        SessionStore::get().put(key, sess);
    else
        return 0;                          // not ours: let OpenSSL free it
    return 1;                              // we took ownership
}

} // namespace detail

// ---------------------------------------------------------------------------
// Process-wide TLS context. One shared SSL_CTX; OpenSSL 1.1+ is thread-safe
// for concurrent use of a single context.
// ---------------------------------------------------------------------------
class TlsCtx {
public:
    static SSL_CTX* get() {
        static TlsCtx inst;
        return inst.ctx_;
    }
private:
    TlsCtx() {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) throw std::runtime_error("SSL_CTX_new failed");
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        SSL_CTX_set_options(ctx_, SSL_OP_NO_COMPRESSION);
        SSL_CTX_set_mode(ctx_, SSL_MODE_AUTO_RETRY);
        if (!SSL_CTX_set_default_verify_paths(ctx_)) {
            // Fall back to the common BSD//etc locations before giving up.
            static const char* kBundles[] = {
                "/etc/ssl/certs/ca-certificates.crt",
                "/etc/pki/tls/certs/ca-bundle.crt",
                "/etc/ssl/cert.pem",
                "/usr/local/share/certs/ca-root-nss.crt",
            };
            for (const char* p : kBundles)
                if (SSL_CTX_load_verify_locations(ctx_, p, nullptr)) break;
        }
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        // SSL_SESS_CACHE_NO_INTERNAL_STORE: we keep sessions ourselves, keyed
        // by host, so OpenSSL's own client cache would just duplicate them.
        SSL_CTX_set_session_cache_mode(
            ctx_, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        SSL_CTX_sess_set_new_cb(ctx_, &detail::on_new_session);
    }
    ~TlsCtx() { if (ctx_) SSL_CTX_free(ctx_); }
    SSL_CTX* ctx_ = nullptr;
};

// ---------------------------------------------------------------------------
// One TCP+TLS connection
// ---------------------------------------------------------------------------
class Conn {
public:
    Conn() = default;
    ~Conn() { close_(); }
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    bool open(const std::string& host, const std::string& port, bool tls,
              int connect_ms) {
        close_();
        addrinfo hints{}, *res = nullptr;
        switch (ip_family()) {
            case IpFamily::V4: hints.ai_family = AF_INET;  break;
            case IpFamily::V6: hints.ai_family = AF_INET6; break;
            default:           hints.ai_family = AF_UNSPEC; break;
        }
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) return false;

        for (addrinfo* a = res; a; a = a->ai_next) {
            fd_ = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd_ < 0) continue;
            set_nonblock(true);
            int rc = ::connect(fd_, a->ai_addr, a->ai_addrlen);
            if (rc != 0) {
                if (errno != EINPROGRESS) { ::close(fd_); fd_ = -1; continue; }
                pollfd p{fd_, POLLOUT, 0};
                if (::poll(&p, 1, connect_ms) <= 0) { ::close(fd_); fd_ = -1; continue; }
                int err = 0; socklen_t el = sizeof err;
                getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &el);
                if (err) { ::close(fd_); fd_ = -1; continue; }
            }
            set_nonblock(false);
            int one = 1;
            setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY,  &one, sizeof one);
            setsockopt(fd_, SOL_SOCKET,  SO_KEEPALIVE, &one, sizeof one);
            break;
        }
        freeaddrinfo(res);
        if (fd_ < 0) return false;

        if (tls) {
            ssl_ = SSL_new(TlsCtx::get());
            if (!ssl_) { close_(); return false; }
            SSL_set_fd(ssl_, fd_);
            SSL_set_tlsext_host_name(ssl_, host.c_str());        // SNI
            SSL_set1_host(ssl_, host.c_str());                   // hostname check

            session_key_ = host + ":" + port;
            SSL_set_ex_data(ssl_, detail::session_index(), (void*)session_key_.c_str());

            if (SSL_SESSION* cached = detail::SessionStore::get().take(session_key_)) {
                SSL_set_session(ssl_, cached);
                SSL_SESSION_free(cached);      // set_session takes its own ref
            }

            if (SSL_connect(ssl_) != 1) { close_(); return false; }
            if (SSL_get_verify_result(ssl_) != X509_V_OK) { close_(); return false; }
            resumed_ = SSL_session_reused(ssl_) != 0;
        }
        host_ = host; port_ = port; tls_ = tls;
        return true;
    }

    bool alive() const { return fd_ >= 0; }
    bool tls_resumed() const { return resumed_; }
    const std::string& host() const { return host_; }
    const std::string& port() const { return port_; }

    void set_timeout(int ms) {
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }

    bool write_all(const char* p, size_t n) {
        while (n) {
            int w = ssl_ ? SSL_write(ssl_, p, (int)n)
                         : (int)::send(fd_, p, n, MSG_NOSIGNAL);
            if (w <= 0) return false;
            p += w; n -= (size_t)w;
        }
        return true;
    }

    // >0 bytes, 0 clean EOF, <0 error
    int read_some(char* p, size_t n) {
        int r = ssl_ ? SSL_read(ssl_, p, (int)n) : (int)::recv(fd_, p, n, 0);
        if (r > 0) return r;
        if (ssl_) {
            int e = SSL_get_error(ssl_, r);
            if (e == SSL_ERROR_ZERO_RETURN) return 0;
            return -1;
        }
        return r == 0 ? 0 : -1;
    }

    void close_() {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        host_.clear(); port_.clear();
    }

private:
    void set_nonblock(bool on) {
        int fl = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
    }
    int         fd_  = -1;
    SSL*        ssl_ = nullptr;
    bool        resumed_ = false;
    std::string session_key_;   // must outlive ssl_: pointed at by ex_data
    bool        tls_ = true;
    std::string host_, port_;
};

// ---------------------------------------------------------------------------
// gzip / deflate
// ---------------------------------------------------------------------------
inline bool gunzip(std::string_view in, std::string& out) {
    if (in.empty()) return true;
    z_stream zs{};
    // 15 window bits + 32 = auto-detect zlib or gzip wrapper.
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return false;
    zs.next_in  = (Bytef*)in.data();
    zs.avail_in = (uInt)in.size();

    out.clear();
    out.reserve(in.size() * 4);
    char buf[65536];
    int rc;
    do {
        zs.next_out  = (Bytef*)buf;
        zs.avail_out = sizeof buf;
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            inflateEnd(&zs);
            return false;
        }
        out.append(buf, sizeof buf - zs.avail_out);
        if (rc == Z_BUF_ERROR && zs.avail_in == 0) break;
    } while (rc != Z_STREAM_END);
    inflateEnd(&zs);
    return true;
}

// ---------------------------------------------------------------------------
// HttpClient — same surface as the libcurl version it replaces
// ---------------------------------------------------------------------------
class HttpClient {
public:
    struct Response {
        long        status = 0;
        std::string body;
        std::string content_type;
    };

    HttpClient() = default;

    Response post(const std::string& url, const std::string& json_body,
                  const std::vector<std::string>& extra_headers = {}) {
        return request("POST", url, &json_body, extra_headers, kMaxRedirects);
    }

    Response get(const std::string& url,
                 const std::vector<std::string>& extra_headers = {}) {
        return request("GET", url, nullptr, extra_headers, kMaxRedirects);
    }

    // Streaming GET. The body is handed to `sink` in chunks and never fully
    // buffered, so a 4K download costs a constant ~64 KB of RAM instead of the
    // file size. `resume_from` issues a Range request for partial files.
    // sink returns false to abort. Returns the HTTP status.
    long download(const std::string& url,
                  const std::function<bool(const char*, size_t)>& sink,
                  const std::function<void(int64_t, int64_t)>& progress = {},
                  int64_t resume_from = 0,
                  const std::vector<std::string>& extra_headers = {}) {
        Url u;
        if (!Url::parse(url, u)) throw std::runtime_error("bad url: " + url);

        for (int redirect = 0; redirect <= kMaxRedirects; ++redirect) {
            if (!conn_.alive() || conn_.host() != u.host || conn_.port() != u.port) {
                if (!conn_.open(u.host, u.port, u.tls, connect_ms_))
                    throw std::runtime_error("connect failed: " + u.host);
            }
            conn_.set_timeout(io_ms_);

            std::vector<std::string> hdrs = extra_headers;
            // identity: we want raw bytes on disk, not a re-inflated stream
            hdrs.emplace_back("Accept-Encoding: identity");
            // The googlevideo CDN 403s a plain unranged GET on these signed
            // URLs -- it only serves them in response to a Range request, even
            // one starting at byte 0. Always send one unless the caller already
            // supplied a more specific Range header of its own.
            bool have_range = false;
            for (const auto& h : hdrs) if (hdr_is(h, "range")) { have_range = true; break; }
            if (!have_range)
                hdrs.emplace_back("Range: bytes=" + std::to_string(resume_from) + "-");

            std::string req = build_request("GET", u, nullptr, hdrs);
            if (!conn_.write_all(req.data(), req.size())) {
                conn_.close_();
                continue;
            }

            long   status = 0;
            int64_t total = -1;
            bool    chunked = false;
            if (!read_head(status, total, chunked)) { conn_.close_(); continue; }

            if (status >= 300 && status < 400 && !location_.empty()) {
                std::string next = resolve(u, location_);
                if (!Url::parse(next, u)) throw std::runtime_error("bad redirect");
                conn_.close_();
                continue;
            }
            if (status != 200 && status != 206) return status;

            int64_t got = 0;
            if (total > 0 && progress) progress(0, total);

            // Whatever of the body arrived alongside the headers
            if (!pending_.empty()) {
                if (!sink(pending_.data(), pending_.size())) return status;
                got += (int64_t)pending_.size();
                pending_.clear();
            }
            char buf[65536];
            while (total < 0 || got < total) {
                int n = conn_.read_some(buf, sizeof buf);
                if (n <= 0) break;
                if (!sink(buf, (size_t)n)) break;
                got += n;
                if (progress) progress(got, total);
            }
            return status;
        }
        throw std::runtime_error("download: too many redirects");
    }

    static std::string url_encode(std::string_view s) {
        static const char* hex = "0123456789ABCDEF";
        std::string o;
        o.reserve(s.size() * 3);
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
            else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
        }
        return o;
    }

    // Total resource size from the last response's Content-Range, or -1.
    int64_t last_range_total() const { return range_total_; }

    // Whether the last connection resumed a cached TLS session.
    bool last_tls_resumed() const { return conn_.tls_resumed(); }

    void set_timeouts(int connect_ms, int io_ms) {
        connect_ms_ = connect_ms;
        io_ms_ = io_ms;
    }

private:
    static constexpr int kMaxRedirects = 5;
    Conn conn_;
    int  connect_ms_ = 8000;
    int  io_ms_      = 30000;
    std::string rbuf_;   // reused across requests, so no per-call allocation

    Response request(const char* method, const std::string& url,
                     const std::string* body,
                     const std::vector<std::string>& extra, int redirects_left) {
        Url u;
        if (!Url::parse(url, u)) throw std::runtime_error("bad url: " + url);

        // Reuse the socket when the target matches, otherwise reconnect.
        if (!conn_.alive() || conn_.host() != u.host || conn_.port() != u.port) {
            if (!conn_.open(u.host, u.port, u.tls, connect_ms_))
                throw std::runtime_error("connect failed: " + u.host);
            conn_.set_timeout(io_ms_);
        }

        std::string req = build_request(method, u, body, extra);

        // One retry: a pooled connection can be closed by the peer between
        // requests, and that surfaces as a write or read failure, not an error.
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (conn_.write_all(req.data(), req.size())) {
                Response r;
                if (read_response(r, std::strcmp(method, "HEAD") == 0)) {
                    if (r.status >= 300 && r.status < 400 && !location_.empty() &&
                        redirects_left > 0) {
                        std::string next = resolve(u, location_);
                        // 303, and 301/302 on POST, become GET per RFC 9110.
                        bool to_get = (r.status == 303) ||
                                      ((r.status == 301 || r.status == 302) &&
                                       std::strcmp(method, "POST") == 0);
                        return request(to_get ? "GET" : method, next,
                                       to_get ? nullptr : body, extra,
                                       redirects_left - 1);
                    }
                    return r;
                }
            }
            conn_.close_();
            if (attempt == 0) {
                if (!conn_.open(u.host, u.port, u.tls, connect_ms_))
                    throw std::runtime_error("reconnect failed: " + u.host);
                conn_.set_timeout(io_ms_);
            }
        }
        throw std::runtime_error("http: request failed");
    }

    std::string build_request(const char* method, const Url& u,
                              const std::string* body,
                              const std::vector<std::string>& extra) {
        std::string r;
        r.reserve(512 + (body ? body->size() : 0));
        r += method; r += ' '; r += u.path; r += " HTTP/1.1\r\n";
        r += "Host: "; r += u.host;
        if (u.port != (u.tls ? "443" : "80")) { r += ':'; r += u.port; }
        r += "\r\n";
        r += "Accept-Language: en-US,en;q=0.9\r\n";
        r += "Connection: keep-alive\r\n";

        bool have_ct = false, have_ua = false, have_enc = false;
        for (const auto& h : extra) {
            if (hdr_is(h, "content-type"))   have_ct = true;
            if (hdr_is(h, "user-agent"))     have_ua = true;
            if (hdr_is(h, "accept-encoding")) have_enc = true;
            r += h; r += "\r\n";
        }
        if (!have_enc)        r += "Accept-Encoding: gzip, deflate\r\n";
        if (body && !have_ct) r += "Content-Type: application/json\r\n";
        if (!have_ua)         r += "User-Agent: ytcui-dl/0.3\r\n";
        if (body) {
            r += "Content-Length: ";
            r += std::to_string(body->size());
            r += "\r\n";
        }
        r += "\r\n";
        if (body) r += *body;
        return r;
    }

    static bool hdr_is(const std::string& h, const char* name) {
        size_t n = std::strlen(name);
        if (h.size() < n + 1 || h[n] != ':') return false;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower((unsigned char)h[i]) != name[i]) return false;
        return true;
    }

    static std::string resolve(const Url& base, const std::string& loc) {
        if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0) return loc;
        std::string s = base.tls ? "https://" : "http://";
        s += base.host;
        if (base.port != (base.tls ? "443" : "80")) { s += ':'; s += base.port; }
        if (loc.empty() || loc[0] != '/') s += '/';
        s += loc;
        return s;
    }

    std::string location_;

    std::string pending_;    // body bytes read while consuming headers
    int64_t     range_total_ = -1;

    // Reads and parses just the status line + headers, leaving any body bytes
    // already in the socket buffer in pending_.
    bool read_head(long& status, int64_t& content_length, bool& chunked) {
        rbuf_.clear();
        location_.clear();
        pending_.clear();
        char tmp[16384];
        size_t hdr_end = std::string::npos;
        while (hdr_end == std::string::npos) {
            int n = conn_.read_some(tmp, sizeof tmp);
            if (n <= 0) return false;
            size_t from = rbuf_.size() >= 3 ? rbuf_.size() - 3 : 0;
            rbuf_.append(tmp, (size_t)n);
            hdr_end = rbuf_.find("\r\n\r\n", from);
            if (rbuf_.size() > (1u << 20)) return false;
        }
        std::string_view head(rbuf_.data(), hdr_end);
        size_t sp = head.find(' ');
        if (sp == std::string_view::npos) return false;
        status = strtol(head.data() + sp + 1, nullptr, 10);
        content_length = -1;
        chunked = false;
        size_t pos = head.find("\r\n");
        while (pos != std::string_view::npos) {
            size_t eol = head.find("\r\n", pos + 2);
            std::string_view line = head.substr(
                pos + 2, (eol == std::string_view::npos ? head.size() : eol) - pos - 2);
            if (line.empty()) break;
            size_t c = line.find(':');
            if (c != std::string_view::npos) {
                std::string_view k = line.substr(0, c), v = line.substr(c + 1);
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
                if      (ieq(k, "content-length"))    content_length = atoll(std::string(v).c_str());
                else if (ieq(k, "transfer-encoding")) chunked = icontains(v, "chunked");
                else if (ieq(k, "location"))          location_ = std::string(v);
                else if (ieq(k, "content-range"))     range_total_ = parse_range_total(v);
            }
            pos = eol;
        }
        pending_.assign(rbuf_, hdr_end + 4, std::string::npos);
        return true;
    }

    // "bytes 0-0/318152" -> 318152. A 206's Content-Length is the length of the
    // range, not of the resource, so this is the only way to learn the real
    // size from a ranged probe.
    static int64_t parse_range_total(std::string_view v) {
        size_t slash = v.rfind('/');
        if (slash == std::string_view::npos) return -1;
        std::string_view t = v.substr(slash + 1);
        if (t.empty() || t[0] == '*') return -1;
        return atoll(std::string(t).c_str());
    }

    bool read_response(Response& out, bool head_only) {
        rbuf_.clear();
        location_.clear();
        char tmp[16384];

        // --- headers ---
        size_t hdr_end = std::string::npos;
        while (hdr_end == std::string::npos) {
            int n = conn_.read_some(tmp, sizeof tmp);
            if (n <= 0) return false;
            size_t search_from = rbuf_.size() >= 3 ? rbuf_.size() - 3 : 0;
            rbuf_.append(tmp, (size_t)n);
            hdr_end = rbuf_.find("\r\n\r\n", search_from);
            if (rbuf_.size() > (1u << 20)) return false;   // runaway headers
        }
        std::string_view head(rbuf_.data(), hdr_end);

        size_t sp = head.find(' ');
        if (sp == std::string_view::npos) return false;
        out.status = strtol(head.data() + sp + 1, nullptr, 10);

        int64_t content_length = -1;
        bool chunked = false, gzipped = false;
        size_t pos = head.find("\r\n");
        while (pos != std::string_view::npos) {
            size_t eol = head.find("\r\n", pos + 2);
            std::string_view line = head.substr(
                pos + 2, (eol == std::string_view::npos ? head.size() : eol) - pos - 2);
            if (line.empty()) break;
            size_t c = line.find(':');
            if (c != std::string_view::npos) {
                std::string_view k = line.substr(0, c);
                std::string_view v = line.substr(c + 1);
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
                    v.remove_prefix(1);
                if      (ieq(k, "content-length"))    content_length = atoll(std::string(v).c_str());
                else if (ieq(k, "transfer-encoding")) chunked = icontains(v, "chunked");
                else if (ieq(k, "content-encoding"))  gzipped = icontains(v, "gzip") ||
                                                                icontains(v, "deflate");
                else if (ieq(k, "content-type"))      out.content_type = std::string(v);
                else if (ieq(k, "location"))          location_ = std::string(v);
                else if (ieq(k, "connection") && icontains(v, "close")) close_after_ = true;
            }
            pos = eol;
        }

        std::string raw(rbuf_, hdr_end + 4);   // body bytes already buffered

        if (head_only || out.status == 204 || out.status == 304) {
            out.body.clear();
            return true;
        }

        if (chunked) {
            if (!read_chunked(raw)) return false;
        } else if (content_length >= 0) {
            while ((int64_t)raw.size() < content_length) {
                int n = conn_.read_some(tmp, sizeof tmp);
                if (n <= 0) break;
                raw.append(tmp, (size_t)n);
            }
            raw.resize(std::min<size_t>(raw.size(), (size_t)content_length));
        } else {
            // No length and no chunking: read until close.
            int n;
            while ((n = conn_.read_some(tmp, sizeof tmp)) > 0) raw.append(tmp, (size_t)n);
            close_after_ = true;
        }

        if (gzipped) {
            if (!gunzip(raw, out.body)) return false;
        } else {
            out.body = std::move(raw);
        }
        if (close_after_) { conn_.close_(); close_after_ = false; }
        return true;
    }

    bool close_after_ = false;

    // `acc` starts with whatever of the body we already read; decode in place.
    bool read_chunked(std::string& acc) {
        std::string out;
        out.reserve(acc.size() * 2 + 4096);
        size_t p = 0;
        char tmp[16384];

        auto need = [&](size_t want) {
            while (acc.size() < want) {
                int n = conn_.read_some(tmp, sizeof tmp);
                if (n <= 0) return false;
                acc.append(tmp, (size_t)n);
            }
            return true;
        };
        auto need_line = [&](size_t from, size_t& eol) {
            for (;;) {
                eol = acc.find("\r\n", from);
                if (eol != std::string::npos) return true;
                int n = conn_.read_some(tmp, sizeof tmp);
                if (n <= 0) return false;
                acc.append(tmp, (size_t)n);
            }
        };

        for (;;) {
            size_t eol;
            if (!need_line(p, eol)) return false;
            size_t sz = (size_t)strtoull(acc.c_str() + p, nullptr, 16);
            p = eol + 2;
            if (sz == 0) break;                       // trailer ignored
            if (!need(p + sz + 2)) return false;
            out.append(acc, p, sz);
            p += sz + 2;                              // skip chunk CRLF
        }
        acc.swap(out);
        return true;
    }

    static bool ieq(std::string_view a, const char* b) {
        size_t n = std::strlen(b);
        if (a.size() != n) return false;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower((unsigned char)a[i]) != b[i]) return false;
        return true;
    }
    static bool icontains(std::string_view h, const char* n) {
        size_t ln = std::strlen(n);
        if (h.size() < ln) return false;
        for (size_t i = 0; i + ln <= h.size(); ++i) {
            size_t j = 0;
            while (j < ln && std::tolower((unsigned char)h[i + j]) == n[j]) ++j;
            if (j == ln) return true;
        }
        return false;
    }
};

// Kept so existing call sites compile. OpenSSL 1.1+ self-initialises, so the
// TLS side of this is now a no-op.
//
// The SIGPIPE guard is not a no-op and is load-bearing: SSL_write() ends up
// calling the underlying write()/send() syscall without MSG_NOSIGNAL (unlike
// this client's own plain-socket send(), which passes it explicitly), so a
// peer that resets the connection mid-write raises SIGPIPE. The default
// disposition for that signal is to terminate the process -- not just fail
// the request -- which is fatal for a downloader that retries aggressively
// against a CDN that closes connections under load. One process-wide ignore
// at startup turns that into an ordinary EPIPE the caller already handles.
struct CurlGlobalInit {
    CurlGlobalInit()  { (void)TlsCtx::get(); std::signal(SIGPIPE, SIG_IGN); }
    ~CurlGlobalInit() = default;
};

inline HttpClient& get_thread_http() {
    thread_local HttpClient tl;
    return tl;
}

} // namespace ytfast
