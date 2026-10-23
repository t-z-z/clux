#pragma once

#include <string>
#include <string_view>

#include "ast.h"

namespace cx {

struct Symbol;

// 类型名节点（Identifier / 限定名 Member 链）的完整名字
std::string type_name_string(Type* t);

// 诊断用类型写法：按源码形式展开 T-*、T-&、T[N]、T{}、$T(...)，
// 供语法与语义报错说明具体类型
std::string describe_type(const Type* t);

// 引用类型的表达式即 lvalue
bool ref_is_lvalue(Type* t);

// 命名类型节点的成员作用域
Scope* named_type_scope(Type* t);

// 目标整数原始类型的符号性与位宽；返回 false 表示不是受检整数类型
bool primitive_int_range(PrimitiveTypeKind k, bool& is_signed, int& bits);

// 整数字面量自身类型对应的符号性与位宽；返回 false 表示不做自身范围检查
bool literal_int_range(LiteralKind k, bool& is_signed, int& bits);

// 解析整数字面量文本（可含进制前缀与类型后缀）为无符号值；超出 64 位返回 false
bool parse_uint_literal(std::string_view text, unsigned long long& out);

// 整数文本转十进制文本：接受 0x/0b/0o 前缀与 iN/uN/l/ll/s 后缀，
// 结果为无前导零的十进制；语法非法返回 false，量级远超 128 位也返回 false，
// 是否超出目标类型的范围由调用方用 int_max_text 判定
bool parse_int_literal(std::string_view text, std::string& dec);

// 十进制非负整数文本比较（无前导零），返回 -1/0/1
int dec_cmp(std::string_view a, std::string_view b);

// 有符号/无符号位宽可表示的最大值（十进制文本，支持到 128 位）
std::string int_max_text(bool is_signed, int bits);

// 浮点类型提升级：float16 < float < double
int float_rank(PrimitiveTypeKind k);

// 整数类型提升级：int8 < short < int < long < int128
int integer_rank(PrimitiveTypeKind k);

// 表达式的编译期数组长度：数组字面量取元素数，字符串字面量取字符数；
// 填充形式 [x ...] 的长度由目标决定，返回 -1 表示无法静态确定
long long const_array_len_of(Expr* e);

// 所有权切片类型 T{}
bool is_slice_type(Type* t);

// 数组类型的编译期长度；切片、无长度或长度非字面量返回 -1
long long const_array_type_len(Type* t);

// 枚举类型（类型位置的标识符/限定名，解析为 Enum 声明）
bool is_enum_type(Type* t);

// 数值类型（非 bool/type/void 的基础类型）
bool is_numeric_type(Type* t);

// 整数类型（含有符号/无符号/char 系）
bool is_integer_type(Type* t);

// 浮点类型
bool is_float_type(Type* t);

// 布尔类型
bool is_bool_type(Type* t);

// 条件位置允许的类型：bool、指针、null（数组按隐式转换报警告）
bool is_condition_type(Type* t);

// 结构等价判定：基础类型同名，指针/引用/数组逐层递归，函数指针签名一致
bool is_same_type(Type* a, Type* b);

// 可整型提升的类型（窄于 int 的整数）
bool is_integer_promotable(Type* t);

// void 类型（含源码中的 void 关键字节点）
bool is_void_type(Type* t);

// 符号的展示名：量取量名，其余取声明名
std::string sym_display_name(Symbol* sym);

} // namespace cx
