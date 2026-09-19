#include "../include/compiler.h"
#include "../include/string_pool.h"
#include "../include/lexer.h"
#include "../include/parser.h"
#include "../include/semantic.h"
#include "../include/ir_writer.h"
#include "../include/translator.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace cx {

namespace {

struct Module {
    std::string path;     // 规范化绝对路径，作为去重键
    std::string dir;      // 所在目录
    std::string filename; // Location.filename 的稳定存储，须活过 AST
    std::string source;
    std::unique_ptr<CompUnit> cu;
    std::unique_ptr<SemanticAnalyzer> sem;
};

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string canonical_path(const std::string& path) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(path, ec);
    if (ec) return path;
    return c.string();
}

// 解析 import 目标文件路径；返回空表示找不到
std::string resolve_import(const Import& imp, const std::string& cur_dir,
                           const std::string& lib_dir) {
    std::string name(imp.path);
    if (imp.is_string) {
        fs::path p(name);
        if (p.is_absolute()) {
            std::error_code ec;
            return fs::exists(p, ec) ? p.string() : "";
        }
        fs::path rel = fs::path(cur_dir) / p;
        std::error_code ec;
        return fs::exists(rel, ec) ? rel.string() : "";
    }
    if (!lib_dir.empty()) {
        fs::path p = fs::path(lib_dir) / (name + ".cx");
        std::error_code ec;
        if (fs::exists(p, ec)) return p.string();
    }
    fs::path p = fs::path(cur_dir) / (name + ".cx");
    std::error_code ec;
    return fs::exists(p, ec) ? p.string() : "";
}

} // namespace

struct FrontendResult {
    std::vector<std::unique_ptr<Module>> modules;
    bool ok = false;
};

FrontendResult run_frontend(StringPool& pool, const std::string& source,
                            const std::string& filename,
                            const std::string& lib_dir) {
    FrontendResult result;
    std::vector<std::unique_ptr<Module>> modules;
    std::unordered_map<std::string, Module*> by_path;

    auto load = [&](const std::string& src, const std::string& fname) -> Module* {
        auto mod = std::make_unique<Module>();
        mod->path = canonical_path(fname);
        mod->dir = fs::path(mod->path).parent_path().string();
        mod->filename = fname;
        mod->source = src;
        Lexer lexer(mod->source, pool, mod->filename);
        mod->cu = std::make_unique<CompUnit>();
        Parser parser(lexer, *mod->cu, pool);
        parser.parse();
        if (parser.has_error()) {
            for (const Diagnostic& d : parser.errors()) emit_diagnostic(d);
            return nullptr;
        }
        Module* raw = mod.get();
        modules.push_back(std::move(mod));
        by_path[raw->path] = raw;
        return raw;
    };

    Module* main_mod = load(source, filename);
    if (!main_mod) return result;

    // 收集阶段：BFS 展开所有 import（重复/导入自己/循环引用均跳过）
    bool load_failed = false;
    for (size_t i = 0; i < modules.size(); ++i) {
        Module* cur = modules[i].get();
        for (Import* imp : cur->cu->imports) {
            if (!imp) continue;
            std::string target = resolve_import(*imp, cur->dir, lib_dir);
            if (target.empty()) {
                emit_diagnostic(Diagnostic(DiagLevel::Error,
                    "cannot find imported module '" + std::string(imp->path) + "'",
                    imp->loc));
                load_failed = true;
                continue;
            }
            std::string canon = canonical_path(target);
            if (by_path.count(canon)) continue;
            std::string src;
            if (!read_file(canon, src)) {
                emit_diagnostic(Diagnostic(DiagLevel::Error,
                    "cannot read imported module '" + target + "'", imp->loc));
                load_failed = true;
                continue;
            }
            if (!load(src, canon)) load_failed = true;
        }
    }
    if (load_failed) return result;

    // 各模块先只收集符号表
    for (auto& m : modules) {
        m->sem = std::make_unique<SemanticAnalyzer>(*m->cu);
        m->sem->collect_only();
    }
    // 合并为全局符号表（同名先到者优先）
    SymbolTable global;
    for (auto& m : modules) {
        global.merge_from(m->sem->symbol_table());
    }
    // 回填全局符号表后遍历各模块
    bool ok = true;
    for (auto& m : modules) {
        m->sem->merge_external_symbols(global);
        m->sem->analyze_rest();
        for (const Diagnostic& d : m->sem->diagnostics()) emit_diagnostic(d);
        if (m->sem->has_error()) ok = false;
    }

    // 只在主模块检查 main（合并后 main 可来自任意模块）
    if (!modules.empty()) {
        SemanticAnalyzer* msem = modules[0]->sem.get();
        size_t before = msem->diagnostics().size();
        msem->check_main();
        const std::vector<Diagnostic>& ds = msem->diagnostics();
        for (size_t i = before; i < ds.size(); ++i) emit_diagnostic(ds[i]);
        if (ds.size() > before) ok = false;
    }
    result.ok = ok;
    result.modules = std::move(modules);
    return result;
}

bool check(const std::string& source, const std::string& filename,
           const std::string& lib_dir) {
    StringPool pool;
    return run_frontend(pool, source, filename, lib_dir).ok;
}

bool translate(const std::string& source, const std::string& filename,
               const std::string& lib_dir, std::ostream& out) {
    // pool 必须活到翻译结束，否则 AST 中 string_view 悬垂
    StringPool pool;
    FrontendResult r = run_frontend(pool, source, filename, lib_dir);
    if (!r.ok) return false;
    IRWriter ir(out);
    ir.emit_header();
    for (auto& m : r.modules) {
        Translator t(*m->cu, ir);
        t.run();
    }
    ir.flush();
    return true;
}

} // namespace cx
