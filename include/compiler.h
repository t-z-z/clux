#pragma once

#include <string>

namespace cx {

// 运行完整前端流程：词法 -> 语法 -> 语义分析。
// 统一字符串池在函数内部持有（而非 lexer 私有），保证词法阶段产生的
// 字符串视图（含转义后的字面量）在后续分析阶段始终有效。
bool compile(const std::string& source, const std::string& filename = "<input>");

} // namespace cx
