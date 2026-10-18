#include "../include/semantic.h"
#include "../include/ast.h"
#include "../include/sem_types.h"
#include "../include/inject.h"
#include "../include/flow_check.h"
#include "../include/const_eval.h"

#include <functional>
#include <unordered_set>
#include <unordered_map>

namespace cx {
/*
semantic基础规则
有效表达式的type和sym在解析后保证至少有一个指针非空
检测到已有的空指针时不报错，在产生空指针时才报错
*/


void SemanticAnalyzer::error(const Location& loc, const std::string& msg) {
    diagnostics_.push_back(Diagnostic(DiagLevel::Error, msg, loc));
    has_error_ = true;
}

void SemanticAnalyzer::note(const Location& loc, const std::string& msg) {
    diagnostics_.push_back(Diagnostic(DiagLevel::Note, msg, loc));
}

void SemanticAnalyzer::warning(const Location& loc, const std::string& msg) {
    diagnostics_.push_back(Diagnostic(DiagLevel::Warning, msg, loc));
}

Type* SemanticAnalyzer::make_type(const Location& loc, Type::Variant v) {
    return cu_.add_node<Type>(loc, std::move(v));
}

Type* SemanticAnalyzer::make_named_type(const Location& loc, std::string_view name,
                                        Decl* decl, Scope* scope) {
    Type* t = cu_.add_node<Type>(loc, Type::Identifier{name});
    t->resolved_decl = decl;
    t->resolved_scope = scope;
    return t;
}

Expr* SemanticAnalyzer::make_int_literal(const Location& loc, size_t value) {
    literal_pool_.push_back(std::to_string(value));
    return cu_.add_node<Expr>(loc,
        Expr::Literal{LiteralKind::Int, literal_pool_.back()});
}

bool SemanticAnalyzer::analyze() {
    collect_only();
    analyze_rest();
    return !has_error_;
}

void SemanticAnalyzer::collect_only() {
    diagnostics_.clear();
    has_error_ = false;
    collect();
}

void SemanticAnalyzer::expand() {
    if (expanded_) return;
    expanded_ = true;
    process_using_namespaces();
    in_expansion_ = true;
    injector_->expand_all();
    in_expansion_ = false;
}

void SemanticAnalyzer::analyze_rest() {
    expand();
    // 泛型模板本体不进语义遍历与 IR：摘除后按使用点实例化
    collect_generics();
    // 多模块下前端已统一补分析过，这里兜住单模块直接调用 analyze() 的路径
    analyze_expand_instances();
    for (Decl* d : cu_.top_decls) collect_global_order(d);
    collect_extern_vars();
    walk(cu_);
    // 实例在创建时已就地分析，这里只把它们挂进顶层声明供 translator 使用
    for (Decl* d : generic_instances_) cu_.top_decls.push_back(d);
    report_infer_arrays();
    check_embedded_layout();
}

// extern 块内的变量由外部库定义，存储不在本模块，其类型不受模块级限制。
// extern 可以写在命名空间里，收集要逐层下钻，否则命名空间内的 extern 变量
// 会被当成模块级变量，走错模块级变量的检查分支
void SemanticAnalyzer::collect_extern_vars() {
    extern_vars_.clear();
    std::function<void(const std::vector<Decl*>&)> walk_containers =
        [&](const std::vector<Decl*>& members) {
            for (Decl* m : members) {
                if (!m) continue;
                if (m->kind == DeclKind::Extern) {
                    auto* ext = static_cast<Extern*>(m);
                    for (Decl* e : ext->members)
                        if (e && e->kind == DeclKind::VarList)
                            extern_vars_.emplace(e, ext->language());
                } else if (m->kind == DeclKind::Namespace) {
                    walk_containers(static_cast<Namespace*>(m)->members);
                }
            }
        };
    walk_containers(cu_.top_decls);
}

// 模块级变量按声明顺序编号：初值只能引用序号更小的变量
void SemanticAnalyzer::collect_global_order(Decl* d) {
    if (!d) return;
    if (d->kind == DeclKind::Namespace) {
        for (Decl* m : static_cast<Namespace*>(d)->members)
            collect_global_order(m);
        return;
    }
    if (d->kind != DeclKind::VarList) return;
    for (VarItem* vi : static_cast<VarList*>(d)->vars)
        if (vi) global_order_[vi] = global_order_.size();
}

std::string SemanticAnalyzer::build_callkey(const std::string& name,
    const std::vector<VarList*>& params) {
    std::string key = name + "_";
    for (const VarList* vl : params) {
        if (!vl) continue;
        for (const VarItem* item : vl->vars) {
            if (!item || !item->type || item->type->is_inferred()) {
                key += "_";
            } else {
                key += type_to_key(*item->type);
            }
        }
    }
    return key;
}

std::string SemanticAnalyzer::build_callkey(const std::string& name,
    const std::vector<Expr*>& args) {
    std::string key = name + "_";
    for (const auto* arg : args) {
        if (!arg || !arg->type || arg->type->is_inferred()) {
            key += "_";
        } else {
            key += type_to_key(*arg->type);
        }
    }
    return key;
}

std::string SemanticAnalyzer::build_conv_key(Func& f) {
    std::string pkey;
    for (VarList* vl : f.params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            if (vi && vi->type && !vi->type->is_inferred()) pkey += type_to_key(*vi->type);
            else pkey += "_";
        }
    }
    std::string rkey;
    if (!f.returns.empty() && f.returns.back()) {
        for (VarItem* ri : f.returns.back()->vars) {
            if (ri && ri->type) rkey += type_to_key(*ri->type);
        }
    }
    if (pkey.empty()) return "conversion_" + rkey;
    return "conversion_" + pkey + "_" + rkey;
}

void SemanticAnalyzer::collect() {
    // 顶层声明默认公开
    for (Decl* decl : cu_.top_decls) {
        collect_decl(decl, true);
    }
}

// using namespace：把命名空间成员并入全局；重复导入跳过，非命名空间报错
void SemanticAnalyzer::process_using_namespaces() {
    std::unordered_set<Decl*> seen;
    for (const auto& un : cu_.using_namespaces) {
        Type* path = un.path;
        if (!path) continue;
        std::string full = type_name_string(path);
        Scope* cur = symbols_.root();
        Symbol* sym = nullptr;
        size_t start = 0;
        bool bad = false;
        while (start <= full.size()) {
            size_t pos = full.find("::", start);
            std::string part = full.substr(start,
                pos == std::string::npos ? std::string::npos : pos - start);
            sym = SymbolTable::lookup_member_in(cur, part);
            if (!sym) { bad = true; break; }
            if (pos == std::string::npos) break;
            cur = sym->scope;
            if (!cur) { bad = true; break; }
            start = pos + 2;
        }
        if (bad || !sym) {
            error(un.loc, "unknown namespace '" + full + "'");
            continue;
        }
        if (sym->kind != SymbolKind::Namespace) {
            error(un.loc, "'" + full + "' is not a namespace");
            continue;
        }
        Decl* ns_decl = sym->get_decl();
        if (!ns_decl || !seen.insert(ns_decl).second) continue;  // 重复导入跳过
        Scope* ns_scope = sym->scope;
        if (!ns_scope) continue;
        // 已有名字优先，仅补空缺
        for (const auto& [name, m] : ns_scope->members) {
            if (symbols_.root()->members.find(name) == symbols_.root()->members.end())
                symbols_.root()->members[name] = m;
        }
    }
}

void SemanticAnalyzer::collect_decl(Decl* decl, bool default_public) {
    if (!decl) return;

    switch (decl->kind) {
        case DeclKind::VarList: {
            auto* vl = static_cast<VarList*>(decl);
            for (auto* var : vl->vars) {
                Symbol sym(SymbolKind::Variable, var);
                sym.specs = &vl->specs;
                sym.default_public = default_public;
                Symbol* s = symbols_.insert_member(std::string(var->name),
                                                   std::move(sym));
                if (!s) {
                    error(vl->loc, "duplicate declaration of variable '" + std::string(var->name) + "'");
                } else {
                    // 成员符号是字段的唯一身份：裸名与 `self.x` 必须解析到它，
                    // 否则同一字段会得到两个互不相干的 Symbol
                    var_symbols_[var] = s;
                }
            }
            break;
        }

        case DeclKind::Func:
        case DeclKind::Operator:
        case DeclKind::Conversion: {
            register_func(*static_cast<Func*>(decl), default_public);
            break;
        }

        case DeclKind::Class: {
            auto* cls = static_cast<Class*>(decl);
            std::string name(cls->name);
            Symbol* existing = symbols_.current_scope()
                ? symbols_.current_scope()->lookup_member(name) : nullptr;
            if (existing) {
                if (existing->kind == SymbolKind::Class || existing->kind == SymbolKind::Namespace)
                    error(cls->loc, "redefinition of class '" + name + "'");
                else
                    error(cls->loc, "cannot declare class '" + name + "' with same name as non-container");
                symbols_.push_scope(name);
            } else {
                Symbol* sym = symbols_.insert_and_push(name, Symbol(SymbolKind::Class, cls));
                if (sym) {
                    sym->specs = &cls->specs;
                    sym->default_public = default_public;
                    sym->node = cls;
                }
            }
            cls->scope = symbols_.current_scope();
            // 类成员与注入由 P1 处理
            symbols_.pop_scope();
            break;
        }

        case DeclKind::Namespace: {
            auto* ns = static_cast<Namespace*>(decl);
            std::string name(ns->name);
            Symbol* existing = symbols_.current_scope()
                ? symbols_.current_scope()->lookup_member(name) : nullptr;
            if (existing) {
                if (existing->kind == SymbolKind::Class || existing->kind == SymbolKind::Namespace)
                    error(ns->loc, "redefinition of namespace '" + name + "'");
                else
                    error(ns->loc, "cannot declare namespace '" + name + "' with same name as non-container");
                symbols_.push_scope(name);
            } else {
                Symbol* sym = symbols_.insert_and_push(name, Symbol(SymbolKind::Namespace, ns));
                if (sym) {
                    sym->specs = &ns->specs;
                    sym->default_public = default_public;
                    sym->node = ns;
                }
            }
            ns->scope = symbols_.current_scope();
            // 命名空间默认公开
            for (Decl* member : ns->members) {
                collect_decl(member, true);
            }
            symbols_.pop_scope();
            break;
        }

        case DeclKind::Extern: {
            auto* ext = static_cast<Extern*>(decl);
            ext->scope = symbols_.current_scope();
            for (Decl* member : ext->members) {
                collect_decl(member, default_public);
            }
            break;
        }

        case DeclKind::Enum: {
            auto* e = static_cast<Enum*>(decl);
            std::string ename(e->name);
            Scope* parent = symbols_.current_scope();
            Symbol* existing = parent ? parent->lookup_member(ename) : nullptr;
            if (existing) {
                error(e->loc, "redefinition of enum '" + ename + "'");
            }
            Scope* enum_scope = parent ? parent->get_or_create_child(ename) : nullptr;
            e->scope = enum_scope;
            if (parent && !existing) {
                Symbol sym(SymbolKind::Enum, e, enum_scope);
                sym.specs = &e->specs;
                sym.default_public = default_public;
                symbols_.insert_member(ename, std::move(sym));
            }
            // 取值在 visit_enum 里按声明顺序求，显式初值要看前面的成员
            for (EnumVariant* var : e->variants) {
                if (!var) continue;
                var->type = make_named_type(var->loc, e->name, e, enum_scope);
                if (enum_scope) {
                    Symbol var_sym(SymbolKind::EnumVar, var);
                    enum_scope->insert_member(std::string(var->name), std::move(var_sym));
                }
            }
            break;
        }

        case DeclKind::SpecialMethod: {
            register_special(*static_cast<SpecialMethod*>(decl), default_public);
            break;
        }

        case DeclKind::TypeDef:
        case DeclKind::ExprDef: {
            bool is_type = decl->kind == DeclKind::TypeDef;
            Symbol sym(is_type ? SymbolKind::TypeAlias : SymbolKind::ExprAlias, decl);
            sym.specs = &decl->specs;
            sym.default_public = default_public;
            if (!symbols_.insert_member(std::string(decl->name), std::move(sym))) {
                error(decl->loc, "redefinition of " +
                      std::string(is_type ? "typedef" : "exprdef") + " '" +
                      std::string(decl->name) + "'");
            }
            break;
        }
        default:
            break;
    }

}

void SemanticAnalyzer::register_func(Func& f, bool default_public) {
    std::string name = std::string(f.name);
    Scope* scope = symbols_.current_scope();

    // 操作符与转换按签名区分，其余函数（含 INIT）不允许重载
    bool keyed = (f.kind == DeclKind::Operator || f.kind == DeclKind::Conversion);
    std::string key;
    if (f.kind == DeclKind::Conversion) key = build_conv_key(f);
    else key = keyed ? build_callkey(name, f.params) : name;
    if (scope && scope->lookup_member(key)) {
        error(f.loc, "duplicate declaration of function '" + name + "'");
        return;
    }
    Symbol sym(SymbolKind::Function, &f);
    sym.specs = &f.specs;
    sym.default_public = default_public;
    symbols_.insert_member(key, std::move(sym));
}

void SemanticAnalyzer::register_special(SpecialMethod& sm, bool default_public) {
    // INIT 通过遍历类成员匹配参数，不进入符号表
    if (sm.kind == SpecialMethod::Kind::Init) return;
    std::string internal_name;
    switch (sm.kind) {
        case SpecialMethod::Kind::Init:       return;
        case SpecialMethod::Kind::Drop:       internal_name = "DROP";       break;
        case SpecialMethod::Kind::Ref:        internal_name = "REF";        break;
        case SpecialMethod::Kind::Deref:      internal_name = "DEREF";      break;
        default: error(sm.loc, "unknown special method"); return;
    }

    Scope* scope = symbols_.current_scope();
    if (scope && scope->lookup_member(internal_name)) {
        error(sm.loc, "duplicate special method '" + internal_name + "'");
        return;
    }

    Symbol sym(SymbolKind::Function, &sm);
    sym.specs = &sm.specs;
    sym.default_public = default_public;
    symbols_.insert_member(internal_name, std::move(sym));
}

std::string SemanticAnalyzer::type_to_key(Type& t) {
    // 枚举底层是数字：与 int 同键
    if (is_enum_type(&t)) return "i32";
    if (t.is_primitive()) {
        auto k = t.get_primitive()->kind;
        switch (k) {
            case PrimitiveTypeKind::Int8:    return "i8";
            case PrimitiveTypeKind::Short:   return "i16";
            case PrimitiveTypeKind::Int:     return "i32";
            case PrimitiveTypeKind::Long:    return "i64";
            case PrimitiveTypeKind::Int128:  return "i128";
            case PrimitiveTypeKind::UInt8:   return "u8";
            case PrimitiveTypeKind::UShort:  return "u16";
            case PrimitiveTypeKind::UInt:    return "u32";
            case PrimitiveTypeKind::ULong:   return "u64";
            case PrimitiveTypeKind::UInt128: return "u128";
            case PrimitiveTypeKind::Float16: return "f16";
            case PrimitiveTypeKind::Float:   return "f32";
            case PrimitiveTypeKind::Double:  return "f64";
            case PrimitiveTypeKind::Char:    return "char";
            case PrimitiveTypeKind::UChar:   return "uchar";
            case PrimitiveTypeKind::Bool:    return "bool";
            case PrimitiveTypeKind::Type:    return "type";
            case PrimitiveTypeKind::Void:    return "void";
            case PrimitiveTypeKind::Enum:    return "enum";
            default: error(t.loc, "unknown primitive type"); return "?";
        }
    }
    if (t.is_string_literal_type()) return "string";
    if (t.is_pointer()) {
        return type_to_key(*t.get_pointer()->pointee) + "*";
    }
    if (t.is_reference()) {
        return type_to_key(*t.get_reference()->target) + "&";
    }
    if (t.is_array_type()) {
        auto* arr = t.get_array_type();
        std::string elem_ir = arr->element ? type_to_key(*arr->element) : "i8";
        std::string len_ir = "x";
        if (arr->kind == ArrayKind::Slice) len_ir = "s";
        else if (arr->length && arr->length->is_literal())
            len_ir = std::string(arr->length->get_literal()->value);
        return "[" + elem_ir + ";" + len_ir + "]";
    }
    if (t.is_func_ptr()) {
        auto* f = t.get_func_ptr();
        std::string ret = f->return_type ? type_to_key(*f->return_type) : "void";
        std::string params;
        for (size_t i = 0; i < f->params.size(); ++i) {
            if (i > 0) params += ",";
            params += f->params[i] ? type_to_key(*f->params[i]) : "i8";
        }
        return ret + "(" + params + ")";
    }
    // 泛型类应用：还没有实例类型，按源码写法记，推导失败时诊断要看得懂
    if (t.is_generic_type()) {
        auto* g = t.get_generic_type();
        std::string args;
        for (size_t i = 0; i < g->args.size(); ++i) {
            Type* a = g->args[i];
            if (i > 0) args += ",";
            // 泛型形参在模板本体里是没解析的裸名，按原样写出
            if (a && a->is_named()) args += type_name_string(a);
            else args += a ? type_to_key(*a) : "?";
        }
        return type_name_string(g->base) + "-<" + args + ">";
    }
    if (t.is_named()) {
        return "%" + type_name_string(&t);
    }
    if (t.is_void()) return "void";
    if (t.is_null()) return "null";
    if (t.is_inferred()) {
        error(t.loc, "'let' is not implemented yet");
        return "_";
    }
    return "i8";
}

void SemanticAnalyzer::scope_in(std::string name) {
    symbols_.push_scope(std::move(name));
}
void SemanticAnalyzer::scope_out() {
    symbols_.pop_scope();
}
void SemanticAnalyzer::visit(CompUnit& cu) {
    for (auto* imp : cu.imports) {
        if (imp) {
            (void)imp;
        }
    }
}
void SemanticAnalyzer::visit(Decl&) {}
void SemanticAnalyzer::visit(Import&) {}
// T{} 是视图切片：所有权不外泄，故不出现在函数签名里；
// 跨函数传递数组一律用指针
void SemanticAnalyzer::check_no_slice(const std::vector<VarList*>& lists,
                                      const char* what) {
    for (VarList* vl : lists) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            if (!vi) continue;
            if (is_slice_type(vi->type ? vi->type : vl->type))
                error(vi->loc, std::string("array slice ") + what +
                      " is not allowed: a view must not escape its owner, "
                      "pass a pointer instead");
        }
    }
}

// extern 声明分两类约束。一类与外部 ABI 无关：符号由外部定义，本模块只有
// 声明，因此初值/默认实参无处附着，入口点也必须由 clux 自己给出。另一类来自
// C 的调用约定（数组按地址传、返回值的寄存器/结构体通道），只对 extern "C"
// 生效——库名块的调用约定尚未识别，不能拿 C 的规矩去套
void SemanticAnalyzer::check_extern_func(Func& f) {
    bool is_c = f.extern_lang == ExternLang::C;
    // 入口点由 clux 自己给出，声明成外部符号会让整个程序没有 main
    if (f.name == "main")
        error(f.loc, "'main' cannot be declared inside an extern block: the "
                     "program entry point must be defined in clux");
    for (VarList* vl : f.params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            if (!vi) continue;
            // C 的形参表按值传不到数组：声明成数组与 C 侧的按地址传对不上
            if (is_c) {
                Type* ty = vi->type ? vi->type : vl->type;
                if (ty && ty->is_array_type())
                    error(vi->loc, "array parameter is not allowed in an "
                                   "extern \"C\" declaration: C passes arrays "
                                   "by address, write a pointer");
            }
            if (vi->init)
                error(vi->loc, "default argument is not allowed in an extern "
                               "declaration: only clux code fills it in");
        }
    }
    if (f.returns.empty() || !f.returns.back()) return;
    VarList* rl = f.returns.back();
    Type* ty = nullptr;
    Location ty_loc = rl->loc;
    if (!rl->vars.empty()) {
        VarItem* rv = rl->vars.back();
        ty = rv->type ? rv->type : rl->type;
        ty_loc = rv->loc;
    } else {
        ty = rl->type;
    }
    if (!ty) return;
    // 聚合按值返回同样走寄存器通道：ABI 怎么收发由 LLVM 后端按目标平台
    // 决定（译者的类值是存储指针，跨 ABI 边界时才按结构体收发）
    bool ok = ty->get_primitive() || ty->get_pointer() ||
              ty->get_reference() || ty->get_func_ptr() || as_class(ty);
    if (ok) {
        f.ret_via_reg = true;
        return;
    }
    if (is_c)
        error(ty_loc, "unsupported extern \"C\" return type: an external C "
                      "function can only return a scalar, a pointer, a "
                      "reference or a class passed by value");
}

void SemanticAnalyzer::visit_func(Func& f) {
    // 变参仅用于 extern 声明
    if (f.is_variadic && !f.is_extern_decl())
        error(f.loc, "'...' is only allowed in extern declarations");
    check_no_slice(f.params, "parameter");
    check_no_slice(f.returns, "return type");
    // extern 内的函数声明不检查返回值是否被赋值；标量/指针返回按 C ABI 走寄存器
    if (f.is_extern_decl()) {
        check_extern_func(f);
        return;
    }
    // 引用返回：返回的是被引用对象的地址。返回变量只是把该地址带出去的
    // 通道，其存储仍在栈帧上，所以必须用寄存器返回；
    // 绑定源能不能返回出去由流检查判定
    if (!f.returns.empty() && f.returns.back()) {
        VarList* rl = f.returns.back();
        VarItem* rv = rl->vars.empty() ? nullptr : rl->vars.back();
        Type* ty = rv ? (rv->type ? rv->type : rl->type) : rl->type;
        if (ty && ty->is_reference()) f.ret_via_reg = true;
    }
    // body 为空（未写 {}）时不检查返回值是否被赋值
    if (!f.body) return;
    flow_.check_jumps(f.body);
    flow_.check_initialization(f.body, f.params, f.returns, false, current_class);
    if (flow_.can_use_register_ret(f)) f.ret_via_reg = true;
}

void SemanticAnalyzer::visit_special_method(SpecialMethod& sm) {
    check_no_slice(sm.params, "parameter");
    if (!sm.body) return;
    flow_.check_jumps(sm.body);
    flow_.check_initialization(sm.body, sm.params, {},
                               sm.kind == SpecialMethod::Kind::Init, current_class);
}

// 收集表达式引用的标识符符号（含成员访问），用于前向引用检测
static void collect_expr_syms(Expr* e, std::vector<Symbol*>& out) {
    if (!e) return;
    // 访问器与闭包体在各自的遍历上下文里已单独处理
    if (e->sym && e->sym->kind == SymbolKind::Variable) out.push_back(e->sym);
    if (auto* b = e->get_binary()) {
        collect_expr_syms(b->left, out);
        collect_expr_syms(b->right, out);
    } else if (auto* u = e->get_unary()) {
        collect_expr_syms(u->operand, out);
    } else if (auto* a = e->get_assign()) {
        collect_expr_syms(a->target, out);
        collect_expr_syms(a->value, out);
    } else if (auto* c = e->get_call()) {
        collect_expr_syms(c->callee, out);
        for (Expr* a : c->args) collect_expr_syms(a, out);
    } else if (auto* t = e->get_ternary()) {
        collect_expr_syms(t->cond, out);
        collect_expr_syms(t->then_expr, out);
        collect_expr_syms(t->else_expr, out);
    } else if (auto* i = e->get_index()) {
        collect_expr_syms(i->target, out);
        collect_expr_syms(i->index, out);
    } else if (auto* m = e->get_member()) {
        collect_expr_syms(m->target, out);
    } else if (auto* ca = e->get_cast()) {
        collect_expr_syms(ca->operand, out);
    } else if (auto* ar = e->get_array()) {
        for (Expr* m : ar->members) collect_expr_syms(m, out);
        collect_expr_syms(ar->fill_value, out);
    } else if (auto* sp = e->get_spawn()) {
        collect_expr_syms(sp->call, out);
    }
}

// 类是否声明了引用字段：这类对象被逐字段复制时，副本会与源共享被引用对象
static bool class_has_ref_field(Class* c) {
    if (!c) return false;
    for (Decl* m : c->members) {
        if (!m || m->kind != DeclKind::VarList) continue;
        for (VarItem* vi : static_cast<VarList*>(m)->vars)
            if (vi && vi->type && vi->type->is_reference()) return true;
    }
    return false;
}

// 该初始化/赋值是否走等号逐字段拷贝：源是已存在的同类对象，
// 且没有自转 conversion 接管（record_conversion 只在找到转换时挂 conv）
static bool copies_reference_fields(Expr* src, Type* dst) {
    if (!src || !dst || src->conv || src->implicit_to) return false;
    if (!src->is_lvalue) return false;
    if (!is_same_type(src->type, dst)) return false;
    return class_has_ref_field(as_class(dst));
}

// T[]：长度由初值推导，取自初值的编译期长度（字符串字面量按字符数）。
// 元素本身也可能是 T[]，按初值的首个元素继续推导。推导不出来时保持 Infer，
// 由 report_infer_arrays 统一报错
void SemanticAnalyzer::resolve_infer_array_len(Type& type, Type* init_type,
                                               Expr* init_expr) {
    Type::ArrayType* arr = type.get_array_type();
    if (!arr || arr->kind != ArrayKind::Infer || arr->length) return;
    long long n = init_expr ? const_array_len_of(init_expr) : -1;
    if (n < 0) n = const_array_type_len(init_type);
    if (n < 0) return;
    arr->length = make_int_literal(type.loc, (size_t)n);
    arr->kind = ArrayKind::Fixed;
    Type* elem_type = init_type && init_type->is_array_type()
        ? init_type->get_array_type()->element : nullptr;
    Expr* elem_expr = nullptr;
    if (init_expr && init_expr->is_array() &&
        !init_expr->get_array()->members.empty())
        elem_expr = init_expr->get_array()->members.front();
    if (arr->element)
        resolve_infer_array_len(*arr->element, elem_type, elem_expr);
}

// T[] 的长度只能来自初值，故报错措辞随声明位置不同：形参/返回值/字段没有
// 初值可用，写成切片也不是出路，只能给定长；只有局部变量能靠初值定长
void SemanticAnalyzer::mark_infer_site(Type* t, InferSite site) {
    if (!t) return;
    if (auto* arr = t->get_array_type()) {
        if (arr->kind == ArrayKind::Infer && !arr->length)
            infer_sites_[t] = site;
        mark_infer_site(arr->element, site);
        return;
    }
    if (auto* p = t->get_pointer()) mark_infer_site(p->pointee, site);
    else if (auto* r = t->get_reference()) mark_infer_site(r->target, site);
}

void SemanticAnalyzer::report_infer_arrays() {
    for (Type* t : pending_infer_arrays_) {
        if (!t || !t->is_array_type()) continue;
        auto* arr = t->get_array_type();
        if (arr->kind != ArrayKind::Infer || arr->length) continue;
        auto site = infer_sites_.find(t);
        InferSite s = site == infer_sites_.end() ? InferSite::Other
                                                  : site->second;
        switch (s) {
        case InferSite::Param:
            error(t->loc, "cannot infer the length of an array parameter: a "
                          "parameter has no initializer, write T[N]");
            break;
        case InferSite::Return:
            error(t->loc, "cannot infer the length of an array return type: "
                          "write T[N]");
            break;
        case InferSite::Field:
            error(t->loc, "cannot infer the length of an array field: write T[N]");
            break;
        case InferSite::Local:
            error(t->loc, "cannot infer array length: give it an initializer of "
                          "known length, or write T{} and initialize it for a "
                          "length known only at run time");
            break;
        case InferSite::Other:
            error(t->loc, "cannot infer array length: no initializer here, "
                          "write T[N] with an explicit length");
            break;
        }
    }
    pending_infer_arrays_.clear();
    infer_sites_.clear();
}

// extern 变量的存储在外部库，本模块只发一条 external global 声明，
// 故初值无处附着。类型约束则来自 C 的对象布局：clux 的类值是存储指针，
// 把类类型的 extern 变量按指针发出去，读写的其实是另一块内存——这类约束
// 只对 extern "C" 生效，库名块的布局尚未识别
void SemanticAnalyzer::check_extern_var_list(VarList& vl, ExternLang lang) {
    bool is_c = lang == ExternLang::C;
    for (VarItem* var : vl.vars) {
        if (!var) continue;
        if (var->init)
            error(var->loc, "an extern variable cannot have an initializer: its "
                            "storage belongs to the external library");
        if (!is_c) continue;
        Type* vt = var->type ? var->type : vl.type;
        if (!vt) continue;
        if (vt->is_inferred()) {
            error(var->loc, "the type of an extern \"C\" variable cannot be "
                            "inferred: the C layout is fixed, write it out");
        } else if (as_class(vt)) {
            error(var->loc, "an extern \"C\" variable cannot be a class: the "
                            "value of a class is a pointer to its storage, "
                            "while the external symbol is the object itself, "
                            "use a pointer");
        } else if (is_slice_type(vt)) {
            error(var->loc, "an extern \"C\" variable cannot be an array slice: "
                            "its length is owned by the local binding, not by "
                            "the external symbol");
        } else if (!(vt->is_primitive() || vt->is_pointer() ||
                     vt->is_func_ptr() || vt->is_array_type())) {
            // 外部符号是一个存储位置，只有布局与 C 侧对象一一对应的类型能对上
            error(var->loc, "unsupported type for an extern \"C\" variable: "
                            "only scalars, pointers, function pointers and "
                            "fixed-length arrays match an external storage "
                            "location");
        }
    }
}

void SemanticAnalyzer::visit_var_list(VarList& vl) {
    // 模块级变量的存储是 LLVM 全局量，其类型必须是常量可表达的
    auto ext = extern_vars_.find(&vl);
    bool in_extern = ext != extern_vars_.end();
    bool module_level = !current_params && !current_members && !in_extern;
    if (in_extern) {
        check_extern_var_list(vl, ext->second);
    }
    if (module_level) {
        // 初值在 main 入口按声明顺序求值，引用后方声明的变量只会读到零值
        for (VarItem* var : vl.vars) {
            if (!var || !var->init) continue;
            auto self = global_order_.find(var);
            if (self == global_order_.end()) continue;
            std::vector<Symbol*> refs;
            collect_expr_syms(var->init, refs);
            for (Symbol* sym : refs) {
                auto* rvi = dynamic_cast<VarItem*>(sym->node);
                if (!rvi) continue;
                auto other = global_order_.find(rvi);
                if (other != global_order_.end() && other->second > self->second)
                    error(var->init->loc,
                          "initializer of module-level variable '" +
                          std::string(var->name) + "' refers to '" +
                          std::string(rvi->name) + "', which is declared later");
            }
        }
    }
    if (module_level && (vl.type == nullptr || !vl.type->is_inferred())) {
        // 无初值的模块级数组/字符串是零初始化的全局量，存储合法；只有
        // 带数组/字符串字面量初值时才无法常量求值（常量解释器尚未实现），
        // 暂予禁止
        for (VarItem* var : vl.vars) {
            if (!var || !var->init) continue;
            Type* dt = var->type ? var->type : vl.type;
            bool init_aggregate = var->init->is_array() ||
                (var->init->type &&
                 var->init->type->is_string_literal_type());
            bool decl_aggregate = dt && (dt->is_array_type() ||
                                         dt->is_string_literal_type());
            // 标记bug
                                         if (init_aggregate || decl_aggregate)
                error(var->loc, "module-level variable cannot be an array or "
                      "string: its storage would not outlive the initializer");
        }
    }
    // 初始化类型检查：显式声明类型时，初值须可转换到声明类型（let 推导跳过）
    for (VarItem* var : vl.vars) {
        if (!var) continue;
        Type* dt = var->type ? var->type : vl.type;
        // T[]：长度由初值推导。多个变量共用一个类型节点时按声明顺序取第一个
        // 能定出长度的初值，各变量长度不一致由数组字面量长度检查报出
        if (dt && dt->is_array_type()) {
            resolve_infer_array_len(*dt, var->init ? var->init->type : nullptr,
                                    var->init);
            InferSite site = InferSite::Local;
            if (is_param_item(var)) site = InferSite::Param;
            else if (is_return_var_item(var)) site = InferSite::Return;
            else if (is_field_item(var)) site = InferSite::Field;
            mark_infer_site(dt, site);
        }
        // T{} / T{n}：长度必须在声明处确定，否则没有可分配的存储。
        // 形参/返回变量/字段本来就不许是切片，由各自的位置检查报错，不在此重复
        if (!is_param_item(var) && !is_return_var_item(var) &&
            !is_field_item(var) && is_slice_type(dt) &&
            !dt->get_array_type()->length &&
            !(var->init && var->init->type &&
              (var->init->type->is_array_type() ||
               var->init->type->is_string_literal_type())))
            error(var->loc, "cannot determine the length of this slice: give "
                            "it an initializer of known length, or write T{n}");

        // 引用的绑定源必须是同目标类型的 lvalue；绑定不走常规值转换。
        // 引用可以延迟绑定：声明处不写初值，第一次赋值才绑定，
        // 绑定前不得读取、绑定后不可再换对象，由流检查判定。
        // 形参在进入函数时由实参绑定，字段在 INIT 中绑定，二者都不在声明处。
        // 没有初值就是延迟绑定：绑定点是本方法体里的第一次赋值，
        // 绑定前不得读取、绑定后不可再换对象，都由流检查判定
        if (dt && dt->is_reference()) {
            if (is_return_var_item(var)) continue;
            if (var->init) check_ref_init_source(*var, *dt);
            continue;
        }
        if (!var->init || !var->init->type) continue;
        if (!dt || dt->is_inferred()) continue;
        // 返回变量不接受数组/字符串字面量：栈存储返回后悬垂
        bool lit_slice = var->init->is_array() ||
            (var->init->type && var->init->type->is_string_literal_type());
        if (lit_slice && is_return_var_item(var)) {
            error(var->init->loc,
                  "cannot initialize return variable with array literal: "
                  "its stack storage would dangle after return");
        }
        // 数组字面量：元素数不得超过声明的编译期常量长度
        if (dt->is_array_type() && var->init->is_array()) {
            check_array_len_fit(var->init, dt, "declared", var->init->loc);
        } else if (dt->is_array_type() && var->init->type &&
                   var->init->type->is_string_literal_type()) {
            // 字符串字面量初值：只检查长度，拷贝交给 translator
            check_array_len_fit(var->init, dt, "declared", var->init->loc);
        }
        check_literal_range(var->init, dt, "initialization", var->init->loc);
        // 同类也允许 self->self conversion，故先尝试记录。
        // record_conversion 对同类 lvalue 一律返回 true（拷贝语义），
        // 是否真找到自转要看它有没有挂上 conv
        bool converted = record_conversion(var->init, dt);
        if (copies_reference_fields(var->init, dt))
            warning(var->init->loc,
                    "initializing '" + type_name_string(dt) +
                    "' copies its reference fields, so the two objects share "
                    "the referenced object");
        if (converted) continue;
        if (is_same_type(var->init->type, dt)) continue;
        check_conversion(var->init->type, dt, "initialization", var->init->loc);
    }
    for (VarItem* var : vl.vars) {
        if (!var->type) var->type = vl.type;
        // 字段的身份是 collect 阶段登记的类成员符号，裸名与 `self.x` 都解析到它；
        // 再登记一个同名局部量只会让同一个字段分裂成两个 Symbol
        if (is_field_item(var)) continue;
        Symbol sym(SymbolKind::Variable, var);
        sym.specs = &vl.specs;
        sym.default_public = true;
        if (!symbols_.insert_local(var->name, std::move(sym))) {
            error(var->loc, "duplicate variable '" + std::string(var->name) + "'");
        } else {
            var_symbols_[var] = symbols_.current_->lookup_local(var->name);
        }
    }
}

// 声明处初值的绑定检查：初值即绑定点，绑定源必须是同目标类型的对象，
// 绑定不走常规值转换
void SemanticAnalyzer::check_ref_init_source(const VarItem& var, const Type& dt) {
    Expr* init = var.init;
    if (!init->type) return;
    if (init->type->is_reference()) {
        // 初值本身已是引用（如引用返回值的接收）：沿用它的绑定即可，
        // 调用结果不是 lvalue，但绑定对象确已存在
        Type* it = init->type->get_reference()->target;
        Type* tgt = dt.get_reference()->target;
        if (tgt && it && !is_same_type(it, tgt))
            error(init->loc, "reference target type mismatch");
        return;
    }
    // 绑定源写成 &lvalue 时等价于绑定该 lvalue
    Expr* src = init->bind_source();
    if (!src->is_lvalue) {
        error(init->loc, "reference must be initialized with an lvalue");
        return;
    }
    Type* tgt = dt.get_reference()->target;
    if (tgt && !tgt->is_reference() && !is_same_type(src->type, tgt))
        error(init->loc, "reference target type mismatch");
}

// 类里是否声明了 INIT：没有 INIT 的类只能走零参默认构造，没有字段赋值点
static bool class_has_init(const Class& c) {
    for (Decl* m : c.members) {
        if (!m || m->kind != DeclKind::SpecialMethod) continue;
        if (static_cast<SpecialMethod*>(m)->kind == SpecialMethod::Kind::Init)
            return true;
    }
    return false;
}

// 当前类的字段按名字取回：字段的身份是 collect 阶段登记的类成员符号
Symbol* SemanticAnalyzer::class_field_symbol(std::string_view name) const {
    if (!current_class || !current_class->scope) return nullptr;
    Symbol* m = SymbolTable::lookup_member_in(current_class->scope,
                                              std::string(name));
    return (m && m->kind == SymbolKind::Variable) ? m : nullptr;
}

// inner 是否在 outer 之内（含自身）
static bool scope_within(Scope* inner, Scope* outer) {
    for (Scope* s = inner; s; s = s->parent) {
        if (s == outer) return true;
    }
    return false;
}

// 引用字段必须在 INIT 中绑定：字段初始化器与 static 字段都没有可用的绑定点，
// INIT 嵌套在条件里的赋值也不保证执行
void SemanticAnalyzer::visit_class(Class& c) {
    for (Decl* m : c.members) {
        if (!m || m->kind != DeclKind::VarList) continue;
        auto* vl = static_cast<VarList*>(m);
        bool is_static = vl->specs.has(SpecKind::Static);
        for (VarItem* vi : vl->vars) {
            if (!vi) continue;
            // 字段的存储大小要参与类布局，切片的长度运行期才知道
            if (is_slice_type(vi->type ? vi->type : vl->type))
                error(vi->loc, "array slice field '" + std::string(vi->name) +
                      "' is not allowed: the size of the class would not be "
                      "computable");
            // 按值内嵌的类字段参与类布局，登记下来供 check_embedded_layout 查环
            if (Class* fc = as_class(vi->type ? vi->type : vl->type)) {
                if (!embed_.count(&c)) embed_order_.push_back(&c);
                embed_[&c].emplace_back(fc, vi);
            }
            if (!vi->type || !vi->type->is_reference()) {
                // const 字段只在声明处的初值或 INIT 里赋值一次，二者都没有
                // 就没有任何赋值点：类一旦建成就再也写不进去
                if (vl->specs.has(SpecKind::Const) && !vi->init) {
                    std::string f = "const field '" + std::string(vi->name) + "'";
                    if (is_static) {
                        error(vi->loc, f + " cannot be static: its storage is "
                               "initialized before any INIT runs, so give it "
                               "an initializer");
                    } else if (!class_has_init(c)) {
                        error(vi->loc, f + " must be assigned in INIT or by an "
                               "initializer, but the class has no INIT");
                    }
                }
                continue;
            }
            std::string f = "reference field '" + std::string(vi->name) + "'";
            if (is_static) {
                error(vi->loc, f + " cannot be static: a reference field can "
                      "only be bound in INIT");
                continue;
            }
            // 绑定点是字段初值，或 INIT 里的赋值：类一旦建成就再也不有绑定点，
            // 两者都没有、连 INIT 都没有的类连绑定的地方都没有
            if (!vi->init && !class_has_init(c))
                error(vi->loc, f + " must be bound by an initializer or by an "
                      "assignment in INIT, but the class has neither");
        }
    }
}

// 类字段按值内嵌使类型定义彼此嵌套：自身内联或相互内联时既算不出实例大小，
// 也生成不出合法的布局，只能报错要求改用指针
bool SemanticAnalyzer::embed_visit(Class* c, std::unordered_map<Class*, int>& state) {
    state[c] = 1;
    auto it = embed_.find(c);
    if (it != embed_.end()) {
        for (auto& [fc, vi] : it->second) {
            if (state[fc] == 1) {
                error(vi->loc, "class field of type '" + std::string(fc->name) +
                      "' is embedded in itself: a class cannot contain itself "
                      "by value, use a pointer instead");
                return false;
            }
            if (state[fc] == 0 && !embed_visit(fc, state)) return false;
        }
    }
    state[c] = 2;
    return true;
}

void SemanticAnalyzer::check_embedded_layout() {
    std::unordered_map<Class*, int> state;
    for (Class* c : embed_order_) {
        if (state[c]) continue;
        if (!embed_visit(c, state)) return;
    }
}

void SemanticAnalyzer::visit_namespace(Namespace&) {}
void SemanticAnalyzer::visit_extern(Extern&) {}
// 枚举成员按声明顺序定值：显式初值先语义分析再常量求值，
// 故只能引用已定值的前序成员；缺省初值沿用前一个值加一
void SemanticAnalyzer::visit_enum(Enum& e) {
    std::unordered_map<std::string_view, bool> seen;
    int64_t next_value = 0;
    for (EnumVariant* v : e.variants) {
        if (!v) continue;
        std::string vname(v->name);
        if (!seen.emplace(v->name, true).second) {
            error(v->loc, "redefinition of enum variant '" + vname + "'");
        }
        int64_t val = next_value;
        if (v->init) {
            walk(*v->init);
            auto cv = eval_const(v->init, const_sink_);
            if (!cv || !cv->is_int()) {
                error(v->loc, "enum variant '" + vname + "' value must be a "
                              "compile-time integer constant; only variants "
                              "declared earlier can be referenced");
            } else {
                val = cv->i64();
            }
        }
        // 枚举在 IR 里是 i32，初值装不下须报错而不是截断
        if (val < INT32_MIN || val > INT32_MAX) {
            error(v->loc, "enum variant '" + vname + "' value out of range for 'int'");
        }
        v->value = val;
        v->has_value = true;
        next_value = val + 1;
    }
    if (e.variants.empty()) {
        warning(e.loc, "enum '" + std::string(e.name) + "' declares no variants");
    }
}
void SemanticAnalyzer::visit_keep(Keep&) {}

void SemanticAnalyzer::visit(Expr&) {}

void SemanticAnalyzer::visit_expr_literal(Expr::Literal& lit) {
    // IntLiteral 既可能是无后缀的数，也可能是显式 i32；十六进制数字不含 i，
    // 靠字面量文本里是否出现 i/I 区分。UIntLiteral 只可能来自显式 u 后缀。
    bool explicit_ty = (lit.kind != LiteralKind::Int);
    for (char c : lit.value)
        if (c == 'i' || c == 'I') { explicit_ty = true; break; }

    if (!explicit_ty) {
        // 无后缀：类型由值决定。优先有符号，装不下才退到无符号与 128 位，
        // 变参位置因此拿到与 C 一致的宽度。
        std::string dec;
        if (!parse_int_literal(lit.value, dec)) {
            error(current_expr->loc,
                  "malformed integer literal " + std::string(lit.value));
            current_expr->type = typeInt;
            current_expr->is_lvalue = false;
            return;
        }
        if      (dec_cmp(dec, int_max_text(true,  32)) <= 0) current_expr->type = typeInt;
        else if (dec_cmp(dec, int_max_text(true,  64)) <= 0) current_expr->type = typeLong;
        else if (dec_cmp(dec, int_max_text(false, 64)) <= 0) current_expr->type = typeULong;
        else if (dec_cmp(dec, int_max_text(true,  128)) <= 0) current_expr->type = typeInt128;
        else if (dec_cmp(dec, int_max_text(false, 128)) <= 0) current_expr->type = typeUInt128;
        else error(current_expr->loc, "integer literal " + std::string(lit.value) +
                      " is out of range");
        if (!current_expr->type) current_expr->type = typeInt;
        current_expr->is_lvalue = false;
        return;
    }

    switch(lit.kind) {
        #define X(l,name) case LiteralKind::name: current_expr->type = type##name; break;
        LITERAL_MAPPING
        #undef X
    }
    // 带显式类型后缀的字面量按后缀定类型，再校验值装不装得下
    bool ls = false; int lb = 0;
    if (literal_int_range(lit.kind, ls, lb)) {
        std::string dec;
        if (!parse_int_literal(lit.value, dec)) {
            error(current_expr->loc,
                  "malformed integer literal " + std::string(lit.value));
        } else if (dec_cmp(dec, int_max_text(ls, lb)) > 0) {
            error(current_expr->loc, "integer literal " + std::string(lit.value) +
                  " is out of range for its type");
        }
    }
    current_expr->is_lvalue = false;
}

void SemanticAnalyzer::visit_expr_null(Expr::Null&) {
    current_expr->type = typeNull;
    current_expr->is_lvalue = false;
}

void SemanticAnalyzer::visit_expr_identifier(Expr::Identifier& id) {
    // self：当前实例本身，类型为自身的值类型
    if (current_class && id.name == "self") {
        current_expr->type = make_named_type(current_expr->loc, current_class->name,
                                             current_class, current_class->scope);
        current_expr->is_lvalue = true;
        return;
    }
    Symbol* sym = symbols_.lookup_local(id.name);
    // 字段只登记为类成员，裸名查找看不到它。类作用域里的字段比外层作用域的
    // 同名量更近，方法体里的局部量则仍按词法作用域优先
    if (current_class && current_class->scope &&
        (!sym || !scope_within(sym->decl_scope, current_class->scope))) {
        if (Symbol* field = class_field_symbol(id.name)) sym = field;
    }
    // 块内的 exprdef 与局部变量同一套查找，先判别名再走变量路径
    if (sym && sym->kind == SymbolKind::ExprAlias) {
        expand_expr_alias(sym);
        return;
    }
    if (sym) {
        current_expr->sym = sym;
        Type* st = sym->get_type();
        // 读取引用变量即读取被引用对象：隐式解引用为其目标类型
        if (st && st->is_reference()) st = st->get_reference()->target;
        current_expr->type = st;
        current_expr->is_lvalue = (sym->kind == SymbolKind::Variable);
        return;
    }

    Symbol* member = symbols_.lookup_member(std::string(id.name));
    if (member) {
        if (member->kind == SymbolKind::ExprAlias) {
            check_access(member, current_expr->loc);
            expand_expr_alias(member);
            return;
        }
        if (member->kind == SymbolKind::TypeAlias) {
            error(current_expr->loc,
                  "typedef '" + std::string(id.name) + "' used as a value");
            return;
        }
        current_expr->sym = member;
        check_access(member, current_expr->loc);
        if (member->kind == SymbolKind::Variable) {
            // 读取引用字段即读取被绑定对象：隐式解引用为其目标类型
            Type* mt = member->get_type();
            if (mt && mt->is_reference()) mt = mt->get_reference()->target;
            current_expr->type = mt;
            current_expr->is_lvalue = true;
        } else if (member->kind == SymbolKind::Function) {
            if (Decl* d = member->get_decl()) {
                if (d->kind == DeclKind::Func || d->kind == DeclKind::Operator)
                    current_expr->type = make_type(current_expr->loc,
                        Type::FuncRef{static_cast<Func*>(d)}
                    );
                // 泛型函数以值引用没有可确定的签名（作为被调用者除外）
                check_generic_ref_use(current_expr, d);
            }
            current_expr->is_lvalue = false;
        } else if (member->kind == SymbolKind::Enum) {
            if (Enum* e = member->get<Enum>()) {
                current_expr->type = make_named_type(
                    current_expr->loc, e->name, e, e->scope);
            }
            current_expr->is_lvalue = false;
        } else {
            current_expr->type = nullptr;
            current_expr->is_lvalue = false;
        }
        return;
    }

    // 作为调用被调用者：函数只以 mangled key 存表，此处无法解析，
    // 交给 resolve_call 用标识符名反查，避免误报未定义标识符
    if (in_callee_) {
        current_expr->sym = nullptr;
        current_expr->type = nullptr;
        current_expr->is_lvalue = false;
        return;
    }

    error(current_expr->loc, "undefined identifier '" + std::string(id.name) + "'");
}

Symbol* SemanticAnalyzer::find_operator_overload(OpKind op, Type& type, Type& other,
                                                 Type** matched_param) {
    std::string opstr = std::string(opkind_to_sv(op));
    std::string otherkey = type_to_key(other);
    Symbol* sym = symbols_.lookup_member(opstr + "_" + type_to_key(type) + otherkey);
    if (sym) return sym;
    if (!type.resolved_decl || type.resolved_decl->kind != DeclKind::Class) return nullptr;
    Scope* scope = static_cast<Class*>(type.resolved_decl)->scope;
    if (!scope) return nullptr;
    Symbol* cls_sym = scope->lookup_member(opstr + "_" + otherkey);
    if (cls_sym) return cls_sym;

    // 退化匹配：精确未命中时，扫描同类操作符，找 other 可转换过去的形参
    std::string prefix = opstr + "_";
    for (auto& [key, member] : scope->members) {
        if (key.size() <= prefix.size() || key.compare(0, prefix.size(), prefix) != 0)
            continue;
        if (member.kind != SymbolKind::Function) continue;
        auto* opd = dynamic_cast<Operator*>(member.get_decl());
        if (!opd || opd->op != op) continue;
        if (opd->params.empty() || !opd->params[0]) continue;
        VarList* pl = opd->params[0];
        if (pl->vars.empty() || !pl->vars[0]) continue;
        Type* pt = pl->vars[0]->type ? pl->vars[0]->type : pl->type;
        if (!pt) continue;
        if (can_convert_to(&other, pt)) {
            if (matched_param) *matched_param = pt;
            return &member;
        }
    }
    return nullptr;
}

Symbol* SemanticAnalyzer::find_operator_overload(OpKind op, Type& type) {
    std::string opstr = std::string(opkind_to_sv(op));
    Symbol* sym = symbols_.lookup_member(opstr + "_" + type_to_key(type));
    if (sym) return sym;
    if (!type.resolved_decl || type.resolved_decl->kind != DeclKind::Class) return nullptr;
    Scope* scope = static_cast<Class*>(type.resolved_decl)->scope;
    if (!scope) return nullptr;
    Symbol* cls_sym = scope->lookup_member(opstr + "_");
    return cls_sym;
}



// 判断作用域 s 是否位于 root -> ... -> current 的路径上
bool SemanticAnalyzer::is_scope_on_path(Scope* s) const {
    if (!s) return false;
    for (Scope* cur = symbols_.current_scope(); cur; cur = cur->parent) {
        if (cur == s) return true;
    }
    return false;
}

// 读访问控制：离开声明作用域后 priv 不可读，readonly 仍可读
void SemanticAnalyzer::check_access(Symbol* sym, const Location& loc) {
    if (!sym) return;
    Access acc = sym->access_level();
    if (acc == Access::Public) return;
    if (is_scope_on_path(sym->decl_scope)) return;
    if (acc == Access::Private) {
        error(loc, "cannot access private member '" + sym_display_name(sym) + "'");
    }
}

// 写访问控制：离开声明作用域后 priv / readonly 均不可写
void SemanticAnalyzer::check_write_access(Symbol* sym, const Location& loc) {
    if (!sym) return;
    // const 不可二次写入：方法体内的赋值由流检查按路径判定，第一次赋值是
    // 初始化；方法体之外（形参默认值、字段与模块级初值）没有这样的流，
    // 对 const 的赋值必定是二次写入
    if (sym->is_const()) {
        if (!current_body)
            error(loc, "cannot assign to const '" + sym_display_name(sym) + "'");
        return;
    }
    Access acc = sym->access_level();
    if (acc == Access::Public) return;
    if (is_scope_on_path(sym->decl_scope)) return;
    // priv 在取值时就报错了，这里不必重复
    if (acc == Access::Readonly) {
        error(loc, "cannot assign to readonly member '" + sym_display_name(sym) + "'");
    }
}


bool SemanticAnalyzer::is_return_var_item(VarItem* vi) const {
    if (!vi || !current_func) return false;
    if (current_func->returns.empty() || !current_func->returns.back())
        return false;
    for (VarItem* rvi : current_func->returns.back()->vars)
        if (rvi == vi) return true;
    return false;
}

bool SemanticAnalyzer::is_param_item(VarItem* vi) const {
    if (!vi || !current_params) return false;
    for (VarList* vl : *current_params) {
        if (!vl) continue;
        for (VarItem* pvi : vl->vars)
            if (pvi == vi) return true;
    }
    return false;
}


void SemanticAnalyzer::visit_expr_binary(Expr::Binary& b) {
    Type* lt = b.left ? b.left->type : nullptr;
    Type* rt = b.right ? b.right->type : nullptr;
    if (!lt || !rt) return;

    // 非同包装类的混合运算：解包包装类一侧，走基础运算
    Class* lw = wrap_class_of(lt);
    Class* rw = wrap_class_of(rt);
    if (!(lw && rw && lw == rw) && (lw || rw)) {
        if (lw) {
            Type* base_t = wrap_base_type(lw);
            record_conversion(b.left, base_t);
            lt = base_t;
        }
        if (rw) {
            Type* base_t = wrap_base_type(rw);
            record_conversion(b.right, base_t);
            rt = base_t;
        }
    }

    // 枚举底层是数字：参与运算时归一为 int
    if (is_enum_type(lt)) lt = typeInt;
    if (is_enum_type(rt)) rt = typeInt;

    Type* matched_param = nullptr;
    if (Symbol* s = find_operator_overload(b.op, *lt, *rt, &matched_param)) {
        Type* overload_ret = typeVoid;
        if (auto* op = s->get<Operator>()) {
            if (!op->returns.empty() && op->returns.back() && op->returns.back()->type)
                overload_ret = op->returns.back()->type;
        }
        // 退化匹配命中的形参类型与实参不一致时记录隐式转换
        if (matched_param && b.right && b.right->type &&
            !is_same_type(b.right->type, matched_param))
            record_conversion(b.right, matched_param);
        current_expr->type = overload_ret;
        current_expr->sym = s;
        current_expr->is_lvalue = false;
        return;
    }

    switch (b.op) {
        case OpKind::Plus:
        case OpKind::Minus:
        case OpKind::Mul:
        case OpKind::Div:
        case OpKind::Mod: {
            if (is_numeric_type(lt) && is_numeric_type(rt)) {
                if (is_float_type(lt) || is_float_type(rt)) {
                    if (is_float_type(lt) && is_float_type(rt)) {
                        current_expr->type =
                            float_rank(lt->get_primitive()->kind) >=
                            float_rank(rt->get_primitive()->kind) ? lt : rt;
                    } else {
                        current_expr->type = is_float_type(lt) ? lt : rt;
                    }
                } else {
                    current_expr->type =
                        integer_rank(lt->get_primitive()->kind) >=
                        integer_rank(rt->get_primitive()->kind) ? lt : rt;
                }
            } else if (b.op == OpKind::Plus && (lt->is_string_literal_type() || rt->is_string_literal_type())) {
                current_expr->type = typeString;
            } else {
                error(current_expr->loc, "operator not applicable to given types");
                current_expr->type = lt;
            }
            current_expr->is_lvalue = false;
            break;
        }

        case OpKind::Less:
        case OpKind::Greater:
        case OpKind::LessEqual:
        case OpKind::GreaterEqual:
        case OpKind::Equal:
        case OpKind::NotEqual:
        case OpKind::And:
        case OpKind::Or: {
            current_expr->type = typeBool;
            current_expr->is_lvalue = false;
            break;
        }

        case OpKind::BitAnd:
        case OpKind::BitOr:
        case OpKind::BitXor:
        case OpKind::Shl:
        case OpKind::Shr: {
            if (is_integer_type(lt) && is_integer_type(rt)) {
                current_expr->type = lt;
            } else {
                error(current_expr->loc, "bitwise operator requires integer operands");
                current_expr->type = lt;
            }
            current_expr->is_lvalue = false;
            break;
        }

        default:
            error(current_expr->loc, "invalid binary operator");
            current_expr->type = nullptr;
            break;
    }
}

void SemanticAnalyzer::visit_expr_unary(Expr::Unary& u) {
    Type* ot = u.operand ? u.operand->type : nullptr;

    if (!ot) return;
    // 枚举底层是数字：一元运算时归一为 int
    if (is_enum_type(ot)) ot = typeInt;

    if (Symbol* s = find_operator_overload(u.op, *ot)) {
        if (auto* op = s->get<Operator>()) {
            if (!op->returns.empty() && op->returns.back() && op->returns.back()->type) {
                current_expr->type = op->returns.back()->type;
            } else {
                current_expr->type = typeVoid;
            }
            current_expr->is_lvalue = false;
            current_expr->sym = s;
            return;
        }
    }

    switch (u.op) {
        case OpKind::Minus:
        case OpKind::Plus: {
            if (is_numeric_type(ot)) {
                current_expr->type = ot;
            } else {
                error(current_expr->loc, "unary +/- requires numeric operand");
                current_expr->type = ot;
            }
            current_expr->is_lvalue = false;
            break;
        }
        case OpKind::Not: {
            current_expr->type = typeBool;
            current_expr->is_lvalue = false;
            break;
        }
        case OpKind::BitNot: {
            if (is_integer_type(ot)) {
                current_expr->type = ot;
            } else {
                error(current_expr->loc, "bitwise NOT requires integer operand");
                current_expr->type = ot;
            }
            current_expr->is_lvalue = false;
            break;
        }
        // Mul在单元运算符中出现实际指-*情况，这是不规范的，但是先标记着之后再改，下面BitAnd同理
        case OpKind::Mul: {
            if (ot->is_pointer()) {
                current_expr->type = ot->get_pointer()->pointee;
                current_expr->is_lvalue = true;
            } else {
                error(current_expr->loc, "dereference requires pointer operand");
                current_expr->type = ot;
                current_expr->is_lvalue = false;
            }
            break;
        }
        case OpKind::BitAnd: {
            if (u.operand && u.operand->is_lvalue) {
                current_expr->type = make_type(current_expr->loc, Type::Pointer{ot});
                current_expr->is_lvalue = false;
            } else {
                error(current_expr->loc, "address-of requires lvalue operand");
                current_expr->type = ot;
                current_expr->is_lvalue = false;
            }
            break;
        }
        default:
            error(current_expr->loc, "invalid unary operator");
            current_expr->type = nullptr;
            break;
    }
}

void SemanticAnalyzer::visit_expr_assign(Expr::Assign& a) {
    if (!a.target || !a.value) return;

    if (!a.target->is_lvalue) {
        error(current_expr->loc, "assignment target is not a modifiable lvalue");
    }

    // 写访问控制：离开作用域后 priv / readonly 均不可写
    if (a.target->sym) {
        check_write_access(a.target->sym, current_expr->loc);
    }

    Type* lt = a.target->type;
    // 引用目标的赋值写入被引用对象，按被引用类型判定
    if (a.target->sym) {
        Type* dt = a.target->sym->get_type();
        if (dt && dt->is_reference()) lt = dt->get_reference()->target;
    }
    if (lt && lt->is_reference()) lt = lt->get_reference()->target;
    Type* rt = a.value->type;

    // 复合赋值操作符重载优先（可能具备与对应二元操作符不同的独特语义）
    if (a.op != OpKind::Assign && lt && rt) {
        Type* matched = nullptr;
        if (Symbol* s = find_operator_overload(a.op, *lt, *rt, &matched)) {
            if (matched && !is_same_type(rt, matched))
                record_conversion(a.value, matched);
            current_expr->sym = s;
            current_expr->type = lt;
            current_expr->is_lvalue = false;
            return;
        }
    }

    // 返回变量不接受数组/字符串字面量：字面量存储留在函数栈上，返回后指针悬垂
    bool literal_slice = a.value->is_array() ||
        (a.value->type && a.value->type->is_string_literal_type());
    if (literal_slice && a.target->sym) {
        VarItem* tvi = dynamic_cast<VarItem*>(a.target->sym->node);
        if (is_return_var_item(tvi)) {
            error(a.value->loc,
                  "cannot assign array literal to return variable: "
                  "its stack storage would dangle after return");
        }
    }
    // 赋值统一走 = 的转换判定
    check_array_len_fit(a.value, lt, "target", current_expr->loc);
    check_literal_range(a.value, lt, "assignment", current_expr->loc);
    // 记录隐式 conversion / 包装转换，供 translator 派发；同类也允许 self->self
    if (rt && lt) {
        bool converted = record_conversion(a.value, lt);
        // 落到等号逐字段拷贝时（无自转 conversion），含引用字段的类会让
        // 副本与源共享被引用对象
        if (copies_reference_fields(a.value, lt))
            warning(current_expr->loc,
                    "assigning '" + type_name_string(lt) +
                    "' copies its reference fields, so the two objects share "
                    "the referenced object");
        if (converted) {
            current_expr->type = lt;
            current_expr->is_lvalue = false;
            return;
        }
    }
    check_conversion(rt, lt, "assignment", current_expr->loc);

    current_expr->type = lt;
    current_expr->is_lvalue = false;
}

void SemanticAnalyzer::visit_expr_call(Expr::Call& c) {
    if (!c.callee) {
        current_expr->type = nullptr;
        return;
    }

    resolve_call(current_expr, c);
}


void SemanticAnalyzer::visit_expr_index(Expr::Index& idx) {
    if (!idx.target) {
        current_expr->type = nullptr;
        return;
    }

    Type* tt = idx.target->type;
    if (!tt) {
        current_expr->type = nullptr;
        return;
    }

    Type* it = idx.index ? idx.index->type : nullptr;
    if (it) {
        if (Symbol* s = find_operator_overload(OpKind::Index, *tt, *it)) {
            Type* ret = typeVoid;
            if (auto* op = s->get<Operator>()) {
                if (!op->returns.empty() && op->returns.back() && op->returns.back()->type)
                    ret = op->returns.back()->type;
            }
            current_expr->type = ret;
            current_expr->is_lvalue = ref_is_lvalue(ret);
            current_expr->sym = s;
            return;
        }
    }

    if (tt->is_array_type()) {
        current_expr->type = tt->get_array_type()->element;
        current_expr->is_lvalue = true;
    } else if (tt->is_pointer()) {
        current_expr->type = tt->get_pointer()->pointee;
        current_expr->is_lvalue = true;
    } else {
        error(current_expr->loc, "cannot index non-array/non-pointer type");
        current_expr->type = nullptr;
        current_expr->is_lvalue = false;
    }
}

void SemanticAnalyzer::visit_expr_member(Expr::Member& m) {
    if (!m.target) {
        current_expr->sym = nullptr;
        current_expr->type = nullptr;
        current_expr->is_lvalue = false;
        return;
    }

    // 数组/切片的 .len：返回长度，长度非负故为 uint
    if (m.member == "len" && m.target->type && m.target->type->is_array_type()) {
        current_expr->sym = nullptr;
        current_expr->type = typeUInt;
        current_expr->is_lvalue = false;
        return;
    }

    // 数组/切片的 .ptr：提取底层指针（指向元素类型）
    if (m.member == "ptr" && m.target->type && m.target->type->is_array_type()) {
        Type* elem = m.target->type->get_array_type()->element;
        current_expr->sym = nullptr;
        current_expr->type = make_type(current_expr->loc, Type::Pointer{elem});
        current_expr->is_lvalue = false;
        return;
    }

    // malloc 实例的内置 free：a.free() 释放堆实例
    if (m.member == "free" && m.target->type) {
        if (is_malloc_value(m.target)) {
            current_expr->sym = nullptr;
            current_expr->type = typeVoid;
            current_expr->is_lvalue = false;
            return;
        }
    }

    Scope* target_scope = nullptr;
    // 优先从表达式类型解析目标作用域，sym 仅作兜底
    if (m.target->type) {
        target_scope = named_type_scope(m.target->type);
    }
    if (!target_scope && m.target->sym &&
        (m.target->sym->kind == SymbolKind::Class ||
         m.target->sym->kind == SymbolKind::Namespace ||
         m.target->sym->kind == SymbolKind::Enum)) {
        target_scope = m.target->sym->scope;
    }

    if (!target_scope) {
        error(current_expr->loc, "cannot access member on non-class/non-namespace type");
        current_expr->type = nullptr;
        current_expr->is_lvalue = false;
        return;
    }
    
    std::string field = std::string(m.member);

    Symbol* member = SymbolTable::lookup_member_in(target_scope, field);
    if (member) {
        current_expr->sym = member;
        check_access(member, current_expr->loc);

        if (member->kind == SymbolKind::Variable && member->get_type()) {
            // 读取引用字段即读取被绑定对象：隐式解引用为其目标类型
            Type* ft = member->get_type();
            if (ft->is_reference()) ft = ft->get_reference()->target;
            current_expr->type = ft;
            current_expr->is_lvalue = true;
            return;
        }
        if (member->kind == SymbolKind::Function) {
            if (Decl* d = member->get_decl()) {
                if (d->kind == DeclKind::Func || d->kind == DeclKind::Operator)
                    current_expr->type = make_type(
                        current_expr->loc,
                        Type::FuncRef{static_cast<Func*>(d)}
                    );
            }
            // 目标是一个类，取到的就是它的实例方法：实际签名多一个接收者，
            // decay 成声明的函数指针类型后调用会走错。作为被调用者不受影响，
            // static 方法没有接收者，可以取地址
            if (!in_callee_ && as_class(m.target->type) &&
                !(member->specs && member->specs->has(SpecKind::Static))) {
                error(current_expr->loc,
                      "cannot take the address of method '" + field +
                      "': a method has no standalone value without its receiver");
            }
            current_expr->is_lvalue = false;
            return;
        }
        if (member->kind == SymbolKind::EnumVar) {
            if (auto* ev = member->get<EnumVariant>()) {
                current_expr->type = ev->type;
            }
            current_expr->is_lvalue = false;
            return;
        }
        current_expr->is_lvalue = false;
        return;
    }

    error(current_expr->loc, "no member '" + field + "' in scope");
    current_expr->type = nullptr;
    current_expr->is_lvalue = false;
}

bool SemanticAnalyzer::is_field_item(VarItem* vi) const {
    if (!vi || !current_members) return false;
    for (Decl* d : *current_members) {
        if (!d || d->kind != DeclKind::VarList) continue;
        for (VarItem* fvi : static_cast<VarList*>(d)->vars)
            if (fvi == vi) return true;
    }
    return false;
}

void SemanticAnalyzer::visit_expr_cast(Expr::Cast& cast) {
    if (!cast.target_type) {
        current_expr->type = nullptr;
        return;
    }
    current_expr->type = cast.target_type;
    current_expr->is_lvalue = false;

    if (cast.operand && cast.operand->type) {
        Type* src = cast.operand->type;
        Type* dst = cast.target_type;
        if (is_same_type(src, dst)) {
            // 枚举与整数等价，int(enum) 是合法写法，不算多余 cast
            if (!is_enum_type(src) && !is_enum_type(dst))
                warning(current_expr->loc, "unnecessary cast, types are identical");
        } else {
            // cast 是显式转换，走 cast 的转换判定
            check_conversion(src, dst, "cast", current_expr->loc, true);
        }
    }
}

void SemanticAnalyzer::visit_expr_array(Expr::Array& arr) {
    if (arr.members.empty()) {
        // 填充形式 [x ...] 的长度只能由目标类型给出，自身长度未知
        Type* elem = arr.fill_value && arr.fill_value->type
            ? arr.fill_value->type : typeInt;
        current_expr->type = make_type(current_expr->loc,
            arr.fill_value
                ? Type::ArrayType{elem, nullptr, ArrayKind::Infer}
                : Type::ArrayType{elem, make_int_literal(current_expr->loc, 0)});
        current_expr->is_lvalue = false;
        return;
    }

    Type* elem_type = nullptr;
    for (auto* member : arr.members) {
        if (!member) continue;
        if (!elem_type) {
            elem_type = member->type;
        } else if (member->type && !is_same_type(elem_type, member->type)) {
            warning(current_expr->loc, "heterogeneous array literal, using first element type");
        }
    }

    current_expr->type = make_type(current_expr->loc,
        Type::ArrayType{elem_type ? elem_type : typeInt,
                    make_int_literal(current_expr->loc, arr.members.size())});
    current_expr->is_lvalue = false;
}

void SemanticAnalyzer::visit_expr_ternary(Expr::Ternary& t) {
    Type* ct = t.cond ? t.cond->type : nullptr;
    if (ct && !is_bool_type(ct)) {
        warning(t.cond->loc, "condition in ternary expression is not bool");
    }

    Type* tt = t.then_expr ? t.then_expr->type : nullptr;
    Type* et = t.else_expr ? t.else_expr->type : nullptr;

    if (tt && et) {
        if (is_same_type(tt, et)) {
            current_expr->type = tt;
        } else {
            warning(current_expr->loc, "then and else branches have different types");
            current_expr->type = tt;
        }
    } else {
        current_expr->type = tt ? tt : et;
    }

    current_expr->is_lvalue = false;
}

void SemanticAnalyzer::visit(Stmt&) {}
void SemanticAnalyzer::visit_stmt_block(Stmt::Block&) {}
void SemanticAnalyzer::visit_stmt_expr(Stmt::ExprStmt&) {}

void SemanticAnalyzer::visit_stmt_if(Stmt::If& if_stmt) {
    if (!if_stmt.condition) {
        error(current_stmt->loc, "if statement missing condition");
        return;
    }
    
    Type* cond_type = if_stmt.condition->type;
    if (cond_type && wrap_class_of(cond_type)) {
        record_conversion(if_stmt.condition, typeBool);
    } else if (cond_type && !is_condition_type(cond_type)) {
        warning(if_stmt.condition->loc, "if condition is not bool, implicit conversion");
    }
}

void SemanticAnalyzer::visit_stmt_while(Stmt::While& while_stmt) {
    if (!while_stmt.condition) {
        error(current_stmt->loc, "while statement missing condition");
        return;
    }
    
    Type* cond_type = while_stmt.condition->type;
    if (cond_type && wrap_class_of(cond_type))
        record_conversion(while_stmt.condition, typeBool);
    if (cond_type && !is_condition_type(cond_type)) {
        warning(while_stmt.condition->loc, "while condition is not bool, implicit conversion");
    }
}

void SemanticAnalyzer::visit_stmt_for(Stmt::For& for_stmt) {
    if (for_stmt.condition) {
        Type* cond_type = for_stmt.condition->type;
        if (cond_type && !is_condition_type(cond_type)) {
            warning(for_stmt.condition->loc, "for condition is not bool, implicit conversion");
        }
    }
}

void SemanticAnalyzer::visit_stmt_return(Stmt::Return&) {
    if (!current_func) {
        error(current_stmt->loc, "return statement outside of function");
        return;
    }
}

void SemanticAnalyzer::visit_stmt_break(Stmt::Break&) {
    if (loop_depth_ == 0) {
        error(current_stmt->loc, "break statement not inside a loop");
    }
}

void SemanticAnalyzer::visit_stmt_continue(Stmt::Continue&) {
    if (loop_depth_ == 0) {
        error(current_stmt->loc, "continue statement not inside a loop");
    }
}

void SemanticAnalyzer::visit_stmt_label(Stmt::Label&) {}
void SemanticAnalyzer::visit_stmt_goto(Stmt::Goto&) {}
void SemanticAnalyzer::visit_stmt_asm(Stmt::Asm&) {}
void SemanticAnalyzer::visit_stmt_keep(Stmt::Keep&) {}
void SemanticAnalyzer::visit_stmt_var_decl(Stmt::VarDecl&) {}

// 函数体内的 typedef / exprdef：登记进当前作用域，之后的代码才看得到
void SemanticAnalyzer::visit_stmt_def(Stmt::Def& d) {
    Decl* decl = d.decl;
    if (!decl) return;
    if (symbols_.current_scope() &&
        symbols_.current_scope()->lookup_local(decl->name)) {
        error(decl->loc, "redefinition of " +
              std::string(decl->kind == DeclKind::TypeDef ? "typedef" : "exprdef") +
              " '" + std::string(decl->name) + "'");
        return;
    }
    Symbol sym(decl->kind == DeclKind::TypeDef
        ? SymbolKind::TypeAlias : SymbolKind::ExprAlias, decl);
    symbols_.insert_local(decl->name, std::move(sym));
}
// spawn(call)：类型继承被调函数的返回类型，禁止传入栈上的指针或引用
void SemanticAnalyzer::visit_expr_spawn(Expr::Spawn& sp) {
    if (!sp.call) {
        current_expr->type = nullptr;
        current_expr->is_lvalue = false;
        return;
    }
    current_expr->type = sp.call->type ? sp.call->type : typeVoid;
    current_expr->is_lvalue = false;

    if (auto* c = sp.call->get_call()) {
        // 引用形参按地址传递，被引用对象可能是栈变量，故同样禁止
        std::vector<Type*> params;
        if (c->resolved) {
            if (c->resolved->kind == DeclKind::Func ||
                c->resolved->kind == DeclKind::Operator)
                params = param_types(static_cast<Func*>(c->resolved)->params);
            else if (c->resolved->kind == DeclKind::SpecialMethod)
                params = param_types(static_cast<SpecialMethod*>(c->resolved)->params);
        }
        for (size_t i = 0; i < c->args.size(); ++i) {
            Expr* arg = c->args[i];
            if (!arg) continue;
            Type* pt = i < params.size() ? params[i] : nullptr;
            if (pt && pt->is_reference()) {
                error(arg->loc, "spawn cannot capture a stack reference argument");
            } else if (arg->type && arg->type->is_pointer()) {
                error(arg->loc, "spawn cannot capture a stack pointer argument");
            }
        }
    }
}

// void 是合法返回类型/指针 pointee：解析器将 void 生成 Primitive{Void}，
// 无法从节点本身判断上下文，故不在此处拒绝
void SemanticAnalyzer::visit_type_primitive(Type::Primitive&) {}
// ast本来应该有指针指向的节点如果是nullptr，说明已经在parser报过错了，不要重复报错
// 引用不可寻址，指针不能指向引用：否则 *p 该返回引用还是被引用对象无从判定
void SemanticAnalyzer::visit_type_pointer(Type::Pointer& p) {
    if (p.pointee && p.pointee->is_reference())
        error(current_type->loc, "cannot form a pointer to a reference");
}
// 编译期常量长度：经常量求值器折叠为整数字面量；
// 返回 nullptr 表示不是编译期整数常量
Expr* SemanticAnalyzer::const_length_literal(Expr* len, int depth) {
    if (!len || depth > 16) return nullptr;
    auto v = eval_const(len, const_sink_);
    if (!v || !v->is_int()) return nullptr;
    return make_int_literal(len->loc, (size_t)v->u);
}

void SemanticAnalyzer::visit_type_array(Type::ArrayType& a) {
    // 数组元素没有逐个绑定的语法，元素类型为引用则无法初始化
    if (a.element && a.element->is_reference())
        error(current_type->loc, "cannot form an array of references");
    // T{}：长度可以在 {n} 里给出（运行期），不参与常量校验
    if (a.kind == ArrayKind::Slice) {
        // {n} 里的长度能折叠成常量就折叠，运行期的长度原样保留
        if (a.length) {
            if (Expr* lit = const_length_literal(a.length)) a.length = lit;
        }
        return;
    }
    // T[]：长度待推导，推导不出来时由 report_infer_arrays 统一报错
    if (!a.length) {
        if (a.kind == ArrayKind::Infer) pending_infer_arrays_.push_back(current_type);
        return;
    }
    Expr* len = const_length_literal(a.length);
    if (!len) {
        error(current_type->loc,
              "array length must be a compile-time integer constant; "
              "use T{n} for a slice whose length is only known at run time");
        return;
    }
    a.length = len;
}
void SemanticAnalyzer::visit_type_func_ptr(Type::FuncPtr& fp) {
    // 函数指针与函数签名同规则：所有权不外泄，形参与返回类型都不许切片
    for (Type* p : fp.params) {
        if (is_slice_type(p))
            error(p->loc, "array slice parameter is not allowed in a function "
                          "pointer type: pass a pointer instead");
        mark_infer_site(p, InferSite::Param);
    }
    if (is_slice_type(fp.return_type))
        error(fp.return_type->loc, "array slice return type is not allowed in a "
                                   "function pointer type: pass a pointer instead");
    mark_infer_site(fp.return_type, InferSite::Return);
}
void SemanticAnalyzer::visit_type_func_ref(Type::FuncRef&) {}
// 引用不可重新绑定，引用不能再包引用
void SemanticAnalyzer::visit_type_reference(Type::Reference& r) {
    if (r.target && r.target->is_reference())
        error(current_type->loc, "reference to reference");
}

void SemanticAnalyzer::visit_type_string_literal(Type::StringLiteralType&) {
    // StringLiteral is a valid type, no additional checks needed
}
Symbol* SemanticAnalyzer::lookup_member_on_type(Type* type, const std::string& field, Scope** found_scope) {
    if (found_scope) *found_scope = nullptr;
    if (!type) return nullptr;
    Scope* s = named_type_scope(type);
    if (found_scope) *found_scope = s;
    return s ? s->lookup_member(field) : nullptr;
}

void SemanticAnalyzer::visit_type_void(Type::Void&) {
    // Void is valid as a return type or pointer pointee context, checked by callers
}

void SemanticAnalyzer::visit_type_inferred(Type::Inferred&) {}

// typedef 使用点：节点内容换成目标类型的克隆件，再按新形态遍历一遍，
// 克隆件内部的别名随之继续展开。def 在 collect 登记符号、在此处按序展开，
// 所以函数体里的 typedef 一样只对其后的代码生效
void SemanticAnalyzer::expand_type_alias(Symbol* sym) {
    TypeDef* td = sym->get<TypeDef>();
    if (!td || !td->target) return;
    if (def_depth_ >= kMaxDefDepth) {
        error(current_type->loc, "circular typedef '" + std::string(td->name) + "'");
        current_type->data = Type::Inferred{};
        return;
    }
    Type* copy = cu_.clone_type(td->target);
    Location use_loc = current_type->loc;
    *current_type = std::move(*copy);
    current_type->loc = use_loc;
    ++def_depth_;
    walk_children(*current_type);
    --def_depth_;
}

// exprdef 使用点：同上，节点内容换成目标表达式的克隆件
void SemanticAnalyzer::expand_expr_alias(Symbol* sym) {
    ExprDef* ed = sym->get<ExprDef>();
    if (!ed || !ed->target) return;
    if (def_depth_ >= kMaxDefDepth) {
        error(current_expr->loc, "circular exprdef '" + std::string(ed->name) + "'");
        current_expr->data = Expr::Inferred{};
        current_expr->type = nullptr;
        current_expr->is_lvalue = false;
        return;
    }
    Expr* copy = cu_.clone_expr(ed->target);
    Location use_loc = current_expr->loc;
    current_expr->data = std::move(copy->data);
    current_expr->loc = use_loc;
    ++def_depth_;
    walk_children(*current_expr);
    --def_depth_;
}

// 类型位置的裸名：按类型解析
void SemanticAnalyzer::visit_type_identifier(Type::Identifier& id) {
    std::string name(id.name);
    // 块内的 typedef 只在其后生效，与局部变量同一套查找
    Symbol* s = symbols_.lookup_local(name);
    if (!s) s = symbols_.lookup_member(name);
    if (s && s->kind == SymbolKind::TypeAlias) {
        check_access(s, current_type->loc);
        expand_type_alias(s);
        return;
    }
    if (s && s->kind == SymbolKind::ExprAlias) {
        error(current_type->loc,
              "exprdef '" + name + "' used as a type");
        return;
    }
    if (s && (s->kind == SymbolKind::Class ||
              s->kind == SymbolKind::Namespace ||
              s->kind == SymbolKind::Enum)) {
        current_type->sym = s;
        current_type->resolved_decl = s->get_decl();
        current_type->resolved_scope = s->scope;
        // 泛型模板本体不参与语义，裸用（未给类型实参）在此拦下
        check_generic_use(current_type, s->get_decl());
    } else {
        error(current_type->loc, "unknown type '" + name + "'");
    }
}

// 类型位置的限定名：A::B 按类型路径解析
void SemanticAnalyzer::visit_type_member(Type::Member& m) {
    Scope* scope = m.target ? m.target->resolved_scope : nullptr;
    Symbol* tsym = scope
        ? scope->lookup_member(std::string(m.member)) : nullptr;
    if (tsym && (tsym->kind == SymbolKind::Class ||
                 tsym->kind == SymbolKind::Namespace ||
                 tsym->kind == SymbolKind::Enum)) {
        current_type->sym = tsym;
        current_type->resolved_decl = tsym->get_decl();
        current_type->resolved_scope = tsym->scope;
        // 泛型模板本体不参与语义，裸用（未给类型实参）在此拦下
        check_generic_use(current_type, tsym->get_decl());
    } else {
        error(current_type->loc,
              "unknown type '" + type_name_string(current_type) + "'");
    }
}

void SemanticAnalyzer::visit_type_null(Type::Null&) {}

// 泛型类应用：实参全在源码里给出，就地实例化并把节点换成指向实例的具名类型
void SemanticAnalyzer::visit_type_generic(Type::GenericType& g) {
    Type* base = g.base;
    Decl* tmpl = base ? (base->sym ? base->sym->get_decl() : base->resolved_decl)
                      : nullptr;
    if (tmpl && tmpl->kind == DeclKind::Class && is_generic_decl(tmpl)) {
        if (Class* inst = instantiate_class(*static_cast<Class*>(tmpl), g.args,
                                            current_type->loc)) {
            *current_type = std::move(*instance_type_node(base, inst));
            return;
        }
        // instantiate 已报出具体原因
        return;
    }
    error(current_type->loc, "'" + type_name_string(base) +
          "' is not a generic class");
}

// 值位置的泛型应用：泛型类的实例类型；泛型函数按显式类型实参实例化后
// 就能作值（函数指针、赋值等）。没有类型实参就无从推导，
// 裸用模板的误用由 check_generic_ref_use 报出
void SemanticAnalyzer::visit_expr_generic(Expr::GenericType& g) {
    Expr* base = g.base;
    Decl* tmpl = base ? (base->sym ? base->sym->get_decl() : nullptr) : nullptr;
    if (tmpl && tmpl->kind == DeclKind::Class && is_generic_decl(tmpl)) {
        if (Class* inst = instantiate_class(*static_cast<Class*>(tmpl), g.args,
                                            current_expr->loc)) {
            // 实例是具名类型（类）或函数引用：把节点整体换成指向实例的名字
            Expr* ref = instance_ref_node(base, inst);
            current_expr->loc = ref->loc;
            current_expr->data = std::move(ref->data);
            current_expr->type = ref->type;
            current_expr->sym = ref->sym;
            return;
        }
        // instantiate 已报出具体原因
        current_expr->type = nullptr;
        current_expr->is_lvalue = false;
        return;
    }
    // 值位置的泛型函数：按显式类型实参实例化后就能作值（函数指针、赋值等）。
    // 作为被调用者时留给 resolve_call：那里还拿得到实参类型，
    // 推导与成员方法的 self 都要靠它。方法实例不带接收者，visit_expr_member
    // 已经拦下取地址，不走这条路
    if (!in_callee_ && base && (base->is_identifier() || base->is_member()) &&
        tmpl && tmpl->kind == DeclKind::Func && is_generic_decl(tmpl) &&
        !g.args.empty()) {
        if (Func* inst = instantiate_func(*static_cast<Func*>(tmpl), g.args, {},
                                          current_expr->loc)) {
            // 函数实例作值：类型是函数引用，decay 成函数指针靠它
            Expr* ref = instance_ref_node(base, inst);
            current_expr->loc = ref->loc;
            current_expr->data = std::move(ref->data);
            current_expr->type = ref->type;
            current_expr->sym = ref->sym;
            return;
        }
        // instantiate 已报出具体原因
        current_expr->type = nullptr;
        current_expr->is_lvalue = false;
        return;
    }
    current_expr->sym = base ? base->sym : nullptr;
    current_expr->type = base ? base->type : nullptr;
}

// main 的合法形式：参数为空或 (int, char**)，返回为空或 int
void SemanticAnalyzer::check_main() {
    Scope* root = symbols_.root();
    Symbol* sym = root ? root->lookup_member("main") : nullptr;
    if (!sym || sym->kind != SymbolKind::Function) return;
    Func* f = sym->get<Func>();
    if (!f) return;

    std::vector<Type*> params;
    for (VarList* vl : f->params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            if (vi && vi->type) params.push_back(vi->type);
        }
    }
    bool params_ok = params.empty();
    if (!params_ok && params.size() == 2) {
        params_ok = type_to_key(*params[0]) == "i32"
                 && type_to_key(*params[1]) == "char**";
    }
    if (!params_ok) {
        error(f->loc, "invalid 'main' signature: parameters must be empty or '(int, char**)'");
    }

    std::vector<Type*> returns;
    for (VarList* vl : f->returns) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            if (vi && vi->type) returns.push_back(vi->type);
        }
    }
    bool returns_ok = returns.empty()
        || (returns.size() == 1 && type_to_key(*returns.back()) == "i32");
    if (!returns_ok) {
        error(f->loc, "invalid 'main' signature: return must be empty or 'int'");
    }
}

// extern 函数声明的 ABI 签名键：只看类型与是否变参，形参名与修饰不进 ABIs
static std::string extern_sig_key(SemanticAnalyzer* sa, Func& f) {
    std::string k = f.is_variadic ? "V(" : "F(";
    Type* ret = nullptr;
    if (!f.returns.empty() && f.returns.back()) {
        VarList* rl = f.returns.back();
        VarItem* rv = rl->vars.empty() ? nullptr : rl->vars.back();
        ret = rv ? (rv->type ? rv->type : rl->type) : rl->type;
    }
    k += ret ? sa->type_to_key(*ret) : "void";
    for (VarList* vl : f.params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            if (!vi) continue;
            Type* pt = vi->type ? vi->type : vl->type;
            k += ",";
            k += pt ? sa->type_to_key(*pt) : "?";
        }
    }
    k += ")";
    return k;
}

void SemanticAnalyzer::check_extern_abi_conflicts(
        const std::vector<SemanticAnalyzer*>& mods) {
    struct Seen {
        SemanticAnalyzer* mod;
        Func* fn;
        std::string name;
        std::string key;
    };
    std::unordered_map<std::string, Seen> first;
    std::vector<std::pair<Seen, Seen>> conflicts;

    for (SemanticAnalyzer* sa : mods) {
        if (!sa) continue;
        // extern 可以写在命名空间里，逐层下钻；按声明顺序遍历，诊断顺序稳定
        std::function<void(const std::vector<Decl*>&)> walk_containers =
            [&](const std::vector<Decl*>& members) {
                for (Decl* d : members) {
                    if (!d) continue;
                    if (d->kind == DeclKind::Namespace) {
                        walk_containers(static_cast<Namespace*>(d)->members);
                        continue;
                    }
                    if (d->kind != DeclKind::Extern) continue;
                    for (Decl* m : static_cast<Extern*>(d)->members) {
                        if (!m || m->kind != DeclKind::Func) continue;
                        auto* f = static_cast<Func*>(m);
                        std::string name(f->name);
                        std::string key = extern_sig_key(sa, *f);
                        auto it = first.find(name);
                        if (it == first.end()) {
                            first.emplace(name, Seen{sa, f, name, key});
                        } else if (it->second.key != key) {
                            conflicts.emplace_back(it->second,
                                                  Seen{sa, f, name, key});
                        }
                    }
                }
            };
        walk_containers(sa->cu_.top_decls);
    }

    for (const auto& [prev, cur] : conflicts) {
        cur.mod->error(cur.fn->loc,
                       "conflicting declarations of the external symbol '" +
                       cur.name + "': the signature differs from the one "
                       "declared at " + std::string(prev.fn->loc.filename) +
                       ":" + std::to_string(prev.fn->loc.lin) +
                       ", so the two modules would call it by different "
                       "conventions");
    }
}

} // namespace cx
