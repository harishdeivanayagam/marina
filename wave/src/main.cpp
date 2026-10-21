// wave — the package manager and build tool for marina programs
// (what cargo is to rust).
//
// Commands:
//   wave init              create package.json + src/main.tsm
//   wave add <pkg>[@ver]   add a dependency to package.json
//   wave remove <pkg>      remove a dependency from package.json
//   wave check [file]      validate package.json + type-check (no codegen)
//   wave build [file]      compile to a native binary in ./.marina/
//   wave run [file]        compile + execute
//
// Port of crates/wave/src/main.rs. The JSON handling replicates serde_json
// (with the `preserve_order` feature): insertion-ordered objects, sorted
// keys for the dependencies map, identical pretty-printing.
#include <marina/checker.hpp>
#include <marina/pipeline.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace pipeline = marina::pipeline;

namespace {

const char* WAVE_VERSION = "0.2.0";

[[noreturn]] void panic_exit(const std::string& what) {
    std::cerr << "thread 'main' panicked: " << what << std::endl;
    std::exit(101);
}

std::string io_msg(int err) {
    return std::string(std::strerror(err)) + " (os error " + std::to_string(err) + ")";
}

std::string io_msg_ec(const std::error_code& ec) {
    if (ec.value() > 0) return io_msg(ec.value());
    return ec.message();
}

std::string disp(const fs::path& p) { return p.string(); }

fs::path cwd_or_dot() {
    std::error_code ec;
    fs::path p = fs::current_path(ec);
    return ec ? fs::path(".") : p;
}

fs::path cwd_unwrap() {
    std::error_code ec;
    fs::path p = fs::current_path(ec);
    if (ec) panic_exit("cannot get current directory: " + io_msg_ec(ec));
    return p;
}

fs::path temp_dir() {
    std::error_code ec;
    fs::path p = fs::temp_directory_path(ec);
    return ec ? fs::path("/tmp") : p;
}

// Rust `Path::file_name()`: the last component, or None for root/curdir/
// parentdir (and paths ending in those).
std::optional<std::string> rust_file_name(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && s[end - 1] == '/') end -= 1;
    // "a/." behaves like "a"
    while (end >= 2 && s[end - 1] == '.' && s[end - 2] == '/') end -= 2;
    if (end == 0) return std::nullopt;
    std::string t = s.substr(0, end);
    size_t pos = t.find_last_of('/');
    std::string comp = pos == std::string::npos ? t : t.substr(pos + 1);
    if (comp.empty() || comp == "." || comp == "..") return std::nullopt;
    return comp;
}

// Rust `Path::parent().display()` for a path like "./package.json" (the C++
// parent_path keeps the trailing slash, Rust's parent does not).
std::string parent_display(const fs::path& p) {
    std::string s = p.parent_path().string();
    if (s.size() > 1 && s.back() == '/') s.pop_back();
    if (s.empty()) return "";
    return s;
}

bool utf8_valid(const std::string& s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int len;
        uint32_t cp;
        if (c < 0x80) {
            i += 1;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + static_cast<size_t>(len) > n) return false;
        for (int k = 1; k < len; k++) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
            (len == 4 && cp < 0x10000))
            return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += static_cast<size_t>(len);
    }
    return true;
}

std::optional<std::string> read_file(const fs::path& p, std::string& err) {
    errno = 0;
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) {
        err = io_msg(errno);
        return std::nullopt;
    }
    std::string out;
    char buf[65536];
    size_t n;
    errno = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    bool failed = std::ferror(f) != 0;
    int errn = errno;
    std::fclose(f);
    if (failed) {
        err = io_msg(errn ? errn : EIO);
        return std::nullopt;
    }
    if (!utf8_valid(out)) {
        err = "stream did not contain valid UTF-8";
        return std::nullopt;
    }
    return out;
}

bool write_file(const fs::path& p, const std::string& text, std::string& err) {
    errno = 0;
    std::FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) {
        err = io_msg(errno);
        return false;
    }
    size_t n = std::fwrite(text.data(), 1, text.size(), f);
    bool ok = n == text.size();
    int errn = errno;
    if (std::fclose(f) != 0) {
        ok = false;
        errn = errno;
    }
    if (!ok) err = io_msg(errn ? errn : EIO);
    return ok;
}

// Spawn a process like Rust's `Command::new(..).args(..).status()` (no shell,
// inherited stdio). Returns true iff the child exited with code 0.
bool run_process(const std::vector<std::string>& args) {
    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string trim_ascii(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e) {
        char c = s[b];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
            b += 1;
        } else {
            break;
        }
    }
    while (e > b) {
        char c = s[e - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
            e -= 1;
        } else {
            break;
        }
    }
    return s.substr(b, e - b);
}

// ---------------------------------------------------------------------------
// JSON (replicates serde_json with the `preserve_order` feature)
// ---------------------------------------------------------------------------

struct Json;
struct JArr {
    std::vector<Json> items;
};
// Object preserving insertion order (IndexMap semantics: re-setting a key
// updates the value in place).
struct JObj {
    std::vector<std::pair<std::string, Json>> entries;

    const Json* get(const std::string& key) const;
    Json* get(const std::string& key);
    void set(const std::string& key, Json value);
    void remove(const std::string& key);
    size_t size() const;
};
struct JNum {
    bool is_int = false;
    bool neg = false;
    uint64_t mag = 0; // magnitude when is_int
    double d = 0.0;
};
struct Json {
    std::variant<std::monostate, bool, JNum, std::string, JArr, JObj> v;

    static Json null() { return Json(); }
    static Json boolean(bool b) {
        Json j;
        j.v = b;
        return j;
    }
    static Json integer(bool neg, uint64_t mag) {
        Json j;
        JNum n;
        n.is_int = true;
        n.neg = neg && mag != 0;
        n.mag = mag;
        j.v = n;
        return j;
    }
    static Json num(double d) {
        Json j;
        JNum n;
        n.is_int = false;
        n.d = d;
        j.v = n;
        return j;
    }
    static Json str(std::string s) {
        Json j;
        j.v = std::move(s);
        return j;
    }
    static Json array(JArr a) {
        Json j;
        j.v = std::move(a);
        return j;
    }
    static Json object(JObj o) {
        Json j;
        j.v = std::move(o);
        return j;
    }

    const Json* get(const std::string& key) const {
        if (auto* m = std::get_if<JObj>(&v)) return m->get(key);
        return nullptr;
    }
    void set(const std::string& key, Json value) {
        if (auto* m = std::get_if<JObj>(&v)) m->set(key, std::move(value));
    }
    const std::string* as_str() const {
        if (auto* s = std::get_if<std::string>(&v)) return s;
        return nullptr;
    }
};

const Json* JObj::get(const std::string& key) const {
    for (const auto& e : entries) {
        if (e.first == key) return &e.second;
    }
    return nullptr;
}

Json* JObj::get(const std::string& key) {
    for (auto& e : entries) {
        if (e.first == key) return &e.second;
    }
    return nullptr;
}

void JObj::set(const std::string& key, Json value) {
    for (auto& e : entries) {
        if (e.first == key) {
            e.second = std::move(value);
            return;
        }
    }
    entries.emplace_back(key, std::move(value));
}

void JObj::remove(const std::string& key) {
    for (size_t i = 0; i < entries.size(); i++) {
        if (entries[i].first == key) {
            entries.erase(entries.begin() + static_cast<long>(i));
            return;
        }
    }
}

size_t JObj::size() const { return entries.size(); }

std::string u64_dec(uint64_t v) {
    char buf[24];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, res.ptr);
}

// serde_json renders f64 values through ryu: shortest round-trip digits,
// always with a decimal point in positional notation, switching to
// `e+XX`/`e-XX` exponent notation outside [1e-5, 1e16).
std::string ryu_fmt(double f) {
    bool neg = std::signbit(f);
    double a = std::fabs(f);
    if (a == 0.0) return neg ? "-0.0" : "0.0";
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), a, std::chars_format::scientific);
    std::string s(buf, res.ptr);
    size_t ep = s.find('e');
    std::string mant = s.substr(0, ep);
    int E = std::stoi(s.substr(ep + 1));
    std::string digits;
    for (char c : mant) {
        if (c >= '0' && c <= '9') digits.push_back(c);
    }
    // value = d.ddd * 10^E; the decimal point position is E + 1
    int kk = E + 1;
    std::string out;
    if (kk > 0 && kk <= 16) {
        if (static_cast<int>(digits.size()) <= kk) {
            out = digits + std::string(static_cast<size_t>(kk) - digits.size(), '0') + ".0";
        } else {
            out = digits.substr(0, static_cast<size_t>(kk)) + "." +
                  digits.substr(static_cast<size_t>(kk));
        }
    } else if (kk > -5 && kk <= 0) {
        out = "0." + std::string(static_cast<size_t>(-kk), '0') + digits;
    } else {
        int expo = kk - 1;
        out = digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        out += "e" + std::string(expo >= 0 ? "+" : "-") +
               u64_dec(static_cast<uint64_t>(expo >= 0 ? expo : -expo));
    }
    return neg ? "-" + out : out;
}

std::string jnum_str(const JNum& n) {
    if (n.is_int) {
        std::string s = u64_dec(n.mag);
        return n.neg ? "-" + s : s;
    }
    return ryu_fmt(n.d);
}

std::string json_escape(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string json_indent(int depth) { return std::string(static_cast<size_t>(depth) * 2, ' '); }

std::string json_pretty(const Json& j, int depth) {
    if (std::holds_alternative<std::monostate>(j.v)) return "null";
    if (auto* b = std::get_if<bool>(&j.v)) return *b ? "true" : "false";
    if (auto* n = std::get_if<JNum>(&j.v)) return jnum_str(*n);
    if (auto* s = std::get_if<std::string>(&j.v)) return json_escape(*s);
    if (auto* a = std::get_if<JArr>(&j.v)) {
        if (a->items.empty()) return "[]";
        std::string out = "[\n";
        for (size_t i = 0; i < a->items.size(); i++) {
            if (i > 0) out += ",\n";
            out += json_indent(depth + 1) + json_pretty(a->items[i], depth + 1);
        }
        out += "\n" + json_indent(depth) + "]";
        return out;
    }
    if (auto* m = std::get_if<JObj>(&j.v)) {
        if (m->entries.empty()) return "{}";
        std::string out = "{\n";
        for (size_t i = 0; i < m->entries.size(); i++) {
            if (i > 0) out += ",\n";
            out += json_indent(depth + 1) + json_escape(m->entries[i].first) + ": " +
                   json_pretty(m->entries[i].second, depth + 1);
        }
        out += "\n" + json_indent(depth) + "}";
        return out;
    }
    return "null";
}

// ---------------------------------------------------------------------------
// JSON parser (serde_json-compatible errors)
// ---------------------------------------------------------------------------

struct JsonParser {
    const std::string& s;
    size_t pos = 0;

    struct Error {
        std::string msg;
    };

    // serde's position: 1-based column of the offending byte; a newline
    // advances to (line + 1, column 0); EOF reports the end-of-line offset.
    std::string err_pos(size_t idx, bool eof) const {
        size_t line = 1, line_start = 0;
        for (size_t k = 0; k < idx && k < s.size(); k++) {
            if (s[k] == '\n') {
                line += 1;
                line_start = k + 1;
            }
        }
        size_t col;
        if (eof) {
            col = idx - line_start;
        } else if (idx < s.size() && s[idx] == '\n') {
            line += 1;
            col = 0;
        } else {
            col = idx - line_start + 1;
        }
        return " at line " + std::to_string(line) + " column " + std::to_string(col);
    }

    [[noreturn]] void fail(const std::string& code, bool eof = false) const {
        throw Error{code + err_pos(pos, eof)};
    }

    bool at_end() const { return pos >= s.size(); }
    char peek() const { return s[pos]; }

    static bool is_digit(char c) { return c >= '0' && c <= '9'; }

    void skip_ws() {
        while (pos < s.size()) {
            char b = s[pos];
            if (b == ' ' || b == '\t' || b == '\n' || b == '\r') {
                pos += 1;
            } else {
                break;
            }
        }
    }

    Json parse() {
        skip_ws();
        if (at_end()) fail("EOF while parsing a value", true);
        Json v = parse_value();
        skip_ws();
        if (pos != s.size()) fail("trailing characters");
        return v;
    }

    Json parse_value() {
        skip_ws();
        char c = peek();
        switch (c) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': return Json::str(parse_string());
            case 't': return literal("true", Json::boolean(true));
            case 'f': return literal("false", Json::boolean(false));
            case 'n': return literal("null", Json::null());
            default:
                if (is_digit(c) || c == '-') return parse_number();
                fail("expected value");
        }
    }

    Json literal(const char* text, Json v) {
        for (size_t i = 0; text[i] != '\0'; i++) {
            if (at_end()) fail("EOF while parsing a value", true);
            if (s[pos] != text[i]) fail("expected ident");
            pos += 1;
        }
        return v;
    }

    static void push_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    uint32_t read_hex4() {
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            if (at_end()) fail("EOF while parsing a string", true);
            char h = s[pos];
            uint32_t d;
            if (h >= '0' && h <= '9') d = static_cast<uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') d = static_cast<uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') d = static_cast<uint32_t>(h - 'A' + 10);
            else {
                pos += 1;
                fail("invalid escape");
            }
            v = (v << 4) | d;
            pos += 1;
        }
        return v;
    }

    std::string parse_string() {
        std::string out;
        pos += 1; // opening quote
        while (true) {
            if (at_end()) fail("EOF while parsing a string", true);
            unsigned char c = static_cast<unsigned char>(s[pos]);
            if (c == '"') {
                pos += 1;
                return out;
            }
            if (c == '\\') {
                pos += 1;
                if (at_end()) fail("EOF while parsing a string", true);
                char e = s[pos];
                switch (e) {
                    case '"': out.push_back('"'); pos += 1; break;
                    case '\\': out.push_back('\\'); pos += 1; break;
                    case '/': out.push_back('/'); pos += 1; break;
                    case 'n': out.push_back('\n'); pos += 1; break;
                    case 't': out.push_back('\t'); pos += 1; break;
                    case 'r': out.push_back('\r'); pos += 1; break;
                    case 'b': out.push_back('\x08'); pos += 1; break;
                    case 'f': out.push_back('\x0c'); pos += 1; break;
                    case 'u': {
                        pos += 1;
                        uint32_t cp = read_hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (pos + 1 < s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
                                pos += 2;
                                uint32_t lo = read_hex4();
                                if (lo < 0xDC00 || lo > 0xDFFF)
                                    fail("lone leading surrogate in hex escape");
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                fail("unexpected end of hex escape");
                            }
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            fail("lone leading surrogate in hex escape");
                        }
                        push_utf8(out, cp);
                        break;
                    }
                    default:
                        fail("invalid escape");
                }
            } else if (c < 0x20) {
                fail("control character (\\u0000-\\u001F) found while parsing a string");
            } else {
                size_t save = pos;
                unsigned char b = c;
                int len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
                if (pos + static_cast<size_t>(len) > s.size()) fail("EOF while parsing a string", true);
                for (int k = 1; k < len; k++) {
                    if ((static_cast<unsigned char>(s[pos + k]) & 0xC0) != 0x80)
                        fail("invalid utf-8 sequence");
                }
                pos += static_cast<size_t>(len);
                out.append(s, save, pos - save);
            }
        }
    }

    Json parse_number() {
        size_t start = pos;
        bool neg = false;
        if (peek() == '-') {
            neg = true;
            pos += 1;
        }
        if (at_end()) fail("EOF while parsing a value", true);
        if (s[pos] == '0') {
            pos += 1;
            if (pos < s.size() && is_digit(s[pos])) fail("invalid number");
        } else if (is_digit(s[pos])) {
            while (pos < s.size() && is_digit(s[pos])) pos += 1;
        } else {
            fail("invalid number");
        }
        bool is_float = false;
        if (pos < s.size() && s[pos] == '.') {
            is_float = true;
            pos += 1;
            if (at_end()) fail("EOF while parsing a value", true);
            if (!is_digit(s[pos])) fail("invalid number");
            while (pos < s.size() && is_digit(s[pos])) pos += 1;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            is_float = true;
            pos += 1;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos += 1;
            if (at_end()) fail("EOF while parsing a value", true);
            if (!is_digit(s[pos])) fail("invalid number");
            while (pos < s.size() && is_digit(s[pos])) pos += 1;
        }
        std::string raw = s.substr(start, pos - start);
        if (!is_float) {
            // integer literal: exact u64/i64 round-trip like serde_json
            uint64_t mag = 0;
            bool overflow = false;
            for (size_t i = neg ? start + 1 : start; i < pos; i++) {
                uint64_t d = static_cast<uint64_t>(s[i] - '0');
                if (mag > (UINT64_MAX - d) / 10) {
                    overflow = true;
                    break;
                }
                mag = mag * 10 + d;
            }
            if (!overflow) return Json::integer(neg, mag);
        }
        errno = 0;
        char* end = nullptr;
        double d = std::strtod(raw.c_str(), &end);
        if (end != raw.c_str() + raw.size() || std::isinf(d)) fail("number out of range");
        return Json::num(d);
    }

    Json parse_object() {
        pos += 1; // {
        JObj obj;
        skip_ws();
        if (at_end()) fail("EOF while parsing an object", true);
        if (peek() == '}') {
            pos += 1;
            return Json::object(std::move(obj));
        }
        while (true) {
            skip_ws();
            if (at_end()) fail("EOF while parsing an object", true);
            if (peek() != '"') fail("key must be a string");
            std::string key = parse_string();
            skip_ws();
            if (at_end()) fail("EOF while parsing an object", true);
            if (peek() != ':') fail("expected `:`");
            pos += 1;
            Json val = parse_value();
            obj.set(key, std::move(val));
            skip_ws();
            if (at_end()) fail("EOF while parsing an object", true);
            if (peek() == ',') {
                pos += 1;
                skip_ws();
                if (at_end()) fail("EOF while parsing a value", true);
                if (peek() == '}') fail("trailing comma");
                continue;
            }
            if (peek() == '}') {
                pos += 1;
                return Json::object(std::move(obj));
            }
            fail("expected `,` or `}`");
        }
    }

    Json parse_array() {
        pos += 1; // [
        JArr arr;
        skip_ws();
        if (at_end()) fail("EOF while parsing a list", true);
        if (peek() == ']') {
            pos += 1;
            return Json::array(std::move(arr));
        }
        while (true) {
            arr.items.push_back(parse_value());
            skip_ws();
            if (at_end()) fail("EOF while parsing a list", true);
            if (peek() == ',') {
                pos += 1;
                skip_ws();
                if (at_end()) fail("EOF while parsing a value", true);
                if (peek() == ']') fail("trailing comma");
                continue;
            }
            if (peek() == ']') {
                pos += 1;
                return Json::array(std::move(arr));
            }
            fail("expected `,` or `]`");
        }
    }
};

Json json_parse_or_throw(const std::string& text, std::string& err) {
    JsonParser p{text};
    try {
        return p.parse();
    } catch (const JsonParser::Error& e) {
        err = e.msg;
        return Json::null();
    }
}

// ---------------------------------------------------------------------------
// package.json
// ---------------------------------------------------------------------------

// Path to the manifest in the current directory.
fs::path manifest_path() { return cwd_or_dot() / "package.json"; }

struct Manifest {
    fs::path path;
    Json root;
    JObj deps;
};

// Minimal schema validation: name (string), version (string),
// dependencies (object of string → version string).
std::optional<std::string> validate_schema(const Json& root) {
    const JObj* obj = std::get_if<JObj>(&root.v);
    if (!obj) return "package.json must contain a JSON object";
    const Json* name = obj->get("name");
    if (!(name && name->as_str() && !trim_ascii(*name->as_str()).empty()))
        return "package.json needs a non-empty string \"name\"";
    const Json* version = obj->get("version");
    if (!(version && version->as_str() && !trim_ascii(*version->as_str()).empty()))
        return "package.json needs a non-empty string \"version\"";
    if (const Json* d = obj->get("dependencies")) {
        const JObj* map = std::get_if<JObj>(&d->v);
        if (!map) return "\"dependencies\" must be an object";
        for (const auto& kv : map->entries) {
            if (!kv.second.as_str()) {
                return "dependency '" + kv.first +
                       "' must map to a version string, e.g. \"" + kv.first +
                       "\": \"1.0.0\"";
            }
        }
    }
    return std::nullopt;
}

struct ManifestLoad {
    std::optional<Manifest> m;
    std::string err;
};

ManifestLoad manifest_load() {
    fs::path path = manifest_path();
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return {std::nullopt,
                "no package.json found in " + parent_display(path) +
                    " (run `wave init` first)"};
    }
    std::string ioerr;
    auto text = read_file(path, ioerr);
    if (!text) return {std::nullopt, "cannot read " + disp(path) + ": " + ioerr};
    Json root;
    {
        std::string jerr;
        root = json_parse_or_throw(*text, jerr);
        if (!jerr.empty()) {
            return {std::nullopt, disp(path) + " is not valid JSON: " + jerr};
        }
    }
    if (auto e = validate_schema(root)) return {std::nullopt, *e};
    Manifest m;
    m.path = path;
    m.root = root;
    if (const Json* d = root.get("dependencies")) {
        if (const JObj* o = std::get_if<JObj>(&d->v)) {
            m.deps = *o;
        } else {
            return {std::nullopt, "`dependencies` must be an object"};
        }
    }
    return {std::move(m), ""};
}

// serde_json sorts nothing by itself; keep deps in sorted order for stable
// diffs.
JObj sorted_map(const JObj& map) {
    JObj out = map;
    std::sort(out.entries.begin(), out.entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

std::optional<std::string> manifest_save(const Manifest& m) {
    Json root = m.root;
    root.set("dependencies", Json::object(sorted_map(m.deps)));
    std::string err;
    if (!write_file(m.path, json_pretty(root, 0) + "\n", err)) {
        return "cannot write " + disp(m.path) + ": " + err;
    }
    return std::nullopt;
}

// `wave run <package> <command>` runs `src/<command>.tsm` when package.json
// "name" is <package> and that file exists. `wave run emsql migrate` is the
// emsql CLI; `wave run src/migrate.tsm` still works on its own.
std::optional<std::string> resolve_run_file(const std::vector<std::string>& rest,
                                            const Manifest* manifest) {
    if (rest.empty()) return std::nullopt;
    std::error_code ec;
    if (fs::is_regular_file(rest[0], ec)) return rest[0];
    if (fs::is_regular_file(rest[0] + ".tsm", ec)) return rest[0] + ".tsm";
    if (rest.size() >= 2 && manifest && marina::ir::is_kebab_case(rest[1])) {
        const Json* n = manifest->root.get("name");
        const std::string* name = n ? n->as_str() : nullptr;
        if (name && *name == rest[0]) {
            std::string cmd = std::string("src/") + rest[1] + ".tsm";
            if (fs::is_regular_file(cmd, ec)) return cmd;
        }
    }
    return rest[0];
}

// Entry point resolution: explicit file arg → "main" field → src/main.tsm.
std::optional<fs::path> entry_path(std::optional<std::string> explicit_file,
                                   const Manifest* manifest, std::string& err) {
    if (explicit_file) {
        fs::path p(*explicit_file);
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return p;
        err = "file not found: " + disp(p);
        return std::nullopt;
    }
    std::string main_field = "src/main.tsm";
    if (manifest) {
        if (const Json* v = manifest->root.get("main")) {
            if (const std::string* s = v->as_str()) main_field = *s;
        }
    }
    fs::path p(main_field);
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) return p;
    err = "entry point '" + disp(p) +
          "' not found (set \"main\" in package.json or pass a file)";
    return std::nullopt;
}

// Shared front half of check/build/run: validate file conventions + read.
struct LoadedSource {
    std::string src;
    std::string file;
    std::string stem;
    // directory of the source file — the root local folder modules
    // (`use sample_mod/sub`) resolve against
    std::string base_dir;
};

std::string stem_of(const fs::path& p) {
    std::string s = p.stem().string();
    if (s.empty()) return "program";
    return s;
}

std::optional<LoadedSource> load_source(const fs::path& path) {
    if (auto msg = pipeline::validate_path(path.string())) {
        std::cerr << "error: " << *msg << std::endl;
        return std::nullopt;
    }
    std::string ioerr;
    auto src = read_file(path, ioerr);
    if (!src) {
        std::cerr << "error: cannot read " << disp(path) << ": " << ioerr << std::endl;
        return std::nullopt;
    }
    LoadedSource r;
    r.src = std::move(*src);
    r.file = disp(path);
    r.stem = stem_of(path);
    r.base_dir = fs::path(path).parent_path().string();
    return r;
}

int print_fail(const std::string& file, pipeline::Fail f) {
    if (f.kind == pipeline::Fail::Kind::Diags) {
        pipeline::print_diags(file, f.diags);
        std::cerr << std::endl;
        std::cerr << "compilation failed: " << f.diags.size() << " error(s)" << std::endl;
        return 1;
    }
    std::cerr << "error: " << f.tool << std::endl;
    return 1;
}

// compile_source reports failure through the `Fail` out-param; success leaves
// it default-constructed.
bool fail_is_default(const pipeline::Fail& f) {
    return f.kind == pipeline::Fail::Kind::Tool && f.tool.empty() && f.diags.empty();
}

std::string hex_u64(uint64_t v) {
    if (v == 0) return "0";
    std::string s;
    while (v) {
        int d = static_cast<int>(v & 15);
        s.push_back(static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10));
        v >>= 4;
    }
    std::reverse(s.begin(), s.end());
    return s;
}

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------

int cmd_init(const std::vector<std::string>& rest) {
    if (!rest.empty()) {
        std::cerr << "usage: wave init" << std::endl;
        return 2;
    }
    fs::path cwd = cwd_or_dot();
    std::string name;
    if (auto fn = rust_file_name(disp(cwd))) {
        name = *fn;
    } else {
        name = "my-project";
    }
    // package names: kebab-case, like files
    for (auto& c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    for (auto& c : name) {
        if (c == ' ' || c == '_') c = '-';
    }

    fs::path manifest = manifest_path();
    std::error_code ec;
    if (fs::is_regular_file(manifest, ec)) {
        std::cerr << "error: package.json already exists here" << std::endl;
        return 1;
    }
    JObj pkg;
    pkg.set("name", Json::str(name));
    pkg.set("version", Json::str("0.0.1"));
    pkg.set("dependencies", Json::object(JObj{}));
    std::string err;
    if (!write_file(manifest, json_pretty(Json::object(std::move(pkg)), 0) + "\n", err)) {
        std::cerr << "error: cannot write " << disp(manifest) << ": " << err << std::endl;
        return 1;
    }

    fs::path src_dir = cwd / "src";
    fs::path main_file = src_dir / "main.tsm";
    if (!fs::exists(main_file, ec)) {
        std::error_code ec3;
        fs::create_directories(src_dir, ec3);
        if (ec3) panic_exit("create src/: " + io_msg_ec(ec3));
        if (!write_file(main_file,
                        "// entry point of the program\nasync function main() {\n"
                        "  console.log(\"hello from wave\")\n}\n",
                        err))
            panic_exit("write src/main.tsm: " + err);
    }
    std::cout << "created package.json" << "\n";
    std::cout << "created src/main.tsm" << "\n";
    std::cout << "next: wave run" << "\n";
    return 0;
}

std::optional<std::string> copy_dir_recursive(const fs::path& from, const fs::path& to);

int install_from_dir(const fs::path& dir, std::optional<std::string> explicit_version,
                     const Manifest* manifest);

int cmd_add(const std::vector<std::string>& rest) {
    if (rest.empty()) {
        std::cerr << "usage: wave add <path|git-url|std-module>" << std::endl;
        return 2;
    }
    const std::string& pkg = rest[0];
    // std modules are built into the compiler/runtime — nothing to install
    if (pkg == "net" || pkg == "http" || pkg == "fs" || pkg == "crypto" || pkg == "sql" ||
        pkg == "map") {
        std::cout << "'" << pkg << "' is a built-in std module — available with `import "
                  << pkg << "`" << "\n";
        return 0;
    }
    // an explicit version suffix is honored for source packages
    std::string source = pkg;
    std::optional<std::string> explicit_version;
    {
        size_t at = pkg.rfind('@');
        if (at != std::string::npos && at > 0 && at + 1 < pkg.size() &&
            std::isdigit(static_cast<unsigned char>(pkg[at + 1]))) {
            source = pkg.substr(0, at);
            explicit_version = pkg.substr(at + 1);
        }
    }

    ManifestLoad loaded = manifest_load();
    std::optional<Manifest> manifest;
    if (loaded.m) manifest = std::move(*loaded.m);

    // local directory package
    fs::path path(source);
    {
        std::error_code ec;
        if (fs::is_directory(path, ec)) {
            return install_from_dir(path, explicit_version,
                                    manifest ? &*manifest : nullptr);
        }
    }

    // git URL: shallow-clone into a temp dir, then install like a local package
    bool is_git_url = source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0 ||
                      (source.size() >= 4 &&
                       source.compare(source.size() - 4, 4, ".git") == 0);
    if (is_git_url) {
        fs::path tmp = temp_dir() / ("wave-install-" +
                                     std::to_string(pipeline::md5ish(source)));
        std::error_code ec;
        if (fs::exists(tmp, ec)) {
            std::error_code ig;
            fs::remove_all(tmp, ig);
        }
        std::cerr << "cloning " << source << "..." << std::endl;
        bool ok = run_process({"git", "clone", "--depth", "1", source, disp(tmp)});
        if (!ok) {
            std::cerr << "error: git clone failed for " << source << std::endl;
            return 1;
        }
        int code = install_from_dir(tmp, explicit_version, manifest ? &*manifest : nullptr);
        std::error_code ig;
        fs::remove_all(tmp, ig);
        return code;
    }

    std::cerr << "error: unknown package '" << source << "'" << std::endl;
    std::cerr << "  std modules net, http, fs, crypto, sql and map are built-in (`import fs`, "
                 "`import crypto`, ...)" << std::endl;
    std::cerr << "  install source packages from a local path or a git URL:" << std::endl;
    std::cerr << "    wave add ./my-pkg" << std::endl;
    std::cerr << "    wave add https://github.com/user/my-pkg.git" << std::endl;
    return 1;
}

// Install a package directory into the global registry
// (~/.wave/registry/<name>/<version>/) and record it as a dependency of the
// current project when a package.json is present.
int install_from_dir(const fs::path& dir, std::optional<std::string> explicit_version,
                     const Manifest* manifest) {
    fs::path main_tsm = dir / "main.tsm";
    {
        std::error_code ec;
        if (!fs::is_regular_file(main_tsm, ec)) {
            std::cerr << "error: " << disp(dir)
                      << " has no main.tsm (packages need an entry file)" << std::endl;
            return 1;
        }
    }
    // package metadata comes from the package's own package.json when present
    std::optional<std::string> pkg_name;
    std::optional<std::string> pkg_version;
    {
        std::string ioerr;
        auto text = read_file(dir / "package.json", ioerr);
        if (text) {
            std::string jerr;
            Json v = json_parse_or_throw(*text, jerr);
            if (jerr.empty()) {
                if (const Json* n = v.get("name")) {
                    if (const std::string* s = n->as_str()) pkg_name = *s;
                }
                if (const Json* n = v.get("version")) {
                    if (const std::string* s = n->as_str()) pkg_version = *s;
                }
            }
        }
    }
    std::string name;
    if (pkg_name) {
        name = *pkg_name;
    } else if (auto fn = rust_file_name(disp(dir))) {
        name = *fn;
    } else {
        std::cerr << "error: cannot determine a package name for " << disp(dir)
                  << std::endl;
        return 1;
    }
    std::string version = explicit_version
                              ? *explicit_version
                              : (pkg_version ? *pkg_version : std::string("0.1.0"));
    if (!marina::ir::is_kebab_case(name)) {
        std::cerr << "error: package names must be kebab-case (lowercase words "
                     "separated by '-'), got '"
                  << name << "'" << std::endl;
        return 1;
    }

    fs::path dest = fs::path(pipeline::registry_root()) / name / version;
    if (auto e = copy_dir_recursive(dir, dest)) {
        std::cerr << "error: cannot install to " << disp(dest) << ": " << *e << std::endl;
        return 1;
    }

    if (manifest) {
        const Json* cur = manifest->deps.get(name);
        bool same = cur && cur->as_str() && *cur->as_str() == version;
        if (!same) {
            JObj deps = manifest->deps;
            deps.set(name, Json::str(version));
            Manifest m2 = *manifest;
            m2.deps = std::move(deps);
            if (auto e = manifest_save(m2)) {
                std::cerr << "error: " << *e << std::endl;
                return 1;
            }
        }
        std::cout << "added " << name << "@" << version << " to dependencies" << "\n";
    }
    std::cout << "installed " << name << "@" << version << " into " << disp(dest)
              << "\n";
    std::cout << "import it with `import " << name << "`" << "\n";
    return 0;
}

std::optional<std::string> copy_dir_recursive(const fs::path& from, const fs::path& to) {
    try {
        std::error_code ec;
        fs::create_directories(to, ec);
        if (ec) return io_msg_ec(ec);
        for (const auto& entry : fs::directory_iterator(from)) {
            std::error_code ec2;
            // Rust DirEntry::file_type does not follow symlinks
            fs::file_status ty = entry.symlink_status(ec2);
            if (ec2) return io_msg_ec(ec2);
            fs::path dest = to / entry.path().filename();
            if (ty.type() == fs::file_type::directory) {
                if (auto e = copy_dir_recursive(entry.path(), dest)) return e;
            } else {
                std::error_code ec3;
                fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing,
                              ec3);
                if (ec3) return io_msg_ec(ec3);
            }
        }
    } catch (const fs::filesystem_error& e) {
        return io_msg_ec(e.code());
    }
    return std::nullopt;
}

int cmd_remove(const std::vector<std::string>& rest) {
    if (rest.empty()) {
        std::cerr << "usage: wave remove <package>" << std::endl;
        return 2;
    }
    std::string name = rest[0];
    ManifestLoad loaded = manifest_load();
    if (!loaded.m) {
        std::cerr << "error: " << loaded.err << std::endl;
        return 1;
    }
    Manifest m = std::move(*loaded.m);
    if (!m.deps.get(name)) {
        std::cerr << "error: '" << name << "' is not in dependencies" << std::endl;
        return 1;
    }
    JObj deps = m.deps;
    deps.remove(name);
    Manifest m2 = m;
    m2.deps = std::move(deps);
    if (auto e = manifest_save(m2)) {
        std::cerr << "error: " << *e << std::endl;
        return 1;
    }
    std::cout << "removed " << name << " from dependencies" << "\n";
    return 0;
}

int cmd_check(const std::vector<std::string>& rest) {
    // with an explicit file, check works outside a project too; without one,
    // `wave check` doubles as a dependency sanity check
    std::optional<Manifest> m;
    {
        ManifestLoad loaded = manifest_load();
        if (!loaded.m) {
            if (rest.empty()) {
                std::cerr << "error: " << loaded.err << std::endl;
                return 1;
            }
        } else {
            m = std::move(*loaded.m);
        }
    }
    if (m) {
        const char* name = "?";
        const char* version = "?";
        if (const Json* n = m->root.get("name")) {
            if (const std::string* s = n->as_str()) name = s->c_str();
        }
        if (const Json* n = m->root.get("version")) {
            if (const std::string* s = n->as_str()) version = s->c_str();
        }
        std::cout << "package.json ok: " << name << " v" << version << " ("
                  << m->deps.size() << " dependencies)" << "\n";
    }

    std::string err;
    auto entry = entry_path(rest.empty() ? std::nullopt : std::optional<std::string>(rest[0]),
                            m ? &*m : nullptr, err);
    if (!entry) {
        std::cerr << "error: " << err << std::endl;
        return 1;
    }
    auto loaded = load_source(*entry);
    if (!loaded) return 1;
    auto diags = pipeline::check_source(loaded->src, loaded->base_dir);
    if (!diags) {
        std::cout << "check passed: " << loaded->file << "\n";
        return 0;
    }
    pipeline::print_diags(loaded->file, *diags);
    std::cerr << std::endl;
    std::cerr << "check failed: " << diags->size() << " error(s)" << std::endl;
    return 1;
}

int cmd_build(const std::vector<std::string>& rest) {
    // with an explicit file, build works outside a project too
    std::optional<Manifest> m;
    {
        ManifestLoad loaded = manifest_load();
        if (!loaded.m) {
            if (rest.empty()) {
                std::cerr << "error: " << loaded.err << std::endl;
                return 1;
            }
        } else {
            m = std::move(*loaded.m);
        }
    }
    std::string err;
    auto entry = entry_path(rest.empty() ? std::nullopt : std::optional<std::string>(rest[0]),
                            m ? &*m : nullptr, err);
    if (!entry) {
        std::cerr << "error: " << err << std::endl;
        return 1;
    }
    auto loaded = load_source(*entry);
    if (!loaded) return 1;
    fs::path out_dir = cwd_unwrap() / ".marina";
    pipeline::Fail fail;
    pipeline::Artifact a =
        pipeline::compile_source(loaded->src, loaded->stem, disp(out_dir), fail, loaded->base_dir);
    if (fail_is_default(fail)) {
        std::cout << "built " << a.bin_path << "\n";
        std::cout << "llvm ir: " << a.ll_path << "\n";
        return 0;
    }
    return print_fail(loaded->file, std::move(fail));
}

int cmd_run(const std::vector<std::string>& rest) {
    std::optional<Manifest> m;
    {
        ManifestLoad loaded = manifest_load();
        if (!loaded.m) {
            // `wave run <file>` also works without a project (compile + run one-off files)
            if (rest.empty()) {
                std::cerr << "error: " << loaded.err << std::endl;
                return 1;
            }
        } else {
            m = std::move(*loaded.m);
        }
    }
    std::string err;
    auto entry = entry_path(resolve_run_file(rest, m ? &*m : nullptr),
                            m ? &*m : nullptr, err);
    if (!entry) {
        std::cerr << "error: " << err << std::endl;
        return 1;
    }
    auto loaded = load_source(*entry);
    if (!loaded) return 1;
    std::string h = hex_u64(pipeline::md5ish(loaded->src));
    fs::path out_dir = temp_dir() / ("marina-run-" + h);
    pipeline::Fail fail;
    pipeline::Artifact a =
        pipeline::compile_source(loaded->src, loaded->stem, disp(out_dir), fail, loaded->base_dir);
    if (fail_is_default(fail)) {
        return pipeline::run_binary(a.bin_path);
    }
    return print_fail(loaded->file, std::move(fail));
}

void print_help() {
    std::cout << R"HELP(wave 0.2.0 — package manager for marina (ts marina)

USAGE:
    wave <COMMAND> [args]

COMMANDS:
    init               create package.json + src/main.tsm in this directory
    add <pkg>[@ver]    add a dependency to package.json
    remove <pkg>       remove a dependency from package.json
    check [file]       validate package.json + type-check the entry point
    build [file]       compile to a native binary (in ./.marina/)
    run [file]         compile + execute
    run <pkg> <cmd>    run src/<cmd>.tsm when package.json name is <pkg>
                       (example: wave run emsql migrate)

Without [file], the project entry point is used:
    "main" field from package.json, defaulting to src/main.tsm)HELP" << "\n";
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string cmd = args.empty() ? "" : args[0];
    std::vector<std::string> rest(args.begin() + static_cast<long>(std::min<size_t>(1, args.size())),
                                  args.end());

    int code;
    if (cmd == "init") {
        code = cmd_init(rest);
    } else if (cmd == "add") {
        code = cmd_add(rest);
    } else if (cmd == "remove" || cmd == "rm") {
        code = cmd_remove(rest);
    } else if (cmd == "check") {
        code = cmd_check(rest);
    } else if (cmd == "build") {
        code = cmd_build(rest);
    } else if (cmd == "run") {
        code = cmd_run(rest);
    } else if (cmd == "-h" || cmd == "--help" || cmd == "help" || cmd.empty()) {
        print_help();
        code = 0;
    } else if (cmd == "-V" || cmd == "--version") {
        std::cout << "wave " << WAVE_VERSION << "\n";
        code = 0;
    } else {
        std::cerr << "error: unknown command '" << cmd << "'" << std::endl;
        print_help();
        code = 2;
    }
    std::exit(code);
}