#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"

namespace cx {

struct Symbol;
struct VarItem;
struct VarList;

// 函数体流检查：label 跳转合法性、变量初始化、引用的一次性绑定、
// 返回变量是否赋值与引用返回的绑定源
class FlowChecker {
public:
    using ErrorFn = std::function<void(const Location&, const std::string&)>;

    FlowChecker(const std::unordered_map<VarItem*, Symbol*>& var_symbols,
                ErrorFn error)
        : var_symbols_(var_symbols), error_(std::move(error)) {}

    // label / goto 的 label 检查：遵循 C++ 的跳转规则
    void check_jumps(Stmt* body);
    // 变量初始化检查：不得读取未赋值过的变量，引用不得在绑定前读取。
    // cls 非空时，类的 const 字段与引用字段也纳入检查：INIT 内赋值/绑定一次，
    // 其余方法视为已构造
    void check_initialization(Stmt* body, const std::vector<VarList*>& params,
                              const std::vector<VarList*>& returns,
                              bool in_init = false, Class* cls = nullptr);
    // 标量返回且每条路径末尾恰好一次赋值 → 可用寄存器 ret
    bool can_use_register_ret(Func& f);

private:
    struct InitCtx {
        std::unordered_set<Symbol*> declared;
        std::unordered_set<Symbol*> assigned;
        // 每条路径都必须已赋值/绑定的类字段（const 字段与引用字段）
        std::vector<Symbol*> must_init;
        // 入口即已绑定或带出的量（形参、返回变量）：对它们的赋值不是绑定点
        std::unordered_set<Symbol*> bound_before;
        // 本类的引用字段：绑定只发生在 INIT 里，且可以多次绑定换观察对象
        std::unordered_set<Symbol*> ref_fields;
        // 本方法体里声明过的局部量：存储在栈帧上，返回即失效
        std::unordered_set<Symbol*> locals;
        // 当前路径上已被消耗的一次性机会：const 已初始化、延迟绑定的引用已绑定。
        // 进分支时随 InitCtx 一起拷贝，if/else 合并时取两支的并集——
        // 任一支用过，分支之后就不许再用，而各支各用一次不冲突
        std::unordered_set<Symbol*> bound;
        Symbol* ret_sym = nullptr;   // 当前函数的返回变量（若有）
        bool terminated = false;     // 当前路径是否已终止（return）
        bool in_init = false;        // 当前检查的是 INIT 方法体
    };

    void init_stmt(InitCtx& ctx, Stmt* s);
    void init_expr(InitCtx& ctx, Expr* e);
    // 赋值：目标符号的 const / 绑定规则，以及绑定点的来源检查
    void check_assign(InitCtx& ctx, Expr::Assign* a);
    // 剥到绑定源最终指着的变量；落在本帧存储上时返回它，否则 nullptr
    VarItem* ref_bind_root(Expr* e, const InitCtx& ctx) const;
    // 绑定点的来源检查：源得指着一个对象；引用返回的源还不能落在本帧局部量上
    void check_bind_source(InitCtx& ctx, Symbol* target, Expr* src);
    // 语句的实际遍历，供 init_stmt 记返回变量改写
    void init_stmt_body(InitCtx& ctx, Stmt* s);
    bool stmt_tail_assign(Stmt* s, Symbol* ret);
    bool seq_tail_assign(const std::vector<Stmt*>& stmts, Symbol* ret);

    Symbol* sym_of(VarItem* vi) const {
        auto it = var_symbols_.find(vi);
        return it == var_symbols_.end() ? nullptr : it->second;
    }

    const std::unordered_map<VarItem*, Symbol*>& var_symbols_;
    ErrorFn error_;
    // 延迟绑定的引用各自绑在了哪个表达式上：沿这条链才能判出绑定源
    // 最终指着谁，才能知道返回出去的地址会不会悬垂
    std::unordered_map<Symbol*, Expr*> ref_src_;
    // 改写过返回变量的语句：函数体遍历时顺带记下，
    // 供 can_use_register_ret 判断"末尾恰好一次赋值"
    std::unordered_set<const Stmt*> ret_written_;
    // ret_written_ 的记录游标：语句前后比对即知其子树内是否改写过返回变量
    size_t ret_assigns_ = 0;
};

} // namespace cx
