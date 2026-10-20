// Lexer implementation — direct port of crates/marina/src/lexer.rs.
#include <marina/lexer.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>

namespace marina::lex {

namespace {

bool is_ident_start(char32_t c) {
    if (c < 0x80) {
        char ch = static_cast<char>(c);
        return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_' || ch == '$';
    }
    // non-ASCII letters count as identifier starts (Rust's char::is_alphabetic)
    return c > 0x7F;
}

class Lexer {
public:
    Lexer(std::u32string chars, std::optional<Span> base)
        : chars_(std::move(chars)), base_(base) {}

    // `run` consumes the lexer (matches the Rust by-value `self` methods).
    std::vector<Token> run() { return std::move(*this).run_impl(); }

private:
    std::u32string chars_;
    size_t pos_ = 0;
    uint32_t line_ = 1;
    uint32_t col_ = 1;
    std::optional<Span> base_;

    Span span() const {
        if (base_) return *base_;
        return Span{line_, col_};
    }

    std::optional<char32_t> peek() const {
        if (pos_ < chars_.size()) return chars_[pos_];
        return std::nullopt;
    }

    std::optional<char32_t> peek2() const {
        if (pos_ + 1 < chars_.size()) return chars_[pos_ + 1];
        return std::nullopt;
    }

    std::optional<char32_t> bump() {
        if (pos_ >= chars_.size()) return std::nullopt;
        char32_t c = chars_[pos_++];
        if (c == U'\n') {
            line_ += 1;
            col_ = 1;
        } else {
            col_ += 1;
        }
        return c;
    }

    template <typename T>
    T error(const std::string& msg) const {
        throw LexError{msg, span()};
    }

    std::vector<Token> run_impl() {
        std::vector<Token> out;
        for (;;) {
            skip_trivia();
            Span sp = span();
            auto c = peek();
            if (!c) {
                out.push_back(Token{TEof{}, sp});
                return out;
            }
            TokV tok;
            if (*c == U'"' || *c == U'\'') {
                tok = TStr{scan_string(*c)};
            } else if (*c == U'`') {
                tok = TTemplate{scan_template()};
            } else if (*c >= U'0' && *c <= U'9') {
                tok = scan_number();
            } else if (is_ident_start(*c)) {
                std::string name;
                while (auto ch = peek()) {
                    if (is_ident_start(*ch) ||
                        (*ch >= U'0' && *ch <= U'9')) {
                        push_utf8(name, *ch);
                        bump();
                    } else {
                        break;
                    }
                }
                if (name == "let") tok = TKw{Kw::Let};
                else if (name == "const") tok = TKw{Kw::Const};
                else if (name == "function") tok = TKw{Kw::Function};
                else if (name == "async") tok = TKw{Kw::Async};
                else if (name == "await") tok = TKw{Kw::Await};
                else if (name == "return") tok = TKw{Kw::Return};
                else if (name == "if") tok = TKw{Kw::If};
                else if (name == "else") tok = TKw{Kw::Else};
                else if (name == "while") tok = TKw{Kw::While};
                else if (name == "for") tok = TKw{Kw::For};
                else if (name == "true") tok = TKw{Kw::True};
                else if (name == "false") tok = TKw{Kw::False};
                else if (name == "new") tok = TKw{Kw::New};
                else if (name == "class") tok = TKw{Kw::Class};
                else if (name == "use") tok = TKw{Kw::Use};
                else if (name == "import") tok = TKw{Kw::Import};
                else if (name == "interface") tok = TKw{Kw::Interface};
                else tok = TIdent{std::move(name)};
            } else {
                tok = scan_punct();
            }
            out.push_back(Token{std::move(tok), sp});
        }
    }

    static void push_utf8(std::string& s, char32_t c) {
        if (c < 0x80) {
            s += static_cast<char>(c);
        } else if (c < 0x800) {
            s += static_cast<char>(0xC0 | (c >> 6));
            s += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s += static_cast<char>(0xE0 | (c >> 12));
            s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (c >> 18));
            s += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (c & 0x3F));
        }
    }

    void skip_trivia() {
        for (;;) {
            auto c = peek();
            if (!c) break;
            if (*c == U' ' || *c == U'\t' || *c == U'\r' || *c == U'\n') {
                bump();
            } else if (*c == U'/' && peek2() == U'/') {
                while (auto ch = peek()) {
                    if (*ch == U'\n') break;
                    bump();
                }
            } else if (*c == U'/' && peek2() == U'*') {
                bump();
                bump();
                for (;;) {
                    auto ch = peek();
                    if (!ch) break;
                    if (*ch == U'*' && peek2() == U'/') {
                        bump();
                        bump();
                        break;
                    }
                    bump();
                }
            } else {
                break;
            }
        }
    }

    // caller has consumed the backslash
    char32_t scan_escape(char32_t quote) {
        auto c = bump();
        if (!c) return error<char32_t>("unterminated escape sequence");
        switch (*c) {
            case U'n': return U'\n';
            case U't': return U'\t';
            case U'r': return U'\r';
            case U'0': return U'\0';
            case U'\\': return U'\\';
            case U'$': return U'$';
            default: return (*c == quote) ? quote : *c;
        }
    }

    std::string scan_string(char32_t quote) {
        bump(); // opening quote
        std::string s;
        for (;;) {
            auto c = peek();
            if (!c) return error<std::string>("unterminated string literal");
            if (*c == quote) {
                bump();
                return s;
            }
            if (*c == U'\\') {
                bump();
                push_utf8(s, scan_escape(quote));
            } else {
                push_utf8(s, *c);
                bump();
            }
        }
    }

    std::vector<LTPart> scan_template() {
        bump(); // opening backtick
        std::vector<LTPart> parts;
        std::string buf;
        for (;;) {
            Span sp = span();
            auto c = peek();
            if (!c) return error<std::vector<LTPart>>("unterminated template literal");
            if (*c == U'`') {
                bump();
                if (!buf.empty() || parts.empty()) parts.push_back(LStr{std::move(buf)});
                return parts;
            }
            if (*c == U'\\') {
                bump();
                push_utf8(buf, scan_escape(U'`'));
            } else if (*c == U'$' && peek2() == U'{') {
                bump();
                bump();
                // capture raw expression until matching '}'
                std::string raw;
                int depth = 1;
                for (;;) {
                    auto ch = peek();
                    if (!ch)
                        return error<std::vector<LTPart>>(
                            "unterminated ${...} in template literal");
                    if (*ch == U'}') {
                        depth -= 1;
                        bump();
                        if (depth == 0) break;
                        raw += '}';
                    } else if (*ch == U'{') {
                        depth += 1;
                        raw += '{';
                        bump();
                    } else if (*ch == U'"' || *ch == U'\'') {
                        char32_t q = *bump();
                        push_utf8(raw, q);
                        for (;;) {
                            auto c2 = peek();
                            if (!c2)
                                return error<std::vector<LTPart>>(
                                    "unterminated string in template expression");
                            if (*c2 == U'\\') {
                                bump();
                                if (auto esc = bump()) {
                                    raw += '\\';
                                    push_utf8(raw, *esc);
                                }
                            } else if (*c2 == q) {
                                push_utf8(raw, *c2);
                                bump();
                                break;
                            } else {
                                push_utf8(raw, *c2);
                                bump();
                            }
                        }
                    } else if (*ch == U'`') {
                        // nested template: skip it entirely
                        bump();
                        int tdepth = 0;
                        for (;;) {
                            auto c2 = peek();
                            if (!c2)
                                return error<std::vector<LTPart>>(
                                    "unterminated template in template expression");
                            if (*c2 == U'\\') {
                                bump();
                                bump();
                            } else if (*c2 == U'$' && peek2() == U'{') {
                                bump();
                                bump();
                                tdepth += 1;
                            } else if (*c2 == U'}' && tdepth > 0) {
                                tdepth -= 1;
                                bump();
                            } else if (*c2 == U'`') {
                                bump();
                                if (tdepth == 0) break;
                                tdepth = 0;
                            } else {
                                bump();
                            }
                        }
                    } else {
                        push_utf8(raw, *ch);
                        bump();
                    }
                }
                (void)sp; // span captured by LExpr below matches Rust behaviour
                parts.push_back(LStr{std::move(buf)});
                parts.push_back(LExpr{std::move(raw), sp});
            } else {
                push_utf8(buf, *c);
                bump();
            }
        }
    }

    TokV scan_number() {
        Span sp = span();
        std::string text;
        bool is_float = false;
        for (;;) {
            auto c = peek();
            if (!c) break;
            if (*c >= U'0' && *c <= U'9') {
                text += static_cast<char>(*c);
                bump();
            } else if (*c == U'.' && !is_float && peek2() && *peek2() >= U'0' &&
                       *peek2() <= U'9') {
                is_float = true;
                text += static_cast<char>(*c);
                bump();
            } else if ((*c == U'e' || *c == U'E') && !text.empty() &&
                       text.back() >= '0' && text.back() <= '9') {
                is_float = true;
                text += static_cast<char>(*c);
                bump();
                if (auto sign = peek()) {
                    if (*sign == U'+' || *sign == U'-') {
                        text += static_cast<char>(*sign);
                        bump();
                    }
                }
            } else {
                break;
            }
        }
        if (is_float) {
            try {
                size_t idx = 0;
                double f = std::stod(text, &idx);
                if (idx != text.size()) throw std::invalid_argument("bad float");
                return TFloat{f};
            } catch (...) {
                return error<TokV>("invalid float literal");
            }
        }
        errno = 0;
        char* end = nullptr;
        unsigned long long i = std::strtoull(text.c_str(), &end, 10);
        if (end != text.c_str() + text.size() || errno == ERANGE) {
            return error<TokV>("integer literal out of range");
        }
        return TInt{static_cast<uint64_t>(i)};
    }

    TokV scan_punct() {
        auto cb = bump();
        if (!cb) return error<TokV>("unexpected end of input");
        char32_t c = *cb;
        switch (c) {
            case U'+':
                if (peek() == U'+') { bump(); return TP{P::PlusPlus}; }
                if (peek() == U'=') { bump(); return TP{P::PlusAssign}; }
                return TP{P::Plus};
            case U'-':
                if (peek() == U'-') { bump(); return TP{P::MinusMinus}; }
                if (peek() == U'=') { bump(); return TP{P::MinusAssign}; }
                return TP{P::Minus};
            case U'*': return TP{P::Star};
            case U'/': return TP{P::Slash};
            case U'%': return TP{P::Percent};
            case U'=':
                if (peek() == U'=') { bump(); return TP{P::Eq}; }
                if (peek() == U'>') { bump(); return TP{P::Arrow}; }
                return TP{P::Assign};
            case U'!':
                if (peek() == U'=') { bump(); return TP{P::Neq}; }
                return TP{P::Bang};
            case U'<':
                if (peek() == U'=') { bump(); return TP{P::Le}; }
                return TP{P::Lt};
            case U'>':
                if (peek() == U'=') { bump(); return TP{P::Ge}; }
                return TP{P::Gt};
            case U'&':
                if (peek() == U'&') { bump(); return TP{P::AndAnd}; }
                return error<TokV>("single '&' is not valid (use '&&')");
            case U'|':
                if (peek() == U'|') { bump(); return TP{P::OrOr}; }
                return error<TokV>("single '|' is not valid (use '||')");
            case U'(': return TP{P::LParen};
            case U')': return TP{P::RParen};
            case U'{': return TP{P::LBrace};
            case U'}': return TP{P::RBrace};
            case U'[': return TP{P::LBracket};
            case U']': return TP{P::RBracket};
            case U',': return TP{P::Comma};
            case U':': return TP{P::Colon};
            case U';': return TP{P::Semi};
            case U'.': return TP{P::Dot};
            default: {
                std::string msg = "unexpected character '";
                push_utf8(msg, c);
                msg += "'";
                return error<TokV>(msg);
            }
        }
    }
};

std::u32string to_utf32(const std::string& src) {
    std::u32string out;
    size_t i = 0;
    while (i < src.size()) {
        unsigned char b = static_cast<unsigned char>(src[i]);
        char32_t cp = 0;
        if (b < 0x80) {
            cp = b;
            i += 1;
        } else if ((b >> 5) == 0x6) {
            cp = (b & 0x1F) << 6 | (static_cast<unsigned char>(src[i + 1]) & 0x3F);
            i += 2;
        } else if ((b >> 4) == 0xE) {
            cp = (b & 0x0F) << 12 |
                 (static_cast<unsigned char>(src[i + 1]) & 0x3F) << 6 |
                 (static_cast<unsigned char>(src[i + 2]) & 0x3F);
            i += 3;
        } else {
            cp = (b & 0x07) << 18 |
                 (static_cast<unsigned char>(src[i + 1]) & 0x3F) << 12 |
                 (static_cast<unsigned char>(src[i + 2]) & 0x3F) << 6 |
                 (static_cast<unsigned char>(src[i + 3]) & 0x3F);
            i += 4;
        }
        out.push_back(cp);
    }
    return out;
}

} // namespace

std::vector<Token> lex(const std::string& src) {
    return Lexer(to_utf32(src), std::nullopt).run();
}

std::vector<Token> lex_sub(const std::string& src, Span base) {
    return Lexer(to_utf32(src), base).run();
}

} // namespace marina::lex
