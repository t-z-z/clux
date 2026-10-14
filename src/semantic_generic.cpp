#include "../include/semantic.h"
#include "../include/sem_types.h"

#include <string>
#include <utility>
#include <vector>

namespace cx {

// 泛型语义：模板摘除与登记、实参推导、按实参实例化。
// 语义阶段结束后 AST 里只剩具体实例，translator 与泛型无关。

namespace {

// 实例名进入 LLVM 标识符，type_to_key 的记号在此映射成字母
std::string safe_key(std::string_view key) {
    std::string out;
    out.reserve(key.size());
    for (char c : key) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.') {
            out += c;
        } else if (c == '*') {
            out += 'p';
        } else if (c == '&') {
            out += 'r';
        } else if (c == '%') {
            // 类类型键的前缀，类名自带
        } else if (c == '[') {
            out += 'a';
        } else if (c == ';') {
            out += 'x';
        } else if (c == ']') {
            out += '_';
        } else if (c == ',') {
            out += '_';
        } else if (c == '(') {
            out += 'f';
        } else if (c == ')') {
            out += '_';
        } else {
            out += '_';
        }
    }
    return out;
}

// 泛型替换点收集：命中泛型形参名的标识符。类型位置换成实参类型，
// 值位置没有对应物（形参只是类型名），另记下来报错
struct SubstCollector : ASTVisitor {
    const TypeSubst* subst = nullptr;
    std::vector<std::pair<Expr*, Expr*>> hits;
    std::vector<std::pair<Expr*, std::string_view>> value_hits;
    void visit_expr_identifier(Expr::Identifier& id) override {
        if (!subst || !current_expr) return;
        auto it = subst->find(id.name);
        if (it == subst->end()) return;
        if (current_expr->is_type) hits.emplace_back(current_expr, it->second);
        else value_hits.emplace_back(current_expr, id.name);
    }
};

} // namespace

// 模板摘除：泛型模板本体不进入语义遍历与 IR，只留登记信息供实例化时取回。
// 成员表里其余成员原样保留
void SemanticAnalyzer::extract_generic_templates(std::vector<Decl*>& members,
                                                Scope* scope,
                                                bool default_public) {
    std::vector<Decl*> keep;
    for (Decl* m : members) {
        if (m && is_generic_decl(m)) {
            generics_->templates[m] = GenericState::Template{m, scope, default_public};
            continue;
        }
        keep.push_back(m);
    }
    members = std::move(keep);
}

// 容器内的泛型模板：命名空间/extern/类逐层下钻，各层 scope 取容器自身的作用域
void SemanticAnalyzer::collect_generic_in(std::vector<Decl*>& list, Scope* scope,
                                         bool default_public) {
    for (Decl* d : list) {
        if (!d) continue;
        switch (d->kind) {
            case DeclKind::Namespace: {
                auto* ns = static_cast<Namespace*>(d);
                if (ns->scope) collect_generic_in(ns->members, ns->scope, true);
                break;
            }
            case DeclKind::Extern: {
                auto* ext = static_cast<Extern*>(d);
                if (ext->scope)
                    collect_generic_in(ext->members, ext->scope, default_public);
                break;
            }
            case DeclKind::Class: {
                // 泛型类的成员随模板整体保留：实例化时从实例成员里再摘一次，
                // 此时它们的作用域才是实例作用域
                if (is_generic_decl(d)) break;
                auto* cls = static_cast<Class*>(d);
                if (cls->scope) collect_generic_in(cls->members, cls->scope, false);
                break;
            }
            default:
                break;
        }
    }
    extract_generic_templates(list, scope, default_public);
}

void SemanticAnalyzer::collect_generics() {
    collect_generic_in(cu_.top_decls, symbols_.root(), true);
}

std::string_view SemanticAnalyzer::intern_name(std::string name) {
    instance_names_.push_back(std::move(name));
    return instance_names_.back();
}

// 实例名与缓存键：实参类型键按顺序以 '$' 连接，同一组实参得到同一个名字
std::string SemanticAnalyzer::instance_key(const std::vector<Expr*>& args) {
    std::string key;
    for (Expr* a : args) {
        key += "$";
        key += safe_key(a ? type_to_key(*a) : "null");
    }
    return key;
}

// 作用域链拼出的限定名（根作用域不计）：实例挂到顶层声明后仍须唯一，
// 否则不同容器里的同名实例会在 IR 里撞名
std::string SemanticAnalyzer::scope_path(Scope* s) const {
    std::vector<std::string_view> parts;
    for (Scope* c = s; c && c->parent; c = c->parent) parts.push_back(c->name);
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        out += *it;
        out += '_';
    }
    return out;
}

// 模板已从 AST 摘除，仍可按声明指针取回登记信息
bool SemanticAnalyzer::generic_template_of(const Decl* d,
                                           GenericState::Template& out) const {
    auto it = generics_->templates.find(d);
    if (it == generics_->templates.end()) return false;
    out = it->second;
    return true;
}

Symbol* SemanticAnalyzer::instance_symbol(Decl* inst) {
    auto it = generics_->instance_scope.find(inst);
    if (it == generics_->instance_scope.end() || !it->second) return nullptr;
    return it->second->lookup_member(std::string(inst->name));
}

// 实例声明的类型节点：限定名与模板同名形，指向实例而非模板
Expr* SemanticAnalyzer::instance_type_node(Expr* base, Decl* inst, bool is_type) {
    Location loc = base ? base->loc : inst->loc;
    Expr* t = nullptr;
    if (auto* m = base ? base->get_member() : nullptr)
        t = cu_.add_node<Expr>(loc, Expr::Member{m->target, inst->name});
    else
        t = cu_.add_node<Expr>(loc, Expr::Identifier{inst->name});
    t->is_type = is_type;
    if (is_type) {
        t->resolved_decl = inst;
        if (inst->kind == DeclKind::Class) {
            t->resolved_scope = static_cast<Class*>(inst)->scope;
        } else {
            auto it = generics_->instance_scope.find(inst);
            t->resolved_scope = it == generics_->instance_scope.end() ? nullptr : it->second;
        }
    } else if (inst->kind == DeclKind::Func || inst->kind == DeclKind::Operator) {
        t->type = make_type(loc, Expr::FuncRef{static_cast<Func*>(inst)});
    }
    if (Symbol* s = instance_symbol(inst)) t->sym = s;
    return t;
}

// 从实参类型推导一个泛型形参的实例化：命中泛型形参则绑定或校验一致性；
// 否则退化为转换判定，转换不通即推导失败
bool SemanticAnalyzer::deduce_type(Expr* param, Expr* arg, TypeSubst& subst,
                                   const std::vector<TypeParam*>& tparams) {
    if (!param || !arg) return false;
    if (param->is_type && param->is_identifier()) {
        std::string_view name = param->get_identifier()->name;
        bool is_tparam = false;
        for (TypeParam* tp : tparams) {
            if (tp && tp->name == name) { is_tparam = true; break; }
        }
        if (is_tparam) {
            auto it = subst.find(name);
            if (it == subst.end()) {
                // null/inferred 没有可用的实例类型，不能作为实参绑定
                if (arg->is_null() || arg->is_inferred()) return false;
                subst[name] = arg;
                return true;
            }
            // 已绑定：实参须与既定实例一致或可转换过去（显式实参优先）
            return can_convert_to(arg, it->second);
        }
    }
    if (param->is_pointer() && arg->is_pointer()) {
        return deduce_type(param->get_pointer()->pointee,
                           arg->get_pointer()->pointee, subst, tparams);
    }
    if (param->is_reference() && arg->is_reference()) {
        return deduce_type(param->get_reference()->target,
                           arg->get_reference()->target, subst, tparams);
    }
    // 数组长度不参与推导，长度不符由 check_param_array_len 报出
    if (param->is_array_type() && arg->is_array_type()) {
        return deduce_type(param->get_array_type()->element,
                           arg->get_array_type()->element, subst, tparams);
    }
    return can_convert_to(arg, param);
}

Class* SemanticAnalyzer::instantiate_class(Class& tmpl,
                                          const std::vector<Expr*>& args,
                                          const Location& loc) {
    if (args.size() != tmpl.type_params.size()) {
        error(loc, "generic class '" + std::string(tmpl.name) + "' expects " +
              std::to_string(tmpl.type_params.size()) +
              " type argument(s), got " + std::to_string(args.size()));
        return nullptr;
    }
    TypeSubst subst;
    for (size_t i = 0; i < args.size(); ++i) {
        if (!args[i]) return nullptr;
        subst[tmpl.type_params[i]->name] = args[i];
    }
    std::string key = instance_key(args);
    auto cached = generics_->cache.find({&tmpl, key});
    if (cached != generics_->cache.end())
        return static_cast<Class*>(cached->second);

    // 模板由别的模块 import 而来时不在本模块登记：作用域退回模块根，
    // 与实例挂到顶层声明后的发射位置一致
    Scope* scope = symbols_.root();
    bool pub = true;
    {
        GenericState::Template t;
        if (generic_template_of(&tmpl, t)) { scope = t.scope; pub = t.default_public; }
    }
    std::string name = scope_path(scope) + std::string(tmpl.name) + key;
    if (scope && scope->lookup_member(name)) {
        error(loc, "conflicting instantiation '" + name + "'");
        return nullptr;
    }

    Class* inst = static_cast<Class*>(cu_.clone_decl(&tmpl));
    substitute_types(inst, subst);
    inst->name = intern_name(std::move(name));
    inst->type_params.clear();
    inst->scope = nullptr;
    // 成员登记与嵌套类展开沿用注入展开的路径：实例类对注入器就是普通类
    Scope* saved = symbols_.current_scope();
    if (scope) symbols_.reset_scope(scope);
    injector_->expand_class(inst, scope);
    symbols_.reset_scope(saved);
    generics_->instance_scope[inst] = scope;
    if (inst->scope && scope) {
        if (Symbol* s = SymbolTable::lookup_member_in(scope, std::string(inst->name))) {
            s->default_public = pub;
            s->decl_scope = scope;
        }
        // 实例内部的泛型模板（嵌套泛型类、泛型方法）改指实例内作用域
        collect_generic_in(inst->members, inst->scope, false);
    }
    generics_->cache[{&tmpl, key}] = inst;
    generic_instances_.push_back(inst);
    analyze_instance(inst, scope);
    return inst;
}

Func* SemanticAnalyzer::instantiate_func(Func& tmpl,
                                        const std::vector<Expr*>& args,
                                        const std::vector<Expr*>& arg_types,
                                        const Location& loc) {
    std::string name(tmpl.name);
    TypeSubst subst;
    if (!args.empty()) {
        if (args.size() != tmpl.type_params.size()) {
            error(loc, "generic function '" + name + "' expects " +
                  std::to_string(tmpl.type_params.size()) +
                  " type argument(s), got " + std::to_string(args.size()));
            return nullptr;
        }
        for (size_t i = 0; i < args.size(); ++i) {
            if (!args[i]) return nullptr;
            subst[tmpl.type_params[i]->name] = args[i];
        }
    }

    // 显式实参只定形参类型，缺的部分仍由实参类型推导；两者冲突即推导失败
    std::vector<Expr*> params = param_types(tmpl.params);
    if (params.size() != arg_types.size()) {
        if (args.empty()) {
            error(loc, "argument count mismatch: expected " +
                  std::to_string(params.size()) + ", got " +
                  std::to_string(arg_types.size()));
            return nullptr;
        }
    } else {
        for (size_t i = 0; i < params.size(); ++i) {
            if (!deduce_type(params[i], arg_types[i], subst, tmpl.type_params)) {
                error(loc, "cannot deduce type arguments for '" + name +
                      "': argument " + std::to_string(i + 1) +
                      " does not match parameter type '" +
                      type_to_key(*params[i]) + "'");
                return nullptr;
            }
        }
    }

    std::vector<Expr*> bound;
    for (TypeParam* tp : tmpl.type_params) {
        auto it = subst.find(tp->name);
        if (it == subst.end() || !it->second) {
            error(loc, "cannot deduce type parameter '" +
                  std::string(tp->name) + "' of '" + name + "'");
            return nullptr;
        }
        bound.push_back(it->second);
    }

    std::string key = instance_key(bound);
    auto cached = generics_->cache.find({&tmpl, key});
    if (cached != generics_->cache.end())
        return static_cast<Func*>(cached->second);

    // 模板由别的模块 import 而来时不在本模块登记：作用域退回模块根，
    // 与实例挂到顶层声明后的发射位置一致
    Scope* scope = symbols_.root();
    bool pub = true;
    {
        GenericState::Template t;
        if (generic_template_of(&tmpl, t)) { scope = t.scope; pub = t.default_public; }
    }
    std::string iname = scope_path(scope) + name + key;
    if (scope && scope->lookup_member(iname)) {
        error(loc, "conflicting instantiation '" + iname + "'");
        return nullptr;
    }

    Func* inst = cu_.clone_func(&tmpl);
    substitute_types(inst, subst);
    inst->name = intern_name(std::move(iname));
    inst->type_params.clear();

    if (scope) {
        Symbol sym(SymbolKind::Function, inst);
        sym.specs = &inst->specs;
        sym.default_public = pub;
        sym.decl_scope = scope;
        if (scope->insert_member(std::string(inst->name), std::move(sym)))
            generics_->instance_scope[inst] = scope;
    }
    generics_->cache[{&tmpl, key}] = inst;
    generic_instances_.push_back(inst);
    analyze_instance(inst, scope);
    return inst;
}

// 泛型实参替换：克隆树里类型位置的形参名换成实例化后的类型
void SemanticAnalyzer::substitute_types(Decl* d, const TypeSubst& subst) {
    if (!d || subst.empty()) return;
    SubstCollector c;
    c.subst = &subst;
    c.walk(*d);
    for (auto& [node, repl] : c.hits) {
        if (!repl) continue;
        if (Expr* t = cu_.clone_type(repl)) *node = std::move(*t);
    }
    for (auto& [node, name] : c.value_hits) {
        error(node->loc, "type parameter '" + std::string(name) +
              "' cannot be used as a value");
    }
}

// 实例在模板的声明作用域里遍历，作用域链才与模板声明处一致。
// 遍历状态由基类各 walk_children 负责保存，这里补齐它们不覆盖的部分
void SemanticAnalyzer::analyze_instance(Decl* d, Scope* scope) {
    if (!d) return;
    Scope* saved_scope = symbols_.current_scope();
    Expr* saved_expr = current_expr;
    Stmt* saved_stmt = current_stmt;
    bool saved_callee = in_callee_;
    bool saved_base = in_generic_base_;
    if (scope) symbols_.reset_scope(scope);
    current_expr = nullptr;
    current_stmt = nullptr;
    in_callee_ = false;
    in_generic_base_ = false;
    walk(*d);
    current_expr = saved_expr;
    current_stmt = saved_stmt;
    in_callee_ = saved_callee;
    in_generic_base_ = saved_base;
    symbols_.reset_scope(saved_scope);
}

// 泛型声明的误用：类型位置缺实参、泛型函数以值引用。
// 作为被调用者时不报，那条路径由 resolve_call 按实参实例化
void SemanticAnalyzer::check_generic_use(Expr* e, Decl* d) {
    if (!e || in_generic_base_ || !is_generic_decl(d)) return;
    if (e->is_type) {
        error(e->loc, "'" + type_name_string(e) +
              "' is generic: write '" + type_name_string(e) + "-<...>' to use it");
        return;
    }
    if (in_callee_) return;
    error(e->loc, "generic function '" + std::string(d->name) +
          "' requires type arguments: call it as '" + std::string(d->name) +
          "-<...>(...)'");
}

} // namespace cx