// Compilation pipeline implementation — port of pipeline.rs.
#include <marina/pipeline.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

std::optional<std::vector<SrcDiag>> check_source(const std::string& src) {
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
    auto use_diags = resolve_uses(program);
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
                        const std::string& out_dir, Fail& fail) {
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
    auto use_diags = resolve_uses(program);
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
    // the C++ runtime also needs the C++ standard library and pthreads
    std::string cmd = "'" + clang + "' -O2 '" + ll_path.string() + "' '" +
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

std::vector<SrcDiag> resolve_uses(ast::Program& program) {
    std::vector<SrcDiag> diags;
    std::vector<std::pair<std::string, lex::Span>> kept;
    std::vector<ast::FnDecl> extra_fns;
    std::vector<ast::ClassDecl> extra_classes;
    std::set<std::string> merged;

    // worklist seeded with the program's own uses (registry order)
    std::vector<std::pair<std::string, lex::Span>> queue = std::move(program.uses);
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
    for (auto& f : extra_fns) program.fns.push_back(std::move(f));
    for (auto& c : extra_classes) program.classes.push_back(std::move(c));
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
