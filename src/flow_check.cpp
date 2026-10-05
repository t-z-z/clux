#include "../include/flow_check.h"

#include "../include/semantic.h"

#include "../include/sem_types.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace cx {

namespace {

struct JumpPoint {
    std::string label;
    std::vector<int> scope_path;
    int loop_depth = 0;
    bool is_flag = false;
    Location loc;
};

}


void FlowChecker::check_jumps(Stmt* body) {
    if (!body) return;
    std::vector<JumpPoint> flags;
    std::vector<JumpPoint> gotos;
    int scope_counter = 0;

    // 收集本体内所有 flag / toflag 及其作用域路径
    std::function<void(Stmt*, std::vector<int>, int)> scan =
        [&](Stmt* st, std::vector<int> path, int loops) {
        if (!st) return;
        if (auto* f = st->get_flag()) {
            flags.push_back({std::string(f->label), path, loops, true, st->loc});
            return;
        }
        if (auto* g = st->get_goto()) {
            gotos.push_back({std::string(g->label), path, loops, false, st->loc});
            return;
        }
        if (auto* b = st->get_block()) {
            std::vector<int> p = path;
            p.push_back(++scope_counter);
            for (Stmt* x : b->stmts) scan(x, p, loops);
            return;
        }
        if (auto* i = st->get_if()) {
            scan(i->then_branch, path, loops);
            scan(i->else_branch, path, loops);
            return;
        }
        if (auto* w = st->get_while()) {
            scan(w->body, path, loops + 1);
            return;
        }
        if (auto* fo = st->get_for()) {
            std::vector<int> p = path;
            p.push_back(++scope_counter);
            scan(fo->init, p, loops);
            scan(fo->next, p, loops);
            scan(fo->body, p, loops + 1);
            return;
        }
    };
    scan(body, {}, 0);

    // 逐个检查 toflag
    for (const JumpPoint& g : gotos) {
        bool found = false;
        for (const JumpPoint& f : flags) {
            if (f.label != g.label) continue;
            found = true;
            // 目标作用域必须是跳转点作用域的祖先或自身
            bool into_scope = f.scope_path.size() > g.scope_path.size();
            if (!into_scope) {
                for (size_t i = 0; i < f.scope_path.size(); ++i) {
                    if (f.scope_path[i] != g.scope_path[i]) { into_scope = true; break; }
                }
            }
            if (into_scope) {
                error_(g.loc, "jump into the scope of label '" + g.label + "'");
            }
            if (f.loop_depth > g.loop_depth) {
                error_(g.loc, "jump into a loop at label '" + g.label + "'");
            }
        }
        if (!found) {
            error_(g.loc, "jump to undefined label '" + g.label + "'");
        }
    }
}

// 变量初始化检查：不得读取未赋值过的变量
void FlowChecker::check_initialization(
        Stmt* body, const std::vector<VarList*>& params,
        const std::vector<VarList*>& returns, bool in_init) {
    if (!body) return;
    InitCtx ctx;
    ctx.in_init = in_init;
    // 参数视为已赋值
    for (VarList* vl : params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            Symbol* sym = sym_of(vi);
            if (sym) { ctx.declared.insert(sym); ctx.assigned.insert(sym); }
        }
    }
    // 返回变量：视作普通变量，在 return 或函数末尾读取时检查是否已赋值
    for (VarList* vl : returns) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            Symbol* sym = sym_of(vi);
            if (!sym) continue;
            ctx.declared.insert(sym);
            if (vi->init) ctx.assigned.insert(sym);
            ctx.ret_sym = sym;
        }
    }
    init_stmt(ctx, body);
    if (ctx.ret_sym && !ctx.terminated && !ctx.assigned.count(ctx.ret_sym)) {
        error_(body->loc, "return variable '" +
              sym_display_name(ctx.ret_sym) + "' is never assigned");
    }
}

void FlowChecker::init_stmt(InitCtx& ctx, Stmt* s) {
    if (!s) return;
    if (auto* b = s->get_block()) {
        for (Stmt* x : b->stmts) {
            if (ctx.terminated) break;
            init_stmt(ctx, x);
        }
        return;
    }
    if (auto* e = s->get_expr()) { init_expr(ctx, e->expr); return; }
    if (auto* i = s->get_if()) {
        init_expr(ctx, i->condition);
        InitCtx then_ctx = ctx, else_ctx = ctx;
        then_ctx.terminated = false;
        else_ctx.terminated = false;
        init_stmt(then_ctx, i->then_branch);
        init_stmt(else_ctx, i->else_branch);
        if (then_ctx.terminated && else_ctx.terminated) {
            ctx.terminated = true;
            return;
        }
        if (then_ctx.terminated) {
            ctx.assigned = std::move(else_ctx.assigned);
        } else if (else_ctx.terminated) {
            ctx.assigned = std::move(then_ctx.assigned);
        } else {
            // 分支合并：仅在两个分支都已赋值的变量才算已赋值
            std::unordered_set<Symbol*> merged;
            for (Symbol* sym : then_ctx.assigned) {
                if (else_ctx.assigned.count(sym)) merged.insert(sym);
            }
            for (Symbol* sym : ctx.assigned) merged.insert(sym);
            ctx.assigned = std::move(merged);
        }
        return;
    }
    if (auto* w = s->get_while()) {
        init_expr(ctx, w->condition);
        InitCtx body_ctx = ctx;
        init_stmt(body_ctx, w->body);
        return;
    }
    if (auto* f = s->get_for()) {
        init_stmt(ctx, f->init);
        init_expr(ctx, f->condition);
        InitCtx body_ctx = ctx;
        init_stmt(body_ctx, f->body);
        init_stmt(body_ctx, f->next);
        return;
    }
    if (s->is_return()) {
        if (ctx.ret_sym && !ctx.assigned.count(ctx.ret_sym)) {
            error_(s->loc, "return variable '" +
                  sym_display_name(ctx.ret_sym) + "' is not assigned on this path");
        }
        ctx.terminated = true;
        return;
    }
    if (auto* v = s->get_var_decl()) {
        VarList* vl = v->vars;
        if (!vl) return;
        for (VarItem* vi : vl->vars) {
            Symbol* sym = sym_of(vi);
            if (vi->init) {
                init_expr(ctx, vi->init);
                if (sym) ctx.assigned.insert(sym);
            } else {
                // 定长数组声明即分配 backing，视为已初始化；
                // 所有权切片 T{} 无 backing，保持未初始化
                Expr* vt = vi->type ? vi->type : vl->type;
                if (sym && vt && vt->is_array_type()) {
                    Expr* len = vt->get_array_type()->length;
                    if (len) ctx.assigned.insert(sym);
                }
            }
            if (sym) ctx.declared.insert(sym);
        }
        return;
    }
}

// 返回变量的"至少赋值一次"扫描：不走流敏感的 assigned 合并
bool FlowChecker::body_assigns_return(Stmt* s, Symbol* target) {
    if (!s) return false;
    if (auto* b = s->get_block()) {
        for (Stmt* x : b->stmts) if (body_assigns_return(x, target)) return true;
        return false;
    }
    if (auto* e = s->get_expr()) return expr_assigns_return(e->expr, target);
    if (auto* i = s->get_if()) {
        return expr_assigns_return(i->condition, target)
            || body_assigns_return(i->then_branch, target)
            || body_assigns_return(i->else_branch, target);
    }
    if (auto* w = s->get_while()) {
        return expr_assigns_return(w->condition, target)
            || body_assigns_return(w->body, target);
    }
    if (auto* f = s->get_for()) {
        return body_assigns_return(f->init, target)
            || expr_assigns_return(f->condition, target)
            || body_assigns_return(f->next, target)
            || body_assigns_return(f->body, target);
    }
    if (s->is_return()) return false;
    if (auto* v = s->get_var_decl()) {
        VarList* vl = v->vars;
        if (!vl) return false;
        for (VarItem* vi : vl->vars) {
            Symbol* sym = sym_of(vi);
            if (sym == target && vi->init) return true;
            if (vi->init && expr_assigns_return(vi->init, target)) return true;
        }
        return false;
    }
    return false;
}

bool FlowChecker::expr_assigns_return(Expr* e, Symbol* target) {
    if (!e) return false;
    if (auto* a = e->get_assign()) {
        if (a->target && a->target->sym == target) return true;
        return expr_assigns_return(a->value, target);
    }
    if (auto* b = e->get_binary()) {
        return expr_assigns_return(b->left, target)
            || expr_assigns_return(b->right, target);
    }
    if (auto* u = e->get_unary()) return expr_assigns_return(u->operand, target);
    if (auto* c = e->get_call()) {
        if (expr_assigns_return(c->callee, target)) return true;
        for (Expr* arg : c->args) if (expr_assigns_return(arg, target)) return true;
        return false;
    }
    if (auto* t = e->get_ternary()) {
        return expr_assigns_return(t->cond, target)
            || expr_assigns_return(t->then_expr, target)
            || expr_assigns_return(t->else_expr, target);
    }
    if (auto* ix = e->get_index()) {
        return expr_assigns_return(ix->target, target)
            || expr_assigns_return(ix->index, target);
    }
    if (auto* m = e->get_member()) return expr_assigns_return(m->target, target);
    if (auto* ca = e->get_cast()) return expr_assigns_return(ca->operand, target);
    if (auto* ar = e->get_array()) {
        for (Expr* m : ar->members) if (expr_assigns_return(m, target)) return true;
        return expr_assigns_return(ar->fill_value, target);
    }
    if (auto* sp = e->get_spawn()) return expr_assigns_return(sp->call, target);
    return false;
}

void FlowChecker::init_assign_target(InitCtx& ctx, Expr* e) {
    if (!e) return;
    if (e->sym && ctx.declared.count(e->sym)) {
        ctx.assigned.insert(e->sym);
    }
}

void FlowChecker::init_expr(InitCtx& ctx, Expr* e) {
    if (!e) return;
    if (auto* id = e->get_identifier()) {
        (void)id;
        if (e->sym && ctx.declared.count(e->sym) && !ctx.assigned.count(e->sym)) {
            error_(e->loc, "use of uninitialized variable '" +
                  sym_display_name(e->sym) + "'");
        }
        return;
    }
    if (auto* a = e->get_assign()) {
        init_expr(ctx, a->value);
        // INIT 内对引用字段的赋值是绑定：形参与局部变量在构造返回后即失效
        if (a->target && a->target->sym) {
            Expr* tt = a->target->sym->get_type();
            if (tt && tt->is_reference() && ctx.in_init &&
                !ctx.declared.count(a->target->sym) && a->value &&
                a->value->sym && ctx.declared.count(a->value->sym))
                error_(a->value->loc,
                       "cannot bind a reference field to a local or parameter "
                       "of INIT: it is dead once INIT returns");
        }
        init_assign_target(ctx, a->target);
        return;
    }
    if (auto* b = e->get_binary()) { init_expr(ctx, b->left); init_expr(ctx, b->right); return; }
    if (auto* u = e->get_unary()) { init_expr(ctx, u->operand); return; }
    if (auto* c = e->get_call()) {
        init_expr(ctx, c->callee);
        for (Expr* arg : c->args) init_expr(ctx, arg);
        return;
    }
    if (auto* t = e->get_ternary()) {
        init_expr(ctx, t->cond); init_expr(ctx, t->then_expr); init_expr(ctx, t->else_expr);
        return;
    }
    if (auto* ix = e->get_index()) { init_expr(ctx, ix->target); init_expr(ctx, ix->index); return; }
    if (auto* m = e->get_member()) { init_expr(ctx, m->target); return; }
    if (auto* ca = e->get_cast()) { init_expr(ctx, ca->operand); return; }
    if (auto* ar = e->get_array()) {
        for (Expr* m : ar->members) init_expr(ctx, m);
        init_expr(ctx, ar->fill_value);
        return;
    }
    if (auto* sp = e->get_spawn()) { init_expr(ctx, sp->call); return; }
}

// 标量返回且每条路径末尾恰好一次赋值 → 可用寄存器 ret
bool FlowChecker::can_use_register_ret(Func& f) {
    if (!f.body) return false;
    if (f.returns.empty() || !f.returns.back()) return false;
    VarList* rl = f.returns.back();
    if (rl->vars.empty()) return false;
    VarItem* ret_var = rl->vars.back();
    Expr* ty = ret_var->type ? ret_var->type : rl->type;
    if (!ty || !ty->get_primitive()) return false;
    Symbol* sym = sym_of(ret_var);
    if (!sym) return false;
    // 返回值声明处带初始化表达式：初始化即唯一一次写入，函数体不再改写
    if (ret_var->init) {
        return !body_assigns_return(f.body, sym);
    }
    std::vector<Stmt*> one = { f.body };
    return seq_tail_assign(one, sym);
}

// 语句本身是否是"末尾恰好一次 r= X"（允许 if/else 两分支各自末尾一次）
bool FlowChecker::stmt_tail_assign(Stmt* s, Symbol* ret) {
    if (!s) return false;
    if (auto* b = s->get_block()) return seq_tail_assign(b->stmts, ret);
    if (auto* es = s->get_expr()) {
        Expr* ex = es->expr;
        if (!ex) return false;
        auto* a = ex->get_assign();
        if (!a) return false;
        if (a->op != OpKind::Assign) return false;
        return a->target && a->target->sym == ret;
    }
    if (auto* ifs = s->get_if()) {
        if (!stmt_tail_assign(ifs->then_branch, ret)) return false;
        if (!ifs->else_branch) return false;
        return stmt_tail_assign(ifs->else_branch, ret);
    }
    return false;
}

// 语句序列：末尾必须 tail 赋值（或末尾 return 前恰好一次赋值），
// 且更早位置不得出现对 ret 的赋值
bool FlowChecker::seq_tail_assign(const std::vector<Stmt*>& stmts, Symbol* ret) {
    if (stmts.empty()) return false;
    size_t tail = stmts.size() - 1;
    // 末尾 return 本身无副作用：允许它之前恰好一次赋值
    if (stmts[tail]->is_return()) {
        if (tail == 0) return false;
        --tail;
    }
    if (!stmt_tail_assign(stmts[tail], ret)) return false;
    for (size_t i = 0; i < tail; ++i) {
        if (body_assigns_return(stmts[i], ret)) return false;
    }
    return true;
}

} // namespace cx
