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

struct Scope;

enum class SymbolKind {
    Variable,
    Function,
    Class,
    Namespace,
    EnumVar,
};

struct Symbol {
    SymbolKind kind = SymbolKind::Variable;
    ASTNode* node = nullptr;
    Scope* scope = nullptr;         // 类/命名空间的符号表指针

    Symbol() = default;
    Symbol(SymbolKind k, ASTNode* n, Scope* s = nullptr)
        : kind(k), node(n), scope(s) {}
    explicit Symbol(Decl* d) : node(d) {
        switch (d->kind) {
            case DeclKind::VarList:       kind = SymbolKind::Variable; break;
            case DeclKind::Func:          kind = SymbolKind::Function; break;
            case DeclKind::Class:         kind = SymbolKind::Class; break;
            case DeclKind::Enum:          kind = SymbolKind::EnumVar; break;
            case DeclKind::Namespace:     kind = SymbolKind::Namespace; break;
            case DeclKind::SpecialMethod: kind = SymbolKind::Function; break;
            case DeclKind::Operator:      kind = SymbolKind::Function; break;
            default:                      kind = SymbolKind::Variable; break;
        }
    }
    Type* get_type() {
        if (!node) return nullptr;
        if (auto* vl = dynamic_cast<VarList*>(node)) {
            return vl->type;
        }
        if (auto* vi = dynamic_cast<VarItem*>(node)) {
            return vi->type;
        }
        return nullptr;
    }
    Decl* get_decl() {
        if (!node) return nullptr;
        if (Type* t = dynamic_cast<Type*>(node)) {
            if (auto* c = t->get_custom()) {
                return c->resolved_decl;
            }
        } else if (Decl* d = dynamic_cast<Decl*>(node)) {
            return d;
        }
        return nullptr;
    }
    template<typename T>
    T* get() {
        if (T* d = dynamic_cast<T*>(node)) {
            return d;
        }
        return nullptr;
    }
};

struct Scope {
    std::string name;
    Scope* parent = nullptr;
    std::vector<std::unique_ptr<Scope>> children;

    svmap<Symbol> locals;
    svmap<ASTNode*> tpls;
    // 函数，类，命名空间，成员变量的统一表，函数使用mangled name作为键
    std::unordered_map<std::string, Symbol, StringViewHash, StringViewEqual> members;

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

    inline bool insert_local(std::string_view name, Symbol sym) {
        auto it = locals.find(name);
        if (it != locals.end()) {
            if (it->second.kind == SymbolKind::Variable && sym.kind == SymbolKind::Variable) {
                it->second = std::move(sym);
                return true;
            }
            return false;
        }
        locals[name] = std::move(sym);
        return true;
    }

    inline Symbol* insert_member(const std::string& name, Symbol sym) {
        auto [it, inserted] = members.emplace(name, std::move(sym));
        if (!inserted) return nullptr;
        return &it->second;
    }

    inline Symbol* lookup_local(std::string_view name) {
        auto it = locals.find(name);
        return it == locals.end() ? nullptr : &it->second;
    }

    inline ASTNode* lookup_tpl(std::string_view name) {
        auto it = tpls.find(name);
        return it == tpls.end() ? nullptr : it->second;
    }

    inline Symbol* lookup_member(const std::string& name) {
        auto it = members.find(name);
        return it == members.end() ? nullptr : &it->second;
    }

    inline void insert_tpl(std::string_view name, ASTNode* n) {
        tpls[name] = n;
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

    bool insert_local(std::string_view name, Symbol sym) {
        return current_ ? current_->insert_local(name, std::move(sym)) : false;
    }

    Symbol* lookup_local(std::string_view name) {
        for (Scope* s = current_; s; s = s->parent) {
            if (Symbol* sym = s->lookup_local(name)) {
                return sym;
            }
        }
        return nullptr;
    }

    Symbol* insert_member(std::string name, Symbol sym) {
        if (current_) return current_->insert_member(std::move(name), std::move(sym));
        return nullptr;
    }

    Symbol* lookup_member(const std::string& name) {
        for (Scope* s = current_; s; s = s->parent) {
            if (Symbol* sym = s->lookup_member(name)) {
                return sym;
            }
        }
        return nullptr;
    }

    Symbol* insert_and_push(std::string name, Symbol sym) {
        Symbol* s = insert_member(name, std::move(sym));
        s->scope = push_scope(name);
        return s;
    }

    // 在指定 scope 中查找成员（不向上搜索）
    static Symbol* lookup_member_in(Scope* scope, const std::string& name) {
        if (!scope) return nullptr;
        return scope->lookup_member(std::move(name));
    }

    // 在指定 scope 及其父链中查找成员
    static Symbol* lookup_member_up(Scope* scope, const std::string& name) {
        for (Scope* s = scope; s; s = s->parent) {
            if (Symbol* sym = s->lookup_member(name)) {
                return sym;
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
};

// 调用解析
struct CallResolution {
    Expr* call = nullptr;
    std::string callee_name;
    std::string resolved_name;
    Decl* resolved_decl = nullptr;
    std::vector<Type*> return_types;  // 所有返回类型
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
    Type* typeInferred;
    
public:
    explicit SemanticAnalyzer(CompUnit& cu) : cu_(cu){
        #define X(name,str) type##name = make_type(Location(), Type::Primitive{PrimitiveTypeKind::name});
        TYPE
        #undef X
        typeString = make_type(Location(), Type::StringLiteral{});
        typeInferred = make_type(Location(), Type::Inferred{});
    }
    void merge_external_symbols(const SymbolTable& external){
        merge_scope(*symbols_.root(), *external.root());
    }
    void merge_scope(Scope& dst, const Scope& src) {
        for (const auto& [name, sym] : src.locals) {
            if (dst.locals.find(name) == dst.locals.end()) {
                dst.locals[name] = sym;
            }
        }
        for (const auto& [name, node] : src.tpls) {
            if (dst.tpls.find(name) == dst.tpls.end()) {
                dst.tpls[name] = node;
            }
        }
        for (const auto& [name, sym] : src.members) {
            if (dst.members.find(name) == dst.members.end()) {
                dst.members[name] = sym;
            }
        }
        for (const auto& src_child : src.children) {
            Scope* dst_child = dst.get_or_create_child(src_child->name);
            merge_scope(*dst_child, *src_child);
        }
    }
    // 主入口
    bool analyze();

    // 诊断信息访问
    bool has_error() const { return has_error_; }
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    // 供其他模块查询
    const SymbolTable& symbol_table() const { return symbols_; }
    const std::vector<CallResolution>& call_resolutions() const { return call_resolutions_; }
    const CallResolution* find_call(Expr* e) const;

    // 类型转字符串（用于生成 callkey）
    std::string type_to_key(Type& t);

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
    void visit_type_reference(Type::Reference&) override;
    void visit_type_inferred(Type::Inferred&) override;
    void visit_type_void(Type::Void&) override;
    void visit_type_string_literal(Type::StringLiteral&) override;

private:

    // 诊断工具
    void error(const Location& loc, const std::string& msg);
    void note(const Location& loc, const std::string& msg);
    void warning(const Location& loc, const std::string& msg);

    // 类型工具
    Type* make_type(const Location& loc, Type::Variant v);
    std::string build_callkey(const std::string& name, const std::vector<VarList*>& params);
    std::string build_callkey(const std::string& name, const std::vector<Expr*>& args);
    
    Symbol* find_operator_overload(OpKind op, Type& type, Type& other);
    Symbol* find_operator_overload(OpKind op, Type& type);
    bool can_convert_to(Type* src, Type* dst);

    // 类型判断
    bool is_numeric_type(Type* t);
    bool is_integer_type(Type* t);
    bool is_float_type(Type* t);
    bool is_bool_type(Type* t);
    bool is_same_type(Type* a, Type* b);
    bool is_integer_promotable(Type* t);
    bool is_void_type(Type* t);

    // 声明收集与注册
    void collect();
    void collect_decl(Decl* decl);
    void register_func(Func& f);
    void register_special(SpecialMethod& sm);

    // 辅助：获取当前作用域的完整路径字符串（用 "::" 连接）
    std::string get_current_full_path() const;

// 调用解析
    void resolve_call(Expr* call, Expr::Call& c);

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
        Symbol* sym,
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