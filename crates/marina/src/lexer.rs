//! Lexer for the marina language (a strict, compiled TypeScript subset).

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Span {
    pub line: u32,
    pub col: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kw {
    Let,
    Const,
    Function,
    Async,
    Await,
    Return,
    If,
    Else,
    While,
    For,
    True,
    False,
    New,
    Class,
    Use,
    Import,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum P {
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    PlusPlus,
    MinusMinus,
    PlusAssign,
    MinusAssign,
    Assign,
    Eq,
    Neq,
    Lt,
    Gt,
    Le,
    Ge,
    AndAnd,
    OrOr,
    Bang,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Colon,
    Semi,
    Dot,
    Arrow,
}

#[derive(Clone, Debug, PartialEq)]
pub enum TPart {
    Str(String),
    /// Raw expression source with the span of the `${`.
    Expr(String, Span),
}

#[derive(Clone, Debug, PartialEq)]
pub enum Tok {
    Ident(String),
    Int(i32),
    Float(f64),
    Str(String),
    Template(Vec<TPart>),
    Kw(Kw),
    P(P),
    Eof,
}

#[derive(Clone, Debug)]
pub struct Token {
    pub tok: Tok,
    pub span: Span,
}

pub struct LexError(pub String, pub Span);

struct Lexer {
    chars: Vec<char>,
    pos: usize,
    line: u32,
    col: u32,
    base: Option<Span>, // when sub-lexing template expressions, override spans
}

impl Lexer {
    fn new(src: &str, base: Option<Span>) -> Self {
        Lexer { chars: src.chars().collect(), pos: 0, line: 1, col: 1, base }
    }

    fn span(&self) -> Span {
        if let Some(b) = self.base {
            return b;
        }
        Span { line: self.line, col: self.col }
    }

    fn peek(&self) -> Option<char> {
        self.chars.get(self.pos).copied()
    }

    fn peek2(&self) -> Option<char> {
        self.chars.get(self.pos + 1).copied()
    }

    fn bump(&mut self) -> Option<char> {
        let c = self.peek()?;
        self.pos += 1;
        if c == '\n' {
            self.line += 1;
            self.col = 1;
        } else {
            self.col += 1;
        }
        Some(c)
    }

    fn error<T>(&self, msg: &str) -> Result<T, LexError> {
        Err(LexError(msg.to_string(), self.span()))
    }

    fn run(mut self) -> Result<Vec<Token>, LexError> {
        let mut out = Vec::new();
        loop {
            self.skip_trivia();
            let span = self.span();
            let c = match self.peek() {
                None => {
                    out.push(Token { tok: Tok::Eof, span });
                    return Ok(out);
                }
                Some(c) => c,
            };
            let tok = match c {
                '"' | '\'' => {
                    let s = self.scan_string(c)?;
                    Tok::Str(s)
                }
                '`' => {
                    let parts = self.scan_template()?;
                    Tok::Template(parts)
                }
                '0'..='9' => self.scan_number()?,
                c if is_ident_start(c) => {
                    let mut name = String::new();
                    while let Some(c) = self.peek() {
                        if is_ident_start(c) || c.is_ascii_digit() {
                            name.push(c);
                            self.bump();
                        } else {
                            break;
                        }
                    }
                    match name.as_str() {
                        "let" => Tok::Kw(Kw::Let),
                        "const" => Tok::Kw(Kw::Const),
                        "function" => Tok::Kw(Kw::Function),
                        "async" => Tok::Kw(Kw::Async),
                        "await" => Tok::Kw(Kw::Await),
                        "return" => Tok::Kw(Kw::Return),
                        "if" => Tok::Kw(Kw::If),
                        "else" => Tok::Kw(Kw::Else),
                        "while" => Tok::Kw(Kw::While),
                        "for" => Tok::Kw(Kw::For),
                        "true" => Tok::Kw(Kw::True),
                        "false" => Tok::Kw(Kw::False),
                        "new" => Tok::Kw(Kw::New),
                        "class" => Tok::Kw(Kw::Class),
                        "use" => Tok::Kw(Kw::Use),
                        "import" => Tok::Kw(Kw::Import),
                        _ => Tok::Ident(name),
                    }
                }
                _ => self.scan_punct()?,
            };
            out.push(Token { tok, span });
        }
    }

    fn skip_trivia(&mut self) {
        loop {
            match self.peek() {
                Some(' ') | Some('\t') | Some('\r') | Some('\n') => {
                    self.bump();
                }
                Some('/') if self.peek2() == Some('/') => {
                    while let Some(c) = self.peek() {
                        if c == '\n' {
                            break;
                        }
                        self.bump();
                    }
                }
                Some('/') if self.peek2() == Some('*') => {
                    self.bump();
                    self.bump();
                    loop {
                        match self.peek() {
                            None => break,
                            Some('*') if self.peek2() == Some('/') => {
                                self.bump();
                                self.bump();
                                break;
                            }
                            _ => {
                                self.bump();
                            }
                        }
                    }
                }
                _ => break,
            }
        }
    }

    fn scan_escape(&mut self, quote: char) -> Result<char, LexError> {
        // caller has consumed the backslash
        match self.bump() {
            Some('n') => Ok('\n'),
            Some('t') => Ok('\t'),
            Some('r') => Ok('\r'),
            Some('0') => Ok('\0'),
            Some('\\') => Ok('\\'),
            Some(c) if c == quote => Ok(c),
            Some('$') => Ok('$'),
            Some(c) => Ok(c),
            None => self.error("unterminated escape sequence"),
        }
    }

    fn scan_string(&mut self, quote: char) -> Result<String, LexError> {
        self.bump(); // opening quote
        let mut s = String::new();
        loop {
            match self.peek() {
                None => return self.error("unterminated string literal"),
                Some(c) if c == quote => {
                    self.bump();
                    return Ok(s);
                }
                Some('\\') => {
                    self.bump();
                    s.push(self.scan_escape(quote)?);
                }
                Some(c) => {
                    s.push(c);
                    self.bump();
                }
            }
        }
    }

    fn scan_template(&mut self) -> Result<Vec<TPart>, LexError> {
        self.bump(); // opening backtick
        let mut parts = Vec::new();
        let mut buf = String::new();
        loop {
            let span = self.span();
            match self.peek() {
                None => return self.error("unterminated template literal"),
                Some('`') => {
                    self.bump();
                    if !buf.is_empty() || parts.is_empty() {
                        parts.push(TPart::Str(buf));
                    }
                    return Ok(parts);
                }
                Some('\\') => {
                    self.bump();
                    buf.push(self.scan_escape('`')?);
                }
                Some('$') if self.peek2() == Some('{') => {
                    self.bump();
                    self.bump();
                    // capture raw expression until matching '}'
                    let mut raw = String::new();
                    let mut depth = 1i32;
                    loop {
                        match self.peek() {
                            None => return self.error("unterminated ${...} in template literal"),
                            Some('}') => {
                                depth -= 1;
                                self.bump();
                                if depth == 0 {
                                    break;
                                }
                                raw.push('}');
                            }
                            Some('{') => {
                                depth += 1;
                                raw.push('{');
                                self.bump();
                            }
                            Some('"') | Some('\'') => {
                                let q = self.bump().unwrap();
                                raw.push(q);
                                loop {
                                    match self.peek() {
                                        None => {
                                            return self
                                                .error("unterminated string in template expression")
                                        }
                                        Some('\\') => {
                                            self.bump();
                                            if let Some(c) = self.bump() {
                                                raw.push('\\');
                                                raw.push(c);
                                            }
                                        }
                                        Some(c2) if c2 == q => {
                                            raw.push(c2);
                                            self.bump();
                                            break;
                                        }
                                        Some(c2) => {
                                            raw.push(c2);
                                            self.bump();
                                        }
                                    }
                                }
                            }
                            Some('`') => {
                                // nested template: skip it entirely
                                self.bump();
                                let mut tdepth = 0i32;
                                loop {
                                    match self.peek() {
                                        None => {
                                            return self
                                                .error("unterminated template in template expression")
                                        }
                                        Some('\\') => {
                                            self.bump();
                                            self.bump();
                                        }
                                        Some('$') if self.peek2() == Some('{') => {
                                            self.bump();
                                            self.bump();
                                            tdepth += 1;
                                        }
                                        Some('}') if tdepth > 0 => {
                                            tdepth -= 1;
                                            self.bump();
                                        }
                                        Some('`') => {
                                            self.bump();
                                            if tdepth == 0 {
                                                break;
                                            }
                                            tdepth = 0;
                                        }
                                        Some(_) => {
                                            self.bump();
                                        }
                                    }
                                }
                            }
                            Some(c) => {
                                raw.push(c);
                                self.bump();
                            }
                        }
                    }
                    parts.push(TPart::Str(std::mem::take(&mut buf)));
                    parts.push(TPart::Expr(raw, span));
                }
                Some(c) => {
                    buf.push(c);
                    self.bump();
                }
            }
        }
    }

    fn scan_number(&mut self) -> Result<Tok, LexError> {
        let span = self.span();
        let mut text = String::new();
        let mut is_float = false;
        while let Some(c) = self.peek() {
            if c.is_ascii_digit() {
                text.push(c);
                self.bump();
            } else if c == '.' && !is_float && self.peek2().map(|c| c.is_ascii_digit()).unwrap_or(false) {
                is_float = true;
                text.push(c);
                self.bump();
            } else if (c == 'e' || c == 'E')
                && text.chars().last().map(|c| c.is_ascii_digit()).unwrap_or(false)
            {
                is_float = true;
                text.push(c);
                self.bump();
                if let Some(sign) = self.peek() {
                    if sign == '+' || sign == '-' {
                        text.push(sign);
                        self.bump();
                    }
                }
            } else {
                break;
            }
        }
        if is_float {
            let f: f64 = text
                .parse()
                .map_err(|_| LexError("invalid float literal".into(), span))?;
            Ok(Tok::Float(f))
        } else {
            let i: i64 = text
                .parse()
                .map_err(|_| LexError("integer literal out of range".into(), span))?;
            if i > i32::MAX as i64 {
                return self.error("integer literal does not fit in Int32");
            }
            Ok(Tok::Int(i as i32))
        }
    }

    fn scan_punct(&mut self) -> Result<Tok, LexError> {
        let c = self.bump().unwrap();
        let p = match c {
            '+' => {
                if self.peek() == Some('+') {
                    self.bump();
                    P::PlusPlus
                } else if self.peek() == Some('=') {
                    self.bump();
                    P::PlusAssign
                } else {
                    P::Plus
                }
            }
            '-' => {
                if self.peek() == Some('-') {
                    self.bump();
                    P::MinusMinus
                } else if self.peek() == Some('=') {
                    self.bump();
                    P::MinusAssign
                } else {
                    P::Minus
                }
            }
            '*' => P::Star,
            '/' => P::Slash,
            '%' => P::Percent,
            '=' => {
                if self.peek() == Some('=') {
                    self.bump();
                    P::Eq
                } else if self.peek() == Some('>') {
                    self.bump();
                    P::Arrow
                } else {
                    P::Assign
                }
            }
            '!' => {
                if self.peek() == Some('=') {
                    self.bump();
                    P::Neq
                } else {
                    P::Bang
                }
            }
            '<' => {
                if self.peek() == Some('=') {
                    self.bump();
                    P::Le
                } else {
                    P::Lt
                }
            }
            '>' => {
                if self.peek() == Some('=') {
                    self.bump();
                    P::Ge
                } else {
                    P::Gt
                }
            }
            '&' => {
                if self.peek() == Some('&') {
                    self.bump();
                    P::AndAnd
                } else {
                    return self.error("single '&' is not valid (use '&&')");
                }
            }
            '|' => {
                if self.peek() == Some('|') {
                    self.bump();
                    P::OrOr
                } else {
                    return self.error("single '|' is not valid (use '||')");
                }
            }
            '(' => P::LParen,
            ')' => P::RParen,
            '{' => P::LBrace,
            '}' => P::RBrace,
            '[' => P::LBracket,
            ']' => P::RBracket,
            ',' => P::Comma,
            ':' => P::Colon,
            ';' => P::Semi,
            '.' => P::Dot,
            _ => return self.error(&format!("unexpected character '{}'", c)),
        };
        Ok(Tok::P(p))
    }
}

fn is_ident_start(c: char) -> bool {
    c.is_alphabetic() || c == '_' || c == '$'
}

pub fn lex(src: &str) -> Result<Vec<Token>, LexError> {
    Lexer::new(src, None).run()
}

/// Re-lex a template interpolation expression; spans are approximated with
/// the span of the `${` that introduced it.
pub fn lex_sub(src: &str, base: Span) -> Result<Vec<Token>, LexError> {
    Lexer::new(src, Some(base)).run()
}