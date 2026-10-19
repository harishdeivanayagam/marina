//! Recursive-descent parser for the marina language.

use crate::ast::*;
use crate::lexer::{lex_sub, Kw, P, Span, TPart as LTPart, Tok, Token};

pub struct ParseError(pub String, pub Span);

struct Parser {
    toks: Vec<Token>,
    pos: usize,
}

pub fn parse(tokens: Vec<Token>) -> Result<Program, ParseError> {
    let mut p = Parser { toks: tokens, pos: 0 };
    p.program()
}

impl Parser {
    fn peek(&self) -> &Tok {
        &self.toks[self.pos].tok
    }

    fn peek_at(&self, off: usize) -> &Tok {
        let i = (self.pos + off).min(self.toks.len() - 1);
        &self.toks[i].tok
    }

    fn span(&self) -> Span {
        self.toks[self.pos].span
    }

    fn bump(&mut self) -> Token {
        let t = self.toks[self.pos].clone();
        if self.pos < self.toks.len() - 1 {
            self.pos += 1;
        }
        t
    }

    fn error<T>(&self, msg: &str) -> Result<T, ParseError> {
        Err(ParseError(msg.to_string(), self.span()))
    }

    fn expect_punct(&mut self, p: P, what: &str) -> Result<Span, ParseError> {
        if *self.peek() == Tok::P(p) {
            Ok(self.bump().span)
        } else {
            self.error(&format!("expected '{}'", what))
        }
    }

    fn eat_punct(&mut self, p: P) -> bool {
        if *self.peek() == Tok::P(p) {
            self.bump();
            true
        } else {
            false
        }
    }

    fn expect_ident(&mut self) -> Result<(String, Span), ParseError> {
        match self.peek().clone() {
            Tok::Ident(n) => {
                let s = self.span();
                self.bump();
                Ok((n, s))
            }
            _ => self.error("expected an identifier"),
        }
    }

    // ------------------------------------------------------------------
    // Program
    // ------------------------------------------------------------------

    fn program(&mut self) -> Result<Program, ParseError> {
        let mut stmts = Vec::new();
        let mut fns = Vec::new();
        let mut classes = Vec::new();
        let mut interfaces = Vec::new();
        let mut uses = Vec::new();
        loop {
            match self.peek() {
                Tok::Eof => break,
                Tok::Kw(Kw::Function) | Tok::Kw(Kw::Async) => {
                    fns.push(self.fn_decl()?);
                }
                Tok::Kw(Kw::Class) => {
                    classes.push(self.class_decl()?);
                }
                Tok::Kw(Kw::Interface) => {
                    interfaces.push(self.interface_decl()?);
                }
                Tok::Kw(Kw::Use) => {
                    uses.push(self.use_decl()?);
                }
                Tok::Kw(Kw::Import) => {
                    return self.error("`import` statements are not supported — declare modules with `use <module>`");
                }
                _ => stmts.push(self.stmt()?),
            }
        }
        Ok(Program { stmts, fns, classes, interfaces, uses })
    }

    /// `use <module>` (optionally with a trailing `;`)
    fn use_decl(&mut self) -> Result<(String, Span), ParseError> {
        let span = self.span();
        self.bump(); // use
        let (name, _) = self.expect_ident()?;
        self.eat_punct(P::Semi);
        Ok((name, span))
    }

    /// `interface Name { field: Type, ... }` — a structural object shape.
    fn interface_decl(&mut self) -> Result<InterfaceDecl, ParseError> {
        let span = self.span();
        self.bump(); // interface
        let (name, _) = self.expect_ident()?;
        self.expect_punct(P::LBrace, "{")?;
        let mut fields = Vec::new();
        let mut seen = std::collections::HashSet::new();
        while *self.peek() != Tok::P(P::RBrace) {
            if *self.peek() == Tok::Eof {
                return self.error("expected '}' before end of file");
            }
            let (fname, _) = self.expect_ident()?;
            self.expect_punct(P::Colon, ":")?;
            let ann = self.ty_ann()?;
            if !seen.insert(fname.clone()) {
                return self.error("duplicate field in interface");
            }
            fields.push((fname, ann));
            if !self.eat_punct(P::Comma) && *self.peek() != Tok::P(P::RBrace) {
                self.expect_punct(P::Semi, ";")?;
            }
        }
        self.bump(); // }
        Ok(InterfaceDecl { name, fields, span })
    }

    /// `class Name { field: Type, ... }` — an optional `public` modifier is
    /// accepted before each field (all fields are public in marina).
    fn class_decl(&mut self) -> Result<ClassDecl, ParseError> {
        let span = self.span();
        self.bump(); // class
        let (name, _) = self.expect_ident()?;
        let type_params = self.type_param_list()?;
        self.expect_punct(P::LBrace, "{")?;
        let mut fields = Vec::new();
        while *self.peek() != Tok::P(P::RBrace) {
            if *self.peek() == Tok::Eof {
                return self.error("expected '}' before end of file");
            }
            // optional `public` modifier (accepted and ignored)
            if let Tok::Ident(w) = self.peek() {
                if w == "public" {
                    self.bump();
                }
            }
            let (fname, _) = self.expect_ident()?;
            self.expect_punct(P::Colon, ":")?;
            let ty = self.ty_ann()?;
            fields.push((fname, ty));
            self.eat_punct(P::Comma);
            self.eat_punct(P::Semi);
        }
        self.bump(); // }
        Ok(ClassDecl { name, type_params, fields, span })
    }

    fn fn_decl(&mut self) -> Result<FnDecl, ParseError> {
        let span = self.span();
        let is_async = if *self.peek() == Tok::Kw(Kw::Async) {
            self.bump();
            true
        } else {
            false
        };
        if *self.peek() != Tok::Kw(Kw::Function) {
            return self.error("expected 'function' after 'async'");
        }
        self.bump();
        let (name, _) = self.expect_ident()?;
        // generic type parameters: `function f<T, U>(...)`
        let type_params = self.type_param_list()?;
        self.expect_punct(P::LParen, "(")?;
        let mut params = Vec::new();
        if !self.eat_punct(P::RParen) {
            loop {
                let pspan = self.span();
                let (pname, _) = self.expect_ident()?;
                let ann = if *self.peek() == Tok::P(P::Colon) {
                    self.bump();
                    Some(self.ty_ann()?)
                } else {
                    None
                };
                params.push(Param { name: pname, ann, span: pspan });
                if !self.eat_punct(P::Comma) {
                    break;
                }
            }
            self.expect_punct(P::RParen, ")")?;
        }
        let ret_ann = if *self.peek() == Tok::P(P::Colon) {
            self.bump();
            Some(self.ty_ann()?)
        } else {
            None
        };
        let body = self.block()?;
        Ok(FnDecl { name, type_params, params, ret_ann, body, is_async, span })
    }

    /// `<T, U>` after a function/class name — empty when absent.
    /// Speculatively parse `<T, U>` in expression position. Returns Some(type
    /// args) only if the full `<...>(` pattern is present; otherwise the
    /// lexer position is restored so `<` can still be a comparison.
    fn try_call_type_args(&mut self) -> Option<Vec<TyAnn>> {
        let save = self.pos;
        self.bump(); // <
        let mut tas = Vec::new();
        loop {
            match self.ty_ann() {
                Ok(a) => tas.push(a),
                Err(_) => {
                    self.pos = save;
                    return None;
                }
            }
            if !self.eat_punct(P::Comma) {
                break;
            }
        }
        if self.expect_punct(P::Gt, ">").is_err() || *self.peek() != Tok::P(P::LParen) {
            self.pos = save;
            return None;
        }
        Some(tas)
    }

    fn type_param_list(&mut self) -> Result<Vec<String>, ParseError> {
        if *self.peek() != Tok::P(P::Lt) {
            return Ok(Vec::new());
        }
        self.bump();
        let mut ps = Vec::new();
        loop {
            let (p, _) = self.expect_ident()?;
            ps.push(p);
            if !self.eat_punct(P::Comma) {
                break;
            }
        }
        self.expect_punct(P::Gt, ">")?;
        Ok(ps)
    }

    fn ty_ann(&mut self) -> Result<TyAnn, ParseError> {
        let span = self.span();
        let (name, _) = self.expect_ident()?;
        // dotted type paths: `http.Request`, `http.Response`
        let mut full = name;
        while *self.peek() == Tok::P(P::Dot) {
            self.bump();
            let (seg, _) = self.expect_ident()?;
            full = format!("{}.{}", full, seg);
        }
        let args = if *self.peek() == Tok::P(P::Lt) {
            self.bump();
            let mut args = Vec::new();
            loop {
                args.push(self.ty_ann()?);
                if !self.eat_punct(P::Comma) {
                    break;
                }
            }
            self.expect_punct(P::Gt, ">")?;
            args
        } else {
            Vec::new()
        };
        Ok(TyAnn { name: full, args, span })
    }

    fn block(&mut self) -> Result<Vec<Stmt>, ParseError> {
        self.expect_punct(P::LBrace, "{")?;
        let mut stmts = Vec::new();
        while *self.peek() != Tok::P(P::RBrace) {
            if *self.peek() == Tok::Eof {
                return self.error("expected '}' before end of file");
            }
            stmts.push(self.stmt()?);
        }
        self.bump(); // }
        Ok(stmts)
    }

    fn stmt(&mut self) -> Result<Stmt, ParseError> {
        match self.peek().clone() {
            Tok::Kw(Kw::Let) | Tok::Kw(Kw::Const) => self.let_stmt(),
            Tok::Kw(Kw::Return) => {
                let span = self.span();
                self.bump();
                let value = match self.peek() {
                    Tok::P(P::Semi) | Tok::P(P::RBrace) | Tok::Eof => None,
                    _ => Some(self.expr()?),
                };
                self.eat_punct(P::Semi);
                Ok(Stmt::Return { value, span })
            }
            Tok::Kw(Kw::If) => self.if_stmt(),
            Tok::Kw(Kw::For) => self.for_stmt(),
            Tok::Kw(Kw::While) => {
                let span = self.span();
                self.bump();
                let cond = self.expr()?;
                let body = self.block()?;
                self.eat_punct(P::Semi);
                Ok(Stmt::While { cond, body, span })
            }
            Tok::Kw(Kw::Function) | Tok::Kw(Kw::Async) => {
                self.error("functions must be declared at the top level of the file")
            }
            _ => {
                // `list[i] = value` — lookahead for the `=` after the `]`
                if let Tok::Ident(_) = self.peek() {
                    if let Tok::P(P::LBracket) = self.peek_at(1) {
                        if let Some(end) = self.matching_bracket(1) {
                            if *self.peek_at(end + 1) == Tok::P(P::Assign) {
                                let span = self.span();
                                let (name, _) = self.expect_ident()?;
                                self.bump(); // [
                                let index = self.expr()?;
                                self.expect_punct(P::RBracket, "]")?;
                                self.bump(); // =
                                let value = self.expr()?;
                                self.eat_punct(P::Semi);
                                return Ok(Stmt::IndexAssign { name, index, value, span });
                            }
                        }
                    }
                    // `obj.field = value` and compound forms (`obj.f += x`,
                    // `obj.f++`) — lookahead for `ident . ident <assign-op>`
                    if let Tok::P(P::Dot) = self.peek_at(1) {
                        if let (Tok::Ident(_), Tok::Ident(_)) =
                            (self.peek(), self.peek_at(2))
                        {
                            let op_off = 3;
                            let compound = match self.peek_at(op_off) {
                                Tok::P(P::Assign) => Some(None),
                                Tok::P(P::PlusAssign) => Some(Some(BinOp::Add)),
                                Tok::P(P::MinusAssign) => Some(Some(BinOp::Sub)),
                                Tok::P(P::PlusPlus) => Some(Some(BinOp::Add)),
                                Tok::P(P::MinusMinus) => Some(Some(BinOp::Sub)),
                                _ => None,
                            };
                            if let Some(delta) = compound {
                                let span = self.span();
                                let (obj, _) = self.expect_ident()?;
                                self.bump(); // .
                                let (field, _) = self.expect_ident()?;
                                let fexpr =
                                    Expr::Member(Box::new(Expr::Ident(obj.clone(), span)), field.clone(), span);
                                let value = match delta {
                                    None => {
                                        self.bump(); // =
                                        self.expr()?
                                    }
                                    Some(op) => {
                                        self.bump(); // += -= ++ --
                                        let rhs = match self.peek_at(0) {
                                            // ++ / -- need no operand
                                            Tok::P(P::Semi)
                                            | Tok::P(P::RBrace)
                                            | Tok::Eof => Expr::Int(1, span),
                                            _ => self.expr()?,
                                        };
                                        Expr::Binary(
                                            op,
                                            Box::new(fexpr),
                                            Box::new(rhs),
                                            span,
                                        )
                                    }
                                };
                                self.eat_punct(P::Semi);
                                return Ok(Stmt::MemberAssign { obj, field, value, span });
                            }
                        }
                    }
                }
                // assignment, increment/decrement, or expression statement
                self.simple_stmt()
            }
        }
    }

    /// Given that self.pos + off is a `[`, find the offset of its matching
    /// `]`, counting nesting. Returns None if unbalanced before EOF.
    fn matching_bracket(&self, off: usize) -> Option<usize> {
        let mut depth = 0i32;
        let mut i = self.pos + off;
        while i < self.toks.len() {
            match &self.toks[i].tok {
                Tok::P(P::LBracket) => depth += 1,
                Tok::P(P::RBracket) => {
                    depth -= 1;
                    if depth == 0 {
                        return Some(i - self.pos);
                    }
                }
                Tok::Eof => return None,
                _ => {}
            }
            i += 1;
        }
        None
    }

    /// Parses a "simple" statement: an assignment (`a = e`), a compound
    /// assignment (`a += e`, `a -= e`), an increment/decrement
    /// (`a++`, `a--`), or a plain expression. Used both for regular
    /// statements and for `for` init/update slots.
    fn simple_stmt(&mut self) -> Result<Stmt, ParseError> {
        if let Tok::Ident(name) = self.peek().clone() {
            let name_span = self.span();
            match self.peek_at(1) {
                Tok::P(P::Assign) => {
                    self.bump();
                    self.bump(); // =
                    let value = self.expr()?;
                    self.eat_punct(P::Semi);
                    return Ok(Stmt::Assign { name, value, span: name_span });
                }
                Tok::P(P::PlusAssign) | Tok::P(P::MinusAssign) => {
                    let op = if *self.peek_at(1) == Tok::P(P::PlusAssign) { BinOp::Add } else { BinOp::Sub };
                    self.bump();
                    self.bump(); // += or -=
                    let rhs = self.expr()?;
                    self.eat_punct(P::Semi);
                    let value = Expr::Binary(
                        op,
                        Box::new(Expr::Ident(name.clone(), name_span)),
                        Box::new(rhs),
                        name_span,
                    );
                    return Ok(Stmt::Assign { name, value, span: name_span });
                }
                Tok::P(P::PlusPlus) | Tok::P(P::MinusMinus) => {
                    let op = if *self.peek_at(1) == Tok::P(P::PlusPlus) { BinOp::Add } else { BinOp::Sub };
                    self.bump();
                    self.bump(); // ++ or --
                    self.eat_punct(P::Semi);
                    let value = Expr::Binary(
                        op,
                        Box::new(Expr::Ident(name.clone(), name_span)),
                        Box::new(Expr::Int(1, name_span)),
                        name_span,
                    );
                    return Ok(Stmt::Assign { name, value, span: name_span });
                }
                _ => {}
            }
        }
        let e = self.expr()?;
        self.eat_punct(P::Semi);
        Ok(Stmt::Expr(e))
    }

    fn for_stmt(&mut self) -> Result<Stmt, ParseError> {
        let span = self.span();
        self.bump(); // for
        self.expect_punct(P::LParen, "(")?;
        let init = match self.peek() {
            Tok::P(P::Semi) => {
                self.bump();
                None
            }
            Tok::Kw(Kw::Let) | Tok::Kw(Kw::Const) => Some(Box::new(self.let_stmt()?)),
            _ => Some(Box::new(self.simple_stmt()?)),
        };
        let cond = match self.peek() {
            Tok::P(P::Semi) => None,
            _ => Some(self.expr()?),
        };
        self.expect_punct(P::Semi, ";")?;
        let update = match self.peek() {
            Tok::P(P::RParen) => None,
            _ => Some(Box::new(self.simple_stmt()?)),
        };
        self.expect_punct(P::RParen, ")")?;
        let body = self.block()?;
        self.eat_punct(P::Semi);
        Ok(Stmt::For { init, cond, update, body, span })
    }

    fn let_stmt(&mut self) -> Result<Stmt, ParseError> {
        let span = self.span();
        let is_const = *self.peek() == Tok::Kw(Kw::Const);
        self.bump();
        let (name, _) = self.expect_ident()?;
        let ann = if *self.peek() == Tok::P(P::Colon) {
            self.bump();
            Some(self.ty_ann()?)
        } else {
            None
        };
        if !self.eat_punct(P::Assign) {
            return self.error("declarations must be initialized (marina has no 'undefined')");
        }
        let init = self.expr()?;
        self.eat_punct(P::Semi);
        Ok(Stmt::Let { name, ann, init, is_const, span })
    }

    fn if_stmt(&mut self) -> Result<Stmt, ParseError> {
        let span = self.span();
        self.bump(); // if
        let cond = self.expr()?;
        let then_body = self.block()?;
        let else_body = if *self.peek() == Tok::Kw(Kw::Else) {
            self.bump();
            if *self.peek() == Tok::Kw(Kw::If) {
                Some(vec![self.if_stmt()?])
            } else {
                Some(self.block()?)
            }
        } else {
            None
        };
        self.eat_punct(P::Semi);
        Ok(Stmt::If { cond, then_body, else_body, span })
    }

    // ------------------------------------------------------------------
    // Expressions (precedence climbing)
    // ------------------------------------------------------------------

    fn expr(&mut self) -> Result<Expr, ParseError> {
        self.or_expr()
    }

    fn or_expr(&mut self) -> Result<Expr, ParseError> {
        let mut l = self.and_expr()?;
        while *self.peek() == Tok::P(P::OrOr) {
            let span = self.span();
            self.bump();
            let r = self.and_expr()?;
            l = Expr::Binary(BinOp::Or, Box::new(l), Box::new(r), span);
        }
        Ok(l)
    }

    fn and_expr(&mut self) -> Result<Expr, ParseError> {
        let mut l = self.eq_expr()?;
        while *self.peek() == Tok::P(P::AndAnd) {
            let span = self.span();
            self.bump();
            let r = self.eq_expr()?;
            l = Expr::Binary(BinOp::And, Box::new(l), Box::new(r), span);
        }
        Ok(l)
    }

    fn eq_expr(&mut self) -> Result<Expr, ParseError> {
        let mut l = self.rel_expr()?;
        loop {
            let op = match self.peek() {
                Tok::P(P::Eq) => BinOp::Eq,
                Tok::P(P::Neq) => BinOp::Neq,
                _ => break,
            };
            let span = self.span();
            self.bump();
            let r = self.rel_expr()?;
            l = Expr::Binary(op, Box::new(l), Box::new(r), span);
        }
        Ok(l)
    }

    fn rel_expr(&mut self) -> Result<Expr, ParseError> {
        let mut l = self.add_expr()?;
        loop {
            let op = match self.peek() {
                Tok::P(P::Lt) => BinOp::Lt,
                Tok::P(P::Gt) => BinOp::Gt,
                Tok::P(P::Le) => BinOp::Le,
                Tok::P(P::Ge) => BinOp::Ge,
                _ => break,
            };
            let span = self.span();
            self.bump();
            let r = self.add_expr()?;
            l = Expr::Binary(op, Box::new(l), Box::new(r), span);
        }
        Ok(l)
    }

    fn add_expr(&mut self) -> Result<Expr, ParseError> {
        let mut l = self.mul_expr()?;
        loop {
            let op = match self.peek() {
                Tok::P(P::Plus) => BinOp::Add,
                Tok::P(P::Minus) => BinOp::Sub,
                _ => break,
            };
            let span = self.span();
            self.bump();
            let r = self.mul_expr()?;
            l = Expr::Binary(op, Box::new(l), Box::new(r), span);
        }
        Ok(l)
    }

    fn mul_expr(&mut self) -> Result<Expr, ParseError> {
        let mut l = self.unary_expr()?;
        loop {
            let op = match self.peek() {
                Tok::P(P::Star) => BinOp::Mul,
                Tok::P(P::Slash) => BinOp::Div,
                Tok::P(P::Percent) => BinOp::Mod,
                _ => break,
            };
            let span = self.span();
            self.bump();
            let r = self.unary_expr()?;
            l = Expr::Binary(op, Box::new(l), Box::new(r), span);
        }
        Ok(l)
    }

    fn unary_expr(&mut self) -> Result<Expr, ParseError> {
        let span = self.span();
        match self.peek() {
            Tok::Kw(Kw::Await) => {
                self.bump();
                let inner = self.unary_expr()?;
                Ok(Expr::Await(Box::new(inner), span))
            }
            Tok::P(P::Minus) => {
                self.bump();
                let inner = self.unary_expr()?;
                Ok(Expr::Unary(UnOp::Neg, Box::new(inner), span))
            }
            Tok::P(P::Bang) => {
                self.bump();
                let inner = self.unary_expr()?;
                Ok(Expr::Unary(UnOp::Not, Box::new(inner), span))
            }
            _ => self.postfix_expr(),
        }
    }

    fn postfix_expr(&mut self) -> Result<Expr, ParseError> {
        let mut e = self.primary()?;
        loop {
            match self.peek() {
                Tok::P(P::Dot) => {
                    self.bump();
                    let (name, _) = self.expect_ident()?;
                    e = Expr::Member(Box::new(e), name, self.span());
                }
                Tok::P(P::LParen) => {
                    self.bump();
                    let mut args = Vec::new();
                    if !self.eat_punct(P::RParen) {
                        loop {
                            args.push(self.expr()?);
                            if !self.eat_punct(P::Comma) {
                                break;
                            }
                        }
                        self.expect_punct(P::RParen, ")")?;
                    }
                    e = Expr::Call(Box::new(e), args, self.span());
                }
                Tok::P(P::Lt) => {
                    // speculative parse of explicit type arguments followed
                    // by a call: `pick<Int32>(...)`. Restored (treated as a
                    // comparison) unless the pattern `<...>(` completes.
                    let save = self.pos;
                    let tas = self.try_call_type_args();
                    match tas {
                        Some(type_args) => {
                            self.bump(); // (
                            let mut args = Vec::new();
                            if !self.eat_punct(P::RParen) {
                                loop {
                                    args.push(self.expr()?);
                                    if !self.eat_punct(P::Comma) {
                                        break;
                                    }
                                }
                                self.expect_punct(P::RParen, ")")?;
                            }
                            e = Expr::CallGen(Box::new(e), type_args, args, self.span());
                        }
                        None => {
                            self.pos = save;
                            break;
                        }
                    }
                }
                Tok::P(P::LBracket) => {
                    self.bump();
                    let idx = self.expr()?;
                    self.expect_punct(P::RBracket, "]")?;
                    e = Expr::Index(Box::new(e), Box::new(idx), self.span());
                }
                Tok::Template(_) => {
                    // tagged template: `sql.stmt\`...\``
                    let (parts, _) = self.parse_template_token()?;
                    e = Expr::Tagged(Box::new(e), parts, self.span());
                }
                _ => break,
            }
        }
        Ok(e)
    }

    /// Consume a peeked `Tok::Template(parts)` and convert its lexer parts
    /// into AST template parts (re-lexing interpolated expressions).
    fn parse_template_token(&mut self) -> Result<(Vec<TPart>, Span), ParseError> {
        let span = self.span();
        let Tok::Template(parts) = self.peek() else {
            return Err(ParseError("expected a template literal".into(), span));
        };
        let parts = parts.clone();
        self.bump();
        let mut out = Vec::new();
        for p in parts {
            match p {
                LTPart::Str(s) => out.push(TPart::Str(s)),
                LTPart::Expr(raw, espan) => {
                    let sub = lex_sub(&raw, espan).map_err(|e| ParseError(e.0, e.1))?;
                    let mut sp = Parser { toks: sub, pos: 0 };
                    let e = sp.expr()?;
                    if *sp.peek() != Tok::Eof {
                        return Err(ParseError(
                            "unexpected tokens in template expression".into(),
                            espan,
                        ));
                    }
                    out.push(TPart::Expr(Box::new(e)));
                }
            }
        }
        Ok((out, span))
    }

    /// Is the '(' at self.pos the start of a lambda parameter list?
    fn is_lambda_ahead(&self) -> bool {
        let mut i = self.pos;
        let mut depth = 0i32;
        while i < self.toks.len() {
            match &self.toks[i].tok {
                Tok::P(P::LParen) => depth += 1,
                Tok::P(P::RParen) => {
                    depth -= 1;
                    if depth == 0 {
                        return matches!(self.toks[i + 1].tok, Tok::P(P::Arrow));
                    }
                }
                Tok::Eof => return false,
                _ => {}
            }
            i += 1;
        }
        false
    }

    fn primary(&mut self) -> Result<Expr, ParseError> {
        let span = self.span();
        match self.peek().clone() {
            Tok::Int(v) => {
                self.bump();
                Ok(Expr::Int(v, span))
            }
            Tok::Float(v) => {
                self.bump();
                Ok(Expr::Float(v, span))
            }
            Tok::Str(s) => {
                self.bump();
                Ok(Expr::Str(s, span))
            }
            Tok::Template(parts) => {
                self.bump();
                let mut out = Vec::new();
                for p in parts {
                    match p {
                        LTPart::Str(s) => out.push(TPart::Str(s)),
                        LTPart::Expr(raw, espan) => {
                            let sub = lex_sub(&raw, espan)
                                .map_err(|e| ParseError(e.0, e.1))?;
                            let mut sp = Parser { toks: sub, pos: 0 };
                            let e = sp.expr()?;
                            if *sp.peek() != Tok::Eof {
                                return Err(ParseError(
                                    "unexpected tokens in template expression".into(),
                                    espan,
                                ));
                            }
                            out.push(TPart::Expr(Box::new(e)));
                        }
                    }
                }
                Ok(Expr::Template(out, span))
            }
            Tok::Kw(Kw::True) => {
                self.bump();
                Ok(Expr::Bool(true, span))
            }
            Tok::Kw(Kw::False) => {
                self.bump();
                Ok(Expr::Bool(false, span))
            }
            Tok::Kw(Kw::New) => {
                self.bump();
                let (name, _) = self.expect_ident()?;
                if name == "Promise" && *self.peek() == Tok::P(P::LParen) {
                    self.expect_punct(P::LParen, "(")?;
                    let l = self.expr()?;
                    self.expect_punct(P::RParen, ")")?;
                    Ok(Expr::NewPromise(Box::new(l), span))
                } else {
                    // dotted constructor path: `new http.Server()` etc.
                    let mut path = vec![name];
                    while *self.peek() == Tok::P(P::Dot) {
                        self.bump();
                        let (seg, _) = self.expect_ident()?;
                        path.push(seg);
                    }
                    // optional explicit type arguments: `new Box<Int32>()`
                    // (parsed before the argument parens)
                    let type_args = if path.len() == 1 && *self.peek() == Tok::P(P::Lt) {
                        self.bump();
                        let mut tas = Vec::new();
                        loop {
                            tas.push(self.ty_ann()?);
                            if !self.eat_punct(P::Comma) {
                                break;
                            }
                        }
                        self.expect_punct(P::Gt, ">")?;
                        tas
                    } else {
                        Vec::new()
                    };
                    // constructor arguments
                    let mut args = Vec::new();
                    if *self.peek() == Tok::P(P::LParen) {
                        self.bump();
                        if !self.eat_punct(P::RParen) {
                            loop {
                                args.push(self.expr()?);
                                if !self.eat_punct(P::Comma) {
                                    break;
                                }
                            }
                            self.expect_punct(P::RParen, ")")?;
                        }
                    }
                    if path.len() == 1 {
                        if args.is_empty() {
                            // `new ClassName()` — no constructor args in marina
                            return Ok(Expr::NewClass { name: path.remove(0), type_args, span });
                        }
                        return self
                            .error("classes take no constructor arguments (use `new ClassName()`)");
                    }
                    Ok(Expr::NewCtor { path, args, span })
                }
            }
            Tok::P(P::LBracket) => {
                self.bump();
                let mut elems = Vec::new();
                if !self.eat_punct(P::RBracket) {
                    loop {
                        elems.push(self.expr()?);
                        if !self.eat_punct(P::Comma) {
                            break;
                        }
                        // allow a trailing comma
                        if self.eat_punct(P::RBracket) {
                            return Ok(Expr::ArrayLit(elems, span));
                        }
                    }
                    self.expect_punct(P::RBracket, "]")?;
                }
                Ok(Expr::ArrayLit(elems, span))
            }
            Tok::P(P::LBrace) => {
                // object literal: { field: expr, ... } (trailing comma ok)
                self.bump();
                let mut fields = Vec::new();
                if !self.eat_punct(P::RBrace) {
                    loop {
                        let (fname, _) = self.expect_ident()?;
                        self.expect_punct(P::Colon, ":")?;
                        let value = self.expr()?;
                        fields.push((fname, value));
                        if !self.eat_punct(P::Comma) {
                            break;
                        }
                        if self.eat_punct(P::RBrace) {
                            return Ok(Expr::ObjectLit(fields, span));
                        }
                    }
                    self.expect_punct(P::RBrace, "}")?;
                }
                Ok(Expr::ObjectLit(fields, span))
            }
            Tok::Ident(n) => {
                self.bump();
                Ok(Expr::Ident(n, span))
            }
            Tok::Kw(Kw::Async) => {
                // async lambda: `async (params) => { ... }`
                self.bump();
                if !self.is_lambda_ahead() {
                    return self.error("expected (params) => { ... } after `async`");
                }
                self.lambda(true)
            }
            Tok::P(P::LParen) => {
                if self.is_lambda_ahead() {
                    self.lambda(false)
                } else {
                    self.bump();
                    let e = self.expr()?;
                    self.expect_punct(P::RParen, ")")?;
                    Ok(e)
                }
            }
            _ => self.error("expected an expression"),
        }
    }

    fn lambda(&mut self, is_async: bool) -> Result<Expr, ParseError> {
        let span = self.span();
        self.expect_punct(P::LParen, "(")?;
        let mut params = Vec::new();
        if !self.eat_punct(P::RParen) {
            loop {
                let pspan = self.span();
                let (pname, _) = self.expect_ident()?;
                let ann = if *self.peek() == Tok::P(P::Colon) {
                    self.bump();
                    Some(self.ty_ann()?)
                } else {
                    None
                };
                params.push(Param { name: pname, ann, span: pspan });
                if !self.eat_punct(P::Comma) {
                    break;
                }
            }
            self.expect_punct(P::RParen, ")")?;
        }
        self.expect_punct(P::Arrow, "=>")?;
        if *self.peek() != Tok::P(P::LBrace) {
            return self.error("lambda bodies must be a { block } in marina");
        }
        let body = self.block()?;
        Ok(Expr::Lambda(Lambda { params, body, is_async, span }, span))
    }
}