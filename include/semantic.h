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

struct Symbol {
    SymbolKind kind = SymbolKind::Variable;
    std::string_view name;  // 视图，指向 AST 中的名字（源字符串/转义缓冲）
    Type* type = nullptr;
    Decl* decl = nullptr;
    std::string ir_type;    // LLVM IR 类型（可用于代码生成）
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


class SymbolTable;   // Scope 与 SymbolTable 互相引用，先前置声明

struct Scope {
    Scope* parent = nullptr;
    svmap<Symbol> symbols;
    svmap<ASTNode*> generics;
    Scope() = default;
    explicit Scope(Scope* p) : parent(p) {}
    bool insert(Symbol sym) {
        auto it = symbols.find(sym.name);
        if (it != symbols.end()) {
            // 变量允许覆盖
            if (it->second.kind == SymbolKind::Variable && sym.kind == SymbolKind::Variable) {
                it->second = std::move(sym);
                return true;
            }
            return false;  // 非变量重复定义
        }
        symbols[sym.name] = std::move(sym);
        return true;
    }
    Symbol* lookup(std::string_view name) {
        auto it = symbols.find(name);
        return it == symbols.end() ? nullptr : &it->second;
    }
    ASTNode* lookup_generic(std::string_view name) {
        auto it = generics.find(name);
        return it == generics.end() ? nullptr : it->second;
    }
};

class SymbolTable {
    std::unique_ptr<Scope> root_;
    Scope* current_ = nullptr;
public:
    SymbolTable() {
        root_ = std::make_unique<Scope>();
        current_ = root_.get();
    }
    Scope* push_scope() {
        Scope* s = new Scope(current_);
        current_ = s;
        return s;
    }
    void pop_scope() {
        if (current_ && current_ != root_.get()) {
            Scope* dead = current_;
            current_ = dead->parent;
            delete dead;
        }
    }
    Scope* current_scope() const { return current_; }
    bool insert(Symbol sym) {
        return current_ ? current_->insert(std::move(sym)) : false;
    }
    Symbol* lookup(std::string_view name) {
        for (Scope* s = current_; s; s = s->parent) {
            if (Symbol* sym = s->lookup(name)) return sym;
        }
        return nullptr;
    }
    void add_generic(std::string_view name, ASTNode* node) {
        if (current_) current_->generics[name] = node;
    }
    ASTNode* lookup_generic(std::string_view name) {
        for (Scope* s = current_; s; s = s->parent) {
            if (ASTNode* node = s->lookup_generic(name)) return node;
        }
        return nullptr;
    }
};
// 重载
struct OverloadCandidate {
    std::string name;
    std::string mangled;
    std::string key;
    size_t param_count = 0;
    Decl* decl = nullptr;
    Type* return_type = nullptr;
    bool has_body = false;
    bool has_template_params = false;
    size_t template_count = 0;
    // 模板参数名列表其实并不需要
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

class SemanticAnalyzer : public ASTVisitor {
public:
    explicit SemanticAnalyzer(CompUnit& cu) : cu_(cu) {}

    bool analyze();
    bool has_error() const { return has_error_; }
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    const SymbolTable& symbol_table() const { return symbols_; }
    const auto& overload_map() const { return overload_map_; }
    const auto& class_overload_maps() const { return class_overload_maps_; }
    const auto& call_resolutions() const { return call_resolutions_; }
    const CallResolution* find_call(Expr* e) const;
    std::string type_to_ir(Type* t) const;  // volang 类型 -> LLVM IR 类型

    using ASTVisitor::visit;

    void scope_in() override;
    void scope_in() override;
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
    std::string build_decl_key(const std::vector<VarList*>& params);
    std::string build_call_key(const std::vector<Expr*>& args);

    void collect();
    void collect_decl(Decl*);
    void register_func(Func* f, const std::string& ns_name = "");
    void register_special(SpecialMethod* sm, const std::string& ns_name = "");
    void register_method(Func* f, Class* cls);
    void resolve_call(Expr* call, Expr::Call& c);
    const OverloadCandidate* match_candidates(
        const std::string& name,
        const std::string& key,
        const std::vector<Expr*>& args,
        const std::vector<OverloadCandidate>& candidates);
    void set_expr_type(Expr* e, Type* t);
    Type* element_type(Type* t);
    void check_type(Type& t);
    struct FieldInfo {Type* type; bool is_readonly;
    } get_field_info(const std::string& cls, const std::string& field);
    struct TemplateDeduction {
        std::string param_name;
        // 二选一
        Type* type = nullptr;
        Expr* expr = nullptr;
        
        // 推导来源（用于诊断）
        std::string deduced_from; // 从哪个参数推导的
        Location deduced_loc;
        
        bool is_deduced() const { return type != nullptr || expr != nullptr; }
    };
    bool can_instantiate(const OverloadCandidate& cand, 
        const std::vector<Expr*>& args, std::vector<TemplateDeduction>& deductions);
    bool deduce_template_params(const Func* f,
        const std::vector<Expr*>& args, std::vector<TemplateDeduction>& deductions);
Type* substitute_type(Type* t, const std::vector<TemplateDeduction>& deductions);

};

} // namespace vo