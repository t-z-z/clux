#include "../include/semantic.h"
#include "ast.h"
#include "sem_types.h"
#include "inject.h"
#include "flow_check.h"

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

Expr* SemanticAnalyzer::make_type(const Location& loc, Expr::Variant v) {
    return cu_.add_node<Expr>(loc, std::move(v));
}

Expr* SemanticAnalyzer::make_named_type(const Location& loc, std::string_view name,
                                        Decl* decl, Scope* scope) {
    Expr* t = cu_.add_node<Expr>(loc, Expr::Identifier{name});
    t->is_type = true;
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

void SemanticAnalyzer::analyze_rest() {
    process_using_namespaces();
    injector_->expand_all();
    for (Decl* d : cu_.top_decls) collect_global_order(d);
    walk(cu_);
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
        Expr* path = un.path;
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
                if (!symbols_.insert_member(std::string(var->name), std::move(sym))) {
                    error(vl->loc, "duplicate declaration of variable '" + std::string(var->name) + "'");
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
            int64_t next_value = 0;
            for (EnumVariant* var : e->variants) {
                if (!var) continue;
                var->value = next_value++;
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

std::string SemanticAnalyzer::type_to_key(Expr& t) {
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
        if (arr->length && arr->length->is_literal())
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
    if (t.is_type && (t.is_identifier() || t.is_member())) {
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
void SemanticAnalyzer::visit_func(Func& f) {
    // 变参仅用于 extern 声明
    if (f.is_variadic) {
        if (!f.is_extern_decl)
            error(f.loc, "'...' is only allowed in extern declarations");
        for (VarList* vl : f.params) {
            if (!vl) continue;
            for (VarItem* vi : vl->vars) {
                Expr* ty = vi->type ? vi->type : vl->type;
                if (ty && ty->is_array_type())
                    error(vi->loc, "slice parameter is not allowed in a variadic function");
            }
        }
    }
    // extern 内的函数声明不检查返回值是否被赋值；标量/指针返回按 C ABI 走寄存器
    if (f.is_extern_decl) {
        if (!f.returns.empty() && f.returns.back()) {
            VarList* rl = f.returns.back();
            Expr* ty = nullptr;
            Location ty_loc = rl->loc;
            if (!rl->vars.empty()) {
                VarItem* rv = rl->vars.back();
                ty = rv->type ? rv->type : rl->type;
                ty_loc = rv->loc;
            } else {
                ty = rl->type;
            }
            if (ty) {
                bool ok = ty->get_primitive() || ty->get_pointer() ||
                          ty->get_reference() || ty->get_func_ptr();
                if (ok) f.ret_via_reg = true;
                else error(ty_loc, "extern 函数不支持聚合返回类型");
            }
        }
        return;
    }
    // 引用返回类型：和 C++ 一样只能返回左值，返回的是被引用对象的别名。
    // 返回变量的存储随栈帧消失，绑定它没有意义，故直接拒绝
    if (!f.returns.empty() && f.returns.back()) {
        VarList* rl = f.returns.back();
        Expr* ty = rl->vars.empty() ? rl->type
                                    : rl->vars.back()->type;
        if (ty && ty->is_reference())
            error(rl->vars.empty() ? rl->loc : rl->vars.back()->loc,
                  "cannot return a reference type");
    }
    // body 为空（未写 {}）时不检查返回值是否被赋值
    if (!f.body) return;
    flow_.check_jumps(f.body);
    flow_.check_initialization(f.body, f.params, f.returns);
    if (flow_.can_use_register_ret(f)) f.ret_via_reg = true;
}




void SemanticAnalyzer::visit_special_method(SpecialMethod& sm) {
    if (!sm.body) return;
    flow_.check_jumps(sm.body);
    flow_.check_initialization(sm.body, sm.params, {},
                               sm.kind == SpecialMethod::Kind::Init);
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
static bool copies_reference_fields(Expr* src, Expr* dst) {
    if (!src || !dst || src->conv || src->implicit_to) return false;
    if (!src->is_lvalue) return false;
    if (!is_same_type(src->type, dst)) return false;
    return class_has_ref_field(as_class(dst));
}

void SemanticAnalyzer::visit_var_list(VarList& vl) {
    // 模块级变量的存储是 LLVM 全局量，其类型必须是常量可表达的
    bool module_level = !current_params && !current_members;
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
        for (VarItem* var : vl.vars) {
            Expr* dt = var && var->type ? var->type : vl.type;
            if (!dt) continue;
            if (dt->is_array_type() || dt->is_string_literal_type() ||
                (var && var->init &&
                 (var->init->is_array() ||
                  (var->init->type &&
                   var->init->type->is_string_literal_type()))))
                error(var->loc, "module-level variable cannot be an array or "
                      "string: its storage would not outlive the initializer");
        }
    }
    // 动态长度数组检查：运行时长度必须显式声明存储方式
    if (vl.type && vl.type->is_array_type()) {
        auto* arr = vl.type->get_array_type();
        if (arr->length) {
            // 检查长度是否为编译期常量（字面量，或引用 const 变量的标识符）
            bool is_const_length = arr->length->is_literal();
            if (!is_const_length && arr->length->sym && arr->length->sym->is_const()) {
                is_const_length = true;
            }
        }
    }
    // 初始化类型检查：显式声明类型时，初值须可转换到声明类型（let 推导跳过）
    for (VarItem* var : vl.vars) {
        if (!var) continue;
        Expr* dt = var->type ? var->type : vl.type;
        // 引用必须创建时绑定：初值即绑定，绑定源必须是同目标类型的 lvalue。
        // 形参在进入函数时由实参绑定，字段在 INIT 中绑定，二者都不在声明处。
        // 绑定不走常规值转换。
        if (dt && dt->is_reference()) {
            // 返回变量的存储随栈帧消失，绑定它没有意义（visit_func 已单独报错）
            if (is_return_var_item(var)) continue;
            if (!var->init) {
                if (!is_param_item(var) && !is_field_item(var))
                    error(var->loc, "reference must be initialized");
            } else if (!var->init->type) {
                // 解析器已报错
            } else if (!var->init->is_lvalue) {
                error(var->init->loc,
                      "reference must be initialized with an lvalue");
            } else {
                Expr* tgt = dt->get_reference()->target;
                if (tgt && !tgt->is_reference() &&
                    !is_same_type(var->init->type, tgt))
                    error(var->init->loc, "reference target type mismatch");
            }
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
            // 字面量类型对齐声明类型，使长度以声明为准
            if (dt->get_array_type()->length &&
                dt->get_array_type()->length->is_literal())
                var->init->type = dt;
        } else if (dt->is_array_type() && var->init->type &&
                   var->init->type->is_string_literal_type()) {
            // 字符串字面量初值：只检查长度，保留其类型供 translator 生成切片
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

// 引用字段的绑定点只能是 INIT 顶层的直接赋值：嵌套在条件里的赋值不保证执行
static bool field_bound_in_init(Class& c, const VarItem* f) {
    for (Decl* m : c.members) {
        if (!m || m->kind != DeclKind::SpecialMethod) continue;
        auto* sm = static_cast<SpecialMethod*>(m);
        if (sm->kind != SpecialMethod::Kind::Init || !sm->body) continue;
        auto* blk = sm->body->get_block();
        if (!blk) return false;
        for (Stmt* st : blk->stmts) {
            if (!st) continue;
            auto* es = st->get_expr();
            auto* asg = es && es->expr ? es->expr->get_assign() : nullptr;
            if (asg && asg->target && asg->target->sym &&
                asg->target->sym->node == f)
                return true;
        }
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
            if (!vi || !vi->type || !vi->type->is_reference()) continue;
            std::string f = "reference field '" + std::string(vi->name) + "'";
            if (is_static) {
                error(vi->loc, f + " cannot be static: a reference field can "
                      "only be bound in INIT");
                continue;
            }
            if (vi->init) {
                error(vi->init->loc, f + " must be bound by an assignment at "
                      "the top level of INIT, not by an initializer");
                continue;
            }
            if (!field_bound_in_init(c, vi))
                error(vi->loc, f + " must be bound by an assignment at the "
                      "top level of INIT");
        }
    }
}

void SemanticAnalyzer::visit_namespace(Namespace&) {}
void SemanticAnalyzer::visit_extern(Extern&) {}
void SemanticAnalyzer::visit_enum(Enum& e) {
    std::unordered_map<std::string_view, bool> seen;
    for (EnumVariant* v : e.variants) {
        if (!v) continue;
        if (!seen.emplace(v->name, true).second) {
            error(v->loc, "redefinition of enum variant '" + std::string(v->name) + "'");
        }
    }
    if (e.variants.empty()) {
        warning(e.loc, "enum '" + std::string(e.name) + "' declares no variants");
    }
}
void SemanticAnalyzer::visit_keep(Keep&) {}

void SemanticAnalyzer::visit(Expr&) {}

void SemanticAnalyzer::visit_expr_literal(Expr::Literal& lit) {
    switch(lit.kind) {
        #define X(l,name) case LiteralKind::name: current_expr->type = type##name; break;
        LITERAL_MAPPING
        #undef X
    }
    // 字面量自身类型范围检查：仅对显式带后缀的字面量（如 300i8）生效，
    // 无后缀字面量的"自身类型"只是词法默认，其范围由目标类型的转换检查负责。
    // IntLiteral 既可能是无后缀十进制数，也可能是显式 i32/i，靠字面量文本区分。
    bool has_suffix = (lit.kind != LiteralKind::Int);
    if (!has_suffix) {
        for (char c : lit.value)
            if (c == 'i' || c == 'I') { has_suffix = true; break; }
    }
    bool ls = false; int lb = 0;
    if (has_suffix && literal_int_range(lit.kind, ls, lb)) {
        unsigned long long v = 0;
        if (parse_uint_literal(lit.value, v) && v > int_max_value(ls, lb))
            error(current_expr->loc, "integer literal " + std::string(lit.value) +
                  " is out of range for its type");
    }
    current_expr->is_lvalue = false;
}

void SemanticAnalyzer::visit_expr_null(Expr::Null&) {
    current_expr->type = typeNull;
    current_expr->is_lvalue = false;
}

void SemanticAnalyzer::visit_expr_identifier(Expr::Identifier& id) {
    // self：当前实例本身，类型为自身的值类型
    if (!current_expr->is_type && current_class && id.name == "self") {
        current_expr->type = make_named_type(current_expr->loc, current_class->name,
                                             current_class, current_class->scope);
        current_expr->is_lvalue = true;
        return;
    }
    // 类型位置的裸名：按类型解析
    if (current_expr->is_type) {
        std::string name(id.name);
        Symbol* s = symbols_.lookup_member(name);
        if (s && (s->kind == SymbolKind::Class ||
                  s->kind == SymbolKind::Namespace ||
                  s->kind == SymbolKind::Enum)) {
            current_expr->sym = s;
            current_expr->resolved_decl = s->get_decl();
            current_expr->resolved_scope = s->scope;
        } else {
            error(current_expr->loc, "unknown type '" + name + "'");
        }
        return;
    }

    Symbol* sym = symbols_.lookup_local(id.name);
    if (sym) {
        current_expr->sym = sym;
        Expr* st = sym->get_type();
        // 读取引用变量即读取被引用对象：隐式解引用为其目标类型
        if (st && st->is_reference()) st = st->get_reference()->target;
        current_expr->type = st;
        current_expr->is_lvalue = (sym->kind == SymbolKind::Variable);
        return;
    }

    Symbol* member = symbols_.lookup_member(std::string(id.name));
    if (member) {
        current_expr->sym = member;
        check_access(member, current_expr->loc);
        if (member->kind == SymbolKind::Variable) {
            current_expr->type = member->get_type();
            current_expr->is_lvalue = true;
        } else if (member->kind == SymbolKind::Function) {
            if (Decl* d = member->get_decl()) {
                if (d->kind == DeclKind::Func || d->kind == DeclKind::Operator)
                    current_expr->type = make_type(current_expr->loc,
                        Expr::FuncRef{static_cast<Func*>(d)}
                    );
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

Symbol* SemanticAnalyzer::find_operator_overload(OpKind op, Expr& type, Expr& other,
                                                 Expr** matched_param) {
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
        Expr* pt = pl->vars[0]->type ? pl->vars[0]->type : pl->type;
        if (!pt) continue;
        if (is_same_type(&other, pt) || can_convert_to(&other, pt)) {
            if (matched_param) *matched_param = pt;
            return &member;
        }
    }
    return nullptr;
}

Symbol* SemanticAnalyzer::find_operator_overload(OpKind op, Expr& type) {
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
    // const 在任何作用域都不可写
    if (sym->is_const()) {
        error(loc, "cannot assign to const '" + sym_display_name(sym) + "'");
        return;
    }
    Access acc = sym->access_level();
    if (acc == Access::Public) return;
    if (is_scope_on_path(sym->decl_scope)) return;
    if (acc == Access::Private) {
        error(loc, "cannot assign to private member '" + sym_display_name(sym) + "'");
    } else if (acc == Access::Readonly) {
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
    Expr* lt = b.left ? b.left->type : nullptr;
    Expr* rt = b.right ? b.right->type : nullptr;
    if (!lt || !rt) return;

    // 非同包装类的混合运算：解包包装类一侧，走基础运算
    Class* lw = wrap_class_of(lt);
    Class* rw = wrap_class_of(rt);
    if (!(lw && rw && lw == rw) && (lw || rw)) {
        if (lw) {
            Expr* base_t = wrap_base_type(lw);
            record_conversion(b.left, base_t);
            lt = base_t;
        }
        if (rw) {
            Expr* base_t = wrap_base_type(rw);
            record_conversion(b.right, base_t);
            rt = base_t;
        }
    }

    // 枚举底层是数字：参与运算时归一为 int
    if (is_enum_type(lt)) lt = typeInt;
    if (is_enum_type(rt)) rt = typeInt;

    Expr* matched_param = nullptr;
    if (Symbol* s = find_operator_overload(b.op, *lt, *rt, &matched_param)) {
        Expr* overload_ret = typeVoid;
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
    Expr* ot = u.operand ? u.operand->type : nullptr;

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
                current_expr->type = make_type(current_expr->loc, Expr::Pointer{ot});
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

    Expr* lt = a.target->type;
    // 引用目标的赋值写入被引用对象，按被引用类型判定
    if (a.target->sym) {
        Expr* dt = a.target->sym->get_type();
        if (dt && dt->is_reference()) lt = dt->get_reference()->target;
    }
    if (lt && lt->is_reference()) lt = lt->get_reference()->target;
    Expr* rt = a.value->type;

    // 复合赋值操作符重载优先（可能具备与对应二元操作符不同的独特语义）
    if (a.op != OpKind::Assign && lt && rt) {
        Expr* matched = nullptr;
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

    Expr* tt = idx.target->type;
    if (!tt) {
        current_expr->type = nullptr;
        return;
    }

    Expr* it = idx.index ? idx.index->type : nullptr;
    if (it) {
        if (Symbol* s = find_operator_overload(OpKind::Index, *tt, *it)) {
            Expr* ret = typeVoid;
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

    // 数组的 .len：返回长度
    if (m.member == "len" && m.target->type && m.target->type->is_array_type()) {
        current_expr->sym = nullptr;
        current_expr->type = typeInt;
        current_expr->is_lvalue = false;
        return;
    }

    // 数组的 .ptr：提取底层指针（指向元素类型）
    if (m.member == "ptr" && m.target->type && m.target->type->is_array_type()) {
        Expr* elem = m.target->type->get_array_type()->element;
        current_expr->sym = nullptr;
        current_expr->type = make_type(current_expr->loc, Expr::Pointer{elem});
        current_expr->is_lvalue = false;
        return;
    }

    // malloc 类的内置 free：a.free() 释放堆实例
    if (m.member == "free" && m.target->type) {
        Class* cls = wrap_or_plain_class(m.target->type);
        if (cls && cls->specs.has(SpecKind::Malloc)) {
            current_expr->sym = nullptr;
            current_expr->type = typeVoid;
            current_expr->is_lvalue = false;
            return;
        }
    }

    // 类型位置的限定名：A::B / A.B 按类型路径解析
    if (current_expr->is_type) {
        Scope* scope = m.target->is_type
            ? m.target->resolved_scope
            : named_type_scope(m.target->type);
        Symbol* tsym = scope
            ? scope->lookup_member(std::string(m.member)) : nullptr;
        if (tsym && (tsym->kind == SymbolKind::Class ||
                     tsym->kind == SymbolKind::Namespace ||
                     tsym->kind == SymbolKind::Enum)) {
            current_expr->sym = tsym;
            current_expr->resolved_decl = tsym->get_decl();
            current_expr->resolved_scope = tsym->scope;
        } else {
            error(current_expr->loc,
                  "unknown type '" + type_name_string(current_expr) + "'");
        }
        return;
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
            Expr* ft = member->get_type();
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
                        Expr::FuncRef{static_cast<Func*>(d)}
                    );
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
        Expr* src = cast.operand->type;
        Expr* dst = cast.target_type;
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
        if (arr.fill_value && arr.fill_value->type) {
            current_expr->type = make_type(current_expr->loc,
                Expr::ArrayType{arr.fill_value->type, nullptr});
        } else {
            current_expr->type = make_type(current_expr->loc,
                Expr::ArrayType{typeInt, nullptr});
        }
        current_expr->is_lvalue = false;
        return;
    }

    Expr* elem_type = nullptr;
    for (auto* member : arr.members) {
        if (!member) continue;
        if (!elem_type) {
            elem_type = member->type;
        } else if (member->type && !is_same_type(elem_type, member->type)) {
            warning(current_expr->loc, "heterogeneous array literal, using first element type");
        }
    }

    current_expr->type = make_type(current_expr->loc,
        Expr::ArrayType{elem_type ? elem_type : typeInt,
                    make_int_literal(current_expr->loc, arr.members.size())});
    current_expr->is_lvalue = false;
}

void SemanticAnalyzer::visit_expr_ternary(Expr::Ternary& t) {
    Expr* ct = t.cond ? t.cond->type : nullptr;
    if (ct && !is_bool_type(ct)) {
        warning(t.cond->loc, "condition in ternary expression is not bool");
    }

    Expr* tt = t.then_expr ? t.then_expr->type : nullptr;
    Expr* et = t.else_expr ? t.else_expr->type : nullptr;

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
    
    Expr* cond_type = if_stmt.condition->type;
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
    
    Expr* cond_type = while_stmt.condition->type;
    if (cond_type && wrap_class_of(cond_type))
        record_conversion(while_stmt.condition, typeBool);
    if (cond_type && !is_condition_type(cond_type)) {
        warning(while_stmt.condition->loc, "while condition is not bool, implicit conversion");
    }
}

void SemanticAnalyzer::visit_stmt_for(Stmt::For& for_stmt) {
    if (for_stmt.condition) {
        Expr* cond_type = for_stmt.condition->type;
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

void SemanticAnalyzer::visit_stmt_flag(Stmt::Flag&) {}
void SemanticAnalyzer::visit_stmt_goto(Stmt::Goto&) {}
void SemanticAnalyzer::visit_stmt_asm(Stmt::Asm&) {}
void SemanticAnalyzer::visit_stmt_keep(Stmt::Keep&) {}
void SemanticAnalyzer::visit_stmt_var_decl(Stmt::VarDecl&) {}
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
        std::vector<Expr*> params;
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
            Expr* pt = i < params.size() ? params[i] : nullptr;
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
void SemanticAnalyzer::visit_type_primitive(Expr::Primitive&) {}
// ast本来应该有指针指向的节点如果是nullptr，说明已经在parser报过错了，不要重复报错
// 引用不可寻址，指针不能指向引用：否则 *p 该返回引用还是被引用对象无从判定
void SemanticAnalyzer::visit_type_pointer(Expr::Pointer& p) {
    if (p.pointee && p.pointee->is_reference())
        error(current_expr->loc, "cannot form a pointer to a reference");
}
void SemanticAnalyzer::visit_type_array(Expr::ArrayType& a) {
    // 数组元素没有逐个绑定的语法，元素类型为引用则无法初始化
    if (a.element && a.element->is_reference())
        error(current_expr->loc, "cannot form an array of references");
}
void SemanticAnalyzer::visit_type_func_ptr(Expr::FuncPtr&) {}
void SemanticAnalyzer::visit_type_func_ref(Expr::FuncRef&) {}
// 引用不可重新绑定，引用不能再包引用
void SemanticAnalyzer::visit_type_reference(Expr::Reference& r) {
    if (r.target && r.target->is_reference())
        error(current_expr->loc, "reference to reference");
}

void SemanticAnalyzer::visit_type_string_literal(Expr::StringLiteralType&) {
    // StringLiteral is a valid type, no additional checks needed
}
Symbol* SemanticAnalyzer::lookup_member_on_type(Expr* type, const std::string& field, Scope** found_scope) {
    if (found_scope) *found_scope = nullptr;
    if (!type) return nullptr;
    Scope* s = named_type_scope(type);
    if (found_scope) *found_scope = s;
    return s ? s->lookup_member(field) : nullptr;
}

void SemanticAnalyzer::visit_type_void(Expr::Void&) {
    // Void is valid as a return type or pointer pointee context, checked by callers
}

void SemanticAnalyzer::visit_type_inferred(Expr::Inferred&) {}


// flag / toflag 的 label 检查：遵循 C++ 的跳转规则
// - 目标 label 必须在本函数体内定义
// - 不得跳入某个作用域内部（即目标所在作用域必须是跳转点作用域的祖先或自身）
// - 不得跳入循环内部








// main 的合法形式：参数为空或 (int, char**)，返回为空或 int
void SemanticAnalyzer::check_main() {
    Scope* root = symbols_.root();
    Symbol* sym = root ? root->lookup_member("main") : nullptr;
    if (!sym || sym->kind != SymbolKind::Function) return;
    Func* f = sym->get<Func>();
    if (!f) return;

    std::vector<Expr*> params;
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

    std::vector<Expr*> returns;
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

} // namespace cx
