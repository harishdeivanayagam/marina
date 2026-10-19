//! Dump the parsed AST in the exact format the self-hosted (tsm) parser's
//! `mast.dumpProgram` produces, so the two front-ends can be diffed.
use marina::ast::*;
use marina::{checker, lexer, parser};

fn esc(s: &str) -> String {
    let mut out = String::new();
    for c in s.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '`' => out.push_str("\\`"),
            '$' => out.push_str("\\$"),
            c => out.push(c),
        }
    }
    out
}

fn fmt_float(f: f64) -> String {
    // must match marina_runtime's fmt_float exactly
    if f.is_finite() && f == f.trunc() && f.abs() < 1e15 {
        format!("{}", f as i64)
    } else {
        format!("{}", f)
    }
}

fn ty(a: &TyAnn) -> String {
    if a.args.is_empty() {
        a.name.clone()
    } else {
        format!("{}<{}>", a.name, ty(&a.args[0]))
    }
}

fn bin_op(op: &BinOp) -> &'static str {
    match op {
        BinOp::Add => "+",
        BinOp::Sub => "-",
        BinOp::Mul => "*",
        BinOp::Div => "/",
        BinOp::Mod => "%",
        BinOp::Lt => "<",
        BinOp::Gt => ">",
        BinOp::Le => "<=",
        BinOp::Ge => ">=",
        BinOp::Eq => "==",
        BinOp::Neq => "!=",
        BinOp::And => "&&",
        BinOp::Or => "||",
    }
}

fn un_op(op: &UnOp) -> &'static str {
    match op {
        UnOp::Neg => "-",
        UnOp::Not => "!",
    }
}

fn pad(d: usize) -> String {
    "  ".repeat(d)
}

fn parts_dump(parts: &[TPart], d: usize, out: &mut Vec<String>) {
    for p in parts {
        match p {
            TPart::Str(s) => out.push(format!("{}PSTR {}", pad(d), esc(s))),
            TPart::Expr(inner) => {
                out.push(format!("{}PEXPR", pad(d)));
                expr_dump(inner, d + 1, out);
            }
        }
    }
}

fn targs_dump(targs: &[TyAnn], d: usize, out: &mut Vec<String>) {
    for t in targs {
        out.push(format!("{}TARG {}", pad(d), ty(t)));
    }
}

fn exprs_dump(es: &[Expr], d: usize, out: &mut Vec<String>) {
    for e in es {
        expr_dump(e, d, out);
    }
}

fn lambda_dump(l: &Lambda, d: usize, out: &mut Vec<String>) {
    let pre = if l.is_async { "alambda" } else { "lambda" };
    for p in &l.params {
        if let Some(a) = &p.ann {
            out.push(format!("{}{}PARAM {} : {}", pad(d), pre, esc(&p.name), ty(a)));
        } else {
            out.push(format!("{}{}PARAM {}", pad(d), pre, esc(&p.name)));
        }
    }
    stmts_dump(&l.body, d, out);
}

fn expr_dump(e: &Expr, d: usize, out: &mut Vec<String>) {
    let p = pad(d);
    let at = format!(" @{}:{}", e.span().line, e.span().col);
    match e {
        Expr::Int(v, _) => out.push(format!("{}INT {}{}", p, v, at)),
        Expr::Float(v, _) => out.push(format!("{}FLOAT {}{}", p, fmt_float(*v), at)),
        Expr::Bool(v, _) => out.push(format!("{}BOOL {}{}", p, v, at)),
        Expr::Str(v, _) => out.push(format!("{}STR {}{}", p, esc(v), at)),
        Expr::Ident(v, _) => out.push(format!("{}IDENT {}{}", p, esc(v), at)),
        Expr::Unary(op, inner, _) => {
            out.push(format!("{}UNARY {}{}", p, un_op(op), at));
            expr_dump(inner, d + 1, out);
        }
        Expr::Binary(op, l, r, _) => {
            out.push(format!("{}BINARY {}{}", p, bin_op(op), at));
            expr_dump(l, d + 1, out);
            expr_dump(r, d + 1, out);
        }
        Expr::Template(parts, _) => {
            out.push(format!("{}TEMPLATE{}", p, at));
            parts_dump(parts, d + 1, out);
        }
        Expr::Tagged(tag, parts, _) => {
            out.push(format!("{}TAGGED{}", p, at));
            expr_dump(tag, d + 1, out);
            parts_dump(parts, d + 1, out);
        }
        Expr::Call(callee, args, _) => {
            out.push(format!("{}CALL{}", p, at));
            expr_dump(callee, d + 1, out);
            exprs_dump(args, d + 1, out);
        }
        Expr::CallGen(callee, targs, args, _) => {
            out.push(format!("{}CALLGEN{}", p, at));
            targs_dump(targs, d + 1, out);
            expr_dump(callee, d + 1, out);
            exprs_dump(args, d + 1, out);
        }
        Expr::Member(obj, name, _) => {
            out.push(format!("{}MEMBER {}{}", p, esc(name), at));
            expr_dump(obj, d + 1, out);
        }
        Expr::Index(obj, idx, _) => {
            out.push(format!("{}INDEX{}", p, at));
            expr_dump(obj, d + 1, out);
            expr_dump(idx, d + 1, out);
        }
        Expr::ArrayLit(elems, _) => {
            out.push(format!("{}ARRAYLIT{}", p, at));
            exprs_dump(elems, d + 1, out);
        }
        Expr::NewCtor { path, args, .. } => {
            out.push(format!("{}NEWCTOR {}{}", p, esc(&path.join(".")), at));
            exprs_dump(args, d + 1, out);
        }
        Expr::ObjectLit(fields, _) => {
            out.push(format!("{}OBJECTLIT{}", p, at));
            for (name, val) in fields {
                out.push(format!("{}FIELD {}", pad(d + 1), esc(name)));
                expr_dump(val, d + 2, out);
            }
        }
        Expr::NewClass { name, type_args, .. } => {
            out.push(format!("{}NEWCLASS {}{}", p, esc(name), at));
            targs_dump(type_args, d + 1, out);
        }
        Expr::Await(inner, _) => {
            out.push(format!("{}AWAIT{}", p, at));
            expr_dump(inner, d + 1, out);
        }
        Expr::NewPromise(inner, _) => {
            out.push(format!("{}NEWPROMISE{}", p, at));
            expr_dump(inner, d + 1, out);
        }
        Expr::Lambda(l, _) => {
            out.push(format!("{}LAMBDA{}", p, at));
            lambda_dump(l, d + 1, out);
        }
    }
}

fn stmt_dump(s: &Stmt, d: usize, out: &mut Vec<String>) {
    let p = pad(d);
    let at = format!(" @{}:{}", stmt_span(s).line, stmt_span(s).col);
    match s {
        Stmt::Let { name, ann, init, is_const, .. } => {
            let k = if *is_const { "CONST" } else { "LET" };
            match ann {
                Some(a) => out.push(format!("{}{} {} : {}{}", p, k, esc(name), ty(a), at)),
                None => out.push(format!("{}{} {}{}", p, k, esc(name), at)),
            }
            expr_dump(init, d + 1, out);
        }
        Stmt::Assign { name, value, .. } => {
            out.push(format!("{}ASSIGN {}{}", p, esc(name), at));
            expr_dump(value, d + 1, out);
        }
        Stmt::MemberAssign { obj, field, value, .. } => {
            out.push(format!("{}MSET {} {}{}", p, esc(obj), esc(field), at));
            expr_dump(value, d + 1, out);
        }
        Stmt::IndexAssign { name, index, value, .. } => {
            out.push(format!("{}ISET {}{}", p, esc(name), at));
            expr_dump(index, d + 1, out);
            expr_dump(value, d + 1, out);
        }
        Stmt::Return { value, .. } => {
            out.push(format!("{}RETURN{}", p, at));
            if let Some(v) = value {
                expr_dump(v, d + 1, out);
            }
        }
        Stmt::If { cond, then_body, else_body, .. } => {
            out.push(format!("{}IF{}", p, at));
            expr_dump(cond, d + 1, out);
            stmts_dump(then_body, d + 1, out);
            if let Some(els) = else_body {
                out.push(format!("{}ELSE", p));
                stmts_dump(els, d + 1, out);
            }
        }
        Stmt::While { cond, body, .. } => {
            out.push(format!("{}WHILE{}", p, at));
            expr_dump(cond, d + 1, out);
            stmts_dump(body, d + 1, out);
        }
        Stmt::For { init, cond, update, body, .. } => {
            out.push(format!("{}FOR{}", p, at));
            if let Some(i) = init {
                out.push(format!("{} INIT", p));
                stmt_dump(i, d + 2, out);
            }
            if let Some(c) = cond {
                out.push(format!("{} COND", p));
                expr_dump(c, d + 2, out);
            }
            if let Some(u) = update {
                out.push(format!("{} UPD", p));
                stmt_dump(u, d + 2, out);
            }
            out.push(format!("{} BODY", p));
            stmts_dump(body, d + 2, out);
        }
        Stmt::Expr(e) => {
            out.push(format!("{}EXPRSTMT{}", p, at));
            expr_dump(e, d + 1, out);
        }
    }
}

fn stmts_dump(ss: &[Stmt], d: usize, out: &mut Vec<String>) {
    for s in ss {
        stmt_dump(s, d, out);
    }
}

fn stmt_span(s: &Stmt) -> lexer::Span {
    match s {
        Stmt::Let { span, .. }
        | Stmt::Assign { span, .. }
        | Stmt::MemberAssign { span, .. }
        | Stmt::IndexAssign { span, .. }
        | Stmt::Return { span, .. }
        | Stmt::If { span, .. }
        | Stmt::While { span, .. }
        | Stmt::For { span, .. } => *span,
        Stmt::Expr(e) => e.span(),
    }
}

fn fn_dump(f: &FnDecl, d: usize, out: &mut Vec<String>) {
    let p = pad(d);
    let pre = if f.is_async { "AFN" } else { "FN" };
    let mut tp = String::new();
    for (i, t) in f.type_params.iter().enumerate() {
        if i == 0 {
            tp = "<".into();
        } else {
            tp = format!("{}, ", tp);
        }
        tp = format!("{}{}", tp, t);
    }
    if !f.type_params.is_empty() {
        tp = format!("{}>", tp);
    }
    let at = format!(" @{}:{}", f.span.line, f.span.col);
    match &f.ret_ann {
        Some(r) => out.push(format!("{}{} {}{} : {}{}", p, pre, esc(&f.name), tp, ty(r), at)),
        None => out.push(format!("{}{} {}{}{}", p, pre, esc(&f.name), tp, at)),
    }
    for prm in &f.params {
        match &prm.ann {
            Some(a) => out.push(format!("{}PARAM {} : {}", pad(d + 1), esc(&prm.name), ty(a))),
            None => out.push(format!("{}PARAM {}", pad(d + 1), esc(&prm.name))),
        }
    }
    stmts_dump(&f.body, d + 1, out);
}

fn main() {
    let path = std::env::args().nth(1).expect("usage: astdump <file.tsm>");
    let src = std::fs::read_to_string(&path).unwrap();
    let tokens = match lexer::lex(&src) {
        Ok(t) => t,
        Err(e) => {
            eprintln!("LEXERR {} @{}:{}", e.0, e.1.line, e.1.col);
            std::process::exit(1);
        }
    };
    let program = match parser::parse(tokens) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("PARSEERR {} @{}:{}", e.0, e.1.line, e.1.col);
            std::process::exit(1);
        }
    };
    // the tsm compiler resolves `use` at the pipeline level; the raw use
    // list is part of both dumps
    let mut out = Vec::new();
    for (name, sp) in &program.uses {
        out.push(format!("USE {} @{}:{}", esc(name), sp.line, sp.col));
    }
    for it in &program.interfaces {
        out.push(format!("IFACE {} @{}:{}", esc(&it.name), it.span.line, it.span.col));
        for f in &it.fields {
            out.push(format!(
                "  IFIELDFLD {} : {} @{}:{}",
                esc(&f.0),
                ty(&f.1),
                f.1.span.line,
                f.1.span.col
            ));
        }
    }
    for c in &program.classes {
        out.push(format!("CLASS {} @{}:{}", esc(&c.name), c.span.line, c.span.col));
        for t in &c.type_params {
            out.push(format!("  TYPEPARAM {}", esc(t)));
        }
        for (fname, ann) in &c.fields {
            out.push(format!(
                "  FIELDFLD {} : {} @{}:{}",
                esc(fname),
                ty(ann),
                ann.span.line,
                ann.span.col
            ));
        }
    }
    for f in &program.fns {
        fn_dump(f, 0, &mut out);
    }
    stmts_dump(&program.stmts, 0, &mut out);
    for line in out {
        println!("{}", line);
    }
}