#include "../include/semantic.h"
#include "../include/sem_types.h"

namespace cx {

// 调用解析：函数指针 / functor / 构造 / free 内置 / 数组实参打包 / 命名调用
// 以及实参匹配检查。路径分支密集，独立成文件以便评审。

void SemanticAnalyzer::call_fail(Expr* call_expr, const std::string& msg) {
    error(call_expr->loc, msg);
    call_expr->type = typeVoid;
    call_expr->is_lvalue = false;
}

// 函数指针调用：callee 的类型是 $(...) 时按类型调用
bool SemanticAnalyzer::resolve_funcptr_call(Expr* call_expr, Expr::Call& c) {
    if (!c.callee->type || !c.callee->type->is_func_ptr()) return false;
    auto* fp = c.callee->type->get_func_ptr();

    if (fp->params.size() != c.args.size()) {
        error(call_expr->loc,
              "function pointer expects " + std::to_string(fp->params.size()) +
              " argument(s), got " + std::to_string(c.args.size()));
    }
    for (size_t i = 0; i < c.args.size() && i < fp->params.size(); ++i) {
        Expr* at = c.args[i] ? c.args[i]->type : nullptr;
        Expr* pt = fp->params[i];
        if (check_ref_param(c.args[i], pt)) continue;
        if (at && pt && !can_convert_to(at, pt)) {
            warning(c.args[i]->loc, "argument type mismatch in call");
        }
    }

    call_expr->sym = c.callee->sym;
    call_expr->type = fp->return_type ? fp->return_type : typeVoid;
    call_expr->is_lvalue = ref_is_lvalue(call_expr->type);
    return true;
}

// 被调用者是类实例 → 查找 operator ()
bool SemanticAnalyzer::resolve_functor_call(Expr* call_expr, Expr::Call& c) {
    if (!(c.callee->type && c.callee->type->resolved_decl &&
          c.callee->type->resolved_decl->kind == DeclKind::Class &&
          !(c.callee->sym && c.callee->sym->kind == SymbolKind::Class)))
        return false;
    auto* cls = static_cast<Class*>(c.callee->type->resolved_decl);
    for (Decl* m : cls->members) {
        if (!m || m->kind != DeclKind::Operator) continue;
        auto* op = static_cast<Operator*>(m);
        if (op->op != OpKind::Paren) continue;
        if (!args_match(op->params, c.args)) continue;
        Expr* ret = typeVoid;
        if (!op->returns.empty() && op->returns.back() && op->returns.back()->type)
            ret = op->returns.back()->type;
        check_call_args(op->params, c.args, op->loc);
        call_expr->sym = nullptr;
        call_expr->type = ret;
        call_expr->is_lvalue = ref_is_lvalue(ret);
        c.resolved = op;
        return true;
    }
    return false;
}

// 类构造：遍历类成员中的 INIT 逐个匹配参数
bool SemanticAnalyzer::resolve_ctor_call(Expr* call_expr, Expr::Call& c,
                                         Symbol* callee_sym, const std::string& name) {
    if (!(callee_sym && callee_sym->kind == SymbolKind::Class)) return false;
    Scope* cls_scope = callee_sym->scope;
    Decl* cls_decl = callee_sym->get_decl();
    if (!cls_scope || !cls_decl || cls_decl->kind != DeclKind::Class) {
        call_fail(call_expr, "class '" + name + "' has no scope");
        return true;
    }
    Class* cls = static_cast<Class*>(cls_decl);

    SpecialMethod* ctor = nullptr;
    bool has_init = false;
    for (Decl* m : cls->members) {
        if (!m || m->kind != DeclKind::SpecialMethod) continue;
        auto* sm = static_cast<SpecialMethod*>(m);
        if (sm->kind != SpecialMethod::Kind::Init) continue;
        has_init = true;
        if (args_match(sm->params, c.args)) { ctor = sm; break; }
    }
    // 无 INIT 的类允许零参默认构造，字段用声明处默认值初始化
    if (!ctor && (has_init || !c.args.empty())) {
        call_fail(call_expr, "no matching constructor for class '" + name + "'");
        return true;
    }
    if (ctor) check_call_args(ctor->params, c.args, ctor->loc);

    call_expr->sym = nullptr;
    call_expr->type = make_named_type(call_expr->loc, cls->name, cls, cls_scope);
    call_expr->is_lvalue = false;
    c.resolved = ctor;
    return true;
}

// malloc 实例的内置 free()：a.free() 释放堆实例；
// 类方法内的裸 free() 同样指向内置实现（self 即被释放对象）
bool SemanticAnalyzer::resolve_free_builtin(Expr* call_expr, Expr::Call& c,
                                            const std::string& name) {
    if (name != "free" || !c.args.empty() || !c.callee) return false;
    Class* cls = nullptr;
    bool on_self = false;
    if (auto* m = c.callee->get_member()) {
        cls = m->target && m->target->type
            ? wrap_or_plain_class(m->target->type) : nullptr;
        if (!is_malloc_value(m->target)) return false;
    } else if (c.callee->is_identifier() && current_class) {
        cls = current_class;
        on_self = true;
    } else {
        return false;
    }
    if (!cls) return false;
    call_expr->sym = nullptr;
    call_expr->type = typeVoid;
    call_expr->is_lvalue = false;
    c.free_builtin = true;
    c.free_on_self = on_self;
    return true;
}

// 数组形参的调用点打包：f(a b c) 等价于 f([a b c])
void SemanticAnalyzer::pack_array_args(Func* func, Expr* call_expr, Expr::Call& c) {
    if (func->params.empty()) return;
    std::vector<Expr*> params;
    for (VarList* vl : func->params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars)
            if (vi && vi->type) params.push_back(vi->type);
    }
    if (params.size() != 1 || !params[0]->is_array_type() || c.args.empty()) return;
    Expr* pt = params[0];
    bool direct = c.args.size() == 1 && c.args[0] && c.args[0]->type &&
                  is_same_type(c.args[0]->type, pt);
    if (direct) return;
    Expr* elem = pt->get_array_type()->element;
    bool ok = elem != nullptr;
    for (Expr* a : c.args) {
        if (!a || !a->type) { ok = false; break; }
        if (!can_convert_to(a->type, elem)) { ok = false; break; }
        check_array_len_fit(a, elem, "parameter element", a->loc);
    }
    if (!ok) return;
    Expr* arr = cu_.add_node<Expr>(call_expr->loc, Expr::Array{c.args, nullptr});
    arr->type = make_type(call_expr->loc,
        Expr::ArrayType{elem, make_int_literal(call_expr->loc, c.args.size())});
    arr->is_lvalue = false;
    c.args = {arr};
}

// 类形参的调用点打包：foo(a b c "Hi") 等价于 foo(Param(a b c "Hi"))
void SemanticAnalyzer::pack_class_args(Func* func, Expr* call_expr, Expr::Call& c) {
    if (func->params.empty() || c.args.empty()) return;
    std::vector<Expr*> params;
    for (VarList* vl : func->params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars)
            if (vi && vi->type) params.push_back(vi->type);
    }
    if (params.size() != 1) return;
    Expr* pt = params[0];
    Class* cls = as_class(pt);
    if (!cls) return;
    // 单个实参且已是该类，无需打包
    if (c.args.size() == 1 && c.args[0] && c.args[0]->type &&
        is_same_type(c.args[0]->type, pt)) return;
    // 仅当类的某个 INIT 能匹配这组实参时才打包，否则交给常规错误报告
    bool packable = false;
    for (Decl* m : cls->members) {
        if (!m || m->kind != DeclKind::SpecialMethod) continue;
        auto* sm = static_cast<SpecialMethod*>(m);
        if (sm->kind != SpecialMethod::Kind::Init) continue;
        if (args_match(sm->params, c.args)) { packable = true; break; }
    }
    if (!packable) return;

    Expr* callee = cu_.add_node<Expr>(call_expr->loc,
        Expr::Identifier{cls->name});
    callee->is_type = true;
    callee->resolved_decl = cls;
    callee->resolved_scope = cls->scope;
    callee->sym = pt->sym ? pt->sym
                          : symbols_.lookup_member(std::string(cls->name));
    Expr* inner = cu_.add_node<Expr>(call_expr->loc,
        Expr::Call{callee, c.args});
    resolve_call(inner, *inner->get_call());
    if (!inner->type || is_void_type(inner->type)) return;
    c.args = {inner};
}

// 命名调用：已解析出函数符号时直接使用
void SemanticAnalyzer::resolve_named_call(Expr* call_expr, Expr::Call& c,
                                          Symbol* callee_sym, const std::string& name) {
    Symbol* f_sym = nullptr;
    if (callee_sym && callee_sym->kind == SymbolKind::Function) {
        f_sym = callee_sym;
    } else {
        f_sym = symbols_.lookup_member(name);
    }
    if (!f_sym || f_sym->kind != SymbolKind::Function) {
        call_fail(call_expr, "no matching function for call to '" + name + "'");
        return;
    }

    Func* func = nullptr;
    if (Decl* d = f_sym->get_decl()) {
        if (d->kind == DeclKind::Func || d->kind == DeclKind::Operator)
            func = static_cast<Func*>(d);
    }
    if (!func) {
        call_fail(call_expr, "callee '" + name + "' is not a function");
        return;
    }

    pack_array_args(func, call_expr, c);
    pack_class_args(func, call_expr, c);
    check_call_args(func, c.args);

    Expr* ret = typeVoid;
    if (!func->returns.empty() && func->returns.back() &&
        func->returns.back()->type) {
        ret = func->returns.back()->type;
    }

    call_expr->sym = f_sym;
    call_expr->type = ret;
    call_expr->is_lvalue = ref_is_lvalue(ret);
    c.resolved = func;
}

void SemanticAnalyzer::resolve_call(Expr* call_expr, Expr::Call& c) {
    if (!c.callee) {
        call_fail(call_expr, "call expression has no callee");
        return;
    }

    if (resolve_funcptr_call(call_expr, c)) return;
    if (resolve_functor_call(call_expr, c)) return;

    // 命名调用 → 解析被调用者名字，构造 callkey 后查表
    std::string name;
    Symbol* callee_sym = c.callee->sym;
    if (callee_sym) {
        if (Decl* d = callee_sym->get_decl()) {
            name = std::string(d->name);
        }
    }
    if (name.empty()) {
        if (auto* id = c.callee->get_identifier()) {
            name = std::string(id->name);
        } else if (auto* m = c.callee->get_member()) {
            name = std::string(m->member);
        }
    }
    if (name.empty()) {
        call_fail(call_expr, "invalid callee expression");
        return;
    }

    if (resolve_ctor_call(call_expr, c, callee_sym, name)) return;
    if (resolve_free_builtin(call_expr, c, name)) return;
    resolve_named_call(call_expr, c, callee_sym, name);
}

// 平铺多个 VarList 声明的形参类型节点
std::vector<Expr*> SemanticAnalyzer::param_types(
        const std::vector<VarList*>& lists) {
    std::vector<Expr*> out;
    for (VarList* vl : lists) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            if (vi) out.push_back(vi->type ? vi->type : vl->type);
        }
    }
    return out;
}

// 引用形参按绑定语义匹配：实参须为 lvalue，且类型与被引用目标一致。
// 绑定不走值转换，故返回 true 表示已处理完毕
bool SemanticAnalyzer::check_ref_param(Expr* arg, Expr* param) {
    if (!arg || !param || !param->is_reference()) return false;
    Expr* tgt = param->get_reference()->target;
    if (!arg->is_lvalue)
        error(arg->loc, "reference parameter requires an lvalue argument");
    else if (tgt && arg->type && !is_same_type(arg->type, tgt))
        error(arg->loc, "reference parameter target type mismatch");
    return true;
}

// 形参列表逐一对实参做转换判定
void SemanticAnalyzer::check_call_args(const std::vector<VarList*>& params_lists,
                                       const std::vector<Expr*>& args,
                                       const Location& loc) {
    std::vector<Expr*> params;
    for (VarList* vl : params_lists) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            if (vi && vi->type) params.push_back(vi->type);
        }
    }
    if (params.size() != args.size()) {
        error(loc, "argument count mismatch: expected " +
              std::to_string(params.size()) + ", got " + std::to_string(args.size()));
        return;
    }
    for (size_t i = 0; i < args.size(); ++i) {
        if (check_ref_param(args[i], params[i])) continue;
        Expr* at = args[i] ? args[i]->type : nullptr;
        if (!at || !params[i]) continue;
        check_array_len_fit(args[i], params[i], "parameter",
                            args[i] ? args[i]->loc : loc);
        check_param_array_len(args[i], at, params[i],
                              args[i] ? args[i]->loc : loc);
        check_literal_range(args[i], params[i], "argument passing",
                            args[i] ? args[i]->loc : loc);
        // 记录隐式 conversion / 包装转换，供 translator 派发；同类也允许 self->self
        if (record_conversion(args[i], params[i])) continue;
        check_conversion(at, params[i], "argument passing",
                         args[i] ? args[i]->loc : loc);
    }
}

// () 传参：按形参类型逐一对实参做转换判定
void SemanticAnalyzer::check_call_args(Func* fn, const std::vector<Expr*>& args) {
    if (!fn) return;
    if (fn->is_variadic) {
        // 变参调用：只检查固定形参前缀，多余实参不做转换检查
        size_t fixed = 0;
        for (VarList* vl : fn->params)
            for (VarItem* vi : vl->vars) if (vi) ++fixed;
        std::vector<Expr*> prefix(args.begin(),
            args.begin() + std::min(args.size(), fixed));
        check_call_args(fn->params, prefix, fn->loc);
        return;
    }
    check_call_args(fn->params, args, fn->loc);
}

// 非报错式匹配：形参数量一致且每个实参可转换
bool SemanticAnalyzer::args_match(const std::vector<VarList*>& params_lists,
                                  const std::vector<Expr*>& args) {
    std::vector<Expr*> params;
    for (VarList* vl : params_lists) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            if (vi && vi->type) params.push_back(vi->type);
        }
    }
    if (params.size() != args.size()) return false;
    for (size_t i = 0; i < args.size(); ++i) {
        Expr* at = args[i] ? args[i]->type : nullptr;
        Expr* pt = params[i];
        if (!at || !pt) continue;
        if (!can_convert_to(at, pt)) return false;
    }
    return true;
}
} // namespace cx
