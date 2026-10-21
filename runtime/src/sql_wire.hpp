// marina runtime — internal helpers shared by the client-side database
// drivers (sql_pg.cpp, sql_mysql.cpp).
//
// Not part of the stable ABI; only included by runtime/src/*.cpp. The
// sqlite driver does not use these (libsqlite3 does everything).
//
// Provides:
//   * small hashing primitives (MD5, SHA-1, SHA-256, HMAC-SHA-256,
//     PBKDF2-HMAC-SHA-256, base64) used by the auth exchanges
//   * blocking TCP helpers with the same 30s read timeout as `net`
//
// All functions are header-only `static` so the two drivers can share
// them without adding a translation unit to the public build graph.
#ifndef MARINA_SQL_WIRE_HPP
#define MARINA_SQL_WIRE_HPP

#include "runtime_internal.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace marina_rt {

// ---------------------------------------------------------------------------
// Hashing
// ---------------------------------------------------------------------------

namespace dbhash {

inline uint32_t rotl32(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

// --- MD5 (RFC 1321) — Postgres MD5Password auth ----------------------------

inline uint32_t md5_f(int round, uint32_t b, uint32_t c, uint32_t d) {
    if (round == 0) return (b & c) | (~b & d);        // F
    if (round == 1) return (b & d) | (~d & c);        // G
    if (round == 2) return b ^ c ^ d;                 // H
    return c ^ (b | ~d);                              // I
}

inline void md5(const uint8_t* data, size_t len, uint8_t out[16]) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const unsigned R[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;
    size_t padded = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> msg(padded, 0);
    std::memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    std::memcpy(msg.data() + padded - 8, &bits, 8);
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t m[16];
        for (int i = 0; i < 16; i++) {
            std::memcpy(&m[i], msg.data() + off + i * 4, 4);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3;
        for (int i = 0; i < 64; i++) {
            int g = i;
            switch (i / 16) {
                case 1: g = (5 * i + 1) % 16; break;
                case 2: g = (3 * i + 5) % 16; break;
                case 3: g = (7 * i) % 16; break;
                default: break;
            }
            uint32_t f = md5_f(i / 16, b, c, d) + a + K[i] + m[g];
            unsigned r = R[i];
            a = d;
            d = c;
            c = b;
            b = b + rotl32(f, r);
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
    }
    std::memcpy(out, &h0, 4);
    std::memcpy(out + 4, &h1, 4);
    std::memcpy(out + 8, &h2, 4);
    std::memcpy(out + 12, &h3, 4);
}

// --- SHA family ------------------------------------------------------------

struct Sha256Ctx {
    uint32_t h[8];
    uint64_t bits = 0;
    uint8_t buf[64];
    size_t buflen = 0;
};

inline void sha256_init(Sha256Ctx& c) {
    c.h[0] = 0x6a09e667;
    c.h[1] = 0xbb67ae85;
    c.h[2] = 0x3c6ef372;
    c.h[3] = 0xa54ff53a;
    c.h[4] = 0x510e527f;
    c.h[5] = 0x9b05688c;
    c.h[6] = 0x1f83d9ab;
    c.h[7] = 0x5be0cd19;
    c.bits = 0;
    c.buflen = 0;
}

inline uint32_t load_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

inline uint64_t load_be64(const uint8_t* p) {
    return ((uint64_t)load_be32(p) << 32) | load_be32(p + 4);
}

inline void store_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

inline void sha256_block(Sha256Ctx& c, const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = load_be32(p + i * 4);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
    uint32_t e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    c.h[0] += a;
    c.h[1] += b;
    c.h[2] += cc;
    c.h[3] += d;
    c.h[4] += e;
    c.h[5] += f;
    c.h[6] += g;
    c.h[7] += h;
}

inline void sha256_update(Sha256Ctx& c, const uint8_t* data, size_t len) {
    c.bits += (uint64_t)len * 8;
    while (len > 0) {
        size_t n = 64 - c.buflen < len ? 64 - c.buflen : len;
        std::memcpy(c.buf + c.buflen, data, n);
        c.buflen += n;
        data += n;
        len -= n;
        if (c.buflen == 64) {
            sha256_block(c, c.buf);
            c.buflen = 0;
        }
    }
}

inline void sha256_final(Sha256Ctx& c, uint8_t out[32]) {
    uint8_t pad = 0x80;
    uint64_t bits = c.bits;
    sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c.buflen != 56) sha256_update(c, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i * 8));
    sha256_update(c, lenb, 8);
    for (int i = 0; i < 8; i++) store_be32(out + i * 4, c.h[i]);
}

inline void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    Sha256Ctx c;
    sha256_init(c);
    sha256_update(c, data, len);
    sha256_final(c, out);
}

struct Sha1Ctx {
    uint32_t h[5];
    uint64_t bits = 0;
    uint8_t buf[64];
    size_t buflen = 0;
};

inline void sha1_init(Sha1Ctx& c) {
    c.h[0] = 0x67452301;
    c.h[1] = 0xEFCDAB89;
    c.h[2] = 0x98BADCFE;
    c.h[3] = 0x10325476;
    c.h[4] = 0xC3D2E1F0;
    c.bits = 0;
    c.buflen = 0;
}

inline void sha1_block(Sha1Ctx& c, const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = load_be32(p + i * 4);
    for (int i = 16; i < 80; i++)
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3], e = c.h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = rotl32(b, 30);
        b = a;
        a = t;
    }
    c.h[0] += a;
    c.h[1] += b;
    c.h[2] += cc;
    c.h[3] += d;
    c.h[4] += e;
}

inline void sha1_update(Sha1Ctx& c, const uint8_t* data, size_t len) {
    c.bits += (uint64_t)len * 8;
    while (len > 0) {
        size_t n = 64 - c.buflen < len ? 64 - c.buflen : len;
        std::memcpy(c.buf + c.buflen, data, n);
        c.buflen += n;
        data += n;
        len -= n;
        if (c.buflen == 64) {
            sha1_block(c, c.buf);
            c.buflen = 0;
        }
    }
}

inline void sha1_final(Sha1Ctx& c, uint8_t out[20]) {
    uint8_t pad = 0x80;
    uint64_t bits = c.bits;
    sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c.buflen != 56) sha1_update(c, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) store_be32(out + i * 4, c.h[i]);
}

inline void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    Sha1Ctx c;
    sha1_init(c);
    sha1_update(c, data, len);
    sha1_final(c, out);
}

// --- HMAC-SHA-256 / PBKDF2 (Postgres SCRAM-SHA-256) ------------------------

inline void hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data,
                        size_t data_len, uint8_t out[32]) {
    uint8_t k0[64];
    std::memset(k0, 0, sizeof k0);
    if (key_len > 64) {
        sha256(key, key_len, k0);
    } else {
        std::memcpy(k0, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k0[i] ^ 0x36;
        opad[i] = k0[i] ^ 0x5c;
    }
    Sha256Ctx c;
    sha256_init(c);
    sha256_update(c, ipad, 64);
    sha256_update(c, data, data_len);
    uint8_t inner[32];
    sha256_final(c, inner);
    sha256_init(c);
    sha256_update(c, opad, 64);
    sha256_update(c, inner, 32);
    sha256_final(c, out);
}

inline void pbkdf2_sha256(const std::string& password, const uint8_t* salt, size_t salt_len,
                          uint32_t iterations, uint8_t out[32]) {
    // one block, dkLen = 32
    uint8_t block[32];
    uint8_t u[32];
    std::string msg;
    msg.reserve(salt_len + 4);
    msg.append(reinterpret_cast<const char*>(salt), salt_len);
    msg.push_back(0);
    msg.push_back(0);
    msg.push_back(0);
    msg.push_back(1);
    hmac_sha256(reinterpret_cast<const uint8_t*>(password.data()), password.size(),
                reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), block);
    std::memcpy(u, block, 32);
    for (uint32_t i = 1; i < iterations; i++) {
        hmac_sha256(reinterpret_cast<const uint8_t*>(password.data()), password.size(), u,
                    32, u);
        for (int j = 0; j < 32; j++) block[j] ^= u[j];
    }
    std::memcpy(out, block, 32);
}

inline std::string hex_encode(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0xf]);
    }
    return out;
}

inline std::string md5_hex(const uint8_t* data, size_t len) {
    uint8_t d[16];
    md5(data, len, d);
    return hex_encode(d, 16);
}

inline std::string sha1_hex(const uint8_t* data, size_t len) {
    uint8_t d[20];
    sha1(data, len, d);
    return hex_encode(d, 20);
}

inline std::string sha256_hex_(const uint8_t* data, size_t len) {
    uint8_t d[32];
    sha256(data, len, d);
    return hex_encode(d, 32);
}

// --- base64 (SCRAM) --------------------------------------------------------

inline std::string b64_enc(const uint8_t* data, size_t len) {
    static const char* tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out.push_back(tab[(v >> 18) & 63]);
        out.push_back(tab[(v >> 12) & 63]);
        out.push_back(i + 1 < len ? tab[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? tab[v & 63] : '=');
    }
    return out;
}

inline int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

inline std::vector<uint8_t> b64_dec(const std::string& s) {
    std::vector<uint8_t> out;
    int acc = 0, bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = b64_val(c);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(acc >> bits));
        }
    }
    return out;
}

inline std::string b64_enc_str(const std::string& s) {
    return b64_enc(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

} // namespace dbhash

// ---------------------------------------------------------------------------
// Blocking TCP with a 30s read timeout (same policy as `net`)
// ---------------------------------------------------------------------------

namespace dbsock {

// Resolve + connect; panics on failure. Returns the fd.
inline int tcp_connect(const std::string& host, uint16_t port, const std::string& what) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string service = std::to_string(port);
    int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
    if (rc != 0) {
        rt_panic(what + ": cannot resolve '" + host + "': " +
                 (rc == EAI_SYSTEM ? os_error(errno) : std::string(gai_strerror(rc))));
    }
    int fd = -1;
    std::string last_err;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        int c = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (c < 0) {
            last_err = os_error(errno);
            continue;
        }
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ::setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (::connect(c, ai->ai_addr, ai->ai_addrlen) == 0) {
            fd = c;
            break;
        }
        last_err = os_error(errno);
        ::close(c);
    }
    ::freeaddrinfo(res);
    if (fd < 0) {
        rt_panic(what + ": cannot connect to '" + host + ":" + service + "' (" + last_err +
                 ")");
    }
    return fd;
}

inline void write_all(int fd, const uint8_t* data, size_t len, const std::string& what) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            ::close(fd);
            rt_panic(what + ": write failed (" + os_error(e) + ")");
        }
        off += (size_t)n;
    }
}

inline void write_all(int fd, const std::string& bytes, const std::string& what) {
    write_all(fd, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), what);
}

// Read exactly `len` bytes; panics on EOF / error / timeout.
inline void read_full(int fd, uint8_t* buf, size_t len, const std::string& what) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::read(fd, buf + off, len - off);
        if (n == 0) {
            ::close(fd);
            rt_panic(what + ": connection closed by server");
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            ::close(fd);
            rt_panic(what + ": read failed (" + os_error(e) + ")");
        }
        off += (size_t)n;
    }
}

// Read the whole socket until EOF (used to drain a server goodbye).
inline void read_until_eof(int fd, std::string* out) {
    char tmp[8192];
    for (;;) {
        ssize_t n = ::read(fd, tmp, sizeof tmp);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        out->append(tmp, (size_t)n);
    }
}

inline void close_fd(int fd) { ::close(fd); }

// /dev/urandom-backed randomness for auth nonces.
inline std::string random_hex(size_t nbytes) {
    std::vector<uint8_t> buf(nbytes);
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t off = 0;
        while (off < buf.size()) {
            ssize_t n = ::read(fd, buf.data() + off, buf.size() - off);
            if (n <= 0) break;
            off += (size_t)n;
        }
        ::close(fd);
    }
    return dbhash::hex_encode(buf.data(), buf.size());
}

} // namespace dbsock

} // namespace marina_rt

#endif // MARINA_SQL_WIRE_HPP
