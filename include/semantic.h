#pragma once

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

#include "ast.h"
#include "inject.h"
#include "flow_check.h"
#include "diagnostic.h"
#include "const_eval.h"
#include "string_view_map.h"

namespace cx {

// 泛型形参名 -> 实例化类型；键为字符串池中的名字，生命周期同 AST
using TypeSubst = std::unordered_map<std::string_view, Type*>;

// 泛型模板判定：带泛型形参表的类或函数
inline bool is_generic_decl(const Decl* d) {
    if (!d) return false;
    if (d->kind == DeclKind::Class)
        return !static_cast<const Class*>(d)->type_params.empty();
    if (d->kind == DeclKind::Func || d->kind == DeclKind::Operator ||
        d->kind == DeclKind::Conversion)
        return !static_cast<const Func*>(d)->type_params.empty();
    return false;
}

// 符号表与作用域

struct Scope;

enum class SymbolKind {
    Variable,
    Function,
    Class,
    Namespace,
    Enum,
    EnumVar,
    TypeAlias,
    ExprAlias,
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
            case DeclKind::TypeDef:       kind = SymbolKind::TypeAlias; break;
            case DeclKind::ExprDef:       kind = SymbolKind::ExprAlias; break;
            default:                      kind = SymbolKind::Variable; break;
        }
    }
    Type* get_type() {
        if (!node) return nullptr;
        if (auto* vi = dynamic_cast<VarItem*>(node)) {
            return vi->type;
        }
        return dynamic_cast<Type*>(node);
    }
    Decl* get_decl() {
        if (!node) return nullptr;
        if (auto* t = dynamic_cast<Type*>(node)) {
            return t->resolved_decl;
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

    // 直接切到某个作用域：泛型实例在模板的声明作用域里重新进入，
    // 无需从根按名字重走一遍
    void reset_scope(Scope* s) {
        if (s) current_ = s;
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

// 泛型的跨模块共享状态：模板登记与实例缓存。多模块下同一模板的同一组
// 实参只能有一个实例，否则两个模块各建一份会在 IR 里重名
struct GenericState {
    // 泛型模板登记：模板本体不进入语义遍历与 IR，实例化时由此取回
    struct Template {
        Decl* decl = nullptr;
        Scope* scope = nullptr;       // 实例符号登记到该作用域
        bool default_public = true;   // 成员默认私有，其余默认公开
    };
    std::unordered_map<const Decl*, Template> templates;
    std::map<std::pair<const Decl*, std::string>, Decl*> cache;
    // 实例符号所在的模板作用域：跨模块复用同一实例时也要能取回它的符号
    std::unordered_map<const Decl*, Scope*> instance_scope;
};

// 语义分析器
class SemanticAnalyzer : public ASTVisitor {
    // 状态
    CompUnit& cu_;
    SymbolTable symbols_;
    std::unique_ptr<InjectionExpander> injector_;
    std::unordered_map<VarItem*, Symbol*> var_symbols_;
    // 模块级变量的声明序号，用于检测初值里的前向引用
    std::unordered_map<const VarItem*, size_t> global_order_;
    void collect_global_order(Decl* d);
    // 单模块时用自带状态，多模块时由前端传入共享状态
    GenericState own_generics_;
    GenericState* generics_ = &own_generics_;
    // 常量求值的诊断出口：溢出、除零等直接进诊断表
    ConstEvalSink const_sink_;
    FlowChecker flow_;
    std::vector<Diagnostic> diagnostics_;
    bool has_error_ = false;

    // 语义分析期生成的整数字面量文本存储（deque 保证元素地址稳定）
    std::deque<std::string> literal_pool_;
    // T[] 的长度待推导；推导不出来时在收尾统一报错
    std::vector<Type*> pending_infer_arrays_;
    // T[] 推导不出长度时所处的声明位置：位置决定可用的补救写法
    enum class InferSite : uint8_t { Other, Local, Param, Return, Field };
    std::unordered_map<const Type*, InferSite> infer_sites_;
    void mark_infer_site(Type* t, InferSite site);
    // extern 块内的变量声明：存储在外部库，不受模块级变量限制。
    // 值是所属 extern 块的外部 ABI，决定施加哪一类约束
    std::unordered_map<const Decl*, ExternLang> extern_vars_;
    void collect_extern_vars();
    // expand() 是否已执行，analyze_rest() 会兜底调用
    bool expanded_ = false;
    // 别名展开深度上限：链式与互相定义到此为止
    static constexpr int kMaxDefDepth = 32;
    int def_depth_ = 0;

    // 泛型（实现见 src/semantic_generic.cpp）
    // 已实例化声明：语义主遍历之后挂到 top_decls，translator 只看得到它们
    std::vector<Decl*> generic_instances_;
    // 生成的实例名存储（deque 保证元素地址稳定，须活过 AST）
    std::deque<std::string> instance_names_;

    #define X(name,str) Type* type##name;
    TYPE
    #undef X
    Type* typeString; // StringLiteral
    Type* typeInferred;
    Type* typeNull;   // Null
    
public:
    explicit SemanticAnalyzer(CompUnit& cu, GenericState* generics = nullptr)
        : cu_(cu),
        generics_(generics ? generics : &own_generics_),
        flow_(var_symbols_,
            [this](const Location& loc, const std::string& msg) { error(loc, msg); }) {
        injector_ = std::make_unique<InjectionExpander>(cu_, symbols_,
            [this](const Location& loc, const std::string& msg) { error(loc, msg); },
            [this](const Location& loc, const std::string& msg) { warning(loc, msg); },
            [this](Decl* d, bool dp) { collect_decl(d, dp); });
        const_sink_.warn  = [this](const Location& loc, const std::string& msg) { warning(loc, msg); };
        const_sink_.error = [this](const Location& loc, const std::string& msg) { error(loc, msg); };
        #define X(name,str) type##name = make_type(Location(), Type::Primitive{PrimitiveTypeKind::name});
        TYPE
        #undef X
        typeString = make_type(Location(), Type::StringLiteralType{});
        typeInferred = make_type(Location(), Type::Inferred{});
        typeNull = make_type(Location(), Type::Null{});
    }
    void merge_external_symbols(const SymbolTable& external){
        symbols_.merge_from(external);
    }
    // 主入口
    bool analyze();
    // 仅收集符号表（不遍历函数体），供多模块分阶段编译
    void collect_only();
    // 类注入展开并把成员登记进类作用域；多模块下须所有模块都做完再进 analyze_rest
    void expand();
    // 遍历体与后续检查（需已收集/合并符号表）
    void analyze_rest();
    // 顶层 main 签名检查（符号表全局化后调用一次）
    void check_main();
    // 跨模块 extern ABI 一致性：extern 的符号名保持原样，同一外部符号在各模块
    // 的声明必须同签名，否则会给它发出两份互相冲突的 LLVM 声明（IR 非法，
    // 且调用点会走错调用约定）。诊断记在冲突的那份声明所属模块上，
    // 须在各模块自身诊断都已发出之后调用一次
    static void check_extern_abi_conflicts(
        const std::vector<SemanticAnalyzer*>& modules);

    // 诊断信息访问
    bool has_error() const { return has_error_; }
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    // 供其他模块查询
    const SymbolTable& symbol_table() const { return symbols_; }

    // 类型转字符串（用于生成 callkey）
    std::string type_to_key(Type& t);

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
    void visit_stmt_label(Stmt::Label&) override;
    void visit_stmt_goto(Stmt::Goto&) override;
    void visit_stmt_asm(Stmt::Asm&) override;
    void visit_stmt_keep(Stmt::Keep&) override;
    void visit_stmt_var_decl(Stmt::VarDecl&) override;
    void visit_stmt_def(Stmt::Def&) override;

    void visit_type_primitive(Type::Primitive&) override;
    void visit_type_pointer(Type::Pointer&) override;
    void visit_type_reference(Type::Reference&) override;
    void visit_type_array(Type::ArrayType&) override;
    void visit_type_identifier(Type::Identifier&) override;
    void visit_type_member(Type::Member&) override;
    void visit_type_func_ptr(Type::FuncPtr&) override;
    void visit_type_func_ref(Type::FuncRef&) override;
    void visit_type_inferred(Type::Inferred&) override;
    void visit_type_void(Type::Void&) override;
    void visit_type_string_literal(Type::StringLiteralType&) override;
    void visit_type_generic(Type::GenericType&) override;
    void visit_type_null(Type::Null&) override;
    void visit_expr_generic(Expr::GenericType&) override;

    // 泛型：模板摘除与登记（实例化在 src/semantic_generic.cpp）
    void collect_generics();
    void collect_generic_in(std::vector<Decl*>& list, Scope* scope,
                            bool default_public);
    void extract_generic_templates(std::vector<Decl*>& members, Scope* scope,
                                   bool default_public);
    Class* instantiate_class(Class& tmpl, const std::vector<Type*>& args,
                             const Location& loc);
    Func* instantiate_func(Func& tmpl, const std::vector<Type*>& args,
                           const std::vector<Type*>& arg_types,
                           const Location& loc);
    // 从实参类型推导形参类型的实例化：形参位置是泛型形参则绑定或校验一致性，
    // 否则退化为 can_convert_to 判定（含 is_same_type）
    bool deduce_type(Type* param, Type* arg, TypeSubst& subst,
                     const std::vector<TypeParam*>& tparams);
    // 模板的登记信息；模板已从 AST 摘除，仍可按声明指针取回
    bool generic_template_of(const Decl* d, GenericState::Template& out) const;
    // 克隆树中类型位置的泛型形参名替换为实参类型
    void substitute_types(Decl* d, const TypeSubst& subst);
    // 实例名与缓存键：类型键按实参顺序以 '$' 连接
    std::string instance_key(const std::vector<Type*>& args);
    // 作用域链拼出的限定名（根作用域不计）：实例挂到顶层声明后仍须唯一
    std::string scope_path(Scope* s) const;
    std::string_view intern_name(std::string name);
    // 实例声明的具名类型节点：与模板名同形（限定名保持限定）
    Type* instance_type_node(Type* base, Decl* inst);
    // 值位置的实例引用：指向实例的标识符/限定名，签名取实例类型
    Expr* instance_ref_node(Expr* base, Decl* inst);
    Symbol* instance_symbol(Decl* inst);
    // 在模板的声明作用域里立即分析实例：调用点随后拿到的签名已是就绪的。
    // 分析过程可能级联实例化别的泛型，靠实例缓存终止
    void analyze_instance(Decl* d, Scope* scope);
    // 泛型误用诊断：类型位置缺实参、泛型函数以值引用
    void check_generic_use(Type* t, Decl* d);
    void check_generic_ref_use(Expr* e, Decl* d);

private:

    // 诊断工具
    void error(const Location& loc, const std::string& msg);
    void note(const Location& loc, const std::string& msg);
    void warning(const Location& loc, const std::string& msg);

    // 类型工具
    Type* make_type(const Location& loc, Type::Variant v);
    Type* make_named_type(const Location& loc, std::string_view name,
                          Decl* decl = nullptr, Scope* scope = nullptr);
    Expr* make_int_literal(const Location& loc, size_t value);
    // 数组长度的编译期常量形式；非编译期常量返回 nullptr
    Expr* const_length_literal(Expr* len, int depth = 0);
    // T[]：按初值推导长度并就地改为定长数组，推导不出来时保持 Infer 待报错
    void resolve_infer_array_len(Type& type, Type* init_type, Expr* init_expr);
    void report_infer_arrays();
    // 类字段按值内嵌另一个类：自身内联或相互内联使布局无解，须报错
    void check_embedded_layout();
    // 内嵌依赖图上的 DFS：返回 false 表示已报错
    bool embed_visit(Class* c, std::unordered_map<Class*, int>& state);
    std::unordered_map<Class*, std::vector<std::pair<Class*, VarItem*>>> embed_;
    std::vector<Class*> embed_order_;
    // 函数签名（形参/返回类型）里不允许出现所有权切片 T{}
    void check_no_slice(const std::vector<VarList*>& lists, const char* what);
    // extern 函数声明的 ABI 约束：形参与返回值必须能与 C 侧对上
    void check_extern_func(Func& f);
    // extern 变量声明的约束：存储在外部库，类型须与外部符号的对象布局对上
    void check_extern_var_list(VarList& vl, ExternLang lang);
    std::string build_callkey(const std::string& name, const std::vector<VarList*>& params);
    std::string build_callkey(const std::string& name, const std::vector<Expr*>& args);
    
    Symbol* find_operator_overload(OpKind op, Type& type, Type& other,
                                   Type** matched_param = nullptr);
    Symbol* find_operator_overload(OpKind op, Type& type);
    // 实际上包含了 is_same_type ，使用之前不用再判 is_same_type
    bool can_convert_to(Type* src, Type* dst);
    // 基础类型来源的包装类判定；非包装类返回 nullptr
    Class* wrap_class_of(Type* t);
    // 任意类（含非包装类）的解析
    Class* wrap_or_plain_class(Type* t);
    // 表达式是否持有 malloc 实例：类本身是 malloc 类，或变量声明带 malloc 修饰
    bool is_malloc_value(Expr* e);
    // 包装类的承载类型
    Type* wrap_base_type(Class* c);
    bool has_conversion(Type* src, Type* dst);
    Symbol* find_conversion(Type& src, Type& dst);
    // 记录隐式转换（conversion 函数或包装转换）到表达式上；已处理返回 true
    bool record_conversion(Expr* e, Type* dst);
    std::string build_conv_key(Func& f);
    // = 赋值、() 传参、cast 转换三种情形共用的转换判定入口
    bool check_conversion(Type* src, Type* dst, const std::string& context,
                         const Location& loc, bool explicit_cast = false);
    // 数组字面量/字符串字面量的编译期元素数不得超过目标数组类型的长度约束
    void check_array_len_fit(Expr* src, Type* dst_type, const std::string& context,
                             const Location& loc);
    // 形参定长时强制实参数组长度相同；形参为所有权切片时实参长度任意
    void check_param_array_len(Expr* arg, Type* at, Type* pt, const Location& loc);
    // 整数字面量在转换到目标整数类型时是否溢出
    void check_literal_range(Expr* value, Type* dst, const std::string& context,
                             const Location& loc);

    // 类型判断
    Symbol* lookup_member_on_type(Type* type, const std::string& field, Scope** found_scope);

    // 声明收集与注册
    // P0：注册类名与类外声明，不进入类内部
    void collect();
    // P1：处理类的修饰注入并注册类成员（含嵌套类递归）
    void process_using_namespaces();
    void collect_decl(Decl* decl, bool default_public);
    void register_func(Func& f, bool default_public);
    void register_special(SpecialMethod& sm, bool default_public);
    // typedef/exprdef 使用点：节点内容换成目标的克隆件，再按新形态遍历一遍
    void expand_type_alias(Symbol* sym);
    void expand_expr_alias(Symbol* sym);

    bool is_scope_on_path(Scope* s) const;
    void check_access(Symbol* sym, const Location& loc);
    void check_write_access(Symbol* sym, const Location& loc);




    // 辅助：获取当前作用域的完整路径字符串（用 "::" 连接）
    std::string get_current_full_path() const;

    // 调用解析
    void resolve_call(Expr* call, Expr::Call& c);
    // 泛型函数调用：按显式或推导的实参实例化，被调用者改写为实例名。
    // 返回 false 表示实例化失败，错误已报出
    bool resolve_generic_call(Expr* call_expr, Expr::Call& c, Symbol*& callee_sym);
    void call_fail(Expr* call_expr, const std::string& msg);
    bool resolve_funcptr_call(Expr* call_expr, Expr::Call& c);
    bool resolve_functor_call(Expr* call_expr, Expr::Call& c);
    bool resolve_ctor_call(Expr* call_expr, Expr::Call& c,
                           Symbol* callee_sym, const std::string& name);
    bool resolve_free_builtin(Expr* call_expr, Expr::Call& c,
                              const std::string& name);
    void pack_array_args(Func* func, Expr* call_expr, Expr::Call& c);
    void pack_class_args(Func* func, Expr* call_expr, Expr::Call& c);
    void resolve_named_call(Expr* call_expr, Expr::Call& c,
                            Symbol* callee_sym, const std::string& name);
    void check_call_args(Func* fn, const std::vector<Expr*>& args,
                         const Location& loc);
    void check_call_args(const std::vector<VarList*>& params,
                         const std::vector<Expr*>& args, const Location& loc);
    // 平铺多个 VarList 声明的形参类型节点
    static std::vector<Type*> param_types(const std::vector<VarList*>& lists);
    // 引用形参：按被引用目标类型匹配且实参须为 lvalue。已按引用形参处理返回 true
    bool check_ref_param(Expr* arg, Type* param);
    bool args_match(const std::vector<VarList*>& params,
                    const std::vector<Expr*>& args);
    // vi 是否为当前函数最后一个返回声明中的返回变量
    bool is_return_var_item(VarItem* vi) const;
    // vi 是否为当前函数某个形参
    bool is_param_item(VarItem* vi) const;
    // vi 是否为当前类声明的字段
    bool is_field_item(VarItem* vi) const;
    // 当前类里名为 name 的字段符号（字段只登记为类成员）
    Symbol* class_field_symbol(std::string_view name) const;
    // 引用声明处初值的绑定检查：绑定源必须是同目标类型的对象
    void check_ref_init_source(const VarItem& var, const Type& dt);

    // 克隆辅助（深拷贝，新节点统一登记到 cu_）

};

} // namespace cx