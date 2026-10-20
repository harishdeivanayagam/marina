// Lexer for the marina language (a strict, compiled TypeScript subset).
#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace marina::lex {

// ---------------------------------------------------------------------------
// Spans & tokens
// ---------------------------------------------------------------------------

struct Span {
    uint32_t line = 1;
    uint32_t col = 1;
};

inline bool operator==(const Span& a, const Span& b) {
    return a.line == b.line && a.col == b.col;
}

enum class Kw {
    Let,
    Const,
    Function,
    Async,
    Await,
    Return,
    If,
    Else,
    While,
    For,
    True,
    False,
    New,
    Class,
    Use,
    Import,
    Interface,
};

enum class P {
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    PlusPlus,
    MinusMinus,
    PlusAssign,
    MinusAssign,
    Assign,
    Eq,
    Neq,
    Lt,
    Gt,
    Le,
    Ge,
    AndAnd,
    OrOr,
    Bang,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Colon,
    Semi,
    Dot,
    Arrow,
};

// Template literal parts (lexer level): literal text, or the raw source of a
// `${...}` interpolation with the span of the `${`.
struct LStr {
    std::string value;
};
struct LExpr {
    std::string raw;
    Span span;
};
using LTPart = std::variant<LStr, LExpr>;

// Token payloads (discriminated by the variant alternative).
struct TIdent {
    std::string name;
};
struct TInt {
    int32_t value = 0;
};
struct TFloat {
    double value = 0.0;
};
struct TStr {
    std::string value;
};
struct TTemplate {
    std::vector<LTPart> parts;
};
struct TKw {
    Kw kw{};
};
struct TP {
    P p{};
};
struct TEof {};

using TokV = std::variant<TIdent, TInt, TFloat, TStr, TTemplate, TKw, TP, TEof>;

struct Token {
    TokV tok;
    Span span;
};

// Convenience predicates over the token variant.
inline bool is_p(const TokV& t, P p) {
    auto* x = std::get_if<TP>(&t);
    return x && x->p == p;
}
inline bool is_kw(const TokV& t, Kw k) {
    auto* x = std::get_if<TKw>(&t);
    return x && x->kw == k;
}
inline bool is_eof(const TokV& t) {
    return std::holds_alternative<TEof>(t);
}

// Thrown by the lexer on malformed input.
struct LexError {
    std::string msg;
    Span span;
};

std::vector<Token> lex(const std::string& src);

// Re-lex a template interpolation expression; spans are approximated with
// the span of the `${` that introduced it.
std::vector<Token> lex_sub(const std::string& src, Span base);

} // namespace marina::lex
