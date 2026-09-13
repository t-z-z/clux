#pragma once

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "ast.h"
#include "diagnostic.h"
#include "string_view_map.h"

namespace cx {

// 符号表与作用域

struct Scope;

enum class SymbolKind {
    Variable,
    Function,
    Class,
    Namespace,
    Enum,
    EnumVar,
};

// 访问控制级别：priv 覆盖 readonly 覆盖 pub
enum class Access {
    Public,
    Readonly,
    Private,
};

struct Symbol {
    SymbolKind kind = SymbolKind::Variable;
    ASTNode* node = nullptr;
    Scope* scope = nullptr; // 类/命名空间的符号表指针，保证在collect时所有解析到的scope都会写入
    // 声明所在作用域；当前作用域不在 root->...->current 路径上时访问控制生效
    Scope* decl_scope = nullptr;
    Access access = Access::Public;

    Symbol() = default;
    Symbol(SymbolKind k, ASTNode* n, Scope* s = nullptr)
        : kind(k), node(n), scope(s) {}
    explicit Symbol(Decl* d) : node(d) {
        switch (d->kind) {
            case DeclKind::VarList:       kind = SymbolKind::Variable; break;
            case DeclKind::Func:          kind = SymbolKind::Function; break;
            case DeclKind::Class:         kind = SymbolKind::Class; break;
            case DeclKind::Enum:          kind = SymbolKind::Enum; break;
            case DeclKind::Namespace:     kind = SymbolKind::Namespace; break;
            case DeclKind::SpecialMethod: kind = SymbolKind::Function; break;
            case DeclKind::Operator:      kind = SymbolKind::Function; break;
            default:                      kind = SymbolKind::Variable; break;
        }
    }
    Expr* get_type() {
        if (!node) return nullptr;
        if (auto* vi = dynamic_cast<VarItem*>(node)) {
            return vi->type;
        }
        if (auto* e = dynamic_cast<Expr*>(node)) {
            if (e->is_type_node()) return e;
        }
        return nullptr;
    }
    Decl* get_decl() {
        if (!node) return nullptr;
        if (auto* e = dynamic_cast<Expr*>(node)) {
            return e->resolved_decl;
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

    // 键为自有字符串，避免拷贝/合并时引用他处存储导致悬垂
    std::unordered_map<std::string, Symbol, StringViewHash, StringViewEqual> locals;
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
        auto [it, inserted] = locals.emplace(std::string(name), std::move(sym));
        (void)it;
        return inserted;
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

    inline Symbol* lookup_member(const std::string& name) {
        auto it = members.find(name);
        return it == members.end() ? nullptr : &it->second;
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
        sym.decl_scope = current_;
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
        sym.decl_scope = current_;
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
};

// 调用解析
struct CallResolution {
    Expr* call = nullptr;
    Func* resolved_func = nullptr;
    Expr* type = nullptr;
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

    // 语义分析期生成的整数字面量文本存储（deque 保证元素地址稳定）
    std::deque<std::string> literal_pool_;

    #define X(name,str) Expr* type##name;
    TYPE
    #undef X
    Expr* typeString; // StringLiteral
    Expr* typeInferred;
    
public:
    explicit SemanticAnalyzer(CompUnit& cu) : cu_(cu){
        #define X(name,str) type##name = make_type(Location(), Expr::Primitive{PrimitiveTypeKind::name});
        TYPE
        #undef X
        typeString = make_type(Location(), Expr::StringLiteralType{});
        typeInferred = make_type(Location(), Expr::Inferred{});
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
    std::string type_to_key(Expr& t);

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

    void visit_type_primitive(Expr::Primitive&) override;
    void visit_type_pointer(Expr::Pointer&) override;
    void visit_type_array(Expr::ArrayType&) override;
    void visit_type_func_ptr(Expr::FuncPtr&) override;
    void visit_type_func_ref(Expr::FuncRef&) override;
    void visit_type_reference(Expr::Reference&) override;
    void visit_type_inferred(Expr::Inferred&) override;
    void visit_type_void(Expr::Void&) override;
    void visit_type_string_literal(Expr::StringLiteralType&) override;

private:

    // 诊断工具
    void error(const Location& loc, const std::string& msg);
    void note(const Location& loc, const std::string& msg);
    void warning(const Location& loc, const std::string& msg);

    // 类型工具
    Expr* make_type(const Location& loc, Expr::Variant v);
    Expr* make_named_type(const Location& loc, std::string_view name,
                          Decl* decl = nullptr, Scope* scope = nullptr);
    Expr* make_int_literal(const Location& loc, size_t value);
    std::string build_callkey(const std::string& name, const std::vector<VarList*>& params);
    std::string build_callkey(const std::string& name, const std::vector<Expr*>& args);
    
    Symbol* find_operator_overload(OpKind op, Expr& type, Expr& other);
    Symbol* find_operator_overload(OpKind op, Expr& type);
    bool can_convert_to(Expr* src, Expr* dst);
    bool has_conversion(Expr* src, Expr* dst);
    // = 赋值、() 传参、cast 转换三种情形共用的转换判定入口
    bool check_conversion(Expr* src, Expr* dst, const std::string& context,
                         const Location& loc, bool explicit_cast = false);

    // 类型判断
    bool is_numeric_type(Expr* t);
    bool is_integer_type(Expr* t);
    bool is_float_type(Expr* t);
    bool is_bool_type(Expr* t);
    bool is_same_type(Expr* a, Expr* b);
    bool is_integer_promotable(Expr* t);
    bool is_void_type(Expr* t);

    // 声明收集与注册
    void collect();
    void collect_decl(Decl* decl, Access default_access);
    void register_func(Func& f, Access default_access);
    void register_special(SpecialMethod& sm, Access default_access);

    // 访问控制：priv 覆盖 readonly 覆盖 pub，未显式声明时按容器默认
    static Access decl_access(const ModfList& modfs, Access fallback);
    bool is_scope_on_path(Scope* s) const;
    void check_access(Symbol* sym, const Location& loc);
    void check_write_access(Symbol* sym, const Location& loc);

    // flag/toflag 的 label 检查与变量初始化检查，按函数体（含闭包体）分别进行
    struct BodyInfo {
        Stmt* body = nullptr;
        std::vector<VarList*> params;
        std::vector<VarList*> returns;
    };
    std::vector<BodyInfo> bodies_;
    void check_jumps();

    std::unordered_map<VarItem*, Symbol*> var_symbols_;

    struct InitCtx {
        std::unordered_set<Symbol*> declared;
        std::unordered_set<Symbol*> assigned;
        std::vector<Symbol*> returns;
    };
    void check_initialization();
    void init_stmt(InitCtx& ctx, Stmt* s);
    void init_expr(InitCtx& ctx, Expr* e);
    void init_assign_target(InitCtx& ctx, Expr* e);

    // 辅助：获取当前作用域的完整路径字符串（用 "::" 连接）
    std::string get_current_full_path() const;

    // 调用解析
    void resolve_call(Expr* call, Expr::Call& c);
    void check_call_args(Func* fn, const std::vector<Expr*>& args);

    // 克隆辅助（深拷贝，新节点统一登记到 cu_）
    Expr* clone_type(Expr* t);
    Expr* clone_expr(Expr* e);
    Stmt* clone_stmt(Stmt* s);
    VarItem* clone_varitem(VarItem* vi);
    VarList* clone_varlist(VarList* vl);
    Func* clone_func(Func* f);
    Scope* clone_scope(Scope* src);
};

} // namespace cx