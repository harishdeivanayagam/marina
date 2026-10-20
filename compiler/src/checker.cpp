// Strict type checker for marina.
//
// C++20 port of crates/marina/src/checker.rs (the Rust source is the
// specification; error strings and behaviour are ported 1:1).
//
// Rules (v0.1):
//   * Function parameters REQUIRE type annotations (strict mode).
//   * No `any`, no implicit truthiness, no string arithmetic.
//   * Numeric types are Int8/16/32/64, UInt8/16/32/64, Float32 and Float64.
//     Unsuffixed integers default to Int32 (Int64 / UInt64 when the literal
//     does not fit). Unsuffixed floats default to Float64.
//   * Implicit conversions: any integer widens to Float32 or Float64, and
//     Float32 widens to Float64. A numeric literal may also be used as any
//     integer or float type it fits in. Integer widths do not convert.
//   * `await` is allowed only at statement level:
//       `let x = await e`, `await e`, or `return await e`.
//   * Lambdas are only allowed as arguments to `setTimeout` / `new Promise`.
//
// The checker lowers the AST to a fully-typed IR (`CStmt` / `CExpr`) that the
// code generator consumes 1:1.

#include <marina/checker.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace marina::ir {

// ---------------------------------------------------------------------------
// Checked types: display names, equality, factories
// ---------------------------------------------------------------------------

std::string CTy::name() const {
    switch (tag) {
        case Tag::Void: return "void";
        case Tag::Int:
            return (unsign ? "UInt" : "Int") + std::to_string(bits == 0 ? 32 : bits);
        case Tag::Float: return bits == 32 ? "Float32" : "Float64";
        case Tag::Bool: return "bool";
        case Tag::Str: return "string";
        case Tag::Promise:
            return "Promise<" + (inner ? inner->name() : std::string("void")) + ">";
        case Tag::Resolver:
            return "Resolver<" + (inner ? inner->name() : std::string("void")) + ">";
        case Tag::Array:
            return "Array<" + (inner ? inner->name() : std::string("void")) + ">";
        case Tag::Object: return object.name;
        case Tag::Stream: return "Stream";
        case Tag::Interface: return iface;
    }
    return "void";
}

bool CTy::operator==(const CTy& o) const {
    if (tag != o.tag) return false;
    switch (tag) {
        case Tag::Promise:
        case Tag::Resolver:
        case Tag::Array:
            if (bool(inner) != bool(o.inner)) return false;
            return !inner || *inner == *o.inner;
        case Tag::Object: return object == o.object; // identity by id
        case Tag::Interface: return iface == o.iface;
        case Tag::Int: return bits == o.bits && unsign == o.unsign;
        case Tag::Float: return bits == o.bits;
        default: return true; // Void/Bool/Str/Stream
    }
}

CTy cty_void() { return CTy{}; }

CTy cty_int(uint8_t bits, bool unsign) {
    CTy t;
    t.tag = CTy::Tag::Int;
    t.bits = bits;
    t.unsign = unsign;
    return t;
}

CTy cty_float(uint8_t bits) {
    CTy t;
    t.tag = CTy::Tag::Float;
    t.bits = bits == 32 ? 32 : 64;
    return t;
}

CTy cty_bool() {
    CTy t;
    t.tag = CTy::Tag::Bool;
    return t;
}

CTy cty_str() {
    CTy t;
    t.tag = CTy::Tag::Str;
    return t;
}

CTy cty_stream() {
    CTy t;
    t.tag = CTy::Tag::Stream;
    return t;
}

CTy cty_promise(CTy inner) {
    CTy t;
    t.tag = CTy::Tag::Promise;
    t.inner = std::make_shared<CTy>(std::move(inner));
    return t;
}

CTy cty_resolver(CTy inner) {
    CTy t;
    t.tag = CTy::Tag::Resolver;
    t.inner = std::make_shared<CTy>(std::move(inner));
    return t;
}

CTy cty_array(CTy elem) {
    CTy t;
    t.tag = CTy::Tag::Array;
    t.inner = std::make_shared<CTy>(std::move(elem));
    return t;
}

CTy cty_object(size_t id, std::string name) {
    CTy t;
    t.tag = CTy::Tag::Object;
    t.object.id = id;
    t.object.name = std::move(name);
    return t;
}

CTy cty_interface(std::string name) {
    CTy t;
    t.tag = CTy::Tag::Interface;
    t.iface = std::move(name);
    return t;
}

// ---------------------------------------------------------------------------
// Strict naming conventions
// ---------------------------------------------------------------------------

/// camelCase: starts lowercase, then letters/digits only (no underscores).
bool is_camel_case(const std::string& s) {
    if (s.empty()) return false;
    unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (!(c0 >= 'a' && c0 <= 'z')) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

/// UPPER_SNAKE_CASE: uppercase letters, digits and underscores only, and it
/// must not start with a digit.
bool is_upper_snake_case(const std::string& s) {
    if (s.empty()) return false;
    unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (!((c0 >= 'A' && c0 <= 'Z') || c0 == '_')) return false;
    for (unsigned char c : s) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

/// PascalCase: starts uppercase, then letters/digits only (no underscores).
bool is_pascal_case(const std::string& s) {
    if (s.empty()) return false;
    unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (!(c0 >= 'A' && c0 <= 'Z')) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

/// PascalCase, or a module-hidden `prefix__PascalName` (non-exported class
/// or interface, renamed so other modules cannot name it).
bool is_module_pascal(const std::string& s) {
    if (is_pascal_case(s)) return true;
    size_t pos = s.rfind("__");
    if (pos == std::string::npos || pos == 0 || pos + 2 >= s.size()) return false;
    return is_pascal_case(s.substr(pos + 2));
}

/// kebab-case file stems: lowercase words separated by single hyphens.
bool is_kebab_case(const std::string& s) {
    if (s.empty()) return false;
    bool prev_hyphen = true; // leading hyphen is invalid
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            prev_hyphen = false;
        } else if (c == '-') {
            if (prev_hyphen) return false;
            prev_hyphen = true;
        } else {
            return false;
        }
    }
    return !prev_hyphen;
}

// ---------------------------------------------------------------------------
// File-local helpers
// ---------------------------------------------------------------------------

namespace {

using marina::lex::Span;

/// A function name from a merged wave registry package (`pkg__fn`): both the
/// package name and the function name must be valid camelCase identifiers.
bool is_namespaced_fn(const std::string& s) {
    size_t pos = s.find("__");
    if (pos == std::string::npos || pos == 0 || pos + 2 > s.size()) return false;
    std::string pkg = s.substr(0, pos);
    // package/module part: lowercase words (+ optional PascalCase class name
    // for lowered methods like `Calc__add`), underscores, digits
    for (unsigned char c : pkg) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_'))
            return false;
    }
    // every `__`-separated part after the module prefix must be a name
    // (camelCase fn or snake_case sub-module segment like `sub_another`)
    size_t start = pos + 2;
    for (;;) {
        size_t nxt = s.find("__", start);
        std::string part = s.substr(start, nxt == std::string::npos ? std::string::npos
                                                                    : nxt - start);
        if (part.empty()) return false;
        for (unsigned char c : part) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_'))
                return false;
        }
        if (nxt == std::string::npos) return true;
        start = nxt + 2;
        if (start >= s.size()) return false; // trailing '__'
    }
}

std::string pad4(size_t n) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04zu", n);
    return buf;
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string numstr(size_t n) { return std::to_string(n); }

// Deref an optional inner type (Promise/Resolver/Array payload).
CTy deref(const CTyPtr& p) { return p ? *p : CTy{}; }

// ---- CExpr / CStmt construction helpers ----

CExprPtr exptr(CExpr e) { return std::make_shared<CExpr>(std::move(e)); }

template <class T>
CExpr ce(T v) {
    CExpr e;
    e.v = std::move(v);
    return e;
}

template <class T>
CStmt cst(T v) {
    CStmt s;
    s.v = std::move(v);
    return s;
}

CExpr cbool(bool b) { return ce(CBool{b}); }

CStmt expr_stmt(CExpr e) {
    CExprStmt s;
    s.expr = exptr(std::move(e));
    return cst(std::move(s));
}

// `CStmt::Expr(CExpr::Bool(false))` — the Rust checker's error fallback.
CStmt fallback_stmt() { return expr_stmt(cbool(false)); }

CStmt ret_stmt(std::optional<CExpr> value, ReturnKindKind kind, std::optional<CTy> inner = std::nullopt) {
    CReturn r;
    if (value) r.value = exptr(std::move(*value));
    r.kind.kind = kind;
    if (inner) r.kind.inner = std::move(*inner);
    return cst(std::move(r));
}

bool contains_str(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

bool contains_sz(const std::vector<size_t>& v, size_t x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

/// Kind of a chainable field-validator expression (walk down the chain).
FieldKind validator_kind_of(const CExpr& c) {
    if (auto* x = std::get_if<CHttpFieldNew>(&c.v)) return x->kind;
    if (auto* x = std::get_if<CHttpFieldOp>(&c.v)) return validator_kind_of(*x->v);
    return FieldKind::Str;
}

/// Mangle-safe token for a concrete type (used in mono'd fn/class names).
std::string mangle_ty(const CTy& t) {
    switch (t.tag) {
        case CTy::Tag::Void: return "Void";
        case CTy::Tag::Int: return t.name();
        case CTy::Tag::Float: return t.name();
        case CTy::Tag::Bool: return "Bool";
        case CTy::Tag::Str: return "string";
        case CTy::Tag::Stream: return "Stream";
        case CTy::Tag::Promise: return "Promise$" + mangle_ty(deref(t.inner));
        case CTy::Tag::Array: return "Array$" + mangle_ty(deref(t.inner));
        case CTy::Tag::Object: {
            std::string s = t.object.name;
            for (char& c : s)
                if (c == '.') c = '$';
            return s;
        }
        case CTy::Tag::Interface: return "Iface$" + t.iface;
        case CTy::Tag::Resolver: return "Resolver";
    }
    return "Void";
}

// ---------------------------------------------------------------------------
// Deep-copy helpers for the AST.
//
// The Rust checker clones declarations before substituting type parameters
// (`decl.clone()` is a deep clone); the C++ AST shares expression nodes via
// shared_ptr, so monomorphization needs an explicit deep copy — otherwise
// substitution would corrupt the original declarations.
// ---------------------------------------------------------------------------

ast::TyAnn clone_tyann(const ast::TyAnn& a) { return a; } // value type

std::vector<ast::TyAnn> clone_tyanns(const std::vector<ast::TyAnn>& v) { return v; }

ast::Lambda clone_lambda(const ast::Lambda& l);

ast::ExprPtr clone_expr(const ast::ExprPtr& e) {
    if (!e) {
        return e;
    }
    auto out = std::make_shared<ast::Expr>();
    out->span = e->span;
    if (auto* x = std::get_if<ast::EInt>(&e->v)) out->v = *x;
    else if (auto* x = std::get_if<ast::EFloat>(&e->v)) out->v = *x;
    else if (auto* x = std::get_if<ast::EBool>(&e->v)) out->v = *x;
    else if (auto* x = std::get_if<ast::EStr>(&e->v)) out->v = *x;
    else if (auto* x = std::get_if<ast::EIdent>(&e->v)) out->v = *x;
    else if (auto* x = std::get_if<ast::ETemplate>(&e->v)) {
        ast::ETemplate t;
        for (const auto& p : x->parts) {
            if (auto* s = std::get_if<ast::TStr>(&p)) t.parts.push_back(*s);
            else if (auto* te = std::get_if<ast::TExpr>(&p)) t.parts.push_back(ast::TExpr{clone_expr(te->expr)});
        }
        out->v = std::move(t);
    } else if (auto* x = std::get_if<ast::ETagged>(&e->v)) {
        ast::ETagged t;
        t.tag = clone_expr(x->tag);
        for (const auto& p : x->parts) {
            if (auto* s = std::get_if<ast::TStr>(&p)) t.parts.push_back(*s);
            else if (auto* te = std::get_if<ast::TExpr>(&p)) t.parts.push_back(ast::TExpr{clone_expr(te->expr)});
        }
        out->v = std::move(t);
    } else if (auto* x = std::get_if<ast::EUnary>(&e->v)) {
        ast::EUnary u;
        u.op = x->op;
        u.operand = clone_expr(x->operand);
        out->v = std::move(u);
    } else if (auto* x = std::get_if<ast::EBinary>(&e->v)) {
        ast::EBinary b;
        b.op = x->op;
        b.lhs = clone_expr(x->lhs);
        b.rhs = clone_expr(x->rhs);
        out->v = std::move(b);
    } else if (auto* x = std::get_if<ast::ECall>(&e->v)) {
        ast::ECall c;
        c.callee = clone_expr(x->callee);
        for (const auto& a : x->args) c.args.push_back(clone_expr(a));
        out->v = std::move(c);
    } else if (auto* x = std::get_if<ast::ECallGen>(&e->v)) {
        ast::ECallGen c;
        c.callee = clone_expr(x->callee);
        c.type_args = clone_tyanns(x->type_args);
        for (const auto& a : x->args) c.args.push_back(clone_expr(a));
        out->v = std::move(c);
    } else if (auto* x = std::get_if<ast::EMember>(&e->v)) {
        ast::EMember m;
        m.obj = clone_expr(x->obj);
        m.name = x->name;
        out->v = std::move(m);
    } else if (auto* x = std::get_if<ast::EIndex>(&e->v)) {
        ast::EIndex ix;
        ix.obj = clone_expr(x->obj);
        ix.index = clone_expr(x->index);
        out->v = std::move(ix);
    } else if (auto* x = std::get_if<ast::EArrayLit>(&e->v)) {
        ast::EArrayLit a;
        for (const auto& el : x->elems) a.elems.push_back(clone_expr(el));
        out->v = std::move(a);
    } else if (auto* x = std::get_if<ast::EObjectLit>(&e->v)) {
        ast::EObjectLit o;
        for (const auto& f : x->fields) o.fields.push_back(ast::EObjectField{f.name, clone_expr(f.value)});
        out->v = std::move(o);
    } else if (auto* x = std::get_if<ast::ENewClass>(&e->v)) {
        ast::ENewClass n;
        n.name = x->name;
        n.type_args = clone_tyanns(x->type_args);
        out->v = std::move(n);
    } else if (auto* x = std::get_if<ast::ENewCtor>(&e->v)) {
        ast::ENewCtor n;
        n.path = x->path;
        for (const auto& a : x->args) n.args.push_back(clone_expr(a));
        out->v = std::move(n);
    } else if (auto* x = std::get_if<ast::EAwait>(&e->v)) {
        ast::EAwait a;
        a.operand = clone_expr(x->operand);
        out->v = std::move(a);
    } else if (auto* x = std::get_if<ast::ENewPromise>(&e->v)) {
        ast::ENewPromise p;
        p.executor = clone_expr(x->executor);
        out->v = std::move(p);
    } else if (auto* x = std::get_if<ast::ELambda>(&e->v)) {
        ast::ELambda l;
        l.lambda = std::make_shared<ast::Lambda>(clone_lambda(*x->lambda));
        out->v = std::move(l);
    }
    return out;
}

ast::Stmt clone_stmt(const ast::Stmt& s) {
    ast::Stmt out;
    if (auto* x = std::get_if<ast::SLet>(&s.v)) {
        out.v = *x;
        if (auto* o = std::get_if<ast::SLet>(&out.v)) o->init = clone_expr(o->init);
    } else if (auto* x = std::get_if<ast::SAssign>(&s.v)) {
        out.v = *x;
        if (auto* o = std::get_if<ast::SAssign>(&out.v)) o->value = clone_expr(o->value);
    } else if (auto* x = std::get_if<ast::SMemberAssign>(&s.v)) {
        out.v = *x;
        if (auto* o = std::get_if<ast::SMemberAssign>(&out.v)) o->value = clone_expr(o->value);
    } else if (auto* x = std::get_if<ast::SIndexAssign>(&s.v)) {
        out.v = *x;
        if (auto* o = std::get_if<ast::SIndexAssign>(&out.v)) {
            o->index = clone_expr(o->index);
            o->value = clone_expr(o->value);
        }
    } else if (auto* x = std::get_if<ast::SReturn>(&s.v)) {
        ast::SReturn r;
        r.span = x->span;
        if (x->value) r.value = clone_expr(*x->value);
        out.v = std::move(r);
    } else if (auto* x = std::get_if<ast::SIf>(&s.v)) {
        ast::SIf i;
        i.span = x->span;
        i.cond = clone_expr(x->cond);
        for (const auto& st : x->then_body) i.then_body.push_back(clone_stmt(st));
        if (x->else_body) {
            i.else_body.emplace();
            for (const auto& st : *x->else_body) i.else_body->push_back(clone_stmt(st));
        }
        out.v = std::move(i);
    } else if (auto* x = std::get_if<ast::SWhile>(&s.v)) {
        ast::SWhile w;
        w.span = x->span;
        w.cond = clone_expr(x->cond);
        for (const auto& st : x->body) w.body.push_back(clone_stmt(st));
        out.v = std::move(w);
    } else if (auto* x = std::get_if<ast::SFor>(&s.v)) {
        ast::SFor f;
        f.span = x->span;
        if (x->init) f.init = std::make_shared<ast::Stmt>(clone_stmt(**x->init));
        if (x->cond) f.cond = clone_expr(*x->cond);
        if (x->update) f.update = std::make_shared<ast::Stmt>(clone_stmt(**x->update));
        for (const auto& st : x->body) f.body.push_back(clone_stmt(st));
        out.v = std::move(f);
    } else if (auto* x = std::get_if<ast::SExpr>(&s.v)) {
        ast::SExpr sx;
        sx.expr = clone_expr(x->expr);
        out.v = std::move(sx);
    } else if (auto* x = std::get_if<ast::SThrow>(&s.v)) {
        ast::SThrow t;
        t.span = x->span;
        t.value = clone_expr(x->value);
        out.v = std::move(t);
    } else if (auto* x = std::get_if<ast::STryCatch>(&s.v)) {
        ast::STryCatch t;
        t.span = x->span;
        t.catch_name = x->catch_name;
        for (const auto& st : x->try_body) t.try_body.push_back(clone_stmt(st));
        for (const auto& st : x->catch_body) t.catch_body.push_back(clone_stmt(st));
        out.v = std::move(t);
    }
    return out;
}

ast::Lambda clone_lambda(const ast::Lambda& l) {
    ast::Lambda out;
    out.params = l.params; // Param is a value type
    for (const auto& st : l.body) out.body.push_back(clone_stmt(st));
    out.is_async = l.is_async;
    out.span = l.span;
    return out;
}

ast::FnDecl clone_fn_decl(const ast::FnDecl& f) {
    ast::FnDecl out;
    out.name = f.name;
    out.type_params = f.type_params;
    out.params = f.params;
    if (f.ret_ann) out.ret_ann = clone_tyann(*f.ret_ann);
    for (const auto& st : f.body) out.body.push_back(clone_stmt(st));
    out.is_async = f.is_async;
    out.span = f.span;
    return out;
}

// ---------------------------------------------------------------------------
// Checker state
// ---------------------------------------------------------------------------

struct FnSig {
    std::vector<CTy> params;
    std::optional<CTy> ret;
    bool is_async = false;
};

struct VarEntry {
    size_t id = 0;
    bool is_const = false;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// The checker
// ---------------------------------------------------------------------------

namespace {

bool lit_fits(uint64_t mag, bool neg, uint8_t bits, bool unsign) {
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) return false;
    if (neg) {
        if (unsign) return false;
        if (bits == 64) return mag <= (1ULL << 63);
        return mag <= (1ULL << (bits - 1));
    }
    if (unsign) {
        if (bits == 64) return true;
        return mag < (1ULL << bits);
    }
    if (bits == 64) return mag <= static_cast<uint64_t>(INT64_MAX);
    return mag < (1ULL << (bits - 1));
}

CTy default_int_ty(uint64_t v) {
    if (v <= 2147483647ULL) return cty_int(32, false);
    if (v <= 9223372036854775807ULL) return cty_int(64, false);
    return cty_int(64, true);
}

bool is_i32(const CTy& t) {
    return t.tag == CTy::Tag::Int && !t.unsign && (t.bits == 0 || t.bits == 32);
}

bool is_num_lit(const CExpr& e) {
    return std::holds_alternative<CInt>(e.v) || std::holds_alternative<CFloat>(e.v);
}

// Any integer widens to either float; Float32 widens to Float64.
bool num_widens(const CTy& from, const CTy& to) {
    if (from.tag == CTy::Tag::Int && to.tag == CTy::Tag::Float) return true;
    if (from.tag == CTy::Tag::Float && to.tag == CTy::Tag::Float && from.bits == 32 && to.bits != 32)
        return true;
    return false;
}

std::optional<CTy> numeric_alias(const std::string& name) {
    if (name == "int8" || name == "Int8" || name == "i8") return cty_int(8, false);
    if (name == "int16" || name == "Int16" || name == "i16") return cty_int(16, false);
    if (name == "int" || name == "int32" || name == "Int32" || name == "i32") return cty_int(32, false);
    if (name == "int64" || name == "Int64" || name == "i64") return cty_int(64, false);
    if (name == "uint8" || name == "UInt8" || name == "u8") return cty_int(8, true);
    if (name == "uint16" || name == "UInt16" || name == "u16") return cty_int(16, true);
    if (name == "uint32" || name == "UInt32" || name == "u32") return cty_int(32, true);
    if (name == "uint64" || name == "UInt64" || name == "u64") return cty_int(64, true);
    if (name == "float32" || name == "Float32" || name == "f32") return cty_float(32);
    if (name == "float" || name == "float64" || name == "Float64" || name == "f64" || name == "double" ||
        name == "number")
        return cty_float(64);
    return std::nullopt;
}

class Checker {
public:
    std::unordered_map<std::string, FnSig> sigs;
    // Abstract method signatures, keyed `Class__method` (no `this` param):
    // abstract methods are not lowered, but calls on abstract receivers
    // still type-check against them and dispatch virtually.
    std::unordered_map<std::string, FnSig> abstract_sigs;
    std::vector<CTy> var_types;
    size_t next_id = 0;
    std::vector<Diag> diags;
    std::vector<CLambda> lambdas;
    std::vector<std::unordered_map<std::string, VarEntry>> scopes;
    // current function context
    bool is_async_ctx = false;
    std::optional<CTy> ret_ty;
    bool saw_float_ret = false;
    // Width of a float return seen alongside an integer return (32 or 64).
    uint8_t ret_float_bits = 0;
    // When a function or callback has an annotated/expected return type,
    // numeric literals in `return` adapt to it.
    std::optional<CTy> ret_literal_target;
    // inferred return type of the most recently checked function
    std::optional<CTy> last_fn_ret;
    // current promise-executor inference: (T, saw_float)
    std::optional<std::pair<CTy, bool>> promise_infer;
    // are we currently checking top-level statements (not a function body)?
    bool in_toplevel = false;
    // nesting depth of if/while bodies (v0.1 restricts await + promise
    // returns to the top level of a function)
    size_t block_depth = 0;
    // lambda capture machinery
    std::vector<size_t> lambda_base; // scope index where each lambda's locals start
    std::vector<size_t> lambda_idx;  // index into lambdas for capture recording
    std::vector<std::vector<std::pair<size_t, CTy>>> cur_captures;
    // whether `return <value>` is allowed in the current lambda (callbacks)
    std::vector<bool> lambda_ret_ok;
    std::vector<size_t> reject_ids;
    // classes: declared (pass 0) + ghost (created while checking literals)
    std::vector<ClassDef> classes;
    std::unordered_map<std::string, size_t> class_by_name;
    size_t ghost_count = 0;
    // names parallel to var_types (set at declaration, empty for internals)
    std::vector<std::string> var_names;
    // modules introduced by top-level `use` statements
    std::vector<std::string> uses;
    // use alias -> mangled-name prefix for the module's functions
    // (`use sample_mod/sub` → alias `sub`, prefix `sample_mod__sub`).
    // Aliases absent from the map mangle to themselves.
    std::unordered_map<std::string, std::string> use_prefix;
    // Mangled names of functions exported from a used module, and class
    // names that arrived from a module (exported or not).
    std::unordered_set<std::string> exported_fns;
    std::unordered_set<std::string> module_classes;
    std::unordered_set<std::string> exported_classes;
    // name of the function whose body is being checked (pass 3); used to
    // decide whether `private` field access is allowed
    std::string cur_fn;
    // `let` variables holding `new http.JsonValidator({...})`: var id ->
    // ordered (field, kind) schema
    std::unordered_map<size_t, std::vector<std::pair<std::string, FieldKind>>> validator_fields;
    // ids of top-level variables (globals: lambdas may assign to them)
    std::unordered_set<size_t> toplevel_var_ids;
    // generic functions (declared with <T>): name -> declaration
    std::unordered_map<std::string, ast::FnDecl> generic_fns;
    // generic classes (declared with <T>): name -> declaration
    std::unordered_map<std::string, ast::ClassDecl> generic_classes;
    // interfaces: name -> ordered (field, type)
    std::unordered_map<std::string, std::vector<std::pair<std::string, CTy>>> interfaces;
    struct IfaceMSig {
        std::vector<CTy> params;
        CTy ret;
    };
    // interface name -> method signatures (no `this`; return type required)
    std::unordered_map<std::string, std::vector<std::pair<std::string, IfaceMSig>>> iface_methods;
    // direct subclasses of a declared class (inheritance tree)
    std::unordered_map<size_t, std::vector<size_t>> children;
    // declaration spans for diagnostics (ghost ids absent)
    std::unordered_map<size_t, Span> class_spans;

    Span def_span(size_t id) const {
        auto it = class_spans.find(id);
        return it != class_spans.end() ? it->second : Span{};
    }
    // monomorphized functions/classes generated during checking
    std::vector<CFn> mono_fns;

    void diag(std::string msg, Span span) { diags.push_back(Diag{std::move(msg), span}); }

    size_t fresh_id() {
        size_t id = next_id++;
        var_types.push_back(cty_void());
        var_names.push_back(std::string());
        return id;
    }

    static bool is_numeric(const CTy& t) { return t.tag == CTy::Tag::Int || t.tag == CTy::Tag::Float; }

    // ------------------------------------------------------------------
    // Built-in std modules
    // ------------------------------------------------------------------

    /// Built-in std classes, registered when the module is `use`d.
    /// These share the class table with user classes (ids from pass 0).
    void register_builtin_modules() {
        bool uses_http = contains_str(uses, "http");
        bool uses_sql = contains_str(uses, "sql");
        if (!uses_http && !uses_sql) return;
        if (uses_sql) {
            const std::pair<const char*, std::vector<std::pair<const char*, CTy>>> sql_builtins[] = {
                {"sql_Connection", {}},
                {"sql_Result", {{"lastInsertId", cty_int()}, {"rowsAffected", cty_int()}}},
            };
            for (const auto& [name, fields] : sql_builtins) {
                if (class_by_name.count(name)) continue;
                size_t id = classes.size();
                ClassDef d;
                d.name = name;
                for (const auto& [n, t] : fields) d.fields.emplace_back(n, t);
                d.is_ghost = false;
                classes.push_back(std::move(d));
                class_by_name[name] = id;
            }
            // a fieldless interface: query rows are dynamically bound to the
            // columns of the SELECT at runtime
            interfaces.emplace("sql_Row", std::vector<std::pair<std::string, CTy>>{});
        }
        if (!uses_http) return;
        const std::pair<const char*, std::vector<std::pair<const char*, CTy>>> builtins[] = {
            {"http_Request", {{"method", cty_str()}, {"path", cty_str()}, {"body", cty_str()}}},
            {"http_Response", {{"status", cty_int()}, {"body", cty_str()}}},
            {"http_Res", {}},
            {"http_Server", {}},
            {"http_Router", {}},
            {"http_JsonValidator", {}},
            {"http_FieldValidator", {}},
        };
        for (const auto& [name, fields] : builtins) {
            if (class_by_name.count(name)) continue;
            size_t id = classes.size();
            ClassDef d;
            d.name = name;
            for (const auto& [n, t] : fields) d.fields.emplace_back(n, t);
            d.is_ghost = false;
            classes.push_back(std::move(d));
            class_by_name[name] = id;
        }
        // dotted aliases for user-facing type annotations
        const std::pair<const char*, const char*> aliases[] = {
            {"http.Request", "http_Request"},
            {"http.Response", "http_Res"}, // server-side response object
        };
        for (const auto& [alias, target] : aliases) {
            auto it = class_by_name.find(target);
            if (it != class_by_name.end()) class_by_name.insert({alias, it->second});
        }
    }

    /// pass 0: hoist all declared classes into the class table so types can
    /// reference classes defined anywhere in the file. Generic classes
    /// (`class Box<T>`) are stashed for monomorphization instead.
    /// Passes: (A) register every non-generic class name, (B) resolve
    /// `extends` bases and detect cycles, (C) flatten fields (base fields
    /// first, so a subclass instance is layout-compatible with its bases)
    /// and record visibility/statics.
    void register_classes(const ast::Program& program) {
        // pass A: names only (enables forward/recursive references)
        std::vector<std::pair<size_t, const ast::ClassDecl*>> pending; // (id, decl)
        for (const auto& cl : program.classes) {
            if (!is_module_pascal(cl.name))
                diag("class '" + cl.name + "' must be PascalCase", cl.span);
            if (class_by_name.count(cl.name)) {
                diag("duplicate class '" + cl.name + "'", cl.span);
                continue;
            }
            if (!cl.type_params.empty()) {
                generic_classes[cl.name] = cl;
                continue;
            }
            size_t id = classes.size();
            ClassDef d;
            d.name = cl.name;
            d.is_ghost = false;
            d.is_abstract = cl.is_abstract;
            classes.push_back(std::move(d));
            class_spans[id] = cl.span;
            class_by_name[cl.name] = id;
            pending.emplace_back(id, &cl);
        }
        // pass B: resolve bases, detect inheritance cycles
        for (const auto& [id, cl] : pending) {
            if (!cl->base.has_value()) continue;
            auto it = class_by_name.find(*cl->base);
            if (it == class_by_name.end()) {
                // hidden (non-exported) module classes are renamed
                // `<prefix>__Name` by the pipeline — try each `use` prefix
                for (const auto& u : uses) {
                    auto hit = class_by_name.find(use_prefix_of(u) + "__" + *cl->base);
                    if (hit != class_by_name.end()) {
                        it = hit;
                        break;
                    }
                }
            }
            if (it == class_by_name.end()) {
                diag("unknown base class '" + *cl->base + "'", cl->span);
                continue;
            }
            if (it->second == id) {
                diag("class '" + cl->name + "' cannot extend itself", cl->span);
                continue;
            }
            // cycle check: walk the base's chain; reaching `id` is a cycle
            size_t c = it->second;
            bool cyclic = false;
            while (classes[c].base.has_value()) {
                c = *classes[c].base;
                if (c == id) {
                    cyclic = true;
                    break;
                }
            }
            if (cyclic) {
                diag("inheritance cycle involving class '" + cl->name + "'", cl->span);
                continue;
            }
            classes[id].base = it->second;
            children[it->second].push_back(id);
        }
        // pass C: fields — flattened (base first), visibility, statics
        for (const auto& [id, cl] : pending) {
            ClassDef& def = classes[id];
            std::vector<std::pair<std::string, CTy>> fields;
            std::unordered_set<std::string> seen_fields;
            if (def.base.has_value()) {
                for (const auto& [fname, fty] : classes[*def.base].fields) {
                    seen_fields.insert(fname);
                    fields.emplace_back(fname, fty);
                }
            }
            std::vector<std::pair<std::string, CTy>> statics;
            for (const auto& f : cl->fields) {
                if (!seen_fields.insert(f.name).second) {
                    diag("duplicate field '" + f.name + "' in class '" + cl->name + "'", cl->span);
                    continue;
                }
                CTy t = ty_of(f.ty);
                if (f.is_static) {
                    statics.emplace_back(f.name, t);
                    continue;
                }
                fields.emplace_back(f.name, t);
                def.own_fields.push_back(ClassDef::OwnField{f.name, f.is_private, f.is_protected});
                if (f.is_private) def.private_fields.push_back(f.name);
                if (f.is_protected) def.protected_fields.push_back(f.name);
            }
            def.fields = std::move(fields);
            def.static_fields = std::move(statics);
            for (const auto& m : cl->methods) {
                if (m.is_private) def.private_methods.push_back(m.name);
                if (m.is_protected) def.protected_methods.push_back(m.name);
                if (m.is_abstract) {
                    FnSig as;
                    for (const auto& p : m.params)
                        as.params.push_back(p.ann ? ty_of(*p.ann) : cty_void());
                    if (m.ret_ann) as.ret = ty_of(*m.ret_ann);
                    abstract_sigs[cl->name + "__" + m.name] = std::move(as);
                }
            }
        }
    }

    /// pass 0: hoist interface declarations (name -> ordered field types
    /// and method signatures). Two passes so interfaces may reference each
    /// other in any order.
    void register_interfaces(const ast::Program& program) {
        for (const auto& it : program.interfaces) {
            if (!is_module_pascal(it.name))
                diag("interface '" + it.name + "' must be PascalCase", it.span);
            if (interfaces.count(it.name) || class_by_name.count(it.name)) {
                diag("duplicate interface '" + it.name + "'", it.span);
                continue;
            }
            interfaces[it.name] = std::vector<std::pair<std::string, CTy>>{};
            iface_methods[it.name] = {};
        }
        std::unordered_set<std::string> filled;
        for (const auto& it : program.interfaces) {
            if (!interfaces.count(it.name)) continue; // duplicate, already diagnosed
            if (!filled.insert(it.name).second) continue;
            std::vector<std::pair<std::string, CTy>> fields;
            std::unordered_set<std::string> seen_fields;
            for (const auto& [fname, ann] : it.fields) {
                if (!seen_fields.insert(fname).second) continue; // parser already diagnosed duplicates
                CTy t = ty_of(ann);
                if (t.tag == CTy::Tag::Array || t.tag == CTy::Tag::Promise) {
                    diag("interface field '" + it.name + "." + fname +
                             "': Array/Promise of interfaces is not supported yet",
                         it.span);
                }
                fields.emplace_back(fname, t);
            }
            interfaces[it.name] = std::move(fields);
            std::vector<std::pair<std::string, IfaceMSig>> methods;
            for (const auto& m : it.methods) {
                if (!is_camel_case(m.name))
                    diag("interface method '" + it.name + "." + m.name + "' must be camelCase",
                         m.span);
                IfaceMSig sig;
                for (const auto& p : m.params) {
                    if (!p.ann) {
                        diag("parameter '" + p.name + "' of interface method '" + it.name + "." +
                                 m.name + "' needs a type annotation (strict mode)",
                             p.span);
                        sig.params.push_back(cty_void());
                    } else {
                        sig.params.push_back(ty_of(*p.ann));
                    }
                }
                sig.ret = ty_of(m.ret);
                methods.emplace_back(m.name, std::move(sig));
            }
            iface_methods[it.name] = std::move(methods);
        }
    }

    /// A fieldless, methodless interface (sql_Row) accepts any shape.
    bool iface_open(const std::string& name) const {
        auto it = interfaces.find(name);
        if (it == interfaces.end() || !it->second.empty()) return false;
        auto mit = iface_methods.find(name);
        return mit == iface_methods.end() || mit->second.empty();
    }

    bool iface_has_method(const std::string& iname, const std::string& method) const {
        auto mit = iface_methods.find(iname);
        if (mit == iface_methods.end()) return false;
        for (const auto& [n, _] : mit->second)
            if (n == method) return true;
        return false;
    }

    /// The id of the class that declares field `name` itself (walking up
    /// the chain), or nullopt.
    std::optional<size_t> field_owner(size_t cid, const std::string& name) const {
        for (size_t c = cid;;) {
            for (const auto& f : classes[c].own_fields)
                if (f.name == name) return c;
            if (!classes[c].base.has_value()) return std::nullopt;
            c = *classes[c].base;
        }
    }

    /// Static field read: `Name.member` where Name is a class.
    CExpr check_static_member(size_t cid, const std::string& name, Span span) {
        const std::pair<std::string, CTy>* sf = find_static_field(cid, name);
        if (!sf) {
            // static getter? (`static get name()`)
            if (auto g = find_member(cid, name, "get_")) {
                auto sit = sigs.find(g->second);
                if (sit->second.ret) {
                    CCallFn f;
                    f.name = g->second;
                    f.ret = *sit->second.ret;
                    f.is_async = false;
                    return ce(std::move(f));
                }
                diag("cannot read '" + name + "' here: the getter's return type is not yet known "
                     "(add a return type annotation)",
                     span);
                return cbool(false);
            }
            diag("class '" + classes[cid].name + "' has no static member '" + name + "'", span);
            return cbool(false);
        }
        bool priv = classes[cid].field_is_private(name);
        bool prot = std::find(classes[cid].protected_fields.begin(),
                              classes[cid].protected_fields.end(),
                              name) != classes[cid].protected_fields.end();
        if (!can_access_member(cid, priv, prot)) {
            diag(std::string("static field '" + name + "' of class '" + classes[cid].name +
                             "' is " + (priv ? "private" : "protected")),
                 span);
            return cbool(false);
        }
        CStaticRef r;
        r.gname = "@static." + classes[cid].name + "__" + name;
        r.ty = sf->second;
        return ce(std::move(r));
    }

    /// Static field write: `Name.member = v` where Name is a class.
    CStmt check_static_member_assign(size_t cid, const std::string& name,
                                      const ast::ExprPtr& value, Span span) {
        const std::pair<std::string, CTy>* sf = find_static_field(cid, name);
        if (!sf) {
            diag("class '" + classes[cid].name + "' has no static member '" + name + "'", span);
            return fallback_stmt();
        }
        bool priv = classes[cid].field_is_private(name);
        bool prot = std::find(classes[cid].protected_fields.begin(),
                              classes[cid].protected_fields.end(),
                              name) != classes[cid].protected_fields.end();
        if (!can_access_member(cid, priv, prot)) {
            diag(std::string("static field '" + name + "' of class '" + classes[cid].name +
                             "' is " + (priv ? "private" : "protected")),
                 span);
            return fallback_stmt();
        }
        CTy elem = sf->second;
        CExpr vc = check_expr(*value, false);
        adapt_literal(vc, elem);
        coerce(expr_ty(vc), elem, span);
        CStaticSet st;
        st.gname = "@static." + classes[cid].name + "__" + name;
        st.value = exptr(std::move(vc));
        st.elem = std::move(elem);
        return cst(std::move(st));
    }

    /// Is `maybe_sub` the same as, or a subclass of, `base`?
    bool is_subclass(size_t maybe_sub, size_t base) const {
        for (size_t c = maybe_sub;;) {
            if (c == base) return true;
            if (c >= classes.size() || !classes[c].base.has_value()) return false;
            c = *classes[c].base;
        }
    }

    /// Nearest implementation of a member walking up the inheritance chain.
    /// `suffix` is "" for methods, "get_" for getters, "set_" for setters,
    /// "static__" for static methods. Only lowered (non-abstract) members
    /// have entries in `sigs`, so abstract-only classes are skipped.
    std::optional<std::pair<size_t, std::string>> find_member(size_t cid,
                                                              const std::string& name,
                                                              const char* suffix) const {
        for (size_t c = cid;;) {
            std::string mangled = classes[c].name + "__" + suffix + name;
            if (sigs.count(mangled)) return std::make_pair(c, mangled);
            if (c >= classes.size() || !classes[c].base.has_value()) return std::nullopt;
            c = *classes[c].base;
        }
    }

    /// A `static` field walking up the chain (statics are inherited).
    const std::pair<std::string, CTy>* find_static_field(size_t cid,
                                                         const std::string& name) const {
        for (size_t c = cid;;) {
            const ClassDef& def = classes[c];
            for (const auto& sf : def.static_fields)
                if (sf.first == name) return &sf;
            if (!def.base.has_value()) return nullptr;
            c = *def.base;
        }
    }

    /// Does any strict descendant of `cid` define its own member `name`
    /// (making runtime dispatch necessary)?
    bool subtree_overrides(size_t cid, const std::string& name, const char* suffix) const {
        auto it = children.find(cid);
        if (it == children.end()) return false;
        for (size_t d : it->second) {
            if (sigs.count(classes[d].name + "__" + suffix + name)) return true;
            if (subtree_overrides(d, name, suffix)) return true;
        }
        return false;
    }

    /// The name of the class whose (lowered) function is being checked:
    /// `cur_fn` is `Class__method` / `Class__new` / `Class__get_x`.
    /// Hidden module classes keep `__` in their own name
    /// (`sample_mod__Base__new`), so try every `__` split and take the
    /// longest prefix that names a class.
    std::optional<size_t> cur_class_id() const {
        size_t start = 0;
        for (;;) {
            size_t pos = cur_fn.find("__", start);
            if (pos == std::string::npos || pos == 0) return std::nullopt;
            auto it = class_by_name.find(cur_fn.substr(0, pos));
            if (it != class_by_name.end()) return it->second;
            start = pos + 2;
        }
    }

    /// Visibility check for a member defined by class `definer`:
    /// private — only inside the defining class; protected — inside the
    /// defining class and its subclasses.
    bool can_access_member(size_t definer, bool priv, bool prot) const {
        if (!priv && !prot) return true;
        auto cur = cur_class_id();
        if (!cur.has_value()) return false;
        if (priv) return *cur == definer;
        return is_subclass(*cur, definer);
    }

    /// Class method `Class__name` (param 0 is `this`) matches an interface
    /// method when every parameter and the return type are identical.
    bool method_sig_eq(const FnSig& sig, const IfaceMSig& m) const {
        if (sig.is_async || !sig.ret) return false;
        if (sig.params.size() != m.params.size() + 1) return false;
        for (size_t i = 0; i < m.params.size(); ++i)
            if (!(sig.params[i + 1] == m.params[i])) return false;
        return *sig.ret == m.ret;
    }

    /// Every interface method is satisfied (directly or inherited) by a
    /// public method with the same signature. Diagnostics-free so element
    /// coercion can use it.
    bool iface_methods_match(size_t class_id, const std::string& iname) const {
        auto mit = iface_methods.find(iname);
        if (mit == iface_methods.end()) return true;
        for (const auto& [mname, msig] : mit->second) {
            auto impl = find_member(class_id, mname, "");
            if (!impl) return false;
            const ClassDef& definer = classes[impl->first];
            if (definer.method_is_private(mname) &&
                std::find(definer.private_methods.begin(), definer.private_methods.end(),
                          mname) != definer.private_methods.end()) {
                // the nearest implementation is private: does not satisfy
                return false;
            }
            auto sit = sigs.find(impl->second);
            if (sit == sigs.end() || !method_sig_eq(sit->second, msig)) return false;
        }
        return true;
    }

    /// Same as `iface_methods_match`, reporting why a class does not conform.
    bool iface_methods_ok(size_t class_id, const std::string& iname, Span span) {
        auto mit = iface_methods.find(iname);
        if (mit == iface_methods.end()) return true;
        bool ok = true;
        for (const auto& [mname, msig] : mit->second) {
            auto impl = find_member(class_id, mname, "");
            if (!impl) {
                diag("interface '" + iname + "': missing method '" + mname + "'", span);
                ok = false;
                continue;
            }
            const ClassDef& definer = classes[impl->first];
            auto sit = sigs.find(impl->second);
            bool priv = std::find(definer.private_methods.begin(), definer.private_methods.end(),
                                  mname) != definer.private_methods.end();
            bool prot = std::find(definer.protected_methods.begin(),
                                  definer.protected_methods.end(),
                                  mname) != definer.protected_methods.end();
            if (priv || prot) {
                diag(std::string("interface '" + iname + "': method '" + mname + "' of class '" +
                                 definer.name + "' is " + (priv ? "private" : "protected")),
                     span);
                ok = false;
                continue;
            }
            if (!method_sig_eq(sit->second, msig)) {
                if (!sit->second.ret) {
                    diag("interface '" + iname + "': method '" + mname + "' of class '" +
                             definer.name + "' needs a return type annotation",
                         span);
                } else {
                    diag("interface '" + iname + "': method '" + mname + "' of class '" +
                             definer.name + "' has a different signature",
                         span);
                }
                ok = false;
            }
        }
        return ok;
    }

    // ------------------------------------------------------------------
    // Types
    // ------------------------------------------------------------------

    void adapt_literal(CExpr& e, const CTy& to) {
        if (auto* x = std::get_if<CInt>(&e.v)) {
            if (to.tag == CTy::Tag::Int && lit_fits(x->value, x->neg, to.bits, to.unsign)) {
                x->bits = to.bits;
                x->unsign = to.unsign;
            }
            return;
        }
        if (auto* x = std::get_if<CFloat>(&e.v)) {
            if (to.tag == CTy::Tag::Float) x->bits = to.bits;
        }
    }

    void adapt_peers(CExpr& a, CExpr& b) {
        bool la = is_num_lit(a);
        bool lb = is_num_lit(b);
        if (la && !lb) adapt_literal(a, expr_ty(b));
        else if (lb && !la) adapt_literal(b, expr_ty(a));
    }

    void note_float_ret(const CTy& t) {
        if (t.tag != CTy::Tag::Float) return;
        saw_float_ret = true;
        uint8_t b = t.bits == 32 ? 32 : 64;
        if (b > ret_float_bits) ret_float_bits = b;
    }

    CTy apply_float_ret(CTy ret) const {
        if (saw_float_ret && ret.tag == CTy::Tag::Int)
            return cty_float(ret_float_bits == 32 ? 32 : 64);
        return ret;
    }

    CTy ty_of(const ast::TyAnn& ann) {
        const std::string& name = ann.name;
        bool has_args = !ann.args.empty();
        CTy t;
        if (auto num = numeric_alias(name)) {
            t = *num;
        } else if (name == "string" || name == "String") {
            t = cty_str();
        } else if (name == "bool" || name == "boolean" || name == "Boolean") {
            t = cty_bool();
        } else if (name == "void" || name == "undefined") {
            t = cty_void();
        } else if ((name == "Stream" || name == "stream") && !has_args) {
            t = cty_stream();
        } else if (name == "Promise" && has_args) {
            t = cty_promise(ty_of(ann.args[0]));
        } else if (name == "Promise") {
            diag("Promise requires a type parameter, e.g. Promise<string>", ann.span);
            t = cty_promise(cty_void());
        } else if (name == "Array" && has_args) {
            t = cty_array(ty_of(ann.args[0]));
        } else if (name == "Array") {
            diag("Array requires a type parameter, e.g. Array<Int32>", ann.span);
            t = cty_array(cty_int());
        } else if (class_by_name.count(name) || generic_classes.count(name)) {
            if (generic_classes.count(name)) {
                // generic class instantiation: `Box<Int32>` (or an
                // erroring placeholder when type args are missing)
                if (ann.args.empty()) {
                    diag("generic class '" + name + "' needs type parameters, e.g. " + name + "<Int32>",
                         ann.span);
                    return cty_void();
                }
                return mono_class(name, ann.args, ann.span);
            }
            if (!ann.args.empty()) {
                diag("class '" + name + "' does not take type parameters", ann.span);
            }
            size_t id = class_by_name[name];
            // canonical name (covers dotted aliases like http.Request)
            std::string display = id < classes.size() ? classes[id].name : std::string();
            t = cty_object(id, std::move(display));
        } else if (!has_args && interfaces.count(name)) {
            if (!ann.args.empty()) {
                diag("interface '" + name + "' does not take type parameters", ann.span);
            }
            t = cty_interface(name);
        } else {
            diag("unknown type '" + name + "'", ann.span);
            t = cty_void();
        }
        if (name != "Promise" && name != "Array" && !ann.args.empty()) {
            diag("type '" + name + "' does not take type parameters", ann.span);
        }
        return t;
    }

    /// Implicit numeric conversions: any integer widens to Float32 or
    /// Float64, and Float32 widens to Float64. Interface conformance is
    /// checked structurally. Integer widths do not convert here — literals
    /// are retargeted by `adapt_literal` before this is called.
    bool coerce(const CTy& from, const CTy& to, Span span) {
        if (from == to) return true;
        // a fieldless interface (e.g. sql_Row) accepts any shape: its
        // fields are bound at runtime
        if (from.tag == CTy::Tag::Interface && iface_open(from.iface)) return true;
        if (num_widens(from, to)) return true;
        // subclass upcast: a derived class flows where a base is expected
        if (from.tag == CTy::Tag::Object && to.tag == CTy::Tag::Object)
            return is_subclass(from.object.id, to.object.id);
        // element-wise coercion: Array<T> accepts Array<U> when U coerces to T
        if (from.tag == CTy::Tag::Array && to.tag == CTy::Tag::Array) {
            // no diagnostics for the element check
            if (coerce_quiet(deref(from.inner), deref(to.inner))) return true;
        }
        // interface conformance (both directions: an interface accepts any
        // conforming object, and a concrete value may flow where its
        // interface is expected)
        if (to.tag == CTy::Tag::Interface && from.tag == CTy::Tag::Object)
            return iface_conform(from.object, to.iface, span);
        if (from.tag == CTy::Tag::Interface && to.tag == CTy::Tag::Object)
            return iface_conform(to.object, from.iface, span);
        diag("type mismatch: expected " + to.name() + ", found " + from.name(), span);
        return false;
    }

    /// Quiet coercion check (no diagnostics): used for element types.
    bool coerce_quiet(const CTy& from, const CTy& to) const {
        if (from == to) return true;
        if (from.tag == CTy::Tag::Interface && iface_open(from.iface)) return true;
        if (num_widens(from, to)) return true;
        if (from.tag == CTy::Tag::Object && to.tag == CTy::Tag::Object)
            return is_subclass(from.object.id, to.object.id);
        if (to.tag == CTy::Tag::Interface && from.tag == CTy::Tag::Object)
            return iface_conform_quiet(from.object, to.iface);
        if (from.tag == CTy::Tag::Interface && to.tag == CTy::Tag::Object)
            return iface_conform_quiet(to.object, from.iface);
        return false;
    }

    /// Does class `o` satisfy interface `iname`? Every interface field must
    /// exist on the class with a matching type (Int32 widens to Float64).
    bool iface_conform(const ObjectTy& o, const std::string& iname, Span span) {
        auto it = interfaces.find(iname);
        if (it == interfaces.end()) return false;
        std::vector<std::pair<std::string, CTy>> ifields = it->second;
        if (o.id >= classes.size()) return false;
        std::vector<std::pair<std::string, CTy>> cfields = classes[o.id].fields;
        bool ok = true;
        for (const auto& [fname, fty] : ifields) {
            const std::pair<std::string, CTy>* found = nullptr;
            for (const auto& cf : cfields)
                if (cf.first == fname) {
                    found = &cf;
                    break;
                }
            if (found) {
                const CTy& t = found->second;
                if (!(t == fty) && !num_widens(t, fty)) {
                    diag("interface '" + iname + "': field '" + fname + "' has type " + t.name() +
                             ", expected " + fty.name(),
                         span);
                    ok = false;
                }
            } else {
                std::vector<std::string> names;
                names.reserve(ifields.size());
                for (const auto& [n, _] : ifields) names.push_back(n);
                diag("interface '" + iname + "': missing field '" + fname + "' (required: " +
                         join(names, ", ") + ")",
                     span);
                ok = false;
            }
        }
        if (!iface_methods_ok(o.id, iname, span)) ok = false;
        return ok;
    }

    /// Diagnostics-free conformance check (for element-type coercion).
    bool iface_conform_quiet(const ObjectTy& o, const std::string& iname) const {
        auto it = interfaces.find(iname);
        if (it == interfaces.end()) return false;
        if (o.id >= classes.size()) return false;
        const ClassDef& cdef = classes[o.id];
        for (const auto& [fname, fty] : it->second) {
            const CTy* t = nullptr;
            for (const auto& cf : cdef.fields)
                if (cf.first == fname) {
                    t = &cf.second;
                    break;
                }
            if (!t) return false;
            if (!(*t == fty) && !num_widens(*t, fty)) return false;
        }
        return iface_methods_match(o.id, iname);
    }

    // ------------------------------------------------------------------
    // Scopes
    // ------------------------------------------------------------------

    size_t declare(const std::string& name, CTy ty, bool is_const, Span span) {
        bool exists = scopes.back().count(name) != 0;
        if (exists) diag("cannot redeclare '" + name + "' in the same scope", span);
        size_t id = fresh_id();
        var_types[id] = ty;
        var_names[id] = name;
        scopes.back()[name] = VarEntry{id, is_const};
        return id;
    }

    /// Resolve an identifier. Returns Some((id, ty, is_capture)).
    std::optional<std::tuple<size_t, CTy, bool>> lookup(const std::string& name, Span span) {
        for (size_t i = scopes.size(); i-- > 0;) {
            auto it = scopes[i].find(name);
            if (it != scopes[i].end()) {
                size_t id = it->second.id;
                CTy ty = id < var_types.size() ? var_types[id] : cty_void();
                // is this inside the innermost lambda's own scope?
                bool is_capture = !lambda_base.empty() && i < lambda_base.back();
                if (is_capture && !cur_captures.empty()) {
                    auto& caps = cur_captures.back();
                    bool has = false;
                    for (const auto& [cid, _] : caps)
                        if (cid == id) {
                            has = true;
                            break;
                        }
                    if (!has) caps.emplace_back(id, ty);
                }
                return std::make_tuple(id, ty, is_capture);
            }
        }
        diag("cannot find name '" + name + "'", span);
        return std::nullopt;
    }

    // ------------------------------------------------------------------
    // Functions
    // ------------------------------------------------------------------

    CFn check_fn(const ast::FnDecl& f) {
        is_async_ctx = f.is_async;
        in_toplevel = false;
        ret_ty.reset();
        saw_float_ret = false;
        ret_float_bits = 0;
        std::optional<CTy> saved_target = std::move(ret_literal_target);
        if (f.ret_ann) {
            CTy at = ty_of(*f.ret_ann);
            if (f.is_async && at.tag == CTy::Tag::Promise) {
                ret_literal_target = deref(at.inner);
                // unify `return` values against the awaited element type so
                // sibling subclasses of the declared base all type-check
                ret_ty = deref(at.inner);
            } else if (!f.is_async) {
                ret_literal_target = at;
                ret_ty = at;
            }
        }
        scopes.emplace_back();
        std::vector<std::pair<size_t, CTy>> params;
        for (const auto& p : f.params) {
            CTy ty = p.ann ? ty_of(*p.ann) : cty_void(); // diag already emitted in pass 1
            size_t id = declare(p.name, ty, true, p.span);
            params.emplace_back(id, ty);
        }
        std::vector<CStmt> body = check_stmts(f.body);
        ret_literal_target = std::move(saved_target);

        // finalize inferred return type
        CTy ret = apply_float_ret(ret_ty ? *ret_ty : cty_void());
        if (f.is_async) ret = cty_promise(std::move(ret));
        if (f.ret_ann) {
            CTy at = ty_of(*f.ret_ann);
            coerce(ret, at, f.span);
            ret = at;
        }
        // For async fns the source-level requirement is about the awaited
        // value, not the returned promise: Promise<void> may fall off the end.
        bool requires_ret;
        if (ret.tag == CTy::Tag::Promise)
            requires_ret = deref(ret.inner).tag != CTy::Tag::Void;
        else
            requires_ret = ret.tag != CTy::Tag::Void;
        if (requires_ret) {
            // If control can fall off the end and the function returns a
            // value, require a return at the top level of the body.
            bool has_return = false;
            for (const auto& s : body)
                if (std::holds_alternative<CReturn>(s.v)) {
                    has_return = true;
                    break;
                }
            if (!has_return) {
                diag("function '" + f.name + "' returns " + ret.name() +
                         " but not all paths return a value",
                     f.span);
            }
        }
        // record the inferred return type so later-declared functions can
        // call this one without a return-type annotation
        last_fn_ret = ret;
        scopes.pop_back();
        CFn cf;
        cf.name = f.name;
        cf.params = std::move(params);
        cf.ret = ret;
        cf.is_async = f.is_async;
        cf.body = std::move(body);
        return cf;
    }

    // ------------------------------------------------------------------
    // Statements
    // ------------------------------------------------------------------

    std::vector<CStmt> check_stmts(const std::vector<ast::Stmt>& stmts) {
        std::vector<CStmt> out;
        out.reserve(stmts.size());
        for (const auto& s : stmts) out.push_back(check_stmt(s));
        return out;
    }

    /// True if the statement's expressions call a function whose (inferred)
    /// return type is not yet known. Used to defer such top-level statements
    /// until all function bodies have been checked.
    bool stmt_calls_unresolved(const ast::Stmt& s) const {
        if (auto* let = std::get_if<ast::SLet>(&s.v)) return expr_calls_unresolved(*let->init);
        return false;
    }

    bool expr_calls_unresolved(const ast::Expr& e) const {
        if (auto* x = std::get_if<ast::ECall>(&e.v)) {
            return call_unresolved(*x->callee, x->args);
        }
        if (auto* x = std::get_if<ast::ECallGen>(&e.v)) {
            return call_unresolved(*x->callee, x->args);
        }
        if (auto* x = std::get_if<ast::EAwait>(&e.v)) return expr_calls_unresolved(*x->operand);
        if (auto* x = std::get_if<ast::ENewPromise>(&e.v)) return expr_calls_unresolved(*x->executor);
        if (auto* x = std::get_if<ast::EUnary>(&e.v)) return expr_calls_unresolved(*x->operand);
        if (auto* x = std::get_if<ast::EMember>(&e.v)) return expr_calls_unresolved(*x->obj);
        if (auto* x = std::get_if<ast::EBinary>(&e.v)) {
            return expr_calls_unresolved(*x->lhs) || expr_calls_unresolved(*x->rhs);
        }
        if (auto* x = std::get_if<ast::EIndex>(&e.v)) {
            return expr_calls_unresolved(*x->obj) || expr_calls_unresolved(*x->index);
        }
        if (auto* x = std::get_if<ast::EArrayLit>(&e.v)) {
            for (const auto& el : x->elems)
                if (expr_calls_unresolved(*el)) return true;
            return false;
        }
        if (auto* x = std::get_if<ast::EObjectLit>(&e.v)) {
            for (const auto& f : x->fields)
                if (expr_calls_unresolved(*f.value)) return true;
            return false;
        }
        if (auto* x = std::get_if<ast::ENewCtor>(&e.v)) {
            for (const auto& a : x->args)
                if (expr_calls_unresolved(*a)) return true;
            return false;
        }
        if (auto* x = std::get_if<ast::ETemplate>(&e.v)) return parts_unresolved(x->parts);
        if (auto* x = std::get_if<ast::ETagged>(&e.v)) return parts_unresolved(x->parts);
        // lambda bodies are checked later on their own
        return false;
    }

    private:
    bool call_unresolved(const ast::Expr& callee, const std::vector<ast::ExprPtr>& args) const {
        bool direct = false;
        if (auto* id = std::get_if<ast::EIdent>(&callee.v)) {
            auto it = sigs.find(id->name);
            if (it != sigs.end()) direct = !it->second.ret.has_value() && it->second.is_async;
        }
        if (direct) return true;
        if (expr_calls_unresolved(callee)) return true;
        for (const auto& a : args)
            if (expr_calls_unresolved(*a)) return true;
        return false;
    }

    bool parts_unresolved(const std::vector<ast::TPart>& parts) const {
        for (const auto& p : parts)
            if (auto* te = std::get_if<ast::TExpr>(&p))
                if (expr_calls_unresolved(*te->expr)) return true;
        return false;
    }

    public:
    CStmt check_stmt(const ast::Stmt& s) {
        // let -----------------------------------------------------------------
        if (auto* x = std::get_if<ast::SLet>(&s.v)) {
            const std::string& name = x->name;
            const std::optional<ast::TyAnn>& ann = x->ann;
            const ast::ExprPtr& init = x->init;
            bool is_const = x->is_const;
            Span span = x->span;
            // naming rules: `let` is camelCase, `const` is UPPER_SNAKE_CASE
            if (is_const) {
                if (!is_upper_snake_case(name)) {
                    diag("const '" + name + "' must be UPPER_SNAKE_CASE (e.g. MAX_RETRIES)", span);
                }
            } else if (!is_camel_case(name)) {
                diag("let variable '" + name + "' must be camelCase", span);
            }
            // an annotated array literal takes its element type from the annotation:
            // `let a: Array<Int32> = []` or `let xs: Array<Named> = [p, q]`
            std::optional<CTy> expected_elem;
            if (ann && std::holds_alternative<ast::EArrayLit>(init->v)) {
                CTy a = ty_of(*ann);
                if (a.tag == CTy::Tag::Array) expected_elem = deref(a.inner);
            }
            // an object literal bound to a variable names its ghost
            // class after the variable: `let abc = {...}` → Class_abc_NNNN.
            // With a class annotation the literal must match that class.
            std::optional<size_t> target_class;
            if (ann && std::holds_alternative<ast::EObjectLit>(init->v)) {
                CTy t = ty_of(*ann);
                if (t.tag == CTy::Tag::Object) {
                    target_class = t.object.id;
                } else {
                    diag("type annotation must be a class for object literals, found " + t.name(),
                         span);
                }
            }
            CExpr init_c;
            CTy init_ty;
            if (auto* aw = std::get_if<ast::EAwait>(&init->v)) {
                auto r = check_await_top(*aw->operand, init->span);
                init_c = std::move(r.first);
                init_ty = std::move(r.second);
            } else if (auto* al = std::get_if<ast::EArrayLit>(&init->v)) {
                init_c = check_array_lit(al->elems, init->span, expected_elem);
                init_ty = expr_ty(init_c);
            } else if (auto* ol = std::get_if<ast::EObjectLit>(&init->v)) {
                init_c = check_object_lit(ol->fields, init->span, target_class, &name);
                init_ty = expr_ty(init_c);
            } else {
                init_c = check_expr(*init, false);
                init_ty = expr_ty(init_c);
            }
            CTy ty = init_ty;
            if (ann) {
                CTy at = ty_of(*ann);
                adapt_literal(init_c, at);
                init_ty = expr_ty(init_c);
                coerce(init_ty, at, span);
                ty = at;
            }
            size_t id = declare(name, ty, is_const, span);
            if (in_toplevel) {
                // top-level variables are globals: lambdas anywhere may
                // read them, and assign back to them
                toplevel_var_ids.insert(id);
            }
            if (auto* hv = std::get_if<CHttpValidatorNew>(&init_c.v)) {
                validator_fields[id] = hv->fields;
            }
            CLet l;
            l.id = id;
            l.ty = ty;
            l.init = exptr(std::move(init_c));
            return cst(std::move(l));
        }
        // assign ---------------------------------------------------------------
        if (auto* x = std::get_if<ast::SAssign>(&s.v)) {
            Span span = x->span;
            auto found = lookup(x->name, span);
            if (!found) return fallback_stmt();
            auto [id, ty, is_capture] = *found;
            if (is_capture && !toplevel_var_ids.count(id)) {
                diag("cannot assign to a captured variable (captures are by value)", span);
                return fallback_stmt();
            }
            bool is_const = false;
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                auto e = it->find(x->name);
                if (e != it->end()) {
                    is_const = e->second.is_const;
                    break;
                }
            }
            if (is_const) diag("cannot assign to 'const " + x->name + "'", span);
            CExpr v = check_expr(*x->value, false);
            adapt_literal(v, ty);
            coerce(expr_ty(v), ty, span);
            CAssign a;
            a.id = id;
            a.value = exptr(std::move(v));
            return cst(std::move(a));
        }
        // index assign ----------------------------------------------------------
        if (auto* x = std::get_if<ast::SIndexAssign>(&s.v)) {
            Span span = x->span;
            auto found = lookup(x->name, span);
            if (!found) return fallback_stmt();
            auto [id, ty, is_capture] = *found;
            if (is_capture && !toplevel_var_ids.count(id)) {
                diag("cannot assign to a captured variable (captures are by value)", span);
                return fallback_stmt();
            }
            // NOTE: `const` only freezes the binding (reassignment via
            // `arr = ...` is rejected in the Assign arm). Writing elements
            // (`arr[i] = v`, `arr.push(...)`) mutates the array object the
            // binding refers to, matching TypeScript semantics.
            if (ty.tag == CTy::Tag::Array) {
                CTy elem = deref(ty.inner);
                CExpr ic = check_expr(*x->index, false);
                if (!is_i32(expr_ty(ic))) {
                    diag("array index must be Int32, found " + expr_ty(ic).name(), span);
                }
                CExpr vc = check_expr(*x->value, false);
                adapt_literal(vc, elem);
                coerce(expr_ty(vc), elem, span);
                CIndexSet is;
                is.id = id;
                is.idx = exptr(std::move(ic));
                is.value = exptr(std::move(vc));
                is.elem = elem;
                return cst(std::move(is));
            }
            diag("indexed assignment requires an Array, found " + ty.name(), span);
            return fallback_stmt();
        }
        // member assign ---------------------------------------------------------
        if (auto* x = std::get_if<ast::SMemberAssign>(&s.v)) {
            Span span = x->span;
            const std::string& obj = x->obj;
            const std::string& field = x->field;
            // static field on a class name? (`Name.count = v`) — checked
            // before variable lookup, which would fail for class names
            if (!contains_str(uses, obj) && class_by_name.count(obj)) {
                return check_static_member_assign(class_by_name[obj], field, x->value, span);
            }
            auto found = lookup(obj, span);
            if (!found) return fallback_stmt();
            auto [id, ty, is_capture] = *found;
            if (is_capture && !toplevel_var_ids.count(id)) {
                diag("cannot assign to a captured variable (captures are by value)", span);
                return fallback_stmt();
            }
            // const freezes the binding, not the object contents
            if (ty.tag == CTy::Tag::Interface) {
                const std::string& iname = ty.iface;
                auto it = interfaces.find(iname);
                std::vector<std::pair<std::string, CTy>> fields =
                    it != interfaces.end() ? it->second : std::vector<std::pair<std::string, CTy>>{};
                const CTy* elem = nullptr;
                for (const auto& [n, t] : fields)
                    if (n == field) {
                        elem = &t;
                        break;
                    }
                if (!elem) {
                    if (iface_has_method(iname, field)) {
                        diag("interface '" + iname + "': '" + field + "' is a method (call it)",
                             span);
                        return fallback_stmt();
                    }
                    std::vector<std::string> names;
                    names.reserve(fields.size());
                    for (const auto& [n, _] : fields) names.push_back(n);
                    diag("interface '" + iname + "' has no field '" + field + "' (fields: " +
                             (names.empty() ? std::string("none") : join(names, ", ")) + ")",
                         span);
                    return fallback_stmt();
                }
                CTy elem_c = *elem;
                CExpr vc = check_expr(*x->value, false);
                adapt_literal(vc, elem_c);
                coerce(expr_ty(vc), elem_c, span);
                CIfaceSet is;
                is.id = id;
                is.field = field;
                is.value = exptr(std::move(vc));
                is.elem = std::move(elem_c);
                return cst(std::move(is));
            }
            if (ty.tag == CTy::Tag::Object) {
                if (ty.object.id >= classes.size()) return fallback_stmt();
                const ClassDef& def = classes[ty.object.id];
                size_t idx = def.fields.size();
                for (size_t i = 0; i < def.fields.size(); ++i)
                    if (def.fields[i].first == field) {
                        idx = i;
                        break;
                    }
                if (idx == def.fields.size()) {
                    // static field on a class name? (`Name.count = v`)
                    if (!contains_str(uses, obj) && class_by_name.count(obj)) {
                        return check_static_member_assign(class_by_name[obj], field,
                                                          x->value, span);
                    }
                    // setter? (`obj.prop = v` with `set prop(v)`)
                    if (auto st = find_member(ty.object.id, field, "set_")) {
                        auto sit = sigs.find(st->second);
                        CTy elem = sit->second.params.size() > 1 ? sit->second.params[1]
                                                                 : cty_void();
                        CExpr vc = check_expr(*x->value, false);
                        adapt_literal(vc, elem);
                        coerce(expr_ty(vc), elem, span);
                        bool virtual_set = classes[ty.object.id].is_abstract ||
                                           subtree_overrides(ty.object.id, field, "set_");
                        if (virtual_set) {
                            CIfaceCall call;
                            CExpr oc = ce(CIdent{id, ty});
                            call.obj = exptr(std::move(oc));
                            call.iface = classes[ty.object.id].name;
                            call.method = "set_" + field;
                            call.args.push_back(std::move(vc));
                            call.ret = cty_void();
                            CExprStmt es;
                            es.expr = exptr(ce(std::move(call)));
                            return cst(std::move(es));
                        }
                        CCallFn f;
                        f.name = st->second;
                        CExpr oc = ce(CIdent{id, ty});
                        f.args.push_back(std::move(oc));
                        f.args.push_back(std::move(vc));
                        f.ret = cty_void();
                        f.is_async = false;
                        CExprStmt es;
                        es.expr = exptr(ce(std::move(f)));
                        return cst(std::move(es));
                    }
                    std::vector<std::string> names;
                    names.reserve(def.fields.size());
                    for (const auto& [n, _] : def.fields) names.push_back(n);
                    diag("class '" + ty.object.name + "' has no field '" + field + "' (fields: " +
                             (names.empty() ? std::string("none") : join(names, ", ")) + ")",
                         span);
                    return fallback_stmt();
                }
                auto own = field_owner(ty.object.id, field);
                const ClassDef* odef =
                    own.has_value() ? &classes[*own] : nullptr;
                bool priv = odef && odef->field_is_private(field);
                bool prot = odef && std::find(odef->protected_fields.begin(),
                                              odef->protected_fields.end(),
                                              field) != odef->protected_fields.end();
                if (odef && !can_access_member(*own, priv, prot)) {
                    diag(std::string("field '" + field + "' of class '" + odef->name +
                                     "' is " + (priv ? "private" : "protected") +
                                     " (assign it inside the class or a subclass)"),
                         span);
                    return fallback_stmt();
                }
                CTy elem = def.fields[idx].second;
                CExpr vc = check_expr(*x->value, false);
                adapt_literal(vc, elem);
                coerce(expr_ty(vc), elem, span);
                CFieldSet fs;
                fs.id = id;
                fs.idx = idx;
                fs.value = exptr(std::move(vc));
                fs.elem = std::move(elem);
                return cst(std::move(fs));
            }
            diag("field assignment requires an object, found " + ty.name() +
                     " (hint: use `" + obj + "[i] = v` for arrays)",
                 span);
            return fallback_stmt();
        }
        // return -----------------------------------------------------------------
        if (auto* x = std::get_if<ast::SReturn>(&s.v)) {
            Span span = x->span;
            bool ret_ok_here = lambda_ret_ok.empty() ? false : lambda_ret_ok.back();
            if (!lambda_idx.empty() && !ret_ok_here) {
                diag("cannot return from a lambda", span);
                return fallback_stmt();
            }
            if (lambda_idx.empty() && in_toplevel) {
                diag("'return' is not allowed at the top level", span);
                return fallback_stmt();
            }
            if (!x->value) {
                unify_ret(cty_void(), span);
                return ret_stmt(std::nullopt, ReturnKindKind::Plain);
            }
            const ast::Expr& val = **x->value;
            if (auto* aw = std::get_if<ast::EAwait>(&val.v)) {
                auto r = check_await_top(*aw->operand, val.span);
                unify_ret(r.second, span);
                return ret_stmt(std::move(r.first), ReturnKindKind::Plain);
            }
            CExpr v = check_expr(val, false);
            if (ret_literal_target) adapt_literal(v, *ret_literal_target);
            CTy t = expr_ty(v);
            if (is_async_ctx) {
                if (t.tag == CTy::Tag::Promise) {
                    if (block_depth > 0) {
                        diag("returning a promise from inside if/while/try is not supported in marina v0.1",
                             span);
                        return ret_stmt(std::nullopt, ReturnKindKind::Plain);
                    }
                    // flatten one level (JS async semantics)
                    if (t.inner && t.inner->tag == CTy::Tag::Promise) {
                        diag("returning a promise of a promise is not supported", span);
                    }
                    t = deref(t.inner);
                    unify_ret(t, span);
                    return ret_stmt(std::move(v), ReturnKindKind::FlattenPromise, t);
                }
            } else if (t.tag == CTy::Tag::Promise) {
                diag("sync functions cannot return a Promise (make the function async)", span);
            }
            unify_ret(t, span);
            return ret_stmt(std::move(v), ReturnKindKind::Plain);
        }
        // if ---------------------------------------------------------------------
        if (auto* x = std::get_if<ast::SIf>(&s.v)) {
            Span span = x->span;
            CExpr c = check_expr(*x->cond, false);
            if (expr_ty(c).tag != CTy::Tag::Bool) {
                diag("if condition must be bool, found " + expr_ty(c).name(), span);
            }
            scopes.emplace_back();
            block_depth += 1;
            auto t = check_stmts(x->then_body);
            block_depth -= 1;
            scopes.pop_back();
            std::optional<std::vector<CStmt>> e;
            if (x->else_body) {
                scopes.emplace_back();
                block_depth += 1;
                e = check_stmts(*x->else_body);
                block_depth -= 1;
                scopes.pop_back();
            }
            CIf ci;
            ci.cond = exptr(std::move(c));
            ci.then_body = std::move(t);
            ci.else_body = std::move(e);
            return cst(std::move(ci));
        }
        // while --------------------------------------------------------------------
        if (auto* x = std::get_if<ast::SWhile>(&s.v)) {
            Span span = x->span;
            CExpr c = check_expr(*x->cond, false);
            if (expr_ty(c).tag != CTy::Tag::Bool) {
                diag("while condition must be bool, found " + expr_ty(c).name(), span);
            }
            scopes.emplace_back();
            block_depth += 1;
            auto b = check_stmts(x->body);
            block_depth -= 1;
            scopes.pop_back();
            CWhile cw;
            cw.cond = exptr(std::move(c));
            cw.body = std::move(b);
            return cst(std::move(cw));
        }
        // for -----------------------------------------------------------------------
        if (auto* x = std::get_if<ast::SFor>(&s.v)) {
            Span span = x->span;
            // the init variable lives in a scope that encloses the body
            scopes.emplace_back();
            std::optional<std::shared_ptr<CStmt>> ci;
            if (x->init) ci = std::make_shared<CStmt>(check_stmt(**x->init));
            CExpr cc = cbool(true);
            if (x->cond) {
                if (auto* aw = std::get_if<ast::EAwait>(&(*x->cond)->v)) {
                    (void)aw->operand;
                    diag("await in a loop condition is not supported in marina v0.1", (*x->cond)->span);
                    cc = cbool(false);
                } else {
                    cc = check_expr(**x->cond, false);
                    if (expr_ty(cc).tag != CTy::Tag::Bool) {
                        diag("for condition must be bool, found " + expr_ty(cc).name(), span);
                    }
                }
            }
            std::optional<std::shared_ptr<CStmt>> cu;
            if (x->update) {
                const ast::Stmt& up = **x->update;
                bool is_await_expr = false;
                Span asp{};
                if (auto* se = std::get_if<ast::SExpr>(&up.v)) {
                    if (std::holds_alternative<ast::EAwait>(se->expr->v)) {
                        is_await_expr = true;
                        asp = se->expr->span;
                    }
                }
                if (is_await_expr) {
                    diag("await in a loop update is not supported in marina v0.1", asp);
                } else {
                    cu = std::make_shared<CStmt>(check_stmt(up));
                }
            }
            block_depth += 1;
            auto cb = check_stmts(x->body);
            block_depth -= 1;
            scopes.pop_back();
            CFor cf;
            cf.init = std::move(ci);
            cf.cond = exptr(std::move(cc));
            cf.update = std::move(cu);
            cf.body = std::move(cb);
            return cst(std::move(cf));
        }
        // throw -------------------------------------------------------------------
        if (auto* x = std::get_if<ast::SThrow>(&s.v)) {
            Span span = x->span;
            CExpr v = check_expr(*x->value, false);
            CTy t = expr_ty(v);
            if (t.tag != CTy::Tag::Str) {
                diag("throw requires a string value, found " + t.name() +
                         " (convert it first, e.g. with a template literal)",
                     span);
            }
            CThrow th;
            th.value = exptr(std::move(v));
            return cst(std::move(th));
        }
        // try / catch -------------------------------------------------------------
        if (auto* x = std::get_if<ast::STryCatch>(&s.v)) {
            Span span = x->span;
            scopes.emplace_back();
            block_depth += 1;
            auto t = check_stmts(x->try_body);
            block_depth -= 1;
            scopes.pop_back();
            std::optional<std::pair<size_t, CTy>> cvar;
            scopes.emplace_back();
            block_depth += 1;
            if (x->catch_name.has_value()) {
                if (!is_camel_case(*x->catch_name)) {
                    diag("catch variable '" + *x->catch_name + "' must be camelCase", span);
                }
                size_t id = declare(*x->catch_name, cty_str(), false, span);
                cvar = {id, cty_str()};
            }
            auto c = check_stmts(x->catch_body);
            block_depth -= 1;
            scopes.pop_back();
            CTry ct;
            ct.try_body = std::move(t);
            ct.catch_var = std::move(cvar);
            ct.catch_body = std::move(c);
            return cst(std::move(ct));
        }
        // expr --------------------------------------------------------------------------
        if (auto* x = std::get_if<ast::SExpr>(&s.v)) {
            if (auto* aw = std::get_if<ast::EAwait>(&x->expr->v)) {
                auto r = check_await_top(*aw->operand, x->expr->span);
                return expr_stmt(std::move(r.first));
            }
            return expr_stmt(check_expr(*x->expr, false));
        }
        return fallback_stmt();
    }

    std::pair<CExpr, CTy> check_await_top(const ast::Expr& inner, Span span) {
        if (block_depth > 0) {
            diag("await inside if/while/try is not supported in marina v0.1 (keep awaits at the top "
                 "level of the async function)",
                 span);
            return {cbool(false), cty_void()};
        }
        CExpr e = check_expr(inner, false);
        CTy t = expr_ty(e);
        if (t.tag == CTy::Tag::Promise) {
            CTy u = deref(t.inner);
            CAwait a;
            a.operand = exptr(std::move(e));
            a.ty = u;
            return {ce(std::move(a)), u};
        }
        diag("can only await a Promise, found " + t.name(), span);
        CAwait a;
        a.operand = exptr(std::move(e));
        a.ty = cty_void();
        return {ce(std::move(a)), cty_void()};
    }

    void unify_ret(const CTy& t, Span span) {
        if (ret_ty) {
            CTy prev = *ret_ty;
            if (prev == t) return;
            if (prev.tag == CTy::Tag::Int && t.tag == CTy::Tag::Float) {
                note_float_ret(t);
                return;
            }
            if (prev.tag == CTy::Tag::Float && t.tag == CTy::Tag::Int) {
                note_float_ret(prev);
                return; // int widens to float
            }
            if (prev.tag == CTy::Tag::Float && t.tag == CTy::Tag::Float && prev.bits != t.bits) {
                ret_ty = cty_float(64);
                note_float_ret(*ret_ty);
                return;
            }
            // polymorphic returns: a subclass widens to its base (both
            // branches may return different subclasses of a common base)
            if (prev.tag == CTy::Tag::Object && t.tag == CTy::Tag::Object &&
                prev.object.id < classes.size() && t.object.id < classes.size()) {
                if (is_subclass(t.object.id, prev.object.id)) return; // keep base
                if (is_subclass(prev.object.id, t.object.id)) {
                    ret_ty = t;
                    return;
                }
            }
            diag("inconsistent return types: expected " + prev.name() + ", found " + t.name(), span);
        } else {
            ret_ty = t;
        }
    }

    // ------------------------------------------------------------------
    // Expressions
    // ------------------------------------------------------------------

    CTy object_ty_of_class(size_t class_id) const {
        std::string name = class_id < classes.size() ? classes[class_id].name : std::string();
        return cty_object(class_id, std::move(name));
    }

    CTy object_ty_by_name(const std::string& n) const {
        auto it = class_by_name.find(n);
        return cty_object(it == class_by_name.end() ? SIZE_MAX : it->second, n);
    }

    CTy expr_ty(const CExpr& e) const {
        if (auto* x = std::get_if<CInt>(&e.v)) return cty_int(x->bits, x->unsign);
        if (auto* x = std::get_if<CFloat>(&e.v)) return cty_float(x->bits);
        if (std::holds_alternative<CBool>(e.v)) return cty_bool();
        if (std::holds_alternative<CStr>(e.v)) return cty_str();
        if (std::holds_alternative<CTemplate>(e.v)) return cty_str();
        if (auto* x = std::get_if<CIdent>(&e.v)) return x->ty;
        if (auto* x = std::get_if<CUnary>(&e.v)) return x->ty;
        if (auto* x = std::get_if<CBinary>(&e.v)) return x->ty;
        if (auto* x = std::get_if<CCallFn>(&e.v)) return x->ret;
        if (std::holds_alternative<CCallResolver>(e.v)) return cty_void();
        if (auto* x = std::get_if<CAwait>(&e.v)) return x->ty;
        if (auto* x = std::get_if<CNewPromise>(&e.v)) {
            // Promise<T> where T is the executor's resolve type
            size_t li = x->lambda;
            if (li < lambdas.size() && !lambdas[li].params.empty()) {
                const CTy& pt = lambdas[li].params[0].second;
                if (pt.tag == CTy::Tag::Resolver) return cty_promise(deref(pt.inner));
            }
            return cty_void();
        }
        if (std::holds_alternative<CConsoleLog>(e.v)) return cty_void();
        if (std::holds_alternative<CSetTimeout>(e.v)) return cty_void();
        if (auto* x = std::get_if<CArrayLit>(&e.v)) return cty_array(x->elem_ty);
        if (auto* x = std::get_if<CIndex>(&e.v)) return x->ty;
        if (auto* x = std::get_if<CArrayOp>(&e.v)) return x->ret;
        if (auto* x = std::get_if<CArrayHof>(&e.v)) return x->ret;
        if (auto* x = std::get_if<CLambdaVal>(&e.v)) return x->ret;
        if (std::holds_alternative<CStrLen>(e.v)) return cty_int();
        if (auto* x = std::get_if<CStrMethod>(&e.v)) return x->ret;
        if (std::holds_alternative<CFsArgs>(e.v)) return cty_array(cty_str());
        if (auto* x = std::get_if<CObjectLit>(&e.v)) return object_ty_of_class(x->class_id);
        if (auto* x = std::get_if<CNewClass>(&e.v)) return object_ty_of_class(x->class_id);
        if (auto* x = std::get_if<CNewClassCtor>(&e.v)) return object_ty_of_class(x->class_id);
        if (auto* x = std::get_if<CFieldGet>(&e.v)) return x->ty;
        if (auto* x = std::get_if<CStaticRef>(&e.v)) return x->ty;
        if (std::holds_alternative<CNetConnect>(e.v)) return cty_promise(cty_stream());
        if (std::holds_alternative<CNetWrite>(e.v) || std::holds_alternative<CNetClose>(e.v))
            return cty_promise(cty_void());
        if (std::holds_alternative<CNetReadAll>(e.v)) return cty_promise(cty_str());
        if (std::holds_alternative<CHttpCall>(e.v)) return cty_promise(object_ty_by_name("http_Response"));
        if (std::holds_alternative<CHttpListen>(e.v)) return cty_void();
        if (std::holds_alternative<CResSend>(e.v)) return cty_void();
        if (std::holds_alternative<CHttpNewServer>(e.v)) return object_ty_by_name("http_Server");
        if (std::holds_alternative<CHttpRouter>(e.v)) return object_ty_by_name("http_Router");
        if (std::holds_alternative<CHttpRouteAdd>(e.v)) return cty_void();
        if (std::holds_alternative<CHttpServerListen>(e.v)) return cty_promise(cty_void());
        if (std::holds_alternative<CHttpValidatorNew>(e.v)) return object_ty_by_name("http_JsonValidator");
        if (std::holds_alternative<CHttpFieldNew>(e.v) || std::holds_alternative<CHttpFieldOp>(e.v))
            return object_ty_by_name("http_FieldValidator");
        if (auto* x = std::get_if<CHttpValidate>(&e.v)) return object_ty_of_class(x->out_class);
        if (auto* x = std::get_if<CIfaceGet>(&e.v)) return x->ty;
        if (auto* x = std::get_if<CIfaceCall>(&e.v)) return x->ret;
        if (std::holds_alternative<CIfaceSetExpr>(e.v)) return cty_void();
        if (std::holds_alternative<CFsReadFile>(e.v)) return cty_promise(cty_str());
        if (std::holds_alternative<CFsWriteFile>(e.v) || std::holds_alternative<CFsMkdir>(e.v) ||
            std::holds_alternative<CFsRemove>(e.v))
            return cty_promise(cty_void());
        if (std::holds_alternative<CFsExists>(e.v)) return cty_promise(cty_bool());
        if (std::holds_alternative<CFsReadDir>(e.v)) return cty_promise(cty_array(cty_str()));
        if (std::holds_alternative<CCryptoSha256>(e.v) || std::holds_alternative<CCryptoBase64>(e.v) ||
            std::holds_alternative<CCryptoRandomHex>(e.v) || std::holds_alternative<CCryptoUuid>(e.v))
            return cty_str();
        if (std::holds_alternative<CSqlOpen>(e.v)) return object_ty_by_name("sql_Connection");
        if (std::holds_alternative<CSqlExec>(e.v)) return cty_promise(object_ty_by_name("sql_Result"));
        if (std::holds_alternative<CSqlQuery>(e.v))
            return cty_promise(cty_array(cty_interface("sql_Row")));
        if (std::holds_alternative<CSqlClose>(e.v)) return cty_void();
        if (std::holds_alternative<CStringFromCharCode>(e.v)) return cty_str();
        if (auto* x = std::get_if<CTimeCall>(&e.v)) {
            switch (x->kind) {
                case TimeKind::Now:
                case TimeKind::NowSecs:
                case TimeKind::NowNanos:
                case TimeKind::Mono: return cty_int(64, false);
                case TimeKind::Format: return cty_str();
                case TimeKind::Sleep: return cty_void();
            }
            return cty_void();
        }
        if (auto* x = std::get_if<COsCall>(&e.v)) {
            switch (x->kind) {
                case OsKind::Platform:
                case OsKind::Arch:
                case OsKind::Hostname:
                case OsKind::Cwd:
                case OsKind::Tmpdir:
                case OsKind::EnvGet:
                case OsKind::EnvGetOr:
                case OsKind::Exec: return cty_str();
                case OsKind::Pid: return cty_int(64, false);
                case OsKind::EnvSet:
                case OsKind::EnvUnset:
                case OsKind::Exit: return cty_void();
            }
            return cty_void();
        }
        return cty_void();
    }

    CExpr check_expr(const ast::Expr& e, bool allow_await_top) {
        if (auto* x = std::get_if<ast::EInt>(&e.v)) {
            CTy dt = default_int_ty(x->value);
            CInt n;
            n.value = x->value;
            n.bits = dt.bits;
            n.unsign = dt.unsign;
            return ce(std::move(n));
        }
        if (auto* x = std::get_if<ast::EFloat>(&e.v)) return ce(CFloat{x->value, 64});
        if (auto* x = std::get_if<ast::EBool>(&e.v)) return ce(CBool{x->value});
        if (auto* x = std::get_if<ast::EStr>(&e.v)) return ce(CStr{x->value});
        if (auto* x = std::get_if<ast::ETemplate>(&e.v)) {
            std::vector<TPartC> out;
            for (const auto& p : x->parts) {
                if (auto* s = std::get_if<ast::TStr>(&p)) {
                    TPartC pc;
                    pc.v = s->value;
                    out.push_back(std::move(pc));
                } else if (auto* te = std::get_if<ast::TExpr>(&p)) {
                    CExpr c = check_expr(*te->expr, false);
                    if (expr_ty(c).tag == CTy::Tag::Void) {
                        diag("cannot interpolate a void expression", te->expr->span);
                    }
                    TPartC pc;
                    pc.v = exptr(std::move(c));
                    out.push_back(std::move(pc));
                }
            }
            if (out.empty()) diag("empty template literal", e.span);
            CTemplate t;
            t.parts = std::move(out);
            return ce(std::move(t));
        }
        if (std::holds_alternative<ast::ETagged>(e.v)) {
            // sql.stmt templates are only consumed by db.execute/db.query
            diag("template tags must be passed directly to db.execute or db.query (e.g. "
                 "db.query(sql.stmt`SELECT 1`))",
                 e.span);
            return cbool(false);
        }
        if (auto* x = std::get_if<ast::EIdent>(&e.v)) {
            auto found = lookup(x->name, e.span);
            if (found) {
                auto [id, ty, _] = *found;
                (void)_;
                return ce(CIdent{id, std::move(ty)});
            }
            return cbool(false);
        }
        if (auto* x = std::get_if<ast::EUnary>(&e.v)) {
            CExpr c = check_expr(*x->operand, false);
            CTy t = expr_ty(c);
            if (x->op == ast::UnOp::Neg) {
                if (!is_numeric(t)) {
                    diag("unary '-' requires a number, found " + t.name(), e.span);
                }
                if (auto* lit = std::get_if<CInt>(&c.v)) {
                    if (lit->value == 0) return c;
                    if (lit->neg || lit->value > (1ULL << 63)) {
                        diag("integer literal does not fit in Int64", e.span);
                        return c;
                    }
                    CInt n = *lit;
                    n.neg = true;
                    n.unsign = false;
                    if (lit_fits(n.value, true, 32, false)) n.bits = 32;
                    else n.bits = 64;
                    return ce(std::move(n));
                }
                if (auto* fl = std::get_if<CFloat>(&c.v)) {
                    fl->value = -fl->value;
                    return c;
                }
                if (t.tag == CTy::Tag::Int && t.unsign) {
                    diag("unary '-' is not allowed on " + t.name(), e.span);
                }
                CUnary u;
                u.op = UnOpC::Neg;
                u.operand = exptr(std::move(c));
                u.ty = t;
                return ce(std::move(u));
            }
            if (t.tag != CTy::Tag::Bool) {
                diag("'!' requires a bool, found " + t.name(), e.span);
            }
            CUnary u;
            u.op = UnOpC::Not;
            u.operand = exptr(std::move(c));
            u.ty = cty_bool();
            return ce(std::move(u));
        }
        if (auto* x = std::get_if<ast::EBinary>(&e.v)) {
            return check_binary(x->op, *x->lhs, *x->rhs, e.span);
        }
        if (auto* x = std::get_if<ast::EAwait>(&e.v)) {
            if (!allow_await_top) {
                diag("await is only allowed as `let x = await ...`, a standalone `await ...` "
                     "statement, or `return await ...`",
                     e.span);
                return cbool(false);
            }
            CExpr c = check_expr(*x->operand, false);
            CTy t = expr_ty(c);
            if (t.tag == CTy::Tag::Promise) {
                CAwait a;
                a.operand = exptr(std::move(c));
                a.ty = deref(t.inner);
                return ce(std::move(a));
            }
            diag("can only await a Promise, found " + t.name(), e.span);
            CAwait a;
            a.operand = exptr(std::move(c));
            a.ty = cty_void();
            return ce(std::move(a));
        }
        if (auto* x = std::get_if<ast::ECall>(&e.v)) {
            return check_call(*x->callee, x->args, e.span);
        }
        if (auto* x = std::get_if<ast::ECallGen>(&e.v)) {
            return check_call_gen(*x->callee, x->type_args, x->args, e.span);
        }
        if (auto* x = std::get_if<ast::EArrayLit>(&e.v)) {
            return check_array_lit(x->elems, e.span, std::nullopt);
        }
        if (auto* x = std::get_if<ast::EObjectLit>(&e.v)) {
            return check_object_lit(x->fields, e.span, std::nullopt, nullptr);
        }
        if (auto* x = std::get_if<ast::ENewClass>(&e.v)) {
            if (generic_classes.count(x->name)) {
                if (x->type_args.empty()) {
                    diag("generic class '" + x->name + "' needs type parameters, e.g. new " + x->name +
                             "<Int32>()",
                         e.span);
                    return cbool(false);
                }
                CTy t = mono_class(x->name, x->type_args, e.span);
                if (t.tag != CTy::Tag::Object) return cbool(false);
                // `new Map<V>()` still runs a zero-argument constructor (own
                // or inherited) like the non-generic path does
                for (size_t c = t.object.id;;) {
                    std::string cand = classes[c].name + "__new";
                    auto sit = sigs.find(cand);
                    if (sit != sigs.end() && sit->second.params.size() == 1) {
                        CNewClassCtor nc;
                        nc.class_id = t.object.id;
                        nc.fn = cand;
                        return ce(std::move(nc));
                    }
                    if (sit != sigs.end()) {
                        diag("constructor of class '" + x->name +
                                 "' takes " + numstr(sit->second.params.size() - 1) +
                                 " argument(s) (generic classes do not support constructor "
                                 "arguments yet)",
                             e.span);
                        return cbool(false);
                    }
                    if (!classes[c].base.has_value()) break;
                    c = *classes[c].base;
                }
                return ce(CNewClass{t.object.id});
            }
            if (!x->type_args.empty()) {
                diag("class '" + x->name + "' does not take type parameters", e.span);
            }
            auto it = class_by_name.find(x->name);
            if (it != class_by_name.end()) {
                if (classes[it->second].is_abstract) {
                    diag("cannot instantiate abstract class '" + x->name + "'", e.span);
                    return cbool(false);
                }
                // `new C()` still runs a zero-argument constructor (own or
                // inherited) like TypeScript does
                for (size_t c = it->second;;) {
                    std::string cand = classes[c].name + "__new";
                    auto sit = sigs.find(cand);
                    if (sit != sigs.end() && sit->second.params.size() == 1) {
                        CNewClassCtor nc;
                        nc.class_id = it->second;
                        nc.fn = cand;
                        return ce(std::move(nc));
                    }
                    if (sit != sigs.end()) {
                        diag("constructor of class '" + x->name + "' takes " +
                                 numstr(sit->second.params.size() - 1) +
                                 " argument(s), got 0",
                             e.span);
                        return cbool(false);
                    }
                    if (!classes[c].base.has_value()) break;
                    c = *classes[c].base;
                }
                return ce(CNewClass{it->second});
            }
            diag("unknown class '" + x->name + "'", e.span);
            return cbool(false);
        }
        if (auto* x = std::get_if<ast::ENewCtor>(&e.v)) {
            const std::vector<std::string>& path = x->path;
            if (path.size() == 2 && path[0] == "sql" && path[1] == "Connection") {
                if (x->args.size() != 2) {
                    diag("new sql.Connection takes (driver, url): e.g. new sql.Connection(\"sqlite\", "
                         "\"/tmp/data.db\")",
                         e.span);
                    return cbool(false);
                }
                CExpr driver = check_expr(*x->args[0], false);
                coerce(expr_ty(driver), cty_str(), x->args[0]->span);
                // catch an unknown driver early when it is a literal
                if (auto* ds = std::get_if<ast::EStr>(&x->args[0]->v)) {
                    const std::string& d = ds->value;
                    if (!(d == "sqlite" || d == "mysql" || d == "pg" || d == "mariadb")) {
                        diag("unknown sql driver '" + d +
                                 "' (supported: sqlite, mysql, pg, mariadb)",
                             x->args[0]->span);
                    }
                    if (d != "sqlite") {
                        diag("sql driver '" + d +
                                 "' is not supported yet (only 'sqlite' is supported in v0.2)",
                             x->args[0]->span);
                    }
                }
                CExpr url = check_expr(*x->args[1], false);
                coerce(expr_ty(url), cty_str(), x->args[1]->span);
                CSqlOpen so;
                so.driver = exptr(std::move(driver));
                so.url = exptr(std::move(url));
                return ce(std::move(so));
            }
            if (path.size() == 2 && path[0] == "http" && path[1] == "Server") {
                if (!x->args.empty()) diag("new http.Server() takes no arguments", e.span);
                return ce(CHttpNewServer{});
            }
            if (path.size() == 2 && path[0] == "http" && path[1] == "JsonValidator") {
                if (x->args.size() != 1) {
                    diag("new http.JsonValidator({...}) takes one schema object: { field: "
                         "http.JsonValidator.string(), ... }",
                         e.span);
                    return cbool(false);
                }
                CExpr schema = check_expr(*x->args[0], false);
                std::vector<std::pair<std::string, FieldKind>> fields;
                auto* ol = std::get_if<CObjectLit>(&schema.v);
                bool schema_ok = ol != nullptr && expr_ty(schema).tag == CTy::Tag::Object;
                if (schema_ok) {
                    for (const auto& [idx, cexpr] : ol->inits) {
                        const std::string& fname =
                            idx < classes[ol->class_id].fields.size()
                                ? classes[ol->class_id].fields[idx].first
                                : std::string();
                        CTy t = expr_ty(cexpr);
                        if (!(t.tag == CTy::Tag::Object && t.object.name == "http_FieldValidator")) {
                            diag("JsonValidator schema field '" + fname +
                                     "' must be a field validator (e.g. "
                                     "http.JsonValidator.string().required())",
                                 e.span);
                            return cbool(false);
                        }
                        fields.emplace_back(fname, validator_kind_of(cexpr));
                    }
                } else {
                    diag("new http.JsonValidator takes an object literal schema: { field: "
                         "http.JsonValidator.string(), ... }",
                         e.span);
                    return cbool(false);
                }
                CHttpValidatorNew vn;
                vn.schema = exptr(std::move(schema));
                vn.fields = std::move(fields);
                return ce(std::move(vn));
            }
            // local class (optionally module-qualified): `new Calc(args)` /
            // `new sample_mod.Calc(args)` — construct, then run the
            // constructor when the class declares one
            std::string cname;
            bool is_std_alias = path.size() == 2 &&
                                (path[0] == "net" || path[0] == "http" || path[0] == "fs" ||
                                 path[0] == "crypto" || path[0] == "sql" ||
                                 path[0] == "time" || path[0] == "os");
            if (path.size() == 1) {
                cname = path[0];
            } else if (path.size() == 2 && !is_std_alias && contains_str(uses, path[0])) {
                cname = path[1];
            }
            if (!cname.empty()) {
                if (path.size() >= 2 && module_classes.count(cname) &&
                    !exported_classes.count(cname)) {
                    diag("no exported member '" + cname + "' from module '" + path[0] + "'", e.span);
                    return cbool(false);
                }
                auto it = class_by_name.find(cname);
                if (it == class_by_name.end()) {
                    if (generic_classes.count(cname)) {
                        diag("generic classes do not support constructor arguments", e.span);
                        return cbool(false);
                    }
                    diag("unknown class '" + cname + "'", e.span);
                    return cbool(false);
                }
                if (classes[it->second].is_abstract) {
                    diag("cannot instantiate abstract class '" + cname + "'", e.span);
                    return cbool(false);
                }
                std::string ctor_fn = cname + "__new";
                auto sit = sigs.find(ctor_fn);
                if (sit == sigs.end()) {
                    // no own constructor: TypeScript calls the inherited
                    // base constructor implicitly — do the same
                    size_t base_id = SIZE_MAX;
                    for (size_t c = it->second;;) {
                        std::string cand = classes[c].name + "__new";
                        if (sigs.count(cand)) {
                            base_id = c;
                            break;
                        }
                        if (!classes[c].base.has_value()) break;
                        c = *classes[c].base;
                    }
                    if (base_id != SIZE_MAX) {
                        sit = sigs.find(classes[base_id].name + "__new");
                        ctor_fn = sit->first;
                    }
                }
                if (x->args.empty() && sit == sigs.end()) return ce(CNewClass{it->second});
                if (sit == sigs.end()) {
                    diag("class '" + cname +
                             "' takes no constructor arguments (declare a constructor to accept "
                             "arguments)",
                         e.span);
                    return cbool(false);
                }
                std::vector<CTy> params;
                for (size_t i = 1; i < sit->second.params.size(); ++i)
                    params.push_back(sit->second.params[i]);
                if (x->args.size() != params.size()) {
                    diag("constructor of class '" + cname + "' takes " + numstr(params.size()) +
                             " argument(s), got " + numstr(x->args.size()),
                         e.span);
                }
                std::vector<CExpr> cargs;
                for (size_t i = 0; i < x->args.size(); ++i) {
                    CExpr c = check_expr(*x->args[i], false);
                    if (i < params.size()) {
                        adapt_literal(c, params[i]);
                        coerce(expr_ty(c), params[i], x->args[i]->span);
                    }
                    cargs.push_back(std::move(c));
                }
                CNewClassCtor nc;
                nc.class_id = it->second;
                nc.fn = ctor_fn;
                nc.args = std::move(cargs);
                return ce(std::move(nc));
            }
            std::vector<std::string> parts = path;
            diag("unknown class '" + join(parts, ".") + "'", e.span);
            return cbool(false);
        }
        if (auto* x = std::get_if<ast::EIndex>(&e.v)) {
            CExpr oc = check_expr(*x->obj, false);
            CTy ot = expr_ty(oc);
            CExpr ic = check_expr(*x->index, false);
            CTy it = expr_ty(ic);
            if (!is_i32(it)) {
                diag("array index must be Int32, found " + it.name(), e.span);
            }
            if (ot.tag == CTy::Tag::Array) {
                CIndex ix;
                ix.obj = exptr(std::move(oc));
                ix.idx = exptr(std::move(ic));
                ix.ty = deref(ot.inner);
                return ce(std::move(ix));
            }
            diag("indexing requires an Array, found " + ot.name(), e.span);
            return cbool(false);
        }
        if (auto* x = std::get_if<ast::EMember>(&e.v)) {
            // static member on a class name: `Name.count` (checked before
            // the object expression, which would fail variable lookup)
            if (auto* id = std::get_if<ast::EIdent>(&x->obj->v)) {
                if (!contains_str(uses, id->name) && class_by_name.count(id->name)) {
                    return check_static_member(class_by_name[id->name], x->name, e.span);
                }
            }
            // property access: `arr.length`, `str.length`, and fields on
            // class instances / ghost objects
            const ast::ExprPtr& obj = x->obj;
            const std::string& name = x->name;
            CExpr oc = check_expr(*obj, false);
            CTy ot = expr_ty(oc);
            if (name == "length") {
                if (ot.tag == CTy::Tag::Array) {
                    CArrayOp a;
                    a.op = ArrayOpKind::Len;
                    a.arr = exptr(std::move(oc));
                    a.ret = cty_int();
                    return ce(std::move(a));
                }
                if (ot.tag == CTy::Tag::Str) {
                    CStrLen sl;
                    sl.v = exptr(std::move(oc));
                    return ce(std::move(sl));
                }
            }
            if (ot.tag == CTy::Tag::Interface) {
                const std::string& iname = ot.iface;
                auto it = interfaces.find(iname);
                std::vector<std::pair<std::string, CTy>> fields =
                    it != interfaces.end() ? it->second : std::vector<std::pair<std::string, CTy>>{};
                for (const auto& [n, t] : fields) {
                    if (n == name) {
                        CIfaceGet g;
                        g.obj = exptr(std::move(oc));
                        g.field = name;
                        g.ty = t;
                        return ce(std::move(g));
                    }
                }
                if (iface_has_method(iname, name)) {
                    diag("interface '" + iname + "': '" + name + "' is a method (call it)", e.span);
                    return cbool(false);
                }
                std::vector<std::string> names;
                names.reserve(fields.size());
                for (const auto& [n, _] : fields) names.push_back(n);
                diag("interface '" + iname + "' has no field '" + name + "' (fields: " +
                         (names.empty() ? std::string("none") : join(names, ", ")) + ")",
                     e.span);
                return cbool(false);
            }
            if (ot.tag == CTy::Tag::Object) {
                if (ot.object.id < classes.size()) {
                    const ClassDef& def = classes[ot.object.id];
                    for (size_t idx = 0; idx < def.fields.size(); ++idx) {
                        if (def.fields[idx].first == name) {
                            // visibility flags live on the defining class
                            auto own = field_owner(ot.object.id, name);
                            if (own.has_value()) {
                                const ClassDef& odef = classes[*own];
                                bool priv = odef.field_is_private(name);
                                bool prot = std::find(odef.protected_fields.begin(),
                                                      odef.protected_fields.end(),
                                                      name) != odef.protected_fields.end();
                                if (!can_access_member(*own, priv, prot)) {
                                    diag(std::string("field '" + name + "' of class '" +
                                                     odef.name + "' is " +
                                                     (priv ? "private" : "protected") +
                                                     " (access it inside the class or a subclass)"),
                                         e.span);
                                    return cbool(false);
                                }
                            }
                            CFieldGet g;
                            g.obj = exptr(std::move(oc));
                            g.idx = idx;
                            g.ty = def.fields[idx].second;
                            return ce(std::move(g));
                        }
                    }
                    // no field: a getter? (`get name()`)
                    if (auto g = find_member(ot.object.id, name, "get_")) {
                        auto sit = sigs.find(g->second);
                        bool virtual_get = classes[ot.object.id].is_abstract ||
                                           subtree_overrides(ot.object.id, name, "get_");
                        if (!virtual_get && sit->second.ret) {
                            CCallFn f;
                            f.name = g->second;
                            f.args.push_back(std::move(oc));
                            f.ret = *sit->second.ret;
                            f.is_async = false;
                            return ce(std::move(f));
                        }
                        if (virtual_get && sit->second.ret) {
                            CIfaceCall call;
                            call.obj = exptr(std::move(oc));
                            call.iface = classes[ot.object.id].name;
                            call.method = "get_" + name;
                            call.ret = *sit->second.ret;
                            return ce(std::move(call));
                        }
                        diag("cannot read '" + name + "' here: the getter's return type is not "
                             "yet known (add a return type annotation)",
                             e.span);
                        return cbool(false);
                    }
                    std::vector<std::string> names;
                    names.reserve(def.fields.size());
                    for (const auto& [n, _] : def.fields) names.push_back(n);
                    diag("class '" + ot.object.name + "' has no field '" + name + "' (fields: " +
                             (names.empty() ? std::string("none") : join(names, ", ")) + ")",
                         e.span);
                    return cbool(false);
                }
            }
            diag("member access is only supported for 'console.log(...)', 'arr.length' and "
                 "'str.length'",
                 e.span);
            return cbool(false);
        }
        if (auto* x = std::get_if<ast::ENewPromise>(&e.v)) {
            auto* lam = std::get_if<ast::ELambda>(&x->executor->v);
            if (!lam) {
                diag("new Promise(...) takes an executor lambda: new Promise((resolve) => { ... })",
                     e.span);
                return cbool(false);
            }
            size_t idx = check_executor_lambda(*lam->lambda);
            return ce(CNewPromise{idx});
        }
        if (std::holds_alternative<ast::ELambda>(e.v)) {
            diag("lambda functions can only be passed directly to setTimeout(...) or new Promise(...)",
                 e.span);
            return cbool(false);
        }
        return cbool(false);
    }

    CExpr check_binary(ast::BinOp op, const ast::Expr& l, const ast::Expr& r, Span span) {
        CExpr lc = check_expr(l, false);
        CExpr rc = check_expr(r, false);
        adapt_peers(lc, rc);
        CTy lt = expr_ty(lc);
        CTy rt = expr_ty(rc);
        BinOpC opc;
        switch (op) {
            case ast::BinOp::Add: opc = BinOpC::Add; break;
            case ast::BinOp::Sub: opc = BinOpC::Sub; break;
            case ast::BinOp::Mul: opc = BinOpC::Mul; break;
            case ast::BinOp::Div: opc = BinOpC::Div; break;
            case ast::BinOp::Mod: opc = BinOpC::Mod; break;
            case ast::BinOp::Lt: opc = BinOpC::Lt; break;
            case ast::BinOp::Gt: opc = BinOpC::Gt; break;
            case ast::BinOp::Le: opc = BinOpC::Le; break;
            case ast::BinOp::Ge: opc = BinOpC::Ge; break;
            case ast::BinOp::Eq: opc = BinOpC::Eq; break;
            case ast::BinOp::Neq: opc = BinOpC::Neq; break;
            case ast::BinOp::And: opc = BinOpC::And; break;
            case ast::BinOp::Or: opc = BinOpC::Or; break;
            default: opc = BinOpC::Add; break;
        }
        switch (opc) {
            case BinOpC::And:
            case BinOpC::Or: {
                if (lt.tag != CTy::Tag::Bool || rt.tag != CTy::Tag::Bool) {
                    diag("'&&'/'||' require bools, found " + lt.name() + " and " + rt.name(), span);
                }
                CBinary b;
                b.op = opc;
                b.lhs = exptr(std::move(lc));
                b.rhs = exptr(std::move(rc));
                b.ty = cty_bool();
                return ce(std::move(b));
            }
            case BinOpC::Lt:
            case BinOpC::Gt:
            case BinOpC::Le:
            case BinOpC::Ge: {
                if (!is_numeric(lt) || !is_numeric(rt)) {
                    diag("comparison requires numbers, found " + lt.name() + " and " + rt.name(), span);
                } else if (lt.tag == CTy::Tag::Int && rt.tag == CTy::Tag::Int && !(lt == rt)) {
                    diag("comparison requires the same integer type, found " + lt.name() + " and " +
                             rt.name(),
                         span);
                }
                CBinary b;
                b.op = opc;
                b.lhs = exptr(std::move(lc));
                b.rhs = exptr(std::move(rc));
                b.ty = cty_bool();
                return ce(std::move(b));
            }
            case BinOpC::Eq:
            case BinOpC::Neq: {
                if (lt.tag == CTy::Tag::Str && rt.tag == CTy::Tag::Str) {
                    // string equality is lowered through the runtime helper
                } else {
                    if (!is_numeric(lt) && lt.tag != CTy::Tag::Bool)
                        diag("'=='/'!=' requires numbers or bools, found " + lt.name(), span);
                    if (!is_numeric(rt) && rt.tag != CTy::Tag::Bool)
                        diag("'=='/'!=' requires numbers or bools, found " + rt.name(), span);
                    if (lt.tag == CTy::Tag::Int && rt.tag == CTy::Tag::Int && !(lt == rt)) {
                        diag("'=='/'!=' requires the same integer type, found " + lt.name() + " and " +
                                 rt.name(),
                             span);
                    }
                }
                CBinary b;
                b.op = opc;
                b.lhs = exptr(std::move(lc));
                b.rhs = exptr(std::move(rc));
                b.ty = cty_bool();
                return ce(std::move(b));
            }
            case BinOpC::Add: {
                if (lt.tag == CTy::Tag::Str || rt.tag == CTy::Tag::Str) {
                    diag("cannot use '+' with strings (use a template literal `...${x}...`)", span);
                }
                return arith(opc, std::move(lc), std::move(rc), lt, rt, span);
            }
            case BinOpC::Mod: {
                adapt_peers(lc, rc);
                lt = expr_ty(lc);
                rt = expr_ty(rc);
                if (lt.tag != CTy::Tag::Int || rt.tag != CTy::Tag::Int || !(lt == rt)) {
                    diag("operator '%' requires integer operands of the same type, found " +
                             lt.name() + " and " + rt.name(),
                         span);
                }
                CBinary b;
                b.op = opc;
                b.lhs = exptr(std::move(lc));
                b.rhs = exptr(std::move(rc));
                b.ty = lt.tag == CTy::Tag::Int ? lt : cty_int();
                return ce(std::move(b));
            }
            case BinOpC::Sub:
            case BinOpC::Mul:
            case BinOpC::Div:
            default:
                return arith(opc, std::move(lc), std::move(rc), lt, rt, span);
        }
    }

    CExpr arith(BinOpC opc, CExpr lc, CExpr rc, CTy lt, CTy rt, Span span) {
        adapt_peers(lc, rc);
        lt = expr_ty(lc);
        rt = expr_ty(rc);
        if (!is_numeric(lt) || !is_numeric(rt)) {
            diag("arithmetic requires numbers, found " + lt.name() + " and " + rt.name(), span);
        }
        CTy result = cty_int();
        if (lt.tag == CTy::Tag::Int && rt.tag == CTy::Tag::Int) {
            if (!(lt == rt)) {
                diag("arithmetic requires the same integer type, found " + lt.name() + " and " +
                         rt.name(),
                     span);
            }
            result = lt.tag == CTy::Tag::Int ? lt : cty_int();
        } else if (lt.tag == CTy::Tag::Float && rt.tag == CTy::Tag::Float) {
            result = (lt.bits == 32 && rt.bits == 32) ? cty_float(32) : cty_float(64);
        } else if (lt.tag == CTy::Tag::Float) {
            result = lt;
        } else if (rt.tag == CTy::Tag::Float) {
            result = rt;
        }
        CBinary b;
        b.op = opc;
        b.lhs = exptr(std::move(lc));
        b.rhs = exptr(std::move(rc));
        b.ty = result;
        return ce(std::move(b));
    }

    // ------------------------------------------------------------------
    // std modules: net
    // ------------------------------------------------------------------

    CExpr check_net_module(const std::string& name, const std::vector<ast::ExprPtr>& args, Span span) {
        if (name == "connect") {
            if (args.size() != 2) {
                diag("net.connect takes (host, port)", span);
                return cbool(false);
            }
            CExpr host = check_expr(*args[0], false);
            CExpr port = check_expr(*args[1], false);
            coerce(expr_ty(host), cty_str(), args[0]->span);
            coerce(expr_ty(port), cty_int(), args[1]->span);
            CNetConnect nc;
            nc.host = exptr(std::move(host));
            nc.port = exptr(std::move(port));
            return ce(std::move(nc));
        }
        diag("net." + name + " is not supported (net has connect; streams have write, read_all, close)",
             span);
        return cbool(false);
    }

    // ------------------------------------------------------------------
    // std modules: http
    // ------------------------------------------------------------------

    CExpr check_http_module(const std::string& name, const std::vector<ast::ExprPtr>& args, Span span) {
        if (name == "get") {
            if (args.size() != 1) {
                diag("http.get takes (url)", span);
                return cbool(false);
            }
            CExpr url = check_expr(*args[0], false);
            coerce(expr_ty(url), cty_str(), args[0]->span);
            CHttpCall c;
            c.method = "GET";
            c.url = exptr(std::move(url));
            return ce(std::move(c));
        }
        if (name == "post") {
            if (args.size() != 2) {
                diag("http.post takes (url, body)", span);
                return cbool(false);
            }
            CExpr url = check_expr(*args[0], false);
            coerce(expr_ty(url), cty_str(), args[0]->span);
            CExpr body = check_expr(*args[1], false);
            coerce(expr_ty(body), cty_str(), args[1]->span);
            CHttpCall c;
            c.method = "POST";
            c.url = exptr(std::move(url));
            c.body = exptr(std::move(body));
            return ce(std::move(c));
        }
        if (name == "listen") {
            if (args.size() != 2) {
                diag("http.listen takes (port, handler)", span);
                return cbool(false);
            }
            CExpr port = check_expr(*args[0], false);
            coerce(expr_ty(port), cty_int(), args[0]->span);
            size_t lambda = 0;
            if (auto* lam = std::get_if<ast::ELambda>(&args[1]->v)) {
                size_t req_id = class_by_name.count("http_Request") ? class_by_name["http_Request"] : SIZE_MAX;
                size_t res_id = class_by_name.count("http_Res") ? class_by_name["http_Res"] : SIZE_MAX;
                lambda = check_callback_lambda(*lam->lambda,
                                               {cty_object(req_id, "http_Request"),
                                                cty_object(res_id, "http_Res")},
                                               cty_void(), LambdaAbi::Listen)
                             .first;
            } else {
                diag("http.listen's handler must be a lambda: (req, res) => { ... }", args[1]->span);
                return cbool(false);
            }
            CHttpListen hl;
            hl.port = exptr(std::move(port));
            hl.lambda = lambda;
            return ce(std::move(hl));
        }
        diag("http." + name + " is not supported (http has: get, post, listen)", span);
        return cbool(false);
    }

    CExpr check_stream_method(CExpr stream, const std::string& name,
                              const std::vector<ast::ExprPtr>& args, Span span) {
        if (name == "write") {
            if (args.size() != 1) {
                diag("stream.write takes one string", span);
                return cbool(false);
            }
            CExpr data = check_expr(*args[0], false);
            coerce(expr_ty(data), cty_str(), args[0]->span);
            CNetWrite w;
            w.stream = exptr(std::move(stream));
            w.data = exptr(std::move(data));
            return ce(std::move(w));
        }
        if (name == "read_all") {
            if (!args.empty()) {
                diag("stream.read_all takes no arguments", span);
                return cbool(false);
            }
            CNetReadAll r;
            r.stream = exptr(std::move(stream));
            return ce(std::move(r));
        }
        if (name == "close") {
            if (!args.empty()) {
                diag("stream.close takes no arguments", span);
                return cbool(false);
            }
            CNetClose c;
            c.stream = exptr(std::move(stream));
            return ce(std::move(c));
        }
        diag("streams have no method '" + name + "' (write, read_all, close)", span);
        return cbool(false);
    }

    CExpr check_res_method(CExpr res, const std::string& name, const std::vector<ast::ExprPtr>& args,
                           Span span) {
        if (name == "send" || name == "json") {
            if (args.size() != 1) {
                diag("res." + name + " takes exactly one value", span);
                return cbool(false);
            }
            CExpr v = check_expr(*args[0], false);
            CTy t = expr_ty(v);
            if (t.tag == CTy::Tag::Void) diag("cannot send a void value", span);
            if (t.tag == CTy::Tag::Promise) diag("cannot send a Promise (await it first)", span);
            CResSend s;
            s.res = exptr(std::move(res));
            s.value = exptr(std::move(v));
            s.json = name == "json";
            return ce(std::move(s));
        }
        diag("responses have no method '" + name + "' (send, json)", span);
        return cbool(false);
    }

    /// Dispatch a member call on a `sql.Connection` object.
    CExpr check_sql_conn_method(CExpr conn, const std::string& name,
                                const std::vector<ast::ExprPtr>& args, Span span) {
        if (name == "execute" || name == "query") {
            if (args.size() != 1) {
                diag("db." + name + " takes exactly one argument: a sql.stmt`...` template", span);
                return cbool(false);
            }
            auto parts = sql_stmt_parts(*args[0], span);
            if (!parts) return cbool(false);
            const std::string& sql = parts->first;
            std::vector<CExpr>& binds = parts->second;
            if (name == "execute") {
                size_t result_class = class_by_name.count("sql_Result") ? class_by_name["sql_Result"]
                                                                        : SIZE_MAX;
                CSqlExec ex;
                ex.conn = exptr(std::move(conn));
                ex.sql = sql;
                ex.binds = std::move(binds);
                ex.result_class = result_class;
                return ce(std::move(ex));
            }
            CSqlQuery q;
            q.conn = exptr(std::move(conn));
            q.sql = sql;
            q.binds = std::move(binds);
            return ce(std::move(q));
        }
        if (name == "close") {
            if (!args.empty()) {
                diag("db.close takes no arguments", span);
                return cbool(false);
            }
            CSqlClose c;
            c.conn = exptr(std::move(conn));
            return ce(std::move(c));
        }
        diag("sql.Connection has no method '" + name + "' (execute, query, close)", span);
        return cbool(false);
    }

    /// Validate a `sql.stmt\`...\`` tagged template: returns the SQL text
    /// with `?` placeholders plus the checked bind expressions.
    std::optional<std::pair<std::string, std::vector<CExpr>>> sql_stmt_parts(const ast::Expr& e,
                                                                             Span span) {
        auto* tag = std::get_if<ast::ETagged>(&e.v);
        if (!tag) {
            diag("expected a sql.stmt`...` template (e.g. db.execute(sql.stmt`SELECT 1`))", span);
            return std::nullopt;
        }
        bool ok_tag = false;
        if (auto* m = std::get_if<ast::EMember>(&tag->tag->v)) {
            if (auto* id = std::get_if<ast::EIdent>(&m->obj->v))
                ok_tag = id->name == "sql" && m->name == "stmt";
        }
        if (!ok_tag) {
            diag("only the sql.stmt template tag is supported (e.g. sql.stmt`SELECT ...`)", e.span);
            return std::nullopt;
        }
        std::string sql;
        std::vector<CExpr> binds;
        for (const auto& p : tag->parts) {
            if (auto* s = std::get_if<ast::TStr>(&p)) {
                sql += s->value;
            } else if (auto* te = std::get_if<ast::TExpr>(&p)) {
                CExpr c = check_expr(*te->expr, false);
                CTy t = expr_ty(c);
                if (t.tag == CTy::Tag::Int || t.tag == CTy::Tag::Float || t.tag == CTy::Tag::Str ||
                    t.tag == CTy::Tag::Bool) {
                    // bindable
                } else if (t.tag == CTy::Tag::Void) {
                    diag("cannot bind a void value in sql.stmt", te->expr->span);
                } else {
                    diag("cannot bind a " + t.name() +
                             " value in sql.stmt (bind an integer, float, string or bool)",
                         te->expr->span);
                }
                sql += '?';
                binds.push_back(std::move(c));
            }
        }
        return std::make_pair(std::move(sql), std::move(binds));
    }

    /// Dispatch a member call on an http std-module object. Returns nullopt
    /// when the receiver is not one of the builtin http objects.
    std::optional<CExpr> check_http_object_method(CExpr oc, const ObjectTy& o, const std::string& name,
                                                  const std::vector<ast::ExprPtr>& args, Span span) {
        if (o.name == "http_Res") return check_res_method(std::move(oc), name, args, span);
        if (o.name == "http_Server") return check_server_method(std::move(oc), name, args, span);
        if (o.name == "http_Router") return check_router_method(std::move(oc), name, args, span);
        if (o.name == "http_JsonValidator")
            return check_validator_method(std::move(oc), name, args, span);
        if (o.name == "http_FieldValidator")
            return check_field_method(std::move(oc), name, args, span);
        if (o.name == "sql_Connection") return check_sql_conn_method(std::move(oc), name, args, span);
        return std::nullopt;
    }

    CExpr check_server_method(CExpr server, const std::string& name,
                              const std::vector<ast::ExprPtr>& args, Span span) {
        if (name == "router") {
            if (args.size() != 1) {
                diag("server.router takes one options object: { path: \"/...\" }", span);
                return cbool(false);
            }
            CExpr path = check_expr(*args[0], false);
            bool ok = false;
            if (auto* ol = std::get_if<CObjectLit>(&path.v)) {
                if (ol->class_id < classes.size()) {
                    const auto& fields = classes[ol->class_id].fields;
                    ok = fields.size() == 1 && fields[0].first == "path" && !ol->inits.empty() &&
                         expr_ty(ol->inits[0].second).tag == CTy::Tag::Str;
                }
            }
            std::optional<CExpr> path_val;
            if (auto* ol = std::get_if<CObjectLit>(&path.v)) {
                // exactly one field `path` with a string value; lower
                // to just the string value
                if (!ol->inits.empty()) path_val = ol->inits[0].second;
            }
            if (!ok) {
                diag("server.router takes { path: \"...\" } with a string path", span);
                return cbool(false);
            }
            CHttpRouter r;
            r.server = exptr(std::move(server));
            r.path = exptr(path_val ? std::move(*path_val) : ce(CStr{std::string()}));
            return ce(std::move(r));
        }
        if (name == "listen") {
            if (args.size() != 2) {
                diag("server.listen takes (host, port)", span);
                return cbool(false);
            }
            CExpr host = check_expr(*args[0], false);
            coerce(expr_ty(host), cty_str(), args[0]->span);
            CExpr port = check_expr(*args[1], false);
            coerce(expr_ty(port), cty_int(), args[1]->span);
            CHttpServerListen sl;
            sl.host = exptr(std::move(host));
            sl.port = exptr(std::move(port));
            return ce(std::move(sl));
        }
        diag("http.Server has no method '" + name + "' (available: router, listen)", span);
        return cbool(false);
    }

    CExpr check_router_method(CExpr router, const std::string& name,
                              const std::vector<ast::ExprPtr>& args, Span span) {
        std::string method;
        if (name == "get") {
            method = "GET";
        } else if (name == "post") {
            method = "POST";
        } else {
            diag("routers have no method '" + name + "' (available: get, post)", span);
            return cbool(false);
        }
        if (args.size() != 2) {
            diag("router." + name + " takes (path, handler)", span);
            return cbool(false);
        }
        CExpr path = check_expr(*args[0], false);
        coerce(expr_ty(path), cty_str(), args[0]->span);
        size_t lambda = 0;
        bool handler_async = false;
        if (auto* lam = std::get_if<ast::ELambda>(&args[1]->v)) {
            bool is_async = lam->lambda->is_async;
            size_t req_id = class_by_name.count("http_Request") ? class_by_name["http_Request"] : SIZE_MAX;
            size_t res_id = class_by_name.count("http_Res") ? class_by_name["http_Res"] : SIZE_MAX;
            auto r = check_callback_lambda(*lam->lambda,
                                           {cty_object(req_id, "http_Request"),
                                            cty_object(res_id, "http_Res")},
                                           cty_void(), LambdaAbi::Listen);
            if (is_async) lambdas[r.first].is_async = true;
            lambda = r.first;
            handler_async = is_async;
        } else {
            diag("router." + name + " takes a handler lambda (req, res) => { ... }", span);
            return cbool(false);
        }
        CHttpRouteAdd ra;
        ra.router = exptr(std::move(router));
        ra.method = std::move(method);
        ra.path = exptr(std::move(path));
        ra.lambda = lambda;
        ra.is_async = handler_async;
        return ce(std::move(ra));
    }

    CExpr check_validator_method(CExpr validator, const std::string& name,
                                 const std::vector<ast::ExprPtr>& args, Span span) {
        if (name != "validate") {
            diag("http.JsonValidator has no method '" + name + "' (available: validate)", span);
            return cbool(false);
        }
        if (args.size() != 1) {
            diag("validate takes the body string to check", span);
            return cbool(false);
        }
        CExpr body = check_expr(*args[0], false);
        coerce(expr_ty(body), cty_str(), args[0]->span);
        // the validated result's shape comes from the schema the validator
        // was created with — tracked when `new http.JsonValidator(...)`
        // was bound to a variable
        std::optional<std::vector<std::pair<std::string, FieldKind>>> fields;
        if (auto* idn = std::get_if<CIdent>(&validator.v)) {
            auto it = validator_fields.find(idn->id);
            if (it != validator_fields.end()) fields = it->second;
        }
        if (!fields) {
            diag("cannot infer the validated shape (assign the validator to a variable first: "
                 "`let v = new http.JsonValidator({...})`)",
                 span);
            return cbool(false);
        }
        // synthesize the ghost class of validated results: same field names,
        // typed by the validator kind
        std::string class_name = "Class_anon_" + pad4(ghost_count);
        ghost_count += 1;
        ClassDef def;
        def.name = class_name;
        for (const auto& [f, k] : *fields) {
            def.fields.emplace_back(f, k == FieldKind::Str ? cty_str() : cty_int());
        }
        def.is_ghost = true;
        classes.push_back(std::move(def));
        size_t out_class = classes.size() - 1;
        CHttpValidate hv;
        hv.v = exptr(std::move(validator));
        hv.body = exptr(std::move(body));
        hv.out_class = out_class;
        return ce(std::move(hv));
    }

    CExpr check_field_method(CExpr v, const std::string& name, const std::vector<ast::ExprPtr>& args,
                             Span span) {
        if (name == "required") {
            if (!args.empty()) {
                diag(".required() takes no arguments", span);
                return cbool(false);
            }
            CHttpFieldOp fo;
            fo.v = exptr(std::move(v));
            fo.op = FieldOp::Required;
            return ce(std::move(fo));
        }
        if (name == "min" || name == "max") {
            if (args.size() != 1) {
                diag("." + name + "() takes one number", span);
                return cbool(false);
            }
            CExpr arg = check_expr(*args[0], false);
            coerce(expr_ty(arg), cty_int(), args[0]->span);
            CHttpFieldOp fo;
            fo.v = exptr(std::move(v));
            fo.op = name == "min" ? FieldOp::Min : FieldOp::Max;
            fo.arg = exptr(std::move(arg));
            return ce(std::move(fo));
        }
        diag("field validators have no method '" + name + "' (chainable: required, min, max)", span);
        return cbool(false);
    }

    // ------------------------------------------------------------------
    // Generics: monomorphization
    // ------------------------------------------------------------------

    /// Build a source annotation for a concrete type (for substituting type
    /// parameters in generic declarations).
    ast::TyAnn to_ann(const CTy& t, Span span) const {
        std::string name;
        std::vector<ast::TyAnn> args;
        switch (t.tag) {
            case CTy::Tag::Void: name = "void"; break;
            case CTy::Tag::Int: name = "Int32"; break;
            case CTy::Tag::Float: name = "Float64"; break;
            case CTy::Tag::Bool: name = "bool"; break;
            case CTy::Tag::Str: name = "string"; break;
            case CTy::Tag::Stream: name = "Stream"; break;
            case CTy::Tag::Promise: name = "Promise"; args.push_back(to_ann(deref(t.inner), span)); break;
            case CTy::Tag::Array: name = "Array"; args.push_back(to_ann(deref(t.inner), span)); break;
            case CTy::Tag::Object: name = t.object.name; break;
            case CTy::Tag::Interface: name = t.iface; break;
            case CTy::Tag::Resolver: name = "void"; break;
        }
        ast::TyAnn a;
        a.name = std::move(name);
        a.args = std::move(args);
        a.span = span;
        return a;
    }

    /// Replace type-parameter annotations with their concrete bindings,
    /// recursively through a declaration.
    void subst_ann(ast::TyAnn& ann, const std::vector<std::pair<std::string, CTy>>& bindings) const {
        if (ann.args.empty()) {
            for (const auto& [n, t] : bindings) {
                if (n == ann.name) {
                    ann = to_ann(t, ann.span);
                    break;
                }
            }
            return;
        }
        for (auto& a : ann.args) subst_ann(a, bindings);
    }

    void subst_stmt(ast::Stmt& s, const std::vector<std::pair<std::string, CTy>>& bindings) const {
        if (auto* x = std::get_if<ast::SLet>(&s.v)) {
            if (x->ann) subst_ann(*x->ann, bindings);
            if (x->init) subst_expr(*x->init, bindings);
        } else if (auto* x = std::get_if<ast::SIf>(&s.v)) {
            subst_expr(*x->cond, bindings);
            for (auto& st : x->then_body) subst_stmt(st, bindings);
            if (x->else_body)
                for (auto& st : *x->else_body) subst_stmt(st, bindings);
        } else if (auto* x = std::get_if<ast::SWhile>(&s.v)) {
            subst_expr(*x->cond, bindings);
            for (auto& st : x->body) subst_stmt(st, bindings);
        } else if (auto* x = std::get_if<ast::SFor>(&s.v)) {
            if (x->init) subst_stmt(**x->init, bindings);
            if (x->cond) subst_expr(**x->cond, bindings);
            if (x->update) subst_stmt(**x->update, bindings);
            for (auto& st : x->body) subst_stmt(st, bindings);
        } else if (auto* x = std::get_if<ast::SReturn>(&s.v)) {
            if (x->value) subst_expr(**x->value, bindings);
        } else if (auto* x = std::get_if<ast::SExpr>(&s.v)) {
            subst_expr(*x->expr, bindings);
        } else if (auto* x = std::get_if<ast::SAssign>(&s.v)) {
            subst_expr(*x->value, bindings);
        } else if (auto* x = std::get_if<ast::SMemberAssign>(&s.v)) {
            subst_expr(*x->value, bindings);
        } else if (auto* x = std::get_if<ast::SIndexAssign>(&s.v)) {
            subst_expr(*x->index, bindings);
            subst_expr(*x->value, bindings);
        }
    }

    void subst_expr(ast::Expr& e, const std::vector<std::pair<std::string, CTy>>& bindings) const {
        if (auto* x = std::get_if<ast::ETemplate>(&e.v)) {
            for (auto& p : x->parts)
                if (auto* te = std::get_if<ast::TExpr>(&p)) subst_expr(*te->expr, bindings);
        } else if (auto* x = std::get_if<ast::ETagged>(&e.v)) {
            for (auto& p : x->parts)
                if (auto* te = std::get_if<ast::TExpr>(&p)) subst_expr(*te->expr, bindings);
        } else if (auto* x = std::get_if<ast::EUnary>(&e.v)) {
            subst_expr(*x->operand, bindings);
        } else if (auto* x = std::get_if<ast::EBinary>(&e.v)) {
            subst_expr(*x->lhs, bindings);
            subst_expr(*x->rhs, bindings);
        } else if (auto* x = std::get_if<ast::ECall>(&e.v)) {
            subst_expr(*x->callee, bindings);
            for (auto& a : x->args) subst_expr(*a, bindings);
        } else if (auto* x = std::get_if<ast::ECallGen>(&e.v)) {
            subst_expr(*x->callee, bindings);
            for (auto& t : x->type_args) subst_ann(t, bindings);
            for (auto& a : x->args) subst_expr(*a, bindings);
        } else if (auto* x = std::get_if<ast::EMember>(&e.v)) {
            subst_expr(*x->obj, bindings);
        } else if (auto* x = std::get_if<ast::EIndex>(&e.v)) {
            subst_expr(*x->obj, bindings);
            subst_expr(*x->index, bindings);
        } else if (auto* x = std::get_if<ast::EArrayLit>(&e.v)) {
            for (auto& el : x->elems) subst_expr(*el, bindings);
        } else if (auto* x = std::get_if<ast::EObjectLit>(&e.v)) {
            for (auto& f : x->fields) subst_expr(*f.value, bindings);
        } else if (auto* x = std::get_if<ast::ENewClass>(&e.v)) {
            for (auto& a : x->type_args) subst_ann(a, bindings);
        } else if (auto* x = std::get_if<ast::ENewCtor>(&e.v)) {
            for (auto& a : x->args) subst_expr(*a, bindings);
        } else if (auto* x = std::get_if<ast::EAwait>(&e.v)) {
            subst_expr(*x->operand, bindings);
        } else if (auto* x = std::get_if<ast::ENewPromise>(&e.v)) {
            subst_expr(*x->executor, bindings);
        } else if (auto* x = std::get_if<ast::ELambda>(&e.v)) {
            for (auto& p : x->lambda->params)
                if (p.ann) subst_ann(*p.ann, bindings);
            for (auto& st : x->lambda->body) subst_stmt(st, bindings);
        }
    }

    /// Monomorphize a generic class instantiation (`Box<Int32>`) and return the
    /// concrete object type.
    CTy mono_class(const std::string& name, const std::vector<ast::TyAnn>& args, Span span) {
        auto git = generic_classes.find(name);
        if (git == generic_classes.end()) return cty_void(); // caller guarantees presence
        const ast::ClassDecl& decl = git->second;
        if (args.size() != decl.type_params.size()) {
            diag("generic class '" + name + "' takes " + numstr(decl.type_params.size()) +
                     " type parameter(s), got " + numstr(args.size()),
                 span);
            return cty_void();
        }
        std::vector<std::pair<std::string, CTy>> bindings;
        for (size_t i = 0; i < decl.type_params.size(); ++i)
            bindings.emplace_back(decl.type_params[i], ty_of(args[i]));
        std::string mangles;
        for (size_t i = 0; i < bindings.size(); ++i) {
            if (i) mangles += "$";
            mangles += mangle_ty(bindings[i].second);
        }
        std::string mangled = decl.name + "$" + mangles;
        auto exist = class_by_name.find(mangled);
        if (exist != class_by_name.end()) return cty_object(exist->second, mangled);
        std::vector<std::pair<std::string, CTy>> fields;
        std::unordered_set<std::string> seen;
        for (const auto& f : decl.fields) {
            if (!seen.insert(f.name).second) continue;
            ast::TyAnn a2 = f.ty;
            subst_ann(a2, bindings);
            fields.emplace_back(f.name, ty_of(a2));
        }
        size_t id = classes.size();
        ClassDef d;
        d.name = mangled;
        d.fields = std::move(fields);
        d.is_ghost = false;
        classes.push_back(std::move(d));
        class_by_name[mangled] = id;

        // Monomorphize the methods: each gets its annotations substituted,
        // a `this` parameter of the concrete class, and the mangled name
        // `Map$T__method` so instance calls resolve through `find_member`.
        // Signatures are all registered before any body is checked, so a
        // method can call its siblings regardless of declaration order.
        struct PrepMethod {
            ast::FnDecl f;
            std::vector<CTy> params;
            std::optional<CTy> pre_ret;
        };
        std::vector<PrepMethod> prepped;
        // the constructor first: `Map$T__new(this, params)`, no return
        if (decl.ctor) {
            PrepMethod pm;
            pm.f = clone_fn_decl(*decl.ctor);
            pm.f.name = mangled + "__new";
            ast::TyAnn self_ann;
            self_ann.name = mangled;
            self_ann.span = span;
            ast::Param this_p;
            this_p.name = "this";
            this_p.ann = self_ann;
            this_p.span = span;
            pm.f.params.insert(pm.f.params.begin(), std::move(this_p));
            for (auto& p : pm.f.params)
                if (p.ann) subst_ann(*p.ann, bindings);
            pm.f.ret_ann.reset();
            for (auto& st : pm.f.body) subst_stmt(st, bindings);
            for (const auto& p : pm.f.params) {
                pm.params.push_back(p.ann ? ty_of(*p.ann) : cty_void());
            }
            {
                FnSig sig;
                sig.params = pm.params;
                sig.ret = cty_void();
                sig.is_async = false;
                sigs[pm.f.name] = std::move(sig);
            }
            prepped.push_back(std::move(pm));
        }
        for (const auto& m : decl.methods) {
            if (m.is_abstract || m.is_static) continue;
            PrepMethod pm;
            pm.f = clone_fn_decl(m);
            switch (m.kind) {
                case ast::MemberKind::Getter: pm.f.name = mangled + "__get_" + m.name; break;
                case ast::MemberKind::Setter: pm.f.name = mangled + "__set_" + m.name; break;
                case ast::MemberKind::Method: pm.f.name = mangled + "__" + m.name; break;
            }
            ast::TyAnn self_ann;
            self_ann.name = mangled;
            self_ann.span = span;
            ast::Param this_p;
            this_p.name = "this";
            this_p.ann = self_ann;
            this_p.span = span;
            pm.f.params.insert(pm.f.params.begin(), std::move(this_p));
            for (auto& p : pm.f.params)
                if (p.ann) subst_ann(*p.ann, bindings);
            if (pm.f.ret_ann) subst_ann(*pm.f.ret_ann, bindings);
            for (auto& st : pm.f.body) subst_stmt(st, bindings);
            for (const auto& p : pm.f.params) {
                pm.params.push_back(p.ann ? ty_of(*p.ann) : cty_void());
            }
            if (pm.f.ret_ann) pm.pre_ret = ty_of(*pm.f.ret_ann);
            {
                FnSig sig;
                sig.params = pm.params;
                sig.ret = pm.pre_ret;
                sig.is_async = false;
                sigs[pm.f.name] = std::move(sig);
            }
            prepped.push_back(std::move(pm));
        }
        for (auto& pm : prepped) {
            // check the body in a fresh function context, exactly like a
            // monomorphized generic function
            std::optional<CTy> saved_ret_ty = std::move(ret_ty);
            ret_ty.reset();
            bool saved_float = saw_float_ret;
            uint8_t saved_float_bits = ret_float_bits;
            bool saved_async = is_async_ctx;
            bool saved_toplevel = in_toplevel;
            std::optional<std::pair<CTy, bool>> saved_promise_infer = std::move(promise_infer);
            promise_infer.reset();
            CFn cf = check_fn(pm.f);
            ret_ty = std::move(saved_ret_ty);
            saw_float_ret = saved_float;
            ret_float_bits = saved_float_bits;
            is_async_ctx = saved_async;
            in_toplevel = saved_toplevel;
            promise_infer = std::move(saved_promise_infer);
            CTy ret = cf.ret;
            mono_fns.push_back(std::move(cf));
            if (!pm.pre_ret) {
                FnSig sig;
                sig.params = pm.params;
                sig.ret = ret;
                sig.is_async = false;
                sigs[pm.f.name] = std::move(sig);
            }
        }
        return cty_object(id, mangled);
    }

    /// Check a call with explicit type arguments: `pick<Int32>(1, 2, true)`.
    /// Also handles module-qualified generic calls: `map.newMap<string>()`.
    CExpr check_call_gen(const ast::Expr& callee, const std::vector<ast::TyAnn>& type_args,
                         const std::vector<ast::ExprPtr>& args, Span span) {
        std::string name;
        if (auto* id = std::get_if<ast::EIdent>(&callee.v)) {
            name = id->name;
        } else if (auto* mem = std::get_if<ast::EMember>(&callee.v)) {
            // `mod.fn<T>(...)` — resolve the member chain to the module
            // function's mangled name, then proceed as a direct generic call
            std::vector<std::string> chain;
            if (member_chain(callee, chain) && chain.size() >= 2 &&
                contains_str(uses, chain[0])) {
                const std::string& m = chain[0];
                std::string full = use_prefix_of(m);
                for (size_t i = 1; i < chain.size(); ++i) full += "__" + chain[i];
                if (generic_fns.count(full)) {
                    if (!exported_fns.count(full)) {
                        diag("no exported member '" + chain.back() + "' from module '" + m + "'",
                             span);
                        return cbool(false);
                    }
                    return check_generic_call(full, &type_args, args, span);
                }
                if (sigs.count(full)) {
                    diag("function '" + chain.back() + "' does not take type parameters", span);
                    return cbool(false);
                }
                diag("module '" + m + "' has no function '" + chain.back() + "'", span);
                return cbool(false);
            }
            diag("type arguments are only supported on direct function calls", span);
            return cbool(false);
        } else {
            diag("type arguments are only supported on direct function calls", span);
            return cbool(false);
        }
        if (generic_fns.count(name)) {
            return check_generic_call(name, &type_args, args, span);
        }
        if (sigs.count(name)) {
            diag("function '" + name + "' does not take type parameters", span);
            return cbool(false);
        }
        diag("unknown function '" + name + "'", span);
        return cbool(false);
    }

    /// Check a call to a generic function: infer type bindings from the
    /// arguments, monomorphize the body, and lower to a call of the mono'd name.
    CExpr check_generic_call(const std::string& name, const std::vector<ast::TyAnn>* type_args,
                             const std::vector<ast::ExprPtr>& args, Span span) {
        auto git = generic_fns.find(name);
        if (git == generic_fns.end()) return cbool(false); // caller guarantees presence
        const ast::FnDecl& decl = git->second;
        if (args.size() != decl.params.size()) {
            diag("function '" + name + "' takes " + numstr(decl.params.size()) + " argument(s), got " +
                     numstr(args.size()),
                 span);
            return cbool(false);
        }
        if (type_args && type_args->size() != decl.type_params.size()) {
            diag("function '" + name + "' takes " + numstr(decl.type_params.size()) +
                     " type parameter(s), got " + numstr(type_args->size()) + " type argument(s)",
                 span);
            return cbool(false);
        }
        std::vector<CExpr> cargs;
        std::vector<CTy> arg_tys;
        for (const auto& a : args) {
            CExpr c = check_expr(*a, false);
            arg_tys.push_back(expr_ty(c));
            cargs.push_back(std::move(c));
        }
        // infer type bindings from the arguments
        std::vector<std::pair<std::string, std::optional<CTy>>> bindings;
        for (const auto& tp : decl.type_params) bindings.emplace_back(tp, std::nullopt);
        for (size_t i = 0; i < decl.params.size() && i < arg_tys.size(); ++i) {
            const auto& p = decl.params[i];
            if (!p.ann) {
                diag("parameter '" + p.name + "' of generic function '" + name +
                         "' needs a type annotation",
                     p.span);
                continue;
            }
            unify_ann(*p.ann, arg_tys[i], bindings, p.span);
        }
        // explicit type arguments win over inference
        if (type_args) {
            for (size_t i = 0; i < type_args->size() && i < bindings.size(); ++i) {
                bindings[i].second = ty_of((*type_args)[i]);
            }
        }
        std::vector<std::string> unbound;
        for (const auto& [n, b] : bindings)
            if (!b) unbound.push_back("'" + n + "'");
        if (!unbound.empty()) {
            diag("cannot infer type parameter(s) " + join(unbound, ", ") +
                     " of generic function '" + name + "' from the call arguments",
                 span);
            return cbool(false);
        }
        std::vector<std::pair<std::string, CTy>> concrete;
        concrete.reserve(bindings.size());
        for (auto& [n, b] : bindings) concrete.emplace_back(n, std::move(*b));
        std::string mangles;
        for (size_t i = 0; i < concrete.size(); ++i) {
            if (i) mangles += "__";
            mangles += mangle_ty(concrete[i].second);
        }
        std::string mangled = decl.name + "__" + mangles;
        // re-check arguments against the substituted parameter types
        auto sit = sigs.find(mangled);
        if (sit != sigs.end()) {
            std::vector<CTy> sig_params = sit->second.params;
            std::optional<CTy> sig_ret = sit->second.ret;
            bool sig_async = sit->second.is_async;
            for (size_t i = 0; i < cargs.size(); ++i) {
                if (i < sig_params.size()) {
                    adapt_literal(cargs[i], sig_params[i]);
                    coerce(expr_ty(cargs[i]), sig_params[i], args[i]->span);
                }
            }
            CCallFn f;
            f.name = mangled;
            f.args = std::move(cargs);
            f.ret = sig_ret ? *sig_ret : cty_void();
            f.is_async = sig_async;
            return ce(std::move(f));
        }
        ast::FnDecl subst = clone_fn_decl(decl);
        subst.name = mangled;
        for (auto& p : subst.params)
            if (p.ann) subst_ann(*p.ann, concrete);
        if (subst.ret_ann) subst_ann(*subst.ret_ann, concrete);
        for (auto& st : subst.body) subst_stmt(st, concrete);
        // register the signature first when the return is annotated, so
        // recursive generic calls resolve
        std::vector<CTy> params;
        for (const auto& p : subst.params) {
            if (p.ann) {
                params.push_back(ty_of(*p.ann));
            } else {
                diag("parameter '" + p.name + "' of generic function '" + subst.name +
                         "' needs a type annotation (strict mode)",
                     p.span);
                params.push_back(cty_void());
            }
        }
        std::optional<CTy> pre_ret;
        if (subst.ret_ann) pre_ret = ty_of(*subst.ret_ann);
        {
            FnSig sig;
            sig.params = params;
            sig.ret = pre_ret;
            sig.is_async = subst.is_async;
            sigs[mangled] = std::move(sig);
        }
        // check_fn is invoked mid-function here, so save and restore the
        // in-progress function context (the mono'd body's `return` statements
        // must not influence the caller's inferred return type)
        std::optional<CTy> saved_ret_ty = std::move(ret_ty);
        ret_ty.reset();
        bool saved_float = saw_float_ret;
        uint8_t saved_float_bits = ret_float_bits;
        bool saved_async = is_async_ctx;
        bool saved_toplevel = in_toplevel;
        std::optional<std::pair<CTy, bool>> saved_promise_infer = std::move(promise_infer);
        promise_infer.reset();
        CFn cf = check_fn(subst);
        ret_ty = std::move(saved_ret_ty);
        saw_float_ret = saved_float;
        ret_float_bits = saved_float_bits;
        is_async_ctx = saved_async;
        in_toplevel = saved_toplevel;
        promise_infer = std::move(saved_promise_infer);
        CTy ret = cf.ret;
        for (size_t i = 0; i < cargs.size() && i < params.size(); ++i) {
            adapt_literal(cargs[i], params[i]);
            coerce(expr_ty(cargs[i]), params[i], args[i]->span);
        }
        mono_fns.push_back(std::move(cf));
        if (!pre_ret) {
            FnSig sig;
            sig.params = params;
            sig.ret = ret;
            sig.is_async = subst.is_async;
            sigs[mangled] = std::move(sig);
        }
        CCallFn f;
        f.name = mangled;
        f.args = std::move(cargs);
        f.ret = ret;
        f.is_async = subst.is_async;
        return ce(std::move(f));
    }

    /// Unify a parameter annotation against an argument type, binding type
    /// parameters. Unbound params stay `None`.
    void unify_ann(const ast::TyAnn& ann, const CTy& actual,
                   std::vector<std::pair<std::string, std::optional<CTy>>>& bindings, Span span) {
        if (ann.args.empty()) {
            bool is_param = false;
            for (const auto& [n, _] : bindings)
                if (n == ann.name) {
                    is_param = true;
                    break;
                }
            if (is_param) {
                for (auto& [n, b] : bindings) {
                    if (n != ann.name) continue;
                    if (!b) {
                        b = actual;
                    } else {
                        const CTy& prev = *b;
                        if (!(prev == actual) && !(prev.tag == CTy::Tag::Int && actual.tag == CTy::Tag::Float) &&
                            !(prev.tag == CTy::Tag::Float && actual.tag == CTy::Tag::Float)) {
                            diag("type parameter '" + n + "' is bound to " + actual.name() +
                                     " here but " + prev.name() + " elsewhere",
                                 span);
                        }
                    }
                }
                return;
            }
        }
        if (ann.name == "Array" && !ann.args.empty() && actual.tag == CTy::Tag::Array) {
            unify_ann(ann.args[0], deref(actual.inner), bindings, span);
            return;
        }
        if (ann.name == "Promise" && !ann.args.empty() && actual.tag == CTy::Tag::Promise) {
            unify_ann(ann.args[0], deref(actual.inner), bindings, span);
            return;
        }
        CTy expected = ty_of(ann);
        coerce(actual, expected, span);
    }

    // ------------------------------------------------------------------
    // std modules: fs
    // ------------------------------------------------------------------

    CExpr check_fs_module(const std::string& name, const std::vector<ast::ExprPtr>& args, Span span) {
        if (name == "args") {
            if (!args.empty()) diag("fs.args takes no arguments", span);
            return ce(CFsArgs{});
        }
        if (!args.empty()) {
            CExpr path = check_expr(*args[0], false);
            coerce(expr_ty(path), cty_str(), args[0]->span);
            CExprPtr p = exptr(std::move(path));
            if (name == "readFile") {
                if (args.size() != 1) {
                    diag("fs.readFile takes (path)", span);
                    return cbool(false);
                }
                CFsReadFile r;
                r.path = std::move(p);
                return ce(std::move(r));
            }
            if (name == "writeFile") {
                if (args.size() != 2) {
                    diag("fs.writeFile takes (path, data)", span);
                    return cbool(false);
                }
                CExpr data = check_expr(*args[1], false);
                coerce(expr_ty(data), cty_str(), args[1]->span);
                CFsWriteFile w;
                w.path = std::move(p);
                w.data = exptr(std::move(data));
                return ce(std::move(w));
            }
            if (name == "exists") {
                if (args.size() != 1) {
                    diag("fs.exists takes (path)", span);
                    return cbool(false);
                }
                CFsExists r;
                r.path = std::move(p);
                return ce(std::move(r));
            }
            if (name == "readDir") {
                if (args.size() != 1) {
                    diag("fs.readDir takes (path)", span);
                    return cbool(false);
                }
                CFsReadDir r;
                r.path = std::move(p);
                return ce(std::move(r));
            }
            if (name == "mkdir") {
                if (args.size() != 1) {
                    diag("fs.mkdir takes (path)", span);
                    return cbool(false);
                }
                CFsMkdir r;
                r.path = std::move(p);
                return ce(std::move(r));
            }
            if (name == "remove") {
                if (args.size() != 1) {
                    diag("fs.remove takes (path)", span);
                    return cbool(false);
                }
                CFsRemove r;
                r.path = std::move(p);
                return ce(std::move(r));
            }
        }
        diag("fs." + name +
                 " is not supported (fs has args, readFile, writeFile, exists, readDir, mkdir, remove)",
             span);
        return cbool(false);
    }

    // ------------------------------------------------------------------
    // std modules: crypto
    // ------------------------------------------------------------------

    CExpr check_crypto_module(const std::string& name, const std::vector<ast::ExprPtr>& args,
                              Span span) {
        if (name == "sha256") {
            if (args.size() != 1) {
                diag("crypto.sha256 takes (data)", span);
                return cbool(false);
            }
            CExpr v = check_expr(*args[0], false);
            coerce(expr_ty(v), cty_str(), args[0]->span);
            CCryptoSha256 c;
            c.v = exptr(std::move(v));
            return ce(std::move(c));
        }
        if (name == "base64Encode") {
            if (args.size() != 1) {
                diag("crypto.base64Encode takes (data)", span);
                return cbool(false);
            }
            CExpr v = check_expr(*args[0], false);
            coerce(expr_ty(v), cty_str(), args[0]->span);
            CCryptoBase64 c;
            c.decode = false;
            c.v = exptr(std::move(v));
            return ce(std::move(c));
        }
        if (name == "base64Decode") {
            if (args.size() != 1) {
                diag("crypto.base64Decode takes (data)", span);
                return cbool(false);
            }
            CExpr v = check_expr(*args[0], false);
            coerce(expr_ty(v), cty_str(), args[0]->span);
            CCryptoBase64 c;
            c.decode = true;
            c.v = exptr(std::move(v));
            return ce(std::move(c));
        }
        if (name == "randomHex") {
            if (args.size() != 1) {
                diag("crypto.randomHex takes (numBytes)", span);
                return cbool(false);
            }
            CExpr v = check_expr(*args[0], false);
            coerce(expr_ty(v), cty_int(), args[0]->span);
            CCryptoRandomHex c;
            c.bytes = exptr(std::move(v));
            return ce(std::move(c));
        }
        if (name == "uuid") {
            if (!args.empty()) {
                diag("crypto.uuid takes no arguments", span);
                return cbool(false);
            }
            return ce(CCryptoUuid{});
        }
        diag("crypto." + name +
                 " is not supported (crypto has sha256, base64Encode, base64Decode, randomHex, uuid)",
             span);
        return cbool(false);
    }

    // ------------------------------------------------------------------
    // std modules: time
    // ------------------------------------------------------------------

    CExpr check_time_module(const std::string& name, const std::vector<ast::ExprPtr>& args,
                            Span span) {
        auto no_args = [&](const char* sig) {
            if (!args.empty()) diag(std::string(sig) + " takes no arguments", span);
        };
        if (name == "now" || name == "nowSecs" || name == "nowNanos" || name == "mono") {
            no_args(("time." + name).c_str());
            TimeKind kind = name == "now"       ? TimeKind::Now
                            : name == "nowSecs" ? TimeKind::NowSecs
                            : name == "nowNanos" ? TimeKind::NowNanos
                                                 : TimeKind::Mono;
            return ce(CTimeCall{kind, nullptr});
        }
        if (name == "sleep") {
            if (args.size() != 1) {
                diag("time.sleep takes (ms)", span);
                return cbool(false);
            }
            CExpr v = check_expr(*args[0], false);
            adapt_literal(v, cty_int(64, false));
            coerce(expr_ty(v), cty_int(64, false), args[0]->span);
            CTimeCall c;
            c.kind = TimeKind::Sleep;
            c.arg = exptr(std::move(v));
            return ce(std::move(c));
        }
        if (name == "format") {
            if (args.size() != 1) {
                diag("time.format takes (ms)", span);
                return cbool(false);
            }
            CExpr v = check_expr(*args[0], false);
            adapt_literal(v, cty_int(64, false));
            coerce(expr_ty(v), cty_int(64, false), args[0]->span);
            CTimeCall c;
            c.kind = TimeKind::Format;
            c.arg = exptr(std::move(v));
            return ce(std::move(c));
        }
        diag("time." + name +
                 " is not supported (time has now, nowSecs, nowNanos, mono, sleep, format)",
             span);
        return cbool(false);
    }

    // ------------------------------------------------------------------
    // std modules: os
    // ------------------------------------------------------------------

    CExpr check_os_module(const std::string& name, const std::vector<ast::ExprPtr>& args,
                          Span span) {
        auto no_args = [&](const char* sig) {
            if (!args.empty()) diag(std::string(sig) + " takes no arguments", span);
        };
        // string-returning no-arg queries
        if (name == "platform" || name == "arch" || name == "hostname" ||
            name == "cwd" || name == "tmpdir") {
            no_args(("os." + name).c_str());
            OsKind kind = name == "platform"   ? OsKind::Platform
                          : name == "arch"     ? OsKind::Arch
                          : name == "hostname" ? OsKind::Hostname
                          : name == "cwd"      ? OsKind::Cwd
                                               : OsKind::Tmpdir;
            return ce(COsCall{kind, nullptr, nullptr});
        }
        if (name == "pid") {
            no_args("os.pid");
            return ce(COsCall{OsKind::Pid, nullptr, nullptr});
        }
        auto str_arg = [&](size_t i) {
            CExpr v = check_expr(*args[i], false);
            coerce(expr_ty(v), cty_str(), args[i]->span);
            return v;
        };
        if (name == "env") {
            if (args.size() != 1) {
                diag("os.env takes (name)", span);
                return cbool(false);
            }
            return ce(COsCall{OsKind::EnvGet, exptr(str_arg(0)), nullptr});
        }
        if (name == "envOr") {
            if (args.size() != 2) {
                diag("os.envOr takes (name, fallback)", span);
                return cbool(false);
            }
            CExpr a = str_arg(0);
            CExpr b = str_arg(1);
            return ce(COsCall{OsKind::EnvGetOr, exptr(std::move(a)), exptr(std::move(b))});
        }
        if (name == "setEnv") {
            if (args.size() != 2) {
                diag("os.setEnv takes (name, value)", span);
                return cbool(false);
            }
            CExpr a = str_arg(0);
            CExpr b = str_arg(1);
            return ce(COsCall{OsKind::EnvSet, exptr(std::move(a)), exptr(std::move(b))});
        }
        if (name == "unsetEnv") {
            if (args.size() != 1) {
                diag("os.unsetEnv takes (name)", span);
                return cbool(false);
            }
            return ce(COsCall{OsKind::EnvUnset, exptr(str_arg(0)), nullptr});
        }
        if (name == "exec") {
            if (args.size() != 1) {
                diag("os.exec takes (command)", span);
                return cbool(false);
            }
            return ce(COsCall{OsKind::Exec, exptr(str_arg(0)), nullptr});
        }
        if (name == "exit") {
            if (args.size() != 1) {
                diag("os.exit takes (code)", span);
                return cbool(false);
            }
            CExpr v = check_expr(*args[0], false);
            adapt_literal(v, cty_int(64, false));
            coerce(expr_ty(v), cty_int(64, false), args[0]->span);
            return ce(COsCall{OsKind::Exit, exptr(std::move(v)), nullptr});
        }
        diag("os." + name +
                 " is not supported (os has platform, arch, hostname, cwd, tmpdir, pid, env, "
                 "envOr, setEnv, unsetEnv, exec, exit)",
             span);
        return cbool(false);
    }

    /// Collect a dotted member chain: `a.b.c` → ["a","b","c"]. Returns false
    /// when the expression isn't a pure identifier/member chain.
    static bool member_chain(const ast::Expr& e, std::vector<std::string>& out) {
        if (auto* id = std::get_if<ast::EIdent>(&e.v)) {
            out.push_back(id->name);
            return true;
        }
        if (auto* mem = std::get_if<ast::EMember>(&e.v)) {
            if (!member_chain(*mem->obj, out)) return false;
            out.push_back(mem->name);
            return true;
        }
        return false;
    }

    /// Mangled-name prefix for a `use` alias (identity when absent: std
    /// modules and registry packages mangle to their own name).
    std::string use_prefix_of(const std::string& alias) const {
        auto it = use_prefix.find(alias);
        return it != use_prefix.end() ? it->second : alias;
    }

    /// True when the function currently being checked belongs to the class
    /// (its name is `Class__method` / `Class__new`) — the only context where
    /// `private` fields may be accessed.
    bool can_access_private(const std::string& class_name) const {
        std::string prefix = class_name + "__";
        return cur_fn.size() > prefix.size() && cur_fn.compare(0, prefix.size(), prefix) == 0;
    }

    /// Method call on a class instance: `obj.method(args)`. The method may
    /// be inherited; when any subclass overrides it (or the declared class
    /// is abstract), the call is virtually dispatched on the runtime class
    /// id, TypeScript-style.
    std::optional<CExpr> check_user_method(CExpr oc, const ObjectTy& obj, const std::string& name,
                                           const std::vector<ast::ExprPtr>& args, Span span) {
        auto impl = find_member(obj.id, name, "");
        // abstract method on an abstract receiver: no implementation exists
        // anywhere up the chain; the call dispatches virtually
        if (!impl) {
            const FnSig* asig = nullptr;
            for (size_t c = obj.id;;) {
                auto it = abstract_sigs.find(classes[c].name + "__" + name);
                if (it != abstract_sigs.end()) {
                    asig = &it->second;
                    break;
                }
                if (!classes[c].base.has_value()) return std::nullopt;
                c = *classes[c].base;
            }
            if (args.size() != asig->params.size()) {
                diag("method '" + name + "' of class '" + obj.name + "' takes " +
                         numstr(asig->params.size()) + " argument(s), got " + numstr(args.size()),
                     span);
            }
            std::vector<CExpr> cargs;
            for (size_t i = 0; i < args.size(); ++i) {
                CExpr c = check_expr(*args[i], false);
                if (i < asig->params.size()) {
                    adapt_literal(c, asig->params[i]);
                    coerce(expr_ty(c), asig->params[i], args[i]->span);
                }
                cargs.push_back(std::move(c));
            }
            if (!asig->ret) {
                diag("cannot call abstract method '" + name + "' of class '" + obj.name +
                         "' here: its return type is not yet known (add a return type annotation)",
                     span);
                return cbool(false);
            }
            CIfaceCall call;
            call.obj = exptr(std::move(oc));
            call.iface = classes[obj.id].name;
            call.method = name;
            call.args = std::move(cargs);
            call.ret = *asig->ret;
            return ce(std::move(call));
        }
        auto [definer, mangled] = *impl;
        const ClassDef& def = classes[definer];
        bool priv = std::find(def.private_methods.begin(), def.private_methods.end(), name) !=
                    def.private_methods.end();
        bool prot = std::find(def.protected_methods.begin(), def.protected_methods.end(), name) !=
                    def.protected_methods.end();
        if (!can_access_member(definer, priv, prot)) {
            diag(std::string("method '" + name + "' of class '" + def.name + "' is " +
                             (priv ? "private" : "protected") +
                             " (call it inside the class or a subclass)"),
                 span);
            return cbool(false);
        }
        const FnSig& fsig = sigs[mangled];
        // param 0 is the implicit `this`
        std::vector<CTy> params;
        for (size_t i = 1; i < fsig.params.size(); ++i) params.push_back(fsig.params[i]);
        if (args.size() != params.size()) {
            diag("method '" + name + "' of class '" + obj.name + "' takes " +
                     numstr(params.size()) + " argument(s), got " + numstr(args.size()),
                 span);
        }
        std::vector<CExpr> cargs;
        cargs.push_back(std::move(oc));
        for (size_t i = 0; i < args.size(); ++i) {
            CExpr c = check_expr(*args[i], false);
            if (i < params.size()) {
                adapt_literal(c, params[i]);
                coerce(expr_ty(c), params[i], args[i]->span);
            }
            cargs.push_back(std::move(c));
        }
        bool dispatch = classes[obj.id].is_abstract || subtree_overrides(obj.id, name, "");
        if (dispatch) {
            CIfaceCall call;
            call.obj = exptr(std::move(cargs[0]));
            call.iface = classes[obj.id].name;
            call.method = name;
            call.args.assign(std::make_move_iterator(cargs.begin()) + 1,
                             std::make_move_iterator(cargs.end()));
            call.ret = fsig.ret ? *fsig.ret : cty_void();
            return ce(std::move(call));
        }
        if (fsig.ret) {
            CCallFn f;
            f.name = mangled;
            f.args = std::move(cargs);
            f.ret = *fsig.ret;
            f.is_async = fsig.is_async;
            return ce(std::move(f));
        }
        diag("cannot call method '" + name + "' of class '" + obj.name +
                 "' here: its return type is not yet known (add a return type annotation)",
             span);
        return cbool(false);
    }

    /// `obj.method(args)` where `obj` has an interface type. The signature
    /// comes from the interface; codegen dispatches on the runtime class id.
    CExpr check_iface_method(CExpr oc, const std::string& iname, const std::string& name,
                             const std::vector<ast::ExprPtr>& args, Span span) {
        const IfaceMSig* msig = nullptr;
        auto mit = iface_methods.find(iname);
        if (mit != iface_methods.end()) {
            for (const auto& [n, s] : mit->second)
                if (n == name) {
                    msig = &s;
                    break;
                }
        }
        if (!msig) {
            diag("interface '" + iname + "' has no method '" + name + "'", span);
            return cbool(false);
        }
        if (args.size() != msig->params.size()) {
            diag("method '" + name + "' of interface '" + iname + "' takes " +
                     numstr(msig->params.size()) + " argument(s), got " + numstr(args.size()),
                 span);
        }
        std::vector<CExpr> cargs;
        for (size_t i = 0; i < args.size(); ++i) {
            CExpr c = check_expr(*args[i], false);
            if (i < msig->params.size()) {
                adapt_literal(c, msig->params[i]);
                coerce(expr_ty(c), msig->params[i], args[i]->span);
            }
            cargs.push_back(std::move(c));
        }
        CIfaceCall call;
        call.obj = exptr(std::move(oc));
        call.iface = iname;
        call.method = name;
        call.args = std::move(cargs);
        call.ret = msig->ret;
        return ce(std::move(call));
    }

    /// `Name.staticMethod(args)` — static methods are plain functions with
    /// no `this`; visibility still applies.
    CExpr check_static_method_call(size_t cid, const std::string& name,
                                   const std::vector<ast::ExprPtr>& args, Span span) {
        auto impl = find_member(cid, name, "static__");
        if (!impl) {
            diag("class '" + classes[cid].name + "' has no static method '" + name + "'", span);
            return cbool(false);
        }
        auto [definer, mangled] = *impl;
        const ClassDef& def = classes[definer];
        bool priv = std::find(def.private_methods.begin(), def.private_methods.end(), name) !=
                    def.private_methods.end();
        bool prot = std::find(def.protected_methods.begin(), def.protected_methods.end(), name) !=
                    def.protected_methods.end();
        if (!can_access_member(definer, priv, prot)) {
            diag(std::string("static method '" + name + "' of class '" + def.name + "' is " +
                             (priv ? "private" : "protected")),
                 span);
            return cbool(false);
        }
        const FnSig& fsig = sigs[mangled];
        if (args.size() != fsig.params.size()) {
            diag("static method '" + name + "' of class '" + def.name + "' takes " +
                     numstr(fsig.params.size()) + " argument(s), got " + numstr(args.size()),
                 span);
        }
        std::vector<CExpr> cargs;
        for (size_t i = 0; i < args.size(); ++i) {
            CExpr c = check_expr(*args[i], false);
            if (i < fsig.params.size()) {
                adapt_literal(c, fsig.params[i]);
                coerce(expr_ty(c), fsig.params[i], args[i]->span);
            }
            cargs.push_back(std::move(c));
        }
        if (fsig.ret) {
            CCallFn f;
            f.name = mangled;
            f.args = std::move(cargs);
            f.ret = *fsig.ret;
            f.is_async = fsig.is_async;
            return ce(std::move(f));
        }
        diag("cannot call static method '" + name + "' of class '" + def.name +
                 "' here: its return type is not yet known (add a return type annotation)",
             span);
        return cbool(false);
    }

    /// `super(args)` inside a constructor: calls the nearest base class
    /// constructor with the current `this`.
    CExpr check_super_ctor(const std::vector<ast::ExprPtr>& args, Span span) {
        size_t cid = SIZE_MAX;
        if (auto c = cur_class_id()) cid = *c;
        if (cid >= classes.size()) {
            diag("'super' can only be used inside a class method or constructor", span);
            return cbool(false);
        }
        if (cur_fn.size() < 5 || cur_fn.compare(cur_fn.size() - 5, 5, "__new") != 0) {
            diag("'super(...)' is only allowed inside a constructor", span);
            return cbool(false);
        }
        if (!classes[cid].base.has_value()) {
            diag("class '" + classes[cid].name + "' has no base class: 'super(...)' cannot be "
                 "called",
                 span);
            return cbool(false);
        }
        // nearest ancestor constructor
        std::string base_new;
        size_t base_id = SIZE_MAX;
        for (size_t c = *classes[cid].base;;) {
            std::string cand = classes[c].name + "__new";
            if (sigs.count(cand)) {
                base_new = cand;
                base_id = c;
                break;
            }
            if (!classes[c].base.has_value()) break;
            c = *classes[c].base;
        }
        if (base_new.empty()) {
            diag("no constructor found in the base classes of '" + classes[cid].name +
                     "': 'super(...)' cannot be called",
                 span);
            return cbool(false);
        }
        auto this_v = lookup("this", span);
        if (!this_v) {
            diag("'super(...)' requires a 'this' binding (internal error)", span);
            return cbool(false);
        }
        auto [this_id, this_ty, iscap] = *this_v;
        (void)iscap;
        const FnSig& fsig = sigs[base_new];
        // param 0 of a lowered ctor is `this`
        std::vector<CTy> params;
        for (size_t i = 1; i < fsig.params.size(); ++i) params.push_back(fsig.params[i]);
        if (args.size() != params.size()) {
            diag("constructor of '" + classes[base_id].name + "' takes " + numstr(params.size()) +
                     " argument(s), got " + numstr(args.size()),
                 span);
        }
        std::vector<CExpr> cargs;
        CExpr self = ce(CIdent{this_id, this_ty});
        cargs.push_back(std::move(self));
        for (size_t i = 0; i < args.size(); ++i) {
            CExpr c = check_expr(*args[i], false);
            if (i < params.size()) {
                adapt_literal(c, params[i]);
                coerce(expr_ty(c), params[i], args[i]->span);
            }
            cargs.push_back(std::move(c));
        }
        CCallFn f;
        f.name = base_new;
        f.args = std::move(cargs);
        f.ret = cty_void();
        f.is_async = false;
        return ce(std::move(f));
    }

    /// `super.method(args)` inside a class method: a direct (non-virtual)
    /// call to the base class implementation with the current `this`.
    CExpr check_super_method(const std::string& name, const std::vector<ast::ExprPtr>& args,
                             Span span) {
        size_t cid = SIZE_MAX;
        if (auto c = cur_class_id()) cid = *c;
        if (cid >= classes.size()) {
            diag("'super' can only be used inside a class method or constructor", span);
            return cbool(false);
        }
        if (!classes[cid].base.has_value()) {
            diag("class '" + classes[cid].name +
                     "' has no base class: 'super." + name + "' cannot be called",
                 span);
            return cbool(false);
        }
        auto impl = find_member(*classes[cid].base, name, "");
        if (!impl) {
            diag("base class of '" + classes[cid].name + "' has no method '" + name + "'", span);
            return cbool(false);
        }
        auto [definer, mangled] = *impl;
        const FnSig& fsig = sigs[mangled];
        std::vector<CTy> params;
        for (size_t i = 1; i < fsig.params.size(); ++i) params.push_back(fsig.params[i]);
        if (args.size() != params.size()) {
            diag("method '" + name + "' of base class '" + classes[definer].name + "' takes " +
                     numstr(params.size()) + " argument(s), got " + numstr(args.size()),
                 span);
        }
        auto this_v = lookup("this", span);
        if (!this_v) {
            diag("'super." + name + "' requires a 'this' binding (internal error)", span);
            return cbool(false);
        }
        auto [this_id, this_ty, iscap] = *this_v;
        (void)iscap;
        std::vector<CExpr> cargs;
        cargs.push_back(ce(CIdent{this_id, this_ty}));
        for (size_t i = 0; i < args.size(); ++i) {
            CExpr c = check_expr(*args[i], false);
            if (i < params.size()) {
                adapt_literal(c, params[i]);
                coerce(expr_ty(c), params[i], args[i]->span);
            }
            cargs.push_back(std::move(c));
        }
        if (fsig.ret) {
            CCallFn f;
            f.name = mangled;
            f.args = std::move(cargs);
            f.ret = *fsig.ret;
            f.is_async = fsig.is_async;
            return ce(std::move(f));
        }
        diag("cannot call 'super." + name +
                 "' here: its return type is not yet known (add a return type annotation)",
             span);
        return cbool(false);
    }

    CExpr check_call(const ast::Expr& callee, const std::vector<ast::ExprPtr>& args, Span span) {
        // super.method(...) — direct call to the base class implementation
        const ast::EMember* mem = std::get_if<ast::EMember>(&callee.v);
        if (mem && std::get_if<ast::EIdent>(&mem->obj->v) &&
            std::get<ast::EIdent>(mem->obj->v).name == "super") {
            return check_super_method(mem->name, args, span);
        }
        // std-module and package functions: `net.connect(...)`, `http.get(...)`,
        // `mypkg.fn(...)`, `mod.sub.fn(...)` (modules expose functions under
        // their mangled `<prefix>__<path>__<fn>` names)
        if (mem) {
            std::vector<std::string> chain;
            if (member_chain(callee, chain) && chain.size() >= 2 &&
                contains_str(uses, chain[0])) {
                const std::string& m = chain[0];
                // statics on exported std-module classes: http.JsonValidator.string()
                bool json_static =
                    chain.size() == 3 && m == "http" && chain[1] == "JsonValidator";
                if (!json_static) {
                    if (chain.size() == 2) {
                        if (m == "net") return check_net_module(chain[1], args, span);
                        if (m == "http") return check_http_module(chain[1], args, span);
                        if (m == "fs") return check_fs_module(chain[1], args, span);
                        if (m == "crypto") return check_crypto_module(chain[1], args, span);
                        if (m == "time") return check_time_module(chain[1], args, span);
                        if (m == "os") return check_os_module(chain[1], args, span);
                    }
                    std::string full = use_prefix_of(m);
                    for (size_t i = 1; i < chain.size(); ++i) full += "__" + chain[i];
                    if (sigs.count(full)) {
                        if (!exported_fns.count(full)) {
                            diag("no exported member '" + chain.back() + "' from module '" + m +
                                     "'",
                                 span);
                            return cbool(false);
                        }
                        ast::Expr ident;
                        ident.span = span;
                        ident.v = ast::EIdent{full};
                        return check_call(ident, args, span);
                    }
                    diag("module '" + m + "' has no function '" + chain.back() + "'", span);
                    return cbool(false);
                }
            }
            // static methods on user classes: `Shape.count()`, `Utils.helpers()`
            if (auto* o = std::get_if<ast::EIdent>(&mem->obj->v)) {
                if (!contains_str(uses, o->name) && class_by_name.count(o->name)) {
                    return check_static_method_call(class_by_name[o->name], mem->name, args, span);
                }
            }
            // static methods on exported std-module classes:
            // `http.JsonValidator.string()` etc.
            if (auto* mem2 = std::get_if<ast::EMember>(&mem->obj->v)) {
                if (auto* m2 = std::get_if<ast::EIdent>(&mem2->obj->v)) {
                    if (m2->name == "http" && contains_str(uses, "http") &&
                        mem2->name == "JsonValidator") {
                        if (mem->name == "string") return ce(CHttpFieldNew{FieldKind::Str});
                        if (mem->name == "number") return ce(CHttpFieldNew{FieldKind::Int});
                        diag("JsonValidator." + mem->name +
                                 " is not supported (statics: string, number)",
                             span);
                        return cbool(false);
                    }
                }
            }
        }
        // console.log(...) and other member calls
        if (mem) {
            const std::string& name = mem->name;
            if (auto* o = std::get_if<ast::EIdent>(&mem->obj->v)) {
                if (o->name == "console") {
                    if (name != "log") {
                        diag("console." + name + " is not supported (only console.log)", span);
                    }
                    if (args.size() != 1) {
                        diag("console.log takes exactly one argument", span);
                        return cbool(false);
                    }
                    CExpr a = check_expr(*args[0], false);
                    CTy t = expr_ty(a);
                    if (t.tag == CTy::Tag::Void) diag("cannot print a void value", span);
                    if (t.tag == CTy::Tag::Promise)
                        diag("cannot print a Promise (await it first)", span);
                    CConsoleLog cl;
                    cl.v = exptr(std::move(a));
                    return ce(std::move(cl));
                }
                if (o->name == "String") {
                    if (name != "fromCharCode") {
                        diag("String." + name + " is not supported (only String.fromCharCode)",
                             span);
                        return cbool(false);
                    }
                    if (args.size() != 1) {
                        diag("String.fromCharCode takes exactly one argument", span);
                        return cbool(false);
                    }
                    CExpr a = check_expr(*args[0], false);
                    adapt_literal(a, cty_int(32, false));
                    coerce(expr_ty(a), cty_int(32, false), args[0]->span);
                    CStringFromCharCode f;
                    f.v = exptr(std::move(a));
                    return ce(std::move(f));
                }
                // other member calls: array methods, streams, http std-module objects
                CExpr oc = check_expr(*mem->obj, false);
                CTy ot = expr_ty(oc);
                if (ot.tag == CTy::Tag::Array)
                    return check_array_method(std::move(oc), deref(ot.inner), name, args, span);
                if (ot.tag == CTy::Tag::Str) return check_str_method(std::move(oc), name, args, span);
                if (ot.tag == CTy::Tag::Stream)
                    return check_stream_method(std::move(oc), name, args, span);
                if (ot.tag == CTy::Tag::Object) {
                    // gate on std-module class prefixes: the helper consumes
                    // its receiver, which must stay intact for user methods
                    if (ot.object.name.rfind("http_", 0) == 0 ||
                        ot.object.name.rfind("sql_", 0) == 0) {
                        if (auto r =
                                check_http_object_method(std::move(oc), ot.object, name, args, span))
                            return std::move(*r);
                    }
                    if (auto r = check_user_method(std::move(oc), ot.object, name, args, span))
                        return std::move(*r);
                    diag("class '" + ot.object.name + "' has no method '" + name + "'", span);
                    return cbool(false);
                }
                if (ot.tag == CTy::Tag::Interface)
                    return check_iface_method(std::move(oc), ot.iface, name, args, span);
                diag("'" + name + "' is not supported", span);
                return cbool(false);
            }
            // member calls on non-identifier objects (e.g. chained method
            // calls): array methods, http std-module objects, class methods
            CExpr oc = check_expr(*mem->obj, false);
            CTy ot = expr_ty(oc);
            if (ot.tag == CTy::Tag::Array)
                return check_array_method(std::move(oc), deref(ot.inner), name, args, span);
            if (ot.tag == CTy::Tag::Str) return check_str_method(std::move(oc), name, args, span);
            if (ot.tag == CTy::Tag::Stream)
                return check_stream_method(std::move(oc), name, args, span);
            if (ot.tag == CTy::Tag::Object) {
                if (ot.object.name.rfind("http_", 0) == 0 ||
                    ot.object.name.rfind("sql_", 0) == 0) {
                    if (auto r =
                            check_http_object_method(std::move(oc), ot.object, name, args, span))
                        return std::move(*r);
                }
                if (auto r = check_user_method(std::move(oc), ot.object, name, args, span))
                    return std::move(*r);
                diag("class '" + ot.object.name + "' has no method '" + name + "'", span);
                return cbool(false);
            }
            if (ot.tag == CTy::Tag::Interface)
                return check_iface_method(std::move(oc), ot.iface, name, args, span);
            diag("member access is only supported for 'console.log(...)' and array methods", span);
            return cbool(false);
        }
        if (auto* id = std::get_if<ast::EIdent>(&callee.v)) {
            const std::string& name = id->name;
            // setTimeout(fn, ms) — builtin, checked before variable lookup
            if (name == "setTimeout") {
                if (args.size() != 2) {
                    diag("setTimeout takes (callback, milliseconds)", span);
                    return cbool(false);
                }
                auto* lam = std::get_if<ast::ELambda>(&args[0]->v);
                if (!lam) {
                    diag("setTimeout's first argument must be a lambda: setTimeout(() => { ... }, ms)",
                         args[0]->span);
                    return cbool(false);
                }
                if (!lam->lambda->params.empty()) {
                    diag("setTimeout callbacks take no parameters", lam->lambda->span);
                }
                size_t idx = check_plain_lambda(*lam->lambda);
                CExpr ms = check_expr(*args[1], false);
                if (!is_i32(expr_ty(ms))) {
                    diag("setTimeout's delay must be an Int32", args[1]->span);
                }
                CSetTimeout st;
                st.lambda = idx;
                st.ms = exptr(std::move(ms));
                return ce(std::move(st));
            }
            // user function
            if (name == "super") return check_super_ctor(args, span);
            if (generic_fns.count(name)) {
                return check_generic_call(name, nullptr, args, span);
            }
            auto sit = sigs.find(name);
            if (sit != sigs.end()) {
                std::vector<CTy> params = sit->second.params;
                std::optional<CTy> ret = sit->second.ret;
                bool is_async = sit->second.is_async;
                if (args.size() != params.size()) {
                    diag("function '" + name + "' takes " + numstr(params.size()) +
                             " argument(s), got " + numstr(args.size()),
                         span);
                }
                std::vector<CExpr> cargs;
                for (size_t i = 0; i < args.size(); ++i) {
                    CExpr c = check_expr(*args[i], false);
                    if (i < params.size()) {
                        adapt_literal(c, params[i]);
                        coerce(expr_ty(c), params[i], args[i]->span);
                    }
                    cargs.push_back(std::move(c));
                }
                if (ret) {
                    CCallFn f;
                    f.name = name;
                    f.args = std::move(cargs);
                    f.ret = *ret;
                    f.is_async = is_async;
                    return ce(std::move(f));
                }
                diag("cannot call '" + name +
                         "' here: its return type is not yet known (add a return type annotation, "
                         "e.g. `: void`)",
                     span);
                return cbool(false);
            }
            // resolver call inside a promise executor?
            auto found = lookup(name, span);
            if (found) {
                auto [rid, ty, iscap] = *found;
                (void)iscap;
                if (ty.tag == CTy::Tag::Resolver) {
                    if (contains_sz(reject_ids, rid)) {
                        diag("'reject' is not supported in marina v0.1 (promises never fail)", span);
                        return cbool(false);
                    }
                    if (args.size() != 1) {
                        diag("resolve takes exactly one value", span);
                        return cbool(false);
                    }
                    CExpr a = check_expr(*args[0], false);
                    CTy at = expr_ty(a);
                    CTy rt = deref(ty.inner);
                    infer_promise_type(rt, at, span);
                    CCallResolver cr;
                    cr.id = rid;
                    cr.arg = exptr(std::move(a));
                    cr.inner = rt;
                    return ce(std::move(cr));
                }
                diag("'" + name + "' is not a function", span);
                return cbool(false);
            }
            diag("'" + name + "' is not a function", span);
            return cbool(false);
        }
        diag("this expression is not callable", span);
        return cbool(false);
    }

    void infer_promise_type(const CTy& declared, CTy arg, Span span) {
        if (!promise_infer) {
            promise_infer = std::make_pair(std::move(arg), false);
            return;
        }
        auto& [cur, saw_float] = *promise_infer;
        if (cur == arg) return;
        if (cur.tag == CTy::Tag::Float && arg.tag == CTy::Tag::Float) {
            cur = cty_float(cur.bits == 32 && arg.bits == 32 ? 32 : 64);
            saw_float = true;
            return;
        }
        if (cur.tag == CTy::Tag::Int && arg.tag == CTy::Tag::Float) {
            cur = arg;
            saw_float = true;
            return;
        }
        if (cur.tag == CTy::Tag::Float && arg.tag == CTy::Tag::Int) {
            saw_float = true;
            return;
        }
        coerce(arg, declared, span);
    }

    // ------------------------------------------------------------------
    // Lambdas
    // ------------------------------------------------------------------

    size_t enter_lambda_scope(size_t idx) {
        size_t base = scopes.size();
        lambda_base.push_back(base);
        lambda_idx.push_back(idx);
        cur_captures.emplace_back();
        lambda_ret_ok.push_back(false);
        scopes.emplace_back();
        return base;
    }

    std::vector<std::pair<size_t, CTy>> exit_lambda_scope() {
        scopes.pop_back();
        lambda_base.pop_back();
        lambda_idx.pop_back();
        std::vector<std::pair<size_t, CTy>> caps;
        if (!cur_captures.empty()) {
            caps = std::move(cur_captures.back());
            cur_captures.pop_back();
        }
        lambda_ret_ok.pop_back();
        return caps;
    }

    /// setTimeout callback: no params, must return void.
    size_t check_plain_lambda(const ast::Lambda& l) {
        size_t idx = lambdas.size();
        CLambda cl;
        cl.abi = LambdaAbi::Plain;
        cl.is_executor = false;
        cl.is_async = l.is_async;
        lambdas.push_back(std::move(cl));
        enter_lambda_scope(idx);
        ret_ty.reset();
        saw_float_ret = false;
        ret_float_bits = 0;
        std::vector<CStmt> body = check_stmts(l.body);
        CTy r = ret_ty ? *ret_ty : cty_void();
        if (r.tag != CTy::Tag::Void) {
            diag("setTimeout callbacks cannot return a value", l.span);
        }
        auto captures = exit_lambda_scope();
        lambdas[idx].body = std::move(body);
        lambdas[idx].captures = std::move(captures);
        return idx;
    }

    /// `new Promise((resolve) => { ... })`: infer T from resolve(...) calls.
    size_t check_executor_lambda(const ast::Lambda& l) {
        if (l.params.empty() || l.params.size() > 2) {
            diag("a promise executor takes (resolve) or (resolve, reject)", l.span);
            // still create a stub so compilation can continue
            size_t idx = lambdas.size();
            CLambda cl;
            cl.abi = LambdaAbi::Plain;
            cl.is_executor = true;
            cl.is_async = l.is_async;
            lambdas.push_back(std::move(cl));
            return idx;
        }
        size_t idx = lambdas.size();
        CLambda cl;
        cl.abi = LambdaAbi::Plain;
        cl.is_executor = true;
        cl.is_async = l.is_async;
        lambdas.push_back(std::move(cl));
        (void)enter_lambda_scope(idx);

        std::optional<std::pair<CTy, bool>> prev_infer = std::move(promise_infer);
        promise_infer.reset();

        std::vector<std::pair<size_t, CTy>> params;
        std::optional<size_t> reject_id;
        for (size_t i = 0; i < l.params.size(); ++i) {
            const auto& p = l.params[i];
            // placeholder type; finalized below once T is inferred
            size_t id = declare(p.name, cty_resolver(cty_void()), true, p.span);
            if (i == 1) reject_id = id;
            params.emplace_back(id, cty_resolver(cty_void()));
        }
        std::vector<size_t> saved_reject = reject_ids;
        if (reject_id) reject_ids.push_back(*reject_id);

        ret_ty.reset();
        saw_float_ret = false;
        ret_float_bits = 0;
        std::vector<CStmt> body = check_stmts(l.body);
        CTy r = ret_ty ? *ret_ty : cty_void();
        if (r.tag != CTy::Tag::Void) {
            diag("a promise executor cannot return a value", l.span);
        }

        CTy t;
        if (promise_infer) {
            auto [ty, saw_float] = *promise_infer;
            promise_infer.reset();
            t = (saw_float && ty.tag == CTy::Tag::Int) ? cty_float(64) : ty;
        } else {
            promise_infer.reset();
            diag("cannot infer the promise value type: the executor never calls resolve (write "
                 "`resolve(<value>)` inside)",
                 l.span);
            t = cty_void();
        }
        promise_infer = std::move(prev_infer);
        reject_ids = std::move(saved_reject);

        std::vector<std::pair<size_t, CTy>> final_params;
        for (const auto& [id, _] : params) {
            (void)_;
            var_types[id] = cty_resolver(t);
            final_params.emplace_back(id, cty_resolver(t));
        }

        auto captures = exit_lambda_scope();
        lambdas[idx].params = std::move(final_params);
        lambdas[idx].captures = std::move(captures);
        lambdas[idx].body = std::move(body);
        return idx;
    }

    /// A lambda passed to an array higher-order method. Param types come
    /// from the context (the array's element type etc.); the return type is
    /// checked against `expect_ret` (or inferred when None).
    std::pair<size_t, CTy> check_callback_lambda(const ast::Lambda& l, std::vector<CTy> param_tys,
                                                 std::optional<CTy> expect_ret, LambdaAbi abi) {
        if (l.params.size() != param_tys.size()) {
            diag("this callback takes " + numstr(param_tys.size()) + " parameter(s), got " +
                     numstr(l.params.size()),
                 l.span);
        }
        size_t idx = lambdas.size();
        CLambda cl;
        cl.abi = abi;
        cl.is_executor = false;
        cl.is_async = l.is_async;
        lambdas.push_back(std::move(cl));
        enter_lambda_scope(idx);
        // callbacks may return a value
        if (!lambda_ret_ok.empty()) lambda_ret_ok.back() = true;
        std::vector<std::pair<size_t, CTy>> params;
        size_t n = std::min(l.params.size(), param_tys.size());
        for (size_t i = 0; i < n; ++i) {
            const auto& p = l.params[i];
            const CTy& ty = param_tys[i];
            if (p.ann) {
                CTy at = ty_of(*p.ann);
                coerce(ty, at, p.span);
            }
            size_t id = declare(p.name, ty, true, p.span);
            params.emplace_back(id, ty);
        }
        ret_ty.reset();
        saw_float_ret = false;
        ret_float_bits = 0;
        std::optional<CTy> saved_target = std::move(ret_literal_target);
        ret_literal_target = expect_ret;
        bool saved_async = is_async_ctx;
        is_async_ctx = l.is_async;
        std::vector<CStmt> body = check_stmts(l.body);
        is_async_ctx = saved_async;
        ret_literal_target = std::move(saved_target);
        CTy ret = apply_float_ret(ret_ty ? *ret_ty : cty_void());
        if (expect_ret) {
            coerce(ret, *expect_ret, l.span);
            ret = *expect_ret;
        }
        auto captures = exit_lambda_scope();
        lambdas[idx].params = std::move(params);
        lambdas[idx].captures = std::move(captures);
        lambdas[idx].body = std::move(body);
        lambdas[idx].ret = ret;
        return {idx, ret};
    }

    /// `{ field: expr, ... }`: with `target` (a declared class id from an
    /// annotation) the literal must match that class field-for-field; without
    /// one, a ghost class is created, named `Class_{var}_{NNNN}` (or
    /// `Class_anon_NNNN` when the literal is not a direct `let` initializer).
    CExpr check_object_lit(const std::vector<ast::EObjectField>& fields, Span span,
                           std::optional<size_t> target, const std::string* var_name) {
        if (fields.empty()) {
            diag("cannot infer the shape of an empty object literal (use `new ClassName()` or "
                 "annotate a class)",
                 span);
            return cbool(false);
        }
        // check field values + duplicate field names
        std::vector<std::pair<std::string, CExpr>> cs;
        std::unordered_set<std::string> seen;
        for (const auto& f : fields) {
            if (!seen.insert(f.name).second) {
                diag("duplicate field '" + f.name + "' in object literal", span);
                continue;
            }
            CExpr c = check_expr(*f.value, false);
            CTy t = expr_ty(c);
            if (t.tag == CTy::Tag::Void || t.tag == CTy::Tag::Promise || t.tag == CTy::Tag::Resolver) {
                diag("object fields cannot be " + t.name(), span);
                continue;
            }
            cs.emplace_back(f.name, std::move(c));
        }
        size_t class_id = 0;
        if (target) {
            // structural match: every declared field must be provided with a
            // compatible value, and no extra fields are allowed
            size_t id = *target;
            if (id >= classes.size()) return cbool(false);
            std::vector<std::pair<std::string, CTy>> def_fields = classes[id].fields;
            std::string def_name = classes[id].name;
            for (const auto& [fname, fty] : def_fields) {
                CExpr* c = nullptr;
                for (auto& [n, ce2] : cs)
                    if (n == fname) {
                        c = &ce2;
                        break;
                    }
                if (c) {
                    adapt_literal(*c, fty);
                    coerce(expr_ty(*c), fty, span);
                } else {
                    diag("missing field '" + fname + "' for class '" + def_name + "'", span);
                }
            }
            for (const auto& [n, _] : cs) {
                (void)_;
                bool has = false;
                for (const auto& [dn, _2] : def_fields)
                    if (dn == n) {
                        has = true;
                        break;
                    }
                if (!has) diag("class '" + def_name + "' has no field '" + n + "'", span);
            }
            class_id = id;
        } else {
            std::string base = var_name ? *var_name : "anon";
            std::string name = "Class_" + base + "_" + pad4(ghost_count + 1);
            ghost_count += 1;
            std::vector<std::pair<std::string, CTy>> fdefs;
            fdefs.reserve(cs.size());
            for (const auto& [n, c] : cs) fdefs.emplace_back(n, expr_ty(c));
            class_id = classes.size();
            ClassDef d;
            d.name = name;
            d.fields = std::move(fdefs);
            d.is_ghost = true;
            classes.push_back(std::move(d));
        }
        // build the initializers in class-field order (codegen writes by index)
        std::vector<std::pair<size_t, CExpr>> inits;
        const auto& def_fields2 = classes[class_id].fields;
        for (size_t idx = 0; idx < def_fields2.size(); ++idx) {
            const std::string& fname = def_fields2[idx].first;
            for (const auto& [n, c] : cs) {
                if (n == fname) {
                    inits.emplace_back(idx, c);
                    break;
                }
            }
        }
        CObjectLit o;
        o.class_id = class_id;
        o.inits = std::move(inits);
        return ce(std::move(o));
    }

    /// `[e1, e2, ...]`: infer the element type, or use `expected` (from a
    /// variable annotation) for empty literals and as a coercion target.
    CExpr check_array_lit(const std::vector<ast::ExprPtr>& elems, Span span,
                          std::optional<CTy> expected) {
        // when an element type is declared, elements are coerced to it
        // (e.g. ghost/declared objects flowing into an Array<Interface>)
        if (expected) {
            std::vector<CExpr> cs;
            for (const auto& el : elems) {
                CExpr c = check_expr(*el, false);
                CTy t = expr_ty(c);
                if (t.tag != CTy::Tag::Void) {
                    adapt_literal(c, *expected);
                    coerce(expr_ty(c), *expected, el->span);
                }
                cs.push_back(std::move(c));
            }
            CArrayLit a;
            a.elems = std::move(cs);
            a.elem_ty = *expected;
            return ce(std::move(a));
        }
        std::vector<CExpr> cs;
        for (const auto& el : elems) cs.push_back(check_expr(*el, false));
        std::optional<CTy> anchor;
        for (const auto& c : cs) {
            if (is_num_lit(c)) continue;
            CTy t = expr_ty(c);
            if (is_numeric(t)) {
                anchor = t;
                break;
            }
        }
        if (anchor) {
            for (auto& c : cs)
                if (is_num_lit(c)) adapt_literal(c, *anchor);
        }
        std::optional<CTy> ty;
        bool saw_float = false;
        for (const auto& c : cs) {
            CTy t = expr_ty(c);
            if (t.tag == CTy::Tag::Float) saw_float = true;
            if (t.tag == CTy::Tag::Void || t.tag == CTy::Tag::Promise || t.tag == CTy::Tag::Resolver) {
                diag("arrays cannot contain " + t.name(), span);
                continue;
            }
            if (!ty) {
                ty = t;
            } else if (*ty == t) {
                ty = t;
            } else if (ty->tag == CTy::Tag::Int && t.tag == CTy::Tag::Float) {
                ty = t;
            } else if (ty->tag == CTy::Tag::Float && t.tag == CTy::Tag::Int) {
                // keep the float
            } else if (ty->tag == CTy::Tag::Float && t.tag == CTy::Tag::Float) {
                ty = cty_float(64);
            } else {
                diag("inconsistent array element types: " + ty->name() + " and " + t.name(), span);
                // keep prev
            }
        }
        CTy elem_ty;
        if (ty && ty->tag == CTy::Tag::Int && saw_float) {
            elem_ty = cty_float(64);
        } else if (ty) {
            elem_ty = *ty;
        } else if (expected) {
            elem_ty = *expected;
        } else {
            diag("cannot infer the element type of an empty array literal (annotate the variable, "
                 "e.g. `let a: Array<Int32> = []`)",
                 span);
            elem_ty = cty_int();
        }
        CArrayLit a;
        a.elems = std::move(cs);
        a.elem_ty = elem_ty;
        return ce(std::move(a));
    }

    /// `<string>.method(...)` — bootstrap primitives used by the
    /// self-hosted compiler (byte-index based, like the runtime).
    CExpr check_str_method(CExpr s, const std::string& name, const std::vector<ast::ExprPtr>& args,
                           Span span) {
        if (name == "charAt" || name == "charCodeAt") {
            if (args.size() != 1) {
                diag("string." + name + " takes (index)", span);
                return cbool(false);
            }
            CExpr i = check_expr(*args[0], false);
            if (!is_i32(expr_ty(i))) {
                diag("string." + name + " index must be Int32", span);
            }
            CStrMethod m;
            m.op = name == "charAt" ? StrOpKind::CharAt : StrOpKind::CharCodeAt;
            m.obj = exptr(std::move(s));
            m.args.push_back(std::move(i));
            // JS parity: charAt is the 1-char string, charCodeAt the code
            m.ret = name == "charAt" ? cty_str() : cty_int();
            return ce(std::move(m));
        }
        if (name == "substring") {
            if (args.size() != 2) {
                diag("string.substring takes (start, end)", span);
                return cbool(false);
            }
            std::vector<CExpr> cargs;
            for (const auto& a : args) {
                CExpr c = check_expr(*a, false);
                if (!is_i32(expr_ty(c))) {
                    diag("string.substring indices must be Int32", span);
                }
                cargs.push_back(std::move(c));
            }
            CStrMethod m;
            m.op = StrOpKind::Substring;
            m.obj = exptr(std::move(s));
            m.args = std::move(cargs);
            m.ret = cty_str();
            return ce(std::move(m));
        }
        diag("strings have no method '" + name +
                 "' (available: length, charAt, charCodeAt, substring)",
             span);
        return cbool(false);
    }

    /// `<array>.method(...)` — the one supported family of method calls.
    CExpr check_array_method(CExpr arr, CTy elem, const std::string& name,
                             const std::vector<ast::ExprPtr>& args, Span span) {
        auto expect_count = [&](size_t n) {
            if (args.size() != n) {
                diag("array." + name + " takes " + numstr(n) + " argument(s), got " + numstr(args.size()),
                     span);
            }
        };
        if (name == "push" || name == "unshift") {
            expect_count(1);
            if (args.empty()) return cbool(false); // Rust would panic here; bail out instead
            CExpr a = check_expr(*args[0], false);
            adapt_literal(a, elem);
            coerce(expr_ty(a), elem, span);
            CArrayOp ao;
            ao.op = name == "push" ? ArrayOpKind::Push : ArrayOpKind::Unshift;
            ao.arr = exptr(std::move(arr));
            ao.args.push_back(std::move(a));
            ao.ret = cty_int();
            return ce(std::move(ao));
        }
        if (name == "pop" || name == "shift") {
            expect_count(0);
            CArrayOp ao;
            ao.op = name == "pop" ? ArrayOpKind::Pop : ArrayOpKind::Shift;
            ao.arr = exptr(std::move(arr));
            ao.ret = elem;
            return ce(std::move(ao));
        }
        if (name == "splice" || name == "slice") {
            expect_count(2);
            std::vector<CExpr> cargs;
            for (const auto& a : args) {
                CExpr c = check_expr(*a, false);
                if (!is_i32(expr_ty(c))) {
                    diag("array." + name + " indices must be Int32", span);
                }
                cargs.push_back(std::move(c));
            }
            CArrayOp ao;
            ao.op = name == "splice" ? ArrayOpKind::Splice : ArrayOpKind::Slice;
            ao.arr = exptr(std::move(arr));
            ao.args = std::move(cargs);
            ao.ret = cty_array(elem);
            return ce(std::move(ao));
        }
        if (name == "concat") {
            expect_count(1);
            if (args.empty()) return cbool(false); // Rust would panic here; bail out instead
            CExpr a = check_expr(*args[0], false);
            CTy at = expr_ty(a);
            if (!(at.tag == CTy::Tag::Array && deref(at.inner) == elem)) {
                diag("concat requires Array<" + elem.name() + ">, found " + at.name(), span);
            }
            CArrayOp ao;
            ao.op = ArrayOpKind::Concat;
            ao.arr = exptr(std::move(arr));
            ao.args.push_back(std::move(a));
            ao.ret = cty_array(elem);
            return ce(std::move(ao));
        }
        if (name == "join") {
            expect_count(1);
            if (args.empty()) return cbool(false); // Rust would panic here; bail out instead
            CExpr a = check_expr(*args[0], false);
            coerce(expr_ty(a), cty_str(), span);
            CArrayOp ao;
            ao.op = ArrayOpKind::Join;
            ao.arr = exptr(std::move(arr));
            ao.args.push_back(std::move(a));
            ao.ret = cty_str();
            return ce(std::move(ao));
        }
        if (name == "reverse" || name == "sort") {
            expect_count(0);
            CArrayOp ao;
            ao.op = name == "reverse" ? ArrayOpKind::Reverse : ArrayOpKind::Sort;
            ao.arr = exptr(std::move(arr));
            ao.ret = cty_array(elem);
            return ce(std::move(ao));
        }
        if (name == "length") {
            expect_count(0);
            CArrayOp ao;
            ao.op = ArrayOpKind::Len;
            ao.arr = exptr(std::move(arr));
            ao.ret = cty_int();
            return ce(std::move(ao));
        }
        if (name == "forEach") {
            expect_count(1);
            if (args.empty()) return cbool(false); // Rust would panic here; bail out instead
            auto* lam = std::get_if<ast::ELambda>(&args[0]->v);
            if (!lam) {
                diag("forEach takes a callback lambda: (item) => { ... }", span);
                return cbool(false);
            }
            size_t idx = check_callback_lambda(*lam->lambda, {elem}, std::nullopt, LambdaAbi::ForEach).first;
            CArrayHof h;
            h.op = HofKind::ForEach;
            h.arr = exptr(std::move(arr));
            h.cb = exptr(ce(CLambdaVal{idx, cty_void()}));
            h.ret = cty_void();
            return ce(std::move(h));
        }
        if (name == "map") {
            expect_count(1);
            if (args.empty()) return cbool(false); // Rust would panic here; bail out instead
            auto* lam = std::get_if<ast::ELambda>(&args[0]->v);
            if (!lam) {
                diag("map takes a callback lambda: (item) => ...", span);
                return cbool(false);
            }
            auto mr = check_callback_lambda(*lam->lambda, {elem}, std::nullopt, LambdaAbi::MapFilter);
            CTy uret = mr.second;
            CArrayHof h;
            h.op = HofKind::Map;
            h.arr = exptr(std::move(arr));
            h.cb = exptr(ce(CLambdaVal{mr.first, uret}));
            h.ret = cty_array(uret);
            return ce(std::move(h));
        }
        if (name == "filter") {
            expect_count(1);
            if (args.empty()) return cbool(false); // Rust would panic here; bail out instead
            auto* lam = std::get_if<ast::ELambda>(&args[0]->v);
            if (!lam) {
                diag("filter takes a callback lambda: (item) => bool", span);
                return cbool(false);
            }
            auto fr = check_callback_lambda(*lam->lambda, {elem}, cty_bool(), LambdaAbi::MapFilter);
            CArrayHof h;
            h.op = HofKind::Filter;
            h.arr = exptr(std::move(arr));
            h.cb = exptr(ce(CLambdaVal{fr.first, cty_bool()}));
            h.ret = cty_array(elem);
            return ce(std::move(h));
        }
        if (name == "reduce") {
            expect_count(2);
            if (args.size() < 2) return cbool(false); // Rust would panic here; bail out instead
            CExpr init = check_expr(*args[1], false);
            CTy uty = expr_ty(init);
            if (uty.tag == CTy::Tag::Void || uty.tag == CTy::Tag::Promise ||
                uty.tag == CTy::Tag::Resolver) {
                diag("reduce initial value must be a plain value", span);
                return cbool(false);
            }
            auto* lam = std::get_if<ast::ELambda>(&args[0]->v);
            if (!lam) {
                diag("reduce takes a callback lambda: (acc, item) => ... and an initial value", span);
                return cbool(false);
            }
            size_t idx =
                check_callback_lambda(*lam->lambda, {uty, elem}, uty, LambdaAbi::Reduce).first;
            CArrayHof h;
            h.op = HofKind::Reduce;
            h.arr = exptr(std::move(arr));
            h.cb = exptr(ce(CLambdaVal{idx, uty}));
            h.extra = exptr(std::move(init));
            h.ret = uty;
            return ce(std::move(h));
        }
        diag("'" + name +
                 "' is not supported on arrays (available: push, pop, shift, unshift, splice, slice, "
                 "concat, join, reverse, sort, length, forEach, map, filter, reduce)",
             span);
        return cbool(false);
    }

    /// Does this expression contain a `super(...)` call? (also descends
    /// into nested calls, conditionals and lambdas)
    bool expr_calls_super(const ast::Expr& e) const {
        if (auto* c = std::get_if<ast::ECall>(&e.v)) {
            if (auto* id = std::get_if<ast::EIdent>(&c->callee->v); id && id->name == "super")
                return true;
            if (expr_calls_super(*c->callee)) return true;
            for (const auto& a : c->args)
                if (expr_calls_super(*a)) return true;
            return false;
        }
        if (auto* m = std::get_if<ast::EMember>(&e.v)) return expr_calls_super(*m->obj);
        if (auto* u = std::get_if<ast::EUnary>(&e.v)) return expr_calls_super(*u->operand);
        if (auto* b = std::get_if<ast::EBinary>(&e.v))
            return expr_calls_super(*b->lhs) || expr_calls_super(*b->rhs);
        if (auto* g = std::get_if<ast::EIndex>(&e.v))
            return expr_calls_super(*g->obj) || expr_calls_super(*g->index);
        return false;
    }

    bool stmt_calls_super(const ast::Stmt& s) const {
        if (auto* x = std::get_if<ast::SExpr>(&s.v)) return expr_calls_super(*x->expr);
        if (auto* x = std::get_if<ast::SLet>(&s.v))
            return static_cast<bool>(x->init) && expr_calls_super(*x->init);
        if (auto* x = std::get_if<ast::SAssign>(&s.v)) return expr_calls_super(*x->value);
        if (auto* x = std::get_if<ast::SMemberAssign>(&s.v)) return expr_calls_super(*x->value);
        if (auto* x = std::get_if<ast::SIndexAssign>(&s.v)) return expr_calls_super(*x->value);
        if (auto* x = std::get_if<ast::SIf>(&s.v)) {
            if (expr_calls_super(*x->cond)) return true;
            for (const auto& st : x->then_body)
                if (stmt_calls_super(st)) return true;
            if (x->else_body)
                for (const auto& st : *x->else_body)
                    if (stmt_calls_super(st)) return true;
            return false;
        }
        if (auto* x = std::get_if<ast::SWhile>(&s.v)) {
            if (expr_calls_super(*x->cond)) return true;
            for (const auto& st : x->body)
                if (stmt_calls_super(st)) return true;
            return false;
        }
        if (auto* x = std::get_if<ast::SFor>(&s.v)) {
            for (const auto& st : x->body)
                if (stmt_calls_super(st)) return true;
            return false;
        }
        return false;
    }

    /// Validate every `class D implements I1, I2` clause: the interfaces
    /// must exist, their fields must be present with matching types, and
    /// their methods must be implemented (inherited implementations count).
    void check_implements(const ast::Program& program) {
        for (const auto& cl : program.classes) {
            if (cl.interfaces.empty()) continue;
            if (generic_classes.count(cl.name)) continue; // diagnosed during lowering
            auto cid_it = class_by_name.find(cl.name);
            if (cid_it == class_by_name.end() || cid_it->second >= classes.size()) continue;
            size_t id = cid_it->second;
            for (const auto& iname : cl.interfaces) {
                auto it = interfaces.find(iname);
                if (it == interfaces.end()) {
                    diag("interface '" + iname + "' is not declared", cl.span);
                    continue;
                }
                ObjectTy o;
                o.id = id;
                o.name = classes[id].name;
                iface_conform(o, iname, cl.span);
            }
        }
        // concrete classes must implement every abstract method inherited
        // from their base chain (TypeScript: "non-abstract class does not
        // implement inherited abstract member")
        for (size_t id = 0; id < classes.size(); ++id) {
            const ClassDef& def = classes[id];
            if (def.is_ghost || def.is_abstract) continue;
            for (size_t anc = id;;) {
                const std::string pfx = classes[anc].name + "__";
                for (const auto& [key, asig] : abstract_sigs) {
                    if (key.size() <= pfx.size() ||
                        key.compare(0, pfx.size(), pfx) != 0)
                        continue;
                    std::string rest = key.substr(pfx.size());
                    if (rest == "new" || rest.rfind("static__", 0) == 0) continue;
                    const char* suffix = "";
                    std::string mname = rest;
                    if (rest.rfind("get_", 0) == 0) {
                        suffix = "get_";
                        mname = rest.substr(4);
                    } else if (rest.rfind("set_", 0) == 0) {
                        suffix = "set_";
                        mname = rest.substr(4);
                    }
                    auto impl = find_member(id, mname, suffix);
                    if (!impl) {
                        diag("class '" + def.name + "' does not implement abstract method '" +
                                 mname + "' of '" + classes[anc].name + "'",
                             def_span(id));
                        continue;
                    }
                    // signature must match the abstract declaration
                    // (impl sigs carry `this` at index 0, abstract sigs don't)
                    const FnSig& is_ = sigs[impl->second];
                    bool mismatch = (is_.params.size() - 1) != asig.params.size() ||
                                    is_.ret.has_value() != asig.ret.has_value() ||
                                    (is_.ret.has_value() && asig.ret.has_value() &&
                                     !(*is_.ret == *asig.ret));
                    if (!mismatch) {
                        // param check: compare pairwise (impl has `this` at 0)
                        for (size_t pi = 0; pi < asig.params.size(); ++pi)
                            if (!(is_.params[pi + 1] == asig.params[pi])) mismatch = true;
                    }
                    if (mismatch) {
                        diag("method '" + mname + "' of class '" + def.name +
                                 "' does not match the abstract declaration in '" +
                                 classes[anc].name + "'",
                             def_span(id));
                    }
                }
                if (!classes[anc].base.has_value()) break;
                anc = *classes[anc].base;
            }
        }
    }

    /// After every method body has been checked (so inferred return types
    /// are known), record the dispatch tables:
    ///   - class vtables: key `Class\nmember` (member = `name`, `get_x`,
    ///     `set_x`) → every concrete implementation across the class's
    ///     subtree, so `CIfaceCall` on an abstract or overridden receiver
    ///     can pick the right method at runtime.
    ///   - interfaces: key `iface\nmethod` → implementations (inherited
    ///     methods count).
    void publish_iface_impls(Checked& out) const {
        for (size_t id = 0; id < classes.size(); ++id) {
            const ClassDef& def = classes[id];
            if (def.is_ghost || def.is_abstract) continue;
            // collect member keys declared anywhere in the base chain
            // (own sigs and abstract signatures); each concrete class gets
            // a vtable entry in itself and every ancestor, pointing at the
            // nearest implementation up its chain
            std::set<std::string> keys;
            for (size_t c = id;;) {
                const std::string pfx = classes[c].name + "__";
                for (const auto& [fname, sig] : sigs) {
                    (void)sig;
                    if (fname.size() > pfx.size() &&
                        fname.compare(0, pfx.size(), pfx) == 0) {
                        std::string rest = fname.substr(pfx.size());
                        if (rest == "new" || rest.rfind("static__", 0) == 0)
                            continue; // constructors and statics are not virtual
                        keys.insert(std::move(rest));
                    }
                }
                for (const auto& [k, sig] : abstract_sigs) {
                    (void)sig;
                    if (k.size() > pfx.size() && k.compare(0, pfx.size(), pfx) == 0)
                        keys.insert(k.substr(pfx.size()));
                }
                if (!classes[c].base.has_value()) break;
                c = *classes[c].base;
            }
            for (const auto& key : keys) {
                const char* suffix = "";
                std::string mname = key;
                if (key.rfind("get_", 0) == 0) {
                    suffix = "get_";
                    mname = key.substr(4);
                } else if (key.rfind("set_", 0) == 0) {
                    suffix = "set_";
                    mname = key.substr(4);
                }
                auto impl = find_member(id, mname, suffix);
                if (!impl) continue; // no concrete implementation in the chain
                for (size_t a = id;;) {
                    out.iface_impls[classes[a].name + "\n" + key].emplace_back(id, impl->second);
                    if (!classes[a].base.has_value()) break;
                    a = *classes[a].base;
                }
            }
        }
        for (const auto& [iname, methods] : iface_methods) {
            for (const auto& [mname, msig] : methods) {
                std::vector<std::pair<size_t, std::string>> impls;
                for (size_t id = 0; id < classes.size(); ++id) {
                    const ClassDef& def = classes[id];
                    if (def.is_ghost || def.is_abstract || def.method_is_private(mname)) continue;
                    // inherited implementations count: walk the base chain
                    auto impl = find_member(id, mname, "");
                    if (!impl) continue;
                    if (!method_sig_eq(sigs.at(impl->second), msig)) continue;
                    impls.emplace_back(id, impl->second);
                }
                out.iface_impls[iname + "\n" + mname] = std::move(impls);
            }
        }
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

/// Lower class constructors/methods/accessors/statics into top-level
/// functions before checking:
///   `constructor(params)`      →  `Name__new(this: Name, params)`
///   `method(args)`             →  `Name__method(this: Name, args)`
///   `get x()`                  →  `Name__get_x(this: Name)`
///   `set x(v)`                 →  `Name__set_x(this: Name, v)`
///   `static method(args)`      →  `Name__static__method(args)`
/// Abstract methods have no body and are not lowered (subclasses provide
/// the implementation; calls dispatch on the runtime class id). The
/// declarations stay on the ClassDecl so `register_classes` can build the
/// method/visibility tables.
std::vector<Diag> lower_classes(ast::Program& program) {
    std::vector<Diag> diags;
    std::vector<ast::FnDecl> hoisted;
    for (auto& cl : program.classes) {
        if (!cl.type_params.empty()) {
            // generic classes are instantiated per type argument (each `new
            // Box<T>()` monomorphizes a concrete class), so the generic
            // declaration itself is never lowered; methods are monomorphized
            // alongside the fields in `mono_class`. Constructors and statics
            // have no per-instantiation story yet.
            if (cl.ctor || std::any_of(cl.fields.begin(), cl.fields.end(),
                                       [](const ast::ClassField& f) { return f.is_static; }) ||
                std::any_of(cl.methods.begin(), cl.methods.end(),
                            [](const ast::FnDecl& m) { return m.is_static; })) {
                diags.push_back(
                    Diag{"generic classes do not support constructors or statics yet", cl.span});
            }
            continue;
        }
        ast::TyAnn self_ann;
        self_ann.name = cl.name;
        self_ann.span = cl.span;
        auto make_this = [&]() {
            ast::Param p;
            p.name = "this";
            p.ann = self_ann;
            p.span = cl.span;
            return p;
        };
        if (cl.ctor) {
            ast::FnDecl f = *cl.ctor;
            f.name = cl.name + "__new";
            f.params.insert(f.params.begin(), make_this());
            f.is_async = false;
            hoisted.push_back(std::move(f));
        }
        for (auto& m : cl.methods) {
            if (m.is_abstract) continue; // no body; nothing to lower
            ast::FnDecl f = m;
            switch (m.kind) {
                case ast::MemberKind::Getter: f.name = cl.name + "__get_" + m.name; break;
                case ast::MemberKind::Setter: f.name = cl.name + "__set_" + m.name; break;
                case ast::MemberKind::Method:
                    f.name = m.is_static ? cl.name + "__static__" + m.name
                                         : cl.name + "__" + m.name;
                    break;
            }
            if (!m.is_static) {
                f.params.insert(f.params.begin(), make_this());
            }
            f.is_async = false;
            hoisted.push_back(std::move(f));
        }
    }
    // hoisted methods come first so their (inferred) return types are known
    // by the time `main` and later-declared functions are checked
    program.fns.insert(program.fns.begin(), std::make_move_iterator(hoisted.begin()),
                       std::make_move_iterator(hoisted.end()));
    return diags;
}

Checked check(const ast::Program& program) {
    std::vector<Diag> diags;
    Checked checked = check_with_diags(program, diags);
    if (diags.empty()) return checked;
    throw CheckDiags(std::move(diags));
}

/// Like `check`, but always returns the (possibly incomplete) `Checked`
/// alongside diagnostics. Used by the language server, which still wants
/// class/variable info from programs that contain errors.
Checked check_with_diags(const ast::Program& program_in, std::vector<Diag>& diags_out) {
    // mutable copy: class methods/constructors are lowered into functions
    ast::Program program = program_in;
    {
        std::vector<Diag> lower_diags = lower_classes(program);
        diags_out.insert(diags_out.end(), std::make_move_iterator(lower_diags.begin()),
                         std::make_move_iterator(lower_diags.end()));
    }
    Checker c;
    for (const auto& [n, _] : program.uses) {
        (void)_;
        c.uses.push_back(n);
    }
    for (const auto& [alias, prefix] : program.use_prefixes) {
        c.use_prefix[alias] = prefix;
    }
    for (const auto& n : program.exported_fns) c.exported_fns.insert(n);
    for (const auto& n : program.module_classes) c.module_classes.insert(n);
    for (const auto& n : program.exported_classes) c.exported_classes.insert(n);

    // --- pass 0: register classes + interfaces (hoisting) ---
    c.register_classes(program);
    c.register_interfaces(program);
    c.register_builtin_modules();

    // --- pass 0.5: unknown `use` targets (registry packages were already
    // merged into the program by the pipeline; net/http are built-in) ---
    // (missing/non-std modules are diagnosed by the pipeline's resolve_uses,
    // which also merges registry packages into the program)

    // --- pass 1: register function signatures (hoisting) ---
    std::unordered_set<std::string> seen;
    for (const auto& f : program.fns) {
        if (!is_camel_case(f.name) && f.name != "main" && !is_namespaced_fn(f.name)) {
            c.diag("function '" + f.name + "' must be camelCase", f.span);
        }
        if (!seen.insert(f.name).second) {
            c.diag("duplicate function '" + f.name + "'", f.span);
            continue;
        }
        if (!f.type_params.empty()) {
            // generic function: stashed and monomorphized per call site
            for (const auto& tp : f.type_params) {
                if (!is_pascal_case(tp)) {
                    c.diag("type parameter '" + tp + "' must be PascalCase (e.g. T)", f.span);
                }
            }
            c.generic_fns[f.name] = f;
            continue;
        }
        std::vector<CTy> params;
        for (const auto& p : f.params) {
            if (p.ann) {
                params.push_back(c.ty_of(*p.ann));
            } else {
                c.diag("parameter '" + p.name + "' of function '" + f.name +
                           "' needs a type annotation (strict mode)",
                       p.span);
                params.push_back(cty_void());
            }
        }
        std::optional<CTy> ret;
        if (f.ret_ann) ret = c.ty_of(*f.ret_ann);
        if (ret && ret->tag == CTy::Tag::Resolver) {
            c.diag("functions cannot return a Resolver", f.span);
        }
        FnSig sig;
        sig.params = std::move(params);
        sig.ret = std::move(ret);
        sig.is_async = f.is_async;
        c.sigs[f.name] = std::move(sig);
    }

    // `implements` conformance needs the lowered method signatures from
    // pass 1, so it runs here (after hoisting).
    c.check_implements(program);

    // --- pass 2: top-level *declarations* (first, so functions can reference
    // top-level variables; codegen stores them as module globals). Other
    // top-level statements are deferred until after fn bodies, because they
    // may call functions whose (inferred) return type is only known then. ---
    std::optional<std::vector<CStmt>> toplevel;
    std::optional<std::vector<std::optional<CStmt>>> toplevel_slots;
    if (!program.stmts.empty()) {
        c.is_async_ctx = false;
        c.in_toplevel = true;
        c.ret_ty.reset();
        c.saw_float_ret = false;
        c.ret_float_bits = 0;
        c.scopes.emplace_back();
        std::vector<std::optional<CStmt>> slots;
        slots.reserve(program.stmts.size());
        for (const auto& s : program.stmts) {
            if (std::holds_alternative<ast::SLet>(s.v) && !c.stmt_calls_unresolved(s)) {
                slots.push_back(c.check_stmt(s));
            } else {
                slots.push_back(std::nullopt);
            }
        }
        toplevel_slots = std::move(slots);
        c.in_toplevel = false;
        // the scope stays on the stack until fn bodies are checked (pass 3)
    }

    // --- pass 3: check fn bodies (top-level variables are in scope) ---
    std::vector<CFn> fns;
    bool has_async_main = false;
    bool has_sync_main = false;
    for (const auto& f : program.fns) {
        c.cur_fn = f.name;
        if (f.name == "main") {
            if (f.is_async) {
                if (has_sync_main) {
                    c.diag("cannot define both an async and a sync 'main'", f.span);
                }
                has_async_main = true;
            } else {
                if (has_async_main) {
                    c.diag("cannot define both an async and a sync 'main'", f.span);
                }
                has_sync_main = true;
            }
        }
        if (!f.type_params.empty()) {
            continue; // generic: checked per monomorphized instantiation
        }
        // a derived-class constructor must call super(...) when the base
        // chain declares a constructor (TypeScript rule)
        if (f.name.size() > 5 && f.name.compare(f.name.size() - 5, 5, "__new") == 0) {
            auto cid_opt = c.cur_class_id();
            size_t cid = cid_opt ? *cid_opt : SIZE_MAX;
            if (cid < c.classes.size() && c.classes[cid].base.has_value()) {
                bool base_has_new = false;
                for (size_t b = *c.classes[cid].base;;) {
                    if (c.sigs.count(c.classes[b].name + "__new")) {
                        base_has_new = true;
                        break;
                    }
                    if (!c.classes[b].base.has_value()) break;
                    b = *c.classes[b].base;
                }
                if (base_has_new) {
                    bool calls_super = false;
                    for (const auto& st : f.body)
                        if (c.stmt_calls_super(st)) {
                            calls_super = true;
                            break;
                        }
                    if (!calls_super) {
                        c.diag("constructor of '" + c.classes[cid].name +
                                   "' must call super(...)",
                               f.span);
                    }
                }
            }
        }
        CFn cf = c.check_fn(f);
        // publish the (possibly inferred) return type for later functions
        if (c.last_fn_ret) {
            auto it = c.sigs.find(f.name);
            if (it != c.sigs.end()) it->second.ret = c.last_fn_ret;
        }
        fns.push_back(std::move(cf));
    }
    if (toplevel_slots) {
        // --- pass 4: remaining top-level statements (all fn return types are
        // known now) ---
        c.is_async_ctx = false;
        c.in_toplevel = true;
        c.ret_ty.reset();
        c.saw_float_ret = false;
        c.ret_float_bits = 0;
        std::vector<std::optional<CStmt>> slots = std::move(*toplevel_slots);
        for (size_t i = 0; i < program.stmts.size(); ++i) {
            if (!slots[i]) slots[i] = c.check_stmt(program.stmts[i]);
        }
        std::vector<CStmt> tl;
        for (auto& s : slots)
            if (s) tl.push_back(std::move(*s));
        toplevel = std::move(tl);
        c.in_toplevel = false;
        c.scopes.pop_back();
    }

    fns.insert(fns.end(), std::make_move_iterator(c.mono_fns.begin()),
               std::make_move_iterator(c.mono_fns.end()));
    // did the program call `main()` explicitly at the top level? If so, the
    // runtime's automatic entry-point invocation must be suppressed or main
    // would execute twice
    bool explicit_main = false;
    for (const auto& s : program.stmts) {
        auto* se = std::get_if<ast::SExpr>(&s.v);
        if (!se) continue;
        auto* call = std::get_if<ast::ECall>(&se->expr->v);
        if (!call) continue;
        auto* id = std::get_if<ast::EIdent>(&call->callee->v);
        if (id && id->name == "main") {
            explicit_main = true;
            break;
        }
    }
    Checked checked;
    c.publish_iface_impls(checked);
    checked.fns = std::move(fns);
    checked.lambdas = std::move(c.lambdas);
    checked.toplevel = std::move(toplevel);
    checked.explicit_main = explicit_main;
    checked.var_types = std::move(c.var_types);
    checked.var_names = std::move(c.var_names);
    checked.classes = std::move(c.classes);
    checked.has_async_main = has_async_main;
    checked.has_sync_main = has_sync_main;
    diags_out = std::move(c.diags);
    return checked;
}

} // namespace marina::ir