//! AST for the marina language.

use crate::lexer::Span;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum BinOp {
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

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum UnOp {
    Neg,
    Not,
}

pub enum TPart {
    Str(String),
    Expr(Box<Expr>),
}

pub struct Lambda {
    pub params: Vec<Param>,
    pub body: Vec<Stmt>,
    pub span: Span,
}

pub enum Expr {
    Int(i32, Span),
    Float(f64, Span),
    Bool(bool, Span),
    Str(String, Span),
    Template(Vec<TPart>, Span),
    Ident(String, Span),
    Unary(UnOp, Box<Expr>, Span),
    Binary(BinOp, Box<Expr>, Box<Expr>, Span),
    Call(Box<Expr>, Vec<Expr>, Span),
    Member(Box<Expr>, String, Span),
    /// `obj[idx]`
    Index(Box<Expr>, Box<Expr>, Span),
    /// `[e1, e2, ...]`
    ArrayLit(Vec<Expr>, Span),
    /// `new a.b.Class(args...)` — module-qualified constructor call
    /// (`new http.Server()`, `new http.JsonValidator({...})`).
    /// Plain `new Name()` stays `NewClass` (classes take no ctor args).
    NewCtor {
        path: Vec<String>,
        args: Vec<Expr>,
        span: Span,
    },
    /// `{ field: expr, ... }` — creates an instance of an inferred
    /// ("ghost") class, or of an annotated class when a target exists.
    ObjectLit(Vec<(String, Expr)>, Span),
    /// `new SomeClass()` — instantiates a declared class.
    NewClass(String, Span),
    Await(Box<Expr>, Span),
    NewPromise(Box<Expr>, Span),
    Lambda(Lambda, Span),
}

impl Expr {
    pub fn span(&self) -> Span {
        match self {
            Expr::Int(_, s)
            | Expr::Float(_, s)
            | Expr::Bool(_, s)
            | Expr::Str(_, s)
            | Expr::Template(_, s)
            | Expr::Ident(_, s)
            | Expr::Unary(_, _, s)
            | Expr::Binary(_, _, _, s)
            | Expr::Call(_, _, s)
            | Expr::Member(_, _, s)
            | Expr::Index(_, _, s)
            | Expr::ArrayLit(_, s)
            | Expr::ObjectLit(_, s)
            | Expr::NewClass(_, s)
            | Expr::NewCtor { span: s, .. }
            | Expr::Await(_, s)
            | Expr::NewPromise(_, s)
            | Expr::Lambda(_, s) => *s,
        }
    }
}

pub struct Param {
    pub name: String,
    pub ann: Option<TyAnn>,
    pub span: Span,
}

pub struct TyAnn {
    pub name: String,
    pub inner: Option<Box<TyAnn>>,
    pub span: Span,
}

pub enum Stmt {
    Let {
        name: String,
        ann: Option<TyAnn>,
        init: Expr,
        is_const: bool,
        span: Span,
    },
    Assign {
        name: String,
        value: Expr,
        span: Span,
    },
    /// `obj.field = value`
    MemberAssign {
        obj: String,
        field: String,
        value: Expr,
        span: Span,
    },
    /// `list[i] = value`
    IndexAssign {
        name: String,
        index: Expr,
        value: Expr,
        span: Span,
    },
    Return {
        value: Option<Expr>,
        span: Span,
    },
    If {
        cond: Expr,
        then_body: Vec<Stmt>,
        else_body: Option<Vec<Stmt>>,
        span: Span,
    },
    While {
        cond: Expr,
        body: Vec<Stmt>,
        span: Span,
    },
    For {
        init: Option<Box<Stmt>>,
        cond: Option<Expr>,
        update: Option<Box<Stmt>>,
        body: Vec<Stmt>,
        span: Span,
    },
    Expr(Expr),
}

pub struct FnDecl {
    pub name: String,
    pub params: Vec<Param>,
    pub ret_ann: Option<TyAnn>,
    pub body: Vec<Stmt>,
    pub is_async: bool,
    pub span: Span,
}

/// `class Name { field: Type, ... }` — fields are public by default.
pub struct ClassDecl {
    pub name: String,
    pub fields: Vec<(String, TyAnn)>,
    pub span: Span,
}

pub struct Program {
    pub stmts: Vec<Stmt>,
    pub fns: Vec<FnDecl>,
    pub classes: Vec<ClassDecl>,
    /// Top-level `use <module>;`
    /// declarations: std modules (net, http) or wave registry packages.
    pub uses: Vec<(String, Span)>,
}