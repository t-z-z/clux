#include "../include/sem_types.h"
#include "../include/semantic.h"

#include <array>
#include <charconv>
#include <iterator>
#include <string>
#include <vector>

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
        case PrimitiveTypeKind::Int128: is_signed = true;  bits = 128; return true;
        case PrimitiveTypeKind::UInt128: is_signed = false; bits = 128; return true;
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
        case LiteralKind::Int128: is_signed = true;  bits = 128; return true;
        case LiteralKind::UInt128: is_signed = false; bits = 128; return true;
        default: return false;
    }
}

namespace {

// 十进制大整数：低位在前的 1e9 进制段，足以容纳 128 位
using Dec = std::vector<unsigned>;

void dec_mul_add(Dec& d, unsigned mul, unsigned add) {
    unsigned long long carry = add;
    for (unsigned& limb : d) {
        unsigned long long cur = static_cast<unsigned long long>(limb) * mul + carry;
        limb = static_cast<unsigned>(cur % 1000000000ull);
        carry = cur / 1000000000ull;
    }
    while (carry) {
        d.push_back(static_cast<unsigned>(carry % 1000000000ull));
        carry /= 1000000000ull;
    }
}

void dec_trim(Dec& d) {
    while (d.size() > 1 && d.back() == 0) d.pop_back();
}

std::string dec_str(const Dec& d) {
    if (d.empty()) return "0";
    std::string out = std::to_string(d.back());
    for (size_t i = d.size() - 1; i > 0; --i) {
        std::string part = std::to_string(d[i - 1]);
        out.append(9 - part.size(), '0');
        out += part;
    }
    return out;
}

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 类型后缀：s/l/ll，或 i|u|I|U 加位宽数字，可再跟 s/l/ll
bool valid_int_suffix(std::string_view s) {
    if (s.empty()) return true;
    if (s == "s" || s == "S" || s == "l" || s == "L") return true;
    if (s.size() == 2 && (s[0] == 'l' || s[0] == 'L') && (s[1] == 'l' || s[1] == 'L'))
        return true;
    if (s[0] != 'i' && s[0] != 'I' && s[0] != 'u' && s[0] != 'U') return false;
    size_t i = 1;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i < s.size() && (s[i] == 's' || s[i] == 'S')) ++i;
    else if (i < s.size() && (s[i] == 'l' || s[i] == 'L')) {
        ++i;
        if (i < s.size() && (s[i] == 'l' || s[i] == 'L')) ++i;
    }
    return i == s.size();
}

std::string make_int_max_text(bool is_signed, int bits) {
    if (bits <= 0) return "";
    int k = bits - (is_signed ? 1 : 0);
    Dec d{1};
    for (int i = 0; i < k; ++i) dec_mul_add(d, 2, 0);
    for (size_t i = 0;; ++i) {
        if (d[i] > 0) { --d[i]; break; }
        d[i] = 999999999u;
        if (i + 1 == d.size()) break;
    }
    dec_trim(d);
    return dec_str(d);
}

} // namespace

int dec_cmp(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

std::string int_max_text(bool is_signed, int bits) {
    // 受检位宽只有 8/16/32/64/128 的两种符号性，逐个算一次后复用
    static const int kBits[] = {8, 16, 32, 64, 128};
    static const std::array<std::string, 10> cache = [] {
        std::array<std::string, 10> a;
        for (int s = 0; s < 2; ++s)
            for (size_t i = 0; i < std::size(kBits); ++i)
                a[s * 5 + i] = make_int_max_text(s != 0, kBits[i]);
        return a;
    }();
    for (size_t i = 0; i < std::size(kBits); ++i)
        if (kBits[i] == bits) return cache[(is_signed ? 1 : 0) * 5 + i];
    return make_int_max_text(is_signed, bits);
}

bool parse_int_literal(std::string_view text, std::string& dec) {
    dec.clear();
    int base = 10;
    size_t i = 0;
    if (text.size() >= 2 && text[0] == '0') {
        char c = text[1];
        if      (c == 'x' || c == 'X') { base = 16; i = 2; }
        else if (c == 'b' || c == 'B') { base = 2;  i = 2; }
        else if (c == 'o' || c == 'O') { base = 8;  i = 2; }
    }
    Dec d;
    size_t digits = 0;
    for (; i < text.size(); ++i) {
        int v = hex_val(text[i]);
        if (v < 0 || v >= base) break;
        dec_mul_add(d, static_cast<unsigned>(base), static_cast<unsigned>(v));
        ++digits;
        if (d.size() > 40) return false;   // 远超 128 位，交给调用方按语法错误处理
    }
    if (digits == 0) return false;
    if (!valid_int_suffix(text.substr(i))) return false;
    dec_trim(d);
    dec = dec_str(d);
    return true;
}

bool parse_uint_literal(std::string_view text, unsigned long long& out) {
    std::string dec;
    if (!parse_int_literal(text, dec)) return false;
    unsigned long long v = 0;
    auto res = std::from_chars(dec.data(), dec.data() + dec.size(), v);
    if (res.ec != std::errc() || res.ptr != dec.data() + dec.size()) return false;
    out = v;
    return true;
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


bool is_slice_type(Expr* t) {
    if (!t || !t->is_array_type()) return false;
    return t->get_array_type()->kind == ArrayKind::Slice;
}

long long const_array_type_len(Expr* t) {
    if (!t || !t->is_array_type()) return -1;
    Expr* len = t->get_array_type()->length;
    if (!len || !len->is_literal()) return -1;
    auto* lit = len->get_literal();
    bool sign = false; int bits = 0;
    if (!literal_int_range(lit->kind, sign, bits)) return -1;
    unsigned long long v = 0;
    if (!parse_uint_literal(lit->value, v)) return -1;
    return (long long)v;
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
    // 数组不进条件：定长数组恒有存储，切片的长度语义也不该隐式当真假用，
    // 一律按“不是 bool”报隐式转换
    return t->is_pointer() || t->is_null();
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
    // 数组：元素类型相同即可（长度由 check_array_len_fit / check_param_array_len 单独校验）；
    // 切片与定长数组是不同类型，只能单向转换
    if (a->is_array_type() && b->is_array_type()) {
        if (is_slice_type(a) != is_slice_type(b)) return false;
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
