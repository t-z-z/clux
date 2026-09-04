#include "../include/semantic.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>

namespace vo {

static bool is_numeric_type(Type* t) {
    if (!t || !t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k != PrimitiveTypeKind::Bool && k != PrimitiveTypeKind::Type && k != PrimitiveTypeKind::Void;
}

static bool is_integer_type(Type* t) {
    if (!t || !t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k == PrimitiveTypeKind::Int8 || k == PrimitiveTypeKind::Short || k == PrimitiveTypeKind::Int
        || k == PrimitiveTypeKind::Long || k == PrimitiveTypeKind::Int128
        || k == PrimitiveTypeKind::UInt8 || k == PrimitiveTypeKind::UShort || k == PrimitiveTypeKind::UInt
        || k == PrimitiveTypeKind::ULong || k == PrimitiveTypeKind::UInt128
        || k == PrimitiveTypeKind::Char || k == PrimitiveTypeKind::UChar;
}

static bool is_float_type(Type* t) {
    if (!t || !t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k == PrimitiveTypeKind::Float16 || k == PrimitiveTypeKind::Float || k == PrimitiveTypeKind::Double;
}

static bool is_bool_type(Type* t) {
    return t && t->is_primitive() && t->get_primitive()->kind == PrimitiveTypeKind::Bool;
}

static bool is_same_type(Type* a, Type* b) {
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

static bool is_integer_promotable(Type* t) {
    if (!t || !t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k == PrimitiveTypeKind::Int8 || k == PrimitiveTypeKind::Short
        || k == PrimitiveTypeKind::UInt8 || k == PrimitiveTypeKind::UChar
        || k == PrimitiveTypeKind::Char;
}

static bool is_void_type(Type* t) {
    return t && t->is_void();
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

bool SemanticAnalyzer::analyze() {
    diagnostics_.clear();
    call_resolutions_.clear();
    has_error_ = false;
    collect();
    walk(cu_);
    return !has_error_;
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
void SemanticAnalyzer::scope_in(std::string name) {
    symbols_.push_scope(std::move(name));
}
void SemanticAnalyzer::visit_func(Func& f) {
    for (const auto& tpl : f.templates) {
        symbols_.insert_local_tpl(tpl.name, tpl.dft);
    }
}

void SemanticAnalyzer::visit_special_method(SpecialMethod&) {}

void SemanticAnalyzer::visit_var_list(VarList& vl) {
    for (const auto& var : vl.vars) {
        symbols_.insert_local(var.name, Symbol(SymbolKind::Variable, &vl, vl.type, &vl));
    }
}

void SemanticAnalyzer::visit_class(Class& c) {
    for (const auto& tpl : c.templates) {
        symbols_.insert_local_tpl(tpl.name, tpl.dft);
    }
}

void SemanticAnalyzer::visit_namespace(Namespace&) {}
void SemanticAnalyzer::visit_extern(Extern&) {}
void SemanticAnalyzer::visit_enum(Enum&) {}
void SemanticAnalyzer::visit_keep(Keep&) {}


std::string SemanticAnalyzer::build_callkey(const std::string& name,
    const std::vector<VarList*>& params, const std::vector<TemplateParam>& templates) {
    std::string key = name + "_";
    for (const auto* param : params) {
        if (!param || !param->type || param->type->is_inferred()) {
            key += "_";
        } else if (param->type->is_custom()) {
            auto* ct = param->type->get_custom();
            bool is_tpl = false;
            for (size_t i = 0; i < templates.size(); ++i) {
                if (templates[i].name == ct->name) {
                    key += "<" + std::to_string(i) + ">";
                    is_tpl = true;
                    break;
                }
            }
            if (!is_tpl) {
                key += type_to_ir(*param->type);
            }
        } else {
            key += type_to_ir(*param->type);
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
            key += type_to_ir(*arg->type);
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
            for (const auto& var : vl->vars) {
                Symbol sym(SymbolKind::Variable, vl, vl->type, vl);
                symbols_.insert_member(std::string(var.name), std::move(sym));
            }
            break;
        }

        case DeclKind::Func:
        case DeclKind::Operator: {
            register_func(static_cast<Func*>(decl));
            break;
        }

        case DeclKind::Class: {
            auto* cls = static_cast<Class*>(decl);
            Scope* cls_scope = symbols_.push_scope(std::string(cls->name));
            Symbol cls_sym(SymbolKind::Class, cls, nullptr, cls, cls_scope);
            symbols_.insert_member(std::string(cls->name), std::move(cls_sym));
            cls->scope = cls_scope;

            for (Decl* member : cls->members) {
                collect_decl(member);
            }
            symbols_.pop_scope();
            break;
        }

        case DeclKind::Namespace: {
            auto* ns = static_cast<Namespace*>(decl);
            Scope* ns_scope = symbols_.push_scope(std::string(ns->name));
            Symbol ns_sym(SymbolKind::Namespace, ns, nullptr, ns, ns_scope);
            symbols_.insert_member(std::string(ns->name), std::move(ns_sym));
            ns->scope = ns_scope;

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
            for (const Token& var : e->variants) {
                Symbol var_sym(SymbolKind::EnumVar, e, nullptr, e);
                symbols_.insert_member(std::string(var.val), std::move(var_sym));
            }
            break;
        }

        case DeclKind::SpecialMethod: {
            register_special(static_cast<SpecialMethod*>(decl));
            break;
        }
        default:
            break;
    }
}

void SemanticAnalyzer::register_func(Func* f) {
    std::string name = std::string(f->name);
    std::string key = build_callkey(name, f->params, f->templates);

    if (symbols_.lookup_member(name)) {
        error(f->loc, "duplicate declaration of function '" + name + "'");
        return;
    }

    Symbol sym(SymbolKind::Function, f, nullptr, f, nullptr);
    symbols_.insert_member(name, std::move(sym));
}

void SemanticAnalyzer::register_special(SpecialMethod* sm) {
    std::string internal_name;
    switch (sm->kind) {
        case SpecialMethod::Kind::Drop:       internal_name = "DROP";       break;
        case SpecialMethod::Kind::Ref:        internal_name = "REF";        break;
        case SpecialMethod::Kind::Deref:      internal_name = "DEREF";      break;
        case SpecialMethod::Kind::Access:     internal_name = "ACCESS";     break;
        case SpecialMethod::Kind::Conversion: internal_name = "conversion"; break;
        default: error(sm->loc, "unknown special method"); return;
    }

    std::string key = sm->conv ? build_callkey(internal_name, {sm->conv}, {}) : internal_name;

    if (symbols_.lookup_member(internal_name)) {
        error(sm->loc, "duplicate special method '" + internal_name + "'");
        return;
    }

    Symbol sym(SymbolKind::Function, sm, nullptr, sm, nullptr);
    symbols_.insert_member(internal_name, std::move(sym));
}

void SemanticAnalyzer::scope_out() {
    symbols_.pop_scope();
}

std::string SemanticAnalyzer::type_to_ir(Type& t) const {
    if (t.is_primitive()) {
        auto k = t.get_primitive()->kind;
        switch (k) {
            case PrimitiveTypeKind::Int8:   return "i8";
            case PrimitiveTypeKind::Short:  return "i16";
            case PrimitiveTypeKind::Int:    return "i32";
            case PrimitiveTypeKind::Long:   return "i64";
            case PrimitiveTypeKind::Int128: return "i128";
            case PrimitiveTypeKind::UInt8:  return "i8";
            case PrimitiveTypeKind::UShort: return "i16";
            case PrimitiveTypeKind::UInt:   return "i32";
            case PrimitiveTypeKind::ULong:  return "i64";
            case PrimitiveTypeKind::UInt128:return "i128";
            case PrimitiveTypeKind::Float16:return "half";
            case PrimitiveTypeKind::Float:  return "float";
            case PrimitiveTypeKind::Double: return "double";
            case PrimitiveTypeKind::Char:   return "i8";
            case PrimitiveTypeKind::UChar:  return "i8";
            case PrimitiveTypeKind::Bool:   return "i1";
            case PrimitiveTypeKind::Type:   return "i32";
            case PrimitiveTypeKind::Void:   return "void";
            case PrimitiveTypeKind::Enum:   return "i32";
            default: return "i8";
        }
    }
    if (t.is_string_literal()) return "i8*";
    if (t.is_pointer()) {
        return type_to_ir(*t.get_pointer()->pointee) + "*";
    }
    if (t.is_reference()) {
        return type_to_ir(*t.get_reference()->target) + "*";
    }
    if (t.is_array()) {
        auto* arr = t.get_array();
        std::string elem_ir = arr->element ? type_to_ir(*arr->element) : "i8";
        return "[" + elem_ir + " x " + elem_ir + "]";
    }
    if (t.is_func()) {
        auto* f = t.get_func();
        std::string ret = f->return_type ? type_to_ir(*f->return_type) : "void";
        std::string params;
        for (size_t i = 0; i < f->params.size(); ++i) {
            if (i > 0) params += ", ";
            params += f->params[i] ? type_to_ir(*f->params[i]) : "i8";
        }
        return ret + " (" + params + ")*";
    }
    if (t.is_custom()) {
        return "%" + std::string(t.get_custom()->name);
    }
    if (t.is_void()) return "void";
    if (t.is_inferred()) return "i8";
    return "i8";
}

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
        current_expr->type = sym->type;
        current_expr->is_lvalue = (sym->kind == SymbolKind::Variable);
        return;
    }

    ASTNode* tpl = symbols_.lookup_local_tpl(id.name);
    if (tpl) {
        current_expr->type = typeInferred;
        current_expr->is_lvalue = true;
        return;
    }

    Symbol* members = symbols_.lookup_member(std::string(id.name));
    if (members) {
        if (members->kind == SymbolKind::Function) {
            current_expr->sym = members;
            current_expr->type = nullptr;
            current_expr->is_lvalue = false;
            return;
        }
        if (members->kind == SymbolKind::Class) {
            current_expr->sym = members;
            current_expr->type = nullptr;
            current_expr->is_lvalue = false;
            return;
        }
        if (members->kind == SymbolKind::Variable) {
            current_expr->sym = members;
            current_expr->type = members->type;
            current_expr->is_lvalue = true;
            return;
        }
        current_expr->sym = members;
        current_expr->type = members->type;
        current_expr->is_lvalue = false;
        return;
    }

    error(current_expr->loc, "undefined identifier '" + std::string(id.name) + "'");
}

bool SemanticAnalyzer::has_operator_overload(Type* type, OpKind op) {
    if (!type) return false;
    auto* cus = type->get_custom();
    if (!cus || !cus->resolved_decl) return false;
    Scope* s = static_cast<Class*>(cus->resolved_decl)->scope;
    if (!s) return false;
    std::string cls_name = std::string(cus->name);
    std::string_view method_name = opkind_to_sv(op);
    Scope* cls_scope = nullptr;
    Symbol* cls_syms = symbols_.lookup_member(cls_name);
    if (cls_syms) {
        if ((cls_syms->kind == SymbolKind::Class || cls_syms->kind == SymbolKind::Namespace) && cls_syms->scope) {
            cls_scope = cls_syms->scope;
        }
    }
    
    if (!cls_scope) return false;
    
    Symbol* methods = SymbolTable::lookup_member_in(cls_scope, std::string(method_name));
    if (methods) {
        if (methods->kind == SymbolKind::Function) return true;
    }
    
    return false;
}

bool SemanticAnalyzer::can_convert_to(Type* src, Type* dst) {
    if (!src || !dst) return false;
    
    if (is_same_type(src, dst)) return true;
    
    if (auto* c = src->get_custom()) {
        if(c->resolved_decl && c->resolved_decl->kind == DeclKind::Class) {
            Class& cls = static_cast<Class&>(*c->resolved_decl);
            Symbol* cands = cls.scope->lookup_member("conversion");
            if (cands) {
                return true;
            }
        }
    }
    
    return false;
}

void SemanticAnalyzer::visit_expr_binary(Expr::Binary& b) {
    Type* lt = b.left ? b.left->type : nullptr;
    Type* rt = b.right ? b.right->type : nullptr;

    if (!lt || !rt) {
        current_expr->type = nullptr;
        return;
    }

    bool left_is_class = lt->is_custom();
    bool right_is_class = rt->is_custom();
    
    if (left_is_class || right_is_class) {
        bool has_left_overload = left_is_class && has_operator_overload(lt, b.op);
        bool has_right_overload = right_is_class && has_operator_overload(rt, b.op);
        
        if (has_left_overload || has_right_overload) {
            current_expr->type = lt;
            current_expr->is_lvalue = false;
            return;
        }
        
        if (b.op == OpKind::Less || b.op == OpKind::Greater || 
            b.op == OpKind::LessEqual || b.op == OpKind::GreaterEqual) {
            bool has_any_comparison = false;
            if (left_is_class) {
                has_any_comparison = has_operator_overload(lt, OpKind::Less) ||
                                    has_operator_overload(lt, OpKind::Greater) ||
                                    has_operator_overload(lt, OpKind::LessEqual) ||
                                    has_operator_overload(lt, OpKind::GreaterEqual);
            }
            if (!has_any_comparison && right_is_class) {
                has_any_comparison = has_operator_overload(rt, OpKind::Less) ||
                                    has_operator_overload(rt, OpKind::Greater) ||
                                    has_operator_overload(rt, OpKind::LessEqual) ||
                                    has_operator_overload(rt, OpKind::GreaterEqual);
            }
            
            if (has_any_comparison) {
                current_expr->type = typeBool;
                current_expr->is_lvalue = false;
                return;
            }
        }
        
        if (b.op == OpKind::Equal || b.op == OpKind::NotEqual) {
            current_expr->type = typeBool;
            current_expr->is_lvalue = false;
            return;
        }
    }

    switch (b.op) {
        case OpKind::Plus:
        case OpKind::Minus:
        case OpKind::Mul:
        case OpKind::Div:
        case OpKind::Mod: {
            if (is_numeric_type(lt) && is_numeric_type(rt)) {
                if (is_float_type(lt) || is_float_type(rt)) {
                    auto lk = lt->get_primitive()->kind;
                    auto rk = rt->get_primitive()->kind;
                    int lp = lk == PrimitiveTypeKind::Double ? 2 : (lk == PrimitiveTypeKind::Float ? 1 : 0);
                    int rp = rk == PrimitiveTypeKind::Double ? 2 : (rk == PrimitiveTypeKind::Float ? 1 : 0);
                    current_expr->type = lp >= rp ? lt : rt;
                } else {
                    current_expr->type = lt;
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

    if (!ot) {
        current_expr->type = nullptr;
        return;
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
    if (!a.target || !a.value) {
        current_expr->type = nullptr;
        return;
    }

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
    if (!c.callee) return;

    Symbol* callee_sym = c.callee->sym;

    if (auto* mem = c.callee->get_member()) {
        Symbol* target_sym = mem->target->sym;
        if (target_sym) {
            Scope* target_scope = target_sym->scope;
            if (target_scope) {
                std::string method = std::string(mem->member);
                Symbol* method_sym = SymbolTable::lookup_member_in(target_scope, method);
                if (method_sym && method_sym->kind == SymbolKind::Function) {
                    callee_sym = method_sym;
                }
            }
        }
    }

    if (!callee_sym) {
        error(call_expr->loc, "invalid callee expression");
        return;
    }

    if (callee_sym->kind == SymbolKind::Class && callee_sym->scope) {
        std::string class_name = std::string(static_cast<Decl*>(callee_sym->node)->name);
        Scope* cls_scope = callee_sym->scope;
        
        Symbol* init_methods = SymbolTable::lookup_member_in(cls_scope, "INIT");
        if (init_methods) {
            if (init_methods->kind == SymbolKind::Function) {
                callee_sym = init_methods;
            }
        } else {
            if (!c.args.empty()) {
                error(call_expr->loc, "class '" + class_name + "' has no INIT method but called with arguments");
                return;
            }
            CallResolution resolution;
            resolution.call = call_expr;
            resolution.callee_name = class_name;
            resolution.resolved_decl = callee_sym->decl;
            resolution.is_constructor = true;
            resolution.resolved = true;
            resolution.return_types.push_back(typeVoid);
            
            current_expr->type = typeVoid;
            call_expr->sym = callee_sym;
            current_expr->is_lvalue = false;
            call_resolutions_.push_back(std::move(resolution));
            return;
        }
    }

    std::string func_name = std::string(static_cast<Decl*>(callee_sym->node)->name);
    std::string callkey = build_callkey(func_name, c.args);

    Symbol* resolved_sym = symbols_.lookup_member(func_name);
    CallResolution resolution;
    resolution.call = call_expr;
    resolution.callee_name = func_name;

    if (resolved_sym && resolved_sym->kind == SymbolKind::Function) {
        resolution.resolved_name = std::string(static_cast<Decl*>(resolved_sym->node)->name);
        resolution.resolved_decl = resolved_sym->decl;
        resolution.resolved = true;

        if (auto* f = dynamic_cast<Func*>(resolved_sym->decl)) {
            for (auto* ret : f->returns) {
                if (ret && ret->type) {
                    resolution.return_types.push_back(ret->type);
                }
            }
        }

        if (!resolution.return_types.empty()) {
            current_expr->type = resolution.return_types[0];
        } else {
            current_expr->type = typeVoid;
        }
    } else {
        Symbol* members_ptr = symbols_.lookup_member(func_name);
        bool is_template = false;
        if (members_ptr && members_ptr->kind == SymbolKind::Function) {
            if (auto* f = dynamic_cast<Func*>(members_ptr->decl)) {
                if (!f->templates.empty()) {
                    is_template = true;
                }
            }
        }
        if (is_template) {
            error(call_expr->loc, "template instantiation not yet implemented for '" + func_name + "'");
        } else {
            error(call_expr->loc, "no matching overload for call to '" + func_name + "'");
        }
        resolution.ambiguous = true;
    }

    current_expr->is_lvalue = false;
    call_resolutions_.push_back(std::move(resolution));
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
        current_expr->type = nullptr;
        return;
    }

    Symbol* target_sym = m.target->sym;
    if (!target_sym) {
        error(current_expr->loc, "cannot access member on unresolved expression");
        current_expr->type = nullptr;
        current_expr->is_lvalue = false;
        return;
    }

    Scope* target_scope = target_sym->scope;
    if (!target_scope) {
        Type* tt = target_sym->type;
        if (tt && tt->is_pointer()) {
            tt = tt->get_pointer()->pointee;
        }
        if (tt && tt->is_custom()) {
            std::string cls_name = std::string(tt->get_custom()->name);
            Symbol* cls_syms = symbols_.lookup_member(cls_name);
            if (cls_syms) {
                if ((cls_syms->kind == SymbolKind::Class || cls_syms->kind == SymbolKind::Namespace)
                    && cls_syms->scope) {
                    target_scope = cls_syms->scope;
                }
            }
        }
        if (!target_scope) {
            error(current_expr->loc, "cannot access member on non-class/non-namespace type");
            current_expr->type = nullptr;
            current_expr->is_lvalue = false;
            return;
        }
    }

    std::string field = std::string(m.member);

    Symbol* members = SymbolTable::lookup_member_in(target_scope, field);
    if (members) {
        if (members->kind == SymbolKind::Variable && members->type) {
            current_expr->sym = members;
            current_expr->type = members->type;
            current_expr->is_lvalue = true;
            return;
        }
        if (members->kind == SymbolKind::Function) {
            current_expr->sym = members;
            current_expr->type = nullptr;
            current_expr->is_lvalue = false;
            return;
        }
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
        Type::Array{elem_type ? elem_type : typeInt, nullptr});
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
    if (!clo.returns.empty() && clo.returns[0]->type) {
        ret_type = clo.returns[0]->type;
    }

    current_expr->type = make_type(current_expr->loc,
        Type::Func{std::move(param_types), ret_type});
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

void SemanticAnalyzer::visit_stmt_return(Stmt::Return& return_stmt) {
    if (!current_func) {
        error(current_stmt->loc, "return statement outside of function");
        return;
    }
}

void SemanticAnalyzer::visit_stmt_break(Stmt::Break&) {
    bool in_loop = false;
    for (Scope* s = symbols_.current_scope(); s; s = s->parent) {
        if (s->name == "while" || s->name == "for" || s->name == "forin") {
            in_loop = true;
            break;
        }
    }
    if (!in_loop) {
        error(current_stmt->loc, "break statement not inside a loop");
    }
}

void SemanticAnalyzer::visit_stmt_continue(Stmt::Continue&) {
    bool in_loop = false;
    for (Scope* s = symbols_.current_scope(); s; s = s->parent) {
        if (s->name == "while" || s->name == "for" || s->name == "forin") {
            in_loop = true;
            break;
        }
    }
    if (!in_loop) {
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
void SemanticAnalyzer::visit_type_primitive(Type::Primitive&) {}
void SemanticAnalyzer::visit_type_pointer(Type::Pointer&) {}
void SemanticAnalyzer::visit_type_array(Type::Array&) {}
void SemanticAnalyzer::visit_type_func(Type::Func&) {}
void SemanticAnalyzer::visit_type_custom(Type::Custom& c) {
    if (cu_.is_type_name(c.name)) {
        auto* decl = cu_.type_registry[c.name];
        c.resolved_decl = decl;
    } else {
        error(current_type->loc, "unresolved type '" + std::string(c.name) + "'");
    }
}
void SemanticAnalyzer::visit_type_inferred(Type::Inferred&) {
    error(current_type->loc, "cannot use inferred type in this context");
}
void SemanticAnalyzer::visit_type_void(Type::Void&) {}

bool SemanticAnalyzer::can_instantiate(
    Symbol*,
    const std::vector<Expr*>&,
    std::vector<TemplateDeduction>&) {
    return false;
}

bool SemanticAnalyzer::deduce_template_params(
    const Func*,
    const std::vector<Expr*>&,
    const std::vector<ASTNode*>&,
    std::vector<TemplateDeduction>&) {
    return false;
}

Type* SemanticAnalyzer::substitute_type(Type* t, const std::vector<TemplateDeduction>&) {
    return t;
}

Expr* SemanticAnalyzer::substitute_expr(Expr* e, const std::vector<TemplateDeduction>&) {
    return e;
}

bool SemanticAnalyzer::can_use_as_type(ASTNode*) { return false; }
bool SemanticAnalyzer::can_use_as_expr(ASTNode*) { return true; }
bool SemanticAnalyzer::param_used_as_type(const std::string&, const Func*) { return false; }
bool SemanticAnalyzer::param_used_as_expr(const std::string&, const Func*) { return false; }

bool SemanticAnalyzer::check_instantiated_body(Stmt*,
    const std::vector<TemplateDeduction>&) { return true; }

bool SemanticAnalyzer::check_instantiated_expr(Expr*,
    const std::vector<TemplateDeduction>&) { return true; }

bool SemanticAnalyzer::is_template_param_type(Type*, const Func*) { return false; }

} // namespace vo
