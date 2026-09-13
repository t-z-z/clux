#include "../include/semantic.h"
#include "ast.h"

namespace cx {

// 类型名节点（Identifier / 限定名 Member 链）的完整名字
static std::string type_name_string(Expr* t) {
    if (!t) return "";
    if (auto* id = t->get_identifier()) return std::string(id->name);
    if (auto* m = t->get_member())
        return type_name_string(m->target) + "::" + std::string(m->member);
    return "";
}

// 从解析后的类型节点取成员作用域
static Scope* named_type_scope(Expr* t) {
    if (!t) return nullptr;
    if (t->resolved_scope) return t->resolved_scope;
    if (t->resolved_decl) {
        if (Cont* cont = dynamic_cast<Cont*>(t->resolved_decl)) return cont->scope;
    }
    return nullptr;
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
    if (!t || !t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k != PrimitiveTypeKind::Bool && k != PrimitiveTypeKind::Type && k != PrimitiveTypeKind::Void;
}

bool SemanticAnalyzer::is_integer_type(Expr* t) {
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
    if (!t || !t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k == PrimitiveTypeKind::Float16 || k == PrimitiveTypeKind::Float || k == PrimitiveTypeKind::Double;
}

bool SemanticAnalyzer::is_bool_type(Expr* t) {
    return t && t->is_primitive() && t->get_primitive()->kind == PrimitiveTypeKind::Bool;
}

bool SemanticAnalyzer::is_same_type(Expr* a, Expr* b) {
    if (!a || !b) return false;
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
    if (a->is_string_literal_type() && b->is_string_literal_type()) return true;
    if (a->is_void() && b->is_void()) return true;
    return false;
}

bool SemanticAnalyzer::is_integer_promotable(Expr* t) {
    if (!t || !t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k == PrimitiveTypeKind::Int8 || k == PrimitiveTypeKind::Short
        || k == PrimitiveTypeKind::UInt8 || k == PrimitiveTypeKind::UChar
        || k == PrimitiveTypeKind::Char;
}

bool SemanticAnalyzer::is_void_type(Expr* t) {
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
    diagnostics_.clear();
    call_resolutions_.clear();
    has_error_ = false;
    collect();
    walk(cu_);
    return !has_error_;
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

void SemanticAnalyzer::collect() {
    for (Decl* decl : cu_.top_decls) {
        collect_decl(decl);
    }
}

void SemanticAnalyzer::collect_decl(Decl* decl) {
    if (!decl) return;

    switch (decl->kind) {
        case DeclKind::VarList: {
            auto* vl = static_cast<VarList*>(decl);
            for (auto* var : vl->vars) {
                Symbol sym(SymbolKind::Variable, var);
                if (!symbols_.insert_member(std::string(var->name), std::move(sym))) {
                    error(vl->loc, "duplicate declaration of variable '" + std::string(var->name) + "'");
                }
            }
            break;
        }

        case DeclKind::Func:
        case DeclKind::Operator: {
            register_func(*static_cast<Func*>(decl));
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
                symbols_.insert_and_push(name, Symbol(SymbolKind::Class, cls));
            }
            for (Decl* member : cls->members) {
                collect_decl(member);
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
                symbols_.insert_and_push(name, Symbol(SymbolKind::Namespace, ns));
            }
            for (Decl* member : ns->members) {
                collect_decl(member);
            }
            symbols_.pop_scope();
            break;
        }

        case DeclKind::Extern: {
            auto* ext = static_cast<Extern*>(decl);
            ext->scope = symbols_.current_scope();
            for (Decl* member : ext->members) {
                collect_decl(member);
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
            register_special(*static_cast<SpecialMethod*>(decl));
            break;
        }
        default:
            break;
    }

}

void SemanticAnalyzer::register_func(Func& f) {
    std::string name = std::string(f.name);
    Scope* scope = symbols_.current_scope();

    if (scope && scope->lookup_member(name)) {
        error(f.loc, "duplicate declaration of function '" + name + "'");
        return;
    }

    std::string key = build_callkey(name, f.params);
    if (scope && scope->lookup_member(key)) {
        error(f.loc, "duplicate declaration of function '" + name + "'");
        return;
    }
    Symbol sym(SymbolKind::Function, &f);
    symbols_.insert_member(key, std::move(sym));
}

void SemanticAnalyzer::register_special(SpecialMethod& sm) {
    std::string internal_name;
    switch (sm.kind) {
        case SpecialMethod::Kind::Drop:       internal_name = "DROP";       break;
        case SpecialMethod::Kind::Ref:        internal_name = "REF";        break;
        case SpecialMethod::Kind::Deref:      internal_name = "DEREF";      break;
        case SpecialMethod::Kind::Access:     internal_name = "ACCESS";     break;
        case SpecialMethod::Kind::Conversion: 
        if (sm.conv) internal_name = "conversion_" + type_to_key(*sm.conv->type);else return; 
        break;
        default: error(sm.loc, "unknown special method"); return;
    }

    Scope* scope = symbols_.current_scope();
    if (scope && scope->lookup_member(internal_name)) {
        error(sm.loc, "duplicate special method '" + internal_name + "'");
        return;
    }

    Symbol sym(SymbolKind::Function, &sm);
    symbols_.insert_member(internal_name, std::move(sym));
}

std::string SemanticAnalyzer::type_to_key(Expr& t) {
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
void SemanticAnalyzer::visit_func(Func&) {}

void SemanticAnalyzer::visit_special_method(SpecialMethod&) {}

void SemanticAnalyzer::visit_var_list(VarList& vl) {
    // 动态长度数组检查：运行时长度必须显式声明存储方式
    if (vl.type && vl.type->is_array_type()) {
        auto* arr = vl.type->get_array_type();
        if (arr->length) {
            // 检查长度是否为编译期常量（字面量或标识符引用常量）
            bool is_const_length = arr->length->is_literal();
            if (!is_const_length) {
                bool has_alloca = vl.modfs.has(ModfKind::Alloca);
                bool has_malloc = vl.modfs.has(ModfKind::Malloc);
                if (!has_alloca && !has_malloc) {
                    warning(vl.loc, "array with runtime length must use 'alloca' or 'malloc' modifier");
                }
            }
        }
    }
    for (VarItem* var : vl.vars) {
        if (!symbols_.insert_local(var->name, Symbol(SymbolKind::Variable, var))) {
            error(var->loc, "duplicate variable '" + std::string(var->name) + "'");
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

    // 类的 conversion 特殊方法
    if (src->resolved_decl && src->resolved_decl->kind == DeclKind::Class) {
        Class& cls = static_cast<Class&>(*src->resolved_decl);
        if (cls.scope) {
            Symbol* cands = cls.scope->lookup_member("conversion_" + type_to_key(*dst));
            if (cands) {
                if (auto* sm = cands->get<SpecialMethod>()) {
                    if (sm->conv && sm->conv->type) {
                        return is_same_type(sm->conv->type, dst);
                    }
                }
                return true;
            }
        }
    }

    return false;
}

void SemanticAnalyzer::visit_expr_binary(Expr::Binary& b) {
    Expr* lt = b.left ? b.left->type : nullptr;
    Expr* rt = b.right ? b.right->type : nullptr;
    if (!lt || !rt) return;

    if (Symbol* s = find_operator_overload(b.op, *lt, *rt)) {
        Expr* overload_ret = typeVoid;
        if (auto* op = s->get<Operator>()) {
            if (!op->returns.empty() && op->returns[0] && op->returns[0]->type)
                overload_ret = op->returns[0]->type;
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
            if (!op->returns.empty() && op->returns[0] && op->returns[0]->type) {
                current_expr->type = op->returns[0]->type;
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

    Expr* lt = a.target->type;
    Expr* rt = a.value->type;

    if (lt && rt && !is_same_type(lt, rt)) {
        if (!can_convert_to(rt, lt)) {
            warning(current_expr->loc, "implicit conversion in assignment");
        }
    }

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
        CallResolution resol;
        resol.call = call_expr;
        resol.type = typeVoid;
        call_resolutions_.push_back(std::move(resol));
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
        call_expr->is_lvalue = false;

        CallResolution resol;
        resol.call = call_expr;
        resol.type = call_expr->type;
        call_resolutions_.push_back(std::move(resol));
        return;
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

    // 2a：类构造，在类自己的 scope 里查 INIT_...
    if (callee_sym && callee_sym->kind == SymbolKind::Class) {
        Scope* cls_scope = callee_sym->scope;
        Decl* cls_decl = callee_sym->get_decl();
        if (!cls_scope) {
            fail("class '" + name + "' has no scope");
            return;
        }

        std::string key = build_callkey("INIT", c.args);
        Symbol* init_sym = SymbolTable::lookup_member_in(cls_scope, key);
        if (!init_sym || init_sym->kind != SymbolKind::Function) {
            fail("no matching constructor for class '" + name + "'");
            return;
        }

        call_expr->sym = init_sym;
        call_expr->type = make_named_type(call_expr->loc,
            cls_decl ? cls_decl->name : std::string_view(),
            cls_decl, cls_scope);
        call_expr->is_lvalue = false;

        CallResolution resol;
        resol.call = call_expr;
        resol.type = call_expr->type;
        if (Decl* d = init_sym->get_decl()) {
            if (d->kind == DeclKind::Func || d->kind == DeclKind::Operator)
                resol.resolved_func = static_cast<Func*>(d);
        }
        call_resolutions_.push_back(std::move(resol));
        return;
    }

    // 2b：普通函数 / 操作符，用 mangled key 在当前作用域链里查
    std::string key = build_callkey(name, c.args);
    Symbol* f_sym = symbols_.lookup_member(key);
    if (!f_sym || f_sym->kind != SymbolKind::Function) {
        fail("no matching overload for call to '" + name + "'");
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

    Expr* ret = typeVoid;
    if (!func->returns.empty() && func->returns[0] &&
        func->returns[0]->type) {
        ret = func->returns[0]->type;
    }

    call_expr->sym = f_sym;
    call_expr->type = ret;
    call_expr->is_lvalue = false;

    CallResolution resol;
    resol.call = call_expr;
    resol.resolved_func = func;
    resol.type = ret;
    call_resolutions_.push_back(std::move(resol));
}

const CallResolution* SemanticAnalyzer::find_call(Expr* e) const {
    for (const auto& res : call_resolutions_) {
        if (res.call == e) return &res;
    }
    return nullptr;
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
    std::vector<Expr*> param_types;
    for (auto* p : clo.params) {
        if (p && p->type) {
            param_types.push_back(p->type);
        }
    }

    Expr* ret_type = nullptr;
    if (!clo.returns.empty() && clo.returns[0] && clo.returns[0]->type) {
        ret_type = clo.returns[0]->type;
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
void SemanticAnalyzer::visit_stmt_spawn(Stmt::Spawn&) {}

void SemanticAnalyzer::visit_stmt_wait(Stmt::Wait& wait_stmt) {
    if (!wait_stmt.condition) {
        error(current_stmt->loc, "wait statement missing condition");
        return;
    }
    
    Expr* cond_type = wait_stmt.condition->type;
    if (cond_type && !is_bool_type(cond_type)) {
        warning(wait_stmt.condition->loc, "wait condition is not bool, implicit conversion");
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
void SemanticAnalyzer::visit_type_void(Expr::Void&) {
    // Void is valid as a return type or pointer pointee context, checked by callers
}

void SemanticAnalyzer::visit_type_inferred(Expr::Inferred&) {}

Expr* SemanticAnalyzer::clone_type(Expr* t) {
    if (!t) return nullptr;
    Location loc = t->loc;
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
    if (auto* v = e->get_closure()) {
        std::vector<VarList*> params;
        for (auto* p : v->params) params.push_back(clone_varlist(p));
        std::vector<VarList*> returns;
        for (auto* r : v->returns) returns.push_back(clone_varlist(r));
        return cu_.add_node<Expr>(loc, Expr::Closure{std::move(params), std::move(returns),
            clone_stmt(v->body), v->capture});
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
    if (auto* v = s->get_spawn()) return cu_.add_node<Stmt>(loc, Stmt::Spawn{clone_stmt(v->body)});
    if (auto* v = s->get_wait()) return cu_.add_node<Stmt>(loc, Stmt::Wait{clone_expr(v->condition)});
    return cu_.add_node<Stmt>(loc, s->data);
}

VarItem* SemanticAnalyzer::clone_varitem(VarItem* vi) {
    if (!vi) return nullptr;
    return cu_.add_node<VarItem>(vi->loc, vi->name, clone_expr(vi->init));
}

VarList* SemanticAnalyzer::clone_varlist(VarList* vl) {
    if (!vl) return nullptr;
    std::vector<VarItem*> vars;
    vars.reserve(vl->vars.size());
    for (auto* vi : vl->vars) vars.push_back(clone_varitem(vi));
    return cu_.add_node<VarList>(vl->loc, vl->modfs, clone_type(vl->type), std::move(vars));
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
