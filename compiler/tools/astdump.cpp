// astdump — dump the parsed AST in the exact format the self-hosted (tsm)
// parser's `mast.dumpProgram` produces, so the two front-ends can be diffed.
// Port of crates/marina/src/bin/astdump.rs.
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

#include <marina/ast.hpp>
#include <marina/lexer.hpp>
#include <marina/parser.hpp>

namespace ast = marina::ast;

namespace {

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

// Must match marina_runtime's fmt_float exactly.
std::string fmt_float(double f) {
    if (std::isfinite(f) && f == std::trunc(f) && std::fabs(f) < 1e15) {
        return fmt_i64(static_cast<int64_t>(f));
    }
    return rust_display_f64(f);
}

std::string ty(const ast::TyAnn& a) {
    if (a.args.empty()) return a.name;
    return a.name + "<" + ty(a.args[0]) + ">";
}

const char* bin_op(ast::BinOp op) {
    switch (op) {
        case ast::BinOp::Add: return "+";
        case ast::BinOp::Sub: return "-";
        case ast::BinOp::Mul: return "*";
        case ast::BinOp::Div: return "/";
        case ast::BinOp::Mod: return "%";
        case ast::BinOp::Lt: return "<";
        case ast::BinOp::Gt: return ">";
        case ast::BinOp::Le: return "<=";
        case ast::BinOp::Ge: return ">=";
        case ast::BinOp::Eq: return "==";
        case ast::BinOp::Neq: return "!=";
        case ast::BinOp::And: return "&&";
        case ast::BinOp::Or: return "||";
    }
    return "";
}

const char* un_op(ast::UnOp op) {
    switch (op) {
        case ast::UnOp::Neg: return "-";
        case ast::UnOp::Not: return "!";
    }
    return "";
}

std::string pad(size_t d) { return std::string(d * 2, ' '); }

void expr_dump(const ast::ExprPtr& e, size_t d, std::vector<std::string>& out);
void stmt_dump(const ast::Stmt& s, size_t d, std::vector<std::string>& out);
void stmts_dump(const std::vector<ast::Stmt>& ss, size_t d, std::vector<std::string>& out);

void parts_dump(const std::vector<ast::TPart>& parts, size_t d, std::vector<std::string>& out) {
    for (const auto& p : parts) {
        if (auto* s = std::get_if<ast::TStr>(&p)) {
            out.push_back(pad(d) + "PSTR " + esc(s->value));
        } else if (auto* e = std::get_if<ast::TExpr>(&p)) {
            out.push_back(pad(d) + "PEXPR");
            expr_dump(e->expr, d + 1, out);
        }
    }
}

void targs_dump(const std::vector<ast::TyAnn>& targs, size_t d, std::vector<std::string>& out) {
    for (const auto& t : targs) {
        out.push_back(pad(d) + "TARG " + ty(t));
    }
}

void exprs_dump(const std::vector<ast::ExprPtr>& es, size_t d, std::vector<std::string>& out) {
    for (const auto& e : es) {
        expr_dump(e, d, out);
    }
}

void lambda_dump(const ast::Lambda& l, size_t d, std::vector<std::string>& out) {
    const char* pre = l.is_async ? "alambda" : "lambda";
    for (const auto& p : l.params) {
        if (p.ann) {
            out.push_back(pad(d) + pre + "PARAM " + esc(p.name) + " : " + ty(*p.ann));
        } else {
            out.push_back(pad(d) + pre + "PARAM " + esc(p.name));
        }
    }
    stmts_dump(l.body, d, out);
}

void expr_dump(const ast::ExprPtr& e, size_t d, std::vector<std::string>& out) {
    std::string p = pad(d);
    std::string at = " @" + std::to_string(e->span.line) + ":" + std::to_string(e->span.col);
    if (auto* x = std::get_if<ast::EInt>(&e->v)) {
        out.push_back(p + "INT " + std::to_string(x->value) + at);
    } else if (auto* x = std::get_if<ast::EFloat>(&e->v)) {
        out.push_back(p + "FLOAT " + fmt_float(x->value) + at);
    } else if (auto* x = std::get_if<ast::EBool>(&e->v)) {
        out.push_back(p + std::string("BOOL ") + (x->value ? "true" : "false") + at);
    } else if (auto* x = std::get_if<ast::EStr>(&e->v)) {
        out.push_back(p + "STR " + esc(x->value) + at);
    } else if (auto* x = std::get_if<ast::EIdent>(&e->v)) {
        out.push_back(p + "IDENT " + esc(x->name) + at);
    } else if (auto* x = std::get_if<ast::EUnary>(&e->v)) {
        out.push_back(p + std::string("UNARY ") + un_op(x->op) + at);
        expr_dump(x->operand, d + 1, out);
    } else if (auto* x = std::get_if<ast::EBinary>(&e->v)) {
        out.push_back(p + std::string("BINARY ") + bin_op(x->op) + at);
        expr_dump(x->lhs, d + 1, out);
        expr_dump(x->rhs, d + 1, out);
    } else if (auto* x = std::get_if<ast::ETemplate>(&e->v)) {
        out.push_back(p + "TEMPLATE" + at);
        parts_dump(x->parts, d + 1, out);
    } else if (auto* x = std::get_if<ast::ETagged>(&e->v)) {
        out.push_back(p + "TAGGED" + at);
        expr_dump(x->tag, d + 1, out);
        parts_dump(x->parts, d + 1, out);
    } else if (auto* x = std::get_if<ast::ECall>(&e->v)) {
        out.push_back(p + "CALL" + at);
        expr_dump(x->callee, d + 1, out);
        exprs_dump(x->args, d + 1, out);
    } else if (auto* x = std::get_if<ast::ECallGen>(&e->v)) {
        out.push_back(p + "CALLGEN" + at);
        targs_dump(x->type_args, d + 1, out);
        expr_dump(x->callee, d + 1, out);
        exprs_dump(x->args, d + 1, out);
    } else if (auto* x = std::get_if<ast::EMember>(&e->v)) {
        out.push_back(p + "MEMBER " + esc(x->name) + at);
        expr_dump(x->obj, d + 1, out);
    } else if (auto* x = std::get_if<ast::EIndex>(&e->v)) {
        out.push_back(p + "INDEX" + at);
        expr_dump(x->obj, d + 1, out);
        expr_dump(x->index, d + 1, out);
    } else if (auto* x = std::get_if<ast::EArrayLit>(&e->v)) {
        out.push_back(p + "ARRAYLIT" + at);
        exprs_dump(x->elems, d + 1, out);
    } else if (auto* x = std::get_if<ast::ENewCtor>(&e->v)) {
        std::string path;
        for (size_t i = 0; i < x->path.size(); i++) {
            if (i > 0) path += ".";
            path += x->path[i];
        }
        out.push_back(p + "NEWCTOR " + esc(path) + at);
        exprs_dump(x->args, d + 1, out);
    } else if (auto* x = std::get_if<ast::EObjectLit>(&e->v)) {
        out.push_back(p + "OBJECTLIT" + at);
        for (const auto& f : x->fields) {
            out.push_back(pad(d + 1) + "FIELD " + esc(f.name));
            expr_dump(f.value, d + 2, out);
        }
    } else if (auto* x = std::get_if<ast::ENewClass>(&e->v)) {
        out.push_back(p + "NEWCLASS " + esc(x->name) + at);
        targs_dump(x->type_args, d + 1, out);
    } else if (auto* x = std::get_if<ast::EAwait>(&e->v)) {
        out.push_back(p + "AWAIT" + at);
        expr_dump(x->operand, d + 1, out);
    } else if (auto* x = std::get_if<ast::ENewPromise>(&e->v)) {
        out.push_back(p + "NEWPROMISE" + at);
        expr_dump(x->executor, d + 1, out);
    } else if (auto* x = std::get_if<ast::ELambda>(&e->v)) {
        out.push_back(p + "LAMBDA" + at);
        lambda_dump(*x->lambda, d + 1, out);
    }
}

void stmt_dump(const ast::Stmt& s, size_t d, std::vector<std::string>& out) {
    std::string p = pad(d);
    ast::Span sp = ast::stmt_span(s);
    std::string at = " @" + std::to_string(sp.line) + ":" + std::to_string(sp.col);
    if (auto* x = std::get_if<ast::SLet>(&s.v)) {
        const char* k = x->is_const ? "CONST" : "LET";
        if (x->ann) {
            out.push_back(p + k + " " + esc(x->name) + " : " + ty(*x->ann) + at);
        } else {
            out.push_back(p + k + " " + esc(x->name) + at);
        }
        expr_dump(x->init, d + 1, out);
    } else if (auto* x = std::get_if<ast::SAssign>(&s.v)) {
        out.push_back(p + "ASSIGN " + esc(x->name) + at);
        expr_dump(x->value, d + 1, out);
    } else if (auto* x = std::get_if<ast::SMemberAssign>(&s.v)) {
        out.push_back(p + "MSET " + esc(x->obj) + " " + esc(x->field) + at);
        expr_dump(x->value, d + 1, out);
    } else if (auto* x = std::get_if<ast::SIndexAssign>(&s.v)) {
        out.push_back(p + "ISET " + esc(x->name) + at);
        expr_dump(x->index, d + 1, out);
        expr_dump(x->value, d + 1, out);
    } else if (auto* x = std::get_if<ast::SReturn>(&s.v)) {
        out.push_back(p + "RETURN" + at);
        if (x->value) {
            expr_dump(*x->value, d + 1, out);
        }
    } else if (auto* x = std::get_if<ast::SIf>(&s.v)) {
        out.push_back(p + "IF" + at);
        expr_dump(x->cond, d + 1, out);
        stmts_dump(x->then_body, d + 1, out);
        if (x->else_body) {
            out.push_back(p + "ELSE");
            stmts_dump(*x->else_body, d + 1, out);
        }
    } else if (auto* x = std::get_if<ast::SWhile>(&s.v)) {
        out.push_back(p + "WHILE" + at);
        expr_dump(x->cond, d + 1, out);
        stmts_dump(x->body, d + 1, out);
    } else if (auto* x = std::get_if<ast::SFor>(&s.v)) {
        out.push_back(p + "FOR" + at);
        if (x->init) {
            out.push_back(p + " INIT");
            stmt_dump(**x->init, d + 2, out);
        }
        if (x->cond) {
            out.push_back(p + " COND");
            expr_dump(*x->cond, d + 2, out);
        }
        if (x->update) {
            out.push_back(p + " UPD");
            stmt_dump(**x->update, d + 2, out);
        }
        out.push_back(p + " BODY");
        stmts_dump(x->body, d + 2, out);
    } else if (auto* x = std::get_if<ast::SExpr>(&s.v)) {
        out.push_back(p + "EXPRSTMT" + at);
        expr_dump(x->expr, d + 1, out);
    }
}

void stmts_dump(const std::vector<ast::Stmt>& ss, size_t d, std::vector<std::string>& out) {
    for (const auto& s : ss) {
        stmt_dump(s, d, out);
    }
}

void fn_dump(const ast::FnDecl& f, size_t d, std::vector<std::string>& out) {
    std::string p = pad(d);
    const char* pre = f.is_async ? "AFN" : "FN";
    std::string tp;
    for (size_t i = 0; i < f.type_params.size(); i++) {
        if (i == 0) {
            tp = "<";
        } else {
            tp = tp + ", ";
        }
        tp = tp + f.type_params[i];
    }
    if (!f.type_params.empty()) {
        tp = tp + ">";
    }
    std::string at = " @" + std::to_string(f.span.line) + ":" + std::to_string(f.span.col);
    if (f.ret_ann) {
        out.push_back(p + pre + " " + esc(f.name) + tp + " : " + ty(*f.ret_ann) + at);
    } else {
        out.push_back(p + pre + " " + esc(f.name) + tp + at);
    }
    for (const auto& prm : f.params) {
        if (prm.ann) {
            out.push_back(pad(d + 1) + "PARAM " + esc(prm.name) + " : " + ty(*prm.ann));
        } else {
            out.push_back(pad(d + 1) + "PARAM " + esc(prm.name));
        }
    }
    stmts_dump(f.body, d + 1, out);
}

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
    if (argc < 2) panic_exit("usage: astdump <file.tsm>");
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
    ast::Program program;
    try {
        program = ast::parse(std::move(tokens));
    } catch (const ast::ParseError& e) {
        std::cerr << "PARSEERR " << e.msg << " @" << e.span.line << ":" << e.span.col
                  << std::endl;
        return 1;
    }
    // the tsm compiler resolves `use` at the pipeline level; the raw use
    // list is part of both dumps
    std::vector<std::string> out;
    for (const auto& u : program.uses) {
        out.push_back("USE " + esc(u.first) + " @" + std::to_string(u.second.line) +
                      ":" + std::to_string(u.second.col));
    }
    for (const auto& it : program.interfaces) {
        out.push_back("IFACE " + esc(it.name) + " @" + std::to_string(it.span.line) +
                      ":" + std::to_string(it.span.col));
        for (const auto& f : it.fields) {
            out.push_back("  IFIELDFLD " + esc(f.first) + " : " + ty(f.second) + " @" +
                          std::to_string(f.second.span.line) + ":" +
                          std::to_string(f.second.span.col));
        }
    }
    for (const auto& c : program.classes) {
        out.push_back("CLASS " + esc(c.name) + " @" + std::to_string(c.span.line) +
                      ":" + std::to_string(c.span.col));
        for (const auto& t : c.type_params) {
            out.push_back("  TYPEPARAM " + esc(t));
        }
        for (const auto& f : c.fields) {
            out.push_back("  FIELDFLD " + esc(f.first) + " : " + ty(f.second) + " @" +
                          std::to_string(f.second.span.line) + ":" +
                          std::to_string(f.second.span.col));
        }
    }
    for (const auto& f : program.fns) {
        fn_dump(f, 0, out);
    }
    stmts_dump(program.stmts, 0, out);
    for (const auto& line : out) {
        std::cout << line << "\n";
    }
    return 0;
}