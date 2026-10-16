#pragma once

#include <vector>
#include <string>
#include <variant>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "string_view_map.h"
#include "token.h"

// AST 语义产物契约
//
// 值与类型分属两种节点：Expr 是值表达式，Type 是类型节点（类型即表达式）。
// 两侧各有 Identifier / Member / GenericType / Null / Inferred —— 同形语法在
// 值位置与类型位置的语义、子节点类型都不同，合一个节点就得靠标志位再分派一次。
// 静态区分之后不再有"该节点是类型吗"的运行时判断：类型即 Type*。
//
// 编译流程分三段，每段结束后进入下一个段的字段必须已就绪：
//
//   P0  collect_only()     仅收集符号表（P0 区，不进入函数体/类成员）
//   P1a expand()           process_using_namespaces → injector_->expand_all
//                          （类注入展开并把成员登记进类作用域）
//   P1b analyze_rest()     expand() → collect_global_order → collect_extern_vars
//                          → walk(cu_)（表达式/语句推导与检查）
//   P2  translator         run/emit_decl 读取 P0+P1 写入的字段生成 IR
//
// 多模块场景（compiler.cpp）：
//   各模块 P0 → 合并为全局 SymbolTable → 各模块回填 global → 各模块 P1a
//   → 各模块 P1b → P2
//   P1a 必须对所有模块先做完再进 P1b：类成员表在 P1a 写入，而成员查找
//   取的是定义模块的 Cont::scope，与模块在 modules 里的下标无关。
//
// 字段分工（下文的 P1 指 P1a+P1b 合起来的整个语义分析段）：
//
//   Expr::type         [写 P1 visit_expr_* / make_type] [读 P1 P2]
//                      值表达式的类型。有效表达式在 P1 结束时与 sym
//                      至少一个非空。
//
//   Expr::sym          [写 P1 visit_expr_identifier / visit_expr_member]
//                      [读 P1 P2]
//                      解析到的符号（变量/函数/类/枚举成员）。变量引用的 lvalue
//                      判定、函数引用的 mangled name、类构造都依赖它。
//                      与 type 至少有一个非空（见文件头"semantic基础规则"）。
//
//   Expr::is_lvalue    [写 P1] [读 P2]
//                      是否可取地址。函数返回值、字面量、cast 结果等为 false；
//                      变量引用、成员访问（变量）、数组下标为 true。
//
//   Type::resolved_decl / Type::resolved_scope
//                      [写 P1 visit_type_identifier / visit_type_member /
//                       visit_type_generic] [读 P1 P2]
//                      具名类型解析到的类/枚举/命名空间声明及其成员作用域，
//                      as_class / map_type / type_to_key 均依赖它。
//
//   Type::sym          [写 P1 visit_type_identifier / visit_type_member]
//                      [读 P1]
//                      类型名解析到的符号，仅用于报出模板本体的位置。
//
//   Expr::conv         [写 P1 record_conversion] [读 P2 needs_convert /
//                      eval_converted]
//                      非空表示该表达式需经 conversion 函数转换到目标类型。
//
//   Expr::implicit_to  [写 P1 record_conversion] [读 P2 needs_convert /
//                      eval_converted]
//                      非空表示该表达式需经隐式数值/包装转换，指向目标类型。
//
//   Expr::Call::resolved [写 P1 resolve_call] [读 P2 eval_call]
//                      resolved->kind 决定后续路径：
//                        Func / Operator          → 普通函数调用
//                        SpecialMethod(Init)      → 类构造
//                        nullptr                  → 零参默认构造、free_builtin、
//                                                   函数指针调用
//
//   Expr::Call::free_builtin / free_on_self
//                        [写 P1 resolve_call] [读 P2 eval_call]
//                      该调用是 malloc 类的 a.free()，P2 走释放路径。
//
//   Type::ArrayType [写 P1 parse(语法) / visit_type_array(常量长度、签名校验) /
//                    resolve_infer_array_len(T[] 推导)] [读 P1 P2]
//                      T[N] 的 length 为编译期整数字面量；T{} 的 length 为空且只允许
//                      出现在局部变量；T[] 推导成功后 length 变为字面量、kind 变为
//                      Fixed，推导不出来时 kind 保持 Infer、length 为空并报错。
//
//   TypeDef::target / ExprDef::target
//                      [写 语法 parse(typedef 名 = 类型 / exprdef 名 = 表达式)]
//                      [读 P1 使用点展开]
//                      声明处不遍历目标（Stmt::Def 同理）。每个使用点克隆一份目标，
//                      就地把节点内容换成克隆件并按新形态继续遍历，因此链式别名
//                      逐层展开成定义处的类型。展开时机与局部变量一致，故别名
//                      先声明后使用。
//
// 约定：
//   - P1 之后，所有语义字段冻结，P2 不再写 AST（除 translator 自己的局部缓存）。
//   - 有效表达式在 P1 结束时保证 type 与 sym 至少一个非空（见文件头规则）。
//   - 检出空指针不报错，产生空指针才报错。
//
namespace cx{

struct ASTNode;
struct CompUnit;
struct Import;
struct SpecList;
struct Expr;
struct Type;
struct Stmt;
struct Decl;
struct Cont;
struct VarItem;
struct VarList;
struct TypeParam;
struct Func;
struct Operator;
struct Conversion;
struct Enum;
struct EnumVariant;
struct Namespace;
struct Extern;
struct Class;
struct SpecialMethod;
struct Keep;
struct ASTVisitor;
struct Symbol;  // 前向声明，定义在 semantic.h
struct Scope;   // 前向声明，定义在 semantic.h

#define B_IS(a,b) inline bool is_##a() const { return std::holds_alternative<b>(data); }
#define B_B(p,k) p(const Location& l, k v) : ASTNode(l), data(std::move(v)) {}
#define B_GET(t,n) inline t* get_##n() { return std::get_if<t>(&data); } \
    inline const t* get_##n() const { return std::get_if<t>(&data); }
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
        ASTNode* raw = ptr.get();
        nodes.push_back(std::move(ptr));
        return raw;
    }
    template<typename T, typename... Args>
    T* add_node(Args&&... args) {
        auto node = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = node.get();
        nodes.push_back(std::move(node));
        return raw;
    }
    svmap<Decl*> type_registry;  // 类型名 -> 类/结构体声明
    void register_type(std::string_view name, Decl* decl) {
        type_registry[name] = decl;
    }
    bool is_type_name(std::string_view name) const {
        return type_registry.find(name) != type_registry.end();
    }
    // using namespace <path>：文件顶层 using 指令（把命名空间成员引入当前作用域，hoisted）
    // path 是类型位置的表达式（Identifier 或 Member 链），语义阶段按路径处理
    struct UsingNamespace {
        Type* path;
        Location loc;
    };
    std::vector<UsingNamespace> using_namespaces;
    void add_using_namespace(Type* path, const Location& loc) {
        using_namespaces.push_back({path, loc});
    }

    // 深拷贝 AST 节点并登记到本 CompUnit；用于类注入展开，避免共享节点
    std::vector<TypeParam*> clone_type_params(const std::vector<TypeParam*>& tps);
    Type* clone_type(Type* t);
    Expr* clone_expr(Expr* e);
    Stmt* clone_stmt(Stmt* s);
    VarList* clone_varlist(VarList* vl);
    Func* clone_func(Func* f);
    Decl* clone_decl(Decl* d);
};

enum class SpecKind {
    #define X(name,str) name,
    MODIFIER
    #undef X
};

inline SpecKind token_to_spec(TokenKind tk) {
    int offset = static_cast<int>(tk) - static_cast<int>(TokenKind::Const);
    return static_cast<SpecKind>(offset);
}

struct SpecList {
    uint32_t flags;
    SpecList(): flags(0) {}
    void set(SpecKind kind) {
        flags |= (1u << static_cast<uint8_t>(kind));
    }
    void del(SpecKind kind) {
        flags &= ~(1u << static_cast<uint8_t>(kind));
    }
    bool has(SpecKind kind) const {
        return flags &(1u << static_cast<uint8_t>(kind));
    }
    void merge(const SpecList& other) {
        flags |= other.flags;
    }
};

struct Import : ASTNode {
    bool no = false;
    bool is_string = false;  // 路径来自字符串字面量（相对/绝对），否则为模块名
    std::string_view path;
    Import(const Location &loc, std::string_view p, bool n, bool str = false)
        : ASTNode(loc), no(n), is_string(str), path(p)  {}
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

// 数组类型的长度种类：T[N] 定长、T[] 长度待推导、T{} 运行期长度的所有权切片。
// T{} 只出现在局部变量上：所有权不外泄，故不能作形参、返回类型或类字段
enum class ArrayKind : uint8_t {
    Fixed,
    Infer,
    Slice,
};

inline OpKind token_to_op(TokenKind kind) {
    switch (kind) {
        #define X(name, str) \
            case TokenKind::name: return OpKind::name;
        OP_KIND
        #undef X
        default: return OpKind::Unknown;
    }
}
inline bool is_binary(OpKind op) {
    return op >= OpKind::Plus && op <= OpKind::Shr;
}
inline bool is_unary(OpKind op) {
    // Mul / BitAnd 在单元位置分别代表 -* 解引用、-& 取地址
    return (op >= OpKind::Not && op <= OpKind::BitNot)
        || (op >= OpKind::Plus && op <= OpKind::Minus)
        || op == OpKind::Mul || op == OpKind::BitAnd;
}
inline bool is_assign(OpKind op) {
    return op >= OpKind::Assign && op <= OpKind::ShrAssign;
}

inline std::string_view opkind_to_sv(OpKind kind) {
    switch (kind) {
        #define X(name, str) \
            case OpKind::name: return str;
        OP_KIND
        #undef X
        case OpKind::Paren: return "()";
        case OpKind::Index: return "[]";
        default: return "";
    }
}

// 值表达式。类型即表达式的类型侧见 Type。
// 与 Type 共有的 variant（Identifier / Member / GenericType / Null / Inferred）
// 在两侧各存一份：同形语法在值位置与类型位置的语义与子节点类型都不同，
// 合成一个节点就得靠 is_type 标志再分一次派发。
struct Expr : ASTNode {
    struct Literal      { LiteralKind kind; std::string_view value; };
    struct Identifier   { std::string_view name; };
    struct Binary       { Expr* left; OpKind op; Expr* right; };
    struct Unary        { OpKind op; Expr* operand; bool is_postfix = false; };
    struct Assign       { Expr* target; OpKind op; Expr* value; };
    struct Call         { Expr* callee; std::vector<Expr*> args; Decl* resolved = nullptr; bool free_builtin = false; bool free_on_self = false; };
    struct Ternary      { Expr* cond; Expr* then_expr; Expr* else_expr; };
    struct Index        { Expr* target; Expr* index; };
    struct Member       { Expr* target; std::string_view member; };
    struct Cast         { Type* target_type; Expr* operand; };
    struct Array        { std::vector<Expr*> members; Expr* fill_value = nullptr; };
    struct Spawn        { Expr* call; };
    // 值位置的泛型应用：foo-<int>(1) 的被调用者。实参仍是类型
    struct GenericType      { Expr* base; std::vector<Type*> args; };
    struct Inferred         {};
    struct Null             {};

    using Variant = std::variant<
        Literal, Identifier, Binary, Unary, Assign,
        Call, Ternary, Index, Member,
        Cast, Array, Spawn,
        GenericType, Inferred, Null
    >;
    
    Variant data;
    Type* type = nullptr;
    Symbol* sym = nullptr;   // 解析后的符号指针
    // 在semantic解析中保证type和sym总有一个是有指向的
    bool is_lvalue = false; // 是否可取地址
    // 该表达式需经 conversion 转换到目标类型时，指向对应的 Conversion 声明
    Decl* conv = nullptr;
    // 隐式数值/包装转换的目标类型：translator 据此生成解包/打包与数值 cast
    Type* implicit_to = nullptr;
    
    Expr() = default;
    Expr(const Location& l, Variant v) : ASTNode(l), data(std::move(v))  {}
    BBB(literal, Literal, Expr)
    BBB(identifier, Identifier, Expr)
    BBB(binary, Binary, Expr)
    BBB(unary, Unary, Expr)
    BBB(assign, Assign, Expr)
    BBB(call, Call, Expr)
    BBB(ternary, Ternary, Expr)
    BBB(index, Index, Expr)
    BBB(member, Member, Expr)
    BBB(cast, Cast, Expr)
    BBB(array, Array, Expr)
    BBB(spawn, Spawn, Expr)
    BBB(generic_type, GenericType, Expr)
    BBB(inferred, Inferred, Expr)
    BBB(null, Null, Expr)
};

// 类型节点。类型即表达式：命名类型（Identifier / Member 链）本身也走 ASTVisitor，
// 泛型实参、形参类型、数组元素类型等子节点一律是 Type。
struct Type : ASTNode {
    struct Primitive        { PrimitiveTypeKind kind; };
    struct Pointer          { Type* pointee; };
    struct Reference        { Type* target; };
    struct ArrayType        { Type* element; Expr* length; ArrayKind kind = ArrayKind::Fixed; };
    // 具名类型与限定名：两类合用 Identifier/Member 两个 variant，
    // 靠 resolved_decl 区分指向的是类型还是命名空间
    struct Identifier       { std::string_view name; };
    struct Member           { Type* target; std::string_view member; };
    struct GenericType      { Type* base; std::vector<Type*> args; };
    struct StringLiteralType {};
    struct FuncPtr          { std::vector<Type*> params; Type* return_type; };
    struct FuncRef          { Func* func; };
    struct Inferred         {};
    struct Void             {};
    struct Null             {};

    using Variant = std::variant<
        Primitive, Pointer, Reference, ArrayType,
        Identifier, Member, GenericType, StringLiteralType,
        FuncPtr, FuncRef, Inferred, Void, Null
    >;

    Variant data;
    Symbol* sym = nullptr;          // 解析到的类/枚举/命名空间符号
    Decl* resolved_decl = nullptr;  // 解析到的类/枚举/命名空间声明
    Scope* resolved_scope = nullptr;// 成员作用域，供成员查找使用

    Type() = default;
    Type(const Location& l, Variant v) : ASTNode(l), data(std::move(v)) {}
    BBB(primitive, Primitive, Type)
    BBB(pointer, Pointer, Type)
    BBB(reference, Reference, Type)
    BBB(array_type, ArrayType, Type)
    BBB(identifier, Identifier, Type)
    BBB(member, Member, Type)
    BBB(generic_type, GenericType, Type)
    BBB(string_literal_type, StringLiteralType, Type)
    BBB(func_ptr, FuncPtr, Type)
    BBB(func_ref, FuncRef, Type)
    BBB(inferred, Inferred, Type)
    BBB(void, Void, Type)
    BBB(null, Null, Type)

    // 具名类型与限定名链
    bool is_named() const {
        return is_identifier() || is_member();
    }
};

struct Stmt : ASTNode {
    struct Block    { std::vector<Stmt*> stmts; };
    struct ExprStmt { Expr* expr; }; // 表达式能作为语句，语句不能作为表达式，除非是闭包
    struct If       { Expr* condition; Stmt* then_branch; Stmt* else_branch; };
    struct While    { Expr* condition; Stmt* body; };
    struct For      { Stmt* init; Expr* condition; Stmt* next; Stmt* body; };
    struct Return   { };
    struct Break    { };
    struct Continue { };
    struct Flag     { std::string_view label; };
    struct Goto     { std::string_view label; };
    struct Asm      { std::string_view code; };
    struct Keep     { std::string_view tokens; };
    struct VarDecl  { VarList* vars; };
    // 函数体内的 typedef / exprdef：和局部变量一样，登记进当前作用域后向下生效
    struct Def      { Decl* decl; };
    using Variant = std::variant<
        Block, ExprStmt, If, While, For,
        Return, Break, Continue, Flag, Goto,
        Asm, Keep, VarDecl, Def
    >;
    Variant data;
    Stmt() = default;
    Stmt(const Location& l, Variant v): ASTNode(l), data(std::move(v)) {}
    BBB(block, Block, Stmt)
    BBB(expr, ExprStmt, Stmt)
    BBB(if, If, Stmt)
    BBB(while, While, Stmt)
    BBB(for, For, Stmt)
    BBB(return, Return, Stmt)
    BBB(break, Break, Stmt)
    BBB(continue, Continue, Stmt)
    BBB(flag, Flag, Stmt)
    BBB(goto, Goto, Stmt)
    BBB(asm, Asm, Stmt)
    BBB(keep, Keep, Stmt)
    BBB(var_decl, VarDecl, Stmt)
    BBB(def, Def, Stmt)
};

enum class DeclKind {
    // 等级从低到高
    Keep, TypeDef, ExprDef, VarList, SpecialMethod, Func, Operator, Conversion, Enum, Class, Extern, Namespace,
};

struct Decl : ASTNode {
    std::string_view name;
    SpecList specs;
    DeclKind kind;
    explicit Decl( const Location &loc, std::string_view n,
        const DeclKind &k, SpecList ml=SpecList()) :
        ASTNode(loc), name(n), specs(std::move(ml)), kind(k) {}
    Decl() = default;virtual ~Decl() = default;
};

struct Cont : Decl { // container 容器，用于包含申明
    std::vector<Decl*> members;
    Scope* scope = nullptr;  // 符号表指针，在collect阶段写入
    explicit Cont(const Location &loc, std::string_view n, const DeclKind &k,
        SpecList ml=SpecList(), std::vector<Decl*> m = {}) :
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
};
// VarItem 是单个变量申明
struct VarItem : ASTNode {
    std::string_view name;
    Expr *init;
    Type* type = nullptr;
    VarItem() = default;
    VarItem(const Location& loc, std::string_view n, Expr* i):
        ASTNode(loc), name(n), init(i) {}
    VarItem(const Location& loc, std::string_view n, Expr* i, Type* t):
        ASTNode(loc), name(n), init(i), type(t) {}
};
// VarList 是同一种类型的多个变量申明
struct VarList : Decl {
    Type* type = nullptr;
    std::vector<VarItem*> vars;
    VarList(const Location& loc,SpecList ml,
            Type* t, std::vector<VarItem*> v
        ):
        Decl(loc, "", DeclKind::VarList, std::move(ml)),
        type(t), vars(std::move(v)) {}
};

// 泛型形参：`foo-<T, U>`、`Array-<T>` 中的 T/U，只是名字，语义阶段与实参绑定
struct TypeParam : ASTNode {
    std::string_view name;
    TypeParam() = default;
    TypeParam(const Location& loc, std::string_view n)
        : ASTNode(loc), name(n) {}
};

// extern 块绑定的外部 ABI。块名本身就是绑定目标：`"c"` / `"C"` 是 C 的语言
// 标识（C 运行时由链接器默认带上，不生成 -l），其余名字是待链接的库名。
// clux 目前只实现 C 的调用约定：库名块仍按 C 路径翻译，但不施加 C 的约束，
// 因为它们的调用约定尚未识别
enum class ExternLang : uint8_t { None, C, Unknown };

inline ExternLang extern_lang_of(std::string_view name) {
    return (name == "c" || name == "C") ? ExternLang::C : ExternLang::Unknown;
}

struct Func : Decl {
    std::vector<VarList*> params;   // 圆括号(int a, $bool cond)
    std::vector<VarList*> returns;  // 返回值声明
    std::vector<TypeParam*> type_params;  // 泛型形参 `foo-<T>`，非空即泛型声明
    Stmt* body = nullptr;           // 函数体
    ExternLang extern_lang = ExternLang::None;  // 非 None 即 extern 内的声明
    bool is_variadic = false;        // 形参表末尾有 '...'（仅 extern 支持）
    bool ret_via_reg = false;       // 标量返回且路径末尾恰好一次赋值：用寄存器 ret 而非 sret
    bool is_extern_decl() const { return extern_lang != ExternLang::None; }
    Func() = default;
    Func(const Location& loc,
        std::string_view name,
        SpecList ml,
        std::vector<VarList*> p,
        std::vector<VarList*> r,
        Stmt* b
        )
        : Decl(loc, name, DeclKind::Func, std::move(ml)),
        params(std::move(p)),
        returns(std::move(r)),
        body(b) {}
    virtual ~Func() = default;
};

struct Operator : Func {
    OpKind op;
    // 由基础类型来源自动派发：body 置空，translator 走解包/运算/打包路径
    bool synthesized = false;
    Operator() = default;
    Operator(
        const Location& loc, 
        OpKind a,
        std::string_view op_name,
        SpecList ml,
        std::vector<VarList*> p,
        std::vector<VarList*> r,
        Stmt* b
    ) : 
        Func(loc, op_name, std::move(ml),
        std::move(p), std::move(r), b), op(a) {
            kind = DeclKind::Operator;
        }
};

struct Conversion : Func {
    Conversion() = default;
    Conversion(const Location& loc, std::string_view name, SpecList ml,
        std::vector<VarList*> p, std::vector<VarList*> r, Stmt* b)
        : Func(loc, name, std::move(ml), std::move(p), std::move(r), b) {
        kind = DeclKind::Conversion;
    }
};

struct Namespace : Cont {
    Namespace() = default;
    Namespace(
        const Location &loc, std::string_view name,
        SpecList ml, std::vector<Decl*> members
    ):
        Cont(loc, name, DeclKind::Namespace, std::move(ml), std::move(members)) {}
};

struct Extern : Cont {
    Extern() = default;
    Extern(const Location& loc,
           std::string_view name,
           SpecList ml,
           std::vector<Decl*> m)
        : Cont(loc, name, DeclKind::Extern, std::move(ml), std::move(m)) {}
    // 块名即绑定目标，由名字识别出的外部 ABI
    ExternLang language() const { return extern_lang_of(name); }
};

struct Class : Cont {
    // 泛型形参 `Array-<T>`，非空即泛型类
    std::vector<TypeParam*> type_params;
    // 修饰注入：`#a B #c` 中类名之前的是前置注入，之后的是尾随注入。
    // 优先级从左到右递减：前置 > 基类 > 尾随。
    std::vector<Type*> injects;
    std::vector<Type*> trailing_injects;
    // 基础类型来源标记：#int 注入时记为 Int，非基础类型来源保持 Void
    PrimitiveTypeKind base_source = PrimitiveTypeKind::Void;
    Class(const Location &loc,
        std::string_view name,
        SpecList ml,
        std::vector<Decl*> m,
        std::vector<Type*> inj = {},
        std::vector<Type*> tinj = {}
    ):
        Cont(loc, name, DeclKind::Class, std::move(ml), std::move(m)),
        injects(std::move(inj)), trailing_injects(std::move(tinj)) {}
    virtual ~Class() = default;
};

struct SpecialMethod : Decl {
    enum class Kind {
        Init,       // 构造
        Drop,       // 折构
        Ref,        // 被引用
        Deref,      // 结束引用
    } kind;
    std::vector<VarList*> params;
    Stmt* body = nullptr;
    SpecialMethod() = default;
    SpecialMethod (
        const Location& loc,
        SpecialMethod::Kind k,
        SpecList ml,
        Stmt* b,
        std::vector<VarList*> p = {}
    ):
        Decl(loc, "", DeclKind::SpecialMethod, std::move(ml)),
        kind(k), params(std::move(p)), body(b) {}
};

struct Keep : Decl {
    std::string_view code;
    Keep(const Location &loc, std::string_view c, SpecList ml)
        : Decl(loc, "", DeclKind::Keep, std::move(ml)), code(c) {}
};

// typedef 名 = 类型：使用点按 target 展开，符号表合并使其可跨模块
struct TypeDef : Decl {
    Type* target = nullptr;
    TypeDef() = default;
    TypeDef(const Location& loc, std::string_view n, Type* t)
        : Decl(loc, n, DeclKind::TypeDef), target(t) {}
};

// exprdef 名 = 表达式：使用点按 target 展开，符号表合并使其可跨模块
struct ExprDef : Decl {
    Expr* target = nullptr;
    ExprDef() = default;
    ExprDef(const Location& loc, std::string_view n, Expr* e)
        : Decl(loc, n, DeclKind::ExprDef), target(e) {}
};

struct EnumVariant : ASTNode {
    std::string_view name;
    Expr* init = nullptr;   // 显式初值（= 后的表达式），缺省按声明顺序递增
    int64_t value = 0;      // 语义阶段写入：编译期整数值
    bool has_value = false; // 语义阶段写入：value 是否已定
    Type* type = nullptr;   // 语义阶段写入：所属枚举类型
    EnumVariant() = default;
    EnumVariant(const Location& loc, std::string_view n, Expr* i = nullptr)
        : ASTNode(loc), name(n), init(i) {}
};

struct Enum : Decl {
    Scope* scope = nullptr;   // 枚举自身的成员作用域
    std::vector<EnumVariant*> variants;
    Enum() = default;
    Enum(const Location& loc, std::string_view name, SpecList ml,
         std::vector<EnumVariant*> vs)
         : Decl(loc, name, DeclKind::Enum, std::move(ml)),
         variants(std::move(vs)) {}
};

// 类型节点的类声明解析；非类返回 nullptr
inline Class* as_class(Type* t) {
    if (!t) return nullptr;
    if (t->resolved_decl && t->resolved_decl->kind == DeclKind::Class)
        return static_cast<Class*>(t->resolved_decl);
    return nullptr;
}

struct ASTVisitor {
    virtual ~ASTVisitor() = default;
    virtual void scope_in(std::string){}
    virtual void scope_out(){}
    Expr* current_expr = nullptr;
    // 当前正在遍历的类型节点；解析类型只需它，无需 current_expr
    Type* current_type = nullptr;
    Func* current_func = nullptr;
    // 当前正在遍历的形参列表：形参允许省略初值，局部声明不允许
    const std::vector<VarList*>* current_params = nullptr;
    // 当前正在遍历的类成员：字段的绑定发生在 INIT，不在声明处
    const std::vector<Decl*>* current_members = nullptr;
    Class* current_class = nullptr;
    Stmt* current_stmt = nullptr;
    // 当前嵌套循环深度，供需要循环上下文的派生 pass 使用
    int loop_depth_ = 0;
    // 匿名作用域（block/for）计数器：避免同名子作用域被复用
    int anon_scope_ = 0;
    // 正在遍历调用表达式的被调用者，供标识符解析区分函数名与普通变量
    bool in_callee_ = false;
    // 正在遍历泛型应用的 base：该名字本就指向模板本体，不算裸用模板
    bool in_generic_base_ = false;
    void walk(CompUnit& cu) {
        for (auto* d : cu.top_decls) {
            if (d) walk(*d);
        }
        visit(cu);
    }

    void walk(Decl& decl) {
        walk_children(decl);
        visit(decl);
    }
    void walk(Expr& expr) {
        walk_children(expr);
        visit(expr);
    }
    void walk(Type& type) {
        walk_children(type);
        visit(type);
    }
    void walk(Stmt& stmt) {
        walk_children(stmt);
        visit(stmt);
    }
    void walk(VarItem& vi) {
        walk_children(vi);
        visit(vi);
    }
    void walk(TypeParam& tp) {
        visit_type_param(tp);
    }

    virtual void visit(CompUnit&) {}
    virtual void visit(Import&) {}


    virtual void visit(Decl&) {}
    virtual void visit_var_list(VarList&) {}
    virtual void visit_var_item(VarItem&) {}
    virtual void visit_type_param(TypeParam&) {}
    virtual void visit_func(Func&) {}
    virtual void visit_namespace(Namespace&) {}
    virtual void visit_extern(Extern&) {}
    virtual void visit_class(Class&) {}
    virtual void visit_special_method(SpecialMethod&) {}
    virtual void visit_enum(Enum&) {}
    virtual void visit_keep(Keep&) {}

    virtual void visit(Expr&) {}
    virtual void visit(VarItem&) {}
    virtual void visit(Type&) {}

    virtual void visit_expr_literal(Expr::Literal&) {}
    virtual void visit_expr_identifier(Expr::Identifier&) {}
    virtual void visit_expr_binary(Expr::Binary&) {}
    virtual void visit_expr_unary(Expr::Unary&) {}
    virtual void visit_expr_assign(Expr::Assign&) {}
    virtual void visit_expr_call(Expr::Call&) {}
    virtual void visit_expr_index(Expr::Index&) {}
    virtual void visit_expr_member(Expr::Member&) {}
    virtual void visit_expr_cast(Expr::Cast&) {}
    virtual void visit_expr_array(Expr::Array&) {}
    virtual void visit_expr_spawn(Expr::Spawn&) {}
    virtual void visit_expr_ternary(Expr::Ternary&) {}
    virtual void visit_expr_generic(Expr::GenericType&) {}
    virtual void visit_expr_null(Expr::Null&) {}

    virtual void visit(Stmt&) {}

    virtual void visit_stmt_block(Stmt::Block&) {}
    virtual void visit_stmt_expr(Stmt::ExprStmt&) {}
    virtual void visit_stmt_if(Stmt::If&) {}
    virtual void visit_stmt_while(Stmt::While&) {}
    virtual void visit_stmt_for(Stmt::For&) {}
    virtual void visit_stmt_return(Stmt::Return&) {}
    virtual void visit_stmt_break(Stmt::Break&) {}
    virtual void visit_stmt_continue(Stmt::Continue&) {}
    virtual void visit_stmt_flag(Stmt::Flag&) {}
    virtual void visit_stmt_goto(Stmt::Goto&) {}
    virtual void visit_stmt_asm(Stmt::Asm&) {}
    virtual void visit_stmt_keep(Stmt::Keep&) {}
    virtual void visit_stmt_var_decl(Stmt::VarDecl&) {}
    virtual void visit_stmt_def(Stmt::Def&) {}

    virtual void visit_type_primitive(Type::Primitive&) {}
    virtual void visit_type_pointer(Type::Pointer&) {}
    virtual void visit_type_reference(Type::Reference&) {}
    virtual void visit_type_array(Type::ArrayType&) {}
    virtual void visit_type_identifier(Type::Identifier&) {}
    virtual void visit_type_member(Type::Member&) {}
    virtual void visit_type_generic(Type::GenericType&) {}
    virtual void visit_type_string_literal(Type::StringLiteralType&) {}
    virtual void visit_type_func_ptr(Type::FuncPtr&) {}
    virtual void visit_type_func_ref(Type::FuncRef&) {}
    virtual void visit_type_inferred(Type::Inferred&) {}
    virtual void visit_type_void(Type::Void&) {}
    virtual void visit_type_null(Type::Null&) {}

    // 递归遍历子节点（基类驱动，结构变更只改这里）
    void walk_children(Decl& decl) {
        switch (decl.kind) {
            case DeclKind::VarList:       walk_children(static_cast<VarList&>(decl)); break;
            case DeclKind::Func:          walk_children(static_cast<Func&>(decl)); break;
            case DeclKind::Operator:      walk_children(static_cast<Func&>(decl)); break;
            case DeclKind::Conversion:    walk_children(static_cast<Func&>(decl)); break;
            case DeclKind::Class:         walk_children(static_cast<Class&>(decl)); break;
            case DeclKind::Namespace:     walk_children(static_cast<Namespace&>(decl)); break;
            case DeclKind::Extern:        walk_children(static_cast<Extern&>(decl)); break;
            case DeclKind::SpecialMethod: walk_children(static_cast<SpecialMethod&>(decl)); break;
            case DeclKind::Enum:          walk_children(static_cast<Enum&>(decl)); break;
            case DeclKind::Keep:          walk_children(static_cast<Keep&>(decl)); break;
            // typedef/exprdef 的目标不在声明处遍历：它只在每个使用点展开后才被分析
            case DeclKind::TypeDef:
            case DeclKind::ExprDef:        break;
            default: break;
        }
    }

    void walk_children(Func& f) {
        Func* pre = current_func;
        auto* pre_params = current_params;
        current_func = &f;
        current_params = &f.params;
        scope_in(std::string(f.name));
        for (auto* tp : f.type_params) {
            if (tp) walk(*tp);
        }
        for (auto* p : f.params) {
            if (p) walk(*p);
        }
        for (auto* r : f.returns) {
            if (r) walk(*r);
        }
        if (f.body) walk(*f.body);
        visit_func(f);
        scope_out();
        current_func = pre;
        current_params = pre_params;
    }

    void walk_children(VarList& vl) {
        if (vl.type) walk(*vl.type);
        for (auto* v : vl.vars) {
            if (!v) continue;
            // 后缀数组维度的类型只挂在 VarItem 上（int a[3]），前缀形式与
            // VarList 共用同一节点，不重复遍历
            if (v->type && v->type != vl.type) walk(*v->type);
            walk(*v);
        }
        visit_var_list(vl);
    }

    void walk_children(Class& c) {
        Class* pre = current_class;
        Func* pre_func = current_func;
        auto* pre_members = current_members;
        current_class = &c;
        // 类体不在任何函数内：返回变量之类的判定不应看到外层函数
        current_func = nullptr;
        current_members = &c.members;
        scope_in(std::string(c.name));
        for (auto* tp : c.type_params) {
            if (tp) walk(*tp);
        }
        for (auto* m : c.members) {
            if (m) walk(*m);
        }
        visit_class(c);
        scope_out();
        current_class = pre;
        current_func = pre_func;
        current_members = pre_members;
    }

    void walk_children(Namespace& ns) {
        scope_in(std::string(ns.name));
        for (auto* m : ns.members) {
            if (m) walk(*m);
        }
        visit_namespace(ns);
        scope_out();
    }

    void walk_children(Extern& e) {
        for (auto* m : e.members) {
            if (m) walk(*m);
        }
        visit_extern(e);
    }

    void walk_children(SpecialMethod& sm) {
        auto* pre_params = current_params;
        current_params = &sm.params;
        scope_in(std::string(sm.name));
        for (auto* p : sm.params) if (p) walk(*p);
        if (sm.body) walk(*sm.body);
        visit_special_method(sm);
        scope_out();
        current_params = pre_params;
    }
    void walk_children(Enum& e) { visit_enum(e); }
    void walk_children(Keep&) {}
    void walk_children(Expr& expr) {
        Expr* pre = current_expr;
        current_expr = &expr;
        std::visit([this](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Expr::Binary>) {
                if (v.left) walk(*v.left);
                if (v.right) walk(*v.right);
                visit_expr_binary(v);
            } else if constexpr (std::is_same_v<T, Expr::Unary>) {
                if (v.operand) walk(*v.operand);
                visit_expr_unary(v);
            } else if constexpr (std::is_same_v<T, Expr::Assign>) {
                if (v.target) walk(*v.target);
                if (v.value) walk(*v.value);
                visit_expr_assign(v);
            } else if constexpr (std::is_same_v<T, Expr::Call>) {
                bool pre_callee = in_callee_;
                in_callee_ = true;
                if (v.callee) walk(*v.callee);
                in_callee_ = pre_callee;
                for (auto* a : v.args) if (a) walk(*a);
                visit_expr_call(v);
            } else if constexpr (std::is_same_v<T, Expr::Ternary>) {
                if (v.cond) walk(*v.cond);
                if (v.then_expr) walk(*v.then_expr);
                if (v.else_expr) walk(*v.else_expr);
                visit_expr_ternary(v);
            } else if constexpr (std::is_same_v<T, Expr::Index>) {
                if (v.target) walk(*v.target);
                if (v.index) walk(*v.index);
                visit_expr_index(v);
            } else if constexpr (std::is_same_v<T, Expr::Member>) {
                // 限定名作为 callee 时，target 仍需正常解析（如 c.method / ns::fn）
                bool pre_callee = in_callee_;
                in_callee_ = false;
                if (v.target) walk(*v.target);
                in_callee_ = pre_callee;
                visit_expr_member(v);
            } else if constexpr (std::is_same_v<T, Expr::Cast>) {
                if (v.operand) walk(*v.operand);
                if (v.target_type) walk(*v.target_type);
                visit_expr_cast(v);
            } else if constexpr (std::is_same_v<T, Expr::Array>) {
                for (auto* m : v.members) if (m) walk(*m);
                if (v.fill_value) walk(*v.fill_value);
                visit_expr_array(v);
            } else if constexpr (std::is_same_v<T, Expr::Spawn>) {
                if (v.call) walk(*v.call);
                visit_expr_spawn(v);
            } else if constexpr (std::is_same_v<T, Expr::GenericType>) {
                bool pre_base = in_generic_base_;
                in_generic_base_ = true;
                if (v.base) walk(*v.base);
                in_generic_base_ = pre_base;
                for (auto* a : v.args) if (a) walk(*a);
                visit_expr_generic(v);
            } else if constexpr (std::is_same_v<T, Expr::Null>) visit_expr_null(v);
            else if constexpr (std::is_same_v<T, Expr::Literal>) visit_expr_literal(v);
            else if constexpr (std::is_same_v<T, Expr::Identifier>) visit_expr_identifier(v);
        }, expr.data);
        current_expr = pre;
    }

    void walk_children(Type& type) {
        Type* pre = current_type;
        current_type = &type;
        std::visit([this](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Type::Pointer>) {
                if (v.pointee) walk(*v.pointee);
                visit_type_pointer(v);
            } else if constexpr (std::is_same_v<T, Type::Reference>) {
                if (v.target) walk(*v.target);
                visit_type_reference(v);
            } else if constexpr (std::is_same_v<T, Type::ArrayType>) {
                if (v.element) walk(*v.element);
                if (v.length) walk(*v.length);
                visit_type_array(v);
            } else if constexpr (std::is_same_v<T, Type::GenericType>) {
                bool pre_base = in_generic_base_;
                in_generic_base_ = true;
                if (v.base) walk(*v.base);
                in_generic_base_ = pre_base;
                for (auto* a : v.args) if (a) walk(*a);
                visit_type_generic(v);
            } else if constexpr (std::is_same_v<T, Type::FuncPtr>) {
                for (auto* p : v.params) if (p) walk(*p);
                if (v.return_type) walk(*v.return_type);
                visit_type_func_ptr(v);
            } else if constexpr (std::is_same_v<T, Type::Primitive>) visit_type_primitive(v);
            else if constexpr (std::is_same_v<T, Type::Inferred>) visit_type_inferred(v);
            else if constexpr (std::is_same_v<T, Type::Void>) visit_type_void(v);
            else if constexpr (std::is_same_v<T, Type::Null>) visit_type_null(v);
            else if constexpr (std::is_same_v<T, Type::StringLiteralType>) visit_type_string_literal(v);
            else if constexpr (std::is_same_v<T, Type::FuncRef>) visit_type_func_ref(v);
            else if constexpr (std::is_same_v<T, Type::Identifier>) visit_type_identifier(v);
            else if constexpr (std::is_same_v<T, Type::Member>) {
                if (v.target) walk(*v.target);
                visit_type_member(v);
            }
        }, type.data);
        current_type = pre;
    }

    void walk_children(Stmt& stmt) {
        Stmt* pre = current_stmt;
        current_stmt = &stmt;
        std::visit([this](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Stmt::Block>) {
                scope_in("block#" + std::to_string(anon_scope_++));
                for (auto* s : v.stmts) if (s) walk(*s);
                visit_stmt_block(v);
                scope_out();
            } else if constexpr (std::is_same_v<T, Stmt::ExprStmt>) {
                if (v.expr) walk(*v.expr);
                visit_stmt_expr(v);
            } else if constexpr (std::is_same_v<T, Stmt::If>) {
                if (v.condition) walk(*v.condition);
                if (v.then_branch) walk(*v.then_branch);
                if (v.else_branch) walk(*v.else_branch);
                visit_stmt_if(v);
            } else if constexpr (std::is_same_v<T, Stmt::While>) {
                ++loop_depth_;
                if (v.condition) walk(*v.condition);
                if (v.body) walk(*v.body);
                visit_stmt_while(v);
                --loop_depth_;
            } else if constexpr (std::is_same_v<T, Stmt::For>) {
                scope_in("for#" + std::to_string(anon_scope_++));
                ++loop_depth_;
                if (v.init) walk(*v.init);
                if (v.condition) walk(*v.condition);
                if (v.next) walk(*v.next);
                if (v.body) walk(*v.body);
                visit_stmt_for(v);
                --loop_depth_;
                scope_out();
            } else if constexpr (std::is_same_v<T, Stmt::VarDecl>) {
                if (v.vars) walk(*v.vars);
                visit_stmt_var_decl(v);
            } else if constexpr (std::is_same_v<T, Stmt::Def>) {
                // 只登记符号，别名目标仍不在声明处遍历：它在每个使用点展开后才被分析
                visit_stmt_def(v);
            }
            else if constexpr (std::is_same_v<T, Stmt::Return>) visit_stmt_return(v);
            else if constexpr (std::is_same_v<T, Stmt::Break>) visit_stmt_break(v);
            else if constexpr (std::is_same_v<T, Stmt::Continue>) visit_stmt_continue(v);
            else if constexpr (std::is_same_v<T, Stmt::Flag>) visit_stmt_flag(v);
            else if constexpr (std::is_same_v<T, Stmt::Goto>) visit_stmt_goto(v);
            else if constexpr (std::is_same_v<T, Stmt::Asm>) visit_stmt_asm(v);
            else if constexpr (std::is_same_v<T, Stmt::Keep>) visit_stmt_keep(v);
        }, stmt.data);
        current_stmt = pre;
    }

    void walk_children(VarItem& vi) {
        visit_var_item(vi);
        if (vi.init) walk(*vi.init);
    }
};

}// namespace cx