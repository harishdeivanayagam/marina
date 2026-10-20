// AST for the marina language.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <marina/lexer.hpp>

namespace marina::ast {

using lex::Span;
using lex::Token;

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

enum class BinOp { Add, Sub, Mul, Div, Mod, Lt, Gt, Le, Ge, Eq, Neq, And, Or };
enum class UnOp { Neg, Not };

// Template parts (AST level): literal text or an already-parsed expression.
struct TStr {
    std::string value;
};
struct TExpr {
    ExprPtr expr;
};
using TPart = std::variant<TStr, TExpr>;

struct Lambda {
    std::vector<struct Param> params;
    std::vector<struct Stmt> body;
    // `async (params) => { ... }` — may await; returns a Promise
    bool is_async = false;
    Span span;
};

// --- expression payloads ---------------------------------------------------

struct EInt {
    int32_t value = 0;
};
struct EFloat {
    double value = 0.0;
};
struct EBool {
    bool value = false;
};
struct EStr {
    std::string value;
};
struct ETemplate {
    std::vector<TPart> parts;
};
// `tag\`...\`` — tagged template (only `sql.stmt` is supported; the checked
// expression may only appear as the sole argument of a `db.execute` /
// `db.query` call).
struct ETagged {
    ExprPtr tag;
    std::vector<TPart> parts;
};
struct EIdent {
    std::string name;
};
struct EUnary {
    UnOp op{};
    ExprPtr operand;
};
struct EBinary {
    BinOp op{};
    ExprPtr lhs;
    ExprPtr rhs;
};
struct ECall {
    ExprPtr callee;
    std::vector<ExprPtr> args;
};
// Call with explicit type arguments: `pick<Int32>(1, 2, true)`.
struct ECallGen {
    ExprPtr callee;
    std::vector<struct TyAnn> type_args;
    std::vector<ExprPtr> args;
};
struct EMember {
    ExprPtr obj;
    std::string name;
};
// `obj[idx]`
struct EIndex {
    ExprPtr obj;
    ExprPtr index;
};
// `[e1, e2, ...]`
struct EArrayLit {
    std::vector<ExprPtr> elems;
};
struct EObjectField {
    std::string name;
    ExprPtr value;
};
// `{ field: expr, ... }` — creates an instance of an inferred ("ghost")
// class, or of an annotated class when a target exists.
struct EObjectLit {
    std::vector<EObjectField> fields;
};
// `new a.b.Class(args...)` — module-qualified constructor call
struct ENewCtor {
    std::vector<std::string> path;
    std::vector<ExprPtr> args;
};
// `new SomeClass()` / `new Generic<Int32>()` — instantiates a declared class
// (classes take no constructor arguments).
struct ENewClass {
    std::string name;
    // Explicit type arguments: `new Box<Int32>()`.
    std::vector<struct TyAnn> type_args;
};
struct EAwait {
    ExprPtr operand;
};
struct ENewPromise {
    ExprPtr executor;
};
struct ELambda {
    std::shared_ptr<Lambda> lambda;
};

struct TyAnn {
    std::string name;
    // Type arguments: `Array<Int32>`, `Promise<string>`, `Box<Int32>`.
    std::vector<TyAnn> args;
    Span span;
};

struct Param {
    std::string name;
    std::optional<TyAnn> ann;
    Span span;
};

// --- expression node ---------------------------------------------------------

struct Expr {
    Span span;
    std::variant<EInt, EFloat, EBool, EStr, ETemplate, ETagged, EIdent, EUnary,
                 EBinary, ECall, ECallGen, EMember, EIndex, EArrayLit,
                 EObjectLit, ENewClass, ENewCtor, EAwait, ENewPromise, ELambda>
        v;

    Expr() : v(EInt{}) {}
};

// --- statements --------------------------------------------------------------

struct Stmt;

struct SLet {
    std::string name;
    std::optional<TyAnn> ann;
    ExprPtr init;
    bool is_const = false;
    Span span;
};
struct SAssign {
    std::string name;
    ExprPtr value;
    Span span;
};
// `obj.field = value`
struct SMemberAssign {
    std::string obj;
    std::string field;
    ExprPtr value;
    Span span;
};
// `list[i] = value`
struct SIndexAssign {
    std::string name;
    ExprPtr index;
    ExprPtr value;
    Span span;
};
struct SReturn {
    std::optional<ExprPtr> value;
    Span span;
};
struct SIf {
    ExprPtr cond;
    std::vector<Stmt> then_body;
    std::optional<std::vector<Stmt>> else_body;
    Span span;
};
struct SWhile {
    ExprPtr cond;
    std::vector<Stmt> body;
    Span span;
};
struct SFor {
    std::optional<std::shared_ptr<Stmt>> init;
    std::optional<ExprPtr> cond;
    std::optional<std::shared_ptr<Stmt>> update;
    std::vector<Stmt> body;
    Span span;
};
struct SExpr {
    ExprPtr expr;
};

struct Stmt {
    std::variant<SLet, SAssign, SMemberAssign, SIndexAssign, SReturn, SIf,
                 SWhile, SFor, SExpr>
        v;
    Stmt() : v(SLet{}) {}
};

Span stmt_span(const Stmt& s);

// --- declarations --------------------------------------------------------------

struct FnDecl {
    std::string name;
    // Generic type parameters: `function f<T>(...)` declares `["T"]`.
    std::vector<std::string> type_params;
    std::vector<Param> params;
    std::optional<TyAnn> ret_ann;
    std::vector<Stmt> body;
    bool is_async = false;
    Span span;
};

// `class Name<T> { field: T, ... }` — fields are public by default; `private`
// restricts access to the class's own methods/constructor.
struct ClassField {
    std::string name;
    TyAnn ty;
    bool is_private = false;
    Span span;
};

// `class Name { ... }` — fields plus (optionally) a constructor and methods.
// Methods and the constructor are lowered to top-level functions
// (`Name__method` with a leading `this` parameter) before checking.
struct ClassDecl {
    std::string name;
    std::vector<std::string> type_params;
    std::vector<ClassField> fields;
    std::optional<FnDecl> ctor;
    std::vector<FnDecl> methods;
    Span span;
};

// `interface Name { field: Type, ... }` — a structural object shape.
// Values of interface type are any object (ghost or declared class) whose
// fields include the interface fields with matching types.
struct InterfaceDecl {
    std::string name;
    std::vector<std::pair<std::string, TyAnn>> fields;
    Span span;
};

struct Program {
    std::vector<Stmt> stmts;
    std::vector<FnDecl> fns;
    std::vector<ClassDecl> classes;
    std::vector<InterfaceDecl> interfaces;
    // Top-level `use <module>;` declarations: std modules (net, http),
    // local folder modules (`sample_mod/sub`), or wave registry packages.
    std::vector<std::pair<std::string, Span>> uses;
    // For each use alias, the prefix used to mangle the module's functions
    // (`sample_mod/sub` → alias `sub`, prefix `sample_mod__sub`). Aliases
    // without an entry mangle to themselves (std modules, registry packages).
    std::vector<std::pair<std::string, std::string>> use_prefixes;
};

} // namespace marina::ast
