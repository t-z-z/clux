#include "../include/semantic.h"

namespace cx {

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

bool SemanticAnalyzer::is_numeric_type(Type* t) {
    if (!t || !t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k != PrimitiveTypeKind::Bool && k != PrimitiveTypeKind::Type && k != PrimitiveTypeKind::Void;
}

bool SemanticAnalyzer::is_integer_type(Type* t) {
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

bool SemanticAnalyzer::is_float_type(Type* t) {
    if (!t || !t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k == PrimitiveTypeKind::Float16 || k == PrimitiveTypeKind::Float || k == PrimitiveTypeKind::Double;
}

bool SemanticAnalyzer::is_bool_type(Type* t) {
    return t && t->is_primitive() && t->get_primitive()->kind == PrimitiveTypeKind::Bool;
}

bool SemanticAnalyzer::is_same_type(Type* a, Type* b) {
    if (!a || !b) return false;
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
    if (a->is_custom() && b->is_custom()) {
        if (a->get_custom()->resolved_decl && b->get_custom()->resolved_decl) {
            return a->get_custom()->resolved_decl == b->get_custom()->resolved_decl;
        }
        return a->get_custom()->name == b->get_custom()->name;
    }
    if (a->is_string_literal() && b->is_string_literal()) return true;
    if (a->is_void() && b->is_void()) return true;
    return false;
}

bool SemanticAnalyzer::is_integer_promotable(Type* t) {
    if (!t || !t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k == PrimitiveTypeKind::Int8 || k == PrimitiveTypeKind::Short
        || k == PrimitiveTypeKind::UInt8 || k == PrimitiveTypeKind::UChar
        || k == PrimitiveTypeKind::Char;
}

bool SemanticAnalyzer::is_void_type(Type* t) {
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

Type* SemanticAnalyzer::make_type(const Location& loc, Type::Variant v) {
    return cu_.add_node<Type>(loc, std::move(v));
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
            } else if (item->type->is_custom()) {
                key += type_to_key(*item->type);
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
                var->type = make_type(var->loc,
                    Type::Custom{e->name, {}, e, enum_scope});
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
        case SpecialMethod::Kind::Conversion: internal_name = "conversion"; break;
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

std::string SemanticAnalyzer::type_to_key(Type& t) {
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
    if (t.is_string_literal()) return "string";
    if (t.is_pointer()) {
        return type_to_key(*t.get_pointer()->pointee) + "*";
    }
    if (t.is_reference()) {
        return type_to_key(*t.get_reference()->target) + "&";
    }
    if (t.is_array()) {
        auto* arr = t.get_array();
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
    if (t.is_custom()) {
        return "%" + std::string(t.get_custom()->name);
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
    if (vl.type && vl.type->is_array()) {
        auto* arr = vl.type->get_array();
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
                        Type::FuncRef{static_cast<Func*>(d)}
                    );
            }
            current_expr->is_lvalue = false;
        } else if (member->kind == SymbolKind::Enum) {
            if (Enum* e = member->get<Enum>()) {
                current_expr->type = make_type(current_expr->loc,
                    Type::Custom{e->name, {}, e, e->scope}
                );
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

Symbol* SemanticAnalyzer::find_operator_overload(OpKind op, Type& type, Type& other) {
    auto* cus = type.get_custom();
    if (!cus || !cus->resolved_decl) return nullptr;
    Scope* scope = static_cast<Class*>(cus->resolved_decl)->scope;
    if (!scope) return nullptr;

    std::string key = std::string(opkind_to_sv(op)) + "_" + type_to_key(other);
    Symbol* cls_sym = scope->lookup_member(key);
    if (cls_sym) return cls_sym;
    key = std::string(opkind_to_sv(op)) + "_" + type_to_key(type) + type_to_key(other);
    return symbols_.lookup_member(key);
}

Symbol* SemanticAnalyzer::find_operator_overload(OpKind op, Type& type) {
    auto* cus = type.get_custom();
    if (!cus || !cus->resolved_decl) return nullptr;
    Scope* scope = static_cast<Class*>(cus->resolved_decl)->scope;
    if (!scope) return nullptr;

    std::string key = std::string(opkind_to_sv(op)) + "_";
    Symbol* cls_sym = scope->lookup_member(key);
    if(cls_sym) return cls_sym;
    key += type_to_key(type);
    return symbols_.lookup_member(key);
}

bool SemanticAnalyzer::can_convert_to(Type* src, Type* dst) {
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
    if (auto* c = src->get_custom()) {
        if (c->resolved_decl && c->resolved_decl->kind == DeclKind::Class) {
            Class& cls = static_cast<Class&>(*c->resolved_decl);
            if (cls.scope) {
                Symbol* cands = cls.scope->lookup_member("conversion");
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
    }

    return false;
}

void SemanticAnalyzer::visit_expr_binary(Expr::Binary& b) {
    Type* lt = b.left ? b.left->type : nullptr;
    Type* rt = b.right ? b.right->type : nullptr;
    if (!lt || !rt) return;
    
    if (Symbol* s = find_operator_overload(b.op, *lt, *rt)) {
        Type* overload_ret = typeVoid;
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
            } else if (b.op == OpKind::Plus && (lt->is_string_literal() || rt->is_string_literal())) {
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

    Type* lt = a.target->type;
    Type* rt = a.value->type;

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
            Type* at = c.args[i] ? c.args[i]->type : nullptr;
            Type* pt = fp->params[i];
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
        call_expr->type = make_type(call_expr->loc,
            Type::Custom{cls_decl ? cls_decl->name : std::string_view(),
                         {}, cls_decl});
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

    Type* ret = typeVoid;
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

    Type* tt = idx.target->type;
    if (!tt) {
        current_expr->type = nullptr;
        return;
    }

    if (tt->is_array()) {
        current_expr->type = tt->get_array()->element;
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
    Scope* target_scope = nullptr;
    // 优先从表达式类型（Custom）解析目标作用域，sym 仅作兜底
    if (m.target->type) {
        if (auto* cus = m.target->type->get_custom()) {
            if (cus->resolved_scope) {
                target_scope = cus->resolved_scope;
            } else if (cus->resolved_decl) {
                if (Cont* cont = dynamic_cast<Cont*>(cus->resolved_decl)) {
                    target_scope = cont->scope;
                }
            }
        }
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
                        Type::FuncRef{static_cast<Func*>(d)}
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
        Type* src = cast.operand->type;
        Type* dst = cast.target_type;
        if (is_same_type(src, dst)) {
            warning(current_expr->loc, "unnecessary cast, types are identical");
        }
    }
}

void SemanticAnalyzer::visit_expr_array(Expr::Array& arr) {
    if (arr.members.empty()) {
        if (arr.fill_value && arr.fill_value->type) {
            current_expr->type = make_type(current_expr->loc,
                Type::Array{arr.fill_value->type, nullptr});
        } else {
            current_expr->type = make_type(current_expr->loc,
                Type::Array{typeInt, nullptr});
        }
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
        Type::Array{elem_type ? elem_type : typeInt,
                    make_int_literal(current_expr->loc, arr.members.size())});
    current_expr->is_lvalue = false;
}

void SemanticAnalyzer::visit_expr_closure(Expr::Closure& clo) {
    std::vector<Type*> param_types;
    for (auto* p : clo.params) {
        if (p && p->type) {
            param_types.push_back(p->type);
        }
    }

    Type* ret_type = nullptr;
    if (!clo.returns.empty() && clo.returns[0] && clo.returns[0]->type) {
        ret_type = clo.returns[0]->type;
    }

    current_expr->type = make_type(current_expr->loc,
        Type::FuncPtr{std::move(param_types), ret_type});
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

void SemanticAnalyzer::visit(Modification&) {}
void SemanticAnalyzer::visit(Stmt&) {}
void SemanticAnalyzer::visit_stmt_block(Stmt::Block&) {}
void SemanticAnalyzer::visit_stmt_expr(Stmt::ExprStmt&) {}

void SemanticAnalyzer::visit_stmt_if(Stmt::If& if_stmt) {
    if (!if_stmt.condition) {
        error(current_stmt->loc, "if statement missing condition");
        return;
    }
    
    Type* cond_type = if_stmt.condition->type;
    if (cond_type && !is_bool_type(cond_type)) {
        warning(if_stmt.condition->loc, "if condition is not bool, implicit conversion");
    }
}

void SemanticAnalyzer::visit_stmt_while(Stmt::While& while_stmt) {
    if (!while_stmt.condition) {
        error(current_stmt->loc, "while statement missing condition");
        return;
    }
    
    Type* cond_type = while_stmt.condition->type;
    if (cond_type && !is_bool_type(cond_type)) {
        warning(while_stmt.condition->loc, "while condition is not bool, implicit conversion");
    }
}

void SemanticAnalyzer::visit_stmt_for(Stmt::For& for_stmt) {
    if (for_stmt.condition) {
        Type* cond_type = for_stmt.condition->type;
        if (cond_type && !is_bool_type(cond_type)) {
            warning(for_stmt.condition->loc, "for condition is not bool, implicit conversion");
        }
    }
}

void SemanticAnalyzer::visit_stmt_for_in(Stmt::ForIn& for_in_stmt) {
    if (!for_in_stmt.collection) {
        error(current_stmt->loc, "for-in statement missing collection");
        return;
    }
    
    Type* coll_type = for_in_stmt.collection->type;
    if (!coll_type) {
        error(current_stmt->loc, "for-in collection has no type");
        return;
    }
    
    bool is_iterable = false;
    if (coll_type->is_array()) {
        is_iterable = true;
    } else if (coll_type->is_pointer()) {
        is_iterable = true;
    } else if (coll_type->is_custom()) {
        is_iterable = true;
    }
    
    if (!is_iterable) {
        error(current_stmt->loc, "for-in requires iterable collection (array, pointer, or iterable type)");
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
    
    Type* cond_type = wait_stmt.condition->type;
    if (cond_type && !is_bool_type(cond_type)) {
        warning(wait_stmt.condition->loc, "wait condition is not bool, implicit conversion");
    }
}

void SemanticAnalyzer::visit(Type&) {}
// void 是合法返回类型/指针 pointee：解析器将 void 生成 Primitive{Void}，
// 无法从节点本身判断上下文，故不在此处拒绝
void SemanticAnalyzer::visit_type_primitive(Type::Primitive&) {}
// ast本来应该有指针指向的节点如果是nullptr，说明已经在parser报过错了，不要重复报错
void SemanticAnalyzer::visit_type_pointer(Type::Pointer&) {}
void SemanticAnalyzer::visit_type_array(Type::Array&) {}
void SemanticAnalyzer::visit_type_func_ptr(Type::FuncPtr&) {}
void SemanticAnalyzer::visit_type_func_ref(Type::FuncRef&) {}
void SemanticAnalyzer::visit_type_reference(Type::Reference&) {}

void SemanticAnalyzer::visit_type_string_literal(Type::StringLiteral&) {
    // StringLiteral is a valid type, no additional checks needed
}
void SemanticAnalyzer::visit_type_void(Type::Void&) {
    // Void is valid as a return type or pointer pointee context, checked by callers
}

void SemanticAnalyzer::visit_type_custom(Type::Custom& c) {
    std::string name(c.name);
    if (Symbol* s = symbols_.lookup_member(name)) {
        if (Decl* d = s->get_decl()) {
            c.resolved_decl = d;
            if (d->kind == DeclKind::Enum) {
                c.resolved_scope = static_cast<Enum*>(d)->scope;
            }
            // 如果有模板实参，实例化类并提取新符号表
            if (!c.args.empty() && d->kind == DeclKind::Class) {
                auto* cls = static_cast<Class*>(d);
                Class* inst = instantiate_class(cls, c.args);
                if (inst) {
                    c.resolved_scope = inst->scope;
                }
            }
        } else {
            error(current_type->loc, "unknown type '" + name + "'");
        }
    } else {
        error(current_type->loc, "unknown type '" + name + "'");
    }
}

Class* SemanticAnalyzer::instantiate_class(Class* cls, const std::vector<ASTNode*>& tpl_args) {
    (void)tpl_args; // Unused after removing generics
    if (!cls) return cls;

    // 创建一个代理 Class 指向新 scope
    Class* inst = cu_.add_node<Class>(
        cls->loc, cls->name, cls->modfs,
        std::vector<Decl*>()
    );
    inst->scope = clone_scope(cls->scope);
    return inst;
}

void SemanticAnalyzer::visit_type_inferred(Type::Inferred&) {}

// 模板实例化

Type* SemanticAnalyzer::clone_type(Type* t) {
    if (!t) return nullptr;
    Location loc = t->loc;
    if (auto* p = t->get_primitive()) return cu_.add_node<Type>(loc, Type::Primitive{p->kind});
    if (t->is_string_literal()) return cu_.add_node<Type>(loc, Type::StringLiteral{});
    if (t->is_void()) return cu_.add_node<Type>(loc, Type::Void{});
    if (t->is_inferred()) return cu_.add_node<Type>(loc, Type::Inferred{});
    if (auto* p = t->get_pointer()) return cu_.add_node<Type>(loc, Type::Pointer{clone_type(p->pointee)});
    if (auto* r = t->get_reference()) return cu_.add_node<Type>(loc, Type::Reference{clone_type(r->target)});
    if (auto* a = t->get_array()) {
        return cu_.add_node<Type>(loc, Type::Array{clone_type(a->element), clone_expr(a->length)});
    }
    if (auto* f = t->get_func_ptr()) {
        std::vector<Type*> params;
        params.reserve(f->params.size());
        for (auto* p : f->params) params.push_back(clone_type(p));
        return cu_.add_node<Type>(loc, Type::FuncPtr{std::move(params), clone_type(f->return_type)});
    }
    if (auto* c = t->get_custom()) {
        std::vector<ASTNode*> args;
        args.reserve(c->args.size());
        for (auto* a : c->args) {
            if (Expr* e = dynamic_cast<Expr*>(a)) args.push_back(clone_expr(e));
            else args.push_back(a);
        }
        return cu_.add_node<Type>(loc, Type::Custom{c->name, std::move(args), nullptr});
    }
    return cu_.add_node<Type>(loc, t->data);
}

Expr* SemanticAnalyzer::clone_expr(Expr* e) {
    if (!e) return nullptr;
    Location loc = e->loc;
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
    if (auto* v = e->get_primitive_type()) {
        return cu_.add_node<Expr>(loc, Expr::PrimitiveType{clone_type(v->type)});
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
    if (auto* v = s->get_for_in()) {
        return cu_.add_node<Stmt>(loc, Stmt::ForIn{v->var_name, clone_expr(v->collection), clone_stmt(v->body)});
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

// 实参 ASTNode 目前统一是 Expr*，这里把它转成可用于类型位置的 Type（新建节点）
Type* SemanticAnalyzer::arg_to_type(Expr* e) {
    if (!e) return nullptr;
    if (auto* pt = e->get_primitive_type()) {
        return pt->type ? clone_type(pt->type) : nullptr;
    }
    if (auto* id = e->get_identifier()) {
        return cu_.add_node<Type>(e->loc, Type::Custom{id->name, {}, nullptr});
    }
    if (auto* call = e->get_call()) {
        if (auto* id = call->callee ? call->callee->get_identifier() : nullptr) {
            return cu_.add_node<Type>(e->loc, Type::Custom{id->name, {}, nullptr});
        }
    }
    return nullptr;
}

void SemanticAnalyzer::subst_type(Type*& t, const std::unordered_map<std::string, ASTNode*>& m) {
    if (!t) return;
    if (auto* c = t->get_custom()) {
        auto it = m.find(std::string(c->name));
        if (it != m.end()) {
            if (Type* rep = dynamic_cast<Type*>(it->second)) {
                t = rep;
            } else if (Expr* e = dynamic_cast<Expr*>(it->second)) {
                Type* rt = arg_to_type(e);
                if (rt) t = rt;
            }
            return;
        }
        for (auto& a : c->args) {
            if (Expr* ae = dynamic_cast<Expr*>(a)) {
                Expr* ne = ae;
                subst_expr(ne, m);
                a = ne;
            }
        }
        return;
    }
    if (auto* p = t->get_pointer()) { subst_type(p->pointee, m); return; }
    if (auto* r = t->get_reference()) { subst_type(r->target, m); return; }
    if (auto* a = t->get_array()) {
        subst_type(a->element, m);
        Expr* le = a->length;
        subst_expr(le, m);
        a->length = le;
        return;
    }
    if (auto* f = t->get_func_ptr()) {
        for (auto& p : f->params) subst_type(p, m);
        subst_type(f->return_type, m);
        return;
    }
}

void SemanticAnalyzer::subst_expr(Expr*& e, const std::unordered_map<std::string, ASTNode*>& m) {
    if (!e) return;
    if (auto* v = e->get_identifier()) {
        auto it = m.find(std::string(v->name));
        if (it != m.end()) {
            if (Expr* arg = dynamic_cast<Expr*>(it->second)) {
                e = clone_expr(arg);
            } else if (Type* tp = dynamic_cast<Type*>(it->second)) {
                e = cu_.add_node<Expr>(current_expr ? current_expr->loc : Location(), Expr::PrimitiveType{clone_type(tp)});
            }
            return;
        }
    }
    if (auto* v = e->get_binary()) { subst_expr(v->left, m); subst_expr(v->right, m); return; }
    if (auto* v = e->get_unary()) { subst_expr(v->operand, m); return; }
    if (auto* v = e->get_assign()) { subst_expr(v->target, m); subst_expr(v->value, m); return; }
    if (auto* v = e->get_call()) {
        subst_expr(v->callee, m);
        for (auto& a : v->args) subst_expr(a, m);
        return;
    }
    if (auto* v = e->get_ternary()) {
        subst_expr(v->cond, m); subst_expr(v->then_expr, m); subst_expr(v->else_expr, m); return;
    }
    if (auto* v = e->get_index()) { subst_expr(v->target, m); subst_expr(v->index, m); return; }
    if (auto* v = e->get_member()) { subst_expr(v->target, m); return; }
    if (auto* v = e->get_cast()) {
        subst_type(v->target_type, m);
        subst_expr(v->operand, m);
        return;
    }
    if (auto* v = e->get_array()) {
        for (auto& a : v->members) subst_expr(a, m);
        subst_expr(v->fill_value, m);
        return;
    }
    if (auto* v = e->get_closure()) {
        for (auto* p : v->params) subst_varlist(p, m);
        for (auto* r : v->returns) subst_varlist(r, m);
        subst_stmt(v->body, m);
        return;
    }
    if (auto* v = e->get_primitive_type()) { subst_type(v->type, m); return; }
}

void SemanticAnalyzer::subst_varlist(VarList* vl, const std::unordered_map<std::string, ASTNode*>& m) {
    if (!vl) return;
    subst_type(vl->type, m);
    for (auto* vi : vl->vars) {
        if (!vi) continue;
        subst_expr(vi->init, m);
        if (vi->type) subst_type(vi->type, m);
    }
}

void SemanticAnalyzer::subst_stmt(Stmt* s, const std::unordered_map<std::string, ASTNode*>& m) {
    if (!s) return;
    if (auto* v = s->get_block()) {
        for (auto* x : v->stmts) subst_stmt(x, m);
        return;
    }
    if (auto* v = s->get_expr()) { subst_expr(v->expr, m); return; }
    if (auto* v = s->get_if()) {
        subst_expr(v->condition, m);
        subst_stmt(v->then_branch, m);
        subst_stmt(v->else_branch, m);
        return;
    }
    if (auto* v = s->get_while()) { subst_expr(v->condition, m); subst_stmt(v->body, m); return; }
    if (auto* v = s->get_for()) {
        subst_stmt(v->init, m);
        subst_expr(v->condition, m);
        subst_stmt(v->next, m);
        subst_stmt(v->body, m);
        return;
    }
    if (auto* v = s->get_for_in()) { subst_expr(v->collection, m); subst_stmt(v->body, m); return; }
    if (auto* v = s->get_var_decl()) { subst_varlist(v->vars, m); return; }
    if (auto* v = s->get_spawn()) { subst_stmt(v->body, m); return; }
    if (auto* v = s->get_wait()) { subst_expr(v->condition, m); return; }
}

} // namespace cx
