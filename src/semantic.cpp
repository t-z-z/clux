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
void SemanticAnalyzer::merge_external_symbols(const SymbolTable& external) {
    symbols_.merge(external);
}
// 语义分析器：工具

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

// 分析
bool SemanticAnalyzer::analyze() {
    diagnostics_.clear();
    call_resolutions_.clear();
    has_error_ = false;
    collect();
    walk(cu_);
    return !has_error_;
}

// 语义分析器：入口
void SemanticAnalyzer::visit(CompUnit&) {}
void SemanticAnalyzer::visit(Decl&) {}
void SemanticAnalyzer::visit(Import&) {}
void SemanticAnalyzer::scope_in(std::string name) {
    symbols_.push_scope(std::move(name));
}
void SemanticAnalyzer::check_type(Type& t) {

}

void SemanticAnalyzer::visit_func(Func& f) {
    for (const auto& tpl : f.templates) {
        symbols_.insert_local_tpl(tpl.name, tpl.dft);
    }
}

void SemanticAnalyzer::visit_special_method(SpecialMethod&) {}

void SemanticAnalyzer::visit_var_list(VarList& vl) {
    if(vl.type) check_type(*vl.type);
    for (const auto& var : vl.vars) {
        Symbol sym(SymbolKind::Variable, var.name, vl.type, &vl);
        if (!symbols_.insert_local(std::move(sym))) {
            error(vl.loc, "duplicate declaration of variable'" + std::string(var.name) + "'");
        }
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


std::string SemanticAnalyzer::build_decl_key(const std::vector<VarList*>& params) {
    std::string key;
    for (const auto* param : params) {
        if (!param || !param->type) {
            key += "_";
        } else if (param->type->is_inferred()) {
            key += "_";
        } else {
            key += type_to_ir(*param->type);
        }
    }
    return key;
}

std::string SemanticAnalyzer::build_call_key(const std::vector<Expr*>& args) {
    std::string key;
    for (const auto* arg : args) {
        if (!arg || !arg->type) {
            key += "_";
        } else if (arg->type->is_inferred()) {
            key += "_";
        } else {
            key += type_to_ir(*arg->type);
        }
    }
    return key;
}

void SemanticAnalyzer::collect() {
    // 根作用域已在构造函数中存在，直接遍历顶层声明
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
                Symbol sym(SymbolKind::Variable, var.name, vl->type, vl);
                symbols_.insert_member(std::move(sym));
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
            // 先创建 scope，获取指针
            Scope* cls_scope = symbols_.push_scope(std::string(cls->name));
            // 再创建带 scope 指针的 Symbol
            Symbol cls_sym(SymbolKind::Class, cls->name, nullptr, cls, cls_scope);
            symbols_.insert_member(std::move(cls_sym));

            for (Decl* member : cls->members) {
                collect_decl(member);
            }
            symbols_.pop_scope();
            break;
        }

        case DeclKind::Namespace: {
            auto* ns = static_cast<Namespace*>(decl);
            // 先创建 scope，获取指针
            Scope* ns_scope = symbols_.push_scope(std::string(ns->name));
            // 再创建带 scope 指针的 Symbol
            Symbol ns_sym(SymbolKind::Namespace, ns->name, nullptr, ns, ns_scope);
            symbols_.insert_member(std::move(ns_sym));

            for (Decl* member : ns->members) {
                collect_decl(member);
            }
            symbols_.pop_scope();
            break;
        }

        case DeclKind::Extern: {
            auto* ext = static_cast<Extern*>(decl);
            // Extern 块不引入新作用域，但成员函数需注册在当前作用域（可带前缀，但实际由 register_func 通过当前路径构造全名）
            for (Decl* member : ext->members) {
                if (member->kind == DeclKind::Func || member->kind == DeclKind::Operator) {
                    register_func(static_cast<Func*>(member));
                }
                // 其他成员（如变量）按需处理，目前暂不处理
            }
            break;
        }

        case DeclKind::Enum: {
            auto* e = static_cast<Enum*>(decl);
            // 注册每个枚举变体到当前作用域
            for (const Token& var : e->variants) {
                Symbol var_sym(SymbolKind::EnumVar, var.val, nullptr, e);
                symbols_.insert_member(std::move(var_sym));
            }
            break;
        }

        case DeclKind::SpecialMethod: {
            register_special(static_cast<SpecialMethod*>(decl));
            break;
        }

        case DeclKind::Keep:
        case DeclKind::Compose:
        default:
            break;
    }
}

void SemanticAnalyzer::register_func(Func* f) {
    std::string name = std::string(f->name);
    std::string path = get_current_full_path();
    std::string full_name = path.empty() ? name : path + "::" + name;

    std::string key = build_decl_key(f->params);

    const auto* existing = symbols_.lookup_overload(full_name);
    if (existing) {
        for (const auto& cand : *existing) {
            if (cand.key == key) {
                error(f->loc, "duplicate declaration of function '" + name + "'");
                return;
            }
        }
    }

    OverloadCandidate cand;
    cand.name = name;
    cand.full_name = full_name;
    cand.key = key;
    cand.param_count = f->params.size();
    cand.decl = f;
    cand.return_type = nullptr;
    cand.has_body = (f->body != nullptr);
    cand.has_template_params = !f->templates.empty();
    symbols_.insert_overload(full_name, std::move(cand));

    Symbol sym(SymbolKind::Function, f->name, nullptr, f);
    symbols_.insert_member(std::move(sym));
}

void SemanticAnalyzer::register_special(SpecialMethod* sm) {
    std::string internal_name;
    switch (sm->kind) {
        case SpecialMethod::Kind::Drop:       internal_name = "DROP";       break;
        case SpecialMethod::Kind::Ref:        internal_name = "REF";        break;
        case SpecialMethod::Kind::Deref:      internal_name = "DEREF";      break;
        case SpecialMethod::Kind::Access:     internal_name = "ACCESS";     break;
        case SpecialMethod::Kind::Conversion: internal_name = "__conversion"; break;
        default: error(sm->loc, "unknown special method"); return;
    }

    std::string path = get_current_full_path();
    std::string full_name = path.empty() ? internal_name : path + "::" + internal_name;

    std::string key = sm->conv ? build_decl_key({sm->conv}) : "";
    const auto* existing = symbols_.lookup_overload(full_name);
    if (existing) {
        for (const auto& cand : *existing) {
            if (cand.key == key) {
                error(sm->loc, "duplicate special method '" + internal_name + "'");
                return;
            }
        }
    }

    OverloadCandidate cand;
    cand.name = internal_name;
    cand.full_name = full_name;
    cand.key = key;
    cand.param_count = sm->conv ? 1 : 0;
    cand.decl = sm;
    cand.return_type = nullptr;
    cand.has_body = (sm->body != nullptr);
    cand.has_template_params = false;
    symbols_.insert_overload(full_name, std::move(cand));

    Symbol sym(SymbolKind::Function, internal_name, nullptr, sm);
    symbols_.insert_member(std::move(sym));
}

std::string SemanticAnalyzer::get_current_full_path() const {
    std::vector<std::string> parts;
    for (Scope* s = symbols_.current_scope(); s && s->parent; s = s->parent) {
        if (!s->name.empty()) {
            parts.push_back(s->name);
        }
    }
    std::string result;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!result.empty()) result += "::";
        result += *it;
    }
    return result;
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
        // 数组长度在编译期已确定
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

void SemanticAnalyzer::set_expr_type(Expr* e, Type* t) {
    if (e) e->type = t;
}

Type* SemanticAnalyzer::element_type(Type* t) {
    if (!t) return nullptr;
    if (t->is_array()) return t->get_array()->element;
    if (t->is_pointer()) return t->get_pointer()->pointee;
    return nullptr;
}

std::vector<std::string> SemanticAnalyzer::resolve_namespace_path(Expr* target) {
    std::vector<std::string> path;
    if (!target) return path;

    if (auto* id = target->get_identifier()) {
        path.push_back(std::string(id->name));
    } else if (auto* mem = target->get_member()) {
        auto prefix = resolve_namespace_path(mem->target);
        path.insert(path.end(), prefix.begin(), prefix.end());
        path.push_back(std::string(mem->member));
    }
    return path;
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
        set_expr_type(current_expr, sym->type);
        current_expr->is_lvalue = (sym->kind == SymbolKind::Variable);
        return;
    }

    // 模板参数占位，实例化时用具体类型替换
    ASTNode* tpl = symbols_.lookup_local_tpl(id.name);
    if (tpl) {
        current_expr->type = make_type(current_expr->loc, Type::Inferred{});
        current_expr->is_lvalue = true;
        return;
    }

    error(current_expr->loc, "undefined identifier '" + std::string(id.name) + "'");
}

void SemanticAnalyzer::visit_expr_binary(Expr::Binary& b) {
    Type* lt = b.left ? b.left->type : nullptr;
    Type* rt = b.right ? b.right->type : nullptr;

    if (!lt || !rt) {
        current_expr->type = nullptr;
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
                    // 浮点优先级更高的类型
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
        warning(current_expr->loc, "implicit conversion in assignment");
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

    // 成员函数调用
    if (auto* mem = c.callee->get_member()) {
        Symbol* target_sym = mem->target->sym;
        if (target_sym) {
            // 通过 target 的 scope 查找方法
            Scope* target_scope = target_sym->scope;
            if (!target_scope && target_sym->type) {
                Type* tt = target_sym->type;
                if (tt->is_pointer()) tt = tt->get_pointer()->pointee;
                if (tt && tt->is_custom()) {
                    std::string cls_name = std::string(tt->get_custom()->name);
                    const auto* cls_syms = symbols_.lookup_member(cls_name);
                    if (cls_syms) {
                        for (const auto& s : *cls_syms) {
                            if ((s.kind == SymbolKind::Class || s.kind == SymbolKind::Namespace)
                                && s.scope) {
                                target_scope = s.scope;
                                break;
                            }
                        }
                    }
                }
            }

            if (target_scope) {
                std::string method = std::string(mem->member);
                const auto* methods = SymbolTable::lookup_member_in(target_scope, method);
                if (methods) {
                    for (const auto& m : *methods) {
                        if (m.kind == SymbolKind::Function) {
                            callee_sym = const_cast<Symbol*>(&m);
                            break;
                        }
                    }
                }
            }
        }
    }

    if (!callee_sym) {
        error(call_expr->loc, "invalid callee expression");
        return;
    }

    // 如果 callee_sym 是函数，直接使用
    if (callee_sym->kind == SymbolKind::Function && callee_sym->decl) {
        CallResolution resolution;
        resolution.call = call_expr;
        resolution.callee_name = std::string(callee_sym->name);
        resolution.resolved_decl = callee_sym->decl;
        resolution.resolved = true;

        if (auto* f = dynamic_cast<Func*>(callee_sym->decl)) {
            if (!f->returns.empty() && f->returns[0]->type) {
                resolution.return_type = f->returns[0]->type;
            }
        } else if (auto* sm = dynamic_cast<SpecialMethod*>(callee_sym->decl)) {
            if (sm->conv && sm->conv->type) {
                resolution.return_type = sm->conv->type;
            }
        }

        set_expr_type(call_expr, resolution.return_type);
        call_expr->sym = callee_sym;
        current_expr->is_lvalue = false;
        call_resolutions_.push_back(std::move(resolution));
        return;
    }

    // 否则按名称查找重载
    std::string func_name = std::string(callee_sym->name);

    // 如果有 scope，构建完整名称
    if (callee_sym->scope) {
        // 这是一个类/命名空间，查找其构造函数或方法
        // 暂时不支持这种情况
    }

    const auto* candidates = symbols_.lookup_overload(func_name);
    if (!candidates || candidates->empty()) {
        error(call_expr->loc, "undefined function '" + func_name + "'");
        return;
    }

    CallResolution resolution;
    resolution.call = call_expr;
    resolution.callee_name = func_name;

    const OverloadCandidate* best = match_candidates(func_name, c.args.size(), *candidates);
    if (best) {
        resolution.resolved_name = best->full_name;
        resolution.resolved_decl = best->decl;
        resolution.resolved = true;

        if (auto* f = dynamic_cast<Func*>(best->decl)) {
            if (!f->returns.empty() && f->returns[0]->type) {
                resolution.return_type = f->returns[0]->type;
            }
        } else if (auto* sm = dynamic_cast<SpecialMethod*>(best->decl)) {
            if (sm->conv && sm->conv->type) {
                resolution.return_type = sm->conv->type;
            }
        }

        set_expr_type(call_expr, resolution.return_type);
    } else {
        resolution.ambiguous = true;
        error(call_expr->loc, "no matching overload for call to '" + func_name + "'");
    }

    current_expr->is_lvalue = false;
    call_resolutions_.push_back(std::move(resolution));
}

const OverloadCandidate* SemanticAnalyzer::match_candidates(
    const std::string& name,
    size_t arg_count,
    const std::vector<OverloadCandidate>& candidates) {
    const OverloadCandidate* best = nullptr;
    int best_score = -1;

    for (const auto& cand : candidates) {
        if (cand.has_template_params) continue; // 模板函数需实例化，暂跳过

        int score = 0;
        if (cand.param_count == arg_count) {
            score = 10; // 完美参数数量匹配
        } else if (cand.param_count < arg_count) {
            score = 5; // 参数多于形参（可变参数场景）
        } else {
            continue; // 参数不足
        }

        if (score > best_score) {
            best_score = score;
            best = &cand;
        }
    }
    return best;
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
        // 如果目标是指针类型，自动解引用
        if (tt->is_pointer()) {
            tt = tt->get_pointer()->pointee;
        }
        // 尝试通过类型名查找 scope
        if (tt && tt->is_custom()) {
            std::string cls_name = std::string(tt->get_custom()->name);
            const auto* cls_syms = symbols_.lookup_member(cls_name);
            if (cls_syms) {
                for (const auto& s : *cls_syms) {
                    if ((s.kind == SymbolKind::Class || s.kind == SymbolKind::Namespace)
                        && s.scope) {
                        target_scope = s.scope;
                        break;
                    }
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

    // 在目标 scope 中查找成员
    auto* members = SymbolTable::lookup_member_in(target_scope, field);
    if (members) {
        for (auto& sym : *members) {
            if (sym.kind == SymbolKind::Variable && sym.type) {
                current_expr->sym = const_cast<Symbol*>(&sym);
                current_expr->type = sym.type;
                current_expr->is_lvalue = true;
                return;
            }
            if (sym.kind == SymbolKind::Function) {
                current_expr->sym = const_cast<Symbol*>(&sym);
                current_expr->type = nullptr;
                current_expr->is_lvalue = false;
                return;
            }
        }
    }

    // 向上搜索父类/父命名空间
    if (target_scope->parent) {
        const auto* inherited = SymbolTable::lookup_member_up(target_scope->parent, field);
        if (inherited) {
            for (const auto& sym : *inherited) {
                if (sym.kind == SymbolKind::Variable && sym.type) {
                    current_expr->sym = const_cast<Symbol*>(&sym);
                    current_expr->type = sym.type;
                    current_expr->is_lvalue = true;
                    return;
                }
            }
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

    check_type(*cast.target_type);
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
        // 空数组：尝试从 fill_value 推导
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
    // 闭包类型: $(params) return_type
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
void SemanticAnalyzer::visit_stmt_if(Stmt::If&) {}
void SemanticAnalyzer::visit_stmt_while(Stmt::While&) {}
void SemanticAnalyzer::visit_stmt_for(Stmt::For&) {}
void SemanticAnalyzer::visit_stmt_for_in(Stmt::ForIn&) {}
void SemanticAnalyzer::visit_stmt_return(Stmt::Return&) {}
void SemanticAnalyzer::visit_stmt_break(Stmt::Break&) {}
void SemanticAnalyzer::visit_stmt_continue(Stmt::Continue&) {}
void SemanticAnalyzer::visit_stmt_flag(Stmt::Flag&) {}
void SemanticAnalyzer::visit_stmt_goto(Stmt::Goto&) {}
void SemanticAnalyzer::visit_stmt_asm(Stmt::Asm&) {}
void SemanticAnalyzer::visit_stmt_keep(Stmt::Keep&) {}
void SemanticAnalyzer::visit_stmt_var_decl(Stmt::VarDecl&) {}
void SemanticAnalyzer::visit_stmt_spawn(Stmt::Spawn&) {}
void SemanticAnalyzer::visit_stmt_wait(Stmt::Wait&) {}

void SemanticAnalyzer::visit(Type&) {}
void SemanticAnalyzer::visit_type_primitive(Type::Primitive&) {}
void SemanticAnalyzer::visit_type_pointer(Type::Pointer&) {}
void SemanticAnalyzer::visit_type_array(Type::Array&) {}
void SemanticAnalyzer::visit_type_func(Type::Func&) {}
void SemanticAnalyzer::visit_type_custom(Type::Custom& c) {
    if (cu_.is_type_name(c.name)) {
        auto* decl = cu_.type_registry[c.name];
        c.resolved_decl = decl;
    }
}
void SemanticAnalyzer::visit_type_inferred(Type::Inferred&) {}
void SemanticAnalyzer::visit_type_void(Type::Void&) {}

// ---- 模板实例化辅助桩 ----

bool SemanticAnalyzer::can_instantiate(
    const OverloadCandidate&,
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