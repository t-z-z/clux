#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace cx {

// Front-end check only: lex -> parse -> semantic, including import handling.
// Returns false if any diagnostic is emitted.
bool check(const std::string& source, const std::string& filename = "<input>",
           const std::string& lib_dir = "");

// Translate to LLVM IR text; returns false if the front end fails.
// libs 收集各模块 extern 块声明的库名（去重、保持声明顺序），
// 供链接阶段生成 -l 参数；为 nullptr 时不收集。
bool translate(const std::string& source, const std::string& filename,
               const std::string& lib_dir, std::ostream& out,
               std::vector<std::string>* libs = nullptr);

} // namespace cx
