// marina-lsp — a minimal Language Server Protocol implementation for
// marina (.tsm) programs, used by the "marina Language Support" editor
// extension (see editors/marina-lsp/).
//
// Protocol: LSP over stdio (Content-Length framed JSON-RPC). Supported:
//   * initialize / initialized / shutdown / exit
//   * textDocument/didOpen + didChange (full sync) → publishDiagnostics
//     (lex, parse and type-check errors, plus the strict naming rules)
//   * textDocument/hover → class shape info (fields + types, ghost classes)
//
// Port of crates/marina/src/bin/marina-lsp.rs.
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <marina/ast.hpp>
#include <marina/checker.hpp>
#include <marina/lexer.hpp>
#include <marina/parser.hpp>
#include <marina/pipeline.hpp>

namespace fs = std::filesystem;

namespace lex = marina::lex;
namespace ast = marina::ast;
namespace ir = marina::ir;
namespace pipeline = marina::pipeline;

namespace {

// ---------------------------------------------------------------------------
// Minimal JSON value + parser (enough for LSP messages)
// ---------------------------------------------------------------------------

struct Json;
using JsonArr = std::vector<Json>;

// Object kept sorted by key (to_json emits keys in sorted order, matching the
// Rust implementation's HashMap + sort).
struct JsonObj {
    std::vector<std::pair<std::string, Json>> entries;

    const Json* get(const std::string& key) const;
    Json* get(const std::string& key);
    void set(const std::string& key, Json value);
};

struct Json {
    std::variant<std::monostate, bool, double, std::string, JsonArr, JsonObj> v;

    static Json null() { return Json(); }
    static Json boolean(bool b) {
        Json j;
        j.v = b;
        return j;
    }
    static Json num(double n) {
        Json j;
        j.v = n;
        return j;
    }
    static Json str(std::string s) {
        Json j;
        j.v = std::move(s);
        return j;
    }
    static Json array(JsonArr a) {
        Json j;
        j.v = std::move(a);
        return j;
    }
    static Json object(JsonObj o) {
        Json j;
        j.v = std::move(o);
        return j;
    }

    const Json* get(const std::string& key) const {
        if (auto* m = std::get_if<JsonObj>(&v)) return m->get(key);
        return nullptr;
    }
    const std::string* as_str() const {
        if (auto* s = std::get_if<std::string>(&v)) return s;
        return nullptr;
    }
    std::optional<int64_t> as_i64() const {
        if (auto* n = std::get_if<double>(&v)) return static_cast<int64_t>(*n);
        return std::nullopt;
    }

    std::string to_json() const;
};

const Json* JsonObj::get(const std::string& key) const {
    auto it = std::lower_bound(entries.begin(), entries.end(), key,
                               [](const auto& e, const std::string& k) { return e.first < k; });
    if (it != entries.end() && it->first == key) return &it->second;
    return nullptr;
}

Json* JsonObj::get(const std::string& key) {
    auto it = std::lower_bound(entries.begin(), entries.end(), key,
                               [](const auto& e, const std::string& k) { return e.first < k; });
    if (it != entries.end() && it->first == key) return &it->second;
    return nullptr;
}

void JsonObj::set(const std::string& key, Json value) {
    auto it = std::lower_bound(entries.begin(), entries.end(), key,
                               [](const auto& e, const std::string& k) { return e.first < k; });
    if (it != entries.end() && it->first == key) {
        it->second = std::move(value);
    } else {
        entries.insert(it, {key, std::move(value)});
    }
}

std::string json_escape(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
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

// Rust `format!("{}", f)` for f64: shortest round-trip digits in positional
// (never scientific) notation.
std::string rust_display_f64(double f) {
    if (std::isnan(f)) return "NaN";
    if (std::isinf(f)) return f > 0 ? "inf" : "-inf";
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), f, std::chars_format::scientific);
    std::string s(buf, res.ptr);
    size_t ep = s.find('e');
    std::string mant = s.substr(0, ep);
    int expo = std::stoi(s.substr(ep + 1));
    bool neg = false;
    if (!mant.empty() && mant[0] == '-') {
        neg = true;
        mant.erase(0, 1);
    }
    std::string digits;
    for (char c : mant) {
        if (c >= '0' && c <= '9') digits.push_back(c);
    }
    std::string out;
    if (expo >= 0) {
        if (static_cast<size_t>(expo) + 1 >= digits.size()) {
            out = digits + std::string(static_cast<size_t>(expo) + 1 - digits.size(), '0');
        } else {
            out = digits.substr(0, static_cast<size_t>(expo) + 1) + "." +
                  digits.substr(static_cast<size_t>(expo) + 1);
        }
    } else {
        out = "0." + std::string(static_cast<size_t>(-expo) - 1, '0') + digits;
    }
    return neg ? "-" + out : out;
}

std::string fmt_i64(int64_t v) {
    char buf[32];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, res.ptr);
}

std::string Json::to_json() const {
    if (std::holds_alternative<std::monostate>(v)) return "null";
    if (auto* b = std::get_if<bool>(&v)) return *b ? "true" : "false";
    if (auto* n = std::get_if<double>(&v)) {
        if (std::trunc(*n) == *n && std::isfinite(*n) && std::fabs(*n) < 9e15) {
            return fmt_i64(static_cast<int64_t>(*n));
        }
        return rust_display_f64(*n);
    }
    if (auto* s = std::get_if<std::string>(&v)) return json_escape(*s);
    if (auto* a = std::get_if<JsonArr>(&v)) {
        std::string out = "[";
        for (size_t i = 0; i < a->size(); i++) {
            if (i > 0) out += ",";
            out += (*a)[i].to_json();
        }
        out += "]";
        return out;
    }
    if (auto* m = std::get_if<JsonObj>(&v)) {
        std::string out = "{";
        for (size_t i = 0; i < m->entries.size(); i++) {
            if (i > 0) out += ",";
            out += json_escape(m->entries[i].first) + ":" + m->entries[i].second.to_json();
        }
        out += "}";
        return out;
    }
    return "null";
}

// ---------------------------------------------------------------------------
// JSON parser
// ---------------------------------------------------------------------------

struct JsonParser {
    const std::string& bytes;
    size_t pos = 0;

    std::optional<Json> parse() {
        skip_ws();
        return value();
    }

    void skip_ws() {
        while (pos < bytes.size()) {
            char b = bytes[pos];
            if (b == ' ' || b == '\t' || b == '\n' || b == '\r') {
                pos += 1;
            } else {
                break;
            }
        }
    }

    std::optional<Json> value() {
        skip_ws();
        if (pos >= bytes.size()) return std::nullopt; // unexpected end of json
        char b = bytes[pos];
        if (b == '{') return object();
        if (b == '[') return array();
        if (b == '"') return Json::str(string());
        if (b == 't') return lit("true", Json::boolean(true));
        if (b == 'f') return lit("false", Json::boolean(false));
        if (b == 'n') return lit("null", Json::null());
        return number();
    }

    std::optional<Json> lit(const std::string& text, Json v) {
        if (bytes.compare(pos, text.size(), text) == 0) {
            pos += text.size();
            return v;
        }
        return std::nullopt; // invalid literal near byte pos
    }

    std::optional<Json> number() {
        size_t start = pos;
        while (pos < bytes.size()) {
            char b = bytes[pos];
            if ((b >= '0' && b <= '9') || b == '-' || b == '+' || b == '.' || b == 'e' ||
                b == 'E') {
                pos += 1;
            } else {
                break;
            }
        }
        std::string text = bytes.substr(start, pos - start);
        // reject inf/nan/hex forms that std::strtod accepts but Rust rejects
        if (text.empty()) return std::nullopt;
        char c0 = text[0];
        if (c0 == 'i' || c0 == 'n' || c0 == 'N' || c0 == 'I') return std::nullopt;
        if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            return std::nullopt;
        errno = 0;
        char* end = nullptr;
        double n = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size()) return std::nullopt; // invalid number
        return Json::num(n);
    }

    // Decode one UTF-8 scalar at `pos`; advances pos. nullopt on invalid utf-8.
    std::optional<uint32_t> utf8_char() {
        unsigned char c = static_cast<unsigned char>(bytes[pos]);
        int len;
        uint32_t cp;
        if (c < 0x80) {
            pos += 1;
            return c;
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
            return std::nullopt;
        }
        if (pos + static_cast<size_t>(len) > bytes.size()) return std::nullopt;
        for (int k = 1; k < len; k++) {
            unsigned char cc = static_cast<unsigned char>(bytes[pos + k]);
            if ((cc & 0xC0) != 0x80) return std::nullopt;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
            (len == 4 && cp < 0x10000))
            return std::nullopt;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return std::nullopt;
        pos += static_cast<size_t>(len);
        return cp;
    }

    std::string string() {
        // caller verified bytes[pos] == '"'
        std::string out;
        pos += 1;
        while (true) {
            if (pos >= bytes.size()) return ""; // unterminated string
            unsigned char b = static_cast<unsigned char>(bytes[pos]);
            if (b == '"') {
                pos += 1;
                return out;
            }
            if (b == '\\') {
                pos += 1;
                if (pos >= bytes.size()) return "";
                char e = bytes[pos];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\x08'); break;
                    case 'f': out.push_back('\x0c'); break;
                    case 'u': {
                        if (pos + 5 > bytes.size()) return ""; // bad \u escape
                        uint32_t cp = 0;
                        bool bad = false;
                        for (int k = 1; k <= 4; k++) {
                            char h = bytes[pos + k];
                            uint32_t d;
                            if (h >= '0' && h <= '9') d = static_cast<uint32_t>(h - '0');
                            else if (h >= 'a' && h <= 'f') d = static_cast<uint32_t>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') d = static_cast<uint32_t>(h - 'A' + 10);
                            else { bad = true; break; }
                            cp = (cp << 4) | d;
                        }
                        if (bad) return ""; // bad \u escape
                        pos += 4;
                        // char::from_u32: surrogates / out of range → U+FFFD
                        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
                        push_utf8(out, cp);
                        break;
                    }
                    default:
                        return ""; // bad escape
                }
                pos += 1;
            } else {
                size_t save = pos;
                auto cp = utf8_char();
                if (!cp) return ""; // invalid utf-8
                out.append(bytes, save, pos - save);
            }
        }
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

    std::optional<Json> object() {
        pos += 1; // {
        JsonObj map;
        skip_ws();
        if (pos < bytes.size() && bytes[pos] == '}') {
            pos += 1;
            return Json::object(std::move(map));
        }
        while (true) {
            skip_ws();
            if (pos >= bytes.size() || bytes[pos] != '"') return std::nullopt;
            std::string key = string();
            skip_ws();
            if (pos >= bytes.size() || bytes[pos] != ':') return std::nullopt;
            pos += 1;
            auto val = value();
            if (!val) return std::nullopt;
            map.set(key, std::move(*val));
            skip_ws();
            if (pos >= bytes.size()) return std::nullopt;
            if (bytes[pos] == ',') {
                pos += 1;
            } else if (bytes[pos] == '}') {
                pos += 1;
                return Json::object(std::move(map));
            } else {
                return std::nullopt; // expected ',' or '}'
            }
        }
    }

    std::optional<Json> array() {
        pos += 1; // [
        JsonArr items;
        skip_ws();
        if (pos < bytes.size() && bytes[pos] == ']') {
            pos += 1;
            return Json::array(std::move(items));
        }
        while (true) {
            auto v = value();
            if (!v) return std::nullopt;
            items.push_back(std::move(*v));
            skip_ws();
            if (pos >= bytes.size()) return std::nullopt;
            if (bytes[pos] == ',') {
                pos += 1;
            } else if (bytes[pos] == ']') {
                pos += 1;
                return Json::array(std::move(items));
            } else {
                return std::nullopt; // expected ',' or ']'
            }
        }
    }
};

// ---------------------------------------------------------------------------
// LSP plumbing
// ---------------------------------------------------------------------------

// Buffered stdin reader (mirrors Rust's locked stdin, which buffers reads).
struct StdinReader {
    unsigned char buf[8192];
    size_t len = 0;
    size_t pos = 0;
    bool eof = false;

    int get() {
        if (pos >= len) {
            if (eof) return -1;
            len = std::fread(buf, 1, sizeof(buf), stdin);
            pos = 0;
            if (len == 0) {
                eof = true;
                return -1;
            }
        }
        return buf[pos++];
    }

    bool read_exact(std::string& out, size_t n) {
        for (size_t i = 0; i < n; i++) {
            int c = get();
            if (c < 0) return false;
            out.push_back(static_cast<char>(c));
        }
        return true;
    }
};

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

std::optional<std::string> read_message(StdinReader& stdin) {
    // read headers until a blank line
    std::string line;
    std::map<std::string, std::string> headers;
    while (true) {
        line.clear();
        while (true) {
            int byte = stdin.get();
            if (byte < 0) return std::nullopt;
            if (byte == '\n') break;
            if (byte != '\r') line.push_back(static_cast<char>(byte));
        }
        if (line.empty()) break; // blank line: end of headers
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = trim_ascii(line.substr(0, colon));
            for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            headers[key] = trim_ascii(line.substr(colon + 1));
        }
    }
    auto it = headers.find("content-length");
    if (it == headers.end()) return std::nullopt;
    // usize::from_str: optional '+', digits only
    const std::string& s = it->second;
    size_t p = 0;
    if (p < s.size() && s[p] == '+') p += 1;
    if (p >= s.size()) return std::nullopt;
    size_t len = 0;
    for (; p < s.size(); p++) {
        if (s[p] < '0' || s[p] > '9') return std::nullopt;
        if (len > (std::string::npos - 9) / 10) return std::nullopt;
        len = len * 10 + static_cast<size_t>(s[p] - '0');
    }
    std::string body;
    if (!stdin.read_exact(body, len)) return std::nullopt;
    return body;
}

void write_message(const std::string& body) {
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::cout.flush();
}

std::string response(const Json& id, Json result) {
    JsonObj m;
    m.set("jsonrpc", Json::str("2.0"));
    m.set("id", id);
    m.set("result", std::move(result));
    return Json::object(std::move(m)).to_json();
}

std::string notification(const std::string& method, Json params) {
    JsonObj m;
    m.set("jsonrpc", Json::str("2.0"));
    m.set("method", Json::str(method));
    m.set("params", std::move(params));
    return Json::object(std::move(m)).to_json();
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

// A diagnostic with an LSP range (0-based line/col).
struct LspDiag {
    uint32_t line;
    uint32_t col;
    uint32_t end_line;
    uint32_t end_col;
    std::string msg;
};

uint32_t sat_sub(uint32_t a, uint32_t b) { return a > b ? a - b : 0; }

LspDiag diag_at(const ir::Diag& d) {
    // Span is 1-based; LSP is 0-based
    return LspDiag{sat_sub(d.span.line, 1), sat_sub(d.span.col, 1),
                   sat_sub(d.span.line, 1), d.span.col, d.msg};
}

LspDiag diag_from_src(const pipeline::SrcDiag& d) {
    return LspDiag{sat_sub(d.line, 1), sat_sub(d.col, 1), sat_sub(d.line, 1), d.col,
                   d.msg};
}

bool valid_utf8(const std::string& s) {
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

// Replicates Rust `Path::ends_with(".tsm")`: a whole-path-component suffix
// match, so "a/b/main.tsm" does NOT end with ".tsm" (only a component that is
// exactly ".tsm" does).
bool path_ends_with_tsm(const std::string& p) {
    std::vector<std::string> comps;
    size_t i = 0;
    bool has_root = !p.empty() && p[0] == '/';
    if (has_root) i = 1;
    size_t start = i;
    auto push = [&](size_t b, size_t e) {
        if (b >= e) return;
        std::string c = p.substr(b, e - b);
        if (c == ".") {
            // Rust Path::components skips CurDir except at the very start of
            // a relative path
            if (comps.empty() && !has_root) comps.push_back(c);
        } else if (c != "") {
            comps.push_back(c);
        }
    };
    for (; i <= p.size(); i++) {
        if (i == p.size() || p[i] == '/') {
            push(start, i);
            start = i + 1;
        }
    }
    return !comps.empty() && comps.back() == ".tsm";
}

// Lex + parse + type-check a document and collect all diagnostics
// (including the strict naming conventions).
std::vector<LspDiag> check_document(const std::string& text, const std::string& uri) {
    std::vector<LspDiag> out;

    // file conventions: .tsm extension + kebab-case stem
    std::string path = uri;
    const std::string prefix = "file://";
    if (path.rfind(prefix, 0) == 0) path = path.substr(prefix.size());
    std::string stem;
    {
        fs::path p(path);
        std::string fn = p.filename().string();
        // Rust file_stem: strip the extension (a leading dot makes a hidden
        // file with no extension)
        size_t dot = fn.find_last_of('.');
        if (dot == std::string::npos || dot == 0) {
            stem = fn;
        } else {
            stem = fn.substr(0, dot);
        }
    }
    bool has_ext = path_ends_with_tsm(path);
    if (!has_ext) {
        out.push_back(LspDiag{0, 0, 0, 1,
                              "marina programs use the .tsm extension (ts marina)"});
    }
    if (has_ext && !ir::is_kebab_case(stem)) {
        out.push_back(
            LspDiag{0, 0, 0, 1,
                    "file names must be kebab-case (lowercase words separated by '-')"});
    }

    std::vector<lex::Token> tokens;
    try {
        tokens = lex::lex(text);
    } catch (const lex::LexError& e) {
        out.push_back(LspDiag{sat_sub(e.span.line, 1), sat_sub(e.span.col, 1),
                              sat_sub(e.span.line, 1), e.span.col, e.msg});
        return out;
    }
    ast::Program program;
    try {
        program = ast::parse(std::move(tokens));
    } catch (const ast::ParseError& e) {
        out.push_back(LspDiag{sat_sub(e.span.line, 1), sat_sub(e.span.col, 1),
                              sat_sub(e.span.line, 1), e.span.col, e.msg});
        return out;
    }
    // resolve `use` statements first (same as `wave check`): registry
    // packages are merged in place, std modules are kept as-is
    {
        auto use_diags = pipeline::resolve_uses(program);
        if (!use_diags.empty()) {
            for (const auto& d : use_diags) out.push_back(diag_from_src(d));
            return out;
        }
    }
    try {
        ir::check(program);
    } catch (const ir::CheckDiags& e) {
        for (const auto& d : e.diags) {
            out.push_back(diag_at(d));
        }
    }
    return out;
}

Json pos_json(uint32_t line, uint32_t col) {
    JsonObj m;
    m.set("line", Json::num(static_cast<double>(line)));
    m.set("character", Json::num(static_cast<double>(col)));
    return Json::object(std::move(m));
}

std::string publish_diagnostics(const std::string& uri, std::optional<int64_t> version,
                                const std::string& text) {
    std::vector<LspDiag> diags = check_document(text, uri);
    JsonArr items;
    for (const auto& d : diags) {
        JsonObj range;
        range.set("start", pos_json(d.line, d.col));
        range.set("end", pos_json(d.end_line, d.end_col));
        JsonObj item;
        item.set("range", Json::object(std::move(range)));
        item.set("severity", Json::num(1.0));
        item.set("source", Json::str("marina"));
        item.set("message", Json::str(d.msg));
        items.push_back(Json::object(std::move(item)));
    }
    JsonObj params;
    params.set("uri", Json::str(uri));
    params.set("diagnostics", Json::array(std::move(items)));
    if (version) {
        params.set("version", Json::num(static_cast<double>(*version)));
    }
    return notification("textDocument/publishDiagnostics", Json::object(std::move(params)));
}

// ---------------------------------------------------------------------------
// Hover
// ---------------------------------------------------------------------------

// Rust `str::lines()`: split on '\n', strip a trailing '\r' per line, no
// final empty line for a trailing newline.
std::vector<std::string> rust_lines(const std::string& s) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    size_t pos = 0;
    while (true) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) {
            std::string line = s.substr(pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            out.push_back(std::move(line));
            break;
        }
        std::string line = s.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
        pos = nl + 1;
        if (pos == s.size()) break; // trailing newline: no empty final line
    }
    return out;
}

// Approximates Rust char::is_alphanumeric (Unicode): ASCII letters/digits,
// plus everything non-ASCII (a superset that keeps non-ASCII letters whole).
bool is_word_char(uint32_t cp) {
    if (cp < 0x80) {
        return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
               (cp >= 'a' && cp <= 'z');
    }
    return true;
}

// Find the byte offset of the `col`-th character (0-based), or s.size().
size_t byte_col_of(const std::string& s, int64_t col) {
    size_t i = 0;
    int64_t c = 0;
    while (i < s.size()) {
        if (c >= col) return i;
        unsigned char b = static_cast<unsigned char>(s[i]);
        int len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
        i += static_cast<size_t>(len);
        c += 1;
    }
    return s.size();
}

std::string hover_class_fields(const ir::ClassDef& def, std::string md) {
    md += "\n\n";
    for (const auto& f : def.fields) {
        md += "- `" + f.first + "`: " + f.second.name() + "\n";
    }
    return md;
}

Json hover_json(std::string markdown) {
    JsonObj contents;
    contents.set("kind", Json::str("markdown"));
    contents.set("value", Json::str(std::move(markdown)));
    JsonObj m;
    m.set("contents", Json::object(std::move(contents)));
    return Json::object(std::move(m));
}

// Hover: shows the shape of a class when the cursor is on a class name.
Json hover(const std::string& text, int64_t line, int64_t col) {
    // tokenize + parse to reach the class table
    std::vector<lex::Token> tokens;
    try {
        tokens = lex::lex(text);
    } catch (const lex::LexError&) {
        return Json::null();
    }
    ast::Program program;
    try {
        program = ast::parse(std::move(tokens));
    } catch (const ast::ParseError&) {
        return Json::null();
    }
    std::vector<ir::Diag> hover_diags;
    ir::Checked checked = ir::check_with_diags(program, hover_diags);

    // find the word under the cursor (1-based span → 0-based position)
    std::vector<std::string> lines = rust_lines(text);
    std::string line_text;
    {
        int64_t idx = line > 0 ? line : 0;
        line_text = static_cast<size_t>(idx) < lines.size() ? lines[static_cast<size_t>(idx)]
                                                            : std::string();
    }
    size_t byte_col = byte_col_of(line_text, col > 0 ? col : 0);
    std::string before = line_text.substr(0, byte_col);
    std::string after = line_text.substr(byte_col);
    size_t start_ws = 0;
    {
        // scan `before` backwards for the first non-word char
        size_t i = before.size();
        bool found = false;
        while (i > 0) {
            // step back one UTF-8 char
            size_t j = i - 1;
            while (j > 0 && (static_cast<unsigned char>(before[j]) & 0xC0) == 0x80) j -= 1;
            uint32_t cp = 0x110000; // treat undecodable as non-word
            {
                unsigned char b = static_cast<unsigned char>(before[j]);
                if (b < 0x80) cp = b;
                else if ((b & 0xE0) == 0xC0 && i - j == 2) cp = (b & 0x1F);
                else if ((b & 0xF0) == 0xE0 && i - j == 3) cp = (b & 0x0F);
                else if ((b & 0xF8) == 0xF0 && i - j == 4) cp = (b & 0x07);
                // combine continuation bytes
                for (size_t k = j + 1; k < i && cp < 0x110000; k++) {
                    cp = (cp << 6) | (static_cast<unsigned char>(before[k]) & 0x3F);
                }
            }
            if (!(is_word_char(cp) || cp == '_')) {
                start_ws = i;
                found = true;
                break;
            }
            i = j;
        }
        if (!found) start_ws = 0;
    }
    size_t end_ws = after.size();
    {
        size_t i = 0;
        while (i < after.size()) {
            unsigned char b = static_cast<unsigned char>(after[i]);
            int len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
            size_t next = i + static_cast<size_t>(len);
            if (next > after.size()) next = after.size();
            uint32_t cp = 0x110000;
            if (b < 0x80) cp = b;
            else if (len >= 2) cp = b & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
            for (size_t k = i + 1; k < next && cp < 0x110000; k++) {
                cp = (cp << 6) | (static_cast<unsigned char>(after[k]) & 0x3F);
            }
            if (!(is_word_char(cp) || cp == '_')) {
                end_ws = i;
                break;
            }
            i = next;
        }
    }
    std::string word = line_text.substr(start_ws, byte_col + end_ws - start_ws);
    if (word.empty()) {
        return Json::null();
    }

    // declared class?
    const ast::ClassDecl* cl = nullptr;
    for (const auto& c : program.classes) {
        if (c.name == word) {
            cl = &c;
            break;
        }
    }
    if (cl) {
        size_t id = SIZE_MAX;
        for (size_t i = 0; i < checked.classes.size(); i++) {
            if (checked.classes[i].name == cl->name) {
                id = i;
                break;
            }
        }
        std::string md = "**class " + cl->name + "**";
        if (id < checked.classes.size()) {
            md = hover_class_fields(checked.classes[id], std::move(md));
        }
        return hover_json(std::move(md));
    }

    // variable holding an object? show its class shape on hover
    std::optional<size_t> var_id;
    {
        const auto& names = checked.var_names;
        for (size_t i = names.size(); i-- > 0;) {
            if (names[i] == word) {
                var_id = i;
                break;
            }
        }
    }
    if (var_id && *var_id < checked.var_types.size()) {
        const ir::CTy& vt = checked.var_types[*var_id];
        if (vt.tag == ir::CTy::Tag::Object) {
            if (vt.object.id < checked.classes.size()) {
                const ir::ClassDef& def = checked.classes[vt.object.id];
                std::string md = def.is_ghost ? "**" + def.name + "** (ghost class)"
                                              : "**" + def.name + "**";
                return hover_json(hover_class_fields(def, std::move(md)));
            }
        }
        if (vt.tag != ir::CTy::Tag::Void) {
            return hover_json("`" + word + "`: " + vt.name());
        }
    }

    // ghost class mentioned in an error-free program? show its shape on use
    for (const auto& def : checked.classes) {
        if (def.is_ghost && def.name == word) {
            std::string md = "**" + def.name + "** (ghost class)";
            return hover_json(hover_class_fields(def, std::move(md)));
        }
    }

    return Json::null();
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

} // namespace

int main() {
    StdinReader stdin_reader;
    std::map<std::string, std::string> docs;
    bool initialized = false;

    while (true) {
        auto body = read_message(stdin_reader);
        if (!body) break;
        if (!valid_utf8(*body)) continue;
        std::optional<Json> msg;
        {
            JsonParser parser{*body};
            msg = parser.parse();
        }
        if (!msg) continue;
        std::string method;
        if (const Json* m = msg->get("method")) {
            if (const std::string* s = m->as_str()) method = *s;
        }
        std::optional<Json> id;
        if (const Json* i = msg->get("id")) id = *i;

        if (method == "initialize") {
            JsonObj caps;
            caps.set("textDocumentSync", Json::num(1.0));
            caps.set("hoverProvider", Json::boolean(true));
            JsonObj server_info;
            server_info.set("name", Json::str("marina-lsp"));
            server_info.set("version", Json::str("0.2.0"));
            JsonObj result;
            result.set("capabilities", Json::object(std::move(caps)));
            result.set("serverInfo", Json::object(std::move(server_info)));
            if (id) {
                write_message(response(*id, Json::object(std::move(result))));
            }
        } else if (method == "initialized") {
            initialized = true;
            // re-publish diagnostics for documents opened early
            std::vector<std::string> uris;
            for (const auto& kv : docs) uris.push_back(kv.first);
            for (const auto& uri : uris) {
                std::string text = docs[uri];
                write_message(publish_diagnostics(uri, std::nullopt, text));
            }
        } else if (method == "shutdown") {
            if (id) {
                write_message(response(*id, Json::null()));
            }
        } else if (method == "exit") {
            break;
        } else if (method == "textDocument/didOpen" ||
                   method == "textDocument/didChange") {
            const Json* params = msg->get("params");
            const Json* td = params ? params->get("textDocument") : nullptr;
            if (td) {
                std::string uri;
                if (const Json* u = td->get("uri")) {
                    if (const std::string* s = u->as_str()) uri = *s;
                }
                std::string text;
                if (method == "textDocument/didOpen") {
                    if (const Json* t = td->get("text")) {
                        if (const std::string* s = t->as_str()) text = *s;
                    }
                } else {
                    if (const Json* t = td->get("text")) {
                        if (const std::string* s = t->as_str()) text = *s;
                        // incremental sync would need contentChanges handling;
                        // we advertise full sync so `text` is always present
                    }
                }
                std::optional<int64_t> version;
                if (const Json* v = td->get("version")) version = v->as_i64();
                docs[uri] = text;
                if (initialized) {
                    write_message(publish_diagnostics(uri, version, text));
                }
            }
        } else if (method == "textDocument/didSave") {
            // re-check the stored document contents on save
            const Json* params = msg->get("params");
            const Json* td = params ? params->get("textDocument") : nullptr;
            if (td) {
                std::string uri;
                if (const Json* u = td->get("uri")) {
                    if (const std::string* s = u->as_str()) uri = *s;
                }
                auto it = docs.find(uri);
                std::string text = it != docs.end() ? it->second : std::string();
                write_message(publish_diagnostics(uri, std::nullopt, text));
            }
        } else if (method == "textDocument/didClose") {
            const Json* params = msg->get("params");
            const Json* td = params ? params->get("textDocument") : nullptr;
            const Json* u = td ? td->get("uri") : nullptr;
            if (u && u->as_str()) {
                std::string uri = *u->as_str();
                docs.erase(uri);
                write_message(publish_diagnostics(uri, std::nullopt, ""));
            }
        } else if (method == "textDocument/hover") {
            const Json* params = msg->get("params");
            const Json* td = params ? params->get("textDocument") : nullptr;
            const Json* u = td ? td->get("uri") : nullptr;
            const Json* pos = params ? params->get("position") : nullptr;
            Json result = Json::null();
            if (u && u->as_str() && pos) {
                auto it = docs.find(*u->as_str());
                std::string text = it != docs.end() ? it->second : std::string();
                int64_t line = 0;
                int64_t character = 0;
                if (const Json* l = pos->get("line")) {
                    if (auto v = l->as_i64()) line = *v;
                }
                if (const Json* c = pos->get("character")) {
                    if (auto v = c->as_i64()) character = *v;
                }
                result = hover(text, line, character);
            }
            if (id) {
                write_message(response(*id, std::move(result)));
            }
        } else {
            // unknown request: answer null so the client never hangs
            if (id) {
                write_message(response(*id, Json::null()));
            }
        }
    }
    return 0;
}