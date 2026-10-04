#include <charconv>

#include "../include/sem_types.h"
#include "../include/semantic.h"

namespace cx {

std::string type_name_string(Expr* t) {
    if (!t) return "";
    if (auto* id = t->get_identifier()) return std::string(id->name);
    if (auto* m = t->get_member())
        return type_name_string(m->target) + "::" + std::string(m->member);
    return "";
}

bool ref_is_lvalue(Expr* t) {
    return t && t->is_reference();
}

Scope* named_type_scope(Expr* t) {
    if (!t) return nullptr;
    if (t->resolved_scope) return t->resolved_scope;
    if (t->resolved_decl) {
        if (Cont* cont = dynamic_cast<Cont*>(t->resolved_decl)) return cont->scope;
    }
    return nullptr;
}

bool primitive_int_range(PrimitiveTypeKind k, bool& is_signed, int& bits) {
    switch (k) {
        case PrimitiveTypeKind::Int8:   is_signed = true;  bits = 8;  return true;
        case PrimitiveTypeKind::UInt8:  is_signed = false; bits = 8;  return true;
        case PrimitiveTypeKind::Short:  is_signed = true;  bits = 16; return true;
        case PrimitiveTypeKind::UShort: is_signed = false; bits = 16; return true;
        case PrimitiveTypeKind::Int:    is_signed = true;  bits = 32; return true;
        case PrimitiveTypeKind::UInt:   is_signed = false; bits = 32; return true;
        case PrimitiveTypeKind::Long:   is_signed = true;  bits = 64; return true;
        case PrimitiveTypeKind::ULong:  is_signed = false; bits = 64; return true;
        default: return false;
    }
}

bool literal_int_range(LiteralKind k, bool& is_signed, int& bits) {
    switch (k) {
        case LiteralKind::Int8:   is_signed = true;  bits = 8;  return true;
        case LiteralKind::UInt8:  is_signed = false; bits = 8;  return true;
        case LiteralKind::Short:  is_signed = true;  bits = 16; return true;
        case LiteralKind::UShort: is_signed = false; bits = 16; return true;
        case LiteralKind::Int:    is_signed = true;  bits = 32; return true;
        case LiteralKind::UInt:   is_signed = false; bits = 32; return true;
        case LiteralKind::Long:   is_signed = true;  bits = 64; return true;
        case LiteralKind::ULong:  is_signed = false; bits = 64; return true;
        default: return false;
    }
}

bool parse_uint_literal(std::string_view text, unsigned long long& out) {
    if (text.empty()) return false;
    int base = 10;
    size_t i = 0;
    if (text.size() >= 2 && text[0] == '0') {
        char c = text[1];
        if      (c == 'x' || c == 'X') { base = 16; i = 2; }
        else if (c == 'b' || c == 'B') { base = 2;  i = 2; }
        else if (c == 'o' || c == 'O') { base = 8;  i = 2; }
    }
    if (i >= text.size()) return false;
    unsigned long long v = 0;
    auto res = std::from_chars(text.data() + i, text.data() + text.size(), v, base);
    if (res.ec != std::errc() || res.ptr == text.data() + i) return false;
    out = v;
    return true;
}

unsigned long long int_max_value(bool is_signed, int bits) {
    if (bits >= 64) return is_signed ? 0x7FFFFFFFFFFFFFFFull : 0xFFFFFFFFFFFFFFFFull;
    return is_signed ? ((1ull << (bits - 1)) - 1) : ((1ull << bits) - 1);
}

int float_rank(PrimitiveTypeKind k) {
    switch (k) {
        case PrimitiveTypeKind::Double: return 2;
        case PrimitiveTypeKind::Float:  return 1;
        default:                        return 0;
    }
}

int integer_rank(PrimitiveTypeKind k) {
    switch (k) {
        case PrimitiveTypeKind::Int128:
        case PrimitiveTypeKind::UInt128: return 4;
        case PrimitiveTypeKind::Long:
        case PrimitiveTypeKind::ULong:   return 3;
        case PrimitiveTypeKind::Int:
        case PrimitiveTypeKind::UInt:    return 2;
        case PrimitiveTypeKind::Short:
        case PrimitiveTypeKind::UShort:  return 1;
        default:                         return 0;
    }
}

long long const_array_len_of(Expr* e) {
    if (!e) return -1;
    if (auto* a = e->get_array()) {
        if (a->fill_value) return -1;
        return (long long)a->members.size();
    }
    if (e->is_literal() && e->get_literal()->kind == LiteralKind::String)
        return (long long)e->get_literal()->value.size();
    return -1;
}

std::string sym_display_name(Symbol* sym) {
    if (!sym) return "";
    if (auto* vi = dynamic_cast<VarItem*>(sym->node)) return std::string(vi->name);
    if (Decl* d = sym->get_decl()) return std::string(d->name);
    return "";
}


bool is_enum_type(Expr* t) {
    if (!t) return false;
    return t->is_type && (t->is_identifier() || t->is_member()) &&
           t->resolved_decl && t->resolved_decl->kind == DeclKind::Enum;
}

bool is_numeric_type(Expr* t) {
    if (!t) return false;
    // 枚举底层是数字
    if (is_enum_type(t)) return true;
    if (!t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k != PrimitiveTypeKind::Bool && k != PrimitiveTypeKind::Type && k != PrimitiveTypeKind::Void;
}

bool is_integer_type(Expr* t) {
    if (!t) return false;
    // 枚举底层是整数
    if (is_enum_type(t)) return true;
    if(auto* p = t->get_primitive()){
        PrimitiveTypeKind k = p->kind;
        return k == PrimitiveTypeKind::Int8 || k == PrimitiveTypeKind::Short || k == PrimitiveTypeKind::Int
            || k == PrimitiveTypeKind::Long || k == PrimitiveTypeKind::Int128
            || k == PrimitiveTypeKind::UInt8 || k == PrimitiveTypeKind::UShort || k == PrimitiveTypeKind::UInt
            || k == PrimitiveTypeKind::ULong || k == PrimitiveTypeKind::UInt128
            || k == PrimitiveTypeKind::Char || k == PrimitiveTypeKind::UChar;
    }
    return false;
}

bool is_float_type(Expr* t) {
    if (!t || !t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k == PrimitiveTypeKind::Float16 || k == PrimitiveTypeKind::Float || k == PrimitiveTypeKind::Double;
}

bool is_bool_type(Expr* t) {
    return t && t->is_primitive() && t->get_primitive()->kind == PrimitiveTypeKind::Bool;
}

bool is_condition_type(Expr* t) {
    if (is_bool_type(t)) return true;
    if (!t) return false;
    return t->is_pointer() || t->is_array_type() || t->is_null();
}

bool is_same_type(Expr* a, Expr* b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    // StringLiteral 与 char[] 视为同类
    auto is_char_array = [](Expr* t) {
        if (!t || !t->is_array_type()) return false;
        Expr* elem = t->get_array_type()->element;
        return elem && elem->is_primitive()
            && elem->get_primitive()->kind == PrimitiveTypeKind::Char;
    };
    if (a->is_string_literal_type() && is_char_array(b)) return true;
    if (b->is_string_literal_type() && is_char_array(a)) return true;
    // 枚举底层是数字：与 int 及其他枚举等价
    if (is_enum_type(a) || is_enum_type(b)) {
        auto as_i32 = [](Expr* x) {
            if (is_enum_type(x)) return true;
            if (auto* p = x->get_primitive())
                return p->kind == PrimitiveTypeKind::Int;
            return false;
        };
        return as_i32(a) && as_i32(b);
    }
    bool a_named = a->is_type && (a->is_identifier() || a->is_member());
    bool b_named = b->is_type && (b->is_identifier() || b->is_member());
    if (a_named && b_named) {
        if (a->resolved_decl && b->resolved_decl) {
            return a->resolved_decl == b->resolved_decl;
        }
        return type_name_string(a) == type_name_string(b);
    }
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
    // 数组：元素类型相同即可（长度可推导，暂不严格比较）
    if (a->is_array_type() && b->is_array_type()) {
        return is_same_type(a->get_array_type()->element,
                            b->get_array_type()->element);
    }
    if (a->is_func_ref() && b->is_func_ref()) {
        return a->get_func_ref()->func == b->get_func_ref()->func;
    }
    if (a->is_func_ptr() && b->is_func_ptr()) {
        auto* fa = a->get_func_ptr();
        auto* fb = b->get_func_ptr();
        if (fa->params.size() != fb->params.size()) return false;
        for (size_t i = 0; i < fa->params.size(); ++i) {
            if (!is_same_type(fa->params[i], fb->params[i])) return false;
        }
        return is_same_type(fa->return_type, fb->return_type);
    }
    if (a->is_string_literal_type() && b->is_string_literal_type()) return true;
    if (a->is_void() && b->is_void()) return true;
    if (a->is_null() && b->is_null()) return true;
    return false;
}

bool is_integer_promotable(Expr* t) {
    if (!t || !t->is_primitive()) return false;
    auto k = t->get_primitive()->kind;
    return k == PrimitiveTypeKind::Int8 || k == PrimitiveTypeKind::Short
        || k == PrimitiveTypeKind::UInt8 || k == PrimitiveTypeKind::UChar
        || k == PrimitiveTypeKind::Char;
}

bool is_void_type(Expr* t) {
    if (!t) return false;
    if (t->is_void()) return true;
    // 解析器把源码中的 void 生成为 Primitive{Void}
    if (auto* p = t->get_primitive())
        return p->kind == PrimitiveTypeKind::Void;
    return false;
}

} // namespace cx
