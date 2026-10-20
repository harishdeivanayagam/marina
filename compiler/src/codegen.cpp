// LLVM IR (textual) generation from the checked IR.
//
// Port of crates/marina/src/codegen.rs (the Rust sources are the spec).
//
// Emits textual LLVM IR (.ll). Every `async` function is lowered to:
//   * `@fn.<name>`           — entry: allocates a frame + promise, runs state 0
//   * `@fn.<name>.resume`    — state machine: one block per continuation
// `await` calls `marina_await(promise, resume, frame, next_state)` which
// parks the frame; the runtime's worker threads resume it later.
//
// ARC: string/promise values carry +1 ownership. The compiler inserts
// `marina_retain` / `marina_release` so every object is released exactly
// once on every path (literals are interned and immortal).

#include <marina/codegen.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace marina::codegen {

using namespace marina::ir;

namespace {

[[noreturn]] void cg_unreachable(const std::string& what) {
    throw std::logic_error(what);
}

// ---------------------------------------------------------------------------
// Codegen types
// ---------------------------------------------------------------------------

enum class CgTy {
    Void,
    Int,
    Float,
    Bool,
    Str,
    Promise,
    Resolver,
    Array,
    Object,
    Stream,
};

const char* cg_ir(CgTy t) {
    switch (t) {
        case CgTy::Void: return "void";
        case CgTy::Int: return "i32";
        case CgTy::Float: return "double";
        case CgTy::Bool: return "i1";
        case CgTy::Stream: return "i64";
        case CgTy::Str:
        case CgTy::Promise:
        case CgTy::Resolver:
        case CgTy::Array:
        case CgTy::Object: return "ptr";
    }
    return "";
}

// Type used for memory slots (allocas / frame / env fields).
const char* cg_mem(CgTy t) {
    if (t == CgTy::Bool) return "i8";
    return cg_ir(t);
}

bool cg_is_ptr(CgTy t) {
    switch (t) {
        case CgTy::Str:
        case CgTy::Promise:
        case CgTy::Resolver:
        case CgTy::Array:
        case CgTy::Object: return true;
        default: return false;
    }
}

CgTy cg_of(const CTy& t) {
    switch (t.tag) {
        case CTy::Tag::Void: return CgTy::Void;
        case CTy::Tag::Int: return CgTy::Int;
        case CTy::Tag::Float: return CgTy::Float;
        case CTy::Tag::Bool: return CgTy::Bool;
        case CTy::Tag::Str: return CgTy::Str;
        case CTy::Tag::Promise: return CgTy::Promise;
        case CTy::Tag::Resolver: return CgTy::Resolver;
        case CTy::Tag::Array: return CgTy::Array;
        case CTy::Tag::Object: return CgTy::Object;
        case CTy::Tag::Interface: return CgTy::Object;
        case CTy::Tag::Stream: return CgTy::Stream;
    }
    return CgTy::Void;
}

// MValue tag for a checked type (used when creating arrays / map results).
int32_t mv_tag_of(const CTy& t) {
    switch (cg_of(t)) {
        case CgTy::Int: return 1;
        case CgTy::Float: return 2;
        case CgTy::Str: return 3;
        case CgTy::Bool: return 4;
        case CgTy::Array: return 5;
        case CgTy::Object: return 6;
        case CgTy::Stream: return 7;
        default: cg_unreachable("array element type must be a value type");
    }
}

// MValue tag for a codegen value (Void -> 0; promise values are not stored).
int32_t cg_tag(CgTy t) {
    switch (t) {
        case CgTy::Void: return 0;
        case CgTy::Int: return 1;
        case CgTy::Float: return 2;
        case CgTy::Str: return 3;
        case CgTy::Bool: return 4;
        case CgTy::Array: return 5;
        case CgTy::Object: return 6;
        case CgTy::Stream: return 7;
        case CgTy::Promise:
        case CgTy::Resolver:
            cg_unreachable("promise values are not printable");
    }
}

// Best-effort static type of a checked expression (used to find the element
// type of an array expression).
std::optional<CTy> cexpr_cty(const CExpr& e) {
    if (auto* x = std::get_if<CIdent>(&e.v)) return x->ty;
    if (auto* x = std::get_if<CUnary>(&e.v)) return x->ty;
    if (auto* x = std::get_if<CBinary>(&e.v)) return x->ty;
    if (auto* x = std::get_if<CCallFn>(&e.v)) return x->ret;
    if (auto* x = std::get_if<CCallResolver>(&e.v)) return x->inner;
    if (auto* x = std::get_if<CAwait>(&e.v)) return x->ty;
    if (auto* x = std::get_if<CArrayLit>(&e.v)) return cty_array(x->elem_ty);
    if (auto* x = std::get_if<CIndex>(&e.v)) return x->ty;
    if (auto* x = std::get_if<CArrayOp>(&e.v)) return x->ret;
    if (auto* x = std::get_if<CArrayHof>(&e.v)) return x->ret;
    return std::nullopt;
}

struct CgVal {
    CgTy ty = CgTy::Void;
    std::string ir;
    bool owned = false;
};

struct Access {
    enum class Kind { Alloca, Frame, Env, Global };
    Kind kind = Kind::Alloca;
    std::string name; // Alloca / Global
    size_t idx = 0;   // Frame / Env
    CgTy ty = CgTy::Void;
};

// Insertion-ordered id -> Access map. Release order must be deterministic
// (unordered_map iteration order is unspecified, which would make the
// emitted .ll differ run-to-run).
struct ScopeMap {
    std::vector<std::pair<size_t, Access>> entries;

    void insert(size_t id, Access a) {
        for (auto& e : entries) {
            if (e.first == id) {
                e.second = std::move(a);
                return;
            }
        }
        entries.emplace_back(id, std::move(a));
    }
    const Access* get(size_t id) const {
        for (const auto& e : entries) {
            if (e.first == id) return &e.second;
        }
        return nullptr;
    }
};

struct IrBlock {
    std::string label;
    std::vector<std::string> instrs;
    std::string term;
    bool has_term = false;
};

struct IrFn {
    std::string name;
    std::string ret;
    std::vector<std::string> params;
    std::vector<std::string> allocas;
    std::vector<IrBlock> blocks;
    // Some(ty_name): async entry fn; render prepends the frame alloc.
    std::optional<std::string> frame_ty;
};

struct Sig {
    CgTy ret = CgTy::Void;
    std::vector<CgTy> params;
    // Kept for future lowering (e.g. async call-eligibility checks).
    bool is_async = false;
};

struct FrameInfo {
    std::string ty;
    // slots[0] is the promise; gep field index == slot index.
    std::vector<CgTy> slots;
    std::unordered_map<size_t, size_t> map;
};

struct EnvInfo {
    std::string ty;
    std::vector<CgTy> slot_tys;
    std::unordered_map<size_t, size_t> map;
};

struct SegEnd {
    enum class Kind { Await, ReturnExpr, ReturnVoid, ResolveReturn };
    Kind kind = Kind::ReturnVoid;
    CExpr expr;                                    // Await / ReturnExpr
    std::optional<std::pair<size_t, CTy>> target;  // Await
    size_t next = 0;                               // Await
    CTy ty;                                        // ResolveReturn
};

struct Segment {
    std::vector<CStmt> stmts;
    SegEnd end;
};

const char* default_value(CgTy t) {
    switch (t) {
        case CgTy::Int: return "0";
        case CgTy::Float: return "0.0";
        case CgTy::Bool: return "false";
        case CgTy::Stream: return "0";
        case CgTy::Str:
        case CgTy::Promise:
        case CgTy::Resolver:
        case CgTy::Array:
        case CgTy::Object: return "null";
        case CgTy::Void: return "void";
    }
    return "";
}

std::string escape_ir_string(const std::string& bytes) {
    std::string s;
    for (unsigned char b : bytes) {
        if (b >= 32 && b <= 126 && b != '"' && b != '\\') {
            s.push_back(static_cast<char>(b));
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\%02X", b);
            s += buf;
        }
    }
    return s;
}

// Rust `format!("{:?}", f64)` equivalent: shortest round-trip decimal,
// positional notation for 1e-4 <= |x| < 1e16, `d.ddde<exp>` otherwise.
std::string format_f64(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    char buf[64];
    for (int p = 1; p <= 17; ++p) {
        std::snprintf(buf, sizeof buf, "%.*e", p - 1, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    std::string s = buf;
    size_t epos = s.find('e');
    std::string mant = s.substr(0, epos);
    int exp = std::atoi(s.c_str() + epos + 1);
    bool neg = false;
    if (!mant.empty() && mant[0] == '-') {
        neg = true;
        mant.erase(0, 1);
    }
    std::string digits;
    for (char ch : mant) {
        if (ch != '.') digits.push_back(ch);
    }
    std::string out;
    if (exp >= -4 && exp < 16) {
        std::string ds = digits;
        if (exp >= 0) {
            if (static_cast<size_t>(exp + 1) >= ds.size()) {
                ds.resize(static_cast<size_t>(exp) + 1, '0');
                out = ds + ".0";
            } else {
                out = ds.substr(0, static_cast<size_t>(exp) + 1) + "." +
                      ds.substr(static_cast<size_t>(exp) + 1);
            }
        } else {
            out = "0." + std::string(static_cast<size_t>(-exp - 1), '0') + ds;
        }
    } else {
        out = digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        out += "e" + std::to_string(exp);
    }
    if (neg) out = "-" + out;
    return out;
}

std::string num(size_t v) {
    return std::to_string(v);
}

const char* DECLARES =
    "declare ptr @marina_frame_alloc(i64)\n"
    "declare void @marina_frame_free(ptr)\n"
    "declare ptr @marina_promise_create()\n"
    "declare void @marina_promise_resolve(ptr, ptr)\n"
    "declare void @marina_await(ptr, ptr, ptr, i32)\n"
    "declare ptr @marina_promise_run_executor(ptr, ptr, ptr, ptr)\n"
    "declare void @marina_set_timeout(ptr, ptr, ptr, i64)\n"
    "declare ptr @marina_closure_alloc(i64)\n"
    "declare void @marina_closure_free(ptr)\n"
    "declare ptr @marina_retain(ptr)\n"
    "declare void @marina_release(ptr)\n"
    "declare ptr @marina_string_literal(ptr, i64)\n"
    "declare ptr @marina_string_concat(ptr, ptr)\n"
    "declare i64 @marina_string_len(ptr)\n"
    "declare i64 @marina_string_charcode_at(ptr, i64)\n"
    "declare ptr @marina_string_substring(ptr, i64, i64)\n"
    "declare ptr @marina_args()\n"
    "declare ptr @marina_to_string(ptr)\n"
    "declare void @marina_print(ptr)\n"
    "declare ptr @marina_array_new(i32)\n"
    "declare i64 @marina_array_len(ptr)\n"
    "declare i64 @marina_array_push(ptr, ptr)\n"
    "declare void @marina_array_pop(ptr, ptr)\n"
    "declare void @marina_array_shift(ptr, ptr)\n"
    "declare i64 @marina_array_unshift(ptr, ptr)\n"
    "declare void @marina_array_get(ptr, i64, ptr)\n"
    "declare void @marina_array_set(ptr, i64, ptr)\n"
    "declare ptr @marina_array_splice(ptr, i64, i64)\n"
    "declare ptr @marina_array_slice(ptr, i64, i64)\n"
    "declare ptr @marina_array_concat(ptr, ptr)\n"
    "declare ptr @marina_array_join(ptr, ptr)\n"
    "declare ptr @marina_array_reverse(ptr)\n"
    "declare ptr @marina_array_sort(ptr)\n"
    "declare void @marina_array_foreach(ptr, ptr, ptr, ptr)\n"
    "declare ptr @marina_array_map(ptr, ptr, ptr, ptr, i32)\n"
    "declare ptr @marina_array_filter(ptr, ptr, ptr, ptr)\n"
    "declare void @marina_array_reduce(ptr, ptr, ptr, ptr, ptr, ptr)\n"
    "declare void @marina_class_register(i32, ptr, i64)\n"
    "declare ptr @marina_object_new(i32, i64)\n"
    "declare void @marina_object_get(ptr, i64, ptr)\n"
    "declare void @marina_object_set(ptr, i64, ptr)\n"
    "declare void @marina_object_get_field(ptr, ptr, ptr)\n"
    "declare void @marina_object_set_field(ptr, ptr, ptr)\n"
    "declare void @marina_fs_read_file(ptr, ptr)\n"
    "declare void @marina_fs_write_file(ptr, ptr)\n"
    "declare void @marina_fs_exists(ptr, ptr)\n"
    "declare void @marina_fs_read_dir(ptr, ptr)\n"
    "declare void @marina_fs_mkdir(ptr, ptr)\n"
    "declare void @marina_fs_remove(ptr, ptr)\n"
    "declare void @marina_crypto_sha256(ptr, ptr)\n"
    "declare void @marina_crypto_base64(i1, ptr, ptr)\n"
    "declare void @marina_crypto_random_hex(i64, ptr)\n"
    "declare void @marina_crypto_uuid(ptr)\n"
    "declare void @marina_net_connect(ptr, ptr, ptr)\n"
    "declare void @marina_net_write(ptr, ptr)\n"
    "declare void @marina_net_read_all(ptr, ptr)\n"
    "declare void @marina_net_close(ptr)\n"
    "declare i1 @marina_str_eq(ptr, ptr)\n"
    "declare void @marina_http_request(ptr, ptr, ptr, ptr, i32)\n"
    "declare void @marina_http_listen(ptr, ptr, ptr, ptr, i32, i32)\n"
    "declare void @marina_http_res_send(ptr, ptr)\n"
    "declare void @marina_http_res_json(ptr, ptr)\n"
    "declare void @marina_http_router_bind(ptr, ptr)\n"
    "declare void @marina_http_route_add(ptr, ptr, ptr, ptr, ptr, ptr)\n"
    "declare void @marina_http_server_listen(ptr, ptr, ptr, i32, i32)\n"
    "declare void @marina_http_validator_init(ptr, ptr)\n"
    "declare void @marina_http_field_init(ptr, i32)\n"
    "declare void @marina_http_field_op(ptr, i32, ptr)\n"
    "declare void @marina_http_validate(ptr, ptr, i32, ptr)\n"
    "declare void @marina_sql_open(ptr, ptr, i32, ptr)\n"
    "declare void @marina_sql_execute(ptr, ptr, i64, ptr, i32, ptr)\n"
    "declare void @marina_sql_query(ptr, ptr, i64, ptr, ptr)\n"
    "declare void @marina_sql_close(ptr)\n"
    "declare ptr @marina_sql_execute_async(ptr, ptr, i64, ptr, i32)\n"
    "declare ptr @marina_sql_query_async(ptr, ptr, i64, ptr)\n"
    "declare ptr @marina_fs_read_file_async(ptr)\n"
    "declare ptr @marina_fs_write_file_async(ptr, ptr)\n"
    "declare ptr @marina_fs_exists_async(ptr)\n"
    "declare ptr @marina_fs_read_dir_async(ptr)\n"
    "declare ptr @marina_fs_mkdir_async(ptr)\n"
    "declare ptr @marina_fs_remove_async(ptr)\n"
    "declare ptr @marina_net_connect_async(ptr, ptr)\n"
    "declare ptr @marina_net_write_async(ptr, ptr)\n"
    "declare ptr @marina_net_read_all_async(ptr)\n"
    "declare ptr @marina_net_close_async(ptr)\n"
    "declare ptr @marina_http_request_async(ptr, ptr, ptr, i32)\n"
    "declare void @marina_event_loop_start()\n"
    "declare i32 @marina_event_loop_run()";

std::string render_fn(const IrFn& f);

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------

struct Gen {
    const Checked* c = nullptr;
    std::vector<std::string> type_defs;
    std::unordered_set<size_t> env_types_done;
    std::vector<std::string> globals;
    std::unordered_map<std::string, std::string> lit_names;
    std::unordered_map<std::string, Sig> fn_sigs;
    std::vector<IrFn> fns;
    // current function state
    size_t cur = 0;
    size_t block = 0;
    std::vector<ScopeMap> scopes;
    std::vector<std::string> pending;  // owned temps to release at end of statement
    std::optional<FrameInfo> frame;
    std::optional<EnvInfo> env;
    CgTy cur_ret = CgTy::Void;
    size_t tmp = 0;
    size_t lbl = 0;
    size_t lit = 0;
    size_t var_tmp = 1'000'000;  // ids for synthetic await targets
    // current callback-lambda context (set while generating a lambda body)
    std::optional<std::string> lambda_out;
    std::optional<size_t> lambda_exit;
    std::optional<CgTy> lambda_ret;
    // true while generating fn.__toplevel: variables declare as globals
    bool toplevel_gen = false;
    // var id -> global access (top-level variables)
    std::unordered_map<size_t, Access> toplevel_globals;

    explicit Gen(const Checked* checked) : c(checked) {
        scopes.emplace_back();
    }

    // ------------------------------------------------------------------
    // IR building primitives
    // ------------------------------------------------------------------

    std::string fresh(const std::string& hint) {
        ++tmp;
        return "%" + hint + "." + num(tmp);
    }

    size_t fresh_var_id() {
        return ++var_tmp;
    }

    void push_fn(IrFn f) {
        fns.push_back(std::move(f));
        cur = fns.size() - 1;
        block = 0;
        scopes.clear();
        scopes.emplace_back();
        pending.clear();
    }

    void emit(const std::string& line) {
        fns[cur].blocks[block].instrs.push_back(line);
    }

    void term(const std::string& line) {
        IrBlock& b = fns[cur].blocks[block];
        if (!b.has_term) {
            b.term = line;
            b.has_term = true;
        }
    }

    bool terminated() const {
        return fns[cur].blocks[block].has_term;
    }

    std::string alloca(const std::string& ty) {
        std::string name = fresh("v");
        fns[cur].allocas.push_back(name + " = alloca " + ty);
        return name;
    }

    size_t new_block(const std::string& hint) {
        ++lbl;
        fns[cur].blocks.push_back(IrBlock{hint + "." + num(lbl), {}, "", false});
        return fns[cur].blocks.size() - 1;
    }

    size_t new_block_fixed(std::string label) {
        fns[cur].blocks.push_back(IrBlock{std::move(label), {}, "", false});
        return fns[cur].blocks.size() - 1;
    }

    void switch_block(size_t idx) {
        block = idx;
    }

    std::string label_of(size_t idx) const {
        return fns[cur].blocks[idx].label;
    }

    void branch_to(size_t idx) {
        if (!terminated()) {
            std::string label = label_of(idx);
            term("br label %" + label);
        }
    }

    void flush_pending() {
        std::vector<std::string> names = std::move(pending);
        pending.clear();
        for (const auto& n : names) {
            emit("call void @marina_release(ptr " + n + ")");
        }
    }

    // ------------------------------------------------------------------
    // Memory access
    // ------------------------------------------------------------------

    size_t slot_for(size_t id, CgTy ty) {
        FrameInfo& fr = *frame;
        auto it = fr.map.find(id);
        if (it != fr.map.end()) return it->second;
        size_t idx = fr.slots.size();
        fr.slots.push_back(ty);
        fr.map.emplace(id, idx);
        return idx;
    }

    Access declare_access(size_t id, CgTy ty) {
        if (toplevel_gen) {
            // top-level variables become module globals so any function
            // (and any lambda capturing them) can read them
            std::string g = "@.g." + num(lit);
            ++lit;
            globals.push_back(g + " = global " + cg_mem(ty) + " zeroinitializer");
            Access a;
            a.kind = Access::Kind::Global;
            a.name = g;
            a.ty = ty;
            toplevel_globals.insert_or_assign(id, a);
            return a;
        }
        if (frame.has_value()) {
            size_t idx = slot_for(id, ty);
            Access a;
            a.kind = Access::Kind::Frame;
            a.idx = idx;
            a.ty = ty;
            return a;
        }
        std::string p = alloca(cg_mem(ty));
        Access a;
        a.kind = Access::Kind::Alloca;
        a.name = p;
        a.ty = ty;
        return a;
    }

    std::optional<Access> access_for(size_t id) const {
        // top-level variables live in globals: lambdas reading or assigning
        // them share program state instead of a by-value capture
        {
            auto it = toplevel_globals.find(id);
            if (it != toplevel_globals.end()) return it->second;
        }
        for (auto sc = scopes.rbegin(); sc != scopes.rend(); ++sc) {
            if (const Access* a = sc->get(id)) return *a;
        }
        if (frame.has_value()) {
            auto it = frame->map.find(id);
            if (it != frame->map.end()) {
                Access a;
                a.kind = Access::Kind::Frame;
                a.idx = it->second;
                a.ty = frame->slots[it->second];
                return a;
            }
        }
        if (env.has_value()) {
            auto it = env->map.find(id);
            if (it != env->map.end()) {
                Access a;
                a.kind = Access::Kind::Env;
                a.idx = it->second;
                a.ty = env->slot_tys[it->second];
                return a;
            }
        }
        {
            auto it = toplevel_globals.find(id);
            if (it != toplevel_globals.end()) return it->second;
        }
        return std::nullopt;
    }

    // Emit a GEP instruction for an access and return its SSA name.
    // (Local base pointers can't be used inside constant expressions.)
    std::string gep_ssa(const Access& a) {
        switch (a.kind) {
            case Access::Kind::Alloca:
            case Access::Kind::Global:
                return a.name;
            case Access::Kind::Frame: {
                std::string fty = frame->ty;
                std::string g = fresh("gp");
                emit(g + " = getelementptr inbounds " + fty + ", ptr %fr, i64 0, i32 " +
                     num(a.idx));
                return g;
            }
            case Access::Kind::Env: {
                std::string ety = env->ty;
                std::string g = fresh("gp");
                emit(g + " = getelementptr inbounds " + ety + ", ptr %env, i64 0, i32 " +
                     num(a.idx));
                return g;
            }
        }
        cg_unreachable("bad access kind");
    }

    // Emit a GEP instruction addressing field `field` of an %mv value.
    std::string mv_field(const std::string& a, size_t field) {
        std::string g = fresh("mv");
        emit(g + " = getelementptr inbounds %mv, ptr " + a + ", i64 0, i32 " + num(field));
        return g;
    }

    std::pair<std::string, CgTy> load_access(const Access& a) {
        CgTy ty = a.ty;
        std::string gep = gep_ssa(a);
        std::string v = fresh("ld");
        if (ty == CgTy::Bool) {
            emit(v + " = load i8, ptr " + gep);
            std::string b = fresh("bb");
            emit(b + " = trunc i8 " + v + " to i1");
            return {b, ty};
        }
        emit(v + " = load " + cg_ir(ty) + ", ptr " + gep);
        return {v, ty};
    }

    void store_access(const Access& a, const std::string& val, CgTy ty) {
        std::string gep = gep_ssa(a);
        if (ty == CgTy::Bool) {
            std::string z = fresh("zz");
            emit(z + " = zext i1 " + val + " to i8");
            emit("store i8 " + z + ", ptr " + gep);
        } else {
            emit("store " + std::string(cg_ir(ty)) + " " + val + ", ptr " + gep);
        }
    }

    // ------------------------------------------------------------------
    // ARC helpers
    // ------------------------------------------------------------------

    CgVal to_owned(CgVal v) {
        if (v.owned || !cg_is_ptr(v.ty)) return v;
        std::string r = fresh("rn");
        emit(r + " = call ptr @marina_retain(ptr " + v.ir + ")");
        CgVal out;
        out.ty = v.ty;
        out.ir = r;
        out.owned = true;
        return out;
    }

    // Borrow for a runtime call that only reads the value. Consumes an owned
    // value exactly once (the queued release) and marks it unowned, so a
    // later `discard` of the same value cannot double-release it.
    std::string borrow(CgVal& v) {
        if (v.owned && cg_is_ptr(v.ty)) {
            pending.push_back(v.ir);
            v.owned = false;
        }
        return v.ir;
    }

    // Consume an expression whose value is discarded: release if owned.
    void discard(const CgVal& v) {
        if (v.owned && cg_is_ptr(v.ty)) {
            pending.push_back(v.ir);
        }
    }

    // ------------------------------------------------------------------
    // Conversions / helpers
    // ------------------------------------------------------------------

    CgVal coerce_val(CgVal v, CgTy to) {
        if (v.ty == CgTy::Int && to == CgTy::Float) {
            std::string r = fresh("cf");
            emit(r + " = sitofp i32 " + v.ir + " to double");
            CgVal out;
            out.ty = CgTy::Float;
            out.ir = r;
            out.owned = v.owned;
            return out;
        }
        return v;
    }

    std::string sext_i64(const CgVal& v) {
        std::string r = fresh("w");
        emit(r + " = sext i32 " + v.ir + " to i64");
        return r;
    }

    CgVal string_lit(const std::string& s) {
        std::string name;
        auto it = lit_names.find(s);
        if (it != lit_names.end()) {
            name = it->second;
        } else {
            name = "@.bytes." + num(lit);
            ++lit;
            std::string bytes = escape_ir_string(s);
            globals.push_back(name + " = private constant [" + num(s.size()) +
                              " x i8] c\"" + bytes + "\"");
            lit_names.emplace(s, name);
        }
        std::string v = fresh("st");
        emit(v + " = call ptr @marina_string_literal(ptr " + name + ", i64 " +
             num(s.size()) + ")");
        return CgVal{CgTy::Str, v, true};
    }

    // Build an %mv value on the stack from a typed value (borrows it).
    std::string build_mv(const CgVal& v) {
        std::string a = alloca("%mv");
        int32_t tag = cg_tag(v.ty);
        emit("store i32 " + num(static_cast<size_t>(tag)) + ", ptr " + a);
        {
            std::string g = mv_field(a, 1);
            emit("store i32 0, ptr " + g);
        }
        switch (v.ty) {
            case CgTy::Int: {
                std::string x = sext_i64(v);
                std::string g = mv_field(a, 2);
                emit("store i64 " + x + ", ptr " + g);
                break;
            }
            case CgTy::Stream: {
                std::string g = mv_field(a, 2);
                emit("store i64 " + v.ir + ", ptr " + g);
                break;
            }
            case CgTy::Float: {
                std::string g = mv_field(a, 3);
                emit("store double " + v.ir + ", ptr " + g);
                break;
            }
            case CgTy::Bool: {
                std::string x = fresh("bw");
                emit(x + " = zext i1 " + v.ir + " to i64");
                std::string g = mv_field(a, 2);
                emit("store i64 " + x + ", ptr " + g);
                break;
            }
            case CgTy::Str: {
                std::string g = mv_field(a, 4);
                emit("store ptr " + v.ir + ", ptr " + g);
                break;
            }
            case CgTy::Array:
            case CgTy::Object: {
                std::string g = mv_field(a, 4);
                emit("store ptr " + v.ir + ", ptr " + g);
                break;
            }
            default: break;
        }
        return a;
    }

    // Load a typed value out of an MValue struct (alloca'd). Ptr payloads are
    // owned by the caller (the runtime retains them before handing them over).
    CgVal mv_load(const std::string& a, CgTy ct) {
        switch (ct) {
            case CgTy::Int: {
                std::string g = mv_field(a, 2);
                std::string l = fresh("ml");
                emit(l + " = load i64, ptr " + g);
                std::string t = fresh("mt");
                emit(t + " = trunc i64 " + l + " to i32");
                return CgVal{CgTy::Int, t, false};
            }
            case CgTy::Float: {
                std::string g = mv_field(a, 3);
                std::string l = fresh("ml");
                emit(l + " = load double, ptr " + g);
                return CgVal{CgTy::Float, l, false};
            }
            case CgTy::Bool: {
                std::string g = mv_field(a, 2);
                std::string l = fresh("ml");
                emit(l + " = load i64, ptr " + g);
                std::string t = fresh("mt");
                emit(t + " = trunc i64 " + l + " to i1");
                return CgVal{CgTy::Bool, t, false};
            }
            case CgTy::Stream: {
                std::string g = mv_field(a, 2);
                std::string l = fresh("ml");
                emit(l + " = load i64, ptr " + g);
                return CgVal{CgTy::Stream, l, false};
            }
            case CgTy::Str:
            case CgTy::Array:
            case CgTy::Object: {
                std::string g = mv_field(a, 4);
                std::string l = fresh("ml");
                emit(l + " = load ptr, ptr " + g);
                return CgVal{ct, l, true};
            }
            default:
                return CgVal{CgTy::Void, std::string(), false};
        }
    }

    // Store a typed value into an MValue struct pointed to by `dst`
    // (used for callback `out` parameters and array element writes).
    void write_mv_fields(const std::string& dst, const CgVal& v) {
        int32_t tag = cg_tag(v.ty);
        emit("store i32 " + num(static_cast<size_t>(tag)) + ", ptr " + dst);
        {
            std::string g = mv_field(dst, 1);
            emit("store i32 0, ptr " + g);
        }
        switch (v.ty) {
            case CgTy::Int: {
                std::string x = sext_i64(v);
                std::string g = mv_field(dst, 2);
                emit("store i64 " + x + ", ptr " + g);
                break;
            }
            case CgTy::Stream: {
                std::string g = mv_field(dst, 2);
                emit("store i64 " + v.ir + ", ptr " + g);
                break;
            }
            case CgTy::Float: {
                std::string g = mv_field(dst, 3);
                emit("store double " + v.ir + ", ptr " + g);
                break;
            }
            case CgTy::Bool: {
                std::string x = fresh("bw");
                emit(x + " = zext i1 " + v.ir + " to i64");
                std::string g = mv_field(dst, 2);
                emit("store i64 " + x + ", ptr " + g);
                break;
            }
            case CgTy::Str:
            case CgTy::Array:
            case CgTy::Object: {
                std::string g = mv_field(dst, 4);
                emit("store ptr " + v.ir + ", ptr " + g);
                break;
            }
            default: break;
        }
    }

    // Unpack an MValue argument (from the runtime) into a typed alloca.
    void store_from_mv(const std::string& src, CgTy ct, const std::string& dst) {
        std::string v;
        switch (ct) {
            case CgTy::Int:
            case CgTy::Bool:
            case CgTy::Stream: {
                std::string g = mv_field(src, 2);
                std::string l = fresh("pm");
                emit(l + " = load i64, ptr " + g);
                v = l;
                break;
            }
            case CgTy::Float: {
                std::string g = mv_field(src, 3);
                std::string l = fresh("pm");
                emit(l + " = load double, ptr " + g);
                v = l;
                break;
            }
            case CgTy::Str:
            case CgTy::Array:
            case CgTy::Object: {
                std::string g = mv_field(src, 4);
                std::string l = fresh("pm");
                emit(l + " = load ptr, ptr " + g);
                v = l;
                break;
            }
            default:
                v = std::string();
                break;
        }
        switch (ct) {
            case CgTy::Int: {
                std::string t = fresh("pt");
                emit(t + " = trunc i64 " + v + " to i32");
                emit("store i32 " + t + ", ptr " + dst);
                break;
            }
            case CgTy::Bool: {
                std::string t = fresh("pt");
                emit(t + " = trunc i64 " + v + " to i1");
                std::string z = fresh("pz");
                emit(z + " = zext i1 " + t + " to i8");
                emit("store i8 " + z + ", ptr " + dst);
                break;
            }
            default:
                emit("store " + std::string(cg_ir(ct)) + " " + v + ", ptr " + dst);
                break;
        }
    }

    // ------------------------------------------------------------------
    // Statements
    // ------------------------------------------------------------------

    void cg_stmt(const CStmt& s) {
        if (auto* st = std::get_if<CLet>(&s.v)) {
            CgTy cg = cg_of(st->ty);
            CgVal v = cg_expr(*st->init);
            v = coerce_val(v, cg);
            v = to_owned(v);
            Access a = declare_access(st->id, cg);
            store_access(a, v.ir, cg);
            scopes.back().insert(st->id, a);
        } else if (auto* st = std::get_if<CAssign>(&s.v)) {
            std::optional<Access> aa = access_for(st->id);
            if (!aa) cg_unreachable("unresolved variable in codegen");
            Access a = *aa;
            CgTy ty = a.ty;
            CgVal v = cg_expr(*st->value);
            v = coerce_val(v, ty);
            v = to_owned(v);
            if (cg_is_ptr(ty)) {
                auto old = load_access(a);
                emit("call void @marina_release(ptr " + old.first + ")");
            }
            store_access(a, v.ir, ty);
        } else if (auto* st = std::get_if<CIndexSet>(&s.v)) {
            std::optional<Access> aa = access_for(st->id);
            if (!aa) cg_unreachable("unresolved variable in codegen");
            auto arr = load_access(*aa);
            CgVal iv = cg_expr(*st->idx);
            std::string i = sext_i64(iv);
            CgVal vv = cg_expr(*st->value);
            vv = coerce_val(vv, cg_of(st->elem));
            vv = to_owned(vv);
            std::string mv = build_mv(vv);
            emit("call void @marina_array_set(ptr " + arr.first + ", i64 " + i +
                 ", ptr " + mv + ")");
            discard(vv);
        } else if (auto* st = std::get_if<CFieldSet>(&s.v)) {
            std::optional<Access> aa = access_for(st->id);
            if (!aa) cg_unreachable("unresolved variable in codegen");
            auto obj = load_access(*aa);
            CgVal vv = cg_expr(*st->value);
            vv = coerce_val(vv, cg_of(st->elem));
            vv = to_owned(vv);
            std::string mv = build_mv(vv);
            emit("call void @marina_object_set(ptr " + obj.first + ", i64 " + num(st->idx) +
                 ", ptr " + mv + ")");
            discard(vv);
        } else if (auto* st = std::get_if<CIfaceSet>(&s.v)) {
            // interface-typed values resolve fields by name at runtime
            std::optional<Access> aa = access_for(st->id);
            if (!aa) cg_unreachable("unresolved variable in codegen");
            auto obj = load_access(*aa);
            CgVal vv = cg_expr(*st->value);
            vv = coerce_val(vv, cg_of(st->elem));
            vv = to_owned(vv);
            CgVal sv = string_lit(st->field);
            std::string nmv = build_mv(sv);
            std::string mv = build_mv(vv);
            emit("call void @marina_object_set_field(ptr " + obj.first + ", ptr " + nmv +
                 ", ptr " + mv + ")");
            discard(vv);
            discard(sv);
        } else if (auto* st = std::get_if<CIf>(&s.v)) {
            CgVal cc = cg_expr(*st->cond);
            size_t then_b = new_block("if.then");
            size_t else_b = new_block("if.else");
            size_t end_b = new_block("if.end");
            std::string tl = label_of(then_b);
            std::string el = label_of(else_b);
            term("br i1 " + cc.ir + ", label %" + tl + ", label %" + el);
            switch_block(then_b);
            scopes.emplace_back();
            cg_body(st->then_body);
            pop_scope();
            branch_to(end_b);
            switch_block(else_b);
            scopes.emplace_back();
            if (st->else_body.has_value()) {
                cg_body(*st->else_body);
            }
            pop_scope();
            branch_to(end_b);
            switch_block(end_b);
        } else if (auto* st = std::get_if<CWhile>(&s.v)) {
            size_t cond_b = new_block("while.cond");
            size_t body_b = new_block("while.body");
            size_t end_b = new_block("while.end");
            branch_to(cond_b);
            switch_block(cond_b);
            CgVal cc = cg_expr(*st->cond);
            std::string bl = label_of(body_b);
            std::string el = label_of(end_b);
            term("br i1 " + cc.ir + ", label %" + bl + ", label %" + el);
            switch_block(body_b);
            scopes.emplace_back();
            cg_body(st->body);
            pop_scope();
            branch_to(cond_b);
            switch_block(end_b);
        } else if (auto* st = std::get_if<CFor>(&s.v)) {
            // scope holding the init variable(s)
            scopes.emplace_back();
            if (st->init.has_value()) {
                std::vector<CStmt> one;
                one.push_back(**st->init);
                cg_body(one);
            }
            size_t cond_b = new_block("for.cond");
            size_t body_b = new_block("for.body");
            size_t end_b = new_block("for.end");
            branch_to(cond_b);
            switch_block(cond_b);
            CgVal cc = cg_expr(*st->cond);
            std::string bl = label_of(body_b);
            std::string el = label_of(end_b);
            term("br i1 " + cc.ir + ", label %" + bl + ", label %" + el);
            switch_block(body_b);
            scopes.emplace_back();
            cg_body(st->body);
            pop_scope();
            if (st->update.has_value()) {
                std::vector<CStmt> one;
                one.push_back(**st->update);
                cg_body(one);
            }
            branch_to(cond_b);
            switch_block(end_b);
            pop_scope();
        } else if (auto* st = std::get_if<CReturn>(&s.v)) {
            // callback-lambda `return`: write result into the %out MValue
            // param (or a void tag) and jump to the lambda exit block
            if (lambda_out.has_value()) {
                std::string out = *lambda_out;
                CgTy rt = lambda_ret.value_or(CgTy::Void);
                if (st->value.has_value()) {
                    CgVal v = cg_expr(**st->value);
                    v = coerce_val(v, rt);
                    write_mv_fields(out, v);
                    discard(v);
                } else {
                    CgVal voidv;
                    voidv.ty = CgTy::Void;
                    voidv.ir = "null";
                    voidv.owned = false;
                    write_mv_fields(out, voidv);
                }
                flush_pending();
                release_scope_vars();
                std::string el = label_of(*lambda_exit);
                term("br label %" + el);
                return;
            }
            bool is_async = frame.has_value();
            bool has_value = st->value.has_value();
            if (has_value && st->kind.kind == ReturnKindKind::FlattenPromise) {
                // FlattenPromise at nested depth is rejected by the
                // checker; treat defensively as a plain value.
                CgVal v = cg_expr(**st->value);
                if (is_async) {
                    finish_async(std::move(v));
                } else {
                    cg_unreachable("unreachable: FlattenPromise outside async fn");
                }
            } else if (has_value && st->kind.kind == ReturnKindKind::Plain) {
                CgVal v = cg_expr(**st->value);
                v = coerce_val(v, cur_ret);
                if (is_async) {
                    finish_async(std::move(v));
                } else if (cur_ret == CgTy::Void) {
                    discard(v);
                    sync_epilogue(std::nullopt);
                } else {
                    v = to_owned(v);
                    sync_epilogue(std::move(v));
                }
            } else {
                if (is_async) {
                    finish_async(std::nullopt);
                } else {
                    sync_epilogue(std::nullopt);
                }
            }
        } else if (auto* st = std::get_if<CExprStmt>(&s.v)) {
            CgVal v = cg_expr(*st->expr);
            discard(v);
        } else {
            cg_unreachable("unknown statement kind in codegen");
        }
        flush_pending();
    }

    void cg_body(const std::vector<CStmt>& stmts) {
        for (const auto& s : stmts) {
            if (terminated()) {
                break;  // unreachable statements after return
            }
            cg_stmt(s);
        }
    }

    // Release every live local variable across all scopes (skips closure env
    // slots, which the generated drop function owns).
    void release_scope_vars() {
        std::vector<Access> accesses;
        for (auto sc = scopes.rbegin(); sc != scopes.rend(); ++sc) {
            for (const auto& e : sc->entries) {
                if (e.second.kind == Access::Kind::Env) continue;
                accesses.push_back(e.second);
            }
        }
        for (const auto& a : accesses) {
            // globals outlive the function — never release them
            if (a.kind == Access::Kind::Global) continue;
            if (cg_is_ptr(a.ty)) {
                auto old = load_access(a);
                emit("call void @marina_release(ptr " + old.first + ")");
            }
        }
    }

    void pop_scope() {
        ScopeMap sc = std::move(scopes.back());
        scopes.pop_back();
        if (terminated()) {
            return;  // releases were emitted by the return epilogue
        }
        std::vector<Access> accesses;
        for (const auto& e : sc.entries) accesses.push_back(e.second);
        for (const auto& a : accesses) {
            // globals outlive the function — never release them
            if (a.kind == Access::Kind::Global) continue;
            if (cg_is_ptr(a.ty)) {
                auto v = load_access(a);
                emit("call void @marina_release(ptr " + v.first + ")");
            }
        }
    }

    // Releases every live variable of the current (sync) function, then ret.
    void sync_epilogue(std::optional<CgVal> v) {
        release_scope_vars();
        flush_pending();
        if (v.has_value()) {
            if (v->ty == CgTy::Void) {
                term("ret void");
            } else {
                term("ret " + std::string(cg_ir(v->ty)) + " " + v->ir);
            }
        } else {
            if (cur_ret == CgTy::Void) {
                term("ret void");
            } else {
                term("ret " + std::string(cg_ir(cur_ret)) + " " + default_value(cur_ret));
            }
        }
    }

    // Terminate an async continuation: resolve this function's promise,
    // release every frame slot, free the frame.
    void finish_async(std::optional<CgVal> v) {
        std::string mv;
        if (v.has_value()) {
            CgVal val = std::move(*v);
            std::string borrowed = borrow(val);
            mv = alloca("%mv");
            copy_mv(borrowed, val.ty, mv);
        } else {
            mv = alloca("%mv");
            emit("store i32 0, ptr " + mv);
            std::string g = mv_field(mv, 1);
            emit("store i32 0, ptr " + g);
        }
        Access promise_access;
        promise_access.kind = Access::Kind::Frame;
        promise_access.idx = 0;
        promise_access.ty = CgTy::Promise;
        auto pr = load_access(promise_access);
        emit("call void @marina_promise_resolve(ptr " + pr.first + ", ptr " + mv + ")");
        flush_pending();
        release_all_frame_slots();
        emit("call void @marina_frame_free(ptr %fr)");
        term("ret void");
    }

    void copy_mv(const std::string& val, CgTy ty, const std::string& a) {
        switch (ty) {
            case CgTy::Int: {
                CgVal iv{CgTy::Int, val, false};
                std::string x = sext_i64(iv);
                std::string g = mv_field(a, 2);
                emit("store i64 " + x + ", ptr " + g);
                emit("store i32 1, ptr " + a);
                break;
            }
            case CgTy::Float: {
                std::string g = mv_field(a, 3);
                emit("store double " + val + ", ptr " + g);
                emit("store i32 2, ptr " + a);
                break;
            }
            case CgTy::Bool: {
                std::string x = fresh("bw");
                emit(x + " = zext i1 " + val + " to i64");
                std::string g = mv_field(a, 2);
                emit("store i64 " + x + ", ptr " + g);
                emit("store i32 4, ptr " + a);
                break;
            }
            case CgTy::Str: {
                std::string g = mv_field(a, 4);
                emit("store ptr " + val + ", ptr " + g);
                emit("store i32 3, ptr " + a);
                break;
            }
            default:
                emit("store i32 0, ptr " + a);
                break;
        }
    }

    void release_all_frame_slots() {
        std::vector<CgTy> slots = frame->slots;
        for (size_t idx = 0; idx < slots.size(); ++idx) {
            CgTy ty = slots[idx];
            if (cg_is_ptr(ty)) {
                Access a;
                a.kind = Access::Kind::Frame;
                a.idx = idx;
                a.ty = ty;
                auto v = load_access(a);
                emit("call void @marina_release(ptr " + v.first + ")");
            }
        }
    }

    // ------------------------------------------------------------------
    // Expressions
    // ------------------------------------------------------------------

    CgVal cg_expr(const CExpr& e) {
        if (auto* x = std::get_if<CInt>(&e.v)) {
            return CgVal{CgTy::Int, std::to_string(x->value), false};
        }
        if (auto* x = std::get_if<CFloat>(&e.v)) {
            return CgVal{CgTy::Float, format_f64(x->value), false};
        }
        if (auto* x = std::get_if<CBool>(&e.v)) {
            return CgVal{CgTy::Bool, std::string(x->value ? "1" : "0"), false};
        }
        if (auto* x = std::get_if<CStr>(&e.v)) {
            return string_lit(x->value);
        }
        if (auto* x = std::get_if<CTemplate>(&e.v)) {
            CgVal acc = string_lit("");
            for (const auto& p : x->parts) {
                CgVal s;
                if (auto* str = std::get_if<std::string>(&p.v)) {
                    s = string_lit(*str);
                } else if (auto* inner = std::get_if<std::shared_ptr<CExpr>>(&p.v)) {
                    CgVal v = cg_expr(**inner);
                    std::string mv = build_mv(v);
                    std::string sv = fresh("ts");
                    emit(sv + " = call ptr @marina_to_string(ptr " + mv + ")");
                    s = CgVal{CgTy::Str, sv, true};
                } else {
                    cg_unreachable("bad template part");
                }
                std::string a = borrow(acc);
                std::string b = borrow(s);
                std::string t = fresh("cc");
                emit(t + " = call ptr @marina_string_concat(ptr " + a + ", ptr " + b + ")");
                acc = CgVal{CgTy::Str, t, true};
            }
            return acc;
        }
        if (auto* x = std::get_if<CIdent>(&e.v)) {
            std::optional<Access> aa = access_for(x->id);
            if (!aa) {
                std::string ctx = "scopes:";
                for (const auto& sc : scopes) {
                    ctx += " [";
                    bool first = true;
                    for (const auto& entry : sc.entries) {
                        if (!first) ctx += ", ";
                        first = false;
                        ctx += num(entry.first);
                    }
                    ctx += "],";
                }
                cg_unreachable("unresolved variable in codegen: " + num(x->id) +
                               " (ty " + x->ty.name() + "); env=" +
                               (env.has_value() ? "true" : "false") + ", frame=" +
                               (frame.has_value() ? "true" : "false") + ", fn=" +
                               fns[cur].name + " [" + ctx + "]");
            }
            auto v = load_access(*aa);
            return CgVal{cg_of(x->ty), v.first, false};
        }
        if (auto* x = std::get_if<CUnary>(&e.v)) {
            CgVal v = cg_expr(*x->operand);
            std::string r = fresh("un");
            if (x->op == UnOpC::Neg && cg_of(x->ty) == CgTy::Int) {
                emit(r + " = sub i32 0, " + v.ir);
                return CgVal{CgTy::Int, r, false};
            }
            if (x->op == UnOpC::Neg && cg_of(x->ty) == CgTy::Float) {
                emit(r + " = fneg double " + v.ir);
                return CgVal{CgTy::Float, r, false};
            }
            if (x->op == UnOpC::Not) {
                emit(r + " = xor i1 " + v.ir + ", true");
                return CgVal{CgTy::Bool, r, false};
            }
            cg_unreachable("unreachable unary op");
        }
        if (auto* x = std::get_if<CBinary>(&e.v)) {
            BinOpC op = x->op;
            CgTy res = cg_of(x->ty);
            CgVal lv = cg_expr(*x->lhs);
            CgVal rv = cg_expr(*x->rhs);
            // string equality/inequality goes through the runtime helper
            if ((op == BinOpC::Eq || op == BinOpC::Neq) && lv.ty == CgTy::Str &&
                rv.ty == CgTy::Str) {
                std::string amv = build_mv(lv);
                std::string bmv = build_mv(rv);
                std::string out = fresh("bc");
                emit(out + " = call i1 @marina_str_eq(ptr " + amv + ", ptr " + bmv + ")");
                std::string ir;
                if (op == BinOpC::Neq) {
                    std::string nx = fresh("bc");
                    emit(nx + " = xor i1 " + out + ", 1");
                    ir = nx;
                } else {
                    ir = out;
                }
                return CgVal{CgTy::Bool, ir, false};
            }
            lv = coerce_val(lv, res);
            rv = coerce_val(rv, res);
            if (op == BinOpC::And) {
                std::string out = fresh("bd");
                emit(out + " = and i1 " + lv.ir + ", " + rv.ir);
                return CgVal{CgTy::Bool, out, false};
            }
            if (op == BinOpC::Or) {
                std::string out = fresh("bd");
                emit(out + " = or i1 " + lv.ir + ", " + rv.ir);
                return CgVal{CgTy::Bool, out, false};
            }
            if (op == BinOpC::Lt || op == BinOpC::Gt || op == BinOpC::Le ||
                op == BinOpC::Ge || op == BinOpC::Eq || op == BinOpC::Neq) {
                // operands must share a numeric type: widen Int to
                // Float when either side is Float (the comparison's
                // own CgTy is Bool, so `res` cannot drive this)
                CgTy cmp_cg = (lv.ty == CgTy::Float || rv.ty == CgTy::Float) ? CgTy::Float
                                                                             : CgTy::Int;
                lv = coerce_val(lv, cmp_cg);
                rv = coerce_val(rv, cmp_cg);
                const char* cmp_ty = cmp_cg == CgTy::Float ? "double" : "i32";
                const char* pred;
                if (op == BinOpC::Lt) pred = cmp_cg == CgTy::Int ? "slt" : "olt";
                else if (op == BinOpC::Gt) pred = cmp_cg == CgTy::Int ? "sgt" : "ogt";
                else if (op == BinOpC::Le) pred = cmp_cg == CgTy::Int ? "sle" : "ole";
                else if (op == BinOpC::Ge) pred = cmp_cg == CgTy::Int ? "sge" : "oge";
                else if (op == BinOpC::Eq) pred = cmp_cg == CgTy::Int ? "eq" : "oeq";
                else pred = cmp_cg == CgTy::Int ? "ne" : "one";
                std::string out = fresh("bc");
                const char* mnemonic = cmp_cg == CgTy::Float ? "fcmp" : "icmp";
                emit(out + " = " + mnemonic + " " + pred + " " + cmp_ty + " " + lv.ir +
                     ", " + rv.ir);
                return CgVal{CgTy::Bool, out, false};
            }
            std::string out = fresh("ba");
            if (res == CgTy::Int && op == BinOpC::Add) emit(out + " = add i32 " + lv.ir + ", " + rv.ir);
            else if (res == CgTy::Int && op == BinOpC::Sub) emit(out + " = sub i32 " + lv.ir + ", " + rv.ir);
            else if (res == CgTy::Int && op == BinOpC::Mul) emit(out + " = mul i32 " + lv.ir + ", " + rv.ir);
            else if (res == CgTy::Int && op == BinOpC::Div) emit(out + " = sdiv i32 " + lv.ir + ", " + rv.ir);
            else if (res == CgTy::Int && op == BinOpC::Mod) emit(out + " = srem i32 " + lv.ir + ", " + rv.ir);
            else if (res == CgTy::Float && op == BinOpC::Add) emit(out + " = fadd double " + lv.ir + ", " + rv.ir);
            else if (res == CgTy::Float && op == BinOpC::Sub) emit(out + " = fsub double " + lv.ir + ", " + rv.ir);
            else if (res == CgTy::Float && op == BinOpC::Mul) emit(out + " = fmul double " + lv.ir + ", " + rv.ir);
            else if (res == CgTy::Float && op == BinOpC::Div) emit(out + " = fdiv double " + lv.ir + ", " + rv.ir);
            else cg_unreachable("unreachable binary op");
            return CgVal{res, out, false};
        }
        if (auto* x = std::get_if<CCallFn>(&e.v)) {
            auto it = fn_sigs.find(x->name);
            if (it == fn_sigs.end()) cg_unreachable("missing fn sig for " + x->name);
            Sig sig = it->second;
            std::vector<std::string> call_args;
            for (size_t i = 0; i < x->args.size(); ++i) {
                CgVal v = cg_expr(x->args[i]);
                CgTy pt = i < sig.params.size() ? sig.params[i] : cg_of(x->ret);
                v = coerce_val(v, pt);
                v = to_owned(v);
                call_args.push_back(std::string(cg_ir(pt)) + " " + v.ir);
            }
            CgTy rty = x->is_async ? CgTy::Promise : cg_of(x->ret);
            std::string fname = "fn." + x->name;
            std::string args = call_args.empty() ? std::string() : call_args[0];
            for (size_t i = 1; i < call_args.size(); ++i) args += ", " + call_args[i];
            if (rty == CgTy::Void) {
                emit("call void @" + fname + "(" + args + ")");
                return CgVal{CgTy::Void, std::string(), false};
            }
            std::string r = fresh("cl");
            emit(r + " = call " + std::string(cg_ir(rty)) + " @" + fname + "(" + args + ")");
            return CgVal{rty, r, true};
        }
        if (auto* x = std::get_if<CCallResolver>(&e.v)) {
            std::optional<Access> aa = access_for(x->id);
            if (!aa) cg_unreachable("resolver in codegen");
            auto p = load_access(*aa);
            CgVal v = cg_expr(*x->arg);
            v = coerce_val(v, cg_of(x->inner));
            std::string mv = build_mv(v);
            emit("call void @marina_promise_resolve(ptr " + p.first + ", ptr " + mv + ")");
            return CgVal{CgTy::Void, std::string(), false};
        }
        if (std::get_if<CAwait>(&e.v)) {
            cg_unreachable("await is handled by the segment transform");
        }
        if (auto* x = std::get_if<CNewPromise>(&e.v)) {
            std::string pr = fresh("pn");
            emit(pr + " = call ptr @marina_promise_create()");
            std::string envp = build_env(x->lambda, pr);
            std::string own = fresh("po");
            emit(own + " = call ptr @marina_promise_run_executor(ptr " + pr +
                 ", ptr @lambda." + num(x->lambda) + ", ptr " + envp + ", ptr @lambda." +
                 num(x->lambda) + ".drop)");
            return CgVal{CgTy::Promise, own, true};
        }
        if (auto* x = std::get_if<CConsoleLog>(&e.v)) {
            CgVal v = cg_expr(*x->v);
            std::string mv = build_mv(v);
            emit("call void @marina_print(ptr " + mv + ")");
            return CgVal{CgTy::Void, std::string(), false};
        }
        if (auto* x = std::get_if<CSetTimeout>(&e.v)) {
            std::string envp = build_env(x->lambda, std::nullopt);
            CgVal m = cg_expr(*x->ms);
            std::string m64 = sext_i64(m);
            emit("call void @marina_set_timeout(ptr @lambda." + num(x->lambda) +
                 ", ptr " + envp + ", ptr @lambda." + num(x->lambda) + ".drop, i64 " +
                 m64 + ")");
            return CgVal{CgTy::Void, std::string(), false};
        }
        if (auto* x = std::get_if<CNetConnect>(&e.v)) {
            CgVal h = cg_expr(*x->host);
            std::string hmv = build_mv(h);
            CgVal p = cg_expr(*x->port);
            std::string pmv = build_mv(p);
            std::string pr = fresh("pr");
            emit(pr + " = call ptr @marina_net_connect_async(ptr " + hmv + ", ptr " +
                 pmv + ")");
            return CgVal{CgTy::Promise, pr, true};
        }
        if (auto* x = std::get_if<CNetWrite>(&e.v)) {
            CgVal s = cg_expr(*x->stream);
            std::string smv = build_mv(s);
            CgVal d = cg_expr(*x->data);
            std::string dmv = build_mv(d);
            std::string pr = fresh("pr");
            emit(pr + " = call ptr @marina_net_write_async(ptr " + smv + ", ptr " + dmv +
                 ")");
            return CgVal{CgTy::Promise, pr, true};
        }
        if (auto* x = std::get_if<CNetReadAll>(&e.v)) {
            CgVal s = cg_expr(*x->stream);
            std::string smv = build_mv(s);
            std::string pr = fresh("pr");
            emit(pr + " = call ptr @marina_net_read_all_async(ptr " + smv + ")");
            return CgVal{CgTy::Promise, pr, true};
        }
        if (auto* x = std::get_if<CNetClose>(&e.v)) {
            CgVal s = cg_expr(*x->stream);
            std::string smv = build_mv(s);
            std::string pr = fresh("pr");
            emit(pr + " = call ptr @marina_net_close_async(ptr " + smv + ")");
            return CgVal{CgTy::Promise, pr, true};
        }
        if (auto* x = std::get_if<CHttpCall>(&e.v)) {
            CgVal u = cg_expr(*x->url);
            std::string umv = build_mv(u);
            std::string bm;
            if (x->body.has_value()) {
                CgVal bv = cg_expr(**x->body);
                bm = build_mv(bv);
            } else {
                // GET: pass a void-tagged MValue as the body
                CgVal voidv;
                voidv.ty = CgTy::Void;
                voidv.ir = std::string();
                voidv.owned = false;
                bm = build_mv(voidv);
            }
            CgVal mlit = string_lit(x->method);
            CgVal mmv_val;
            mmv_val.ty = CgTy::Str;
            mmv_val.ir = mlit.ir;
            mmv_val.owned = true;
            std::string mmv = build_mv(mmv_val);
            size_t cls = num_classes();
            for (size_t i = 0; i < c->classes.size(); ++i) {
                if (c->classes[i].name == "http_Response") {
                    cls = i;
                    break;
                }
            }
            std::string pr = fresh("pr");
            emit(pr + " = call ptr @marina_http_request_async(ptr " + mmv + ", ptr " +
                 umv + ", ptr " + bm + ", i32 " +
                 std::to_string(static_cast<int32_t>(cls)) + ")");
            return CgVal{CgTy::Promise, pr, true};
        }
        if (auto* x = std::get_if<CHttpListen>(&e.v)) {
            CgVal p = cg_expr(*x->port);
            std::string pmv = build_mv(p);
            std::string envp = build_env(x->lambda, std::nullopt);
            size_t req_cls = num_classes();
            for (size_t i = 0; i < c->classes.size(); ++i) {
                if (c->classes[i].name == "http_Request") {
                    req_cls = i;
                    break;
                }
            }
            size_t res_cls = num_classes();
            for (size_t i = 0; i < c->classes.size(); ++i) {
                if (c->classes[i].name == "http_Res") {
                    res_cls = i;
                    break;
                }
            }
            emit("call void @marina_http_listen(ptr " + pmv + ", ptr @lambda." +
                 num(x->lambda) + ", ptr " + envp + ", ptr @lambda." + num(x->lambda) +
                 ".drop, i32 " + std::to_string(static_cast<int32_t>(req_cls)) +
                 ", i32 " + std::to_string(static_cast<int32_t>(res_cls)) + ")");
            return CgVal{CgTy::Void, std::string(), false};
        }
        if (std::get_if<CHttpNewServer>(&e.v)) {
            std::string obj = fresh("obj");
            emit(obj + " = call ptr @marina_object_new(i32 " +
                 num(static_cast<int32_t>(builtin_class("http_Server"))) + ", i64 0)");
            return CgVal{CgTy::Object, obj, true};
        }
        if (auto* x = std::get_if<CHttpRouter>(&e.v)) {
            CgVal s = cg_expr(*x->server);
            std::string smv = build_mv(s);
            (void)smv;
            CgVal pv = cg_expr(*x->path);
            std::string pmv = build_mv(pv);
            std::string obj = fresh("obj");
            emit(obj + " = call ptr @marina_object_new(i32 " +
                 num(static_cast<int32_t>(builtin_class("http_Router"))) + ", i64 0)");
            CgVal robj;
            robj.ty = CgTy::Object;
            robj.ir = obj;
            robj.owned = true;
            std::string rmv = build_mv(robj);
            emit("call void @marina_http_router_bind(ptr " + rmv + ", ptr " + pmv + ")");
            discard(s);
            return CgVal{CgTy::Object, obj, true};
        }
        if (auto* x = std::get_if<CHttpRouteAdd>(&e.v)) {
            CgVal r = cg_expr(*x->router);
            std::string rmv = build_mv(r);
            CgVal pv = cg_expr(*x->path);
            std::string pmv = build_mv(pv);
            CgVal mlit = string_lit(x->method);
            CgVal mmv_val;
            mmv_val.ty = CgTy::Str;
            mmv_val.ir = mlit.ir;
            mmv_val.owned = true;
            std::string mmv = build_mv(mmv_val);
            std::string envp = build_env(x->lambda, std::nullopt);
            int32_t aflag = x->is_async ? 1 : 0;
            emit("call void @marina_http_route_add(ptr " + rmv + ", ptr " + mmv +
                 ", ptr " + pmv + ", ptr @lambda." + num(x->lambda) + ", ptr " + envp +
                 ", ptr @lambda." + num(x->lambda) + ".drop, i32 " + num(static_cast<size_t>(aflag)) +
                 ")");
            discard(r);
            return CgVal{CgTy::Void, std::string(), false};
        }
        if (auto* x = std::get_if<CHttpServerListen>(&e.v)) {
            CgVal hv = cg_expr(*x->host);
            std::string hmv = build_mv(hv);
            CgVal pv = cg_expr(*x->port);
            std::string pmv = build_mv(pv);
            std::string pr = fresh("pn");
            emit(pr + " = call ptr @marina_promise_create()");
            emit("call void @marina_http_server_listen(ptr " + hmv + ", ptr " + pmv +
                 ", ptr " + pr + ", i32 " +
                 num(static_cast<int32_t>(builtin_class("http_Request"))) + ", i32 " +
                 num(static_cast<int32_t>(builtin_class("http_Res"))) + ")");
            return CgVal{CgTy::Promise, pr, true};
        }
        if (auto* x = std::get_if<CHttpValidatorNew>(&e.v)) {
            CgVal s = cg_expr(*x->schema);
            std::string smv = build_mv(s);
            std::string obj = fresh("obj");
            emit(obj + " = call ptr @marina_object_new(i32 " +
                 num(static_cast<int32_t>(builtin_class("http_JsonValidator"))) + ", i64 0)");
            emit("call void @marina_http_validator_init(ptr " + obj + ", ptr " + smv + ")");
            discard(s);
            return CgVal{CgTy::Object, obj, true};
        }
        if (auto* x = std::get_if<CHttpFieldNew>(&e.v)) {
            std::string obj = fresh("obj");
            emit(obj + " = call ptr @marina_object_new(i32 " +
                 num(static_cast<int32_t>(builtin_class("http_FieldValidator"))) + ", i64 0)");
            int32_t k = x->kind == FieldKind::Str ? 0 : 1;
            emit("call void @marina_http_field_init(ptr " + obj + ", i32 " +
                 num(static_cast<size_t>(k)) + ")");
            return CgVal{CgTy::Object, obj, true};
        }
        if (auto* x = std::get_if<CHttpFieldOp>(&e.v)) {
            CgVal vv = cg_expr(*x->v);
            std::string p = borrow(vv);
            std::string amv = "null";
            if (x->arg.has_value()) {
                CgVal av = cg_expr(**x->arg);
                amv = build_mv(av);
            }
            int32_t o = x->op == FieldOp::Required ? 0 : (x->op == FieldOp::Min ? 1 : 2);
            emit("call void @marina_http_field_op(ptr " + p + ", i32 " +
                 num(static_cast<size_t>(o)) + ", ptr " + amv + ")");
            // the same validator object flows on (chaining); the borrow
            // already queued its single release, so return it unowned
            return CgVal{vv.ty, vv.ir, false};
        }
        if (auto* x = std::get_if<CHttpValidate>(&e.v)) {
            CgVal vv = cg_expr(*x->v);
            std::string p = borrow(vv);
            CgVal bv = cg_expr(*x->body);
            std::string bmv = build_mv(bv);
            std::string out = alloca("%mv");
            emit("call void @marina_http_validate(ptr " + p + ", ptr " + bmv + ", i32 " +
                 num(x->out_class) + ", ptr " + out + ")");
            return mv_load(out, CgTy::Object);
        }
        if (auto* x = std::get_if<CResSend>(&e.v)) {
            CgVal r = cg_expr(*x->res);
            CgVal v = cg_expr(*x->value);
            std::string vmv = build_mv(v);
            const char* f = x->json ? "marina_http_res_json" : "marina_http_res_send";
            emit("call void @" + std::string(f) + "(ptr " + r.ir + ", ptr " + vmv + ")");
            return CgVal{CgTy::Void, std::string(), false};
        }
        if (auto* x = std::get_if<CArrayLit>(&e.v)) {
            int32_t tag = mv_tag_of(x->elem_ty);
            std::string arr = fresh("arr");
            emit(arr + " = call ptr @marina_array_new(i32 " +
                 num(static_cast<size_t>(tag)) + ")");
            CgTy ect = cg_of(x->elem_ty);
            for (const auto& el : x->elems) {
                CgVal v = cg_expr(el);
                v = coerce_val(v, ect);
                v = to_owned(v);
                std::string mv = build_mv(v);
                emit("call i64 @marina_array_push(ptr " + arr + ", ptr " + mv + ")");
                discard(v);
            }
            return CgVal{CgTy::Array, arr, true};
        }
        if (auto* x = std::get_if<CIndex>(&e.v)) {
            CgVal o = cg_expr(*x->obj);
            std::string p = borrow(o);
            CgVal iv = cg_expr(*x->idx);
            std::string i = sext_i64(iv);
            std::string out = alloca("%mv");
            emit("call void @marina_array_get(ptr " + p + ", i64 " + i + ", ptr " + out +
                 ")");
            CgVal v = mv_load(out, cg_of(x->ty));
            discard(o);
            return v;
        }
        if (auto* x = std::get_if<CArrayOp>(&e.v)) {
            CgVal a = cg_expr(*x->arr);
            std::string p = borrow(a);
            CgVal v;
            switch (x->op) {
                case ArrayOpKind::Len: {
                    std::string n = fresh("an");
                    emit(n + " = call i64 @marina_array_len(ptr " + p + ")");
                    std::string t = fresh("al");
                    emit(t + " = trunc i64 " + n + " to i32");
                    v = CgVal{CgTy::Int, t, false};
                    break;
                }
                case ArrayOpKind::Push:
                case ArrayOpKind::Unshift: {
                    // element type comes from the array expression
                    std::optional<CgTy> elem;
                    if (auto t = cexpr_cty(*x->arr)) {
                        if (t->tag == CTy::Tag::Array && t->inner) {
                            elem = cg_of(*t->inner);
                        } else {
                            elem = cg_of(*t);
                        }
                    }
                    CgVal av = cg_expr(x->args[0]);
                    if (elem.has_value()) av = coerce_val(av, *elem);
                    av = to_owned(av);
                    std::string mv = build_mv(av);
                    const char* f =
                        x->op == ArrayOpKind::Push ? "marina_array_push" : "marina_array_unshift";
                    std::string n = fresh("an");
                    emit(n + " = call i64 @" + f + "(ptr " + p + ", ptr " + mv + ")");
                    std::string t = fresh("al");
                    emit(t + " = trunc i64 " + n + " to i32");
                    discard(av);
                    v = CgVal{CgTy::Int, t, false};
                    break;
                }
                case ArrayOpKind::Pop:
                case ArrayOpKind::Shift: {
                    std::string out = alloca("%mv");
                    const char* f =
                        x->op == ArrayOpKind::Pop ? "marina_array_pop" : "marina_array_shift";
                    emit(std::string("call void @") + f + "(ptr " + p + ", ptr " + out + ")");
                    v = mv_load(out, cg_of(x->ret));
                    break;
                }
                case ArrayOpKind::Splice:
                case ArrayOpKind::Slice: {
                    CgVal s = cg_expr(x->args[0]);
                    std::string s64 = sext_i64(s);
                    CgVal ev = cg_expr(x->args[1]);
                    std::string e64 = sext_i64(ev);
                    const char* f = x->op == ArrayOpKind::Splice ? "marina_array_splice"
                                                                 : "marina_array_slice";
                    std::string r = fresh("ar");
                    emit(r + " = call ptr @" + f + "(ptr " + p + ", i64 " + s64 + ", i64 " +
                         e64 + ")");
                    v = CgVal{CgTy::Array, r, true};
                    break;
                }
                case ArrayOpKind::Concat: {
                    CgVal b = cg_expr(x->args[0]);
                    std::string r = fresh("ar");
                    emit(r + " = call ptr @marina_array_concat(ptr " + p + ", ptr " + b.ir +
                         ")");
                    discard(b);
                    v = CgVal{CgTy::Array, r, true};
                    break;
                }
                case ArrayOpKind::Join: {
                    CgVal s = cg_expr(x->args[0]);
                    std::string r = fresh("js");
                    emit(r + " = call ptr @marina_array_join(ptr " + p + ", ptr " + s.ir +
                         ")");
                    discard(s);
                    v = CgVal{CgTy::Str, r, true};
                    break;
                }
                case ArrayOpKind::Reverse:
                case ArrayOpKind::Sort: {
                    const char* f = x->op == ArrayOpKind::Reverse ? "marina_array_reverse"
                                                                  : "marina_array_sort";
                    std::string r = fresh("ar");
                    emit(r + " = call ptr @" + f + "(ptr " + p + ")");
                    v = CgVal{CgTy::Array, r, true};
                    break;
                }
            }
            discard(a);
            return v;
        }
        if (auto* x = std::get_if<CArrayHof>(&e.v)) {
            CgVal a = cg_expr(*x->arr);
            std::string p = borrow(a);
            const CLambdaVal* cb = std::get_if<CLambdaVal>(&x->cb->v);
            if (!cb) cg_unreachable("array callback must be a lambda");
            size_t li = cb->li;
            std::string envp = build_env(li, std::nullopt);
            CgVal v;
            switch (x->op) {
                case HofKind::ForEach: {
                    emit("call void @marina_array_foreach(ptr " + p + ", ptr @lambda." +
                         num(li) + ", ptr " + envp + ", ptr @lambda." + num(li) + ".drop)");
                    v = CgVal{CgTy::Void, std::string(), false};
                    break;
                }
                case HofKind::Map: {
                    if (x->ret.tag != CTy::Tag::Array || !x->ret.inner) {
                        cg_unreachable("map returns Array");
                    }
                    int32_t out_tag = mv_tag_of(*x->ret.inner);
                    std::string r = fresh("ar");
                    emit(r + " = call ptr @marina_array_map(ptr " + p + ", ptr @lambda." +
                         num(li) + ", ptr " + envp + ", ptr @lambda." + num(li) +
                         ".drop, i32 " + num(static_cast<size_t>(out_tag)) + ")");
                    v = CgVal{CgTy::Array, r, true};
                    break;
                }
                case HofKind::Filter: {
                    std::string r = fresh("ar");
                    emit(r + " = call ptr @marina_array_filter(ptr " + p + ", ptr @lambda." +
                         num(li) + ", ptr " + envp + ", ptr @lambda." + num(li) + ".drop)");
                    v = CgVal{CgTy::Array, r, true};
                    break;
                }
                case HofKind::Reduce: {
                    CgTy rt = cg_of(x->ret);
                    if (!x->extra.has_value()) cg_unreachable("reduce needs an init value");
                    CgVal init = cg_expr(**x->extra);
                    init = coerce_val(init, rt);
                    init = to_owned(init);
                    std::string imv = build_mv(init);
                    std::string out = alloca("%mv");
                    emit("call void @marina_array_reduce(ptr " + p + ", ptr @lambda." +
                         num(li) + ", ptr " + envp + ", ptr @lambda." + num(li) +
                         ".drop, ptr " + imv + ", ptr " + out + ")");
                    CgVal rv = mv_load(out, rt);
                    discard(init);
                    v = rv;
                    break;
                }
            }
            discard(a);
            return v;
        }
        if (auto* x = std::get_if<CStrLen>(&e.v)) {
            CgVal s = cg_expr(*x->v);
            std::string p = borrow(s);
            std::string n = fresh("sl");
            emit(n + " = call i64 @marina_string_len(ptr " + p + ")");
            std::string t = fresh("slt");
            emit(t + " = trunc i64 " + n + " to i32");
            discard(s);
            return CgVal{CgTy::Int, t, false};
        }
        if (auto* x = std::get_if<CStrMethod>(&e.v)) {
            CgVal s = cg_expr(*x->obj);
            std::string p = borrow(s);
            CgVal v;
            if (x->op == StrOpKind::CharAt) {
                CgVal iv = cg_expr(x->args[0]);
                std::string i = sext_i64(iv);
                std::string n = fresh("sc");
                emit(n + " = call i64 @marina_string_charcode_at(ptr " + p + ", i64 " + i +
                     ")");
                std::string t = fresh("sct");
                emit(t + " = trunc i64 " + n + " to i32");
                v = CgVal{CgTy::Int, t, false};
            } else {
                CgVal av = cg_expr(x->args[0]);
                std::string a64 = sext_i64(av);
                CgVal bv = cg_expr(x->args[1]);
                std::string b64 = sext_i64(bv);
                std::string r = fresh("ss");
                emit(r + " = call ptr @marina_string_substring(ptr " + p + ", i64 " + a64 +
                     ", i64 " + b64 + ")");
                v = CgVal{CgTy::Str, r, true};
            }
            // borrow already queued the string's single release
            return v;
        }
        if (std::get_if<CFsArgs>(&e.v)) {
            std::string r = fresh("arg");
            emit(r + " = call ptr @marina_args()");
            return CgVal{CgTy::Array, r, true};
        }
        if (std::get_if<CLambdaVal>(&e.v)) {
            cg_unreachable("lambda values are consumed by call sites");
        }
        if (auto* x = std::get_if<CObjectLit>(&e.v)) {
            size_t n = c->classes[x->class_id].fields.size();
            std::string obj = fresh("obj");
            emit(obj + " = call ptr @marina_object_new(i32 " + num(x->class_id) + ", i64 " +
                 num(n) + ")");
            for (const auto& [idx, expr] : x->inits) {
                CTy fty = c->classes[x->class_id].fields[idx].second;
                CgVal v = cg_expr(expr);
                v = coerce_val(v, cg_of(fty));
                v = to_owned(v);
                std::string mv = build_mv(v);
                emit("call void @marina_object_set(ptr " + obj + ", i64 " + num(idx) +
                     ", ptr " + mv + ")");
                discard(v);
            }
            return CgVal{CgTy::Object, obj, true};
        }
        if (auto* x = std::get_if<CNewClass>(&e.v)) {
            size_t n = c->classes[x->class_id].fields.size();
            std::string obj = fresh("obj");
            emit(obj + " = call ptr @marina_object_new(i32 " + num(x->class_id) + ", i64 " +
                 num(n) + ")");
            return CgVal{CgTy::Object, obj, true};
        }
        if (auto* x = std::get_if<CNewClassCtor>(&e.v)) {
            size_t n = c->classes[x->class_id].fields.size();
            std::string obj = fresh("obj");
            emit(obj + " = call ptr @marina_object_new(i32 " + num(x->class_id) + ", i64 " +
                 num(n) + ")");
            auto it = fn_sigs.find(x->fn);
            if (it == fn_sigs.end()) cg_unreachable("missing ctor sig for " + x->fn);
            const Sig& sig = it->second;
            // the constructor consumes its `this` argument (like any owned
            // parameter), so pass a retained reference — the fresh instance
            // stays owned by the caller
            CgVal self{CgTy::Object, obj, false};
            self = to_owned(self);
            std::vector<std::string> call_args;
            call_args.push_back(std::string(cg_ir(CgTy::Object)) + " " + self.ir);
            for (size_t i = 0; i < x->args.size(); ++i) {
                CgVal v = cg_expr(x->args[i]);
                CgTy pt = i + 1 < sig.params.size() ? sig.params[i + 1] : sig.ret;
                v = coerce_val(v, pt);
                v = to_owned(v);
                call_args.push_back(std::string(cg_ir(pt)) + " " + v.ir);
            }
            std::string args = call_args.empty() ? std::string() : call_args[0];
            for (size_t i = 1; i < call_args.size(); ++i) args += ", " + call_args[i];
            if (sig.ret == CgTy::Void) {
                emit("call void @fn." + x->fn + "(" + args + ")");
            } else {
                std::string r = fresh("ctor");
                emit(r + " = call " + std::string(cg_ir(sig.ret)) + " @fn." + x->fn + "(" +
                     args + ")");
                discard(CgVal{sig.ret, r, true});
            }
            return CgVal{CgTy::Object, obj, true};
        }
        if (auto* x = std::get_if<CFieldGet>(&e.v)) {
            CgVal o = cg_expr(*x->obj);
            std::string p = borrow(o);
            std::string out = alloca("%mv");
            emit("call void @marina_object_get(ptr " + p + ", i64 " + num(x->idx) +
                 ", ptr " + out + ")");
            CgVal v = mv_load(out, cg_of(x->ty));
            discard(o);
            return v;
        }
        if (auto* x = std::get_if<CIfaceGet>(&e.v)) {
            // interface-typed values resolve fields by name at runtime
            CgVal o = cg_expr(*x->obj);
            std::string p = borrow(o);
            CgVal sv = string_lit(x->field);
            std::string nmv = build_mv(sv);
            std::string out = alloca("%mv");
            emit("call void @marina_object_get_field(ptr " + p + ", ptr " + nmv + ", ptr " +
                 out + ")");
            CgVal v = mv_load(out, cg_of(x->ty));
            discard(sv);
            discard(o);
            return v;
        }
        if (std::get_if<CIfaceSetExpr>(&e.v)) {
            // never generated (interface writes are statements); keeps the match exhaustive
            return CgVal{CgTy::Void, std::string(), false};
        }
        if (auto* x = std::get_if<CFsReadFile>(&e.v)) {
            CgVal p = cg_expr(*x->path);
            std::string pmv = build_mv(p);
            std::string pr = fresh("pr");
            emit(pr + " = call ptr @marina_fs_read_file_async(ptr " + pmv + ")");
            return CgVal{CgTy::Promise, pr, true};
        }
        if (auto* x = std::get_if<CFsWriteFile>(&e.v)) {
            CgVal p = cg_expr(*x->path);
            std::string pmv = build_mv(p);
            CgVal d = cg_expr(*x->data);
            std::string dmv = build_mv(d);
            std::string pr = fresh("pr");
            emit(pr + " = call ptr @marina_fs_write_file_async(ptr " + pmv + ", ptr " +
                 dmv + ")");
            discard(d);
            return CgVal{CgTy::Promise, pr, true};
        }
        if (auto* x = std::get_if<CFsExists>(&e.v)) {
            CgVal p = cg_expr(*x->path);
            std::string pmv = build_mv(p);
            std::string pr = fresh("pr");
            emit(pr + " = call ptr @marina_fs_exists_async(ptr " + pmv + ")");
            return CgVal{CgTy::Promise, pr, true};
        }
        if (auto* x = std::get_if<CFsReadDir>(&e.v)) {
            CgVal p = cg_expr(*x->path);
            std::string pmv = build_mv(p);
            std::string pr = fresh("pr");
            emit(pr + " = call ptr @marina_fs_read_dir_async(ptr " + pmv + ")");
            return CgVal{CgTy::Promise, pr, true};
        }
        if (auto* x = std::get_if<CFsMkdir>(&e.v)) {
            CgVal p = cg_expr(*x->path);
            std::string pmv = build_mv(p);
            std::string pr = fresh("pr");
            emit(pr + " = call ptr @marina_fs_mkdir_async(ptr " + pmv + ")");
            return CgVal{CgTy::Promise, pr, true};
        }
        if (auto* x = std::get_if<CFsRemove>(&e.v)) {
            CgVal p = cg_expr(*x->path);
            std::string pmv = build_mv(p);
            std::string pr = fresh("pr");
            emit(pr + " = call ptr @marina_fs_remove_async(ptr " + pmv + ")");
            return CgVal{CgTy::Promise, pr, true};
        }
        if (auto* x = std::get_if<CCryptoSha256>(&e.v)) {
            CgVal d = cg_expr(*x->v);
            std::string dmv = build_mv(d);
            std::string out = alloca("%mv");
            emit("call void @marina_crypto_sha256(ptr " + dmv + ", ptr " + out + ")");
            discard(d);
            return mv_load(out, CgTy::Str);
        }
        if (auto* x = std::get_if<CCryptoBase64>(&e.v)) {
            CgVal d = cg_expr(*x->v);
            std::string dmv = build_mv(d);
            std::string out = alloca("%mv");
            emit("call void @marina_crypto_base64(i1 " + std::string(x->decode ? "true" : "false") +
                 ", ptr " + dmv + ", ptr " + out + ")");
            discard(d);
            return mv_load(out, CgTy::Str);
        }
        if (auto* x = std::get_if<CCryptoRandomHex>(&e.v)) {
            CgVal b = cg_expr(*x->bytes);
            std::string n = fresh("nb");
            emit(n + " = sext i32 " + b.ir + " to i64");
            std::string out = alloca("%mv");
            emit("call void @marina_crypto_random_hex(i64 " + n + ", ptr " + out + ")");
            discard(b);
            return mv_load(out, CgTy::Str);
        }
        if (std::get_if<CCryptoUuid>(&e.v)) {
            std::string out = alloca("%mv");
            emit("call void @marina_crypto_uuid(ptr " + out + ")");
            return mv_load(out, CgTy::Str);
        }
        if (auto* x = std::get_if<CSqlOpen>(&e.v)) {
            CgVal d = cg_expr(*x->driver);
            std::string dmv = build_mv(d);
            CgVal u = cg_expr(*x->url);
            std::string umv = build_mv(u);
            int32_t cls = builtin_class("sql_Connection");
            std::string out = alloca("%mv");
            emit("call void @marina_sql_open(ptr " + dmv + ", ptr " + umv + ", i32 " +
                 num(static_cast<int32_t>(cls)) + ", ptr " + out + ")");
            discard(d);
            discard(u);
            return mv_load(out, CgTy::Object);
        }
        if (auto* x = std::get_if<CSqlExec>(&e.v)) {
            CgVal cv = cg_expr(*x->conn);
            // cg_sql_call_async borrows the connection (single queued release)
            std::string pr = cg_sql_call_async(cv, x->sql, x->binds, x->result_class);
            return CgVal{CgTy::Promise, pr, true};
        }
        if (auto* x = std::get_if<CSqlQuery>(&e.v)) {
            CgVal cv = cg_expr(*x->conn);
            std::string pr = cg_sql_call_async(cv, x->sql, x->binds, std::nullopt);
            return CgVal{CgTy::Promise, pr, true};
        }
        if (auto* x = std::get_if<CSqlClose>(&e.v)) {
            CgVal cv = cg_expr(*x->conn);
            std::string p = borrow(cv);
            emit("call void @marina_sql_close(ptr " + p + ")");
            return CgVal{CgTy::Void, std::string(), false};
        }
        cg_unreachable("unhandled expression kind in codegen");
    }

    // Async variant of `cg_sql_call`: emits the `_async` runtime call and
    // returns the promise pointer.
    std::string cg_sql_call_async(CgVal& conn, const std::string& sql,
                                  const std::vector<CExpr>& binds,
                                  std::optional<size_t> result_class) {
        std::string p = borrow(conn);
        CgVal sv = string_lit(sql);
        std::string smv = build_mv(sv);
        size_t n = binds.size() > 1 ? binds.size() : 1;
        std::string buf = fresh("bnds");
        fns[cur].allocas.push_back(buf + " = alloca %mv, i64 " + num(n));
        for (size_t i = 0; i < binds.size(); ++i) {
            CgVal bv = cg_expr(binds[i]);
            std::string bmv = build_mv(bv);
            std::string ld = fresh("bl");
            emit(ld + " = load %mv, ptr " + bmv);
            std::string slot = fresh("bs");
            emit(slot + " = getelementptr inbounds %mv, ptr " + buf + ", i64 " + num(i));
            emit("store %mv " + ld + ", ptr " + slot);
            discard(bv);
        }
        std::string pr = fresh("pr");
        if (result_class.has_value()) {
            emit(pr + " = call ptr @marina_sql_execute_async(ptr " + p + ", ptr " + smv +
                 ", i64 " + num(binds.size()) + ", ptr " + buf + ", i32 " +
                 num(*result_class) + ")");
        } else {
            emit(pr + " = call ptr @marina_sql_query_async(ptr " + p + ", ptr " + smv +
                 ", i64 " + num(binds.size()) + ", ptr " + buf + ")");
        }
        discard(sv);
        return pr;
    }

    // Lambda layout: executor params first (Plain ABI only), then captured
    // variables. Callback lambdas take their parameters as IR arguments, so
    // only captures occupy env slots.
    std::vector<std::pair<size_t, CTy>> lambda_slots(size_t li) const {
        const CLambda& lam = c->lambdas[li];
        std::vector<std::pair<size_t, CTy>> slots;
        if (lam.abi == LambdaAbi::Plain) {
            for (const auto& p : lam.params) slots.push_back(p);
        }
        for (const auto& cp : lam.captures) slots.push_back(cp);
        return slots;
    }

    std::string env_type_def(size_t li) {
        std::string ty = "%env." + num(li);
        if (env_types_done.insert(li).second) {
            std::vector<std::string> tys;
            for (const auto& [id, t] : lambda_slots(li)) {
                tys.push_back(cg_mem(cg_of(t)));
            }
            std::string joined;
            for (size_t i = 0; i < tys.size(); ++i) {
                if (i) joined += ", ";
                joined += tys[i];
            }
            type_defs.push_back(ty + " = type { " + joined + " }");
        }
        return ty;
    }

    // Class id of a builtin std-module class (http_Server, …).
    int32_t builtin_class(const std::string& name) const {
        for (size_t i = 0; i < c->classes.size(); ++i) {
            if (c->classes[i].name == name) return static_cast<int32_t>(i);
        }
        return -1;  // usize::MAX as i32 in the Rust original
    }

    size_t num_classes() const { return c->classes.size(); }

    std::string build_env(size_t li, std::optional<std::string> preset) {
        auto slots = lambda_slots(li);
        std::string ty = env_type_def(li);

        std::string sz;
        if (slots.empty()) {
            sz = "i64 8";
        } else {
            sz = "i64 ptrtoint (ptr getelementptr inbounds (" + ty +
                 ", ptr null, i64 1) to i64)";
        }
        std::string envp = fresh("en");
        emit(envp + " = call ptr @marina_closure_alloc(" + sz + ")");

        for (size_t i = 0; i < slots.size(); ++i) {
            size_t id = slots[i].first;
            CgTy ct = cg_of(slots[i].second);
            // address this env directly (self.env is not set in the
            // enclosing function, so we can't use gep_ssa here)
            std::string gep = fresh("eg");
            emit(gep + " = getelementptr inbounds " + ty + ", ptr " + envp + ", i64 0, i32 " +
                 num(i));
            if (i == 0 && preset.has_value()) {
                // env slots own their refs: retain the preset promise
                std::string r = fresh("ep");
                emit(r + " = call ptr @marina_retain(ptr " + *preset + ")");
                emit("store ptr " + r + ", ptr " + gep);
                continue;
            }
            std::optional<Access> aa = access_for(id);
            if (!aa) cg_unreachable("capture not found in codegen");
            auto v = load_access(*aa);
            if (cg_is_ptr(ct)) {
                std::string r = fresh("cp");
                emit(r + " = call ptr @marina_retain(ptr " + v.first + ")");
                emit("store ptr " + r + ", ptr " + gep);
            } else {
                emit("store " + std::string(cg_ir(ct)) + " " + v.first + ", ptr " + gep);
            }
        }
        return envp;
    }

    // ------------------------------------------------------------------
    // Lambdas
    // ------------------------------------------------------------------

    void gen_lambda(size_t li) {
        const CLambda& lam = c->lambdas[li];
        if (lam.is_async && lam.abi == LambdaAbi::Listen) {
            // async route handler: shim + promise state machine
            gen_async_listen_lambda(li);
            return;
        }
        LambdaAbi abi = lam.abi;
        CTy lret = lam.ret;
        auto slots = lambda_slots(li);
        std::vector<CStmt> body = c->lambdas[li].body;
        std::string ty = env_type_def(li);

        // ---- lambda body ----
        // IR signature depends on the callback ABI; extra params are raw
        // pointers to MValues passed by the runtime.
        std::vector<std::string> ir_params{"ptr %env"};
        switch (abi) {
            case LambdaAbi::Plain: break;
            case LambdaAbi::ForEach: ir_params.push_back("ptr %item"); break;
            case LambdaAbi::MapFilter:
                ir_params.push_back("ptr %item");
                ir_params.push_back("ptr %out");
                break;
            case LambdaAbi::Reduce:
                ir_params.push_back("ptr %acc");
                ir_params.push_back("ptr %item");
                ir_params.push_back("ptr %out");
                break;
            case LambdaAbi::Listen:
                ir_params.push_back("ptr %req");
                ir_params.push_back("ptr %res");
                break;
        }
        IrFn f;
        f.name = "lambda." + num(li);
        f.ret = "void";
        f.params = ir_params;
        f.blocks.push_back(IrBlock{"entry", {}, "", false});
        push_fn(std::move(f));
        cur_ret = CgTy::Void;
        size_t exit_b = new_block_fixed("exit");
        lambda_exit = exit_b;
        {
            EnvInfo info;
            info.ty = ty;
            for (const auto& [id, t] : slots) info.slot_tys.push_back(cg_of(t));
            for (size_t i = 0; i < slots.size(); ++i) info.map.emplace(slots[i].first, i);
            env = std::move(info);
        }
        for (size_t i = 0; i < slots.size(); ++i) {
            Access a;
            a.kind = Access::Kind::Env;
            a.idx = i;
            a.ty = cg_of(slots[i].second);
            scopes[0].insert(slots[i].first, a);
        }
        // Callback parameters arrive as MValue pointers; unpack them into
        // allocas owned by the lambda body. The runtime retains string/array
        // payloads before the call, so these start out owned.
        if (abi != LambdaAbi::Plain) {
            const CLambda& lam2 = c->lambdas[li];
            std::vector<const char*> arg_names;
            switch (abi) {
                case LambdaAbi::ForEach: arg_names = {"%item"}; break;
                case LambdaAbi::MapFilter: arg_names = {"%item"}; break;
                case LambdaAbi::Reduce: arg_names = {"%acc", "%item"}; break;
                case LambdaAbi::Listen: arg_names = {"%req", "%res"}; break;
                case LambdaAbi::Plain: arg_names = {}; break;
            }
            for (size_t i = 0; i < lam2.params.size(); ++i) {
                size_t id = lam2.params[i].first;
                CgTy ct = cg_of(lam2.params[i].second);
                std::string a = alloca(cg_mem(ct));
                store_from_mv(arg_names[i], ct, a);
                Access acc;
                acc.kind = Access::Kind::Alloca;
                acc.name = a;
                acc.ty = ct;
                scopes[0].insert(id, acc);
            }
            if (abi == LambdaAbi::MapFilter || abi == LambdaAbi::Reduce) {
                lambda_out = std::string("%out");
                lambda_ret = cg_of(lret);
            }
        }
        cg_body(body);
        if (!terminated()) {
            if (lambda_out.has_value()) {
                std::string out = *lambda_out;
                // fell off the end of a map/filter/reduce callback: write a
                // void value (the checker requires a return; defensive)
                emit("store i32 0, ptr " + out);
                std::string g = mv_field(out, 1);
                emit("store i32 0, ptr " + g);
                flush_pending();
                release_scope_vars();
                std::string el = label_of(exit_b);
                term("br label %" + el);
            } else {
                sync_epilogue(std::nullopt);
            }
        }
        switch_block(exit_b);
        term("ret void");
        env.reset();
        lambda_out.reset();
        lambda_exit.reset();
        lambda_ret.reset();

        // ---- drop function (called by the runtime when the env dies) ----
        IrFn drop;
        drop.name = "lambda." + num(li) + ".drop";
        drop.ret = "void";
        drop.params = {"ptr %env"};
        drop.blocks.push_back(IrBlock{"entry", {}, "", false});
        push_fn(std::move(drop));
        {
            EnvInfo info;
            info.ty = ty;
            for (const auto& [id, t] : slots) info.slot_tys.push_back(cg_of(t));
            for (size_t i = 0; i < slots.size(); ++i) info.map.emplace(slots[i].first, i);
            env = std::move(info);
        }
        for (size_t i = 0; i < slots.size(); ++i) {
            CgTy ct = cg_of(slots[i].second);
            if (cg_is_ptr(ct)) {
                Access a;
                a.kind = Access::Kind::Env;
                a.idx = i;
                a.ty = ct;
                std::string gep = gep_ssa(a);
                std::string v = fresh("dl");
                emit(v + " = load ptr, ptr " + gep);
                emit("call void @marina_release(ptr " + v + ")");
            }
        }
        emit("call void @marina_closure_free(ptr %env)");
        term("ret void");
        env.reset();
    }

    // ------------------------------------------------------------------
    // Sync functions
    // ------------------------------------------------------------------

    void gen_sync_fn_into(const std::string& name,
                          const std::vector<std::pair<size_t, CTy>>& params,
                          const CTy& ret, const std::vector<CStmt>& body) {
        std::string ir_ret = cg_ir(cg_of(ret));
        std::vector<std::string> ir_params;
        for (size_t i = 0; i < params.size(); ++i) {
            ir_params.push_back(std::string(cg_ir(cg_of(params[i].second))) + " %a" + num(i));
        }
        IrFn f;
        f.name = name;
        f.ret = ir_ret;
        f.params = ir_params;
        f.blocks.push_back(IrBlock{"entry", {}, "", false});
        push_fn(std::move(f));
        cur_ret = cg_of(ret);
        for (size_t i = 0; i < params.size(); ++i) {
            CgTy ct = cg_of(params[i].second);
            Access a;
            a.kind = Access::Kind::Alloca;
            a.name = alloca(cg_mem(ct));
            a.ty = ct;
            store_access(a, "%a" + num(i), ct);
            scopes[0].insert(params[i].first, a);
        }
        cg_body(body);
        if (!terminated()) {
            sync_epilogue(std::nullopt);
        }
    }

    void gen_sync_fn(const CFn& f) {
        std::string name = "fn." + f.name;
        gen_sync_fn_into(name, f.params, f.ret, f.body);
    }

    // An `async (req, res) => { ... }` route handler compiles to:
    //   * `lambda.N` — the Listen-ABI shim the runtime registers; returns
    //     the handler's promise,
    //   * `lambda.N.async` — state machine entry: creates the promise,
    //     copies captures + (req, res) into the frame, runs segment 0,
    //   * `lambda.N.async.resume` — continuations after each await,
    //   * `lambda.N.drop` — env cleanup (unchanged).
    void gen_async_listen_lambda(size_t li) {
        CTy lret = c->lambdas[li].ret;
        auto captures = lambda_slots(li);  // Listen ABI: captures only
        auto lam_params = c->lambdas[li].params;
        std::vector<CStmt> body = c->lambdas[li].body;
        std::string fty = "%frame.lambda." + num(li);
        std::string entry_name = "lambda." + num(li) + ".async";

        // ---- 1. shim: the runtime calls this; it returns the promise ----
        IrFn shim;
        shim.name = "lambda." + num(li);
        shim.ret = "ptr";
        shim.params = {"ptr %env", "ptr %req", "ptr %res"};
        shim.blocks.push_back(IrBlock{"entry", {}, "", false});
        push_fn(std::move(shim));
        cur_ret = CgTy::Promise;
        std::string sp = fresh("sp");
        emit(sp + " = call ptr @" + entry_name + "(ptr %env, ptr %req, ptr %res)");
        term("ret ptr " + sp);

        // ---- 2. state machine frame ----
        {
            FrameInfo info;
            info.ty = fty;
            info.slots = {CgTy::Promise};
            frame = std::move(info);
        }
        std::string env_ty = env_type_def(li);
        {
            EnvInfo info;
            info.ty = env_ty;
            for (const auto& [id, t] : captures) info.slot_tys.push_back(cg_of(t));
            for (size_t i = 0; i < captures.size(); ++i) info.map.emplace(captures[i].first, i);
            env = std::move(info);
        }

        // ---- 3. entry: create promise + frame, unpack, run segment 0 ----
        IrFn entry;
        entry.name = entry_name;
        entry.ret = "ptr";
        entry.params = {"ptr %env", "ptr %req", "ptr %res"};
        entry.frame_ty = fty;
        entry.blocks.push_back(IrBlock{"entry", {}, "", false});
        push_fn(std::move(entry));
        cur_ret = CgTy::Promise;
        std::string pr = fresh("pr");
        emit(pr + " = call ptr @marina_promise_create()");
        {
            Access fa;
            fa.kind = Access::Kind::Frame;
            fa.idx = 0;
            fa.ty = CgTy::Promise;
            std::string gep = gep_ssa(fa);
            emit("store ptr " + pr + ", ptr " + gep);
        }
        // captures: the env owns its refs; the frame gets its own (retained)
        for (size_t i = 0; i < captures.size(); ++i) {
            size_t id = captures[i].first;
            CgTy ct = cg_of(captures[i].second);
            size_t slot = slot_for(id, ct);
            Access fa;
            fa.kind = Access::Kind::Frame;
            fa.idx = slot;
            fa.ty = ct;
            std::string gep = gep_ssa(fa);
            if (cg_is_ptr(ct)) {
                Access ea;
                ea.kind = Access::Kind::Env;
                ea.idx = i;
                ea.ty = ct;
                std::string egep = gep_ssa(ea);
                std::string v = fresh("cv");
                emit(v + " = load ptr, ptr " + egep);
                std::string r = fresh("cr");
                emit(r + " = call ptr @marina_retain(ptr " + v + ")");
                emit("store ptr " + r + ", ptr " + gep);
            } else {
                Access ea;
                ea.kind = Access::Kind::Env;
                ea.idx = i;
                ea.ty = ct;
                std::string egep = gep_ssa(ea);
                std::string v = fresh("cv");
                emit(v + " = load " + cg_ir(ct) + ", ptr " + egep);
                emit("store " + std::string(cg_ir(ct)) + " " + v + ", ptr " + gep);
            }
        }
        // Listen-ABI params arrive as pointers to MValues (rc 1 handed over)
        for (size_t i = 0; i < lam_params.size(); ++i) {
            CgTy ct = cg_of(lam_params[i].second);
            size_t slot = slot_for(lam_params[i].first, ct);
            Access fa;
            fa.kind = Access::Kind::Frame;
            fa.idx = slot;
            fa.ty = ct;
            std::string gep = gep_ssa(fa);
            const char* src = i == 0 ? "%req" : "%res";
            store_from_mv(src, ct, gep);
        }
        std::string rv = fresh("rv");
        emit(rv + " = call ptr @marina_retain(ptr " + pr + ")");
        emit("call void @" + entry_name + ".resume(ptr %fr, i32 0, ptr null)");
        term("ret ptr " + rv);

        // ---- 4. resume: continuations (mirrors gen_async_fn) ----
        std::vector<Segment> segs;
        transform(std::move(body), segs);
        IrFn resume;
        resume.name = entry_name + ".resume";
        resume.ret = "void";
        resume.params = {"ptr %fr", "i32 %st", "ptr %val"};
        resume.blocks.push_back(IrBlock{"pro", {}, "", false});
        push_fn(std::move(resume));
        cur_ret = CgTy::Void;

        std::vector<std::optional<size_t>> vblock;
        std::vector<size_t> sblock;
        for (size_t k = 0; k < segs.size(); ++k) {
            if (k > 0) {
                vblock.push_back(new_block_fixed("seg." + num(k) + "v"));
            } else {
                vblock.push_back(std::nullopt);
            }
            sblock.push_back(new_block_fixed("seg." + num(k)));
        }
        std::vector<std::optional<std::pair<size_t, CTy>>> await_targets;
        for (const auto& s : segs) {
            if (s.end.kind == SegEnd::Kind::Await) {
                await_targets.push_back(s.end.target);
            } else {
                await_targets.push_back(std::nullopt);
            }
        }
        for (size_t k = 0; k < segs.size(); ++k) {
            Segment& seg = segs[k];
            if (k > 0) {
                switch_block(*vblock[k]);
                if (await_targets[k - 1].has_value()) {
                    auto [id, ty] = *await_targets[k - 1];
                    materialize_await_value(id, ty);
                }
                std::string sl = label_of(sblock[k]);
                term("br label %" + sl);
            }
            switch_block(sblock[k]);
            scopes.emplace_back();
            cg_body(seg.stmts);
            if (!terminated()) {
                switch (seg.end.kind) {
                    case SegEnd::Kind::Await: {
                        CgVal v = cg_expr(seg.end.expr);
                        v = to_owned(v);
                        emit("call void @marina_await(ptr " + v.ir + ", ptr @" +
                             entry_name + ".resume, ptr %fr, i32 " + num(seg.end.next) + ")");
                        flush_pending();
                        term("ret void");
                        break;
                    }
                    case SegEnd::Kind::ReturnExpr: {
                        CgVal v = cg_expr(seg.end.expr);
                        v = coerce_val(v, cg_of(lret));
                        finish_async(std::move(v));
                        break;
                    }
                    case SegEnd::Kind::ReturnVoid:
                        finish_async(std::nullopt);
                        break;
                    case SegEnd::Kind::ResolveReturn: {
                        const auto& [id, ty] = *seg.end.target;
                        CgTy ct = cg_of(ty);
                        size_t slot = slot_for(id, ct);
                        Access fa;
                        fa.kind = Access::Kind::Frame;
                        fa.idx = slot;
                        fa.ty = ct;
                        auto v = load_access(fa);
                        CgVal out;
                        out.ty = ct;
                        out.ir = v.first;
                        out.owned = false;
                        finish_async(std::move(out));
                        break;
                    }
                }
            }
            pop_scope();
        }

        std::string sw = "switch i32 %st, label %" + label_of(sblock[0]) + " [";
        for (size_t k = 1; k < sblock.size(); ++k) {
            sw += " i32 " + num(k) + ", label %" + label_of(*vblock[k]);
        }
        sw += " ]";
        switch_block(0);
        term(sw);

        std::vector<CgTy> slots = frame->slots;
        std::string tys;
        for (size_t i = 0; i < slots.size(); ++i) {
            if (i) tys += ", ";
            tys += cg_mem(slots[i]);
        }
        type_defs.push_back(fty + " = type { " + tys + " }");
        frame.reset();

        // ---- 5. drop function (env cleanup, same as sync lambdas) ----
        IrFn drop;
        drop.name = "lambda." + num(li) + ".drop";
        drop.ret = "void";
        drop.params = {"ptr %env"};
        drop.blocks.push_back(IrBlock{"entry", {}, "", false});
        push_fn(std::move(drop));
        for (size_t i = 0; i < captures.size(); ++i) {
            CgTy ct = cg_of(captures[i].second);
            if (cg_is_ptr(ct)) {
                Access a;
                a.kind = Access::Kind::Env;
                a.idx = i;
                a.ty = ct;
                std::string gep = gep_ssa(a);
                std::string v = fresh("dl");
                emit(v + " = load ptr, ptr " + gep);
                emit("call void @marina_release(ptr " + v + ")");
            }
        }
        emit("call void @marina_closure_free(ptr %env)");
        term("ret void");
        env.reset();
    }

    // ------------------------------------------------------------------
    // Async functions
    // ------------------------------------------------------------------

    // Split an async body into linear segments at await points.
    void transform(std::vector<CStmt> stmts, std::vector<Segment>& segs) {
        std::vector<CStmt> cur;
        for (auto& s : stmts) {
            if (auto* st = std::get_if<CLet>(&s.v)) {
                if (st->init && std::get_if<CAwait>(&st->init->v)) {
                    CAwait* aw = std::get_if<CAwait>(&st->init->v);
                    Segment seg;
                    seg.stmts = std::move(cur);
                    seg.end.kind = SegEnd::Kind::Await;
                    seg.end.expr = std::move(*aw->operand);
                    seg.end.target = std::make_pair(st->id, st->ty);
                    seg.end.next = segs.size() + 1;
                    segs.push_back(std::move(seg));
                    continue;
                }
                cur.push_back(std::move(s));
                continue;
            }
            if (auto* st = std::get_if<CExprStmt>(&s.v)) {
                if (st->expr && std::get_if<CAwait>(&st->expr->v)) {
                    CAwait* aw = std::get_if<CAwait>(&st->expr->v);
                    Segment seg;
                    seg.stmts = std::move(cur);
                    seg.end.kind = SegEnd::Kind::Await;
                    seg.end.expr = std::move(*aw->operand);
                    seg.end.target = std::nullopt;
                    seg.end.next = segs.size() + 1;
                    segs.push_back(std::move(seg));
                    continue;
                }
                cur.push_back(std::move(s));
                continue;
            }
            if (auto* st = std::get_if<CReturn>(&s.v)) {
                if (st->value.has_value() && st->kind.kind == ReturnKindKind::Plain) {
                    CExpr& e = **st->value;
                    if (auto* aw = std::get_if<CAwait>(&e.v)) {
                        // `return await e` == await then resolve
                        size_t tmpid = fresh_var_id();
                        Segment seg;
                        seg.stmts = std::move(cur);
                        seg.end.kind = SegEnd::Kind::Await;
                        seg.end.expr = std::move(*aw->operand);
                        seg.end.target = std::make_pair(tmpid, aw->ty);
                        seg.end.next = segs.size() + 1;
                        segs.push_back(std::move(seg));
                        Segment seg2;
                        seg2.end.kind = SegEnd::Kind::ResolveReturn;
                        seg2.end.target = std::make_pair(tmpid, aw->ty);
                        seg2.end.ty = aw->ty;
                        segs.push_back(std::move(seg2));
                    } else {
                        Segment seg;
                        seg.stmts = std::move(cur);
                        seg.end.kind = SegEnd::Kind::ReturnExpr;
                        seg.end.expr = std::move(e);
                        segs.push_back(std::move(seg));
                    }
                    return;
                }
                if (st->value.has_value() && st->kind.kind == ReturnKindKind::FlattenPromise) {
                    // `return promiseExpr`: await the inner promise, then
                    // resolve our own promise with its value (JS flattening).
                    size_t tmpid = fresh_var_id();
                    Segment seg;
                    seg.stmts = std::move(cur);
                    seg.end.kind = SegEnd::Kind::Await;
                    seg.end.expr = std::move(**st->value);
                    seg.end.target = std::make_pair(tmpid, st->kind.inner);
                    seg.end.next = segs.size() + 1;
                    segs.push_back(std::move(seg));
                    Segment seg2;
                    seg2.end.kind = SegEnd::Kind::ResolveReturn;
                    seg2.end.target = std::make_pair(tmpid, st->kind.inner);
                    seg2.end.ty = st->kind.inner;
                    segs.push_back(std::move(seg2));
                    return;
                }
                if (!st->value.has_value()) {
                    Segment seg;
                    seg.stmts = std::move(cur);
                    seg.end.kind = SegEnd::Kind::ReturnVoid;
                    segs.push_back(std::move(seg));
                    return;
                }
                cur.push_back(std::move(s));
                continue;
            }
            cur.push_back(std::move(s));
        }
        Segment seg;
        seg.stmts = std::move(cur);
        seg.end.kind = SegEnd::Kind::ReturnVoid;
        segs.push_back(std::move(seg));
    }

    void gen_async_fn(const CFn& f) {
        std::string fname = "fn." + f.name;
        std::vector<CStmt> body = f.body;

        // 1. transform into segments
        std::vector<Segment> segs;
        transform(std::move(body), segs);

        // 2. frame layout: slot 0 = promise
        std::string fty = "%frame." + fname;
        {
            FrameInfo info;
            info.ty = fty;
            info.slots = {CgTy::Promise};
            frame = std::move(info);
        }

        // 3. entry function
        std::vector<std::string> ir_params;
        for (size_t i = 0; i < f.params.size(); ++i) {
            ir_params.push_back(std::string(cg_ir(cg_of(f.params[i].second))) + " %a" + num(i));
        }
        IrFn entry;
        entry.name = fname;
        entry.ret = "ptr";
        entry.params = ir_params;
        entry.frame_ty = fty;
        entry.blocks.push_back(IrBlock{"entry", {}, "", false});
        push_fn(std::move(entry));
        cur_ret = CgTy::Promise;
        std::string pr = fresh("pr");
        emit(pr + " = call ptr @marina_promise_create()");
        {
            Access fa;
            fa.kind = Access::Kind::Frame;
            fa.idx = 0;
            fa.ty = CgTy::Promise;
            std::string gep = gep_ssa(fa);
            emit("store ptr " + pr + ", ptr " + gep);
        }
        for (size_t i = 0; i < f.params.size(); ++i) {
            CgTy ct = cg_of(f.params[i].second);
            size_t slot = slot_for(f.params[i].first, ct);
            Access fa;
            fa.kind = Access::Kind::Frame;
            fa.idx = slot;
            fa.ty = ct;
            std::string gep = gep_ssa(fa);
            emit("store " + std::string(cg_ir(ct)) + " %a" + num(i) + ", ptr " + gep);
        }
        std::string rv = fresh("rv");
        emit(rv + " = call ptr @marina_retain(ptr " + pr + ")");
        emit("call void @" + fname + ".resume(ptr %fr, i32 0, ptr null)");
        term("ret ptr " + rv);

        // 4. resume function (state machine)
        IrFn resume;
        resume.name = fname + ".resume";
        resume.ret = "void";
        resume.params = {"ptr %fr", "i32 %st", "ptr %val"};
        resume.blocks.push_back(IrBlock{"pro", {}, "", false});
        push_fn(std::move(resume));
        cur_ret = CgTy::Void;

        // pre-create segment blocks with stable labels
        std::vector<std::optional<size_t>> vblock;
        std::vector<size_t> sblock;
        for (size_t k = 0; k < segs.size(); ++k) {
            if (k > 0) {
                vblock.push_back(new_block_fixed("seg." + num(k) + "v"));
            } else {
                vblock.push_back(std::nullopt);
            }
            sblock.push_back(new_block_fixed("seg." + num(k)));
        }

        // compile segments
        // The continuation block of segment k receives the awaited value of
        // segment k-1 (the segment that suspended before it).
        std::vector<std::optional<std::pair<size_t, CTy>>> await_targets;
        for (const auto& s : segs) {
            if (s.end.kind == SegEnd::Kind::Await) {
                await_targets.push_back(s.end.target);
            } else {
                await_targets.push_back(std::nullopt);
            }
        }
        for (size_t k = 0; k < segs.size(); ++k) {
            Segment& seg = segs[k];
            if (k > 0) {
                switch_block(*vblock[k]);
                if (await_targets[k - 1].has_value()) {
                    auto [id, ty] = *await_targets[k - 1];
                    materialize_await_value(id, ty);
                }
                std::string sl = label_of(sblock[k]);
                term("br label %" + sl);
            }
            switch_block(sblock[k]);
            scopes.emplace_back();
            cg_body(seg.stmts);
            if (!terminated()) {
                switch (seg.end.kind) {
                    case SegEnd::Kind::Await: {
                        CgVal v = cg_expr(seg.end.expr);
                        v = to_owned(v);
                        emit("call void @marina_await(ptr " + v.ir + ", ptr @" + fname +
                             ".resume, ptr %fr, i32 " + num(seg.end.next) + ")");
                        flush_pending();
                        term("ret void");
                        break;
                    }
                    case SegEnd::Kind::ReturnExpr: {
                        CTy inner = cty_void();
                        if (f.ret.tag == CTy::Tag::Promise && f.ret.inner) {
                            inner = *f.ret.inner;
                        }
                        CgVal v = cg_expr(seg.end.expr);
                        v = coerce_val(v, cg_of(inner));
                        finish_async(std::move(v));
                        break;
                    }
                    case SegEnd::Kind::ReturnVoid:
                        finish_async(std::nullopt);
                        break;
                    case SegEnd::Kind::ResolveReturn: {
                        const auto& [id, ty] = *seg.end.target;
                        CgTy ct = cg_of(ty);
                        size_t slot = slot_for(id, ct);
                        Access fa;
                        fa.kind = Access::Kind::Frame;
                        fa.idx = slot;
                        fa.ty = ct;
                        auto v = load_access(fa);
                        CgVal out;
                        out.ty = ct;
                        out.ir = v.first;
                        out.owned = false;
                        finish_async(std::move(out));
                        break;
                    }
                }
            }
            pop_scope();
        }

        // 5. switch on state in the prologue block
        std::string sw = "switch i32 %st, label %" + label_of(sblock[0]) + " [";
        for (size_t k = 1; k < sblock.size(); ++k) {
            sw += " i32 " + num(k) + ", label %" + label_of(*vblock[k]);
        }
        sw += " ]";
        switch_block(0);
        term(sw);

        // 6. emit the frame type def (slot count now final)
        std::vector<CgTy> slots = frame->slots;
        std::string tys;
        for (size_t i = 0; i < slots.size(); ++i) {
            if (i) tys += ", ";
            tys += cg_mem(slots[i]);
        }
        type_defs.push_back(fty + " = type { " + tys + " }");

        frame.reset();
    }

    // Materialize the awaited value (in `%val`) into `id`'s frame slot.
    void materialize_await_value(size_t id, const CTy& ty) {
        CgTy ct = cg_of(ty);
        size_t slot = slot_for(id, ct);
        Access fa;
        fa.kind = Access::Kind::Frame;
        fa.idx = slot;
        fa.ty = ct;
        std::string gep = gep_ssa(fa);
        std::string vgep2 = mv_field("%val", 2);
        std::string vgep3 = mv_field("%val", 3);
        std::string vgep4 = mv_field("%val", 4);
        switch (ct) {
            case CgTy::Int: {
                std::string l = fresh("aw");
                emit(l + " = load i64, ptr " + vgep2);
                std::string t = fresh("at");
                emit(t + " = trunc i64 " + l + " to i32");
                emit("store i32 " + t + ", ptr " + gep);
                break;
            }
            case CgTy::Float: {
                std::string l = fresh("aw");
                emit(l + " = load double, ptr " + vgep3);
                emit("store double " + l + ", ptr " + gep);
                break;
            }
            case CgTy::Str: {
                std::string l = fresh("aw");
                emit(l + " = load ptr, ptr " + vgep4);
                std::string r = fresh("ar");
                emit(r + " = call ptr @marina_retain(ptr " + l + ")");
                emit("store ptr " + r + ", ptr " + gep);
                break;
            }
            case CgTy::Bool: {
                std::string l = fresh("aw");
                emit(l + " = load i64, ptr " + vgep2);
                std::string t = fresh("at");
                emit(t + " = trunc i64 " + l + " to i1");
                std::string z = fresh("az");
                emit(z + " = zext i1 " + t + " to i8");
                emit("store i8 " + z + ", ptr " + gep);
                break;
            }
            // Reference values: the waiter task hands us a retained copy
            // (marina_promise_resolve retains per waiter), so retain again
            // for the frame slot — the slot releases it when the frame dies
            // and run_waiter drops the task's copy. Stream values are plain
            // integer handles (no payload to move).
            case CgTy::Stream: {
                std::string l = fresh("aw");
                emit(l + " = load i64, ptr " + vgep2);
                emit("store i64 " + l + ", ptr " + gep);
                break;
            }
            case CgTy::Array:
            case CgTy::Object:
            case CgTy::Promise:
            case CgTy::Resolver: {
                std::string l = fresh("aw");
                emit(l + " = load ptr, ptr " + vgep4);
                std::string r = fresh("ar");
                emit(r + " = call ptr @marina_retain(ptr " + l + ")");
                emit("store ptr " + r + ", ptr " + gep);
                break;
            }
            default: break;
        }
    }

    // ------------------------------------------------------------------
    // Program entry point
    // ------------------------------------------------------------------

    void gen_main(const Checked& checked) {
        IrFn mainfn;
        mainfn.name = "main";
        mainfn.ret = "i32";
        mainfn.blocks.push_back(IrBlock{"entry", {}, "", false});
        push_fn(std::move(mainfn));
        emit("call void @marina_event_loop_start()");
        // register class metadata (name + field names) so the runtime can
        // print objects and keep classes self-describing
        for (size_t id = 0; id < checked.classes.size(); ++id) {
            const ClassDef& def = checked.classes[id];
            std::string bytes;
            bytes += def.name;
            bytes.push_back('\0');
            for (const auto& [fname, fty] : def.fields) {
                (void)fty;
                bytes += fname;
                bytes.push_back('\0');
            }
            size_t l = bytes.size();
            std::string gname = "@.classmeta." + num(id);
            globals.push_back(gname + " = private constant [" + num(l) + " x i8] c\"" +
                              escape_ir_string(bytes) + "\", align 8");
            emit("call void @marina_class_register(i32 " + num(id) +
                 ", ptr getelementptr inbounds ([" + num(l) + " x i8], ptr " + gname +
                 ", i64 0, i64 0), i64 " + num(l) + ")");
        }
        if (checked.toplevel.has_value()) {
            emit("call void @fn.__toplevel()");
        }
        if (checked.has_sync_main && !checked.explicit_main) {
            CgTy rty = CgTy::Void;
            auto it = fn_sigs.find("main");
            if (it != fn_sigs.end()) rty = it->second.ret;
            emit("call " + std::string(cg_ir(rty)) + " @fn.main()");
        }
        if (checked.has_async_main && !checked.explicit_main) {
            std::string mp = fresh("mp");
            emit(mp + " = call ptr @fn.main()");
            emit("call void @marina_release(ptr " + mp + ")");
        }
        std::string r = fresh("mr");
        emit(r + " = call i32 @marina_event_loop_run()");
        term("ret i32 " + r);
    }

    // ------------------------------------------------------------------
    // Module rendering
    // ------------------------------------------------------------------

    std::string render() const {
        std::string out;
        out += "; ModuleID = 'marina'\n";
        out += "; generated by the marina compiler v0.1\n\n";
        out += "%mv = type { i32, i32, i64, double, ptr }\n\n";
        for (const auto& t : type_defs) {
            out += t;
            out += '\n';
        }
        if (!type_defs.empty()) {
            out += '\n';
        }
        for (const auto& g : globals) {
            out += g;
            out += '\n';
        }
        if (!globals.empty()) {
            out += '\n';
        }
        out += DECLARES;
        out += "\n\n";
        for (const auto& f : fns) {
            out += render_fn(f);
        }
        return out;
    }
};

std::string render_fn(const IrFn& f) {
    std::string s = "define " + f.ret + " @" + f.name + "(";
    for (size_t i = 0; i < f.params.size(); ++i) {
        if (i) s += ", ";
        s += f.params[i];
    }
    s += ") {\n";
    const IrBlock& b0 = f.blocks[0];
    s += b0.label + ":\n";
    if (f.frame_ty.has_value()) {
        s += "  %fr = call ptr @marina_frame_alloc(i64 ptrtoint (ptr getelementptr inbounds (" +
             *f.frame_ty + ", ptr null, i64 1) to i64))\n";
    }
    for (const auto& a : f.allocas) {
        s += "  " + a + "\n";
    }
    for (const auto& i : b0.instrs) {
        s += "  " + i + "\n";
    }
    if (b0.has_term) {
        s += "  " + b0.term + "\n";
    } else {
        throw std::runtime_error("block " + b0.label + " in " + f.name + " is not terminated");
    }
    for (size_t bi = 1; bi < f.blocks.size(); ++bi) {
        const IrBlock& b = f.blocks[bi];
        s += "\n" + b.label + ":\n";
        for (const auto& i : b.instrs) {
            s += "  " + i + "\n";
        }
        if (b.has_term) {
            s += "  " + b.term + "\n";
        } else {
            throw std::runtime_error("block " + b.label + " in " + f.name +
                                     " is not terminated");
        }
    }
    s += "}\n\n";
    return s;
}

}  // namespace

std::string generate(const Checked& c) {
    Gen g(&c);
    for (const auto& f : c.fns) {
        Sig sig;
        sig.ret = cg_of(f.ret);
        for (const auto& [id, t] : f.params) {
            (void)id;
            sig.params.push_back(cg_of(t));
        }
        sig.is_async = f.is_async;
        g.fn_sigs.insert_or_assign(f.name, sig);
    }
    // top-level code runs first AND declares its variables as globals, so it
    // must be generated before the lambdas and functions that reference those
    // variables (lambda bodies read top-level vars through the globals)
    if (c.toplevel.has_value()) {
        std::vector<CStmt> body = *c.toplevel;
        g.toplevel_gen = true;
        g.gen_sync_fn_into("fn.__toplevel", {}, CTy{}, body);
        g.toplevel_gen = false;
    }
    for (size_t li = 0; li < c.lambdas.size(); ++li) {
        g.gen_lambda(li);
    }
    for (const auto& f : c.fns) {
        if (f.is_async) {
            g.gen_async_fn(f);
        } else {
            g.gen_sync_fn(f);
        }
    }
    g.gen_main(c);
    return g.render();
}

}  // namespace marina::codegen
