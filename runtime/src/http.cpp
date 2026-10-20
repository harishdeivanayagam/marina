// marina runtime — std module: http (client + server), built on net.
//
// * client: http_parse_url / http_do_request (same wire format)
// * blocking server: marina_http_listen (sequential accept loop)
// * evented server: marina_http_server_listen (poll(2) multiplexing with
//   a socketpair wakeup so responses retire connections immediately)
// * express-style layer: routers, routes, JsonValidator
// * JSON serialization + the minimal JSON parser used by validators.
#include "runtime_internal.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace marina_rt {

namespace {

// ---------------------------------------------------------------------------
// Small ASCII helpers (Rust's char::is_ascii_whitespace etc.)
// ---------------------------------------------------------------------------

inline bool isspace_ascii(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\x0b' || c == '\x0c';
}
inline bool isdigit_ascii(unsigned char c) { return c >= '0' && c <= '9'; }
inline int tolower_ascii(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

void write_all_ignore_errors(int fd, const std::string& bytes) {
    size_t off = 0;
    while (off < bytes.size()) {
        ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;
        }
        off += (size_t)n;
    }
}

bool http_debug() { return std::getenv("MARINA_HTTP_DEBUG") != nullptr; }

void set_nonblocking(int fd, bool nb) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0) return;
    if (nb) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    else ::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
}

void set_read_timeout_30s(int fd) {
    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

// Split a header line into whitespace-separated tokens.
std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isspace_ascii((unsigned char)s[i])) i++;
        size_t st = i;
        while (i < s.size() && !isspace_ascii((unsigned char)s[i])) i++;
        if (i > st) parts.push_back(s.substr(st, i - st));
    }
    return parts;
}

// Split on "\r\n" (Rust's `head.split("\r\n")`).
std::vector<std::string> split_crlf(const std::string& s) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (;;) {
        size_t p = s.find("\r\n", start);
        if (p == std::string::npos) {
            lines.push_back(s.substr(start));
            break;
        }
        lines.push_back(s.substr(start, p - start));
        start = p + 2;
    }
    return lines;
}

// Parse `content-length:` style headers (lowercase prefix, trimmed value).
size_t content_length_from(const std::vector<std::string>& lines) {
    size_t content_len = 0;
    const std::string prefix = "content-length:";
    for (size_t li = 1; li < lines.size(); li++) {
        std::string low = lines[li];
        for (char& ch : low) ch = (char)tolower_ascii((unsigned char)ch);
        if (low.compare(0, prefix.size(), prefix) == 0) {
            std::string v = low.substr(prefix.size());
            size_t b = 0;
            while (b < v.size() && isspace_ascii((unsigned char)v[b])) b++;
            size_t e = v.size();
            while (e > b && isspace_ascii((unsigned char)v[e - 1])) e--;
            size_t n = 0;
            // Rust: v.trim().parse().unwrap_or(0)
            if (parse_usize_strict(v.substr(b, e - b), &n)) content_len = n;
            else content_len = 0;
        }
    }
    return content_len;
}

// ---------------------------------------------------------------------------
// bind with SO_REUSEADDR (so a quick server restart doesn't hit
// EADDRINUSE against lingering TIME-WAIT sockets)
// ---------------------------------------------------------------------------

int bind_tcp_reuse(const std::string& host, uint16_t port) {
    // Returns a listener fd, or -1 with errno set.
    in_addr ip;
    if (::inet_pton(AF_INET, host.c_str(), &ip) == 1) {
        // Literal IPv4 host: raw socket path with SO_REUSEADDR.
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr = ip;
        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa) != 0) {
            int e = errno;
            ::close(fd);
            errno = e;
            return -1;
        }
        if (::listen(fd, 128) != 0) {
            int e = errno;
            ::close(fd);
            errno = e;
            return -1;
        }
        return fd;
    }
    // Non-literal host (hostname): resolve and bind (no SO_REUSEADDR),
    // like std's TcpListener::bind.
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    struct addrinfo* res = nullptr;
    std::string service = std::to_string(port);
    if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0) {
        errno = EIO;
        return -1;
    }
    int fd = -1;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        int c = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (c < 0) continue;
        if (::bind(c, ai->ai_addr, ai->ai_addrlen) != 0) {
            ::close(c);
            continue;
        }
        if (::listen(c, 128) != 0) {
            ::close(c);
            continue;
        }
        fd = c;
        break;
    }
    ::freeaddrinfo(res);
    if (fd < 0 && errno == 0) errno = EADDRINUSE;
    return fd;
}

// ---------------------------------------------------------------------------
// Self-pipe: http_respond (running on worker threads) nudges the evented
// listener loop the moment a response is written, so the connection is
// retired (fd closed) without waiting for the poll timeout.
// ---------------------------------------------------------------------------

std::atomic<int>& wake_tx_fd() {
    static std::atomic<int>* f = new std::atomic<int>(-1); // leaked
    return *f;
}

void wake_listener() {
    int w = wake_tx_fd().load(std::memory_order_relaxed);
    if (w >= 0) {
        // keep the socket non-blocking (shared descriptor); ignore errors
        int fl = ::fcntl(w, F_GETFL, 0);
        if (fl >= 0) ::fcntl(w, F_SETFL, fl | O_NONBLOCK);
        char byte = 1;
        ssize_t rc = ::write(w, &byte, 1);
        (void)rc;
    }
}

// ---------------------------------------------------------------------------
// res sockets: sockets backing `res` objects of in-flight handler
// invocations, keyed by the res object address.
// ---------------------------------------------------------------------------

struct ResSockets {
    std::mutex mu;
    std::unordered_map<size_t, int> fds;
};
ResSockets& res_sockets() {
    static ResSockets* r = new ResSockets(); // leaked
    return *r;
}

[[noreturn]] void panic_http_listen_port(uint16_t port_n, const std::string& what) {
    rt_panic("http: cannot listen on port " + std::to_string(port_n) + " (" + what + ")");
}

[[noreturn]] void panic_http_listen_host(const std::string& host_s, uint16_t port_n,
                                         const std::string& what) {
    rt_panic("http: cannot listen on " + host_s + ":" + std::to_string(port_n) + " (" +
             what + ")");
}

// ---------------------------------------------------------------------------
// JSON serialization (for res.json)
// ---------------------------------------------------------------------------

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
    size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        // s is valid UTF-8 (lossy-decoded); walk Unicode scalars.
        uint8_t b = p[i];
        size_t len = (b < 0x80) ? 1 : (b >= 0xF0 ? 4 : (b >= 0xE0 ? 3 : 2));
        uint32_t c = b;
        if (len > 1) {
            c = b & (len == 2 ? 0x1Fu : (len == 3 ? 0x0Fu : 0x07u));
            bool ok = (i + len <= n);
            for (size_t k = 1; ok && k < len; k++) {
                if ((p[i + k] & 0xC0) != 0x80) {
                    ok = false;
                } else {
                    c = (c << 6) | (p[i + k] & 0x3F);
                }
            }
            if (!ok) len = 1;
        }
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out.append(reinterpret_cast<const char*>(p) + i, len);
        }
        i += len;
    }
    out += '"';
    return out;
}

std::string json_value(const MValue& v) {
    switch (v.tag) {
        case TAG_STRING:
            return json_escape(mv_to_string(v));
        case TAG_INT:
            return fmt_tagged_int(v);
        case TAG_FLOAT:
            return fmt_tagged_float(v);
        case TAG_BOOL:
            return v.i != 0 ? "true" : "false";
        case TAG_ARRAY: {
            const ArrayObj* arr = reinterpret_cast<const ArrayObj*>(v.p);
            std::string out = "[";
            for (size_t i = 0; i < arr->data.size(); i++) {
                if (i) out += ",";
                out += json_value(arr->data[i]);
            }
            return out + "]";
        }
        case TAG_OBJECT: {
            const ObjectObj* obj = reinterpret_cast<const ObjectObj*>(v.p);
            std::lock_guard<std::mutex> lk(class_mutex());
            auto it = class_map().find(obj->class_id);
            std::string out = "{";
            if (it != class_map().end()) {
                const ClassInfo& ci = it->second;
                for (size_t i = 0; i < obj->data.size(); i++) {
                    if (i) out += ",";
                    std::string name = i < ci.fields.size() ? ci.fields[i] : "?";
                    out += json_escape(name) + ":" + json_value(obj->data[i]);
                }
            }
            return out + "}";
        }
        default:
            return "null";
    }
}

// ---------------------------------------------------------------------------
// Minimal JSON parser (for JsonValidator.validate)
// ---------------------------------------------------------------------------

struct JVal {
    enum Kind { Null, Bool, Num, Str, Arr, Obj };
    Kind k = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s)
        : b_(reinterpret_cast<const uint8_t*>(s.data())), n_(s.size()) {}

    std::optional<JVal> parse() {
        std::optional<JVal> v = value();
        if (!v.has_value()) return std::nullopt;
        skip_ws();
        if (i_ != n_) return std::nullopt;
        return v;
    }

private:
    const uint8_t* b_;
    size_t n_;
    size_t i_ = 0;

    void skip_ws() {
        while (i_ < n_ && isspace_ascii(b_[i_])) i_++;
    }
    bool peek(uint8_t* out) const {
        if (i_ >= n_) return false;
        *out = b_[i_];
        return true;
    }
    bool eat(uint8_t c) {
        uint8_t p;
        if (peek(&p) && p == c) {
            i_++;
            return true;
        }
        return false;
    }

    std::optional<JVal> value() {
        skip_ws();
        uint8_t c;
        if (!peek(&c)) return std::nullopt;
        switch (c) {
            case '{': return object();
            case '[': return array();
            case '"': {
                auto s = string();
                if (!s.has_value()) return std::nullopt;
                JVal v;
                v.k = JVal::Str;
                v.str = std::move(*s);
                return v;
            }
            case 't': return lit("true", JVal::Bool, true);
            case 'f': return lit("false", JVal::Bool, false);
            case 'n': return lit("null", JVal::Null, false);
            default: return number();
        }
    }

    std::optional<JVal> lit(const char* word, JVal::Kind k, bool b) {
        size_t len = std::strlen(word);
        if (i_ + len <= n_ && std::memcmp(b_ + i_, word, len) == 0) {
            i_ += len;
            JVal v;
            v.k = k;
            v.b = b;
            return v;
        }
        return std::nullopt;
    }

    std::optional<std::string> string() {
        if (!eat('"')) return std::nullopt;
        std::string out;
        for (;;) {
            if (i_ >= n_) return std::nullopt;
            uint8_t c = b_[i_];
            i_ += 1;
            if (c == '"') break;
            if (c == '\\') {
                if (i_ >= n_) return std::nullopt;
                uint8_t e = b_[i_];
                i_ += 1;
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (i_ + 4 > n_) return std::nullopt;
                        uint32_t nv = 0;
                        for (int k = 0; k < 4; k++) {
                            uint8_t h = b_[i_ + k];
                            uint32_t d;
                            if (h >= '0' && h <= '9') d = h - '0';
                            else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                            else return std::nullopt;
                            nv = nv * 16 + d;
                        }
                        i_ += 4;
                        // char::from_u32(n).unwrap_or('\u{FFFD}')
                        uint32_t ch = nv;
                        if (nv > 0x10FFFF || (nv >= 0xD800 && nv <= 0xDFFF)) ch = 0xFFFD;
                        if (ch < 0x80) out += (char)ch;
                        else if (ch < 0x800) {
                            out += (char)(0xC0 | (ch >> 6));
                            out += (char)(0x80 | (ch & 0x3F));
                        } else if (ch < 0x10000) {
                            out += (char)(0xE0 | (ch >> 12));
                            out += (char)(0x80 | ((ch >> 6) & 0x3F));
                            out += (char)(0x80 | (ch & 0x3F));
                        } else {
                            out += (char)(0xF0 | (ch >> 18));
                            out += (char)(0x80 | ((ch >> 12) & 0x3F));
                            out += (char)(0x80 | ((ch >> 6) & 0x3F));
                            out += (char)(0x80 | (ch & 0x3F));
                        }
                        break;
                    }
                    default:
                        return std::nullopt;
                }
            } else {
                out += (char)c;
            }
        }
        return utf8_lossy(reinterpret_cast<const uint8_t*>(out.data()), out.size());
    }

    static bool mantissa_ok(const std::string& m) {
        // Digits [ '.' [digits] ] — digits required before '.', at least
        // one digit overall (checked by the caller).
        size_t i = 0;
        while (i < m.size() && isdigit_ascii((unsigned char)m[i])) i++;
        if (i < m.size() && m[i] == '.') {
            i++;
            while (i < m.size() && isdigit_ascii((unsigned char)m[i])) i++;
        }
        return i == m.size();
    }

    std::optional<JVal> number() {
        size_t start = i_;
        eat('-');
        while (i_ < n_) {
            uint8_t c = b_[i_];
            if (isdigit_ascii(c) || c == '.' || c == 'e' || c == 'E' || c == '+' ||
                c == '-') {
                i_++;
            } else {
                break;
            }
        }
        if (start == i_) return std::nullopt;
        std::string s(reinterpret_cast<const char*>(b_) + start, i_ - start);
        JVal v;
        v.k = JVal::Num;
        // Match Rust f64 FromStr acceptance.
        size_t q = 0;
        if (q < s.size() && (s[q] == '+' || s[q] == '-')) q++;
        std::string rest = s.substr(q);
        for (char& ch : rest) ch = (char)tolower_ascii((unsigned char)ch);
        if (rest == "inf" || rest == "infinity" || rest == "nan") {
            if (rest == "nan") v.num = std::nan("");
            else v.num = (s[0] == '-') ? -HUGE_VAL : HUGE_VAL;
            return v;
        }
        if (rest.empty() || !isdigit_ascii((unsigned char)rest[0])) {
            return std::nullopt; // Rust rejects ".5" and "-.5"
        }
        // Validate shape: digits['.'[digits]][e[sign]digits] — Rust
        // requires exponent digits when 'e' is present.
        size_t epos = rest.find('e');
        if (epos != std::string::npos) {
            std::string mant = rest.substr(0, epos);
            std::string exp = rest.substr(epos + 1);
            size_t j = 0;
            if (j < exp.size() && (exp[j] == '+' || exp[j] == '-')) j++;
            if (j >= exp.size()) return std::nullopt; // "1e" rejected
            for (; j < exp.size(); j++) {
                if (!isdigit_ascii((unsigned char)exp[j])) return std::nullopt;
            }
            if (!mantissa_ok(mant)) return std::nullopt;
        } else {
            if (!mantissa_ok(rest)) return std::nullopt;
        }
        v.num = std::strtod(s.c_str(), nullptr);
        return v;
    }

    std::optional<JVal> array() {
        if (!eat('[')) return std::nullopt;
        JVal v;
        v.k = JVal::Arr;
        skip_ws();
        if (eat(']')) return v;
        for (;;) {
            std::optional<JVal> e = value();
            if (!e.has_value()) return std::nullopt;
            v.arr.push_back(std::move(*e));
            skip_ws();
            if (eat(']')) break;
            if (!eat(',')) return std::nullopt;
        }
        return v;
    }

    std::optional<JVal> object() {
        if (!eat('{')) return std::nullopt;
        JVal v;
        v.k = JVal::Obj;
        skip_ws();
        if (eat('}')) return v;
        for (;;) {
            skip_ws();
            std::optional<std::string> key = string();
            if (!key.has_value()) return std::nullopt;
            skip_ws();
            if (!eat(':')) return std::nullopt;
            std::optional<JVal> val = value();
            if (!val.has_value()) return std::nullopt;
            v.obj.emplace_back(std::move(*key), std::move(*val));
            skip_ws();
            if (eat('}')) break;
            if (!eat(',')) return std::nullopt;
        }
        return v;
    }
};

std::optional<JVal> json_parse(const std::string& s) {
    JsonParser p(s);
    return p.parse();
}

// ---------------------------------------------------------------------------
// express-style layer (Server / Router / JsonValidator)
// ---------------------------------------------------------------------------

// One field validator spec (`http.JsonValidator.string().required().min(n)...`)
struct FieldSpec {
    // 0 = string, 1 = number
    uint8_t kind = 0;
    bool required = false;
    std::optional<int64_t> min;
    std::optional<int64_t> max;
};

// router object ptr -> path prefix
struct Routers {
    std::mutex mu;
    std::unordered_map<size_t, std::string> map;
};
Routers& http_routers() {
    static Routers* r = new Routers(); // leaked
    return *r;
}

// A registered route handler.
struct HttpRoute {
    std::string method;
    // full matched path (prefix + route path; prefix itself for "/")
    std::string exact;
    // router prefix (also matched for "/" routes, express-style)
    std::string prefix;
    MarinaListenCb f = nullptr;
    void* env = nullptr;
    // handler is an async lambda: it returns a promise and responds later
    bool is_async = false;
};

struct Routes {
    std::mutex mu;
    std::vector<HttpRoute> routes;
};
Routes& http_routes() {
    static Routes* r = new Routes(); // leaked
    return *r;
}

// field-validator object ptr -> spec
struct FieldSpecs {
    std::mutex mu;
    std::unordered_map<size_t, FieldSpec> map;
};
FieldSpecs& http_field_specs() {
    static FieldSpecs* f = new FieldSpecs(); // leaked
    return *f;
}

// JsonValidator object ptr -> ordered (field name, spec)
struct ValidatorSpecs {
    std::mutex mu;
    std::unordered_map<size_t, std::vector<std::pair<std::string, FieldSpec>>> map;
};
ValidatorSpecs& http_validator_specs() {
    static ValidatorSpecs* v = new ValidatorSpecs(); // leaked
    return *v;
}

// ---------------------------------------------------------------------------
// Shared HTTP response writing
// ---------------------------------------------------------------------------

void http_respond(void* res, const MValue* value, bool json) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lk(res_sockets().mu);
        auto it = res_sockets().fds.find(reinterpret_cast<size_t>(res));
        if (it != res_sockets().fds.end()) {
            fd = ::dup(it->second);
            res_sockets().fds.erase(it);
        }
    }
    if (fd < 0) {
        // no socket behind this response (e.g. called outside a handler):
        // fall back to printing
        std::cerr << value_to_string(*value) << "\n";
        return;
    }
    // The socket is (and stays) non-blocking: dup'd descriptors share the
    // O_NONBLOCK flag with the event loop's read side, so we must NOT
    // flip it to blocking here. Short/EAGAIN writes wait via poll(POLLOUT).
    const char* ctype = json ? "application/json" : "text/plain";
    std::string payload = json ? json_value(*value) : value_to_string(*value);
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: ";
    resp += ctype;
    resp += "\r\nContent-Length: ";
    resp += std::to_string(payload.size());
    resp += "\r\nConnection: keep-alive\r\n\r\n";
    resp += payload;
    {
        const char* bytes = resp.data();
        size_t len = resp.size();
        size_t off = 0;
        while (off < len) {
            ssize_t n = ::write(fd, bytes + off, len - off);
            if (n == 0) break;
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct pollfd pfd;
                    pfd.fd = fd;
                    pfd.events = POLLOUT;
                    pfd.revents = 0;
                    int r = ::poll(&pfd, 1, 5000);
                    if (r <= 0) break; // peer gone or poll failed
                    continue;
                }
                break;
            }
            off += (size_t)n;
        }
    }
    if (http_debug()) {
        std::cerr << "[evloop] responded " << resp.size() << " bytes\n";
    }
    // nudge the evented listener loop so the connection is retired now
    wake_listener();
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

struct ParsedUrl {
    std::string host;
    uint16_t port;
    std::string path;
};

ParsedUrl http_parse_url(const std::string& url) {
    const std::string scheme = "http://";
    if (url.compare(0, scheme.size(), scheme) != 0) {
        rt_panic("http: only http:// URLs are supported (https is not available yet), got '" +
                 url + "'");
    }
    std::string rest = url.substr(scheme.size());
    std::string hostport, path;
    size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        hostport = rest.substr(0, slash);
        path = rest.substr(slash);
    } else {
        hostport = rest;
        path = "/";
    }
    std::string host;
    uint16_t port;
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        size_t n = 0;
        if (!parse_usize_strict(hostport.substr(colon + 1), &n) || n > 65535) {
            rt_panic("http: invalid port in url '" + url + "'");
        }
        host = hostport.substr(0, colon);
        port = (uint16_t)n;
    } else {
        host = hostport;
        port = 80;
    }
    if (host.empty()) {
        rt_panic("http: missing host in url '" + url + "'");
    }
    return ParsedUrl{host, port, path};
}

// Perform a request using the net std module (connect -> write ->
// read_all -> close), then parse the response into (status, body).
std::pair<int64_t, std::string> http_do_request(const std::string& method,
                                                const std::string& url,
                                                std::optional<std::string> body) {
    ParsedUrl u = http_parse_url(url);
    std::string req = method + " " + u.path +
                      " HTTP/1.1\r\nHost: " + u.host +
                      "\r\nUser-Agent: marina-http/0.2\r\nConnection: close\r\n";
    if (body.has_value()) {
        req += "Content-Type: text/plain\r\nContent-Length: " +
               std::to_string(body->size()) + "\r\n\r\n" + *body;
    } else {
        req += "Content-Length: 0\r\n\r\n";
    }

    MValue host_mv = mv_str_owned(u.host);
    MValue port_mv = mv_int((int64_t)u.port);
    MValue st_mv = mv_void();
    marina_net_connect(&host_mv, &port_mv, &st_mv);
    drop_mv(host_mv);
    MValue req_mv = mv_str_owned(req);
    marina_net_write(&st_mv, &req_mv);
    drop_mv(req_mv);
    MValue resp_mv = mv_void();
    marina_net_read_all(&st_mv, &resp_mv);
    marina_net_close(&st_mv);
    std::string resp = mv_to_string(resp_mv);
    drop_mv(resp_mv);

    std::string head, body_s;
    size_t sep = resp.find("\r\n\r\n");
    if (sep != std::string::npos) {
        head = resp.substr(0, sep);
        body_s = resp.substr(sep + 4);
    } else {
        head = resp;
        body_s = "";
    }
    int64_t status = 0;
    {
        std::vector<std::string> tokens = split_ws(head);
        // split_whitespace().nth(1)
        if (tokens.size() > 1) {
            if (!parse_i64_strict(tokens[1], &status)) status = 0;
        }
    }
    return {status, body_s};
}

// ---------------------------------------------------------------------------
// Blocking-variant HTTP server (http.listen)
// ---------------------------------------------------------------------------

struct ListenJob {
    void* env;
    MarinaListenCb f;
    uint16_t port_n;
    int32_t req_class_id;
    int32_t res_class_id;
};

// Read one HTTP request from a connection: request line + headers + body.
std::optional<std::tuple<std::string, std::string, std::string>> read_http_request(int fd) {
    set_read_timeout_30s(fd);
    std::vector<uint8_t> buf;
    uint8_t one;
    for (;;) {
        ssize_t n = ::read(fd, &one, 1);
        if (n == 0) return std::nullopt;
        if (n < 0) return std::nullopt;
        buf.push_back(one);
        if (buf.size() >= 4 && buf[buf.size() - 4] == '\r' && buf[buf.size() - 3] == '\n' &&
            buf[buf.size() - 2] == '\r' && buf[buf.size() - 1] == '\n') {
            break;
        }
        if (buf.size() > 64 * 1024) return std::nullopt;
    }
    std::string head = utf8_lossy(buf.data(), buf.size());
    std::vector<std::string> lines = split_crlf(head);
    std::string reqline = lines.empty() ? "" : lines[0];
    std::vector<std::string> parts = split_ws(reqline);
    std::string method = parts.empty() ? "" : parts[0];
    std::string path = parts.size() > 1 ? parts[1] : "/";
    size_t content_len = content_length_from(lines);
    if (content_len > 16 * 1024 * 1024) return std::nullopt;
    std::string body_bytes(content_len, '\0');
    size_t got = 0;
    while (got < content_len) {
        ssize_t n = ::read(fd, &body_bytes[got], content_len - got);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return std::nullopt;
        }
        got += (size_t)n;
    }
    return std::make_tuple(
        method, path,
        utf8_lossy(reinterpret_cast<const uint8_t*>(body_bytes.data()),
                   body_bytes.size()));
}

void blocking_listen_thread(ListenJob job) {
    int listener = bind_tcp_reuse("0.0.0.0", job.port_n);
    if (listener < 0) {
        panic_http_listen_port(job.port_n, os_error(errno));
    }
    for (;;) {
        int conn = ::accept(listener, nullptr, nullptr);
        if (conn < 0) continue;
        auto req = read_http_request(conn);
        if (!req.has_value()) {
            ::close(conn);
            continue;
        }
        auto [method, path, body] = *req;
        // request object { method, path, body }
        void* req_o = marina_object_new(job.req_class_id, 3);
        set_object_field(req_o, 0, mv_str_owned(method));
        set_object_field(req_o, 1, mv_str_owned(path));
        set_object_field(req_o, 2, mv_str_owned(body));
        MValue req_mv{TAG_OBJECT, 0, 0, 0.0, req_o};
        // response object (no fields; send/json write to the socket)
        void* res_o = marina_object_new(job.res_class_id, 0);
        MValue res_mv{TAG_OBJECT, 0, 0, 0.0, res_o};
        int clone = ::dup(conn);
        if (clone >= 0) {
            std::lock_guard<std::mutex> lk(res_sockets().mu);
            res_sockets().fds.emplace(reinterpret_cast<size_t>(res_o), clone);
        }
        // ownership: both objects are handed to the handler with rc 1;
        // the compiled handler releases them when it returns.
        job.f(job.env, &req_mv, &res_mv);
        {
            std::lock_guard<std::mutex> lk(res_sockets().mu);
            auto it = res_sockets().fds.find(reinterpret_cast<size_t>(res_o));
            if (it != res_sockets().fds.end()) {
                ::close(it->second);
                res_sockets().fds.erase(it);
            }
        }
        ::close(conn);
    }
}

// ---------------------------------------------------------------------------
// Evented HTTP server (server.listen)
// ---------------------------------------------------------------------------

// Incrementally parse an HTTP request from an in-progress byte buffer.
// Return: 0 = incomplete, 1 = malformed/oversized, 2 = full request.
int try_parse_http(const std::vector<uint8_t>& buf, std::string* method_out,
                   std::string* path_out, std::string* body_out, size_t* consumed_out) {
    // find "\r\n\r\n"
    size_t head_end = std::string::npos;
    if (buf.size() >= 4) {
        for (size_t i = 0; i + 4 <= buf.size(); i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
                buf[i + 3] == '\n') {
                head_end = i;
                break;
            }
        }
    }
    if (head_end == std::string::npos) {
        if (buf.size() > 64 * 1024) return 1; // header too large
        return 0;                             // incomplete
    }
    std::string head = utf8_lossy(buf.data(), head_end);
    std::vector<std::string> lines = split_crlf(head);
    std::string reqline = lines.empty() ? "" : lines[0];
    std::vector<std::string> parts = split_ws(reqline);
    std::string method = parts.empty() ? "" : parts[0];
    std::string path = parts.size() > 1 ? parts[1] : "/";
    if (method.empty() || parts.size() < 3) {
        return 1; // malformed request line
    }
    size_t content_len = content_length_from(lines);
    if (content_len > 16 * 1024 * 1024) return 1; // body too large
    size_t total = head_end + 4 + content_len;
    if (buf.size() < total) return 0; // incomplete body
    *body_out = utf8_lossy(buf.data() + head_end + 4, content_len);
    *method_out = method;
    *path_out = path;
    *consumed_out = total;
    return 2;
}

void evented_listen_thread(std::string host_s, uint16_t port_n, void* promise,
                           int32_t req_class_id, int32_t res_class_id) {
    int listener = bind_tcp_reuse(host_s, port_n);
    if (listener < 0) {
        panic_http_listen_host(host_s, port_n, os_error(errno));
    }
    // bound successfully: resume the awaiting main function
    MValue void_v = mv_void();
    marina_promise_resolve(promise, &void_v);

    // self-pipe wakeup: http_respond writes a byte here after each
    // response so retirements happen immediately, not on poll timeout
    int wake_fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, wake_fds) != 0) {
        rt_panic("http: cannot create wakeup pipe (" + os_error(errno) + ")");
    }
    int wake_rx = wake_fds[0];
    int wake_tx = wake_fds[1];
    int expected = -1;
    if (!wake_tx_fd().compare_exchange_strong(expected, wake_tx)) {
        ::close(wake_tx); // another server already owns the global pipe
    }
    set_nonblocking(wake_rx, true);

    // Evented accept loop: a single listener thread multiplexes every
    // open connection with poll(2). No per-connection threads, no
    // blocking reads. Each fully-read request is handed to its route:
    //   * sync handler  — runs inline here, respond-or-404 immediately;
    //   * async handler — runs to its first await, returns a promise,
    //     and the worker-thread continuation responds via http_respond.
    // A connection is retired when its res socket leaves res_sockets()
    // (response written) or after 30s without a response.
    set_nonblocking(listener, true);
    struct Conn {
        int fd;
        std::vector<uint8_t> buf;
        void* res;       // set while a request is in flight (pinned)
        bool dispatched; // request handed to an async handler
        std::chrono::steady_clock::time_point deadline; // idle deadline (30s)
    };
    std::vector<Conn> conns;
    const std::string resp404 =
        "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\n"
        "Connection: close\r\n\r\nnot found";

    for (;;) {
        // Retire connections past their idle deadline (stuck handler or
        // idle keep-alive connection): close silently. A late response
        // still finds its res_sockets entry and its write fails (peer
        // gone), which removes the entry.
        auto now = std::chrono::steady_clock::now();
        size_t i = 0;
        while (i < conns.size()) {
            if (now >= conns[i].deadline) {
                Conn c = std::move(conns[i]);
                conns.erase(conns.begin() + i);
                ::close(c.fd);
                if (c.dispatched) release_ptr(c.res);
            } else {
                i += 1;
            }
        }

        // Build the poll set: listener + wakeup pipe + connections still
        // reading.
        std::vector<struct pollfd> fds;
        fds.reserve(conns.size() + 2);
        fds.push_back({listener, POLLIN, 0});
        fds.push_back({wake_rx, POLLIN, 0});
        for (const Conn& c : conns) fds.push_back({c.fd, POLLIN, 0});
        int nready = ::poll(fds.data(), (nfds_t)fds.size(), 100);
        if (http_debug()) {
            std::string ev;
            for (size_t k = 0; k < fds.size(); k++) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "fd%d:r%04x", fds[k].fd,
                              (unsigned)fds[k].revents);
                if (k) ev += " ";
                ev += buf;
            }
            std::cerr << "[evloop] nready=" << nready << " conns=" << conns.size()
                      << " fds=" << ev << "\n";
        }
        if (nready <= 0) continue;
        if (fds[1].revents & POLLIN) {
            // drain the wakeup pipe (a response was written somewhere)
            char drain[4096];
            for (;;) {
                ssize_t n = ::read(wake_rx, drain, sizeof drain);
                if (n <= 0) break;
            }
        }
        if (fds[0].revents & POLLIN) {
            // accept every queued connection. NOTE: on Linux an accepted
            // socket does NOT inherit O_NONBLOCK from the listener (macOS
            // does), so set it explicitly or reads below block the loop.
            for (;;) {
                int cfd = ::accept(listener, nullptr, nullptr);
                if (cfd < 0) break;
                set_nonblocking(cfd, true);
                Conn c;
                c.fd = cfd;
                c.res = nullptr;
                c.dispatched = false;
                c.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                conns.push_back(std::move(c));
            }
        }
        for (size_t fi = 2; fi < fds.size(); fi++) {
            int polled_fd = fds[fi].fd;
            if (fds[fi].revents == 0) continue;
            size_t idx = SIZE_MAX;
            for (size_t k = 0; k < conns.size(); k++) {
                if (conns[k].fd == polled_fd) {
                    idx = k;
                    break;
                }
            }
            if (idx == SIZE_MAX) continue;
            if (conns[idx].dispatched) {
                // A signal on an in-flight connection: either the client
                // aborted (res_sockets entry still present — the handler
                // hasn't responded) or the response was written (entry
                // gone) and this is the client's NEXT request / its close.
                bool still_in_flight;
                {
                    std::lock_guard<std::mutex> lk(res_sockets().mu);
                    still_in_flight =
                        res_sockets().fds.count(reinterpret_cast<size_t>(conns[idx].res)) != 0;
                }
                if (still_in_flight) {
                    if (http_debug()) {
                        std::cerr << "[evloop] in-flight fd=" << polled_fd
                                  << " aborted -> retire\n";
                    }
                    Conn c = std::move(conns[idx]);
                    conns.erase(conns.begin() + idx);
                    ::close(c.fd);
                    release_ptr(c.res);
                    continue;
                }
                // response was written: connection goes back to reading
                // (HTTP keep-alive). The pin keeps this res address ours.
                if (http_debug()) {
                    std::cerr << "[evloop] fd=" << polled_fd
                              << " keep-alive -> next request\n";
                }
                // undo http_respond's blocking flip (shared description!)
                set_nonblocking(conns[idx].fd, true);
                release_ptr(conns[idx].res);
                conns[idx].dispatched = false;
                conns[idx].res = nullptr;
                conns[idx].deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            }
            // read everything currently available (socket is nonblocking)
            char tmp[8192];
            bool dead = false;
            bool eof = false;
            if (http_debug()) {
                std::cerr << "[evloop] reading fd=" << polled_fd << "\n";
            }
            for (;;) {
                ssize_t n = ::read(conns[idx].fd, tmp, sizeof tmp);
                if (n == 0) {
                    eof = true;
                    break;
                }
                if (n > 0) {
                    conns[idx].buf.insert(conns[idx].buf.end(), tmp, tmp + n);
                    if (conns[idx].buf.size() > (size_t)(64 * 1024 + 16 * 1024 * 1024)) {
                        dead = true;
                        break;
                    }
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    dead = true;
                    break;
                }
            }
            if (dead) {
                ::close(conns[idx].fd);
                conns.erase(conns.begin() + idx);
                continue;
            }
            // try to parse a full request out of the buffer
            std::string method, path, body;
            size_t consumed = 0;
            int parsed = try_parse_http(conns[idx].buf, &method, &path, &body, &consumed);
            if (parsed == 0) {
                if (eof) {
                    ::close(conns[idx].fd);
                    conns.erase(conns.begin() + idx); // EOF mid-request
                }
                continue;
            }
            if (parsed == 1) {
                ::close(conns[idx].fd);
                conns.erase(conns.begin() + idx); // malformed/oversized
                continue;
            }
            conns[idx].buf.erase(conns[idx].buf.begin(),
                                 conns[idx].buf.begin() + consumed);
            if (http_debug()) {
                std::cerr << "[evloop] request " << method << " " << path << " ("
                          << body.size() << " bytes body)\n";
            }

            std::optional<HttpRoute> route;
            {
                std::lock_guard<std::mutex> lk(http_routes().mu);
                for (const HttpRoute& r : http_routes().routes) {
                    if (r.method == method &&
                        (path == r.exact ||
                         (path == r.prefix && r.exact == r.prefix))) {
                        route = r;
                        break;
                    }
                }
            }
            if (!route.has_value()) {
                set_nonblocking(conns[idx].fd, false);
                write_all_ignore_errors(conns[idx].fd, resp404);
                ::close(conns[idx].fd);
                conns.erase(conns.begin() + idx);
                continue;
            }
            // req object: { method, path, body }
            void* req = marina_object_new(req_class_id, 3);
            set_object_field(req, 0, mv_str_owned(method));
            set_object_field(req, 1, mv_str_owned(path));
            set_object_field(req, 2, mv_str_owned(body));
            // res object backed by this connection
            void* res = marina_object_new(res_class_id, 0);
            int sock = ::dup(conns[idx].fd);
            if (sock < 0) {
                ::close(conns[idx].fd);
                conns.erase(conns.begin() + idx);
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(res_sockets().mu);
                res_sockets().fds.emplace(reinterpret_cast<size_t>(res), sock);
            }
            MValue req_mv{TAG_OBJECT, 0, 0, 0.0, req};
            MValue res_mv{TAG_OBJECT, 0, 0, 0.0, res};
            // ownership: both objects are handed to the handler with rc 1;
            // the compiled handler releases them when its frame dies.
            if (route->is_async) {
                // The handler is an async lambda: it runs to its first
                // await on this thread and returns a promise; the worker
                // continuation responds later via http_respond. Hand our
                // reference to the event loop with a no-op waiter so the
                // promise is released after resolution.
                // Pin the res object for the connection's lifetime: the
                // handler frees it as soon as its frame dies, and if its
                // address were reused by a later connection the retire
                // sweep below could mistake this connection for
                // responded. (Released when the connection retires.)
                retain_ptr(res);
                auto af = reinterpret_cast<MarinaAsyncListenCb>(route->f);
                void* pr = af(route->env, &req_mv, &res_mv);
                if (http_debug()) {
                    std::cerr << "[evloop] async dispatch -> promise " << pr << "\n";
                }
                marina_await(pr, reinterpret_cast<const void*>(&marina_noop_resume),
                             nullptr, 0);
                conns[idx].dispatched = true;
                conns[idx].res = res;
                conns[idx].deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(30);
            } else {
                route->f(route->env, &req_mv, &res_mv);
                // handler never responded -> 404
                bool unresponded;
                {
                    std::lock_guard<std::mutex> lk(res_sockets().mu);
                    auto it = res_sockets().fds.find(reinterpret_cast<size_t>(res));
                    if (it != res_sockets().fds.end()) {
                        ::close(it->second);
                        res_sockets().fds.erase(it);
                        unresponded = true;
                    } else {
                        unresponded = false;
                    }
                }
                if (unresponded) {
                    set_nonblocking(conns[idx].fd, false);
                    write_all_ignore_errors(conns[idx].fd, resp404);
                }
                ::close(conns[idx].fd);
                conns.erase(conns.begin() + idx);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// marina_http_validate helper
// ---------------------------------------------------------------------------

// String length in Unicode scalars (Rust `s.chars().count()`); the input
// is always valid UTF-8 (lossy-decoded beforehand).
size_t char_count(const std::string& s) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
    size_t n = s.size();
    size_t count = 0;
    size_t i = 0;
    while (i < n) {
        uint8_t b = p[i];
        i += (b < 0x80) ? 1 : (b >= 0xF0 ? 4 : (b >= 0xE0 ? 3 : 2));
        count++;
    }
    return count;
}

} // namespace

} // namespace marina_rt

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

extern "C" void marina_http_request(const MValue* method, const MValue* url, const MValue* body,
                         int32_t class_id, MValue* out) {
    std::string m = marina_rt::mv_to_string(*method);
    std::string u = marina_rt::mv_to_string(*url);
    std::optional<std::string> b;
    if (body->tag == TAG_STRING) b = marina_rt::mv_to_string(*body);
    auto [status, resp_body] = marina_rt::http_do_request(m, u, b);
    void* o = marina_object_new(class_id, 2);
    marina_rt::set_object_field(o, 0, marina_rt::mv_int(status));
    marina_rt::set_object_field(o, 1, marina_rt::mv_str_owned(resp_body));
    marina_rt::out_mv(out, MValue{TAG_OBJECT, 0, 0, 0.0, o});
}

extern "C" void* marina_http_request_async(const MValue* method, const MValue* url,
                                const MValue* body, int32_t class_id) {
    std::string m = marina_rt::mv_to_string(*method);
    std::string u = marina_rt::mv_to_string(*url);
    std::optional<std::string> b;
    if (body->tag == TAG_STRING) b = marina_rt::mv_to_string(*body);
    return marina_rt::spawn_promise_io(
        [m, u, b, class_id] {
            auto [status, resp_body] = marina_rt::http_do_request(m, u, b);
            void* o = marina_object_new(class_id, 2);
            marina_rt::set_object_field(o, 0, marina_rt::mv_int(status));
            marina_rt::set_object_field(o, 1, marina_rt::mv_str_owned(resp_body));
            return MValue{TAG_OBJECT, 0, 0, 0.0, o};
        },
        {});
}

extern "C" void marina_http_listen(const MValue* port, const void* cb, void* env,
                        const void* env_drop, int32_t req_class_id,
                        int32_t res_class_id) {
    uint16_t port_n = (uint16_t)port->i;
    MarinaListenCb f = reinterpret_cast<MarinaListenCb>(const_cast<void*>(cb));
    // env_drop is intentionally never called: the closure (its captures)
    // lives as long as the server does.
    (void)env_drop;

    {
        std::lock_guard<std::mutex> lk(marina_rt::rt_mutex());
        marina_rt::rt().pending_listeners += 1;
    }
    marina_rt::ListenJob job{env, f, port_n, req_class_id, res_class_id};
    std::thread(marina_rt::blocking_listen_thread, job).detach();
}

extern "C" void marina_http_res_send(void* res, const MValue* value) {
    marina_rt::http_respond(res, value, false);
}

extern "C" void marina_http_res_json(void* res, const MValue* value) {
    marina_rt::http_respond(res, value, true);
}

extern "C" void marina_http_router_bind(const MValue* router, const MValue* path) {
    std::string prefix = marina_rt::mv_to_string(*path);
    size_t rp = reinterpret_cast<size_t>(router->p);
    std::lock_guard<std::mutex> lk(marina_rt::http_routers().mu);
    marina_rt::http_routers().map[rp] = prefix;
}

extern "C" void marina_http_route_add(const MValue* router, const MValue* method,
                           const MValue* path, MarinaListenCb f, void* env,
                           const void* env_drop, int32_t is_async) {
    (void)env_drop;
    size_t rp = reinterpret_cast<size_t>(router->p);
    std::string prefix;
    {
        std::lock_guard<std::mutex> lk(marina_rt::http_routers().mu);
        auto it = marina_rt::http_routers().map.find(rp);
        if (it != marina_rt::http_routers().map.end()) prefix = it->second;
    }
    std::string m = marina_rt::mv_to_string(*method);
    std::string p = marina_rt::mv_to_string(*path);
    std::string exact = (p == "/") ? prefix : prefix + p;
    std::lock_guard<std::mutex> lk(marina_rt::http_routes().mu);
    marina_rt::http_routes().routes.push_back(marina_rt::HttpRoute{
        std::move(m), std::move(exact), std::move(prefix), f, env, is_async != 0});
}

extern "C" void marina_http_server_listen(const MValue* host, const MValue* port, void* promise,
                               int32_t req_class_id, int32_t res_class_id) {
    std::string host_s = marina_rt::mv_to_string(*host);
    uint16_t port_n = (uint16_t)port->i;
    {
        std::lock_guard<std::mutex> lk(marina_rt::rt_mutex());
        marina_rt::rt().pending_listeners += 1;
    }
    std::thread(marina_rt::evented_listen_thread, std::move(host_s), port_n, promise,
                req_class_id, res_class_id)
        .detach();
}

extern "C" void marina_http_field_init(void* field_obj, int32_t kind) {
    std::lock_guard<std::mutex> lk(marina_rt::http_field_specs().mu);
    marina_rt::FieldSpec spec;
    spec.kind = (uint8_t)kind;
    marina_rt::http_field_specs().map[reinterpret_cast<size_t>(field_obj)] = spec;
}

extern "C" void marina_http_field_op(void* field_obj, int32_t op, const MValue* arg) {
    std::lock_guard<std::mutex> lk(marina_rt::http_field_specs().mu);
    auto it = marina_rt::http_field_specs().map.find(reinterpret_cast<size_t>(field_obj));
    if (it == marina_rt::http_field_specs().map.end()) return;
    switch (op) {
        case 0: it->second.required = true; break;
        case 1: it->second.min = arg->i; break;
        case 2: it->second.max = arg->i; break;
        default: break;
    }
}

extern "C" void marina_http_validator_init(void* v, const MValue* schema) {
    void* sp = schema->p;
    std::vector<std::pair<std::string, marina_rt::FieldSpec>> specs;
    {
        marina_rt::ObjectObj* obj = reinterpret_cast<marina_rt::ObjectObj*>(sp);
        int32_t class_id = obj->class_id;
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> lk(marina_rt::class_mutex());
            auto it = marina_rt::class_map().find(class_id);
            if (it != marina_rt::class_map().end()) names = it->second.fields;
        }
        for (size_t i = 0; i < names.size(); i++) {
            MValue mv = marina_rt::mv_void();
            marina_object_get(sp, (int64_t)i, &mv);
            size_t fptr = marina_rt::is_ptr_tag(mv.tag) ? reinterpret_cast<size_t>(mv.p) : 0;
            marina_rt::FieldSpec spec;
            {
                std::lock_guard<std::mutex> lk(marina_rt::http_field_specs().mu);
                auto it = marina_rt::http_field_specs().map.find(fptr);
                if (it != marina_rt::http_field_specs().map.end()) spec = it->second;
            }
            marina_rt::drop_mv(mv);
            specs.emplace_back(names[i], spec);
        }
    }
    std::lock_guard<std::mutex> lk(marina_rt::http_validator_specs().mu);
    marina_rt::http_validator_specs().map[reinterpret_cast<size_t>(v)] = std::move(specs);
}

extern "C" void marina_http_validate(void* v, const MValue* body, int32_t out_class, MValue* out) {
    size_t vp = reinterpret_cast<size_t>(v);
    std::optional<std::vector<std::pair<std::string, marina_rt::FieldSpec>>> specs;
    {
        std::lock_guard<std::mutex> lk(marina_rt::http_validator_specs().mu);
        auto it = marina_rt::http_validator_specs().map.find(vp);
        if (it != marina_rt::http_validator_specs().map.end()) specs = it->second;
    }
    if (!specs.has_value()) {
        marina_rt::rt_panic("validate called on an invalid validator object");
    }
    std::string body_s = marina_rt::mv_to_string(*body);
    std::optional<marina_rt::JVal> parsed = marina_rt::json_parse(body_s);
    if (!parsed.has_value()) {
        marina_rt::rt_panic("http: invalid JSON body");
    }
    if (parsed->k != marina_rt::JVal::Obj) {
        marina_rt::rt_panic("http: request body must be a JSON object");
    }
    const std::vector<std::pair<std::string, marina_rt::JVal>>& fields = parsed->obj;
    void* obj = marina_object_new(out_class, (int64_t)specs->size());
    for (size_t i = 0; i < specs->size(); i++) {
        const std::string& name = (*specs)[i].first;
        const marina_rt::FieldSpec& spec = (*specs)[i].second;
        const marina_rt::JVal* jv = nullptr;
        for (const auto& kv : fields) {
            if (kv.first == name) {
                jv = &kv.second;
                break;
            }
        }
        if (jv == nullptr) {
            if (spec.required) {
                marina_rt::rt_panic("http: missing required field '" + name + "'");
            }
            continue;
        }
        MValue mv = marina_rt::mv_void();
        if (spec.kind == 0 && jv->k == marina_rt::JVal::Str) {
            size_t len = marina_rt::char_count(jv->str);
            if (spec.min.has_value() && (int64_t)len < *spec.min) {
                marina_rt::rt_panic("http: field '" + name + "' must be at least " +
                                    std::to_string(*spec.min) + " characters");
            }
            if (spec.max.has_value() && (int64_t)len > *spec.max) {
                marina_rt::rt_panic("http: field '" + name + "' must be at most " +
                                    std::to_string(*spec.max) + " characters");
            }
            mv = marina_rt::mv_str_owned(jv->str);
        } else if (spec.kind == 1 && jv->k == marina_rt::JVal::Num) {
            mv = marina_rt::mv_int(marina_rt::sat_i64(jv->num));
        } else {
            marina_rt::rt_panic("http: field '" + name + "' has the wrong type");
        }
        marina_rt::set_object_field(obj, (int64_t)i, mv);
    }
    marina_rt::out_mv(out, MValue{TAG_OBJECT, 0, 0, 0.0, obj});
}