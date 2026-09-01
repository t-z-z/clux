#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <variant>
#include <map>

#include "ast.h"
#include "diagnostic.h"
#include "string_view_map.h"   // svmap, svset

namespace vo {

// 符号表与作用域

enum class SymbolKind {
    Variable,
    Function,
    Class,
    Namespace,
    EnumVar,
};

struct Symbol {
    SymbolKind kind = SymbolKind::Variable;
    std::string_view name;          // 指向 AST 中的名字（由 AST 保证生命周期）
    Type* type = nullptr;           // 变量或函数的返回类型
    Decl* decl = nullptr;           // 对应的声明节点
    std::string ir_type;            // LLVM IR 类型字符串（缓存）

    Symbol() = default;
    Symbol(SymbolKind k, std::string_view n, Type* t = nullptr, Decl* d = nullptr,
           std::string ir = "")
        : kind(k), name(n), type(t), decl(d), ir_type(std::move(ir)) {}
    explicit Symbol(Decl* d) : name(d->name), decl(d) {
        switch (d->kind) {
            case DeclKind::VarList:       kind = SymbolKind::Variable; break;
            case DeclKind::Func:          kind = SymbolKind::Function; break;
            case DeclKind::Class:         kind = SymbolKind::Class; break;
            case DeclKind::Enum:          kind = SymbolKind::Class; break;
            case DeclKind::Namespace:     kind = SymbolKind::Namespace; break;
            case DeclKind::SpecialMethod: kind = SymbolKind::Function; break;
            case DeclKind::Operator:      kind = SymbolKind::Function; break;
            default:                      kind = SymbolKind::Variable; break;
        }
    }
};

struct Scope {
    svmap<Symbol> locals;            // 局部符号（变量、参数、模板参数等）
    svmap<ASTNode*> tpls;
    std::string name;
    Scope() = default;
    Scope(std::string n) : name(std::move(n)) {}
    // 插入局部符号（变量允许覆盖，其他不允许重复）
    inline bool insert_local(Symbol sym) {
        auto it = locals.find(sym.name);
        if (it != locals.end()) {
            // 仅变量与变量之间允许覆盖
            if (it->second.kind == SymbolKind::Variable &&
                sym.kind == SymbolKind::Variable) {
                it->second = std::move(sym);
                return true;
            }
            return false;
        }
        locals[sym.name] = std::move(sym);
        return true;
    }
    // 查找本作用域的局部符号
    inline Symbol* lookup_local(std::string_view name) {
        auto it = locals.find(name);
        return it == locals.end() ? nullptr : &it->second;
    }
    inline void insert_tpl(std::string_view name, ASTNode* n) {
        tpls[name] = n;
    }
    // 查找本作用域的局部符号
    inline ASTNode* lookup_tpl(std::string_view name) {
        auto it = tpls.find(name);
        return it == tpls.end() ? nullptr : it->second;
    }
};

class SymbolTable {
    std::vector<std::unique_ptr<Scope>> scopes_;   // 作用域栈（拥有所有权）
    Scope* current_ = nullptr;                     // 当前作用域
    std::unordered_map<std::string, std::vector<Symbol>> global_symbols_;    // 大表：持久符号（函数、类、命名空间等）

public:
    SymbolTable() {
        scopes_.push_back(std::make_unique<Scope>("<global>"));
        current_ = scopes_.back().get();
    }

    // 创建并进入新作用域
    Scope* push_scope(std::string name) {
        auto new_scope = std::make_unique<Scope>(std::move(name));
        Scope* raw = new_scope.get();
        scopes_.push_back(std::move(new_scope));
        current_ = raw;
        return raw;
    }

    // 退出当前作用域
    void pop_scope() {
        if (scopes_.size() <= 1) return;   // 根作用域不可弹出
        auto& top = scopes_.back();
        scopes_.pop_back();
        current_ = scopes_.back().get();
    }

    Scope* current_scope() const { return current_; }
    Scope* root() const { return scopes_.front().get(); }

    // 插入局部符号到当前作用域
    bool insert_local(Symbol sym) {
        return current_ ? current_->insert_local(std::move(sym)) : false;
    }

    void insert_local_tpl(std::string_view name, ASTNode* n) {
        if (current_) current_->insert_tpl(name, n);
    }

    // 插入持久符号到大表（同名符号按 vector 存储，支持重载）
    void insert_global(Symbol sym, std::string key) {
        global_symbols_[std::move(key)].push_back(std::move(sym));
    }

    // 从当前作用域开始，向上遍历栈查找局部符号
    Symbol* lookup_local(std::string_view name) {
        for (size_t i = scopes_.size(); i-- > 0;) {
            if (Symbol* sym = scopes_[i]->lookup_local(name)) {
                return sym;
            }
        }
        return nullptr;
    }
    ASTNode* lookup_local_tpl(std::string_view name) {
        for (size_t i = scopes_.size(); i-- > 0;) {
            if (ASTNode* n = scopes_[i]->lookup_tpl(name)) {
                return n;
            }
        }
        return nullptr;
    }
    // 查找大表中所有同名持久符号（可能为空）
    const std::vector<Symbol>* lookup_global(const std::string& name) const {
        auto it = global_symbols_.find(name);
        return it == global_symbols_.end() ? nullptr : &it->second;
    }
    const std::vector<Symbol>* lookup_global(const std::vector<std::string>& name) const {
        
    }
    void merge_globals(const std::unordered_map<std::string,std::vector<Symbol>>& external_globals) {
        for (const auto& [name, syms] : external_globals) {
            auto& target = global_symbols_[name];
            // 简单合并，暂不检测重复，由驱动层或后续分析处理
            target.insert(target.end(), syms.begin(), syms.end());
        }
    }
    const std::unordered_map<std::string,std::vector<Symbol>>& get_global_symbols() const {
        return global_symbols_;
    }
};

// 重载候选与调用解析

struct OverloadCandidate {
    std::string name;           // 短名称（不带路径）
    std::string full_name;      // 完整限定名（如 "math::add"）
    std::string mangled;        // 名称修饰（用于 IR）
    std::string key;            // 参数类型编码（模板参数用 <n> 表示）
    size_t param_count = 0;
    Decl* decl = nullptr;       // 对应的函数声明（Func* 或 SpecialMethod*）
    Type* return_type = nullptr;
    bool has_body = false;
    bool has_template_params = false;  // 是否为模板函数
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
    // 状态
    CompUnit& cu_;
    SymbolTable symbols_;
    std::vector<Diagnostic> diagnostics_;
    bool has_error_ = false;

    // 统一重载表：完整限定名 -> 候选列表
    svmap<std::vector<OverloadCandidate>> overload_map_;

    // 调用解析结果
    std::vector<CallResolution> call_resolutions_;

    // 模板实例化缓存：键 = 函数完整限定名 + "(" + 具体参数类型编码 + ")"
    std::map<std::string, Func*> instantiated_templates_;

    int suppress_undefined_ = 0;
    #define X(name,str) Type* type##name;
    TYPE
    #undef X
    Type* typeString; // StringLiteral
    
public:
    explicit SemanticAnalyzer(CompUnit& cu) : cu_(cu){
        #define X(name,str) type##name = make_type(Location(), Type::Primitive{PrimitiveTypeKind::name});
        TYPE
        #undef X
        typeString = make_type(Location(), Type::StringLiteral{});
    }
    void merge_external_symbols(
        const SymbolTable& external_globals_table,
        const svmap<std::vector<OverloadCandidate>>& external_overloads
    );
    // 主入口
    bool analyze();

    // 诊断信息访问
    bool has_error() const { return has_error_; }
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    // 供其他模块查询
    const SymbolTable& symbol_table() const { return symbols_; }
    const svmap<std::vector<OverloadCandidate>>& overload_map() const { return overload_map_; }
    const std::vector<CallResolution>& call_resolutions() const { return call_resolutions_; }
    const CallResolution* find_call(Expr* e) const;

    // 类型转 IR 字符串
    std::string type_to_ir(Type& t) const;

    // 作用域管理（配合 ASTVisitor）
    void scope_in(std::string) override;
    void scope_out() override;

    void visit(CompUnit&) override;
    void visit(Import&) override;
    void visit(Modification&) override;

    void visit(Decl&) override;
    void visit_var_list(VarList&) override;
    void visit_func(Func&) override;
    void visit_namespace(Namespace&) override;
    void visit_extern(Extern&) override;
    void visit_class(Class&) override;
    void visit_special_method(SpecialMethod&) override;
    void visit_enum(Enum&) override;
    void visit_keep(Keep&) override;

    void visit(Expr&) override;
    void visit_expr_literal(Expr::Literal&) override;
    void visit_expr_identifier(Expr::Identifier&) override;
    void visit_expr_binary(Expr::Binary&) override;
    void visit_expr_unary(Expr::Unary&) override;
    void visit_expr_assign(Expr::Assign&) override;
    void visit_expr_call(Expr::Call&) override;
    void visit_expr_index(Expr::Index&) override;
    void visit_expr_member(Expr::Member&) override;
    void visit_expr_cast(Expr::Cast&) override;
    void visit_expr_array(Expr::Array&) override;
    void visit_expr_closure(Expr::Closure&) override;
    void visit_expr_ternary(Expr::Ternary&) override;

    void visit(Stmt&) override;
    void visit_stmt_block(Stmt::Block&) override;
    void visit_stmt_expr(Stmt::ExprStmt&) override;
    void visit_stmt_if(Stmt::If&) override;
    void visit_stmt_while(Stmt::While&) override;
    void visit_stmt_for(Stmt::For&) override;
    void visit_stmt_for_in(Stmt::ForIn&) override;
    void visit_stmt_return(Stmt::Return&) override;
    void visit_stmt_break(Stmt::Break&) override;
    void visit_stmt_continue(Stmt::Continue&) override;
    void visit_stmt_flag(Stmt::Flag&) override;
    void visit_stmt_goto(Stmt::Goto&) override;
    void visit_stmt_asm(Stmt::Asm&) override;
    void visit_stmt_keep(Stmt::Keep&) override;
    void visit_stmt_var_decl(Stmt::VarDecl&) override;
    void visit_stmt_spawn(Stmt::Spawn&) override;
    void visit_stmt_wait(Stmt::Wait&) override;

    void visit(Type&) override;
    void visit_type_primitive(Type::Primitive&) override;
    void visit_type_pointer(Type::Pointer&) override;
    void visit_type_array(Type::Array&) override;
    void visit_type_func(Type::Func&) override;
    void visit_type_custom(Type::Custom&) override;
    void visit_type_inferred(Type::Inferred&) override;
    void visit_type_void(Type::Void&) override;

private:

    // 诊断工具
    void error(const Location& loc, const std::string& msg);
    void note(const Location& loc, const std::string& msg);
    void warning(const Location& loc, const std::string& msg);

    // 类型工具
    Type* make_type(const Location& loc, Type::Variant v);
    std::string build_decl_key(const std::vector<VarList*>& params);
    std::string build_call_key(const std::vector<Expr*>& args);
    void set_expr_type(Expr* e, Type* t);
    Type* element_type(Type* t);
    void check_type(Type& t);

    struct FieldInfo {
        Type* type;
        bool is_readonly;
    } get_field_info(const std::string& cls, const std::string& field);

    // 声明收集与注册
    void collect();                     // 第一遍：收集所有声明到符号表和重载表
    void collect_decl(Decl* decl, const std::string& ns_prefix);
    void register_func(Func* f, const std::string& ns_name = "");
    void register_special(SpecialMethod* sm, const std::string& ns_name = "");

    // 辅助：获取当前作用域的完整路径字符串（用 "::" 连接）
    std::string get_current_full_path() const;

    // 调用解析
    void resolve_call(Expr* call, Expr::Call& c);
    const OverloadCandidate* match_candidates(
        const std::string& name,
        size_t arg_count,
        const std::vector<OverloadCandidate>& candidates);

    // 作用域路径解析（将 Expr 转换为命名空间路径）
    std::vector<std::string> resolve_namespace_path(Expr* target);

    // 模板实例化辅助
    struct TemplateDeduction {
        std::string param_name;
        Type* type = nullptr;
        Expr* expr = nullptr;
        std::string deduced_from;
        Location deduced_loc;
        bool is_deduced() const { return type != nullptr || expr != nullptr; }
    };

    bool can_instantiate(
        const OverloadCandidate& cand,
        const std::vector<Expr*>& args,
        std::vector<TemplateDeduction>& deductions);

    bool deduce_template_params(
        const Func* f,
        const std::vector<Expr*>& args,
        const std::vector<ASTNode*>& template_args,
        std::vector<TemplateDeduction>& deductions);

    Type* substitute_type(Type* t, const std::vector<TemplateDeduction>& deductions);
    Expr* substitute_expr(Expr* e, const std::vector<TemplateDeduction>& deductions);

    bool can_use_as_type(ASTNode* node);
    bool can_use_as_expr(ASTNode* node);
    bool param_used_as_type(const std::string& param_name, const Func* f);
    bool param_used_as_expr(const std::string& param_name, const Func* f);

    bool check_instantiated_body(Stmt* body,
                                 const std::vector<TemplateDeduction>& deductions);
    bool check_instantiated_expr(Expr* expr,
                                 const std::vector<TemplateDeduction>& deductions);

    // 判断类型是否为模板参数占位符
    bool is_template_param_type(Type* t, const Func* f = nullptr);
};

} // namespace vo