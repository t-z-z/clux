#include "../include/translator.h"
#include "../include/ir_writer.h"
#include "../include/semantic.h"
#include "../include/sem_types.h"
#include "../include/translator_mangle.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace cx {

namespace {
// 长度文本是整数字面量时取出数值；否则视为运行期值
bool const_count(const std::string& s, long long& out) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        return false;
    out = 0;
    for (char c : s) {
        if (out > (1LL << 40)) return false;   // 过大按运行期处理
        out = out * 10 + (c - '0');
    }
    return true;
}
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

Type* strip_modifiers(Type* t) {
    return t;
}

bool expr_is_unsigned(Type* t);

// 比较/算术操作数提升后的无符号性：位宽不同取宽者，相同则任一为无符号即无符号
bool common_unsigned(const std::string& lt, const std::string& rt,
                     Type* l, Type* r) {
    if (ir_is_fp_type(lt) || ir_is_fp_type(rt)) return false;
    int lb = ir_is_int_type(lt) ? ir_int_bits(lt) : 0;
    int rb = ir_is_int_type(rt) ? ir_int_bits(rt) : 0;
    bool lu = expr_is_unsigned(l), ru = expr_is_unsigned(r);
    if (lb != rb) return lb > rb ? lu : ru;
    return lu || ru;
}

// 表达式类型是否为无符号整数（不含 bool）
bool expr_is_unsigned(Type* t) {
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

// 行首是块终结指令的助记符：keep 保留的代码由用户书写，
// 只能从末行判断本块是否已被终结
bool is_terminator_mnemonic(std::string_view line) {
    static const char* kTerms[] = {
        "ret", "br", "switch", "indirectbr", "invoke", "callbr",
        "resume", "catchswitch", "catchret", "cleanupret", "unreachable"
    };
    for (const char* t : kTerms) {
        size_t n = std::strlen(t);
        if (line.size() <= n || line.compare(0, n, t) != 0) continue;
        char c = line[n];
        if (c == ' ' || c == '\t') return true;
    }
    return false;
}
} // namespace


// 收集

void Translator::error(const Location& loc, std::string msg) {
    diags_.emplace_back(DiagLevel::Error, std::move(msg), loc);
    has_error_ = true;
}

void Translator::collect() {
    mangler_.collect(cu_);
    collect_classes();
    collect_globals_decl(nullptr, "");
}

void Translator::run() {
    // 类布局有环时已报错，不再发射任何 IR
    if (has_error_) return;
    for (auto& g : globals_) {
        if (g.skip_emit) continue;
        ir_.emit_global(g.is_extern
            ? "@" + g.sym + " = external global " + g.ir_type
            : "@" + g.sym + " = global " + g.ir_type + " zeroinitializer");
    }
    for (Decl* d : cu_.top_decls) emit_decl(d);
    if (uses_alwaysinline_)
        ir_.emit_module_tail("attributes #0 = { alwaysinline }");
}

// 类结构体类型与 static 字段的全局存储覆盖所有模块，只能发一次
void Translator::emit_class_decls() {
    emit_class_types();
    for (auto& [cls, info] : classes_) {
        if (info.from_keep_ir) continue;
        for (auto& f : info.static_fields)
            ir_.emit_global("@" + mangler_.field_of(f.var) +
                            " = global " + f.ir_type + " zeroinitializer");
    }
}

// 模块级变量：顶层、命名空间内、extern 内的 VarList，按 mangled 名逐个登记
void Translator::collect_globals_decl(Decl* d, const std::string& prefix,
                                      ExternLang lang) {
    if (!d) {
        for (Decl* t : cu_.top_decls) collect_globals_decl(t, prefix, lang);
        return;
    }
    if (d->kind == DeclKind::Namespace) {
        std::string here = prefix.empty()
            ? std::string(d->name) : prefix + "_" + std::string(d->name);
        for (Decl* m : static_cast<Namespace*>(d)->members)
            collect_globals_decl(m, here, lang);
        return;
    }
    // extern 内符号名保持原样，与 Mangler 的处理一致
    if (d->kind == DeclKind::Extern) {
        for (Decl* m : static_cast<Extern*>(d)->members)
            collect_globals_decl(m, "", static_cast<Extern*>(d)->language());
        return;
    }
    if (d->kind != DeclKind::VarList) return;
    for (VarItem* vi : static_cast<VarList*>(d)->vars) {
        if (!vi) continue;
        std::string sym = mangler_.global_of(vi);
        if (sym.empty()) continue;
        Type* vt = vi->type ? vi->type : static_cast<VarList*>(d)->type;
        GlobalVar g;
        g.var = vi;
        g.sym = sym;
        g.ir_type = vt ? object_type(vt) : "i32";
        g.is_extern = lang != ExternLang::None;
        // extern "LLVMIR" 的符号在 clux 侧只有声明，实现由 keep`` 给出
        g.skip_emit = lang == ExternLang::LLVMIR;
        globals_.push_back(g);
        global_names_[vi] = sym;
    }
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
        case DeclKind::Extern: {
            auto* ext = static_cast<Extern*>(d);
            // extern "LLVMIR" 的符号实现由 keep`` 给出，这里只保留 clux 侧可见性
            if (ext->language() == ExternLang::LLVMIR) break;
            for (Decl* m : ext->members) {
                if (m && (m->kind == DeclKind::Func || m->kind == DeclKind::Operator))
                    emit_declare(*static_cast<Func*>(m));
            }
            break;
        }
        case DeclKind::Keep:
            ir_.emit_type(std::string(static_cast<Keep*>(d)->code));
            break;
        default: break;
    }
}

void Translator::collect_classes() {
    for (Decl* d : cu_.top_decls) collect_classes_decl(d);
}

// 内联类字段的类型依赖：A 内联 B 要求 %B 先于 %A 定义。
// 出现回边说明布局本身无解（自身内联或相互内联），由 cycle_at 带出 culprit
static void topo_visit(const std::unordered_map<Class*, std::vector<Class*>>& deps,
                       Class* c, std::unordered_map<Class*, int>& state,
                       std::vector<Class*>& out, Class** cycle_at) {
    int& st = state[c];
    if (st == 2) return;
    if (st == 1) { *cycle_at = c; return; }
    st = 1;
    auto it = deps.find(c);
    if (it != deps.end()) {
        for (Class* d : it->second) {
            topo_visit(deps, d, state, out, cycle_at);
            if (*cycle_at) return;
        }
    }
    st = 2;
    out.push_back(c);
}

// 每个类的内联类字段所指向的类
std::unordered_map<Class*, std::vector<Class*>> Translator::inline_deps() const {
    std::unordered_map<Class*, std::vector<Class*>> deps;
    for (Class* c : class_order_) {
        auto it = classes_.find(c);
        if (it == classes_.end()) continue;
        for (const auto& f : it->second.fields) {
            if (!f.var) continue;
            // 自身内联也记为依赖：语义阶段已拦下成环的情况，这里只用于排序
            if (as_class(f.type)) deps[c].push_back(as_class(f.type));
        }
    }
    return deps;
}

// 全部类结构体类型集中发射一次：LLVM 文本 IR 要求被引用的类型先定义，
// 逐个随 emit_class 发射会在内联字段引用后声明的类时产生前向引用
void Translator::emit_class_types() {
    auto deps = inline_deps();
    std::unordered_map<Class*, int> state;
    std::vector<Class*> order;
    for (Class* c : class_order_) {
        Class* cycle_at = nullptr;
        topo_visit(deps, c, state, order, &cycle_at);
    }
    for (Class* c : order) {
        const auto& info = classes_[c];
        // extern "LLVMIR" 的类布局由 keep`` 给出，类型也由 keep`` 定义
        if (info.from_keep_ir) continue;
        std::string ty = info.llvm_name + " = type {";
        for (size_t i = 0; i < info.fields.size(); ++i) {
            if (i) ty += ", ";
            ty += info.fields[i].ir_type;
        }
        ty += "}";
        ir_.emit_type(ty);
    }
}

void Translator::collect_classes_decl(Decl* d, bool from_keep_ir) {
    if (!d) return;
    switch (d->kind) {
        case DeclKind::Class: {
            Class* c = static_cast<Class*>(d);
            TranslatorClass info;
            info.from_keep_ir = from_keep_ir;
            info.llvm_name = "%" + mangler_.of(c);
            int idx = 0;
            for (Decl* m : c->members) {
                if (!m || m->kind != DeclKind::VarList) continue;
                VarList* vl = static_cast<VarList*>(m);
                bool is_static = vl->specs.has(SpecKind::Static);
                for (VarItem* vi : vl->vars) {
                    if (!vi) continue;
                    TranslatorField f;
                    f.var = vi;
                    f.index = is_static ? -1 : idx++;
                    // 定长数组与内联类字段在布局里占真实空间。类字段内联后
                    // 随宿主对象一起活着，不会像指针字段那样指向 INIT 栈帧上
                    // 的临时实例；指针与引用字段仍是 ptr，槽里存地址
                    Type* ft = vi->type ? vi->type : vl->type;
                    Class* fc = as_class(ft);
                    f.type = ft;
                    f.ir_type = fc ? ("%" + mangler_.of(fc))
                                   : (ft ? object_type(ft) : "i32");
                    if (is_static) {
                        // static 字段存于模块级全局变量，不进实例 layout
                        info.static_fields.push_back(f);
                    } else {
                        if (vi->name == "__value") info.value_index = f.index;
                        info.field_index[vi] = f.index;
                        info.fields.push_back(f);
                    }
                }
            }
            classes_[c] = std::move(info);
            class_order_.push_back(c);
            own_classes_.push_back(c);
            for (Decl* m : c->members) {
                if (m && m->kind == DeclKind::Class)
                    collect_classes_decl(m, from_keep_ir);
            }
            break;
        }
        case DeclKind::Namespace:
            for (Decl* m : static_cast<Namespace*>(d)->members)
                collect_classes_decl(m);
            break;
        // extern "LLVMIR" 的类布局来自 keep`` 的 IR：只登记名字与字段，
        // 结构体类型与 static 字段存储都由 keep`` 负责
        case DeclKind::Extern: {
            auto* ext = static_cast<Extern*>(d);
            if (ext->language() != ExternLang::LLVMIR) break;
            for (Decl* m : ext->members)
                if (m && m->kind == DeclKind::Class)
                    collect_classes_decl(m, true);
            break;
        }
        default: break;
    }
}

std::string Translator::map_type(Type* t) {
    // 语义通过的程序里表达式类型必然非空；走到这里说明翻译器内部有漏配类型，
    // 静默当作 void 会生成非法 IR 且报错指向 .ll 而非源码，故显式报错
    if (!t) {
        error(Location(), "internal: map_type on null type");
        return "i32";
    }
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
    // 定长数组的值即存储地址（见 object_type），切片是 {ptr,len} 二元组
    if (t->get_array_type()) return is_fixed_array(t) ? "ptr" : "{ ptr, i32 }";
    if (t->get_string_literal_type()) return "ptr";
    if (t->get_func_ptr()) return "ptr";
    if (t->get_func_ref()) return "ptr";
    if (t->is_null()) return "ptr";
    if (as_class(t)) return "ptr";
    return "i32";
}

bool Translator::is_fixed_array(Type* t) const {
    auto* a = t ? t->get_array_type() : nullptr;
    return a && a->kind != ArrayKind::Slice;
}

std::string Translator::object_type(Type* t) {
    if (!is_fixed_array(t)) return map_type(t);
    auto* a = t->get_array_type();
    // 元素本身也可能是定长数组（多维数组）：必须递归展开成内联存储，
    // 用 map_type 会把内层数组退化成 ptr，生成 [N x ptr] 这种没有实际
    // 存储的悬空类型。
    return "[" + std::to_string(array_length_of(t)) + " x " +
           (a->element ? object_type(a->element) : "i32") + "]";
}

// 入口块 alloca：长度是编译期常量的存储在这里分配，整个函数一次，
// 循环里复用同一块，不会逐轮吃栈
std::string Translator::entry_alloca(const std::string& ir_type) {
    std::string slot = ir_.reg();
    ir_.emit_entry(slot + " = alloca " + ir_type);
    return slot;
}

// 运行期长度的存储：仍然是栈上 alloca（用户没写 malloc 修饰就不该动堆），
// 但 alloca 只在函数返回时释放，循环里逐轮分配就是纯粹的栈浪费，
// 故用 stacksave/stackrestore 把这段栈在作用域结束时还给调用者
std::string Translator::alloc_slice_storage(Type* elem_type,
                                            const std::string& count) {
    ir_.need_stackrw();
    std::string save = ir_.reg();
    emit_instr(save + " = call ptr @llvm.stacksave()");
    std::string et = elem_type ? map_type(elem_type) : "i32";
    std::string p = ir_.reg();
    emit_instr(p + " = alloca " + et + ", i64 " + count);
    CleanupEntry e;
    e.kind = CleanupEntry::Restore;
    e.ptr = save;
    e.scope_depth = ctx_.scope_depth;
    ctx_.drops.push_back(e);
    return p;
}

// 数组/切片/字符串的长度：编译期已知返回常量文本，否则返回运行期值
std::string Translator::length_value_of(Expr* e) {
    if (!e || !e->type) return "";
    if (is_fixed_array(e->type))
        return std::to_string(array_length_of(e->type));
    if (is_slice_type(e->type)) {
        std::string v = eval(e);
        std::string n = ir_.reg();
        emit_instr(n + " = extractvalue { ptr, i32 } " + v + ", 1");
        return n;
    }
    if (auto* lit = e->get_literal())
        if (lit->kind == LiteralKind::String)
            return std::to_string(lit->value.size());
    return "";
}

// 拷贝 count 个元素（count 为 i64 值或整数字面量）
void Translator::copy_elements(const std::string& dst, const std::string& src,
                               Type* elem_type, const std::string& count) {
    std::string et = elem_type ? map_type(elem_type) : "i32";
    long esz = type_bytes_of(et);
    if (esz <= 0) return;
    std::string bytes = count;
    if (esz != 1) {
        std::string b = ir_.reg();
        emit_instr(b + " = mul i64 " + count + ", " + std::to_string(esz));
        bytes = b;
    }
    ir_.need_memcpy();
    emit_instr("call void @llvm.memcpy.p0i8.p0i8.i64(ptr " + dst + ", ptr " + src +
               ", i64 " + bytes + ", i1 false)");
}

// 两个长度（常量文本或运行期值）取较小者，运行期部分用 icmp/select
std::string Translator::min_count(const std::string& a, const std::string& b) {
    long long av = 0, bv = 0;
    bool ac = const_count(a, av), bc = const_count(b, bv);
    if (ac && bc) return std::to_string(std::min(av, bv));
    if (ac) return min_with_const(b, av);
    if (bc) return min_with_const(a, bv);
    std::string cmp = ir_.reg();
    emit_instr(cmp + " = icmp ult i64 " + a + ", " + b);
    std::string r = ir_.reg();
    emit_instr(r + " = select i1 " + cmp + ", i64 " + a + ", i64 " + b);
    return r;
}

// 运行期长度 v 与常量 k 取小
std::string Translator::min_with_const(const std::string& v, long long k) {
    std::string cmp = ir_.reg();
    emit_instr(cmp + " = icmp ult i64 " + v + ", " + std::to_string(k));
    std::string r = ir_.reg();
    emit_instr(r + " = select i1 " + cmp + ", i64 " + v + ", i64 " +
               std::to_string(k));
    return r;
}

// 把初值内容拷进已有存储：实际拷贝长度取目标长度与初值长度的较小者，
// 否则源或目标会有一侧越界
void Translator::copy_init_into(const std::string& backing, Type* el,
                                const std::string& len64, Expr* init) {
    if (!init || backing.empty() || backing == "null" || len64.empty()) return;
    if (auto* lit = init->get_literal()) {
        if (lit->kind == LiteralKind::String) {
            // 字符串字面量：连 NUL 一起，放得下几个拷几个
            std::string sym = ir_.string_const(lit->value);
            copy_elements(backing, "@" + sym, el,
                          min_count(len64, std::to_string(lit->value.size() + 1)));
            return;
        }
    }
    std::string src = data_ptr_of(init);
    std::string slen = length_value_of(init);
    if (src.empty() || src == "null" || slen.empty()) return;
    long long sv = 0;
    if (!const_count(slen, sv)) {
        // 切片长度是运行期的 uint，先零扩展到 i64 再比
        std::string s64 = ir_.reg();
        emit_instr(s64 + " = zext i32 " + slen + " to i64");
        slen = s64;
    }
    copy_elements(backing, src, el, min_count(len64, slen));
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

Class* Translator::pointee_class(Type* t) {
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

// 对象销毁时其指针字段持有的对象也结束引用：字段指向的类有 DEREF 才登记。
// 内联类字段逐级下钻，path 记录从宿主实例到目标指针字段的 getelementptr 链
void Translator::register_field_derefs(
        const std::string& slot, Class* cls, int depth,
        std::vector<std::pair<std::string, int>> path) {
    if (!cls) return;
    auto it = classes_.find(cls);
    if (it == classes_.end()) return;
    for (const auto& f : it->second.fields) {
        if (!f.type) continue;
        // 内联类字段：字段存储即嵌套对象，继续往下一层找它内部的指针字段
        if (as_class(f.type)) {
            auto sub = path;
            sub.emplace_back(it->second.llvm_name, f.index);
            register_field_derefs(slot, as_class(f.type), depth, sub);
            continue;
        }
        if (!f.type->is_pointer()) continue;
        Class* pc = pointee_class(f.type);
        if (!find_special(pc, SpecialMethod::Kind::Deref)) continue;
        CleanupEntry e;
        e.kind = CleanupEntry::Deref;
        e.slot = slot;
        e.cls = pc;
        e.path = path;
        e.path.emplace_back(it->second.llvm_name, f.index);
        e.scope_depth = depth >= 0 ? depth : ctx_.scope_depth;
        ctx_.drops.push_back(e);
    }
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

// 开启新的基本块。调用前当前块必须已经用 br/ret 终结：本函数无条件复位
// ctx_.term，若当前块尚未终结就调用，会把本该丢弃的后续指令重新发射出来，
// 而此前消耗掉的寄存器号已无法回收，最终产生悬空引用。
void Translator::open_block(const std::string& label) {
    ir_.emit(label + ":");
    ctx_.term = false;
}

// 函数体内的 keep：保留的代码落在当前基本块里，而非模块头部。
// 反引号包裹产生的首尾换行不属于代码本身，剥掉避免多出空行。
// 末行是块终结指令时本块就此结束，翻译器随后不再发射任何指令，
// 否则会在终结指令之后再追加指令，产出非法 IR
void Translator::emit_keep(std::string_view code) {
    while (!code.empty() && (code.front() == '\n' || code.front() == '\r'))
        code.remove_prefix(1);
    while (!code.empty() && (code.back() == '\n' || code.back() == '\r'))
        code.remove_suffix(1);
    if (code.empty()) return;
    ir_.emit(code);
    size_t nl = code.rfind('\n');
    std::string_view last =
        (nl == std::string_view::npos) ? code : code.substr(nl + 1);
    while (!last.empty() && (last.front() == ' ' || last.front() == '\t'))
        last.remove_prefix(1);
    if (is_terminator_mnemonic(last)) ctx_.term = true;
}

// DROP 清理

void Translator::emit_drops_to(int depth) {
    while (!ctx_.drops.empty() && ctx_.drops.back().scope_depth > depth) {
        auto& d = ctx_.drops.back();
        if (!ctx_.term) {
            if (d.kind == CleanupEntry::Restore) {
                emit_instr("call void @llvm.stackrestore(ptr " + d.ptr + ")");
            } else if (d.kind == CleanupEntry::Deref) {
                std::string obj = ir_.reg();
                emit_instr(obj + " = load ptr, ptr " + d.slot);
                std::string v = obj;
                // 逐级 getelementptr 走到目标指针字段，内联类字段的中间层
                // 是存储地址而非指针，只有最后一级需要 load
                for (size_t i = 0; i < d.path.size(); ++i) {
                    std::string f = ir_.reg();
                    emit_instr(f + " = getelementptr " + d.path[i].first + ", ptr " + v +
                               ", i32 0, i32 " + std::to_string(d.path[i].second));
                    if (i + 1 == d.path.size()) {
                        v = ir_.reg();
                        emit_instr(v + " = load ptr, ptr " + f);
                    } else {
                        v = f;
                    }
                }
                emit_guard_call(find_special(d.cls, SpecialMethod::Kind::Deref), v);
            } else {
                std::string v = ir_.reg();
                emit_instr(v + " = load ptr, ptr " + d.slot);
                emit_guard_call(find_special(d.cls, SpecialMethod::Kind::Drop), v);
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
    Type* ret_expr = nullptr;
    if (!f.returns.empty() && f.returns.back()) {
        VarList* rl = f.returns.back();
        if (!rl->vars.empty()) ret_var = rl->vars.back();
        Type* rt_ = (ret_var && ret_var->type) ? ret_var->type : rl->type;
        if (rt_) { ret_t = object_type(rt_); ret_expr = rt_; }
    }
    // main 有返回变量：不用 sret，改为局部变量 + 统一 exit 块
    bool main_with_ret = is_main && ret_var;
    bool plain_main = is_main && !ret_var && ret_t == "void";
    ctx_.plain_main = plain_main;
    // 类返回：调用方提供对象存储，callee 就地构造
    Class* ret_cls = (!main_with_ret && !f.ret_via_reg)
        ? as_class(ret_expr) : nullptr;
    if (ret_cls) {
        ctx_.ret_obj_var = ret_var;
        ctx_.ret_obj_ptr = "%ret";
        ctx_.ret_obj_cls = ret_cls;
    }
    // 引用返回变量只是把被引用对象的地址带出去的通道：给它赋值是绑定，
    // 存的是地址而非被引用对象的值
    if (ret_var && ret_expr && ret_expr->is_reference())
        ctx_.ret_ref_var = ret_var;

    std::vector<std::string> params;
    std::vector<std::tuple<VarItem*, std::string, std::string>> arg_inits;

    if (ret_var && !main_with_ret && !f.ret_via_reg) {
        if (ret_cls)
            params.push_back("ptr sret(" + classes_[ret_cls].llvm_name + ") %ret");
        else
            params.push_back("ptr sret(" + ret_t + ") %ret");
    }
    if (has_self) {
        params.push_back("ptr %self");
        ctx_.self_ptr = "%self";
    }

    int idx = 0;
    for (VarList* vl : f.params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            Type* pt = vi->type ? vi->type : vl->type;
            // 定长数组形参按值传递：IR 类型即真实的 [N x T]
            std::string vt = pt ? object_type(pt) : "i32";
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
        // 引用形参：入参即被绑定对象的地址，槽已是绑定态。
        // 必须先于下面的类指针分支，pointee_class 会解包引用 target
        if (is_ref_item(vi, nullptr)) {
            ctx_.ref_bound.insert(vi);
            continue;
        }
        // 非引用的类指针形参只是借用：不新增持有，也不登记退出时的解引用。
        // 持有关系由调用方与字段赋值各自计一次，形参重复计会把同一次持有数两遍
    }
    // static 字段与模块级变量的默认值：在任何 INIT 执行前写入
    if (is_main) {
        emit_global_inits();
        emit_static_inits();
    }
    if (main_with_ret) {
        std::string slot = ir_.reg();
        emit_instr(slot + " = alloca i32");
        ctx_.slots[ret_var] = slot;
    } else if (ret_var && f.ret_via_reg) {
        std::string slot = ir_.reg();
        emit_instr(slot + " = alloca " + ret_t);
        ctx_.slots[ret_var] = slot;
    } else if (ret_var && !ret_cls) {
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
        // 类返回变量：把初值对象逐字段拷进调用方存储，不产生新的一份持有
        if (ret_cls && rv == ret_var) {
            std::string v = eval(rv->init);
            emit_fields_copy(ret_cls, v, "%ret", false);
            continue;
        }
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
    ctx_.in_init = (sm.kind == SpecialMethod::Kind::Init);

    std::vector<std::string> params;
    params.push_back("ptr %self");
    std::vector<std::tuple<VarItem*, std::string, std::string>> arg_inits;
    int idx = 0;
    for (VarList* vl : sm.params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            Type* pt = vi->type ? vi->type : vl->type;
            // 定长数组形参按值传递：IR 类型即真实的 [N x T]
            std::string vt = pt ? object_type(pt) : "i32";
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
        // 引用形参：入参即被绑定对象的地址，槽已是绑定态
        if (is_ref_item(vi, nullptr)) {
            ctx_.ref_bound.insert(vi);
            continue;
        }
        // 非引用的类指针形参只是借用：INIT 内的字段赋值才是那次持有
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
    int idx = (it != classes_.end()) ? it->second.value_index : 0;
    if (idx < 0) idx = 0;
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
    // 当前块已终结：后续语句是死代码，不再生成。
    // 不能放任死代码进入 emit_var_decl / emit_for 等分支：那些分支照常调用
    // ir_.reg() 消耗寄存器号，但指令被 emit_instr 丢弃；随后 open_block 又
    // 无条件复位 ctx_.term 复活控制流，就会发射出引用未定义寄存器的指令。
    // Label 是 goto 跳转目标，Block 内部可能含 Label，二者必须继续处理，
    // 否则跳转目标无法重新开启新块。
    if (ctx_.term &&
        !std::holds_alternative<Stmt::Label>(s->data) &&
        !std::holds_alternative<Stmt::Block>(s->data)) {
        return;
    }
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
        } else if constexpr (std::is_same_v<T, Stmt::Label>) {
            emit_label(v);
        } else if constexpr (std::is_same_v<T, Stmt::Goto>) {
            emit_goto(v);
        } else if constexpr (std::is_same_v<T, Stmt::Keep>) {
            emit_keep(v.tokens);
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
    // malloc 可以写在类上，也可以写在变量声明上，两者都是堆分配实例
    bool is_malloc = cls->specs.has(SpecKind::Malloc) ||
                     (vl && vl->specs.has(SpecKind::Malloc));
    if (vi->init) {
        Class* src_cls = as_class(vi->init->type);
        // 初值是 INIT 调用时是构造而非拷贝：构造结果就是本实例，
        // 再逐字段复制会把引用字段的绑定目标也复制成指向临时实例
        bool ctor_call = false;
        if (auto* call = vi->init->get_call())
            ctor_call = call->resolved &&
                        call->resolved->kind == DeclKind::SpecialMethod;
        if (is_malloc) {
            // 堆存储是新的一份持有：构造调用已分配实例可直接沿用，
            // 其余初值逐字段搬进新分配的堆块
            inst = ctor_call ? eval_converted(vi->init)
                             : emit_class_copy(cls, eval_converted(vi->init), true);
        } else if (!ctor_call && src_cls == cls && !needs_convert(vi->init)) {
            // 同类初始化：拷贝构造，新建实例并逐字段复制
            inst = emit_class_copy(cls, eval(vi->init));
        } else {
            inst = eval_converted(vi->init);
        }
    } else if (is_malloc) {
        inst = emit_malloc_inst(cls);
        emit_field_defaults(cls, inst);
        emit_bind_ref(cls, inst, SpecialMethod::Kind::Ref);
    } else {
        // 变量绑定的实例存储：整个函数一次，循环里复用同一块
        inst = entry_alloca(it->second.llvm_name);
    }
    std::string slot = entry_alloca("ptr");
    emit_instr("store ptr " + inst + ", ptr " + slot);
    ctx_.slots[vi] = slot;
    if (is_malloc) {
        // 堆存储的分配与初值拷贝都在 emit_malloc_inst 内触发过一次 REF；
        // 释放由 free() 显式进行，作用域结束不解引用
        ctx_.malloc_vars.insert(vi);
    } else if (!vl->specs.has(SpecKind::Malloc)) {
        for (Decl* m : cls->members) {
            if (m && m->kind == DeclKind::SpecialMethod) {
                auto* sm = static_cast<SpecialMethod*>(m);
                if (sm->kind == SpecialMethod::Kind::Drop) {
                    CleanupEntry e;
                    e.kind = CleanupEntry::Drop;
                    e.slot = slot;      // 槽地址：按当前指针清理，兼容赋值重定向
                    e.drop_fn = mangler_.of(sm);
                    e.cls = cls;
                    e.scope_depth = ctx_.scope_depth;
                    ctx_.drops.push_back(e);
                    break;
                }
            }
        }
    }
    // 字段清理后登记：清理是栈式的，后登记者先执行，
    // 字段要在对象自身的 DROP / DEREF 之前释放
    register_field_derefs(slot, cls);
    return true;
}


// 定长数组初始化：把初值的元素写入已分配的存储
void Translator::emit_fixed_array_init(const std::string& slot, Type* el,
                                       int n, Expr* init) {
    if (!init || n <= 0) return;
    // 元素本身可能是多维数组，必须用内联存储类型；map_type 会退化成 ptr，
    // 导致元素间 GEP 的步长按指针大小算，初始化写错位置
    std::string et = el ? object_type(el) : "i32";
    // 单个元素的写入：元素本身是数组而成员是字符串字面量时按字符拷贝
    auto write_elem = [&](const std::string& dst, Expr* mem) {
        auto* lit = mem->get_literal();
        if (lit && lit->kind == LiteralKind::String && is_fixed_array(el)) {
            std::string sym = ir_.string_const(lit->value);
            copy_elements(dst, "@" + sym, el->get_array_type()->element,
                          std::to_string(std::min((size_t)array_length_of(el),
                                                   lit->value.size() + 1)));
            return;
        }
        std::string v = eval(mem);
        std::string mt = mem->type ? map_type(mem->type) : et;
        if (mt != et)
            v = cast_value(v, mt, et, !expr_is_unsigned(mem->type),
                           !expr_is_unsigned(el));
        emit_instr("store " + et + " " + v + ", ptr " + dst);
    };
    auto elem_at = [&](int k) {
        std::string p = ir_.reg();
        emit_instr(p + " = getelementptr " + et + ", ptr " + slot + ", i64 " +
                   std::to_string(k));
        return p;
    };
    // 数组字面量：逐元素写入
    if (auto* arr = init->get_array()) {
        int i = 0;
        for (Expr* mem : arr->members) {
            if (i >= n) break;
            if (mem) write_elem(elem_at(i), mem);
            ++i;
        }
        if (arr->fill_value)
            for (int k = i; k < n; ++k) write_elem(elem_at(k), arr->fill_value);
        return;
    }
    // 字符串字面量：连 NUL 一起拷贝，放得下几个拷几个
    if (auto* lit = init->get_literal()) {
        if (lit->kind == LiteralKind::String) {
            std::string sym = ir_.string_const(lit->value);
            copy_elements(slot, "@" + sym, el,
                          std::to_string(std::min((size_t)n,
                                                   lit->value.size() + 1)));
            return;
        }
    }
    // 指针：按目标长度拷贝
    if (init->type && init->type->is_pointer()) {
        copy_elements(slot, eval(init), el, std::to_string(n));
        return;
    }
    // 数组 / 切片：按两者长度的较小值拷贝
    copy_init_into(slot, el, std::to_string(n), init);
}

// 按目标数组类型求值：字面量与字符串按目标的元素类型构造，避免元素类型不符
std::string Translator::eval_as_array(Expr* e, Type* target) {
    std::string ot = object_type(target);
    Type* el = target->get_array_type()->element;
    int n = array_length_of(target);
    std::string tmp = entry_alloca(ot);
    if (e && e->is_array())
        emit_fixed_array_init(tmp, el, n, e);
    else
        copy_init_into(tmp, el, std::to_string(n), e);
    std::string v = ir_.reg();
    emit_instr(v + " = load " + ot + ", ptr " + tmp);
    return v;
}

// 所有权切片初始化：按长度分配存储，拷贝初值内容，存入 {ptr,len}
void Translator::emit_slice_init(VarList* vl, const std::string& slot,
                                 Type* vtype, Expr* init) {
    Type* el = vtype->get_array_type()->element;
    // 长度：类型里的 {n} 优先，否则取初值的长度
    Expr* len_expr = vtype->get_array_type()->length;
    std::string len;
    if (len_expr) len = len_expr->is_literal()
        ? std::string(len_expr->get_literal()->value) : eval(len_expr);
    else if (init) len = length_value_of(init);
    if (len.empty()) len = "0";
    std::string len64 = len;
    std::string len32 = len;
    if (!len.empty() && len[0] == '%') {
        std::string lt = len_expr && len_expr->type
            ? map_type(len_expr->type) : "i32";
        bool lu = len_expr ? expr_is_unsigned(len_expr->type) : true;
        if (lt == "i64") {
            std::string t = ir_.reg();
            emit_instr(t + " = trunc i64 " + len + " to i32");
            len32 = t;
        } else {
            std::string t = ir_.reg();
            emit_instr(t + " = " + std::string(lu ? "zext " : "sext ") + lt + " " +
                       len + " to i64");
            len64 = t;
        }
    }
    // 存储：常量长度放入口块（整函数一次，循环里复用同一块）；
    // 运行期长度在栈上分配，并用 stacksave/stackrestore 在作用域结束时回收；
    // malloc 修饰的切片走堆，不参与自动回收
    bool heap = vl && vl->specs.has(SpecKind::Malloc);
    std::string et = el ? map_type(el) : "i32";
    long long fix_len = 0;
    bool const_len = const_count(len64, fix_len);
    std::string backing;
    if (!heap && const_len)
        backing = entry_alloca(et + ", i64 " + len64);
    else if (heap) {
        long esz = type_bytes_of(et);
        std::string bytes = len64;
        if (esz != 1) {
            std::string b = ir_.reg();
            emit_instr(b + " = mul i64 " + len64 + ", " + std::to_string(esz));
            bytes = b;
        }
        ir_.need_malloc();
        backing = ir_.reg();
        emit_instr(backing + " = call ptr @malloc(i64 " + bytes + ")");
    } else {
        backing = alloc_slice_storage(el, len64);
    }
    // 字面量初值直接写进新分配的存储，省掉中间那份临时 backing
    long long lit_len = 0;
    bool literal = init && init->get_array() && const_count(len64, lit_len);
    if (literal)
        emit_fixed_array_init(backing, el, (int)lit_len, init);
    else
        copy_init_into(backing, el, len64, init);
    std::string s1 = ir_.reg();
    emit_instr(s1 + " = insertvalue { ptr, i32 } undef, ptr " + backing + ", 0");
    std::string s2 = ir_.reg();
    emit_instr(s2 + " = insertvalue { ptr, i32 } " + s1 + ", i32 " + len32 + ", 1");
    emit_instr("store { ptr, i32 } " + s2 + ", ptr " + slot);
}

void Translator::emit_var_decl(VarList* vl) {
    if (!vl) return;
    std::string t = map_type(vl->type);
    for (VarItem* vi : vl->vars) {
        if (!vi) continue;
        Type* vtype = vi->type ? vi->type : vl->type;
        // 引用声明：分配槽存被引用对象地址；有初值则立即绑定
        if (vtype && vtype->is_reference()) {
            std::string slot = entry_alloca("ptr");
            ctx_.slots[vi] = slot;
            if (vi->init) {
                // 引用一律存被引用对象的地址：类实例变量要取出槽里的实例
                // 指针，直接用 eval_lvalue 会存成槽地址，读时多绕一层错位
                // 绑定源写成 &lvalue 时取其 lvalue，与语义检查绑定的对象一致
                std::string addr = eval_ref_addr(vi->init->bind_source());
                if (addr.empty()) addr = eval(vi->init);
                emit_instr("store ptr " + addr + ", ptr " + slot);
                ctx_.ref_bound.insert(vi);
            }
            continue;
        }
        Class* cls = as_class(vtype);
        if (cls && emit_class_var(vl, vi, cls)) continue;
        std::string vt = vtype ? map_type(vtype) : t;
        // 定长数组：槽即数组本体（[N x T]），初值直接写进这块存储
        if (is_fixed_array(vtype)) {
            std::string slot = entry_alloca(object_type(vtype));
            ctx_.slots[vi] = slot;
            if (vi->init)
                emit_fixed_array_init(slot, vtype->get_array_type()->element,
                                      array_length_of(vtype), vi->init);
            continue;
        }
        // 所有权切片：槽存 {ptr,len}，存储按长度另行分配
        if (is_slice_type(vtype)) {
            std::string slot = entry_alloca("{ ptr, i32 }");
            ctx_.slots[vi] = slot;
            emit_slice_init(vl, slot, vtype, vi->init);
            continue;
        }
        std::string slot = entry_alloca(vt);
        ctx_.slots[vi] = slot;
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

    // 分支各自登记自己的清理；一条路径以 break/return 收尾时它已把清理发掉，
    // 翻译下一条路径前要整份还原登记栈，否则兄弟路径漏释放
    std::vector<CleanupEntry> drops_saved = ctx_.drops;
    open_block(then_l);
    ++ctx_.scope_depth;
    emit_stmt(i.then_branch);
    --ctx_.scope_depth;
    if (!ctx_.term) emit_drops_to(ctx_.scope_depth);
    if (!ctx_.term) emit_term("br label %" + end_l);
    ctx_.drops = drops_saved;

    open_block(else_l);
    ++ctx_.scope_depth;
    if (i.else_branch) emit_stmt(i.else_branch);
    --ctx_.scope_depth;
    if (!ctx_.term) emit_drops_to(ctx_.scope_depth);
    if (!ctx_.term) emit_term("br label %" + end_l);
    ctx_.drops = drops_saved;

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
    // 无返回变量的 main 签名为 i32，裸 return 按返回 0 处理
    if (ctx_.plain_main) {
        emit_term("ret i32 0");
        return;
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

void Translator::emit_label(Stmt::Label& f) {
    std::string tgt = "label." + std::string(f.label);
    if (!ctx_.term) emit_term("br label %" + tgt);
    open_block(tgt);
}

void Translator::emit_goto(Stmt::Goto& g) {
    std::string tgt = "label." + std::string(g.label);
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
    if (is_slice_type(e->type)) {
        std::string n = ir_.reg();
        emit_instr(n + " = extractvalue { ptr, i32 } " + v + ", 1");
        std::string r = ir_.reg();
        emit_instr(r + " = icmp ne i32 " + n + ", 0");
        return r;
    }
    // 定长数组恒有存储，条件恒为真
    if (is_fixed_array(e->type)) return "1";
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
    if (auto* sp  = e->get_spawn())       return eval_spawn(e, sp);
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

Class* Translator::as_wrap_class(Type* t) {
    Class* c = as_class(t);
    if (!c) return nullptr;
    if (c->base_source == PrimitiveTypeKind::Void) return nullptr;
    return c;
}

// 逐字段拷贝 src 所指对象到 dst 所指对象
void Translator::emit_fields_copy(Class* cls, const std::string& src,
                                  const std::string& dst, bool copy_holds) {
    auto it = classes_.find(cls);
    if (it == classes_.end()) return;
    const std::string& cls_ir = it->second.llvm_name;
    // extern "LLVMIR" 的类布局由 keep`` 给出：clux 侧没有字段表，逐字段搬
    // 会一个字节都不拷，目标存储保持未初始化。这种类的值按结构体本身收发，
    // 与 extern 聚合实参/返回值的处理一致（见 eval_plain_call）
    if (it->second.from_keep_ir) {
        if (src.empty() || dst.empty()) return;
        std::string v = ir_.reg();
        emit_instr(v + " = load " + cls_ir + ", ptr " + src);
        emit_instr("store " + cls_ir + " " + v + ", ptr " + dst);
        return;
    }
    for (auto& f : it->second.fields) {
        std::string sa = ir_.reg();
        emit_instr(sa + " = getelementptr " + cls_ir + ", ptr " + src +
                   ", i32 0, i32 " + std::to_string(f.index));
        std::string da = ir_.reg();
        emit_instr(da + " = getelementptr " + cls_ir + ", ptr " + dst +
                   ", i32 0, i32 " + std::to_string(f.index));
        // 内联类字段：字段存储即嵌套对象，整体搬进目标存储，
        // 其内部的指针字段持有要递归计入，故下沉一层而不是按 ptr 拷
        if (Class* fc = as_class(f.type)) {
            emit_fields_copy(fc, sa, da, copy_holds);
            continue;
        }
        std::string sv = ir_.reg();
        emit_instr(sv + " = load " + f.ir_type + ", ptr " + sa);
        emit_instr("store " + f.ir_type + " " + sv + ", ptr " + da);
        // 复制出新的一份持有时指针字段才各计一次 REF；把已构造好的对象搬进
        // 另一处存储（返回值落进调用方 sret 槽）不产生新持有，计一次即多一次
        if (copy_holds && f.type && f.type->is_pointer())
            emit_bind_ref(pointee_class(f.type), sv,
                          SpecialMethod::Kind::Ref);
    }
}

// 字段是否有需要写入的声明处默认值：无初值的引用字段写 null，
// 使未绑定时的读取确定性地失败而非读到垃圾。内联类字段即使自身无初值，
// 也要为嵌套对象写入它自己的字段默认值
bool Translator::field_has_default(const TranslatorField& f) {
    if (!f.var) return false;
    if (f.var->type && f.var->type->is_reference()) return true;
    if (as_class(f.type)) return true;
    return f.var->init != nullptr;
}

// 写入单个字段的声明处默认值：引用字段的初值即绑定点，存被引用对象的地址
void Translator::emit_field_default(const std::string& dst_addr,
                                   const std::string& inst_addr, Class* cls,
                                   const TranslatorField& f) {
    if (!f.var) return;
    if (f.var->type && f.var->type->is_reference()) {
        // 引用字段的初值即绑定点，存被引用对象的地址。
        // 没有初值的字段靠 INIT 绑定，构造开始前写 null：
        // INIT 绑定语句之前就读取会确定性地失败而非读到垃圾
        if (!f.var->init) {
            emit_instr("store ptr null, ptr " + dst_addr);
            return;
        }
        // 绑定源是本类的另一个字段时，初值求值处没有接收者表达式可依，
        // 按字段下标从实例存储算它的地址
        Expr* src = f.var->init->bind_source();
        std::string raddr = field_addr_in(cls, inst_addr, src);
        if (raddr.empty()) raddr = eval_ref_addr(src);
        if (raddr.empty()) raddr = eval(src);
        emit_instr("store ptr " + raddr + ", ptr " + dst_addr);
        return;
    }
    // 内联类字段：字段存储即嵌套对象，构造就地写进这块存储，
    // 不会像指针字段那样在构造帧退出后悬垂
    if (Class* fc = as_class(f.type)) {
        emit_field_defaults(fc, dst_addr);
        if (!f.var->init) return;
        auto* call = f.var->init->get_call();
        if (call && call->resolved &&
            call->resolved->kind == DeclKind::SpecialMethod) {
            emit_ctor_into(call, static_cast<SpecialMethod*>(call->resolved),
                           fc, dst_addr);
            return;
        }
        bool conv = needs_convert(f.var->init);
        std::string src = conv ? eval_converted(f.var->init)
                                : eval(f.var->init);
        emit_fields_copy(fc, src, dst_addr, true);
        return;
    }
    if (!f.var->init) return;
    // 定长数组成内联字段：初值元素直接写进字段存储
    if (is_fixed_array(f.var->type)) {
        Type* el = f.var->type->get_array_type()->element;
        emit_fixed_array_init(dst_addr, el, array_length_of(f.var->type),
                              f.var->init);
        return;
    }
    bool converted = needs_convert(f.var->init);
    std::string v = converted ? eval_converted(f.var->init) : eval(f.var->init);
    std::string st = f.var->init->type ? map_type(f.var->init->type) : f.ir_type;
    if (!converted && st != f.ir_type)
        v = cast_value(v, st, f.ir_type, !expr_is_unsigned(f.var->init->type),
                       !expr_is_unsigned(f.var->type));
    emit_instr("store " + f.ir_type + " " + v + ", ptr " + dst_addr);
}

// 模块入口为所有 static 字段写入声明处默认值
void Translator::emit_static_inits() {
    // 只写本模块声明的类：static 字段的默认值由声明它的模块负责一次
    for (Class* cls : own_classes_) {
        auto it = classes_.find(cls);
        if (it == classes_.end()) continue;
        for (auto& f : it->second.static_fields) {
            if (!f.var || !field_has_default(f)) continue;
            emit_field_default("@" + mangler_.field_of(f.var), "", nullptr, f);
        }
    }
}

// 模块入口为所有模块级变量写入声明处默认值。
// 初值在main 入口求值而非写成 LLVM 常量初始化式，因此可以引用其他变量、
// 调用函数；代价是 main 之前这些全局量都是零值
void Translator::emit_global_inits() {
    for (auto& g : globals_) {
        if (!g.var || g.is_extern || g.skip_emit) continue;
        if (!g.var->init) {
            // 标量、指针与定长数组的零初始化存储已是完整初值；类值的全局槽
            // 里存的是实例指针，零值即 null，须在运行期建出对象再写入
            if (Class* cls = as_class(g.var->type)) {
                std::string slot = emit_default_object(cls);
                if (slot != "0")
                    emit_instr("store ptr " + slot + ", ptr @" + g.sym);
            }
            continue;
        }
        if (g.var->type && g.var->type->is_reference()) {
            std::string addr = eval_ref_addr(g.var->init->bind_source());
            if (addr.empty()) addr = "null";
            emit_instr("store ptr " + addr + ", ptr @" + g.sym);
            continue;
        }
        bool converted = needs_convert(g.var->init);
        std::string v = converted ? eval_converted(g.var->init)
                                  : eval(g.var->init);
        std::string st = g.var->init->type ? map_type(g.var->init->type)
                                           : g.ir_type;
        if (!converted && st != g.ir_type)
            v = cast_value(v, st, g.ir_type, !expr_is_unsigned(g.var->init->type),
                           !expr_is_unsigned(g.var->type));
        emit_instr("store " + g.ir_type + " " + v + ", ptr @" + g.sym);
    }
}

// 构造时为实例字段写入声明处默认值（INIT body 之前）
void Translator::emit_field_defaults(Class* cls, const std::string& slot) {
    auto it = classes_.find(cls);
    if (it == classes_.end()) return;
    for (auto& f : it->second.fields) {
        if (!field_has_default(f)) continue;
        std::string addr = ir_.reg();
        emit_instr(addr + " = getelementptr " + it->second.llvm_name +
                   ", ptr " + slot + ", i32 0, i32 " + std::to_string(f.index));
        emit_field_default(addr, slot, cls, f);
    }
}

// 隐式包装/解包：源或目标为包装类，先解包到承载类型，数值 cast，再打包
std::string Translator::emit_class_copy(Class* cls, const std::string& src, bool heap) {
    auto it = classes_.find(cls);
    std::string cls_ir = (it != classes_.end()) ? it->second.llvm_name
                                                 : "%" + mangler_.of(cls);
    std::string dst = heap ? emit_malloc_inst(cls) : entry_alloca(cls_ir);
    emit_fields_copy(cls, src, dst, true);
    // 堆存储是新的一份持有：字段就位之后才引用，否则 REF 改的计数会被拷贝覆盖
    if (heap) emit_bind_ref(cls, dst, SpecialMethod::Kind::Ref);
    return dst;
}

// malloc 实例：堆上一块实例存储
std::string Translator::emit_malloc_inst(Class* cls) {
    auto it = classes_.find(cls);
    std::string cls_ir = (it != classes_.end()) ? it->second.llvm_name
                                                 : "%" + mangler_.of(cls);
    ir_.need_malloc();
    std::string dst = ir_.reg();
    emit_instr(dst + " = call ptr @malloc(i64 " + emit_size_of(cls_ir) + ")");
    return dst;
}

std::string Translator::emit_wrap_convert(Expr* e) {
    Type* src_t = e->type;
    Type* dst_t = e->implicit_to;
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
        int idx = (it != classes_.end()) ? it->second.value_index : 0;
        if (idx < 0) idx = 0;
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

    Type* target_t = dw ? nullptr : dst_t;
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
        int idx = (it != classes_.end()) ? it->second.value_index : 0;
        if (idx < 0) idx = 0;
        std::string slot;
        if (dw->specs.has(SpecKind::Malloc)) {
            ir_.need_malloc();
            slot = ir_.reg();
            emit_instr(slot + " = call ptr @malloc(i64 " +
                       emit_size_of(cls_ir) + ")");
            emit_bind_ref(dw, slot, SpecialMethod::Kind::Ref);
        } else {
            slot = entry_alloca(cls_ir);
        }
        std::string addr = ir_.reg();
        emit_instr(addr + " = getelementptr " + cls_ir + ", ptr " + slot +
                   ", i32 0, i32 " + std::to_string(idx));
        emit_instr("store " + dst_ir + " " + v + ", ptr " + addr);
        return slot;
    }
    return v;
}

// 单个元素的字节数：类型名已知且是定长标量/指针，可直接算出
long Translator::type_bytes_of(const std::string& t) {
    if (t == "i1" || t == "i8") return 1;
    if (t == "i16" || t == "half") return 2;
    if (t == "i32" || t == "float") return 4;
    if (t == "i64" || t == "double" || t == "ptr") return 8;
    if (t == "i128") return 16;
    if (t == "{ ptr, i32 }") return 16;
    // 定长数组 [N x T]：字节数按元素累加
    if (t.size() > 5 && t.front() == '[' && t.back() == ']') {
        size_t x = t.find(" x ");
        if (x != std::string::npos) {
            long n = std::stol(t.substr(1, x - 1));
            return n * type_bytes_of(t.substr(x + 3, t.size() - x - 4));
        }
    }
    return 8;
}

// 实例字节数：交给 LLVM 用常量表达式算出真实布局（含聚合类型与对齐），
// 手写字段分派无法覆盖任意聚合类型且会漏掉对齐填充。
// 直接内联成常量表达式，不必为每个类发射尺寸全局
std::string Translator::emit_size_of(const std::string& llvm_name) {
    return "ptrtoint (ptr getelementptr (" + llvm_name +
           ", ptr null, i32 1) to i64)";
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

// 成员表达式的存储地址：static 字段为模块级全局符号，实例字段为 getelementptr
std::string Translator::field_addr_of(Expr* e) {
    if (!e || !e->sym) return "";
    auto* vi = dynamic_cast<VarItem*>(e->sym->node);
    if (!vi) return "";
    // static 字段：模块级全局符号，由 Mangler 统一命名
    std::string sg = mangler_.field_of(vi);
    if (!sg.empty()) return "@" + sg;
    // 模块级变量
    auto git = global_names_.find(vi);
    if (git != global_names_.end()) return "@" + git->second;
    // 实例字段：需要成员表达式的接收者
    auto* m = e->get_member();
    if (!m) return "";
    Class* cls = as_class(m->target ? m->target->type : nullptr);
    if (!cls) return "";
    auto it = classes_.find(cls);
    if (it == classes_.end()) return "";
    auto fit = it->second.field_index.find(vi);
    if (fit == it->second.field_index.end()) return "";
    std::string base = eval(m->target);
    std::string addr = ir_.reg();
    emit_instr(addr + " = getelementptr " + it->second.llvm_name + ", ptr " + base +
               ", i32 0, i32 " + std::to_string(fit->second));
    return addr;
}

// 字段初值绑定到本类的另一个字段：初值求值处没有接收者表达式，
// 按字段下标从实例存储算出被绑定字段的地址
std::string Translator::field_addr_in(Class* cls, const std::string& inst_addr,
                                       Expr* src) {
    if (!cls || inst_addr.empty() || !src || !src->sym) return "";
    if (src->sym->kind != SymbolKind::Variable) return "";
    auto* sv = dynamic_cast<VarItem*>(src->sym->node);
    if (!sv) return "";
    auto it = classes_.find(cls);
    if (it == classes_.end()) return "";
    auto fit = it->second.field_index.find(sv);
    if (fit == it->second.field_index.end()) return "";
    std::string addr = ir_.reg();
    emit_instr(addr + " = getelementptr " + it->second.llvm_name +
               ", ptr " + inst_addr + ", i32 0, i32 " + std::to_string(fit->second));
    if (!sv->type || !sv->type->is_reference()) return addr;
    std::string loaded = ir_.reg();
    emit_instr(loaded + " = load ptr, ptr " + addr);
    return loaded;
}

// 引用字段的槽存被引用对象的地址，取其 lvalue 需先取出该地址
std::string Translator::field_lvalue(Expr* e, const std::string& slot_addr) {
    Type* dt = e && e->sym ? e->sym->get_type() : nullptr;
    if (slot_addr.empty() || !dt || !dt->is_reference()) return slot_addr;
    std::string addr = ir_.reg();
    emit_instr(addr + " = load ptr, ptr " + slot_addr);
    return addr;
}

// 引用字段的目标是类：取出被引用对象的地址后，字段的值即该地址本身。
// 与 eval_identifier 的 ref_bound 分支一致，此时不能再按 map_type 做一次 load，
// 否则会把实例存储当成指针槽再读一次。
static bool ref_targets_class(Type* decl_type) {
    auto* r = decl_type ? decl_type->get_reference() : nullptr;
    return r && as_class(r->target);
}

// 字段读取：定长数组、内联类字段、引用字段的类目标的值都是存储地址，无需 load
std::string Translator::eval_field_value(Expr* e, const std::string& slot_addr) {
    std::string addr = field_lvalue(e, slot_addr);
    if (is_fixed_array(e->type)) return addr;
    // 内联类字段：字段存储就是对象本身
    if (as_class(e->type)) return addr;
    if (ref_targets_class(e->sym ? e->sym->get_type() : nullptr)) return addr;
    std::string t = e->type ? map_type(e->type) : "i32";
    std::string r = ir_.reg();
    emit_instr(r + " = load " + t + ", ptr " + addr);
    return r;
}

// 引用字段绑定：见 bind_ref_field。
// 引用字段的绑定：仅 INIT 内对字段的直接赋值是绑定，其余位置都是写穿。
// 局部引用与引用形参都在创建时已绑定（声明处 / 进入函数时），不经过这里
std::string Translator::bind_ref_field(Expr* target, Expr* value) {
    if (!ctx_.in_init || !target || !target->sym) return "";
    auto* vi = dynamic_cast<VarItem*>(target->sym->node);
    if (!vi || !is_ref_item(vi, nullptr)) return "";
    std::string dst = field_addr_of(target);
    if (dst.empty()) return "";
    std::string addr = eval_ref_addr(value);
    if (addr.empty()) addr = eval(value);
    emit_instr("store ptr " + addr + ", ptr " + dst);
    return addr;
}

// 被引用对象的地址：引用绑定、引用形参传参、取地址都走这里。
// 与 eval_lvalue 的唯一分歧是类值——类实例变量的槽、模块级变量与 static
// 字段存的都只是实例指针，作为被引用对象时要取出该指针而非槽地址
std::string Translator::eval_ref_addr(Expr* e) {
    if (e && e->sym && e->sym->kind == SymbolKind::Variable && as_class(e->type)) {
        if (auto* vi = dynamic_cast<VarItem*>(e->sym->node)) {
            std::string g;
            auto it = ctx_.slots.find(vi);
            if (it != ctx_.slots.end() && !ctx_.ref_bound.count(vi))
                g = it->second;
            else {
                auto git = global_names_.find(vi);
                if (git != global_names_.end()) g = "@" + git->second;
                else {
                    std::string sg = mangler_.field_of(vi);
                    if (!sg.empty()) g = "@" + sg;
                }
            }
            if (!g.empty()) {
                std::string addr = ir_.reg();
                emit_instr(addr + " = load ptr, ptr " + g);
                return addr;
            }
        }
    }
    return eval_lvalue(e);
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
                // 模块级变量：其存储本身就是全局量，地址即该符号
                auto git = global_names_.find(vi);
                if (git != global_names_.end()) {
                    // 模块级引用的 lvalue 是被引用对象：槽存其地址
                    if (is_ref_item(vi, nullptr)) {
                        std::string addr = ir_.reg();
                        emit_instr(addr + " = load ptr, ptr @" + git->second);
                        return addr;
                    }
                    return "@" + git->second;
                }
                // static 字段：模块级全局符号
                std::string sg = mangler_.field_of(vi);
                if (!sg.empty()) return field_lvalue(e, "@" + sg);
                if (ctx_.current_class) {
                    auto cit = classes_.find(ctx_.current_class);
                    if (cit != classes_.end()) {
                        auto fit = cit->second.field_index.find(vi);
                        if (fit != cit->second.field_index.end()) {
                            std::string addr = ir_.reg();
                            emit_instr(addr + " = getelementptr " + cit->second.llvm_name +
                                       ", ptr " + ctx_.self_ptr + ", i32 0, i32 " +
                                       std::to_string(fit->second));
                            return field_lvalue(e, addr);
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
            bool iu = expr_is_unsigned(ix->index ? ix->index->type : nullptr);
            emit_instr(ext + " = " + std::string(iu ? "zext " : "sext ") + it + " " +
                       idx + " to i64");
            idx = ext;
        }
        // 元素是多维数组时必须用内联存储类型，map_type 会退化成 ptr
        std::string et = object_type(e->type);
        std::string addr = ir_.reg();
        emit_instr(addr + " = getelementptr " + et + ", ptr " + base + ", i64 " + idx);
        return addr;
    }
    if (e->sym && e->sym->kind == SymbolKind::Variable)
        return field_lvalue(e, field_addr_of(e));
    return "";
}

std::string Translator::data_ptr_of(Expr* e) {
    if (!e || !e->type) return "null";
    if (e->type->is_pointer()) return eval(e);
    // 定长数组的值即存储地址，也就是首元素地址
    if (is_fixed_array(e->type)) return eval(e);
    if (is_slice_type(e->type)) {
        std::string s = eval(e);
        std::string r = ir_.reg();
        emit_instr(r + " = extractvalue { ptr, i32 } " + s + ", 0");
        return r;
    }
    return "null";
}

// LLVM 的浮点常量只认「能被目标类型精确表示」的值：float/half 写十进制
// 小数（1.01f、1.1h）一律报 floating point constant invalid for type，
// 指数形式若没有小数点还会被词法当成整数。十六进制形式则必须是 16 位
// double 位模式。故三种浮点字面量统一写成 double 位模式，与 clang 的产物同形。
static std::string fp_bits_hex(double d) {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    static const char* kHex = "0123456789ABCDEF";
    std::string out = "0x";
    for (int i = 15; i >= 0; --i) out += kHex[(bits >> (i * 4)) & 0xF];
    return out;
}

// 单精度按就近偶数舍入到半精度，返回 16 位位模式
static uint16_t f32_to_f16_bits(float f) {
    uint32_t x = 0;
    std::memcpy(&x, &f, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t raw_exp = (x >> 23) & 0xFFu;
    uint32_t mant = x & 0x7FFFFFu;
    if (raw_exp == 0xFF) {   // inf / nan
        return (uint16_t)(sign | 0x7C00u |
                          (mant ? (mant >> 13) | 0x200u : 0u));
    }
    int exp = (int)raw_exp - 127 + 15;
    if (exp >= 0x1F) return (uint16_t)(sign | 0x7C00u);   // 溢出即 inf
    if (exp <= 0) {       // 规格化前的次正规数，含下溢到 0
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t half = mant >> shift;
        uint32_t rem = mant & ((1u << shift) - 1);
        uint32_t mid = 1u << (shift - 1);
        if (rem > mid || (rem == mid && (half & 1))) ++half;
        return (uint16_t)(sign | half);
    }
    uint32_t half = ((uint32_t)exp << 10) | (mant >> 13);
    uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1))) ++half;
    return (uint16_t)(sign | half);
}

static double f16_bits_to_double(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    double d;
    if (exp == 0) {
        d = std::ldexp((double)mant, -24);
    } else if (exp == 0x1F) {
        d = mant ? std::numeric_limits<double>::infinity()
                 : std::numeric_limits<double>::quiet_NaN();
    } else {
        d = std::ldexp((double)(mant + 0x400u), (int)exp - 25);
    }
    return sign ? -d : d;
}

// 词法把类型后缀并进 token 文本，解析前剥掉末尾一个 f/d/h；
// 良构的浮点文本末尾必是数字（十六进制浮点的尾数与指数都是十六进制数位），
// 故无条件剥一个后缀字符是安全的
static bool parse_fp_literal(std::string_view text, float& out) {
    if (!text.empty()) {
        char c = text.back();
        if (c == 'f' || c == 'F' || c == 'd' || c == 'D' ||
            c == 'h' || c == 'H')
            text.remove_suffix(1);
    }
    // from_chars 要求吃满整个文本，且其浮点实现不收十六进制形式（0x1.8p3），
    // 那种字面量退回 strtod
    const char* b = text.data();
    const char* e = b + text.size();
    if (text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        std::string tmp(text);
        char* end = nullptr;
        double d = std::strtod(tmp.c_str(), &end);
        if (end != tmp.c_str() + tmp.size()) return false;
        out = (float)d;
        return true;
    }
    auto res = std::from_chars(b, e, out);
    return res.ec == std::errc() && res.ptr == e;
}

// LLVM 不接受类型后缀与 0x/0b/0o 进制前缀，整数须转成十进制文本
static std::string literal_ir_text(LiteralKind kind, std::string_view text) {
    switch (kind) {
        case LiteralKind::Int8: case LiteralKind::Short: case LiteralKind::Int:
        case LiteralKind::Long: case LiteralKind::Int128:
        case LiteralKind::UInt8: case LiteralKind::UShort: case LiteralKind::UInt:
        case LiteralKind::ULong: case LiteralKind::UInt128: {
            std::string dec;
            if (parse_int_literal(text, dec)) return dec;
            return std::string(text);
        }
        case LiteralKind::Float16: case LiteralKind::Float:
        case LiteralKind::Double: {
            float f = 0.0f;
            if (parse_fp_literal(text, f)) {
                if (kind == LiteralKind::Float16)
                    return fp_bits_hex(f16_bits_to_double(f32_to_f16_bits(f)));
                return fp_bits_hex((double)f);
            }
            return std::string(text);
        }
        default:
            return std::string(text);
    }
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
            return literal_ir_text(lit->kind, lit->value);
    }
}

bool Translator::is_ref_item(const VarItem* vi, Type** target_out) const {
    if (!vi || !vi->type) return false;
    if (auto* r = vi->type->get_reference()) {
        if (target_out) *target_out = r->target;
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
            // 类返回变量：值即调用方提供的对象存储指针
            if (vi == ctx_.ret_obj_var && !ctx_.ret_obj_ptr.empty())
                return ctx_.ret_obj_ptr;
            auto it = ctx_.slots.find(vi);
            if (it != ctx_.slots.end()) {
                // 引用变量：槽存被引用对象地址，需二次加载
                if (ctx_.ref_bound.count(vi)) {
                    std::string addr = ir_.reg();
                    emit_instr(addr + " = load ptr, ptr " + it->second);
                    // 类值即对象地址本身，取出地址即为该引用的值
                    if (as_class(e->type)) return addr;
                    // 定长数组的存储地址在被引用对象处
                    if (is_fixed_array(e->type)) return addr;
                    std::string t = e->type ? map_type(e->type) : "i32";
                    std::string r = ir_.reg();
                    emit_instr(r + " = load " + t + ", ptr " + addr);
                    return r;
                }
                // 定长数组：值即槽地址
                if (is_fixed_array(e->type)) return it->second;
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
            // 模块级变量：按 VarItem 身份判定，与同名的类字段区分
            auto git = global_names_.find(vi);
            if (git != global_names_.end()) {
                if (is_fixed_array(e->type)) return "@" + git->second;
                // 模块级引用的槽存被引用对象地址，与局部引用已绑定时同一路径
                if (is_ref_item(vi, nullptr)) {
                    std::string addr = ir_.reg();
                    emit_instr(addr + " = load ptr, ptr @" + git->second);
                    // 类值即对象地址本身，定长数组的存储在被引用对象处
                    if (as_class(e->type) || is_fixed_array(e->type)) return addr;
                    std::string rt = e->type ? map_type(e->type) : "i32";
                    std::string rr = ir_.reg();
                    emit_instr(rr + " = load " + rt + ", ptr " + addr);
                    return rr;
                }
                std::string t = e->type ? map_type(e->type) : "i32";
                std::string r = ir_.reg();
                emit_instr(r + " = load " + t + ", ptr @" + git->second);
                return r;
            }
            if (ctx_.current_class) {
                auto cit = classes_.find(ctx_.current_class);
                if (cit != classes_.end()) {
                    // static 字段：模块级全局符号
                    std::string sg = mangler_.field_of(vi);
                    if (!sg.empty()) return eval_field_value(e, "@" + sg);
                    auto fit = cit->second.field_index.find(vi);
                    if (fit != cit->second.field_index.end()) {
                        std::string addr = ir_.reg();
                        emit_instr(addr + " = getelementptr " + cit->second.llvm_name +
                                   ", ptr " + ctx_.self_ptr + ", i32 0, i32 " +
                                   std::to_string(fit->second));
                        return eval_field_value(e, addr);
                    }
                }
            }
        }
    }
    // 走到这里说明该变量没有已登记的存储：语义已通过，翻译器却无法定位它。
    // 静默返回 0 会把变量引用变成空指针，必须报错而不是掩盖。
    if (e->sym && e->sym->kind == SymbolKind::Variable)
        error(e->loc, "internal: no storage found for variable '" +
                      std::string(e->get_identifier()
                                      ? e->get_identifier()->name
                                      : std::string_view("<unknown>")) +
                      "'");
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

    // 逻辑与/或：短路求值，右操作数仅在必要时求值
    if (b->op == OpKind::And || b->op == OpKind::Or) {
        bool is_and = (b->op == OpKind::And);
        std::string l = to_bool(b->left, eval_operand(b->left));
        std::string slot = entry_alloca("i1");
        emit_instr("store i1 " + l + ", ptr " + slot);
        std::string rhs_l = ir_.named_label("logic.rhs");
        std::string end_l = ir_.named_label("logic.end");
        // and 左为假、or 左为真时短路，直接跳到出口
        emit_term("br i1 " + l + ", label %" + (is_and ? rhs_l : end_l) +
                  ", label %" + (is_and ? end_l : rhs_l));
        open_block(rhs_l);
        std::string rr = to_bool(b->right, eval_operand(b->right));
        emit_instr("store i1 " + rr + ", ptr " + slot);
        emit_term("br label %" + end_l);
        open_block(end_l);
        std::string res = ir_.reg();
        emit_instr(res + " = load i1, ptr " + slot);
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
        // 类值的槽与字段存的都是实例地址，取地址统一走 eval_ref_addr：
        // 类实例变量由它取出槽里的指针，内联类字段直接就是字段存储地址
        return eval_ref_addr(u->operand);
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
    // 类返回变量：r = v 把 v 所指对象逐字段拷进调用方存储，不改 r 的指针。
    // 这只是把已构造好的对象搬进调用方的存储，不产生新的一份持有
    if (a->op == OpKind::Assign && a->target && a->target->sym &&
        !ctx_.ret_obj_ptr.empty()) {
        if (auto* vi = dynamic_cast<VarItem*>(a->target->sym->node)) {
            if (vi == ctx_.ret_obj_var) {
                std::string v = eval(a->value);
                emit_fields_copy(ctx_.ret_obj_cls, v, ctx_.ret_obj_ptr, false);
                return v;
            }
        }
    }
    // 复合赋值操作符重载：t += v 派发到 operator+=
    if (a->op != OpKind::Assign && e && e->sym &&
        e->sym->kind == SymbolKind::Function) {
        if (Decl* d = e->sym->get_decl()) {
            if (d->kind == DeclKind::Operator)
                return emit_operator_call(e, static_cast<Operator*>(d),
                                          a->target, a->value);
        }
    }
    // INIT 内对引用字段的赋值是绑定
    if (std::string bound = bind_ref_field(a->target, a->value); !bound.empty())
        return bound;
    std::string ptr = eval_lvalue(a->target);
    if (ptr.empty()) return "0";
    // 引用返回变量：赋值即绑定，存被引用对象的地址（与引用声明同一路径），
    // 否则存进去的是对象的值，返回出去就是一个被当作地址的数值。
    // 必须先于下面的类赋值：引用返回变量的表达式类型是目标类，
    // as_class 同样为真，会被误当成值拷贝
    if (a->target->sym && a->target->sym->node == ctx_.ret_ref_var) {
        std::string addr = eval_ref_addr(a->value);
        if (addr.empty()) addr = eval(a->value);
        emit_instr("store ptr " + addr + ", ptr " + ptr);
        return addr;
    }
    // 延迟绑定的局部引用：槽里还没有地址，这次赋值就是绑定，存被引用对象的
    // 地址，与引用声明处初值同一路径。入口即绑定的形参、已带初值的引用
    // 都在 ref_bound 里，引用返回变量在上面已单独处理；字段不在 slots 里，
    // INIT 之外的字段赋值是写穿，都不走这里
    if (a->op == OpKind::Assign && a->target->sym &&
        a->target->sym->kind == SymbolKind::Variable) {
        auto* vi = dynamic_cast<VarItem*>(a->target->sym->node);
        if (vi && is_ref_item(vi, nullptr) && ctx_.slots.count(vi) &&
            !ctx_.ref_bound.count(vi)) {
            std::string addr = eval_ref_addr(a->value);
            if (addr.empty()) addr = eval(a->value);
            emit_instr("store ptr " + addr + ", ptr " + ptr);
            ctx_.ref_bound.insert(vi);
            return addr;
        }
    }
    // 数组赋值：定长数组写进自己的存储，切片写进它拥有的存储
    if (a->op == OpKind::Assign && a->target && a->target->type &&
        a->target->type->is_array_type()) {
        Type* el = a->target->type->get_array_type()->element;
        if (is_fixed_array(a->target->type)) {
            emit_fixed_array_init(ptr, el, array_length_of(a->target->type),
                                  a->value);
            return ptr;
        }
        // 切片：存储与长度已在槽里，只替换内容
        std::string sv = eval(a->target);
        std::string sp = ir_.reg();
        emit_instr(sp + " = extractvalue { ptr, i32 } " + sv + ", 0");
        std::string sn = ir_.reg();
        emit_instr(sn + " = extractvalue { ptr, i32 } " + sv + ", 1");
        std::string sn64 = ir_.reg();
        emit_instr(sn64 + " = zext i32 " + sn + " to i64");
        copy_init_into(sp, el, sn64, a->value);
        return ptr;
    }
    // 类赋值是值拷贝：字段就地搬进内联存储，变量另分配副本，与声明式
    // `Node b = a` 同一语义。否则两个变量共享同一实例，各自登记的字段
    // DEREF 会把同一对象析构两次。变量目标 + 需转换时交回通用路径：
    // 同类 conversion 代替等号拷贝（spec.md）
    if (a->op == OpKind::Assign && a->target && a->value &&
        as_class(a->target->type) &&
        (a->target->get_member() || !needs_convert(a->value)))
        return assign_class_value(a->target, a->value);
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
    // 赋值覆盖类实例：先释放目标旧实例，避免旧资源泄漏；源对象不 drop。
    // 新旧指针相同时（自赋值）跳过释放，否则会 double free。
    if (a->op == OpKind::Assign && a->target) {
        if (Class* pc = as_class(a->target->type)) {
            if (SpecialMethod* drop = find_special(pc, SpecialMethod::Kind::Drop)) {
                std::string old = ir_.reg();
                emit_instr(old + " = load ptr, ptr " + ptr);
                std::string same = ir_.reg();
                emit_instr(same + " = icmp eq ptr " + old + ", " + v);
                std::string drop_l = ir_.named_label("assign.drop");
                std::string cont_l = ir_.named_label("assign.cont");
                emit_term("br i1 " + same + ", label %" + cont_l +
                          ", label %" + drop_l);
                open_block(drop_l);
                emit_guard_call(drop, old);
                emit_term("br label %" + cont_l);
                open_block(cont_l);
            }
        }
    }
    emit_instr("store " + t + " " + v + ", ptr " + ptr);
    if (t == "ptr" && a->target) {
        Class* pc = pointee_class(a->target->type);
        if (pc) emit_bind_ref(pc, v, SpecialMethod::Kind::Ref);
    }
    return v;
}

// 类左值 = 类右值
// 字段是内联存储：把源对象逐字段搬进字段自身，不另分配，字段因此不悬垂。
// 变量是实例指针槽：另分配副本逐字段复制，源对象保持不变。
std::string Translator::assign_class_value(Expr* target, Expr* value) {
    Class* pc = as_class(target->type);
    if (!pc) return "0";
    std::string src = eval_converted(value);
    std::string ptr = eval_lvalue(target);
    if (ptr.empty()) return "0";
    // 存储形态决定写法：引用左值与内联字段指向的就是对象本身，逐字段搬进去；
    // 类实例变量的槽存的是实例指针，须另分配副本后改写指针。
    // 引用左值是写穿而非重新绑定，与 int-& 一致（s = b 改的是 s 指向的对象）
    Type* dt = target->sym ? target->sym->get_type() : nullptr;
    bool ref_target = dt && dt->is_reference();
    if (ref_target || target->get_member()) {
        emit_fields_copy(pc, src, ptr, true);
        return ptr;
    }
    std::string old = ir_.reg();
    emit_instr(old + " = load ptr, ptr " + ptr);
    std::string same = ir_.reg();
    emit_instr(same + " = icmp eq ptr " + old + ", " + src);
    std::string copy_l = ir_.named_label("assign.copy");
    std::string cont_l = ir_.named_label("assign.cont");
    // 自赋值：源与目标同址，复制后再释放会毁掉复制源本身，直接跳过
    emit_term("br i1 " + same + ", label %" + cont_l + ", label %" + copy_l);
    open_block(copy_l);
    // 先释放目标原有实例：副本存储在循环里可能就是这一块，
    // 拷贝会覆盖它，先拷贝后释放等于对刚被覆盖的对象调 DROP
    emit_guard_call(find_special(pc, SpecialMethod::Kind::Drop), old);
    // malloc 类（或 malloc 变量）被赋值时复制的是堆上的对象（spec.md），
    // 否则副本落在栈上，free() 会拿到栈指针
    bool heap = pc->specs.has(SpecKind::Malloc);
    if (!heap && target->sym)
        heap = ctx_.malloc_vars.count(static_cast<const VarItem*>(
                    target->sym->node)) > 0;
    std::string dst = emit_class_copy(pc, src, heap);
    emit_instr("store ptr " + dst + ", ptr " + ptr);
    emit_term("br label %" + cont_l);
    open_block(cont_l);
    std::string r = ir_.reg();
    emit_instr(r + " = load ptr, ptr " + ptr);
    return r;
}

std::string Translator::eval_index(Expr* e, Expr::Index*) {
    std::string addr = eval_lvalue(e);
    if (addr.empty()) return "0";
    // 元素本身是数组：值即其地址
    if (is_fixed_array(e->type)) return addr;
    std::string et = map_type(e->type);
    std::string v = ir_.reg();
    emit_instr(v + " = load " + et + ", ptr " + addr);
    return v;
}

std::string Translator::eval_member(Expr* e, Expr::Member* m) {
    // 限定名函数作为值：与裸名一样 decay 为函数指针，取其地址
    if (e->type && e->type->is_func_ref())
        return "@" + IRWriter::quote(mangler_.of(e->type->get_func_ref()->func));
    // 枚举值：编译期常量，直接内联为整数
    if (e->sym && e->sym->kind == SymbolKind::EnumVar) {
        if (auto* ev = e->sym->get<EnumVariant>())
            return std::to_string(ev->value);
    }
    // static 字段与模块级变量：都是 LLVM 全局量
    if (e->sym && e->sym->kind == SymbolKind::Variable) {
        if (auto* vi = dynamic_cast<VarItem*>(e->sym->node)) {
            std::string g = mangler_.field_of(vi);
            if (g.empty()) {
                auto git = global_names_.find(vi);
                if (git != global_names_.end()) g = git->second;
            }
            if (!g.empty()) return eval_field_value(e, "@" + g);
        }
    }
    // 数组/切片的 .len：定长数组的长度是编译期常量
    if (m->member == "len" && m->target && m->target->type &&
        m->target->type->is_array_type()) {
        if (is_fixed_array(m->target->type))
            return std::to_string(array_length_of(m->target->type));
        std::string s = eval(m->target);
        std::string len = ir_.reg();
        emit_instr(len + " = extractvalue { ptr, i32 } " + s + ", 1");
        return len;
    }
    // 数组/切片的 .ptr：数组值即首元素地址，切片取其指针域
    if (m->member == "ptr" && m->target && m->target->type &&
        m->target->type->is_array_type()) {
        if (is_fixed_array(m->target->type)) return eval(m->target);
        std::string s = eval(m->target);
        std::string p = ir_.reg();
        emit_instr(p + " = extractvalue { ptr, i32 } " + s + ", 0");
        return p;
    }
    // 类字段
    Expr* tgt = m->target;
    auto* vi = (e->sym && e->sym->kind == SymbolKind::Variable)
        ? dynamic_cast<VarItem*>(e->sym->node) : nullptr;
    // 类名限定访问 static 字段 C::counter：C 在值位置只带符号，
    // 成员查找已由语义阶段按类作用域完成，落到上面的 static 字段分支
    if (Class* cls = as_class(tgt ? tgt->type : nullptr)) {
        auto it = classes_.find(cls);
        if (it != classes_.end() && vi) {
            auto fit = it->second.field_index.find(vi);
            if (fit != it->second.field_index.end()) {
                std::string base = eval(tgt);
                std::string addr = ir_.reg();
                emit_instr(addr + " = getelementptr " + it->second.llvm_name +
                           ", ptr " + base + ", i32 0, i32 " +
                           std::to_string(fit->second));
                return eval_field_value(e, addr);
            }
        }
    }
    return "0";
}

std::string Translator::eval_array(Expr* e, Expr::Array* arr) {
    Type* elem_type = nullptr;
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

    // 定长数组：元素写进 [N x T] 存储，值即其地址
    if (is_fixed_array(e->type)) {
        std::string backing = entry_alloca(object_type(e->type));
        emit_fixed_array_init(backing, elem_type, (int)n, e);
        return backing;
    }

    std::string backing = entry_alloca(et + ", i64 " +
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
    // 多分配 1 字节存放 NUL 终止符；长度是编译期常量，放入口块
    std::string backing = entry_alloca("i8, i64 " + std::to_string(n + 1));
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
    // 数组/切片 -> 指针：取底层指针域
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
    // 指针 -> 整数：地址值
    if (src == "ptr" && is_int_n(dst)) {
        emit_instr(r + " = ptrtoint ptr " + v + " to " + dst);
        return r;
    }
    // 整数 -> 指针：地址值重解释
    if (is_int_n(src) && dst == "ptr") {
        emit_instr(r + " = inttoptr " + src + " " + v + " to ptr");
        return r;
    }
    return v;
}

std::string Translator::eval_cast(Expr*, Expr::Cast* c) {
    Type* src_expr = c->operand ? c->operand->type : nullptr;
    Type* dst_expr = c->target_type;
    std::string src = map_type(src_expr);
    std::string dst = map_type(dst_expr);
    std::string v = eval(c->operand);
    return cast_value(v, src, dst,
                      !expr_is_unsigned(src_expr), !expr_is_unsigned(dst_expr));
}

std::string Translator::emit_conversion_call(Expr* e, Conversion* cf) {
    Type* ret_type_expr = nullptr;
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
    Class* ret_cls = (!via_reg) ? as_class(ret_type_expr) : nullptr;
    std::string sret_slot;
    std::vector<std::string> args;
    if (!via_reg) {
        if (ret_cls) {
            sret_slot = entry_alloca(classes_[ret_cls].llvm_name);
            args.push_back("ptr sret(" + classes_[ret_cls].llvm_name + ") " +
                           sret_slot);
        } else {
            sret_slot = entry_alloca(ret_ir);
            args.push_back("ptr sret(" + ret_ir + ") " + sret_slot);
        }
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
                    Type* pt_expr = vi->type ? vi->type : vl->type;
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
    // 类返回：对象存储指针即类值；标量返回直接给出存储
    return sret_slot;
}

std::string Translator::emit_operator_call(Expr* e, Operator* op,
                                           Expr* lhs, Expr* rhs) {
    // 语义通过的程序里操作符表达式必有类型，缺失即内部错误
    if (!e || !e->type) {
        error(Location(), "internal: operator call without result type");
        return "0";
    }
    std::string ret_t = object_type(e->type);

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
                Type* pte = vi->type ? vi->type : vl->type;
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
    Class* ret_cls = (!via_reg && e) ? as_class(e->type) : nullptr;
    std::string sret_slot;
    std::vector<std::string> args;
    if (!via_reg) {
        if (ret_cls) {
            sret_slot = entry_alloca(classes_[ret_cls].llvm_name);
            args.push_back("ptr sret(" + classes_[ret_cls].llvm_name + ") " +
                           sret_slot);
        } else {
            sret_slot = entry_alloca(ret_t);
            args.push_back("ptr sret(" + ret_t + ") " + sret_slot);
        }
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
            Type* pt_expr = vi->type ? vi->type : vl->type;
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
    // 类返回：sret 目标对象指针即类值
    if (ret_cls) return sret_slot;
    std::string r = ir_.reg();
    emit_instr(r + " = load " + ret_t + ", ptr " + sret_slot);
    return r;
}

void Translator::emit_declare(Func& f) {
    std::string ret_t = "void";
    VarItem* ret_var = nullptr;
    if (!f.returns.empty() && f.returns.back()) {
        VarList* rl = f.returns.back();
        if (!rl->vars.empty()) ret_var = rl->vars.back();
        Type* rt = (ret_var && ret_var->type) ? ret_var->type : rl->type;
        if (rt) {
            // 按值返回聚合的 extern 函数：声明的返回类型必须是真实结构体，
            // 否则 ABI 就对不上了（类值在 IR 里是存储指针，只在跨 ABI 时按结构体收发）
            if (Class* cls = as_class(rt)) ret_t = classes_[cls].llvm_name;
            else ret_t = object_type(rt);
        }
    }

    std::vector<std::string> params;
    if (ret_var && !f.ret_via_reg)
        params.push_back("ptr sret(" + ret_t + ") %ret");
    int idx = 0;
    for (VarList* vl : f.params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            Type* pt = vi->type ? vi->type : vl->type;
            // extern 按值收聚合：形参是真实结构体，不是类值的存储指针
            Class* pc = pt ? as_class(pt) : nullptr;
            params.push_back((pc ? classes_[pc].llvm_name
                                 : (pt ? object_type(pt) : "i32")) + " %a" +
                             std::to_string(idx++));
        }
    }

    std::string ret_sig = (ret_var && !f.ret_via_reg) ? "void" : ret_t;
    std::string sig = "declare " + ret_sig + " @" +
                      IRWriter::quote(mangler_.of(&f)) + "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) sig += ", ";
        sig += params[i];
    }
    if (f.is_variadic) sig += params.empty() ? "..." : ", ...";
    sig += ")";
    ir_.emit_type(sig);
}

// 内置 free：释放 malloc 实例。类方法内的裸 free() 释放 self
std::string Translator::eval_free_builtin(Expr* e, Expr::Call* c) {
    (void)e;
    if (!c->free_builtin || !c->callee) return "";
    if (c->free_on_self) {
        std::string s = ctx_.self_ptr;
        if (s.empty()) return "0";
        Class* c2 = ctx_.current_class;
        if (c2) {
            if (SpecialMethod* drop = find_special(c2, SpecialMethod::Kind::Drop))
                emit_instr("call void @" + IRWriter::quote(mangler_.of(drop)) +
                           "(ptr " + s + ")");
        }
        ir_.need_free();
        emit_instr("call void @free(ptr " + s + ")");
        if (c2) self_free_cls_.insert(c2);
        return "0";
    }
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
        // 释放即结束这一次持有，与分配时的 REF 配对
        emit_guard_call(find_special(cls, SpecialMethod::Kind::Deref), self);
    }
    // DEREF 内部自行 free 过就不再重复释放
    if (cls && self_free_cls_.count(cls)) return "0";
    ir_.need_free();
    emit_instr("call void @free(ptr " + self + ")");
    return "0";
}

// 无 INIT 的类零参默认构造：分配实例
std::string Translator::eval_default_ctor(Expr* e, Expr::Call* c) {
    if (c->resolved || !c->callee || !c->callee->sym ||
        c->callee->sym->kind != SymbolKind::Class) return "";
    return emit_default_object(as_class(e->type));
}

// 分配类实例存储并写入声明处字段默认值。不执行 INIT：INIT 是手动调用的
// 构造方法，模块级变量与 static 字段的默认初始化都只落字段初值
std::string Translator::emit_default_object(Class* cls) {
    if (!cls) return "0";
    auto it = classes_.find(cls);
    if (it == classes_.end()) return "0";
    std::string slot;
    if (cls->specs.has(SpecKind::Malloc)) {
        ir_.need_malloc();
        slot = ir_.reg();
        emit_instr(slot + " = call ptr @malloc(i64 " +
                   emit_size_of(it->second.llvm_name) + ")");
        emit_bind_ref(cls, slot, SpecialMethod::Kind::Ref);
    } else {
        slot = entry_alloca(it->second.llvm_name);
    }
    emit_field_defaults(cls, slot);
    return slot;
}

// 把 C(...) 的 INIT 就地发到已分配好的 slot 上：字段内联构造与普通构造共用
void Translator::emit_ctor_into(Expr::Call* c, SpecialMethod* sm, Class* cls,
                                const std::string& slot) {
    emit_field_defaults(cls, slot);
    std::vector<std::string> args;
    args.push_back("ptr " + slot);
    size_t ai = 0;
    for (Expr* a : c->args) {
        bool converted = needs_convert(a);
        std::string v = converted ? eval_converted(a) : eval(a);
        std::string at = a && a->type ? map_type(a->type) : "i32";
        // 定位该实参对应的形参 IR 类型，用于字符串数组构造与数值 cast
        Type* pte = nullptr;
        {
            size_t k = 0;
            for (VarList* vl : sm->params) {
                if (!vl) continue;
                bool found = false;
                for (VarItem* vi : vl->vars) {
                    if (k == ai) { pte = vi->type ? vi->type : vl->type; found = true; break; }
                    ++k;
                }
                if (found) break;
            }
        }
        std::string want = pte ? map_type(pte) : at;
        if (converted) {
            // 转换后实参类型即对应形参类型
            at = want;
        } else if (want == "{ ptr, i32 }" && a && a->type &&
                   a->type->is_string_literal_type()) {
            v = build_string_slice(a);
            at = want;
        } else if (at != want) {
            v = cast_value(v, at, want, !expr_is_unsigned(a ? a->type : nullptr),
                           !expr_is_unsigned(pte));
            at = want;
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
                   emit_size_of(it->second.llvm_name) + ")");
    } else {
        slot = entry_alloca(it->second.llvm_name);
    }
    emit_ctor_into(c, sm, cls, slot);
    if (is_malloc) emit_bind_ref(cls, slot, SpecialMethod::Kind::Ref);
    return slot;
}

// 普通函数/运算符调用
std::string Translator::eval_plain_call(Expr*, Expr::Call* c, Func* fn) {
    std::string fname = mangler_.of(fn);
    std::string ret_t = "void";
    VarItem* ret_var = nullptr;
    Type* ret_expr = nullptr;
    if (!fn->returns.empty() && fn->returns.back()) {
        VarList* rl = fn->returns.back();
        if (!rl->vars.empty()) ret_var = rl->vars.back();
        Type* rt_ = (ret_var && ret_var->type) ? ret_var->type : rl->type;
        if (rt_) { ret_t = object_type(rt_); ret_expr = rt_; }
    }

    std::vector<std::string> args;
    // 每个实参的 IR 类型，与 args 同步；变参调用的显式函数类型要按它拼
    std::vector<std::string> arg_ty;

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
        } else if (as_class(callee->type) &&
                   !fn->specs.has(SpecKind::Static)) {
            // functor 调用 f(...)：callee 即接收者实例
            self_arg = eval(callee);
            has_self = true;
        }
    }

    bool via_reg = fn->ret_via_reg && ret_var;
    Class* ret_cls = (!via_reg && ret_var) ? as_class(ret_expr) : nullptr;
    // extern 按值返回聚合：返回值按 C ABI 进寄存器/栈，收到后落进调用方的存储，
    // 再把存储指针当类值交出去。clux 自身函数的类返回走 sret，不走这条路
    Class* extern_ret_cls = (via_reg && ret_var && fn->is_extern_decl())
        ? as_class(ret_expr) : nullptr;
    std::string sret_slot;
    if (ret_var && !via_reg) {
        if (ret_cls) {
            sret_slot = entry_alloca(classes_[ret_cls].llvm_name);
            args.push_back("ptr sret(" + classes_[ret_cls].llvm_name + ") " +
                           sret_slot);
        } else {
            sret_slot = entry_alloca(ret_t);
            args.push_back("ptr sret(" + ret_t + ") " + sret_slot);
        }
        arg_ty.push_back("ptr");
    }
    if (has_self) { args.push_back("ptr " + self_arg); arg_ty.push_back("ptr"); }
    // sret / self 实参排在声明形参之前，变参签名里的固定形参要从它们之后数
    size_t lead_args = arg_ty.size();
    // 形参 IR 类型表，用于字符串字面量按数组形参构造 {ptr,i32}；
    // param_ref 标记引用形参：实参传地址而非值
    std::vector<std::string> param_ir;
    std::vector<bool> param_ref;
    // extern 按值收聚合的形参：类型是真实结构体，实参要从类值里 load 出来
    std::vector<bool> param_agg;
    for (VarList* vl : fn->params) {
        if (!vl) continue;
        for (VarItem* vi : vl->vars) {
            Type* pt = vi->type ? vi->type : vl->type;
            Class* pc = (fn->is_extern_decl() && pt) ? as_class(pt) : nullptr;
            param_ir.push_back(pc ? classes_[pc].llvm_name
                                  : (pt ? object_type(pt) : "i32"));
            param_ref.push_back(is_ref_item(vi, nullptr));
            param_agg.push_back(pc != nullptr);
        }
    }
    for (size_t ai = 0; ai < c->args.size(); ++ai) {
        Expr* a = c->args[ai];
        std::string at = a && a->type ? map_type(a->type) : "i32";
        // 引用形参：按绑定语义传被绑定对象的地址，跳过值转换
        if (ai < param_ref.size() && param_ref[ai]) {
            std::string addr = eval_ref_addr(a);
            args.push_back("ptr " + (addr.empty() ? eval(a) : addr));
            arg_ty.push_back("ptr");
            continue;
        }
        // 变参多余实参：按 C 默认实参提升（char/short -> int，float -> double）
        if (fn->is_variadic && ai >= param_ir.size()) {
            std::string v;
            std::string want;
            if (a && a->type && a->type->is_string_literal_type()) {
                v = eval(a);          // 字符串常量地址
                want = "ptr";
            } else if (a && a->type && is_float_type(a->type)) {
                v = eval(a);
                want = "double";
                if (at != want) v = cast_value(v, at, want, true, true);
            } else if (a && a->type && is_bool_type(a->type)) {
                v = eval(a);
                want = "i32";
                v = cast_value(v, "i1", "i32", false, true);
            } else {
                v = eval(a);
                want = at;
                if (at.size() >= 2 && at[0] == 'i' && at != "i1" &&
                    std::stoi(at.substr(1)) < 32) {
                    want = "i32";
                    v = cast_value(v, at, want, !expr_is_unsigned(a->type), true);
                }
            }
            args.push_back(want + " " + v);
            arg_ty.push_back(want);
            continue;
        }
        std::string want = ai < param_ir.size() ? param_ir[ai] : at;
        std::string v;
        bool converted = needs_convert(a);
        // extern 按值收聚合：实参是类值（存储指针），按 ABI 取出结构体本身
        bool agg_param = ai < param_agg.size() && param_agg[ai] &&
                         ai < param_ref.size() && !param_ref[ai];
        if (agg_param) {
            // 类值是存储指针，按 ABI 传结构体本身：先把值取出来
            std::string reg = ir_.reg();
            emit_instr(reg + " = load " + want + ", ptr " +
                       (converted ? eval_converted(a) : eval(a)));
            v = reg;
        } else if (converted)
            v = eval_converted(a);
        else if (want == "{ ptr, i32 }" && a && a->type &&
            a->type->is_string_literal_type())
            v = build_string_slice(a);
        else {
            // 找到该实参对应的形参类型节点，用于取符号与构造数组实参
            Type* pt_expr = nullptr;
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
            // 形参是定长数组：按形参的元素类型构造实参，值即数组本身
            if (is_fixed_array(pt_expr)) {
                v = eval_as_array(a, pt_expr);
                at = want;
            } else {
                v = eval(a);
                if (at != want)
                    v = cast_value(v, at, want,
                        !expr_is_unsigned(a ? a->type : nullptr),
                        !expr_is_unsigned(pt_expr));
            }
        }
        args.push_back(want + " " + v);
        arg_ty.push_back(want);
    }

    bool is_main = (std::string(fn->name) == "main" && ctx_.current_class == nullptr);
    bool plain_main = is_main && !ret_var && ret_t == "void";

    std::string call_ret;
    if (via_reg)
        call_ret = extern_ret_cls ? classes_[extern_ret_cls].llvm_name : ret_t;
    else call_ret = ret_var ? "void" : (plain_main ? "i32" : ret_t);
    std::string sig;
    if (fn->is_variadic) {
        // 变参调用必须写显式变参函数类型：x86-64 后端据此把 xmm 使用个数放进
        // %al，glibc 的 printf 靠它决定浮点变参是走寄存器还是栈
        size_t fixed = std::min(lead_args + param_ir.size(), arg_ty.size());
        sig = call_ret + " (";
        for (size_t i = lead_args; i < fixed; ++i) {
            if (i > lead_args) sig += ", ";
            sig += arg_ty[i];
        }
        sig += fixed ? ", ...) @" : "...) @";
    } else {
        sig = call_ret + " @";
    }
    sig += IRWriter::quote(fname) + "(";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) sig += ", ";
        sig += args[i];
    }
    sig += ")";

    if (ret_var && !via_reg) {
        emit_instr("call " + sig);
        // 类返回：对象存储指针即类值，直接返回，无需 load
        if (ret_cls) return sret_slot;
        // 定长数组返回：sret 槽就是数组存储
        if (is_fixed_array(ret_expr)) return sret_slot;
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
    if (extern_ret_cls) {
        // 聚合返回值没有自己的存储，拷进入口块槽位后按类值（存储指针）返回
        std::string agg = classes_[extern_ret_cls].llvm_name;
        std::string slot = entry_alloca(agg);
        emit_instr("store " + agg + " " + r + ", ptr " + slot);
        return slot;
    }
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

std::string Translator::call_func_ptr(Expr* e, Expr::Call* c, Type::FuncPtr* fp) {
    std::string fpv = eval(c->callee);
    (void)e;
    // 函数指针类型的返回类型由 parser 保证非空，缺失即内部错误
    if (!fp->return_type) {
        error(Location(), "internal: func-ptr type without return type");
        return "0";
    }
    std::string ret_t = map_type(fp->return_type);
    bool via_reg = (ret_t != "void");

    std::vector<std::string> args;
    for (size_t i = 0; i < c->args.size(); ++i) {
        Expr* a = c->args[i];
        Type* pt_expr = i < fp->params.size() ? fp->params[i] : nullptr;
        std::string pt = pt_expr ? object_type(pt_expr)
                                 : (a && a->type ? map_type(a->type) : "i32");
        // 引用形参：按绑定语义传被绑定对象的地址，跳过值转换
        if (pt_expr && pt_expr->is_reference()) {
            std::string addr = eval_ref_addr(a);
            args.push_back("ptr " + (addr.empty() ? eval(a) : addr));
            continue;
        }
        std::string v = needs_convert(a) ? eval_converted(a) : std::string();
        std::string at = a && a->type ? map_type(a->type) : pt;
        bool converted = needs_convert(a);
        // 形参是定长数组：按形参的元素类型构造实参
        if (!converted && is_fixed_array(pt_expr)) {
            v = eval_as_array(a, pt_expr);
            at = pt;
        } else if (!converted) {
            v = eval(a);
        }
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
    std::string res_slot = entry_alloca(ttype);

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
    std::string wrapper = "__spawn_" + std::to_string(aux_fn_counter_++);
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
        if (rl->type) ret_t = object_type(rl->type);
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

    std::string tid = entry_alloca("i64");
    emit_instr("call i32 @pthread_create(ptr " + tid + ", ptr null, ptr @" +
               IRWriter::quote(wrapper) + ", ptr " + pack + ")");
    return "0";
}

int Translator::array_length_of(Type* arrtype) {
    if (!arrtype || !arrtype->is_array_type()) return 0;
    Expr* len = arrtype->get_array_type()->length;
    if (!len || !len->get_literal()) return 0;
    try { return std::stoi(std::string(len->get_literal()->value)); }
    catch (...) { return 0; }
}

} // namespace cx
