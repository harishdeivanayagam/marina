// Recursive-descent parser implementation — direct port of parser.rs.
#include <marina/parser.hpp>

#include <set>
#include <stdexcept>

namespace marina::ast {

namespace {

using marina::lex::Kw;
using marina::lex::LExpr;
using marina::lex::LStr;
using marina::lex::LTPart;
using marina::lex::P;
using marina::lex::Span;
using marina::lex::TEof;
using marina::lex::TFloat;
using marina::lex::TIdent;
using marina::lex::TInt;
using marina::lex::TKw;
using marina::lex::TP;
using marina::lex::TStr;
using marina::lex::TTemplate;
using marina::lex::Token;
using marina::lex::TokV;

ExprPtr mk(Expr e) { return std::make_shared<Expr>(std::move(e)); }

class Parser {
public:
    Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    Program program() { return program_impl(); }

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;

    const TokV& peek() const { return toks_[pos_].tok; }

    const TokV& peek_at(size_t off) const {
        size_t i = pos_ + off;
        if (i >= toks_.size()) i = toks_.size() - 1;
        return toks_[i].tok;
    }

    Span span() const { return toks_[pos_].span; }

    Token bump() {
        Token t = toks_[pos_];
        if (pos_ < toks_.size() - 1) pos_ += 1;
        return t;
    }

    template <typename T>
    T error(const std::string& msg) const {
        throw ParseError{msg, span()};
    }

    Span expect_punct(P p, const char* what) {
        if (is_p(peek(), p)) return bump().span;
        return error<Span>(std::string("expected '") + what + "'");
    }

    bool eat_punct(P p) {
        if (is_p(peek(), p)) {
            bump();
            return true;
        }
        return false;
    }

    std::pair<std::string, Span> expect_ident() {
        if (auto* n = std::get_if<TIdent>(&peek())) {
            Span s = span();
            bump();
            return {n->name, s};
        }
        return error<std::pair<std::string, Span>>("expected an identifier");
    }

    // ------------------------------------------------------------------
    // Program
    // ------------------------------------------------------------------

    Program program_impl() {
        Program prog;
        for (;;) {
            if (is_eof(peek())) break;
            if (is_kw(peek(), Kw::Function) || is_kw(peek(), Kw::Async)) {
                prog.fns.push_back(fn_decl());
            } else if (is_kw(peek(), Kw::Class)) {
                prog.classes.push_back(class_decl());
            } else if (is_kw(peek(), Kw::Interface)) {
                prog.interfaces.push_back(interface_decl());
            } else if (is_kw(peek(), Kw::Use)) {
                prog.uses.push_back(use_decl());
            } else if (is_kw(peek(), Kw::Import)) {
                return error<Program>(
                    "`import` statements are not supported — declare modules with `use <module>`");
            } else {
                prog.stmts.push_back(stmt());
            }
        }
        return prog;
    }

    // `use <module>` (optionally with a trailing `;`)
    std::pair<std::string, Span> use_decl() {
        Span sp = span();
        bump(); // use
        auto name = expect_ident();
        eat_punct(P::Semi);
        return {name.first, sp};
    }

    // `interface Name { field: Type, ... }` — a structural object shape.
    InterfaceDecl interface_decl() {
        Span sp = span();
        bump(); // interface
        auto name = expect_ident();
        expect_punct(P::LBrace, "{");
        std::vector<std::pair<std::string, TyAnn>> fields;
        std::set<std::string> seen;
        while (!is_p(peek(), P::RBrace)) {
            if (is_eof(peek()))
                error<int>("expected '}' before end of file");
            auto fname = expect_ident();
            expect_punct(P::Colon, ":");
            TyAnn ann = ty_ann();
            if (!seen.insert(fname.first).second)
                error<int>("duplicate field in interface");
            fields.emplace_back(fname.first, std::move(ann));
            if (!eat_punct(P::Comma) && !is_p(peek(), P::RBrace)) {
                expect_punct(P::Semi, ";");
            }
        }
        bump(); // }
        InterfaceDecl d;
        d.name = name.first;
        d.fields = std::move(fields);
        d.span = sp;
        return d;
    }

    // `class Name { field: Type, ... }` — an optional `public` modifier is
    // accepted before each field (all fields are public in marina).
    ClassDecl class_decl() {
        Span sp = span();
        bump(); // class
        auto name = expect_ident();
        auto type_params = type_param_list();
        expect_punct(P::LBrace, "{");
        std::vector<std::pair<std::string, TyAnn>> fields;
        while (!is_p(peek(), P::RBrace)) {
            if (is_eof(peek()))
                error<int>("expected '}' before end of file");
            // optional `public` modifier (accepted and ignored)
            if (auto* w = std::get_if<TIdent>(&peek())) {
                if (w->name == "public") bump();
            }
            auto fname = expect_ident();
            expect_punct(P::Colon, ":");
            TyAnn ty = ty_ann();
            fields.emplace_back(fname.first, std::move(ty));
            eat_punct(P::Comma);
            eat_punct(P::Semi);
        }
        bump(); // }
        ClassDecl d;
        d.name = name.first;
        d.type_params = std::move(type_params);
        d.fields = std::move(fields);
        d.span = sp;
        return d;
    }

    FnDecl fn_decl() {
        FnDecl f;
        f.span = span();
        if (is_kw(peek(), Kw::Async)) {
            bump();
            f.is_async = true;
        }
        if (!is_kw(peek(), Kw::Function))
            return error<FnDecl>("expected 'function' after 'async'");
        bump();
        auto name = expect_ident();
        f.name = name.first;
        // generic type parameters: `function f<T, U>(...)`
        f.type_params = type_param_list();
        expect_punct(P::LParen, "(");
        if (!eat_punct(P::RParen)) {
            for (;;) {
                Span pspan = span();
                auto pname = expect_ident();
                Param p;
                p.name = pname.first;
                p.span = pspan;
                if (is_p(peek(), P::Colon)) {
                    bump();
                    p.ann = ty_ann();
                }
                f.params.push_back(std::move(p));
                if (!eat_punct(P::Comma)) break;
            }
            expect_punct(P::RParen, ")");
        }
        if (is_p(peek(), P::Colon)) {
            bump();
            f.ret_ann = ty_ann();
        }
        f.body = block();
        return f;
    }

    // `<T, U>` after a function/class name — empty when absent.
    // Speculatively parse `<T, U>` in expression position. Returns a value
    // only if the full `<...>(` pattern is present; otherwise the lexer
    // position is restored so `<` can still be a comparison.
    std::optional<std::vector<TyAnn>> try_call_type_args() {
        size_t save = pos_;
        bump(); // <
        std::vector<TyAnn> tas;
        for (;;) {
            try {
                tas.push_back(ty_ann());
            } catch (const ParseError&) {
                pos_ = save;
                return std::nullopt;
            }
            if (!eat_punct(P::Comma)) break;
        }
        try {
            expect_punct(P::Gt, ">");
        } catch (const ParseError&) {
            pos_ = save;
            return std::nullopt;
        }
        if (!is_p(peek(), P::LParen)) {
            pos_ = save;
            return std::nullopt;
        }
        return tas;
    }

    std::vector<std::string> type_param_list() {
        if (!is_p(peek(), P::Lt)) return {};
        bump();
        std::vector<std::string> ps;
        for (;;) {
            auto p = expect_ident();
            ps.push_back(p.first);
            if (!eat_punct(P::Comma)) break;
        }
        expect_punct(P::Gt, ">");
        return ps;
    }

    TyAnn ty_ann() {
        TyAnn ann;
        ann.span = span();
        auto name = expect_ident();
        // dotted type paths: `http.Request`, `http.Response`
        std::string full = name.first;
        while (is_p(peek(), P::Dot)) {
            bump();
            auto seg = expect_ident();
            full = full + "." + seg.first;
        }
        if (is_p(peek(), P::Lt)) {
            bump();
            for (;;) {
                ann.args.push_back(ty_ann());
                if (!eat_punct(P::Comma)) break;
            }
            expect_punct(P::Gt, ">");
        }
        ann.name = std::move(full);
        return ann;
    }

    std::vector<Stmt> block() {
        expect_punct(P::LBrace, "{");
        std::vector<Stmt> stmts;
        while (!is_p(peek(), P::RBrace)) {
            if (is_eof(peek()))
                error<int>("expected '}' before end of file");
            stmts.push_back(stmt());
        }
        bump(); // }
        return stmts;
    }

    Stmt stmt() {
        if (is_kw(peek(), Kw::Let) || is_kw(peek(), Kw::Const)) {
            return let_stmt();
        }
        if (is_kw(peek(), Kw::Return)) {
            SReturn r;
            r.value = std::nullopt;
            Span sp = span();
            bump();
            if (!(is_p(peek(), P::Semi) || is_p(peek(), P::RBrace) ||
                  is_eof(peek()))) {
                r.value = expr();
            }
            eat_punct(P::Semi);
            r.span = sp;
            Stmt s;
            s.v = std::move(r);
            return s;
        }
        if (is_kw(peek(), Kw::If)) return if_stmt();
        if (is_kw(peek(), Kw::For)) return for_stmt();
        if (is_kw(peek(), Kw::While)) {
            SWhile w;
            Span sp = span();
            bump();
            w.cond = expr();
            w.body = block();
            eat_punct(P::Semi);
            w.span = sp;
            Stmt s;
            s.v = std::move(w);
            return s;
        }
        if (is_kw(peek(), Kw::Function) || is_kw(peek(), Kw::Async)) {
            return error<Stmt>("functions must be declared at the top level of the file");
        }

        // `list[i] = value` — lookahead for the `=` after the `]`
        if (std::holds_alternative<TIdent>(peek())) {
            if (is_p(peek_at(1), P::LBracket)) {
                if (auto end = matching_bracket(1)) {
                    if (is_p(peek_at(*end + 1), P::Assign)) {
                        SIndexAssign ia;
                        Span sp = span();
                        auto name = expect_ident();
                        ia.name = name.first;
                        bump(); // [
                        ia.index = expr();
                        expect_punct(P::RBracket, "]");
                        bump(); // =
                        ia.value = expr();
                        eat_punct(P::Semi);
                        ia.span = sp;
                        Stmt s;
                        s.v = std::move(ia);
                        return s;
                    }
                }
            }
            // `obj.field = value` and compound forms (`obj.f += x`,
            // `obj.f++`) — lookahead for `ident . ident <assign-op>`
            if (is_p(peek_at(1), P::Dot)) {
                if (std::holds_alternative<TIdent>(peek()) &&
                    std::holds_alternative<TIdent>(peek_at(2))) {
                    size_t op_off = 3;
                    // delta: nullopt means plain `=`, otherwise the binop
                    std::optional<std::optional<BinOp>> compound;
                    const TokV& op_tok = peek_at(op_off);
                    if (is_p(op_tok, P::Assign)) compound = std::optional<BinOp>();
                    else if (is_p(op_tok, P::PlusAssign)) compound = BinOp::Add;
                    else if (is_p(op_tok, P::MinusAssign)) compound = BinOp::Sub;
                    else if (is_p(op_tok, P::PlusPlus)) compound = BinOp::Add;
                    else if (is_p(op_tok, P::MinusMinus)) compound = BinOp::Sub;
                    if (compound.has_value()) {
                        SMemberAssign ma;
                        Span sp = span();
                        auto obj = expect_ident();
                        ma.obj = obj.first;
                        bump(); // .
                        auto field = expect_ident();
                        ma.field = field.first;
                        ExprPtr fexpr = mk(Expr{});
                        fexpr->span = sp;
                        EMember mem;
                        ExprPtr ident = mk(Expr{});
                        ident->span = sp;
                        ident->v = EIdent{ma.obj};
                        mem.obj = ident;
                        mem.name = ma.field;
                        fexpr->v = std::move(mem);

                        if (!compound->has_value()) {
                            bump(); // =
                            ma.value = expr();
                        } else {
                            bump(); // += -= ++ --
                            ExprPtr rhs;
                            const TokV& next = peek_at(0);
                            // ++ / -- need no operand
                            if (is_p(next, P::Semi) || is_p(next, P::RBrace) ||
                                is_eof(next)) {
                                rhs = mk(Expr{});
                                rhs->span = sp;
                                rhs->v = EInt{1};
                            } else {
                                rhs = expr();
                            }
                            EBinary bin;
                            bin.op = **compound;
                            bin.lhs = fexpr;
                            bin.rhs = rhs;
                            ExprPtr bin_e = mk(Expr{});
                            bin_e->span = sp;
                            bin_e->v = std::move(bin);
                            ma.value = bin_e;
                        }
                        eat_punct(P::Semi);
                        ma.span = sp;
                        Stmt s;
                        s.v = std::move(ma);
                        return s;
                    }
                }
            }
        }
        // assignment, increment/decrement, or expression statement
        return simple_stmt();
    }

    // Given that self.pos + off is a `[`, find the offset of its matching
    // `]`, counting nesting. Returns nullopt if unbalanced before EOF.
    std::optional<size_t> matching_bracket(size_t off) const {
        int depth = 0;
        size_t i = pos_ + off;
        while (i < toks_.size()) {
            const TokV& t = toks_[i].tok;
            if (is_p(t, P::LBracket)) {
                depth += 1;
            } else if (is_p(t, P::RBracket)) {
                depth -= 1;
                if (depth == 0) return i - pos_;
            } else if (is_eof(t)) {
                return std::nullopt;
            }
            i += 1;
        }
        return std::nullopt;
    }

    // Parses a "simple" statement: an assignment (`a = e`), a compound
    // assignment (`a += e`, `a -= e`), an increment/decrement
    // (`a++`, `a--`), or a plain expression. Used both for regular
    // statements and for `for` init/update slots.
    Stmt simple_stmt() {
        if (auto* name_p = std::get_if<TIdent>(&peek())) {
            std::string name = name_p->name;
            Span name_span = span();
            const TokV& next = peek_at(1);
            if (is_p(next, P::Assign)) {
                bump();
                bump(); // =
                SAssign a;
                a.value = expr();
                eat_punct(P::Semi);
                a.name = std::move(name);
                a.span = name_span;
                Stmt s;
                s.v = std::move(a);
                return s;
            }
            if (is_p(next, P::PlusAssign) || is_p(next, P::MinusAssign)) {
                BinOp op = is_p(peek_at(1), P::PlusAssign) ? BinOp::Add : BinOp::Sub;
                bump();
                bump(); // += or -=
                SAssign a;
                ExprPtr rhs = expr();
                eat_punct(P::Semi);
                ExprPtr lhs = mk(Expr{});
                lhs->span = name_span;
                lhs->v = EIdent{name};
                EBinary bin;
                bin.op = op;
                bin.lhs = lhs;
                bin.rhs = rhs;
                ExprPtr bin_e = mk(Expr{});
                bin_e->span = name_span;
                bin_e->v = std::move(bin);
                a.name = std::move(name);
                a.value = bin_e;
                a.span = name_span;
                Stmt s;
                s.v = std::move(a);
                return s;
            }
            if (is_p(next, P::PlusPlus) || is_p(next, P::MinusMinus)) {
                BinOp op = is_p(peek_at(1), P::PlusPlus) ? BinOp::Add : BinOp::Sub;
                bump();
                bump(); // ++ or --
                eat_punct(P::Semi);
                SAssign a;
                ExprPtr lhs = mk(Expr{});
                lhs->span = name_span;
                lhs->v = EIdent{name};
                ExprPtr one = mk(Expr{});
                one->span = name_span;
                one->v = EInt{1};
                EBinary bin;
                bin.op = op;
                bin.lhs = lhs;
                bin.rhs = one;
                ExprPtr bin_e = mk(Expr{});
                bin_e->span = name_span;
                bin_e->v = std::move(bin);
                a.name = std::move(name);
                a.value = bin_e;
                a.span = name_span;
                Stmt s;
                s.v = std::move(a);
                return s;
            }
        }
        SExpr se;
        se.expr = expr();
        eat_punct(P::Semi);
        Stmt s;
        s.v = std::move(se);
        return s;
    }

    Stmt for_stmt() {
        SFor f;
        Span sp = span();
        bump(); // for
        expect_punct(P::LParen, "(");
        if (is_p(peek(), P::Semi)) {
            bump();
        } else if (is_kw(peek(), Kw::Let) || is_kw(peek(), Kw::Const)) {
            f.init = std::make_shared<Stmt>(let_stmt());
        } else {
            f.init = std::make_shared<Stmt>(simple_stmt());
        }
        if (!is_p(peek(), P::Semi)) {
            f.cond = expr();
        }
        expect_punct(P::Semi, ";");
        if (!is_p(peek(), P::RParen)) {
            f.update = std::make_shared<Stmt>(simple_stmt());
        }
        expect_punct(P::RParen, ")");
        f.body = block();
        eat_punct(P::Semi);
        f.span = sp;
        Stmt s;
        s.v = std::move(f);
        return s;
    }

    Stmt let_stmt() {
        SLet l;
        Span sp = span();
        l.is_const = is_kw(peek(), Kw::Const);
        bump();
        auto name = expect_ident();
        l.name = name.first;
        if (is_p(peek(), P::Colon)) {
            bump();
            l.ann = ty_ann();
        }
        if (!eat_punct(P::Assign)) {
            return error<Stmt>(
                "declarations must be initialized (marina has no 'undefined')");
        }
        l.init = expr();
        eat_punct(P::Semi);
        l.span = sp;
        Stmt s;
        s.v = std::move(l);
        return s;
    }

    Stmt if_stmt() {
        SIf i;
        Span sp = span();
        bump(); // if
        i.cond = expr();
        i.then_body = block();
        if (is_kw(peek(), Kw::Else)) {
            bump();
            if (is_kw(peek(), Kw::If)) {
                std::vector<Stmt> nested;
                nested.push_back(if_stmt());
                i.else_body = std::move(nested);
            } else {
                i.else_body = block();
            }
        }
        eat_punct(P::Semi);
        i.span = sp;
        Stmt s;
        s.v = std::move(i);
        return s;
    }

    // ------------------------------------------------------------------
    // Expressions (precedence climbing)
    // ------------------------------------------------------------------

    ExprPtr expr() { return or_expr(); }

    ExprPtr or_expr() {
        ExprPtr l = and_expr();
        while (is_p(peek(), P::OrOr)) {
            Span sp = span();
            bump();
            ExprPtr r = and_expr();
            EBinary bin;
            bin.op = BinOp::Or;
            bin.lhs = l;
            bin.rhs = r;
            l = mk(Expr{});
            l->span = sp;
            l->v = std::move(bin);
        }
        return l;
    }

    ExprPtr and_expr() {
        ExprPtr l = eq_expr();
        while (is_p(peek(), P::AndAnd)) {
            Span sp = span();
            bump();
            ExprPtr r = eq_expr();
            EBinary bin;
            bin.op = BinOp::And;
            bin.lhs = l;
            bin.rhs = r;
            l = mk(Expr{});
            l->span = sp;
            l->v = std::move(bin);
        }
        return l;
    }

    ExprPtr eq_expr() {
        ExprPtr l = rel_expr();
        for (;;) {
            BinOp op;
            if (is_p(peek(), P::Eq)) op = BinOp::Eq;
            else if (is_p(peek(), P::Neq)) op = BinOp::Neq;
            else break;
            Span sp = span();
            bump();
            ExprPtr r = rel_expr();
            EBinary bin;
            bin.op = op;
            bin.lhs = l;
            bin.rhs = r;
            l = mk(Expr{});
            l->span = sp;
            l->v = std::move(bin);
        }
        return l;
    }

    ExprPtr rel_expr() {
        ExprPtr l = add_expr();
        for (;;) {
            BinOp op;
            if (is_p(peek(), P::Lt)) op = BinOp::Lt;
            else if (is_p(peek(), P::Gt)) op = BinOp::Gt;
            else if (is_p(peek(), P::Le)) op = BinOp::Le;
            else if (is_p(peek(), P::Ge)) op = BinOp::Ge;
            else break;
            Span sp = span();
            bump();
            ExprPtr r = add_expr();
            EBinary bin;
            bin.op = op;
            bin.lhs = l;
            bin.rhs = r;
            l = mk(Expr{});
            l->span = sp;
            l->v = std::move(bin);
        }
        return l;
    }

    ExprPtr add_expr() {
        ExprPtr l = mul_expr();
        for (;;) {
            BinOp op;
            if (is_p(peek(), P::Plus)) op = BinOp::Add;
            else if (is_p(peek(), P::Minus)) op = BinOp::Sub;
            else break;
            Span sp = span();
            bump();
            ExprPtr r = mul_expr();
            EBinary bin;
            bin.op = op;
            bin.lhs = l;
            bin.rhs = r;
            l = mk(Expr{});
            l->span = sp;
            l->v = std::move(bin);
        }
        return l;
    }

    ExprPtr mul_expr() {
        ExprPtr l = unary_expr();
        for (;;) {
            BinOp op;
            if (is_p(peek(), P::Star)) op = BinOp::Mul;
            else if (is_p(peek(), P::Slash)) op = BinOp::Div;
            else if (is_p(peek(), P::Percent)) op = BinOp::Mod;
            else break;
            Span sp = span();
            bump();
            ExprPtr r = unary_expr();
            EBinary bin;
            bin.op = op;
            bin.lhs = l;
            bin.rhs = r;
            l = mk(Expr{});
            l->span = sp;
            l->v = std::move(bin);
        }
        return l;
    }

    ExprPtr unary_expr() {
        Span sp = span();
        if (is_kw(peek(), Kw::Await)) {
            bump();
            EAwait a;
            a.operand = unary_expr();
            ExprPtr e = mk(Expr{});
            e->span = sp;
            e->v = std::move(a);
            return e;
        }
        if (is_p(peek(), P::Minus)) {
            bump();
            EUnary u;
            u.op = UnOp::Neg;
            u.operand = unary_expr();
            ExprPtr e = mk(Expr{});
            e->span = sp;
            e->v = std::move(u);
            return e;
        }
        if (is_p(peek(), P::Bang)) {
            bump();
            EUnary u;
            u.op = UnOp::Not;
            u.operand = unary_expr();
            ExprPtr e = mk(Expr{});
            e->span = sp;
            e->v = std::move(u);
            return e;
        }
        return postfix_expr();
    }

    ExprPtr postfix_expr() {
        ExprPtr e = primary();
        for (;;) {
            if (is_p(peek(), P::Dot)) {
                bump();
                auto name = expect_ident();
                EMember m;
                m.obj = e;
                m.name = name.first;
                Span sp = span();
                e = mk(Expr{});
                e->span = sp;
                e->v = std::move(m);
            } else if (is_p(peek(), P::LParen)) {
                bump();
                ECall c;
                c.callee = e;
                if (!eat_punct(P::RParen)) {
                    for (;;) {
                        c.args.push_back(expr());
                        if (!eat_punct(P::Comma)) break;
                    }
                    expect_punct(P::RParen, ")");
                }
                Span sp = span();
                e = mk(Expr{});
                e->span = sp;
                e->v = std::move(c);
            } else if (is_p(peek(), P::Lt)) {
                // speculative parse of explicit type arguments followed
                // by a call: `pick<Int32>(...)`. Restored (treated as a
                // comparison) unless the pattern `<...>(` completes.
                size_t save = pos_;
                auto tas = try_call_type_args();
                if (tas.has_value()) {
                    bump(); // (
                    ECallGen cg;
                    cg.callee = e;
                    cg.type_args = std::move(*tas);
                    if (!eat_punct(P::RParen)) {
                        for (;;) {
                            cg.args.push_back(expr());
                            if (!eat_punct(P::Comma)) break;
                        }
                        expect_punct(P::RParen, ")");
                    }
                    Span sp = span();
                    e = mk(Expr{});
                    e->span = sp;
                    e->v = std::move(cg);
                } else {
                    pos_ = save;
                    break;
                }
            } else if (is_p(peek(), P::LBracket)) {
                bump();
                EIndex ix;
                ix.obj = e;
                ix.index = expr();
                expect_punct(P::RBracket, "]");
                Span sp = span();
                e = mk(Expr{});
                e->span = sp;
                e->v = std::move(ix);
            } else if (std::holds_alternative<TTemplate>(peek())) {
                // tagged template: `sql.stmt\`...\``
                auto [parts, sp] = parse_template_token();
                ETagged tg;
                tg.tag = e;
                tg.parts = std::move(parts);
                e = mk(Expr{});
                e->span = sp;
                e->v = std::move(tg);
            } else {
                break;
            }
        }
        return e;
    }

    // Consume a peeked TTemplate token and convert its lexer parts into AST
    // template parts (re-lexing interpolated expressions).
    std::pair<std::vector<TPart>, Span> parse_template_token() {
        Span sp = span();
        auto* tpl = std::get_if<TTemplate>(&peek());
        if (!tpl) throw ParseError{"expected a template literal", sp};
        std::vector<LTPart> parts = tpl->parts;
        bump();
        std::vector<TPart> out;
        for (auto& p : parts) {
            if (auto* s = std::get_if<LStr>(&p)) {
                out.push_back(ast::TPart{ast::TStr{s->value}});
            } else if (auto* e = std::get_if<LExpr>(&p)) {
                auto sub = lex::lex_sub(e->raw, e->span);
                Parser sp2(std::move(sub));
                ExprPtr inner = sp2.expr();
                if (!is_eof(sp2.peek())) {
                    throw ParseError{"unexpected tokens in template expression", e->span};
                }
                out.push_back(ast::TPart{ast::TExpr{inner}});
            }
        }
        return {std::move(out), sp};
    }

    // Is the '(' at pos_ the start of a lambda parameter list?
    bool is_lambda_ahead() const {
        size_t i = pos_;
        int depth = 0;
        while (i < toks_.size()) {
            const TokV& t = toks_[i].tok;
            if (is_p(t, P::LParen)) {
                depth += 1;
            } else if (is_p(t, P::RParen)) {
                depth -= 1;
                if (depth == 0) {
                    return i + 1 < toks_.size() && is_p(toks_[i + 1].tok, P::Arrow);
                }
            } else if (is_eof(t)) {
                return false;
            }
            i += 1;
        }
        return false;
    }

    ExprPtr primary() {
        Span sp = span();
        if (auto* v = std::get_if<TInt>(&peek())) {
            bump();
            ExprPtr e = mk(Expr{});
            e->span = sp;
            e->v = EInt{v->value};
            return e;
        }
        if (auto* v = std::get_if<TFloat>(&peek())) {
            bump();
            ExprPtr e = mk(Expr{});
            e->span = sp;
            e->v = EFloat{v->value};
            return e;
        }
        if (auto* v = std::get_if<TStr>(&peek())) {
            bump();
            ExprPtr e = mk(Expr{});
            e->span = sp;
            e->v = EStr{v->value};
            return e;
        }
        if (auto* v = std::get_if<TTemplate>(&peek())) {
            bump();
            ETemplate t;
            for (auto& p : v->parts) {
                if (auto* s = std::get_if<LStr>(&p)) {
                    t.parts.push_back(ast::TPart{ast::TStr{s->value}});
                } else if (auto* e = std::get_if<LExpr>(&p)) {
                    auto sub = lex::lex_sub(e->raw, e->span);
                    Parser sp2(std::move(sub));
                    ExprPtr inner = sp2.expr();
                    if (!is_eof(sp2.peek())) {
                        throw ParseError{"unexpected tokens in template expression", e->span};
                    }
                    t.parts.push_back(ast::TPart{ast::TExpr{inner}});
                }
            }
            ExprPtr e = mk(Expr{});
            e->span = sp;
            e->v = std::move(t);
            return e;
        }
        if (is_kw(peek(), Kw::True)) {
            bump();
            ExprPtr e = mk(Expr{});
            e->span = sp;
            e->v = EBool{true};
            return e;
        }
        if (is_kw(peek(), Kw::False)) {
            bump();
            ExprPtr e = mk(Expr{});
            e->span = sp;
            e->v = EBool{false};
            return e;
        }
        if (is_kw(peek(), Kw::New)) {
            bump();
            auto name = expect_ident();
            if (name.first == "Promise" && is_p(peek(), P::LParen)) {
                expect_punct(P::LParen, "(");
                ExprPtr l = expr();
                expect_punct(P::RParen, ")");
                ExprPtr e = mk(Expr{});
                e->span = sp;
                e->v = ENewPromise{l};
                return e;
            }
            // dotted constructor path: `new http.Server()` etc.
            std::vector<std::string> path;
            path.push_back(name.first);
            while (is_p(peek(), P::Dot)) {
                bump();
                auto seg = expect_ident();
                path.push_back(seg.first);
            }
            // optional explicit type arguments: `new Box<Int32>()`
            // (parsed before the argument parens)
            std::vector<TyAnn> type_args;
            if (path.size() == 1 && is_p(peek(), P::Lt)) {
                bump();
                for (;;) {
                    type_args.push_back(ty_ann());
                    if (!eat_punct(P::Comma)) break;
                }
                expect_punct(P::Gt, ">");
            }
            // constructor arguments
            std::vector<ExprPtr> args;
            if (is_p(peek(), P::LParen)) {
                bump();
                if (!eat_punct(P::RParen)) {
                    for (;;) {
                        args.push_back(expr());
                        if (!eat_punct(P::Comma)) break;
                    }
                    expect_punct(P::RParen, ")");
                }
            }
            if (path.size() == 1) {
                if (args.empty()) {
                    // `new ClassName()` — no constructor args in marina
                    ExprPtr e = mk(Expr{});
                    e->span = sp;
                    e->v = ENewClass{path[0], std::move(type_args)};
                    return e;
                }
                return error<ExprPtr>(
                    "classes take no constructor arguments (use `new ClassName()`)");
            }
            ExprPtr e = mk(Expr{});
            e->span = sp;
            e->v = ENewCtor{std::move(path), std::move(args)};
            return e;
        }
        if (is_p(peek(), P::LBracket)) {
            bump();
            EArrayLit arr;
            if (!eat_punct(P::RBracket)) {
                for (;;) {
                    arr.elems.push_back(expr());
                    if (!eat_punct(P::Comma)) break;
                    // allow a trailing comma
                    if (eat_punct(P::RBracket)) {
                        ExprPtr e = mk(Expr{});
                        e->span = sp;
                        e->v = std::move(arr);
                        return e;
                    }
                }
                expect_punct(P::RBracket, "]");
            }
            ExprPtr e = mk(Expr{});
            e->span = sp;
            e->v = std::move(arr);
            return e;
        }
        if (is_p(peek(), P::LBrace)) {
            // object literal: { field: expr, ... } (trailing comma ok)
            bump();
            EObjectLit obj;
            if (!eat_punct(P::RBrace)) {
                for (;;) {
                    auto fname = expect_ident();
                    expect_punct(P::Colon, ":");
                    ExprPtr value = expr();
                    obj.fields.push_back(EObjectField{fname.first, value});
                    if (!eat_punct(P::Comma)) break;
                    if (eat_punct(P::RBrace)) {
                        ExprPtr e = mk(Expr{});
                        e->span = sp;
                        e->v = std::move(obj);
                        return e;
                    }
                }
                expect_punct(P::RBrace, "}");
            }
            ExprPtr e = mk(Expr{});
            e->span = sp;
            e->v = std::move(obj);
            return e;
        }
        if (auto* n = std::get_if<TIdent>(&peek())) {
            bump();
            ExprPtr e = mk(Expr{});
            e->span = sp;
            e->v = EIdent{n->name};
            return e;
        }
        if (is_kw(peek(), Kw::Async)) {
            // async lambda: `async (params) => { ... }`
            bump();
            if (!is_lambda_ahead()) {
                return error<ExprPtr>("expected (params) => { ... } after `async`");
            }
            return lambda(true);
        }
        if (is_p(peek(), P::LParen)) {
            if (is_lambda_ahead()) {
                return lambda(false);
            }
            bump();
            ExprPtr e = expr();
            expect_punct(P::RParen, ")");
            return e;
        }
        return error<ExprPtr>("expected an expression");
    }

    ExprPtr lambda(bool is_async) {
        Span sp = span();
        expect_punct(P::LParen, "(");
        Lambda lam;
        lam.is_async = is_async;
        lam.span = sp;
        if (!eat_punct(P::RParen)) {
            for (;;) {
                Param p;
                p.span = span();
                auto pname = expect_ident();
                p.name = pname.first;
                if (is_p(peek(), P::Colon)) {
                    bump();
                    p.ann = ty_ann();
                }
                lam.params.push_back(std::move(p));
                if (!eat_punct(P::Comma)) break;
            }
            expect_punct(P::RParen, ")");
        }
        expect_punct(P::Arrow, "=>");
        if (!is_p(peek(), P::LBrace)) {
            return error<ExprPtr>("lambda bodies must be a { block } in marina");
        }
        lam.body = block();
        ELambda el;
        el.lambda = std::make_shared<Lambda>(std::move(lam));
        ExprPtr e = mk(Expr{});
        e->span = sp;
        e->v = std::move(el);
        return e;
    }
};

} // namespace

Program parse(std::vector<Token> tokens) {
    Parser p(std::move(tokens));
    return p.program();
}

Span stmt_span(const Stmt& s) {
    if (auto* x = std::get_if<SLet>(&s.v)) return x->span;
    if (auto* x = std::get_if<SAssign>(&s.v)) return x->span;
    if (auto* x = std::get_if<SMemberAssign>(&s.v)) return x->span;
    if (auto* x = std::get_if<SIndexAssign>(&s.v)) return x->span;
    if (auto* x = std::get_if<SReturn>(&s.v)) return x->span;
    if (auto* x = std::get_if<SIf>(&s.v)) return x->span;
    if (auto* x = std::get_if<SWhile>(&s.v)) return x->span;
    if (auto* x = std::get_if<SFor>(&s.v)) return x->span;
    if (auto* x = std::get_if<SExpr>(&s.v)) return x->expr->span;
    return Span{1, 1};
}

} // namespace marina::ast
