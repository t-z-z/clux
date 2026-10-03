#pragma once

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

#include "ast.h"
#include "inject.h"
#include "flow_check.h"
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
    // 声明的修饰符（VarItem 借其所属 VarList 的），访问控制与 const 直接查此
    SpecList* specs = nullptr;
    // 未显式 pub/priv 时的容器默认：类成员默认私有，其余默认公开
    bool default_public = true;

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

    // 有效访问级别：priv > readonly > pub > default
    Access access_level() const {
        if (specs) {
            if (specs->has(SpecKind::Priv))     return Access::Private;
            if (specs->has(SpecKind::Readonly)) return Access::Readonly;
            if (specs->has(SpecKind::Pub))      return Access::Public;
        }
        return default_public ? Access::Public : Access::Private;
    }
    bool is_const() const {
        return specs && specs->has(SpecKind::Const);
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

    // 把 other 的符号并入本表；同名以本表已有者优先
    void merge_from(const SymbolTable& other) {
        if (!root_ || !other.root_) return;
        merge_scope(*root_, *other.root_);
    }
    static void merge_scope(Scope& dst, const Scope& src) {
        for (const auto& [name, sym] : src.locals) {
            if (dst.locals.find(name) == dst.locals.end()) dst.locals[name] = sym;
        }
        for (const auto& [name, sym] : src.members) {
            if (dst.members.find(name) == dst.members.end()) dst.members[name] = sym;
        }
        for (const auto& src_child : src.children) {
            Scope* dst_child = dst.get_or_create_child(src_child->name);
            merge_scope(*dst_child, *src_child);
        }
    }
};

// 语义分析器
class SemanticAnalyzer : public ASTVisitor {
    // 状态
    CompUnit& cu_;
    SymbolTable symbols_;
    std::unique_ptr<InjectionExpander> injector_;
    std::unordered_map<VarItem*, Symbol*> var_symbols_;
    FlowChecker flow_;
    std::vector<Diagnostic> diagnostics_;
    bool has_error_ = false;

    // 语义分析期生成的整数字面量文本存储（deque 保证元素地址稳定）
    std::deque<std::string> literal_pool_;

    #define X(name,str) Expr* type##name;
    TYPE
    #undef X
    Expr* typeString; // StringLiteral
    Expr* typeInferred;
    Expr* typeNull;   // Null
    
public:
    explicit SemanticAnalyzer(CompUnit& cu) : cu_(cu),
        flow_(var_symbols_,
            [this](const Location& loc, const std::string& msg) { error(loc, msg); }) {
        injector_ = std::make_unique<InjectionExpander>(cu_, symbols_,
            [this](const Location& loc, const std::string& msg) { error(loc, msg); },
            [this](Decl* d, bool dp) { collect_decl(d, dp); });
        #define X(name,str) type##name = make_type(Location(), Expr::Primitive{PrimitiveTypeKind::name});
        TYPE
        #undef X
        typeString = make_type(Location(), Expr::StringLiteralType{});
        typeInferred = make_type(Location(), Expr::Inferred{});
        typeNull = make_type(Location(), Expr::Null{});
    }
    void merge_external_symbols(const SymbolTable& external){
        symbols_.merge_from(external);
    }
    // 主入口
    bool analyze();
    // 仅收集符号表（不遍历函数体），供多模块分阶段编译
    void collect_only();
    // 遍历体与后续检查（需已收集/合并符号表）
    void analyze_rest();
    // 顶层 main 签名检查（符号表全局化后调用一次）
    void check_main();

    // 诊断信息访问
    bool has_error() const { return has_error_; }
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    // 供其他模块查询
    const SymbolTable& symbol_table() const { return symbols_; }

    // 类型转字符串（用于生成 callkey）
    std::string type_to_key(Expr& t);

    // 作用域管理（配合 ASTVisitor）
    void scope_in(std::string) override;
    void scope_out() override;

    void visit(CompUnit&) override;
    void visit(Import&) override;

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
    void visit_expr_null(Expr::Null&) override;
    void visit_expr_binary(Expr::Binary&) override;
    void visit_expr_unary(Expr::Unary&) override;
    void visit_expr_assign(Expr::Assign&) override;
    void visit_expr_call(Expr::Call&) override;
    void visit_expr_index(Expr::Index&) override;
    void visit_expr_member(Expr::Member&) override;
    void visit_expr_cast(Expr::Cast&) override;
    void visit_expr_array(Expr::Array&) override;
    void visit_expr_closure(Expr::Closure&) override;
    void visit_expr_spawn(Expr::Spawn&) override;
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
    
    Symbol* find_operator_overload(OpKind op, Expr& type, Expr& other,
                                   Expr** matched_param = nullptr);
    Symbol* find_operator_overload(OpKind op, Expr& type);
    bool can_convert_to(Expr* src, Expr* dst);
    // 基础类型来源的包装类判定；非包装类返回 nullptr
    Class* wrap_class_of(Expr* t);
    // 任意类（含非包装类）的解析
    Class* wrap_or_plain_class(Expr* t);
    // 包装类的承载类型表达式
    Expr* wrap_base_type(Class* c);
    bool has_conversion(Expr* src, Expr* dst);
    Symbol* find_conversion(Expr& src, Expr& dst);
    // 记录隐式转换（conversion 函数或包装转换）到表达式上；已处理返回 true
    bool record_conversion(Expr* e, Expr* dst);
    std::string build_conv_key(Func& f);
    // = 赋值、() 传参、cast 转换三种情形共用的转换判定入口
    bool check_conversion(Expr* src, Expr* dst, const std::string& context,
                         const Location& loc, bool explicit_cast = false);
    // 数组字面量/字符串字面量的编译期元素数不得超过目标数组类型的长度约束
    void check_array_len_fit(Expr* src, Expr* dst_type, const std::string& context,
                             const Location& loc);
    // 形参定长时强制实参数组长度相同；形参未定长时实参长度任意
    void check_param_array_len(Expr* arg, Expr* at, Expr* pt, const Location& loc);
    // 整数字面量在转换到目标整数类型时是否溢出
    void check_literal_range(Expr* value, Expr* dst, const std::string& context,
                             const Location& loc);

    // 类型判断
    Symbol* lookup_member_on_type(Expr* type, const std::string& field, Scope** found_scope);

    // 声明收集与注册
    // P0：注册类名与类外声明，不进入类内部
    void collect();
    // P1：处理类的修饰注入并注册类成员（含嵌套类递归）
    void process_using_namespaces();
    void collect_decl(Decl* decl, bool default_public);
    void register_func(Func& f, bool default_public);
    void register_special(SpecialMethod& sm, bool default_public);

    bool is_scope_on_path(Scope* s) const;
    void check_access(Symbol* sym, const Location& loc);
    void check_write_access(Symbol* sym, const Location& loc);




    // 辅助：获取当前作用域的完整路径字符串（用 "::" 连接）
    std::string get_current_full_path() const;

    // 调用解析
    void resolve_call(Expr* call, Expr::Call& c);
    void call_fail(Expr* call_expr, const std::string& msg);
    bool resolve_funcptr_call(Expr* call_expr, Expr::Call& c);
    bool resolve_functor_call(Expr* call_expr, Expr::Call& c);
    bool resolve_ctor_call(Expr* call_expr, Expr::Call& c,
                           Symbol* callee_sym, const std::string& name);
    bool resolve_free_builtin(Expr* call_expr, Expr::Call& c,
                              const std::string& name);
    void pack_slice_args(Func* func, Expr* call_expr, Expr::Call& c);
    void pack_class_args(Func* func, Expr* call_expr, Expr::Call& c);
    void resolve_named_call(Expr* call_expr, Expr::Call& c,
                            Symbol* callee_sym, const std::string& name);
    void check_call_args(Func* fn, const std::vector<Expr*>& args);
    void check_call_args(const std::vector<VarList*>& params,
                         const std::vector<Expr*>& args, const Location& loc);
    bool args_match(const std::vector<VarList*>& params,
                    const std::vector<Expr*>& args);
    // vi 是否为当前函数最后一个返回声明中的返回变量
    bool is_return_var_item(VarItem* vi) const;

    // 克隆辅助（深拷贝，新节点统一登记到 cu_）

};

} // namespace cx