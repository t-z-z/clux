#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <windows.h>

#include "../include/lexer.h"
#include "../include/parser.h"
#include "../include/translator.h"
#include "../include/semantic.h"

#define VOIC_VERSION "0.1.0"

// ── 路径工具 ──────────────────────────────────────────────────────────

std::string get_exe_dir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string path(buf);
    size_t sep = path.find_last_of("\\/");
    if (sep != std::string::npos)
        path = path.substr(0, sep);
    return path;
}

std::string get_default_std_lib(const std::string& exe_dir) {
    // exe: voic/bin/voic.exe  →  std lib: voic/lib/rust/
    size_t sep = exe_dir.find_last_of("\\/");
    if (sep == std::string::npos) return "";
    std::string parent = exe_dir.substr(0, sep);
    sep = parent.find_last_of("\\/");
    if (sep == std::string::npos) return "";
    return parent.substr(0, sep) + "\\lib\\rust";
}

std::string exe_name(const std::string& path) {
    size_t sep = path.find_last_of("\\/");
    std::string name = (sep == std::string::npos) ? path : path.substr(sep + 1);
    if (name.size() > 3 && name.substr(name.size() - 3) == ".vo")
        name = name.substr(0, name.size() - 3);
    return name;
}

bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

bool remove_file(const std::string& path) {
    return DeleteFileA(path.c_str()) != 0;
}

// ── rustc 检测 ──────────────────────────────────────────────────────

bool has_rustc() {
    int ret = system("rustc --version > nul 2>&1");
    return ret == 0;
}

// ── VOIC → Rust 翻译 ────────────────────────────────────────────────

bool translate_voic(const std::string& source, const std::string& filename,
                    const std::string& rs_path, const std::string& std_lib_path,
                    bool verbose, bool emit_comments = true,
                    std::vector<std::string>* generated_files = nullptr) {
    vo::Lexer lexer(source, filename);
    vo::CompUnit cu;
    vo::Parser parser(lexer, cu);
    parser.parse();

    bool had_error = false;
    if (lexer.has_error()) {
        for (const auto& err : lexer.errors()) {
            vo::emit_diagnostic(err);
            had_error = true;
        }
    }
    if (parser.has_error()) {
        for (const auto& err : parser.errors()) {
            vo::emit_diagnostic(err);
            had_error = true;
        }
    }
    if (had_error)
        return false;

    // 语义分析：parse → semantic → translate
    vo::SemanticAnalyzer semantic(cu);
    semantic.analyze();
    if (semantic.has_error()) {
        for (const auto& err : semantic.diagnostics())
            vo::emit_diagnostic(err);
        if (verbose)
            std::cerr << "[voic] semantic analysis failed" << std::endl;
        return false;
    }

    std::string base_dir;
    size_t sep = filename.find_last_of("\\/");
    if (sep != std::string::npos)
        base_dir = filename.substr(0, sep);

    if (verbose)
        std::cerr << "[voic] translating " << filename << " ..." << std::endl;

    vo::RustTranslator translator(base_dir, std_lib_path);
    translator.set_semantic(&semantic);
    translator.set_emit_comments(emit_comments);
    // import 模块 .rs 写到主输出 .rs 同目录（rustc 按主文件目录解析 mod）
    {
        size_t sep = rs_path.find_last_of("\\/");
        if (sep != std::string::npos)
            translator.set_output_dir(rs_path.substr(0, sep));
    }
    translator.visit(cu);
    std::string rust_code = translator.get_result();

    if (generated_files)
        *generated_files = translator.generated_files();

    std::ofstream out(rs_path);
    if (!out.is_open()) {
        std::cerr << "Error: cannot write " << rs_path << std::endl;
        return false;
    }
    out << rust_code;
    out.close();

    if (verbose)
        std::cerr << "[voic] wrote " << rs_path << std::endl;
    return true;
}

// ── rustc 编译 ───────────────────────────────────────────────────────

bool compile_rustc(const std::string& rs_path, const std::string& bin_path,
                   const std::string& target, bool release, bool verbose) {
    if (!has_rustc()) {
        std::cerr << "Error: rustc not found. Install Rust from https://rustup.rs" << std::endl;
        return false;
    }
    std::string cmd = "rustc \"" + rs_path + "\" -o \"" + bin_path + "\"";
    if (!target.empty())
        cmd += " --target " + target;
    if (release)
        cmd += " -C opt-level=3";
    if (verbose)
        std::cerr << "[voic] running: " << cmd << std::endl;
    int ret = system(cmd.c_str());
    if (ret != 0) {
        std::cerr << "Error: rustc failed (exit code " << ret << ")" << std::endl;
        return false;
    }
    return true;
}

// ── 命令实现 ─────────────────────────────────────────────────────────

int cmd_translate(const std::string& source, const std::string& filename,
                  const std::string& rs_path, const std::string& std_lib_path,
                  bool verbose, bool emit_comments) {
    return translate_voic(source, filename, rs_path, std_lib_path, verbose, emit_comments) ? 0 : 1;
}

int cmd_check(const std::string& source, const std::string& filename) {
    vo::Lexer lexer(source, filename);
    vo::CompUnit cu;
    vo::Parser parser(lexer, cu);
    parser.parse();

    bool had_error = false;
    if (lexer.has_error()) {
        for (const auto& err : lexer.errors()) {
            vo::emit_diagnostic(err);
            had_error = true;
        }
    }
    if (parser.has_error()) {
        for (const auto& err : parser.errors()) {
            vo::emit_diagnostic(err);
            had_error = true;
        }
    }
    if (!had_error) {
        vo::SemanticAnalyzer semantic(cu);
        semantic.analyze();
        for (const auto& err : semantic.diagnostics())
            vo::emit_diagnostic(err);
        had_error = semantic.has_error();
    }
    if (!had_error)
        std::cerr << "Syntax OK" << std::endl;
    return had_error ? 1 : 0;
}

int cmd_build(const std::string& source, const std::string& filename,
              const std::string& bin_path, const std::string& rs_path,
              const std::string& std_lib_path,
              const std::string& target, bool release, bool emit_rs, bool verbose,
              bool emit_comments) {
    std::vector<std::string> generated;
    if (!translate_voic(source, filename, rs_path, std_lib_path, verbose, emit_comments, &generated))
        return 1;
    if (!compile_rustc(rs_path, bin_path, target, release, verbose))
        return 1;
    if (!emit_rs) {
        // 清理主翻译文件 + 链接翻译的 .rs（import 模块 / 标准库模块，含递归子模块）
        remove_file(rs_path);
        for (const auto& f : generated)
            remove_file(f);
    }
    if (verbose)
        std::cerr << "[voic] built " << bin_path << std::endl;
    return 0;
}

int cmd_run(const std::string& source, const std::string& filename,
            const std::string& bin_path, const std::string& rs_path,
            const std::string& std_lib_path,
            const std::string& target, bool release, bool emit_rs, bool verbose,
            bool emit_comments) {
    int ret = cmd_build(source, filename, bin_path, rs_path,
                        std_lib_path, target, release, emit_rs, verbose, emit_comments);
    if (ret != 0) return ret;

    if (verbose)
        std::cerr << "[voic] running " << bin_path << " ..." << std::endl;
    std::string bin = bin_path;
    for (auto& ch : bin) if (ch == '/') ch = '\\';  // Windows cmd 不接受正斜杠路径
    std::string cmd = "\"" + bin + "\"";
    ret = system(cmd.c_str());
    return ret;
}

int cmd_clean(const std::string& filename) {
    std::string name = exe_name(filename);
    std::string rs = name + ".rs";
    std::string bin = name + ".exe";
    std::string pdb = name + ".pdb";

    int removed = 0;
    auto clean_one = [&removed](const std::string& p) {
        if (file_exists(p)) { remove_file(p); std::cout << "removed " << p << std::endl; ++removed; }
    };

    clean_one(rs);
    clean_one(bin);
    clean_one(pdb);

    // also remove from parent dir if build was run from source dir
    size_t sep = filename.find_last_of("\\/");
    if (sep != std::string::npos) {
        std::string dir = filename.substr(0, sep);
        clean_one(dir + "\\" + rs);
        clean_one(dir + "\\" + bin);
        clean_one(dir + "\\" + pdb);
    }

    if (removed == 0)
        std::cout << "Nothing to clean." << std::endl;
    return 0;
}

// ── CLI ──────────────────────────────────────────────────────────────

void print_usage(const char* prog) {
    std::cerr << "voic " << VOIC_VERSION << std::endl;
    std::cerr << "Usage: voic <COMMAND> [OPTIONS] <FILE>" << std::endl;
    std::cerr << std::endl;
    std::cerr << "COMMANDS:" << std::endl;
    std::cerr << "  build      Compile to executable" << std::endl;
    std::cerr << "  run        Compile and run" << std::endl;
    std::cerr << "  check      Syntax check only (no code generation)" << std::endl;
    std::cerr << "  translate  Generate Rust code only (no compilation)" << std::endl;
    std::cerr << "  clean      Remove compiled artifacts" << std::endl;
    std::cerr << std::endl;
    std::cerr << "OPTIONS:" << std::endl;
    std::cerr << "  -o, --output <FILE>   Specify output file name" << std::endl;
    std::cerr << "  --target <TRIPLE>     Target platform (default: host)" << std::endl;
    std::cerr << "  --release             Release mode (optimized)" << std::endl;
    std::cerr << "  --std-lib <PATH>      Standard library path" << std::endl;
    std::cerr << "  --emit-rs             Keep generated Rust code" << std::endl;
    std::cerr << "  --no-comments         Do not emit comments in generated Rust code" << std::endl;
    std::cerr << "  -v, --verbose         Verbose output" << std::endl;
    std::cerr << "  -h, --help            Show this help" << std::endl;
    std::cerr << "  --version             Show version" << std::endl;
}

struct CliOptions {
    std::string command;
    std::string input_file;
    std::string output_file;
    std::string target;
    std::string std_lib_path;
    bool release = false;
    bool emit_rs = false;
    bool no_comments = false;
    bool verbose = false;
};

bool parse_args(int argc, char** argv, CliOptions& opts) {
    if (argc < 2) {
        print_usage(argv[0]);
        return false;
    }

    int i = 1;

    // parse command (first non-option argument if it's a known command)
    std::string first = argv[i];
    if (first == "build" || first == "run" || first == "check" ||
        first == "translate" || first == "clean") {
        opts.command = first;
        ++i;
    } else {
        // default command is "translate" for backward compatibility
        opts.command = "translate";
    }

    for (; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return false;
        }
        if (arg == "--version") {
            std::cout << "voic " << VOIC_VERSION << std::endl;
            return false;
        }
        if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) opts.output_file = argv[++i];
            else { std::cerr << "Error: " << arg << " requires an argument" << std::endl; return false; }
        } else if (arg == "--target") {
            if (i + 1 < argc) opts.target = argv[++i];
            else { std::cerr << "Error: --target requires an argument" << std::endl; return false; }
        } else if (arg == "--std-lib") {
            if (i + 1 < argc) opts.std_lib_path = argv[++i];
            else { std::cerr << "Error: --std-lib requires an argument" << std::endl; return false; }
        } else if (arg == "--release") {
            opts.release = true;
        } else if (arg == "--emit-rs") {
            opts.emit_rs = true;
        } else if (arg == "--no-comments") {
            opts.no_comments = true;
        } else if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (arg[0] == '-') {
            std::cerr << "Error: unknown option: " << arg << std::endl;
            std::cerr << "" << arg << std::endl;
            return false;
        } else {
            if (!opts.input_file.empty()) {
                std::cerr << "Error: multiple input files specified" << std::endl;
                return false;
            }
            opts.input_file = arg;
        }
    }

    if (opts.command == "clean") {
        // clean needs the file name to derive artifact names; input_file is optional
        return true;
    }

    if (opts.input_file.empty()) {
        std::cerr << "Error: no input file specified" << std::endl;
        return false;
    }

    return true;
}

// ── 入口 ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    CliOptions opts;
    if (!parse_args(argc, argv, opts))
        return 1;

    // 默认标准库路径
    if (opts.std_lib_path.empty())
        opts.std_lib_path = get_default_std_lib(get_exe_dir());

    if (opts.verbose && !opts.std_lib_path.empty())
        std::cerr << "[voic] std lib: " << opts.std_lib_path << std::endl;

    // 读取源文件
    std::string source, filename;
    if (opts.command == "clean") {
        return cmd_clean(opts.input_file.empty() ? "" : opts.input_file);
    }

    if (opts.input_file.empty()) {
        // 从 stdin 读取
        std::string line;
        while (std::getline(std::cin, line))
            source += line + "\n";
        filename = "<stdin>";
    } else {
        std::ifstream file(opts.input_file);
        if (!file.is_open()) {
            std::cerr << "Error: cannot open file: " << opts.input_file << std::endl;
            return 1;
        }
        source = std::string((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        filename = opts.input_file;
    }

    // 确定输出路径
    std::string bin_path, rs_path;

    if (opts.command == "translate" || opts.command == "build" || opts.command == "run") {
        if (!opts.output_file.empty()) {
            rs_path = opts.output_file;
            // if output ends with .exe, derive .rs path
            if (rs_path.size() > 4 && rs_path.substr(rs_path.size() - 4) == ".exe")
                rs_path = rs_path.substr(0, rs_path.size() - 4) + ".rs";
        } else {
            std::string base = filename;
            if (base.size() > 3 && base.substr(base.size() - 3) == ".vo")
                base = base.substr(0, base.size() - 3);
            rs_path = base + ".rs";
        }
        bin_path = rs_path;
        if (bin_path.size() > 3 && bin_path.substr(bin_path.size() - 3) == ".rs")
            bin_path = bin_path.substr(0, bin_path.size() - 3) + ".exe";
    }

    // 执行命令
    if (opts.command == "check")
        return cmd_check(source, filename);

    if (opts.command == "translate")
        return cmd_translate(source, filename, rs_path, opts.std_lib_path, opts.verbose, !opts.no_comments);

    if (opts.command == "build")
        return cmd_build(source, filename, bin_path, rs_path, opts.std_lib_path,
                         opts.target, opts.release, opts.emit_rs, opts.verbose, !opts.no_comments);

    if (opts.command == "run")
        return cmd_run(source, filename, bin_path, rs_path, opts.std_lib_path,
                       opts.target, opts.release, opts.emit_rs, opts.verbose, !opts.no_comments);

    std::cerr << "Error: unknown command: " << opts.command << std::endl;
    return 1;
}