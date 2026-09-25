#include "../include/translator.h"
#include "../include/ir_writer.h"
#include "../include/semantic.h"

#include <type_traits>
#include <unordered_set>

namespace cx {


// 收集

void Translator::run() {
    collect_mangled();
    collect_classes();
    for (Decl* d : cu_.top_decls) emit_decl(d);
}

void Translator::emit_decl(Decl* d) {
    if (!d) return;
    switch (d->kind) {
        case DeclKind::Func:
        case DeclKind::Operator:
        case DeclKind::Conversion:
            emit_func(*static_cast<Func*>(d), nullptr);
            break;
        case DeclKind::Namespace:
            for (Decl* m : static_cast<Namespace*>(d)->members) emit_decl(m);
            break;
        case DeclKind::Class:
            emit_class(static_cast<Class*>(d));
            break;
        case DeclKind::Extern:
            for (Decl* m : static_cast<Extern*>(d)->members) {
                if (m && (m->kind == DeclKind::Func || m->kind == DeclKind::Operator))
                    emit_declare(*static_cast<Func*>(m));
            }
            break;
        case DeclKind::Keep:
            ir_.emit_type(std::string(static_cast<Keep*>(d)->code));
            break;
        default: break;
    }
}

void Translator::collect_mangled() {
    for (Decl* d : cu_.top_decls) if (d) walk_mangled(d, "");
}

void Translator::walk_mangled(Decl* d, const std::string& prefix) {
    if (!d) return;
    std::string here = prefix.empty()
        ? std::string(d->name) : prefix + "_" + std::string(d->name);
    switch (d->kind) {
        case DeclKind::Namespace:
            for (Decl* m : static_cast<Namespace*>(d)->members) walk_mangled(m, here);
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
                } else {
                    walk_mangled(m, here);
                }
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
                walk_mangled(m, "");  // extern 内符号名保持原样
            break;
        default: break;
    }
}

std::string Translator::mangled_of(Decl* d) const {
    if (!d) return "";
    auto it = mangled_.find(d);
    if (it != mangled_.end()) return it->second;
    return std::string(d->name);
}

std::string Translator::type_key(Expr* t) {
    std::string s = map_type(t);
    std::string out;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) out += c;
        else if (c == '*') out += "P";
        else if (c == '%') out += "C";
        else out += "_";
    }
    return out;
}

std::string Translator::special_name(SpecialMethod* sm) {
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

std::string Translator::conversion_name(Conversion* cf) {
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

void Translator::collect_classes() {
    for (Decl* d : cu_.top_decls) collect_classes_decl(d);
}

void Translator::collect_classes_decl(Decl* d) {
    if (!d) return;
    switch (d->kind) {
        case DeclKind::Class: {
            Class* c = static_cast<Class*>(d);
            ClassInfo info;
            info.llvm_name = "%" + mangled_of(c);
            int idx = 0;
            for (Decl* m : c->members) {
                if (!m || m->kind != DeclKind::VarList) continue;
                VarList* vl = static_cast<VarList*>(m);
                for (VarItem* vi : vl->vars) {
                    if (!vi) continue;
                    Field f;
                    f.var = vi;
                    f.index = idx++;
                    f.ir_type = vi->type ? map_type(vi->type)
                               : (vl->type ? map_type(vl->type) : "i32");
                    info.field_index[std::string(vi->name)] = f.index;
                    info.fields.push_back(f);
                }
            }
            classes_[c] = std::move(info);
            for (Decl* m : c->members) {
                if (m && m->kind == DeclKind::Class)
                    collect_classes_decl(m);
            }
            break;
        }
        case DeclKind::Namespace:
            for (Decl* m : static_cast<Namespace*>(d)->members)
                collect_classes_decl(m);
            break;
        default: break;
    }
}

Class* Translator::as_class(Expr* t) {
    if (!t) return nullptr;
    if (auto* m = t->get_modifier()) {
        // 承载层：最后一个非 mod 层
        for (int i = (int)m->layers.size() - 1; i >= 0; --i) {
            if (i < (int)m->is_mod.size() && m->is_mod[i]) continue;
            Expr* layer = m->layers[i];
            if (layer && layer->resolved_decl &&
                layer->resolved_decl->kind == DeclKind::Class)
                return static_cast<Class*>(layer->resolved_decl);
        }
        return nullptr;
    }
    if (t->resolved_decl && t->resolved_decl->kind == DeclKind::Class)
        return static_cast<Class*>(t->resolved_decl);
    return nullptr;
}

std::string Translator::map_type(Expr* t) {
    if (!t) return "void";
    if (auto* p = t->get_primitive()) {
        switch (p->kind) {
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
    if (t->get_pointer()) return "ptr";
    if (t->get_reference()) return "ptr";
    if (t->get_array_type()) return "{ ptr, i64 }";
    if (t->get_string_literal_type()) return "ptr";
    if (t->get_func_ptr()) return "ptr";
    if (t->is_null()) return "ptr";
    if (as_class(t)) return "ptr";
    return "i32";
}

SpecialMethod* Translator::find_special(Class* cls, SpecialMethod::Kind k) const {
    if (!cls) return nullptr;
    for (Decl* m : cls->members) {
        if (!m || m->kind != DeclKind::SpecialMethod) continue;
        auto* sm = static_cast<SpecialMethod*>(m);
        if (sm->kind == k) return sm;
    }
    return nullptr;
}

Class* Translator::pointee_class(Expr* t) {
    if (!t) return nullptr;
    if (auto* p = t->get_pointer())   return as_class(p->pointee);
    if (auto* r = t->get_reference()) return as_class(r->target);
    return nullptr;
}

void Translator::emit_guard_call(SpecialMethod* sm, const std::string& val) {
    if (!sm) return;
    if (val == "null") return;
    std::string nn = ir_.reg();
    emit_instr(nn + " = icmp ne ptr " + val + ", null");
    std::string call_l = ir_.named_label("hook.call");
    std::string cont_l = ir_.named_label("hook.cont");
    emit_term("br i1 " + nn + ", label %" + call_l + ", label %" + cont_l);
    open_block(call_l);
    emit_instr("call void @" + IRWriter::quote(mangled_of(sm)) +
               "(ptr " + val + ")");
    emit_term("br label %" + cont_l);
    open_block(cont_l);
}

void Translator::emit_bind_ref(Class* cls, const std::string& val,
                               SpecialMethod::Kind k) {
    if (!cls) return;
    emit_guard_call(find_special(cls, k), val);
}

void Translator::register_deref(const std::string& slot, Class* cls) {
    if (!cls) return;
    if (!find_special(cls, SpecialMethod::Kind::Deref)) return;
    CleanupEntry e;
    e.kind = CleanupEntry::Deref;
    e.slot = slot;
    e.cls = cls;
    e.scope_depth = ctx_.scope_depth;
    ctx_.drops.push_back(e);
}

// 指令发射

void Translator::emit_instr(const std::string& s) {
    if (ctx_.term) return;
    ir_.emit("  " + s);
}

void Translator::emit_term(const std::string& s) {
    if (ctx_.term) return;
    ir_.emit("  " + s);
    ctx_.term = true;
}

void Translator::open_block(const std::string& label) {
    ir_.emit(label + ":");
    ctx_.term = false;
}

// DROP 清理

void Translator::emit_drops_to(int depth) {
    while (!ctx_.drops.empty() && ctx_.drops.back().scope_depth > depth) {
        auto& d = ctx_.drops.back();
        if (!ctx_.term) {
            if (d.kind == CleanupEntry::Deref) {
                std::string v = ir_.reg();
                emit_instr(v + " = load ptr, ptr " + d.slot);
                emit_guard_call(find_special(d.cls, SpecialMethod::Kind::Deref), v);
            } else {
                emit_instr("call void @" + IRWriter::quote(d.drop_fn) +
                           "(ptr " + d.slot + ")");
            }
        }
        ctx_.drops.pop_back();
    }
}

// 函数发射

void Translator::emit_func(Func& f, Class* cls) {
    ir_.begin_func();
    ctx_ = Ctx{};
    ctx_.fn = &f;
    ctx_.current_class = cls;

    bool is_main = (std::string(f.name) == "main" && cls == nullptr);
    bool is_static = f.modfs.has(ModfKind::Static);
    bool has_self = cls != nullptr && !is_static;

    std::string ret_t = "void";
    VarItem* ret_var = nullptr;
    if (!f.returns.empty() && f.returns.back()) {
        VarList* rl = f.returns.back();
        if (rl->type) ret_t = map_type(rl->type);
        if (!rl->vars.empty()) ret_var = rl->vars.back();
    }
    // main 有返回变量：不用 sret，改为局部变量 + 统一 exit 块
    bool main_with_ret = is_main && ret_var;
    bool plain_main = is_main && !ret_var && ret_t == "void";

    std::vector<std::string> params;
    std::vector<std::tuple<VarItem*, std::string, std::string>> arg_inits;

    if (ret_var && !main_with_ret) params.push_back("ptr sret(" + ret_t + ") %ret");
    if (has_self) {
        params.push_back("ptr %self");
        ctx_.self_ptr = "%self";
    }

    int idx = 0;
    for (VarList* vl : f.params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            std::string vt = vi->type ? map_type(vi->type)
                                      : (vl->type ? map_type(vl->type) : "i32");
            std::string argn = "%a" + std::to_string(idx++);
            params.push_back(vt + " " + argn);
            arg_inits.emplace_back(vi, argn, vt);
        }
    }

    std::string ret_sig = (plain_main || main_with_ret) ? "i32" : "void";
    ctx_.ret_type = ret_t;
    if (main_with_ret) ctx_.exit_label = "main.exit";

    std::string sig = "define " + ret_sig + " @" +
                      IRWriter::quote(mangled_of(&f)) + "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) sig += ", ";
        sig += params[i];
    }
    sig += ") {";
    ir_.emit(sig);
    open_block("entry");

    for (auto& [vi, argn, vt] : arg_inits) {
        std::string slot = ir_.reg();
        emit_instr(slot + " = alloca " + vt);
        emit_instr("store " + vt + " " + argn + ", ptr " + slot);
        ctx_.slots[vi] = slot;
        if (vt == "ptr") {
            Expr* vt_expr = vi->type;
            Class* pc = pointee_class(vt_expr);
            if (pc) {
                emit_bind_ref(pc, argn, SpecialMethod::Kind::Ref);
                register_deref(slot, pc);
            }
        }
    }
    if (main_with_ret) {
        std::string slot = ir_.reg();
        emit_instr(slot + " = alloca i32");
        ctx_.slots[ret_var] = slot;
    } else if (ret_var) {
        ctx_.slots[ret_var] = "%ret";
    }

    if (f.body) emit_stmt(f.body);

    if (main_with_ret) {
        if (!ctx_.term) emit_term("br label %" + ctx_.exit_label);
        open_block(ctx_.exit_label);
        std::string rs = ctx_.slots.count(ret_var) ? ctx_.slots[ret_var] : "null";
        std::string r = ir_.reg();
        emit_instr(r + " = load i32, ptr " + rs);
        emit_term("ret i32 " + r);
    } else if (!ctx_.term) {
        emit_drops_to(0);
        if (plain_main) emit_term("ret i32 0");
        else            emit_term("ret void");
    }
    ir_.emit("}");
    ir_.blank();
}

void Translator::emit_special(SpecialMethod& sm, Class* cls) {
    ir_.begin_func();
    ctx_ = Ctx{};
    ctx_.current_class = cls;
    ctx_.self_ptr = "%self";

    std::vector<std::string> params;
    params.push_back("ptr %self");
    std::vector<std::tuple<VarItem*, std::string, std::string>> arg_inits;
    int idx = 0;
    for (VarList* vl : sm.params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            std::string vt = vi->type ? map_type(vi->type)
                                      : (vl->type ? map_type(vl->type) : "i32");
            std::string argn = "%a" + std::to_string(idx++);
            params.push_back(vt + " " + argn);
            arg_inits.emplace_back(vi, argn, vt);
        }
    }

    std::string sig = "define void @" +
                      IRWriter::quote(mangled_of(&sm)) + "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) sig += ", ";
        sig += params[i];
    }
    sig += ") {";
    ir_.emit(sig);
    open_block("entry");

    for (auto& [vi, argn, vt] : arg_inits) {
        std::string slot = ir_.reg();
        emit_instr(slot + " = alloca " + vt);
        emit_instr("store " + vt + " " + argn + ", ptr " + slot);
        ctx_.slots[vi] = slot;
        if (vt == "ptr") {
            Class* pc = pointee_class(vi->type);
            if (pc) {
                emit_bind_ref(pc, argn, SpecialMethod::Kind::Ref);
                register_deref(slot, pc);
            }
        }
    }

    if (sm.body) emit_stmt(sm.body);
    if (!ctx_.term) {
        emit_drops_to(0);
        emit_term("ret void");
    }
    ir_.emit("}");
    ir_.blank();
}

void Translator::emit_class(Class* c) {
    auto& info = classes_[c];
    if (info.llvm_name.empty()) info.llvm_name = "%" + mangled_of(c);

    std::string ty = info.llvm_name + " = type {";
    for (size_t i = 0; i < info.fields.size(); ++i) {
        if (i) ty += ", ";
        ty += info.fields[i].ir_type;
    }
    ty += "}";
    ir_.emit_type(ty);

    std::unordered_set<std::string> emitted;
    for (Decl* m : c->members) {
        if (!m) continue;
        if (m->kind == DeclKind::Func || m->kind == DeclKind::Operator ||
            m->kind == DeclKind::Conversion ||
            m->kind == DeclKind::SpecialMethod) {
            std::string name = mangled_of(m);
            if (!emitted.insert(name).second) continue;
            if (m->kind == DeclKind::SpecialMethod)
                emit_special(*static_cast<SpecialMethod*>(m), c);
            else
                emit_func(*static_cast<Func*>(m), c);
        } else if (m->kind == DeclKind::Class) {
            emit_class(static_cast<Class*>(m));
        }
    }
}

// 语句

void Translator::emit_stmt(Stmt* s) {
    if (!s) return;
    std::visit([this](auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Stmt::Block>) {
            ++ctx_.scope_depth;
            for (Stmt* st : v.stmts) emit_stmt(st);
            --ctx_.scope_depth;
            if (!ctx_.term) emit_drops_to(ctx_.scope_depth);
        } else if constexpr (std::is_same_v<T, Stmt::ExprStmt>) {
            if (v.expr) eval(v.expr);
        } else if constexpr (std::is_same_v<T, Stmt::VarDecl>) {
            emit_var_decl(v.vars);
        } else if constexpr (std::is_same_v<T, Stmt::If>) {
            emit_if(v);
        } else if constexpr (std::is_same_v<T, Stmt::While>) {
            emit_while(v);
        } else if constexpr (std::is_same_v<T, Stmt::For>) {
            emit_for(v);
        } else if constexpr (std::is_same_v<T, Stmt::Return>) {
            emit_return(v);
        } else if constexpr (std::is_same_v<T, Stmt::Break>) {
            emit_break(v);
        } else if constexpr (std::is_same_v<T, Stmt::Continue>) {
            emit_continue(v);
        } else if constexpr (std::is_same_v<T, Stmt::Flag>) {
            emit_flag(v);
        } else if constexpr (std::is_same_v<T, Stmt::Goto>) {
            emit_goto(v);
        } else if constexpr (std::is_same_v<T, Stmt::Keep>) {
            ir_.emit_type(std::string(v.tokens));
        } else if constexpr (std::is_same_v<T, Stmt::Asm>) {
            std::string esc;
            for (char c : v.code) {
                if (c == '"' || c == '\\') esc += '\\';
                if (c == '\n') { esc += "\\0A"; continue; }
                if (c == '\t') { esc += "\\09"; continue; }
                esc += c;
            }
            emit_instr("call void asm sideeffect \"" + esc + "\", \"\"()");
        }
    }, s->data);
}

void Translator::emit_var_decl(VarList* vl) {
    if (!vl) return;
    std::string t = map_type(vl->type);
    for (VarItem* vi : vl->vars) {
        if (!vi) continue;
        Expr* vtype = vi->type ? vi->type : vl->type;
        Class* cls = as_class(vtype);
        if (cls) {
            auto it = classes_.find(cls);
            if (it == classes_.end()) { ctx_.slots[vi] = "null"; continue; }
            std::string inst;
            if (vi->init) {
                Conversion* cf = nullptr;
                if (vi->init->conv && vi->init->conv->kind == DeclKind::Conversion)
                    cf = static_cast<Conversion*>(vi->init->conv);
                if (cf) inst = emit_conversion_call(vi->init, cf);
                else    inst = eval(vi->init);
            } else {
                inst = ir_.reg();
                emit_instr(inst + " = alloca " + it->second.llvm_name);
            }
            std::string slot = ir_.reg();
            emit_instr(slot + " = alloca ptr");
            emit_instr("store ptr " + inst + ", ptr " + slot);
            ctx_.slots[vi] = slot;
            if (!vl->modfs.has(ModfKind::Malloc)) {
                for (Decl* m : cls->members) {
                    if (m && m->kind == DeclKind::SpecialMethod) {
                        auto* sm = static_cast<SpecialMethod*>(m);
                        if (sm->kind == SpecialMethod::Kind::Drop) {
                            CleanupEntry e;
                            e.kind = CleanupEntry::Drop;
                            e.slot = inst;
                            e.drop_fn = mangled_of(sm);
                            e.scope_depth = ctx_.scope_depth;
                            ctx_.drops.push_back(e);
                            break;
                        }
                    }
                }
            }
            continue;
        }
        std::string vt = vtype ? map_type(vtype) : t;
        std::string slot = ir_.reg();
        emit_instr(slot + " = alloca " + vt);
        ctx_.slots[vi] = slot;
        if (vtype && vtype->is_array_type() && !vi->init) {
            int len = array_length_of(vtype);
            if (len > 0) {
                Expr* el = vtype->get_array_type()->element;
                std::string et = el ? map_type(el) : "i32";
                std::string backing = ir_.reg();
                emit_instr(backing + " = alloca " + et + ", i64 " +
                           std::to_string((long long)len));
                std::string s1 = ir_.reg();
                emit_instr(s1 + " = insertvalue { ptr, i64 } undef, ptr " +
                           backing + ", 0");
                std::string s2 = ir_.reg();
                emit_instr(s2 + " = insertvalue { ptr, i64 } " + s1 +
                           ", i64 " + std::to_string((long long)len) + ", 1");
                emit_instr("store { ptr, i64 } " + s2 + ", ptr " + slot);
                continue;
            }
        }
        if (vtype && vtype->is_array_type() && vi->init &&
            vi->init->type && vi->init->type->is_string_literal_type()) {
            auto* lit = vi->init->get_literal();
            std::string content = lit ? std::string(lit->value) : std::string();
            size_t n = content.size();
            std::string sym = ir_.string_const(content);
            std::string backing = ir_.reg();
            emit_instr(backing + " = alloca i8, i64 " +
                       std::to_string(n ? n : 1));
            for (size_t k = 0; k < n; ++k) {
                std::string src = ir_.reg();
                emit_instr(src + " = getelementptr i8, ptr @" + sym +
                           ", i64 " + std::to_string(k));
                std::string b = ir_.reg();
                emit_instr(b + " = load i8, ptr " + src);
                std::string dst = ir_.reg();
                emit_instr(dst + " = getelementptr i8, ptr " + backing +
                           ", i64 " + std::to_string(k));
                emit_instr("store i8 " + b + ", ptr " + dst);
            }
            std::string s1 = ir_.reg();
            emit_instr(s1 + " = insertvalue { ptr, i64 } undef, ptr " +
                       backing + ", 0");
            std::string s2 = ir_.reg();
            emit_instr(s2 + " = insertvalue { ptr, i64 } " + s1 + ", i64 " +
                       std::to_string(static_cast<long long>(n)) + ", 1");
            emit_instr("store { ptr, i64 } " + s2 + ", ptr " + slot);
            continue;
        }
        if (vi->init) {
            std::string v = eval(vi->init);
            std::string src_t = vi->init->type ? map_type(vi->init->type) : vt;
            if (src_t != vt) v = cast_value(v, src_t, vt);
            emit_instr("store " + vt + " " + v + ", ptr " + slot);
            if (vt == "ptr") {
                Class* pc = pointee_class(vtype);
                if (pc) {
                    emit_bind_ref(pc, v, SpecialMethod::Kind::Ref);
                    register_deref(slot, pc);
                }
            }
        }
    }
}

void Translator::emit_if(Stmt::If& i) {
    std::string cond = eval_cond(i.condition);
    std::string then_l = ir_.named_label("if.then");
    std::string else_l = ir_.named_label("if.else");
    std::string end_l  = ir_.named_label("if.end");
    emit_term("br i1 " + cond + ", label %" + then_l + ", label %" + else_l);

    open_block(then_l);
    ++ctx_.scope_depth;
    emit_stmt(i.then_branch);
    --ctx_.scope_depth;
    if (!ctx_.term) emit_drops_to(ctx_.scope_depth);
    if (!ctx_.term) emit_term("br label %" + end_l);

    open_block(else_l);
    ++ctx_.scope_depth;
    if (i.else_branch) emit_stmt(i.else_branch);
    --ctx_.scope_depth;
    if (!ctx_.term) emit_drops_to(ctx_.scope_depth);
    if (!ctx_.term) emit_term("br label %" + end_l);

    open_block(end_l);
}

void Translator::emit_while(Stmt::While& w) {
    std::string cond_l = ir_.named_label("while.cond");
    std::string body_l = ir_.named_label("while.body");
    std::string end_l  = ir_.named_label("while.end");
    emit_term("br label %" + cond_l);

    open_block(cond_l);
    std::string c = eval_cond(w.condition);
    emit_term("br i1 " + c + ", label %" + body_l + ", label %" + end_l);

    open_block(body_l);
    loops_.push_back({end_l, cond_l, ctx_.scope_depth});
    ++ctx_.scope_depth;
    emit_stmt(w.body);
    --ctx_.scope_depth;
    if (!ctx_.term) emit_drops_to(ctx_.scope_depth);
    loops_.pop_back();
    if (!ctx_.term) emit_term("br label %" + cond_l);

    open_block(end_l);
}

void Translator::emit_for(Stmt::For& f) {
    ++ctx_.scope_depth;
    if (f.init) emit_stmt(f.init);

    std::string cond_l = ir_.named_label("for.cond");
    std::string body_l = ir_.named_label("for.body");
    std::string next_l = ir_.named_label("for.next");
    std::string end_l  = ir_.named_label("for.end");
    emit_term("br label %" + cond_l);

    open_block(cond_l);
    std::string c = f.condition ? eval_cond(f.condition) : "1";
    emit_term("br i1 " + c + ", label %" + body_l + ", label %" + end_l);

    open_block(body_l);
    loops_.push_back({end_l, next_l, ctx_.scope_depth});
    ++ctx_.scope_depth;
    emit_stmt(f.body);
    --ctx_.scope_depth;
    if (!ctx_.term) emit_drops_to(ctx_.scope_depth);
    loops_.pop_back();
    if (!ctx_.term) emit_term("br label %" + next_l);

    open_block(next_l);
    if (f.next) emit_stmt(f.next);
    if (!ctx_.term) emit_term("br label %" + cond_l);

    open_block(end_l);
    --ctx_.scope_depth;
    if (!ctx_.term) emit_drops_to(ctx_.scope_depth);
}

void Translator::emit_return(Stmt::Return&) {
    if (!ctx_.exit_label.empty()) {
        emit_drops_to(0);
        emit_term("br label %" + ctx_.exit_label);
        return;
    }
    emit_drops_to(0);
    emit_term("ret void");
}

void Translator::emit_break(Stmt::Break&) {
    if (loops_.empty()) return;
    emit_drops_to(loops_.back().scope_depth);
    if (!ctx_.term) emit_term("br label %" + loops_.back().break_label);
}

void Translator::emit_continue(Stmt::Continue&) {
    if (loops_.empty()) return;
    emit_drops_to(loops_.back().scope_depth);
    if (!ctx_.term) emit_term("br label %" + loops_.back().continue_label);
}

void Translator::emit_flag(Stmt::Flag& f) {
    std::string tgt = "flag." + std::string(f.label);
    if (!ctx_.term) emit_term("br label %" + tgt);
    open_block(tgt);
}

void Translator::emit_goto(Stmt::Goto& g) {
    std::string tgt = "flag." + std::string(g.label);
    emit_term("br label %" + tgt);
}

// 表达式

std::string Translator::to_bool(Expr* e, const std::string& v) {
    if (!e || !e->type) return v;
    if (e->type->is_pointer()) {
        std::string r = ir_.reg();
        emit_instr(r + " = icmp ne ptr " + v + ", null");
        return r;
    }
    if (e->type->is_array_type()) {
        std::string n = ir_.reg();
        emit_instr(n + " = extractvalue { ptr, i64 } " + v + ", 1");
        std::string r = ir_.reg();
        emit_instr(r + " = icmp ne i64 " + n + ", 0");
        return r;
    }
    return v;
}

std::string Translator::eval_cond(Expr* e) {
    return to_bool(e, eval(e));
}

std::string Translator::eval(Expr* e) {
    if (!e) return "0";
    if (e->is_null())                     return "null";
    if (auto* lit = e->get_literal())     return eval_literal(lit);
    if (auto* id  = e->get_identifier())  return eval_identifier(e, id);
    if (auto* b   = e->get_binary())      return eval_binary(e, b);
    if (auto* u   = e->get_unary())       return eval_unary(e, u);
    if (auto* a   = e->get_assign())      return eval_assign(a);
    if (auto* c   = e->get_call())        return eval_call(e, c);
    if (auto* ix  = e->get_index())       return eval_index(e, ix);
    if (auto* m   = e->get_member())      return eval_member(e, m);
    if (auto* ar  = e->get_array())       return eval_array(e, ar);
    if (auto* ca  = e->get_cast())        return eval_cast(e, ca);
    if (auto* tr  = e->get_ternary())     return eval_ternary(e, tr);
    if (auto* cl  = e->get_closure())     return eval_closure(e, cl);
    if (auto* sp  = e->get_spawn())       return eval_spawn(e, sp);
    return "0";
}

std::string Translator::eval_lvalue(Expr* e) {
    if (!e) return "";
    if (auto* id = e->get_identifier()) {
        if (std::string(id->name) == "self" && !ctx_.self_ptr.empty())
            return ctx_.self_ptr;
        if (e->sym && e->sym->kind == SymbolKind::Variable) {
            if (auto* vi = dynamic_cast<VarItem*>(e->sym->node)) {
                auto it = ctx_.slots.find(vi);
                if (it != ctx_.slots.end()) return it->second;
                if (ctx_.current_class) {
                    auto cit = classes_.find(ctx_.current_class);
                    if (cit != classes_.end()) {
                        auto fit = cit->second.field_index.find(std::string(vi->name));
                        if (fit != cit->second.field_index.end()) {
                            std::string addr = ir_.reg();
                            emit_instr(addr + " = getelementptr " + cit->second.llvm_name +
                                       ", ptr " + ctx_.self_ptr + ", i32 0, i32 " +
                                       std::to_string(fit->second));
                            return addr;
                        }
                    }
                }
            }
        }
    }
    if (auto* u = e->get_unary()) {
        if (u->op == OpKind::Mul) return eval(u->operand);
    }
    if (auto* ix = e->get_index()) {
        std::string base = data_ptr_of(ix->target);
        std::string idx = eval(ix->index);
        std::string it = ix->index && ix->index->type
                       ? map_type(ix->index->type) : "i32";
        if (it != "i64" && !idx.empty() && idx[0] == '%') {
            std::string ext = ir_.reg();
            emit_instr(ext + " = sext " + it + " " + idx + " to i64");
            idx = ext;
        }
        std::string et = map_type(e->type);
        std::string addr = ir_.reg();
        emit_instr(addr + " = getelementptr " + et + ", ptr " + base + ", i64 " + idx);
        return addr;
    }
    if (auto* m = e->get_member()) {
        Expr* tgt = m->target;
        if (Class* cls = as_class(tgt ? tgt->type : nullptr)) {
            auto it = classes_.find(cls);
            if (it != classes_.end()) {
                auto fit = it->second.field_index.find(std::string(m->member));
                if (fit != it->second.field_index.end()) {
                    std::string base = eval(tgt);
                    std::string addr = ir_.reg();
                    emit_instr(addr + " = getelementptr " + it->second.llvm_name +
                               ", ptr " + base + ", i32 0, i32 " +
                               std::to_string(fit->second));
                    return addr;
                }
            }
        }
    }
    return "";
}

std::string Translator::data_ptr_of(Expr* e) {
    if (!e || !e->type) return "null";
    if (e->type->is_pointer()) return eval(e);
    if (e->type->is_array_type()) {
        std::string s = eval(e);
        std::string r = ir_.reg();
        emit_instr(r + " = extractvalue { ptr, i64 } " + s + ", 0");
        return r;
    }
    return "null";
}

std::string Translator::eval_literal(Expr::Literal* lit) {
    switch (lit->kind) {
        case LiteralKind::Bool:
            return (lit->value == "true" || lit->value == "1") ? "1" : "0";
        case LiteralKind::Char: {
            unsigned char c = lit->value.empty()
                            ? 0 : static_cast<unsigned char>(lit->value[0]);
            return std::to_string(static_cast<unsigned>(c));
        }
        case LiteralKind::String:
            return "@" + ir_.string_const(lit->value);
        default:
            return std::string(lit->value);
    }
}

std::string Translator::eval_identifier(Expr* e, Expr::Identifier* id) {
    if (std::string(id->name) == "self" && !ctx_.self_ptr.empty())
        return ctx_.self_ptr;
    if (e->sym && e->sym->kind == SymbolKind::Variable) {
        if (auto* vi = dynamic_cast<VarItem*>(e->sym->node)) {
            auto it = ctx_.slots.find(vi);
            if (it != ctx_.slots.end()) {
                if (as_class(e->type)) {
                    // 类实例变量：槽里存的是实例指针
                    std::string r = ir_.reg();
                    emit_instr(r + " = load ptr, ptr " + it->second);
                    return r;
                }
                std::string t = e->type ? map_type(e->type) : "i32";
                std::string r = ir_.reg();
                emit_instr(r + " = load " + t + ", ptr " + it->second);
                return r;
            }
            if (ctx_.current_class) {
                auto cit = classes_.find(ctx_.current_class);
                if (cit != classes_.end()) {
                    auto fit = cit->second.field_index.find(std::string(vi->name));
                    if (fit != cit->second.field_index.end()) {
                        std::string addr = ir_.reg();
                        emit_instr(addr + " = getelementptr " + cit->second.llvm_name +
                                   ", ptr " + ctx_.self_ptr + ", i32 0, i32 " +
                                   std::to_string(fit->second));
                        std::string t = e->type ? map_type(e->type) : "i32";
                        std::string r = ir_.reg();
                        emit_instr(r + " = load " + t + ", ptr " + addr);
                        return r;
                    }
                }
            }
        }
    }
    return "0";
}

std::string Translator::eval_binary(Expr* e, Expr::Binary* b) {
    // 操作符重载
    if (e->sym && e->sym->kind == SymbolKind::Function) {
        if (Decl* d = e->sym->get_decl()) {
            if (d->kind == DeclKind::Operator)
                return emit_operator_call(e, static_cast<Operator*>(d),
                                          b->left, b->right);
        }
    }
    std::string lt = b->left && b->left->type ? map_type(b->left->type) : "i32";
    std::string lv = eval(b->left);
    std::string rv = eval(b->right);
    std::string res = ir_.reg();
    bool is_fp = (lt == "float" || lt == "double" || lt == "half");
    switch (b->op) {
        case OpKind::Plus:
            emit_instr(res + " = " + (is_fp ? "fadd " : "add ") + lt + " " + lv + ", " + rv);
            break;
        case OpKind::Minus:
            emit_instr(res + " = " + (is_fp ? "fsub " : "sub ") + lt + " " + lv + ", " + rv);
            break;
        case OpKind::Mul:
            emit_instr(res + " = " + (is_fp ? "fmul " : "mul ") + lt + " " + lv + ", " + rv);
            break;
        case OpKind::Div:
            emit_instr(res + " = " + (is_fp ? "fdiv " : "sdiv ") + lt + " " + lv + ", " + rv);
            break;
        case OpKind::Mod:
            emit_instr(res + " = srem " + lt + " " + lv + ", " + rv);
            break;
        case OpKind::Less:
            emit_instr(res + " = " + (is_fp ? "fcmp olt " : "icmp slt ") + lt + " " + lv + ", " + rv);
            break;
        case OpKind::Greater:
            emit_instr(res + " = " + (is_fp ? "fcmp ogt " : "icmp sgt ") + lt + " " + lv + ", " + rv);
            break;
        case OpKind::LessEqual:
            emit_instr(res + " = " + (is_fp ? "fcmp ole " : "icmp sle ") + lt + " " + lv + ", " + rv);
            break;
        case OpKind::GreaterEqual:
            emit_instr(res + " = " + (is_fp ? "fcmp oge " : "icmp sge ") + lt + " " + lv + ", " + rv);
            break;
        case OpKind::Equal:
            emit_instr(res + " = " + (is_fp ? "fcmp oeq " : "icmp eq ") + lt + " " + lv + ", " + rv);
            break;
        case OpKind::NotEqual:
            emit_instr(res + " = " + (is_fp ? "fcmp one " : "icmp ne ") + lt + " " + lv + ", " + rv);
            break;
        case OpKind::BitAnd:
            emit_instr(res + " = and " + lt + " " + lv + ", " + rv);
            break;
        case OpKind::BitOr:
            emit_instr(res + " = or " + lt + " " + lv + ", " + rv);
            break;
        case OpKind::BitXor:
            emit_instr(res + " = xor " + lt + " " + lv + ", " + rv);
            break;
        case OpKind::Shl:
            emit_instr(res + " = shl " + lt + " " + lv + ", " + rv);
            break;
        case OpKind::Shr:
            emit_instr(res + " = ashr " + lt + " " + lv + ", " + rv);
            break;
        case OpKind::And: {
            std::string l = to_bool(b->left, lv);
            std::string rr = to_bool(b->right, rv);
            emit_instr(res + " = and i1 " + l + ", " + rr);
            break;
        }
        case OpKind::Or: {
            std::string l = to_bool(b->left, lv);
            std::string rr = to_bool(b->right, rv);
            emit_instr(res + " = or i1 " + l + ", " + rr);
            break;
        }
        default:
            emit_instr(res + " = add " + lt + " 0, 0");
            break;
    }
    return res;
}

std::string Translator::eval_unary(Expr* e, Expr::Unary* u) {
    if (u->op == OpKind::BitAnd) {
        Expr* op = u->operand;
        if (op && as_class(op->type)) {
            std::string slot = eval_lvalue(op);
            std::string r = ir_.reg();
            emit_instr(r + " = load ptr, ptr " + slot);
            return r;
        }
        return eval_lvalue(op);
    }
    std::string ot = u->operand && u->operand->type ? map_type(u->operand->type) : "i32";
    std::string ov = eval(u->operand);
    std::string res = ir_.reg();
    switch (u->op) {
        case OpKind::Minus:
            if (ot == "float" || ot == "double" || ot == "half")
                emit_instr(res + " = fneg " + ot + " " + ov);
            else
                emit_instr(res + " = sub " + ot + " 0, " + ov);
            break;
        case OpKind::Not: {
            std::string b = to_bool(u->operand, ov);
            emit_instr(res + " = xor i1 " + b + ", true");
            break;
        }
        case OpKind::BitNot:
            emit_instr(res + " = xor " + ot + " " + ov + ", -1");
            break;
        case OpKind::Plus:
            return ov;
        case OpKind::Mul: {
            std::string t = e->type ? map_type(e->type)
                                    : (u->operand && u->operand->type
                                       ? map_type(u->operand->type) : "i32");
            emit_instr(res + " = load " + t + ", ptr " + ov);
            return res;
        }
        default:
            return ov;
    }
    return res;
}

std::string Translator::eval_assign(Expr::Assign* a) {
    std::string ptr = eval_lvalue(a->target);
    if (ptr.empty()) return "0";
    std::string t = a->target && a->target->type ? map_type(a->target->type) : "i32";
    // 字符串字面量赋给切片：构造 {ptr,i64}，而非把常量指针 @.str 直接存
    std::string v;
    if (t == "{ ptr, i64 }" && a->value && a->value->type &&
        a->value->type->is_string_literal_type())
        v = build_string_slice(a->value);
    else
        v = eval(a->value);
    if (a->op != OpKind::Assign) {
        std::string cur = ir_.reg();
        emit_instr(cur + " = load " + t + ", ptr " + ptr);
        std::string r2 = ir_.reg();
        switch (a->op) {
            case OpKind::PlusAssign:  emit_instr(r2 + " = add " + t + " " + cur + ", " + v); break;
            case OpKind::MinusAssign: emit_instr(r2 + " = sub " + t + " " + cur + ", " + v); break;
            case OpKind::MulAssign:   emit_instr(r2 + " = mul " + t + " " + cur + ", " + v); break;
            case OpKind::DivAssign:   emit_instr(r2 + " = sdiv " + t + " " + cur + ", " + v); break;
            case OpKind::ModAssign:   emit_instr(r2 + " = srem " + t + " " + cur + ", " + v); break;
            case OpKind::AndAssign:   emit_instr(r2 + " = and " + t + " " + cur + ", " + v); break;
            case OpKind::OrAssign:    emit_instr(r2 + " = or " + t + " " + cur + ", " + v); break;
            case OpKind::XorAssign:   emit_instr(r2 + " = xor " + t + " " + cur + ", " + v); break;
            case OpKind::ShlAssign:   emit_instr(r2 + " = shl " + t + " " + cur + ", " + v); break;
            case OpKind::ShrAssign:   emit_instr(r2 + " = ashr " + t + " " + cur + ", " + v); break;
            default:                  emit_instr(r2 + " = add " + t + " " + cur + ", " + v); break;
        }
        v = r2;
    }
    emit_instr("store " + t + " " + v + ", ptr " + ptr);
    if (t == "ptr" && a->target) {
        Class* pc = pointee_class(a->target->type);
        if (pc) emit_bind_ref(pc, v, SpecialMethod::Kind::Ref);
    }
    return v;
}

std::string Translator::eval_index(Expr* e, Expr::Index*) {
    std::string addr = eval_lvalue(e);
    if (addr.empty()) return "0";
    std::string et = map_type(e->type);
    std::string v = ir_.reg();
    emit_instr(v + " = load " + et + ", ptr " + addr);
    return v;
}

std::string Translator::eval_member(Expr* e, Expr::Member* m) {
    if (m->member == "len" && m->target && m->target->type &&
        m->target->type->is_array_type()) {
        std::string s = eval(m->target);
        std::string len64 = ir_.reg();
        emit_instr(len64 + " = extractvalue { ptr, i64 } " + s + ", 1");
        std::string len32 = ir_.reg();
        emit_instr(len32 + " = trunc i64 " + len64 + " to i32");
        return len32;
    }
    // 类字段
    Expr* tgt = m->target;
    if (Class* cls = as_class(tgt ? tgt->type : nullptr)) {
        auto it = classes_.find(cls);
        if (it != classes_.end()) {
            auto fit = it->second.field_index.find(std::string(m->member));
            if (fit != it->second.field_index.end()) {
                std::string base = eval(tgt);
                std::string addr = ir_.reg();
                emit_instr(addr + " = getelementptr " + it->second.llvm_name +
                           ", ptr " + base + ", i32 0, i32 " +
                           std::to_string(fit->second));
                std::string t = e->type ? map_type(e->type) : "i32";
                std::string v = ir_.reg();
                emit_instr(v + " = load " + t + ", ptr " + addr);
                return v;
            }
        }
    }
    return "0";
}

std::string Translator::eval_array(Expr* e, Expr::Array* arr) {
    Expr* elem_type = nullptr;
    if (e->type && e->type->is_array_type())
        elem_type = e->type->get_array_type()->element;
    std::string et;
    if (elem_type && elem_type->is_string_literal_type())
        et = "{ ptr, i64 }";
    else
        et = elem_type ? map_type(elem_type) : "i32";

    // 长度以声明类型的编译期常量为准；否则由字面量成员数决定
    size_t n = arr->members.size();
    int decl_len = array_length_of(e->type);
    if (decl_len > 0) n = (size_t)decl_len;
    else if (n == 0 && arr->fill_value) n = 1;

    std::string backing = ir_.reg();
    emit_instr(backing + " = alloca " + et + ", i64 " +
               std::to_string((long long)(n ? n : 1)));

    if (!arr->members.empty()) {
        size_t i = 0;
        for (Expr* mem : arr->members) {
            if (i >= n) break;
            if (!mem) { ++i; continue; }
            std::string v;
            if (mem->is_literal() && mem->get_literal()->kind == LiteralKind::String)
                v = build_string_slice(mem);
            else
                v = eval(mem);
            std::string p = ir_.reg();
            emit_instr(p + " = getelementptr " + et + ", ptr " + backing +
                       ", i64 " + std::to_string(i));
            emit_instr("store " + et + " " + v + ", ptr " + p);
            ++i;
        }
    } else if (arr->fill_value) {
        // 字符串填充：每个元素独立构造 backing，避免写入共享数据
        bool str_fill = arr->fill_value->is_literal() &&
                        arr->fill_value->get_literal()->kind == LiteralKind::String;
        std::string v;
        if (!str_fill) v = eval(arr->fill_value);
        for (size_t k = 0; k < n; ++k) {
            std::string vk = str_fill ? build_string_slice(arr->fill_value) : v;
            std::string p = ir_.reg();
            emit_instr(p + " = getelementptr " + et + ", ptr " + backing +
                       ", i64 " + std::to_string(k));
            emit_instr("store " + et + " " + vk + ", ptr " + p);
        }
    }

    std::string s1 = ir_.reg();
    emit_instr(s1 + " = insertvalue { ptr, i64 } undef, ptr " + backing + ", 0");
    std::string s2 = ir_.reg();
    emit_instr(s2 + " = insertvalue { ptr, i64 } " + s1 + ", i64 " +
               std::to_string((long long)n) + ", 1");
    return s2;
}

std::string Translator::build_string_slice(Expr* lit) {
    auto* l = lit ? lit->get_literal() : nullptr;
    std::string content = l ? std::string(l->value) : std::string();
    size_t n = content.size();
    std::string sym = ir_.string_const(content);
    std::string backing = ir_.reg();
    emit_instr(backing + " = alloca i8, i64 " + std::to_string(n ? n : 1));
    for (size_t k = 0; k < n; ++k) {
        std::string src = ir_.reg();
        emit_instr(src + " = getelementptr i8, ptr @" + sym +
                   ", i64 " + std::to_string(k));
        std::string b = ir_.reg();
        emit_instr(b + " = load i8, ptr " + src);
        std::string dst = ir_.reg();
        emit_instr(dst + " = getelementptr i8, ptr " + backing +
                   ", i64 " + std::to_string(k));
        emit_instr("store i8 " + b + ", ptr " + dst);
    }
    std::string s1 = ir_.reg();
    emit_instr(s1 + " = insertvalue { ptr, i64 } undef, ptr " + backing + ", 0");
    std::string s2 = ir_.reg();
    emit_instr(s2 + " = insertvalue { ptr, i64 } " + s1 + ", i64 " +
               std::to_string((long long)n) + ", 1");
    return s2;
}

std::string Translator::cast_value(const std::string& v,
                                   const std::string& src,
                                   const std::string& dst) {
    if (src == dst) return v;
    auto is_int_n = [](const std::string& t) {
        return t.size() >= 2 && t[0] == 'i' && t != "i1";
    };
    auto is_fp = [](const std::string& t) {
        return t == "float" || t == "double" || t == "half";
    };
    auto int_bits = [](const std::string& t) { return std::stoi(t.substr(1)); };
    auto fp_bits = [](const std::string& t) {
        return t == "half" ? 16 : (t == "float" ? 32 : 64);
    };
    std::string r = ir_.reg();

    if (src == "i1") {
        if (is_int_n(dst)) { emit_instr(r + " = zext i1 " + v + " to " + dst); return r; }
        if (is_fp(dst))    { emit_instr(r + " = uitofp i1 " + v + " to " + dst); return r; }
        return v;
    }
    if (dst == "i1") {
        if (is_int_n(src)) { emit_instr(r + " = icmp ne " + src + " " + v + ", 0"); return r; }
        if (is_fp(src))    { emit_instr(r + " = fcmp one " + src + " " + v + ", 0.0"); return r; }
        if (src == "ptr")  { emit_instr(r + " = icmp ne ptr " + v + ", null"); return r; }
        if (src == "{ ptr, i64 }") {
            std::string n = ir_.reg();
            emit_instr(n + " = extractvalue { ptr, i64 } " + v + ", 1");
            emit_instr(r + " = icmp ne i64 " + n + ", 0");
            return r;
        }
        return v;
    }
    if (is_int_n(src) && is_int_n(dst)) {
        int s = int_bits(src), d = int_bits(dst);
        if (s < d)      emit_instr(r + " = sext " + src + " " + v + " to " + dst);
        else if (s > d) emit_instr(r + " = trunc " + src + " " + v + " to " + dst);
        else            return v;
        return r;
    }
    if (is_int_n(src) && is_fp(dst)) {
        emit_instr(r + " = sitofp " + src + " " + v + " to " + dst);
        return r;
    }
    if (is_fp(src) && is_int_n(dst)) {
        emit_instr(r + " = fptosi " + src + " " + v + " to " + dst);
        return r;
    }
    if (is_fp(src) && is_fp(dst)) {
        int s = fp_bits(src), d = fp_bits(dst);
        if (s < d)      emit_instr(r + " = fpext " + src + " " + v + " to " + dst);
        else if (s > d) emit_instr(r + " = fptrunc " + src + " " + v + " to " + dst);
        else            return v;
        return r;
    }
    return v;
}

std::string Translator::eval_cast(Expr*, Expr::Cast* c) {
    Expr* src_expr = c->operand ? c->operand->type : nullptr;
    std::string src = map_type(src_expr);
    std::string dst = map_type(c->target_type);
    std::string v = eval(c->operand);
    return cast_value(v, src, dst);
}

std::string Translator::emit_conversion_call(Expr* e, Conversion* cf) {
    Expr* ret_type_expr = nullptr;
    if (!cf->returns.empty() && cf->returns.back()) {
        VarList* rl = cf->returns.back();
        if (!rl->vars.empty() && rl->vars.back() && rl->vars.back()->type)
            ret_type_expr = rl->vars.back()->type;
        else
            ret_type_expr = rl->type;
    }
    std::string ret_ir = ret_type_expr ? map_type(ret_type_expr) : "ptr";

    std::string sret_slot = ir_.reg();
    emit_instr(sret_slot + " = alloca " + ret_ir);

    std::vector<std::string> args;
    args.push_back("ptr sret(" + ret_ir + ") " + sret_slot);

    int param_count = 0;
    for (VarList* vl : cf->params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) if (vi) ++param_count;
    }

    if (param_count == 0) {
        // 无参形式：声明在源类，e 即源对象
        std::string self = eval(e);
        args.push_back("ptr " + self);
    } else {
        // 有参形式：e 即源值，对应第一个形参
        int i = 0;
        for (VarList* vl : cf->params) {
            if (!vl) continue;
            for (VarItem* vi : vl->vars) {
                if (!vi) continue;
                if (i == 0) {
                    Expr* pt_expr = vi->type ? vi->type : vl->type;
                    std::string pt = pt_expr ? map_type(pt_expr) : "i32";
                    std::string v = eval(e);
                    args.push_back(pt + " " + v);
                }
                ++i;
            }
        }
    }

    std::string sig = "call void @" + IRWriter::quote(mangled_of(cf)) + "(";
    for (size_t k = 0; k < args.size(); ++k) {
        if (k) sig += ", ";
        sig += args[k];
    }
    sig += ")";
    emit_instr(sig);
    // 类返回值经 sret 传出的是实例指针，加载后返回
    if (ret_type_expr && as_class(ret_type_expr)) {
        std::string loaded = ir_.reg();
        emit_instr(loaded + " = load ptr, ptr " + sret_slot);
        return loaded;
    }
    return sret_slot;
}

std::string Translator::emit_operator_call(Expr* e, Operator* op,
                                           Expr* lhs, Expr* rhs) {
    std::string ret_t = e->type ? map_type(e->type) : "void";
    std::string sret_slot = ir_.reg();
    emit_instr(sret_slot + " = alloca " + ret_t);

    std::vector<std::string> args;
    args.push_back("ptr sret(" + ret_t + ") " + sret_slot);

    std::string self_ptr = eval(lhs);
    args.push_back("ptr " + self_ptr);

    std::vector<Expr*> rhs_list = {rhs};
    int i = 0;
    for (VarList* vl : op->params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            if (i >= (int)rhs_list.size()) break;
            Expr* arg = rhs_list[i];
            Expr* pt_expr = vi->type ? vi->type : vl->type;
            std::string pt = pt_expr ? map_type(pt_expr) : "i32";
            std::string v = eval(arg);
            args.push_back(pt + " " + v);
            ++i;
        }
    }

    std::string sig = "call void @" + IRWriter::quote(mangled_of(op)) + "(";
    for (size_t k = 0; k < args.size(); ++k) {
        if (k) sig += ", ";
        sig += args[k];
    }
    sig += ")";
    emit_instr(sig);
    return sret_slot;
}

void Translator::emit_declare(Func& f) {
    std::string ret_t = "void";
    VarItem* ret_var = nullptr;
    if (!f.returns.empty() && f.returns.back()) {
        VarList* rl = f.returns.back();
        if (rl->type) ret_t = map_type(rl->type);
        if (!rl->vars.empty()) ret_var = rl->vars.back();
    }

    std::vector<std::string> params;
    if (ret_var) params.push_back("ptr sret(" + ret_t + ") %ret");
    int idx = 0;
    for (VarList* vl : f.params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            std::string vt = vi->type ? map_type(vi->type)
                                      : (vl->type ? map_type(vl->type) : "i32");
            params.push_back(vt + " %a" + std::to_string(idx++));
        }
    }

    std::string ret_sig = ret_var ? "void" : ret_t;
    std::string sig = "declare " + ret_sig + " @" +
                      IRWriter::quote(mangled_of(&f)) + "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) sig += ", ";
        sig += params[i];
    }
    sig += ")";
    ir_.emit_type(sig);
}

std::string Translator::eval_call(Expr* e, Expr::Call* c) {
    // funcptr 调用
    if (c->callee && c->callee->type && c->callee->type->is_func_ptr()) {
        auto* fp = c->callee->type->get_func_ptr();
        return call_func_ptr(e, c, fp);
    }
    // 无 INIT 的类零参默认构造：分配实例
    if (!c->resolved && c->callee && c->callee->sym &&
        c->callee->sym->kind == SymbolKind::Class) {
        Class* cls = as_class(e->type);
        if (cls) {
            auto it = classes_.find(cls);
            if (it != classes_.end()) {
                std::string slot = ir_.reg();
                emit_instr(slot + " = alloca " + it->second.llvm_name);
                return slot;
            }
        }
        return "0";
    }
    if (!c->resolved) return "0";

    // 类构造：C(...) — resolved 是 INIT 特殊方法
    if (c->resolved->kind == DeclKind::SpecialMethod) {
        auto* sm = static_cast<SpecialMethod*>(c->resolved);
        if (sm->kind == SpecialMethod::Kind::Init) {
            Class* cls = as_class(e->type);
            if (!cls) return "0";
            auto it = classes_.find(cls);
            if (it == classes_.end()) return "0";
            std::string slot = ir_.reg();
            emit_instr(slot + " = alloca " + it->second.llvm_name);
            std::vector<std::string> args;
            args.push_back("ptr " + slot);
            for (Expr* a : c->args) {
                std::string v = eval(a);
                std::string at = a && a->type ? map_type(a->type) : "i32";
                args.push_back(at + " " + v);
            }
            std::string sig = "call void @" +
                IRWriter::quote(mangled_of(sm)) + "(";
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) sig += ", ";
                sig += args[i];
            }
            sig += ")";
            emit_instr(sig);
            return slot;
        }
        return "0";
    }

    if (c->resolved->kind != DeclKind::Func &&
        c->resolved->kind != DeclKind::Operator) return "0";
    Func* fn = static_cast<Func*>(c->resolved);

    std::string fname = mangled_of(fn);
    std::string ret_t = "void";
    VarItem* ret_var = nullptr;
    if (!fn->returns.empty() && fn->returns.back()) {
        VarList* rl = fn->returns.back();
        if (rl->type) ret_t = map_type(rl->type);
        if (!rl->vars.empty()) ret_var = rl->vars.back();
    }

    std::vector<std::string> args;

    // 方法调用：callee 是 Member 且 target 是类实例
    bool has_self = false;
    std::string self_arg;
    if (Expr* callee = c->callee) {
        if (auto* m = callee->get_member()) {
            Expr* tgt = m->target;
            if (as_class(tgt ? tgt->type : nullptr) &&
                !fn->modfs.has(ModfKind::Static)) {
                self_arg = eval(tgt);
                has_self = true;
            }
        }
    }

    std::string sret_slot;
    if (ret_var) {
        sret_slot = ir_.reg();
        emit_instr(sret_slot + " = alloca " + ret_t);
        args.push_back("ptr sret(" + ret_t + ") " + sret_slot);
    }
    if (has_self) args.push_back("ptr " + self_arg);
    for (Expr* a : c->args) {
        std::string v = eval(a);
        std::string at = a && a->type ? map_type(a->type) : "i32";
        args.push_back(at + " " + v);
    }

    bool is_main = (std::string(fn->name) == "main" && ctx_.current_class == nullptr);
    bool plain_main = is_main && !ret_var && ret_t == "void";

    std::string call_ret = ret_var ? "void" : (plain_main ? "i32" : ret_t);
    std::string sig = call_ret + " @" + IRWriter::quote(fname) + "(";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) sig += ", ";
        sig += args[i];
    }
    sig += ")";

    if (ret_var) {
        emit_instr("call " + sig);
        std::string r = ir_.reg();
        emit_instr(r + " = load " + ret_t + ", ptr " + sret_slot);
        return r;
    }
    if (call_ret == "void") {
        emit_instr("call " + sig);
        return "0";
    }
    std::string r = ir_.reg();
    emit_instr(r + " = call " + sig);
    return r;
}

std::string Translator::emit_closure_fn(Expr::Closure* c) {
    std::string name = "__closure_" + std::to_string(closure_counter_++);

    ir_.capture_late();
    Ctx saved = ctx_;
    Ctx fresh;
    ctx_ = fresh;
    ir_.begin_func();

    std::string ret_t = "void";
    VarItem* ret_var = nullptr;
    if (!c->returns.empty() && c->returns.back()) {
        VarList* rl = c->returns.back();
        if (rl->type) ret_t = map_type(rl->type);
        if (!rl->vars.empty()) ret_var = rl->vars.back();
    }

    std::vector<std::string> params;
    std::vector<std::tuple<VarItem*, std::string, std::string>> arg_inits;
    if (ret_var) params.push_back("ptr sret(" + ret_t + ") %ret");
    int idx = 0;
    for (VarList* vl : c->params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            std::string vt = vi->type ? map_type(vi->type)
                                      : (vl->type ? map_type(vl->type) : "i32");
            std::string argn = "%a" + std::to_string(idx++);
            params.push_back(vt + " " + argn);
            arg_inits.emplace_back(vi, argn, vt);
        }
    }

    std::string sig = "define void @" + IRWriter::quote(name) + "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) sig += ", ";
        sig += params[i];
    }
    sig += ") {";
    ir_.emit(sig);
    open_block("entry");

    for (auto& [vi, argn, vt] : arg_inits) {
        std::string slot = ir_.reg();
        emit_instr(slot + " = alloca " + vt);
        emit_instr("store " + vt + " " + argn + ", ptr " + slot);
        ctx_.slots[vi] = slot;
    }
    if (ret_var) ctx_.slots[ret_var] = "%ret";

    if (c->body) emit_stmt(c->body);
    if (!ctx_.term) {
        emit_drops_to(0);
        emit_term("ret void");
    }
    ir_.emit("}");
    ir_.blank();
    ir_.release_late();

    ctx_ = saved;
    return name;
}

std::string Translator::eval_closure(Expr*, Expr::Closure* c) {
    return "@" + emit_closure_fn(c);
}

std::string Translator::call_func_ptr(Expr* e, Expr::Call* c, Expr::FuncPtr* fp) {
    std::string fpv = eval(c->callee);
    (void)e;
    std::string ret_t = fp->return_type ? map_type(fp->return_type) : "void";

    std::vector<std::string> args;
    std::string sret_slot;
    if (ret_t != "void") {
        sret_slot = ir_.reg();
        emit_instr(sret_slot + " = alloca " + ret_t);
        args.push_back("ptr sret(" + ret_t + ") " + sret_slot);
    }
    for (size_t i = 0; i < c->args.size(); ++i) {
        Expr* a = c->args[i];
        Expr* pt_expr = i < fp->params.size() ? fp->params[i] : nullptr;
        std::string pt = pt_expr ? map_type(pt_expr) : (a && a->type ? map_type(a->type) : "i32");
        std::string v = eval(a);
        args.push_back(pt + " " + v);
    }

    std::string sig = (ret_t == "void" ? "void " : "void ") + fpv + "(";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) sig += ", ";
        sig += args[i];
    }
    sig += ")";
    emit_instr("call " + sig);
    if (ret_t != "void") {
        std::string r = ir_.reg();
        emit_instr(r + " = load " + ret_t + ", ptr " + sret_slot);
        return r;
    }
    return "0";
}

std::string Translator::eval_ternary(Expr* e, Expr::Ternary* t) {
    std::string cond = eval(t->cond);
    std::string then_l = ir_.named_label("tern.then");
    std::string else_l = ir_.named_label("tern.else");
    std::string end_l  = ir_.named_label("tern.end");
    emit_term("br i1 " + cond + ", label %" + then_l + ", label %" + else_l);

    std::string ttype = e->type ? map_type(e->type) : "i32";

    open_block(then_l);
    std::string tv = eval(t->then_expr);
    std::string tv_slot = ir_.reg();
    emit_instr(tv_slot + " = alloca " + ttype);
    emit_instr("store " + ttype + " " + tv + ", ptr " + tv_slot);
    emit_term("br label %" + end_l);

    open_block(else_l);
    std::string ev = eval(t->else_expr);
    std::string ev_slot = ir_.reg();
    emit_instr(ev_slot + " = alloca " + ttype);
    emit_instr("store " + ttype + " " + ev + ", ptr " + ev_slot);
    emit_term("br label %" + end_l);

    open_block(end_l);
    std::string res_slot = ir_.reg();
    emit_instr(res_slot + " = phi ptr [ " + tv_slot + ", %" + then_l +
               " ], [ " + ev_slot + ", %" + else_l + " ]");
    std::string res = ir_.reg();
    emit_instr(res + " = load " + ttype + ", ptr " + res_slot);
    return res;
}

std::string Translator::eval_spawn(Expr*, Expr::Spawn* s) {
    Expr* call_expr = s->call;
    if (!call_expr) return "0";
    auto* c = call_expr->get_call();
    if (!c || !c->resolved) return "0";
    if (c->resolved->kind != DeclKind::Func &&
        c->resolved->kind != DeclKind::Operator) return "0";
    Func* fn = static_cast<Func*>(c->resolved);

    // 收集 self 与实参
    bool has_self = false;
    std::string self_val;
    if (c->callee) {
        if (auto* m = c->callee->get_member()) {
            Expr* tgt = m->target;
            if (as_class(tgt ? tgt->type : nullptr) &&
                !fn->modfs.has(ModfKind::Static)) {
                self_val = eval(tgt);
                has_self = true;
            }
        }
    }
    std::vector<std::string> field_types;
    std::vector<std::string> field_vals;
    if (has_self) {
        field_types.push_back("ptr");
        field_vals.push_back(self_val);
    }
    for (Expr* a : c->args) {
        std::string v = eval(a);
        std::string t = a && a->type ? map_type(a->type) : "i32";
        field_types.push_back(t);
        field_vals.push_back(v);
    }

    std::string struct_ty;
    if (field_types.empty()) struct_ty = "{}";
    else {
        struct_ty = "{ ";
        for (size_t i = 0; i < field_types.size(); ++i) {
            if (i) struct_ty += ", ";
            struct_ty += field_types[i];
        }
        struct_ty += " }";
    }

    // 参数打包到堆
    std::string pack = ir_.reg();
    size_t total = field_types.empty() ? 1 : field_types.size() * 8;
    emit_instr(pack + " = call ptr @malloc(i64 " + std::to_string(total) + ")");
    for (size_t i = 0; i < field_vals.size(); ++i) {
        std::string addr = ir_.reg();
        emit_instr(addr + " = getelementptr " + struct_ty + ", ptr " + pack +
                   ", i32 0, i32 " + std::to_string(i));
        emit_instr("store " + field_types[i] + " " + field_vals[i] + ", ptr " + addr);
    }

    // 生成 wrapper（到 late 缓冲）
    std::string wrapper = "__spawn_" + std::to_string(closure_counter_++);
    ir_.capture_late();
    ir_.begin_func();
    Ctx saved = ctx_;
    ctx_ = Ctx{};

    ir_.emit("define ptr @" + IRWriter::quote(wrapper) + "(ptr %raw) {");
    open_block("entry");

    std::vector<std::string> unpacked;
    for (size_t i = 0; i < field_types.size(); ++i) {
        std::string addr = ir_.reg();
        emit_instr(addr + " = getelementptr " + struct_ty + ", ptr %raw, i32 0, i32 " +
                   std::to_string(i));
        std::string v = ir_.reg();
        emit_instr(v + " = load " + field_types[i] + ", ptr " + addr);
        unpacked.push_back(v);
    }

    std::string ret_t = "void";
    VarItem* ret_var = nullptr;
    if (!fn->returns.empty() && fn->returns.back()) {
        VarList* rl = fn->returns.back();
        if (rl->type) ret_t = map_type(rl->type);
        if (!rl->vars.empty()) ret_var = rl->vars.back();
    }

    std::vector<std::string> call_args;
    std::string sret_slot;
    if (ret_var) {
        sret_slot = ir_.reg();
        emit_instr(sret_slot + " = alloca " + ret_t);
        call_args.push_back("ptr sret(" + ret_t + ") " + sret_slot);
    }

    size_t ui = 0;
    if (has_self) call_args.push_back("ptr " + unpacked[ui++]);
    for (size_t i = ui; i < unpacked.size(); ++i)
        call_args.push_back(field_types[i] + " " + unpacked[i]);

    std::string call_ret = ret_var ? "void" : ret_t;
    std::string sig = call_ret + " @" + IRWriter::quote(mangled_of(fn)) + "(";
    for (size_t i = 0; i < call_args.size(); ++i) {
        if (i) sig += ", ";
        sig += call_args[i];
    }
    sig += ")";
    emit_instr("call " + sig);
    emit_term("ret ptr null");
    ir_.emit("}");
    ir_.blank();
    ir_.release_late();
    ctx_ = saved;

    std::string tid = ir_.reg();
    emit_instr(tid + " = alloca i64");
    emit_instr("call i32 @pthread_create(ptr " + tid + ", ptr null, ptr @" +
               IRWriter::quote(wrapper) + ", ptr " + pack + ")");
    return "0";
}

int Translator::array_length_of(Expr* arrtype) {
    if (!arrtype || !arrtype->is_array_type()) return 0;
    Expr* len = arrtype->get_array_type()->length;
    if (!len || !len->get_literal()) return 0;
    try { return std::stoi(std::string(len->get_literal()->value)); }
    catch (...) { return 0; }
}

} // namespace cx
