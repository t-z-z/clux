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
        Decl* d = nullptr;
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
/*
// 第一遍：收集顶层符号（支持前向引用）

void SemanticAnalyzer::collect_top_level() {
    has_imports_or_keep_ = false;
    imports_metatype_ = false;
    for (auto& imp : cu_.imports) {
        if (imp->no) continue;
        has_imports_or_keep_ = true;
        if (imp->path == "metatype") imports_metatype_ = true;
    }
    
    for (auto& d : cu_.decls) {
        if (d->is_nested) continue;
        switch (d->kind) {
            case DeclKind::Func: {
                register_func(static_cast<Func*>(d.get()));
                break;
            }
            case DeclKind::Extern: {
                Extern* ex = static_cast<Extern*>(d.get());
                for (auto* m : ex->members)
                    if (m->kind == DeclKind::Func)
                        register_func(static_cast<Func*>(m));
                break;
            }
            case DeclKind::Class: {
                Class* c = static_cast<Class*>(d.get());
                Symbol s;
                s.kind = SymbolKind::Class;
                s.name = c->name;
                s.decl = c;
                symbols_.insert(std::move(s));
                break;
            }
            case DeclKind::Namespace: {
                Namespace* ns = static_cast<Namespace*>(d.get());
                Symbol s;
                s.kind = SymbolKind::Namespace;
                s.name = ns->name;
                s.decl = ns;
                symbols_.insert(std::move(s));
                // 预注册命名空间内函数，保证 ns::fn 可在任意位置解析
                for (auto* m : ns->members) {
                    if (m->kind == DeclKind::Func)
                        register_func(static_cast<Func*>(m), ns->name);
                    else if (m->kind == DeclKind::SpecialMethod)
                        register_special(static_cast<SpecialMethod*>(m), ns->name);
                }
                // namespace <name>: 把命名空间顶层变量注册进 root 作用域（hoisted）
                if (std::find_if(cu_.using_namespaces.begin(), cu_.using_namespaces.end(),
                                 [&](const CompUnit::UsingNamespace& u) {
                                     return u.name == ns->name;
                                 }) != cu_.using_namespaces.end()) {
                    for (auto* m : ns->members) {
                        if (m->kind != DeclKind::VarList) continue;
                        VarList* vl = static_cast<VarList*>(m);
                        if (vl->type) check_type(*vl->type);
                        std::string rt = vl->type ? type_string(*vl->type) : "";
                        for (auto& v : vl->vars) {
                            Symbol vs;
                            vs.kind = SymbolKind::Variable;
                            vs.name = v.name;
                            vs.type = vl->type;
                            vs.rust_type = rt;
                            symbols_.insert(std::move(vs)); // 冲突时跳过（顶层同名优先）
                        }
                    }
                }
                break;
            }
            case DeclKind::Ref: {
                Ref* r = static_cast<Ref*>(d.get());
                Symbol s;
                s.kind = SymbolKind::TypeAlias;
                s.name = r->name;
                s.decl = r;
                symbols_.insert(std::move(s));
                break;
            }
            case DeclKind::Keep:
                has_imports_or_keep_ = true;
                break;
            default:
                break;
        }
    }
    // 校验 using 指令引用的命名空间确实存在
    for (const auto& u : cu_.using_namespaces) {
        Symbol* s = symbols_.lookup(u.name);
        if (!s || s->kind != SymbolKind::Namespace)
            error(u.loc, "using namespace '" + u.name + "' is not defined in this file");
    }
    
}

void SemanticAnalyzer::add_candidate(std::vector<OverloadCandidate>& vec, const std::string& name,
                                     const std::string& key, Decl* decl, Type* return_type, bool has_body) {
    for (auto& c : vec) {
        if (c.key == key) {
            // 前向声明 + 定义：保留带函数体的那个
            if (c.has_body && has_body) {
                error(decl->loc, "duplicate definition of '" + name + "' with the same parameter types");
            } else if (has_body) {
                c.decl = decl;
                c.has_body = true;
                c.return_type = return_type;
            }
            return;
        }
    }
    OverloadCandidate cand;
    cand.name = name;
    cand.mangled = name + "__" + sanitize_ident(key);
    cand.key = key;
    
    cand.param_count = (decl->kind == DeclKind::Func)
        ? count_param_vars(static_cast<Func*>(decl)->params)
        : count_param_vars(static_cast<SpecialMethod*>(decl)->params);
    
        cand.decl = decl;
    cand.return_type = return_type;
    cand.has_body = has_body;
    vec.push_back(std::move(cand));
}

void SemanticAnalyzer::register_func(Func* f, const std::string& ns_name) {
    std::string key = build_decl_key(f->params);
    std::vector<OverloadCandidate>* vec;
    if (ns_name.empty())
        vec = &overload_map_[f->name];
    else
        vec = &namespace_overload_maps_[ns_name][f->name];
    //add_candidate(*vec, f->name, key, f, f->return_type, f->body != nullptr);

    if (!ns_name.empty()) return;
    // 顶层函数符号（供调用侧区分函数/变量）
    Scope* sc = symbols_.current_scope();
    Symbol* s = sc->lookup(f->name);
    if (!s) {
        Symbol sym;
        sym.kind = SymbolKind::Function;
        sym.name = f->name;
        sym.first_func = f;
        sym.func_count = 1;
        sc->insert(std::move(sym));
    } else if (s->kind == SymbolKind::Function) {
        s->first_func = f;
        s->func_count++;
    }
}

void SemanticAnalyzer::register_special(SpecialMethod* sm, const std::string& ns_name) {
    std::string mname = special_name(*sm);
    std::string key = build_decl_key(sm->params);
    std::vector<OverloadCandidate>* vec;
    if (ns_name.empty())
        vec = &overload_map_[mname];
    else
        vec = &namespace_overload_maps_[ns_name][mname];
    add_candidate(*vec, mname, key, sm, sm->type, sm->body != nullptr);
}

// 声明分析
*/
void SemanticAnalyzer::visit(Decl& decl) {

}

void SemanticAnalyzer::visit_func(Func& f) {
    Scope scope(symbols_);
    symbols_.set_current_scope(&scope);
    for (auto& tp : f.templates) {
        symbols_.add_generic(tp.name, tp.dft);
    }
    for (auto* vl : f.params) {
        if (vl && vl->type) check_type(*vl->type);
    }
    for (auto* rt : f.returns) {
        if (rt && rt->type) check_type(*rt->type);
    }
    for (auto* vl : f.params) {
        if (!vl) continue;
        if (vl->type) {
            std::string ir_type = type_to_ir(*vl->type);
            for (auto& v : vl->vars) {
                Symbol sym = Symbol(SymbolKind::Variable, v.name, vl->type, vl, ir_type);
            }
        }
    }
    for (auto* vl : f.returns) {
        if (!vl) continue;
        if (vl->type) {
            std::string ir_type = type_to_ir(*vl->type);
            for (auto& v : vl->vars) {
                Symbol sym = Symbol(SymbolKind::Variable, v.name, vl->type, vl, ir_type);
            }
        }
    }
    // scope 折构 自动弹出
}

void SemanticAnalyzer::visit_operator(Operator& op) {
    visit_func(op);
}

void SemanticAnalyzer::visit_special_method(SpecialMethod& f){
    Scope scope(symbols_);
    symbols_.set_current_scope(&scope);
    if (f.conv && f.conv->type) {
        if (f.conv->type) check_type(*f.conv->type);
    }
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
    Scope scope(symbols_);
    symbols_.set_current_scope(&scope);
    for (auto& tp : c.templates) {
        symbols_.add_generic(tp.name, tp.dft);
    }
    for (Decl* m : c.members) {
        if (!c.can_contain(m)) {
            error(m->loc, "invalid member " + std::string(m->name) + " in class");
            continue;
        }
        Symbol sym;
        if (m->kind == DeclKind::Class) {
            sym.kind = SymbolKind::Class;
        } else if (m->kind == DeclKind::VarList) {
            sym.kind = SymbolKind::Variable;
        } else if (m->kind == DeclKind::Func) {
            sym.kind = SymbolKind::Function;
        } else if (m->kind == DeclKind::SpecialMethod) {
            continue;
        } else if (m->kind == DeclKind::Enum) {

        }
        sym.decl = m; sym.name = m->name;
    }
}

} // namespace vo