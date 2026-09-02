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

struct Scope;  // 前向声明

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
    Scope* scope = nullptr;         // 类/命名空间的符号表指针
    std::string ir_type;            // LLVM IR 类型字符串（缓存）

    Symbol() = default;
    Symbol(SymbolKind k, std::string_view n, Type* t = nullptr, Decl* d = nullptr,
           Scope* s = nullptr, std::string ir = "")
        : kind(k), name(n), type(t), decl(d), scope(s), ir_type(std::move(ir)) {}
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

struct Scope {
    std::string name;
    Scope* parent = nullptr;
    std::vector<std::unique_ptr<Scope>> children;

    svmap<Symbol> locals;
    svmap<ASTNode*> tpls;
    std::unordered_map<std::string, std::vector<Symbol>> members;
    svmap<std::vector<OverloadCandidate>> overloads;

    Scope() = default;
    Scope(std::string n) : name(std::move(n)) {}

    Scope* find_child(std::string_view child_name) {
        for (auto& c : children) {
            if (c->name == child_name) return c.get();
        }
        return nullptr;
    }

    Scope* get_or_create_child(std::string child_name) {
        if (Scope* existing = find_child(child_name)) return existing;
        auto child = std::make_unique<Scope>(std::move(child_name));
        child->parent = this;
        Scope* raw = child.get();
        children.push_back(std::move(child));
        return raw;
    }

    inline bool insert_local(Symbol sym) {
        auto it = locals.find(sym.name);
        if (it != locals.end()) {
            if (it->second.kind == SymbolKind::Variable && sym.kind == SymbolKind::Variable) {
                it->second = std::move(sym);
                return true;
            }
            return false;
        }
        locals[sym.name] = std::move(sym);
        return true;
    }

    inline void insert_member(Symbol sym) {
        members[std::string(sym.name)].push_back(std::move(sym));
    }

    inline void insert_overload(const std::string& full_name, OverloadCandidate cand) {
        overloads[full_name].push_back(std::move(cand));
    }

    inline Symbol* lookup_local(std::string_view name) {
        auto it = locals.find(name);
        return it == locals.end() ? nullptr : &it->second;
    }

    inline ASTNode* lookup_tpl(std::string_view name) {
        auto it = tpls.find(name);
        return it == tpls.end() ? nullptr : it->second;
    }

    inline std::vector<Symbol>* lookup_member(std::string_view name) {
        auto it = members.find(std::string(name));
        return it == members.end() ? nullptr : &it->second;
    }

    inline std::vector<OverloadCandidate>* lookup_overload(std::string_view name) {
        auto it = overloads.find(name);
        return it == overloads.end() ? nullptr : &it->second;
    }

    inline void insert_tpl(std::string_view name, ASTNode* n) {
        tpls[name] = n;
    }

    void merge_from(const Scope& other) {
        for (const auto& [name, sym] : other.locals) {
            if (locals.find(name) == locals.end()) {
                locals[name] = sym;
            }
        }
        for (const auto& tpl : other.tpls) {
            if (tpls.find(tpl.first) == tpls.end()) {
                tpls[tpl.first] = tpl.second;
            }
        }
        for (const auto& [name, syms] : other.members) {
            auto& target = members[name];
            target.insert(target.end(), syms.begin(), syms.end());
        }
        for (const auto& [name, candidates] : other.overloads) {
            auto& target = overloads[name];
            target.insert(target.end(), candidates.begin(), candidates.end());
        }
    }
};

struct SymbolTable {
    std::unique_ptr<Scope> root_;
    Scope* current_ = nullptr;

    SymbolTable() {
        root_ = std::make_unique<Scope>("<global>");
        current_ = root_.get();
    }

    Scope* push_scope(const std::string& name) {
        current_ = current_->get_or_create_child(name);
        return current_;
    }

    void pop_scope() {
        if (current_ && current_->parent) {
            current_ = current_->parent;
        }
    }

    Scope* current_scope() const { return current_; }
    Scope* root() const { return root_.get(); }

    bool insert_local(Symbol sym) {
        return current_ ? current_->insert_local(std::move(sym)) : false;
    }

    Symbol* lookup_local(std::string_view name) {
        for (Scope* s = current_; s; s = s->parent) {
            if (Symbol* sym = s->lookup_local(name)) {
                return sym;
            }
        }
        return nullptr;
    }

    void insert_member(Symbol sym) {
        if (current_) current_->insert_member(std::move(sym));
    }

    std::vector<Symbol>* lookup_member(std::string_view name) {
        for (Scope* s = current_; s; s = s->parent) {
            if (auto* vec = s->lookup_member(name)) {
                return vec;
            }
        }
        return nullptr;
    }

    // 在指定 scope 中查找成员（不向上搜索）
    static std::vector<Symbol>* lookup_member_in(Scope* scope, std::string_view name) {
        if (!scope) return nullptr;
        return scope->lookup_member(name);
    }

    // 在指定 scope 及其父链中查找成员
    static std::vector<Symbol>* lookup_member_up(Scope* scope, std::string_view name) {
        for (Scope* s = scope; s; s = s->parent) {
            if (auto* vec = s->lookup_member(name)) {
                return vec;
            }
        }
        return nullptr;
    }

    void insert_overload(const std::string& full_name, OverloadCandidate cand) {
        if (current_) current_->insert_overload(full_name, std::move(cand));
    }

    std::vector<OverloadCandidate>* lookup_overload(std::string_view name) {
        for (Scope* s = current_; s; s = s->parent) {
            if (auto* vec = s->lookup_overload(name)) {
                return vec;
            }
        }
        return nullptr;
    }

    void insert_local_tpl(std::string_view name, ASTNode* n) {
        if (current_) current_->insert_tpl(name, n);
    }

    ASTNode* lookup_local_tpl(std::string_view name) {
        for (Scope* s = current_; s; s = s->parent) {
            if (ASTNode* n = s->lookup_tpl(name)) {
                return n;
            }
        }
        return nullptr;
    }

    void merge(const SymbolTable& other) {
        if (!other.root_) return;
        merge_scope(*root_, *other.root_);
    }

    static void merge_scope(Scope& dst, const Scope& src) {
        dst.merge_from(src);
        for (const auto& src_child : src.children) {
            Scope* dst_child = dst.get_or_create_child(src_child->name);
            merge_scope(*dst_child, *src_child);
        }
    }
};

// 调用解析
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

    // 调用解析结果
    std::vector<CallResolution> call_resolutions_;

    // 模板实例化缓存：键 = 函数完整限定名 + "(" + 具体参数类型编码 + ")"
    std::map<std::string, Func*> instantiated_templates_;

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
    void merge_external_symbols(const SymbolTable& external);
    // 主入口
    bool analyze();

    // 诊断信息访问
    bool has_error() const { return has_error_; }
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    // 供其他模块查询
    const SymbolTable& symbol_table() const { return symbols_; }
    std::vector<OverloadCandidate>* lookup_overload(std::string_view name) {
        return symbols_.lookup_overload(name);
    }
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
    bool has_operator_overload(Type* type, OpKind op);

    // 声明收集与注册
    void collect();                     // 第一遍：收集所有声明到符号表和重载表
    void collect_decl(Decl* decl);
    void register_func(Func* f);
    void register_special(SpecialMethod* sm);

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