#include "../include/translator.h"
#include "../include/ir_writer.h"
#include "../include/semantic.h"
#include "../include/translator_mangle.h"

#include <type_traits>
#include <unordered_set>

namespace cx {

namespace {
bool ir_is_int_type(const std::string& t) {
    return t.size() >= 2 && t[0] == 'i' && t != "i1";
}
bool ir_is_fp_type(const std::string& t) {
    return t == "float" || t == "double" || t == "half";
}
int ir_int_bits(const std::string& t) {
    return std::stoi(t.substr(1));
}
std::string ir_common_type(const std::string& a, const std::string& b) {
    if (a == b) return a;
    if (ir_is_fp_type(a) && ir_is_fp_type(b)) {
        auto bits = [](const std::string& t) {
            return t == "half" ? 16 : (t == "float" ? 32 : 64);
        };
        return bits(a) >= bits(b) ? a : b;
    }
    if (ir_is_fp_type(a)) return a;
    if (ir_is_fp_type(b)) return b;
    if (ir_is_int_type(a) && ir_is_int_type(b))
        return ir_int_bits(a) >= ir_int_bits(b) ? a : b;
    return a;
}

Expr* strip_modifiers(Expr* t) {
    return t;
}

bool expr_is_unsigned(Expr* t);

// 比较/算术操作数提升后的无符号性：位宽不同取宽者，相同则任一为无符号即无符号
bool common_unsigned(const std::string& lt, const std::string& rt,
                     Expr* l, Expr* r) {
    if (ir_is_fp_type(lt) || ir_is_fp_type(rt)) return false;
    int lb = ir_is_int_type(lt) ? ir_int_bits(lt) : 0;
    int rb = ir_is_int_type(rt) ? ir_int_bits(rt) : 0;
    bool lu = expr_is_unsigned(l), ru = expr_is_unsigned(r);
    if (lb != rb) return lb > rb ? lu : ru;
    return lu || ru;
}

// 表达式类型是否为无符号整数（不含 bool）
bool expr_is_unsigned(Expr* t) {
    t = strip_modifiers(t);
    if (!t) return false;
    auto* p = t->get_primitive();
    if (!p) return false;
    switch (p->kind) {
        case PrimitiveTypeKind::UInt8:
        case PrimitiveTypeKind::UShort:
        case PrimitiveTypeKind::UInt:
        case PrimitiveTypeKind::ULong:
        case PrimitiveTypeKind::UInt128:
        case PrimitiveTypeKind::UChar:
            return true;
        default:
            return false;
    }
}
} // namespace


// 收集

void Translator::run() {
    mangler_.collect(cu_);
    collect_classes();
    for (Decl* d : cu_.top_decls) emit_decl(d);
    if (uses_alwaysinline_)
        ir_.emit_module_tail("attributes #0 = { alwaysinline }");
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

void Translator::collect_classes() {
    for (Decl* d : cu_.top_decls) collect_classes_decl(d);
}

void Translator::collect_classes_decl(Decl* d) {
    if (!d) return;
    switch (d->kind) {
        case DeclKind::Class: {
            Class* c = static_cast<Class*>(d);
            ClassInfo info;
            info.llvm_name = "%" + mangler_.of(c);
            int idx = 0;
            for (Decl* m : c->members) {
                if (!m || m->kind != DeclKind::VarList) continue;
                VarList* vl = static_cast<VarList*>(m);
                if (vl->specs.has(SpecKind::Static)) continue;
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
    if (t->get_array_type()) return "{ ptr, i32 }";
    if (t->get_string_literal_type()) return "ptr";
    if (t->get_func_ptr()) return "ptr";
    if (t->get_func_ref()) return "ptr";
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
    emit_instr("call void @" + IRWriter::quote(mangler_.of(sm)) +
               "(ptr " + val + ")");
    emit_term("br label %" + cont_l);
    open_block(cont_l);
}

void Translator::emit_bind_ref(Class* cls, const std::string& val,
                               SpecialMethod::Kind k) {
    if (!cls) return;
    emit_guard_call(find_special(cls, k), val);
}

void Translator::register_deref(const std::string& slot, Class* cls, int depth) {
    if (!cls) return;
    if (!find_special(cls, SpecialMethod::Kind::Deref)) return;
    CleanupEntry e;
    e.kind = CleanupEntry::Deref;
    e.slot = slot;
    e.cls = cls;
    e.scope_depth = depth >= 0 ? depth : ctx_.scope_depth;
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

void Translator::emit_scope_cleanup(int depth) {
    // 离开作用域时遗忘引用绑定（引用变量本身不承载资源，无需释放）
    for (auto it = ctx_.ref_slots.begin(); it != ctx_.ref_slots.end(); ) {
        if (it->second > depth) it = ctx_.ref_slots.erase(it);
        else ++it;
    }
}

void Translator::emit_drops_to(int depth) {
    emit_scope_cleanup(depth);
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
    bool is_static = f.specs.has(SpecKind::Static);
    bool has_self = cls != nullptr && !is_static;

    std::string ret_t = "void";
    VarItem* ret_var = nullptr;
    if (!f.returns.empty() && f.returns.back()) {
        VarList* rl = f.returns.back();
        if (!rl->vars.empty()) ret_var = rl->vars.back();
        Expr* rt_ = (ret_var && ret_var->type) ? ret_var->type : rl->type;
        if (rt_) ret_t = map_type(rt_);
    }
    // main 有返回变量：不用 sret，改为局部变量 + 统一 exit 块
    bool main_with_ret = is_main && ret_var;
    bool plain_main = is_main && !ret_var && ret_t == "void";

    std::vector<std::string> params;
    std::vector<std::tuple<VarItem*, std::string, std::string>> arg_inits;

    if (ret_var && !main_with_ret && !f.ret_via_reg)
        params.push_back("ptr sret(" + ret_t + ") %ret");
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

    std::string ret_sig = (plain_main || main_with_ret) ? "i32"
                         : (f.ret_via_reg && ret_var ? ret_t : "void");
    ctx_.ret_type = ret_t;
    if (main_with_ret) ctx_.exit_label = "main.exit";

    std::string sig = "define " + ret_sig + " @" +
                      IRWriter::quote(mangler_.of(&f)) + "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) sig += ", ";
        sig += params[i];
    }
    sig += ")";
    if (f.specs.has(SpecKind::Inline)) {
        sig += " #0";
        uses_alwaysinline_ = true;
    }
    sig += " {";
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
                register_deref(slot, pc, 1);
            }
        }
    }
    if (main_with_ret) {
        std::string slot = ir_.reg();
        emit_instr(slot + " = alloca i32");
        ctx_.slots[ret_var] = slot;
    } else if (ret_var && f.ret_via_reg) {
        std::string slot = ir_.reg();
        emit_instr(slot + " = alloca " + ret_t);
        ctx_.slots[ret_var] = slot;
    } else if (ret_var) {
        ctx_.slots[ret_var] = "%ret";
    }

    // 返回变量按声明顺序处理：最后一个为真正的返回值，
    // 其余按普通局部变量分配槽位并执行初始化
    std::vector<VarItem*> ret_vars;
    for (VarList* rl : f.returns) {
        if (!rl) continue;
        for (VarItem* vi : rl->vars) if (vi) ret_vars.push_back(vi);
    }
    for (VarItem* rv : ret_vars) {
        if (rv == ret_var) continue;
        std::string vt = rv->type ? map_type(rv->type) : "i32";
        std::string slot = ir_.reg();
        emit_instr(slot + " = alloca " + vt);
        ctx_.slots[rv] = slot;
    }
    for (VarItem* rv : ret_vars) {
        if (!rv->init) continue;
        auto sit = ctx_.slots.find(rv);
        if (sit == ctx_.slots.end()) continue;
        std::string vt = (rv == ret_var)
            ? ret_t : (rv->type ? map_type(rv->type) : "i32");
        if (vt == "void") continue;
        bool converted = needs_convert(rv->init);
        std::string v = converted ? eval_converted(rv->init) : eval(rv->init);
        if (!converted) {
            std::string src_t = rv->init->type ? map_type(rv->init->type) : vt;
            if (src_t != vt) v = cast_value(v, src_t, vt,
                !expr_is_unsigned(rv->init->type), !expr_is_unsigned(rv->type));
        }
        emit_instr("store " + vt + " " + v + ", ptr " + sit->second);
    }

    if (cls && f.kind == DeclKind::Operator &&
        static_cast<Operator*>(&f)->synthesized)
        emit_synth_operator(f, cls, ret_var);
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
        if (f.ret_via_reg && ret_var) {
            std::string rs = ctx_.slots.count(ret_var) ? ctx_.slots[ret_var] : "null";
            std::string r = ir_.reg();
            emit_instr(r + " = load " + ret_t + ", ptr " + rs);
            emit_term("ret " + ret_t + " " + r);
        } else if (plain_main) emit_term("ret i32 0");
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
                      IRWriter::quote(mangler_.of(&sm)) + "(";
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
                register_deref(slot, pc, 1);
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
    if (info.llvm_name.empty()) info.llvm_name = "%" + mangler_.of(c);

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
            std::string name = mangler_.of(m);
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

void Translator::emit_synth_operator(Func& f, Class* cls, VarItem* ret_var) {
    auto* op = static_cast<Operator*>(&f);
    std::string base_ir = base_ir_of(cls->base_source);
    auto it = classes_.find(cls);
    std::string cls_ir = (it != classes_.end()) ? it->second.llvm_name
                                                 : "%" + mangler_.of(cls);
    int idx = 0;
    if (it != classes_.end()) {
        auto fi = it->second.field_index.find("__value");
        if (fi != it->second.field_index.end()) idx = fi->second;
    }
    std::string self = ctx_.self_ptr.empty() ? "%self" : ctx_.self_ptr;
    std::string addr = ir_.reg();
    emit_instr(addr + " = getelementptr " + cls_ir + ", ptr " + self +
               ", i32 0, i32 " + std::to_string(idx));
    std::string lv = ir_.reg();
    emit_instr(lv + " = load " + base_ir + ", ptr " + addr);

    OpKind k = op->op;
    bool is_cmp = (k == OpKind::Less || k == OpKind::Greater ||
                   k == OpKind::LessEqual || k == OpKind::GreaterEqual ||
                   k == OpKind::Equal || k == OpKind::NotEqual);
    bool is_unary = op->params.empty();

    bool is_compound_assign = (k == OpKind::PlusAssign || k == OpKind::MinusAssign ||
                               k == OpKind::MulAssign || k == OpKind::DivAssign ||
                               k == OpKind::ModAssign || k == OpKind::AndAssign ||
                               k == OpKind::OrAssign || k == OpKind::XorAssign ||
                               k == OpKind::ShlAssign || k == OpKind::ShrAssign);
    if (is_compound_assign) {
        VarItem* avi = nullptr;
        for (auto* vl : op->params) {
            for (auto* vi : vl->vars) { avi = vi; break; }
            if (avi) break;
        }
        if (!avi) return;
        auto sit = ctx_.slots.find(avi);
        if (sit == ctx_.slots.end()) return;
        std::string rv = ir_.reg();
        emit_instr(rv + " = load " + base_ir + ", ptr " + sit->second);
        std::string res = ir_.reg();
        std::string opc = "add";
        switch (k) {
            case OpKind::PlusAssign:  opc = "add";  break;
            case OpKind::MinusAssign: opc = "sub";  break;
            case OpKind::MulAssign:   opc = "mul";  break;
            case OpKind::DivAssign:   opc = "sdiv"; break;
            case OpKind::ModAssign:   opc = "srem"; break;
            case OpKind::AndAssign:   opc = "and";  break;
            case OpKind::OrAssign:    opc = "or";   break;
            case OpKind::XorAssign:   opc = "xor";  break;
            case OpKind::ShlAssign:   opc = "shl";  break;
            case OpKind::ShrAssign:   opc = "ashr"; break;
            default: break;
        }
        emit_instr(res + " = " + opc + " " + base_ir + " " + lv + ", " + rv);
        emit_instr("store " + base_ir + " " + res + ", ptr " + addr);
        return;
    }

    std::string result;
    if (is_unary) {
        std::string r = ir_.reg();
        if (k == OpKind::Minus)
            emit_instr(r + " = sub " + base_ir + " 0, " + lv);
        else if (k == OpKind::Not)
            emit_instr(r + " = icmp eq " + base_ir + " " + lv + ", 0");
        else if (k == OpKind::BitNot)
            emit_instr(r + " = xor " + base_ir + " " + lv + ", -1");
        result = r;
    } else {
        VarItem* avi = nullptr;
        for (auto* vl : op->params) {
            for (auto* vi : vl->vars) { avi = vi; break; }
            if (avi) break;
        }
        if (!avi) return;
        auto sit = ctx_.slots.find(avi);
        if (sit == ctx_.slots.end()) return;
        std::string rv = ir_.reg();
        emit_instr(rv + " = load " + base_ir + ", ptr " + sit->second);
        std::string res = ir_.reg();
        if (is_cmp) {
            std::string pred = "eq";
            switch (k) {
                case OpKind::Less:         pred = "slt"; break;
                case OpKind::Greater:      pred = "sgt"; break;
                case OpKind::LessEqual:    pred = "sle"; break;
                case OpKind::GreaterEqual: pred = "sge"; break;
                case OpKind::Equal:        pred = "eq";  break;
                default:                   pred = "ne";  break;
            }
            emit_instr(res + " = icmp " + pred + " " + base_ir + " " + lv + ", " + rv);
        } else {
            std::string opc = "add";
            switch (k) {
                case OpKind::Plus:   opc = "add";  break;
                case OpKind::Minus:  opc = "sub";  break;
                case OpKind::Mul:    opc = "mul";  break;
                case OpKind::Div:    opc = "sdiv"; break;
                case OpKind::Mod:    opc = "srem"; break;
                    case OpKind::And:    opc = "and";  break;
                case OpKind::Or:     opc = "or";   break;
                case OpKind::BitAnd: opc = "and";  break;
                case OpKind::BitOr:  opc = "or";   break;
                case OpKind::BitXor: opc = "xor";  break;
                case OpKind::Shl:    opc = "shl";  break;
                case OpKind::Shr:    opc = "ashr"; break;
                default: break;
            }
            emit_instr(res + " = " + opc + " " + base_ir + " " + lv + ", " + rv);
        }
        result = res;
    }

    if (!result.empty() && ret_var) {
        auto sit = ctx_.slots.find(ret_var);
        if (sit != ctx_.slots.end()) {
            std::string ret_ir = ret_var->type ? map_type(ret_var->type) : base_ir;
            emit_instr("store " + ret_ir + " " + result + ", ptr " + sit->second);
        }
    }
}

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

// 类实例声明：分配实例槽（引用/拷贝构造/malloc），登记析构清理
bool Translator::emit_class_var(VarList* vl, VarItem* vi, Class* cls) {
    auto it = classes_.find(cls);
    if (it == classes_.end()) { ctx_.slots[vi] = "null"; return true; }
    std::string inst;
    bool is_malloc_cls = cls->specs.has(SpecKind::Malloc);
    bool created = false;
    if (vi->init) {
        Class* src_cls = as_class(vi->init->type);
        if (src_cls == cls && !needs_convert(vi->init)) {
            // 同类初始化：拷贝构造，新建实例并逐字段复制
            std::string src = eval(vi->init);
            inst = emit_class_copy(cls, src);
            created = true;
        } else {
            if (vi->init->implicit_to && as_wrap_class(vi->init->implicit_to))
                created = true;
            else if (auto* call = vi->init->get_call()) {
                if (call->resolved &&
                    call->resolved->kind == DeclKind::SpecialMethod)
                    created = true;
            }
            inst = eval_converted(vi->init);
        }
    } else if (is_malloc_cls) {
        ir_.need_malloc();
        inst = ir_.reg();
        emit_instr(inst + " = call ptr @malloc(i64 " +
                   std::to_string(type_size(it->second.llvm_name)) + ")");
        created = true;
    } else {
        inst = ir_.reg();
        emit_instr(inst + " = alloca " + it->second.llvm_name);
    }
    std::string slot = ir_.reg();
    emit_instr(slot + " = alloca ptr");
    emit_instr("store ptr " + inst + ", ptr " + slot);
    ctx_.slots[vi] = slot;
    if (is_malloc_cls) {
        if (!created) emit_bind_ref(cls, inst, SpecialMethod::Kind::Ref);
        register_deref(slot, cls);
    } else if (!vl->specs.has(SpecKind::Malloc)) {
        for (Decl* m : cls->members) {
            if (m && m->kind == DeclKind::SpecialMethod) {
                auto* sm = static_cast<SpecialMethod*>(m);
                if (sm->kind == SpecialMethod::Kind::Drop) {
                    CleanupEntry e;
                    e.kind = CleanupEntry::Drop;
                    e.slot = inst;
                    e.drop_fn = mangler_.of(sm);
                    e.scope_depth = ctx_.scope_depth;
                    ctx_.drops.push_back(e);
                    break;
                }
            }
        }
    }
    return true;
}


// 定长数组无初值：分配 backing 并以 {ptr,i32} 切片形式存入槽
bool Translator::emit_array_noinit(VarItem* vi, Expr* vtype, const std::string& slot) {
    int len = array_length_of(vtype);
    if (len <= 0) return false;
    Expr* el = vtype->get_array_type()->element;
    std::string et = el ? map_type(el) : "i32";
    std::string backing = ir_.reg();
    emit_instr(backing + " = alloca " + et + ", i64 " +
               std::to_string((long long)len));
    std::string s1 = ir_.reg();
    emit_instr(s1 + " = insertvalue { ptr, i32 } undef, ptr " + backing + ", 0");
    std::string s2 = ir_.reg();
    emit_instr(s2 + " = insertvalue { ptr, i32 } " + s1 + ", i32 " +
               std::to_string((long long)len) + ", 1");
    emit_instr("store { ptr, i32 } " + s2 + ", ptr " + slot);
    (void)vi;
    return true;
}

// 字符数组以字符串字面量初始化：分配 backing 并逐字节复制（含 NUL）
bool Translator::emit_array_string_init(VarItem* vi, const std::string& slot) {
    auto* lit = vi->init->get_literal();
    std::string content = lit ? std::string(lit->value) : std::string();
    size_t n = content.size();
    std::string sym = ir_.string_const(content);
    std::string backing = ir_.reg();
    // 多分配 1 字节存放 NUL 终止符，使 .ptr / 切片转指针可作 C 字符串
    emit_instr(backing + " = alloca i8, i64 " + std::to_string(n + 1));
    for (size_t k = 0; k <= n; ++k) {
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
    emit_instr(s1 + " = insertvalue { ptr, i32 } undef, ptr " + backing + ", 0");
    std::string s2 = ir_.reg();
    emit_instr(s2 + " = insertvalue { ptr, i32 } " + s1 + ", i32 " +
               std::to_string(static_cast<long long>(n)) + ", 1");
    emit_instr("store { ptr, i32 } " + s2 + ", ptr " + slot);
    return true;
}


void Translator::emit_var_decl(VarList* vl) {
    if (!vl) return;
    std::string t = map_type(vl->type);
    for (VarItem* vi : vl->vars) {
        if (!vi) continue;
        Expr* vtype = vi->type ? vi->type : vl->type;
        // 引用声明：分配槽存被引用对象地址；有初值则立即绑定
        if (vtype && vtype->is_reference()) {
            std::string slot = ir_.reg();
            emit_instr(slot + " = alloca ptr");
            ctx_.slots[vi] = slot;
            ctx_.ref_slots[vi] = ctx_.scope_depth;
            if (vi->init) {
                std::string addr = eval_lvalue(vi->init);
                if (addr.empty()) addr = eval(vi->init);
                emit_instr("store ptr " + addr + ", ptr " + slot);
                ctx_.ref_bound.insert(vi);
            }
            continue;
        }
        Class* cls = as_class(vtype);
        if (cls && emit_class_var(vl, vi, cls)) continue;
        std::string vt = vtype ? map_type(vtype) : t;
        std::string slot = ir_.reg();
        emit_instr(slot + " = alloca " + vt);
        ctx_.slots[vi] = slot;
        if (vtype && vtype->is_array_type() && !vi->init) {
            if (emit_array_noinit(vi, vtype, slot)) continue;
        }
        if (vtype && vtype->is_array_type() && vi->init &&
            vi->init->type && vi->init->type->is_string_literal_type()) {
            if (emit_array_string_init(vi, slot)) continue;
        }
        if (vi->init) {
            bool converted = needs_convert(vi->init);
            std::string v = converted ? eval_converted(vi->init)
                                      : eval(vi->init);
            if (!converted) {
                std::string src_t = vi->init->type ? map_type(vi->init->type) : vt;
                if (src_t != vt) v = cast_value(v, src_t, vt,
                    !expr_is_unsigned(vi->init->type), !expr_is_unsigned(vtype));
            }
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
    // 寄存器返回：return 前加载返回变量
    if (ctx_.fn && ctx_.fn->ret_via_reg && !ctx_.fn->returns.empty() &&
        ctx_.fn->returns.back() && !ctx_.fn->returns.back()->vars.empty()) {
        VarItem* rv = ctx_.fn->returns.back()->vars.back();
        auto it = ctx_.slots.find(rv);
        if (it != ctx_.slots.end()) {
            std::string r = ir_.reg();
            emit_instr(r + " = load " + ctx_.ret_type + ", ptr " + it->second);
            emit_term("ret " + ctx_.ret_type + " " + r);
            return;
        }
    }
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
    if (e->implicit_to) {
        std::string b = emit_wrap_convert(e);
        if (map_type(e->implicit_to) == "i1") return b;
        std::string r = ir_.reg();
        emit_instr(r + " = icmp ne i1 " + b + ", false");
        return r;
    }
    if (e->type->is_pointer()) {
        std::string r = ir_.reg();
        emit_instr(r + " = icmp ne ptr " + v + ", null");
        return r;
    }
    if (e->type->is_array_type()) {
        std::string n = ir_.reg();
        emit_instr(n + " = extractvalue { ptr, i32 } " + v + ", 1");
        std::string r = ir_.reg();
        emit_instr(r + " = icmp ne i32 " + n + ", 0");
        return r;
    }
    return v;
}

std::string Translator::eval_cond(Expr* e) {
    if (needs_convert(e)) return to_bool(e, eval_converted(e));
    return to_bool(e, eval(e));
}

std::string Translator::eval(Expr* e) {
    if (!e) return "0";
    if (e->is_null())                     return "null";
    if (auto* lit = e->get_literal())     return eval_literal(lit);
    if (auto* id  = e->get_identifier())  return eval_identifier(e, id);
    if (auto* b   = e->get_binary())      return eval_binary(e, b);
    if (auto* u   = e->get_unary())       return eval_unary(e, u);
    if (auto* a   = e->get_assign())      return eval_assign(e, a);
    if (auto* c   = e->get_call())        return eval_call(e, c);
    if (auto* ix  = e->get_index())       return eval_index(e, ix);
    if (auto* m   = e->get_member())      return eval_member(e, m);
    if (auto* ar  = e->get_array())       return eval_array(e, ar);
    if (auto* ca  = e->get_cast())        return eval_cast(e, ca);
    if (auto* tr  = e->get_ternary())     return eval_ternary(e, tr);
    if (auto* cl  = e->get_closure())     return eval_closure(e, cl);
    if (auto* sp  = e->get_spawn())       return eval_spawn(e, sp);
    if (auto* fr  = e->get_func_ref())
        return "@" + IRWriter::quote(mangler_.of(fr->func));
    return "0";
}

// 值位置求值：需要隐式 conversion 时派发到转换函数，否则常规求值
std::string Translator::base_ir_of(PrimitiveTypeKind k) {
    switch (k) {
        case PrimitiveTypeKind::Int8:
        case PrimitiveTypeKind::UInt8:
        case PrimitiveTypeKind::Char:
        case PrimitiveTypeKind::UChar:    return "i8";
        case PrimitiveTypeKind::Short:
        case PrimitiveTypeKind::UShort:   return "i16";
        case PrimitiveTypeKind::Int:
        case PrimitiveTypeKind::UInt:     return "i32";
        case PrimitiveTypeKind::Long:
        case PrimitiveTypeKind::ULong:    return "i64";
        case PrimitiveTypeKind::Int128:
        case PrimitiveTypeKind::UInt128:  return "i128";
        case PrimitiveTypeKind::Float16:  return "half";
        case PrimitiveTypeKind::Float:    return "float";
        case PrimitiveTypeKind::Double:   return "double";
        case PrimitiveTypeKind::Bool:     return "i1";
        default:                          return "i32";
    }
}

Class* Translator::as_wrap_class(Expr* t) {
    Class* c = as_class(t);
    if (!c) return nullptr;
    if (c->base_source == PrimitiveTypeKind::Void) return nullptr;
    return c;
}

// 隐式包装/解包：源或目标为包装类，先解包到承载类型，数值 cast，再打包
std::string Translator::emit_class_copy(Class* cls, const std::string& src) {
    auto it = classes_.find(cls);
    std::string cls_ir = (it != classes_.end()) ? it->second.llvm_name
                                                 : "%" + mangler_.of(cls);
    std::string dst;
    if (cls->specs.has(SpecKind::Malloc)) {
        ir_.need_malloc();
        dst = ir_.reg();
        emit_instr(dst + " = call ptr @malloc(i64 " +
                   std::to_string(type_size(cls_ir)) + ")");
        emit_bind_ref(cls, dst, SpecialMethod::Kind::Ref);
    } else {
        dst = ir_.reg();
        emit_instr(dst + " = alloca " + cls_ir);
    }
    if (it != classes_.end()) {
        for (auto& f : it->second.fields) {
            std::string sa = ir_.reg();
            emit_instr(sa + " = getelementptr " + cls_ir + ", ptr " + src +
                       ", i32 0, i32 " + std::to_string(f.index));
            std::string sv = ir_.reg();
            emit_instr(sv + " = load " + f.ir_type + ", ptr " + sa);
            std::string da = ir_.reg();
            emit_instr(da + " = getelementptr " + cls_ir + ", ptr " + dst +
                       ", i32 0, i32 " + std::to_string(f.index));
            emit_instr("store " + f.ir_type + " " + sv + ", ptr " + da);
        }
    }
    return dst;
}

std::string Translator::emit_wrap_convert(Expr* e) {
    Expr* src_t = e->type;
    Expr* dst_t = e->implicit_to;
    Class* sw = as_wrap_class(src_t);
    Class* dw = as_wrap_class(dst_t);

    std::string v;
    std::string src_ir;
    if (sw) {
        std::string inst = eval(e);
        std::string base_ir = base_ir_of(sw->base_source);
        auto it = classes_.find(sw);
        std::string cls_ir = (it != classes_.end()) ? it->second.llvm_name
                                                     : "%" + mangler_.of(sw);
        int idx = 0;
        if (it != classes_.end()) {
            auto fi = it->second.field_index.find("__value");
            if (fi != it->second.field_index.end()) idx = fi->second;
        }
        std::string addr = ir_.reg();
        emit_instr(addr + " = getelementptr " + cls_ir + ", ptr " + inst +
                   ", i32 0, i32 " + std::to_string(idx));
        v = ir_.reg();
        emit_instr(v + " = load " + base_ir + ", ptr " + addr);
        src_ir = base_ir;
    } else {
        v = eval(e);
        src_ir = src_t ? map_type(src_t) : "i32";
    }

    Expr* target_t = dw ? nullptr : dst_t;
    std::string dst_ir = dw ? base_ir_of(dw->base_source)
                            : (target_t ? map_type(target_t) : src_ir);

    if (src_ir != dst_ir) {
        bool su = sw ? false : !expr_is_unsigned(src_t);
        bool du = dw ? false : !expr_is_unsigned(dst_t);
        v = cast_value(v, src_ir, dst_ir, su, du);
    }

    if (dw) {
        auto it = classes_.find(dw);
        std::string cls_ir = (it != classes_.end()) ? it->second.llvm_name
                                                     : "%" + mangler_.of(dw);
        int idx = 0;
        if (it != classes_.end()) {
            auto fi = it->second.field_index.find("__value");
            if (fi != it->second.field_index.end()) idx = fi->second;
        }
        std::string slot;
        if (dw->specs.has(SpecKind::Malloc)) {
            ir_.need_malloc();
            slot = ir_.reg();
            emit_instr(slot + " = call ptr @malloc(i64 " +
                       std::to_string(type_size(cls_ir)) + ")");
            emit_bind_ref(dw, slot, SpecialMethod::Kind::Ref);
        } else {
            slot = ir_.reg();
            emit_instr(slot + " = alloca " + cls_ir);
        }
        std::string addr = ir_.reg();
        emit_instr(addr + " = getelementptr " + cls_ir + ", ptr " + slot +
                   ", i32 0, i32 " + std::to_string(idx));
        emit_instr("store " + dst_ir + " " + v + ", ptr " + addr);
        return slot;
    }
    return v;
}

size_t Translator::type_size(const std::string& llvm_name) const {
    for (auto& [cls, info] : classes_) {
        if (info.llvm_name != llvm_name) continue;
        size_t total = 0;
        for (auto& f : info.fields) {
            const std::string& t = f.ir_type;
            if (t == "i1" || t == "i8") total += 1;
            else if (t == "i16" || t == "half") total += 2;
            else if (t == "i32" || t == "float") total += 4;
            else if (t == "i64" || t == "double" || t == "ptr") total += 8;
            else if (t == "i128") total += 16;
            else total += 8;
        }
        return total == 0 ? 1 : total;
    }
    return 1;
}

bool Translator::needs_convert(Expr* e) {
    return e && ((e->conv && e->conv->kind == DeclKind::Conversion) ||
                 e->implicit_to);
}

std::string Translator::eval_converted(Expr* e) {
    if (e && e->conv && e->conv->kind == DeclKind::Conversion)
        return emit_conversion_call(e, static_cast<Conversion*>(e->conv));
    if (e && e->implicit_to)
        return emit_wrap_convert(e);
    return eval(e);
}

std::string Translator::eval_lvalue(Expr* e) {
    if (!e) return "";
    if (auto* id = e->get_identifier()) {
        if (std::string(id->name) == "self" && !ctx_.self_ptr.empty())
            return ctx_.self_ptr;
        if (e->sym && e->sym->kind == SymbolKind::Variable) {
            if (auto* vi = dynamic_cast<VarItem*>(e->sym->node)) {
                auto it = ctx_.slots.find(vi);
                if (it != ctx_.slots.end()) {
                    // 引用变量的 lvalue 是被引用对象：槽存其地址
                    if (ctx_.ref_bound.count(vi)) {
                        std::string addr = ir_.reg();
                        emit_instr(addr + " = load ptr, ptr " + it->second);
                        return addr;
                    }
                    return it->second;
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
        emit_instr(r + " = extractvalue { ptr, i32 } " + s + ", 0");
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

bool Translator::is_ref_item(const VarItem* vi, Expr** target_out) const {
    if (!vi || !vi->type) return false;
    if (auto* r = vi->type->get_reference()) {
        if (target_out) *target_out = r->target;
        return true;
    }
    return false;
}

bool Translator::is_ref_expr(Expr* e, Expr** target_out) const {
    if (!e) return false;
    if (e->type && e->type->is_reference()) {
        if (target_out) *target_out = e->type->get_reference()->target;
        return true;
    }
    return false;
}

std::string Translator::eval_identifier(Expr* e, Expr::Identifier* id) {
    if (std::string(id->name) == "self" && !ctx_.self_ptr.empty())
        return ctx_.self_ptr;
    // 函数名作为值：decay 为函数指针，取其地址
    if (e->type && e->type->is_func_ref())
        return "@" + IRWriter::quote(mangler_.of(e->type->get_func_ref()->func));
    if (e->sym && e->sym->kind == SymbolKind::Variable) {
        if (auto* vi = dynamic_cast<VarItem*>(e->sym->node)) {
            auto it = ctx_.slots.find(vi);
            if (it != ctx_.slots.end()) {
                // 引用变量：槽存被引用对象地址，需二次加载
                if (ctx_.ref_bound.count(vi)) {
                    std::string addr = ir_.reg();
                    emit_instr(addr + " = load ptr, ptr " + it->second);
                    std::string t = e->type ? map_type(e->type) : "i32";
                    std::string r = ir_.reg();
                    emit_instr(r + " = load " + t + ", ptr " + addr);
                    return r;
                }
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
    auto operand_type = [this](Expr* x) -> std::string {
        if (!x) return "i32";
        if (x->implicit_to) return map_type(x->implicit_to);
        return x->type ? map_type(x->type) : "i32";
    };
    auto eval_operand = [this](Expr* x) -> std::string {
        if (!x) return "0";
        return needs_convert(x) ? eval_converted(x) : eval(x);
    };
    std::string ltt = operand_type(b->left);
    std::string rtt = operand_type(b->right);

    // 逻辑与/或：两侧转 bool 后按 i1 运算
    if (b->op == OpKind::And || b->op == OpKind::Or) {
        std::string lv = eval_operand(b->left);
        std::string rv = eval_operand(b->right);
        std::string l = to_bool(b->left, lv);
        std::string rr = to_bool(b->right, rv);
        std::string res = ir_.reg();
        emit_instr(res + " = " + (b->op == OpKind::And ? "and i1 " : "or i1 ")
                   + l + ", " + rr);
        return res;
    }

    // 比较运算：结果 i1，操作数提升到共同类型后比较
    if (b->op >= OpKind::Less && b->op <= OpKind::NotEqual) {
        std::string op_t = ir_common_type(ltt, rtt);
        bool fp = ir_is_fp_type(op_t);
        bool uns = !fp && common_unsigned(ltt, rtt, b->left->type, b->right->type);
        std::string lv = eval_operand(b->left);
        std::string rv = eval_operand(b->right);
        if (ltt != op_t) lv = cast_value(lv, ltt, op_t,
            !expr_is_unsigned(b->left->type), !uns);
        if (rtt != op_t) rv = cast_value(rv, rtt, op_t,
            !expr_is_unsigned(b->right->type), !uns);
        std::string res = ir_.reg();
        std::string pred;
        switch (b->op) {
            case OpKind::Less:         pred = fp ? "fcmp olt " : (uns ? "icmp ult " : "icmp slt "); break;
            case OpKind::Greater:      pred = fp ? "fcmp ogt " : (uns ? "icmp ugt " : "icmp sgt "); break;
            case OpKind::LessEqual:    pred = fp ? "fcmp ole " : (uns ? "icmp ule " : "icmp sle "); break;
            case OpKind::GreaterEqual: pred = fp ? "fcmp oge " : (uns ? "icmp uge " : "icmp sge "); break;
            case OpKind::Equal:        pred = fp ? "fcmp oeq " : "icmp eq ";  break;
            default:                   pred = fp ? "fcmp one " : "icmp ne ";  break;
        }
        emit_instr(res + " = " + pred + op_t + " " + lv + ", " + rv);
        return res;
    }

    // 算术/位运算：运算类型取语义结果类型（已做提升），操作数转换到该类型
    std::string op_t = e->type ? map_type(e->type) : ir_common_type(ltt, rtt);
    if (op_t == "i1" || op_t.empty()) op_t = ir_common_type(ltt, rtt);
    bool is_fp = ir_is_fp_type(op_t);
    bool uns = !is_fp && common_unsigned(ltt, rtt, b->left->type, b->right->type);
    std::string lv = eval_operand(b->left);
    std::string rv = eval_operand(b->right);
    if (ltt != op_t) lv = cast_value(lv, ltt, op_t,
        !expr_is_unsigned(b->left->type), !uns);
    if (rtt != op_t) rv = cast_value(rv, rtt, op_t,
        !expr_is_unsigned(b->right->type), !uns);
    std::string res = ir_.reg();
    switch (b->op) {
        case OpKind::Plus:
            emit_instr(res + " = " + (is_fp ? "fadd " : "add ") + op_t + " " + lv + ", " + rv);
            break;
        case OpKind::Minus:
            emit_instr(res + " = " + (is_fp ? "fsub " : "sub ") + op_t + " " + lv + ", " + rv);
            break;
        case OpKind::Mul:
            emit_instr(res + " = " + (is_fp ? "fmul " : "mul ") + op_t + " " + lv + ", " + rv);
            break;
        case OpKind::Div:
            emit_instr(res + " = " + (is_fp ? "fdiv " : (uns ? "udiv " : "sdiv ")) + op_t + " " + lv + ", " + rv);
            break;
        case OpKind::Mod:
            emit_instr(res + " = " + (uns ? "urem " : "srem ") + op_t + " " + lv + ", " + rv);
            break;
        case OpKind::BitAnd:
            emit_instr(res + " = and " + op_t + " " + lv + ", " + rv);
            break;
        case OpKind::BitOr:
            emit_instr(res + " = or " + op_t + " " + lv + ", " + rv);
            break;
        case OpKind::BitXor:
            emit_instr(res + " = xor " + op_t + " " + lv + ", " + rv);
            break;
        case OpKind::Shl:
            emit_instr(res + " = shl " + op_t + " " + lv + ", " + rv);
            break;
        case OpKind::Shr:
            emit_instr(res + " = " + (uns ? "lshr " : "ashr ") + op_t + " " + lv + ", " + rv);
            break;
        default:
            emit_instr(res + " = add " + op_t + " 0, 0");
            break;
    }
    return res;
}

std::string Translator::eval_unary(Expr* e, Expr::Unary* u) {
    // 一元操作符重载派发
    if (e->sym && e->sym->kind == SymbolKind::Function) {
        if (Decl* d = e->sym->get_decl()) {
            if (d->kind == DeclKind::Operator)
                return emit_operator_call(e, static_cast<Operator*>(d),
                                          u->operand, nullptr);
        }
    }
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
            // 类值在 IR 中即为实例指针，解引用直接给出指针
            if (e->type && as_class(e->type)) return ov;
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

std::string Translator::eval_assign(Expr* e, Expr::Assign* a) {
    // 复合赋值操作符重载：t += v 派发到 operator+=
    if (a->op != OpKind::Assign && e && e->sym &&
        e->sym->kind == SymbolKind::Function) {
        if (Decl* d = e->sym->get_decl()) {
            if (d->kind == DeclKind::Operator)
                return emit_operator_call(e, static_cast<Operator*>(d),
                                          a->target, a->value);
        }
    }
    // 引用首次绑定：把右值的地址存入引用槽
    if (a->target && a->target->sym) {
        if (auto* vi = dynamic_cast<VarItem*>(a->target->sym->node)) {
            if (is_ref_item(vi, nullptr) && !ctx_.ref_bound.count(vi)) {
                auto it = ctx_.slots.find(vi);
                if (it != ctx_.slots.end()) {
                    std::string addr = eval_lvalue(a->value);
                    if (addr.empty()) addr = eval(a->value);
                    emit_instr("store ptr " + addr + ", ptr " + it->second);
                    ctx_.ref_bound.insert(vi);
                    return addr;
                }
            }
        }
    }
    std::string ptr = eval_lvalue(a->target);
    if (ptr.empty()) return "0";
    std::string t = a->target && a->target->type ? map_type(a->target->type) : "i32";
    // 字符串字面量赋给切片：构造 {ptr,i32}，而非把常量指针 @.str 直接存
    std::string v;
    std::string vsrc = a->value && a->value->type
        ? map_type(a->value->type) : t;
    bool converted = needs_convert(a->value);
    if (converted)
        v = eval_converted(a->value);
    else if (t == "{ ptr, i32 }" && a->value && a->value->type &&
        a->value->type->is_string_literal_type())
        v = build_string_slice(a->value);
    else {
        v = eval(a->value);
        if (vsrc != t) v = cast_value(v, vsrc, t,
            !expr_is_unsigned(a->value ? a->value->type : nullptr),
            !expr_is_unsigned(a->target ? a->target->type : nullptr));
    }
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
            case OpKind::ShrAssign: {
                bool u = !ir_is_fp_type(t) && expr_is_unsigned(a->target ? a->target->type : nullptr);
                emit_instr(r2 + " = " + (u ? "lshr " : "ashr ") + t + " " + cur + ", " + v);
                break;
            }
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
        std::string len = ir_.reg();
        emit_instr(len + " = extractvalue { ptr, i32 } " + s + ", 1");
        return len;
    }
    if (m->member == "ptr" && m->target && m->target->type &&
        m->target->type->is_array_type()) {
        std::string s = eval(m->target);
        std::string p = ir_.reg();
        emit_instr(p + " = extractvalue { ptr, i32 } " + s + ", 0");
        return p;
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
        et = "{ ptr, i32 }";
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
    emit_instr(s1 + " = insertvalue { ptr, i32 } undef, ptr " + backing + ", 0");
    std::string s2 = ir_.reg();
    emit_instr(s2 + " = insertvalue { ptr, i32 } " + s1 + ", i32 " +
               std::to_string((long long)n) + ", 1");
    return s2;
}

std::string Translator::build_string_slice(Expr* lit) {
    auto* l = lit ? lit->get_literal() : nullptr;
    std::string content = l ? std::string(l->value) : std::string();
    size_t n = content.size();
    std::string sym = ir_.string_const(content);
    std::string backing = ir_.reg();
    // 多分配 1 字节存放 NUL 终止符
    emit_instr(backing + " = alloca i8, i64 " + std::to_string(n + 1));
    for (size_t k = 0; k <= n; ++k) {
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
    emit_instr(s1 + " = insertvalue { ptr, i32 } undef, ptr " + backing + ", 0");
    std::string s2 = ir_.reg();
    emit_instr(s2 + " = insertvalue { ptr, i32 } " + s1 + ", i32 " +
               std::to_string((long long)n) + ", 1");
    return s2;
}

std::string Translator::cast_value(const std::string& v,
                                   const std::string& src,
                                   const std::string& dst,
                                   bool src_signed,
                                   bool dst_signed) {
    if (src == dst) return v;
    // 切片 -> 指针：取底层指针域
    if (src == "{ ptr, i32 }" && dst == "ptr") {
        std::string r0 = ir_.reg();
        emit_instr(r0 + " = extractvalue { ptr, i32 } " + v + ", 0");
        return r0;
    }
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
        if (src == "{ ptr, i32 }") {
            std::string n = ir_.reg();
            emit_instr(n + " = extractvalue { ptr, i32 } " + v + ", 1");
            emit_instr(r + " = icmp ne i32 " + n + ", 0");
            return r;
        }
        return v;
    }
    if (is_int_n(src) && is_int_n(dst)) {
        int s = int_bits(src), d = int_bits(dst);
        if (s < d)
            emit_instr(r + " = " + std::string(src_signed ? "sext " : "zext ") +
                       src + " " + v + " to " + dst);
        else if (s > d)
            emit_instr(r + " = trunc " + src + " " + v + " to " + dst);
        else
            return v;
        return r;
    }
    if (is_int_n(src) && is_fp(dst)) {
        emit_instr(r + " = " + std::string(src_signed ? "sitofp " : "uitofp ") +
                   src + " " + v + " to " + dst);
        return r;
    }
    if (is_fp(src) && is_int_n(dst)) {
        emit_instr(r + " = " + std::string(dst_signed ? "fptosi " : "fptoui ") +
                   src + " " + v + " to " + dst);
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
    Expr* dst_expr = c->target_type;
    std::string src = map_type(src_expr);
    std::string dst = map_type(dst_expr);
    std::string v = eval(c->operand);
    return cast_value(v, src, dst,
                      !expr_is_unsigned(src_expr), !expr_is_unsigned(dst_expr));
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

    // 标量返回且走寄存器：无 sret，调用直接取返回值
    bool via_reg = cf->ret_via_reg;
    std::string sret_slot;
    std::vector<std::string> args;
    if (!via_reg) {
        sret_slot = ir_.reg();
        emit_instr(sret_slot + " = alloca " + ret_ir);
        args.push_back("ptr sret(" + ret_ir + ") " + sret_slot);
    }

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

    std::string call_ret = via_reg ? ret_ir : "void";
    std::string sig = "call " + call_ret + " @" +
                      IRWriter::quote(mangler_.of(cf)) + "(";
    for (size_t k = 0; k < args.size(); ++k) {
        if (k) sig += ", ";
        sig += args[k];
    }
    sig += ")";
    if (via_reg) {
        std::string r = ir_.reg();
        emit_instr(r + " = " + sig);
        return r;
    }
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
    std::string ret_t = e && e->type ? map_type(e->type) : "void";

    // 合成的复合赋值：原地修改 self，无 sret、无返回值
    bool compound = op->synthesized &&
        (op->op == OpKind::PlusAssign || op->op == OpKind::MinusAssign ||
         op->op == OpKind::MulAssign  || op->op == OpKind::DivAssign ||
         op->op == OpKind::ModAssign  || op->op == OpKind::AndAssign ||
         op->op == OpKind::OrAssign   || op->op == OpKind::XorAssign ||
         op->op == OpKind::ShlAssign  || op->op == OpKind::ShrAssign);
    if (compound) {
        std::vector<std::string> args;
        std::string self_arg = eval(lhs);
        args.push_back("ptr " + self_arg);
        std::string v = eval_converted(rhs);
        std::string pt = "i32";
        for (VarList* vl : op->params) {
            if (!vl) continue;
            for (VarItem* vi : vl->vars) {
                Expr* pte = vi->type ? vi->type : vl->type;
                if (pte) pt = map_type(pte);
                break;
            }
            break;
        }
        args.push_back(pt + " " + v);
        std::string sig = "call void @" + IRWriter::quote(mangler_.of(op)) + "(";
        for (size_t k = 0; k < args.size(); ++k) {
            if (k) sig += ", ";
            sig += args[k];
        }
        sig += ")";
        emit_instr(sig);
        return "0";
    }

    // 标量返回且走寄存器：无 sret，调用直接取返回值
    bool via_reg = op->ret_via_reg;
    std::string sret_slot;
    std::vector<std::string> args;
    if (!via_reg) {
        sret_slot = ir_.reg();
        emit_instr(sret_slot + " = alloca " + ret_t);
        args.push_back("ptr sret(" + ret_t + ") " + sret_slot);
    }

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
            std::string v = eval_converted(arg);
            args.push_back(pt + " " + v);
            ++i;
        }
    }

    std::string call_ret = via_reg ? ret_t : "void";
    std::string sig = "call " + call_ret + " @" +
                      IRWriter::quote(mangler_.of(op)) + "(";
    for (size_t k = 0; k < args.size(); ++k) {
        if (k) sig += ", ";
        sig += args[k];
    }
    sig += ")";
    if (via_reg) {
        std::string r = ir_.reg();
        emit_instr(r + " = " + sig);
        return r;
    }
    emit_instr(sig);
    // 类返回值经 sret 传出的是实例指针，加载后返回
    if (e->type && as_class(e->type)) {
        std::string loaded = ir_.reg();
        emit_instr(loaded + " = load ptr, ptr " + sret_slot);
        return loaded;
    }
    return sret_slot;
}

void Translator::emit_declare(Func& f) {
    std::string ret_t = "void";
    VarItem* ret_var = nullptr;
    if (!f.returns.empty() && f.returns.back()) {
        VarList* rl = f.returns.back();
        if (!rl->vars.empty()) ret_var = rl->vars.back();
        Expr* rt = (ret_var && ret_var->type) ? ret_var->type : rl->type;
        if (rt) ret_t = map_type(rt);
    }

    std::vector<std::string> params;
    if (ret_var && !f.ret_via_reg)
        params.push_back("ptr sret(" + ret_t + ") %ret");
    int idx = 0;
    for (VarList* vl : f.params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            std::string vt = vi->type ? map_type(vi->type)
                                      : (vl->type ? map_type(vl->type) : "i32");
            params.push_back(vt + " %a" + std::to_string(idx++));
        }
    }

    std::string ret_sig = (ret_var && !f.ret_via_reg) ? "void" : ret_t;
    std::string sig = "declare " + ret_sig + " @" +
                      IRWriter::quote(mangler_.of(&f)) + "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) sig += ", ";
        sig += params[i];
    }
    sig += ")";
    ir_.emit_type(sig);
}

// 内置 free：释放 malloc 类实例
std::string Translator::eval_free_builtin(Expr* e, Expr::Call* c) {
    (void)e;
    if (!c->free_builtin || !c->callee) return "";
    auto* m = c->callee->get_member();
    if (!m) return "";
    std::string self = eval(m->target);
    Class* cls = m->target && m->target->type
        ? as_wrap_class(m->target->type) : nullptr;
    if (!cls && m->target && m->target->type)
        cls = as_class(m->target->type);
    if (cls) {
        if (SpecialMethod* drop = find_special(cls, SpecialMethod::Kind::Drop))
            emit_instr("call void @" + IRWriter::quote(mangler_.of(drop)) +
                       "(ptr " + self + ")");
    }
    ir_.need_free();
    emit_instr("call void @free(ptr " + self + ")");
    return "0";
}

// 无 INIT 的类零参默认构造：分配实例
std::string Translator::eval_default_ctor(Expr* e, Expr::Call* c) {
    if (c->resolved || !c->callee || !c->callee->sym ||
        c->callee->sym->kind != SymbolKind::Class) return "";
    Class* cls = as_class(e->type);
    if (!cls) return "0";
    auto it = classes_.find(cls);
    if (it == classes_.end()) return "0";
    std::string slot;
    if (cls->specs.has(SpecKind::Malloc)) {
        ir_.need_malloc();
        slot = ir_.reg();
        emit_instr(slot + " = call ptr @malloc(i64 " +
                   std::to_string(type_size(it->second.llvm_name)) + ")");
        emit_bind_ref(cls, slot, SpecialMethod::Kind::Ref);
    } else {
        slot = ir_.reg();
        emit_instr(slot + " = alloca " + it->second.llvm_name);
    }
    return slot;
}

// 类构造：C(...) — resolved 是 INIT 特殊方法
std::string Translator::eval_ctor_call(Expr* e, Expr::Call* c, SpecialMethod* sm) {
    if (sm->kind != SpecialMethod::Kind::Init) return "0";
    Class* cls = as_class(e->type);
    if (!cls) return "0";
    auto it = classes_.find(cls);
    if (it == classes_.end()) return "0";
    std::string slot;
    bool is_malloc = cls->specs.has(SpecKind::Malloc);
    if (is_malloc) {
        ir_.need_malloc();
        slot = ir_.reg();
        emit_instr(slot + " = call ptr @malloc(i64 " +
                   std::to_string(type_size(it->second.llvm_name)) + ")");
    } else {
        slot = ir_.reg();
        emit_instr(slot + " = alloca " + it->second.llvm_name);
    }
    std::vector<std::string> args;
    args.push_back("ptr " + slot);
    size_t ai = 0;
    for (Expr* a : c->args) {
        bool converted = needs_convert(a);
        std::string v = converted ? eval_converted(a) : eval(a);
        std::string at = a && a->type ? map_type(a->type) : "i32";
        if (converted) {
            // 转换后实参类型即对应形参类型
            size_t k = 0;
            for (VarList* vl : sm->params) {
                if (!vl) continue;
                bool found = false;
                for (VarItem* vi : vl->vars) {
                    if (k == ai) {
                        Expr* pte = vi->type ? vi->type : vl->type;
                        if (pte) at = map_type(pte);
                        found = true;
                        break;
                    }
                    ++k;
                }
                if (found) break;
            }
        }
        args.push_back(at + " " + v);
        ++ai;
    }
    std::string sig = "call void @" + IRWriter::quote(mangler_.of(sm)) + "(";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) sig += ", ";
        sig += args[i];
    }
    sig += ")";
    emit_instr(sig);
    if (is_malloc) emit_bind_ref(cls, slot, SpecialMethod::Kind::Ref);
    return slot;
}

// 普通函数/运算符调用
std::string Translator::eval_plain_call(Expr* e, Expr::Call* c, Func* fn) {
    (void)e;
    std::string fname = mangler_.of(fn);
    std::string ret_t = "void";
    VarItem* ret_var = nullptr;
    if (!fn->returns.empty() && fn->returns.back()) {
        VarList* rl = fn->returns.back();
        if (!rl->vars.empty()) ret_var = rl->vars.back();
        Expr* rt_ = (ret_var && ret_var->type) ? ret_var->type : rl->type;
        if (rt_) ret_t = map_type(rt_);
    }

    std::vector<std::string> args;

    // 方法调用：callee 是 Member 且 target 是类实例
    bool has_self = false;
    std::string self_arg;
    if (Expr* callee = c->callee) {
        if (auto* m = callee->get_member()) {
            Expr* tgt = m->target;
            if (as_class(tgt ? tgt->type : nullptr) &&
                !fn->specs.has(SpecKind::Static)) {
                self_arg = eval(tgt);
                has_self = true;
            }
        }
    }

    bool via_reg = fn->ret_via_reg && ret_var;
    std::string sret_slot;
    if (ret_var && !via_reg) {
        sret_slot = ir_.reg();
        emit_instr(sret_slot + " = alloca " + ret_t);
        args.push_back("ptr sret(" + ret_t + ") " + sret_slot);
    }
    if (has_self) args.push_back("ptr " + self_arg);
    // 形参 IR 类型表，用于字符串字面量按切片形参构造 {ptr,i32}
    std::vector<std::string> param_ir;
    for (VarList* vl : fn->params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            std::string pt = vi->type ? map_type(vi->type)
                                      : (vl->type ? map_type(vl->type) : "i32");
            param_ir.push_back(pt);
        }
    }
    for (size_t ai = 0; ai < c->args.size(); ++ai) {
        Expr* a = c->args[ai];
        std::string at = a && a->type ? map_type(a->type) : "i32";
        std::string want = ai < param_ir.size() ? param_ir[ai] : at;
        std::string v;
        bool converted = needs_convert(a);
        if (converted)
            v = eval_converted(a);
        else if (want == "{ ptr, i32 }" && a && a->type &&
            a->type->is_string_literal_type())
            v = build_string_slice(a);
        else {
            v = eval(a);
            if (at != want) {
                Expr* pt_expr = nullptr;
                // 找到该实参对应的形参类型节点，用于取符号
                size_t k = 0;
                for (VarList* vl : fn->params) {
                    if (!vl) continue;
                    bool found = false;
                    for (VarItem* vi : vl->vars) {
                        if (k == ai) { pt_expr = vi->type ? vi->type : vl->type; found = true; break; }
                        ++k;
                    }
                    if (found) break;
                }
                v = cast_value(v, at, want,
                    !expr_is_unsigned(a ? a->type : nullptr),
                    !expr_is_unsigned(pt_expr));
            }
        }
        args.push_back(want + " " + v);
    }

    bool is_main = (std::string(fn->name) == "main" && ctx_.current_class == nullptr);
    bool plain_main = is_main && !ret_var && ret_t == "void";

    std::string call_ret;
    if (via_reg) call_ret = ret_t;
    else call_ret = ret_var ? "void" : (plain_main ? "i32" : ret_t);
    std::string sig = call_ret + " @" + IRWriter::quote(fname) + "(";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) sig += ", ";
        sig += args[i];
    }
    sig += ")";

    if (ret_var && !via_reg) {
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

std::string Translator::eval_call(Expr* e, Expr::Call* c) {
    if (c->free_builtin && c->callee) {
        std::string r = eval_free_builtin(e, c);
        if (!r.empty()) return r;
    }
    // funcptr 调用
    if (c->callee && c->callee->type && c->callee->type->is_func_ptr()) {
        auto* fp = c->callee->type->get_func_ptr();
        return call_func_ptr(e, c, fp);
    }
    if (!c->resolved && c->callee && c->callee->sym &&
        c->callee->sym->kind == SymbolKind::Class) {
        return eval_default_ctor(e, c);
    }
    if (!c->resolved) return "0";

    if (c->resolved->kind == DeclKind::SpecialMethod) {
        auto* sm = static_cast<SpecialMethod*>(c->resolved);
        return eval_ctor_call(e, c, sm);
    }

    if (c->resolved->kind != DeclKind::Func &&
        c->resolved->kind != DeclKind::Operator) return "0";
    return eval_plain_call(e, c, static_cast<Func*>(c->resolved));
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
    bool via_reg = (ret_t != "void");

    std::vector<std::string> args;
    for (size_t i = 0; i < c->args.size(); ++i) {
        Expr* a = c->args[i];
        Expr* pt_expr = i < fp->params.size() ? fp->params[i] : nullptr;
        std::string pt = pt_expr ? map_type(pt_expr) : (a && a->type ? map_type(a->type) : "i32");
        std::string v = eval_converted(a);
        std::string at = a && a->type ? map_type(a->type) : pt;
        bool converted = needs_convert(a);
        if (!converted && at != pt)
            v = cast_value(v, at, pt,
                !expr_is_unsigned(a ? a->type : nullptr),
                !expr_is_unsigned(pt_expr));
        args.push_back(pt + " " + v);
    }

    std::string arglist;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) arglist += ", ";
        arglist += args[i];
    }
    if (via_reg) {
        std::string r = ir_.reg();
        emit_instr(r + " = call " + ret_t + " " + fpv + "(" + arglist + ")");
        return r;
    }
    emit_instr("call void " + fpv + "(" + arglist + ")");
    return "0";
}

std::string Translator::eval_ternary(Expr* e, Expr::Ternary* t) {
    std::string ttype = e->type ? map_type(e->type) : "i32";
    std::string res_slot = ir_.reg();
    emit_instr(res_slot + " = alloca " + ttype);

    std::string cond = eval_cond(t->cond);
    std::string then_l = ir_.named_label("tern.then");
    std::string else_l = ir_.named_label("tern.else");
    std::string end_l  = ir_.named_label("tern.end");
    emit_term("br i1 " + cond + ", label %" + then_l + ", label %" + else_l);

    open_block(then_l);
    std::string tv = eval(t->then_expr);
    emit_instr("store " + ttype + " " + tv + ", ptr " + res_slot);
    emit_term("br label %" + end_l);

    open_block(else_l);
    std::string ev = eval(t->else_expr);
    emit_instr("store " + ttype + " " + ev + ", ptr " + res_slot);
    emit_term("br label %" + end_l);

    open_block(end_l);
    std::string res = ir_.reg();
    emit_instr(res + " = load " + ttype + ", ptr " + res_slot);
    return res;
}

std::string Translator::eval_spawn(Expr*, Expr::Spawn* s) {
    ir_.need_pthread();
    ir_.need_malloc();
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
                !fn->specs.has(SpecKind::Static)) {
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
    size_t total = field_types.empty() ? 1 : field_types.size() * 16;
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
    std::string sig = call_ret + " @" + IRWriter::quote(mangler_.of(fn)) + "(";
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
