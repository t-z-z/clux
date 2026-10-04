#pragma once

#include <string>
#include <unordered_map>

#include "ast.h"

namespace cx {

// 声明的 LLVM 符号名（mangled name）
class Mangler {
public:
    void collect(CompUnit& cu);
    std::string of(Decl* d) const;
    std::string special_name(SpecialMethod* sm);
    std::string conversion_name(Conversion* cf);
    std::string type_key(Expr* t);
    // 模块级变量的 LLVM 全局符号名；类字段不在此表内
    std::string global_of(const VarItem* vi) const;

private:
    void walk(Decl* d, const std::string& prefix);

    std::unordered_map<Decl*, std::string> mangled_;
    std::unordered_map<const VarItem*, std::string> globals_;
};

} // namespace cx
