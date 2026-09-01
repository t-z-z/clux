#include "../include/semantic.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace vo {
void SemanticAnalyzer::merge_external_symbols(
    const SymbolTable& external_globals_table,
    const svmap<std::vector<OverloadCandidate>>& external_overloads
) {
    // 合并全局符号表（大表）
    symbols_.merge_globals(external_globals_table.get_global_symbols()); // 注意：需要为 SymbolTable 增加 getter

    // 合并重载表
    for (const auto& [name, candidates] : external_overloads) {
        auto& target = overload_map_[name];
        target.insert(target.end(), candidates.begin(), candidates.end());
    }
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
}
void SemanticAnalyzer::visit_expr_identifier(Expr::Identifier& id) {
    Symbol* sym = symbols_.lookup_local(id.name);
    if (!sym) {
        error(current_expr->loc, "undefined identifier '" + std::string(id.name) + "'");
        return;
    }
}

void SemanticAnalyzer::visit_expr_binary(Expr::Binary& bin) {
    /*Type* lt = bin.left->type;
    Type* rt = bin.right->type;
    if (!lt || !rt) return;

    // 根据操作符确定结果类型，简化处理：算术运算要求数值类型，结果与左操作数相同（或提升）
    if (is_binary(bin.op)) {
        // 检查是否为数值类型（可调用 type_to_ir 判断）
        current_expr().type = lt;  // 简化：结果类型与左操作数相同
    } else if (bin.op == OpKind::And || bin.op == OpKind::Or) {
        // 逻辑运算，结果 bool
        current_expr().type = make_type(loc, Type::Primitive{PrimitiveTypeKind::Bool});
    } else {
        error(current_loc(), "不支持的二元操作符");
    }*/
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
        collect_decl(decl,"");
    }
}

void SemanticAnalyzer::collect_decl(Decl* decl, const std::string& ns_prefix) {
    if (!decl) return;

    switch (decl->kind) {
        case DeclKind::VarList: {
            auto* vl = static_cast<VarList*>(decl);
            // 全局变量：将每个变量名注册为全局符号
            for (const auto& var : vl->vars) {
                std::string full_name = 
            ns_prefix.empty() ? std::string(var.name) : ns_prefix + "::" + std::string(var.name);
                Symbol sym(SymbolKind::Variable, var.name, vl->type, vl);
                symbols_.insert_global(sym, std::move(full_name));  // 直接插入大表，键为 full_name
            }
            break;
        }

        case DeclKind::Func:
        case DeclKind::Operator: {
            auto* func = static_cast<Func*>(decl);
            register_func(func, ns_prefix);
            break;
        }

        case DeclKind::Class: {
            auto* cls = static_cast<Class*>(decl);
            // 类名注册为全局符号（类型名）
            std::string cls_full_name = 
                ns_prefix.empty() ? std::string(cls->name) : ns_prefix + "::" + std::string(cls->name);
            Symbol cls_sym(SymbolKind::Class, cls->name, nullptr, cls);
            symbols_.insert_global(cls_sym, std::move(cls_full_name));

            // 类的成员函数注册（完整限定名包含类名）
            std::string cls_prefix = cls_full_name;
            for (Decl* member : cls->members) {
                collect_decl(member, cls_full_name);
                // 类内的变量、特殊方法等暂不处理（它们属于类作用域，由类自身管理）
            }
            break;
        }

        case DeclKind::Namespace: {
            auto* ns = static_cast<Namespace*>(decl);
            // 命名空间名注册为全局符号（便于限定路径查找）
            std::string ns_full_name = 
                ns_prefix.empty() ? std::string(ns->name) : ns_prefix + "::" + std::string(ns->name);
            Symbol ns_sym(SymbolKind::Namespace, ns->name, nullptr, ns);
            symbols_.insert_global(ns_sym,std::move(ns_full_name));

            // 递归处理命名空间内的声明
            for (Decl* member : ns->members) {
                collect_decl(member, ns_full_name);
            }
            break;
        }

        case DeclKind::Extern: {
            auto* ext = static_cast<Extern*>(decl);
            // Extern 块内的函数按普通函数注册（使用 extern 名作为额外前缀？规范中 extern 后跟库名）
            std::string ext_prefix = 
            ns_prefix.empty() ? std::string(ext->name) : ns_prefix + "::" + std::string(ext->name);
            for (Decl* member : ext->members) {
                if (member->kind == DeclKind::Func) {
                    register_func(static_cast<Func*>(member), ext_prefix);
                }
            }
            break;
        }

        case DeclKind::Enum: {
            auto* e = static_cast<Enum*>(decl);
            // 注册每个枚举变体
            for (const Token& var : e->variants) {
                std::string var_full_name = 
            ns_prefix.empty() ? std::string(var.val) : ns_prefix + "::" + std::string(var.val);
                Symbol var_sym(SymbolKind::EnumVar, var.val, nullptr, e);
                symbols_.insert_global(var_sym, var_full_name);
            }
            break;
        }

        case DeclKind::SpecialMethod: {
            auto* sm = static_cast<SpecialMethod*>(decl);
            register_special(sm, ns_prefix);
            break;
        }
        case DeclKind::Keep:
        case DeclKind::Compose:   // 暂不处理
        default:
            // 这些声明类型不直接注册到全局符号表
            break;
    }
}

void SemanticAnalyzer::register_func(Func* f, const std::string& ns_name) {
    // 构造完整限定名（用于重载表和全局符号表）
    std::string full_name = 
        ns_name.empty() ? std::string(f->name): ns_name + "::" + std::string(f->name);
    // 生成参数类型编码 key（模板参数统一占位，不参与重载）
    std::string key = build_decl_key(f->params);
    // 检查是否存在相同参数数量的同名函数（重载规则：只看函数名和参数个数）
    auto& candidates = overload_map_[full_name];
    for (const auto& existing : candidates) {
        if (existing.key == key) {
            error(f->loc, "dulplicate declaration of functiom '" + std::string(f->name) + "'");
            return;
        }
    }
    // 创建重载候选
    OverloadCandidate cand;
    cand.name = std::string(f->name);
    cand.full_name = full_name;
    cand.key = key;                       // 仅用于诊断或调试
    cand.param_count = f->params.size();
    cand.decl = f;
    cand.return_type = nullptr;           // 返回类型在后续分析中填充
    cand.has_body = (f->body != nullptr);
    cand.has_template_params = !f->templates.empty();
    // 插入重载表
    candidates.push_back(std::move(cand));
    // 插入全局符号表（用于非重载场景，如取函数地址）
    Symbol sym(SymbolKind::Function, f->name, nullptr, f);
    symbols_.insert_global(sym, std::move(full_name));
}

void SemanticAnalyzer::register_special(SpecialMethod* sm, const std::string& ns_name) {
    // 根据特殊方法类型生成内部名称
    std::string internal_name;
    switch (sm->kind) {
        case SpecialMethod::Kind::Drop:       internal_name = "DROP";       break;
        case SpecialMethod::Kind::Ref:        internal_name = "REF";        break;
        case SpecialMethod::Kind::Deref:      internal_name = "DEREF";      break;
        case SpecialMethod::Kind::Access:     internal_name = "ACCESS";     break;
        case SpecialMethod::Kind::Conversion: internal_name = "__conversion"; break;
        default: error(sm->loc,"unknown special method"); break;
    }
    // 构造完整限定名（通常为 类名::DROP）
    std::string full_name = 
    ns_name.empty() ? internal_name : ns_name + "::" + internal_name;
    // 检查是否已存在同名特殊方法（每种特殊方法在类中只能有一个直接定义）
    std::string key = sm->conv ? build_decl_key({sm->conv}) : "";
    auto& candidates = overload_map_[full_name];
    for (const auto& existing : candidates) {
        if (existing.key == key) {
            error(sm->loc, "duplicate special method '" + std::string(sm->name) + "'");
            return;
        }
    }
    // 创建重载候选
    OverloadCandidate cand;
    cand.name = internal_name;
    cand.full_name = full_name;
    cand.key = key;                    // 特殊方法不参与普通重载匹配
    cand.param_count = 0;             // 暂不处理参数（如 Conversion 的目标类型）
    cand.decl = sm;
    cand.return_type = nullptr;
    cand.has_body = (sm->body != nullptr);
    cand.has_template_params = false;
    // 插入重载表
    candidates.push_back(std::move(cand));
    // 插入全局符号表，键为完整限定名
    Symbol sym(SymbolKind::Function, internal_name, nullptr, sm);
    symbols_.insert_global(sym, std::move(full_name));
}
} // namespace vo