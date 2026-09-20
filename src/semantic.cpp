#include "../include/semantic.h"
#include "ast.h"

#include <unordered_set>
#include <unordered_map>
#include <functional>

namespace cx {

// 类型名节点（Identifier / 限定名 Member 链）的完整名字
static std::string type_name_string(Expr* t) {
    if (!t) return "";
    if (auto* id = t->get_identifier()) return std::string(id->name);
    if (auto* m = t->get_member())
        return type_name_string(m->target) + "::" + std::string(m->member);
    return "";
}

// 引用类型的表达式即 lvalue
static bool ref_is_lvalue(Expr* t) {
    return t && t->is_reference();
}

// 从解析后的类型节点取成员作用域
static Scope* named_type_scope(Expr* t) {
    if (!t) return nullptr;
    if (t->is_modifier()) {
        auto* m = t->get_modifier();
        for (size_t i = 0; i < m->layers.size(); ++i) {
            if (!m->is_mod[i]) continue;
            if (Scope* s = named_type_scope(m->layers[i])) return s;
        }
        return nullptr;
    }
    if (t->resolved_scope) return t->resolved_scope;
    if (t->resolved_decl) {
        if (Cont* cont = dynamic_cast<Cont*>(t->resolved_decl)) return cont->scope;
    }
    return nullptr;
}

// 组合类型中承载值的层（唯一的非修饰层），逐层剥去修饰
static Expr* underlying_type(Expr* t) {
    while (t && t->is_modifier()) {
        auto* m = t->get_modifier();
        Expr* val = nullptr;
        for (size_t i = 0; i < m->layers.size(); ++i) {
            if (!m->is_mod[i]) { val = m->layers[i]; break; }
        }
        if (!val || val == t) break;
        t = val;
    }
    return t;
}

// 浮点类型提升级：float16 < float < double
static int float_rank(PrimitiveTypeKind k) {
    switch (k) {
        case PrimitiveTypeKind::Double: return 2;
        case PrimitiveTypeKind::Float:  return 1;
        default:                        return 0;
    }
}

// 整数类型提升级：int8 < short < int < long < int128
static int integer_rank(PrimitiveTypeKind k) {
    switch (k) {
        case PrimitiveTypeKind::Int128:
        case PrimitiveTypeKind::UInt128: return 4;
        case PrimitiveTypeKind::Long:
        case PrimitiveTypeKind::ULong:   return 3;
        case PrimitiveTypeKind::Int:
        case PrimitiveTypeKind::UInt:    return 2;
        case PrimitiveTypeKind::Short:
        case PrimitiveTypeKind::UShort:  return 1;
        default:                         return 0;
    }
}

bool SemanticAnalyzer::is_numeric_type(Expr* t) {
    t = underlying_type(t);
    if (!t || !t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k != PrimitiveTypeKind::Bool && k != PrimitiveTypeKind::Type && k != PrimitiveTypeKind::Void;
}

bool SemanticAnalyzer::is_integer_type(Expr* t) {
    t = underlying_type(t);
    if (!t) return false;
    if(auto* p = t->get_primitive()){
        PrimitiveTypeKind k = p->kind;
        return k == PrimitiveTypeKind::Int8 || k == PrimitiveTypeKind::Short || k == PrimitiveTypeKind::Int
            || k == PrimitiveTypeKind::Long || k == PrimitiveTypeKind::Int128
            || k == PrimitiveTypeKind::UInt8 || k == PrimitiveTypeKind::UShort || k == PrimitiveTypeKind::UInt
            || k == PrimitiveTypeKind::ULong || k == PrimitiveTypeKind::UInt128
            || k == PrimitiveTypeKind::Char || k == PrimitiveTypeKind::UChar;
    }
    return false;
}

bool SemanticAnalyzer::is_float_type(Expr* t) {
    t = underlying_type(t);
    if (!t || !t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k == PrimitiveTypeKind::Float16 || k == PrimitiveTypeKind::Float || k == PrimitiveTypeKind::Double;
}

bool SemanticAnalyzer::is_bool_type(Expr* t) {
    t = underlying_type(t);
    return t && t->is_primitive() && t->get_primitive()->kind == PrimitiveTypeKind::Bool;
}

bool SemanticAnalyzer::is_same_type(Expr* a, Expr* b) {
    if (!a || !b) return false;
    // 双方都带修饰：逐层比较；仅一方带修饰：剥离修饰层后按值类型比较
    if (a->is_modifier() && b->is_modifier()) {
        auto* ma = a->get_modifier();
        auto* mb = b->get_modifier();
        if (ma->layers.size() != mb->layers.size()) return false;
        for (size_t i = 0; i < ma->layers.size(); ++i) {
            if (ma->is_mod[i] != mb->is_mod[i]) return false;
            if (!is_same_type(ma->layers[i], mb->layers[i])) return false;
        }
        return true;
    }
    a = underlying_type(a);
    b = underlying_type(b);
    // StringLiteral 与 char[] 视为同类
    auto is_char_array = [](Expr* t) {
        if (!t || !t->is_array_type()) return false;
        Expr* elem = t->get_array_type()->element;
        return elem && elem->is_primitive()
            && elem->get_primitive()->kind == PrimitiveTypeKind::Char;
    };
    if (a->is_string_literal_type() && is_char_array(b)) return true;
    if (b->is_string_literal_type() && is_char_array(a)) return true;
    bool a_named = a->is_type && (a->is_identifier() || a->is_member());
    bool b_named = b->is_type && (b->is_identifier() || b->is_member());
    if (a_named && b_named) {
        if (a->resolved_decl && b->resolved_decl) {
            return a->resolved_decl == b->resolved_decl;
        }
        return type_name_string(a) == type_name_string(b);
    }
    if (a->data.index() != b->data.index()) return false;
    if (a->is_primitive() && b->is_primitive()) {
        return a->get_primitive()->kind == b->get_primitive()->kind;
    }
    if (a->is_pointer() && b->is_pointer()) {
        return is_same_type(a->get_pointer()->pointee, b->get_pointer()->pointee);
    }
    if (a->is_reference() && b->is_reference()) {
        return is_same_type(a->get_reference()->target, b->get_reference()->target);
    }
    // 数组：元素类型相同即可（长度可推导，暂不严格比较）
    if (a->is_array_type() && b->is_array_type()) {
        return is_same_type(a->get_array_type()->element,
                            b->get_array_type()->element);
    }
    if (a->is_func_ptr() && b->is_func_ptr()) {
        auto* fa = a->get_func_ptr();
        auto* fb = b->get_func_ptr();
        if (fa->params.size() != fb->params.size()) return false;
        for (size_t i = 0; i < fa->params.size(); ++i) {
            if (!is_same_type(fa->params[i], fb->params[i])) return false;
        }
        return is_same_type(fa->return_type, fb->return_type);
    }
    if (a->is_string_literal_type() && b->is_string_literal_type()) return true;
    if (a->is_void() && b->is_void()) return true;
    return false;
}

bool SemanticAnalyzer::is_integer_promotable(Expr* t) {
    t = underlying_type(t);
    if (!t || !t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k == PrimitiveTypeKind::Int8 || k == PrimitiveTypeKind::Short
        || k == PrimitiveTypeKind::UInt8 || k == PrimitiveTypeKind::UChar
        || k == PrimitiveTypeKind::Char;
}

bool SemanticAnalyzer::is_void_type(Expr* t) {
    t = underlying_type(t);
    if (!t) return false;
    if (t->is_void()) return true;
    // 解析器把源码中的 void 生成为 Primitive{Void}
    if (auto* p = t->get_primitive())
        return p->kind == PrimitiveTypeKind::Void;
    return false;
}

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
    bodies_.clear();
    collect();
}

void SemanticAnalyzer::analyze_rest() {
    process_using_namespaces();
    walk(cu_);
    // 跳转与初始化检查需要完整函数体，放在遍历之后进行
    check_jumps();
    check_initialization();
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
                sym.modfs = &vl->modfs;
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
            // 在注册本类之前解析注入来源，使自注入与来源失败能正确报错
            apply_injections(cls);
            if (existing) {
                if (existing->kind == SymbolKind::Class || existing->kind == SymbolKind::Namespace)
                    error(cls->loc, "redefinition of class '" + name + "'");
                else
                    error(cls->loc, "cannot declare class '" + name + "' with same name as non-container");
                symbols_.push_scope(name);
            } else {
                Symbol* sym = symbols_.insert_and_push(name, Symbol(SymbolKind::Class, cls));
                if (sym) {
                    sym->modfs = &cls->modfs;
                    sym->default_public = default_public;
                    sym->node = cls;
                }
            }
            cls->scope = symbols_.current_scope();
            // 类默认私有
            for (Decl* member : cls->members) {
                collect_decl(member, false);
            }
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
                    sym->modfs = &ns->modfs;
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
                sym.modfs = &e->modfs;
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
    sym.modfs = &f.modfs;
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
        case SpecialMethod::Kind::Access:     internal_name = "ACCESS";     break;
        default: error(sm.loc, "unknown special method"); return;
    }

    Scope* scope = symbols_.current_scope();
    if (scope && scope->lookup_member(internal_name)) {
        error(sm.loc, "duplicate special method '" + internal_name + "'");
        return;
    }

    Symbol sym(SymbolKind::Function, &sm);
    sym.modfs = &sm.modfs;
    sym.default_public = default_public;
    symbols_.insert_member(internal_name, std::move(sym));
}

std::string SemanticAnalyzer::type_to_key(Expr& t) {
    if (Expr* u = underlying_type(&t); u != &t) return type_to_key(*u);
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
        return "[" + elem_ir + "]";
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
    if (t.is_inferred()) {
        error(t.loc, "cannot convert inferred type to unique key");
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
    if (f.body) bodies_.push_back({f.body, f.params, f.returns});
}

void SemanticAnalyzer::visit_special_method(SpecialMethod& sm) {
    if (sm.body) bodies_.push_back({sm.body, sm.params, {}});
}

void SemanticAnalyzer::visit_var_list(VarList& vl) {
    // 动态长度数组检查：运行时长度必须显式声明存储方式
    if (vl.type && vl.type->is_array_type()) {
        auto* arr = vl.type->get_array_type();
        if (arr->length) {
            // 检查长度是否为编译期常量（字面量，或引用 const 变量的标识符）
            bool is_const_length = arr->length->is_literal();
            if (!is_const_length && arr->length->sym && arr->length->sym->is_const()) {
                is_const_length = true;
            }
            if (!is_const_length) {
                bool has_alloca = vl.modfs.has(ModfKind::Alloca);
                bool has_malloc = vl.modfs.has(ModfKind::Malloc);
                if (!has_alloca && !has_malloc) {
                    warning(vl.loc, "array with runtime length must use 'alloca' or 'malloc' modifier");
                }
            }
        }
    }
    // 初始化类型检查：显式声明类型时，初值须可转换到声明类型（let 推导跳过）
    for (VarItem* var : vl.vars) {
        if (!var || !var->init || !var->init->type) continue;
        Expr* dt = var->type ? var->type : vl.type;
        if (!dt || dt->is_inferred()) continue;
        if (is_same_type(var->init->type, dt)) continue;
        // 隐式转换优先匹配 conversion
        if (Symbol* conv = find_conversion(*var->init->type, *dt)) {
            var->init->conv = conv->get_decl();
            continue;
        }
        // 目标是类且存在匹配的 INIT 时，视为构造初始化
        if (init_matches_class_ctor(dt, var->init)) continue;
        check_conversion(var->init->type, dt, "initialization", var->init->loc);
    }
    for (VarItem* var : vl.vars) {
        Symbol sym(SymbolKind::Variable, var);
        sym.modfs = &vl.modfs;
        sym.default_public = true;
        if (!symbols_.insert_local(var->name, std::move(sym))) {
            error(var->loc, "duplicate variable '" + std::string(var->name) + "'");
        } else {
            var_symbols_[var] = symbols_.current_->lookup_local(var->name);
        }
    }
}

void SemanticAnalyzer::visit_class(Class&) {}

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
        current_expr->type = sym->get_type();
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

Symbol* SemanticAnalyzer::find_operator_overload(OpKind op, Expr& type, Expr& other) {
    std::string opstr = std::string(opkind_to_sv(op));
    std::string otherkey = type_to_key(other);
    Symbol* sym = symbols_.lookup_member(opstr + "_" + type_to_key(type) + otherkey);
    if (sym) return sym;
    if (!type.resolved_decl || type.resolved_decl->kind != DeclKind::Class) return nullptr;
    Scope* scope = static_cast<Class*>(type.resolved_decl)->scope;
    if (!scope) return nullptr;
    Symbol* cls_sym = scope->lookup_member(opstr + "_" + otherkey);
    return cls_sym;
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

Symbol* SemanticAnalyzer::find_conversion(Expr& src, Expr& dst) {
    std::string srckey = type_to_key(src);
    std::string dstkey = type_to_key(dst);
    if (Symbol* sym = symbols_.lookup_member("conversion_" + srckey + "_" + dstkey))
        return sym;
    if (!src.resolved_decl || src.resolved_decl->kind != DeclKind::Class) return nullptr;
    Scope* scope = static_cast<Class*>(src.resolved_decl)->scope;
    if (!scope) return nullptr;
    return scope->lookup_member("conversion_" + dstkey);
}

bool SemanticAnalyzer::can_convert_to(Expr* src, Expr* dst) {
    if (!src || !dst) return false;
    if (is_same_type(src, dst)) return true;

    // 数值类型之间可隐式转换
    if (is_numeric_type(src) && is_numeric_type(dst)) return true;

    // bool 与整数互转
    if (is_bool_type(src) && is_integer_type(dst)) return true;
    if (is_integer_type(src) && is_bool_type(dst)) return true;

    // 任意指针 → void*
    if (src->is_pointer() && dst->is_pointer() &&
        is_void_type(dst->get_pointer()->pointee)) {
        return true;
    }

    // conversion：全局有参形式或源类无参形式
    if (find_conversion(*src, *dst)) return true;

    return false;
}


bool SemanticAnalyzer::init_matches_class_ctor(Expr* dst, Expr* init) {
    Expr* u = underlying_type(dst);
    if (!u || !u->resolved_decl || u->resolved_decl->kind != DeclKind::Class) return false;
    auto* cls = static_cast<Class*>(u->resolved_decl);
    std::vector<Expr*> args = {init};
    for (Decl* m : cls->members) {
        if (!m || m->kind != DeclKind::SpecialMethod) continue;
        auto* sm = static_cast<SpecialMethod*>(m);
        if (sm->kind == SpecialMethod::Kind::Init && args_match(sm->params, args))
            return true;
    }
    return false;
}

// 判断作用域 s 是否位于 root -> ... -> current 的路径上
bool SemanticAnalyzer::is_scope_on_path(Scope* s) const {
    if (!s) return false;
    for (Scope* cur = symbols_.current_scope(); cur; cur = cur->parent) {
        if (cur == s) return true;
    }
    return false;
}

static std::string sym_display_name(Symbol* sym) {
    if (!sym) return "";
    if (auto* vi = dynamic_cast<VarItem*>(sym->node)) return std::string(vi->name);
    if (Decl* d = sym->get_decl()) return std::string(d->name);
    return "";
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

// conversion 判定：判断 src 能否转换为 dst，复用 can_convert_to 的转换规则
bool SemanticAnalyzer::has_conversion(Expr* src, Expr* dst) {
    return can_convert_to(src, dst);
}

// = 赋值、() 传参、cast 转换三种情形共用的转换判定入口
bool SemanticAnalyzer::check_conversion(Expr* src, Expr* dst, const std::string& context,
                                        const Location& loc, bool explicit_cast) {
    if (!src || !dst) return false;
    if (is_same_type(src, dst)) return true;
    if (has_conversion(src, dst)) return true;
    if (explicit_cast) {
        warning(loc, "explicit cast between incompatible types in " + context);
        return true;
    }
    error(loc, "cannot implicitly convert between incompatible types in " + context);
    return false;
}

void SemanticAnalyzer::visit_expr_binary(Expr::Binary& b) {
    Expr* lt = b.left ? b.left->type : nullptr;
    Expr* rt = b.right ? b.right->type : nullptr;
    if (!lt || !rt) return;

    if (Symbol* s = find_operator_overload(b.op, *lt, *rt)) {
        Expr* overload_ret = typeVoid;
        if (auto* op = s->get<Operator>()) {
            if (!op->returns.empty() && op->returns.back() && op->returns.back()->type)
                overload_ret = op->returns.back()->type;
        }
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
    Expr* rt = a.value->type;

    // 赋值统一走 = 的转换判定
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

void SemanticAnalyzer::resolve_call(Expr* call_expr, Expr::Call& c) {
    auto fail = [&](const std::string& msg) {
        error(call_expr->loc, msg);
        call_expr->type = typeVoid;
        call_expr->is_lvalue = false;
    };

    if (!c.callee) {
        fail("call expression has no callee");
        return;
    }

    // 路径 1：callee 的类型是函数指针 $(...) → 按类型调用
    // 覆盖：函数指针变量、闭包字面量、返回函数指针的表达式
    if (c.callee->type && c.callee->type->is_func_ptr()) {
        auto* fp = c.callee->type->get_func_ptr();

        if (fp->params.size() != c.args.size()) {
            error(call_expr->loc,
                  "function pointer expects " + std::to_string(fp->params.size()) +
                  " argument(s), got " + std::to_string(c.args.size()));
        }
        for (size_t i = 0; i < c.args.size() && i < fp->params.size(); ++i) {
            Expr* at = c.args[i] ? c.args[i]->type : nullptr;
            Expr* pt = fp->params[i];
            if (at && pt && !is_same_type(at, pt) && !can_convert_to(at, pt)) {
                warning(c.args[i]->loc, "argument type mismatch in call");
            }
        }

        call_expr->sym = c.callee->sym;
        call_expr->type = fp->return_type ? fp->return_type : typeVoid;
        call_expr->is_lvalue = ref_is_lvalue(call_expr->type);
        return;
    }

    // 路径 1.5：被调用者是类实例 → 查找 operator ()
    if (c.callee->type && c.callee->type->resolved_decl &&
        c.callee->type->resolved_decl->kind == DeclKind::Class &&
        !(c.callee->sym && c.callee->sym->kind == SymbolKind::Class)) {
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
            return;
        }
    }

    // 路径 2：命名调用 → 解析被调用者名字，构造 callkey 后查表
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
        fail("invalid callee expression");
        return;
    }

    // 2a：类构造，遍历类成员中的 INIT 逐个匹配参数
    if (callee_sym && callee_sym->kind == SymbolKind::Class) {
        Scope* cls_scope = callee_sym->scope;
        Decl* cls_decl = callee_sym->get_decl();
        if (!cls_scope || !cls_decl || cls_decl->kind != DeclKind::Class) {
            fail("class '" + name + "' has no scope");
            return;
        }
        auto* cls = static_cast<Class*>(cls_decl);

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
            fail("no matching constructor for class '" + name + "'");
            return;
        }
        if (ctor) check_call_args(ctor->params, c.args, ctor->loc);

        call_expr->sym = nullptr;
        call_expr->type = make_named_type(call_expr->loc,
            cls->name, cls, cls_scope);
        call_expr->is_lvalue = false;
        c.resolved = ctor;
        return;
    }

    // 2b：callee 已解析出函数符号（限定名/成员方法）时直接使用
    Symbol* f_sym = nullptr;
    if (callee_sym && callee_sym->kind == SymbolKind::Function) {
        f_sym = callee_sym;
    } else {
        f_sym = symbols_.lookup_member(name);
    }
    if (!f_sym || f_sym->kind != SymbolKind::Function) {
        fail("no matching function for call to '" + name + "'");
        return;
    }

    Func* func = nullptr;
    if (Decl* d = f_sym->get_decl()) {
        if (d->kind == DeclKind::Func || d->kind == DeclKind::Operator)
            func = static_cast<Func*>(d);
    }
    if (!func) {
        fail("callee '" + name + "' is not a function");
        return;
    }

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
        Expr* at = args[i] ? args[i]->type : nullptr;
        if (!at || !params[i]) continue;
        check_conversion(at, params[i], "argument passing",
                         args[i] ? args[i]->loc : loc);
    }
}

// () 传参：按形参类型逐一对实参做转换判定
void SemanticAnalyzer::check_call_args(Func* fn, const std::vector<Expr*>& args) {
    if (!fn) return;
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
        if (!is_same_type(at, pt) && !can_convert_to(at, pt)) return false;
    }
    return true;
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

    Symbol* member = nullptr;
    if (m.target->type && m.target->type->is_modifier()) {
        member = lookup_member_on_type(m.target->type, field, nullptr);
    } else {
        member = SymbolTable::lookup_member_in(target_scope, field);
    }
    if (member) {
        current_expr->sym = member;
        check_access(member, current_expr->loc);

        if (member->kind == SymbolKind::Variable && member->get_type()) {
            current_expr->type = member->get_type();
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

void SemanticAnalyzer::visit_expr_closure(Expr::Closure& clo) {
    if (clo.body) bodies_.push_back({clo.body, clo.params, clo.returns});
    std::vector<Expr*> param_types;
    for (auto* p : clo.params) {
        if (p && p->type) {
            param_types.push_back(p->type);
        }
    }

    Expr* ret_type = nullptr;
    if (!clo.returns.empty() && clo.returns.back() && clo.returns.back()->type) {
        ret_type = clo.returns.back()->type;
    }

    current_expr->type = make_type(current_expr->loc,
        Expr::FuncPtr{std::move(param_types), ret_type});
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

void SemanticAnalyzer::visit(Modification&) {}
void SemanticAnalyzer::visit(Stmt&) {}
void SemanticAnalyzer::visit_stmt_block(Stmt::Block&) {}
void SemanticAnalyzer::visit_stmt_expr(Stmt::ExprStmt&) {}

void SemanticAnalyzer::visit_stmt_if(Stmt::If& if_stmt) {
    if (!if_stmt.condition) {
        error(current_stmt->loc, "if statement missing condition");
        return;
    }
    
    Expr* cond_type = if_stmt.condition->type;
    if (cond_type && !is_bool_type(cond_type)) {
        warning(if_stmt.condition->loc, "if condition is not bool, implicit conversion");
    }
}

void SemanticAnalyzer::visit_stmt_while(Stmt::While& while_stmt) {
    if (!while_stmt.condition) {
        error(current_stmt->loc, "while statement missing condition");
        return;
    }
    
    Expr* cond_type = while_stmt.condition->type;
    if (cond_type && !is_bool_type(cond_type)) {
        warning(while_stmt.condition->loc, "while condition is not bool, implicit conversion");
    }
}

void SemanticAnalyzer::visit_stmt_for(Stmt::For& for_stmt) {
    if (for_stmt.condition) {
        Expr* cond_type = for_stmt.condition->type;
        if (cond_type && !is_bool_type(cond_type)) {
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
        for (Expr* arg : c->args) {
            if (!arg || !arg->type) continue;
            if (arg->type->is_reference()) {
                error(arg->loc, "spawn cannot capture a stack reference argument");
            } else if (arg->type->is_pointer()) {
                error(arg->loc, "spawn cannot capture a stack pointer argument");
            }
        }
    }
}

// void 是合法返回类型/指针 pointee：解析器将 void 生成 Primitive{Void}，
// 无法从节点本身判断上下文，故不在此处拒绝
void SemanticAnalyzer::visit_type_primitive(Expr::Primitive&) {}
// ast本来应该有指针指向的节点如果是nullptr，说明已经在parser报过错了，不要重复报错
void SemanticAnalyzer::visit_type_pointer(Expr::Pointer&) {}
void SemanticAnalyzer::visit_type_array(Expr::ArrayType&) {}
void SemanticAnalyzer::visit_type_func_ptr(Expr::FuncPtr&) {}
void SemanticAnalyzer::visit_type_func_ref(Expr::FuncRef&) {}
void SemanticAnalyzer::visit_type_reference(Expr::Reference&) {}

void SemanticAnalyzer::visit_type_string_literal(Expr::StringLiteralType&) {
    // StringLiteral is a valid type, no additional checks needed
}
void SemanticAnalyzer::visit_type_modifier(Expr::Modifier&) {}

// 组合类型成员查找：层从左到右，左侧优先
Symbol* SemanticAnalyzer::lookup_member_on_type(Expr* type, const std::string& field, Scope** found_scope) {
    if (found_scope) *found_scope = nullptr;
    if (!type) return nullptr;
    if (type->is_modifier()) {
        auto* m = type->get_modifier();
        for (size_t i = 0; i < m->layers.size(); ++i) {
            if (!m->is_mod[i]) continue;
            Scope* s = named_type_scope(m->layers[i]);
            if (!s) continue;
            if (Symbol* sym = s->lookup_member(field)) {
                if (found_scope) *found_scope = s;
                return sym;
            }
        }
        return nullptr;
    }
    Scope* s = named_type_scope(type);
    if (found_scope) *found_scope = s;
    return s ? s->lookup_member(field) : nullptr;
}

void SemanticAnalyzer::visit_type_void(Expr::Void&) {
    // Void is valid as a return type or pointer pointee context, checked by callers
}

void SemanticAnalyzer::visit_type_inferred(Expr::Inferred&) {}

Expr* SemanticAnalyzer::clone_type(Expr* t) {
    if (!t) return nullptr;
    Location loc = t->loc;
    if (auto* m = t->get_modifier()) {
        std::vector<Expr*> layers;
        layers.reserve(m->layers.size());
        for (auto* l : m->layers) layers.push_back(clone_type(l));
        Expr* n = cu_.add_node<Expr>(loc, Expr::Modifier{std::move(layers), m->is_mod});
        n->is_type = true;
        return n;
    }
    if (auto* p = t->get_primitive()) return cu_.add_node<Expr>(loc, Expr::Primitive{p->kind});
    if (t->is_string_literal_type()) return cu_.add_node<Expr>(loc, Expr::StringLiteralType{});
    if (t->is_void()) return cu_.add_node<Expr>(loc, Expr::Void{});
    if (t->is_inferred()) return cu_.add_node<Expr>(loc, Expr::Inferred{});
    if (auto* p = t->get_pointer()) return cu_.add_node<Expr>(loc, Expr::Pointer{clone_type(p->pointee)});
    if (auto* r = t->get_reference()) return cu_.add_node<Expr>(loc, Expr::Reference{clone_type(r->target)});
    if (auto* a = t->get_array_type()) {
        return cu_.add_node<Expr>(loc, Expr::ArrayType{clone_type(a->element), clone_expr(a->length)});
    }
    if (auto* f = t->get_func_ptr()) {
        std::vector<Expr*> params;
        params.reserve(f->params.size());
        for (auto* p : f->params) params.push_back(clone_type(p));
        return cu_.add_node<Expr>(loc, Expr::FuncPtr{std::move(params), clone_type(f->return_type)});
    }
    if (t->is_type && (t->is_identifier() || t->is_member())) {
        Expr* n = nullptr;
        if (auto* id = t->get_identifier()) {
            n = cu_.add_node<Expr>(loc, Expr::Identifier{id->name});
        } else {
            auto* m = t->get_member();
            n = cu_.add_node<Expr>(loc, Expr::Member{clone_expr(m->target), m->member});
        }
        n->is_type = true;
        n->resolved_decl = t->resolved_decl;
        n->resolved_scope = t->resolved_scope;
        return n;
    }
    return cu_.add_node<Expr>(loc, t->data);
}

Expr* SemanticAnalyzer::clone_expr(Expr* e) {
    if (!e) return nullptr;
    Location loc = e->loc;
    if (e->is_type_node()) return clone_type(e);
    if (auto* v = e->get_literal()) return cu_.add_node<Expr>(loc, Expr::Literal{*v});
    if (auto* v = e->get_identifier()) return cu_.add_node<Expr>(loc, Expr::Identifier{v->name});
    if (auto* v = e->get_binary()) {
        return cu_.add_node<Expr>(loc, Expr::Binary{
            clone_expr(v->left), v->op, clone_expr(v->right)});
    }
    if (auto* v = e->get_unary()) {
        return cu_.add_node<Expr>(loc, Expr::Unary{v->op, clone_expr(v->operand), v->is_postfix});
    }
    if (auto* v = e->get_assign()) {
        return cu_.add_node<Expr>(loc, Expr::Assign{clone_expr(v->target), v->op, clone_expr(v->value)});
    }
    if (auto* v = e->get_call()) {
        std::vector<Expr*> args;
        args.reserve(v->args.size());
        for (auto* a : v->args) args.push_back(clone_expr(a));
        return cu_.add_node<Expr>(loc, Expr::Call{clone_expr(v->callee), std::move(args)});
    }
    if (auto* v = e->get_ternary()) {
        return cu_.add_node<Expr>(loc, Expr::Ternary{
            clone_expr(v->cond), clone_expr(v->then_expr), clone_expr(v->else_expr)});
    }
    if (auto* v = e->get_index()) {
        return cu_.add_node<Expr>(loc, Expr::Index{clone_expr(v->target), clone_expr(v->index)});
    }
    if (auto* v = e->get_member()) {
        return cu_.add_node<Expr>(loc, Expr::Member{clone_expr(v->target), v->member});
    }
    if (auto* v = e->get_cast()) {
        return cu_.add_node<Expr>(loc, Expr::Cast{clone_type(v->target_type), clone_expr(v->operand)});
    }
    if (auto* v = e->get_array()) {
        std::vector<Expr*> members;
        members.reserve(v->members.size());
        for (auto* m : v->members) members.push_back(clone_expr(m));
        return cu_.add_node<Expr>(loc, Expr::Array{std::move(members), clone_expr(v->fill_value)});
    }
    if (auto* v = e->get_spawn()) {
        return cu_.add_node<Expr>(loc, Expr::Spawn{clone_expr(v->call)});
    }
    if (auto* v = e->get_closure()) {
        std::vector<VarList*> params;
        for (auto* p : v->params) params.push_back(clone_varlist(p));
        std::vector<VarList*> returns;
        for (auto* r : v->returns) returns.push_back(clone_varlist(r));
        return cu_.add_node<Expr>(loc, Expr::Closure{std::move(params), std::move(returns),
            clone_stmt(v->body)});
    }
    return cu_.add_node<Expr>(loc, e->data);
}

Stmt* SemanticAnalyzer::clone_stmt(Stmt* s) {
    if (!s) return nullptr;
    Location loc = s->loc;
    if (auto* v = s->get_block()) {
        std::vector<Stmt*> stmts;
        stmts.reserve(v->stmts.size());
        for (auto* x : v->stmts) stmts.push_back(clone_stmt(x));
        return cu_.add_node<Stmt>(loc, Stmt::Block{std::move(stmts)});
    }
    if (auto* v = s->get_expr()) return cu_.add_node<Stmt>(loc, Stmt::ExprStmt{clone_expr(v->expr)});
    if (auto* v = s->get_if()) {
        return cu_.add_node<Stmt>(loc, Stmt::If{
            clone_expr(v->condition), clone_stmt(v->then_branch), clone_stmt(v->else_branch)});
    }
    if (auto* v = s->get_while()) {
        return cu_.add_node<Stmt>(loc, Stmt::While{clone_expr(v->condition), clone_stmt(v->body)});
    }
    if (auto* v = s->get_for()) {
        return cu_.add_node<Stmt>(loc, Stmt::For{
            clone_stmt(v->init), clone_expr(v->condition), clone_stmt(v->next), clone_stmt(v->body)});
    }
    if (s->is_return()) return cu_.add_node<Stmt>(loc, Stmt::Return{});
    if (s->is_break()) return cu_.add_node<Stmt>(loc, Stmt::Break{});
    if (s->is_continue()) return cu_.add_node<Stmt>(loc, Stmt::Continue{});
    if (auto* v = s->get_flag()) return cu_.add_node<Stmt>(loc, Stmt::Flag{v->label});
    if (auto* v = s->get_goto()) return cu_.add_node<Stmt>(loc, Stmt::Goto{v->label});
    if (auto* v = s->get_asm()) return cu_.add_node<Stmt>(loc, Stmt::Asm{v->code});
    if (auto* v = s->get_keep()) return cu_.add_node<Stmt>(loc, Stmt::Keep{v->tokens});
    if (auto* v = s->get_var_decl()) {
        return cu_.add_node<Stmt>(loc, Stmt::VarDecl{clone_varlist(v->vars)});
    }
    return cu_.add_node<Stmt>(loc, s->data);
}

VarList* SemanticAnalyzer::clone_varlist(VarList* vl) {
    if (!vl) return nullptr;
    Expr* t = clone_type(vl->type);
    std::vector<VarItem*> vars;
    vars.reserve(vl->vars.size());
    for (auto* vi : vl->vars) {
        if (!vi) { vars.push_back(nullptr); continue; }
        vars.push_back(cu_.add_node<VarItem>(
            vi->loc, vi->name, clone_expr(vi->init), t));
    }
    return cu_.add_node<VarList>(vl->loc, vl->modfs, t, std::move(vars));
}

Func* SemanticAnalyzer::clone_func(Func* f) {
    std::vector<VarList*> params;
    params.reserve(f->params.size());
    for (auto* p : f->params) params.push_back(clone_varlist(p));
    std::vector<VarList*> returns;
    returns.reserve(f->returns.size());
    for (auto* r : f->returns) returns.push_back(clone_varlist(r));
    return cu_.add_node<Func>(f->loc, f->name, f->modfs,
        std::move(params), std::move(returns), clone_stmt(f->body));
}

// flag / toflag 的 label 检查：遵循 C++ 的跳转规则
// - 目标 label 必须在本函数体内定义
// - 不得跳入某个作用域内部（即目标所在作用域必须是跳转点作用域的祖先或自身）
// - 不得跳入循环内部
namespace {
// 声明的名字键：变量列表取各变量名，特殊方法取内部名，其余取声明名
std::vector<std::string> decl_key_names(const Decl* d) {
    std::vector<std::string> names;
    if (!d) return names;
    if (auto* vl = dynamic_cast<const VarList*>(d)) {
        for (auto* v : vl->vars) {
            if (v) names.push_back(std::string(v->name));
        }
        return names;
    }
    if (auto* sm = dynamic_cast<const SpecialMethod*>(d)) {
        switch (sm->kind) {
            case SpecialMethod::Kind::Drop:   names.push_back("DROP"); break;
            case SpecialMethod::Kind::Ref:    names.push_back("REF"); break;
            case SpecialMethod::Kind::Deref:  names.push_back("DEREF"); break;
            case SpecialMethod::Kind::Access: names.push_back("ACCESS"); break;
            default: break;
        }
        return names;
    }
    if (!d->name.empty()) names.push_back(std::string(d->name));
    return names;
}

struct JumpPoint {
    std::string label;
    std::vector<int> scope_path;
    int loop_depth = 0;
    bool is_flag = false;
    Location loc;
};
}

void SemanticAnalyzer::check_jumps() {
    std::unordered_set<Symbol*> init_tracked;
    (void)init_tracked;
    for (const BodyInfo& info : bodies_) {
        if (!info.body) continue;
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
        scan(info.body, {}, 0);

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
                    error(g.loc, "jump into the scope of label '" + g.label + "'");
                }
                if (f.loop_depth > g.loop_depth) {
                    error(g.loc, "jump into a loop at label '" + g.label + "'");
                }
            }
            if (!found) {
                error(g.loc, "jump to undefined label '" + g.label + "'");
            }
        }
    }
}

// 变量初始化检查：不得读取未赋值过的变量
void SemanticAnalyzer::check_initialization() {
    for (const BodyInfo& info : bodies_) {
        if (!info.body) continue;
        InitCtx ctx;
        // 参数视为已赋值
        for (VarList* vl : info.params) {
            if (!vl) continue;
            for (VarItem* vi : vl->vars) {
                Symbol* sym = var_symbols_.count(vi) ? var_symbols_[vi] : nullptr;
                if (sym) { ctx.declared.insert(sym); ctx.assigned.insert(sym); }
            }
        }
        // 返回变量：声明处已赋值则跳过，否则要求函数体内至少赋值一次
        for (VarList* vl : info.returns) {
            if (!vl) continue;
            for (VarItem* vi : vl->vars) {
                Symbol* sym = var_symbols_.count(vi) ? var_symbols_[vi] : nullptr;
                if (!sym) continue;
                ctx.declared.insert(sym);
                if (!vi->init && !body_assigns_return(info.body, sym)) {
                    error(vi->loc, "return variable '" + std::string(vi->name) +
                          "' is never assigned");
                }
            }
        }
        init_stmt(ctx, info.body);
    }
}

void SemanticAnalyzer::init_stmt(InitCtx& ctx, Stmt* s) {
    if (!s) return;
    if (auto* b = s->get_block()) {
        for (Stmt* x : b->stmts) init_stmt(ctx, x);
        return;
    }
    if (auto* e = s->get_expr()) { init_expr(ctx, e->expr); return; }
    if (auto* i = s->get_if()) {
        init_expr(ctx, i->condition);
        InitCtx then_ctx = ctx, else_ctx = ctx;
        init_stmt(then_ctx, i->then_branch);
        init_stmt(else_ctx, i->else_branch);
        // 分支合并：仅在两个分支都已赋值的变量才算已赋值
        std::unordered_set<Symbol*> merged;
        for (Symbol* sym : then_ctx.assigned) {
            if (else_ctx.assigned.count(sym)) merged.insert(sym);
        }
        for (Symbol* sym : ctx.assigned) merged.insert(sym);
        ctx.assigned = std::move(merged);
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
    if (s->is_return()) return;
    if (auto* v = s->get_var_decl()) {
        VarList* vl = v->vars;
        if (!vl) return;
        for (VarItem* vi : vl->vars) {
            Symbol* sym = var_symbols_.count(vi) ? var_symbols_[vi] : nullptr;
            if (vi->init) {
                init_expr(ctx, vi->init);
                if (sym) ctx.assigned.insert(sym);
            } else {
                // 类实例无初值时默认构造，视为已赋值
                Expr* vt = vi->type ? vi->type : vl->type;
                if (sym && vt && vt->resolved_decl &&
                    vt->resolved_decl->kind == DeclKind::Class)
                    ctx.assigned.insert(sym);
            }
            if (sym) ctx.declared.insert(sym);
        }
        return;
    }
}

// 返回变量的"至少赋值一次"扫描：不走流敏感的 assigned 合并
bool SemanticAnalyzer::body_assigns_return(Stmt* s, Symbol* target) {
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
            Symbol* sym = var_symbols_.count(vi) ? var_symbols_[vi] : nullptr;
            if (sym == target && vi->init) return true;
            if (vi->init && expr_assigns_return(vi->init, target)) return true;
        }
        return false;
    }
    return false;
}

bool SemanticAnalyzer::expr_assigns_return(Expr* e, Symbol* target) {
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
    if (auto* cl = e->get_closure()) return body_assigns_return(cl->body, target);
    return false;
}

void SemanticAnalyzer::init_assign_target(InitCtx& ctx, Expr* e) {
    if (!e) return;
    if (e->sym && ctx.declared.count(e->sym)) {
        ctx.assigned.insert(e->sym);
    }
}

void SemanticAnalyzer::init_expr(InitCtx& ctx, Expr* e) {
    if (!e) return;
    if (auto* id = e->get_identifier()) {
        (void)id;
        if (e->sym && ctx.declared.count(e->sym) && !ctx.assigned.count(e->sym)) {
            error(e->loc, "use of uninitialized variable '" +
                  sym_display_name(e->sym) + "'");
        }
        return;
    }
    if (auto* a = e->get_assign()) {
        init_expr(ctx, a->value);
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
    if (auto* cl = e->get_closure()) {
        // 闭包体独立处理，捕获变量在此处按使用检查
        InitCtx body_ctx;
        for (VarList* vl : cl->params) {
            if (!vl) continue;
            for (VarItem* vi : vl->vars) {
                Symbol* sym = var_symbols_.count(vi) ? var_symbols_[vi] : nullptr;
                if (sym) { body_ctx.declared.insert(sym); body_ctx.assigned.insert(sym); }
            }
        }
        init_stmt(body_ctx, cl->body);
        return;
    }
}

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

Decl* SemanticAnalyzer::clone_decl(Decl* d) {
    if (!d) return nullptr;
    switch (d->kind) {
        case DeclKind::VarList:
            return clone_varlist(static_cast<VarList*>(d));
        case DeclKind::Func:
            return clone_func(static_cast<Func*>(d));
        case DeclKind::Operator: {
            auto* op = static_cast<Operator*>(d);
            Func* f = clone_func(op);
            return cu_.add_node<Operator>(op->loc, op->op, op->name, op->modfs,
                f->params, f->returns, f->body);
        }
        case DeclKind::SpecialMethod: {
            auto* sm = static_cast<SpecialMethod*>(d);
            std::vector<VarList*> params;
            params.reserve(sm->params.size());
            for (auto* p : sm->params) params.push_back(clone_varlist(p));
            return cu_.add_node<SpecialMethod>(sm->loc, sm->kind, sm->modfs,
                clone_stmt(sm->body), std::move(params));
        }
        case DeclKind::Conversion: {
            auto* cf = static_cast<Conversion*>(d);
            std::vector<VarList*> params;
            for (auto* p : cf->params) params.push_back(clone_varlist(p));
            std::vector<VarList*> returns;
            for (auto* r : cf->returns) returns.push_back(clone_varlist(r));
            return cu_.add_node<Conversion>(cf->loc, cf->name, cf->modfs,
                std::move(params), std::move(returns), clone_stmt(cf->body));
        }
        case DeclKind::Keep: {
            auto* k = static_cast<Keep*>(d);
            return cu_.add_node<Keep>(k->loc, k->code, k->modfs);
        }
        case DeclKind::Class: {
            auto* c = static_cast<Class*>(d);
            std::vector<Decl*> members;
            members.reserve(c->members.size());
            for (auto* m : c->members) members.push_back(clone_decl(m));
            return cu_.add_node<Class>(c->loc, c->name, c->modfs, std::move(members));
        }
        case DeclKind::Enum: {
            auto* e = static_cast<Enum*>(d);
            std::vector<EnumVariant*> variants;
            variants.reserve(e->variants.size());
            for (auto* v : e->variants) {
                variants.push_back(cu_.add_node<EnumVariant>(v->loc, v->name));
            }
            return cu_.add_node<Enum>(e->loc, e->name, e->modfs, std::move(variants));
        }
        default:
            return d;
    }
}

// 修饰注入：`#a @b #c` 的优先级从左到右递减（前置注入 > 基类 > 尾随注入）。
// 高优先级来源先占用名字，低优先级同名的成员被忽略。
void SemanticAnalyzer::apply_injections(Class* cls) {
    if (!cls) return;
    if (cls->injects.empty() && cls->trailing_injects.empty()) return;

    auto resolve = [&](Modification* mod) -> Class* {
        if (!mod) return nullptr;
        Symbol* sym = symbols_.lookup_member(std::string(mod->name));
        Decl* d = sym ? sym->get_decl() : nullptr;
        if (!d || d->kind != DeclKind::Class) {
            error(mod->loc, "cannot inject from '" + std::string(mod->name) +
                  "': not a class");
            return nullptr;
        }
        return static_cast<Class*>(d);
    };

    // 来源按优先级从高到低：前置注入（左→右）、基类自身、尾随注入（左→右）
    std::vector<Class*> sources;
    for (Modification* mod : cls->injects) {
        if (Class* c = resolve(mod)) sources.push_back(c);
    }
    sources.push_back(cls);
    for (Modification* mod : cls->trailing_injects) {
        if (Class* c = resolve(mod)) sources.push_back(c);
    }

    std::vector<Decl*> result;
    std::unordered_map<std::string, size_t> index;
    for (Class* src : sources) {
        for (Decl* m : src->members) {
            if (!m) continue;
            bool claimed = false;
            for (const std::string& n : decl_key_names(m)) {
                if (index.count(n)) { claimed = true; break; }
            }
            if (claimed) continue;
            // 基类自身成员保持原指针，注入来源的成员克隆，避免共享 AST 节点
            Decl* placed = (src == cls) ? m : clone_decl(m);
            for (const std::string& n : decl_key_names(placed)) {
                index[n] = result.size();
            }
            result.push_back(placed);
        }
    }
    cls->members = std::move(result);
}

Scope* SemanticAnalyzer::clone_scope(Scope* src) {
    if (!src) return nullptr;
    auto* dst = new Scope(src->name);
    dst->locals = src->locals;
    dst->members = src->members;
    for (const auto& ch : src->children) {
        auto* c = clone_scope(ch.get());
        c->parent = dst;
        dst->children.emplace_back(c);
    }
    return dst;
}

} // namespace cx
