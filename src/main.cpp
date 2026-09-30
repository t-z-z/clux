#include "../include/compiler.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

enum class Action { None, Translate, Build, Run, Check, EmitAsm, EmitObj };

void usage() {
    std::cout <<
        "clux - the clux language compiler\n"
        "\n"
        "usage: clux [options] <file.cx>\n"
        "\n"
        "actions:\n"
        "  -t, --translate        translate to LLVM IR (.ll)\n"
        "  -b, --build            build an executable\n"
        "  -r, --run              build and run\n"
        "  -c, --check            front-end check only, no output\n"
        "      --emit-asm         emit assembly (.s)\n"
        "      --emit-obj         emit an object file (.o)\n"
        "\n"
        "options:\n"
        "  -o, --output FILE      output path\n"
        "  -I DIR                 add DIR to the module search path\n"
        "  -O N                   optimization level 0-3 (translate runs opt; build passes to clang++)\n"
        "  -v, --verbose          print the clang++ command line\n"
        "      --version          show version\n"
        "  -h, --help             show this help\n";
}

const char* kVersion = "clux 0.1.0";

std::string replace_ext(const std::string& path, const std::string& ext) {
    fs::path p(path);
    p.replace_extension(ext);
    return p.string();
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)),
               std::istreambuf_iterator<char>());
    return true;
}

// 把 IR 文本经 stdin 管道交给 clang++，不生成中间 .ll 文件
// 把 IR 文本经 stdin 管道交给 clang++，不生成中间 .ll 文件
int run_clang_ir(const std::string& ir, const std::string& out_path,
                 const std::vector<std::string>& extra, bool verbose) {
    int fds[2];
    if (pipe(fds) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        close(fds[1]);
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        std::vector<std::string> args = {"clang++", "-x", "ir", "-", "-o", out_path};
        for (const auto& e : extra) args.push_back(e);
        std::vector<char*> cargv;
        for (auto& a : args) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        execvp("clang++", cargv.data());
        _exit(127);
    }
    close(fds[0]);
    size_t off = 0;
    while (off < ir.size()) {
        ssize_t n = write(fds[1], ir.data() + off, ir.size() - off);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
    close(fds[1]);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    (void)verbose;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// 把 input 写入 prog 的 stdin，捕获其 stdout；避免写入/读取死锁用线程写
int run_filter(const std::string& prog, const std::vector<std::string>& args,
               const std::string& input, std::string& output) {
    int in_fds[2], out_fds[2];
    if (pipe(in_fds) < 0) return -1;
    if (pipe(out_fds) < 0) {
        close(in_fds[0]); close(in_fds[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(in_fds[0]); close(in_fds[1]);
        close(out_fds[0]); close(out_fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(in_fds[0], STDIN_FILENO);
        dup2(out_fds[1], STDOUT_FILENO);
        close(in_fds[0]); close(in_fds[1]);
        close(out_fds[0]); close(out_fds[1]);
        std::vector<std::string> full = {prog};
        for (const auto& a : args) full.push_back(a);
        std::vector<char*> cargv;
        for (auto& a : full) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        execvp(prog.c_str(), cargv.data());
        _exit(127);
    }
    close(in_fds[0]);
    close(out_fds[1]);

    std::thread writer([&]() {
        size_t off = 0;
        while (off < input.size()) {
            ssize_t n = write(in_fds[1], input.data() + off, input.size() - off);
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
        close(in_fds[1]);
    });

    output.clear();
    char buf[4096];
    ssize_t n;
    while ((n = read(out_fds[0], buf, sizeof(buf))) > 0) {
        output.append(buf, static_cast<size_t>(n));
    }
    close(out_fds[0]);
    writer.join();

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int run_process(const std::vector<std::string>& argv) {
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

} // namespace

int main(int argc, char** argv) {
    Action action = Action::None;
    std::string output;
    std::string input;
    std::string lib_dir;
    std::vector<std::string> clang_extra;
    int opt_level = -1;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-t" || a == "--translate")            action = Action::Translate;
        else if (a == "-b" || a == "--build")           action = Action::Build;
        else if (a == "-r" || a == "--run")             action = Action::Run;
        else if (a == "-c" || a == "--check")           action = Action::Check;
        else if (a == "--emit-asm")                     action = Action::EmitAsm;
        else if (a == "--emit-obj")                     action = Action::EmitObj;
        else if (a == "-o" || a == "--output") {
            if (i + 1 >= argc) { std::cerr << "error: -o requires an argument\n"; return 1; }
            output = argv[++i];
        }
        else if (a == "-I") {
            if (i + 1 >= argc) { std::cerr << "error: -I requires an argument\n"; return 1; }
            lib_dir = argv[++i];
        }
        else if (a.size() > 2 && a[0] == '-' && a[1] == 'I') {
            lib_dir = a.substr(2);
        }
        else if (a == "-O") {
            if (i + 1 >= argc) { std::cerr << "error: -O requires an argument\n"; return 1; }
            std::string v = argv[++i];
            opt_level = std::atoi(v.c_str());
            clang_extra.push_back("-O" + v);
        }
        else if (a.size() == 3 && a[0] == '-' && a[1] == 'O' &&
                 a[2] >= '0' && a[2] <= '3') {
            opt_level = a[2] - '0';
            clang_extra.push_back(a);
        }
        else if (a == "-v" || a == "--verbose")         verbose = true;
        else if (a == "--version")                      { std::cout << kVersion << "\n"; return 0; }
        else if (a == "-h" || a == "--help")            { usage(); return 0; }
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "error: unknown option '" << a << "'\n";
            return 1;
        }
        else if (input.empty())                          input = a;
        else { std::cerr << "error: only one input file is accepted\n"; return 1; }
    }

    if (input.empty()) { usage(); return 1; }
    if (action == Action::None) { usage(); return 1; }

    std::string source;
    if (!read_file(input, source)) {
        std::cerr << "error: cannot read file '" << input << "'\n";
        return 1;
    }

    std::string dir = fs::path(input).parent_path().string();
    std::string base = fs::path(input).stem().string();

    if (action == Action::Check) {
        return cx::check(source, input, lib_dir) ? 0 : 1;
    }

    if (action == Action::Translate) {
        std::string ll = output.empty() ? replace_ext(input, ".ll") : output;

        std::ostringstream ir_buf;
        if (!cx::translate(source, input, lib_dir, ir_buf)) return 1;
        std::string final_ir = ir_buf.str();

        if (opt_level >= 1) {
            std::string passes = "default<O" + std::to_string(opt_level) + ">";
            std::string optimized;
            if (verbose)
                std::cout << "opt -passes=" << passes << " -S -\n";
            int rc = run_filter("opt", {"-passes=" + passes, "-S", "-"},
                                final_ir, optimized);
            if (rc != 0) {
                std::cerr << "error: opt failed (exit code " << rc << ")\n";
                return 1;
            }
            final_ir = optimized;
        }

        std::ofstream out(ll);
        if (!out) { std::cerr << "error: cannot write '" << ll << "'\n"; return 1; }
        out << final_ir;
        out.close();
        std::cout << "generated " << ll << "\n";
        return 0;
    }

    // -b / -r / --emit-asm / --emit-obj: IR in memory, then clang++ via stdin
    std::ostringstream ir;
    if (!cx::translate(source, input, lib_dir, ir)) return 1;

    std::vector<std::string> extra = clang_extra;
    if (action == Action::EmitAsm) extra.push_back("-S");
    if (action == Action::EmitObj) extra.push_back("-c");

    std::string default_ext;
    if (action == Action::EmitAsm) default_ext = ".s";
    else if (action == Action::EmitObj) default_ext = ".o";

    std::string out_path;
    if (!output.empty()) {
        out_path = output;
    } else if (!default_ext.empty()) {
        out_path = replace_ext(input, default_ext);
    } else {
        out_path = dir.empty() ? "./" + base : dir + "/" + base;
    }

    if (verbose) {
        std::cout << "clang++ -x ir - -o " << out_path;
        for (const auto& e : extra) std::cout << " " << e;
        std::cout << "\n";
    }

    int rc = run_clang_ir(ir.str(), out_path, extra, verbose);
    if (rc != 0) {
        std::cerr << "error: clang++ failed (exit code " << rc << ")\n";
        return 1;
    }

    if (action == Action::Build || action == Action::EmitAsm ||
        action == Action::EmitObj) {
        std::cout << "generated " << out_path << "\n";
        return 0;
    }

    rc = run_process({out_path});
    return rc;
}
