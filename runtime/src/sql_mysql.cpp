// marina runtime — std module: sql driver for MySQL / MariaDB.
//
// A client-side implementation of the MySQL client/server protocol
// (protocol 4.1, servers 5.5+, MySQL 8.x and all MariaDB versions) —
// no libmysqlclient / libmariadb dependency. Supports:
//   * mysql_native_password and caching_sha2_password (fast path) auth,
//     including auth-switch requests
//   * prepared statements (COM_STMT_PREPARE / COM_STMT_EXECUTE) so
//     values are always bound as parameters, never string-concatenated
//   * the binary result format
//
// URL: mysql://user:password@host:port/database
// (mariadb:// is accepted too; defaults: user "root", port 3306).
//
// TLS is not negotiated (CLIENT_SSL is not requested). caching_sha2
// accounts whose entries are not on the server's cache require the RSA
// password exchange over TLS and are rejected with a clear error.
#include "sql_backend.hpp"
#include "sql_wire.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace marina_rt {

namespace my {

// Capability flags.
constexpr uint32_t CAP_LONG_PASSWORD = 0x00000001;
constexpr uint32_t CAP_PROTOCOL_41 = 0x00000200;
constexpr uint32_t CAP_CONNECT_WITH_DB = 0x00000008;
constexpr uint32_t CAP_SECURE_CONNECTION = 0x00008000;
constexpr uint32_t CAP_PLUGIN_AUTH = 0x00080000;

// Column types (protocol constants).
constexpr int MYSQL_TYPE_DECIMAL = 0;
constexpr int MYSQL_TYPE_TINY = 1;
constexpr int MYSQL_TYPE_SHORT = 2;
constexpr int MYSQL_TYPE_LONG = 3;
constexpr int MYSQL_TYPE_FLOAT = 4;
constexpr int MYSQL_TYPE_DOUBLE = 5;
constexpr int MYSQL_TYPE_NULL = 6;
constexpr int MYSQL_TYPE_TIMESTAMP = 7;
constexpr int MYSQL_TYPE_LONGLONG = 8;
constexpr int MYSQL_TYPE_INT24 = 9;
constexpr int MYSQL_TYPE_DATE = 10;
constexpr int MYSQL_TYPE_TIME = 11;
constexpr int MYSQL_TYPE_DATETIME = 12;
constexpr int MYSQL_TYPE_YEAR = 13;
constexpr int MYSQL_TYPE_VARCHAR = 15;
constexpr int MYSQL_TYPE_BIT = 16;
constexpr int MYSQL_TYPE_NEWDATE = 14;
constexpr int MYSQL_TYPE_JSON = 245;
constexpr int MYSQL_TYPE_NEWDECIMAL = 246;
constexpr int MYSQL_TYPE_ENUM = 254;
constexpr int MYSQL_TYPE_SET = 248;
constexpr int MYSQL_TYPE_TINY_BLOB = 249;
constexpr int MYSQL_TYPE_MEDIUM_BLOB = 250;
constexpr int MYSQL_TYPE_LONG_BLOB = 251;
constexpr int MYSQL_TYPE_BLOB = 252;
constexpr int MYSQL_TYPE_VAR_STRING = 253;
constexpr int MYSQL_TYPE_STRING = 254;

constexpr uint16_t FLAG_UNSIGNED = 0x8000;

struct MyConn {
    int fd;
    std::mutex mu; // serializes statements (async queries can race)
    uint8_t seq = 0;
    // MariaDB 12+ wants each parameter typed as a bare int<2> in
    // COM_STMT_EXECUTE; MySQL documents [int<2> type][byte flags]. We
    // start with the MySQL layout and flip to the 2-byte form on the
    // server's ER_WRONG_ARGUMENTS (or immediately for MariaDB servers).
    bool compat_2b_types = false;
};

// --- packet framing ---------------------------------------------------------

std::string my_recv(MyConn* c) {
    uint8_t hdr[4];
    dbsock::read_full(c->fd, hdr, 4, "mysql");
    uint32_t len = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16);
    if (len == 0xffffff) {
        dbsock::close_fd(c->fd);
        rt_panic("mysql: packets larger than 16 MiB are not supported");
    }
    c->seq = (uint8_t)(hdr[3] + 1);
    std::string payload(len, '\0');
    if (len > 0) {
        dbsock::read_full(c->fd, reinterpret_cast<uint8_t*>(payload.data()), len, "mysql");
    }
    return payload;
}

void my_send(MyConn* c, const std::string& payload) {
    uint8_t hdr[4];
    size_t len = payload.size();
    hdr[0] = (uint8_t)len;
    hdr[1] = (uint8_t)(len >> 8);
    hdr[2] = (uint8_t)(len >> 16);
    hdr[3] = c->seq;
    c->seq = (uint8_t)(c->seq + 1);
    dbsock::write_all(c->fd, reinterpret_cast<const uint8_t*>(hdr), 4, "mysql");
    dbsock::write_all(c->fd, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
                      "mysql");
}

// --- payload readers ---------------------------------------------------------

struct Payload {
    const std::string& s;
    size_t off = 0;
    bool eof() const { return off >= s.size(); }
    uint8_t u8() { return off < s.size() ? (uint8_t)s[off++] : 0; }
    uint16_t u16() {
        uint16_t v = (uint16_t)(u8() | (u8() << 8));
        return v;
    }
    uint32_t u32() {
        uint32_t v = (uint32_t)u8();
        v |= (uint32_t)u8() << 8;
        v |= (uint32_t)u8() << 16;
        v |= (uint32_t)u8() << 24;
        return v;
    }
    uint64_t u64() {
        uint64_t v = (uint64_t)u32();
        v |= (uint64_t)u32() << 32;
        return v;
    }
    std::string cstr() {
        size_t start = off;
        while (off < s.size() && s[off] != '\0') off++;
        std::string out = s.substr(start, off - start);
        if (off < s.size()) off++;
        return out;
    }
    uint64_t lenc() {
        uint8_t f = u8();
        if (f < 0xfb) return f;
        if (f == 0xfb) return 0; // NULL — caller checks first
        if (f == 0xfc) return u16();
        if (f == 0xfd) return ((uint64_t)u8()) | ((uint64_t)u8() << 8) |
                              ((uint64_t)u8() << 16);
        if (f == 0xfe) return u64();
        return 0;
    }
    std::string lstr() {
        uint8_t f = off < s.size() ? (uint8_t)s[off] : 0;
        if (off >= s.size()) return std::string(); // exhausted — nothing left to read
        uint64_t len;
        if (f < 0xfb) {
            off++;
            len = f;
        } else if (f == 0xfc) {
            off++;
            len = u16();
        } else if (f == 0xfd) {
            off++;
            len = (uint64_t)u8() | ((uint64_t)u8() << 8) | ((uint64_t)u8() << 16);
        } else if (f == 0xfe) {
            off++;
            len = u64();
        } else {
            off++;
            return std::string(); // 0xfb: NULL
        }
        if (off + len > s.size()) len = s.size() - off;
        std::string out = s.substr(off, (size_t)len);
        off += (size_t)len;
        return out;
    }
    std::string rest() {
        std::string out = s.substr(off);
        off = s.size();
        return out;
    }
};

// Split an ERR packet into (code, message).
bool my_parse_err(const std::string& payload, uint16_t* code, std::string* msg) {
    if (payload.empty() || (uint8_t)payload[0] != 0xff) return false;
    Payload p{payload};
    (void)p.u8();
    *code = p.u16();
    if (!p.eof() && p.s[p.off] == '#') {
        p.off += 6; // '#' + 5-byte SQL state
    }
    *msg = p.rest();
    return true;
}

[[noreturn]] void my_panic_err(const std::string& payload) {
    uint16_t code = 0;
    std::string msg;
    if (!my_parse_err(payload, &code, &msg)) {
        rt_panic("mysql: unknown server error");
    }
    rt_panic("mysql: " + msg + " (error " + std::to_string(code) + ")");
}

// --- auth tokens --------------------------------------------------------------

// mysql_native_password: SHA1(pw) XOR SHA1(salt || SHA1(SHA1(pw)))
std::string native_token(const std::string& password, const std::string& salt) {
    uint8_t h1[20], h2[20], inner[20];
    dbhash::sha1(reinterpret_cast<const uint8_t*>(password.data()), password.size(), h1);
    dbhash::sha1(h1, 20, h2);
    std::string msg = salt;
    msg.append(reinterpret_cast<const char*>(h2), 20);
    dbhash::sha1(reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), inner);
    std::string token;
    token.reserve(20);
    for (int i = 0; i < 20; i++) {
        token.push_back((char)(h1[i] ^ inner[i]));
    }
    return token;
}

// caching_sha2_password (fast path): SHA256(pw) XOR SHA256(SHA256(SHA256(pw)) || nonce)
std::string sha2_token(const std::string& password, const std::string& nonce) {
    uint8_t h1[32], h2[32], inner[32];
    dbhash::sha256(reinterpret_cast<const uint8_t*>(password.data()), password.size(), h1);
    dbhash::sha256(h1, 32, h2);
    std::string msg = nonce;
    msg.append(reinterpret_cast<const char*>(h2), 32);
    dbhash::sha256(reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), inner);
    std::string token;
    token.reserve(32);
    for (int i = 0; i < 32; i++) {
        token.push_back((char)(h1[i] ^ inner[i]));
    }
    return token;
}

std::string auth_token(const std::string& plugin, const std::string& password,
                       const std::string& salt) {
    if (plugin == "mysql_native_password") return native_token(password, salt);
    if (plugin == "caching_sha2_password") return sha2_token(password, salt);
    rt_panic("mysql: unsupported auth plugin '" + plugin +
             "' (supported: mysql_native_password, caching_sha2_password)");
}

// Column definition packet (protocol 4.1).
struct ColDef {
    std::string name;
    int type = 0;
    uint16_t flags = 0;
};

ColDef read_col_def(Payload& p) {
    ColDef c;
    (void)p.lstr(); // catalog "def"
    (void)p.lstr(); // schema
    (void)p.lstr(); // table
    (void)p.lstr(); // org_table
    c.name = p.lstr(); // name
    (void)p.lstr(); // org_name
    (void)p.lenc(); // length of fixed fields (0x0c)
    (void)p.u16(); // charset
    (void)p.u32(); // column length
    c.type = (int)p.u8();
    c.flags = p.u16();
    (void)p.u8(); // decimals
    (void)p.u16(); // filler
    return c;
}

// Extract one column value from a binary-protocol row packet. Fixed-size
// numeric types carry raw little-endian bytes; every other type (strings,
// DECIMAL, DATE/DATETIME/TIME, BIT, ...) is length-encoded.
std::string my_read_value(Payload& p, const ColDef& col) {
    size_t fixed = 0;
    switch (col.type) {
        case MYSQL_TYPE_TINY: fixed = 1; break;
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_YEAR: fixed = 2; break;
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_FLOAT: fixed = 4; break;
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_DOUBLE: fixed = 8; break;
        default: fixed = 0; break;
    }
    if (fixed > 0) {
        if (p.off + fixed > p.s.size()) fixed = p.s.size() - p.off;
        std::string out = p.s.substr(p.off, fixed);
        p.off += fixed;
        return out;
    }
    return p.lstr();
}

MValue my_decode_binary(const ColDef& col, const std::string& v) {
    bool uns = (col.flags & FLAG_UNSIGNED) != 0;
    switch (col.type) {
        case MYSQL_TYPE_TINY: {
            int8_t b;
            std::memcpy(&b, v.data(), 1);
            return mv_int(uns ? (int64_t)(uint8_t)b : (int64_t)b);
        }
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_YEAR: {
            int16_t x;
            std::memcpy(&x, v.data(), 2);
            return mv_int(uns ? (int64_t)(uint16_t)x : (int64_t)x);
        }
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_INT24: {
            int32_t x;
            std::memcpy(&x, v.data(), 4);
            return mv_int(uns ? (int64_t)(uint32_t)x : (int64_t)x);
        }
        case MYSQL_TYPE_LONGLONG: {
            uint64_t x;
            std::memcpy(&x, v.data(), 8);
            return mv_int((int64_t)x);
        }
        case MYSQL_TYPE_FLOAT: {
            float f;
            std::memcpy(&f, v.data(), 4);
            MValue m = mv_void();
            m.tag = TAG_FLOAT;
            m.f = (double)f;
            return m;
        }
        case MYSQL_TYPE_DOUBLE: {
            double f;
            std::memcpy(&f, v.data(), 8);
            MValue m = mv_void();
            m.tag = TAG_FLOAT;
            m.f = f;
            return m;
        }
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIMESTAMP: {
            if (v.empty()) return mv_str_owned("0000-00-00 00:00:00");
            char buf[40];
            uint8_t len = (uint8_t)v[0];
            if (len < 4) return mv_str_owned("0000-00-00 00:00:00");
            uint16_t y = (uint16_t)((uint8_t)v[1] | ((uint16_t)(uint8_t)v[2] << 8));
            uint8_t mo = (uint8_t)v[3];
            uint8_t d = (uint8_t)v[4];
            if (len == 4) {
                std::snprintf(buf, sizeof buf, "%04u-%02u-%02u", y, mo, d);
                return mv_str_owned(std::string(buf));
            }
            uint8_t hh = (uint8_t)v[5];
            uint8_t mi = (uint8_t)v[6];
            uint8_t ss = (uint8_t)v[7];
            if (len < 11) {
                std::snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u", y, mo, d,
                              hh, mi, ss);
                return mv_str_owned(std::string(buf));
            }
            uint32_t us = (uint32_t)(uint8_t)v[8] |
                          ((uint32_t)(uint8_t)v[9] << 8) |
                          ((uint32_t)(uint8_t)v[10] << 16) |
                          ((uint32_t)(uint8_t)v[11] << 24);
            std::snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u.%06u", y, mo, d,
                          hh, mi, ss, us);
            return mv_str_owned(std::string(buf));
        }
        case MYSQL_TYPE_TIME: {
            if (v.empty()) return mv_str_owned("00:00:00");
            uint8_t len = (uint8_t)v[0];
            if (len < 8) return mv_str_owned("00:00:00");
            uint8_t neg = (uint8_t)v[1];
            uint32_t days = (uint32_t)(uint8_t)v[2] | ((uint32_t)(uint8_t)v[3] << 8) |
                            ((uint32_t)(uint8_t)v[4] << 16) |
                            ((uint32_t)(uint8_t)v[5] << 24);
            uint8_t hh = (uint8_t)v[6];
            uint8_t mi = (uint8_t)v[7];
            uint8_t ss = (uint8_t)v[8];
            char buf[40];
            if (len >= 12) {
                uint32_t us = (uint32_t)(uint8_t)v[9] |
                              ((uint32_t)(uint8_t)v[10] << 8) |
                              ((uint32_t)(uint8_t)v[11] << 16) |
                              ((uint32_t)(uint8_t)v[12] << 24);
                std::snprintf(buf, sizeof buf, "%s%02u:%02u:%02u.%06u", neg ? "-" : "",
                              (unsigned)(days * 24 + hh), mi, ss, us);
            } else {
                std::snprintf(buf, sizeof buf, "%s%02u:%02u:%02u", neg ? "-" : "",
                              (unsigned)(days * 24 + hh), mi, ss);
            }
            return mv_str_owned(std::string(buf));
        }
        case MYSQL_TYPE_BIT: {
            // BIT arrives as a length-encoded big-endian byte string.
            uint64_t x = 0;
            for (unsigned char ch : v) x = (x << 8) | (uint8_t)ch;
            return mv_int((int64_t)x);
        }
        case MYSQL_TYPE_NEWDECIMAL:
        case MYSQL_TYPE_DECIMAL: {
            char* end = nullptr;
            double f = std::strtod(v.c_str(), &end);
            (void)end;
            MValue m = mv_void();
            m.tag = TAG_FLOAT;
            m.f = f;
            return m;
        }
        default: // VARCHAR/STRING/BLOB/JSON/ENUM/SET/... — length-encoded text
            return mv_str_owned(utf8_lossy(reinterpret_cast<const uint8_t*>(v.data()),
                                           v.size()));
    }
}

} // namespace my

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

void* my_connect(const std::string& url) {
    SqlUrl u = sql_parse_url(url, "mysql", /*is_pg=*/false);
    int fd = dbsock::tcp_connect(u.host, u.port, "mysql");
    my::MyConn c{fd};

    // Initial handshake packet.
    std::string hs = my::my_recv(&c);
    std::string server_version;
    if (hs.empty()) {
        dbsock::close_fd(fd);
        rt_panic("mysql: empty handshake packet");
    }
    {
        my::Payload p{hs};
        uint8_t proto = p.u8();
        if (proto != 10) {
            dbsock::close_fd(fd);
            rt_panic("mysql: unsupported protocol version " + std::to_string(proto) +
                     " (only 4.1+ servers are supported)");
        }
        std::string salt;
        server_version = p.cstr();
        (void)p.u32(); // thread id
        for (int i = 0; i < 8; i++) salt.push_back((char)p.u8()); // part 1
        (void)p.u8(); // filler
        uint16_t cap_low = p.u16();
        (void)p.u8(); // charset
        (void)p.u16(); // status
        uint16_t cap_high = p.u16();
        uint8_t data_len = p.u8();
        for (int i = 0; i < 10; i++) (void)p.u8(); // reserved
        if ((cap_low | ((uint32_t)cap_high << 16)) & my::CAP_SECURE_CONNECTION) {
            size_t n = data_len > 8 ? (size_t)data_len - 8 : 0;
            if (n < 13) n = 13;
            for (size_t i = 0; i < n && !p.eof(); i++) salt.push_back((char)p.u8());
            if (!salt.empty() && salt[salt.size() - 1] == '\0') salt.pop_back();
        }
        std::string plugin;
        if ((cap_low | ((uint32_t)cap_high << 16)) & my::CAP_PLUGIN_AUTH) {
            plugin = p.cstr();
        }
        if (plugin.empty()) plugin = "mysql_native_password";
        if (salt.size() < 20) {
            dbsock::close_fd(fd);
            rt_panic("mysql: handshake auth data too short");
        }
        salt.resize(20);

        // Handshake Response 41.
        uint32_t caps = my::CAP_LONG_PASSWORD | my::CAP_PROTOCOL_41 |
                        my::CAP_SECURE_CONNECTION | my::CAP_PLUGIN_AUTH;
        std::string resp;
        if (!u.database.empty()) caps |= my::CAP_CONNECT_WITH_DB;
        for (int i = 0; i < 4; i++) resp.push_back((char)(caps >> (8 * i)));
        uint32_t max_pkt = 0x01000000; // 16 MiB
        for (int i = 0; i < 4; i++) resp.push_back((char)(max_pkt >> (8 * i)));
        resp.push_back((char)45); // utf8mb4_general_ci
        for (int i = 0; i < 23; i++) resp.push_back('\0');
        resp += u.user;
        resp.push_back('\0');
        std::string token = my::auth_token(plugin, u.password, salt);
        resp.push_back((char)token.size());
        resp += token;
        if (!u.database.empty()) {
            resp += u.database;
            resp.push_back('\0');
        }
        resp += plugin;
        resp.push_back('\0');
        my::my_send(&c, resp);
        // Auth loop: OK / ERR / auth-switch / auth-more-data.
        for (;;) {
            std::string pk = my::my_recv(&c);
            if (pk.empty()) {
                dbsock::close_fd(fd);
                rt_panic("mysql: empty packet during authentication");
            }
            uint8_t t = (uint8_t)pk[0];
            if (t == 0x00) break; // OK
            if (t == 0xff) my::my_panic_err(pk);
            if (t == 0xfe) { // AuthSwitchRequest
                my::Payload p2{pk};
                (void)p2.u8();
                std::string new_plugin = p2.cstr();
                std::string new_salt = p2.rest();
                while (!new_salt.empty() && new_salt[new_salt.size() - 1] == '\0') {
                    new_salt.pop_back();
                }
                if (new_salt.size() < 20) {
                    dbsock::close_fd(fd);
                    rt_panic("mysql: auth-switch auth data too short");
                }
                new_salt.resize(20);
                std::string tok = my::auth_token(new_plugin, u.password, new_salt);
                my::my_send(&c, tok); // seq was advanced by my_recv
                continue;
            }
            if (t == 0x01) { // AuthMoreData (caching_sha2_password)
                if (pk.size() >= 2 && pk[1] == '\x03') continue; // fast auth OK
                if (pk.size() >= 2 && pk[1] == '\x04') {
                    dbsock::close_fd(fd);
                    rt_panic("mysql: caching_sha2_password full authentication requires "
                             "TLS/RSA (unsupported); use an account in the server's "
                             "password cache or mysql_native_password");
                }
                // (the "3rd auth data" packet for clear-text over non-TLS is
                // not supported)
                dbsock::close_fd(fd);
                rt_panic("mysql: unsupported caching_sha2_password exchange "
                         "(TLS required)");
            }
            dbsock::close_fd(fd);
            rt_panic("mysql: unexpected packet 0x" + dbhash::hex_encode(&t, 1) +
                     " during authentication");
        }
    }

    void* p = new my::MyConn{fd};
    // MariaDB 12+ uses a 2-byte parameter type block (no per-parameter
    // flag byte) in COM_STMT_EXECUTE.
    if (server_version.find("MariaDB") != std::string::npos) {
        reinterpret_cast<my::MyConn*>(p)->compat_2b_types = true;
    }
    return p;
}

void my_disconnect(void* p) {
    my::MyConn* c = reinterpret_cast<my::MyConn*>(p);
    c->seq = 0;
    std::string quit;
    quit.push_back('\x01'); // COM_QUIT
    my::my_send(c, quit);
    dbsock::close_fd(c->fd);
    delete c;
}

// ---------------------------------------------------------------------------
// Prepared statements (COM_STMT_PREPARE / COM_STMT_EXECUTE / COM_STMT_CLOSE)
// ---------------------------------------------------------------------------

namespace my {

struct MyStmt {
    uint32_t id;
    uint16_t num_params;
    uint16_t num_cols;
};

MyStmt my_prepare(my::MyConn* c, const std::string& sql) {
    c->seq = 0;
    std::string cmd;
    cmd.push_back('\x16'); // COM_STMT_PREPARE
    cmd += sql;
    my::my_send(c, cmd);
    std::string pk = my::my_recv(c);
    if (pk.empty()) {
        dbsock::close_fd(c->fd);
        rt_panic("mysql: empty response to COM_STMT_PREPARE");
    }
    if ((uint8_t)pk[0] == 0xff) my::my_panic_err(pk);
    if ((uint8_t)pk[0] != 0x00) {
        dbsock::close_fd(c->fd);
        rt_panic("mysql: unexpected response to COM_STMT_PREPARE");
    }
    my::Payload p{pk};
    (void)p.u8();
    MyStmt st;
    st.id = p.u32();
    st.num_cols = p.u16();
    st.num_params = p.u16();
    // Consume the parameter-definition block (num_params defs + EOF) and
    // the column-definition block (num_cols defs + EOF) that follow the
    // prepare response.
    for (int block = 0; block < 2; block++) {
        uint16_t ndefs = block == 0 ? st.num_params : st.num_cols;
        for (int i = 0; i < ndefs; i++) {
            std::string dpk = my::my_recv(c);
            if (!dpk.empty() && (uint8_t)dpk[0] == 0xff) my::my_panic_err(dpk);
        }
        if (ndefs > 0) {
            std::string epk = my::my_recv(c); // EOF terminator
            if (!epk.empty() && (uint8_t)epk[0] == 0xff) my::my_panic_err(epk);
        }
    }
    return st;
}

// Read the column definition block (num defs + EOF terminator).
std::vector<my::ColDef> my_read_cols(my::MyConn* c, int32_t num) {
    std::vector<my::ColDef> cols;
    for (int32_t i = 0; i < num; i++) {
        std::string pk = my::my_recv(c);
        if (!pk.empty() && (uint8_t)pk[0] == 0xff) my::my_panic_err(pk);
        my::Payload p{pk};
        cols.push_back(my::read_col_def(p));
    }
    // EOF packet terminator (CLIENT_DEPRECATE_EOF not negotiated).
    std::string pk = my::my_recv(c);
    if (!pk.empty() && (uint8_t)pk[0] == 0xff) my::my_panic_err(pk);
    return cols;
}

void my_stmt_close(my::MyConn* c, uint32_t id) {
    c->seq = 0;
    std::string cmd;
    cmd.push_back('\x19'); // COM_STMT_CLOSE
    for (int i = 0; i < 4; i++) cmd.push_back((char)(id >> (8 * i)));
    my::my_send(c, cmd); // no response
}

// Encode one bound parameter into the execute packet.
void my_write_param(std::string* values, uint16_t* type, const MValue& v,
                    bool with_flag_byte) {
    switch (v.tag) {
        case TAG_INT:
            *type = (uint16_t)my::MYSQL_TYPE_LONGLONG;
            for (int i = 0; i < 8; i++) values->push_back((char)((uint64_t)v.i >> (8 * i)));
            return;
        case TAG_FLOAT:
            *type = (uint16_t)my::MYSQL_TYPE_DOUBLE;
            {
                double f = v.f;
                uint8_t b[8];
                std::memcpy(b, &f, 8);
                values->append(reinterpret_cast<const char*>(b), 8);
            }
            return;
        case TAG_BOOL:
            *type = (uint16_t)my::MYSQL_TYPE_TINY;
            values->push_back((char)(v.i != 0 ? 1 : 0));
            return;
        case TAG_STRING: {
            *type = (uint16_t)my::MYSQL_TYPE_VAR_STRING;
            void* obj = v.p;
            size_t len = (size_t)str_len(obj);
            const uint8_t* bytes = str_bytes(obj);
            if (len < 0xfb) {
                values->push_back((char)len);
            } else if (len <= 0xffff) {
                values->push_back((char)0xfc);
                values->push_back((char)len);
                values->push_back((char)(len >> 8));
            } else {
                values->push_back((char)0xfe);
                uint64_t l = (uint64_t)len;
                for (int i = 0; i < 8; i++) values->push_back((char)(l >> (8 * i)));
            }
            values->append(reinterpret_cast<const char*>(bytes), len);
            return;
        }
        case TAG_VOID:
            *type = (uint16_t)my::MYSQL_TYPE_NULL;
            return;
        default:
            (void)with_flag_byte;
            rt_panic("mysql: cannot bind a value of tag " + std::to_string(v.tag) +
                     " (bind an integer, float, string, bool or null)");
    }
}

void my_stmt_execute(my::MyConn* c, const MyStmt& st, const std::vector<MValue>& binds,
                     SqlRowSet* out, int64_t* last_id, int64_t* changes) {
    if ((size_t)st.num_params != binds.size()) {
        rt_panic("mysql: statement takes " + std::to_string(st.num_params) +
                 " parameter(s), got " + std::to_string(binds.size()));
    }
    size_t n = (size_t)st.num_params;
    size_t bitmap_bytes = (n + 7) / 8;
    std::vector<uint8_t> nullbits(bitmap_bytes, 0);
    std::string types;
    std::string values;
    for (size_t i = 0; i < n; i++) {
        uint16_t type = 0;
        my::my_write_param(&values, &type, binds[i], /*with_flag_byte=*/!c->compat_2b_types);
        if (binds[i].tag == TAG_VOID) {
            nullbits[i / 8] |= (uint8_t)(1u << (i % 8));
        }
        types.push_back((char)(type & 0xff));
        types.push_back((char)(type >> 8));
        if (!c->compat_2b_types) types.push_back('\0'); // unsigned flag
    }
    auto build_packet = [&]() {
        std::string cmd;
        cmd.push_back('\x17'); // COM_STMT_EXECUTE
        for (int i = 0; i < 4; i++) cmd.push_back((char)(st.id >> (8 * i)));
        cmd.push_back('\0'); // flags: CURSOR_TYPE_NO_CURSOR
        cmd.push_back('\x01'); // iteration count = 1 (little-endian int32)
        cmd.push_back('\0');
        cmd.push_back('\0');
        cmd.push_back('\0');
        if (n > 0) {
            cmd.append(reinterpret_cast<const char*>(nullbits.data()), bitmap_bytes);
            cmd.push_back('\x01'); // new-params-bound flag
            cmd += types;
            cmd += values;
        }
        return cmd;
    };

    c->seq = 0;
    my::my_send(c, build_packet());

    // Response header: OK packet or a column count.
    std::string pk = my::my_recv(c);
    if (!pk.empty() && (uint8_t)pk[0] == 0xff && n > 0 && !c->compat_2b_types) {
        uint16_t code;
        std::string msg;
        my::my_parse_err(pk, &code, &msg);
        if (code == 1210) { // ER_WRONG_ARGUMENTS: 2-byte type layout wanted
            c->compat_2b_types = true;
            // rebuild the types block without the flag bytes and retry
            types.clear();
            for (size_t i = 0; i < n; i++) {
                uint16_t type = 0;
                std::string vbuf;
                my::my_write_param(&vbuf, &type, binds[i], /*with_flag_byte=*/false);
                (void)vbuf;
                types.push_back((char)(type & 0xff));
                types.push_back((char)(type >> 8));
            }
            c->seq = 0;
            my::my_send(c, build_packet());
            pk = my::my_recv(c);
        }
    }
    int32_t ncols;
    {
        if (pk.empty()) {
            dbsock::close_fd(c->fd);
            rt_panic("mysql: empty response to COM_STMT_EXECUTE");
        }
        uint8_t t = (uint8_t)pk[0];
        if (t == 0xff) my::my_panic_err(pk);
        if (t == 0x00) { // OK packet
            my::Payload p{pk};
            (void)p.u8();
            if (changes != nullptr) *changes = (int64_t)p.lenc();
            if (last_id != nullptr) *last_id = (int64_t)p.lenc();
            ncols = -1;
        } else {
            ncols = (int32_t)t; // column count (1 byte, always < 250)
        }
    }
    if (ncols < 0) {
        // no result set
        my_stmt_close(c, st.id);
        return;
    }
    if (out == nullptr) {
        // execute() on a statement that unexpectedly returns rows — drain
        // the rows so the connection stays usable, then panic like the
        // sqlite driver does.
        for (;;) {
            std::string pk = my::my_recv(c);
            if (pk.empty() || (uint8_t)pk[0] == 0xfe) break;
            if ((uint8_t)pk[0] == 0xff) my::my_panic_err(pk);
        }
        my_stmt_close(c, st.id);
        rt_panic("mysql: statement returned rows (use db.query() instead)");
    }
    std::vector<my::ColDef> cols = my::my_read_cols(c, ncols);
    out->col_names.reserve(cols.size());
    for (const my::ColDef& cd : cols) out->col_names.push_back(cd.name);

    // Rows (binary protocol): each row packet = null bitmap (offset 2)
    // followed by the non-null values.
    for (;;) {
        std::string pk = my::my_recv(c);
        if (pk.empty()) {
            dbsock::close_fd(c->fd);
            rt_panic("mysql: connection closed while reading rows");
        }
        uint8_t t = (uint8_t)pk[0];
        if (t == 0xff) my::my_panic_err(pk);
        if (t == 0xfe) break; // EOF — end of rows
        if (t != 0x00) continue; // not a row packet (shouldn't happen)
        my::Payload p{pk};
        (void)p.u8(); // 0x00 row header
        size_t bitmap_bytes = (cols.size() + 7 + 2) / 8;
        std::vector<uint8_t> bits(bitmap_bytes, 0);
        for (size_t i = 0; i < bitmap_bytes && !p.eof(); i++) {
            bits[i] = (uint8_t)p.u8();
        }
        auto is_null = [&](size_t i) {
            size_t idx = i + 2; // two reserved bits at the start
            return (bits[idx / 8] >> (idx % 8)) & 1;
        };
        std::vector<MValue> row;
        row.reserve(cols.size());
        for (size_t i = 0; i < cols.size(); i++) {
            if (is_null(i)) {
                row.push_back(mv_void());
                continue;
            }
            std::string v = my::my_read_value(p, cols[i]);
            row.push_back(my::my_decode_binary(cols[i], v));
        }
        out->rows.push_back(std::move(row));
    }
    my_stmt_close(c, st.id);
}

} // namespace my

void my_execute(void* p, const std::string& sql, const std::vector<MValue>& binds,
                int64_t* last_id, int64_t* changes) {
    my::MyConn* c = reinterpret_cast<my::MyConn*>(p);
    std::lock_guard<std::mutex> lk(c->mu);
    my::MyStmt st = my::my_prepare(c, sql);
    my_stmt_execute(c, st, binds, /*out=*/nullptr, last_id, changes);
}

void my_query(void* p, const std::string& sql, const std::vector<MValue>& binds,
              SqlRowSet* out) {
    my::MyConn* c = reinterpret_cast<my::MyConn*>(p);
    std::lock_guard<std::mutex> lk(c->mu);
    my::MyStmt st = my::my_prepare(c, sql);
    my_stmt_execute(c, st, binds, out, /*last_id=*/nullptr, /*changes=*/nullptr);
}

} // namespace marina_rt
