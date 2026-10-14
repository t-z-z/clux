#include "../include/translator_mangle.h"

namespace cx {

namespace {

// 基础类型到 LLVM IR 类型名
std::string primitive_ir(PrimitiveTypeKind k) {
    switch (k) {
        case PrimitiveTypeKind::Int8:
        case PrimitiveTypeKind::UInt8:
        case PrimitiveTypeKind::Char:
        case PrimitiveTypeKind::UChar:      return "i8";
        case PrimitiveTypeKind::Short:
        case PrimitiveTypeKind::UShort:     return "i16";
        case PrimitiveTypeKind::Int:
        case PrimitiveTypeKind::UInt:       return "i32";
        case PrimitiveTypeKind::Long:
        case PrimitiveTypeKind::ULong:      return "i64";
        case PrimitiveTypeKind::Int128:
        case PrimitiveTypeKind::UInt128:    return "i128";
        case PrimitiveTypeKind::Float16:    return "half";
        case PrimitiveTypeKind::Float:      return "float";
        case PrimitiveTypeKind::Double:     return "double";
        case PrimitiveTypeKind::Bool:       return "i1";
        case PrimitiveTypeKind::Void:       return "void";
        default:                            return "i32";
    }
}

}

void Mangler::collect(CompUnit& cu) {
    for (Decl* d : cu.top_decls) if (d) walk(d, "");
}

void Mangler::walk(Decl* d, const std::string& prefix) {
    if (!d) return;
    std::string here = prefix.empty()
        ? std::string(d->name) : prefix + "_" + std::string(d->name);
    switch (d->kind) {
        case DeclKind::Namespace:
            for (Decl* m : static_cast<Namespace*>(d)->members) walk(m, here);
            break;
        case DeclKind::Class: {
            mangled_[d] = here;
            for (Decl* m : static_cast<Class*>(d)->members) {
                if (!m) continue;
                if (m->kind == DeclKind::SpecialMethod) {
                    mangled_[m] = here + "_" +
                        special_name(static_cast<SpecialMethod*>(m));
                } else if (m->kind == DeclKind::Conversion) {
                    mangled_[m] = here + "_" +
                        conversion_name(static_cast<Conversion*>(m));
                } else if (m->kind == DeclKind::VarList) {
                    // static 字段存于模块级全局变量，分配全局符号名；
                    // 实例字段进实例 layout，无全局符号
                    auto* vl = static_cast<VarList*>(m);
                    if (vl->specs.has(SpecKind::Static)) {
                        for (VarItem* vi : vl->vars) {
                            if (!vi) continue;
                            fields_[vi] = here + "__" + std::string(vi->name);
                        }
                    }
                } else {
                    walk(m, here);
                }
            }
            break;
        }
        case DeclKind::VarList: {
            // 模块级 / 命名空间级变量：每个 VarItem 一个 LLVM 全局符号
            for (VarItem* vi : static_cast<VarList*>(d)->vars) {
                if (!vi) continue;
                globals_[vi] = prefix.empty()
                    ? std::string(vi->name)
                    : prefix + "_" + std::string(vi->name);
            }
            break;
        }
        case DeclKind::Func:
        case DeclKind::Operator:
            mangled_[d] = here;
            break;
        case DeclKind::Conversion:
            mangled_[d] = conversion_name(static_cast<Conversion*>(d));
            break;
        case DeclKind::Extern:
            for (Decl* m : static_cast<Extern*>(d)->members)
                walk(m, "");  // extern 内符号名保持原样
            break;
        default: break;
    }
}

std::string Mangler::of(Decl* d) const {
    if (!d) return "";
    auto it = mangled_.find(d);
    if (it != mangled_.end()) return it->second;
    return std::string(d->name);
}

std::string Mangler::global_of(const VarItem* vi) const {
    if (!vi) return "";
    auto it = globals_.find(vi);
    return it != globals_.end() ? it->second : std::string();
}

std::string Mangler::field_of(const VarItem* vi) const {
    if (!vi) return "";
    auto it = fields_.find(vi);
    return it != fields_.end() ? it->second : std::string();
}

std::string Mangler::type_key(Type* t) {
    if (!t) return "nullptr";
    if (auto* p = t->get_primitive()) return primitive_ir(p->kind);
    if (auto* p = t->get_pointer())   return "P_" + type_key(p->pointee);
    if (auto* r = t->get_reference()) return "R_" + type_key(r->target);
    if (auto* a = t->get_array_type()) {
        std::string lk = "x";
        if (a->kind == ArrayKind::Slice) lk = "s";
        else if (a->length && a->length->get_literal())
            lk = std::string(a->length->get_literal()->value);
        return "A_" + type_key(a->element) + "_" + lk;
    }
    if (t->get_string_literal_type()) return "S";
    if (auto* fp = t->get_func_ptr()) {
        std::string s = "F";
        for (Type* p : fp->params) s += "_" + type_key(p);
        s += "_ret_" + type_key(fp->return_type);
        return s;
    }
    if (t->is_null()) return "N";
    if (Class* c = as_class(t)) return "C_" + of(c);
    return "x";
}

std::string Mangler::special_name(SpecialMethod* sm) {
    std::string base;
    switch (sm->kind) {
        case SpecialMethod::Kind::Init:       base = "INIT"; break;
        case SpecialMethod::Kind::Drop:       base = "DROP"; break;
        case SpecialMethod::Kind::Ref:        base = "REF"; break;
        case SpecialMethod::Kind::Deref:      base = "DEREF"; break;
        default: break;
    }
    if (sm->kind == SpecialMethod::Kind::Init) {
        for (VarList* vl : sm->params) {
            if (!vl) continue;
            for (VarItem* vi : vl->vars) {
                if (!vi) continue;
                base += "_";
                base += vi->type ? type_key(vi->type)
                                 : (vl->type ? type_key(vl->type) : "x");
            }
        }
    }
    return base;
}

std::string Mangler::conversion_name(Conversion* cf) {
    std::string base = "conversion";
    for (VarList* vl : cf->params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            if (!vi) continue;
            base += "_";
            base += vi->type ? type_key(vi->type)
                             : (vl->type ? type_key(vl->type) : "x");
        }
    }
    base += "_ret";
    if (!cf->returns.empty() && cf->returns.back()) {
        for (VarItem* ri : cf->returns.back()->vars) {
            if (ri && ri->type) base += "_" + type_key(ri->type);
        }
        if (cf->returns.back()->type && cf->returns.back()->vars.empty())
            base += "_" + type_key(cf->returns.back()->type);
    }
    return base;
}

} // namespace cx
