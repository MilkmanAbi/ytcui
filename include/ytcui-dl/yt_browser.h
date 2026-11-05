#pragma once
#include "yt_cookie_policy.h"
#include <openssl/sha.h>
/*
 * ytcui-dl — yt_browser.h
 *
 * Summon the user's own default browser, then pick the session back up out of
 * whichever browser they signed in with.
 *
 * The design decision worth stating: this does NOT ship or embed a browser.
 * An earlier plan did -- a minimal WebKitGTK window -- and it was wrong twice
 * over. It needs ~50 dev packages to build (so it cannot be installed
 * unattended), and Google routinely refuses sign-in from an unrecognised
 * embedded engine ("this browser or app may not be secure"). The user's real
 * browser has none of those problems: it is already installed, already trusted
 * by Google, and already where they are signed in. So the job here is only to
 * open it at the right URL and afterwards read its cookie store.
 *
 * Supported: Firefox (cookies are plaintext in SQLite) and the Chromium family
 * -- Chrome, Chromium, Brave, Edge, Vivaldi, Opera -- whose cookie values are
 * AES-128-CBC encrypted with a key derived from a per-platform secret.
 * OpenSSL is already a dependency, so the decryption costs nothing new.
 *
 * NOT supported, deliberately: Safari. Its store is a wholly different binary
 * format (Cookies.binarycookies), not SQLite, and it is sandboxed under macOS
 * TCC; pretending otherwise would just produce a confusing failure.
 *
 * Platforms: Linux, macOS, and the BSDs. There is no Windows path here -- the
 * rest of this project is POSIX (fork/exec, signals) and so is this.
 */

#include "yt_sqlite.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/evp.h>

namespace ytfast {
namespace browser {

// ---------------------------------------------------------------------------
// Small filesystem helpers
// ---------------------------------------------------------------------------
inline std::string home() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    return {};
}
inline bool is_file(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
inline bool is_dir(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
inline int64_t mtime_of(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 ? (int64_t)st.st_mtime : 0;
}
inline std::vector<std::string> subdirs(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        if (is_dir(dir + "/" + n)) out.push_back(n);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// Run a command and capture stdout. Used only for the platform secret stores
// (`security` on macOS, `secret-tool` on Linux), which have no C API worth
// linking for one lookup.
inline std::string run_capture(const std::string& cmd) {
    std::string out;
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return out;
    char b[512];
    while (std::fgets(b, sizeof b, p)) out += b;
    ::pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

inline bool have_cmd(const char* name) {
    return !run_capture(std::string("command -v ") + name + " 2>/dev/null").empty();
}

// ---------------------------------------------------------------------------
// Opening the default browser
//
// $BROWSER first, because a user who set it means it. It is a colon-separated
// list and an entry may contain %s where the URL goes (the de-facto convention
// xdg-open itself honours). Then the usual per-platform openers.
// ---------------------------------------------------------------------------
inline std::vector<std::string> browser_openers() {
#ifdef YTFAST_MACOS
    return {"open"};
#else
    return {"xdg-open", "gio", "x-www-browser", "sensible-browser", "www-browser",
            "firefox", "chromium", "google-chrome"};
#endif
}

// What we would run, without running it -- for --print-command and for error
// messages that tell the user exactly what to do by hand.
inline std::vector<std::string> browser_command(const std::string& url) {
    if (const char* b = std::getenv("BROWSER"); b && *b) {
        std::string list(b), entry;
        size_t start = 0;
        while (start <= list.size()) {
            const size_t colon = list.find(':', start);
            entry = list.substr(start, colon == std::string::npos
                                           ? std::string::npos : colon - start);
            if (!entry.empty()) {
                const size_t pct = entry.find("%s");
                if (pct != std::string::npos) {
                    std::string cmd = entry;
                    cmd.replace(pct, 2, url);
                    return {"/bin/sh", "-c", cmd};
                }
                // Bare program name: only use it if it actually exists, so a
                // stale $BROWSER doesn't dead-end the whole flow.
                if (entry.find('/') != std::string::npos ? is_file(entry)
                                                         : have_cmd(entry.c_str()))
                    return {entry, url};
            }
            if (colon == std::string::npos) break;
            start = colon + 1;
        }
    }
    for (const std::string& o : browser_openers()) {
        if (!have_cmd(o.c_str())) continue;
        if (o == "gio") return {"gio", "open", url};
        return {o, url};
    }
    return {};
}

// Launch and do not wait: the point is to hand the user their browser and
// carry on. Double-fork so the browser is reparented to init rather than left
// as a zombie child of a process that is about to exit.
inline bool open_url(const std::string& url) {
    const std::vector<std::string> cmd = browser_command(url);
    if (cmd.empty()) return false;
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (::fork() == 0) {
            // Detach from the terminal so browser chatter doesn't land in the
            // middle of our own output.
            ::setsid();
            const int null = ::open("/dev/null", O_RDWR);
            if (null >= 0) { ::dup2(null, 1); ::dup2(null, 2); }
            std::vector<char*> argv;
            for (const auto& s : cmd) argv.push_back(const_cast<char*>(s.c_str()));
            argv.push_back(nullptr);
            ::execvp(argv[0], argv.data());
            ::_exit(127);
        }
        ::_exit(0);
    }
    int st = 0;
    ::waitpid(pid, &st, 0);
    return true;
}

// ---------------------------------------------------------------------------
// Browser profiles
// ---------------------------------------------------------------------------
enum class Engine { Firefox, Chromium };

struct Profile {
    Engine      engine = Engine::Chromium;
    std::string browser;        // "firefox", "chrome", "brave", ...
    std::string label;          // browser + profile directory, for display
    std::string cookie_db;      // path to the SQLite file
    std::string keyring_app;    // Linux secret-tool lookup value
    std::string keychain_svc;   // macOS "<X> Safe Storage"
    std::string keychain_acct;  // macOS account name
    int64_t     mtime = 0;      // freshest profile wins when auto-picking
};

struct ChromiumFamily {
    const char* name;
    const char* linux_dir;      // under ~/.config
    const char* mac_dir;        // under ~/Library/Application Support
    const char* keyring_app;
    const char* keychain;       // "<X> Safe Storage"; account is the same word
};

// Order matters only for display; auto-pick uses mtime.
static const ChromiumFamily CHROMIUM_FAMILIES[] = {
    {"chrome",   "google-chrome",                   "Google/Chrome",                "chrome",   "Chrome"},
    {"chromium", "chromium",                        "Chromium",                     "chromium", "Chromium"},
    {"brave",    "BraveSoftware/Brave-Browser",     "BraveSoftware/Brave-Browser",  "brave",    "Brave"},
    {"edge",     "microsoft-edge",                  "Microsoft Edge",               "chromium", "Microsoft Edge"},
    {"vivaldi",  "vivaldi",                         "Vivaldi",                      "vivaldi",  "Vivaldi"},
    {"opera",    "opera",                           "com.operasoftware.Opera",      "chromium", "Opera"},
};

inline std::vector<std::string> chromium_roots(const ChromiumFamily& f) {
    const std::string h = home();
    if (h.empty()) return {};
#ifdef YTFAST_MACOS
    return {h + "/Library/Application Support/" + f.mac_dir};
#else
    return {
        h + "/.config/" + f.linux_dir,
        // Flatpak and snap relocate the whole config tree.
        h + "/.var/app/com.google.Chrome/config/" + f.linux_dir,
        h + "/.var/app/com.brave.Browser/config/" + f.linux_dir,
        h + "/.var/app/org.chromium.Chromium/config/" + f.linux_dir,
        h + "/snap/" + f.linux_dir + "/current/.config/" + f.linux_dir,
    };
#endif
}

inline std::vector<std::string> firefox_roots() {
    const std::string h = home();
    if (h.empty()) return {};
#ifdef YTFAST_MACOS
    return {h + "/Library/Application Support/Firefox/Profiles"};
#else
    return {
        h + "/.mozilla/firefox",
        h + "/snap/firefox/common/.mozilla/firefox",
        h + "/.var/app/org.mozilla.firefox/.mozilla/firefox",
    };
#endif
}

inline std::vector<Profile> discover() {
    std::vector<Profile> out;

    for (const std::string& root : firefox_roots()) {
        if (!is_dir(root)) continue;
        for (const std::string& sub : subdirs(root)) {
            const std::string db = root + "/" + sub + "/cookies.sqlite";
            if (!is_file(db)) continue;
            Profile p;
            p.engine  = Engine::Firefox;
            p.browser = "firefox";
            p.label   = "firefox / " + sub;
            p.cookie_db = db;
            p.mtime   = mtime_of(db);
            out.push_back(std::move(p));
        }
    }

    for (const ChromiumFamily& f : CHROMIUM_FAMILIES) {
        for (const std::string& root : chromium_roots(f)) {
            if (!is_dir(root)) continue;
            for (const std::string& sub : subdirs(root)) {
                if (sub != "Default" && sub.rfind("Profile ", 0) != 0) continue;
                // Newer Chromium moved the file under Network/.
                std::string db = root + "/" + sub + "/Network/Cookies";
                if (!is_file(db)) db = root + "/" + sub + "/Cookies";
                if (!is_file(db)) continue;
                Profile p;
                p.engine        = Engine::Chromium;
                p.browser       = f.name;
                p.label         = std::string(f.name) + " / " + sub;
                p.cookie_db     = db;
                p.keyring_app   = f.keyring_app;
                p.keychain_svc  = std::string(f.keychain) + " Safe Storage";
                p.keychain_acct = f.keychain;
                p.mtime         = mtime_of(db);
                out.push_back(std::move(p));
            }
        }
    }

    std::sort(out.begin(), out.end(),
              [](const Profile& a, const Profile& b) { return a.mtime > b.mtime; });
    return out;
}

// ---------------------------------------------------------------------------
// Chromium cookie decryption
// ---------------------------------------------------------------------------
inline std::string chromium_secret(const Profile& p, bool v11) {
#ifdef YTFAST_MACOS
    (void)v11;
    // The Keychain holds a random per-install password. This prompts the user
    // the first time, which is the OS doing its job.
    const std::string cmd = "security find-generic-password -w -s \"" +
                            p.keychain_svc + "\" -a \"" + p.keychain_acct +
                            "\" 2>/dev/null";
    const std::string s = run_capture(cmd);
    return s;
#else
    // v10 on Linux means "no keyring available", and Chromium falls back to a
    // fixed, well-known password. v11 means the key is in the login keyring.
    if (!v11) return "peanuts";
    if (have_cmd("secret-tool")) {
        const std::string s = run_capture(
            "secret-tool lookup application " + p.keyring_app + " 2>/dev/null");
        if (!s.empty()) return s;
    }
    return "";
#endif
}

inline bool mostly_printable(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 && c != '\t') return false;
    return true;
}

// Returns false when the value cannot be recovered (wrong key, unknown
// scheme), which the caller reports rather than silently dropping.
inline bool decrypt_chromium(const std::string& blob, const std::string& secret,
                             std::string& out, const std::string& domain = "",
                             bool domain_bound = false) {
    if (blob.size() < 3 + 16) return false;
    const bool v10 = blob.compare(0, 3, "v10") == 0;
    const bool v11 = blob.compare(0, 3, "v11") == 0;
    if (!v10 && !v11) return false;

#ifdef YTFAST_MACOS
    const int iters = 1003;
#else
    const int iters = 1;
#endif
    unsigned char key[16];
    static const unsigned char salt[] = {'s','a','l','t','y','s','a','l','t'};
    if (PKCS5_PBKDF2_HMAC_SHA1(secret.data(), (int)secret.size(), salt,
                               (int)sizeof salt, iters, (int)sizeof key, key) != 1)
        return false;

    unsigned char iv[16];
    std::memset(iv, ' ', sizeof iv);               // Chromium uses 16 spaces

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    bool ok = false;
    std::string plain;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, key, iv) == 1) {
        EVP_CIPHER_CTX_set_padding(ctx, 0);        // unpad by hand: see below
        const unsigned char* ct = (const unsigned char*)blob.data() + 3;
        const int ctlen = (int)blob.size() - 3;
        if (ctlen > 0 && ctlen % 16 == 0) {
            plain.resize((size_t)ctlen);
            int n1 = 0, n2 = 0;
            if (EVP_DecryptUpdate(ctx, (unsigned char*)plain.data(), &n1, ct, ctlen) == 1 &&
                EVP_DecryptFinal_ex(ctx, (unsigned char*)plain.data() + n1, &n2) == 1) {
                plain.resize((size_t)(n1 + n2));
                ok = true;
            }
        }
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;

    // PKCS#7 by hand, because a wrong key usually yields a bogus pad byte and
    // OpenSSL's own unpadding would just fail the whole decrypt with no way to
    // tell that case apart from a malformed record.
    if (plain.empty()) return false;
    const unsigned char pad = (unsigned char)plain.back();
    if (pad < 1 || pad > 16 || pad > plain.size()) return false;
    for (size_t i = plain.size() - pad; i < plain.size(); ++i)
        if ((unsigned char)plain[i] != pad) return false;
    plain.resize(plain.size() - pad);

    // Chrome 130-ish began prefixing the plaintext with a 32-byte SHA-256 of
    // the cookie's domain. There is no version bump to key off, so detect it:
    // a cookie value is printable, a hash prefix is not.
    if (domain_bound || (!mostly_printable(plain) && plain.size() >= 32)) {
        if (domain.empty() || plain.size() < SHA256_DIGEST_LENGTH) return false;
        unsigned char expected[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(domain.data()), domain.size(), expected);
        if (CRYPTO_memcmp(plain.data(), expected, sizeof expected) != 0) return false;
        plain.erase(0, 32);
    }
    if (!mostly_printable(plain)) return false;

    out = std::move(plain);
    return true;
}

// ---------------------------------------------------------------------------
// Reading a profile's cookies
// ---------------------------------------------------------------------------
struct Cookie {
    std::string domain, path, name, value;
    bool        secure = false, http_only = false;
    int64_t     expiry = 0;     // unix seconds; 0 = session cookie
};

struct ExtractResult {
    bool        ok = false;
    bool        wal_warning = false;
    int         total = 0;          // youtube/google cookies found
    int         undecryptable = 0;  // present but unreadable (locked keyring)
    std::string error;
};

inline bool interesting_domain(const std::string& d) {
    return youtube_cookie_domain(d);
}

inline ExtractResult read_cookies(const Profile& p, std::vector<Cookie>& out) {
    ExtractResult r;
    sqlite::Db db;
    if (!db.open(p.cookie_db)) {
        r.error = "cannot read " + p.cookie_db;
        return r;
    }
    r.wal_warning = db.wal_pending();

    const char* table = p.engine == Engine::Firefox ? "moz_cookies" : "cookies";
    const std::vector<std::string> cols = db.columns(table);
    if (cols.empty()) { r.error = std::string("no ") + table + " table"; return r; }
    auto idx = [&](const char* want) -> int {
        for (size_t i = 0; i < cols.size(); ++i) {
            // Column order differs across browser versions, so everything is
            // looked up by name rather than position.
            if (cols[i] == want) return (int)i;
        }
        return -1;
    };

    std::string secret;
    bool secret_ready = false;
    bool domain_bound = false;
    if (p.engine == Engine::Chromium) {
        db.scan("meta", [&](const std::vector<sqlite::Value>& row) {
            if (row.size() >= 2 && row[0].as_str() == "version")
                domain_bound = row[1].as_int() >= 24;
            return true;
        });
    }

    int i_host, i_name, i_value, i_path, i_exp, i_sec, i_http, i_enc = -1;
    if (p.engine == Engine::Firefox) {
        i_host = idx("host"); i_name = idx("name"); i_value = idx("value");
        i_path = idx("path"); i_exp = idx("expiry");
        i_sec  = idx("isSecure"); i_http = idx("isHttpOnly");
    } else {
        i_host = idx("host_key"); i_name = idx("name"); i_value = idx("value");
        i_path = idx("path"); i_exp = idx("expires_utc");
        i_sec  = idx("is_secure"); i_http = idx("is_httponly");
        i_enc  = idx("encrypted_value");
    }
    if (i_host < 0 || i_name < 0 || i_path < 0) {
        r.error = "unexpected cookie table layout";
        return r;
    }

    db.scan(table, [&](const std::vector<sqlite::Value>& row) {
        auto at = [&](int i) -> const sqlite::Value* {
            return (i >= 0 && (size_t)i < row.size()) ? &row[i] : nullptr;
        };
        const sqlite::Value* h = at(i_host);
        if (!h || !interesting_domain(h->as_str())) return true;

        Cookie c;
        c.domain = h->as_str();
        c.name   = at(i_name) ? at(i_name)->as_str() : "";
        c.path   = at(i_path) ? at(i_path)->as_str() : "/";
        if (c.path.empty()) c.path = "/";
        c.secure    = at(i_sec)  && at(i_sec)->as_int()  != 0;
        c.http_only = at(i_http) && at(i_http)->as_int() != 0;

        if (p.engine == Engine::Firefox) {
            c.value  = at(i_value) ? at(i_value)->as_str() : "";
            c.expiry = at(i_exp) ? at(i_exp)->as_int() : 0;
        } else {
            const sqlite::Value* plain = at(i_value);
            const sqlite::Value* enc   = at(i_enc);
            if (plain && !plain->as_str().empty()) {
                c.value = plain->as_str();
            } else if (enc && !enc->as_str().empty()) {
                if (!secret_ready) {
                    const bool v11 = enc->as_str().compare(0, 3, "v11") == 0;
                    secret = chromium_secret(p, v11);
                    secret_ready = true;
                }
                if (secret.empty()) { ++r.undecryptable; return true; }
                if (!decrypt_chromium(enc->as_str(), secret, c.value, c.domain, domain_bound)) {
                    ++r.undecryptable;
                    return true;
                }
            } else {
                return true;                    // nothing to read
            }
            // Chromium stores microseconds since 1601-01-01.
            const int64_t us = at(i_exp) ? at(i_exp)->as_int() : 0;
            c.expiry = us > 0 ? (us / 1000000 - 11644473600LL) : 0;
        }
        if (c.name.empty()) return true;
        out.push_back(std::move(c));
        ++r.total;
        return true;
    });

    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// Netscape cookies.txt — the format ytcui-dl's --cookies already reads
// ---------------------------------------------------------------------------
inline bool write_netscape(const std::string& path, const std::vector<Cookie>& v,
                           std::string* err = nullptr) {
    // 0600 from the start, not chmod'd after: this file is a live credential,
    // and a window where it is world-readable is a window too many.
    for (const Cookie& c : v) {
        if (!interesting_domain(c.domain) || !cookie_field_safe(c.domain) ||
            !cookie_field_safe(c.path) || !cookie_field_safe(c.name) || !cookie_field_safe(c.value)) {
            if (err) *err = "invalid cookie domain or field";
            return false;
        }
    }
    std::string temporary = path + ".tmp.XXXXXX";
    const int fd = ::mkstemp(temporary.data());
    if (fd < 0) { if (err) *err = "cannot create " + path; return false; }
    struct stat permissions{};
    if (::fchmod(fd, 0600) != 0 || ::fstat(fd, &permissions) != 0 ||
        (permissions.st_mode & 0777) != 0600) {
        ::close(fd); ::unlink(temporary.c_str());
        if (err) *err = "filesystem cannot enforce private cookie permissions";
        return false;
    }
    FILE* f = ::fdopen(fd, "w");
    if (!f) { ::close(fd); ::unlink(temporary.c_str()); if (err) *err = "cannot write " + path; return false; }

    std::fprintf(f, "# Netscape HTTP Cookie File\n");
    std::fprintf(f, "# Written by ytcui-dl session. Treat this as a password.\n");
    const int64_t fallback = (int64_t)::time(nullptr) + 180 * 24 * 3600;
    for (const Cookie& c : v) {
        // The #HttpOnly_ prefix is the curl/yt-dlp convention and ytcui-dl's
        // reader strips it, so it is safe to emit and keeps the file portable.
        std::fprintf(f, "%s%s\t%s\t%s\t%s\t%lld\t%s\t%s\n",
                     c.http_only ? "#HttpOnly_" : "",
                     c.domain.c_str(),
                     c.domain.empty() || c.domain[0] == '.' ? "TRUE" : "FALSE",
                     c.path.c_str(),
                     c.secure ? "TRUE" : "FALSE",
                     (long long)(c.expiry > 0 ? c.expiry : fallback),
                     c.name.c_str(), c.value.c_str());
    }
    bool ok = std::fflush(f) == 0;
    if (::fsync(fd) != 0) ok = false;
    if (std::fclose(f) != 0) ok = false;
    if (!ok || ::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        if (err) *err = "cannot finalize cookie jar " + path;
        return false;
    }
    return true;
}

// Does this cookie set actually represent a signed-in account?
inline bool has_auth(const std::vector<Cookie>& v) {
    for (const Cookie& c : v)
        if (c.name == "SAPISID" || c.name == "__Secure-3PAPISID") return true;
    return false;
}

}  // namespace browser
}  // namespace ytfast
