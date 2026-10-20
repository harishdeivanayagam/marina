// Strict type checker for marina.
//
// Rules (v0.1):
//   * Function parameters REQUIRE type annotations (strict mode).
//   * No `any`, no implicit truthiness, no string arithmetic.
//   * Numeric types are Int8/16/32/64, UInt8/16/32/64, Float32 and Float64.
//     Unsuffixed integers default to Int32 (Int64 / UInt64 when the literal
//     does not fit). Unsuffixed floats default to Float64.
//   * Implicit conversions: any integer widens to Float32 or Float64, and
//     Float32 widens to Float64. Integer widths do not convert, except that
//     a numeric literal may be used as any integer or float type it fits in.
//   * `await` is allowed only at statement level:
//       `let x = await e`, `await e`, or `return await e`.
//   * Lambdas are only allowed as arguments to `setTimeout` / `new Promise`.
//
// The checker lowers the AST to a fully-typed IR (`CStmt` / `CExpr`) that the
// code generator consumes 1:1.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <marina/ast.hpp>
#include <marina/lexer.hpp>

namespace marina::ir {

using lex::Span;

// ---------------------------------------------------------------------------
// Checked types
// ---------------------------------------------------------------------------

struct CTy;
using CTyPtr = std::shared_ptr<CTy>;

// Identity + display name of a class type. Equality is by id: every ghost
// class is distinct, even if two shapes happen to be identical.
struct ObjectTy {
    size_t id = 0;
    std::string name;
    bool operator==(const ObjectTy& o) const { return id == o.id; }
};

struct CTy {
    enum class Tag {
        Void,
        Int,
        Float,
        Bool,
        Str,
        Promise,
        Resolver,
        Array,
        // Instance of a class (declared or ghost-inferred); id indexes
        // `Checked::classes`.
        Object,
        // An open TCP stream handle from the `net` std module.
        Stream,
        // An interface type (structural shape). Values are objects whose
        // fields and public methods include the interface's, with matching
        // types. Field access goes through the runtime by field *name*;
        // method calls dispatch on the object's class id.
        Interface,
    };

    Tag tag = Tag::Void;
    // Inner type of Promise<T> / Resolver<T> / Array<T>.
    CTyPtr inner;
    // Payload of Object.
    ObjectTy object;
    // Payload of Interface.
    std::string iface;
    // Int: 8/16/32/64. Float: 32/64. Ignored for every other tag.
    uint8_t bits = 32;
    // Int only: true for UInt8..UInt64.
    bool unsign = false;

    // Display name: "void", "Int32", "Promise<string>", ...
    std::string name() const;

    bool operator==(const CTy& o) const;
    bool operator!=(const CTy& o) const { return !(*this == o); }
};

// Factories.
CTy cty_void();
CTy cty_int(uint8_t bits = 32, bool unsign = false);
CTy cty_float(uint8_t bits = 64);
CTy cty_bool();
CTy cty_str();
CTy cty_stream();
CTy cty_promise(CTy inner);
CTy cty_resolver(CTy inner);
CTy cty_array(CTy elem);
CTy cty_object(size_t id, std::string name);
CTy cty_interface(std::string name);

// A class (declared or ghost). Fields are ordered; codegen addresses them
// by index and registers `name\0field1\0field2\0` metadata with the runtime.
// Declared classes may extend a base class: `fields` is the flattened
// layout (base fields first), `base` is the base class id.
struct ClassDef {
    std::string name;
    std::vector<std::pair<std::string, CTy>> fields;
    bool is_ghost = false;
    // Declared-class extras.
    bool is_abstract = false;
    std::optional<size_t> base;
    // Own (non-inherited) fields with their access flags.
    struct OwnField {
        std::string name;
        bool priv = false;
        bool prot = false;
    };
    std::vector<OwnField> own_fields;
    // Names of `private` fields/methods (accessible only inside the
    // defining class's own methods/constructor).
    std::vector<std::string> private_fields;
    std::vector<std::string> protected_fields;
    std::vector<std::string> private_methods;
    std::vector<std::string> protected_methods;
    // `static` fields (not inherited into the instance layout).
    std::vector<std::pair<std::string, CTy>> static_fields;

    bool field_is_private(const std::string& n) const {
        for (const auto& f : private_fields)
            if (f == n) return true;
        return false;
    }
    bool method_is_private(const std::string& n) const {
        for (const auto& m : private_methods)
            if (m == n) return true;
        return false;
    }
};

struct Diag {
    std::string msg;
    Span span;
};

enum class BinOpC { Add, Sub, Mul, Div, Mod, Lt, Gt, Le, Ge, Eq, Neq, And, Or };
enum class UnOpC { Neg, Not };

struct TPartC {
    std::variant<std::string, std::shared_ptr<struct CExpr>> v;
};

enum class ReturnKindKind { Plain, FlattenPromise };
struct ReturnKind {
    ReturnKindKind kind = ReturnKindKind::Plain;
    // Payload of FlattenPromise: the inner type of the returned promise.
    CTy inner;
};

// ---------------------------------------------------------------------------
// Checked statements
// ---------------------------------------------------------------------------

struct CStmt;

struct CLet {
    size_t id = 0;
    CTy ty;
    std::shared_ptr<struct CExpr> init;
};
struct CAssign {
    size_t id = 0;
    std::shared_ptr<struct CExpr> value;
};
struct CIf {
    std::shared_ptr<struct CExpr> cond;
    std::vector<CStmt> then_body;
    std::optional<std::vector<CStmt>> else_body;
};
struct CWhile {
    std::shared_ptr<struct CExpr> cond;
    std::vector<CStmt> body;
};
// `list[idx] = value`
struct CIndexSet {
    size_t id = 0;
    std::shared_ptr<struct CExpr> idx;
    std::shared_ptr<struct CExpr> value;
    CTy elem;
};
// `obj.field = value` (field addressed by index)
struct CFieldSet {
    size_t id = 0;
    size_t idx = 0;
    std::shared_ptr<struct CExpr> value;
    CTy elem;
};
// Interface-typed field assignment: field resolved by name at runtime.
struct CIfaceSet {
    size_t id = 0;
    std::string field;
    std::shared_ptr<struct CExpr> value;
    CTy elem;
};
struct CFor {
    std::optional<std::shared_ptr<CStmt>> init;
    std::shared_ptr<struct CExpr> cond;
    std::optional<std::shared_ptr<CStmt>> update;
    std::vector<CStmt> body;
};
struct CReturn {
    std::optional<std::shared_ptr<struct CExpr>> value;
    ReturnKind kind;
};
struct CExprStmt {
    std::shared_ptr<struct CExpr> expr;
};
// `Name.staticField = value` — static field write through a global.
struct CStaticSet {
    std::string gname;
    std::shared_ptr<struct CExpr> value;
    CTy elem;
};
// `throw expr;` — the value is a string carrying the error message.
struct CThrow {
    std::shared_ptr<struct CExpr> value;
};
// `try { ... } catch (e) { ... }` — the catch binding, when present, is a
// `string` (the panic / throw message).
struct CTry {
    std::vector<CStmt> try_body;
    std::optional<std::pair<size_t, CTy>> catch_var;
    std::vector<CStmt> catch_body;
};

struct CStmt {
    std::variant<CLet, CAssign, CIf, CWhile, CIndexSet, CFieldSet, CIfaceSet,
                 CStaticSet, CFor, CReturn, CExprStmt, CThrow, CTry>
        v;
    CStmt() : v(CLet{}) {}
};

// ---------------------------------------------------------------------------
// Checked expressions
// ---------------------------------------------------------------------------

// What a JsonValidator field checks: string or number.
enum class FieldKind { Str, Int };
// Chainable JsonValidator operations.
enum class FieldOp { Required, Min, Max };

enum class ArrayOpKind {
    Push,
    Pop,
    Shift,
    Unshift,
    Splice,
    Slice,
    Concat,
    Join,
    Reverse,
    Sort,
    Len,
};

enum class HofKind { ForEach, Map, Filter, Reduce };

enum class StrOpKind { CharAt, Substring };

// How the generated lambda is called by the runtime.
enum class LambdaAbi {
    // `(env)` — promise executors and setTimeout callbacks.
    Plain,
    // `(env, item)` — forEach.
    ForEach,
    // `(env, item, out)` — map / filter.
    MapFilter,
    // `(env, acc, item, out)` — reduce.
    Reduce,
    // `(env, req, res)` — http.listen handlers.
    Listen,
};

struct CExpr;
using CExprPtr = std::shared_ptr<CExpr>;

struct CInt {
    // Magnitude. `neg` means the mathematical value is `-value`
    // (signed types only; `value == 2^63` is Int64::MIN).
    uint64_t value = 0;
    bool neg = false;
    uint8_t bits = 32;
    bool unsign = false;
};
struct CFloat {
    double value = 0.0;
    uint8_t bits = 64;
};
struct CBool {
    bool value = false;
};
struct CStr {
    std::string value;
};
struct CTemplate {
    std::vector<TPartC> parts;
};
// Variable reference with its type (borrowed read).
struct CIdent {
    size_t id = 0;
    CTy ty;
};
struct CUnary {
    UnOpC op{};
    CExprPtr operand;
    CTy ty;
};
struct CBinary {
    BinOpC op{};
    CExprPtr lhs;
    CExprPtr rhs;
    CTy ty;
};
struct CCallFn {
    std::string name;
    std::vector<CExpr> args;
    CTy ret;
    bool is_async = false;
};
struct CCallResolver {
    size_t id = 0;
    CExprPtr arg;
    // The resolver's value type T (for Int -> Float widening).
    CTy inner;
};
// Await a promise; carries the resolved value type.
struct CAwait {
    CExprPtr operand;
    CTy ty;
};
struct CNewPromise {
    size_t lambda = 0;
};
struct CConsoleLog {
    CExprPtr v;
};
struct CSetTimeout {
    size_t lambda = 0;
    CExprPtr ms;
};
// `[e1, e2, ...]` with the inferred element type.
struct CArrayLit {
    std::vector<CExpr> elems;
    CTy elem_ty;
};
// `obj[idx]` read; ty is the element type.
struct CIndex {
    CExprPtr obj;
    CExprPtr idx;
    CTy ty;
};
// Non-callback array operations (push/pop/shift/unshift/splice/slice/
// concat/join/reverse/sort/length). `ret` is the result type.
struct CArrayOp {
    ArrayOpKind op{};
    CExprPtr arr;
    std::vector<CExpr> args;
    CTy ret;
};
// Higher-order array operations (forEach/map/filter/reduce) with a callback
// lambda. `extra` carries reduce's initial value.
struct CArrayHof {
    HofKind op{};
    CExprPtr arr;
    CExprPtr cb;
    std::optional<CExprPtr> extra;
    CTy ret;
};
// A general (callback) lambda value captured at the call site. The lambda's
// params/abi live on the `CLambda` it refers to; here we only carry the index
// and result type.
struct CLambdaVal {
    size_t li = 0;
    CTy ret;
};
// `str.length`
struct CStrLen {
    CExprPtr v;
};
// `str.charAt(i)` / `str.substring(a, b)` — bootstrap primitives for the
// self-hosted compiler (byte-index based, matching the runtime).
struct CStrMethod {
    StrOpKind op{};
    CExprPtr obj;
    std::vector<CExpr> args;
    CTy ret;
};
// `fs.args()` — process arguments as `Array<string>`.
struct CFsArgs {};
// `{ field: expr, ... }` — `class_id` indexes `Checked::classes`;
// `inits` are (field index, checked value) pairs in class-field order.
struct CObjectLit {
    size_t class_id = 0;
    std::vector<std::pair<size_t, CExpr>> inits;
};
// `new ClassName()` — fresh instance, fields default to `undefined`.
struct CNewClass {
    size_t class_id = 0;
};
// `new ClassName(args)` / `new mod.Class(args)` — fresh instance, then run
// the class constructor (`ClassName__new`) on it. `this` is retained for the
// call so the callee's consume leaves the instance owned by the caller.
struct CNewClassCtor {
    size_t class_id = 0;
    std::string fn; // mangled constructor function (`ClassName__new`)
    std::vector<CExpr> args;
};
// `obj.field` read; ty is the field type.
struct CFieldGet {
    CExprPtr obj;
    size_t idx = 0;
    CTy ty;
};

// ---- std modules (net / http) ----
// `net.connect(host, port)` — opens a TCP stream.
struct CNetConnect {
    CExprPtr host;
    CExprPtr port;
};
// `stream.write(data)` — send bytes over a TCP stream.
struct CNetWrite {
    CExprPtr stream;
    CExprPtr data;
};
// `stream.read_all()` — read until EOF into a string.
struct CNetReadAll {
    CExprPtr stream;
};
// `stream.close()`
struct CNetClose {
    CExprPtr stream;
};
// `http.get(url)` / `http.post(url, body)` — HTTP client request that
// returns an `http_Response` object `{ status, body }`.
struct CHttpCall {
    std::string method;
    CExprPtr url;
    std::optional<CExprPtr> body;
};
// `http.listen(port, handler)` — start the HTTP server (never returns).
struct CHttpListen {
    CExprPtr port;
    size_t lambda = 0;
};
// `res.send(value)` / `res.json(value)` — respond on an HTTP server
// connection.
struct CResSend {
    CExprPtr res;
    CExprPtr value;
    bool json = false;
};
// `new http.Server()` — a fresh server object (routes register via
// `server.router(...)`).
struct CHttpNewServer {};
// `server.router({ path: "/x" })` — a router scoped to a path prefix.
struct CHttpRouter {
    CExprPtr server;
    CExprPtr path;
};
// `router.get/post(path, handler)` — register a route handler
// (Listen-ABI lambda).
struct CHttpRouteAdd {
    CExprPtr router;
    std::string method;
    CExprPtr path;
    size_t lambda = 0;
    // handler is an async lambda (responds after awaiting)
    bool is_async = false;
};
// `await server.listen(host, port)` — start the accept loop; resolves its
// promise once the listener is bound.
struct CHttpServerListen {
    CExprPtr host;
    CExprPtr port;
};
// `new http.JsonValidator({ field: <field validator>, ... })`.
struct CHttpValidatorNew {
    CExprPtr schema;
    std::vector<std::pair<std::string, FieldKind>> fields;
};
// `http.JsonValidator.string()` / `.number()` (static) — a fresh chainable
// field validator.
struct CHttpFieldNew {
    FieldKind kind{};
};
// `.required()` / `.min(n)` / `.max(n)` — returns the same validator.
struct CHttpFieldOp {
    CExprPtr v;
    FieldOp op{};
    std::optional<CExprPtr> arg;
};
// `validator.validate(body)` — parse + check a JSON body; returns an object
// of the synthesized ghost class `out_class`.
struct CHttpValidate {
    CExprPtr v;
    CExprPtr body;
    size_t out_class = 0;
};

// ---- interfaces (structural field access by name) ----
// `obj.field` where obj is interface-typed — resolved at runtime by field
// name.
struct CIfaceGet {
    CExprPtr obj;
    std::string field;
    CTy ty;
};
// `obj.field = value` where obj is interface-typed.
struct CIfaceSetExpr {
    size_t id = 0;
    std::string field;
    CExprPtr value;
    CTy elem;
};
// Static field on a class: `Name.count` (read through a global).
struct CStaticRef {
    std::string gname;
    CTy ty;
};
// `Name.staticField = value` — static field write through a global.
// struct CStaticSet — defined above with the statement node types.
// `obj.method(args)` where the receiver is interface-typed or the method is
// virtually dispatched (declared type is a base class / abstract class).
// Codegen switches on the object's runtime class id and calls the matching
// implementation.
struct CIfaceCall {
    CExprPtr obj;
    std::string iface;
    std::string method;
    std::vector<CExpr> args;
    CTy ret;
};

// ---- fs std module ----
struct CFsReadFile {
    CExprPtr path;
};
struct CFsWriteFile {
    CExprPtr path;
    CExprPtr data;
};
struct CFsExists {
    CExprPtr path;
};
struct CFsReadDir {
    CExprPtr path;
};
struct CFsMkdir {
    CExprPtr path;
};
struct CFsRemove {
    CExprPtr path;
};

// ---- crypto std module ----
struct CCryptoSha256 {
    CExprPtr v;
};
struct CCryptoBase64 {
    bool decode = false;
    CExprPtr v;
};
struct CCryptoRandomHex {
    CExprPtr bytes;
};
struct CCryptoUuid {};

// ---- sql std module ----
// `new sql.Connection(driver, url)` — opens a database connection.
struct CSqlOpen {
    CExprPtr driver;
    CExprPtr url;
};
// `db.execute(sql.stmt\`...\`)` — run a statement, discard rows; returns a
// `sql_Result` object (lastInsertId, rowsAffected).
struct CSqlExec {
    CExprPtr conn;
    std::string sql;
    std::vector<CExpr> binds;
    size_t result_class = 0;
};
// `db.query(sql.stmt\`...\`)` — run a statement, collect rows as objects
// whose fields are the column names (typed `Array<sql_Row>`).
struct CSqlQuery {
    CExprPtr conn;
    std::string sql;
    std::vector<CExpr> binds;
};
// `db.close()`
struct CSqlClose {
    CExprPtr conn;
};

struct CExpr {
    std::variant<CInt, CFloat, CBool, CStr, CTemplate, CIdent, CUnary,
                 CBinary, CCallFn, CCallResolver, CAwait, CNewPromise,
                 CConsoleLog, CSetTimeout, CArrayLit, CIndex, CArrayOp,
                 CArrayHof, CLambdaVal, CStrLen, CStrMethod, CFsArgs,
                 CObjectLit, CNewClass, CNewClassCtor, CFieldGet, CStaticRef,
                 CNetConnect,
                 CNetWrite, CNetReadAll, CNetClose, CHttpCall, CHttpListen,
                 CResSend, CHttpNewServer, CHttpRouter, CHttpRouteAdd,
                 CHttpServerListen, CHttpValidatorNew, CHttpFieldNew,
                 CHttpFieldOp, CHttpValidate, CIfaceGet, CIfaceSetExpr, CIfaceCall,
                 CFsReadFile, CFsWriteFile, CFsExists, CFsReadDir, CFsMkdir,
                 CFsRemove, CCryptoSha256, CCryptoBase64, CCryptoRandomHex,
                 CCryptoUuid, CSqlOpen, CSqlExec, CSqlQuery, CSqlClose>
        v;

    CExpr() : v(CInt{}) {}
};

// ---------------------------------------------------------------------------
// Checked program
// ---------------------------------------------------------------------------

struct CFn {
    std::string name;
    std::vector<std::pair<size_t, CTy>> params;
    CTy ret;
    bool is_async = false;
    std::vector<CStmt> body;
};

struct CLambda {
    // Executor lambdas may have a `resolve` (and optional `reject`) param.
    std::vector<std::pair<size_t, CTy>> params;
    // Captured variables (by value), in capture order.
    std::vector<std::pair<size_t, CTy>> captures;
    std::vector<CStmt> body;
    // Inferred return type (callbacks).
    CTy ret;
    // How the runtime calls this lambda.
    LambdaAbi abi = LambdaAbi::Plain;
    // True when this lambda is a promise executor (its param 0 is the
    // promise handle).
    bool is_executor = false;
    // `async (...) => { ... }` — the lambda may await and runs as an async
    // state machine (its Listen-ABI shim returns a promise).
    bool is_async = false;
};

struct Checked {
    std::vector<CFn> fns;
    std::vector<CLambda> lambdas;
    std::optional<std::vector<CStmt>> toplevel;
    // Types of all checked variables, indexed by variable id.
    std::vector<CTy> var_types;
    // Names of all checked variables, parallel to `var_types` (consumed by
    // the language server for hover).
    std::vector<std::string> var_names;
    // All classes: declared ones first (in declaration order), then ghost
    // classes in the order their object literals were checked.
    std::vector<ClassDef> classes;
    // `iface + "\n" + method` → (class id, mangled `Class__method`) for every
    // public method whose signature matches the interface method.
    std::unordered_map<std::string, std::vector<std::pair<size_t, std::string>>> iface_impls;
    bool has_async_main = false;
    bool has_sync_main = false;
    // The program called `main()` explicitly at the top level — the runtime
    // entry point must not invoke it a second time.
    bool explicit_main = false;
};

// Thrown by `check` when the program does not type-check.
struct CheckDiags : std::runtime_error {
    std::vector<Diag> diags;
    explicit CheckDiags(std::vector<Diag> d)
        : std::runtime_error(d.empty() ? "check failed" : d[0].msg),
          diags(std::move(d)) {}
};

// Type-check a program; throws CheckDiags on error.
Checked check(const ast::Program& program);

// Type-check a program, collecting diagnostics and returning the best-effort
// checked result (used by the language server).
Checked check_with_diags(const ast::Program& program, std::vector<Diag>& diags);

// Naming rules (enforced by the checker, reused by pipeline/LSP).
bool is_camel_case(const std::string& s);
bool is_upper_snake_case(const std::string& s);
bool is_pascal_case(const std::string& s);
bool is_kebab_case(const std::string& s);

} // namespace marina::ir
