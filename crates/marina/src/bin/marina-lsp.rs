//! marina-lsp — a minimal Language Server Protocol implementation for
//! marina (.tsm) programs, used by the "marina Language Support" editor
//! extension (see editors/marina-lsp/).
//!
//! Protocol: LSP over stdio (Content-Length framed JSON-RPC). Supported:
//!   * initialize / initialized / shutdown / exit
//!   * textDocument/didOpen + didChange (full sync) → publishDiagnostics
//!     (lex, parse and type-check errors, plus the strict naming rules)
//!   * textDocument/hover → class shape info (fields + types, ghost classes)

use marina::{ast::Program, checker, lexer, parser};

use std::collections::HashMap;
use std::io::{Read, Write};

use checker::Diag;

// ---------------------------------------------------------------------------
// Minimal JSON value + parser (enough for LSP messages)
// ---------------------------------------------------------------------------

#[derive(Clone, Debug)]
enum Json {
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
    Arr(Vec<Json>),
    Obj(HashMap<String, Json>),
}

impl Json {
    fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Obj(m) => m.get(key),
            _ => None,
        }
    }

    fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }

    fn as_i64(&self) -> Option<i64> {
        match self {
            Json::Num(n) => Some(*n as i64),
            _ => None,
        }
    }

    fn to_json(&self) -> String {
        match self {
            Json::Null => "null".into(),
            Json::Bool(b) => b.to_string(),
            Json::Num(n) => {
                if n.fract() == 0.0 && n.is_finite() && n.abs() < 9e15 {
                    format!("{}", *n as i64)
                } else {
                    format!("{}", n)
                }
            }
            Json::Str(s) => json_escape(s),
            Json::Arr(items) => {
                let parts: Vec<String> = items.iter().map(|i| i.to_json()).collect();
                format!("[{}]", parts.join(","))
            }
            Json::Obj(m) => {
                let mut keys: Vec<&String> = m.keys().collect();
                keys.sort();
                let parts: Vec<String> = keys
                    .iter()
                    .map(|k| format!("{}:{}", json_escape(k), m[*k].to_json()))
                    .collect();
                format!("{{{}}}", parts.join(","))
            }
        }
    }
}

fn json_escape(s: &str) -> String {
    let mut out = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

struct JsonParser<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> JsonParser<'a> {
    fn parse(mut self) -> Result<Json, String> {
        self.skip_ws();
        let v = self.value()?;
        Ok(v)
    }

    fn skip_ws(&mut self) {
        while let Some(&b) = self.bytes.get(self.pos) {
            if b == b' ' || b == b'\t' || b == b'\n' || b == b'\r' {
                self.pos += 1;
            } else {
                break;
            }
        }
    }

    fn value(&mut self) -> Result<Json, String> {
        self.skip_ws();
        match self.bytes.get(self.pos) {
            None => Err("unexpected end of json".into()),
            Some(b'{') => self.object(),
            Some(b'[') => self.array(),
            Some(b'"') => Ok(Json::Str(self.string()?)),
            Some(b't') => self.lit("true", Json::Bool(true)),
            Some(b'f') => self.lit("false", Json::Bool(false)),
            Some(b'n') => self.lit("null", Json::Null),
            Some(_) => self.number(),
        }
    }

    fn lit(&mut self, text: &str, v: Json) -> Result<Json, String> {
        if self.bytes[self.pos..].starts_with(text.as_bytes()) {
            self.pos += text.len();
            Ok(v)
        } else {
            Err(format!("invalid literal near byte {}", self.pos))
        }
    }

    fn number(&mut self) -> Result<Json, String> {
        let start = self.pos;
        while let Some(&b) = self.bytes.get(self.pos) {
            if b.is_ascii_digit() || b == b'-' || b == b'+' || b == b'.' || b == b'e' || b == b'E'
            {
                self.pos += 1;
            } else {
                break;
            }
        }
        let text = std::str::from_utf8(&self.bytes[start..self.pos])
            .map_err(|_| "invalid number".to_string())?;
        text.parse::<f64>()
            .map(Json::Num)
            .map_err(|_| "invalid number".to_string())
    }

    fn string(&mut self) -> Result<String, String> {
        if self.bytes.get(self.pos) != Some(&b'"') {
            return Err("expected string".into());
        }
        self.pos += 1;
        let mut out = String::new();
        loop {
            match self.bytes.get(self.pos) {
                None => return Err("unterminated string".into()),
                Some(b'"') => {
                    self.pos += 1;
                    return Ok(out);
                }
                Some(b'\\') => {
                    self.pos += 1;
                    match self.bytes.get(self.pos) {
                        Some(b'"') => out.push('"'),
                        Some(b'\\') => out.push('\\'),
                        Some(b'/') => out.push('/'),
                        Some(b'n') => out.push('\n'),
                        Some(b't') => out.push('\t'),
                        Some(b'r') => out.push('\r'),
                        Some(b'b') => out.push('\u{8}'),
                        Some(b'f') => out.push('\u{c}'),
                        Some(b'u') => {
                            let hex = std::str::from_utf8(
                                &self.bytes[self.pos + 1..self.pos + 5],
                            )
                            .map_err(|_| "bad \\u escape".to_string())?;
                            let cp = u32::from_str_radix(hex, 16)
                                .map_err(|_| "bad \\u escape".to_string())?;
                            out.push(char::from_u32(cp).unwrap_or('\u{fffd}'));
                            self.pos += 4;
                        }
                        _ => return Err("bad escape".into()),
                    }
                    self.pos += 1;
                }
                Some(_) => {
                    // consume one UTF-8 encoded char
                    let rest = std::str::from_utf8(&self.bytes[self.pos..])
                        .map_err(|_| "invalid utf-8".to_string())?;
                    let c = rest.chars().next().unwrap();
                    out.push(c);
                    self.pos += c.len_utf8();
                }
            }
        }
    }

    fn object(&mut self) -> Result<Json, String> {
        self.pos += 1; // {
        let mut map = HashMap::new();
        self.skip_ws();
        if self.bytes.get(self.pos) == Some(&b'}') {
            self.pos += 1;
            return Ok(Json::Obj(map));
        }
        loop {
            self.skip_ws();
            let key = self.string()?;
            self.skip_ws();
            if self.bytes.get(self.pos) != Some(&b':') {
                return Err("expected ':'".into());
            }
            self.pos += 1;
            let val = self.value()?;
            map.insert(key, val);
            self.skip_ws();
            match self.bytes.get(self.pos) {
                Some(b',') => {
                    self.pos += 1;
                }
                Some(b'}') => {
                    self.pos += 1;
                    return Ok(Json::Obj(map));
                }
                _ => return Err("expected ',' or '}'".into()),
            }
        }
    }

    fn array(&mut self) -> Result<Json, String> {
        self.pos += 1; // [
        let mut items = Vec::new();
        self.skip_ws();
        if self.bytes.get(self.pos) == Some(&b']') {
            self.pos += 1;
            return Ok(Json::Arr(items));
        }
        loop {
            let v = self.value()?;
            items.push(v);
            self.skip_ws();
            match self.bytes.get(self.pos) {
                Some(b',') => {
                    self.pos += 1;
                }
                Some(b']') => {
                    self.pos += 1;
                    return Ok(Json::Arr(items));
                }
                _ => return Err("expected ',' or ']'".into()),
            }
        }
    }
}

// ---------------------------------------------------------------------------
// LSP plumbing
// ---------------------------------------------------------------------------

fn read_message(stdin: &mut impl Read) -> Option<Vec<u8>> {
    // read headers until a blank line
    let mut line = Vec::new();
    let mut headers: HashMap<String, String> = HashMap::new();
    loop {
        line.clear();
        let mut byte = [0u8; 1];
        loop {
            match stdin.read_exact(&mut byte) {
                Ok(()) => {
                    if byte[0] == b'\n' {
                        break;
                    }
                    if byte[0] != b'\r' {
                        line.push(byte[0]);
                    }
                }
                Err(_) => return None,
            }
        }
        if line.is_empty() {
            break; // blank line: end of headers
        }
        let text = String::from_utf8_lossy(&line).to_string();
        if let Some((k, v)) = text.split_once(':') {
            headers.insert(k.trim().to_lowercase(), v.trim().to_string());
        }
    }
    let len: usize = headers.get("content-length")?.parse().ok()?;
    let mut body = vec![0u8; len];
    stdin.read_exact(&mut body).ok()?;
    Some(body)
}

fn write_message(stdout: &mut impl Write, body: &str) {
    let _ = write!(stdout, "Content-Length: {}\r\n\r\n{}", body.len(), body);
    let _ = stdout.flush();
}

fn response(id: &Json, result: Json) -> String {
    Json::Obj(HashMap::from([
        ("jsonrpc".to_string(), Json::Str("2.0".into())),
        ("id".to_string(), id.clone()),
        ("result".to_string(), result),
    ]))
    .to_json()
}

fn notification(method: &str, params: Json) -> String {
    Json::Obj(HashMap::from([
        ("jsonrpc".to_string(), Json::Str("2.0".into())),
        ("method".to_string(), Json::Str(method.into())),
        ("params".to_string(), params),
    ]))
    .to_json()
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

/// A diagnostic with an LSP range (0-based line/col).
struct LspDiag {
    line: u32,
    col: u32,
    end_line: u32,
    end_col: u32,
    msg: String,
}

fn diag_at(d: &Diag) -> LspDiag {
    // Span is 1-based; LSP is 0-based
    LspDiag {
        line: d.span.line.saturating_sub(1),
        col: d.span.col.saturating_sub(1),
        end_line: d.span.line.saturating_sub(1),
        end_col: d.span.col,
        msg: d.msg.clone(),
    }
}

/// Lex + parse + type-check a document and collect all diagnostics
/// (including the strict naming conventions).
fn check_document(text: &str, uri: &str) -> Vec<LspDiag> {
    let mut out = Vec::new();

    // file conventions: .tsm extension + kebab-case stem
    let path = uri.strip_prefix("file://").unwrap_or(uri);
    let stem = std::path::Path::new(path)
        .file_stem()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_default();
    let has_ext = path.ends_with(".tsm");
    if !has_ext {
        out.push(LspDiag {
            line: 0,
            col: 0,
            end_line: 0,
            end_col: 1,
            msg: "marina programs use the .tsm extension (ts marina)".into(),
        });
    }
    if has_ext && !checker::is_kebab_case(&stem) {
        out.push(LspDiag {
            line: 0,
            col: 0,
            end_line: 0,
            end_col: 1,
            msg: "file names must be kebab-case (lowercase words separated by '-')".into(),
        });
    }

    let tokens = match lexer::lex(text) {
        Ok(t) => t,
        Err(e) => {
            out.push(LspDiag {
                line: e.1.line.saturating_sub(1),
                col: e.1.col.saturating_sub(1),
                end_line: e.1.line.saturating_sub(1),
                end_col: e.1.col,
                msg: e.0,
            });
            return out;
        }
    };
    let program: Program = match parser::parse(tokens) {
        Ok(p) => p,
        Err(e) => {
            out.push(LspDiag {
                line: e.1.line.saturating_sub(1),
                col: e.1.col.saturating_sub(1),
                end_line: e.1.line.saturating_sub(1),
                end_col: e.1.col,
                msg: e.0,
            });
            return out;
        }
    };
    match checker::check(&program) {
        Ok(_) => {}
        Err(diags) => {
            for d in &diags {
                out.push(diag_at(d));
            }
        }
    }
    out
}

fn publish_diagnostics(uri: &str, version: Option<i64>, text: &str) -> String {
    let diags = check_document(text, uri);
    let items: Vec<Json> = diags
        .iter()
        .map(|d| {
            Json::Obj(HashMap::from([
                (
                    "range".to_string(),
                    Json::Obj(HashMap::from([
                        (
                            "start".to_string(),
                            pos_json(d.line, d.col),
                        ),
                        (
                            "end".to_string(),
                            pos_json(d.end_line, d.end_col),
                        ),
                    ])),
                ),
                ("severity".to_string(), Json::Num(1.0)),
                ("source".to_string(), Json::Str("marina".into())),
                ("message".to_string(), Json::Str(d.msg.clone())),
            ]))
        })
        .collect();
    let mut params = HashMap::from([
        ("uri".to_string(), Json::Str(uri.into())),
        ("diagnostics".to_string(), Json::Arr(items)),
    ]);
    if let Some(v) = version {
        params.insert("version".to_string(), Json::Num(v as f64));
    }
    notification("textDocument/publishDiagnostics", Json::Obj(params))
}

fn pos_json(line: u32, col: u32) -> Json {
    Json::Obj(HashMap::from([
        ("line".to_string(), Json::Num(line as f64)),
        ("character".to_string(), Json::Num(col as f64)),
    ]))
}

/// Hover: shows the shape of a class when the cursor is on a class name.
fn hover(text: &str, line: i64, col: i64) -> Json {
    // tokenize + parse to reach the class table
    let tokens = match lexer::lex(text) {
        Ok(t) => t,
        Err(_) => return Json::Null,
    };
    let program = match parser::parse(tokens) {
        Ok(p) => p,
        Err(_) => return Json::Null,
    };
    let (checked, _) = checker::check_with_diags(&program);

    // find the word under the cursor (1-based span → 0-based position)
    let lines: Vec<&str> = text.lines().collect();
    let line_text = lines.get(line.max(0) as usize).copied().unwrap_or("");
    let byte_col = line_text
        .char_indices()
        .nth(col.max(0) as usize)
        .map(|(i, _)| i)
        .unwrap_or(line_text.len());
    let before = &line_text[..byte_col];
    let after = &line_text[byte_col..];
    let start_ws = before
        .char_indices()
        .rev()
        .find(|(_, c)| !(c.is_alphanumeric() || *c == '_'))
        .map(|(i, _)| i + 1)
        .unwrap_or(0);
    let end_ws = after
        .char_indices()
        .find(|(_, c)| !(c.is_alphanumeric() || *c == '_'))
        .map(|(i, _)| i)
        .unwrap_or(after.len());
    let word = &line_text[start_ws..byte_col + end_ws];
    if word.is_empty() {
        return Json::Null;
    }

    // declared class?
    if let Some(cl) = program.classes.iter().find(|c| c.name == word) {
        let id = checked
            .classes
            .iter()
            .enumerate()
            .find(|(_, d)| d.name == cl.name)
            .map(|(i, _)| i)
            .unwrap_or(usize::MAX);
        let mut md = format!("**class {}**", cl.name);
        if id < checked.classes.len() {
            let def = &checked.classes[id];
            md.push_str("\n\n");
            for (fname, fty) in &def.fields {
                md.push_str(&format!("- `{}`: {}\n", fname, fty.name()));
            }
        }
        return hover_json(md);
    }

    // variable holding an object? show its class shape on hover
    let var_id = checked
        .var_names
        .iter()
        .rposition(|n| *n == word);
    if let Some(id) = var_id {
        if let Some(vt) = checked.var_types.get(id) {
            if let checker::CTy::Object(o) = vt {
                if let Some(def) = checked.classes.get(o.id) {
                    let mut md = if def.is_ghost {
                        format!("**{}** (ghost class)", def.name)
                    } else {
                        format!("**{}**", def.name)
                    };
                    md.push_str("\n\n");
                    for (fname, fty) in &def.fields {
                        md.push_str(&format!("- `{}`: {}\n", fname, fty.name()));
                    }
                    return hover_json(md);
                }
            }
            if !matches!(vt, checker::CTy::Void) {
                return hover_json(format!("`{}`: {}", word, vt.name()));
            }
        }
    }

    // ghost class mentioned in an error-free program? show its shape on use
    if let Some(def) = checked.classes.iter().find(|d| d.is_ghost && d.name == word) {
        let mut md = format!("**{}** (ghost class)", def.name);
        md.push_str("\n\n");
        for (fname, fty) in &def.fields {
            md.push_str(&format!("- `{}`: {}\n", fname, fty.name()));
        }
        return hover_json(md);
    }

    Json::Null
}

fn hover_json(markdown: String) -> Json {
    Json::Obj(HashMap::from([(
        "contents".to_string(),
        Json::Obj(HashMap::from([
            ("kind".to_string(), Json::Str("markdown".into())),
            ("value".to_string(), Json::Str(markdown)),
        ])),
    )]))
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

fn main() {
    let stdin = std::io::stdin();
    let mut stdout = std::io::stdout();
    let mut docs: HashMap<String, String> = HashMap::new();
    let mut initialized = false;

    loop {
        let body = match read_message(&mut stdin.lock()) {
            Some(b) => b,
            None => break,
        };
        match std::str::from_utf8(&body) {
            Ok(_) => {}
            Err(_) => continue,
        };
        let msg = match (JsonParser { bytes: body.as_slice(), pos: 0 }).parse() {
            Ok(m) => m,
            Err(_) => continue,
        };
        let method = msg.get("method").and_then(|m| m.as_str()).unwrap_or("");
        let id = msg.get("id").cloned();

        match method {
            "initialize" => {
                let result = Json::Obj(HashMap::from([
                    (
                        "capabilities".to_string(),
                        Json::Obj(HashMap::from([
                            ("textDocumentSync".to_string(), Json::Num(1.0)),
                            ("hoverProvider".to_string(), Json::Bool(true)),
                        ])),
                    ),
                    (
                        "serverInfo".to_string(),
                        Json::Obj(HashMap::from([
                            ("name".to_string(), Json::Str("marina-lsp".into())),
                            ("version".to_string(), Json::Str("0.2.0".into())),
                        ])),
                    ),
                ]));
                if let Some(id) = &id {
                    write_message(&mut stdout, &response(id, result));
                }
            }
            "initialized" => {
                initialized = true;
                // re-publish diagnostics for documents opened early
                let uris: Vec<String> = docs.keys().cloned().collect();
                for uri in uris {
                    let text = docs[&uri].clone();
                    write_message(&mut stdout, &publish_diagnostics(&uri, None, &text));
                }
            }
            "shutdown" => {
                if let Some(id) = &id {
                    write_message(&mut stdout, &response(id, Json::Null));
                }
            }
            "exit" => break,
            "textDocument/didOpen" | "textDocument/didChange" => {
                let td = msg
                    .get("params")
                    .and_then(|p| p.get("textDocument"))
                    .cloned();
                if let Some(td) = td {
                    let uri = td.get("uri").and_then(|u| u.as_str()).unwrap_or("").to_string();
                    let text = match method {
                        "textDocument/didOpen" => {
                            td.get("text").and_then(|t| t.as_str()).unwrap_or("").to_string()
                        }
                        _ => match td.get("text") {
                            Some(Json::Str(t)) => t.clone(),
                            // incremental sync would need contentChanges handling;
                            // we advertise full sync so `text` is always present
                            _ => String::new(),
                        },
                    };
                    let version = td.get("version").and_then(|v| v.as_i64());
                    docs.insert(uri.clone(), text.clone());
                    if initialized {
                        write_message(&mut stdout, &publish_diagnostics(&uri, version, &text));
                    }
                }
            }
            "textDocument/didClose" => {
                if let Some(uri) = msg
                    .get("params")
                    .and_then(|p| p.get("textDocument"))
                    .and_then(|t| t.get("uri"))
                    .and_then(|u| u.as_str())
                {
                    docs.remove(uri);
                    write_message(&mut stdout, &publish_diagnostics(uri, None, ""));
                }
            }
            "textDocument/hover" => {
                let result = match (
                    msg.get("params")
                        .and_then(|p| p.get("textDocument"))
                        .and_then(|t| t.get("uri"))
                        .and_then(|u| u.as_str()),
                    msg.get("params").and_then(|p| p.get("position")),
                ) {
                    (Some(uri), Some(pos)) => {
                        let text = docs.get(uri).cloned().unwrap_or_default();
                        hover(
                            &text,
                            pos.get("line").and_then(|l| l.as_i64()).unwrap_or(0),
                            pos.get("character").and_then(|c| c.as_i64()).unwrap_or(0),
                        )
                    }
                    _ => Json::Null,
                };
                if let Some(id) = &id {
                    write_message(&mut stdout, &response(id, result));
                }
            }
            _ => {
                // unknown request: answer null so the client never hangs
                if let Some(id) = &id {
                    write_message(&mut stdout, &response(id, Json::Null));
                }
            }
        }
    }
}
