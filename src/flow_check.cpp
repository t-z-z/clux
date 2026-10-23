#include "../include/flow_check.h"
#include "../include/semantic.h"
#include "../include/sem_types.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
    bool is_label = false;
    Location loc;
    size_t order = 0;
};

struct DeclPoint {
    std::string name;
    std::vector<int> scope_path;
    size_t order = 0;
    Location loc;
};

// path 是否为 target 的祖先作用域路径（含相同）
static bool scope_prefix(const std::vector<int>& path,
                         const std::vector<int>& target) {
    if (path.size() > target.size()) return false;
    for (size_t i = 0; i < path.size(); ++i)
        if (path[i] != target[i]) return false;
    return true;
}

}


void FlowChecker::check_jumps(Stmt* body) {
    if (!body) return;
    std::vector<JumpPoint> labels;
    std::vector<JumpPoint> gotos;
    std::vector<DeclPoint> decls;
    int scope_counter = 0;
    size_t order = 0;

    // 收集本体内所有 label / goto / 局部声明及其作用域路径与出现顺序
    std::function<void(Stmt*, std::vector<int>, int)> scan =
        [&](Stmt* st, std::vector<int> path, int loops) {
        if (!st) return;
        if (auto* f = st->get_label()) {
            labels.push_back({std::string(f->label), path, loops, true, st->loc,
                              order++});
            return;
        }
        if (auto* g = st->get_goto()) {
            gotos.push_back({std::string(g->label), path, loops, false, st->loc,
                             order++});
            return;
        }
        if (auto* v = st->get_var_decl()) {
            if (v->vars) {
                for (VarItem* vi : v->vars->vars) {
                    if (!vi) continue;
                    decls.push_back({std::string(vi->name), path, order, vi->loc});
                }
            }
            ++order;
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

    // 逐个检查 goto
    for (const JumpPoint& g : gotos) {
        bool found = false;
        for (const JumpPoint& f : labels) {
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
            // 前向跳转越过同一作用域里的局部声明：声明处的存储/初值没有执行，
            // 标签之后不能再把这个名字当作已声明量使用
            if (f.order > g.order) {
                for (const DeclPoint& d : decls) {
                    if (d.order <= g.order || d.order >= f.order) continue;
                    if (!scope_prefix(d.scope_path, f.scope_path)) continue;
                    error_(g.loc, "jump over the declaration of '" + d.name + "'");
                }
            }
        }
        if (!found) {
            error_(g.loc, "jump to undefined label '" + g.label + "'");
        }
    }
}

// 量初始化检查：不得读取未赋值过的量
void FlowChecker::check_initialization(
        Stmt* body, const std::vector<VarList*>& params,
        const std::vector<VarList*>& returns, bool in_init, Class* cls) {
    // 一次性的绑定链与返回量改写记录都只在本方法体内有意义，
    // 放在函数体判空之前，免得空函数体把上一个的记录带过来
    ref_src_.clear();
    ret_written_.clear();
    ret_assigns_ = 0;
    goto_states_.clear();
    if (!body) return;
    InitCtx ctx;
    ctx.in_init = in_init;
    // 参数视为已赋值
    for (VarList* vl : params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            Symbol* sym = sym_of(vi);
            if (sym) {
                ctx.declared.insert(sym);
                ctx.assigned.insert(sym);
                ctx.bound_before.insert(sym);
            }
        }
    }
    // 返回量：视作普通量，在 return 或函数末尾读取时检查是否已赋值。
    // 声明处的初值同样是把地址带出去的绑定点，与函数体里的赋值一个规矩
    for (VarList* vl : returns) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            Symbol* sym = sym_of(vi);
            if (!sym) continue;
            ctx.declared.insert(sym);
            ctx.bound_before.insert(sym);
            ctx.ret_sym = sym;
            if (vi->init) ctx.assigned.insert(sym);
            // 引用返回量声明处的初值就是绑定点，与函数体里的赋值一个规矩
            if (vi->init && vi->type && vi->type->is_reference())
                check_bind_source(ctx, sym, vi->init);
        }
    }
    // 字段：每个非 uninit 字段都要有声明处初值，或在 INIT 内每条路径上赋值。
    // static 字段的存储是模块级的（零初始化），其余字段在 INIT 内必须初始化，
    // 非 INIT 的方法则由构造期保证已初始化
    if (cls) {
        for (Decl* m : cls->members) {
            if (!m || m->kind != DeclKind::VarList) continue;
            VarList* vl = static_cast<VarList*>(m);
            bool is_static = vl->specs.has(SpecKind::Static);
            bool is_uninit = vl->specs.has(SpecKind::Uninit);
            for (VarItem* vi : vl->vars) {
                if (is_uninit) continue;
                Type* ft = vi->type ? vi->type : vl->type;
                if (!ft) continue;
                Symbol* sym = sym_of(vi);
                if (!sym) continue;
                ctx.declared.insert(sym);
                if (ft->is_reference()) ctx.ref_fields.insert(sym);
                if (vi->init || is_static || !in_init) {
                    ctx.assigned.insert(sym);
                } else {
                    ctx.must_init.push_back(sym);
                }
            }
        }
    }
    init_stmt(ctx, body);
    if (ctx.ret_sym && !ctx.terminated && !ctx.assigned.count(ctx.ret_sym)) {
        error_(body->loc, "return value '" +
              sym_display_name(ctx.ret_sym) + "' is never assigned");
    }
    // 构造完成时每个受管字段都必须已被赋值/绑定：不赋值就用不到它，
    // 而类一旦建成就再也不有赋值点
    if (!ctx.terminated) {
        for (Symbol* sym : ctx.must_init) {
            if (ctx.assigned.count(sym)) continue;
            auto* vi = dynamic_cast<VarItem*>(sym->node);
            Location loc = vi ? vi->loc : body->loc;
            Type* ft = sym->get_type();
            if (ft && ft->is_reference())
                error_(loc, "reference field '" + sym_display_name(sym) +
                      "' is never bound: bind it in INIT on every path");
            else if (sym->is_const())
                error_(loc, "const field '" + sym_display_name(sym) +
                      "' is never assigned: assign it in INIT on every path "
                      "or give it an initializer");
            else
                error_(loc, "field '" + sym_display_name(sym) +
                      "' is never initialized: assign it in INIT on every path "
                      "or give it an initializer");
        }
    }
}

void FlowChecker::init_stmt(InitCtx& ctx, Stmt* s) {
    if (!s) return;
    // 语句前后比对游标：子树里改写过返回量的，连同本语句一起记下来，
    // can_use_register_ret 据此判断"末尾恰好一次赋值"
    size_t mark = ret_assigns_;
    init_stmt_body(ctx, s);
    if (ret_assigns_ != mark) ret_written_.insert(s);
}

void FlowChecker::init_stmt_body(InitCtx& ctx, Stmt* s) {
    if (auto* b = s->get_block()) {
        for (Stmt* x : b->stmts) {
            // 跳转/返回之后的语句在当前路径不可达，但 label 可能是别的
            // goto 的目标，仍要按合并后的入口状态继续检查
            if (ctx.terminated && !x->is_label()) continue;
            init_stmt(ctx, x);
        }
        return;
    }
    if (auto* l = s->get_label()) {
        auto it = goto_states_.find(std::string(l->label));
        if (it == goto_states_.end() || it->second.empty()) return;
        // 标签可能由顺序落入和若干 goto 同时到达：只有每条路径都已赋值
        // 的量才算在标签之后已初始化
        InitCtx merged = it->second.front();
        for (size_t i = 1; i < it->second.size(); ++i)
            merge_label_state(merged, it->second[i]);
        if (!ctx.terminated) merge_label_state(merged, ctx);
        ctx = std::move(merged);
        ctx.terminated = false;
        return;
    }
    if (auto* g = s->get_goto()) {
        // 先记下跳转点上的状态，目标 label 处再做交集合并
        goto_states_[std::string(g->label)].push_back(ctx);
        ctx.terminated = true;
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
            ctx.bound = std::move(else_ctx.bound);
        } else if (else_ctx.terminated) {
            ctx.assigned = std::move(then_ctx.assigned);
            ctx.bound = std::move(then_ctx.bound);
        } else {
            // 分支合并：仅在两个分支都已赋值的量才算已赋值
            std::unordered_set<Symbol*> merged;
            for (Symbol* sym : then_ctx.assigned) {
                if (else_ctx.assigned.count(sym)) merged.insert(sym);
            }
            for (Symbol* sym : ctx.assigned) merged.insert(sym);
            ctx.assigned = std::move(merged);
            // 一次性机会取两支的并集：任一支用过，分支之后就不许再用，
            // 两支各用一次不算冲突（每条路径各一次）
            ctx.bound.insert(then_ctx.bound.begin(), then_ctx.bound.end());
            ctx.bound.insert(else_ctx.bound.begin(), else_ctx.bound.end());
        }
        return;
    }
    if (auto* w = s->get_while()) {
        init_expr(ctx, w->condition);
        InitCtx body_ctx = ctx;
        init_stmt(body_ctx, w->body);
        // 循环体可能一次都不走，也可能反复走：其中的赋值同样消耗掉一次机会
        ctx.bound.insert(body_ctx.bound.begin(), body_ctx.bound.end());
        return;
    }
    if (auto* f = s->get_for()) {
        init_stmt(ctx, f->init);
        init_expr(ctx, f->condition);
        InitCtx body_ctx = ctx;
        init_stmt(body_ctx, f->body);
        init_stmt(body_ctx, f->next);
        ctx.bound.insert(body_ctx.bound.begin(), body_ctx.bound.end());
        return;
    }
    if (s->is_return()) {
        if (ctx.ret_sym && !ctx.assigned.count(ctx.ret_sym)) {
            error_(s->loc, "return value '" +
                  sym_display_name(ctx.ret_sym) + "' is not assigned on this path");
        }
        ctx.terminated = true;
        return;
    }
    if (auto* v = s->get_var_decl()) {
        VarList* vl = v->vars;
        if (!vl) return;
        bool is_static = vl->specs.has(SpecKind::Static);
        for (VarItem* vi : vl->vars) {
            Symbol* sym = sym_of(vi);
            if (vi->init) {
                init_expr(ctx, vi->init);
                if (sym) ctx.assigned.insert(sym);
            }
            // 数组没有初值时只分配了存储，元素仍未初始化：与标量一样，
            // 声明之后必须赋值才能读取
            if (sym) {
                ctx.declared.insert(sym);
                // static 局部量的初值在 main 入口写入，存储不属于本帧：
                // 除存储位置外的使用规则与普通局部量一致
                if (!is_static) ctx.locals.insert(sym);
            }
        }
        return;
    }
}

void FlowChecker::merge_label_state(InitCtx& dst, const InitCtx& src) {
    for (auto it = dst.assigned.begin(); it != dst.assigned.end();) {
        if (!src.assigned.count(*it)) it = dst.assigned.erase(it);
        else ++it;
    }
    // 一次性机会取并集：任一路径消耗过，合并之后都不许再用
    dst.bound.insert(src.bound.begin(), src.bound.end());
    dst.declared.insert(src.declared.begin(), src.declared.end());
    dst.locals.insert(src.locals.begin(), src.locals.end());
    dst.ref_fields.insert(src.ref_fields.begin(), src.ref_fields.end());
    dst.bound_before.insert(src.bound_before.begin(), src.bound_before.end());
    for (Symbol* sym : src.must_init)
        if (std::find(dst.must_init.begin(), dst.must_init.end(), sym) ==
            dst.must_init.end())
            dst.must_init.push_back(sym);
}

// 绑定源逐层剥到最终指着的量：剥成员、下标、一元解引用，
// 落在本帧存储上时返回该量，否则 nullptr。
// 本帧存储有两处：局部量，以及按值传入的标量/指针形参——形参的槽同样在
// 栈帧上，绑它自己返回即悬垂（引用形参与类形参的槽里存的是调用方的地址
// 与实例指针，不算）。全局量与字段的存储不在本帧。
// 引用量的值就是绑定源的地址，故要沿绑定链剥到底：声明处有初值的剥初值，
// 延迟绑用的剥流检查记下的那次绑定（自引用成环时按剥不动处理）。
// 剥过 -* 时地址落在被指向的对象上，形参指的地方不可能是本帧
// （值由调用方给出，那时本帧还不存在），因此放过；局部量指的地方无从判断，
// 按本帧存储处理
VarItem* FlowChecker::ref_bind_root(Expr* e, const InitCtx& ctx) const {
    bool derefed = false;
    for (int hop = 0; e && hop < 64; ++hop) {
        Symbol* sym = e->sym;
        if (sym && sym->kind == SymbolKind::Value) {
            auto* vi = dynamic_cast<VarItem*>(sym->node);
            if (!vi) return nullptr;
            Type* vt = vi->type;
            bool is_ref = vt && vt->is_reference();
            bool local = ctx.locals.count(sym) != 0;
            // 形参与返回量之外的都是全局量与类字段：存储不在本帧
            if (!local && !ctx.bound_before.count(sym)) return nullptr;
            if (!local) {
                if (is_ref || as_class(vt)) return nullptr;   // 引用/类形参
                if (derefed) return nullptr;                 // 剥过解引用
            }
            if (!is_ref) return vi;      // 局部量或按值形参的栈槽
            auto it = ref_src_.find(sym);
            if (it != ref_src_.end() && it->second && it->second != e) {
                e = it->second;
                derefed = false;
                continue;
            }
            if (vi->init && vi->init != e) {
                e = vi->init;
                derefed = false;
                continue;
            }
            return vi;
        }
        if (auto* m = e->get_member()) { e = m->target; continue; }
        if (auto* ix = e->get_index()) { e = ix->target; continue; }
        if (auto* u = e->get_unary()) {
            derefed = u->op == OpKind::Mul;
            e = u->operand;
            continue;
        }
        return nullptr;   // 字面量、调用等：没有量根
    }
    return nullptr;
}

// 赋值：目标符号的 const 与绑定规则，以及绑定点上的来源检查。
// const 的第一次赋值是初始化，之后再赋值报错；延迟绑定的局部引用同理，
// 本方法体里第一次赋值绑定被引用对象，之后的赋值是写穿。
// 登记在 declared 里的是本方法体的存储（局部量、形参、返回量、本类的
// 受管字段），未登记的量（全局量、static 字段、他类的字段）都已带着值，
// 不是初始化点；声明处带值的量（形参、字段初值、构造期）同理。
// 判断"已经赋过值"看当前路径：赋值写在分支里、分支之后再来一次才是二次写入，
    // 而 if/else 各赋一次是两条路径各一次，不冲突
    void FlowChecker::check_assign(InitCtx& ctx, Expr::Assign* a) {
    Expr* target = a ? a->target : nullptr;
    if (!target || !target->sym) return;
    Symbol* sym = target->sym;
    bool tracked = ctx.declared.count(sym) != 0;
    Type* dt = sym->get_type();
    // 延迟绑定的局部引用：声明处没有初值，第一次赋值才绑定。形参入口即绑定、
    // 返回量由 return 带出、引用字段只在 INIT 里绑定，都不是这里的绑定点
    VarItem* vi = dynamic_cast<VarItem*>(sym->node);
    bool late_ref = tracked && dt && dt->is_reference() && vi && !vi->init &&
                    !ctx.bound_before.count(sym) && !ctx.ref_fields.count(sym);
    bool ref_binding = false;
    if (sym->is_const()) {
        if (!tracked) {
            error_(target->loc,
                   "cannot assign to const '" + sym_display_name(sym) + "'");
        } else if (ctx.assigned.count(sym) || ctx.bound.count(sym)) {
            error_(target->loc, "cannot assign to const '" + sym_display_name(sym) +
                  "': it is already initialized");
        } else {
            ctx.bound.insert(sym);
        }
    } else if (late_ref) {
        if (ctx.bound.count(sym)) {
            // 绑定之后这次赋值是写穿：写穿得先有绑定可写，
            // 绑定点写在分支里时不保证已经执行
            if (!ctx.assigned.count(sym))
                error_(target->loc, "use of reference '" + sym_display_name(sym) +
                      "' before it is bound");
        } else {
            ctx.bound.insert(sym);
            ref_binding = true;
        }
    } else if (dt && dt->is_reference()) {
        // 引用字段在 INIT 里的赋值、引用返回量的赋值，存的都是地址
        ref_binding = (ctx.in_init && ctx.ref_fields.count(sym)) ||
                      sym == ctx.ret_sym;
    }
    if (tracked) ctx.assigned.insert(sym);
    if (sym == ctx.ret_sym) ++ret_assigns_;
    if (!ref_binding) return;
    if (late_ref) ref_src_[sym] = a->value;
    check_bind_source(ctx, sym, a->value);
}

// 绑定点的来源检查：存进引用槽的是被引用对象的地址，源得指着一个对象。
// 引用表达式沿用它已有的绑定，类值本身就是一个对象；
// 字面量（含 null）与当场算出来的值没有可绑定的存储。
// 引用返回还要再判一层：返回出去的地址得比本函数帧活得久
void FlowChecker::check_bind_source(InitCtx& ctx, Symbol* target, Expr* src) {
    if (!src) return;   // 解析器已报错
    // 绑定源写成 &lvalue 时等价于绑定该 lvalue
    src = src->bind_source();
    Type* st = src->type;
    bool obj = src->is_lvalue ||
               (st && (st->is_reference() || as_class(st) != nullptr));
    if (!obj) {
        error_(src->loc,
               "cannot bind a reference to a value that does not denote an object");
        return;
    }
    // 绑定不走常规值转换：引用目标类型与绑定源类型必须一致。
    // 绑定源本身是引用时沿用它已有的绑定，比较的是两边被引用的目标类型
    Type* tgt = target->get_type();
    if (tgt && tgt->is_reference()) tgt = tgt->get_reference()->target;
    if (st && st->is_reference()) st = st->get_reference()->target;
    if (tgt && st && !is_same_type(st, tgt)) {
        error_(src->loc, "reference target type mismatch");
        return;
    }
    if (target != ctx.ret_sym) return;
    VarItem* frame = ref_bind_root(src, ctx);
    if (!frame) return;
    bool local = ctx.locals.count(sym_of(frame)) != 0;
    error_(src->loc, std::string("cannot return a reference to ") +
          (local ? "local value '" : "parameter '") +
          std::string(frame->name) + "': its storage is gone once the "
          "function returns");
}

void FlowChecker::init_expr(InitCtx& ctx, Expr* e) {
    if (!e) return;
    // 读取检查：局部量、形参、返回量与类的受管字段都登记在 declared 里，
    // 其中 const 字段在 INIT 内尚未赋值、引用字段尚未绑定时同样不得读取
    auto check_read = [&](Symbol* sym) {
        if (!sym || !ctx.declared.count(sym) || ctx.assigned.count(sym)) return;
        Type* dt = sym->get_type();
        if (dt && dt->is_reference())
            error_(e->loc, "use of reference '" + sym_display_name(sym) +
                  "' before it is bound");
        else
            error_(e->loc, "use of uninitialized value '" +
                  sym_display_name(sym) + "'");
    };
    if (e->get_identifier()) {
        check_read(e->sym);
        return;
    }
    if (auto* a = e->get_assign()) {
        init_expr(ctx, a->value);
        // INIT 内对引用字段的赋值是绑定：形参与局部量在构造返回后即失效
        if (a->target && a->target->sym && ctx.in_init &&
            ctx.ref_fields.count(a->target->sym) && a->value &&
            a->value->sym &&
            (ctx.locals.count(a->value->sym) ||
             ctx.bound_before.count(a->value->sym)))
            error_(a->value->loc,
                   "cannot bind a reference field to a local or parameter "
                   "of INIT: it is dead once INIT returns");
        check_assign(ctx, a);
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
    if (auto* m = e->get_member()) { init_expr(ctx, m->target); check_read(e->sym); return; }
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
    Type* ty = ret_var->type ? ret_var->type : rl->type;
    if (!ty) return false;
    bool scalar_ret = false;
    if (auto* p = ty->get_primitive())
        scalar_ret = p->kind != PrimitiveTypeKind::Void;
    else if (ty->get_pointer() || ty->get_reference() ||
             ty->get_func_ptr() || ty->get_func_ref() || ty->is_null())
        scalar_ret = true;
    else if (is_enum_type(ty))
        scalar_ret = true;
    if (!scalar_ret) return false;
    Symbol* sym = sym_of(ret_var);
    if (!sym) return false;
    // 返回值声明处带初始化表达式：初始化即唯一一次写入，函数体不再改写
    if (ret_var->init) {
        return ret_written_.empty();
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
// 语句序列末尾恰好一次 r = X：末尾语句自身是这次赋值，
// 其余语句都不能改写过 r（流检查遍历时记在 ret_written_ 里）
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
        if (ret_written_.count(stmts[i])) return false;
    }
    return true;
}

} // namespace cx
