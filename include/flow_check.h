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

// 函数体流检查：label 跳转合法性、变量初始化、返回变量是否赋值
class FlowChecker {
public:
    using ErrorFn = std::function<void(const Location&, const std::string&)>;

    FlowChecker(const std::unordered_map<VarItem*, Symbol*>& var_symbols,
                ErrorFn error)
        : var_symbols_(var_symbols), error_(std::move(error)) {}

    // label / goto 的 label 检查：遵循 C++ 的跳转规则
    void check_jumps(Stmt* body);
    // 变量初始化检查：不得读取未赋值过的变量
    void check_initialization(Stmt* body, const std::vector<VarList*>& params,
                              const std::vector<VarList*>& returns,
                              bool in_init = false);
    // 标量返回且每条路径末尾恰好一次赋值 → 可用寄存器 ret
    bool can_use_register_ret(Func& f);

private:
    struct InitCtx {
        std::unordered_set<Symbol*> declared;
        std::unordered_set<Symbol*> assigned;
        Symbol* ret_sym = nullptr;   // 当前函数的返回变量（若有）
        bool terminated = false;     // 当前路径是否已终止（return）
        bool in_init = false;        // 当前检查的是 INIT 方法体
    };

    void init_stmt(InitCtx& ctx, Stmt* s);
    void init_expr(InitCtx& ctx, Expr* e);
    void init_assign_target(InitCtx& ctx, Expr* e);
    // 返回变量只在函数体内任意位置被赋值一次即可，不做流敏感检查
    bool body_assigns_return(Stmt* s, Symbol* target);
    bool expr_assigns_return(Expr* e, Symbol* target);
    bool stmt_tail_assign(Stmt* s, Symbol* ret);
    bool seq_tail_assign(const std::vector<Stmt*>& stmts, Symbol* ret);

    Symbol* sym_of(VarItem* vi) const {
        auto it = var_symbols_.find(vi);
        return it == var_symbols_.end() ? nullptr : it->second;
    }

    const std::unordered_map<VarItem*, Symbol*>& var_symbols_;
    ErrorFn error_;
};

} // namespace cx
