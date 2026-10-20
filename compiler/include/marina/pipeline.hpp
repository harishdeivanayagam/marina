// Compilation pipeline shared by the compiler tools and the `wave` package
// manager: file conventions, front-end (lex/parse/check), LLVM codegen and
// clang linking against the marina runtime.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <marina/ast.hpp>

namespace marina::pipeline {

// A user-facing diagnostic with 1-based line/column (matches compiler output).
struct SrcDiag {
    std::string msg;
    uint32_t line = 0;
    uint32_t col = 0;
};

// Why a compilation failed.
struct Fail {
    enum class Kind { Diags, Tool };
    Kind kind = Kind::Tool;
    // Kind::Diags — type/parse/lex errors (already formatted, ready to print)
    std::vector<SrcDiag> diags;
    // Kind::Tool — toolchain problem (missing clang, io error, …)
    std::string tool;
};

static Fail fail_diags(std::vector<SrcDiag> d) {
    Fail f;
    f.kind = Fail::Kind::Diags;
    f.diags = std::move(d);
    return f;
}
static Fail fail_tool(std::string msg) {
    Fail f;
    f.kind = Fail::Kind::Tool;
    f.tool = std::move(msg);
    return f;
}

// Result of a successful compile.
struct Artifact {
    std::string bin_path;
    std::string ll_path;
};

// Enforce strict file conventions: `.tsm` extension + kebab-case stem.
// Returns an error message, or empty on success.
std::optional<std::string> validate_path(const std::string& path);

// Type-check a source string without generating code (`wave check`).
// Returns diagnostics in source order; nullopt means the program passes.
std::optional<std::vector<SrcDiag>> check_source(const std::string& src);

// Full pipeline: lex → parse → check → LLVM IR → clang link with the runtime.
// Writes `<stem>.ll` and the `<stem>` binary into `out_dir` (created if
// needed).
Artifact compile_source(const std::string& src, const std::string& stem,
                        const std::string& out_dir, Fail& fail);

// Run a compiled binary, returning its exit code.
int run_binary(const std::string& bin);

// Print diagnostics the way the compiler does (`file:line:col`).
void print_diags(const std::string& file, const std::vector<SrcDiag>& diags);

// The global package registry root (`WAVE_REGISTRY` overrides it).
// Packages live at `<root>/<name>/<version>/main.tsm` — like Rust crates in
// the crates.io registry, not like node_modules.
std::string registry_root();

// Resolve `use <pkg>` statements in place (mutates the program: merges
// registry packages' functions/classes under `<pkg>__<name>` names).
// Returns diagnostics; empty means success.
std::vector<SrcDiag> resolve_uses(ast::Program& program);

// Cheap stable hash for cache dir naming (not cryptographic).
uint64_t md5ish(const std::string& s);

// Search $PATH for an executable; nullopt when not found.
std::optional<std::string> which(const std::string& cmd);

// Directory that contains this workspace (…/marina), even when invoked from
// an arbitrary project directory (baked in at compile time).
std::string workspace_root();

} // namespace marina::pipeline
