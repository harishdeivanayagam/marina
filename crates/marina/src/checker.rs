//! Strict type checker for marina.
//!
//! Rules (v0.1):
//!   * Function parameters REQUIRE type annotations (strict mode).
//!   * No `any`, no implicit truthiness, no string arithmetic.
//!   * Int32 -> Float64 widening is the only implicit conversion
//!     (so `10 + 20.5` is `Float64`, matching the examples).
//!   * `await` is allowed only at statement level:
//!       `let x = await e`, `await e`, or `return await e`.
//!   * Lambdas are only allowed as arguments to `setTimeout` / `new Promise`.

use crate::ast::*;
use crate::lexer::Span;

#[derive(Clone, Debug, PartialEq)]
pub enum CTy {
    Void,
    Int,
    Float,
    Bool,
    Str,
    Promise(Box<CTy>),
    Resolver(Box<CTy>),
    Array(Box<CTy>),
    /// Instance of a class (declared or ghost-inferred); id indexes
    /// `Checked::classes`.
    Object(ObjectTy),
    /// An open TCP stream handle from the `net` std module.
    Stream,
}

/// Identity + display name of a class type. Equality is by id: every ghost
/// class is distinct, even if two shapes happen to be identical.
#[derive(Clone, Debug, PartialEq)]
pub struct ObjectTy {
    pub id: usize,
    pub name: String,
}

/// A class (declared or ghost). Fields are ordered; codegen addresses them
/// by index and registers `name\0field1\0field2\0` metadata with the runtime.
#[derive(Clone, Debug)]
pub struct ClassDef {
    pub name: String,
    pub fields: Vec<(String, CTy)>,
    pub is_ghost: bool,
}

impl CTy {
    pub fn name(&self) -> String {
        match self {
            CTy::Void => "void".into(),
            CTy::Int => "Int32".into(),
            CTy::Float => "Float64".into(),
            CTy::Bool => "bool".into(),
            CTy::Str => "string".into(),
            CTy::Promise(t) => format!("Promise<{}>", t.name()),
            CTy::Resolver(t) => format!("Resolver<{}>", t.name()),
            CTy::Array(t) => format!("Array<{}>", t.name()),
            CTy::Object(o) => o.name.clone(),
            CTy::Stream => "Stream".into(),
        }
    }
}

pub struct Diag {
    pub msg: String,
    pub span: Span,
}

#[derive(Clone, Copy, PartialEq)]
pub enum BinOpC {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Lt,
    Gt,
    Le,
    Ge,
    Eq,
    Neq,
    And,
    Or,
}

#[derive(Clone, Copy, PartialEq)]
pub enum UnOpC {
    Neg,
    Not,
}

#[derive(Clone)]
pub enum TPartC {
    Str(String),
    Expr(Box<CExpr>),
}

#[derive(Clone)]
pub enum CStmt {
    Let {
        id: usize,
        ty: CTy,
        init: CExpr,
    },
    Assign {
        id: usize,
        value: CExpr,
    },
    If {
        cond: CExpr,
        then_body: Vec<CStmt>,
        else_body: Option<Vec<CStmt>>,
    },
    While {
        cond: CExpr,
        body: Vec<CStmt>,
    },
    /// `list[idx] = value`
    IndexSet {
        id: usize,
        idx: CExpr,
        value: CExpr,
        elem: CTy,
    },
    /// `obj.field = value`
    FieldSet {
        id: usize,
        idx: usize,
        value: CExpr,
        elem: CTy,
    },
    For {
        init: Option<Box<CStmt>>,
        cond: CExpr,
        update: Option<Box<CStmt>>,
        body: Vec<CStmt>,
    },
    /// kind: Plain, or FlattenPromise(inner) when an async function returns a
    /// promise that must be flattened (JS semantics).
    Return(Option<CExpr>, ReturnKind),
    Expr(CExpr),
}

#[derive(Clone, Debug)]
pub enum ReturnKind {
    Plain,
    FlattenPromise(CTy),
}

#[derive(Clone)]
pub enum CExpr {
    Int(i32),
    Float(f64),
    Bool(bool),
    Str(String),
    Template(Vec<TPartC>),
    /// Variable reference with its type (borrowed read).
    Ident(usize, CTy),
    Unary(UnOpC, Box<CExpr>, CTy),
    Binary(BinOpC, Box<CExpr>, Box<CExpr>, CTy),
    CallFn {
        name: String,
        args: Vec<CExpr>,
        ret: CTy,
        is_async: bool,
    },
    CallResolver {
        id: usize,
        arg: Box<CExpr>,
        /// The resolver's value type T (for Int -> Float widening).
        inner: CTy,
    },
    /// Await a promise; carries the resolved value type.
    Await(Box<CExpr>, CTy),
    NewPromise {
        lambda: usize,
    },
    ConsoleLog(Box<CExpr>),
    SetTimeout {
        lambda: usize,
        ms: Box<CExpr>,
    },
    /// `[e1, e2, ...]` with the inferred element type.
    ArrayLit {
        elems: Vec<CExpr>,
        elem_ty: CTy,
    },
    /// `obj[idx]` read; ty is the element type.
    Index {
        obj: Box<CExpr>,
        idx: Box<CExpr>,
        ty: CTy,
    },
    /// Non-callback array operations (push/pop/shift/unshift/splice/slice/
    /// concat/join/reverse/sort/length). `ret` is the result type.
    ArrayOp {
        op: ArrayOpKind,
        arr: Box<CExpr>,
        args: Vec<CExpr>,
        ret: CTy,
    },
    /// Higher-order array operations (forEach/map/filter/reduce) with a
    /// callback lambda. `extra` carries reduce's initial value.
    ArrayHof {
        op: HofKind,
        arr: Box<CExpr>,
        cb: Box<CExpr>,
        extra: Option<Box<CExpr>>,
        ret: CTy,
    },
    /// A general (callback) lambda value captured at the call site. The
    /// lambda's params/abi live on the `CLambda` it refers to; here we only
    /// carry the index and result type.
    LambdaVal {
        li: usize,
        ret: CTy,
    },
    /// `str.length`
    StrLen(Box<CExpr>),
    /// `{ field: expr, ... }` — `class_id` indexes `Checked::classes`;
    /// `inits` are (field index, checked value) pairs in class-field order.
    ObjectLit {
        class_id: usize,
        inits: Vec<(usize, CExpr)>,
    },
    /// `new ClassName()` — fresh instance, fields default to `undefined`.
    NewClass {
        class_id: usize,
    },
    /// `obj.field` read; ty is the field type.
    FieldGet {
        obj: Box<CExpr>,
        idx: usize,
        ty: CTy,
    },
    // ---- std modules (net / http) ----
    /// `net.connect(host, port)` — opens a TCP stream.
    NetConnect {
        host: Box<CExpr>,
        port: Box<CExpr>,
    },
    /// `stream.write(data)` — send bytes over a TCP stream.
    NetWrite {
        stream: Box<CExpr>,
        data: Box<CExpr>,
    },
    /// `stream.read_all()` — read until EOF into a string.
    NetReadAll {
        stream: Box<CExpr>,
    },
    /// `stream.close()`
    NetClose {
        stream: Box<CExpr>,
    },
    /// `http.get(url)` / `http.post(url, body)` — HTTP client request that
    /// returns an `http_Response` object `{ status, body }`.
    HttpCall {
        method: String,
        url: Box<CExpr>,
        body: Option<Box<CExpr>>,
    },
    /// `http.listen(port, handler)` — start the HTTP server (never returns).
    HttpListen {
        port: Box<CExpr>,
        lambda: usize,
    },
    /// `res.send(value)` / `res.json(value)` — respond on an HTTP server
    /// connection.
    ResSend {
        res: Box<CExpr>,
        value: Box<CExpr>,
        json: bool,
    },
    /// `new http.Server()` — a fresh server object (routes register via
    /// `server.router(...)`).
    HttpNewServer,
    /// `server.router({ path: "/x" })` — a router scoped to a path prefix.
    HttpRouter {
        server: Box<CExpr>,
        path: Box<CExpr>,
    },
    /// `router.get/post(path, handler)` — register a route handler
    /// (Listen-ABI lambda).
    HttpRouteAdd {
        router: Box<CExpr>,
        method: String,
        path: Box<CExpr>,
        lambda: usize,
    },
    /// `await server.listen(host, port)` — start the accept loop; resolves
    /// its promise once the listener is bound.
    HttpServerListen {
        host: Box<CExpr>,
        port: Box<CExpr>,
    },
    /// `new http.JsonValidator({ field: <field validator>, ... })`.
    HttpValidatorNew {
        schema: Box<CExpr>,
        fields: Vec<(String, FieldKind)>,
    },
    /// `http.JsonValidator.string()` / `.number()` (static) — a fresh
    /// chainable field validator.
    HttpFieldNew {
        kind: FieldKind,
    },
    /// `.required()` / `.min(n)` / `.max(n)` — returns the same validator.
    HttpFieldOp {
        v: Box<CExpr>,
        op: FieldOp,
        arg: Option<Box<CExpr>>,
    },
    /// `validator.validate(body)` — parse + check a JSON body; returns an
    /// object of the synthesized ghost class `out_class`.
    HttpValidate {
        v: Box<CExpr>,
        body: Box<CExpr>,
        out_class: usize,
    },
}

/// What a JsonValidator field checks: string or number.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum FieldKind {
    Str,
    Int,
}

/// Chainable JsonValidator operations.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum FieldOp {
    Required,
    Min,
    Max,
}

#[derive(Clone, Copy, PartialEq)]
pub enum ArrayOpKind {
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
}

#[derive(Clone, Copy, PartialEq)]
pub enum HofKind {
    ForEach,
    Map,
    Filter,
    Reduce,
}

/// How the generated lambda is called by the runtime.
#[derive(Clone, Copy, PartialEq)]
pub enum LambdaAbi {
    /// `(env)` — promise executors and setTimeout callbacks.
    Plain,
    /// `(env, item)` — forEach.
    ForEach,
    /// `(env, item, out)` — map / filter.
    MapFilter,
    /// `(env, acc, item, out)` — reduce.
    Reduce,
    /// `(env, req, res)` — http.listen handlers.
    Listen,
}

pub struct CFn {
    pub name: String,
    pub params: Vec<(usize, CTy)>,
    pub ret: CTy,
    pub is_async: bool,
    pub body: Vec<CStmt>,
}

pub struct CLambda {
    /// Executor lambdas may have a `resolve` (and optional `reject`) param.
    pub params: Vec<(usize, CTy)>,
    /// Captured variables (by value), in capture order.
    pub captures: Vec<(usize, CTy)>,
    pub body: Vec<CStmt>,
    /// Inferred return type (callbacks).
    pub ret: CTy,
    /// How the runtime calls this lambda.
    pub abi: LambdaAbi,
    /// True when this lambda is a promise executor (its param 0 is the
    /// promise handle). Kept for diagnostics/future lowering work.
    #[allow(dead_code)]
    pub is_executor: bool,
}

pub struct Checked {
    pub fns: Vec<CFn>,
    pub lambdas: Vec<CLambda>,
    pub toplevel: Option<Vec<CStmt>>,
    /// Types of all checked variables, indexed by variable id.
    #[allow(dead_code)]
    pub var_types: Vec<CTy>,
    /// Names of all checked variables, parallel to `var_types`
    /// (consumed by the language server for hover).
    #[allow(dead_code)]
    pub var_names: Vec<String>,
    /// All classes: declared ones first (in declaration order), then ghost
    /// classes in the order their object literals were checked.
    pub classes: Vec<ClassDef>,
    pub has_async_main: bool,
    pub has_sync_main: bool,
}

#[derive(Clone)]
struct VarEntry {
    id: usize,
    is_const: bool,
}

struct FnSig {
    params: Vec<CTy>,
    ret: Option<CTy>,
    is_async: bool,
}

struct Checker {
    sigs: std::collections::HashMap<String, FnSig>,
    var_types: Vec<CTy>,
    next_id: usize,
    diags: Vec<Diag>,
    lambdas: Vec<CLambda>,
    scopes: Vec<std::collections::HashMap<String, VarEntry>>,
    // current function context
    is_async: bool,
    ret_ty: Option<CTy>,
    saw_float_ret: bool,
    // inferred return type of the most recently checked function
    last_fn_ret: Option<CTy>,
    // current promise-executor inference
    promise_infer: Option<(CTy, bool)>, // (T, saw_float)
    // are we currently checking top-level statements (not a function body)?
    in_toplevel: bool,
    // nesting depth of if/while bodies (v0.1 restricts await + promise
    // returns to the top level of a function)
    block_depth: usize,
    // lambda capture machinery
    lambda_base: Vec<usize>,       // scope index where each lambda's locals start
    lambda_idx: Vec<usize>,        // index into self.lambdas for capture recording
    cur_captures: Vec<Vec<(usize, CTy)>>,
    // whether `return <value>` is allowed in the current lambda (callbacks)
    lambda_ret_ok: Vec<bool>,
    reject_ids: Vec<usize>,
    // classes: declared (pass 0) + ghost (created while checking literals)
    classes: Vec<ClassDef>,
    class_by_name: std::collections::HashMap<String, usize>,
    ghost_count: usize,
    // names parallel to var_types (set at declaration, empty for internals)
    var_names: Vec<String>,
    // modules introduced by top-level `use` statements (std or wave packages)
    uses: Vec<String>,
    // `let` variables holding `new http.JsonValidator({...})`: var id ->
    // ordered (field, kind) schema, used by `.validate()` to synthesize the
    // ghost class of the validated result
    validator_fields: std::collections::HashMap<usize, Vec<(String, FieldKind)>>,
    // ids of top-level variables (globals: lambdas may assign to them)
    toplevel_var_ids: std::collections::HashSet<usize>,
}

pub fn check(program: &Program) -> Result<Checked, Vec<Diag>> {
    let (checked, diags) = check_with_diags(program);
    if diags.is_empty() {
        Ok(checked)
    } else {
        Err(diags)
    }
}

/// Like `check`, but always returns the (possibly incomplete) `Checked`
/// alongside diagnostics. Used by the language server, which still wants
/// class/variable info from programs that contain errors.
pub fn check_with_diags(program: &Program) -> (Checked, Vec<Diag>) {
    let mut c = Checker {
        sigs: Default::default(),
        var_types: Vec::new(),
        next_id: 0,
        diags: Vec::new(),
        lambdas: Vec::new(),
        scopes: Vec::new(),
        is_async: false,
        ret_ty: None,
        saw_float_ret: false,
        last_fn_ret: None,
        promise_infer: None,
        in_toplevel: false,
        block_depth: 0,
        lambda_base: Vec::new(),
        lambda_idx: Vec::new(),
        cur_captures: Vec::new(),
        lambda_ret_ok: Vec::new(),
        reject_ids: Vec::new(),
        classes: Vec::new(),
        class_by_name: Default::default(),
        ghost_count: 0,
        var_names: Vec::new(),
        uses: program.uses.iter().map(|(n, _)| n.clone()).collect(),
        validator_fields: Default::default(),
        toplevel_var_ids: Default::default(),
    };

    // --- pass 0: register classes (hoisting) ---
    c.register_classes(program);
    c.register_builtin_modules();

    // --- pass 0.5: unknown `use` targets (registry packages were already
    // merged into the program by the pipeline; net/http are built-in) ---
    // (missing/non-std modules are diagnosed by the pipeline's resolve_uses,
    // which also merges registry packages into the program)

    // --- pass 1: register function signatures (hoisting) ---
    let mut seen = std::collections::HashSet::new();
    for f in &program.fns {
        if !is_camel_case(&f.name) && f.name != "main" && !is_namespaced_fn(&f.name) {
            c.diag(
                format!("function '{}' must be camelCase", f.name),
                f.span,
            );
        }
        if !seen.insert(f.name.clone()) {
            c.diag(format!("duplicate function '{}'", f.name), f.span);
            continue;
        }
        let mut params = Vec::new();
        for p in &f.params {
            match &p.ann {
                Some(ann) => params.push(c.ty_of(ann)),
                None => {
                    c.diag(
                        format!(
                            "parameter '{}' of function '{}' needs a type annotation (strict mode)",
                            p.name, f.name
                        ),
                        p.span,
                    );
                    params.push(CTy::Void);
                }
            }
        }
        let ret = f.ret_ann.as_ref().map(|a| c.ty_of(a));
        if let Some(CTy::Resolver(_)) = &ret {
            c.diag("functions cannot return a Resolver", f.span);
        }
        c.sigs.insert(
            f.name.clone(),
            FnSig { params, ret, is_async: f.is_async },
        );
    }

    // --- pass 2: top-level *declarations* (first, so functions can reference
    // top-level variables; codegen stores them as module globals). Other
    // top-level statements are deferred until after fn bodies, because they
    // may call functions whose (inferred) return type is only known then. ---
    let mut toplevel: Option<Vec<CStmt>> = None;
    let mut toplevel_slots: Option<Vec<Option<CStmt>>> = None;
    if !program.stmts.is_empty() {
        c.is_async = false;
        c.in_toplevel = true;
        c.ret_ty = None;
        c.saw_float_ret = false;
        c.scopes.push(Default::default());
        let mut slots: Vec<Option<CStmt>> = Vec::with_capacity(program.stmts.len());
        for s in &program.stmts {
            if matches!(s, Stmt::Let { .. })
                && !c.stmt_calls_unresolved(s)
            {
                slots.push(Some(c.check_stmt(s)));
            } else {
                slots.push(None);
            }
        }
        toplevel_slots = Some(slots);
        c.in_toplevel = false;
        // the scope stays on the stack until fn bodies are checked (pass 3)
    }

    // --- pass 3: check fn bodies (top-level variables are in scope) ---
    let mut fns = Vec::new();
    let mut has_async_main = false;
    let mut has_sync_main = false;
    for f in &program.fns {
        if f.name == "main" {
            if f.is_async {
                if has_sync_main {
                    c.diag("cannot define both an async and a sync 'main'", f.span);
                }
                has_async_main = true;
            } else {
                if has_async_main {
                    c.diag("cannot define both an async and a sync 'main'", f.span);
                }
                has_sync_main = true;
            }
        }
        let cf = c.check_fn(f);
        // publish the (possibly inferred) return type for later functions
        if let Some(r) = c.last_fn_ret.clone() {
            if let Some(sig) = c.sigs.get_mut(&f.name) {
                sig.ret = Some(r);
            }
        }
        fns.push(cf);
    }
    if toplevel_slots.is_some() {
        // --- pass 4: remaining top-level statements (all fn return types are
        // known now) ---
        c.is_async = false;
        c.in_toplevel = true;
        c.ret_ty = None;
        c.saw_float_ret = false;
        let mut slots = toplevel_slots.take().unwrap_or_default();
        for (i, s) in program.stmts.iter().enumerate() {
            if slots[i].is_none() {
                slots[i] = Some(c.check_stmt(s));
            }
        }
        toplevel = Some(slots.into_iter().flatten().collect());
        c.in_toplevel = false;
        c.scopes.pop();
    }

    let checked = Checked {
        fns,
        lambdas: c.lambdas,
        toplevel,
        var_types: c.var_types,
        var_names: c.var_names,
        classes: c.classes,
        has_async_main,
        has_sync_main,
    };
    (checked, c.diags)
}

impl Checker {
    /// Built-in std classes, registered when the module is `use`d.
    /// These share the class table with user classes (ids from pass 0).
    fn register_builtin_modules(&mut self) {
        if !self.uses.iter().any(|m| m == "http") {
            return;
        }
        let builtins = [
            (
                "http_Request",
                vec![("method", CTy::Str), ("path", CTy::Str), ("body", CTy::Str)],
            ),
            ("http_Response", vec![("status", CTy::Int), ("body", CTy::Str)]),
            ("http_Res", vec![]),
            ("http_Server", vec![]),
            ("http_Router", vec![]),
            ("http_JsonValidator", vec![]),
            ("http_FieldValidator", vec![]),
        ];
        for (name, fields) in builtins {
            if self.class_by_name.contains_key(name) {
                continue;
            }
            let id = self.classes.len();
            self.classes.push(ClassDef {
                name: name.to_string(),
                fields: fields.into_iter().map(|(n, t)| (n.to_string(), t)).collect(),
                is_ghost: false,
            });
            self.class_by_name.insert(name.to_string(), id);
        }
        // dotted aliases for user-facing type annotations
        let aliases = [
            ("http.Request", "http_Request"),
            ("http.Response", "http_Res"), // server-side response object
        ];
        for (alias, target) in aliases {
            if let Some(&id) = self.class_by_name.get(target) {
                self.class_by_name.entry(alias.to_string()).or_insert(id);
            }
        }
    }

    /// pass 0: hoist all declared classes into the class table so types can
    /// reference classes defined anywhere in the file
    fn register_classes(&mut self, program: &Program) {
        for cl in &program.classes {
            if !is_pascal_case(&cl.name) {
                self.diag(format!("class '{}' must be PascalCase", cl.name), cl.span);
            }
            if self.class_by_name.contains_key(&cl.name) {
                self.diag(format!("duplicate class '{}'", cl.name), cl.span);
                continue;
            }
            let mut fields: Vec<(String, CTy)> = Vec::new();
            let mut seen_fields = std::collections::HashSet::new();
            for (fname, ann) in &cl.fields {
                if !seen_fields.insert(fname.clone()) {
                    self.diag(
                        format!("duplicate field '{}' in class '{}'", fname, cl.name),
                        cl.span,
                    );
                    continue;
                }
                let t = self.ty_of(ann);
                fields.push((fname.clone(), t));
            }
            let id = self.classes.len();
            self.classes
                .push(ClassDef { name: cl.name.clone(), fields, is_ghost: false });
            self.class_by_name.insert(cl.name.clone(), id);
        }
    }

    fn diag(&mut self, msg: impl Into<String>, span: Span) {
        self.diags.push(Diag { msg: msg.into(), span });
    }

    fn fresh_id(&mut self) -> usize {
        let id = self.next_id;
        self.next_id += 1;
        self.var_types.push(CTy::Void);
        self.var_names.push(String::new());
        id
    }

    // ------------------------------------------------------------------
    // Types
    // ------------------------------------------------------------------

    fn ty_of(&mut self, ann: &TyAnn) -> CTy {
        let t = match (ann.name.as_str(), &ann.inner) {
            ("int", _) | ("Int32", _) | ("i32", _) => CTy::Int,
            ("float", _) | ("Float64", _) | ("f64", _) | ("double", _) | ("number", _) => CTy::Float,
            ("string", _) | ("String", _) => CTy::Str,
            ("bool", _) | ("boolean", _) | ("Boolean", _) => CTy::Bool,
            ("void", _) | ("undefined", _) => CTy::Void,
            ("Stream", None) | ("stream", None) => CTy::Stream,
            ("Promise", Some(inner)) => CTy::Promise(Box::new(self.ty_of(inner))),
            ("Promise", None) => {
                self.diag("Promise requires a type parameter, e.g. Promise<string>", ann.span);
                CTy::Promise(Box::new(CTy::Void))
            }
            ("Array", Some(inner)) => CTy::Array(Box::new(self.ty_of(inner))),
            ("Array", None) => {
                self.diag("Array requires a type parameter, e.g. Array<Int32>", ann.span);
                CTy::Array(Box::new(CTy::Int))
            }
            (name, None) if self.class_by_name.contains_key(name) => {
                let id = self.class_by_name[name];
                // canonical name (covers dotted aliases like http.Request)
                let display = self.classes[id].name.clone();
                CTy::Object(ObjectTy { id, name: display })
            }
            (other, _) => {
                self.diag(format!("unknown type '{}'", other), ann.span);
                CTy::Void
            }
        };
        if !matches!(ann.name.as_str(), "Promise" | "Array") {
            if ann.inner.is_some() {
                self.diag(format!("type '{}' does not take type parameters", ann.name), ann.span);
            }
        }
        t
    }

    /// The only implicit conversion: Int32 widens to Float64.
    fn coerce(&mut self, from: &CTy, to: &CTy, span: Span) -> bool {
        if from == to {
            return true;
        }
        if matches!(from, CTy::Int) && matches!(to, CTy::Float) {
            return true;
        }
        self.diag(
            format!("type mismatch: expected {}, found {}", to.name(), from.name()),
            span,
        );
        false
    }

    fn is_numeric(t: &CTy) -> bool {
        matches!(t, CTy::Int | CTy::Float)
    }

    // ------------------------------------------------------------------
    // Scopes
    // ------------------------------------------------------------------

    fn declare(&mut self, name: &str, ty: CTy, is_const: bool, span: Span) -> usize {
        let exists = self.scopes.last().unwrap().contains_key(name);
        if exists {
            self.diag(format!("cannot redeclare '{}' in the same scope", name), span);
        }
        let id = self.fresh_id();
        self.var_types[id] = ty;
        self.var_names[id] = name.to_string();
        self.scopes
            .last_mut()
            .unwrap()
            .insert(name.to_string(), VarEntry { id, is_const });
        id
    }

    /// Resolve an identifier. Returns Some((id, ty, is_capture)).
    fn lookup(&mut self, name: &str, span: Span) -> Option<(usize, CTy, bool)> {
        let len = self.scopes.len();
        for i in (0..len).rev() {
            if let Some(e) = self.scopes[i].get(name) {
                let (id, ty) = (e.id, self.var_types[e.id].clone());
                // is this inside the innermost lambda's own scope?
                let is_capture = match self.lambda_base.last() {
                    Some(&base) => i < base,
                    None => false,
                };
                if is_capture {
                    if let Some(caps) = self.cur_captures.last_mut() {
                        if !caps.iter().any(|(cid, _)| *cid == id) {
                            caps.push((id, ty.clone()));
                        }
                    }
                }
                return Some((id, ty, is_capture));
            }
        }
        self.diag(format!("cannot find name '{}'", name), span);
        None
    }

    // ------------------------------------------------------------------
    // Functions
    // ------------------------------------------------------------------

    fn check_fn(&mut self, f: &FnDecl) -> CFn {
        self.is_async = f.is_async;
        self.in_toplevel = false;
        self.ret_ty = None;
        self.saw_float_ret = false;
        self.scopes.push(Default::default());
        let mut params = Vec::new();
        for p in &f.params {
            let ty = match &p.ann {
                Some(ann) => self.ty_of(ann),
                None => CTy::Void, // diag already emitted in pass 1
            };
            let id = self.declare(&p.name, ty.clone(), true, p.span);
            params.push((id, ty));
        }
        let body = self.check_stmts(&f.body);

        // finalize inferred return type
        let mut ret = self.ret_ty.clone().unwrap_or(CTy::Void);
        if self.saw_float_ret && ret == CTy::Int {
            ret = CTy::Float;
        }
        if f.is_async {
            ret = CTy::Promise(Box::new(ret));
        }
        if let Some(ann) = &f.ret_ann {
            let at = self.ty_of(ann);
            self.coerce(&ret, &at, f.span);
            ret = at;
        }
        // For async fns the source-level requirement is about the awaited
        // value, not the returned promise: Promise<void> may fall off the end.
        let requires_ret = match &ret {
            CTy::Promise(inner) => **inner != CTy::Void,
            other => *other != CTy::Void,
        };
        if requires_ret {
            // If control can fall off the end and the function returns a
            // value, require a return at the top level of the body.
            let has_return = body
                .iter()
                .any(|s| matches!(s, CStmt::Return(..)));
            if !has_return {
                self.diag(
                    format!(
                        "function '{}' returns {} but not all paths return a value",
                        f.name,
                        ret.name()
                    ),
                    f.span,
                );
            }
        }
        // record the inferred return type so later-declared functions can
        // call this one without a return-type annotation
        self.last_fn_ret = Some(ret.clone());
        self.scopes.pop();
        CFn { name: f.name.clone(), params, ret, is_async: f.is_async, body }
    }

    // ------------------------------------------------------------------
    // Statements
    // ------------------------------------------------------------------

    fn check_stmts(&mut self, stmts: &[Stmt]) -> Vec<CStmt> {
        stmts.iter().map(|s| self.check_stmt(s)).collect()
    }

    /// True if the statement's expressions call a function whose (inferred)
    /// return type is not yet known. Used to defer such top-level statements
    /// until all function bodies have been checked.
    fn stmt_calls_unresolved(&self, s: &Stmt) -> bool {
        match s {
            Stmt::Let { init, .. } => self.expr_calls_unresolved(init),
            _ => false,
        }
    }

    fn expr_calls_unresolved(&self, e: &Expr) -> bool {
        match e {
            Expr::Call(callee, args, _) => {
                let direct = match callee.as_ref() {
                    Expr::Ident(n, _) => self
                        .sigs
                        .get(n)
                        .map(|s| s.ret.is_none() && s.is_async)
                        .unwrap_or(false),
                    _ => false,
                };
                direct
                    || self.expr_calls_unresolved(callee)
                    || args.iter().any(|a| self.expr_calls_unresolved(a))
                }
            Expr::Await(inner, _)
            | Expr::NewPromise(inner, _)
            | Expr::Unary(_, inner, _)
            | Expr::Member(inner, _, _) => self.expr_calls_unresolved(inner),
            Expr::Binary(_, l, r, _) => {
                self.expr_calls_unresolved(l) || self.expr_calls_unresolved(r)
            }
            Expr::Index(o, i, _) => {
                self.expr_calls_unresolved(o) || self.expr_calls_unresolved(i)
            }
            Expr::ArrayLit(es, _) => es.iter().any(|e| self.expr_calls_unresolved(e)),
            Expr::ObjectLit(fs, _) => fs.iter().any(|(_, e)| self.expr_calls_unresolved(e)),
            Expr::NewCtor { args, .. } => args.iter().any(|a| self.expr_calls_unresolved(a)),
            Expr::Template(parts, _) => parts
                .iter()
                .any(|p| match p {
                    TPart::Expr(e) => self.expr_calls_unresolved(e),
                    TPart::Str(_) => false,
                }),
            // lambda bodies are checked later on their own
            Expr::Lambda(..) | Expr::Ident(..) | Expr::Str(..) | Expr::Int(..)
            | Expr::Float(..) | Expr::Bool(..) | Expr::NewClass(..) => false,
        }
    }

    fn check_stmt(&mut self, s: &Stmt) -> CStmt {
        match s {
            Stmt::Let { name, ann, init, is_const, span } => {
                // naming rules: `let` is camelCase, `const` is UPPER_SNAKE_CASE
                if *is_const {
                    if !is_upper_snake_case(name) {
                        self.diag(
                            format!("const '{}' must be UPPER_SNAKE_CASE (e.g. MAX_RETRIES)", name),
                            *span,
                        );
                    }
                } else if !is_camel_case(name) {
                    self.diag(format!("let variable '{}' must be camelCase", name), *span);
                }
                // an annotated empty array literal takes its element type
                // from the annotation: `let a: Array<Int32> = []`
                let expected_elem = match (ann, init) {
                    (Some(a), Expr::ArrayLit(elems, _)) if elems.is_empty() => {
                        match self.ty_of(a) {
                            CTy::Array(elem) => Some(*elem),
                            _ => None,
                        }
                    }
                    _ => None,
                };
                // an object literal bound to a variable names its ghost
                // class after the variable: `let abc = {...}` → Class_abc_NNNN.
                // With a class annotation the literal must match that class.
                let target_class = match (ann, init) {
                    (Some(a), Expr::ObjectLit(..)) => match self.ty_of(a) {
                        CTy::Object(o) => Some(o.id),
                        other => {
                            self.diag(
                                format!("type annotation must be a class for object literals, found {}", other.name()),
                                *span,
                            );
                            None
                        }
                    },
                    _ => None,
                };
                let (init_c, init_ty) = if let Expr::Await(inner, asp) = init {
                    self.check_await_top(inner, *asp)
                } else if let Expr::ArrayLit(elems, asp) = init {
                    let e = self.check_array_lit(elems, *asp, expected_elem);
                    let t = self.expr_ty(&e);
                    (e, t)
                } else if let Expr::ObjectLit(fields, asp) = init {
                    let e = self.check_object_lit(fields, *asp, target_class, Some(name));
                    let t = self.expr_ty(&e);
                    (e, t)
                } else {
                    let e = self.check_expr(init, false);
                    let t = self.expr_ty(&e);
                    (e, t)
                };
                let ty = match ann {
                    Some(a) => {
                        let at = self.ty_of(a);
                        self.coerce(&init_ty, &at, *span);
                        at
                    }
                    None => init_ty,
                };
                let id = self.declare(name, ty.clone(), *is_const, *span);
                if self.in_toplevel {
                    // top-level variables are globals: lambdas anywhere may
                    // read them, and assign back to them
                    self.toplevel_var_ids.insert(id);
                }
                if let CExpr::HttpValidatorNew { fields, .. } = &init_c {
                    self.validator_fields.insert(id, fields.clone());
                }
                CStmt::Let { id, ty, init: init_c }
            }
            Stmt::Assign { name, value, span } => {
                let found = self.lookup(name, *span);
                let (id, ty, is_capture) = match found {
                    Some(v) => v,
                    None => return CStmt::Expr(CExpr::Bool(false)),
                };
                if is_capture && !self.toplevel_var_ids.contains(&id) {
                    self.diag(
                        "cannot assign to a captured variable (captures are by value)",
                        *span,
                    );
                    return CStmt::Expr(CExpr::Bool(false));
                }
                let is_const = {
                    let mut found_const = false;
                    for sc in self.scopes.iter().rev() {
                        if let Some(e) = sc.get(name) {
                            found_const = e.is_const;
                            break;
                        }
                    }
                    found_const
                };
                if is_const {
                    self.diag(format!("cannot assign to 'const {}'", name), *span);
                }
                let v = self.check_expr(value, false);
                self.coerce(&self.expr_ty(&v), &ty, *span);
                CStmt::Assign { id, value: v }
            }
            Stmt::IndexAssign { name, index, value, span } => {
                let found = self.lookup(name, *span);
                let (id, ty, is_capture) = match found {
                    Some(v) => v,
                    None => return CStmt::Expr(CExpr::Bool(false)),
                };
                if is_capture && !self.toplevel_var_ids.contains(&id) {
                    self.diag(
                        "cannot assign to a captured variable (captures are by value)",
                        *span,
                    );
                    return CStmt::Expr(CExpr::Bool(false));
                }
                // NOTE: `const` only freezes the binding (reassignment via
                // `arr = ...` is rejected in the Assign arm). Writing elements
                // (`arr[i] = v`, `arr.push(...)`) mutates the array object the
                // binding refers to, matching TypeScript semantics.
                match ty {
                    CTy::Array(elem) => {
                        let ic = self.check_expr(index, false);
                        if self.expr_ty(&ic) != CTy::Int {
                            self.diag(
                                format!("array index must be Int32, found {}", self.expr_ty(&ic).name()),
                                *span,
                            );
                        }
                        let vc = self.check_expr(value, false);
                        self.coerce(&self.expr_ty(&vc), &elem, *span);
                        CStmt::IndexSet { id, idx: ic, value: vc, elem: *elem.clone() }
                    }
                    other => {
                        self.diag(
                            format!("indexed assignment requires an Array, found {}", other.name()),
                            *span,
                        );
                        CStmt::Expr(CExpr::Bool(false))
                    }
                }
            }
            Stmt::MemberAssign { obj, field, value, span } => {
                let found = self.lookup(obj, *span);
                let (id, ty, is_capture) = match found {
                    Some(v) => v,
                    None => return CStmt::Expr(CExpr::Bool(false)),
                };
                if is_capture && !self.toplevel_var_ids.contains(&id) {
                    self.diag(
                        "cannot assign to a captured variable (captures are by value)",
                        *span,
                    );
                    return CStmt::Expr(CExpr::Bool(false));
                }
                // const freezes the binding, not the object contents
                match ty {
                    CTy::Object(o) => {
                        let def = &self.classes[o.id];
                        let idx = match def.fields.iter().position(|(n, _)| n == field) {
                            Some(i) => i,
                            None => {
                                let names: Vec<String> =
                                    def.fields.iter().map(|(n, _)| n.clone()).collect();
                                self.diag(
                                    format!(
                                        "class '{}' has no field '{}' (fields: {})",
                                        o.name,
                                        field,
                                        if names.is_empty() { "none".to_string() } else { names.join(", ") }
                                    ),
                                    *span,
                                );
                                return CStmt::Expr(CExpr::Bool(false));
                            }
                        };
                        let elem = def.fields[idx].1.clone();
                        let vc = self.check_expr(value, false);
                        self.coerce(&self.expr_ty(&vc), &elem, *span);
                        CStmt::FieldSet { id, idx, value: vc, elem }
                    }
                    other => {
                        self.diag(
                            format!(
                                "field assignment requires an object, found {} (hint: use `{}[i] = v` for arrays)",
                                other.name(),
                                obj
                            ),
                            *span,
                        );
                        CStmt::Expr(CExpr::Bool(false))
                    }
                }
            }
            Stmt::Return { value, span } => {
                let lambda_ret_ok = self.lambda_ret_ok.last().copied().unwrap_or(false);
                if self.lambda_idx.last().is_some() && !lambda_ret_ok {
                    self.diag("cannot return from a lambda", *span);
                    return CStmt::Expr(CExpr::Bool(false));
                }
                if self.lambda_idx.last().is_none() && self.in_toplevel {
                    self.diag("'return' is not allowed at the top level", *span);
                    return CStmt::Expr(CExpr::Bool(false));
                }
                match value {
                    None => {
                        self.unify_ret(CTy::Void, *span);
                        CStmt::Return(None, ReturnKind::Plain)
                    }
                    Some(Expr::Await(inner, asp)) => {
                        let (a, ty) = self.check_await_top(inner, *asp);
                        self.unify_ret(ty.clone(), *span);
                        CStmt::Return(Some(a), ReturnKind::Plain)
                    }
                    Some(e) => {
                        let v = self.check_expr(e, false);
                        let mut t = self.expr_ty(&v);
                        if self.is_async {
                            if let CTy::Promise(inner) = &t {
                                if self.block_depth > 0 {
                                    self.diag(
                                        "returning a promise from inside if/while is not supported in marina v0.1",
                                        *span,
                                    );
                                    return CStmt::Return(None, ReturnKind::Plain);
                                }
                                // flatten one level (JS async semantics)
                                if let CTy::Promise(_) = &**inner {
                                    self.diag(
                                        "returning a promise of a promise is not supported",
                                        *span,
                                    );
                                }
                                t = (**inner).clone();
                                self.unify_ret(t.clone(), *span);
                                return CStmt::Return(
                                    Some(v),
                                    ReturnKind::FlattenPromise(t),
                                );
                            }
                        } else if let CTy::Promise(_) = &t {
                            self.diag(
                                "sync functions cannot return a Promise (make the function async)",
                                *span,
                            );
                        }
                        self.unify_ret(t, *span);
                        CStmt::Return(Some(v), ReturnKind::Plain)
                    }
                }
            }
            Stmt::If { cond, then_body, else_body, span } => {
                let c = self.check_expr(cond, false);
                if self.expr_ty(&c) != CTy::Bool {
                    self.diag(
                        format!(
                            "if condition must be bool, found {}",
                            self.expr_ty(&c).name()
                        ),
                        *span,
                    );
                }
                self.scopes.push(Default::default());
                self.block_depth += 1;
                let t = self.check_stmts(then_body);
                self.block_depth -= 1;
                self.scopes.pop();
                let e = else_body.as_ref().map(|b| {
                    self.scopes.push(Default::default());
                    self.block_depth += 1;
                    let e = self.check_stmts(b);
                    self.block_depth -= 1;
                    self.scopes.pop();
                    e
                });
                CStmt::If { cond: c, then_body: t, else_body: e }
            }
            Stmt::While { cond, body, span } => {
                let c = self.check_expr(cond, false);
                if self.expr_ty(&c) != CTy::Bool {
                    self.diag(
                        format!(
                            "while condition must be bool, found {}",
                            self.expr_ty(&c).name()
                        ),
                        *span,
                    );
                }
                self.scopes.push(Default::default());
                self.block_depth += 1;
                let b = self.check_stmts(body);
                self.block_depth -= 1;
                self.scopes.pop();
                CStmt::While { cond: c, body: b }
            }
            Stmt::For { init, cond, update, body, span } => {
                // the init variable lives in a scope that encloses the body
                self.scopes.push(Default::default());
                let ci = match init {
                    Some(s) => Some(Box::new(self.check_stmt(s))),
                    None => None,
                };
                let cc = match cond {
                    Some(e) => {
                        if let Expr::Await(inner, asp) = e {
                            self.diag(
                                "await in a loop condition is not supported in marina v0.1",
                                *asp,
                            );
                            let _ = inner;
                            CExpr::Bool(false)
                        } else {
                            let c = self.check_expr(e, false);
                            if self.expr_ty(&c) != CTy::Bool {
                                self.diag(
                                    format!(
                                        "for condition must be bool, found {}",
                                        self.expr_ty(&c).name()
                                    ),
                                    *span,
                                );
                            }
                            c
                        }
                    }
                    None => CExpr::Bool(true),
                };
                let cu = match update {
                    Some(s) => {
                        if let Stmt::Expr(Expr::Await(_, asp)) = &**s {
                            self.diag(
                                "await in a loop update is not supported in marina v0.1",
                                *asp,
                            );
                            None
                        } else {
                            Some(Box::new(self.check_stmt(s)))
                        }
                    }
                    None => None,
                };
                self.block_depth += 1;
                let cb = self.check_stmts(body);
                self.block_depth -= 1;
                self.scopes.pop();
                CStmt::For { init: ci, cond: cc, update: cu, body: cb }
            }
            Stmt::Expr(e) => {
                if let Expr::Await(inner, asp) = e {
                    let (a, _ty) = self.check_await_top(inner, *asp);
                    CStmt::Expr(a)
                } else {
                    CStmt::Expr(self.check_expr(e, false))
                }
            }
        }
    }

    fn check_await_top(&mut self, inner: &Expr, span: Span) -> (CExpr, CTy) {
        if self.block_depth > 0 {
            self.diag(
                "await inside if/while is not supported in marina v0.1 (keep awaits at the top level of the async function)",
                span,
            );
            return (CExpr::Bool(false), CTy::Void);
        }
        let e = self.check_expr(inner, false);
        let t = self.expr_ty(&e);
        match t {
            CTy::Promise(u) => {
                let u2 = (*u).clone();
                (CExpr::Await(Box::new(e), u2.clone()), u2)
            }
            other => {
                self.diag(format!("can only await a Promise, found {}", other.name()), span);
                (CExpr::Await(Box::new(e), CTy::Void), CTy::Void)
            }
        }
    }

    fn unify_ret(&mut self, t: CTy, span: Span) {
        if let Some(prev) = &self.ret_ty {
            let prev = prev.clone();
            if prev == t {
                return;
            }
            if prev == CTy::Int && t == CTy::Float {
                self.saw_float_ret = true;
                return;
            }
            if prev == CTy::Float && t == CTy::Int {
                return; // int widens to float
            }
            self.diag(
                format!(
                    "inconsistent return types: expected {}, found {}",
                    prev.name(),
                    t.name()
                ),
                span,
            );
        } else {
            self.ret_ty = Some(t);
        }
    }

    // ------------------------------------------------------------------
    // Expressions
    // ------------------------------------------------------------------

    fn expr_ty(&self, e: &CExpr) -> CTy {
        match e {
            CExpr::Int(_) => CTy::Int,
            CExpr::Float(_) => CTy::Float,
            CExpr::Bool(_) => CTy::Bool,
            CExpr::Str(_) => CTy::Str,
            CExpr::Template(_) => CTy::Str,
            CExpr::Ident(_, t) => t.clone(),
            CExpr::Unary(_, _, t) => t.clone(),
            CExpr::Binary(_, _, _, t) => t.clone(),
            CExpr::CallFn { ret, .. } => ret.clone(),
            CExpr::CallResolver { .. } => CTy::Void,
            CExpr::Await(_, t) => t.clone(),
            CExpr::NewPromise { lambda } => {
                // Promise<T> where T is the executor's resolve type
                self.lambdas[*lambda]
                    .params
                    .first()
                    .and_then(|(_, ty)| match ty {
                        CTy::Resolver(u) => Some(CTy::Promise(u.clone())),
                        _ => None,
                    })
                    .unwrap_or(CTy::Void)
            }
            CExpr::ConsoleLog(_) => CTy::Void,
            CExpr::SetTimeout { .. } => CTy::Void,
            CExpr::ArrayLit { elem_ty, .. } => CTy::Array(Box::new(elem_ty.clone())),
            CExpr::Index { ty, .. } => ty.clone(),
            CExpr::ArrayOp { ret, .. } => ret.clone(),
            CExpr::ArrayHof { ret, .. } => ret.clone(),
            CExpr::LambdaVal { ret, .. } => ret.clone(),
            CExpr::StrLen(_) => CTy::Int,
            CExpr::ObjectLit { class_id, .. } => {
                let name = self.classes[*class_id].name.clone();
                CTy::Object(ObjectTy { id: *class_id, name })
            }
            CExpr::NewClass { class_id } => {
                let name = self.classes[*class_id].name.clone();
                CTy::Object(ObjectTy { id: *class_id, name })
            }
            CExpr::FieldGet { ty, .. } => ty.clone(),
            CExpr::NetConnect { .. } => CTy::Stream,
            CExpr::NetWrite { .. } => CTy::Void,
            CExpr::NetReadAll { .. } => CTy::Str,
            CExpr::NetClose { .. } => CTy::Void,
            CExpr::HttpCall { .. } => {
                let id = self.class_by_name.get("http_Response").copied().unwrap_or(usize::MAX);
                CTy::Object(ObjectTy { id, name: "http_Response".into() })
            }
            CExpr::HttpListen { .. } => CTy::Void,
            CExpr::ResSend { .. } => CTy::Void,
            CExpr::HttpNewServer => {
                let id = *self.class_by_name.get("http_Server").unwrap_or(&usize::MAX);
                CTy::Object(ObjectTy { id, name: "http_Server".into() })
            }
            CExpr::HttpRouter { .. } => {
                let id = *self.class_by_name.get("http_Router").unwrap_or(&usize::MAX);
                CTy::Object(ObjectTy { id, name: "http_Router".into() })
            }
            CExpr::HttpRouteAdd { .. } => CTy::Void,
            CExpr::HttpServerListen { .. } => CTy::Promise(Box::new(CTy::Void)),
            CExpr::HttpValidatorNew { .. } => {
                let id = *self.class_by_name.get("http_JsonValidator").unwrap_or(&usize::MAX);
                CTy::Object(ObjectTy { id, name: "http_JsonValidator".into() })
            }
            CExpr::HttpFieldNew { .. } | CExpr::HttpFieldOp { .. } => {
                let id = *self.class_by_name.get("http_FieldValidator").unwrap_or(&usize::MAX);
                CTy::Object(ObjectTy { id, name: "http_FieldValidator".into() })
            }
            CExpr::HttpValidate { out_class, .. } => {
                let name = self.classes.get(*out_class).map(|d| d.name.clone()).unwrap_or_default();
                CTy::Object(ObjectTy { id: *out_class, name })
            }
        }
    }

    fn check_expr(&mut self, e: &Expr, allow_await_top: bool) -> CExpr {
        match e {
            Expr::Int(v, _) => CExpr::Int(*v),
            Expr::Float(v, _) => CExpr::Float(*v),
            Expr::Bool(v, _) => CExpr::Bool(*v),
            Expr::Str(s, _) => CExpr::Str(s.clone()),
            Expr::Template(parts, span) => {
                let mut out = Vec::new();
                for p in parts {
                    match p {
                        TPart::Str(s) => out.push(TPartC::Str(s.clone())),
                        TPart::Expr(inner) => {
                            let c = self.check_expr(inner, false);
                            if self.expr_ty(&c) == CTy::Void {
                                self.diag("cannot interpolate a void expression", inner.span());
                            }
                            out.push(TPartC::Expr(Box::new(c)));
                        }
                    }
                }
                if out.is_empty() {
                    self.diag("empty template literal", *span);
                }
                CExpr::Template(out)
            }
            Expr::Ident(name, span) => match self.lookup(name, *span) {
                Some((id, ty, _)) => CExpr::Ident(id, ty),
                None => CExpr::Bool(false),
            },
            Expr::Unary(op, inner, span) => {
                let c = self.check_expr(inner, false);
                let t = self.expr_ty(&c);
                match op {
                    UnOp::Neg => {
                        if !Self::is_numeric(&t) {
                            self.diag(
                                format!("unary '-' requires a number, found {}", t.name()),
                                *span,
                            );
                        }
                        CExpr::Unary(UnOpC::Neg, Box::new(c), t)
                    }
                    UnOp::Not => {
                        if t != CTy::Bool {
                            self.diag(
                                format!("'!' requires a bool, found {}", t.name()),
                                *span,
                            );
                        }
                        CExpr::Unary(UnOpC::Not, Box::new(c), CTy::Bool)
                    }
                }
            }
            Expr::Binary(op, l, r, span) => self.check_binary(*op, l, r, *span),
            Expr::Await(inner, span) => {
                if !allow_await_top {
                    self.diag(
                        "await is only allowed as `let x = await ...`, a standalone `await ...` statement, or `return await ...`",
                        *span,
                    );
                    return CExpr::Bool(false);
                }
                let c = self.check_expr(inner, false);
                let t = self.expr_ty(&c);
                match t {
                    CTy::Promise(u) => CExpr::Await(Box::new(c), *u),
                    other => {
                        self.diag(format!("can only await a Promise, found {}", other.name()), *span);
                        CExpr::Await(Box::new(c), CTy::Void)
                    }
                }
            }
            Expr::Call(callee, args, span) => self.check_call(callee, args, *span),
            Expr::ArrayLit(elems, span) => self.check_array_lit(elems, *span, None),
            Expr::ObjectLit(fields, span) => self.check_object_lit(fields, *span, None, None),
            Expr::NewClass(name, span) => {
                match self.class_by_name.get(name) {
                    Some(&id) => CExpr::NewClass { class_id: id },
                    None => {
                        self.diag(format!("unknown class '{}'", name), *span);
                        CExpr::Bool(false)
                    }
                }
            }
            Expr::NewCtor { path, args, span } => {
                if path.len() == 2 && path[0] == "http" && path[1] == "Server" {
                    if !args.is_empty() {
                        self.diag("new http.Server() takes no arguments", *span);
                    }
                    return CExpr::HttpNewServer;
                }
                if path.len() == 2 && path[0] == "http" && path[1] == "JsonValidator" {
                    if args.len() != 1 {
                        self.diag(
                            "new http.JsonValidator({...}) takes one schema object: { field: http.JsonValidator.string(), ... }",
                            *span,
                        );
                        return CExpr::Bool(false);
                    }
                    let schema = self.check_expr(&args[0], false);
                    let fields = match (&schema, self.expr_ty(&schema)) {
                        (CExpr::ObjectLit { class_id, inits }, CTy::Object(_)) => {
                            let mut fs = Vec::new();
                            for (idx, c) in inits {
                                let fname = self.classes[*class_id].fields[*idx].0.clone();
                                let t = self.expr_ty(c);
                                if !matches!(&t, CTy::Object(o) if o.name == "http_FieldValidator") {
                                    self.diag(
                                        format!(
                                            "JsonValidator schema field '{}' must be a field validator (e.g. http.JsonValidator.string().required())",
                                            fname
                                        ),
                                        *span,
                                    );
                                    return CExpr::Bool(false);
                                }
                                fs.push((fname, validator_kind_of(c)));
                            }
                            fs
                        }
                        _ => {
                            self.diag(
                                "new http.JsonValidator takes an object literal schema: { field: http.JsonValidator.string(), ... }",
                                *span,
                            );
                            return CExpr::Bool(false);
                        }
                    };
                    return CExpr::HttpValidatorNew { schema: Box::new(schema), fields };
                }
                if path.len() == 1 {
                    return match self.class_by_name.get(&path[0]) {
                        Some(&id) => CExpr::NewClass { class_id: id },
                        None => {
                            self.diag(format!("unknown class '{}'", path[0]), *span);
                            CExpr::Bool(false)
                        }
                    };
                }
                self.diag(format!("unknown class '{}'", path.join(".")), *span);
                CExpr::Bool(false)
            }
            Expr::Index(obj, idx, span) => {
                let oc = self.check_expr(obj, false);
                let ot = self.expr_ty(&oc);
                let ic = self.check_expr(idx, false);
                let it = self.expr_ty(&ic);
                if it != CTy::Int {
                    self.diag(
                        format!("array index must be Int32, found {}", it.name()),
                        *span,
                    );
                }
                match ot {
                    CTy::Array(elem) => CExpr::Index {
                        obj: Box::new(oc),
                        idx: Box::new(ic),
                        ty: *elem,
                    },
                    other => {
                        self.diag(
                            format!("indexing requires an Array, found {}", other.name()),
                            *span,
                        );
                        CExpr::Bool(false)
                    }
                }
            }
            Expr::Member(..) => {
                // property access: `arr.length`, `str.length`, and fields on
                // class instances / ghost objects
                if let Expr::Member(obj, name, _span) = e {
                    let oc = self.check_expr(obj, false);
                    let ot = self.expr_ty(&oc);
                    if name == "length" {
                        match &ot {
                            CTy::Array(_) => {
                                return CExpr::ArrayOp {
                                    op: ArrayOpKind::Len,
                                    arr: Box::new(oc),
                                    args: Vec::new(),
                                    ret: CTy::Int,
                                };
                            }
                            CTy::Str => return CExpr::StrLen(Box::new(oc)),
                            _ => {}
                        }
                    }
                    if let CTy::Object(o) = &ot {
                        let def = &self.classes[o.id];
                        if let Some(idx) = def.fields.iter().position(|(n, _)| n == name) {
                            let ty = def.fields[idx].1.clone();
                            return CExpr::FieldGet { obj: Box::new(oc), idx, ty };
                        }
                        let names: Vec<String> =
                            def.fields.iter().map(|(n, _)| n.clone()).collect();
                        self.diag(
                            format!(
                                "class '{}' has no field '{}' (fields: {})",
                                o.name,
                                name,
                                if names.is_empty() { "none".to_string() } else { names.join(", ") }
                            ),
                            e.span(),
                        );
                        return CExpr::Bool(false);
                    }
                }
                self.diag(
                    "member access is only supported for 'console.log(...)', 'arr.length' and 'str.length'",
                    e.span(),
                );
                CExpr::Bool(false)
            }
            Expr::NewPromise(executor, span) => {
                let lambda = match &**executor {
                    Expr::Lambda(l, _) => l,
                    _ => {
                        self.diag(
                            "new Promise(...) takes an executor lambda: new Promise((resolve) => { ... })",
                            *span,
                        );
                        return CExpr::Bool(false);
                    }
                };
                let idx = self.check_executor_lambda(lambda);
                CExpr::NewPromise { lambda: idx }
            }
            Expr::Lambda(..) => {
                self.diag(
                    "lambda functions can only be passed directly to setTimeout(...) or new Promise(...)",
                    e.span(),
                );
                CExpr::Bool(false)
            }
        }
    }

    fn check_binary(&mut self, op: BinOp, l: &Expr, r: &Expr, span: Span) -> CExpr {
        let lc = self.check_expr(l, false);
        let rc = self.check_expr(r, false);
        let lt = self.expr_ty(&lc);
        let rt = self.expr_ty(&rc);
        let opc = match op {
            BinOp::Add => BinOpC::Add,
            BinOp::Sub => BinOpC::Sub,
            BinOp::Mul => BinOpC::Mul,
            BinOp::Div => BinOpC::Div,
            BinOp::Mod => BinOpC::Mod,
            BinOp::Lt => BinOpC::Lt,
            BinOp::Gt => BinOpC::Gt,
            BinOp::Le => BinOpC::Le,
            BinOp::Ge => BinOpC::Ge,
            BinOp::Eq => BinOpC::Eq,
            BinOp::Neq => BinOpC::Neq,
            BinOp::And => BinOpC::And,
            BinOp::Or => BinOpC::Or,
        };
        match opc {
            BinOpC::And | BinOpC::Or => {
                if lt != CTy::Bool || rt != CTy::Bool {
                    self.diag(
                        format!(
                            "'&&'/'||' require bools, found {} and {}",
                            lt.name(),
                            rt.name()
                        ),
                        span,
                    );
                }
                CExpr::Binary(opc, Box::new(lc), Box::new(rc), CTy::Bool)
            }
            BinOpC::Lt | BinOpC::Gt | BinOpC::Le | BinOpC::Ge => {
                if !Self::is_numeric(&lt) || !Self::is_numeric(&rt) {
                    self.diag(
                        format!(
                            "comparison requires numbers, found {} and {}",
                            lt.name(),
                            rt.name()
                        ),
                        span,
                    );
                }
                CExpr::Binary(opc, Box::new(lc), Box::new(rc), CTy::Bool)
            }
            BinOpC::Eq | BinOpC::Neq => {
                if lt == CTy::Str && rt == CTy::Str {
                    // string equality is lowered through the runtime helper
                } else {
                    if !Self::is_numeric(&lt) && lt != CTy::Bool {
                        self.diag(format!("'=='/'!=' requires numbers or bools, found {}", lt.name()), span);
                    }
                    if !Self::is_numeric(&rt) && rt != CTy::Bool {
                        self.diag(format!("'=='/'!=' requires numbers or bools, found {}", rt.name()), span);
                    }
                }
                CExpr::Binary(opc, Box::new(lc), Box::new(rc), CTy::Bool)
            }
            BinOpC::Add => {
                if lt == CTy::Str || rt == CTy::Str {
                    self.diag(
                        "cannot use '+' with strings (use a template literal `...${x}...`)",
                        span,
                    );
                }
                self.arith(opc, lc, rc, lt, rt, span)
            }
            BinOpC::Mod => {
                if lt != CTy::Int || rt != CTy::Int {
                    self.diag("operator '%' requires Int32 operands", span);
                }
                CExpr::Binary(opc, Box::new(lc), Box::new(rc), CTy::Int)
            }
            BinOpC::Sub | BinOpC::Mul | BinOpC::Div => {
                self.arith(opc, lc, rc, lt, rt, span)
            }
        }
    }

    fn arith(
        &mut self,
        opc: BinOpC,
        lc: CExpr,
        rc: CExpr,
        lt: CTy,
        rt: CTy,
        span: Span,
    ) -> CExpr {
        if !Self::is_numeric(&lt) || !Self::is_numeric(&rt) {
            self.diag(
                format!(
                    "arithmetic requires numbers, found {} and {}",
                    lt.name(),
                    rt.name()
                ),
                span,
            );
        }
        // If either operand is Float64, both are computed as Float64
        // (`a + b` in calculate.ts: Int32 a is cast to Float64).
        let result = if matches!(lt, CTy::Float) || matches!(rt, CTy::Float) {
            CTy::Float
        } else {
            CTy::Int
        };
        CExpr::Binary(opc, Box::new(lc), Box::new(rc), result)
    }

    fn check_net_module(&mut self, name: &str, args: &[Expr], span: Span) -> CExpr {
        match name {
            "connect" => {
                if args.len() != 2 {
                    self.diag("net.connect takes (host, port)", span);
                    return CExpr::Bool(false);
                }
                let host = self.check_expr(&args[0], false);
                let port = self.check_expr(&args[1], false);
                self.coerce(&self.expr_ty(&host), &CTy::Str, args[0].span());
                self.coerce(&self.expr_ty(&port), &CTy::Int, args[1].span());
                CExpr::NetConnect { host: Box::new(host), port: Box::new(port) }
            }
            other => {
                self.diag(
                    format!(
                        "net.{} is not supported (net has connect; streams have write, read_all, close)",
                        other
                    ),
                    span,
                );
                CExpr::Bool(false)
            }
        }
    }

    fn check_http_module(&mut self, name: &str, args: &[Expr], span: Span) -> CExpr {
        match name {
            "get" => {
                if args.len() != 1 {
                    self.diag("http.get takes (url)", span);
                    return CExpr::Bool(false);
                }
                let url = self.check_expr(&args[0], false);
                self.coerce(&self.expr_ty(&url), &CTy::Str, args[0].span());
                CExpr::HttpCall { method: "GET".into(), url: Box::new(url), body: None }
            }
            "post" => {
                if args.len() != 2 {
                    self.diag("http.post takes (url, body)", span);
                    return CExpr::Bool(false);
                }
                let url = self.check_expr(&args[0], false);
                self.coerce(&self.expr_ty(&url), &CTy::Str, args[0].span());
                let body = self.check_expr(&args[1], false);
                self.coerce(&self.expr_ty(&body), &CTy::Str, args[1].span());
                CExpr::HttpCall { method: "POST".into(), url: Box::new(url), body: Some(Box::new(body)) }
            }
            "listen" => {
                if args.len() != 2 {
                    self.diag("http.listen takes (port, handler)", span);
                    return CExpr::Bool(false);
                }
                let port = self.check_expr(&args[0], false);
                self.coerce(&self.expr_ty(&port), &CTy::Int, args[0].span());
                let lambda = match &args[1] {
                    Expr::Lambda(l, _) => {
                        let req_id = *self.class_by_name.get("http_Request").unwrap_or(&usize::MAX);
                        let res_id = *self.class_by_name.get("http_Res").unwrap_or(&usize::MAX);
                        let (li, _ret) = self.check_callback_lambda(
                            l,
                            vec![
                                CTy::Object(ObjectTy { id: req_id, name: "http_Request".into() }),
                                CTy::Object(ObjectTy { id: res_id, name: "http_Res".into() }),
                            ],
                            Some(CTy::Void),
                            LambdaAbi::Listen,
                        );
                        li
                    }
                    _ => {
                        self.diag(
                            "http.listen's handler must be a lambda: (req, res) => { ... }",
                            args[1].span(),
                        );
                        return CExpr::Bool(false);
                    }
                };
                CExpr::HttpListen { port: Box::new(port), lambda }
            }
            other => {
                self.diag(
                    format!("http.{} is not supported (http has: get, post, listen)", other),
                    span,
                );
                CExpr::Bool(false)
            }
        }
    }

    fn check_stream_method(&mut self, stream: CExpr, name: &str, args: &[Expr], span: Span) -> CExpr {
        match name {
            "write" => {
                if args.len() != 1 {
                    self.diag("stream.write takes one string", span);
                    return CExpr::Bool(false);
                }
                let data = self.check_expr(&args[0], false);
                self.coerce(&self.expr_ty(&data), &CTy::Str, args[0].span());
                CExpr::NetWrite { stream: Box::new(stream), data: Box::new(data) }
            }
            "read_all" => {
                if !args.is_empty() {
                    self.diag("stream.read_all takes no arguments", span);
                    return CExpr::Bool(false);
                }
                CExpr::NetReadAll { stream: Box::new(stream) }
            }
            "close" => {
                if !args.is_empty() {
                    self.diag("stream.close takes no arguments", span);
                    return CExpr::Bool(false);
                }
                CExpr::NetClose { stream: Box::new(stream) }
            }
            other => {
                self.diag(
                    format!("streams have no method '{}' (write, read_all, close)", other),
                    span,
                );
                CExpr::Bool(false)
            }
        }
    }

    fn check_res_method(&mut self, res: CExpr, name: &str, args: &[Expr], span: Span) -> CExpr {
        match name {
            "send" | "json" => {
                if args.len() != 1 {
                    self.diag(format!("res.{} takes exactly one value", name), span);
                    return CExpr::Bool(false);
                }
                let v = self.check_expr(&args[0], false);
                let t = self.expr_ty(&v);
                if t == CTy::Void {
                    self.diag("cannot send a void value", span);
                }
                if let CTy::Promise(_) = &t {
                    self.diag("cannot send a Promise (await it first)", span);
                }
                CExpr::ResSend { res: Box::new(res), value: Box::new(v), json: name == "json" }
            }
            other => {
                self.diag(format!("responses have no method '{}' (send, json)", other), span);
                CExpr::Bool(false)
            }
        }
    }

    /// Dispatch a member call on an http std-module object. Returns None when
/// the receiver is not one of the builtin http objects.
fn check_http_object_method(
        &mut self,
        oc: CExpr,
        o: &ObjectTy,
        name: &str,
        args: &[Expr],
        span: Span,
    ) -> Option<CExpr> {
        match o.name.as_str() {
            "http_Res" => Some(self.check_res_method(oc, name, args, span)),
            "http_Server" => Some(self.check_server_method(oc, name, args, span)),
            "http_Router" => Some(self.check_router_method(oc, name, args, span)),
            "http_JsonValidator" => Some(self.check_validator_method(oc, name, args, span)),
            "http_FieldValidator" => Some(self.check_field_method(oc, name, args, span)),
            _ => None,
        }
    }

    fn check_server_method(&mut self, server: CExpr, name: &str, args: &[Expr], span: Span) -> CExpr {
        match name {
            "router" => {
                if args.len() != 1 {
                    self.diag("server.router takes one options object: { path: \"/...\" }", span);
                    return CExpr::Bool(false);
                }
                let path = self.check_expr(&args[0], false);
                let pt = self.expr_ty(&path);
                let ok = match &path {
                    CExpr::ObjectLit { class_id, inits } => {
                        // exactly one field `path` with a string value
                        let fields = &self.classes[*class_id].fields;
                        fields.len() == 1
                            && fields[0].0 == "path"
                            && match inits.first() {
                                Some((_, c)) => self.expr_ty(c) == CTy::Str,
                                None => false,
                            }
                    }
                    _ => false,
                };
                let path_val = match &path {
                    CExpr::ObjectLit { inits, .. } => {
                        // exactly one field `path` with a string value; lower
                        // to just the string value
                        inits.first().map(|(_, c)| c.clone())
                    }
                    _ => None,
                };
  if !ok {
                    self.diag(
                        "server.router takes { path: \"...\" } with a string path",
                        span,
                    );
                    return CExpr::Bool(false);
                }
                let _ = pt;
                CExpr::HttpRouter {
                    server: Box::new(server),
                    path: Box::new(path_val.unwrap_or(CExpr::Str(String::new()))),
                }
            }
            "listen" => {
                if args.len() != 2 {
                    self.diag("server.listen takes (host, port)", span);
                    return CExpr::Bool(false);
                }
                let host = self.check_expr(&args[0], false);
                self.coerce(&self.expr_ty(&host), &CTy::Str, args[0].span());
                let port = self.check_expr(&args[1], false);
                self.coerce(&self.expr_ty(&port), &CTy::Int, args[1].span());
                CExpr::HttpServerListen { host: Box::new(host), port: Box::new(port) }
            }
            other => {
                self.diag(
                    format!("http.Server has no method '{}' (available: router, listen)", other),
                    span,
                );
                CExpr::Bool(false)
            }
        }
    }

    fn check_router_method(&mut self, router: CExpr, name: &str, args: &[Expr], span: Span) -> CExpr {
        let method = match name {
            "get" => "GET",
            "post" => "POST",
            other => {
                self.diag(
                    format!("routers have no method '{}' (available: get, post)", other),
                    span,
                );
                return CExpr::Bool(false);
            }
        };
        if args.len() != 2 {
            self.diag(format!("router.{} takes (path, handler)", name), span);
            return CExpr::Bool(false);
        }
        let path = self.check_expr(&args[0], false);
        self.coerce(&self.expr_ty(&path), &CTy::Str, args[0].span());
        let lambda = match &args[1] {
            Expr::Lambda(l, _) => {
                let req_id = *self.class_by_name.get("http_Request").unwrap_or(&usize::MAX);
                let res_id = *self.class_by_name.get("http_Res").unwrap_or(&usize::MAX);
                let (li, _ret) = self.check_callback_lambda(
                    l,
                    vec![
                        CTy::Object(ObjectTy { id: req_id, name: "http_Request".into() }),
                        CTy::Object(ObjectTy { id: res_id, name: "http_Res".into() }),
                    ],
                    Some(CTy::Void),
                    LambdaAbi::Listen,
                );
                li
            }
            _ => {
                self.diag(format!("router.{} takes a handler lambda (req, res) => {{ ... }}", name), span);
                return CExpr::Bool(false);
            }
        };
        CExpr::HttpRouteAdd {
            router: Box::new(router),
            method: method.to_string(),
            path: Box::new(path),
            lambda,
        }
    }

    fn check_validator_method(
        &mut self,
        validator: CExpr,
        name: &str,
        args: &[Expr],
        span: Span,
    ) -> CExpr {
        if name != "validate" {
            self.diag(
                format!("http.JsonValidator has no method '{}' (available: validate)", name),
                span,
            );
            return CExpr::Bool(false);
        }
        if args.len() != 1 {
            self.diag("validate takes the body string to check", span);
            return CExpr::Bool(false);
        }
        let body = self.check_expr(&args[0], false);
        self.coerce(&self.expr_ty(&body), &CTy::Str, args[0].span());
        // the validated result's shape comes from the schema the validator
        // was created with — tracked when `new http.JsonValidator(...)`
        // was bound to a variable
        let fields = match &validator {
            CExpr::Ident(id, _) => self.validator_fields.get(id).cloned(),
            _ => None,
        };
        let Some(fields) = fields else {
            self.diag(
                "cannot infer the validated shape (assign the validator to a variable first: `let v = new http.JsonValidator({...})`)",
                span,
            );
            return CExpr::Bool(false);
        };
        // synthesize the ghost class of validated results: same field names,
        // typed by the validator kind
        let class_name = format!("Class_anon_{:04}", self.ghost_count);
        self.ghost_count += 1;
        let def = ClassDef {
            name: class_name,
            fields: fields
                .iter()
                .map(|(f, k)| {
                    (f.clone(), match k {
                        FieldKind::Str => CTy::Str,
                        FieldKind::Int => CTy::Int,
                    })
                })
                .collect(),
            is_ghost: true,
        };
        self.classes.push(def);
        let out_class = self.classes.len() - 1;
        CExpr::HttpValidate { v: Box::new(validator), body: Box::new(body), out_class }
    }

    fn check_field_method(&mut self, v: CExpr, name: &str, args: &[Expr], span: Span) -> CExpr {
        match name {
            "required" => {
                if !args.is_empty() {
                    self.diag(".required() takes no arguments", span);
                    return CExpr::Bool(false);
                }
                CExpr::HttpFieldOp { v: Box::new(v), op: FieldOp::Required, arg: None }
            }
            "min" | "max" => {
                if args.len() != 1 {
                    self.diag(format!(".{}() takes one number", name), span);
                    return CExpr::Bool(false);
                }
                let arg = self.check_expr(&args[0], false);
                self.coerce(&self.expr_ty(&arg), &CTy::Int, args[0].span());
                let op = if name == "min" { FieldOp::Min } else { FieldOp::Max };
                CExpr::HttpFieldOp { v: Box::new(v), op, arg: Some(Box::new(arg)) }
            }
            other => {
                self.diag(
                    format!(
                        "field validators have no method '{}' (chainable: required, min, max)",
                        other
                    ),
                    span,
                );
                CExpr::Bool(false)
            }
        }
    }

    fn check_call(&mut self, callee: &Expr, args: &[Expr], span: Span) -> CExpr {
        // std-module and package functions: `net.connect(...)`, `http.get(...)`,
        // `mypkg.fn(...)` (registry packages are exposed as `pkg__fn`)
        if let Expr::Member(obj, name, _) = callee {
            if let Expr::Ident(m, _) = &**obj {
                if self.uses.iter().any(|u| u == m) {
                    match m.as_str() {
                        "net" => return self.check_net_module(name, args, span),
                        "http" => return self.check_http_module(name, args, span),
                        _ => {
                            let full = format!("{}__{}", m, name);
                            if self.sigs.contains_key(&full) {
                                let ident = Expr::Ident(full, span);
                                return self.check_call(&ident, args, span);
                            }
                            self.diag(
                                format!("module '{}' has no function '{}'", m, name),
                                span,
                            );
                            return CExpr::Bool(false);
                        }
                    }
                }
            }
            // static methods on exported std-module classes:
            // `http.JsonValidator.string()` etc.
            if let Expr::Member(obj2, cls, _) = &**obj {
                if let Expr::Ident(m2, _) = &**obj2 {
                    if m2 == "http" && self.uses.iter().any(|u| u == "http") && cls == "JsonValidator" {
                        return match name.as_str() {
                            "string" => CExpr::HttpFieldNew { kind: FieldKind::Str },
                            "number" => CExpr::HttpFieldNew { kind: FieldKind::Int },
                            other => {
                                self.diag(
                                    format!(
                                        "JsonValidator.{} is not supported (statics: string, number)",
                                        other
                                    ),
                                    span,
                                );
                                CExpr::Bool(false)
                            }
                        };
                    }
                }
            }
        }
        // console.log(...)
        if let Expr::Member(obj, name, _) = callee {
            if let Expr::Ident(o, _) = &**obj {
                if o == "console" {
                    if name != "log" {
                        self.diag(format!("console.{} is not supported (only console.log)", name), span);
                    }
                    if args.len() != 1 {
                        self.diag("console.log takes exactly one argument", span);
                        return CExpr::Bool(false);
                    }
                    let a = self.check_expr(&args[0], false);
                    let t = self.expr_ty(&a);
                    if t == CTy::Void {
                        self.diag("cannot print a void value", span);
                    }
                    if let CTy::Promise(_) = &t {
                        self.diag(
                            "cannot print a Promise (await it first)",
                            span,
                        );
                    }
                    return CExpr::ConsoleLog(Box::new(a));
                }
                // other member calls: array methods, streams, http std-module objects
                let oc = self.check_expr(obj, false);
                let ot = self.expr_ty(&oc);
                if let CTy::Array(elem) = ot {
                    return self.check_array_method(oc, *elem, name, args, span);
                }
                if let CTy::Stream = ot {
                    return self.check_stream_method(oc, name, args, span);
                }
                if let CTy::Object(o) = &ot {
                    if let Some(r) = self.check_http_object_method(oc, o, name, args, span) {
                        return r;
                    }
                    self.diag(
                        format!(
                            "class '{}' has no methods (marina objects are fields only)",
                            o.name
                        ),
                        span,
                    );
                    return CExpr::Bool(false);
                }
                self.diag(format!("'{}' is not supported", name), span);
                return CExpr::Bool(false);
            }
            // member calls on non-identifier objects (e.g. chained method
            // calls): array methods and http std-module objects too
            let oc = self.check_expr(obj, false);
            let ot = self.expr_ty(&oc);
            if let CTy::Array(elem) = ot {
                return self.check_array_method(oc, *elem, name, args, span);
            }
            if let CTy::Stream = ot {
                return self.check_stream_method(oc, name, args, span);
            }
            if let CTy::Object(o) = &ot {
                if let Some(r) = self.check_http_object_method(oc, o, name, args, span) {
                    return r;
                }
            }
            self.diag("member access is only supported for 'console.log(...)' and array methods", span);
            return CExpr::Bool(false);
        }
        if let Expr::Ident(name, _) = callee {
            // setTimeout(fn, ms) — builtin, checked before variable lookup
            if name == "setTimeout" {
                if args.len() != 2 {
                    self.diag("setTimeout takes (callback, milliseconds)", span);
                    return CExpr::Bool(false);
                }
                let lambda = match &args[0] {
                    Expr::Lambda(l, _) => l,
                    _ => {
                        self.diag(
                            "setTimeout's first argument must be a lambda: setTimeout(() => { ... }, ms)",
                            args[0].span(),
                        );
                        return CExpr::Bool(false);
                    }
                };
                if !lambda.params.is_empty() {
                    self.diag("setTimeout callbacks take no parameters", lambda.span);
                }
                let idx = self.check_plain_lambda(lambda);
                let ms = self.check_expr(&args[1], false);
                if self.expr_ty(&ms) != CTy::Int {
                    self.diag("setTimeout's delay must be an Int32", args[1].span());
                }
                return CExpr::SetTimeout { lambda: idx, ms: Box::new(ms) };
            }
            // user function
            if let Some(sig) = self.sigs.get(name).map(|s| (s.params.clone(), s.ret.clone(), s.is_async)) {
                let (params, ret, is_async) = sig;
                if args.len() != params.len() {
                    self.diag(
                        format!(
                            "function '{}' takes {} argument(s), got {}",
                            name,
                            params.len(),
                            args.len()
                        ),
                        span,
                    );
                }
                let mut cargs = Vec::new();
                for (i, a) in args.iter().enumerate() {
                    let c = self.check_expr(a, false);
                    if let Some(pt) = params.get(i) {
                        let at = self.expr_ty(&c);
                        self.coerce(&at, pt, a.span());
                    }
                    cargs.push(c);
                }
                match ret {
                    Some(r) => CExpr::CallFn { name: name.clone(), args: cargs, ret: r, is_async },
                    None => {
                        self.diag(
                            format!(
                                "cannot call '{}' here: its return type is not yet known (add a return type annotation, e.g. `: void`)",
                                name
                            ),
                            span,
                        );
                        CExpr::Bool(false)
                    }
                }
            }
            // resolver call inside a promise executor?
            else if let Some((id, ty, _)) = self.lookup(name, span) {
                if let CTy::Resolver(t) = &ty {
                    if self.reject_ids.contains(&id) {
                        self.diag("'reject' is not supported in marina v0.1 (promises never fail)", span);
                        return CExpr::Bool(false);
                    }
                    if args.len() != 1 {
                        self.diag("resolve takes exactly one value", span);
                        return CExpr::Bool(false);
                    }
                    let a = self.check_expr(&args[0], false);
                    let at = self.expr_ty(&a);
                    let rt = (**t).clone();
                    self.infer_promise_type(&rt, at, span);
                    return CExpr::CallResolver { id, arg: Box::new(a), inner: rt };
                }
                self.diag(format!("'{}' is not a function", name), span);
                CExpr::Bool(false)
            } else {
                self.diag(format!("'{}' is not a function", name), span);
                CExpr::Bool(false)
            }
        } else {
            self.diag("this expression is not callable", span);
            CExpr::Bool(false)
        }
    }

    fn infer_promise_type(&mut self, declared: &CTy, arg: CTy, span: Span) {
        match &mut self.promise_infer {
            None => {
                self.promise_infer = Some((arg, false));
            }
            Some((cur, saw_float)) => {
                if *cur == arg {
                    return;
                }
                if *cur == CTy::Int && arg == CTy::Float
                    || *cur == CTy::Float && arg == CTy::Int
                {
                    *cur = CTy::Float;
                    *saw_float = true;
                    return;
                }
                self.coerce(&arg, declared, span);
            }
        }
        let _ = declared;
    }

    // ------------------------------------------------------------------
    // Lambdas
    // ------------------------------------------------------------------

    fn enter_lambda_scope(&mut self, lambda_idx: usize) -> usize {
        let base = self.scopes.len();
        self.lambda_base.push(base);
        self.lambda_idx.push(lambda_idx);
        self.cur_captures.push(Vec::new());
        self.lambda_ret_ok.push(false);
        self.scopes.push(Default::default());
        base
    }

    fn exit_lambda_scope(&mut self) -> Vec<(usize, CTy)> {
        self.scopes.pop();
        self.lambda_base.pop();
        self.lambda_idx.pop();
        let caps = self.cur_captures.pop().unwrap_or_default();
        self.lambda_ret_ok.pop();
        caps
    }

    /// setTimeout callback: no params, must return void.
    fn check_plain_lambda(&mut self, l: &Lambda) -> usize {
        let idx = self.lambdas.len();
        self.lambdas.push(CLambda {
            params: Vec::new(),
            captures: Vec::new(),
            body: Vec::new(),
            ret: CTy::Void,
            abi: LambdaAbi::Plain,
            is_executor: false,
        });
        self.enter_lambda_scope(idx);
        self.ret_ty = None;
        self.saw_float_ret = false;
        let body = self.check_stmts(&l.body);
        let r = self.ret_ty.clone().unwrap_or(CTy::Void);
        if r != CTy::Void {
            self.diag("setTimeout callbacks cannot return a value", l.span);
        }
        let captures = self.exit_lambda_scope();
        self.lambdas[idx].body = body;
        self.lambdas[idx].captures = captures;
        idx
    }

    /// `new Promise((resolve) => { ... })`: infer T from resolve(...) calls.
    fn check_executor_lambda(&mut self, l: &Lambda) -> usize {
        if l.params.is_empty() || l.params.len() > 2 {
            self.diag(
                "a promise executor takes (resolve) or (resolve, reject)",
                l.span,
            );
            // still create a stub so compilation can continue
            let idx = self.lambdas.len();
            self.lambdas.push(CLambda {
                params: Vec::new(),
                captures: Vec::new(),
                body: Vec::new(),
                ret: CTy::Void,
                abi: LambdaAbi::Plain,
                is_executor: true,
            });
            return idx;
        }
        let idx = self.lambdas.len();
        self.lambdas.push(CLambda {
            params: Vec::new(),
            captures: Vec::new(),
            body: Vec::new(),
            ret: CTy::Void,
            abi: LambdaAbi::Plain,
            is_executor: true,
        });
        let base = self.enter_lambda_scope(idx);
        let _ = base;

        let prev_infer = self.promise_infer.take();
        self.promise_infer = None;

        let mut params = Vec::new();
        let mut reject_id = None;
        for (i, p) in l.params.iter().enumerate() {
            // placeholder type; finalized below once T is inferred
            let id = self.declare(&p.name, CTy::Resolver(Box::new(CTy::Void)), true, p.span);
            if i == 1 {
                reject_id = Some(id);
            }
            params.push((id, CTy::Resolver(Box::new(CTy::Void))));
        }
        let saved_reject = self.reject_ids.clone();
        if let Some(rid) = reject_id {
            self.reject_ids.push(rid);
        }

        self.ret_ty = None;
        self.saw_float_ret = false;
        let body = self.check_stmts(&l.body);
        let r = self.ret_ty.clone().unwrap_or(CTy::Void);
        if r != CTy::Void {
            self.diag("a promise executor cannot return a value", l.span);
        }

        let t = match self.promise_infer.take() {
            None => {
                self.diag(
                    "cannot infer the promise value type: the executor never calls resolve (write `resolve(<value>)` inside)",
                    l.span,
                );
                CTy::Void
            }
            Some((ty, saw_float)) => {
                if saw_float && ty == CTy::Int {
                    CTy::Float
                } else {
                    ty
                }
            }
        };
        self.promise_infer = prev_infer;
        self.reject_ids = saved_reject;

        let final_params: Vec<(usize, CTy)> = params
            .iter()
            .map(|(id, _)| {
                self.var_types[*id] = CTy::Resolver(Box::new(t.clone()));
                (*id, CTy::Resolver(Box::new(t.clone())))
            })
            .collect();

        let captures = self.exit_lambda_scope();
        self.lambdas[idx].params = final_params;
        self.lambdas[idx].captures = captures;
        self.lambdas[idx].body = body;
        idx
    }

    /// A lambda passed to an array higher-order method. Param types come
    /// from the context (the array's element type etc.); the return type is
    /// checked against `expect_ret` (or inferred when None).
    fn check_callback_lambda(
        &mut self,
        l: &Lambda,
        param_tys: Vec<CTy>,
        expect_ret: Option<CTy>,
        abi: LambdaAbi,
    ) -> (usize, CTy) {
        if l.params.len() != param_tys.len() {
            self.diag(
                format!(
                    "this callback takes {} parameter(s), got {}",
                    param_tys.len(),
                    l.params.len()
                ),
                l.span,
            );
        }
        let idx = self.lambdas.len();
        self.lambdas.push(CLambda {
            params: Vec::new(),
            captures: Vec::new(),
            body: Vec::new(),
            ret: CTy::Void,
            abi,
            is_executor: false,
        });
        self.enter_lambda_scope(idx);
        // callbacks may return a value
        if let Some(flag) = self.lambda_ret_ok.last_mut() {
            *flag = true;
        }
        let mut params = Vec::new();
        for (p, ty) in l.params.iter().zip(&param_tys) {
            if let Some(ann) = &p.ann {
                let at = self.ty_of(ann);
                self.coerce(ty, &at, p.span);
            }
            let id = self.declare(&p.name, ty.clone(), true, p.span);
            params.push((id, ty.clone()));
        }
        self.ret_ty = None;
        self.saw_float_ret = false;
        let body = self.check_stmts(&l.body);
        let mut ret = self.ret_ty.clone().unwrap_or(CTy::Void);
        if self.saw_float_ret && ret == CTy::Int {
            ret = CTy::Float;
        }
        if let Some(exp) = &expect_ret {
            self.coerce(&ret, exp, l.span);
            ret = exp.clone();
        }
        let captures = self.exit_lambda_scope();
        self.lambdas[idx].params = params;
        self.lambdas[idx].captures = captures;
        self.lambdas[idx].body = body;
        self.lambdas[idx].ret = ret.clone();
        (idx, ret)
    }

/// `{ field: expr, ... }`: with `target` (a declared class id from an
/// annotation) the literal must match that class field-for-field; without
/// one, a ghost class is created, named `Class_{var}_{NNNN}` (or
/// `Class_anon_NNNN` when the literal is not a direct `let` initializer).
fn check_object_lit(
    &mut self,
    fields: &[(String, Expr)],
    span: Span,
    target: Option<usize>,
    var_name: Option<&str>,
) -> CExpr {
    if fields.is_empty() {
        self.diag(
            "cannot infer the shape of an empty object literal (use `new ClassName()` or annotate a class)",
            span,
        );
        return CExpr::Bool(false);
    }
    // check field values + duplicate field names
    let mut cs: Vec<(String, CExpr)> = Vec::new();
    let mut seen = std::collections::HashSet::new();
    for (name, e) in fields {
        if !seen.insert(name.clone()) {
            self.diag(format!("duplicate field '{}' in object literal", name), span);
            continue;
        }
        let c = self.check_expr(e, false);
        let t = self.expr_ty(&c);
        if t == CTy::Void || matches!(t, CTy::Promise(_) | CTy::Resolver(_)) {
            self.diag(format!("object fields cannot be {}", t.name()), span);
            continue;
        }
        cs.push((name.clone(), c));
    }
    let class_id = match target {
        Some(id) => {
            // structural match: every declared field must be provided with a
            // compatible value, and no extra fields are allowed
            let def_fields = self.classes[id].fields.clone();
            let def_name = self.classes[id].name.clone();
            for (fname, fty) in &def_fields {
                match cs.iter().find(|(n, _)| n == fname) {
                    Some((_, c)) => {
                        self.coerce(&self.expr_ty(c), fty, span);
                    }
                    None => {
                        self.diag(
                            format!("missing field '{}' for class '{}'", fname, def_name),
                            span,
                        );
                    }
                }
            }
            for (n, _) in &cs {
                if !def_fields.iter().any(|(dn, _)| dn == n) {
                    self.diag(format!("class '{}' has no field '{}'", def_name, n), span);
                }
            }
            id
        }
        None => {
            let base = var_name.unwrap_or("anon");
            let name = format!("Class_{}_{:04}", base, self.ghost_count + 1);
            self.ghost_count += 1;
            let fdefs: Vec<(String, CTy)> =
                cs.iter().map(|(n, c)| (n.clone(), self.expr_ty(c))).collect();
            let id = self.classes.len();
            self.classes
                .push(ClassDef { name: name.clone(), fields: fdefs, is_ghost: true });
            id
        }
    };
    // build the initializers in class-field order (codegen writes by index)
    let def_fields = self.classes[class_id].fields.clone();
    let mut inits = Vec::new();
    for (idx, (fname, _)) in def_fields.iter().enumerate() {
        if let Some((_, c)) = cs.iter().find(|(n, _)| n == fname) {
            inits.push((idx, c.clone()));
        }
    }
    CExpr::ObjectLit { class_id, inits }
}

/// `[e1, e2, ...]`: infer the element type, or use `expected` (from a
/// variable annotation) for empty literals.
fn check_array_lit(&mut self, elems: &[Expr], span: Span, expected: Option<CTy>) -> CExpr {
        let mut cs = Vec::new();
        for el in elems {
            cs.push(self.check_expr(el, false));
        }
        let mut ty: Option<CTy> = None;
        let mut saw_float = false;
        for c in &cs {
            let t = self.expr_ty(c);
            if matches!(t, CTy::Float) {
                saw_float = true;
            }
            if t == CTy::Void || matches!(t, CTy::Promise(_) | CTy::Resolver(_)) {
                self.diag(format!("arrays cannot contain {}", t.name()), span);
                continue;
            }
            ty = Some(match ty {
                None => t,
                Some(prev) if prev == t => t,
                Some(CTy::Int) if t == CTy::Float => CTy::Float,
                Some(CTy::Float) if t == CTy::Int => CTy::Float,
                Some(prev) => {
                    self.diag(
                        format!(
                            "inconsistent array element types: {} and {}",
                            prev.name(),
                            t.name()
                        ),
                        span,
                    );
                    prev
                }
            });
        }
        let elem_ty = match ty {
            Some(CTy::Int) if saw_float => CTy::Float,
            Some(t) => t,
            None => match expected {
                Some(e) => e,
                None => {
                    self.diag(
                        "cannot infer the element type of an empty array literal (annotate the variable, e.g. `let a: Array<Int32> = []`)",
                        span,
                    );
                    CTy::Int
                }
            },
        };
        CExpr::ArrayLit { elems: cs, elem_ty }
    }

    /// `<array>.method(...)` — the one supported family of method calls.
    fn check_array_method(
        &mut self,
        arr: CExpr,
        elem: CTy,
        name: &str,
        args: &[Expr],
        span: Span,
    ) -> CExpr {
        let expect_count = |me: &mut Self, n: usize| {
            if args.len() != n {
                me.diag(format!("{}.{} takes {} argument(s), got {}", "array", name, n, args.len()), span);
            }
        };
        match name {
            "push" | "unshift" => {
                expect_count(self, 1);
                let a = self.check_expr(&args[0], false);
                self.coerce(&self.expr_ty(&a), &elem, span);
                let ret = CTy::Int;
                CExpr::ArrayOp {
                    op: if name == "push" { ArrayOpKind::Push } else { ArrayOpKind::Unshift },
                    arr: Box::new(arr),
                    args: vec![a],
                    ret,
                }
            }
            "pop" | "shift" => {
                expect_count(self, 0);
                CExpr::ArrayOp {
                    op: if name == "pop" { ArrayOpKind::Pop } else { ArrayOpKind::Shift },
                    arr: Box::new(arr),
                    args: Vec::new(),
                    ret: elem.clone(),
                }
            }
            "splice" | "slice" => {
                expect_count(self, 2);
                let mut cargs = Vec::new();
                for a in args {
                    let c = self.check_expr(a, false);
                    if self.expr_ty(&c) != CTy::Int {
                        self.diag(
                            format!("{}.{} indices must be Int32", "array", name),
                            span,
                        );
                    }
                    cargs.push(c);
                }
                CExpr::ArrayOp {
                    op: if name == "splice" { ArrayOpKind::Splice } else { ArrayOpKind::Slice },
                    arr: Box::new(arr),
                    args: cargs,
                    ret: CTy::Array(Box::new(elem.clone())),
                }
            }
            "concat" => {
                expect_count(self, 1);
                let a = self.check_expr(&args[0], false);
                let at = self.expr_ty(&a);
                if at != CTy::Array(Box::new(elem.clone())) {
                    self.diag(
                        format!(
                            "concat requires Array<{}>, found {}",
                            elem.name(),
                            at.name()
                        ),
                        span,
                    );
                }
                CExpr::ArrayOp {
                    op: ArrayOpKind::Concat,
                    arr: Box::new(arr),
                    args: vec![a],
                    ret: CTy::Array(Box::new(elem)),
                }
            }
            "join" => {
                expect_count(self, 1);
                let a = self.check_expr(&args[0], false);
                self.coerce(&self.expr_ty(&a), &CTy::Str, span);
                CExpr::ArrayOp {
                    op: ArrayOpKind::Join,
                    arr: Box::new(arr),
                    args: vec![a],
                    ret: CTy::Str,
                }
            }
            "reverse" | "sort" => {
                expect_count(self, 0);
                CExpr::ArrayOp {
                    op: if name == "reverse" { ArrayOpKind::Reverse } else { ArrayOpKind::Sort },
                    arr: Box::new(arr),
                    args: Vec::new(),
                    ret: CTy::Array(Box::new(elem)),
                }
            }
            "length" => {
                expect_count(self, 0);
                CExpr::ArrayOp {
                    op: ArrayOpKind::Len,
                    arr: Box::new(arr),
                    args: Vec::new(),
                    ret: CTy::Int,
                }
            }
            "forEach" => {
                expect_count(self, 1);
                let (idx, _ret) = match &args[0] {
                    Expr::Lambda(l, _) => {
                        self.check_callback_lambda(l, vec![elem.clone()], None, LambdaAbi::ForEach)
                    }
                    _ => {
                        self.diag("forEach takes a callback lambda: (item) => { ... }", span);
                        return CExpr::Bool(false);
                    }
                };
                CExpr::ArrayHof {
                    op: HofKind::ForEach,
                    arr: Box::new(arr),
                    cb: Box::new(CExpr::LambdaVal {
                        li: idx,
                        ret: CTy::Void,
                    }),
                    extra: None,
                    ret: CTy::Void,
                }
            }
            "map" => {
                expect_count(self, 1);
                let (idx, uret) = match &args[0] {
                    Expr::Lambda(l, _) => self.check_callback_lambda(
                        l,
                        vec![elem.clone()],
                        None,
                        LambdaAbi::MapFilter,
                    ),
                    _ => {
                        self.diag("map takes a callback lambda: (item) => ...", span);
                        return CExpr::Bool(false);
                    }
                };
                CExpr::ArrayHof {
                    op: HofKind::Map,
                    arr: Box::new(arr),
                    cb: Box::new(CExpr::LambdaVal {
                        li: idx,
                        ret: uret.clone(),
                    }),
                    extra: None,
                    ret: CTy::Array(Box::new(uret)),
                }
            }
            "filter" => {
                expect_count(self, 1);
                let (idx, _ret) = match &args[0] {
                    Expr::Lambda(l, _) => self.check_callback_lambda(
                        l,
                        vec![elem.clone()],
                        Some(CTy::Bool),
                        LambdaAbi::MapFilter,
                    ),
                    _ => {
                        self.diag("filter takes a callback lambda: (item) => bool", span);
                        return CExpr::Bool(false);
                    }
                };
                CExpr::ArrayHof {
                    op: HofKind::Filter,
                    arr: Box::new(arr),
                    cb: Box::new(CExpr::LambdaVal {
                        li: idx,
                        ret: CTy::Bool,
                    }),
                    extra: None,
                    ret: CTy::Array(Box::new(elem)),
                }
            }
            "reduce" => {
                expect_count(self, 2);
                let init = self.check_expr(&args[1], false);
                let uty = self.expr_ty(&init);
                if uty == CTy::Void || matches!(uty, CTy::Promise(_) | CTy::Resolver(_)) {
                    self.diag("reduce initial value must be a plain value", span);
                    return CExpr::Bool(false);
                }
                let (idx, _ret) = match &args[0] {
                    Expr::Lambda(l, _) => self.check_callback_lambda(
                        l,
                        vec![uty.clone(), elem.clone()],
                        Some(uty.clone()),
                        LambdaAbi::Reduce,
                    ),
                    _ => {
                        self.diag(
                            "reduce takes a callback lambda: (acc, item) => ... and an initial value",
                            span,
                        );
                        return CExpr::Bool(false);
                    }
                };
                CExpr::ArrayHof {
                    op: HofKind::Reduce,
                    arr: Box::new(arr),
                    cb: Box::new(CExpr::LambdaVal {
                        li: idx,
                        ret: uty.clone(),
                    }),
                    extra: Some(Box::new(init)),
                    ret: uty,
                }
            }
            other => {
                self.diag(
                    format!(
                        "'{}' is not supported on arrays (available: push, pop, shift, unshift, splice, slice, concat, join, reverse, sort, length, forEach, map, filter, reduce)",
                        other
                    ),
                    span,
                );
                CExpr::Bool(false)
            }
        }
    }
}
// ---------------------------------------------------------------------------
// Strict naming conventions
// ---------------------------------------------------------------------------

/// Kind of a chainable field-validator expression (walk down the chain).
fn validator_kind_of(c: &CExpr) -> FieldKind {
    match c {
        CExpr::HttpFieldNew { kind } => *kind,
        CExpr::HttpFieldOp { v, .. } => validator_kind_of(v),
        _ => FieldKind::Str,
    }
}

/// camelCase: starts lowercase, then letters/digits only (no underscores).
pub fn is_camel_case(s: &str) -> bool {
    let mut chars = s.chars();
    match chars.next() {
        Some(c) if c.is_ascii_lowercase() => {}
        _ => return false,
    }
    chars.all(|c| c.is_ascii_alphanumeric())
}

/// A function name from a merged wave registry package (`pkg__fn`): both the
/// package name and the function name must be valid camelCase identifiers.
fn is_namespaced_fn(s: &str) -> bool {
    match s.split_once("__") {
        Some((pkg, f)) => {
            !pkg.is_empty()
                && pkg
                    .chars()
                    .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_')
                && is_camel_case(f)
        }
        None => false,
    }
}

/// UPPER_SNAKE_CASE: uppercase letters, digits and underscores only, and it
/// must not start with a digit.
pub fn is_upper_snake_case(s: &str) -> bool {
    let mut chars = s.chars();
    match chars.next() {
        Some(c) if c.is_ascii_uppercase() || c == '_' => {}
        _ => return false,
    }
    s.chars().all(|c| c.is_ascii_uppercase() || c.is_ascii_digit() || c == '_')
}

/// PascalCase: starts uppercase, then letters/digits only (no underscores).
pub fn is_pascal_case(s: &str) -> bool {
    let mut chars = s.chars();
    match chars.next() {
        Some(c) if c.is_ascii_uppercase() => {}
        _ => return false,
    }
    chars.all(|c| c.is_ascii_alphanumeric())
}

/// kebab-case file stems: lowercase words separated by single hyphens.
pub fn is_kebab_case(s: &str) -> bool {
    if s.is_empty() {
        return false;
    }
    let mut prev_hyphen = true; // leading hyphen is invalid
    for c in s.chars() {
        match c {
            'a'..='z' | '0'..='9' => prev_hyphen = false,
            '-' => {
                if prev_hyphen {
                    return false;
                }
                prev_hyphen = true;
            }
            _ => return false,
        }
    }
    !prev_hyphen
}
