//! Dump the token stream in the exact format the self-hosted (tsm) lexer's
//! `mast.dumpTok` produces, so the two front-ends can be diffed.
use marina::lexer::{self, Kw, P, Tok};

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
    // must match marina_runtime's fmt_float exactly (single source of truth
    // for float rendering through marina_to_string / template literals)
    if f.is_finite() && f == f.trunc() && f.abs() < 1e15 {
        format!("{}", f as i64)
    } else {
        format!("{}", f)
    }
}

fn kw_name(k: Kw) -> &'static str {
    match k {
        Kw::Let => "let",
        Kw::Const => "const",
        Kw::Function => "function",
        Kw::Async => "async",
        Kw::Await => "await",
        Kw::Return => "return",
        Kw::If => "if",
        Kw::Else => "else",
        Kw::While => "while",
        Kw::For => "for",
        Kw::True => "true",
        Kw::False => "false",
        Kw::New => "new",
        Kw::Class => "class",
        Kw::Use => "use",
        Kw::Import => "import",
        Kw::Interface => "interface",
    }
}

fn p_name(p: P) -> &'static str {
    match p {
        P::Plus => "+",
        P::Minus => "-",
        P::Star => "*",
        P::Slash => "/",
        P::Percent => "%",
        P::PlusPlus => "++",
        P::MinusMinus => "--",
        P::PlusAssign => "+=",
        P::MinusAssign => "-=",
        P::Assign => "=",
        P::Eq => "==",
        P::Neq => "!=",
        P::Lt => "<",
        P::Gt => ">",
        P::Le => "<=",
        P::Ge => ">=",
        P::AndAnd => "&&",
        P::OrOr => "||",
        P::Bang => "!",
        P::LParen => "(",
        P::RParen => ")",
        P::LBrace => "{",
        P::RBrace => "}",
        P::LBracket => "[",
        P::RBracket => "]",
        P::Comma => ",",
        P::Colon => ":",
        P::Semi => ";",
        P::Dot => ".",
        P::Arrow => "=>",
    }
}

fn dump_tok(t: &lexer::Token) -> String {
    let at = format!(" @{}:{}", t.span.line, t.span.col);
    match &t.tok {
        Tok::Ident(s) => format!("IDENT {}{}", esc(s), at),
        Tok::Int(i) => format!("INT {}{}", i, at),
        Tok::Float(f) => format!("FLOAT {}{}", fmt_float(*f), at),
        Tok::Str(s) => format!("STR {}{}", esc(s), at),
        Tok::Template(parts) => {
            let mut out = format!("TPL{}", at);
            for p in parts {
                match p {
                    lexer::TPart::Str(s) => out.push_str(&format!("\n  PSTR {}", esc(s))),
                    lexer::TPart::Expr(raw, sp) => {
                        out.push_str(&format!("\n  PEXPR {} @{}:{}", esc(raw), sp.line, sp.col))
                    }
                }
            }
            out
        }
        Tok::Kw(k) => format!("KW {}{}", kw_name(*k), at),
        Tok::P(p) => format!("P {}{}", p_name(*p), at),
        Tok::Eof => format!("EOF{}", at),
    }
}

fn main() {
    let path = std::env::args().nth(1).expect("usage: tokdump <file.tsm>");
    let src = std::fs::read_to_string(&path).unwrap();
    match lexer::lex(&src) {
        Ok(tokens) => {
            for t in &tokens {
                println!("{}", dump_tok(t));
            }
        }
        Err(e) => {
            eprintln!("LEXERR {} @{}:{}", e.0, e.1.line, e.1.col);
            std::process::exit(1);
        }
    }
}