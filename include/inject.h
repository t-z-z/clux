#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "ast.h"

namespace cx {

struct SymbolTable;
struct Symbol;
struct Scope;
struct Class;
struct Decl;

// 类修饰注入展开：把 #mod / 尾随注入的成员按优先级合并进目标类
class InjectionExpander {
public:
    using ErrorFn = std::function<void(const Location&, const std::string&)>;
    using WarnFn = std::function<void(const Location&, const std::string&)>;
    using CollectFn = std::function<void(Decl*, bool)>;
    // 泛型注入来源 #foo-<T>：实参类型由语义阶段解析并按实参实例化，
    // 返回展开好、成员表已就位的实例类
    using GenericSourceFn = std::function<Class*(Class& tmpl, std::vector<Type*>& args,
                                                 Symbol* tmpl_sym, Scope* search_scope,
                                                 const Location& loc)>;

    InjectionExpander(CompUnit& cu, SymbolTable& symbols,
                      ErrorFn error, WarnFn warn, CollectFn collect,
                      GenericSourceFn generic_source = nullptr)
        : cu_(cu), symbols_(symbols),
          error_(std::move(error)), warn_(std::move(warn)),
          collect_(std::move(collect)),
          generic_source_(std::move(generic_source)) {}

    // 遍历顶层声明，展开所有类的注入
    void expand_all();
    void expand_decl(Decl* decl, Scope* parent_scope);
    void expand_class(Class* cls, Scope* parent_scope);

private:
    enum class ExpandState { InProgress, Done };

    Class* make_base_source(PrimitiveTypeKind base, const Location& loc,
                            Class* main);

    CompUnit& cu_;
    SymbolTable& symbols_;
    ErrorFn error_;
    WarnFn warn_;
    CollectFn collect_;
    GenericSourceFn generic_source_;
    // 当前展开上下文是否在泛型模板体内：模板体里的泛型注入推迟到实例化之后
    int template_depth_ = 0;
    // 类注入展开状态：InProgress 用于环检测，Done 用作缓存
    std::unordered_map<Class*, ExpandState> expand_state_;
};

} // namespace cx
