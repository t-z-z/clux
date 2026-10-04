#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "ast.h"

namespace cx {

struct SymbolTable;
struct Scope;
struct Class;
struct Decl;

// 类修饰注入展开：把 #mod / 尾随注入的成员按优先级合并进目标类
class InjectionExpander {
public:
    using ErrorFn = std::function<void(const Location&, const std::string&)>;
    using WarnFn = std::function<void(const Location&, const std::string&)>;
    using CollectFn = std::function<void(Decl*, bool)>;

    InjectionExpander(CompUnit& cu, SymbolTable& symbols,
                      ErrorFn error, WarnFn warn, CollectFn collect)
        : cu_(cu), symbols_(symbols),
          error_(std::move(error)), warn_(std::move(warn)),
          collect_(std::move(collect)) {}

    // 遍历顶层声明，展开所有类的注入
    void expand_all();
    void expand_decl(Decl* decl, Scope* parent_scope);
    void expand_class(Class* cls, Scope* parent_scope);

private:
    enum class ExpandState { InProgress, Done };

    Class* make_base_source(PrimitiveTypeKind base, const Location& loc);

    CompUnit& cu_;
    SymbolTable& symbols_;
    ErrorFn error_;
    WarnFn warn_;
    CollectFn collect_;
    // 类注入展开状态：InProgress 用于环检测，Done 用作缓存
    std::unordered_map<Class*, ExpandState> expand_state_;
};

} // namespace cx
