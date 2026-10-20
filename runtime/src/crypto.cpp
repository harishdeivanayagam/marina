// marina runtime — std module: crypto (pure implementation, no external
// libraries): SHA-256 (byte-for-byte port of the Rust version), base64
// encode/decode (same alphabet/padding rules), /dev/urandom-backed
// random_hex and UUID v4.
#include "runtime_internal.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace marina_rt {

namespace {

constexpr uint32_t SHA256_K[64] = {
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
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

std::string hex32(uint32_t x) {
    char buf[9];
    std::snprintf(buf, sizeof buf, "%08x", x);
    return std::string(buf);
}

std::string hex8(uint8_t x) {
    char buf[3];
    std::snprintf(buf, sizeof buf, "%02x", x);
    return std::string(buf);
}

std::string sha256_hex(const uint8_t* data, size_t len) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t bitlen = (uint64_t)len * 8; // wrapping in Rust; same in C++
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int k = 7; k >= 0; k--) msg.push_back((uint8_t)(bitlen >> (8 * k))); // to_be_bytes

    for (size_t off = 0; off < msg.size(); off += 64) {
        const uint8_t* chunk = msg.data() + off;
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)chunk[4 * i] << 24) | ((uint32_t)chunk[4 * i + 1] << 16) |
                   ((uint32_t)chunk[4 * i + 2] << 8) | (uint32_t)chunk[4 * i + 3];
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1; // uint32 wraps like wrapping_add
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + s1 + ch + SHA256_K[i] + w[i];
            uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    std::string out;
    for (int i = 0; i < 8; i++) out += hex32(h[i]);
    return out;
}

constexpr const char B64_CHARS[65] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const uint8_t* data, size_t len) {
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        size_t chunk_len = (len - i >= 3) ? 3 : (len - i);
        uint32_t b0 = data[i];
        uint32_t b1 = chunk_len > 1 ? data[i + 1] : 0;
        uint32_t b2 = chunk_len > 2 ? data[i + 2] : 0;
        uint32_t n = (b0 << 16) | (b1 << 8) | b2;
        out += B64_CHARS[(n >> 18) & 63];
        out += B64_CHARS[(n >> 12) & 63];
        out += chunk_len > 1 ? B64_CHARS[(n >> 6) & 63] : '=';
        out += chunk_len > 2 ? B64_CHARS[n & 63] : '=';
    }
    return out;
}

std::string b64_decode(const std::string& s) {
    std::vector<uint32_t> vals;
    for (char ch : s) {
        uint8_t c = (uint8_t)ch;
        uint32_t v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else continue; // '=', whitespace
        vals.push_back(v);
    }
    std::vector<uint8_t> out;
    for (size_t i = 0; i < vals.size(); i += 4) {
        size_t chunk_len = vals.size() - i < 4 ? vals.size() - i : 4;
        uint32_t n = vals[i] << 18;
        if (chunk_len > 1) n |= vals[i + 1] << 12;
        if (chunk_len > 2) n |= vals[i + 2] << 6;
        if (chunk_len > 3) n |= vals[i + 3];
        out.push_back((uint8_t)((n >> 16) & 0xff));
        if (chunk_len > 2) out.push_back((uint8_t)((n >> 8) & 0xff));
        if (chunk_len > 3) out.push_back((uint8_t)(n & 0xff));
    }
    return utf8_lossy(out.data(), out.size());
}

std::vector<uint8_t> random_bytes(size_t n) {
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        rt_panic("crypto: cannot open /dev/urandom (" + os_error(errno) + ")");
    }
    std::vector<uint8_t> buf(n);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, buf.data() + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            ::close(fd);
            rt_panic("crypto: cannot read /dev/urandom (" + os_error(e) + ")");
        }
        if (r == 0) {
            ::close(fd);
            // Rust's read_exact yields UnexpectedEof("failed to fill whole buffer")
            rt_panic("crypto: cannot read /dev/urandom (failed to fill whole buffer)");
        }
        got += (size_t)r;
    }
    ::close(fd);
    return buf;
}

} // namespace

} // namespace marina_rt

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

extern "C" void marina_crypto_sha256(const MValue* data, MValue* out) {
    std::string d = marina_rt::mv_to_string(*data);
    marina_rt::out_mv(
        out, marina_rt::mv_str_owned(marina_rt::sha256_hex(
                 reinterpret_cast<const uint8_t*>(d.data()), d.size())));
}

extern "C" void marina_crypto_base64(bool decode, const MValue* data, MValue* out) {
    std::string d = marina_rt::mv_to_string(*data);
    std::string result;
    if (decode) {
        result = marina_rt::b64_decode(d);
    } else {
        result = marina_rt::b64_encode(
            reinterpret_cast<const uint8_t*>(d.data()), d.size());
    }
    marina_rt::out_mv(out, marina_rt::mv_str_owned(result));
}

extern "C" void marina_crypto_random_hex(int64_t num_bytes, MValue* out) {
    size_t n = (size_t)(num_bytes < 0 ? 0 : (num_bytes > 4096 ? 4096 : num_bytes));
    std::vector<uint8_t> bytes = marina_rt::random_bytes(n);
    std::string hex;
    for (uint8_t b : bytes) hex += marina_rt::hex8(b);
    marina_rt::out_mv(out, marina_rt::mv_str_owned(hex));
}

extern "C" void marina_crypto_uuid(MValue* out) {
    std::vector<uint8_t> b = marina_rt::random_bytes(16);
    b[6] = (b[6] & 0x0f) | 0x40; // version 4
    b[8] = (b[8] & 0x3f) | 0x80; // variant 10xx
    std::string hex;
    for (uint8_t x : b) hex += marina_rt::hex8(x);
    std::string uuid = hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" +
                       hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
                       hex.substr(20, 12);
    marina_rt::out_mv(out, marina_rt::mv_str_owned(uuid));
}