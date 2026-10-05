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
    // 模块级变量的 LLVM 全局符号名；类实例字段不在此表内
    std::string global_of(const VarItem* vi) const;
    // static 字段的 LLVM 全局符号名；实例字段无全局符号，返回空
    std::string field_of(const VarItem* vi) const;

private:
    void walk(Decl* d, const std::string& prefix);

    std::unordered_map<Decl*, std::string> mangled_;
    std::unordered_map<const VarItem*, std::string> globals_;
    std::unordered_map<const VarItem*, std::string> fields_;
};

} // namespace cx
