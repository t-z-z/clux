#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ast.h"
#include "diagnostic.h"
#include "string_view_map.h"

namespace vo {

enum class SymbolKind {
    Variable,
    Function,
    Class,
    Namespace,
};
// 符号
struct Symbol {
    SymbolKind kind = SymbolKind::Variable;
    std::string_view name;       // 视图，指向 AST 中的名字（源字符串/转义缓冲）
    Type* type = nullptr;
    Decl* decl = nullptr;
    std::string ir_type;        // LLVM IR 类型（可用于代码生成）
    Symbol() = default;
    Symbol(SymbolKind k, std::string_view n, Type* t = nullptr, Decl* d = nullptr, std::string ir = "")
        : kind(k), name(n), type(t), decl(d), ir_type(std::move(ir)) {}
    explicit Symbol(Decl* d) 
        : name(d->name), decl(d) {
        switch (d->kind) {
            case DeclKind::VarList: kind = SymbolKind::Variable; break;
            case DeclKind::Func:    kind = SymbolKind::Function; break;
            case DeclKind::Class:   kind = SymbolKind::Class; break;
            case DeclKind::Enum:    kind = SymbolKind::Class; break;
            case DeclKind::Namespace: kind = SymbolKind::Namespace; break;
            case DeclKind::SpecialMethod: kind = SymbolKind::Function; break;
            case DeclKind::Operator: kind = SymbolKind::Function; break;
            default: kind = SymbolKind::Variable; break;
        }
    }
};

// 作用域
class SymbolTable;   // Scope 与 SymbolTable 互相引用，先前置声明

class Scope {
    SymbolTable& st_;           // 所属符号表
    Scope* parent_ = nullptr;
public:
    // 键为 string_view：符号名来自词法阶段统一池，生命周期覆盖整个分析
    svmap<Symbol> symbols_;
    svmap<ASTNode*> generics_;
    // 禁止拷贝/移动
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    // 构造/析构、insert/lookup 在函数末尾统一定义（需要完整的 SymbolTable）
    Scope(SymbolTable& st);
    ~Scope();
    bool insert(Symbol sym);
    Symbol* lookup(const std::string_view& name);
    Scope* parent() const { return parent_; }
};

class SymbolTable {
    std::unique_ptr<Scope> root_;
    Scope* current_ = nullptr;
public:
    SymbolTable();
    void set_current_scope(Scope* s) { current_ = s; }
    Scope* current_scope() const { return current_; }
    // Scope 通过构造函数自动注册，不需要手动 push/pop
    Symbol* lookup(const std::string_view& name) {
        for (Scope* s = current_; s; s = s->parent()) {
            if (Symbol* sym = s->lookup(name)) return sym;
        }
        return nullptr;
    }
    bool insert(Symbol sym) {
        return current_ ? current_->insert(std::move(sym)) : false;
    }
    void add_generic(const std::string_view& name, ASTNode* decl) {
        current_->generics_[name] = decl;
    }
    ASTNode* lookup_generic(const std::string_view& name) {
        for (Scope* s = current_; s; s = s->parent()) {
            auto it = s->generics_.find(name);
            if (it != s->generics_.end()) return it->second;
        }
        return nullptr;
    }
};

// 两个类型均已完整，定义互相依赖的成员函数
inline Scope::Scope(SymbolTable& st) : st_(st), parent_(st.current_scope()) {
    st_.set_current_scope(this);
}
inline Scope::~Scope() {
    st_.set_current_scope(parent_);
}
inline bool Scope::insert(Symbol sym) {
    // variable 可以直接覆盖，语言特性
    auto it = symbols_.find(sym.name);
    if (it !=symbols_.end() &&
     !(it->second.kind == SymbolKind::Variable && sym.kind == SymbolKind::Variable)) {
        if (symbols_.count(sym.name)) return false;
    }
    symbols_[sym.name] = std::move(sym);
    return true;
}
inline Symbol* Scope::lookup(const std::string_view& name) {
    auto it = symbols_.find(name);
    return it == symbols_.end() ? nullptr : &it->second;
}
inline SymbolTable::SymbolTable() {
    root_ = std::make_unique<Scope>(*this);
    current_ = root_.get();
}

// 重载
struct OverloadCandidate {
    std::string name;
    std::string mangled;
    std::string key;
    size_t param_count = 0;
    Decl* decl = nullptr;
    Type* return_type = nullptr;
    bool has_body = false;
};

struct CallResolution {
    Expr* call = nullptr;
    std::string callee_name;
    std::string resolved_name;
    Decl* resolved_decl = nullptr;
    Type* return_type = nullptr;
    bool is_constructor = false;
    bool resolved = false;
    bool ambiguous = false;
    std::string message;
};

// 语义分析器
class SemanticAnalyzer : public ASTVisitor {
public:
    explicit SemanticAnalyzer(CompUnit& cu) : cu_(cu) {}

    bool analyze();
    bool has_error() const { return has_error_; }
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    // 查询接口（供代码生成使用）
    const SymbolTable& symbol_table() const { return symbols_; }
    const auto& overload_map() const { return overload_map_; }
    const auto& class_overload_maps() const { return class_overload_maps_; }
    const auto& call_resolutions() const { return call_resolutions_; }
    const CallResolution* find_call(Expr* e) const;
    std::string type_to_ir(Type* t) const;  // volang 类型 -> LLVM IR 类型

    using ASTVisitor::visit;

    void visit(CompUnit& cu) override;
    // 声明
    void visit(Decl& decl) override;
    void visit(Modification& mod) override;
    void visit_func(Func& f) override;
    void visit_var_list(VarList& vl) override;
    void visit_class(Class& c) override;
    void visit_namespace(Namespace& ns) override;
    void visit_extern(Extern& e) override;
    void visit_operator(Operator& op) override;
    void visit_special_method(SpecialMethod& sm) override;
    void visit_enum(Enum& e) override;
    void visit_keep(Keep& k) override;

    // 语句
    void visit_stmt_block(Stmt::Block& s) override;
    void visit_stmt_expr(Stmt::ExprStmt& s) override;
    void visit_stmt_if(Stmt::If& s) override;
    void visit_stmt_while(Stmt::While& s) override;
    void visit_stmt_for(Stmt::For& s) override;
    void visit_stmt_for_in(Stmt::ForIn& s) override;
    void visit_stmt_return(Stmt::Return& s) override;
    void visit_stmt_var_decl(Stmt::VarDecl& s) override;
    void visit_stmt_spawn(Stmt::Spawn& s) override;
    void visit_stmt_wait(Stmt::Wait& s) override;
    void visit_stmt_asm(Stmt::Asm& s) override;
    void visit_stmt_keep(Stmt::Keep& s) override;
    void visit_modification(Modification& mod);

    // 表达式
    void visit(Expr& expr) override;

private:
    CompUnit& cu_;
    SymbolTable symbols_;
    std::vector<Diagnostic> diagnostics_;
    bool has_error_ = false;

    // 重载表（键为 string_view，来自统一字符串池，透明查找无需拷贝）
    svmap<std::vector<OverloadCandidate>> overload_map_;
    svmap<svmap<std::vector<OverloadCandidate>>> class_overload_maps_;
    svmap<svmap<std::vector<OverloadCandidate>>> namespace_overload_maps_;
    std::vector<CallResolution> call_resolutions_;

    // 当前类上下文
    Class* current_class_ = nullptr;
    svset current_class_fields_;
    svset current_class_readonly_fields_;
    svmap<std::vector<OverloadCandidate>>* current_class_overloads_ = nullptr;
    
    bool has_imports_or_keep_ = false;
    int suppress_undefined_ = 0;

    void error(const Location& loc, const std::string& msg);
    void note(const Location& loc, const std::string& msg);
    void warning(const Location& loc, const std::string& msg);

    Type* make_type(const Location& loc, Type::Variant v);
    std::string type_to_ir(Type& t) const;
    std::string build_decl_key(const std::vector<Decl*>& params);
    std::string build_call_key(const std::vector<Expr*>& args);

    void collect_top_level();
    void register_func(Func* f, const std::string& ns_name = "");
    void register_special(SpecialMethod* sm, const std::string& ns_name = "");
    void add_candidate(...);
    void resolve_call(Expr* call, Expr::Call& c);
    const OverloadCandidate* match_candidates(...) const;
    void set_expr_type(Expr* e, Type* t);
    Type* element_type(Type* t);
    std::string special_name(SpecialMethod& sm);
    void check_type(Type& t);
    bool is_known_type(const std::string& name);
    Type* find_field_type(const std::string& cls, const std::string& field);
    bool is_field_readonly(const std::string& cls, const std::string& field);
};

} // namespace vo