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
        Type* at = c.args[i] ? c.args[i]->type : nullptr;
        Type* pt = fp->params[i];
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
        if (op->specs.has(SpecKind::No))
            error(call_expr->loc, "operator '()' is marked 'no' and cannot be "
                                  "called");
        Type* ret = typeVoid;
        if (!op->returns.empty() && op->returns.back() && op->returns.back()->type)
            ret = op->returns.back()->type;
        check_call_args(op->params, c.args, call_expr->loc);
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
    SpecialMethod* synth_ctor = nullptr;
    bool has_init = false;
    for (Decl* m : cls->members) {
        if (!m || m->kind != DeclKind::SpecialMethod) continue;
        auto* sm = static_cast<SpecialMethod*>(m);
        if (sm->kind != SpecialMethod::Kind::Init) continue;
        // 派发的 INIT(T) 不阻止零参默认构造：无用户 INIT 的包装类仍可 num()
        if (!sm->synthesized) has_init = true;
        if (!args_match(sm->params, c.args)) continue;
        // 用户 INIT 优先于派发的 INIT(T)，注入方向不影响这一优先级
        if (!sm->synthesized) { ctor = sm; break; }
        if (!synth_ctor) synth_ctor = sm;
    }
    if (!ctor) ctor = synth_ctor;
    // 无 INIT 的类允许零参默认构造，字段用声明处默认值初始化
    if (!ctor && (has_init || !c.args.empty())) {
        call_fail(call_expr, "no matching constructor for class '" + name + "'");
        return true;
    }
    if (ctor) check_call_args(ctor->params, c.args, call_expr->loc);
    // 参数已是同类临时值：派发的 INIT(T) 只会先解包再原样包回去
    if (ctor && ctor == synth_ctor && c.args.size() == 1 && c.args[0] &&
        c.args[0]->type && !c.args[0]->is_lvalue &&
        c.args[0]->type->resolved_decl == cls_decl)
        warning(c.callee ? c.callee->loc : call_expr->loc,
                "unnecessary cast, types are identical");

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
    std::vector<Type*> params;
    for (VarList* vl : func->params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars)
            if (vi && vi->type) params.push_back(vi->type);
    }
    if (params.size() != 1 || !params[0]->is_array_type() || c.args.empty()) return;
    Type* pt = params[0];
    bool direct = c.args.size() == 1 && c.args[0] && c.args[0]->type &&
                  is_same_type(c.args[0]->type, pt);
    if (direct) return;
    Type* elem = pt->get_array_type()->element;
    bool ok = elem != nullptr;
    for (Expr* a : c.args) {
        if (!a || !a->type) { ok = false; break; }
        if (!can_convert_to(a->type, elem)) { ok = false; break; }
        check_array_len_fit(a, elem, "parameter element", a->loc);
    }
    if (!ok) return;
    Expr* arr = cu_.add_node<Expr>(call_expr->loc, Expr::Array{c.args, nullptr});
    arr->type = make_type(call_expr->loc,
        Type::ArrayType{elem, make_int_literal(call_expr->loc, c.args.size())});
    arr->is_lvalue = false;
    c.args = {arr};
}

// 类形参的调用点打包：foo(a b c "Hi") 等价于 foo(Param(a b c "Hi"))
void SemanticAnalyzer::pack_class_args(Func* func, Expr* call_expr, Expr::Call& c) {
    if (func->params.empty() || c.args.empty()) return;
    std::vector<Type*> params;
    for (VarList* vl : func->params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars)
            if (vi && vi->type) params.push_back(vi->type);
    }
    if (params.size() != 1) return;
    Type* pt = params[0];
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

    check_no_call(f_sym, call_expr->loc);

    // mut 方法会修改自身对象：不带 mut 的成员方法不得调用
    if (func->specs.has(SpecKind::Mut) && in_non_mut_method()) {
        error(call_expr->loc, "cannot call mut method '" + name +
              "' from a non-mut method: declare the calling method mut");
    }

    pack_array_args(func, call_expr, c);
    pack_class_args(func, call_expr, c);
    check_call_args(func, c.args, call_expr->loc);

    Type* ret = typeVoid;
    if (!func->returns.empty() && func->returns.back() &&
        func->returns.back()->type) {
        ret = func->returns.back()->type;
    }

    call_expr->sym = f_sym;
    call_expr->type = ret;
    call_expr->is_lvalue = ref_is_lvalue(ret);
    c.resolved = func;
}

// 泛型调用：显式 `-<T>` 实参或按实参类型推导，实例化后把被调用者
// 改写为实例，后续按普通命名调用解析。泛型类在类型位置已实例化，
// 值位置的裸调用无从确定实例，在此拦下。
// 返回 false 表示实例化失败：错误已报出，调用到此结束
bool SemanticAnalyzer::resolve_generic_call(Expr* call_expr, Expr::Call& c,
                                            Symbol*& callee_sym) {
    Decl* tmpl = callee_sym ? callee_sym->get_decl() : nullptr;
    if (!is_generic_decl(tmpl)) return true;
    if (tmpl->kind == DeclKind::Class) {
        call_fail(call_expr, "'" + std::string(tmpl->name) + "' is generic: write '" +
                             std::string(tmpl->name) + "-<...>' to use it");
        return false;
    }

    std::vector<Type*> deduced_only;   // 无显式实参：全靠实参类型推导
    const std::vector<Type*>& args = c.callee->is_generic_type()
        ? c.callee->get_generic_type()->args : deduced_only;
    std::vector<Type*> arg_types;
    arg_types.reserve(c.args.size());
    for (Expr* a : c.args) arg_types.push_back(a ? a->type : nullptr);

    Func* inst = instantiate_func(*static_cast<Func*>(tmpl), args, arg_types,
                                  call_expr->loc);
    if (!inst) {
        // instantiate 已报出具体原因
        call_expr->type = typeVoid;
        call_expr->is_lvalue = false;
        return false;
    }
    // 实例是普通函数，被调用者写成裸名：原模板若是成员方法，实例化后
    // 不再带 self，改由实例所在作用域里的裸函数符号调用
    c.callee = instance_ref_node(nullptr, inst);
    callee_sym = c.callee->sym;
    return true;
}

void SemanticAnalyzer::resolve_call(Expr* call_expr, Expr::Call& c) {
    if (!c.callee) {
        call_fail(call_expr, "call expression has no callee");
        return;
    }

    if (resolve_funcptr_call(call_expr, c)) return;
    if (resolve_functor_call(call_expr, c)) return;

    // 命名调用 → 解析被调用者名字，构造 callkey 后查表。
    // 被调用者得是名字引用（裸名、限定名，或泛型应用的基名）：其他表达式
    // 取它的 sym 会拿到表达式内部函数的符号，`(foo(a))()` 就会被当成
    // 拿 0 个实参再调一次 foo。函数指针与 functor 已按类型在上面处理，
    // 到不了这里的才是真调用不了
    Expr* name_expr = c.callee;
    if (auto* g = c.callee->get_generic_type()) name_expr = g->base;
    std::string name;
    if (auto* id = name_expr ? name_expr->get_identifier() : nullptr) {
        name = std::string(id->name);
    } else if (auto* m = name_expr ? name_expr->get_member() : nullptr) {
        name = std::string(m->member);
    } else {
        call_fail(call_expr, "invalid callee expression");
        return;
    }
    Symbol* callee_sym = c.callee->sym;

    if (!resolve_generic_call(call_expr, c, callee_sym)) return;

    if (resolve_ctor_call(call_expr, c, callee_sym, name)) return;
    if (resolve_free_builtin(call_expr, c, name)) return;
    resolve_named_call(call_expr, c, callee_sym, name);
}

// 平铺多个 VarList 声明的形参类型节点
std::vector<Type*> SemanticAnalyzer::param_types(
        const std::vector<VarList*>& lists) {
    std::vector<Type*> out;
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
bool SemanticAnalyzer::check_ref_param(Expr* arg, Type* param) {
    if (!arg || !param || !param->is_reference()) return false;
    Type* tgt = param->get_reference()->target;
    // 绑定源写成 &lvalue 时与直接绑定该 lvalue 等价
    Expr* src = arg->bind_source();
    if (!src->is_lvalue)
        error(arg->loc, "reference parameter requires an lvalue argument");
    else if (tgt && src->type && !is_same_type(src->type, tgt))
        error(arg->loc, "reference parameter target type mismatch");
    return true;
}

// 形参列表逐一对实参做转换判定
void SemanticAnalyzer::check_call_args(const std::vector<VarList*>& params_lists,
                                       const std::vector<Expr*>& args,
                                       const Location& loc) {
    std::vector<Type*> params;
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
        Type* at = args[i] ? args[i]->type : nullptr;
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

// () 传参：按形参类型逐一对实参做转换判定。
// loc 是调用点位置，实参个数不符时报在那里而不是被调函数的声明处
void SemanticAnalyzer::check_call_args(Func* fn, const std::vector<Expr*>& args,
                                       const Location& loc) {
    if (!fn) return;
    if (fn->is_variadic) {
        // 变参调用：只检查固定形参前缀，多余实参不做转换检查
        size_t fixed = 0;
        for (VarList* vl : fn->params)
            for (VarItem* vi : vl->vars) if (vi) ++fixed;
        std::vector<Expr*> prefix(args.begin(),
            args.begin() + std::min(args.size(), fixed));
        check_call_args(fn->params, prefix, loc);
        return;
    }
    check_call_args(fn->params, args, loc);
}

// 非报错式匹配：形参数量一致且每个实参可转换
bool SemanticAnalyzer::args_match(const std::vector<VarList*>& params_lists,
                                  const std::vector<Expr*>& args) {
    std::vector<Type*> params;
    for (VarList* vl : params_lists) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            if (vi && vi->type) params.push_back(vi->type);
        }
    }
    if (params.size() != args.size()) return false;
    for (size_t i = 0; i < args.size(); ++i) {
        Type* at = args[i] ? args[i]->type : nullptr;
        Type* pt = params[i];
        if (!at || !pt) continue;
        if (!can_convert_to(at, pt)) return false;
    }
    return true;
}
} // namespace cx
