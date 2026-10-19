//! marina — a strictly-typed, ahead-of-time compiled TypeScript subset
//! with ARC memory management and a multithreaded event loop.
//!
//! This is the bare compiler CLI. For day-to-day work use the `wave`
//! package manager (`wave run`, `wave build`, `wave check`, …).
//!
//! Usage:
//!   marina run <file.tsm>     compile + execute
//!   marina build <file.tsm>   compile to a native binary (in ./.marina/)

use marina::pipeline::{self, Fail};
use std::path::PathBuf;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: marina <run|build> <file.tsm>");
        eprintln!("(or use the wave package manager: wave <add|remove|check|build|run>)");
        std::process::exit(2);
    }
    let cmd = args[1].as_str();
    let path = PathBuf::from(&args[2]);
    if cmd != "run" && cmd != "build" {
        eprintln!("usage: marina <run|build> <file.tsm>");
        std::process::exit(2);
    }

    if let Err(msg) = pipeline::validate_path(&path) {
        eprintln!("error: {}", msg);
        std::process::exit(1);
    }
    let src = match std::fs::read_to_string(&path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("error: cannot read {}: {}", path.display(), e);
            std::process::exit(1);
        }
    };
    let file = path.display().to_string();
    let stem = path
        .file_stem()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "program".to_string());

    let dir: PathBuf = match cmd {
        "build" => std::env::current_dir().unwrap().join(".marina"),
        "run" => {
            let h = format!("{:x}", pipeline::md5ish(&src));
            std::env::temp_dir().join(format!("marina-run-{}", h))
        }
        _ => unreachable!(),
    };

    match pipeline::compile_source(&src, &stem, &dir) {
        Ok(artifact) => match cmd {
            "build" => {
                println!("built {}", artifact.bin_path.display());
                println!("llvm ir: {}", artifact.ll_path.display());
            }
            "run" => {
                std::process::exit(pipeline::run_binary(&artifact.bin_path));
            }
            _ => unreachable!(),
        },
        Err(Fail::Diags(diags)) => {
            pipeline::print_diags(&file, &diags);
            eprintln!();
            eprintln!("compilation failed: {} error(s)", diags.len());
            std::process::exit(1);
        }
        Err(Fail::Tool(msg)) => {
            eprintln!("error: {}", msg);
            std::process::exit(1);
        }
    }
}