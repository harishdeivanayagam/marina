// tokdump — dump the token stream in the exact format the self-hosted (tsm)
// lexer's `mast.dumpTok` produces, so the two front-ends can be diffed.
// Port of crates/marina/src/bin/tokdump.rs.
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <variant>

#include <marina/lexer.hpp>

namespace {

// Escape a string the way the tsm dumpers do.
std::string esc(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '`': out += "\\`"; break;
            case '$': out += "\\$"; break;
            default: out.push_back(static_cast<char>(c)); break;
        }
    }
    return out;
}

std::string fmt_i64(int64_t v) {
    char buf[32];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, res.ptr);
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
    // value = d.ddd * 10^expo  →  decimal point sits after digit index `expo`
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

// Must match marina_runtime's fmt_float exactly (single source of truth for
// float rendering through marina_to_string / template literals).
std::string fmt_float(double f) {
    if (std::isfinite(f) && f == std::trunc(f) && std::fabs(f) < 1e15) {
        return fmt_i64(static_cast<int64_t>(f));
    }
    return rust_display_f64(f);
}

const char* kw_name(marina::lex::Kw k) {
    switch (k) {
        case marina::lex::Kw::Let: return "let";
        case marina::lex::Kw::Const: return "const";
        case marina::lex::Kw::Function: return "function";
        case marina::lex::Kw::Async: return "async";
        case marina::lex::Kw::Await: return "await";
        case marina::lex::Kw::Return: return "return";
        case marina::lex::Kw::If: return "if";
        case marina::lex::Kw::Else: return "else";
        case marina::lex::Kw::While: return "while";
        case marina::lex::Kw::For: return "for";
        case marina::lex::Kw::True: return "true";
        case marina::lex::Kw::False: return "false";
        case marina::lex::Kw::New: return "new";
        case marina::lex::Kw::Class: return "class";
        case marina::lex::Kw::Use: return "use";
        case marina::lex::Kw::Import: return "import";
        case marina::lex::Kw::Interface: return "interface";
    }
    return "";
}

const char* p_name(marina::lex::P p) {
    switch (p) {
        case marina::lex::P::Plus: return "+";
        case marina::lex::P::Minus: return "-";
        case marina::lex::P::Star: return "*";
        case marina::lex::P::Slash: return "/";
        case marina::lex::P::Percent: return "%";
        case marina::lex::P::PlusPlus: return "++";
        case marina::lex::P::MinusMinus: return "--";
        case marina::lex::P::PlusAssign: return "+=";
        case marina::lex::P::MinusAssign: return "-=";
        case marina::lex::P::Assign: return "=";
        case marina::lex::P::Eq: return "==";
        case marina::lex::P::Neq: return "!=";
        case marina::lex::P::Lt: return "<";
        case marina::lex::P::Gt: return ">";
        case marina::lex::P::Le: return "<=";
        case marina::lex::P::Ge: return ">=";
        case marina::lex::P::AndAnd: return "&&";
        case marina::lex::P::OrOr: return "||";
        case marina::lex::P::Bang: return "!";
        case marina::lex::P::LParen: return "(";
        case marina::lex::P::RParen: return ")";
        case marina::lex::P::LBrace: return "{";
        case marina::lex::P::RBrace: return "}";
        case marina::lex::P::LBracket: return "[";
        case marina::lex::P::RBracket: return "]";
        case marina::lex::P::Comma: return ",";
        case marina::lex::P::Colon: return ":";
        case marina::lex::P::Semi: return ";";
        case marina::lex::P::Dot: return ".";
        case marina::lex::P::Arrow: return "=>";
    }
    return "";
}

std::string dump_tok(const marina::lex::Token& t) {
    std::string at = " @" + std::to_string(t.span.line) + ":" + std::to_string(t.span.col);
    if (auto* x = std::get_if<marina::lex::TIdent>(&t.tok)) {
        return "IDENT " + esc(x->name) + at;
    }
    if (auto* x = std::get_if<marina::lex::TInt>(&t.tok)) {
        return "INT " + std::to_string(x->value) + at;
    }
    if (auto* x = std::get_if<marina::lex::TFloat>(&t.tok)) {
        return "FLOAT " + fmt_float(x->value) + at;
    }
    if (auto* x = std::get_if<marina::lex::TStr>(&t.tok)) {
        return "STR " + esc(x->value) + at;
    }
    if (auto* x = std::get_if<marina::lex::TTemplate>(&t.tok)) {
        std::string out = "TPL" + at;
        for (const auto& p : x->parts) {
            if (auto* s = std::get_if<marina::lex::LStr>(&p)) {
                out += "\n  PSTR " + esc(s->value);
            } else if (auto* e = std::get_if<marina::lex::LExpr>(&p)) {
                out += "\n  PEXPR " + esc(e->raw) + " @" +
                       std::to_string(e->span.line) + ":" + std::to_string(e->span.col);
            }
        }
        return out;
    }
    if (auto* x = std::get_if<marina::lex::TKw>(&t.tok)) {
        return std::string("KW ") + kw_name(x->kw) + at;
    }
    if (auto* x = std::get_if<marina::lex::TP>(&t.tok)) {
        return std::string("P ") + p_name(x->p) + at;
    }
    return "EOF" + at;
}

// Emulate a Rust `.expect(...)` / `.unwrap()` panic (exit code 101).
[[noreturn]] void panic_exit(const std::string& what) {
    std::cerr << "thread 'main' panicked: " << what << std::endl;
    std::exit(101);
}

std::string io_msg(int err) {
    return std::string(std::strerror(err)) + " (os error " + std::to_string(err) + ")";
}

std::string read_file_or_panic(const std::string& path) {
    errno = 0;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) panic_exit("cannot read " + path + ": " + io_msg(errno));
    std::string out;
    char buf[65536];
    size_t n;
    errno = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    bool failed = std::ferror(f) != 0;
    int err = errno;
    std::fclose(f);
    if (failed) panic_exit("cannot read " + path + ": " + io_msg(err));
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) panic_exit("usage: tokdump <file.tsm>");
    std::string path = argv[1];
    std::string src = read_file_or_panic(path);
    std::vector<marina::lex::Token> tokens;
    try {
        tokens = marina::lex::lex(src);
    } catch (const marina::lex::LexError& e) {
        std::cerr << "LEXERR " << e.msg << " @" << e.span.line << ":" << e.span.col
                  << std::endl;
        return 1;
    }
    for (const auto& t : tokens) {
        std::cout << dump_tok(t) << "\n";
    }
    return 0;
}