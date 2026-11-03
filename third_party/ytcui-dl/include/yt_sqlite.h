#pragma once
/*
 * ytcui-dl — yt_sqlite.h
 *
 * A read-only, single-table SQLite reader. Enough to walk one table's rows and
 * no more.
 *
 * Why this exists rather than `-lsqlite3`: every browser on every platform
 * keeps its cookies in a SQLite file, so reading one is the only way to pick a
 * session up from a browser the user already signed into. Linking libsqlite3
 * for it would break the one property this project is actually about -- it
 * ships against OpenSSL, zlib and pthreads and nothing else. Shelling out to
 * the `sqlite3` CLI was the other option and was rejected: it isn't installed
 * by default on a stock Ubuntu (confirmed: not present on 24.04), so the
 * feature would silently not exist for most users.
 *
 * The scope is deliberately tiny. There is no SQL, no query planner, no
 * writing, no index handling: find a table by name in sqlite_schema, then walk
 * its B-tree and decode each record. That is a few hundred lines of
 * well-specified file format, versus a dependency.
 *
 * Reference: https://sqlite.org/fileformat2.html
 *
 * Caveat worth knowing: if the browser is running, recent writes may sit in a
 * `-wal` sidecar that has not been checkpointed into the main file yet. This
 * reader does not replay a WAL, so wal_pending() reports it and callers tell
 * the user to close the browser first -- the same advice yt-dlp gives for the
 * same reason.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace ytfast {
namespace sqlite {

struct Value {
    enum Kind { Null, Int, Real, Text, Blob } kind = Null;
    int64_t     i = 0;
    double      d = 0;
    std::string s;          // Text and Blob both land here; Blob may hold NULs

    bool        is_null() const { return kind == Null; }
    int64_t     as_int() const {
        if (kind == Int) return i;
        if (kind == Real) return (int64_t)d;
        if (kind == Text) return std::strtoll(s.c_str(), nullptr, 10);
        return 0;
    }
    const std::string& as_str() const { return s; }
};

class Db {
public:
    bool open(const std::string& path) {
        path_ = path;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        // A cookie database is a few hundred KB. Refuse something absurd
        // rather than map an arbitrary file into memory.
        if (n < 512 || n > 512 * 1024 * 1024) { std::fclose(f); return false; }
        buf_.resize((size_t)n);
        const size_t got = std::fread(buf_.data(), 1, buf_.size(), f);
        std::fclose(f);
        if (got != buf_.size()) return false;

        if (std::memcmp(buf_.data(), "SQLite format 3\0", 16) != 0) return false;
        uint32_t ps = be16(16);
        if (ps == 1) ps = 65536;                       // the documented escape
        if (ps < 512 || (ps & (ps - 1)) != 0) return false;
        page_size_ = ps;
        reserved_  = buf_[20];
        if (reserved_ >= page_size_) return false;
        usable_ = page_size_ - reserved_;
        return true;
    }

    // True when a non-empty write-ahead log sits next to the database, i.e.
    // the newest rows may not be in the file we just read.
    bool wal_pending() const {
        struct stat st{};
        return ::stat((path_ + "-wal").c_str(), &st) == 0 && st.st_size > 0;
    }

    // Column names, in declaration order, parsed out of the CREATE TABLE text.
    std::vector<std::string> columns(const std::string& table) const {
        std::string sql;
        uint32_t root = 0;
        if (!find_table(table, root, sql)) return {};
        return parse_columns(sql);
    }

    // Walk every row of `table`. The callback returning false stops the scan.
    bool scan(const std::string& table,
              const std::function<bool(const std::vector<Value>&)>& cb) const {
        std::string sql;
        uint32_t root = 0;
        if (!find_table(table, root, sql) || root == 0) return false;
        bool stop = false;
        walk(root, cb, stop, 0);
        return true;
    }

private:
    std::string          path_;
    std::vector<uint8_t> buf_;
    uint32_t             page_size_ = 0;
    uint32_t             reserved_  = 0;
    uint32_t             usable_    = 0;

    uint32_t be16(size_t o) const {
        return ((uint32_t)buf_[o] << 8) | buf_[o + 1];
    }
    uint32_t be32(size_t o) const {
        return ((uint32_t)buf_[o] << 24) | ((uint32_t)buf_[o + 1] << 16) |
               ((uint32_t)buf_[o + 2] << 8) | buf_[o + 3];
    }
    bool in_range(size_t o, size_t n) const { return o + n <= buf_.size(); }

    // SQLite varint: big-endian 7 bits per byte, up to 8 bytes, with the ninth
    // byte contributing a full 8 bits.
    static int varint(const uint8_t* p, size_t avail, uint64_t& out) {
        out = 0;
        for (int k = 0; k < 8; ++k) {
            if ((size_t)k >= avail) return 0;
            out = (out << 7) | (p[k] & 0x7F);
            if (!(p[k] & 0x80)) return k + 1;
        }
        if (avail < 9) return 0;
        out = (out << 8) | p[8];
        return 9;
    }

    bool find_table(const std::string& table, uint32_t& root, std::string& sql) const {
        bool found = false, stop = false;
        // sqlite_schema always lives at page 1: (type, name, tbl_name,
        // rootpage, sql).
        walk(1, [&](const std::vector<Value>& r) {
            if (r.size() < 5) return true;
            if (r[0].as_str() != "table" || r[1].as_str() != table) return true;
            root  = (uint32_t)r[3].as_int();
            sql   = r[4].as_str();
            found = true;
            return false;                       // stop: we have what we need
        }, stop, 0);
        return found;
    }

    static std::vector<std::string> parse_columns(const std::string& sql) {
        const size_t lp = sql.find('(');
        if (lp == std::string::npos) return {};
        // Split the column list on top-level commas only; a column can carry
        // parenthesised constraints of its own.
        std::vector<std::string> out;
        int depth = 0;
        std::string cur;
        for (size_t i = lp + 1; i < sql.size(); ++i) {
            const char c = sql[i];
            if (c == '(') { ++depth; cur += c; continue; }
            if (c == ')') {
                if (depth == 0) break;
                --depth; cur += c; continue;
            }
            if (c == ',' && depth == 0) { out.push_back(cur); cur.clear(); continue; }
            cur += c;
        }
        if (!cur.empty()) out.push_back(cur);

        std::vector<std::string> names;
        names.reserve(out.size());
        for (std::string s : out) {
            size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) continue;
            s = s.substr(a);
            // A table-level constraint is not a column.
            static const char* kw[] = {"PRIMARY", "UNIQUE", "CHECK", "FOREIGN",
                                       "CONSTRAINT", "primary", "unique",
                                       "check", "foreign", "constraint"};
            bool is_kw = false;
            for (const char* k : kw)
                if (s.rfind(k, 0) == 0) { is_kw = true; break; }
            if (is_kw) continue;

            std::string name;
            if (s[0] == '"' || s[0] == '`' || s[0] == '[') {
                const char close = s[0] == '[' ? ']' : s[0];
                const size_t e = s.find(close, 1);
                name = e == std::string::npos ? s.substr(1) : s.substr(1, e - 1);
            } else {
                const size_t e = s.find_first_of(" \t\r\n(");
                name = e == std::string::npos ? s : s.substr(0, e);
            }
            if (!name.empty()) names.push_back(name);
        }
        return names;
    }

    // Depth-first walk of a table B-tree. `depth` only guards against a
    // corrupt file pointing a page at itself.
    void walk(uint32_t pgno,
              const std::function<bool(const std::vector<Value>&)>& cb,
              bool& stop, int depth) const {
        if (stop || pgno == 0 || depth > 32) return;
        const size_t base = (size_t)(pgno - 1) * page_size_;
        if (!in_range(base, page_size_)) return;
        const size_t hdr = base + (pgno == 1 ? 100 : 0);
        if (!in_range(hdr, 12)) return;

        const uint8_t type = buf_[hdr];
        const uint32_t ncell = be16(hdr + 3);
        const bool leaf = (type == 0x0D);
        const bool interior = (type == 0x05);
        if (!leaf && !interior) return;                 // index page: not ours
        const size_t cellptr = hdr + (interior ? 12 : 8);

        for (uint32_t c = 0; c < ncell && !stop; ++c) {
            if (!in_range(cellptr + 2 * c, 2)) return;
            const size_t off = base + be16(cellptr + 2 * c);
            if (!in_range(off, 1)) continue;
            if (interior) {
                // 4-byte left child page, then a rowid varint we don't need.
                if (!in_range(off, 4)) continue;
                walk(be32(off), cb, stop, depth + 1);
            } else {
                std::vector<Value> row;
                if (read_leaf_cell(off, row) && !cb(row)) { stop = true; return; }
            }
        }
        if (interior && !stop) {
            if (!in_range(hdr + 8, 4)) return;
            walk(be32(hdr + 8), cb, stop, depth + 1);   // right-most pointer
        }
    }

    // Assemble a leaf cell's payload, following the overflow chain when the
    // record is too big to sit in one page, then decode it.
    bool read_leaf_cell(size_t off, std::vector<Value>& row) const {
        uint64_t payload = 0, rowid = 0;
        int n = varint(&buf_[off], buf_.size() - off, payload);
        if (n == 0) return false;
        size_t p = off + n;
        n = varint(&buf_[p], buf_.size() - p, rowid);
        if (n == 0) return false;
        p += n;
        if (payload > 64u * 1024 * 1024) return false;

        const uint32_t U = usable_;
        const uint32_t X = U - 35;                      // max local for a leaf
        size_t local = payload;
        if (payload > X) {
            const uint32_t M = ((U - 12) * 32 / 255) - 23;
            const uint32_t K = M + (uint32_t)((payload - M) % (U - 4));
            local = (K <= X) ? K : M;
        }
        if (!in_range(p, local)) return false;

        std::string rec((const char*)&buf_[p], local);
        if (local < payload) {
            if (!in_range(p + local, 4)) return false;
            uint32_t next = be32(p + local);
            size_t remaining = payload - local;
            int guard = 0;
            while (next && remaining && guard++ < 4096) {
                const size_t ob = (size_t)(next - 1) * page_size_;
                if (!in_range(ob, U < 4 ? 4 : U)) return false;
                const size_t take = remaining < (size_t)(U - 4) ? remaining : (size_t)(U - 4);
                if (!in_range(ob + 4, take)) return false;
                rec.append((const char*)&buf_[ob + 4], take);
                remaining -= take;
                next = be32(ob);
            }
            if (remaining) return false;
        }
        return decode_record(rec, row);
    }

    static bool decode_record(const std::string& rec, std::vector<Value>& row) {
        const uint8_t* p = (const uint8_t*)rec.data();
        const size_t n = rec.size();
        uint64_t hdr_size = 0;
        int k = varint(p, n, hdr_size);
        if (k == 0 || hdr_size > n) return false;

        std::vector<uint64_t> types;
        size_t h = (size_t)k;
        while (h < hdr_size) {
            uint64_t t = 0;
            const int m = varint(p + h, hdr_size - h, t);
            if (m == 0) return false;
            h += m;
            types.push_back(t);
        }

        size_t body = (size_t)hdr_size;
        row.clear();
        row.reserve(types.size());
        for (uint64_t t : types) {
            Value v;
            auto need = [&](size_t want) { return body + want <= n; };
            if (t == 0) { v.kind = Value::Null; }
            else if (t >= 1 && t <= 6) {
                static const int sz[] = {0, 1, 2, 3, 4, 6, 8};
                const int w = sz[t];
                if (!need((size_t)w)) return false;
                int64_t x = (p[body] & 0x80) ? -1 : 0;   // sign-extend
                for (int b = 0; b < w; ++b) x = (x << 8) | p[body + b];
                v.kind = Value::Int; v.i = x; body += w;
            } else if (t == 7) {
                if (!need(8)) return false;
                uint64_t bits = 0;
                for (int b = 0; b < 8; ++b) bits = (bits << 8) | p[body + b];
                double dv; std::memcpy(&dv, &bits, 8);
                v.kind = Value::Real; v.d = dv; body += 8;
            } else if (t == 8) { v.kind = Value::Int; v.i = 0; }
            else if (t == 9) { v.kind = Value::Int; v.i = 1; }
            else if (t >= 12) {
                const size_t len = (size_t)((t - (t % 2 ? 13 : 12)) / 2);
                if (!need(len)) return false;
                v.kind = (t % 2) ? Value::Text : Value::Blob;
                v.s.assign((const char*)p + body, len);
                body += len;
            } else {
                v.kind = Value::Null;                    // 10/11: reserved
            }
            row.push_back(std::move(v));
        }
        return true;
    }
};

}  // namespace sqlite
}  // namespace ytfast
