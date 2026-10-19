//! wave — the package manager and build tool for marina programs
//! (what cargo is to rust).
//!
//! Commands:
//!   wave init              create package.json + src/main.tsm
//!   wave add <pkg>[@ver]   add a dependency to package.json
//!   wave remove <pkg>      remove a dependency from package.json
//!   wave check [file]      validate package.json + type-check (no codegen)
//!   wave build [file]      compile to a native binary in ./.marina/
//!   wave run [file]        compile + execute
//!
//! Project layout (a `package.json` in the current directory makes a project):
//!   package.json
//!   src/main.tsm        entry point (or "main": "src/other.tsm" in package.json)
//!
//! package.json schema:
//! {
//!   "name": "<project-name>",
//!   "version": "0.0.1",
//!   "dependencies": { "<pkg>": "<version>" }
//! }

use marina::pipeline::{self, Fail};
use serde_json::{Map, Value};
use std::path::{Path, PathBuf};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let cmd = args.first().map(|s| s.as_str()).unwrap_or("");
    let rest = &args[1.min(args.len())..];

    let code = match cmd {
        "init" => cmd_init(rest),
        "add" => cmd_add(rest),
        "remove" | "rm" => cmd_remove(rest),
        "check" => cmd_check(rest),
        "build" => cmd_build(rest),
        "run" => cmd_run(rest),
        "-h" | "--help" | "help" | "" => {
            print_help();
            0
        }
        "-V" | "--version" => {
            println!("wave {}", env!("CARGO_PKG_VERSION"));
            0
        }
        other => {
            eprintln!("error: unknown command '{}'", other);
            print_help();
            2
        }
    };
    std::process::exit(code);
}

fn print_help() {
    println!(
        "wave {} — package manager for marina (ts marina)

USAGE:
    wave <COMMAND> [args]

COMMANDS:
    init               create package.json + src/main.tsm in this directory
    add <pkg>[@ver]    add a dependency to package.json
    remove <pkg>       remove a dependency from package.json
    check [file]       validate package.json + type-check the entry point
    build [file]       compile to a native binary (in ./.marina/)
    run [file]         compile + execute

Without [file], the project entry point is used:
    \"main\" field from package.json, defaulting to src/main.tsm",
        env!("CARGO_PKG_VERSION")
    );
}

// ---------------------------------------------------------------------------
// package.json
// ---------------------------------------------------------------------------

/// Path to the manifest in the current directory.
fn manifest_path() -> PathBuf {
    std::env::current_dir()
        .unwrap_or_else(|_| PathBuf::from("."))
        .join("package.json")
}

#[derive(Clone)]
struct Manifest {
    path: PathBuf,
    root: Value,
    deps: Map<String, Value>,
}

impl Manifest {
    fn load() -> Result<Manifest, String> {
        let path = manifest_path();
        if !path.is_file() {
            return Err(format!(
                "no package.json found in {} (run `wave init` first)",
                path.parent().unwrap_or(Path::new(".")).display()
            ));
        }
        let text = std::fs::read_to_string(&path)
            .map_err(|e| format!("cannot read {}: {}", path.display(), e))?;
        let root: Value = serde_json::from_str(&text)
            .map_err(|e| format!("{} is not valid JSON: {}", path.display(), e))?;
        validate_schema(&root)?;
        let deps = root
            .get("dependencies")
            .cloned()
            .unwrap_or_else(|| Value::Object(Map::new()));
        let deps = deps
            .as_object()
            .cloned()
            .ok_or_else(|| "`dependencies` must be an object".to_string())?;
        Ok(Manifest { path, root, deps })
    }

    fn save(&self) -> Result<(), String> {
        let mut root = self.root.clone();
        root["dependencies"] = Value::Object(sorted_map(&self.deps));
        let text = serde_json::to_string_pretty(&root)
            .map_err(|e| format!("cannot serialize package.json: {}", e))?;
        std::fs::write(&self.path, text + "\n")
            .map_err(|e| format!("cannot write {}: {}", self.path.display(), e))
    }
}

/// Minimal schema validation: name (string), version (string),
/// dependencies (object of string → version string).
fn validate_schema(root: &Value) -> Result<(), String> {
    let obj = root
        .as_object()
        .ok_or_else(|| "package.json must contain a JSON object".to_string())?;
    match obj.get("name") {
        Some(Value::String(n)) if !n.trim().is_empty() => {}
        _ => return Err("package.json needs a non-empty string \"name\"".into()),
    }
    match obj.get("version") {
        Some(Value::String(v)) if !v.trim().is_empty() => {}
        _ => return Err("package.json needs a non-empty string \"version\"".into()),
    }
    if let Some(d) = obj.get("dependencies") {
        let map = d
            .as_object()
            .ok_or_else(|| "\"dependencies\" must be an object".to_string())?;
        for (k, v) in map {
            if !v.is_string() {
                return Err(format!(
                    "dependency '{}' must map to a version string, e.g. \"{}\": \"1.0.0\"",
                    k, k
                ));
            }
        }
    }
    Ok(())
}

/// serde_json sorts nothing by itself; keep deps in sorted order for stable diffs.
fn sorted_map(map: &Map<String, Value>) -> Map<String, Value> {
    let mut keys: Vec<&String> = map.keys().collect();
    keys.sort();
    keys.into_iter()
        .map(|k| (k.clone(), map[k].clone()))
        .collect()
}

/// Entry point resolution: explicit file arg → "main" field → src/main.tsm.
fn entry_path(explicit: Option<&str>, manifest: Option<&Manifest>) -> Result<PathBuf, String> {
    if let Some(f) = explicit {
        let p = PathBuf::from(f);
        return if p.is_file() {
            Ok(p)
        } else {
            Err(format!("file not found: {}", p.display()))
        };
    }
    let main_field = manifest
        .and_then(|m| m.root.get("main"))
        .and_then(|v| v.as_str())
        .unwrap_or("src/main.tsm");
    let p = PathBuf::from(main_field);
    if p.is_file() {
        Ok(p)
    } else {
        Err(format!(
            "entry point '{}' not found (set \"main\" in package.json or pass a file)",
            p.display()
        ))
    }
}

/// Shared front half of check/build/run: validate file conventions + read.
fn load_source(path: &Path) -> Result<(String, String, String), i32> {
    if let Err(msg) = pipeline::validate_path(path) {
        eprintln!("error: {}", msg);
        return Err(1);
    }
    let src = match std::fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("error: cannot read {}: {}", path.display(), e);
            return Err(1);
        }
    };
    let file = path.display().to_string();
    let stem = path
        .file_stem()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "program".to_string());
    Ok((src, file, stem))
}

fn print_fail(file: &str, f: Fail) -> i32 {
    match f {
        Fail::Diags(diags) => {
            pipeline::print_diags(file, &diags);
            eprintln!();
            eprintln!("compilation failed: {} error(s)", diags.len());
            1
        }
        Fail::Tool(msg) => {
            eprintln!("error: {}", msg);
            1
        }
    }
}

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------

fn cmd_init(rest: &[String]) -> i32 {
    if !rest.is_empty() {
        eprintln!("usage: wave init");
        return 2;
    }
    let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    let name = cwd
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "my-project".to_string());
    // package names: kebab-case, like files
    let name = name.to_lowercase().replace([' ', '_'], "-");

    let manifest = manifest_path();
    if manifest.is_file() {
        eprintln!("error: package.json already exists here");
        return 1;
    }
    let pkg = serde_json::json!({
        "name": name,
        "version": "0.0.1",
        "dependencies": {}
    });
    let text = serde_json::to_string_pretty(&pkg).expect("serialize package.json");
    if let Err(e) = std::fs::write(&manifest, text + "\n") {
        eprintln!("error: cannot write {}: {}", manifest.display(), e);
        return 1;
    }

    let src_dir = cwd.join("src");
    let main_file = src_dir.join("main.tsm");
    if !main_file.exists() {
        std::fs::create_dir_all(&src_dir).expect("create src/");
        std::fs::write(
            &main_file,
            "// entry point of the program\nasync function main() {\n  console.log(\"hello from wave\")\n}\n",
        )
        .expect("write src/main.tsm");
    }
    println!("created package.json");
    println!("created src/main.tsm");
    println!("next: wave run");
    0
}

fn cmd_add(rest: &[String]) -> i32 {
    let pkg = match rest.first() {
        Some(p) => p,
        None => {
            eprintln!("usage: wave add <path|git-url|std-module>");
            return 2;
        }
    };
    // std modules are built into the compiler/runtime — nothing to install
    if matches!(pkg.as_str(), "net" | "http" | "fs" | "crypto" | "sql") {
        println!(
            "'{}' is a built-in std module — available with `use {}`",
            pkg, pkg
        );
        return 0;
    }
    // an explicit version suffix is honored for source packages
    let (source, explicit_version) = match pkg.rsplit_once('@') {
        Some((s, v))
            if !s.is_empty()
                && v.chars().next().map(|c| c.is_ascii_digit()).unwrap_or(false) =>
        {
            (s.to_string(), Some(v.to_string()))
        }
        _ => (pkg.clone(), None),
    };

    let manifest = Manifest::load().ok();

    // local directory package
    let path = PathBuf::from(&source);
    if path.is_dir() {
        return install_from_dir(&path, explicit_version, manifest.as_ref());
    }

    // git URL: shallow-clone into a temp dir, then install like a local package
    if source.starts_with("http://") || source.starts_with("https://") || source.ends_with(".git") {
        let tmp = std::env::temp_dir().join(format!("wave-install-{}", pipeline::md5ish(&source)));
        if tmp.exists() {
            let _ = std::fs::remove_dir_all(&tmp);
        }
        eprintln!("cloning {}...", source);
        let st = std::process::Command::new("git")
            .args(["clone", "--depth", "1", &source])
            .arg(&tmp)
            .status();
        match st {
            Ok(s) if s.success() => {}
            _ => {
                eprintln!("error: git clone failed for {}", source);
                return 1;
            }
        }
        let code = install_from_dir(&tmp, explicit_version, manifest.as_ref());
        let _ = std::fs::remove_dir_all(&tmp);
        return code;
    }

    eprintln!("error: unknown package '{}'", source);
    eprintln!("  std modules net, http, fs, crypto and sql are built-in (`use fs`, `use crypto`, ...)");
    eprintln!("  install source packages from a local path or a git URL:");
    eprintln!("    wave add ./my-pkg");
    eprintln!("    wave add https://github.com/user/my-pkg.git");
    1
}

/// Install a package directory into the global registry
/// (~/.wave/registry/<name>/<version>/) and record it as a dependency of the
/// current project when a package.json is present.
fn install_from_dir(dir: &Path, explicit_version: Option<String>, manifest: Option<&Manifest>) -> i32 {
    let main_tsm = dir.join("main.tsm");
    if !main_tsm.is_file() {
        eprintln!("error: {} has no main.tsm (packages need an entry file)", dir.display());
        return 1;
    }
    // package metadata comes from the package's own package.json when present
    let mut pkg_name: Option<String> = None;
    let mut pkg_version: Option<String> = None;
    if let Ok(text) = std::fs::read_to_string(dir.join("package.json")) {
        if let Ok(v) = serde_json::from_str::<Value>(&text) {
            pkg_name = v.get("name").and_then(|n| n.as_str()).map(|s| s.to_string());
            pkg_version = v.get("version").and_then(|n| n.as_str()).map(|s| s.to_string());
        }
    }
    let name = match pkg_name {
        Some(n) => n,
        None => match dir.file_name().and_then(|n| n.to_str()) {
            Some(n) => n.to_string(),
            None => {
                eprintln!("error: cannot determine a package name for {}", dir.display());
                return 1;
            }
        },
    };
    let version = explicit_version
        .or(pkg_version)
        .unwrap_or_else(|| "0.1.0".to_string());
    if !marina::checker::is_kebab_case(&name) {
        eprintln!(
            "error: package names must be kebab-case (lowercase words separated by '-'), got '{}'",
            name
        );
        return 1;
    }

    let dest = pipeline::registry_root().join(&name).join(&version);
    if let Err(e) = copy_dir_recursive(dir, &dest) {
        eprintln!("error: cannot install to {}: {}", dest.display(), e);
        return 1;
    }

    if let Some(m) = manifest {
        if m.deps.get(&name).and_then(|v| v.as_str()) != Some(version.as_str()) {
            let mut deps = m.deps.clone();
            deps.insert(name.clone(), Value::String(version.clone()));
            if let Err(e) = (Manifest { deps, ..m.clone() }).save() {
                eprintln!("error: {}", e);
                return 1;
            }
        }
        println!("added {}@{} to dependencies", name, version);
    }
    println!("installed {}@{} into {}", name, version, dest.display());
    println!("use it with `use {}`", name);
    0
}

fn copy_dir_recursive(from: &Path, to: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(to)?;
    for entry in std::fs::read_dir(from)? {
        let entry = entry?;
        let ty = entry.file_type()?;
        let dest = to.join(entry.file_name());
        if ty.is_dir() {
            copy_dir_recursive(&entry.path(), &dest)?;
        } else {
            std::fs::copy(entry.path(), &dest)?;
        }
    }
    Ok(())
}

fn cmd_remove(rest: &[String]) -> i32 {
    let name = match rest.first() {
        Some(p) => p,
        None => {
            eprintln!("usage: wave remove <package>");
            return 2;
        }
    };
    let m = match Manifest::load() {
        Ok(m) => m,
        Err(e) => {
            eprintln!("error: {}", e);
            return 1;
        }
    };
    if !m.deps.contains_key(name) {
        eprintln!("error: '{}' is not in dependencies", name);
        return 1;
    }
    let mut deps = m.deps.clone();
    deps.remove(name);
    let m2 = Manifest { deps, ..m };
    if let Err(e) = m2.save() {
        eprintln!("error: {}", e);
        return 1;
    }
    println!("removed {} from dependencies", name);
    0
}

fn cmd_check(rest: &[String]) -> i32 {
    // with an explicit file, check works outside a project too; without one,
    // `wave check` doubles as a dependency sanity check
    let m: Option<Manifest> = match Manifest::load() {
        Ok(m) => Some(m),
        Err(e) => {
            if rest.is_empty() {
                eprintln!("error: {}", e);
                return 1;
            }
            None
        }
    };
    if let Some(m) = &m {
        println!(
            "package.json ok: {} v{} ({} dependencies)",
            m.root["name"].as_str().unwrap_or("?"),
            m.root["version"].as_str().unwrap_or("?"),
            m.deps.len()
        );
    }

    let entry = match entry_path(rest.first().map(|s| s.as_str()), m.as_ref()) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("error: {}", e);
            return 1;
        }
    };
    let (src, file, _stem) = match load_source(&entry) {
        Ok(v) => v,
        Err(c) => return c,
    };
    match pipeline::check_source(&src) {
        Ok(()) => {
            println!("check passed: {}", file);
            0
        }
        Err(diags) => {
            pipeline::print_diags(&file, &diags);
            eprintln!();
            eprintln!("check failed: {} error(s)", diags.len());
            1
        }
    }
}

fn cmd_build(rest: &[String]) -> i32 {
    // with an explicit file, build works outside a project too
    let m: Option<Manifest> = match Manifest::load() {
        Ok(m) => Some(m),
        Err(e) => {
            if rest.is_empty() {
                eprintln!("error: {}", e);
                return 1;
            }
            None
        }
    };
    let entry = match entry_path(rest.first().map(|s| s.as_str()), m.as_ref()) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("error: {}", e);
            return 1;
        }
    };
    let (src, file, stem) = match load_source(&entry) {
        Ok(v) => v,
        Err(c) => return c,
    };
    let out_dir = std::env::current_dir().unwrap().join(".marina");
    match pipeline::compile_source(&src, &stem, &out_dir) {
        Ok(a) => {
            println!("built {}", a.bin_path.display());
            println!("llvm ir: {}", a.ll_path.display());
            0
        }
        Err(f) => print_fail(&file, f),
    }
}

fn cmd_run(rest: &[String]) -> i32 {
    let m: Option<Manifest> = match Manifest::load() {
        Ok(m) => Some(m),
        Err(e) => {
            // `wave run <file>` also works without a project (compile + run one-off files)
            if rest.is_empty() {
                eprintln!("error: {}", e);
                return 1;
            }
            None
        }
    };
    let entry = match entry_path(rest.first().map(|s| s.as_str()), m.as_ref()) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("error: {}", e);
            return 1;
        }
    };
    let (src, file, stem) = match load_source(&entry) {
        Ok(v) => v,
        Err(c) => return c,
    };
    let h = format!("{:x}", pipeline::md5ish(&src));
    let out_dir = std::env::temp_dir().join(format!("marina-run-{}", h));
    match pipeline::compile_source(&src, &stem, &out_dir) {
        Ok(a) => pipeline::run_binary(&a.bin_path),
        Err(f) => print_fail(&file, f),
    }
}