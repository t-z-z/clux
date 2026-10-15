#include "../include/semantic.h"
#include "../include/sem_types.h"

namespace cx {


// 类型转换判定与检查集中于此：本组逻辑分支密集、最易出错，
// 单独成文件以便评审与测试。
bool SemanticAnalyzer::record_conversion(Expr* e, Type* dst) {
    if (!e || !e->type || !dst) return false;
    bool same = is_same_type(e->type, dst);
    // self->self conversion 是拷贝语义（COPY）：仅当源为已存在对象（lvalue）
    // 时才需要深拷贝；新产生的临时值（rvalue）本身即新鲜对象，直接取用。
    if (same && !e->is_lvalue) return true;
    if (Symbol* conv = find_conversion(*e->type, *dst)) {
        e->conv = conv->get_decl();
        return true;
    }
    if (same) return true;
    // 包装类参与：记隐式转换目标，translator 走解包/打包 + 数值 cast
    if (wrap_class_of(dst) || wrap_class_of(e->type)) {
        e->implicit_to = dst;
        return true;
    }
    return false;
}

Symbol* SemanticAnalyzer::find_conversion(Type& src, Type& dst) {
    std::string srckey = type_to_key(src);
    std::string dstkey = type_to_key(dst);
    if (Symbol* sym = symbols_.lookup_member("conversion_" + srckey + "_" + dstkey))
        return sym;
    if (!src.resolved_decl || src.resolved_decl->kind != DeclKind::Class) return nullptr;
    Scope* scope = static_cast<Class*>(src.resolved_decl)->scope;
    if (!scope) return nullptr;
    return scope->lookup_member("conversion_" + dstkey);
}

Class* SemanticAnalyzer::wrap_or_plain_class(Type* t) {
    if (!t) return nullptr;
    if (!t->resolved_decl || t->resolved_decl->kind != DeclKind::Class)
        return nullptr;
    return static_cast<Class*>(t->resolved_decl);
}

bool SemanticAnalyzer::is_malloc_value(Expr* e) {
    if (!e || !e->type) return false;
    if (Class* c = wrap_or_plain_class(e->type)) {
        if (c->specs.has(SpecKind::Malloc)) return true;
    }
    // malloc 修饰写在变量声明上时由符号带出（VarItem 借所属 VarList 的修饰）
    return e->sym && e->sym->specs && e->sym->specs->has(SpecKind::Malloc);
}

Class* SemanticAnalyzer::wrap_class_of(Type* t) {
    if (!t) return nullptr;
    if (!t->resolved_decl || t->resolved_decl->kind != DeclKind::Class)
        return nullptr;
    auto* c = static_cast<Class*>(t->resolved_decl);
    if (c->base_source == PrimitiveTypeKind::Void) return nullptr;
    return c;
}

Type* SemanticAnalyzer::wrap_base_type(Class* c) {
    return cu_.add_node<Type>(c->loc, Type::Primitive{c->base_source});
}

// 实际上包含了is_same_type，使用之前不用再判is_same_type
bool SemanticAnalyzer::can_convert_to(Type* src, Type* dst) {
    if (!src || !dst) return false;
    if (!src && !dst) return true;
    if (auto* r=src->get_reference()) {
        src = r->target;
        if (!src) return false;
    }
    if (auto* r=dst->get_reference()) {
        dst = r->target;
        if (!dst) return false;
    }
    if (is_same_type(src, dst)) return true;

    // 数值类型之间可隐式转换
    if (is_numeric_type(src) && is_numeric_type(dst)) return true;

    // bool 与整数互转
    if (is_bool_type(src) && is_integer_type(dst)) return true;
    if (is_integer_type(src) && is_bool_type(dst)) return true;

    // 指针→bool：按非空；数组→bool：按 len != 0（切片是视图，无 null 态）
    if (is_bool_type(dst) && (src->is_pointer() || src->is_array_type()))
        return true;
    if (is_bool_type(dst) && src->is_null()) return true;

    // null → 任意指针（数组是视图，无 null 态，不在此列）
    if (src->is_null() && dst->is_pointer()) return true;

    // 任意指针 → void*
    if (src->is_pointer() && dst->is_pointer() &&
        is_void_type(dst->get_pointer()->pointee)) {
        return true;
    }

    // 指针 → 定长数组：由目标类型提供长度（int[N] s = ptr）
    if (src->is_pointer() && dst->is_array_type() &&
        !is_slice_type(dst) && dst->get_array_type()->length) {
        Type* elem = dst->get_array_type()->element;
        Type* pointee = src->get_pointer()->pointee;
        if (elem && pointee &&
            (is_same_type(elem, pointee) || is_void_type(pointee)))
            return true;
    }

    // 定长数组 → 所有权切片：backing 与长度都现成；反向不可，长度运行期才知道
    if (src->is_array_type() && !is_slice_type(src) && is_slice_type(dst)) {
        Type* elem = src->get_array_type()->element;
        Type* delem = dst->get_array_type()->element;
        if (elem && delem && is_same_type(elem, delem)) return true;
    }

    // 数组/切片 → 指向元素类型的指针（元素相同，或目标为 void*）
    if (src->is_array_type() && dst->is_pointer()) {
        Type* elem = src->get_array_type()->element;
        Type* pointee = dst->get_pointer()->pointee;
        if (elem && pointee &&
            (is_same_type(elem, pointee) || is_void_type(pointee)))
            return true;
    }

    // 字符串字面量 → 字符指针（底层是带 NUL 的字符序列）
    if (src->is_string_literal_type() && dst->is_pointer()) {
        Type* pointee = dst->get_pointer()->pointee;
        if (pointee && pointee->is_primitive()) {
            auto k = pointee->get_primitive()->kind;
            if (k == PrimitiveTypeKind::Char || k == PrimitiveTypeKind::UChar ||
                k == PrimitiveTypeKind::Int8 || k == PrimitiveTypeKind::UInt8 ||
                is_void_type(pointee))
                return true;
        }
    }

    // 函数 decay：函数引用 → 同签名函数指针
    if (src->is_func_ref() && dst->is_func_ptr()) {
        Func* f = src->get_func_ref()->func;
        auto* fp = dst->get_func_ptr();
        if (f) {
            std::vector<Type*> params;
            for (VarList* vl : f->params) {
                if (!vl) continue;
                for (VarItem* vi : vl->vars)
                    if (vi && vi->type) params.push_back(vi->type);
            }
            if (params.size() == fp->params.size()) {
                bool ok = true;
                for (size_t i = 0; i < params.size(); ++i)
                    if (!is_same_type(params[i], fp->params[i])) { ok = false; break; }
                if (ok) {
                    Type* ret = typeVoid;
                    if (!f->returns.empty() && f->returns.back() &&
                        f->returns.back()->type)
                        ret = f->returns.back()->type;
                    // 函数指针 ABI：void 无返回，否则寄存器返回，
                    // 故仅 void 返回或标量寄存器返回的函数可 decay；
                    // 按值返回聚合的函数走另一套 ABI，不能取地址
                    if (is_same_type(ret, fp->return_type) &&
                        (is_void_type(ret) || (f->ret_via_reg && !as_class(ret))))
                        return true;
                }
            }
        }
    }

    // conversion：全局有参形式或源类无参形式
    if (find_conversion(*src, *dst)) return true;

    // 包装特判：目标为包装类则比承载类型；源为包装类则比承载类型
    if (Class* dw = wrap_class_of(dst)) {
        Type* bt = wrap_base_type(dw);
        if (can_convert_to(src, bt)) return true;
    }
    if (Class* sw = wrap_class_of(src)) {
        Type* bt = wrap_base_type(sw);
        if (can_convert_to(bt, dst)) return true;
    }

    return false;
}

// conversion 判定：判断 src 能否转换为 dst，复用 can_convert_to 的转换规则
bool SemanticAnalyzer::has_conversion(Type* src, Type* dst) {
    return can_convert_to(src, dst);
}

void SemanticAnalyzer::check_array_len_fit(Expr* src, Type* dst_type,
                                           const std::string& context,
                                           const Location& loc) {
    if (!src || !dst_type || !dst_type->is_array_type()) return;
    long long decl_len = const_array_type_len(dst_type);
    if (decl_len <= 0) return;
    // 字面量按元素数算；定长数组实参见其类型长度
    long long n = const_array_len_of(src);
    bool literal = n >= 0;
    if (!literal) n = const_array_type_len(src->type);
    if (n >= 0 && n > decl_len) {
        error(loc, std::string(literal ? "array literal has " : "array has ") +
              std::to_string(n) + " elements but " + context +
              " length is " + std::to_string(decl_len));
    }
}

// 整数字面量在转换到目标整数类型时是否溢出
void SemanticAnalyzer::check_literal_range(Expr* value, Type* dst,
                                           const std::string& context,
                                           const Location& loc) {
    if (!value || !value->is_literal() || !dst) return;
    Type* ud = dst;
    if (!ud || !ud->is_primitive()) return;
    bool dst_signed = false; int dst_bits = 0;
    if (!primitive_int_range(ud->get_primitive()->kind, dst_signed, dst_bits))
        return;
    auto* lit = value->get_literal();
    bool lit_signed = false; int lit_bits = 0;
    if (!literal_int_range(lit->kind, lit_signed, lit_bits)) return;
    std::string dec;
    if (!parse_int_literal(lit->value, dec)) return;
    if (dec_cmp(dec, int_max_text(dst_signed, dst_bits)) > 0) {
        error(loc, "integer literal " + std::string(lit->value) +
                   " is out of range for " + type_to_key(*dst) +
                   " in " + context);
    }
}

// 形参定长时强制实参数组长度相同；形参为所有权切片时实参长度任意。
// 数组字面量实参不受此限（允许按目标长度补齐）。
void SemanticAnalyzer::check_param_array_len(Expr* arg, Type* at, Type* pt,
                                             const Location& loc) {
    if (!at || !pt) return;
    if (!pt->is_array_type() || !at->is_array_type()) return;
    if (is_slice_type(at) && !is_slice_type(pt)) return;  // 转换本身不可行，由 check_conversion 报
    if (arg && arg->is_array()) return;  // 字面量：按目标长度补齐
    long long plen = const_array_type_len(pt);
    if (plen < 0) return;  // 形参未定长
    long long alen = const_array_type_len(at);
    if (alen < 0) {
        error(loc, "array argument length must be known when parameter "
                   "length is fixed");
        return;
    }
    if (alen != plen) {
        error(loc, "array argument length " + std::to_string(alen) +
              " does not match parameter length " + std::to_string(plen));
    }
}

bool SemanticAnalyzer::check_conversion(Type* src, Type* dst, const std::string& context,
                                        const Location& loc, bool explicit_cast) {
    if (!src || !dst) return false;
    if (is_same_type(src, dst)) return true;
    if (has_conversion(src, dst)) return true;
    // 切片长度运行期才知道，不能当定长数组用；反向的数组 → 切片是允许的
    if (src->is_array_type() && dst->is_array_type() &&
        is_slice_type(src) && !is_slice_type(dst)) {
        error(loc, "cannot use a slice as a fixed-length array: its length is "
                   "only known at run time, in " + context);
        return false;
    }
    if (explicit_cast) {
        // 仅当显式 cast 能落到确定语义时才放行；否则报错，避免生成非法 IR
        Type* us = src;
        Type* ud = dst;
        bool base_same = us && ud && is_same_type(us, ud);
        bool both_ptr = us && ud && (us->is_pointer() || us->is_reference()) &&
                        (ud->is_pointer() || ud->is_reference());
        // 指针 <-> 整数：底层地址值的重解释
        bool ptr_int = us && ud &&
            ((us->is_pointer() && is_integer_type(ud)) ||
             (is_integer_type(us) && ud->is_pointer()));
        if (ptr_int) {
            // 目标平台指针 64 位：窄于 64 位的整数会截断/扩展地址
            Type* int_t = us->is_pointer() ? ud : us;
            bool sign = false; int bits = 0;
            if (int_t->is_primitive() &&
                primitive_int_range(int_t->get_primitive()->kind, sign, bits) &&
                bits < 64) {
                warning(loc, "conversion between pointer and " +
                        std::to_string(bits) + "-bit integer loses address bits in " +
                        context);
            }
            return true;
        }
        if (base_same || both_ptr) {
            warning(loc, "explicit cast between incompatible types in " + context);
            return true;
        }
        error(loc, "cannot cast between incompatible types in " + context);
        return false;
    }
    error(loc, "cannot implicitly convert between incompatible types in " + context);
    return false;
}

} // namespace cx
