#pragma once

#include <vector>
#include <string>
#include <variant>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>

#include "token.h"

namespace vo{

struct ASTNode;
struct CompUnit;
struct Import;
struct ModfList;
struct Modification;
struct Type;
struct Expr;
struct Stmt;
struct Decl;
struct Cont;
struct VarItem;
struct VarList;
struct Func;
struct Operator;
struct Enum;
struct Namespace;
struct Extern;
struct Class;
struct SpecialMethod;
struct Keep;
struct TemplateParam;
struct ASTVisitor;

#define B_IS(a,b) inline bool is_##a() const { return std::holds_alternative<b>(data); }
#define B_B(p,k) p(const Location& l, k v) : data(std::move(v)), ASTNode(l) {}
#define B_GET(t,n) inline t* get_##n() { return std::get_if<t>(&data); }
//t* get_##n() { return std::get_if<t>(&data); }
#define BBB(n,t,p) B_IS(n,t) B_B(p,t) B_GET(t,n)
#define OP_KIND \
    /* 多种可能的的运算符 */\
    X(Plus, "+") \
    X(Minus, "-") \
    /* 二元运算符 */ \
    X(Mul, "*") \
    X(Div, "/") \
    X(Mod, "%") \
    X(Less, "<") \
    X(Greater, ">") \
    X(LessEqual, "<=") \
    X(GreaterEqual, ">=") \
    X(Equal, "==") \
    X(NotEqual, "!=") \
    X(BitOr, "|") \
    X(BitAnd, "&") \
    X(And, "&&") \
    X(Or, "||") \
    X(BitXor, "^") \
    X(Shl, "<<") \
    X(Shr, ">>") \
    /* 一元运算符 */ \
    X(Not, "!") \
    X(BitNot, "~") \
    /* 赋值运算符 */ \
    X(Assign, "=") \
    X(PlusAssign, "+=") \
    X(MinusAssign, "-=") \
    X(MulAssign, "*=") \
    X(DivAssign, "/=") \
    X(ModAssign, "%=") \
    X(AndAssign, "&=") \
    X(OrAssign, "|=") \
    X(XorAssign, "^=") \
    X(ShlAssign, "<<=") \
    X(ShrAssign, ">>=") \
    \
    X(Unknown, "")

#define LITERAL_MAPPING \
    X(Int8Literal, Int8) \
    X(ShortLiteral, Short) \
    X(IntLiteral, Int) \
    X(LongLiteral, Long) \
    X(Int128Literal, Int128) \
    X(UInt8Literal, UInt8) \
    X(UShortLiteral, UShort) \
    X(UIntLiteral, UInt) \
    X(ULongLiteral, ULong) \
    X(UInt128Literal, UInt128) \
    X(Float16Literal, Float16) \
    X(FloatLiteral, Float) \
    X(DoubleLiteral, Double) \
    X(CharLiteral, Char) \
    X(StringLiteral, String) \
    X(BoolLiteral, Bool)

struct ASTNode {
    Location loc;
    explicit ASTNode(const Location& l) : loc(l) {}
    virtual ~ASTNode() = default;
    // 明确移动语义
    ASTNode(ASTNode&&) = default;
    ASTNode& operator=(ASTNode&&) = default;
    // 禁止拷贝（如果有资源需要管理）
    ASTNode(const ASTNode&) = delete;
    ASTNode& operator=(const ASTNode&) = delete;
protected:
    ASTNode() = default;  // 仅派生类可访问
};

struct CompUnit {
    std::vector<Decl*> top_decls;
    std::vector<Import*> imports;
    std::vector<std::unique_ptr<ASTNode>> nodes;
    ASTNode* add_node(std::unique_ptr<ASTNode> ptr) {
        nodes.push_back(ptr);
        return ptr.get();
    }
    template<typename T, typename... Args>
    T* add_node(Args&&... args) {
        auto node = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = node.get();
        nodes.push_back(std::move(node));
        return raw;
    }
    std::unordered_map<std::string, Decl*> type_registry;  // 类型名 -> 类/结构体声明
    void register_type(const std::string_view& name, Decl* decl) {
        type_registry[std::string(name)] = decl;
    }
    bool is_type_name(const std::string_view& name) const {
        return type_registry.find(std::string(name)) != type_registry.end();
    }
    // namespace <name>: 文件顶层 using 指令（把命名空间成员引入当前作用域，hoisted）
    struct UsingNamespace {
        std::vector<std::string_view> path;
        Location loc;
    };
    std::vector<UsingNamespace> using_namespaces;
    void add_using_namespace(std::vector<std::string_view> path, const Location& loc) {
        using_namespaces.push_back({path, loc});
    }
};

enum class ModfKind {
    #define X(name,str) name,
    MODIFIER
    #undef X
};

inline ModfKind token_to_modf(TokenKind tk) {
    int offset = static_cast<int>(tk) - static_cast<int>(TokenKind::Const);
    return static_cast<ModfKind>(offset);
}

// 修饰符应用
struct Modification : ASTNode{
    std::string_view name;
    Decl* decl = nullptr;
    std::vector<ASTNode*> args;
    Modification(const Location&l, std::string_view n, std::vector<ASTNode*> a)
        :ASTNode(l), name(n), args(std::move(a)) {}
};

struct ModfList {
    uint32_t flags;
    std::vector<Modification*> customs;
    ModfList(): flags(0) {}
    void set(ModfKind kind) {
        flags |= (1u << static_cast<uint8_t>(kind));
    }
    void del(ModfKind kind) {
        flags &= ~(1u << static_cast<uint8_t>(kind));
    }
    bool has(ModfKind kind) const {
        return flags &(1u << static_cast<uint8_t>(kind));
    }
    void merge(const ModfList& other) {
        flags |= other.flags;
        for (Modification* mod : other.customs) {
            if (!has_custom(mod->name)) {
                customs.push_back(mod);
            }
        }
    }
    bool has_custom(const std::string_view &name) const {
        for (const Modification* mod : customs) {
            if (mod->name == name) return true;
        }
        return false;
    }
    void add_custom(Modification* mod) {
        if (!has_custom(mod->name))
            customs.push_back(mod);
    }
};

struct Import : ASTNode {
    bool no = false;
    std::string_view path;
    Import(const Location &loc, std::string_view p, bool n): path(p), no(n), ASTNode(loc) {}
    Import() = default;
};

enum class PrimitiveTypeKind {
    #define X(name,str) name,
    TYPE
    #undef X
};

inline PrimitiveTypeKind token_to_primitive_type(TokenKind kind) {
    return static_cast<PrimitiveTypeKind>(
        static_cast<int>(kind) - static_cast<int>(TokenKind::Int8)
    );
}

struct Type : ASTNode {
    // 变体列表
    struct Primitive {
        PrimitiveTypeKind kind;
    };
    struct Pointer {
        Type *pointee;
    };
    struct Array { 
        Type *element;
        Expr* length;
    };
    struct Func {
        std::vector<Type*> params;
        Type *return_type;
    };
    struct TypeParam {
        std::string_view name;
    };
    struct Custom {
        std::string_view name;
        std::vector<ASTNode*> args;        // 语义分析后填充
        Decl* resolved_decl = nullptr;  // 实际申明的位置
    };
    struct Reference {
        Type* target;
    };
    struct Inferred {};     // let 推导占位
    struct Void {};
    using Variant = std::variant<
        Primitive,
        Pointer,
        Array,
        Func,
        TypeParam,
        Custom,
        Reference,
        Inferred,
        Void,
    >;
    Variant data;
    Type() = default;
    explicit Type(const Location &loc, const Variant &d) : data(d), ASTNode(loc) {}
    BBB(primitive, Primitive, Type)
    BBB(pointer, Pointer, Type)
    BBB(array, Array, Type)
    BBB(func, Func, Type)
    BBB(type_param, TypeParam, Type)
    BBB(custom, Custom, Type)
    BBB(reference, Reference, Type)
    BBB(inferred, Inferred, Type)
    BBB(void, Void, Type)
};

enum class LiteralKind {
#define X(L,K) K,
LITERAL_MAPPING
#undef X
};

enum class OpKind{
    #define X(name, str) name,
    OP_KIND
    #undef X
    Paren,
    Index,
};

inline OpKind token_to_op(TokenKind kind) {
    switch (kind) {
        #define X(name, str) \
            case TokenKind::name: return OpKind::name;
        OP_KIND
        #undef X
        case TokenKind::AngleL: return OpKind::Less;
        case TokenKind::AngleR: return OpKind::Greater;
        default: return OpKind::Unknown;
    }
}
inline bool is_binary(OpKind op) {
    return op >= OpKind::Plus && op <= OpKind::Shr;
}
inline bool is_unary(OpKind op) {
    return op >= OpKind::Not && op <= OpKind::BitNot
        || op >= OpKind::Plus && op <= OpKind::Minus;
}
inline bool is_assign(OpKind op) {
    return op >= OpKind::Assign && op <= OpKind::ShrAssign;
}

struct Expr : ASTNode {
    struct Literal      { LiteralKind kind; std::string_view value; };
    struct Identifier   { std::string_view name; };
    struct Binary       { Expr* left; OpKind op; Expr* right; };
    struct Unary        { OpKind op; Expr* operand; bool is_postfix = false; };
    struct Assign       { Expr* target; OpKind op; Expr* value; };
    struct Call         { Expr* callee; std::vector<ASTNode*> templates; std::vector<Expr*> args; };
    struct Ternary      { Expr* cond; Expr* then_expr; Expr* else_expr; };
    struct Index        { Expr* target; Expr* index; };
    struct Member       { Expr* target; std::string_view member; };
    struct Scope        { Expr* target; std::string_view member; };
    struct Cast         { Type* target_type; Expr* operand; };
    struct Array        { std::vector<Expr*> members; Expr* fill_value = nullptr; };
    struct Closure      { std::vector<VarList*> params; std::vector<VarList*> returns;
                        Stmt* body = nullptr; std::vector<Token> capture;};
    struct PrimitiveType{ Type* type; };
    
    using Variant = std::variant<
        Literal, Identifier, Binary, Unary, Assign,
        Call, Ternary, Index, Member, Scope,
        Cast, Array, Closure, PrimitiveType
    >;
    
    Variant data;
    Type* type = nullptr;
    bool is_lvalue = false; // 是否可取地址
    
    Expr() = default;
    Expr(const Location& l, const Variant& v) : data(v), ASTNode(l) {}
    BBB(literal, Literal, Expr)
    BBB(identifier, Identifier, Expr)
    BBB(binary, Binary, Expr)
    BBB(unary, Unary, Expr)
    BBB(assign, Assign, Expr)
    BBB(call, Call, Expr)
    BBB(ternary, Ternary, Expr)
    BBB(index, Index, Expr)
    BBB(member, Member, Expr)
    BBB(scope, Scope, Expr)
    BBB(cast, Cast, Expr)
    BBB(array, Array, Expr)
    BBB(closure, Closure, Expr)
    BBB(primitive_type, PrimitiveType, Expr)
    std::optional<Type> into_type() {
        if (Identifier* i = get_identifier()) {
            return Type(loc,Type::Custom{i->name, std::vector<ASTNode*>(), nullptr});
        } else if (Call* c = get_call()) {
            if (Identifier* i = c->callee->get_identifier()) {
                return Type(loc,Type::Custom{i->name, c->templates, nullptr});
            }
        } else if (PrimitiveType* p = get_primitive_type()) {
            if (p->type){
                return Type(p->type->loc,p->type->data);
            }
        }
        return std::nullopt;
    }
};

struct Stmt : ASTNode {
    struct Block    { std::vector<Stmt*> stmts; };
    struct ExprStmt { Expr* expr; }; // 表达式能作为语句，语句不能作为表达式，除非是闭包
    struct If       { Expr* condition; Stmt* then_branch; Stmt* else_branch; };
    struct While    { Expr* condition; Stmt* body; };
    struct ForIn    { Token var_name; Expr* collection; Stmt* body; };
    struct For      { Stmt* init; Expr* condition; Stmt* next; Stmt* body; };
    struct Return   { };
    struct Break    { };
    struct Continue { };
    struct Flag     { std::string_view label; };
    struct Goto     { std::string_view label; };
    struct Asm      { std::string_view code; };
    struct Keep     { std::string_view tokens; };
    struct VarDecl  { VarList* vars; };
    struct Spawn    { Stmt* body; };
    struct Wait     { Expr* condition; };
    using Variant = std::variant<
        Block, ExprStmt, If, While, For, ForIn,
        Return, Break, Continue, Flag, Goto,
        Asm, Keep, VarDecl, Spawn, Wait
    >;
    Variant data;
    Stmt() = default;
    Stmt(const Location& l, Variant v): data(std::move(v)), ASTNode(l) {}
    BBB(block, Block, Stmt)
    BBB(expr, ExprStmt, Stmt)
    BBB(if, If, Stmt)
    BBB(while, While, Stmt)
    BBB(for, For, Stmt)
    BBB(for_in, ForIn, Stmt)
    BBB(return, Return, Stmt)
    BBB(break, Break, Stmt)
    BBB(continue, Continue, Stmt)
    BBB(flag, Flag, Stmt)
    BBB(goto, Goto, Stmt)
    BBB(asm, Asm, Stmt)
    BBB(keep, Keep, Stmt)
    BBB(var_decl, VarDecl, Stmt)
    BBB(spawn, Spawn, Stmt)
    BBB(wait, Wait, Stmt)
};

enum class DeclKind {
    // 等级从低到高
    Keep, VarList, SpecialMethod, Func, Compose, Operator, Enum, Class, Extern, Namespace,
};

struct Decl : ASTNode {
    std::string_view name;
    ModfList modfs;
    DeclKind kind;
    explicit Decl( const Location &loc, std::string_view n,
        const DeclKind &k, ModfList ml=ModfList()) :
        ASTNode(loc), kind(k), name(n), modfs(std::move(ml)) {}
    Decl() = default;virtual ~Decl() = default;
};

struct Cont : Decl { // container 容器，用于包含申明
    std::vector<Decl*> members;
    explicit Cont(const Location &loc, std::string_view n, const DeclKind &k,
        ModfList ml=ModfList(), std::vector<Decl*> m = {}) :
        Decl(loc, n, k, std::move(ml)), members(std::move(m)) {}
    Cont() = default;virtual ~Cont() = default;
    bool can_contain(const Decl *d) const {
        return d->kind <= kind;
    }
    bool can_contain(const DeclKind &k) const {
        return k <= kind;
    }
    bool add_member(Decl *member) {
        if (!can_contain(member)) {
            return false;
        }
        members.push_back(member);
        return true;
    }
    Decl* find_member(const std::string_view &name) const {
        for (Decl* m : members) if (m->name == name) return m;
        return nullptr;
    }
};
// VarItem 是类型无关的单个变量申明
struct VarItem{
    std::string_view name;
    Expr *init;
    VarItem() = default;
    VarItem(std::string_view n, Expr* i):
        name(n), init(i) {}
};
// VarList 是同一种类型的多个变量申明
struct VarList : Decl {
    Type* type = nullptr;
    std::vector<VarItem> vars;
    VarList(const Location& loc,ModfList ml,
            Type* t, std::vector<VarItem> v
        ):
        Decl(loc, "", DeclKind::VarList, std::move(ml)),
        type(t), vars(std::move(v)) {}
};

struct TemplateParam {
    std::string_view name;
    ASTNode* dft = nullptr;
    TemplateParam() = default;
    TemplateParam(std::string_view n, ASTNode* a) : name(n),dft(a) {}
};

struct Func : Decl {
    std::vector<TemplateParam> templates; // 泛型-<T>
    std::vector<VarList*> params;   // 圆括号(int a, $bool cond)
    std::vector<VarList*> returns;  // 返回值声明
    Stmt* body = nullptr;           // 函数体
    Func() = default;
    Func(const Location& loc,
        std::string_view name,
        ModfList ml,
        std::vector<TemplateParam> t,
        std::vector<VarList*> p,
        std::vector<VarList*> r,
        Stmt* b
        )
        : Decl(loc, name, DeclKind::Func, std::move(ml)),
        templates(std::move(t)),
        params(std::move(p)),
        returns(std::move(r)),
        body(b) {}
    virtual ~Func() = default;
};

struct Compose : Cont {
    Compose() = default;
    Compose(const Location& loc, ModfList ml, std::vector<Decl*> component):
        Cont(loc, "co", DeclKind::Compose, std::move(ml), component) {}
};

struct Operator : Func {
    OpKind op;
    Operator() = default;
    Operator(
        const Location& loc, 
        OpKind a,
        ModfList ml,
        std::vector<TemplateParam> t,
        std::vector<VarList*> p,
        std::vector<VarList*> r,
        Stmt* b
    ) : 
        Func(loc, "op", std::move(ml),
        std::move(t), std::move(p), std::move(r), b), op(a) {
            kind = DeclKind::Operator;
        }
};

struct Namespace : Cont {
    Namespace() = default;
    Namespace(
        const Location &loc, std::string_view name,
        ModfList ml, std::vector<Decl*> members
    ):
        Cont(loc, name, DeclKind::Namespace, std::move(ml), std::move(members)) {}
};

struct Extern : Cont {
    Extern() = default;
    Extern(const Location& loc,
           std::string_view name,
           ModfList ml,
           std::vector<Decl*> m)
        : Cont(loc, name, DeclKind::Extern, std::move(ml), std::move(m)) {}
};

struct Class : Cont {
    std::vector<TemplateParam> templates;
    Class(const Location &loc,
        std::string_view name,
        ModfList ml,
        std::vector<Decl*> m,
        std::vector<TemplateParam> tpls = {}
    ):
        Cont(loc, name, DeclKind::Class, std::move(ml), std::move(m)),
        templates(std::move(tpls)) {}
    virtual ~Class() = default;
};

struct SpecialMethod : Decl {
    enum class Kind {
        Init,       // 类名初始化函数
        Drop,       // 折构
        Ref,        // 被引用
        Deref,      // 结束引用
        Access,     // 获取
        Conversion  // 类型转换
    } kind;
    Stmt* body = nullptr;
    VarList* conv = nullptr;
    SpecialMethod() = default;
    SpecialMethod (
        const Location& loc,
        SpecialMethod::Kind k,
        ModfList ml,
        Stmt* b,
        VarList* r = nullptr
    ):
        Decl(loc, "", DeclKind::SpecialMethod, std::move(ml)),
        kind(k), body(b), conv(r) {}
};

struct Keep : Decl {
    std::string_view code;
    Keep(const Location &loc, std::string_view c, ModfList ml)
        : Decl(loc, "", DeclKind::Keep, std::move(ml)), code(c) {}
};

struct Enum : Decl {
    std::vector<Token> variants;
    Enum() = default;
    Enum(const Location&loc, std::vector<Token> var, ModfList ml):
        Decl(loc, "", DeclKind::Enum, std::move(ml)), variants(std::move(var)) {}
};

struct ASTVisitor {
    virtual ~ASTVisitor() = default;

    void walk(CompUnit& cu) {
        visit(cu);
        for (auto* d : cu.top_decls) {
            if (d) walk(*d);
        }
    }

    void walk(Decl& decl) {
        for (auto* mod : decl.modfs.customs) {
            if (mod) walk(*mod);
        }

        visit(decl);

        walk_children(decl);
    }
    void walk(Expr& expr) {
        visit(expr);
        walk_children(expr);
    }
    void walk(Stmt& stmt) {
        visit(stmt);
        walk_children(stmt);
    }
    void walk(Type& type) {
        visit(type);
        walk_children(type);
    }
    void walk(Modification& mod) {
        visit(mod);
        walk_children(mod);
    }

    virtual void visit(CompUnit&) {}
    virtual void visit(Import&) {}
    virtual void visit(Modification&) {}

    virtual void visit(Decl&) {}
    virtual void visit_var_list(VarList&) {}
    virtual void visit_func(Func&) {}
    virtual void visit_operator(Operator&) {}
    virtual void visit_namespace(Namespace&) {}
    virtual void visit_extern(Extern&) {}
    virtual void visit_class(Class&) {}
    virtual void visit_special_method(SpecialMethod&) {}
    virtual void visit_enum(Enum&) {}
    virtual void visit_keep(Keep&) {}

    virtual void visit(Expr& node) {
        std::visit([this](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Expr::Literal>) visit_expr_literal(v);
            else if constexpr (std::is_same_v<T, Expr::Identifier>) visit_expr_identifier(v);
            else if constexpr (std::is_same_v<T, Expr::Binary>) visit_expr_binary(v);
            else if constexpr (std::is_same_v<T, Expr::Unary>) visit_expr_unary(v);
            else if constexpr (std::is_same_v<T, Expr::Assign>) visit_expr_assign(v);
            else if constexpr (std::is_same_v<T, Expr::Call>) visit_expr_call(v);
            else if constexpr (std::is_same_v<T, Expr::Index>) visit_expr_index(v);
            else if constexpr (std::is_same_v<T, Expr::Member>) visit_expr_member(v);
            else if constexpr (std::is_same_v<T, Expr::Scope>) visit_expr_scope(v);
            else if constexpr (std::is_same_v<T, Expr::Cast>) visit_expr_cast(v);
            else if constexpr (std::is_same_v<T, Expr::Array>) visit_expr_array(v);
            else if constexpr (std::is_same_v<T, Expr::Closure>) visit_expr_closure(v);
        }, node.data);
    }

    virtual void visit_expr_literal(Expr::Literal&) {}
    virtual void visit_expr_identifier(Expr::Identifier&) {}
    virtual void visit_expr_binary(Expr::Binary&) {}
    virtual void visit_expr_unary(Expr::Unary&) {}
    virtual void visit_expr_assign(Expr::Assign&) {}
    virtual void visit_expr_call(Expr::Call&) {}
    virtual void visit_expr_index(Expr::Index&) {}
    virtual void visit_expr_member(Expr::Member&) {}
    virtual void visit_expr_scope(Expr::Scope&) {}
    virtual void visit_expr_cast(Expr::Cast&) {}
    virtual void visit_expr_array(Expr::Array&) {}
    virtual void visit_expr_closure(Expr::Closure&) {}

    virtual void visit(Stmt& node) {
        std::visit([this](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Stmt::Block>) visit_stmt_block(v);
            else if constexpr (std::is_same_v<T, Stmt::ExprStmt>) visit_stmt_expr(v);
            else if constexpr (std::is_same_v<T, Stmt::If>) visit_stmt_if(v);
            else if constexpr (std::is_same_v<T, Stmt::While>) visit_stmt_while(v);
            else if constexpr (std::is_same_v<T, Stmt::For>) visit_stmt_for(v);
            else if constexpr (std::is_same_v<T, Stmt::ForIn>) visit_stmt_for_in(v);
            else if constexpr (std::is_same_v<T, Stmt::Return>) visit_stmt_return(v);
            else if constexpr (std::is_same_v<T, Stmt::Break>) visit_stmt_break(v);
            else if constexpr (std::is_same_v<T, Stmt::Continue>) visit_stmt_continue(v);
            else if constexpr (std::is_same_v<T, Stmt::Flag>) visit_stmt_flag(v);
            else if constexpr (std::is_same_v<T, Stmt::Goto>) visit_stmt_goto(v);
            else if constexpr (std::is_same_v<T, Stmt::Asm>) visit_stmt_asm(v);
            else if constexpr (std::is_same_v<T, Stmt::Keep>) visit_stmt_keep(v);
            else if constexpr (std::is_same_v<T, Stmt::VarDecl>) visit_stmt_var_decl(v);
            else if constexpr (std::is_same_v<T, Stmt::Spawn>) visit_stmt_spawn(v);
            else if constexpr (std::is_same_v<T, Stmt::Wait>) visit_stmt_wait(v);
        }, node.data);
    }

    virtual void visit_stmt_block(Stmt::Block&) {}
    virtual void visit_stmt_expr(Stmt::ExprStmt&) {}
    virtual void visit_stmt_if(Stmt::If&) {}
    virtual void visit_stmt_while(Stmt::While&) {}
    virtual void visit_stmt_for(Stmt::For&) {}
    virtual void visit_stmt_for_in(Stmt::ForIn&) {}
    virtual void visit_stmt_return(Stmt::Return&) {}
    virtual void visit_stmt_break(Stmt::Break&) {}
    virtual void visit_stmt_continue(Stmt::Continue&) {}
    virtual void visit_stmt_flag(Stmt::Flag&) {}
    virtual void visit_stmt_goto(Stmt::Goto&) {}
    virtual void visit_stmt_asm(Stmt::Asm&) {}
    virtual void visit_stmt_keep(Stmt::Keep&) {}
    virtual void visit_stmt_var_decl(Stmt::VarDecl&) {}
    virtual void visit_stmt_spawn(Stmt::Spawn&) {}
    virtual void visit_stmt_wait(Stmt::Wait&) {}

    virtual void visit(Type& node) {
        std::visit([this](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Type::Primitive>) visit_type_primitive(v);
            else if constexpr (std::is_same_v<T, Type::Pointer>) visit_type_pointer(v);
            else if constexpr (std::is_same_v<T, Type::Array>) visit_type_array(v);
            else if constexpr (std::is_same_v<T, Type::Func>) visit_type_func(v);
            else if constexpr (std::is_same_v<T, Type::TypeParam>) visit_type_param(v);
            else if constexpr (std::is_same_v<T, Type::Custom>) visit_type_custom(v);
            else if constexpr (std::is_same_v<T, Type::Inferred>) visit_type_inferred(v);
            else if constexpr (std::is_same_v<T, Type::Void>) visit_type_void(v);
        }, node.data);
    }

    virtual void visit_type_primitive(Type::Primitive&) {}
    virtual void visit_type_pointer(Type::Pointer&) {}
    virtual void visit_type_array(Type::Array&) {}
    virtual void visit_type_func(Type::Func&) {}
    virtual void visit_type_param(Type::TypeParam&) {}
    virtual void visit_type_custom(Type::Custom&) {}
    virtual void visit_type_inferred(Type::Inferred&) {}
    virtual void visit_type_void(Type::Void&) {}

    // 递归遍历子节点（基类驱动，结构变更只改这里）
    void walk_children(Decl& decl) {
        switch (decl.kind) {
            case DeclKind::VarList:       walk_children(static_cast<VarList&>(decl)); break;
            case DeclKind::Func:          walk_children(static_cast<Func&>(decl)); break;
            case DeclKind::Operator:      walk_children(static_cast<Operator&>(decl)); break;
            case DeclKind::Class:         walk_children(static_cast<Class&>(decl)); break;
            case DeclKind::Namespace:     walk_children(static_cast<Namespace&>(decl)); break;
            case DeclKind::Extern:        walk_children(static_cast<Extern&>(decl)); break;
            case DeclKind::SpecialMethod: walk_children(static_cast<SpecialMethod&>(decl)); break;
            case DeclKind::Enum:          walk_children(static_cast<Enum&>(decl)); break;
            case DeclKind::Keep:          walk_children(static_cast<Keep&>(decl)); break;
            default: break;
        }
    }

    void walk_children(Func& f) {
        for (auto* p : f.params) {
            if (p) walk(*p);
        }
        for (auto* r : f.returns) {
            if (r) walk(*r);
        }
        if (f.body) walk(*f.body);
    }

    void walk_children(VarList& vl) {
        if (vl.type) walk(*vl.type);
        for (auto& v : vl.vars) {
            if (v.init) walk(*v.init);
        }
    }

    void walk_children(Class& c) {
        for (auto* m : c.members) {
            if (m) walk(*m);
        }
    }

    void walk_children(Namespace& ns) {
        for (auto* m : ns.members) {
            if (m) walk(*m);
        }
    }

    void walk_children(Extern& e) {
        for (auto* m : e.members) {
            if (m) walk(*m);
        }
    }

    void walk_children(Operator& op) {
        // 复用 Func 的遍历
        walk_children(static_cast<Func&>(op));
    }

    void walk_children(SpecialMethod& sm) {
        if (sm.body) walk(*sm.body);
        if (sm.conv) walk(*sm.conv);
    }
    void walk_children(Enum& e) {
    }
    void walk_children(Keep& k) {
    }
    void walk_children(Expr& expr) {
        std::visit([this](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Expr::Binary>) {
                if (v.left) walk(*v.left);
                if (v.right) walk(*v.right);
            } else if constexpr (std::is_same_v<T, Expr::Unary>) {
                if (v.operand) walk(*v.operand);
            } else if constexpr (std::is_same_v<T, Expr::Assign>) {
                if (v.target) walk(*v.target);
                if (v.value) walk(*v.value);
            } else if constexpr (std::is_same_v<T, Expr::Call>) {
                if (v.callee) walk(*v.callee);
                for (auto* a : v.args) if (a) walk(*a);
            } else if constexpr (std::is_same_v<T, Expr::Ternary>) {
                if (v.cond) walk(*v.cond);
                if (v.then_expr) walk(*v.then_expr);
                if (v.else_expr) walk(*v.else_expr);
            } else if constexpr (std::is_same_v<T, Expr::Index>) {
                if (v.target) walk(*v.target);
                if (v.index) walk(*v.index);
            } else if constexpr (std::is_same_v<T, Expr::Member>) {
                if (v.target) walk(*v.target);
            } else if constexpr (std::is_same_v<T, Expr::Scope>) {
                if (v.target) walk(*v.target);
            } else if constexpr (std::is_same_v<T, Expr::Cast>) {
                if (v.operand) walk(*v.operand);
                if (v.target_type) walk(*v.target_type);
            } else if constexpr (std::is_same_v<T, Expr::Array>) {
                for (auto* m : v.members) if (m) walk(*m);
                if (v.fill_value) walk(*v.fill_value);
            } else if constexpr (std::is_same_v<T, Expr::Closure>) {
                for (auto* p : v.params) if (p) walk(*p);
                for (auto* r : v.returns) if (r) walk(*r);
                if (v.body) walk(*v.body);
            }
        }, expr.data);
    }

    void walk_children(Stmt& stmt) {
        std::visit([this](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Stmt::Block>) {
                for (auto* s : v.stmts) if (s) walk(*s);
            } else if constexpr (std::is_same_v<T, Stmt::ExprStmt>) {
                if (v.expr) walk(*v.expr);
            } else if constexpr (std::is_same_v<T, Stmt::If>) {
                if (v.condition) walk(*v.condition);
                if (v.then_branch) walk(*v.then_branch);
                if (v.else_branch) walk(*v.else_branch);
            } else if constexpr (std::is_same_v<T, Stmt::While>) {
                if (v.condition) walk(*v.condition);
                if (v.body) walk(*v.body);
            } else if constexpr (std::is_same_v<T, Stmt::For>) {
                if (v.init) walk(*v.init);
                if (v.condition) walk(*v.condition);
                if (v.next) walk(*v.next);
                if (v.body) walk(*v.body);
            } else if constexpr (std::is_same_v<T, Stmt::ForIn>) {
                if (v.collection) walk(*v.collection);
                if (v.body) walk(*v.body);
            } else if constexpr (std::is_same_v<T, Stmt::VarDecl>) {
                if (v.vars) walk(*v.vars);
            } else if constexpr (std::is_same_v<T, Stmt::Spawn>) {
                if (v.body) walk(*v.body);
            } else if constexpr (std::is_same_v<T, Stmt::Wait>) {
                if (v.condition) walk(*v.condition);
            }
        }, stmt.data);
    }

    void walk_children(Type& type) {
        std::visit([this](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Type::Pointer>) {
                if (v.pointee) walk(*v.pointee);
            } else if constexpr (std::is_same_v<T, Type::Array>) {
                if (v.element) walk(*v.element);
                if (v.length) walk(*v.length);
            } else if constexpr (std::is_same_v<T, Type::Func>) {
                for (auto* p : v.params) if (p) walk(*p);
                if (v.return_type) walk(*v.return_type);
            } else if constexpr (std::is_same_v<T, Type::Custom>) {
                if (v.resolved_decl) walk(*v.resolved_decl);
            }
        }, type.data);
    }

    void walk_children(Modification& mod) {
    }
};

}// namespace vo