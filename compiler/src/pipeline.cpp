// Compilation pipeline implementation — port of pipeline.rs.
#include <marina/pipeline.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

#include <marina/checker.hpp>
#include <marina/codegen.hpp>
#include <marina/lexer.hpp>
#include <marina/parser.hpp>

namespace marina::pipeline {

namespace fs = std::filesystem;

namespace {

SrcDiag diag_from(const std::string& msg, lex::Span span) {
    SrcDiag d;
    d.msg = msg;
    d.line = span.line;
    d.col = span.col;
    return d;
}

std::vector<SrcDiag> checker_diags(const std::vector<ir::Diag>& ds) {
    std::vector<SrcDiag> out;
    out.reserve(ds.size());
    for (const auto& d : ds) out.push_back(diag_from(d.msg, d.span));
    return out;
}

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Mangled-name prefix for module path segments: {sample_mod, sub} →
/// `sample_mod__sub`.
std::string prefix_of(const std::vector<std::string>& segs) {
    std::string p;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i) p += "__";
        p += segs[i];
    }
    return p;
}

} // namespace

std::optional<std::string> validate_path(const std::string& path) {
    fs::path p(path);
    if (p.extension() == ".tsm") {
        std::string stem = p.stem().string();
        if (!ir::is_kebab_case(stem)) {
            return "file names must be kebab-case (lowercase words separated by '-'), got '" +
                   stem + "'";
        }
        return std::nullopt;
    }
    std::string ext = p.extension().string();
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    return "marina programs use the .tsm extension (ts marina), got '" + path +
           "' (ext: \"" + ext + "\")";
}

std::optional<std::vector<SrcDiag>> check_source(const std::string& src,
                                                 const std::string& base_dir) {
    std::vector<lex::Token> tokens;
    try {
        tokens = lex::lex(src);
    } catch (const lex::LexError& e) {
        return std::vector<SrcDiag>{diag_from(e.msg, e.span)};
    }
    ast::Program program;
    try {
        program = ast::parse(std::move(tokens));
    } catch (const ast::ParseError& e) {
        return std::vector<SrcDiag>{diag_from(e.msg, e.span)};
    }
    auto use_diags = resolve_uses(program, base_dir);
    if (!use_diags.empty()) return use_diags;
    try {
        ir::check(program);
    } catch (const ir::CheckDiags& e) {
        return checker_diags(e.diags);
    }
    return std::nullopt;
}

std::optional<std::string> which(const std::string& cmd) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return std::nullopt;
    std::istringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) dir = ".";
        fs::path p = fs::path(dir) / cmd;
        if (fs::is_regular_file(p)) return p.string();
    }
    return std::nullopt;
}

std::string workspace_root() {
#ifdef MARINA_WORKSPACE_ROOT
    return MARINA_WORKSPACE_ROOT;
#else
    return ".";
#endif
}

// The host's LLVM target triple (e.g. arm64-apple-macosx26.0.0), asked from
// clang once and cached. The generated .ll would otherwise have an empty
// module triple, making every clang invocation print a -Woverride-module
// warning. Returns "" if the triple cannot be determined.
std::string host_triple() {
    static const std::string cached = [] {
        std::optional<std::string> clang = which("clang");
        for (const char* c : {"clang-21", "clang-20", "clang-18", "cc"}) {
            if (!clang) clang = which(c);
        }
        if (!clang) return std::string();
        std::string cmd = "'" + *clang + "' -print-target-triple 2>/dev/null";
        std::string out;
        if (std::FILE* p = ::popen(cmd.c_str(), "r")) {
            char buf[256];
            size_t n = std::fread(buf, 1, sizeof buf, p);
            ::pclose(p);
            out.assign(buf, n);
            while (!out.empty() &&
                   (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
                out.pop_back();
            // keep only the first line
            size_t nl = out.find('\n');
            if (nl != std::string::npos) out.erase(nl);
            if (out.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") !=
                std::string::npos)
                return std::string(); // unexpected output; treat as unknown
            return out;
        }
        return std::string();
    }();
    return cached;
}

namespace {

std::optional<std::string> locate_runtime() {
#ifdef MARINA_RUNTIME_LIB
    if (fs::is_regular_file(MARINA_RUNTIME_LIB)) return MARINA_RUNTIME_LIB;
#endif
    if (const char* env = std::getenv("MARINA_RUNTIME_LIB")) {
        if (fs::is_regular_file(env)) return env;
    }
    fs::path candidate = fs::path(workspace_root()) / "build" / "runtime" /
                         "libmarina_runtime.a";
    if (fs::is_regular_file(candidate)) return candidate.string();
    return std::nullopt;
}

bool build_runtime() {
    std::cerr << "building marina runtime..." << std::endl;
    fs::path root = workspace_root();
    fs::path build_dir = root / "build";
    std::error_code ec;
    fs::create_directories(build_dir, ec);
    // configure once
    if (!fs::exists(build_dir / "CMakeCache.txt")) {
        std::string cmd = "cmake -S '" + root.string() + "' -B '" +
                          build_dir.string() + "'";
        if (std::system(cmd.c_str()) != 0) return false;
    }
    std::string cmd = "cmake --build '" + build_dir.string() +
                      "' --target marina_runtime";
    return std::system(cmd.c_str()) == 0;
}

} // namespace

Artifact compile_source(const std::string& src, const std::string& stem,
                        const std::string& out_dir, Fail& fail,
                        const std::string& base_dir) {
    std::vector<lex::Token> tokens;
    try {
        tokens = lex::lex(src);
    } catch (const lex::LexError& e) {
        fail = fail_diags({diag_from(e.msg, e.span)});
        return {};
    }
    ast::Program program;
    try {
        program = ast::parse(std::move(tokens));
    } catch (const ast::ParseError& e) {
        fail = fail_diags({diag_from(e.msg, e.span)});
        return {};
    }
    auto use_diags = resolve_uses(program, base_dir);
    if (!use_diags.empty()) {
        fail = fail_diags(std::move(use_diags));
        return {};
    }
    ir::Checked checked;
    try {
        checked = ir::check(program);
    } catch (const ir::CheckDiags& e) {
        fail = fail_diags(checker_diags(e.diags));
        return {};
    }
    std::string ir_text = codegen::generate(checked);

    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        fail = fail_tool("cannot create " + out_dir + ": " + ec.message());
        return {};
    }
    fs::path ll_path = fs::path(out_dir) / (stem + ".ll");
    fs::path bin_path = fs::path(out_dir) / stem;
    {
        std::ofstream out(ll_path, std::ios::binary);
        if (!out) {
            fail = fail_tool("cannot write " + ll_path.string());
            return {};
        }
        // Pin the module to the host triple: without a `target triple` line
        // the module's triple is empty and clang prints a -Woverride-module
        // warning for every compile.
        std::string triple = host_triple();
        bool has_triple = ir_text.find("target triple") != std::string::npos;
        if (!triple.empty() && !has_triple) {
            out << "target triple = \"" << triple << "\"\n\n";
        }
        out << ir_text;
        out.close();
        if (!out) {
            fail = fail_tool("cannot write " + ll_path.string());
            return {};
        }
    }

    auto runtime = locate_runtime();
    if (!runtime) {
        if (!build_runtime()) {
            fail = fail_tool("failed to build the marina runtime (cmake)");
            return {};
        }
        runtime = locate_runtime();
        if (!runtime) {
            fail = fail_tool("runtime library not found after build");
            return {};
        }
    }

    std::string clang = "clang";
    for (const char* c : {"clang", "clang-21", "clang-20", "clang-18", "cc"}) {
        if (which(c)) {
            clang = c;
            break;
        }
    }
    // the runtime contains the sql module (sqlite FFI), so libsqlite3 is
    // always required (macOS ships it; Linux needs the sqlite3 package);
    // the C++ runtime also needs the C++ standard library and pthreads.
    // -Wno-override-module: the driver retargets the module to the host
    // triple (macOS derives e.g. arm64-apple-macosx26.0.0 from the SDK);
    // that override is intended, so don't warn about it.
    std::string cmd = "'" + clang + "' -O2 -Wno-override-module '" + ll_path.string() + "' '" +
                      *runtime + "' -lsqlite3 -lc++ -lpthread -o '" +
                      bin_path.string() + "'";
    int status = std::system(cmd.c_str());
    if (status != 0) {
        fail = fail_tool("linking failed (clang). LLVM IR kept at " + ll_path.string());
        return {};
    }
    Artifact a;
    a.bin_path = bin_path.string();
    a.ll_path = ll_path.string();
    return a;
}

int run_binary(const std::string& bin) {
    int status = std::system(("'" + bin + "'").c_str());
    if (status == -1 || !WIFEXITED(status)) {
        std::cerr << "error: cannot run " << bin << std::endl;
        return 1;
    }
    return WEXITSTATUS(status);
}

void print_diags(const std::string& file, const std::vector<SrcDiag>& diags) {
    for (const auto& d : diags) {
        std::cerr << "error: " << d.msg << std::endl;
        std::cerr << "  --> " << file << ":" << d.line << ":" << d.col << std::endl;
    }
}

std::string registry_root() {
    if (const char* r = std::getenv("WAVE_REGISTRY")) return r;
    const char* home = std::getenv("HOME");
    std::string h = home ? home : ".";
    return (fs::path(h) / ".wave" / "registry").string();
}

namespace {

// Sort key for `MAJOR.MINOR.PATCH` versions (falls back to lexicographic).
std::vector<uint64_t> version_key(const std::string& v) {
    std::vector<uint64_t> key;
    std::istringstream ss(v);
    std::string part;
    while (std::getline(ss, part, '.')) {
        try {
            key.push_back(std::stoull(part));
        } catch (...) {
            key.push_back(0);
        }
    }
    return key;
}

// Rewrite unqualified intra-package calls in a package's functions: an
// identifier call target that names one of the package's own functions
// becomes `<pkg>__<name>` (package functions are renamed on merge, and the
// checker only resolves qualified `pkg.fn(...)` calls across modules).
void qual_expr(ast::ExprPtr& e, const std::set<std::string>& local,
               const std::string& pkg);

void qual_callee(ast::ExprPtr& callee, const std::set<std::string>& local,
                 const std::string& pkg) {
    qual_expr(callee, local, pkg);
    if (auto* id = std::get_if<ast::EIdent>(&callee->v)) {
        if (local.count(id->name)) {
            id->name = pkg + "__" + id->name;
        }
    }
}

void qual_stmt(ast::Stmt& s, const std::set<std::string>& local,
               const std::string& pkg) {
    if (auto* x = std::get_if<ast::SLet>(&s.v)) {
        qual_expr(x->init, local, pkg);
    } else if (auto* x = std::get_if<ast::SAssign>(&s.v)) {
        qual_expr(x->value, local, pkg);
    } else if (auto* x = std::get_if<ast::SMemberAssign>(&s.v)) {
        qual_expr(x->value, local, pkg);
    } else if (auto* x = std::get_if<ast::SIndexAssign>(&s.v)) {
        qual_expr(x->index, local, pkg);
        qual_expr(x->value, local, pkg);
    } else if (auto* x = std::get_if<ast::SReturn>(&s.v)) {
        if (x->value) qual_expr(*x->value, local, pkg);
    } else if (auto* x = std::get_if<ast::SIf>(&s.v)) {
        qual_expr(x->cond, local, pkg);
        for (auto& t : x->then_body) qual_stmt(t, local, pkg);
        if (x->else_body) {
            for (auto& t : *x->else_body) qual_stmt(t, local, pkg);
        }
    } else if (auto* x = std::get_if<ast::SWhile>(&s.v)) {
        qual_expr(x->cond, local, pkg);
        for (auto& t : x->body) qual_stmt(t, local, pkg);
    } else if (auto* x = std::get_if<ast::SFor>(&s.v)) {
        if (x->init) qual_stmt(**x->init, local, pkg);
        if (x->cond) qual_expr(*x->cond, local, pkg);
        if (x->update) qual_stmt(**x->update, local, pkg);
        for (auto& t : x->body) qual_stmt(t, local, pkg);
    } else if (auto* x = std::get_if<ast::SExpr>(&s.v)) {
        qual_expr(x->expr, local, pkg);
    }
}

void qual_expr(ast::ExprPtr& e, const std::set<std::string>& local,
               const std::string& pkg) {
    if (auto* x = std::get_if<ast::ETemplate>(&e->v)) {
        for (auto& p : x->parts) {
            if (auto* tp = std::get_if<ast::TExpr>(&p)) {
                qual_expr(tp->expr, local, pkg);
            }
        }
    } else if (auto* x = std::get_if<ast::ETagged>(&e->v)) {
        qual_expr(x->tag, local, pkg);
        for (auto& p : x->parts) {
            if (auto* tp = std::get_if<ast::TExpr>(&p)) {
                qual_expr(tp->expr, local, pkg);
            }
        }
    } else if (auto* x = std::get_if<ast::EUnary>(&e->v)) {
        qual_expr(x->operand, local, pkg);
    } else if (auto* x = std::get_if<ast::EBinary>(&e->v)) {
        qual_expr(x->lhs, local, pkg);
        qual_expr(x->rhs, local, pkg);
    } else if (auto* x = std::get_if<ast::ECall>(&e->v)) {
        qual_callee(x->callee, local, pkg);
        for (auto& a : x->args) qual_expr(a, local, pkg);
    } else if (auto* x = std::get_if<ast::ECallGen>(&e->v)) {
        qual_callee(x->callee, local, pkg);
        for (auto& a : x->args) qual_expr(a, local, pkg);
    } else if (auto* x = std::get_if<ast::EMember>(&e->v)) {
        qual_expr(x->obj, local, pkg);
    } else if (auto* x = std::get_if<ast::EIndex>(&e->v)) {
        qual_expr(x->obj, local, pkg);
        qual_expr(x->index, local, pkg);
    } else if (auto* x = std::get_if<ast::EArrayLit>(&e->v)) {
        for (auto& x2 : x->elems) qual_expr(x2, local, pkg);
    } else if (auto* x = std::get_if<ast::EObjectLit>(&e->v)) {
        for (auto& f : x->fields) qual_expr(f.value, local, pkg);
    } else if (auto* x = std::get_if<ast::ENewCtor>(&e->v)) {
        for (auto& a : x->args) qual_expr(a, local, pkg);
    } else if (auto* x = std::get_if<ast::EAwait>(&e->v)) {
        qual_expr(x->operand, local, pkg);
    } else if (auto* x = std::get_if<ast::ENewPromise>(&e->v)) {
        qual_expr(x->executor, local, pkg);
    } else if (auto* x = std::get_if<ast::ELambda>(&e->v)) {
        for (auto& s : x->lambda->body) qual_stmt(s, local, pkg);
    }
    // EInt, EFloat, EBool, EStr, EIdent, ENewClass: nothing to do
}

bool load_registry_package(const std::string& name, std::string& out_src,
                           std::string& out_err) {
    if (!ir::is_kebab_case(name)) {
        out_err = "invalid module name '" + name + "'";
        return false;
    }
    fs::path pkg_dir = fs::path(registry_root()) / name;
    std::error_code ec;
    if (!fs::is_directory(pkg_dir)) {
        out_err = "module '" + name + "' is not installed (run `wave add " + name + "`)";
        return false;
    }
    std::vector<std::string> versions;
    for (const auto& entry : fs::directory_iterator(pkg_dir, ec)) {
        if (entry.is_directory()) versions.push_back(entry.path().filename().string());
    }
    if (versions.empty()) {
        out_err = "package '" + name + "' has no installed versions";
        return false;
    }
    std::sort(versions.begin(), versions.end(), [](const std::string& a, const std::string& b) {
        return version_key(a) < version_key(b);
    });
    const std::string& latest = versions.back();
    auto main = read_file(pkg_dir / latest / "main.tsm");
    if (!main) {
        out_err = "package '" + name + "' v" + latest + " is missing main.tsm";
        return false;
    }
    out_src = *main;
    return true;
}

} // namespace

std::vector<SrcDiag> resolve_uses(ast::Program& program, const std::string& base_dir_str) {
    std::vector<SrcDiag> diags;
    std::vector<std::pair<std::string, lex::Span>> kept;
    // hoisted module declarations, merged in front of the program's own
    // functions so their (inferred) return types are known first
    std::vector<ast::FnDecl> extra_fns;
    std::vector<ast::ClassDecl> extra_classes;
    std::set<std::string> merged;
    std::set<std::string> seen_aliases;
    // module files already merged (a module can be reached through several
    // use paths: `use sample_mod` and `use sample_mod/sub`)
    std::set<std::string> loaded_files;
    fs::path base_dir(base_dir_str);
    // worklist seeded with the program's own uses
    std::vector<std::pair<std::string, lex::Span>> queue = std::move(program.uses);

    // Load a local folder module: every .tsm file directly inside the folder
    // is part of the module, and each subfolder becomes a nested namespace
    // (`use sample_mod` exposes calc.tsm's class `Calc` as `sample_mod.Calc`
    // and sub/sub.tsm's function `sub` as `sample_mod.sub.sub`). Functions
    // are renamed to their `__`-joined path from base_dir, and the use alias
    // maps to that prefix via program.use_prefixes.
    std::function<void(const fs::path&, const std::vector<std::string>&)> load_dir =
        [&](const fs::path& dir, const std::vector<std::string>& segs) {
            std::error_code ec;
            std::vector<fs::path> files;
            std::vector<fs::path> subdirs;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (entry.is_directory()) {
                    // skip hidden/build directories
                    const std::string fn = entry.path().filename().string();
                    if (!fn.empty() && fn[0] != '.') subdirs.push_back(entry.path());
                } else if (entry.path().extension() == ".tsm") {
                    files.push_back(entry.path());
                }
            }
            std::sort(files.begin(), files.end());
            std::sort(subdirs.begin(), subdirs.end());
            std::string prefix = prefix_of(segs);
            for (const auto& file : files) {
                std::string canon = fs::weakly_canonical(file, ec).string();
                if (!canon.empty() && !loaded_files.insert(canon).second) continue; // already merged
                auto src = read_file(file);
                if (!src) {
                    diags.push_back(diag_from("cannot read module file '" + file.string() + "'",
                                              lex::Span{}));
                    continue;
                }
                std::string rel = fs::relative(file, base_dir, ec).string();
                try {
                    auto tokens = lex::lex(*src);
                    ast::Program mod = ast::parse(std::move(tokens));
                    if (!mod.stmts.empty()) {
                        diags.push_back(diag_from(
                            "module file '" + rel +
                                "' cannot have top-level statements (functions and classes only)",
                            ast::stmt_span(mod.stmts[0])));
                    }
                    for (const auto& f : mod.fns) {
                        if (f.name == "main") {
                            diags.push_back(diag_from(
                                "module file '" + rel + "' must not define main (it is a library)",
                                f.span));
                        }
                    }
                    // rewrite unqualified intra-module calls to their mangled
                    // names (module functions are renamed on merge)
                    {
                        std::set<std::string> local;
                        for (const auto& f : mod.fns) local.insert(f.name);
                        for (auto& f : mod.fns) {
                            for (auto& s : f.body) qual_stmt(s, local, prefix);
                        }
                    }
                    // the module's own `use` statements are resolved against
                    // the same base directory
                    for (auto& u : mod.uses) queue.push_back(std::move(u));
                    mod.uses.clear();
                    for (auto& f : mod.fns) {
                        f.name = prefix + "__" + f.name;
                        extra_fns.push_back(std::move(f));
                    }
                    for (auto& c : mod.classes) extra_classes.push_back(std::move(c));
                } catch (const lex::LexError& e) {
                    diags.push_back(diag_from("module file '" + rel + "': " + e.msg, e.span));
                } catch (const ast::ParseError& e) {
                    diags.push_back(diag_from("module file '" + rel + "': " + e.msg, e.span));
                }
            }
            for (const auto& sub : subdirs) {
                std::vector<std::string> sub_segs = segs;
                sub_segs.push_back(sub.filename().string());
                load_dir(sub, sub_segs);
            }
        };

    size_t qi = 0;
    while (qi < queue.size()) {
        auto [name, span] = queue[qi];
        qi += 1;
        bool is_std = name == "net" || name == "http" || name == "fs" ||
                      name == "crypto" || name == "sql";
        bool already_kept = std::any_of(kept.begin(), kept.end(),
                                        [&](const auto& k) { return k.first == name; });
        if (is_std) {
            if (!already_kept) kept.emplace_back(name, span);
            continue;
        }
        if (merged.count(name)) {
            if (!already_kept) kept.emplace_back(name, span);
            continue;
        }
        // local folder module: a directory relative to the entry file's
        // directory (`use sample_mod`, `use sample_mod/sub`)
        std::vector<std::string> segs;
        bool path_ok = !name.empty();
        {
            size_t start = 0;
            while (path_ok) {
                size_t slash = name.find('/', start);
                std::string seg = name.substr(start, slash == std::string::npos
                                                         ? std::string::npos
                                                         : slash - start);
                if (seg.empty() || seg.find("__") != std::string::npos) path_ok = false;
                segs.push_back(seg);
                if (slash == std::string::npos) break;
                start = slash + 1;
            }
        }
        if (!path_ok || segs.empty() || segs.back().empty()) {
            diags.push_back(diag_from("invalid module name '" + name + "'", span));
            continue;
        }
        std::string alias = segs.back();
        if (!base_dir_str.empty()) {
            fs::path dir = base_dir;
            for (const auto& s : segs) dir /= s;
            std::error_code ec;
            if (fs::is_directory(dir, ec)) {
                if (seen_aliases.count(alias)) {
                    diags.push_back(diag_from("duplicate module alias '" + alias + "'", span));
                    continue;
                }
                merged.insert(name);
                seen_aliases.insert(alias);
                load_dir(dir, segs);
                kept.emplace_back(alias, span);
                program.use_prefixes.emplace_back(alias, prefix_of(segs));
                continue;
            }
            if (name.find('/') != std::string::npos) {
                diags.push_back(diag_from("module '" + name + "' not found: no directory '" +
                                              dir.string() + "'",
                                          span));
                continue;
            }
        }
        std::string pkg_src, pkg_err;
        if (!load_registry_package(name, pkg_src, pkg_err)) {
            diags.push_back(diag_from(pkg_err, span));
            continue;
        }
        try {
            auto tokens = lex::lex(pkg_src);
            ast::Program pkg = ast::parse(std::move(tokens));
            if (!pkg.stmts.empty()) {
                diags.push_back(diag_from(
                    "package '" + name +
                        "' cannot have top-level statements (functions and classes only)",
                    ast::stmt_span(pkg.stmts[0])));
            }
            for (const auto& f : pkg.fns) {
                if (f.name == "main") {
                    diags.push_back(diag_from(
                        "package '" + name +
                            "' must not define main (it is a library)",
                        f.span));
                }
            }
            // mark merged before walking dependencies (cycle guard)
            merged.insert(name);
            if (seen_aliases.count(name)) {
                diags.push_back(diag_from("duplicate module alias '" + name + "'", span));
                continue;
            }
            seen_aliases.insert(name);
            // rewrite unqualified intra-package calls to their mangled
            // names (package functions are renamed on merge)
            {
                std::set<std::string> local;
                for (const auto& f : pkg.fns) local.insert(f.name);
                for (auto& f : pkg.fns) {
                    for (auto& s : f.body) qual_stmt(s, local, name);
                }
            }
            // dependencies first, so a package's deps are always fully
            // merged by the time the checker resolves calls against it
            for (auto& u : pkg.uses) queue.push_back(u);
            pkg.uses.clear();
            for (auto& f : pkg.fns) {
                f.name = name + "__" + f.name;
                extra_fns.push_back(std::move(f));
            }
            for (auto& c : pkg.classes) extra_classes.push_back(std::move(c));
            kept.emplace_back(name, span);
        } catch (const lex::LexError& e) {
            diags.push_back(diag_from("package '" + name + "': " + e.msg, e.span));
        } catch (const ast::ParseError& e) {
            diags.push_back(diag_from("package '" + name + "': " + e.msg, e.span));
        }
    }

    program.uses = std::move(kept);
    // module declarations merge in FRONT of the program's own functions so
    // their (inferred) return types are known by the time the program's
    // functions are checked
    program.fns.insert(program.fns.begin(), std::make_move_iterator(extra_fns.begin()),
                       std::make_move_iterator(extra_fns.end()));
    program.classes.insert(program.classes.begin(), std::make_move_iterator(extra_classes.begin()),
                           std::make_move_iterator(extra_classes.end()));
    return diags;
}

uint64_t md5ish(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char b : s) {
        h ^= static_cast<uint64_t>(b);
        h *= 0x100000001b3ULL;
    }
    return h;
}

} // namespace marina::pipeline
