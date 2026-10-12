#pragma once

#include "token.h"
#include "ast.h"

#include <optional>
#include <functional>
#include <string>

namespace cx {

struct Expr;

// 编译期常量值。
// kind 复用 PrimitiveTypeKind：整数系/Char/Bool/Float 系为数值，Void 表示空指针。
struct ConstValue {
    PrimitiveTypeKind kind = PrimitiveTypeKind::Int;
    unsigned long long u = 0;   // 原始位模式（含符号位）
    bool is_signed = true;
    int bits = 32;
    double f = 0.0;

    // 有符号解释；无符号高位不截断直接按位重解释
    long long i64() const;
    // 真值：非零即真（指针与整数同理）
    bool truthy() const;
    bool is_float() const {
        return kind == PrimitiveTypeKind::Float16 ||
               kind == PrimitiveTypeKind::Float ||
               kind == PrimitiveTypeKind::Double;
    }
    bool is_int() const {
        return kind != PrimitiveTypeKind::Float16 &&
               kind != PrimitiveTypeKind::Float &&
               kind != PrimitiveTypeKind::Double &&
               kind != PrimitiveTypeKind::Void;
    }
};

// 溢出/负移位走 warn，除零/模零走 error。
// error 被调用时 eval_const 返回 nullopt。
struct ConstEvalSink {
    std::function<void(const Location&, const std::string&)> warn;
    std::function<void(const Location&, const std::string&)> error;
};

// 求值入口。e 必须已通过 P1（type/sym 就绪）。
// 无法在编译期求值时返回 nullopt，且不产生诊断。
std::optional<ConstValue> eval_const(Expr* e, ConstEvalSink& sink, int depth = 0);

} // namespace cx
