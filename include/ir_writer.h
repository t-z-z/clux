#pragma once

#include <ostream>
#include <sstream>
#include <string>
#include <memory>
#include <string_view>
#include <vector>

namespace cx {

// LLVM IR 文本发射器
class IRWriter {
public:
    explicit IRWriter(std::ostream& out) : out_(out) {}

    // 进入新函数：重置具名寄存器/标签计数
    void begin_func() { next_reg_ = 0; next_label_ = 0; }

    std::string reg() { return "%t" + std::to_string(next_reg_++); }
    std::string named_label(const std::string& hint) {
        return hint + "." + std::to_string(next_label_++);
    }

    static std::string quote(std::string_view name);

    void emit(std::string_view s) { (*cur_) << s << '\n'; }
    void blank() { (*cur_) << '\n'; }

    // 闭包等延后发射的函数体：每层延迟段各自持有独立缓冲，退出时把该段
    // 完整追加到 late_，flush 时 late_ 追加到 body_ 之后。
    // 进入/退出时按栈保存并恢复寄存器、标签计数器，避免内部的 begin_func()
    // 清零计数后污染宿主函数的命名空间；独立缓冲保证嵌套时不会把内层函数
    // 体插进正在写的宿主函数中间。
    void capture_late() {
        late_stack_.push_back(std::make_unique<std::ostringstream>());
        cur_ = late_stack_.back().get();
        reg_stack_.push_back(next_reg_);
        label_stack_.push_back(next_label_);
    }
    void release_late() {
        late_ << late_stack_.back()->str();
        late_stack_.pop_back();
        cur_ = late_stack_.empty() ? &body_ : late_stack_.back().get();
        next_reg_   = reg_stack_.back();   reg_stack_.pop_back();
        next_label_ = label_stack_.back(); label_stack_.pop_back();
    }

    // 注册一个全局字符串常量；返回其符号名（不含 @）
    std::string string_const(std::string_view content);

    void emit_header();
    void need_pthread() { uses_pthread_ = true; }
    void need_malloc()  { uses_malloc_ = true; }
    void need_free()    { uses_free_ = true; }
    void emit_type(const std::string& s) { header_ << s << '\n'; }
    void emit_global(const std::string& s) { globals_ << s << '\n'; }
    void emit_module_tail(const std::string& s) { tail_ << s << '\n'; }
    void flush();

private:
    std::ostream& out_;
    std::ostringstream header_;
    std::ostringstream globals_;
    std::ostringstream body_;
    std::ostringstream late_;
    std::ostringstream tail_;
    std::ostream* cur_ = &body_;
    std::vector<std::unique_ptr<std::ostringstream>> late_stack_;
    int next_reg_ = 0;
    bool uses_pthread_ = false;
    bool uses_malloc_ = false;
    bool uses_free_ = false;
    int next_label_ = 0;
    int next_str_ = 0;
    std::vector<int> reg_stack_;
    std::vector<int> label_stack_;
};

} // namespace cx
