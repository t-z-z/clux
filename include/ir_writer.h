#pragma once

#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

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

    // 闭包等延后发射的函数体：写入 late_，flush 时追加到 body_ 之后
    void capture_late() {
        if (late_depth_++ == 0) cur_ = &late_;
    }
    void release_late() {
        if (--late_depth_ == 0) cur_ = &body_;
    }

    // 注册一个全局字符串常量；返回其符号名（不含 @）
    std::string string_const(std::string_view content);

    void emit_header();
    void emit_type(const std::string& s) { header_ << s << '\n'; }
    void flush();

private:
    std::ostream& out_;
    std::ostringstream header_;
    std::ostringstream globals_;
    std::ostringstream body_;
    std::ostringstream late_;
    std::ostream* cur_ = &body_;
    int late_depth_ = 0;
    int next_reg_ = 0;
    int next_label_ = 0;
    int next_str_ = 0;
};

} // namespace cx
