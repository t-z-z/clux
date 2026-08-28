#include "../include/semantic.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace vo {

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
    visit(cu_);
    return !has_error_;
}

// 语义分析器：入口
void SemanticAnalyzer::visit(CompUnit& cu) {
    for (Decl* decl : cu.top_decls) {
        visit(*decl);
    }
}

void SemanticAnalyzer::check_type(Type& t) {
    if (auto* c = t.get_custom()) {
        if (Symbol* s = symbols_.lookup(c->name)) {
           c->resolved_decl =  s->decl;
        } else if (cu_.is_type_name(c->name)) {
            c->resolved_decl = cu_.type_registry[std::string(c->name)];
        } else if (!symbols_.lookup_generic(c->name)){
            error(t.loc, "unknown type '" + std::string(c->name) + "'");
        }
    } else if (auto* p = t.get_pointer()) {
        if (p->pointee) check_type(*p->pointee);
    } else if (auto* r = t.get_reference()) {
        if (r->target) check_type(*r->target);
    } else if (auto* a = t.get_array()) {
        if (a->element) check_type(*a->element);
        if (a->length) visit(*a->length);
    } else if (auto* f = t.get_func()) {
        for (auto* p : f->params) if (p) check_type(*p);
        if (f->return_type) check_type(*f->return_type);
    }
}

void SemanticAnalyzer::visit(Decl&) {}

void SemanticAnalyzer::visit_func(Func& f) {
}

void SemanticAnalyzer::visit_operator(Operator& op) {
}

void SemanticAnalyzer::visit_special_method(SpecialMethod& f){
}

void SemanticAnalyzer::visit_var_list(VarList& vl) {
    if (vl.type) {
        if (vl.type->is_void()) {
            error(vl.loc, "can not create variable with void");
        }
        if (!vl.type->is_inferred()) {
            check_type(*vl.type);
        }
    }
    for (auto& v : vl.vars) {
        Symbol sym;
        sym.kind = SymbolKind::Variable;
        sym.name = v.name;
        sym.type = vl.type;  // 可能是 Inferred
        sym.decl = &vl;
        sym.ir_type = vl.type ? type_to_ir(*vl.type) : "";
        symbols_.insert(std::move(sym));
    }
}

void SemanticAnalyzer::visit_class(Class& c) {
}

// 以下 visit_* 在语义分析骨架阶段尚未实现，提供空实现以保证可链接。
// 后续实现具体语义检查时再填充函数体。
void SemanticAnalyzer::visit(Expr& expr) { (void)expr; }
void SemanticAnalyzer::visit(Modification& mod) { (void)mod; }
void SemanticAnalyzer::visit_modification(Modification& mod) { (void)mod; }
void SemanticAnalyzer::visit_namespace(Namespace& ns) { (void)ns; }
void SemanticAnalyzer::visit_extern(Extern& e) { (void)e; }
void SemanticAnalyzer::visit_enum(Enum& e) { (void)e; }
void SemanticAnalyzer::visit_keep(Keep& k) { (void)k; }
void SemanticAnalyzer::visit_stmt_block(Stmt::Block& s) { (void)s; }
void SemanticAnalyzer::visit_stmt_expr(Stmt::ExprStmt& s) { (void)s; }
void SemanticAnalyzer::visit_stmt_if(Stmt::If& s) { (void)s; }
void SemanticAnalyzer::visit_stmt_while(Stmt::While& s) { (void)s; }
void SemanticAnalyzer::visit_stmt_for(Stmt::For& s) { (void)s; }
void SemanticAnalyzer::visit_stmt_for_in(Stmt::ForIn& s) { (void)s; }
void SemanticAnalyzer::visit_stmt_return(Stmt::Return& s) { (void)s; }
void SemanticAnalyzer::visit_stmt_var_decl(Stmt::VarDecl& s) { (void)s; }
void SemanticAnalyzer::visit_stmt_spawn(Stmt::Spawn& s) { (void)s; }
void SemanticAnalyzer::visit_stmt_wait(Stmt::Wait& s) { (void)s; }
void SemanticAnalyzer::visit_stmt_asm(Stmt::Asm& s) { (void)s; }
void SemanticAnalyzer::visit_stmt_keep(Stmt::Keep& s) { (void)s; }

std::string SemanticAnalyzer::type_to_ir(Type* t) const {
    if (!t) return "";
    return type_to_ir(*t);
}
std::string SemanticAnalyzer::type_to_ir(Type& t) const {
    // 骨架阶段：返回占位 IR 类型，待代码生成阶段完善
    (void)t;
    return "";
}

std::string SemanticAnalyzer::type_to_ir(Type& t) const {
    if (auto* p = t.get_primitive()) {
        switch (p->kind) {
            case PrimitiveTypeKind::Int8: return "i8";
            case PrimitiveTypeKind::Short: return "i16";
            case PrimitiveTypeKind::Int: return "i32";
            case PrimitiveTypeKind::Long: return "i64";
            case PrimitiveTypeKind::Int128: return "i128";
            case PrimitiveTypeKind::UInt8: return "i8";
            case PrimitiveTypeKind::UShort: return "i16";
            case PrimitiveTypeKind::UInt: return "i32";
            case PrimitiveTypeKind::ULong: return "i64";
            case PrimitiveTypeKind::UInt128: return "i128";
            case PrimitiveTypeKind::Float16: return "half";
            case PrimitiveTypeKind::Float: return "float";
            case PrimitiveTypeKind::Double: return "double";
            case PrimitiveTypeKind::Char: return "i8";
            case PrimitiveTypeKind::UChar: return "i8";
            case PrimitiveTypeKind::Bool: return "i1";
            case PrimitiveTypeKind::Type: return "i64";
            case PrimitiveTypeKind::Void: return "void";
            case PrimitiveTypeKind::Enum: return "i32";
        }
        return "";
    }
    
    if (auto* ptr = t.get_pointer()) {
        return type_to_ir(*ptr->pointee) + "*";
    }
    
    if (auto* ref = t.get_reference()) {
        return type_to_ir(*ref->target) + "*";
    }
    
    if (auto* arr = t.get_array()) {
        std::string elem = type_to_ir(*arr->element);
        if (arr->length) {
            return "[" + std::to_string(0) + " x " + elem + "]";
        }
        return elem + "*";
    }
    
    if (auto* func = t.get_func()) {
        std::string result;
        if (func->return_type) {
            result = type_to_ir(*func->return_type);
        } else {
            result = "void";
        }
        result += " (";
        for (size_t i = 0; i < func->params.size(); ++i) {
            if (i > 0) result += ", ";
            result += type_to_ir(*func->params[i]);
        }
        result += ")*";
        return result;
    }
    
    if (auto* custom = t.get_custom()) {
        return std::string(custom->name);
    }
    
    if (t.is_void()) {
        return "void";
    }
    
    return "";
}

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
    for (Decl* decl : cu_.top_decls) {
        collect_decl(decl);
    }
}

void SemanticAnalyzer::collect_decl(Decl* decl) {
    if (!decl) return;
    
    switch (decl->kind) {
        case DeclKind::VarList: {
            auto* vl = static_cast<VarList*>(decl);
            if (vl->type) {
                check_type(*vl->type);
            }
            for (auto& v : vl->vars) {
                Symbol sym(SymbolKind::Variable, std::string(v.name), vl->type, vl);
                if (symbols_.lookup(std::string(v.name))) {
                    if (symbols_.lookup(std::string(v.name))->kind != SymbolKind::Variable) {
                        error(vl->loc, "name '" + std::string(v.name) + "' already declared");
                    }
                } else {
                    symbols_.insert(std::move(sym));
                }
            }
            break;
        }
        
        case DeclKind::Func: {
            auto* f = static_cast<Func*>(decl);
            if (current_class_) {
                register_method(f, current_class_);
            } else {
                register_func(f);
            }
            break;
        }
        
        case DeclKind::Class: {
            auto* c = static_cast<Class*>(decl);
            Class* saved = current_class_;
            svmap<std::vector<OverloadCandidate>>* saved_overloads = current_class_overloads_;
            
            current_class_ = c;
            current_class_overloads_ = &class_overload_maps_[std::string(c->name)];
            
            if (symbols_.lookup(std::string(c->name))) {
                error(c->loc, "class name already declared: " + std::string(c->name));
            } else {
                Symbol sym(SymbolKind::Class, std::string(c->name), nullptr, c);
                symbols_.insert(std::move(sym));
                cu_.register_type(c->name, c);
            }
            
            for (Decl* member : c->members) {
                if (member->kind == DeclKind::VarList) {
                    auto* vl = static_cast<VarList*>(member);
                    for (auto& v : vl->vars) {
                        current_class_fields_.insert(std::string(v.name));
                        if (vl->modfs.has(ModfKind::Readonly)) {
                            current_class_readonly_fields_.insert(std::string(v.name));
                        }
                    }
                }
                collect_decl(member);
            }
            
            current_class_overloads_ = saved_overloads;
            current_class_ = saved;
            break;
        }
        
        case DeclKind::Namespace: {
            auto* ns = static_cast<Namespace*>(decl);
            if (symbols_.lookup(std::string(ns->name))) {
                error(ns->loc, "namespace already declared: " + std::string(ns->name));
            } else {
                Symbol sym(SymbolKind::Namespace, std::string(ns->name), nullptr, ns);
                symbols_.insert(std::move(sym));
            }
            
            for (Decl* member : ns->members) {
                collect_decl(member);
            }
            break;
        }
        
        case DeclKind::Extern: {
            auto* ext = static_cast<Extern*>(decl);
            for (Decl* member : ext->members) {
                collect_decl(member);
            }
            break;
        }
        
        case DeclKind::Enum: {
            auto* e = static_cast<Enum*>(decl);
            if (symbols_.lookup(std::string(e->name))) {
                error(e->loc, "enum already declared: " + std::string(e->name));
            } else {
                Symbol sym(SymbolKind::Class, std::string(e->name), nullptr, e);
                symbols_.insert(std::move(sym));
            }
            break;
        }
        
        case DeclKind::SpecialMethod: {
            auto* sm = static_cast<SpecialMethod*>(decl);
            if (current_class_) {
                register_special(sm);
            } else {
                error(sm->loc, "special method should be contained by class");
            }
            break;
        }
        
        case DeclKind::Operator: {
            auto* op = static_cast<Operator*>(decl);
            if (current_class_) {
                register_method(op, current_class_);
            } else {
                error(op->loc, "operator overload should be contained by class");
            }
            break;
        }
        
        default:
            break;
    }
}

void SemanticAnalyzer::register_func(Func* f, const std::string& ns_name) {
    std::string full_name = std::string(f->name);
    if (!ns_name.empty()) {
        full_name = ns_name + "::" + std::string(f->name);
    }

    bool has_template_params = !f->templates.empty();
    std::string key = build_decl_key(f->params);

    auto& candidates = overload_map_[full_name];
    
    if (!has_template_params) {
        for (const auto& c : candidates) {
            if (!c.has_template_params && c.key == key) {
                error(f->loc, "duplicate non-template function overload: " + full_name);
                return;
            }
        }
    }

    OverloadCandidate cand;
    cand.name = full_name;
    cand.mangled = full_name + (has_template_params ? "_tpl" : "") + key;
    cand.key = key;
    cand.param_count = f->params.size();
    cand.decl = f;
    cand.has_body = f->body != nullptr;
    cand.has_template_params = has_template_params;
    cand.template_count = f->templates.size();
    if (!f->returns.empty() && f->returns[0] && f->returns[0]->type) {
        cand.return_type = f->returns[0]->type;
    } else {
        cand.return_type = nullptr;
    }
    candidates.push_back(cand);

    if (!symbols_.lookup(full_name)) {
        Symbol sym(SymbolKind::Function, full_name, nullptr, f);
        symbols_.insert(std::move(sym));
    }
}
void SemanticAnalyzer::register_method(Func* f, Class* cls) {
    if (!cls) return;
    
    bool has_template_params = !f->templates.empty();
    std::string key = build_decl_key(f->params);

    if (!current_class_overloads_) {
        current_class_overloads_ = &class_overload_maps_[std::string(cls->name)];
    }
    
    auto& candidates = (*current_class_overloads_)[std::string(f->name)];
    
    if (!has_template_params) {
        for (const auto& c : candidates) {
            if (!c.has_template_params && c.key == key) {
                error(f->loc, "duplicate non-template method overload in class " + std::string(cls->name));
                return;
            }
        }
    }

    OverloadCandidate cand;
    cand.name = std::string(f->name);
    cand.mangled = std::string(cls->name) + "::" + std::string(f->name) + key;
    cand.key = key;
    cand.param_count = f->params.size();
    cand.decl = f;
    cand.has_body = f->body != nullptr;
    cand.has_template_params = has_template_params;
    cand.template_count = f->templates.size();
    if (!f->returns.empty() && f->returns[0] && f->returns[0]->type) {
        cand.return_type = f->returns[0]->type;
    } else {
        cand.return_type = nullptr;
    }
    candidates.push_back(cand);
}

void SemanticAnalyzer::register_special(SpecialMethod* sm, const std::string& ns_name) {
    if (!current_class_) {
        error(sm->loc, "special method outside class");
        return;
    }
    std::string method_name;
    switch (sm->kind) {
        case SpecialMethod::Kind::Drop:
            method_name = "DROP";
            break;
        case SpecialMethod::Kind::Ref:
            method_name = "REF";
            break;
        case SpecialMethod::Kind::Deref:
            method_name = "DEREF";
            break;
        case SpecialMethod::Kind::Access:
            method_name = "ACCESS";
            break;
        case SpecialMethod::Kind::Conversion:
            method_name = "conversion";
            break;
        default:
            return;
    }
    std::string key = build_decl_key({});
    if (!current_class_overloads_) {
        current_class_overloads_ = &class_overload_maps_[std::string(current_class_->name)];
    }
    std::string full_name = "special_" + method_name;
    auto& candidates = (*current_class_overloads_)[full_name];
    for (const auto& c : candidates) {
        if (c.key == key) {
            error(sm->loc, "duplicate special method: " + method_name);
            return;
        }
    }

    OverloadCandidate cand;
    cand.name = full_name;
    cand.mangled = std::string(current_class_->name) + "::" + full_name + key;
    cand.key = key;
    cand.param_count = 0;
    cand.decl = sm;
    cand.has_body = sm->body != nullptr;
    cand.has_template_params = false;
    cand.template_count = 0;
    if (sm->kind == SpecialMethod::Kind::Conversion && sm->conv && sm->conv->type) {
        cand.return_type = sm->conv->type;
    } else {
        cand.return_type = nullptr;
    }
    candidates.push_back(cand);
}

void SemanticAnalyzer::resolve_call(Expr* call, Expr::Call& c) {
    std::string callee_name;
    bool is_method_call = false;
    Expr* object_expr = nullptr;

    if (auto* member = c.callee->get_member()) {
        object_expr = member->target;
        callee_name = std::string(member->member);
        is_method_call = true;
    } else if (auto* scope = c.callee->get_scope()) {
        if (auto* ident = scope->target->get_identifier()) {
            callee_name = std::string(ident->name) + "::" + std::string(scope->member);
        } else {
            callee_name = std::string(scope->member);
        }
    } else if (auto* ident = c.callee->get_identifier()) {
        callee_name = std::string(ident->name);
    } else {
        error(call->loc, "invalid callee in function call");
        return;
    }

    std::string call_key = build_call_key(c.args);

    const OverloadCandidate* best = nullptr;
    
    if (is_method_call) {
        if (object_expr && object_expr->type) {
            if (auto* custom = object_expr->type->get_custom()) {
                std::string class_name = std::string(custom->name);
                auto class_it = class_overload_maps_.find(class_name);
                if (class_it != class_overload_maps_.end()) {
                    auto& method_map = class_it->second;
                    auto method_it = method_map.find(callee_name);
                    if (method_it != method_map.end()) {
                        best = match_candidates(callee_name, call_key, c.args, method_it->second);
                    }
                }
            } else {
                error(call->loc, "method call on non-class type");
                return;
            }
        } else {
            error(call->loc, "cannot determine object type for method call");
            return;
        }
    } else {
        auto it = overload_map_.find(callee_name);
        if (it != overload_map_.end()) {
            best = match_candidates(callee_name, call_key, c.args, it->second);
        }
    }

    if (!best) {
        error(call->loc, "no matching function for call to '" + callee_name + "'");
        return;
    }

    if (best->return_type) {
        set_expr_type(call, best->return_type);
    } else {
        set_expr_type(call, make_type(call->loc, Type::Void{}));
    }

    CallResolution res;
    res.call = call;
    res.callee_name = callee_name;
    res.resolved_name = best->name;
    res.resolved_decl = best->decl;
    res.return_type = best->return_type;
    res.is_constructor = false;
    res.resolved = true;
    res.ambiguous = false;
    call_resolutions_.push_back(res);
}

const OverloadCandidate* SemanticAnalyzer::match_candidates(
    const std::string& name,
    const std::string& key,
    const std::vector<Expr*>& args,
    const std::vector<OverloadCandidate>& candidates) {
    (void)name;
    (void)args;

    std::vector<const OverloadCandidate*> matches;
    for (const auto& cand : candidates) {
        if (cand.key == key) {
            matches.push_back(&cand);
        }
    }

    if (matches.empty()) return nullptr;
    if (matches.size() > 1) {
        warning(matches[0]->decl->loc, "ambiguous overload, choosing first");
        return matches[0];
    }
    return matches[0];
}
} // namespace vo