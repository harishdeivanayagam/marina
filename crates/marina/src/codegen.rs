//! LLVM IR code generation for marina.
//!
//! Emits textual LLVM IR (.ll). Every `async` function is lowered to:
//!   * `@fn.<name>`           — entry: allocates a frame + promise, runs state 0
//!   * `@fn.<name>.resume`    — state machine: one block per continuation
//! `await` calls `marina_await(promise, resume, frame, next_state)` which
//! parks the frame; the runtime's worker threads resume it later.
//!
//! ARC: string/promise values carry +1 ownership. The compiler inserts
//! `marina_retain` / `marina_release` so every object is released exactly
//! once on every path (literals are interned and immortal).

use crate::checker::*;
use std::collections::HashMap;

// ---------------------------------------------------------------------------
// Codegen types
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CgTy {
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
}

impl CgTy {
    fn ir(self) -> &'static str {
        match self {
            CgTy::Void => "void",
            CgTy::Int => "i32",
            CgTy::Float => "double",
            CgTy::Bool => "i1",
            CgTy::Stream => "i64",
            CgTy::Str | CgTy::Promise | CgTy::Resolver | CgTy::Array | CgTy::Object => "ptr",
        }
    }

    /// Type used for memory slots (allocas / frame / env fields).
    fn mem(self) -> &'static str {
        match self {
            CgTy::Bool => "i8",
            other => other.ir(),
        }
    }

    fn is_ptr(self) -> bool {
        matches!(self, CgTy::Str | CgTy::Promise | CgTy::Resolver | CgTy::Array | CgTy::Object)
    }
}

fn cg_ty(t: &CTy) -> CgTy {
    match t {
        CTy::Void => CgTy::Void,
        CTy::Int => CgTy::Int,
        CTy::Float => CgTy::Float,
        CTy::Bool => CgTy::Bool,
        CTy::Str => CgTy::Str,
        CTy::Promise(_) => CgTy::Promise,
        CTy::Resolver(_) => CgTy::Resolver,
        CTy::Array(_) => CgTy::Array,
        CTy::Object(_) => CgTy::Object,
        CTy::Interface(_) => CgTy::Object,
        CTy::Stream => CgTy::Stream,
    }
}

/// MValue tag for a checked type (used when creating arrays / map results).
fn mv_tag_of(t: &CTy) -> i32 {
    match cg_ty(t) {
        CgTy::Int => 1,
        CgTy::Float => 2,
        CgTy::Str => 3,
        CgTy::Bool => 4,
        CgTy::Array => 5,
        CgTy::Object => 6,
        CgTy::Stream => 7,
        _ => unreachable!("array element type must be a value type"),
    }
}

/// Best-effort static type of a checked expression (used to find the element
/// type of an array expression).
fn cexpr_cty(e: &CExpr) -> Option<CTy> {
    match e {
        CExpr::Ident(_, t) => Some(t.clone()),
        CExpr::Unary(_, _, t) => Some(t.clone()),
        CExpr::Binary(_, _, _, t) => Some(t.clone()),
        CExpr::CallFn { ret, .. } => Some(ret.clone()),
        CExpr::CallResolver { inner, .. } => Some(inner.clone()),
        CExpr::Await(_, t) => Some(t.clone()),
        CExpr::ArrayLit { elem_ty, .. } => Some(CTy::Array(Box::new(elem_ty.clone()))),
        CExpr::Index { ty, .. } => Some(ty.clone()),
        CExpr::ArrayOp { ret, .. } => Some(ret.clone()),
        CExpr::ArrayHof { ret, .. } => Some(ret.clone()),
        _ => None,
    }
}

#[derive(Clone)]
struct CgVal {
    ty: CgTy,
    ir: String,
    owned: bool,
}

#[derive(Clone)]
enum Access {
    Alloca(String, CgTy),
    Frame(usize, CgTy),
    Env(usize, CgTy),
    /// module-level global (top-level variables, readable from any function)
    Global(String, CgTy),
}

struct IrBlock {
    label: String,
    instrs: Vec<String>,
    term: Option<String>,
}

struct IrFn {
    name: String,
    ret: &'static str,
    params: Vec<String>,
    allocas: Vec<String>,
    blocks: Vec<IrBlock>,
    /// Some(ty_name): async entry fn; render prepends the frame alloc.
    frame_ty: Option<String>,
}

#[derive(Clone)]
struct Sig {
    ret: CgTy,
    params: Vec<CgTy>,
    /// Kept for future lowering (e.g. async call-eligibility checks).
    #[allow(dead_code)]
    is_async: bool,
}

struct FrameInfo {
    ty: String,
    /// slots[0] is the promise; gep field index == slot index.
    slots: Vec<CgTy>,
    map: HashMap<usize, usize>,
}

struct EnvInfo {
    ty: String,
    slot_tys: Vec<CgTy>,
    map: HashMap<usize, usize>,
}

enum SegEnd {
    Await {
        expr: CExpr,
        target: Option<(usize, CTy)>,
        next: usize,
    },
    ReturnExpr(CExpr),
    ReturnVoid,
    ResolveReturn { id: usize, ty: CTy },
}

struct Segment {
    stmts: Vec<CStmt>,
    end: SegEnd,
}

fn default_value(t: CgTy) -> &'static str {
    match t {
        CgTy::Int => "0",
        CgTy::Float => "0.0",
        CgTy::Bool => "false",
        CgTy::Stream => "0",
        CgTy::Str | CgTy::Promise | CgTy::Resolver | CgTy::Array | CgTy::Object => "null",
        CgTy::Void => "void",
    }
}

fn escape_ir_string(bytes: &[u8]) -> String {
    let mut s = String::new();
    for &b in bytes {
        if (32..=126).contains(&b) && b != b'"' && b != b'\\' {
            s.push(b as char);
        } else {
            s.push_str(&format!("\\{:02X}", b));
        }
    }
    s
}

const DECLARES: &str = "\
declare ptr @marina_frame_alloc(i64)
declare void @marina_frame_free(ptr)
declare ptr @marina_promise_create()
declare void @marina_promise_resolve(ptr, ptr)
declare void @marina_await(ptr, ptr, ptr, i32)
declare ptr @marina_promise_run_executor(ptr, ptr, ptr, ptr)
declare void @marina_set_timeout(ptr, ptr, ptr, i64)
declare ptr @marina_closure_alloc(i64)
declare void @marina_closure_free(ptr)
declare ptr @marina_retain(ptr)
declare void @marina_release(ptr)
declare ptr @marina_string_literal(ptr, i64)
declare ptr @marina_string_concat(ptr, ptr)
declare i64 @marina_string_len(ptr)
declare ptr @marina_to_string(ptr)
declare void @marina_print(ptr)
declare ptr @marina_array_new(i32)
declare i64 @marina_array_len(ptr)
declare i64 @marina_array_push(ptr, ptr)
declare void @marina_array_pop(ptr, ptr)
declare void @marina_array_shift(ptr, ptr)
declare i64 @marina_array_unshift(ptr, ptr)
declare void @marina_array_get(ptr, i64, ptr)
declare void @marina_array_set(ptr, i64, ptr)
declare ptr @marina_array_splice(ptr, i64, i64)
declare ptr @marina_array_slice(ptr, i64, i64)
declare ptr @marina_array_concat(ptr, ptr)
declare ptr @marina_array_join(ptr, ptr)
declare ptr @marina_array_reverse(ptr)
declare ptr @marina_array_sort(ptr)
declare void @marina_array_foreach(ptr, ptr, ptr, ptr)
declare ptr @marina_array_map(ptr, ptr, ptr, ptr, i32)
declare ptr @marina_array_filter(ptr, ptr, ptr, ptr)
declare void @marina_array_reduce(ptr, ptr, ptr, ptr, ptr, ptr)
declare void @marina_class_register(i32, ptr, i64)
declare ptr @marina_object_new(i32, i64)
declare void @marina_object_get(ptr, i64, ptr)
declare void @marina_object_set(ptr, i64, ptr)
declare void @marina_object_get_field(ptr, ptr, ptr)
declare void @marina_object_set_field(ptr, ptr, ptr)
declare void @marina_fs_read_file(ptr, ptr)
declare void @marina_fs_write_file(ptr, ptr)
declare void @marina_fs_exists(ptr, ptr)
declare void @marina_fs_read_dir(ptr, ptr)
declare void @marina_fs_mkdir(ptr, ptr)
declare void @marina_fs_remove(ptr, ptr)
declare void @marina_crypto_sha256(ptr, ptr)
declare void @marina_crypto_base64(i1, ptr, ptr)
declare void @marina_crypto_random_hex(i64, ptr)
declare void @marina_crypto_uuid(ptr)
declare void @marina_net_connect(ptr, ptr, ptr)
declare void @marina_net_write(ptr, ptr)
declare void @marina_net_read_all(ptr, ptr)
declare void @marina_net_close(ptr)
declare i1 @marina_str_eq(ptr, ptr)
declare void @marina_http_request(ptr, ptr, ptr, ptr, i32)
declare void @marina_http_listen(ptr, ptr, ptr, ptr, i32, i32)
declare void @marina_http_res_send(ptr, ptr)
declare void @marina_http_res_json(ptr, ptr)
declare void @marina_http_router_bind(ptr, ptr)
declare void @marina_http_route_add(ptr, ptr, ptr, ptr, ptr, ptr)
declare void @marina_http_server_listen(ptr, ptr, ptr, i32, i32)
declare void @marina_http_validator_init(ptr, ptr)
declare void @marina_http_field_init(ptr, i32)
declare void @marina_http_field_op(ptr, i32, ptr)
declare void @marina_http_validate(ptr, ptr, i32, ptr)
declare void @marina_sql_open(ptr, ptr, i32, ptr)
declare void @marina_sql_execute(ptr, ptr, i64, ptr, i32, ptr)
declare void @marina_sql_query(ptr, ptr, i64, ptr, ptr)
declare void @marina_sql_close(ptr)
declare ptr @marina_sql_execute_async(ptr, ptr, i64, ptr, i32)
declare ptr @marina_sql_query_async(ptr, ptr, i64, ptr)
declare ptr @marina_fs_read_file_async(ptr)
declare ptr @marina_fs_write_file_async(ptr, ptr)
declare ptr @marina_fs_exists_async(ptr)
declare ptr @marina_fs_read_dir_async(ptr)
declare ptr @marina_fs_mkdir_async(ptr)
declare ptr @marina_fs_remove_async(ptr)
declare ptr @marina_net_connect_async(ptr, ptr)
declare ptr @marina_net_write_async(ptr, ptr)
declare ptr @marina_net_read_all_async(ptr)
declare ptr @marina_net_close_async(ptr)
declare ptr @marina_http_request_async(ptr, ptr, ptr, i32)
declare void @marina_event_loop_start()
declare i32 @marina_event_loop_run()";

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------

pub fn generate(c: &Checked) -> String {
    let mut g = Gen::new(c);
    for f in &c.fns {
        g.fn_sigs.insert(
            f.name.clone(),
            Sig {
                ret: cg_ty(&f.ret),
                params: f.params.iter().map(|(_, t)| cg_ty(t)).collect(),
                is_async: f.is_async,
            },
        );
    }
    // top-level code runs first AND declares its variables as globals, so it
    // must be generated before the lambdas and functions that reference those
    // variables (lambda bodies read top-level vars through the globals)
    if let Some(t) = &c.toplevel {
        let body = t.clone();
        g.toplevel_gen = true;
        g.gen_sync_fn_into("fn.__toplevel".into(), &[], &CTy::Void, &body);
        g.toplevel_gen = false;
    }
    for li in 0..c.lambdas.len() {
        g.gen_lambda(li);
    }
    for f in &c.fns {
        if f.is_async {
            g.gen_async_fn(f);
        } else {
            g.gen_sync_fn(f);
        }
    }
    g.gen_main(c);
    g.render()
}

struct Gen<'a> {
    c: &'a Checked,
    type_defs: Vec<String>,
    env_types_done: std::collections::HashSet<usize>,
    globals: Vec<String>,
    lit_names: HashMap<String, String>,
    fn_sigs: HashMap<String, Sig>,
    fns: Vec<IrFn>,
    // current function state
    cur: usize,
    block: usize,
    scopes: Vec<HashMap<usize, Access>>,
    pending: Vec<String>, // owned temps to release at end of statement
    frame: Option<FrameInfo>,
    env: Option<EnvInfo>,
    cur_ret: CgTy,
    tmp: usize,
    lbl: usize,
    lit: usize,
    var_tmp: usize, // ids for synthetic await targets (start at 1_000_000)
    // current callback-lambda context (set while generating a lambda body)
    lambda_out: Option<String>,
    lambda_exit: Option<usize>,
    lambda_ret: Option<CgTy>,
    /// true while generating fn.__toplevel: variables declare as globals
    toplevel_gen: bool,
    /// var id -> global access (top-level variables)
    toplevel_globals: HashMap<usize, Access>,
}

impl<'a> Gen<'a> {
    fn new(c: &'a Checked) -> Self {
        Gen {
            c,
            type_defs: Vec::new(),
            env_types_done: Default::default(),
            globals: Vec::new(),
            lit_names: HashMap::new(),
            fn_sigs: HashMap::new(),
            fns: Vec::new(),
            cur: 0,
            block: 0,
            scopes: vec![HashMap::new()],
            pending: Vec::new(),
            frame: None,
            env: None,
            cur_ret: CgTy::Void,
            tmp: 0,
            lbl: 0,
            lit: 0,
            var_tmp: 1_000_000,
            lambda_out: None,
            lambda_exit: None,
            lambda_ret: None,
            toplevel_gen: false,
            toplevel_globals: HashMap::new(),
        }
    }

    // ------------------------------------------------------------------
    // IR building primitives
    // ------------------------------------------------------------------

    fn fresh(&mut self, hint: &str) -> String {
        self.tmp += 1;
        format!("%{}.{}", hint, self.tmp)
    }

    fn fresh_var_id(&mut self) -> usize {
        self.var_tmp += 1;
        self.var_tmp
    }

    fn push_fn(&mut self, f: IrFn) {
        self.fns.push(f);
        self.cur = self.fns.len() - 1;
        self.block = 0;
        self.scopes = vec![HashMap::new()];
        self.pending.clear();
    }

    fn emit(&mut self, line: String) {
        self.fns[self.cur].blocks[self.block].instrs.push(line);
    }

    fn term(&mut self, line: String) {
        let b = &mut self.fns[self.cur].blocks[self.block];
        if b.term.is_none() {
            b.term = Some(line);
        }
    }

    fn terminated(&self) -> bool {
        self.fns[self.cur].blocks[self.block].term.is_some()
    }

    fn alloca(&mut self, ty: &str) -> String {
        let name = self.fresh("v");
        self.fns[self.cur]
            .allocas
            .push(format!("{} = alloca {}", name, ty));
        name
    }

    fn new_block(&mut self, hint: &str) -> usize {
        self.lbl += 1;
        self.fns[self.cur].blocks.push(IrBlock {
            label: format!("{}.{}", hint, self.lbl),
            instrs: Vec::new(),
            term: None,
        });
        self.fns[self.cur].blocks.len() - 1
    }

    fn new_block_fixed(&mut self, label: String) -> usize {
        self.fns[self.cur].blocks.push(IrBlock {
            label,
            instrs: Vec::new(),
            term: None,
        });
        self.fns[self.cur].blocks.len() - 1
    }

    fn switch_block(&mut self, idx: usize) {
        self.block = idx;
    }

    fn label_of(&self, idx: usize) -> String {
        self.fns[self.cur].blocks[idx].label.clone()
    }

    fn branch_to(&mut self, idx: usize) {
        if !self.terminated() {
            let label = self.label_of(idx);
            self.term(format!("br label %{}", label));
        }
    }

    fn flush_pending(&mut self) {
        let names = std::mem::take(&mut self.pending);
        for n in names {
            self.emit(format!("call void @marina_release(ptr {})", n));
        }
    }

    // ------------------------------------------------------------------
    // Memory access
    // ------------------------------------------------------------------

    fn slot_for(&mut self, id: usize, ty: CgTy) -> usize {
        let fr = self.frame.as_mut().unwrap();
        if let Some(&idx) = fr.map.get(&id) {
            return idx;
        }
        let idx = fr.slots.len();
        fr.slots.push(ty);
        fr.map.insert(id, idx);
        idx
    }

    fn declare_access(&mut self, id: usize, ty: CgTy) -> Access {
        if self.toplevel_gen {
            // top-level variables become module globals so any function
            // (and any lambda capturing them) can read them
            let g = format!("@.g.{}", self.lit);
            self.lit += 1;
            self.globals.push(format!("{} = global {} zeroinitializer", g, ty.mem()));
            let a = Access::Global(g, ty);
            self.toplevel_globals.insert(id, a.clone());
            return a;
        }
        if self.frame.is_some() {
            let idx = self.slot_for(id, ty);
            Access::Frame(idx, ty)
        } else {
            let p = self.alloca(ty.mem());
            Access::Alloca(p, ty)
        }
    }

    fn access_for(&self, id: usize) -> Option<Access> {
        // top-level variables live in globals: lambdas reading or assigning
        // them share program state instead of a by-value capture
        if let Some(a) = self.toplevel_globals.get(&id) {
            return Some(a.clone());
        }
        for sc in self.scopes.iter().rev() {
            if let Some(a) = sc.get(&id) {
                return Some(a.clone());
            }
        }
        if let Some(fr) = &self.frame {
            if let Some(&idx) = fr.map.get(&id) {
                return Some(Access::Frame(idx, fr.slots[idx]));
            }
        }
        if let Some(env) = &self.env {
            if let Some(&idx) = env.map.get(&id) {
                return Some(Access::Env(idx, env.slot_tys[idx]));
            }
        }
        if let Some(a) = self.toplevel_globals.get(&id) {
            return Some(a.clone());
        }
        None
    }

    /// Emit a GEP instruction for an access and return its SSA name.
    /// (Local base pointers can't be used inside constant expressions.)
    fn gep_ssa(&mut self, a: &Access) -> String {
        match a {
            Access::Alloca(p, _) => return p.clone(),
            Access::Global(p, _) => return p.clone(),
            Access::Frame(idx, _) => {
                let fty = self.frame.as_ref().unwrap().ty.clone();
                let g = self.fresh("gp");
                self.emit(format!(
                    "{} = getelementptr inbounds {}, ptr %fr, i64 0, i32 {}",
                    g, fty, idx
                ));
                g
            }
            Access::Env(idx, _) => {
                let ety = self.env.as_ref().unwrap().ty.clone();
                let g = self.fresh("gp");
                self.emit(format!(
                    "{} = getelementptr inbounds {}, ptr %env, i64 0, i32 {}",
                    g, ety, idx
                ));
                g
            }
        }
    }

    /// Emit a GEP instruction addressing field `field` of an %mv value.
    fn mv_field(&mut self, a: &str, field: usize) -> String {
        let g = self.fresh("mv");
        self.emit(format!(
            "{} = getelementptr inbounds %mv, ptr {}, i64 0, i32 {}",
            g, a, field
        ));
        g
    }

    fn load_access(&mut self, a: &Access) -> (String, CgTy) {
        let ty = match a {
            Access::Alloca(_, t) | Access::Frame(_, t) | Access::Env(_, t) | Access::Global(_, t) => *t,
        };
        let gep = self.gep_ssa(a);
        let v = self.fresh("ld");
        match ty {
            CgTy::Bool => {
                self.emit(format!("{} = load i8, ptr {}", v, gep));
                let b = self.fresh("bb");
                self.emit(format!("{} = trunc i8 {} to i1", b, v));
                (b, ty)
            }
            other => {
                self.emit(format!("{} = load {}, ptr {}", v, other.ir(), gep));
                (v, ty)
            }
        }
    }

    fn store_access(&mut self, a: &Access, val: &str, ty: CgTy) {
        let gep = self.gep_ssa(a);
        match ty {
            CgTy::Bool => {
                let z = self.fresh("zz");
                self.emit(format!("{} = zext i1 {} to i8", z, val));
                self.emit(format!("store i8 {}, ptr {}", z, gep));
            }
            other => {
                self.emit(format!("store {} {}, ptr {}", other.ir(), val, gep));
            }
        }
    }

    // ------------------------------------------------------------------
    // ARC helpers
    // ------------------------------------------------------------------

    fn to_owned(&mut self, v: CgVal) -> CgVal {
        if v.owned || !v.ty.is_ptr() {
            return v;
        }
        let r = self.fresh("rn");
        self.emit(format!("{} = call ptr @marina_retain(ptr {})", r, v.ir));
        CgVal { ty: v.ty, ir: r, owned: true }
    }

    /// Borrow for a runtime call that only reads the value.
    fn borrow(&mut self, v: &CgVal) -> String {
        if v.owned && v.ty.is_ptr() {
            self.pending.push(v.ir.clone());
        }
        v.ir.clone()
    }

    /// Consume an expression whose value is discarded: release if owned.
    fn discard(&mut self, v: &CgVal) {
        if v.owned && v.ty.is_ptr() {
            self.pending.push(v.ir.clone());
        }
    }

    // ------------------------------------------------------------------
    // Conversions / helpers
    // ------------------------------------------------------------------

    fn coerce_val(&mut self, v: CgVal, to: CgTy) -> CgVal {
        if v.ty == CgTy::Int && to == CgTy::Float {
            let r = self.fresh("cf");
            self.emit(format!("{} = sitofp i32 {} to double", r, v.ir));
            return CgVal { ty: CgTy::Float, ir: r, owned: v.owned };
        }
        v
    }

    fn sext_i64(&mut self, v: &CgVal) -> String {
        let r = self.fresh("w");
        self.emit(format!("{} = sext i32 {} to i64", r, v.ir));
        r
    }

    fn string_lit(&mut self, s: &str) -> CgVal {
        let name = if let Some(n) = self.lit_names.get(s) {
            n.clone()
        } else {
            let n = format!("@.bytes.{}", self.lit);
            self.lit += 1;
            let bytes = escape_ir_string(s.as_bytes());
            self.globals.push(format!(
                "{} = private constant [{} x i8] c\"{}\"",
                n,
                s.len(),
                bytes
            ));
            self.lit_names.insert(s.to_string(), n.clone());
            n
        };
        let v = self.fresh("st");
        self.emit(format!(
            "{} = call ptr @marina_string_literal(ptr {}, i64 {})",
            v,
            name,
            s.len()
        ));
        CgVal { ty: CgTy::Str, ir: v, owned: true }
    }

    /// Build an %mv value on the stack from a typed value (borrows it).
    fn build_mv(&mut self, v: &CgVal) -> String {
        let a = self.alloca("%mv");
        let tag = match v.ty {
            CgTy::Void => 0,
            CgTy::Int => 1,
            CgTy::Float => 2,
            CgTy::Str => 3,
            CgTy::Bool => 4,
            CgTy::Array => 5,
            CgTy::Object => 6,
            CgTy::Stream => 7,
            CgTy::Promise | CgTy::Resolver => unreachable!("promise values are not printable"),
        };
        self.emit(format!("store i32 {}, ptr {}", tag, a));
        {
            let g = self.mv_field(&a, 1);
            self.emit(format!("store i32 0, ptr {}", g));
        }
        match v.ty {
            CgTy::Int => {
                let x = self.sext_i64(v);
                let g = self.mv_field(&a, 2);
                self.emit(format!("store i64 {}, ptr {}", x, g));
            }
            CgTy::Stream => {
                let g = self.mv_field(&a, 2);
                self.emit(format!("store i64 {}, ptr {}", v.ir, g));
            }
            CgTy::Float => {
                let g = self.mv_field(&a, 3);
                self.emit(format!("store double {}, ptr {}", v.ir, g));
            }
            CgTy::Bool => {
                let x = self.fresh("bw");
                self.emit(format!("{} = zext i1 {} to i64", x, v.ir));
                let g = self.mv_field(&a, 2);
                self.emit(format!("store i64 {}, ptr {}", x, g));
            }
            CgTy::Str => {
                let g = self.mv_field(&a, 4);
                self.emit(format!("store ptr {}, ptr {}", v.ir, g));
            }
            CgTy::Array | CgTy::Object => {
                let g = self.mv_field(&a, 4);
                self.emit(format!("store ptr {}, ptr {}", v.ir, g));
            }
            _ => {}
        }
        a
    }

    /// Load a typed value out of an MValue struct (alloca'd). Ptr payloads are
/// owned by the caller (the runtime retains them before handing them over).
fn mv_load(&mut self, a: &str, ct: CgTy) -> CgVal {
    match ct {
        CgTy::Int => {
            let g = self.mv_field(&a.to_string(), 2);
            let l = self.fresh("ml");
            self.emit(format!("{} = load i64, ptr {}", l, g));
            let t = self.fresh("mt");
            self.emit(format!("{} = trunc i64 {} to i32", t, l));
            CgVal { ty: CgTy::Int, ir: t, owned: false }
        }
        CgTy::Float => {
            let g = self.mv_field(&a.to_string(), 3);
            let l = self.fresh("ml");
            self.emit(format!("{} = load double, ptr {}", l, g));
            CgVal { ty: CgTy::Float, ir: l, owned: false }
        }
        CgTy::Bool => {
            let g = self.mv_field(&a.to_string(), 2);
            let l = self.fresh("ml");
            self.emit(format!("{} = load i64, ptr {}", l, g));
            let t = self.fresh("mt");
            self.emit(format!("{} = trunc i64 {} to i1", t, l));
            CgVal { ty: CgTy::Bool, ir: t, owned: false }
        }
        CgTy::Stream => {
            let g = self.mv_field(&a.to_string(), 2);
            let l = self.fresh("ml");
            self.emit(format!("{} = load i64, ptr {}", l, g));
            CgVal { ty: CgTy::Stream, ir: l, owned: false }
        }
        CgTy::Str | CgTy::Array | CgTy::Object => {
            let g = self.mv_field(&a.to_string(), 4);
            let l = self.fresh("ml");
            self.emit(format!("{} = load ptr, ptr {}", l, g));
            CgVal { ty: ct, ir: l, owned: true }
        }
        _ => CgVal { ty: CgTy::Void, ir: String::new(), owned: false },
    }
}

/// Store a typed value into an MValue struct pointed to by `dst`
/// (used for callback `out` parameters and array element writes).
fn write_mv_fields(&mut self, dst: &str, v: &CgVal) {
    let tag = match v.ty {
        CgTy::Void => 0,
        CgTy::Int => 1,
        CgTy::Float => 2,
        CgTy::Str => 3,
        CgTy::Bool => 4,
        CgTy::Array => 5,
        CgTy::Object => 6,
        CgTy::Stream => 7,
        CgTy::Promise | CgTy::Resolver => unreachable!(),
    };
    self.emit(format!("store i32 {}, ptr {}", tag, dst));
    {
        let g = self.mv_field(&dst.to_string(), 1);
        self.emit(format!("store i32 0, ptr {}", g));
    }
    match v.ty {
        CgTy::Int => {
            let x = self.sext_i64(v);
            let g = self.mv_field(&dst.to_string(), 2);
            self.emit(format!("store i64 {}, ptr {}", x, g));
        }
        CgTy::Stream => {
            let g = self.mv_field(&dst.to_string(), 2);
            self.emit(format!("store i64 {}, ptr {}", v.ir, g));
        }
        CgTy::Float => {
            let g = self.mv_field(&dst.to_string(), 3);
            self.emit(format!("store double {}, ptr {}", v.ir, g));
        }
        CgTy::Bool => {
            let x = self.fresh("bw");
            self.emit(format!("{} = zext i1 {} to i64", x, v.ir));
            let g = self.mv_field(&dst.to_string(), 2);
            self.emit(format!("store i64 {}, ptr {}", x, g));
        }
        CgTy::Str | CgTy::Array | CgTy::Object => {
            let g = self.mv_field(&dst.to_string(), 4);
            self.emit(format!("store ptr {}, ptr {}", v.ir, g));
        }
        _ => {}
    }
}

/// Unpack an MValue argument (from the runtime) into a typed alloca.
fn store_from_mv(&mut self, src: &str, ct: CgTy, dst: &str) {
    let v = match ct {
        CgTy::Int | CgTy::Bool | CgTy::Stream => {
            let g = self.mv_field(&src.to_string(), 2);
            let l = self.fresh("pm");
            self.emit(format!("{} = load i64, ptr {}", l, g));
            l
        }
        CgTy::Float => {
            let g = self.mv_field(&src.to_string(), 3);
            let l = self.fresh("pm");
            self.emit(format!("{} = load double, ptr {}", l, g));
            l
        }
        CgTy::Str | CgTy::Array | CgTy::Object => {
            let g = self.mv_field(&src.to_string(), 4);
            let l = self.fresh("pm");
            self.emit(format!("{} = load ptr, ptr {}", l, g));
            l
        }
        _ => String::new(),
    };
    match ct {
        CgTy::Int => {
            let t = self.fresh("pt");
            self.emit(format!("{} = trunc i64 {} to i32", t, v));
            self.emit(format!("store i32 {}, ptr {}", t, dst));
        }
        CgTy::Bool => {
            let t = self.fresh("pt");
            self.emit(format!("{} = trunc i64 {} to i1", t, v));
            let z = self.fresh("pz");
            self.emit(format!("{} = zext i1 {} to i8", z, t));
            self.emit(format!("store i8 {}, ptr {}", z, dst));
        }
        _ => self.emit(format!("store {} {}, ptr {}", ct.ir(), v, dst)),
    }
}

// ------------------------------------------------------------------
// Statements
// ------------------------------------------------------------------

    fn cg_stmt(&mut self, s: &CStmt) {
        match s {
            CStmt::Let { id, ty, init } => {
                let cg = cg_ty(ty);
                let v = self.cg_expr(init);
                let v = self.coerce_val(v, cg);
                let v = self.to_owned(v);
                let a = self.declare_access(*id, cg);
                self.store_access(&a, &v.ir, cg);
                self.scopes.last_mut().unwrap().insert(*id, a);
            }
            CStmt::Assign { id, value } => {
                let a = self.access_for(*id).expect("unresolved variable in codegen");
                let ty = match &a {
                    Access::Alloca(_, t) | Access::Frame(_, t) | Access::Env(_, t) | Access::Global(_, t) => *t,
                };
                let v = self.cg_expr(value);
                let v = self.coerce_val(v, ty);
                let v = self.to_owned(v);
                if ty.is_ptr() {
                    let (old, _) = self.load_access(&a);
                    self.emit(format!("call void @marina_release(ptr {})", old));
                }
                self.store_access(&a, &v.ir, ty);
            }
            CStmt::IndexSet { id, idx, value, elem } => {
                let a = self.access_for(*id).expect("unresolved variable in codegen");
                let (arr, _) = self.load_access(&a);
                let iv = self.cg_expr(idx);
                let i = self.sext_i64(&iv);
                let vv = self.cg_expr(value);
                let vv = self.coerce_val(vv, cg_ty(elem));
                let vv = self.to_owned(vv);
                let mv = self.build_mv(&vv);
                self.emit(format!(
                    "call void @marina_array_set(ptr {}, i64 {}, ptr {})",
                    arr, i, mv
                ));
                self.discard(&vv);
            }
            CStmt::FieldSet { id, idx, value, elem } => {
                let a = self.access_for(*id).expect("unresolved variable in codegen");
                let (obj, _) = self.load_access(&a);
                let vv = self.cg_expr(value);
                let vv = self.coerce_val(vv, cg_ty(elem));
                let vv = self.to_owned(vv);
                let mv = self.build_mv(&vv);
                self.emit(format!(
                    "call void @marina_object_set(ptr {}, i64 {}, ptr {})",
                    obj, idx, mv
                ));
                self.discard(&vv);
            }
            CStmt::IfaceSet { id, field, value, elem } => {
                // interface-typed values resolve fields by name at runtime
                let a = self.access_for(*id).expect("unresolved variable in codegen");
                let (obj, _) = self.load_access(&a);
                let vv = self.cg_expr(value);
                let vv = self.coerce_val(vv, cg_ty(elem));
                let vv = self.to_owned(vv);
                let sv = self.string_lit(field.as_str());
                let nmv = self.build_mv(&sv);
                let mv = self.build_mv(&vv);
                self.emit(format!(
                    "call void @marina_object_set_field(ptr {}, ptr {}, ptr {})",
                    obj, nmv, mv
                ));
                self.discard(&vv);
                self.discard(&sv);
            }
            CStmt::If { cond, then_body, else_body } => {
                let c = self.cg_expr(cond);
                let then_b = self.new_block("if.then");
                let else_b = self.new_block("if.else");
                let end_b = self.new_block("if.end");
                let (tl, el) = (self.label_of(then_b), self.label_of(else_b));
                self.term(format!("br i1 {}, label %{}, label %{}", c.ir, tl, el));
                self.switch_block(then_b);
                self.scopes.push(HashMap::new());
                self.cg_body(then_body);
                self.pop_scope();
                self.branch_to(end_b);
                self.switch_block(else_b);
                self.scopes.push(HashMap::new());
                if let Some(eb) = else_body {
                    self.cg_body(eb);
                }
                self.pop_scope();
                self.branch_to(end_b);
                self.switch_block(end_b);
            }
            CStmt::While { cond, body } => {
                let cond_b = self.new_block("while.cond");
                let body_b = self.new_block("while.body");
                let end_b = self.new_block("while.end");
                self.branch_to(cond_b);
                self.switch_block(cond_b);
                let c = self.cg_expr(cond);
                let (bl, el) = (self.label_of(body_b), self.label_of(end_b));
                self.term(format!("br i1 {}, label %{}, label %{}", c.ir, bl, el));
                self.switch_block(body_b);
                self.scopes.push(HashMap::new());
                self.cg_body(body);
                self.pop_scope();
                self.branch_to(cond_b);
                self.switch_block(end_b);
            }
            CStmt::For { init, cond, update, body } => {
                // scope holding the init variable(s)
                self.scopes.push(HashMap::new());
                if let Some(i) = init {
                    self.cg_body(std::slice::from_ref(&**i));
                }
                let cond_b = self.new_block("for.cond");
                let body_b = self.new_block("for.body");
                let end_b = self.new_block("for.end");
                self.branch_to(cond_b);
                self.switch_block(cond_b);
                let c = self.cg_expr(cond);
                let (bl, el) = (self.label_of(body_b), self.label_of(end_b));
                self.term(format!("br i1 {}, label %{}, label %{}", c.ir, bl, el));
                self.switch_block(body_b);
                self.scopes.push(HashMap::new());
                self.cg_body(body);
                self.pop_scope();
                if let Some(u) = update {
                    self.cg_body(std::slice::from_ref(&**u));
                }
                self.branch_to(cond_b);
                self.switch_block(end_b);
                self.pop_scope();
            }
            CStmt::Return(value, kind) => {
                // callback-lambda `return`: write result into the %out MValue
                // param (or a void tag) and jump to the lambda exit block
                if let Some(out) = self.lambda_out.clone() {
                    let rt = self.lambda_ret.unwrap_or(CgTy::Void);
                    match value {
                        Some(e) => {
                            let v = self.cg_expr(e);
                            let v = self.coerce_val(v, rt);
                            self.write_mv_fields(&out, &v);
                            self.discard(&v);
                        }
                        None => {
                            self.write_mv_fields(
                                &out,
                                &CgVal { ty: CgTy::Void, ir: "null".to_string(), owned: false },
                            );
                        }
                    }
                    self.flush_pending();
                    self.release_scope_vars();
                    let el = self.label_of(self.lambda_exit.unwrap());
                    self.term(format!("br label %{}", el));
                    return;
                }
                let is_async = self.frame.is_some();
                match (value, kind) {
                    (Some(e), ReturnKind::FlattenPromise(inner)) => {
                        // FlattenPromise at nested depth is rejected by the
                        // checker; treat defensively as a plain value.
                        let _ = inner;
                        let v = self.cg_expr(e);
                        if is_async {
                            self.finish_async(Some(v));
                        } else {
                            unreachable!()
                        }
                    }
                    (Some(e), ReturnKind::Plain) => {
                        let v = self.cg_expr(e);
                        let v = self.coerce_val(v, self.cur_ret);
                        if is_async {
                            self.finish_async(Some(v));
                        } else if self.cur_ret == CgTy::Void {
                            self.discard(&v);
                            self.sync_epilogue(None);
                        } else {
                            let v = self.to_owned(v);
                            self.sync_epilogue(Some(v));
                        }
                    }
                    (None, _) => {
                        if is_async {
                            self.finish_async(None);
                        } else {
                            self.sync_epilogue(None);
                        }
                    }
                }
            }
            CStmt::Expr(e) => {
                let v = self.cg_expr(e);
                self.discard(&v);
            }
        }
        self.flush_pending();
    }

    fn cg_body(&mut self, stmts: &[CStmt]) {
        for s in stmts {
            if self.terminated() {
                break; // unreachable statements after return
            }
            self.cg_stmt(s);
        }
    }

    /// Release every live local variable across all scopes (skips closure env
/// slots, which the generated drop function owns).
fn release_scope_vars(&mut self) {
    let mut accesses: Vec<Access> = Vec::new();
    for sc in self.scopes.iter().rev() {
        for a in sc.values() {
            if matches!(a, Access::Env(..)) {
                continue;
            }
            accesses.push(a.clone());
        }
    }
    for a in &accesses {
        // globals outlive the function — never release them
        if matches!(a, Access::Global(..)) {
            continue;
        }
        let ty = match a {
            Access::Alloca(_, t) | Access::Frame(_, t) | Access::Env(_, t) | Access::Global(_, t) => *t,
        };
        if ty.is_ptr() {
            let (old, _) = self.load_access(a);
            self.emit(format!("call void @marina_release(ptr {})", old));
        }
    }
}

fn pop_scope(&mut self) {
        let sc = self.scopes.pop().unwrap();
        if self.terminated() {
            return; // releases were emitted by the return epilogue
        }
        let accesses: Vec<Access> = sc.values().cloned().collect();
        for a in &accesses {
            // globals outlive the function — never release them
            if matches!(a, Access::Global(..)) {
                continue;
            }
            let ty = match a {
                Access::Alloca(_, t) | Access::Frame(_, t) | Access::Env(_, t) | Access::Global(_, t) => *t,
            };
            if ty.is_ptr() {
                let (v, _) = self.load_access(a);
                self.emit(format!("call void @marina_release(ptr {})", v));
            }
        }
    }

    /// Releases every live variable of the current (sync) function, then ret.
    fn sync_epilogue(&mut self, v: Option<CgVal>) {
        self.release_scope_vars();
        self.flush_pending();
        match v {
            Some(val) => {
                if val.ty == CgTy::Void {
                    self.term("ret void".to_string());
                } else {
                    self.term(format!("ret {} {}", val.ty.ir(), val.ir));
                }
            }
            None => {
                if self.cur_ret == CgTy::Void {
                    self.term("ret void".to_string());
                } else {
                    let d = default_value(self.cur_ret);
                    self.term(format!("ret {} {}", self.cur_ret.ir(), d));
                }
            }
        }
    }

    /// Terminate an async continuation: resolve this function's promise,
    /// release every frame slot, free the frame.
    fn finish_async(&mut self, v: Option<CgVal>) {
        let mv = match v {
            Some(val) => {
                let borrowed = self.borrow(&val);
                let a = self.alloca("%mv");
                self.copy_mv(&borrowed, val.ty, &a);
                a
            }
            None => {
                let a = self.alloca("%mv");
                self.emit(format!("store i32 0, ptr {}", a));
                let g = self.mv_field(&a, 1);
                self.emit(format!("store i32 0, ptr {}", g));
                a
            }
        };
        let (pr, _) = self.load_access(&Access::Frame(0, CgTy::Promise));
        self.emit(format!(
            "call void @marina_promise_resolve(ptr {}, ptr {})",
            pr, mv
        ));
        self.flush_pending();
        self.release_all_frame_slots();
        self.emit("call void @marina_frame_free(ptr %fr)".to_string());
        self.term("ret void".to_string());
    }

    fn copy_mv(&mut self, val: &str, ty: CgTy, a: &str) {
        match ty {
            CgTy::Int => {
                let x = self.sext_i64(&CgVal { ty: CgTy::Int, ir: val.to_string(), owned: false });
                let g = self.mv_field(a, 2);
                self.emit(format!("store i64 {}, ptr {}", x, g));
                self.emit(format!("store i32 1, ptr {}", a));
            }
            CgTy::Float => {
                let g = self.mv_field(a, 3);
                self.emit(format!("store double {}, ptr {}", val, g));
                self.emit(format!("store i32 2, ptr {}", a));
            }
            CgTy::Bool => {
                let x = self.fresh("bw");
                self.emit(format!("{} = zext i1 {} to i64", x, val));
                let g = self.mv_field(a, 2);
                self.emit(format!("store i64 {}, ptr {}", x, g));
                self.emit(format!("store i32 4, ptr {}", a));
            }
            CgTy::Str => {
                let g = self.mv_field(a, 4);
                self.emit(format!("store ptr {}, ptr {}", val, g));
                self.emit(format!("store i32 3, ptr {}", a));
            }
            _ => {
                self.emit(format!("store i32 0, ptr {}", a));
            }
        }
    }

    fn release_all_frame_slots(&mut self) {
        let slots = self.frame.as_ref().unwrap().slots.clone();
        for (idx, ty) in slots.iter().enumerate() {
            if ty.is_ptr() {
                let (v, _) = self.load_access(&Access::Frame(idx, *ty));
                self.emit(format!("call void @marina_release(ptr {})", v));
            }
        }
    }

    // ------------------------------------------------------------------
    // Expressions
    // ------------------------------------------------------------------

    fn cg_expr(&mut self, e: &CExpr) -> CgVal {
        match e {
            CExpr::Int(v) => CgVal { ty: CgTy::Int, ir: format!("{}", v), owned: false },
            CExpr::Float(v) => CgVal { ty: CgTy::Float, ir: format!("{:?}", v), owned: false },
            CExpr::Bool(v) => CgVal { ty: CgTy::Bool, ir: format!("{}", *v as i32), owned: false },
            CExpr::Str(s) => self.string_lit(s),
            CExpr::Ident(id, ty) => {
                let a = self.access_for(*id).unwrap_or_else(|| {
                    let mut ctx = String::from("scopes:");
                    for s in &self.scopes {
                        ctx.push_str(&format!(" {:?},", s.keys().collect::<Vec<_>>()));
                    }
                    panic!(
                        "unresolved variable in codegen: {} (ty {}); env={}, frame={}, fn={} [{}]",
                        id,
                        ty.name(),
                        self.env.is_some(),
                        self.frame.is_some(),
                        self.fns[self.cur].name,
                        ctx
                    )
                });
                let (v, _) = self.load_access(&a);
                CgVal { ty: cg_ty(ty), ir: v, owned: false }
            }
            CExpr::Template(parts) => {
                let mut acc = self.string_lit("");
                for p in parts {
                    let s = match p {
                        TPartC::Str(x) => self.string_lit(x),
                        TPartC::Expr(inner) => {
                            let v = self.cg_expr(inner);
                            let mv = self.build_mv(&v);
                            let sv = self.fresh("ts");
                            self.emit(format!(
                                "{} = call ptr @marina_to_string(ptr {})",
                                sv, mv
                            ));
                            CgVal { ty: CgTy::Str, ir: sv, owned: true }
                        }
                    };
                    let a = self.borrow(&acc);
                    let b = self.borrow(&s);
                    let t = self.fresh("cc");
                    self.emit(format!(
                        "{} = call ptr @marina_string_concat(ptr {}, ptr {})",
                        t, a, b
                    ));
                    acc = CgVal { ty: CgTy::Str, ir: t, owned: true };
                }
                acc
            }
            CExpr::Unary(op, inner, ty) => {
                let v = self.cg_expr(inner);
                let r = self.fresh("un");
                match (op, cg_ty(ty)) {
                    (UnOpC::Neg, CgTy::Int) => {
                        self.emit(format!("{} = sub i32 0, {}", r, v.ir));
                        CgVal { ty: CgTy::Int, ir: r, owned: false }
                    }
                    (UnOpC::Neg, CgTy::Float) => {
                        self.emit(format!("{} = fneg double {}", r, v.ir));
                        CgVal { ty: CgTy::Float, ir: r, owned: false }
                    }
                    (UnOpC::Not, _) => {
                        self.emit(format!("{} = xor i1 {}, true", r, v.ir));
                        CgVal { ty: CgTy::Bool, ir: r, owned: false }
                    }
                    _ => unreachable!(),
                }
            }
            CExpr::Binary(op, l, r, ty) => {
                let res = cg_ty(ty);
                let lv = self.cg_expr(l);
                let rv = self.cg_expr(r);
                // string equality/inequality goes through the runtime helper
                if matches!(op, BinOpC::Eq | BinOpC::Neq)
                    && lv.ty == CgTy::Str
                    && rv.ty == CgTy::Str
                {
                    let amv = self.build_mv(&lv);
                    let bmv = self.build_mv(&rv);
                    let out = self.fresh("bc");
                    self.emit(format!(
                        "{} = call i1 @marina_str_eq(ptr {}, ptr {})",
                        out, amv, bmv
                    ));
                    let ir = if *op == BinOpC::Neq {
                        let nx = self.fresh("bc");
                        self.emit(format!("{} = xor i1 {}, 1", nx, out));
                        nx
                    } else {
                        out
                    };
                    return CgVal { ty: CgTy::Bool, ir, owned: false };
                }
                let lv = self.coerce_val(lv, res);
                let rv = self.coerce_val(rv, res);
                match op {
                    BinOpC::And => {
                        let out = self.fresh("bd");
                        self.emit(format!("{} = and i1 {}, {}", out, lv.ir, rv.ir));
                        return CgVal { ty: CgTy::Bool, ir: out, owned: false };
                    }
                    BinOpC::Or => {
                        let out = self.fresh("bd");
                        self.emit(format!("{} = or i1 {}, {}", out, lv.ir, rv.ir));
                        return CgVal { ty: CgTy::Bool, ir: out, owned: false };
                    }
                    BinOpC::Lt | BinOpC::Gt | BinOpC::Le | BinOpC::Ge | BinOpC::Eq | BinOpC::Neq => {
                        let cmp_ty = if lv.ty == CgTy::Float || rv.ty == CgTy::Float {
                            "double"
                        } else {
                            "i32"
                        };
                        let pred = match (op, cmp_ty) {
                            (BinOpC::Lt, "i32") => "slt",
                            (BinOpC::Lt, _) => "olt",
                            (BinOpC::Gt, "i32") => "sgt",
                            (BinOpC::Gt, _) => "ogt",
                            (BinOpC::Le, "i32") => "sle",
                            (BinOpC::Le, _) => "ole",
                            (BinOpC::Ge, "i32") => "sge",
                            (BinOpC::Ge, _) => "oge",
                            (BinOpC::Eq, "i32") => "eq",
                            (BinOpC::Eq, _) => "oeq",
                            (BinOpC::Neq, "i32") => "ne",
                            (BinOpC::Neq, _) => "one",
                            _ => unreachable!(),
                        };
                        let out = self.fresh("bc");
                        self.emit(format!(
                            "{} = icmp {} {} {}, {}",
                            out, pred, cmp_ty, lv.ir, rv.ir
                        ));
                        return CgVal { ty: CgTy::Bool, ir: out, owned: false };
                    }
                    _ => {}
                }
                let out = self.fresh("ba");
                match (res, op) {
                    (CgTy::Int, BinOpC::Add) => self.emit(format!("{} = add i32 {}, {}", out, lv.ir, rv.ir)),
                    (CgTy::Int, BinOpC::Sub) => self.emit(format!("{} = sub i32 {}, {}", out, lv.ir, rv.ir)),
                    (CgTy::Int, BinOpC::Mul) => self.emit(format!("{} = mul i32 {}, {}", out, lv.ir, rv.ir)),
                    (CgTy::Int, BinOpC::Div) => self.emit(format!("{} = sdiv i32 {}, {}", out, lv.ir, rv.ir)),
                    (CgTy::Int, BinOpC::Mod) => self.emit(format!("{} = srem i32 {}, {}", out, lv.ir, rv.ir)),
                    (CgTy::Float, BinOpC::Add) => self.emit(format!("{} = fadd double {}, {}", out, lv.ir, rv.ir)),
                    (CgTy::Float, BinOpC::Sub) => self.emit(format!("{} = fsub double {}, {}", out, lv.ir, rv.ir)),
                    (CgTy::Float, BinOpC::Mul) => self.emit(format!("{} = fmul double {}, {}", out, lv.ir, rv.ir)),
                    (CgTy::Float, BinOpC::Div) => self.emit(format!("{} = fdiv double {}, {}", out, lv.ir, rv.ir)),
                    _ => unreachable!(),
                }
                CgVal { ty: res, ir: out, owned: false }
            }
            CExpr::CallFn { name, args, ret, is_async } => {
                let sig = self.fn_sigs.get(name).cloned().unwrap();
                let mut call_args = Vec::new();
                for (i, a) in args.iter().enumerate() {
                    let v = self.cg_expr(a);
                    let pt = sig.params.get(i).copied().unwrap_or(cg_ty(ret));
                    let v = self.coerce_val(v, pt);
                    let v = self.to_owned(v);
                    call_args.push(format!("{} {}", pt.ir(), v.ir));
                }
                let rty = if *is_async { CgTy::Promise } else { cg_ty(ret) };
                let fname = format!("fn.{}", name);
                if rty == CgTy::Void {
                    self.emit(format!("call void @{}({})", fname, call_args.join(", ")));
                    CgVal { ty: CgTy::Void, ir: String::new(), owned: false }
                } else {
                    let r = self.fresh("cl");
                    self.emit(format!(
                        "{} = call {} @{}({})",
                        r,
                        rty.ir(),
                        fname,
                        call_args.join(", ")
                    ));
                    CgVal { ty: rty, ir: r, owned: true }
                }
            }
            CExpr::CallResolver { id, arg, inner } => {
                let a = self.access_for(*id).expect("resolver in codegen");
                let (p, _) = self.load_access(&a);
                let v = self.cg_expr(arg);
                let v = self.coerce_val(v, cg_ty(inner));
                let mv = self.build_mv(&v);
                self.emit(format!(
                    "call void @marina_promise_resolve(ptr {}, ptr {})",
                    p, mv
                ));
                CgVal { ty: CgTy::Void, ir: String::new(), owned: false }
            }
            CExpr::Await(..) => unreachable!("await is handled by the segment transform"),
            CExpr::NewPromise { lambda } => {
                let pr = self.fresh("pn");
                self.emit(format!("{} = call ptr @marina_promise_create()", pr));
                let env = self.build_env(*lambda, Some(pr.clone()));
                let own = self.fresh("po");
                self.emit(format!(
                    "{} = call ptr @marina_promise_run_executor(ptr {}, ptr @lambda.{}, ptr {}, ptr @lambda.{}.drop)",
                    own, pr, lambda, env, lambda
                ));
                CgVal { ty: CgTy::Promise, ir: own, owned: true }
            }
            CExpr::ConsoleLog(arg) => {
                let v = self.cg_expr(arg);
                let mv = self.build_mv(&v);
                self.emit(format!("call void @marina_print(ptr {})", mv));
                CgVal { ty: CgTy::Void, ir: String::new(), owned: false }
            }
            CExpr::SetTimeout { lambda, ms } => {
                let env = self.build_env(*lambda, None);
                let m = self.cg_expr(ms);
                let m64 = self.sext_i64(&m);
                self.emit(format!(
                    "call void @marina_set_timeout(ptr @lambda.{}, ptr {}, ptr @lambda.{}.drop, i64 {})",
                    lambda, env, lambda, m64
                ));
                CgVal { ty: CgTy::Void, ir: String::new(), owned: false }
            }
            CExpr::NetConnect { host, port } => {
                let h = self.cg_expr(host);
                let hmv = self.build_mv(&h);
                let p = self.cg_expr(port);
                let pmv = self.build_mv(&p);
                let pr = self.fresh("pr");
                self.emit(format!(
                    "{} = call ptr @marina_net_connect_async(ptr {}, ptr {})",
                    pr, hmv, pmv
                ));
                CgVal { ty: CgTy::Promise, ir: pr, owned: true }
            }
            CExpr::NetWrite { stream, data } => {
                let s = self.cg_expr(stream);
                let smv = self.build_mv(&s);
                let d = self.cg_expr(data);
                let dmv = self.build_mv(&d);
                let pr = self.fresh("pr");
                self.emit(format!("{} = call ptr @marina_net_write_async(ptr {}, ptr {})", pr, smv, dmv));
                CgVal { ty: CgTy::Promise, ir: pr, owned: true }
            }
            CExpr::NetReadAll { stream } => {
                let s = self.cg_expr(stream);
                let smv = self.build_mv(&s);
                let pr = self.fresh("pr");
                self.emit(format!("{} = call ptr @marina_net_read_all_async(ptr {})", pr, smv));
                CgVal { ty: CgTy::Promise, ir: pr, owned: true }
            }
            CExpr::NetClose { stream } => {
                let s = self.cg_expr(stream);
                let smv = self.build_mv(&s);
                let pr = self.fresh("pr");
                self.emit(format!("{} = call ptr @marina_net_close_async(ptr {})", pr, smv));
                CgVal { ty: CgTy::Promise, ir: pr, owned: true }
            }
            CExpr::HttpCall { method, url, body } => {
                let u = self.cg_expr(url);
                let umv = self.build_mv(&u);
                let bm = match body {
                    Some(b) => {
                        let bv = self.cg_expr(b);
                        self.build_mv(&bv)
                    }
                    None => {
                        // GET: pass a void-tagged MValue as the body
                        self.build_mv(&CgVal { ty: CgTy::Void, ir: String::new(), owned: false })
                    }
                };
                let mlit = self.string_lit(method.as_str());
                let mmv = self.build_mv(&CgVal {
                    ty: CgTy::Str,
                    ir: mlit.ir,
                    owned: true,
                });
                let cls = self
                    .c
                    .classes
                    .iter()
                    .position(|d| d.name == "http_Response")
                    .unwrap_or(usize::MAX);
                let pr = self.fresh("pr");
                self.emit(format!(
                    "{} = call ptr @marina_http_request_async(ptr {}, ptr {}, ptr {}, i32 {})",
                    pr, mmv, umv, bm, cls as i32
                ));
                CgVal { ty: CgTy::Promise, ir: pr, owned: true }
            }
            CExpr::HttpListen { port, lambda } => {
                let p = self.cg_expr(port);
                let pmv = self.build_mv(&p);
                let env = self.build_env(*lambda, None);
                let req_cls = self
                    .c
                    .classes
                    .iter()
                    .position(|d| d.name == "http_Request")
                    .unwrap_or(usize::MAX);
                let res_cls = self
                    .c
                    .classes
                    .iter()
                    .position(|d| d.name == "http_Res")
                    .unwrap_or(usize::MAX);
                self.emit(format!(
                    "call void @marina_http_listen(ptr {}, ptr @lambda.{}, ptr {}, ptr @lambda.{}.drop, i32 {}, i32 {})",
                    pmv, lambda, env, lambda, req_cls as i32, res_cls as i32
                ));
                CgVal { ty: CgTy::Void, ir: String::new(), owned: false }
            }
            CExpr::HttpNewServer => {
                let obj = self.fresh("obj");
                self.emit(format!(
                    "{} = call ptr @marina_object_new(i32 {}, i64 0)",
                    obj,
                    self.builtin_class("http_Server")
                ));
                CgVal { ty: CgTy::Object, ir: obj, owned: true }
            }
            CExpr::HttpRouter { server, path } => {
                let s = self.cg_expr(server);
                let _smv = self.build_mv(&s);
                let pv = self.cg_expr(path);
                let pmv = self.build_mv(&pv);
                let obj = self.fresh("obj");
                self.emit(format!(
                    "{} = call ptr @marina_object_new(i32 {}, i64 0)",
                    obj,
                    self.builtin_class("http_Router")
                ));
                let rmv = self.build_mv(&CgVal { ty: CgTy::Object, ir: obj.clone(), owned: true });
                self.emit(format!("call void @marina_http_router_bind(ptr {}, ptr {})", rmv, pmv));
                self.discard(&s);
                CgVal { ty: CgTy::Object, ir: obj, owned: true }
            }
            CExpr::HttpRouteAdd { router, method, path, lambda, is_async } => {
                let r = self.cg_expr(router);
                let rmv = self.build_mv(&r);
                let pv = self.cg_expr(path);
                let pmv = self.build_mv(&pv);
                let mlit = self.string_lit(method.as_str());
                let mmv = self.build_mv(&CgVal { ty: CgTy::Str, ir: mlit.ir, owned: true });
                let env = self.build_env(*lambda, None);
                let aflag = if *is_async { 1 } else { 0 };
                self.emit(format!(
                    "call void @marina_http_route_add(ptr {}, ptr {}, ptr {}, ptr @lambda.{}, ptr {}, ptr @lambda.{}.drop, i32 {})",
                    rmv, mmv, pmv, lambda, env, lambda, aflag
                ));
                self.discard(&r);
                CgVal { ty: CgTy::Void, ir: String::new(), owned: false }
            }
            CExpr::HttpServerListen { host, port } => {
                let hv = self.cg_expr(host);
                let hmv = self.build_mv(&hv);
                let pv = self.cg_expr(port);
                let pmv = self.build_mv(&pv);
                let pr = self.fresh("pn");
                self.emit(format!("{} = call ptr @marina_promise_create()", pr));
                self.emit(format!(
                    "call void @marina_http_server_listen(ptr {}, ptr {}, ptr {}, i32 {}, i32 {})",
                    hmv,
                    pmv,
                    pr,
                    self.builtin_class("http_Request"),
                    self.builtin_class("http_Res")
                ));
                CgVal { ty: CgTy::Promise, ir: pr, owned: true }
            }
            CExpr::HttpValidatorNew { schema, .. } => {
                let s = self.cg_expr(schema);
                let smv = self.build_mv(&s);
                let obj = self.fresh("obj");
                self.emit(format!(
                    "{} = call ptr @marina_object_new(i32 {}, i64 0)",
                    obj,
                    self.builtin_class("http_JsonValidator")
                ));
                self.emit(format!(
                    "call void @marina_http_validator_init(ptr {}, ptr {})",
                    obj, smv
                ));
                self.discard(&s);
                CgVal { ty: CgTy::Object, ir: obj, owned: true }
            }
            CExpr::HttpFieldNew { kind } => {
                let obj = self.fresh("obj");
                self.emit(format!(
                    "{} = call ptr @marina_object_new(i32 {}, i64 0)",
                    obj,
                    self.builtin_class("http_FieldValidator")
                ));
                let k = match kind {
                    FieldKind::Str => 0,
                    FieldKind::Int => 1,
                };
                self.emit(format!("call void @marina_http_field_init(ptr {}, i32 {})", obj, k));
                CgVal { ty: CgTy::Object, ir: obj, owned: true }
            }
            CExpr::HttpFieldOp { v, op, arg } => {
                let vv = self.cg_expr(v);
                let p = self.borrow(&vv);
                let amv = match arg {
                    Some(a) => {
                        let av = self.cg_expr(a);
                        self.build_mv(&av)
                    }
                    None => "null".to_string(),
                };
                let o = match op {
                    FieldOp::Required => 0,
                    FieldOp::Min => 1,
                    FieldOp::Max => 2,
                };
                self.emit(format!("call void @marina_http_field_op(ptr {}, i32 {}, ptr {})", p, o, amv));
                // the same validator object flows on (chaining); the borrow
                // already queued its single release, so return it unowned
                CgVal { ty: vv.ty.clone(), ir: vv.ir.clone(), owned: false }
            }
            CExpr::HttpValidate { v, body, out_class } => {
                let vv = self.cg_expr(v);
                let p = self.borrow(&vv);
                let bv = self.cg_expr(body);
                let bmv = self.build_mv(&bv);
                let out = self.alloca("%mv");
                self.emit(format!(
                    "call void @marina_http_validate(ptr {}, ptr {}, i32 {}, ptr {})",
                    p, bmv, out_class, out
                ));
                self.mv_load(&out, CgTy::Object)
            }
            CExpr::ResSend { res, value, json } => {
                let r = self.cg_expr(res);
                let v = self.cg_expr(value);
                let vmv = self.build_mv(&v);
                let f = if *json { "marina_http_res_json" } else { "marina_http_res_send" };
                self.emit(format!("call void @{}(ptr {}, ptr {})", f, r.ir, vmv));
                CgVal { ty: CgTy::Void, ir: String::new(), owned: false }
            }
            CExpr::ArrayLit { elems, elem_ty } => {
                let tag = mv_tag_of(elem_ty);
                let arr = self.fresh("arr");
                self.emit(format!("{} = call ptr @marina_array_new(i32 {})", arr, tag));
                let ect = cg_ty(elem_ty);
                for e in elems {
                    let v = self.cg_expr(e);
                    let v = self.coerce_val(v, ect);
                    let v = self.to_owned(v);
                    let mv = self.build_mv(&v);
                    self.emit(format!("call i64 @marina_array_push(ptr {}, ptr {})", arr, mv));
                    self.discard(&v);
                }
                CgVal { ty: CgTy::Array, ir: arr, owned: true }
            }
            CExpr::Index { obj, idx, ty } => {
                let o = self.cg_expr(obj);
                let p = self.borrow(&o);
                let iv = self.cg_expr(idx);
                let i = self.sext_i64(&iv);
                let out = self.alloca("%mv");
                self.emit(format!("call void @marina_array_get(ptr {}, i64 {}, ptr {})", p, i, out));
                let v = self.mv_load(&out, cg_ty(ty));
                self.discard(&o);
                v
            }
            CExpr::ArrayOp { op, arr, args, ret } => {
                let a = self.cg_expr(arr);
                let p = self.borrow(&a);
                let v = match op {
                    ArrayOpKind::Len => {
                        let n = self.fresh("an");
                        self.emit(format!("{} = call i64 @marina_array_len(ptr {})", n, p));
                        let t = self.fresh("al");
                        self.emit(format!("{} = trunc i64 {} to i32", t, n));
                        CgVal { ty: CgTy::Int, ir: t, owned: false }
                    }
                    ArrayOpKind::Push | ArrayOpKind::Unshift => {
                        // element type comes from the array expression
                        let elem = cexpr_cty(arr)
                            .map(|t| match t {
                                CTy::Array(e) => *e,
                                _ => t,
                            })
                            .map(|t| cg_ty(&t));
                        let av = self.cg_expr(&args[0]);
                        let av = match elem {
                            Some(et) => self.coerce_val(av, et),
                            None => av,
                        };
                        let av = self.to_owned(av);
                        let mv = self.build_mv(&av);
                        let f = if *op == ArrayOpKind::Push {
                            "marina_array_push"
                        } else {
                            "marina_array_unshift"
                        };
                        let n = self.fresh("an");
                        self.emit(format!("{} = call i64 @{}(ptr {}, ptr {})", n, f, p, mv));
                        let t = self.fresh("al");
                        self.emit(format!("{} = trunc i64 {} to i32", t, n));
                        self.discard(&av);
                        CgVal { ty: CgTy::Int, ir: t, owned: false }
                    }
                    ArrayOpKind::Pop | ArrayOpKind::Shift => {
                        let out = self.alloca("%mv");
                        let f = if *op == ArrayOpKind::Pop {
                            "marina_array_pop"
                        } else {
                            "marina_array_shift"
                        };
                        self.emit(format!("call void @{}(ptr {}, ptr {})", f, p, out));
                        self.mv_load(&out, cg_ty(ret))
                    }
                    ArrayOpKind::Splice | ArrayOpKind::Slice => {
                        let s = self.cg_expr(&args[0]);
                        let s64 = self.sext_i64(&s);
                        let e = self.cg_expr(&args[1]);
                        let e64 = self.sext_i64(&e);
                        let f = if *op == ArrayOpKind::Splice {
                            "marina_array_splice"
                        } else {
                            "marina_array_slice"
                        };
                        let r = self.fresh("ar");
                        self.emit(format!(
                            "{} = call ptr @{}(ptr {}, i64 {}, i64 {})",
                            r, f, p, s64, e64
                        ));
                        CgVal { ty: CgTy::Array, ir: r, owned: true }
                    }
                    ArrayOpKind::Concat => {
                        let b = self.cg_expr(&args[0]);
                        let r = self.fresh("ar");
                        self.emit(format!("{} = call ptr @marina_array_concat(ptr {}, ptr {})", r, p, b.ir));
                        self.discard(&b);
                        CgVal { ty: CgTy::Array, ir: r, owned: true }
                    }
                    ArrayOpKind::Join => {
                        let s = self.cg_expr(&args[0]);
                        let r = self.fresh("js");
                        self.emit(format!("{} = call ptr @marina_array_join(ptr {}, ptr {})", r, p, s.ir));
                        self.discard(&s);
                        CgVal { ty: CgTy::Str, ir: r, owned: true }
                    }
                    ArrayOpKind::Reverse | ArrayOpKind::Sort => {
                        let f = if *op == ArrayOpKind::Reverse {
                            "marina_array_reverse"
                        } else {
                            "marina_array_sort"
                        };
                        let r = self.fresh("ar");
                        self.emit(format!("{} = call ptr @{}(ptr {})", r, f, p));
                        CgVal { ty: CgTy::Array, ir: r, owned: true }
                    }
                };
                self.discard(&a);
                v
            }
            CExpr::ArrayHof { op, arr, cb, extra, ret } => {
                let a = self.cg_expr(arr);
                let p = self.borrow(&a);
                let li = match &**cb {
                    CExpr::LambdaVal { li, .. } => *li,
                    _ => unreachable!("array callback must be a lambda"),
                };
                let env = self.build_env(li, None);
                let v = match op {
                    HofKind::ForEach => {
                        self.emit(format!(
                            "call void @marina_array_foreach(ptr {}, ptr @lambda.{}, ptr {}, ptr @lambda.{}.drop)",
                            p, li, env, li
                        ));
                        CgVal { ty: CgTy::Void, ir: String::new(), owned: false }
                    }
                    HofKind::Map => {
                        let out_tag = match ret {
                            CTy::Array(e) => mv_tag_of(e),
                            _ => unreachable!("map returns Array"),
                        };
                        let r = self.fresh("ar");
                        self.emit(format!(
                            "{} = call ptr @marina_array_map(ptr {}, ptr @lambda.{}, ptr {}, ptr @lambda.{}.drop, i32 {})",
                            r, p, li, env, li, out_tag
                        ));
                        CgVal { ty: CgTy::Array, ir: r, owned: true }
                    }
                    HofKind::Filter => {
                        let r = self.fresh("ar");
                        self.emit(format!(
                            "{} = call ptr @marina_array_filter(ptr {}, ptr @lambda.{}, ptr {}, ptr @lambda.{}.drop)",
                            r, p, li, env, li
                        ));
                        CgVal { ty: CgTy::Array, ir: r, owned: true }
                    }
                    HofKind::Reduce => {
                        let rt = cg_ty(ret);
                        let init = self.cg_expr(extra.as_ref().unwrap());
                        let init = self.coerce_val(init, rt);
                        let init = self.to_owned(init);
                        let imv = self.build_mv(&init);
                        let out = self.alloca("%mv");
                        self.emit(format!(
                            "call void @marina_array_reduce(ptr {}, ptr @lambda.{}, ptr {}, ptr @lambda.{}.drop, ptr {}, ptr {})",
                            p, li, env, li, imv, out
                        ));
                        let rv = self.mv_load(&out, rt);
                        self.discard(&init);
                        rv
                    }
                };
                self.discard(&a);
                v
            }
            CExpr::StrLen(inner) => {
                let s = self.cg_expr(inner);
                let p = self.borrow(&s);
                let n = self.fresh("sl");
                self.emit(format!("{} = call i64 @marina_string_len(ptr {})", n, p));
                let t = self.fresh("slt");
                self.emit(format!("{} = trunc i64 {} to i32", t, n));
                self.discard(&s);
                CgVal { ty: CgTy::Int, ir: t, owned: false }
            }
            CExpr::LambdaVal { .. } => unreachable!("lambda values are consumed by call sites"),
            CExpr::ObjectLit { class_id, inits } => {
                let n = self.c.classes[*class_id].fields.len();
                let obj = self.fresh("obj");
                self.emit(format!(
                    "{} = call ptr @marina_object_new(i32 {}, i64 {})",
                    obj, class_id, n
                ));
                for (idx, e) in inits {
                    let fty = self.c.classes[*class_id].fields[*idx].1.clone();
                    let v = self.cg_expr(e);
                    let v = self.coerce_val(v, cg_ty(&fty));
                    let v = self.to_owned(v);
                    let mv = self.build_mv(&v);
                    self.emit(format!(
                        "call void @marina_object_set(ptr {}, i64 {}, ptr {})",
                        obj, idx, mv
                    ));
                    self.discard(&v);
                }
                CgVal { ty: CgTy::Object, ir: obj, owned: true }
            }
            CExpr::NewClass { class_id } => {
                let n = self.c.classes[*class_id].fields.len();
                let obj = self.fresh("obj");
                self.emit(format!(
                    "{} = call ptr @marina_object_new(i32 {}, i64 {})",
                    obj, class_id, n
                ));
                CgVal { ty: CgTy::Object, ir: obj, owned: true }
            }
            CExpr::FieldGet { obj, idx, ty } => {
                let o = self.cg_expr(obj);
                let p = self.borrow(&o);
                let out = self.alloca("%mv");
                self.emit(format!(
                    "call void @marina_object_get(ptr {}, i64 {}, ptr {})",
                    p, idx, out
                ));
                let v = self.mv_load(&out, cg_ty(ty));
                self.discard(&o);
                v
            }
            CExpr::IfaceGet { obj, field, ty } => {
                // interface-typed values resolve fields by name at runtime
                let o = self.cg_expr(obj);
                let p = self.borrow(&o);
                let sv = self.string_lit(field.as_str());
                let nmv = self.build_mv(&sv);
                let out = self.alloca("%mv");
                self.emit(format!(
                    "call void @marina_object_get_field(ptr {}, ptr {}, ptr {})",
                    p, nmv, out
                ));
                let v = self.mv_load(&out, cg_ty(ty));
                self.discard(&sv);
                self.discard(&o);
                v
            }
            // never generated (interface writes are statements); keeps the match exhaustive
            CExpr::IfaceSet { .. } => CgVal { ty: CgTy::Void, ir: String::new(), owned: false },
            CExpr::FsReadFile { path } => {
                let p = self.cg_expr(path);
                let pmv = self.build_mv(&p);
                let pr = self.fresh("pr");
                self.emit(format!("{} = call ptr @marina_fs_read_file_async(ptr {})", pr, pmv));
                CgVal { ty: CgTy::Promise, ir: pr, owned: true }
            }
            CExpr::FsWriteFile { path, data } => {
                let p = self.cg_expr(path);
                let pmv = self.build_mv(&p);
                let d = self.cg_expr(data);
                let dmv = self.build_mv(&d);
                let pr = self.fresh("pr");
                self.emit(format!("{} = call ptr @marina_fs_write_file_async(ptr {}, ptr {})", pr, pmv, dmv));
                self.discard(&d);
                CgVal { ty: CgTy::Promise, ir: pr, owned: true }
            }
            CExpr::FsExists { path } => {
                let p = self.cg_expr(path);
                let pmv = self.build_mv(&p);
                let pr = self.fresh("pr");
                self.emit(format!("{} = call ptr @marina_fs_exists_async(ptr {})", pr, pmv));
                CgVal { ty: CgTy::Promise, ir: pr, owned: true }
            }
            CExpr::FsReadDir { path } => {
                let p = self.cg_expr(path);
                let pmv = self.build_mv(&p);
                let pr = self.fresh("pr");
                self.emit(format!("{} = call ptr @marina_fs_read_dir_async(ptr {})", pr, pmv));
                CgVal { ty: CgTy::Promise, ir: pr, owned: true }
            }
            CExpr::FsMkdir { path } => {
                let p = self.cg_expr(path);
                let pmv = self.build_mv(&p);
                let pr = self.fresh("pr");
                self.emit(format!("{} = call ptr @marina_fs_mkdir_async(ptr {})", pr, pmv));
                CgVal { ty: CgTy::Promise, ir: pr, owned: true }
            }
            CExpr::FsRemove { path } => {
                let p = self.cg_expr(path);
                let pmv = self.build_mv(&p);
                let pr = self.fresh("pr");
                self.emit(format!("{} = call ptr @marina_fs_remove_async(ptr {})", pr, pmv));
                CgVal { ty: CgTy::Promise, ir: pr, owned: true }
            }
            CExpr::CryptoSha256 { v } => {
                let d = self.cg_expr(v);
                let dmv = self.build_mv(&d);
                let out = self.alloca("%mv");
                self.emit(format!("call void @marina_crypto_sha256(ptr {}, ptr {})", dmv, out));
                self.discard(&d);
                self.mv_load(&out, CgTy::Str)
            }
            CExpr::CryptoBase64 { decode, v } => {
                let d = self.cg_expr(v);
                let dmv = self.build_mv(&d);
                let out = self.alloca("%mv");
                self.emit(format!(
                    "call void @marina_crypto_base64(i1 {}, ptr {}, ptr {})",
                    if *decode { "true" } else { "false" },
                    dmv,
                    out
                ));
                self.discard(&d);
                self.mv_load(&out, CgTy::Str)
            }
            CExpr::CryptoRandomHex { bytes } => {
                let b = self.cg_expr(bytes);
                let n = self.fresh("nb");
                self.emit(format!("{} = sext i32 {} to i64", n, b.ir));
                let out = self.alloca("%mv");
                self.emit(format!("call void @marina_crypto_random_hex(i64 {}, ptr {})", n, out));
                self.discard(&b);
                self.mv_load(&out, CgTy::Str)
            }
            CExpr::CryptoUuid => {
                let out = self.alloca("%mv");
                self.emit(format!("call void @marina_crypto_uuid(ptr {})", out));
                self.mv_load(&out, CgTy::Str)
            }
            CExpr::SqlOpen { driver, url } => {
                let d = self.cg_expr(driver);
                let dmv = self.build_mv(&d);
                let u = self.cg_expr(url);
                let umv = self.build_mv(&u);
                let cls = self.builtin_class("sql_Connection");
                let out = self.alloca("%mv");
                self.emit(format!(
                    "call void @marina_sql_open(ptr {}, ptr {}, i32 {}, ptr {})",
                    dmv, umv, cls, out
                ));
                self.discard(&d);
                self.discard(&u);
                self.mv_load(&out, CgTy::Object)
            }
            CExpr::SqlExec { conn, sql, binds, result_class } => {
                let cv = self.cg_expr(conn);
                // cg_sql_call borrows the connection (single queued release)
                let pr = self.cg_sql_call_async(&cv, sql, binds, Some(*result_class));
                CgVal { ty: CgTy::Promise, ir: pr, owned: true }
            }
            CExpr::SqlQuery { conn, sql, binds } => {
                let cv = self.cg_expr(conn);
                let pr = self.cg_sql_call_async(&cv, sql, binds, None);
                CgVal { ty: CgTy::Promise, ir: pr, owned: true }
            }
            CExpr::SqlClose { conn } => {
                let cv = self.cg_expr(conn);
                let p = self.borrow(&cv);
                self.emit(format!("call void @marina_sql_close(ptr {})", p));
                CgVal { ty: CgTy::Void, ir: String::new(), owned: false }
            }
        }
    }

    /// Async variant of `cg_sql_call`: emits the `_async` runtime call and
    /// returns the promise pointer.
    fn cg_sql_call_async(
        &mut self,
        conn: &CgVal,
        sql: &str,
        binds: &[CExpr],
        result_class: Option<usize>,
    ) -> String {
        let p = self.borrow(conn);
        let sv = self.string_lit(sql);
        let smv = self.build_mv(&sv);
        let n = binds.len().max(1);
        let buf = self.fresh("bnds");
        self.fns[self.cur]
            .allocas
            .push(format!("{} = alloca %mv, i64 {}", buf, n));
        for (i, b) in binds.iter().enumerate() {
            let bv = self.cg_expr(b);
            let bmv = self.build_mv(&bv);
            let ld = self.fresh("bl");
            self.emit(format!("{} = load %mv, ptr {}", ld, bmv));
            let slot = self.fresh("bs");
            self.emit(format!(
                "{} = getelementptr inbounds %mv, ptr {}, i64 {}",
                slot, buf, i
            ));
            self.emit(format!("store %mv {}, ptr {}", ld, slot));
            self.discard(&bv);
        }
        let pr = self.fresh("pr");
        match result_class {
            Some(cls) => {
                self.emit(format!(
                    "{} = call ptr @marina_sql_execute_async(ptr {}, ptr {}, i64 {}, ptr {}, i32 {})",
                    pr,
                    p,
                    smv,
                    binds.len() as i64,
                    buf,
                    cls
                ));
            }
            None => {
                self.emit(format!(
                    "{} = call ptr @marina_sql_query_async(ptr {}, ptr {}, i64 {}, ptr {})",
                    pr,
                    p,
                    smv,
                    binds.len() as i64,
                    buf
                ));
            }
        }
        self.discard(&sv);
        pr
    }

    /// Lambda layout: executor params first (Plain ABI only), then captured
/// variables. Callback lambdas take their parameters as IR arguments, so
/// only captures occupy env slots.
fn lambda_slots(&self, li: usize) -> Vec<(usize, CTy)> {
    let lam = &self.c.lambdas[li];
    let mut slots: Vec<(usize, CTy)> = Vec::new();
    if lam.abi == LambdaAbi::Plain {
        slots.extend(lam.params.iter().cloned());
    }
    slots.extend(lam.captures.iter().cloned());
    slots
}

    fn env_type_def(&mut self, li: usize) -> String {
        let ty = format!("%env.{}", li);
        if self.env_types_done.insert(li) {
            let tys: Vec<String> = self
                .lambda_slots(li)
                .iter()
                .map(|(_, t)| cg_ty(t).mem().to_string())
                .collect();
            self.type_defs.push(format!("{} = type {{ {} }}", ty, tys.join(", ")));
        }
        ty
    }

    /// Allocate + populate a closure environment for lambda `li`.
    /// `preset` fills env slot 0 directly (the executor's promise).
    /// Class id of a builtin std-module class (http_Server, …).
fn builtin_class(&self, name: &str) -> i32 {
        self.c
            .classes
            .iter()
            .position(|d| d.name == name)
            .unwrap_or(usize::MAX) as i32
    }

    fn build_env(&mut self, li: usize, preset: Option<String>) -> String {
        let slots = self.lambda_slots(li);
        let ty = self.env_type_def(li);

        let sz = if slots.is_empty() {
            "i64 8".to_string()
        } else {
            format!(
                "i64 ptrtoint (ptr getelementptr inbounds ({}, ptr null, i64 1) to i64)",
                ty
            )
        };
        let envp = self.fresh("en");
        self.emit(format!("{} = call ptr @marina_closure_alloc({})", envp, sz));

        for (i, (id, t)) in slots.iter().enumerate() {
            let ct = cg_ty(t);
            // address this env directly (self.env is not set in the
            // enclosing function, so we can't use gep_ssa here)
            let gep = self.fresh("eg");
            self.emit(format!(
                "{} = getelementptr inbounds {}, ptr {}, i64 0, i32 {}",
                gep, ty, envp, i
            ));
            if i == 0 {
                if let Some(p) = &preset {
                    // env slots own their refs: retain the preset promise
                    let r = self.fresh("ep");
                    self.emit(format!("{} = call ptr @marina_retain(ptr {})", r, p));
                    self.emit(format!("store ptr {}, ptr {}", r, gep));
                    continue;
                }
            }
            let a = self.access_for(*id).expect("capture not found in codegen");
            let (v, _) = self.load_access(&a);
            if ct.is_ptr() {
                let r = self.fresh("cp");
                self.emit(format!("{} = call ptr @marina_retain(ptr {})", r, v));
                self.emit(format!("store ptr {}, ptr {}", r, gep));
            } else {
                self.emit(format!("store {} {}, ptr {}", ct.ir(), v, gep));
            }
        }
        envp
    }

    // ------------------------------------------------------------------
    // Lambdas
    // ------------------------------------------------------------------

    fn gen_lambda(&mut self, li: usize) {
        let lam = &self.c.lambdas[li];
        if lam.is_async && lam.abi == LambdaAbi::Listen {
            // async route handler: shim + promise state machine
            self.gen_async_listen_lambda(li);
            return;
        }
        let (abi, lret) = {
            let lam = &self.c.lambdas[li];
            (lam.abi, lam.ret.clone())
        };
        let slots = self.lambda_slots(li);
        let body = self.c.lambdas[li].body.clone();
        let ty = self.env_type_def(li);

        // ---- lambda body ----
        // IR signature depends on the callback ABI; extra params are raw
        // pointers to MValues passed by the runtime.
        let mut ir_params = vec!["ptr %env".to_string()];
        match abi {
            LambdaAbi::Plain => {}
            LambdaAbi::ForEach => ir_params.push("ptr %item".to_string()),
            LambdaAbi::MapFilter => {
                ir_params.push("ptr %item".to_string());
                ir_params.push("ptr %out".to_string());
            }
            LambdaAbi::Reduce => {
                ir_params.push("ptr %acc".to_string());
                ir_params.push("ptr %item".to_string());
                ir_params.push("ptr %out".to_string());
            }
            LambdaAbi::Listen => {
                ir_params.push("ptr %req".to_string());
                ir_params.push("ptr %res".to_string());
            }
        }
        self.push_fn(IrFn {
            name: format!("lambda.{}", li),
            ret: "void",
            params: ir_params,
            allocas: Vec::new(),
            blocks: vec![IrBlock { label: "entry".into(), instrs: Vec::new(), term: None }],
            frame_ty: None,
        });
        self.cur_ret = CgTy::Void;
        let exit_b = self.new_block_fixed("exit".to_string());
        self.lambda_exit = Some(exit_b);
        self.env = Some(EnvInfo {
            slot_tys: slots.iter().map(|(_, t)| cg_ty(t)).collect(),
            map: slots
                .iter()
                .enumerate()
                .map(|(i, (id, _))| (*id, i))
                .collect(),
            ty: ty.clone(),
        });
        for (i, (id, t)) in slots.iter().enumerate() {
            self.scopes[0].insert(*id, Access::Env(i, cg_ty(t)));
        }
        // Callback parameters arrive as MValue pointers; unpack them into
        // allocas owned by the lambda body. The runtime retains string/array
        // payloads before the call, so these start out owned.
        if abi != LambdaAbi::Plain {
            let lam = &self.c.lambdas[li];
            let arg_names: Vec<&str> = match abi {
                LambdaAbi::ForEach => vec!["%item"],
                LambdaAbi::MapFilter => vec!["%item"],
                LambdaAbi::Reduce => vec!["%acc", "%item"],
                LambdaAbi::Listen => vec!["%req", "%res"],
                LambdaAbi::Plain => vec![],
            };
            for (i, (id, t)) in lam.params.iter().enumerate() {
                let ct = cg_ty(t);
                let a = self.alloca(ct.mem());
                let src = arg_names[i];
                self.store_from_mv(src, ct, &a);
                self.scopes[0].insert(*id, Access::Alloca(a, ct));
            }
            if matches!(abi, LambdaAbi::MapFilter | LambdaAbi::Reduce) {
                self.lambda_out = Some("%out".to_string());
                self.lambda_ret = Some(cg_ty(&lret));
            }
        }
        self.cg_body(&body);
        if !self.terminated() {
            if let Some(out) = &self.lambda_out.clone() {
                // fell off the end of a map/filter/reduce callback: write a
                // void value (the checker requires a return; defensive)
                self.emit(format!("store i32 0, ptr {}", out));
                let g = self.mv_field(out, 1);
                self.emit(format!("store i32 0, ptr {}", g));
                self.flush_pending();
                self.release_scope_vars();
                let el = self.label_of(exit_b);
                self.term(format!("br label %{}", el));
            } else {
                self.sync_epilogue(None);
            }
        }
        self.switch_block(exit_b);
        self.term("ret void".to_string());
        self.env = None;
        self.lambda_out = None;
        self.lambda_exit = None;
        self.lambda_ret = None;

// ---- drop function (called by the runtime when the env dies) ----
        self.push_fn(IrFn {
            name: format!("lambda.{}.drop", li),
            ret: "void",
            params: vec!["ptr %env".to_string()],
            allocas: Vec::new(),
            blocks: vec![IrBlock { label: "entry".into(), instrs: Vec::new(), term: None }],
            frame_ty: None,
        });
        self.env = Some(EnvInfo {
            slot_tys: slots.iter().map(|(_, t)| cg_ty(t)).collect(),
            map: slots
                .iter()
                .enumerate()
                .map(|(i, (id, _))| (*id, i))
                .collect(),
            ty: ty.clone(),
        });
        for (i, (_, t)) in slots.iter().enumerate() {
            let ct = cg_ty(t);
            if ct.is_ptr() {
                let gep = self.gep_ssa(&Access::Env(i, ct));
                let v = self.fresh("dl");
                self.emit(format!("{} = load ptr, ptr {}", v, gep));
                self.emit(format!("call void @marina_release(ptr {})", v));
            }
        }
        self.emit("call void @marina_closure_free(ptr %env)".to_string());
        self.term("ret void".to_string());
        self.env = None;
    }

    // ------------------------------------------------------------------
    // Sync functions
    // ------------------------------------------------------------------

    fn gen_sync_fn_into(&mut self, name: String, params: &[(usize, CTy)], ret: &CTy, body: &[CStmt]) {
        let ir_ret = cg_ty(ret).ir();
        let mut ir_params = Vec::new();
        for (i, (_, t)) in params.iter().enumerate() {
            ir_params.push(format!("{} %a{}", cg_ty(t).ir(), i));
        }
        self.push_fn(IrFn {
            name,
            ret: ir_ret,
            params: ir_params,
            allocas: Vec::new(),
            blocks: vec![IrBlock { label: "entry".into(), instrs: Vec::new(), term: None }],
            frame_ty: None,
        });
        self.cur_ret = cg_ty(ret);
        for (i, (id, t)) in params.iter().enumerate() {
            let ct = cg_ty(t);
            let a = Access::Alloca(self.alloca(ct.mem()), ct);
            self.store_access(&a, &format!("%a{}", i), ct);
            self.scopes[0].insert(*id, a);
        }
        self.cg_body(body);
        if !self.terminated() {
            self.sync_epilogue(None);
        }
    }

    fn gen_sync_fn(&mut self, f: &CFn) {
        let name = format!("fn.{}", f.name);
        let params = f.params.clone();
        let ret = f.ret.clone();
        let body = f.body.clone();
        self.gen_sync_fn_into(name, &params, &ret, &body);
    }

    /// An `async (req, res) => { ... }` route handler compiles to:
    ///   * `lambda.N` — the Listen-ABI shim the runtime registers; returns
    ///     the handler's promise,
    ///   * `lambda.N.async` — state machine entry: creates the promise,
    ///     copies captures + (req, res) into the frame, runs segment 0,
    ///   * `lambda.N.async.resume` — continuations after each await,
    ///   * `lambda.N.drop` — env cleanup (unchanged).
    fn gen_async_listen_lambda(&mut self, li: usize) {
        let lret = self.c.lambdas[li].ret.clone();
        let captures = self.lambda_slots(li); // Listen ABI: captures only
        let lam_params: Vec<(usize, CTy)> = self.c.lambdas[li].params.clone();
        let body = self.c.lambdas[li].body.clone();
        let fty = format!("%frame.lambda.{}", li);
        let entry_name = format!("lambda.{}.async", li);

        // ---- 1. shim: the runtime calls this; it returns the promise ----
        self.push_fn(IrFn {
            name: format!("lambda.{}", li),
            ret: "ptr",
            params: vec!["ptr %env".into(), "ptr %req".into(), "ptr %res".into()],
            allocas: Vec::new(),
            blocks: vec![IrBlock { label: "entry".into(), instrs: Vec::new(), term: None }],
            frame_ty: None,
        });
        self.cur_ret = CgTy::Promise;
        let sp = self.fresh("sp");
        self.emit(format!(
            "{} = call ptr @{}(ptr %env, ptr %req, ptr %res)",
            sp, entry_name
        ));
        self.term(format!("ret ptr {}", sp));

        // ---- 2. state machine frame ----
        self.frame = Some(FrameInfo {
            ty: fty.clone(),
            slots: vec![CgTy::Promise],
            map: HashMap::new(),
        });
        let env_ty = self.env_type_def(li);
        self.env = Some(EnvInfo {
            slot_tys: captures.iter().map(|(_, t)| cg_ty(t)).collect(),
            map: captures.iter().enumerate().map(|(i, (id, _))| (*id, i)).collect(),
            ty: env_ty.clone(),
        });

        // ---- 3. entry: create promise + frame, unpack, run segment 0 ----
        self.push_fn(IrFn {
            name: entry_name.clone(),
            ret: "ptr",
            params: vec!["ptr %env".into(), "ptr %req".into(), "ptr %res".into()],
            allocas: Vec::new(),
            blocks: vec![IrBlock { label: "entry".into(), instrs: Vec::new(), term: None }],
            frame_ty: Some(fty.clone()),
        });
        self.cur_ret = CgTy::Promise;
        let pr = self.fresh("pr");
        self.emit(format!("{} = call ptr @marina_promise_create()", pr));
        {
            let gep = self.gep_ssa(&Access::Frame(0, CgTy::Promise));
            self.emit(format!("store ptr {}, ptr {}", pr, gep));
        }
        // captures: the env owns its refs; the frame gets its own (retained)
        for (i, (id, t)) in captures.iter().enumerate() {
            let ct = cg_ty(t);
            let slot = self.slot_for(*id, ct);
            let gep = self.gep_ssa(&Access::Frame(slot, ct));
            if ct.is_ptr() {
                let egep = self.gep_ssa(&Access::Env(i, ct));
                let v = self.fresh("cv");
                self.emit(format!("{} = load ptr, ptr {}", v, egep));
                let r = self.fresh("cr");
                self.emit(format!("{} = call ptr @marina_retain(ptr {})", r, v));
                self.emit(format!("store ptr {}, ptr {}", r, gep));
            } else {
                let egep = self.gep_ssa(&Access::Env(i, ct));
                let v = self.fresh("cv");
                self.emit(format!("{} = load {}, ptr {}", v, ct.ir(), egep));
                self.emit(format!("store {} {}, ptr {}", ct.ir(), v, gep));
            }
        }
        // Listen-ABI params arrive as pointers to MValues (rc 1 handed over)
        for (i, (id, t)) in lam_params.iter().enumerate() {
            let ct = cg_ty(t);
            let slot = self.slot_for(*id, ct);
            let gep = self.gep_ssa(&Access::Frame(slot, ct));
            let src = if i == 0 { "%req" } else { "%res" };
            self.store_from_mv(src, ct, &gep);
        }
        let rv = self.fresh("rv");
        self.emit(format!("{} = call ptr @marina_retain(ptr {})", rv, pr));
        self.emit(format!(
            "call void @{}.resume(ptr %fr, i32 0, ptr null)",
            entry_name
        ));
        self.term(format!("ret ptr {}", rv));

        // ---- 4. resume: continuations (mirrors gen_async_fn) ----
        let mut segs: Vec<Segment> = Vec::new();
        self.transform(body, &mut segs);
        self.push_fn(IrFn {
            name: format!("{}.resume", entry_name),
            ret: "void",
            params: vec!["ptr %fr".into(), "i32 %st".into(), "ptr %val".into()],
            allocas: Vec::new(),
            blocks: vec![IrBlock { label: "pro".into(), instrs: Vec::new(), term: None }],
            frame_ty: None,
        });
        self.cur_ret = CgTy::Void;

        let mut vblock: Vec<Option<usize>> = Vec::new();
        let mut sblock: Vec<usize> = Vec::new();
        for k in 0..segs.len() {
            if k > 0 {
                vblock.push(Some(self.new_block_fixed(format!("seg.{}v", k))));
            } else {
                vblock.push(None);
            }
            sblock.push(self.new_block_fixed(format!("seg.{}", k)));
        }
        let await_targets: Vec<Option<(usize, CTy)>> = segs
            .iter()
            .map(|s| match &s.end {
                SegEnd::Await { target, .. } => target.clone(),
                _ => None,
            })
            .collect();
        for (k, seg) in segs.into_iter().enumerate() {
            if k > 0 {
                self.switch_block(vblock[k].unwrap());
                if let Some((id, ty)) = await_targets[k - 1].clone() {
                    self.materialize_await_value(id, &ty);
                }
                let sl = self.label_of(sblock[k]);
                self.term(format!("br label %{}", sl));
            }
            self.switch_block(sblock[k]);
            self.scopes.push(HashMap::new());
            self.cg_body(&seg.stmts);
            if !self.terminated() {
                match &seg.end {
                    SegEnd::Await { expr, next, .. } => {
                        let v = self.cg_expr(expr);
                        let v = self.to_owned(v);
                        self.emit(format!(
                            "call void @marina_await(ptr {}, ptr @{}.resume, ptr %fr, i32 {})",
                            v.ir, entry_name, next
                        ));
                        self.flush_pending();
                        self.term("ret void".to_string());
                    }
                    SegEnd::ReturnExpr(e) => {
                        let v = self.cg_expr(e);
                        let v = self.coerce_val(v, cg_ty(&lret));
                        self.finish_async(Some(v));
                    }
                    SegEnd::ReturnVoid => self.finish_async(None),
                    SegEnd::ResolveReturn { id, ty } => {
                        let slot = self.slot_for(*id, cg_ty(ty));
                        let (v, _) = self.load_access(&Access::Frame(slot, cg_ty(ty)));
                        self.finish_async(Some(CgVal { ty: cg_ty(ty), ir: v, owned: false }));
                    }
                }
            }
            self.pop_scope();
        }

        let mut sw = format!("switch i32 %st, label %{} [", self.label_of(sblock[0]));
        for k in 1..sblock.len() {
            sw.push_str(&format!(
                " i32 {}, label %{}",
                k,
                self.label_of(vblock[k].unwrap())
            ));
        }
        sw.push_str(" ]");
        self.switch_block(0);
        self.term(sw);

        let slots = self.frame.as_ref().unwrap().slots.clone();
        let tys: Vec<String> = slots.iter().map(|t| t.mem().to_string()).collect();
        self.type_defs
            .push(format!("{} = type {{ {} }}", fty, tys.join(", ")));
        self.frame = None;

        // ---- 5. drop function (env cleanup, same as sync lambdas) ----
        self.push_fn(IrFn {
            name: format!("lambda.{}.drop", li),
            ret: "void",
            params: vec!["ptr %env".to_string()],
            allocas: Vec::new(),
            blocks: vec![IrBlock { label: "entry".into(), instrs: Vec::new(), term: None }],
            frame_ty: None,
        });
        for (i, (_, t)) in captures.iter().enumerate() {
            let ct = cg_ty(t);
            if ct.is_ptr() {
                let gep = self.gep_ssa(&Access::Env(i, ct));
                let v = self.fresh("dl");
                self.emit(format!("{} = load ptr, ptr {}", v, gep));
                self.emit(format!("call void @marina_release(ptr {})", v));
            }
        }
        self.emit("call void @marina_closure_free(ptr %env)".to_string());
        self.term("ret void".to_string());
        self.env = None;
    }

    // ------------------------------------------------------------------
    // Async functions
    // ------------------------------------------------------------------

    /// Split an async body into linear segments at await points.
    fn transform(&mut self, stmts: Vec<CStmt>, segs: &mut Vec<Segment>) {
        let mut cur: Vec<CStmt> = Vec::new();
        for s in stmts {
            match s {
                CStmt::Let { id, ty, init: CExpr::Await(inner, _) } => {
                    segs.push(Segment {
                        stmts: std::mem::take(&mut cur),
                        end: SegEnd::Await {
                            expr: *inner,
                            target: Some((id, ty)),
                            next: segs.len() + 1,
                        },
                    });
                }
                CStmt::Expr(e @ CExpr::Await(..)) => {
                    if let CExpr::Await(inner, _) = e {
                        segs.push(Segment {
                            stmts: std::mem::take(&mut cur),
                            end: SegEnd::Await {
                                expr: *inner,
                                target: None,
                                next: segs.len() + 1,
                            },
                        });
                    }
                }
                CStmt::Return(Some(e), ReturnKind::Plain) => {
                    if let CExpr::Await(inner, ity) = e {
                        // `return await e` == await then resolve
                        let tmp = self.fresh_var_id();
                        segs.push(Segment {
                            stmts: std::mem::take(&mut cur),
                            end: SegEnd::Await {
                                expr: *inner,
                                target: Some((tmp, ity.clone())),
                                next: segs.len() + 1,
                            },
                        });
                        segs.push(Segment {
                            stmts: vec![],
                            end: SegEnd::ResolveReturn { id: tmp, ty: ity },
                        });
                    } else {
                        segs.push(Segment {
                            stmts: std::mem::take(&mut cur),
                            end: SegEnd::ReturnExpr(e),
                        });
                    }
                    return;
                }
                CStmt::Return(Some(e), ReturnKind::FlattenPromise(inner)) => {
                    // `return promiseExpr`: await the inner promise, then
                    // resolve our own promise with its value (JS flattening).
                    let tmp = self.fresh_var_id();
                    segs.push(Segment {
                        stmts: std::mem::take(&mut cur),
                        end: SegEnd::Await {
                            expr: e,
                            target: Some((tmp, inner.clone())),
                            next: segs.len() + 1,
                        },
                    });
                    segs.push(Segment {
                        stmts: vec![],
                        end: SegEnd::ResolveReturn { id: tmp, ty: inner },
                    });
                    return;
                }
                CStmt::Return(None, _) => {
                    segs.push(Segment {
                        stmts: std::mem::take(&mut cur),
                        end: SegEnd::ReturnVoid,
                    });
                    return;
                }
                other => cur.push(other),
            }
        }
        segs.push(Segment {
            stmts: cur,
            end: SegEnd::ReturnVoid,
        });
    }

    fn gen_async_fn(&mut self, f: &CFn) {
        let fname = format!("fn.{}", f.name);
        let body = f.body.clone();

        // 1. transform into segments
        let mut segs: Vec<Segment> = Vec::new();
        self.transform(body, &mut segs);

        // 2. frame layout: slot 0 = promise
        let fty = format!("%frame.{}", fname);
        self.frame = Some(FrameInfo {
            ty: fty.clone(),
            slots: vec![CgTy::Promise],
            map: HashMap::new(),
        });

        // 3. entry function
        let mut ir_params = Vec::new();
        for (i, (_, t)) in f.params.iter().enumerate() {
            ir_params.push(format!("{} %a{}", cg_ty(t).ir(), i));
        }
        self.push_fn(IrFn {
            name: fname.clone(),
            ret: "ptr",
            params: ir_params,
            allocas: Vec::new(),
            blocks: vec![IrBlock { label: "entry".into(), instrs: Vec::new(), term: None }],
            frame_ty: Some(fty.clone()),
        });
        self.cur_ret = CgTy::Promise;
        let pr = self.fresh("pr");
        self.emit(format!("{} = call ptr @marina_promise_create()", pr));
        {
            let gep = self.gep_ssa(&Access::Frame(0, CgTy::Promise));
            self.emit(format!("store ptr {}, ptr {}", pr, gep));
        }
        for (i, (id, t)) in f.params.iter().enumerate() {
            let ct = cg_ty(t);
            let slot = self.slot_for(*id, ct);
            let gep = self.gep_ssa(&Access::Frame(slot, ct));
            self.emit(format!("store {} {}, ptr {}", ct.ir(), format!("%a{}", i), gep));
        }
        let rv = self.fresh("rv");
        self.emit(format!("{} = call ptr @marina_retain(ptr {})", rv, pr));
        self.emit(format!(
            "call void @{}.resume(ptr %fr, i32 0, ptr null)",
            fname
        ));
        self.term(format!("ret ptr {}", rv));

        // 4. resume function (state machine)
        self.push_fn(IrFn {
            name: format!("{}.resume", fname),
            ret: "void",
            params: vec!["ptr %fr".into(), "i32 %st".into(), "ptr %val".into()],
            allocas: Vec::new(),
            blocks: vec![IrBlock { label: "pro".into(), instrs: Vec::new(), term: None }],
            frame_ty: None,
        });
        self.cur_ret = CgTy::Void;

        // pre-create segment blocks with stable labels
        let mut vblock: Vec<Option<usize>> = Vec::new();
        let mut sblock: Vec<usize> = Vec::new();
        for k in 0..segs.len() {
            if k > 0 {
                vblock.push(Some(self.new_block_fixed(format!("seg.{}v", k))));
            } else {
                vblock.push(None);
            }
            sblock.push(self.new_block_fixed(format!("seg.{}", k)));
        }

// compile segments
        // The continuation block of segment k receives the awaited value of
        // segment k-1 (the segment that suspended before it).
        let await_targets: Vec<Option<(usize, CTy)>> = segs
            .iter()
            .map(|s| match &s.end {
                SegEnd::Await { target, .. } => target.clone(),
                _ => None,
            })
            .collect();
        for (k, seg) in segs.into_iter().enumerate() {
            if let (k_v, Some(vi)) = (k, vblock[k]) {
                let _ = k_v;
                self.switch_block(vi);
                if let Some((id, ty)) = await_targets[k - 1].clone() {
                    self.materialize_await_value(id, &ty);
                }
                let sl = self.label_of(sblock[k]);
                self.term(format!("br label %{}", sl));
            }
            self.switch_block(sblock[k]);
            self.scopes.push(HashMap::new());
            self.cg_body(&seg.stmts);
            if !self.terminated() {
                match &seg.end {
                    SegEnd::Await { expr, next, .. } => {
                        let v = self.cg_expr(expr);
                        let v = self.to_owned(v);
                        self.emit(format!(
                            "call void @marina_await(ptr {}, ptr @{}.resume, ptr %fr, i32 {})",
                            v.ir, fname, next
                        ));
                        self.flush_pending();
                        self.term("ret void".to_string());
                    }
                    SegEnd::ReturnExpr(e) => {
                        let inner = match &f.ret {
                            CTy::Promise(t) => (**t).clone(),
                            _ => CTy::Void,
                        };
                        let v = self.cg_expr(e);
                        let v = self.coerce_val(v, cg_ty(&inner));
                        self.finish_async(Some(v));
                    }
                    SegEnd::ReturnVoid => self.finish_async(None),
                    SegEnd::ResolveReturn { id, ty } => {
                        let slot = self.slot_for(*id, cg_ty(ty));
                        let (v, _) = self.load_access(&Access::Frame(slot, cg_ty(ty)));
                        self.finish_async(Some(CgVal { ty: cg_ty(ty), ir: v, owned: false }));
                    }
                }
            }
            self.pop_scope();
        }

        // 5. switch on state in the prologue block
        let mut sw = format!("switch i32 %st, label %{} [", self.label_of(sblock[0]));
        for k in 1..sblock.len() {
            sw.push_str(&format!(
                " i32 {}, label %{}",
                k,
                self.label_of(vblock[k].unwrap())
            ));
        }
        sw.push_str(" ]");
        self.switch_block(0);
        self.term(sw);

        // 6. emit the frame type def (slot count now final)
        let slots = self.frame.as_ref().unwrap().slots.clone();
        let tys: Vec<String> = slots.iter().map(|t| t.mem().to_string()).collect();
        self.type_defs
            .push(format!("{} = type {{ {} }}", fty, tys.join(", ")));

        self.frame = None;
    }

    /// Materialize the awaited value (in `%val`) into `id`'s frame slot.
    fn materialize_await_value(&mut self, id: usize, ty: &CTy) {
        let ct = cg_ty(ty);
        let slot = self.slot_for(id, ct);
        let gep = self.gep_ssa(&Access::Frame(slot, ct));
        let vgep2 = self.mv_field("%val", 2);
        let vgep3 = self.mv_field("%val", 3);
        let vgep4 = self.mv_field("%val", 4);
        match ct {
            CgTy::Int => {
                let l = self.fresh("aw");
                self.emit(format!("{} = load i64, ptr {}", l, vgep2));
                let t = self.fresh("at");
                self.emit(format!("{} = trunc i64 {} to i32", t, l));
                self.emit(format!("store i32 {}, ptr {}", t, gep));
            }
            CgTy::Float => {
                let l = self.fresh("aw");
                self.emit(format!("{} = load double, ptr {}", l, vgep3));
                self.emit(format!("store double {}, ptr {}", l, gep));
            }
            CgTy::Str => {
                let l = self.fresh("aw");
                self.emit(format!("{} = load ptr, ptr {}", l, vgep4));
                let r = self.fresh("ar");
                self.emit(format!("{} = call ptr @marina_retain(ptr {})", r, l));
                self.emit(format!("store ptr {}, ptr {}", r, gep));
            }
            CgTy::Bool => {
                let l = self.fresh("aw");
                self.emit(format!("{} = load i64, ptr {}", l, vgep2));
                let t = self.fresh("at");
                self.emit(format!("{} = trunc i64 {} to i1", t, l));
                let z = self.fresh("az");
                self.emit(format!("{} = zext i1 {} to i8", z, t));
                self.emit(format!("store i8 {}, ptr {}", z, gep));
            }
            // Reference values: the waiter task hands us a retained copy
            // (marina_promise_resolve retains per waiter), so retain again
            // for the frame slot — the slot releases it when the frame dies
            // and run_waiter drops the task's copy. Stream values are plain
            // integer handles (no payload to move).
            CgTy::Stream => {
                let l = self.fresh("aw");
                self.emit(format!("{} = load i64, ptr {}", l, vgep2));
                self.emit(format!("store i64 {}, ptr {}", l, gep));
            }
            CgTy::Array | CgTy::Object | CgTy::Promise | CgTy::Resolver => {
                let l = self.fresh("aw");
                self.emit(format!("{} = load ptr, ptr {}", l, vgep4));
                let r = self.fresh("ar");
                self.emit(format!("{} = call ptr @marina_retain(ptr {})", r, l));
                self.emit(format!("store ptr {}, ptr {}", r, gep));
            }
            _ => {}
        }
    }

    // ------------------------------------------------------------------
    // Program entry point
    // ------------------------------------------------------------------

    fn gen_main(&mut self, c: &Checked) {
        self.push_fn(IrFn {
            name: "main".into(),
            ret: "i32",
            params: vec![],
            allocas: Vec::new(),
            blocks: vec![IrBlock { label: "entry".into(), instrs: Vec::new(), term: None }],
            frame_ty: None,
        });
        self.emit("call void @marina_event_loop_start()".to_string());
        // register class metadata (name + field names) so the runtime can
        // print objects and keep classes self-describing
        for (id, def) in c.classes.iter().enumerate() {
            let mut bytes = Vec::new();
            bytes.extend_from_slice(def.name.as_bytes());
            bytes.push(0);
            for (f, _) in &def.fields {
                bytes.extend_from_slice(f.as_bytes());
                bytes.push(0);
            }
            let l = bytes.len();
            let gname = format!("@.classmeta.{}", id);
            self.globals.push(format!(
                "{} = private constant [{} x i8] c\"{}\", align 8",
                gname,
                l,
                escape_ir_string(&bytes)
            ));
            self.emit(format!(
                "call void @marina_class_register(i32 {}, ptr getelementptr inbounds ([{} x i8], ptr {}, i64 0, i64 0), i64 {})",
                id, l, gname, l
            ));
        }
        if c.toplevel.is_some() {
            self.emit("call void @fn.__toplevel()".to_string());
        }
        if c.has_sync_main && !c.explicit_main {
            let rty = self
                .fn_sigs
                .get("main")
                .map(|s| s.ret)
                .unwrap_or(CgTy::Void);
            self.emit(format!("call {} @fn.main()", rty.ir()));
        }
        if c.has_async_main && !c.explicit_main {
            let mp = self.fresh("mp");
            self.emit(format!("{} = call ptr @fn.main()", mp));
            self.emit(format!("call void @marina_release(ptr {})", mp));
        }
        let r = self.fresh("mr");
        self.emit(format!("{} = call i32 @marina_event_loop_run()", r));
        self.term(format!("ret i32 {}", r));
    }

    // ------------------------------------------------------------------
    // Module rendering
    // ------------------------------------------------------------------

    fn render(self) -> String {
        let mut out = String::new();
        out.push_str("; ModuleID = 'marina'\n");
        out.push_str("; generated by the marina compiler v0.1\n\n");
        out.push_str("%mv = type { i32, i32, i64, double, ptr }\n\n");
        for t in &self.type_defs {
            out.push_str(t);
            out.push('\n');
        }
        if !self.type_defs.is_empty() {
            out.push('\n');
        }
        for g in &self.globals {
            out.push_str(g);
            out.push('\n');
        }
        if !self.globals.is_empty() {
            out.push('\n');
        }
        out.push_str(DECLARES);
        out.push_str("\n\n");
        for f in &self.fns {
            out.push_str(&render_fn(f));
        }
        out
    }
}

fn render_fn(f: &IrFn) -> String {
    let mut s = format!("define {} @{}({}) {{\n", f.ret, f.name, f.params.join(", "));
    let b0 = &f.blocks[0];
    s.push_str(&format!("{}:\n", b0.label));
    if let Some(ty) = &f.frame_ty {
        s.push_str(&format!(
            "  %fr = call ptr @marina_frame_alloc(i64 ptrtoint (ptr getelementptr inbounds ({}, ptr null, i64 1) to i64))\n",
            ty
        ));
    }
    for a in &f.allocas {
        s.push_str(&format!("  {}\n", a));
    }
    for i in &b0.instrs {
        s.push_str(&format!("  {}\n", i));
    }
    match &b0.term {
        Some(t) => s.push_str(&format!("  {}\n", t)),
        None => panic!("block {} in {} is not terminated", b0.label, f.name),
    }
    for b in &f.blocks[1..] {
        s.push_str(&format!("\n{}:\n", b.label));
        for i in &b.instrs {
            s.push_str(&format!("  {}\n", i));
        }
        match &b.term {
            Some(t) => s.push_str(&format!("  {}\n", t)),
            None => panic!("block {} in {} is not terminated", b.label, f.name),
        }
    }
    s.push_str("}\n\n");
    s
}