//! Compilation pipeline shared by the `marina` CLI and the `wave` package
//! manager: file conventions, front-end (lex/parse/check), LLVM codegen and
//! clang linking against the marina runtime.

use std::path::{Path, PathBuf};
use std::process::Command;

use crate::ast::Stmt;
use crate::{checker, codegen, lexer, parser};

/// A user-facing diagnostic with 1-based line/column (matches compiler output).
#[derive(Debug, Clone)]
pub struct SrcDiag {
    pub msg: String,
    pub line: u32,
    pub col: u32,
}

impl SrcDiag {
    fn from(msg: &str, span: lexer::Span) -> Self {
        SrcDiag { msg: msg.to_string(), line: span.line, col: span.col }
    }
}

/// Why a compilation failed.
pub enum Fail {
    /// Type/parse/lex errors (already formatted, ready to print)
    Diags(Vec<SrcDiag>),
    /// Toolchain problem (missing clang, io error, …)
    Tool(String),
}

/// Result of a successful compile.
pub struct Artifact {
    pub bin_path: PathBuf,
    pub ll_path: PathBuf,
}

/// Enforce strict file conventions: `.tsm` extension + kebab-case stem.
pub fn validate_path(path: &Path) -> Result<(), String> {
    match path.extension().and_then(|e| e.to_str()) {
        Some("tsm") => {}
        other => {
            return Err(format!(
                "marina programs use the .tsm extension (ts marina), got '{}' (ext: {:?})",
                path.display(),
                other
            ));
        }
    }
    let stem = path
        .file_stem()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_default();
    if !checker::is_kebab_case(&stem) {
        return Err(format!(
            "file names must be kebab-case (lowercase words separated by '-'), got '{}'",
            stem
        ));
    }
    Ok(())
}

/// Type-check a source string without generating code (`wave check`).
/// Returns diagnostics in source order; Ok(()) means the program passes.
pub fn check_source(src: &str) -> Result<(), Vec<SrcDiag>> {
    let tokens = match lexer::lex(src) {
        Ok(t) => t,
        Err(e) => return Err(vec![SrcDiag::from(&e.0, e.1)]),
    };
    let mut program = match parser::parse(tokens) {
        Ok(p) => p,
        Err(e) => return Err(vec![SrcDiag::from(&e.0, e.1)]),
    };
    if let Err(diags) = resolve_uses(&mut program) {
        return Err(diags);
    }
    match checker::check(&program) {
        Ok(_) => Ok(()),
        Err(diags) => Err(diags.iter().map(|d| SrcDiag::from(&d.msg, d.span)).collect()),
    }
}

/// Full pipeline: lex → parse → check → LLVM IR → clang link with the runtime.
/// Writes `<stem>.ll` and the `<stem>` binary into `out_dir` (created if needed).
pub fn compile_source(
    src: &str,
    stem: &str,
    out_dir: &Path,
) -> Result<Artifact, Fail> {
    let tokens = match lexer::lex(src) {
        Ok(t) => t,
        Err(e) => return Err(Fail::Diags(vec![SrcDiag::from(&e.0, e.1)])),
    };
    let mut program = match parser::parse(tokens) {
        Ok(p) => p,
        Err(e) => return Err(Fail::Diags(vec![SrcDiag::from(&e.0, e.1)])),
    };
    if let Err(diags) = resolve_uses(&mut program) {
        return Err(Fail::Diags(diags));
    }
    let checked = match checker::check(&program) {
        Ok(c) => c,
        Err(diags) => {
            return Err(Fail::Diags(
                diags.iter().map(|d| SrcDiag::from(&d.msg, d.span)).collect(),
            ))
        }
    };
    let ir = codegen::generate(&checked);

    std::fs::create_dir_all(out_dir).map_err(|e| Fail::Tool(format!("cannot create {}: {}", out_dir.display(), e)))?;
    let ll_path = out_dir.join(format!("{}.ll", stem));
    let bin_path = out_dir.join(stem);
    std::fs::write(&ll_path, &ir)
        .map_err(|e| Fail::Tool(format!("cannot write {}: {}", ll_path.display(), e)))?;

    let runtime = match locate_runtime() {
        Some(r) => r,
        None => {
            build_runtime();
            locate_runtime().expect("runtime library not found after build")
        }
    };

    let clang = ["clang", "clang-21", "clang-20", "clang-18", "cc"]
        .iter()
        .find(|c| which(c).is_some())
        .copied()
        .unwrap_or("clang");
    let status = {
        let mut cmd = Command::new(clang);
        cmd.arg("-O2").arg(&ll_path).arg(&runtime);
        // the runtime contains the sql module (sqlite FFI), so libsqlite3 is
        // always required (macOS ships it; Linux needs the sqlite3 package)
        cmd.arg("-lsqlite3");
        cmd.arg("-o").arg(&bin_path);
        cmd.status()
    };
    match status {
        Ok(s) if s.success() => Ok(Artifact { bin_path, ll_path }),
        Ok(_) => Err(Fail::Tool(format!(
            "linking failed (clang). LLVM IR kept at {}",
            ll_path.display()
        ))),
        Err(e) => Err(Fail::Tool(format!("cannot run clang: {}", e))),
    }
}

/// Run a compiled binary, returning its exit code.
pub fn run_binary(bin: &Path) -> i32 {
    match Command::new(bin).status() {
        Ok(s) => s.code().unwrap_or(1),
        Err(e) => {
            eprintln!("error: cannot run {}: {}", bin.display(), e);
            1
        }
    }
}

/// Print diagnostics the way the compiler does (`file:line:col`).
pub fn print_diags(file: &str, diags: &[SrcDiag]) {
    for d in diags {
        eprintln!("error: {}", d.msg);
        eprintln!("  --> {}:{}:{}", file, d.line, d.col);
    }
}

/// The global package registry root (`WAVE_REGISTRY` overrides it).
/// Packages live at `<root>/<name>/<version>/main.tsm` — like Rust crates in
/// the crates.io registry, not like node_modules.
pub fn registry_root() -> PathBuf {
    if let Ok(r) = std::env::var("WAVE_REGISTRY") {
        return PathBuf::from(r);
    }
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".to_string());
    Path::new(&home).join(".wave").join("registry")
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

/// Resolve `use <pkg>` statements.
///
/// Std modules (`net`, `http`) are built into the runtime and kept as-is.
/// Any other module is a registry package: its source is read from
/// `<registry>/<pkg>/<latest>/main.tsm`, its functions are renamed
/// `<pkg>__<name>` and appended to the main program, so `pkg.fn(...)` calls
/// type-check as plain function calls (see `checker`).
pub fn resolve_uses(program: &mut crate::ast::Program) -> Result<(), Vec<SrcDiag>> {
    let mut diags: Vec<SrcDiag> = Vec::new();
    let mut kept: Vec<(String, lexer::Span)> = Vec::new();
    let mut extra_fns = Vec::new();
    let mut extra_classes = Vec::new();

    for (name, span) in program.uses.drain(..) {
        if matches!(name.as_str(), "net" | "http" | "fs" | "crypto" | "sql") {
            kept.push((name, span));
            continue;
        }
        match load_registry_package(&name) {
            Ok(src) => {
                let tokens = match lexer::lex(&src) {
                    Ok(t) => t,
                    Err(e) => {
                        diags.push(SrcDiag::from(
                            &format!("package '{}': {}", name, e.0),
                            e.1,
                        ));
                        continue;
                    }
                };
                let mut pkg = match parser::parse(tokens) {
                    Ok(p) => p,
                    Err(e) => {
                        diags.push(SrcDiag::from(
                            &format!("package '{}': {}", name, e.0),
                            e.1,
                        ));
                        continue;
                    }
                };
                if let Some(first) = pkg.stmts.first() {
                    diags.push(SrcDiag::from(
                        &format!(
                            "package '{}' cannot have top-level statements (functions and classes only)",
                            name
                        ),
                        stmt_span(first),
                    ));
                }
                if let Some(f) = pkg.fns.iter().find(|f| f.name == "main") {
                    diags.push(SrcDiag::from(
                        &format!("package '{}' must not define main (it is a library)", name),
                        f.span,
                    ));
                }
                for (u, s) in pkg.uses.drain(..) {
                    if matches!(u.as_str(), "net" | "http" | "fs" | "crypto" | "sql") {
                        // make the std module visible to the merged functions
                        if !kept.iter().any(|(n, _)| *n == u) {
                            kept.push((u, s));
                        }
                    } else {
                        diags.push(SrcDiag::from(
                            "packages cannot depend on other packages yet (v0.2: std modules net/http/fs/crypto only)",
                            s,
                        ));
                    }
                }
                for mut f in pkg.fns.drain(..) {
                    f.name = format!("{}__{}", name, f.name);
                    extra_fns.push(f);
                }
                extra_classes.extend(pkg.classes.drain(..));
                kept.push((name, span));
            }
            Err(e) => diags.push(SrcDiag::from(&e, span)),
        }
    }

    program.uses = kept;
    program.fns.extend(extra_fns);
    program.classes.extend(extra_classes);
    if diags.is_empty() {
        Ok(())
    } else {
        Err(diags)
    }
}

fn load_registry_package(name: &str) -> Result<String, String> {
    if !checker::is_kebab_case(name) {
        return Err(format!("invalid module name '{}'", name));
    }
    let pkg_dir = registry_root().join(name);
    if !pkg_dir.is_dir() {
        return Err(format!(
            "module '{}' is not installed (run `wave add {}`)",
            name, name
        ));
    }
    let mut versions: Vec<String> = std::fs::read_dir(&pkg_dir)
        .map_err(|e| format!("cannot read registry directory {}: {}", pkg_dir.display(), e))?
        .filter_map(|e| e.ok())
        .filter(|e| e.path().is_dir())
        .filter_map(|e| e.file_name().into_string().ok())
        .collect();
    if versions.is_empty() {
        return Err(format!("package '{}' has no installed versions", name));
    }
    versions.sort_by(|a, b| version_key(a).cmp(&version_key(b)));
    let latest = versions.last().unwrap();
    let main = pkg_dir.join(latest).join("main.tsm");
    std::fs::read_to_string(&main)
        .map_err(|_| format!("package '{}' v{} is missing main.tsm", name, latest))
}

/// Sort key for `MAJOR.MINOR.PATCH` versions (falls back to lexicographic).
fn version_key(v: &str) -> Vec<u64> {
    v.split('.')
        .map(|p| p.parse::<u64>().unwrap_or(0))
        .collect()
}

/// Cheap stable hash for cache dir naming (not cryptographic).
pub fn md5ish(s: &str) -> u64 {
    let mut h: u64 = 0xcbf29ce484222325;
    for b in s.as_bytes() {
        h ^= *b as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    h
}

pub fn which(cmd: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    for dir in std::env::split_paths(&path) {
        let p = dir.join(cmd);
        if p.is_file() {
            return Some(p);
        }
    }
    None
}

/// Directory that contains this crate's workspace (…/marina), even when
/// invoked from an arbitrary project directory.
pub fn workspace_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(|p| p.parent())
        .map(|p| p.to_path_buf())
        .unwrap_or_else(|| PathBuf::from("."))
}

fn locate_runtime() -> Option<PathBuf> {
    let profile = if cfg!(debug_assertions) { "debug" } else { "release" };
    let mut candidates = Vec::new();
    if let Ok(td) = std::env::var("CARGO_TARGET_DIR") {
        candidates.push(PathBuf::from(td).join(profile).join("libmarina_runtime.a"));
    }
    // check both profiles: an installed (release) binary may share a target
    // dir with a debug `cargo build`, and vice versa
    for p in [profile, "debug", "release"] {
        candidates.push(
            workspace_root()
                .join("target")
                .join(p)
                .join("libmarina_runtime.a"),
        );
    }
    candidates.into_iter().find(|p| p.is_file())
}

fn build_runtime() {
    let root = workspace_root();
    eprintln!("building marina runtime...");
    let mut cmd = Command::new("cargo");
    cmd.args(["build", "-p", "marina-runtime"]);
    if !cfg!(debug_assertions) {
        cmd.arg("--release"); // match the profile locate_runtime() looks for
    }
    let st = cmd
        .current_dir(&root)
        .env_remove("CARGO_TARGET_DIR") // build into <workspace>/target
        .status();
    match st {
        Ok(s) if s.success() => {}
        _ => {
            eprintln!("error: failed to build the marina runtime (cargo)");
            std::process::exit(1);
        }
    }
}