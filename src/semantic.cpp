#include "../include/semantic.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace vo {
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

void SemanticAnalyzer::visit_expr_literal(Expr::Literal& lit) {
    switch(lit.kind) {
        #define X(l,name) case LiteralKind::name: current_expr->type = type##name; break;
        LITERAL_MAPPING
        #undef X
    }
    current_expr->is_lvalue = false;
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
            Symbol cls_sym(SymbolKind::Class, cls->name, nullptr, cls);
            symbols_.insert_member(std::move(cls_sym));

            symbols_.push_scope(std::string(cls->name));
            for (Decl* member : cls->members) {
                collect_decl(member);
            }
            symbols_.pop_scope();
            break;
        }

        case DeclKind::Namespace: {
            auto* ns = static_cast<Namespace*>(decl);
            Symbol ns_sym(SymbolKind::Namespace, ns->name, nullptr, ns);
            symbols_.insert_member(std::move(ns_sym));

            symbols_.push_scope(std::string(ns->name));
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
} // namespace vo