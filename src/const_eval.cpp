#include "../include/const_eval.h"
#include "../include/ast.h"
#include "../include/sem_types.h"
#include "../include/semantic.h"

#include <algorithm>
#include <climits>
#include <cmath>

namespace cx {

long long ConstValue::i64() const {
    if (!is_signed) return static_cast<long long>(u);
    if (bits >= 64) return static_cast<long long>(u);
    unsigned long long mask = (1ull << bits) - 1;
    unsigned long long val = u & mask;
    unsigned long long signbit = 1ull << (bits - 1);
    if (val & signbit) return static_cast<long long>(val | ~mask);
    return static_cast<long long>(val);
}

bool ConstValue::truthy() const {
    if (is_float()) return f != 0.0;
    return u != 0;
}

static PrimitiveTypeKind prim_of_int(bool is_signed, int bits) {
    if (bits <= 8)  return is_signed ? PrimitiveTypeKind::Int8  : PrimitiveTypeKind::UInt8;
    if (bits <= 16) return is_signed ? PrimitiveTypeKind::Short : PrimitiveTypeKind::UShort;
    if (bits <= 32) return is_signed ? PrimitiveTypeKind::Int   : PrimitiveTypeKind::UInt;
    if (bits <= 64) return is_signed ? PrimitiveTypeKind::Long  : PrimitiveTypeKind::ULong;
    return is_signed ? PrimitiveTypeKind::Int128 : PrimitiveTypeKind::UInt128;
}

static ConstValue make_int(unsigned long long v, bool is_signed, int bits) {
    ConstValue r;
    r.kind = prim_of_int(is_signed, bits);
    r.is_signed = is_signed;
    r.bits = bits;
    r.u = v;
    return r;
}

static ConstValue make_bool(bool b) {
    ConstValue r;
    r.kind = PrimitiveTypeKind::Bool;
    r.is_signed = false;
    r.bits = 8;
    r.u = b ? 1 : 0;
    return r;
}

static ConstValue make_float(double d, PrimitiveTypeKind k) {
    ConstValue r;
    r.kind = k;
    r.is_signed = true;
    r.bits = (k == PrimitiveTypeKind::Double) ? 64
           : (k == PrimitiveTypeKind::Float)  ? 32 : 16;
    r.f = d;
    return r;
}

static ConstValue make_null() {
    ConstValue r;
    r.kind = PrimitiveTypeKind::Void;
    r.is_signed = false;
    r.bits = 64;
    r.u = 0;
    return r;
}

static std::optional<ConstValue> eval_expr(Expr* e, ConstEvalSink& sink, int depth);

static std::optional<ConstValue> eval_literal(const Expr::Literal& lit,
                                              Type* ty,
                                              const Location& loc,
                                              ConstEvalSink& sink) {
    (void)sink; (void)loc;
    switch (lit.kind) {
        case LiteralKind::Int8:  case LiteralKind::Int:   case LiteralKind::Long:
        case LiteralKind::Short: case LiteralKind::Int128:
        case LiteralKind::UInt8: case LiteralKind::UInt:  case LiteralKind::ULong:
        case LiteralKind::UShort: case LiteralKind::UInt128: {
            unsigned long long v = 0;
            if (!parse_uint_literal(lit.value, v)) return std::nullopt;
            bool sign = true; int bits = 32;
            // 无后缀整数字面量的宽度由 P1 按字面量大小定（见 visit_expr_literal），
            // 只按 LiteralKind 取宽度会把放不进 int 的值按 32 位截断
            if (auto* p = ty ? ty->get_primitive() : nullptr) {
                primitive_int_range(p->kind, sign, bits);
            } else {
                literal_int_range(lit.kind, sign, bits);
            }
            return make_int(v, sign, bits);
        }
        case LiteralKind::Char: {
            unsigned long long v = 0;
            if (lit.value.size() == 1) v = (unsigned char)lit.value[0];
            else if (!parse_uint_literal(lit.value, v)) return std::nullopt;
            return make_int(v, true, 8);
        }
        case LiteralKind::Bool:
            return make_bool(lit.value == "true" || lit.value == "1");
        case LiteralKind::Float16:
        case LiteralKind::Float:
        case LiteralKind::Double: {
            double d = 0.0;
            try { d = std::stod(std::string(lit.value)); }
            catch (...) { return std::nullopt; }
            PrimitiveTypeKind k = (lit.kind == LiteralKind::Double) ? PrimitiveTypeKind::Double
                                : (lit.kind == LiteralKind::Float)  ? PrimitiveTypeKind::Float
                                                                    : PrimitiveTypeKind::Float16;
            return make_float(d, k);
        }
        default:
            return std::nullopt;
    }
}

static std::optional<ConstValue> eval_symbol_value(Expr* e, ConstEvalSink& sink, int depth) {
    Symbol* sym = e->sym;
    if (!sym) return std::nullopt;
    if (sym->kind == SymbolKind::EnumVar) {
        auto* ev = dynamic_cast<EnumVariant*>(sym->node);
        // has_value 为假表示该成员排在自己的初值之后（向前引用），值尚未定
        if (!ev || !ev->has_value) return std::nullopt;
        return make_int((unsigned long long)ev->value, true, 32);
    }
    if (sym->is_const()) {
        auto* vi = dynamic_cast<VarItem*>(sym->node);
        if (vi && vi->init) return eval_expr(vi->init, sink, depth + 1);
    }
    return std::nullopt;
}

static std::optional<ConstValue> eval_unary(const Expr::Unary& u,
                                            const Location& loc,
                                            ConstEvalSink& sink,
                                            int depth) {
    (void)loc;
    auto v = eval_expr(u.operand, sink, depth + 1);
    if (!v) return std::nullopt;
    switch (u.op) {
        case OpKind::Plus:
            return v;
        case OpKind::Minus:
            if (v->is_float()) return make_float(-v->f, v->kind);
            return make_int((~v->u) + 1, v->is_signed, v->bits);
        case OpKind::Not:
            return make_bool(!v->truthy());
        case OpKind::BitNot:
            if (v->is_float()) return std::nullopt;
            return make_int(~v->u, v->is_signed, v->bits);
        default:
            return std::nullopt;
    }
}

static std::optional<ConstValue> eval_cast(const ConstValue& v,
                                           Type* target_type,
                                           ConstEvalSink& sink,
                                           const Location& loc) {
    if (!target_type || !target_type->is_primitive()) return std::nullopt;
    PrimitiveTypeKind k = target_type->get_primitive()->kind;

    if (k == PrimitiveTypeKind::Bool) return make_bool(v.truthy());

    if (k == PrimitiveTypeKind::Float || k == PrimitiveTypeKind::Double
        || k == PrimitiveTypeKind::Float16) {
        double d = v.is_float() ? v.f
                 : v.is_signed ? (double)v.i64()
                               : (double)v.u;
        return make_float(d, k);
    }

    bool is_signed = true; int bits = 8;
    if (k != PrimitiveTypeKind::Char &&
        !primitive_int_range(k, is_signed, bits)) return std::nullopt;

    unsigned long long val = v.u;
    if (v.is_float()) {
        double d = v.f;
        // 常量折叠按目标整数范围判界：越界或 NaN 转换在 C++ 里是未定义行为，
        // 直接转换会把长度等常量悄悄吞成 0。
        int range_bits = bits > 64 ? 64 : bits;
        bool in_range;
        if (is_signed) {
            double hi = std::ldexp(1.0, range_bits - 1);
            in_range = std::isfinite(d) && d >= -hi && d < hi;
        } else {
            double hi = std::ldexp(1.0, range_bits);
            in_range = std::isfinite(d) && d >= 0.0 && d < hi;
        }
        if (!in_range) {
            sink.error(loc, "constant floating-point value is out of range "
                            "for integer conversion");
            return std::nullopt;
        }
        val = is_signed ? (unsigned long long)(long long)d
                        : (unsigned long long)d;
    }
    if (bits < 64) val &= (1ull << bits) - 1;
    return make_int(val, is_signed, bits);
}

static std::optional<ConstValue> eval_binary(const Expr::Binary& b,
                                             const Location& loc,
                                             ConstEvalSink& sink,
                                             int depth) {
    if (b.op == OpKind::And) {
        auto l = eval_expr(b.left, sink, depth + 1);
        if (!l) return std::nullopt;
        if (!l->truthy()) return make_bool(false);
        auto r = eval_expr(b.right, sink, depth + 1);
        if (!r) return std::nullopt;
        return make_bool(r->truthy());
    }
    if (b.op == OpKind::Or) {
        auto l = eval_expr(b.left, sink, depth + 1);
        if (!l) return std::nullopt;
        if (l->truthy()) return make_bool(true);
        auto r = eval_expr(b.right, sink, depth + 1);
        if (!r) return std::nullopt;
        return make_bool(r->truthy());
    }

    auto l = eval_expr(b.left, sink, depth + 1);
    if (!l) return std::nullopt;
    auto r = eval_expr(b.right, sink, depth + 1);
    if (!r) return std::nullopt;

    // 浮点
    if (l->is_float() || r->is_float()) {
        double lf = l->is_float() ? l->f : (l->is_signed ? (double)l->i64() : (double)l->u);
        double rf = r->is_float() ? r->f : (r->is_signed ? (double)r->i64() : (double)r->u);
        PrimitiveTypeKind rk = PrimitiveTypeKind::Double;
        if (l->kind != PrimitiveTypeKind::Double && r->kind != PrimitiveTypeKind::Double) {
            rk = (l->kind == PrimitiveTypeKind::Float || r->kind == PrimitiveTypeKind::Float)
                 ? PrimitiveTypeKind::Float : PrimitiveTypeKind::Float16;
        }
        switch (b.op) {
            case OpKind::Plus:  return make_float(lf + rf, rk);
            case OpKind::Minus: return make_float(lf - rf, rk);
            case OpKind::Mul:   return make_float(lf * rf, rk);
            case OpKind::Div:
                if (rf == 0.0) { sink.error(loc, "constant division by zero"); return std::nullopt; }
                return make_float(lf / rf, rk);
            case OpKind::Equal:        return make_bool(lf == rf);
            case OpKind::NotEqual:     return make_bool(lf != rf);
            case OpKind::Less:         return make_bool(lf <  rf);
            case OpKind::Greater:      return make_bool(lf >  rf);
            case OpKind::LessEqual:    return make_bool(lf <= rf);
            case OpKind::GreaterEqual: return make_bool(lf >= rf);
            default: return std::nullopt;
        }
    }

    // 整数提升：同宽混合符号性按无符号；否则取大位宽
    bool is_signed = l->is_signed && r->is_signed;
    int bits = std::max(l->bits, r->bits);
    if (l->bits == r->bits && l->is_signed != r->is_signed) is_signed = false;

    auto cmp_int = [&](OpKind op) -> std::optional<ConstValue> {
        if (is_signed) {
            long long a = l->i64(), c = r->i64();
            switch (op) {
                case OpKind::Equal:        return make_bool(a == c);
                case OpKind::NotEqual:     return make_bool(a != c);
                case OpKind::Less:         return make_bool(a <  c);
                case OpKind::Greater:      return make_bool(a >  c);
                case OpKind::LessEqual:    return make_bool(a <= c);
                case OpKind::GreaterEqual: return make_bool(a >= c);
                default: return std::nullopt;
            }
        } else {
            unsigned long long a = l->u, c = r->u;
            switch (op) {
                case OpKind::Equal:        return make_bool(a == c);
                case OpKind::NotEqual:     return make_bool(a != c);
                case OpKind::Less:         return make_bool(a <  c);
                case OpKind::Greater:      return make_bool(a >  c);
                case OpKind::LessEqual:    return make_bool(a <= c);
                case OpKind::GreaterEqual: return make_bool(a >= c);
                default: return std::nullopt;
            }
        }
    };

    switch (b.op) {
        case OpKind::Equal: case OpKind::NotEqual:
        case OpKind::Less: case OpKind::Greater:
        case OpKind::LessEqual: case OpKind::GreaterEqual:
            return cmp_int(b.op);
        default: break;
    }

    unsigned long long mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    unsigned long long umax = mask;
    long long smin = 0, smax = 0;
    if (is_signed) {
        if (bits >= 64) {
            smin = LLONG_MIN;
            smax = LLONG_MAX;
        } else {
            smax = (long long)(umax >> 1);
            smin = -smax - 1;
        }
    }

    auto overflow_warn = [&]() {
        sink.warn(loc, "constant expression overflows");
    };

    switch (b.op) {
        case OpKind::Plus:
        case OpKind::Minus:
        case OpKind::Mul: {
            // 环绕是预期语义：一律用无符号运算得到低位，避免 C++ 有符号溢出
            unsigned long long res = 0;
            bool overflow = false;
            if (is_signed) {
                long long sl = l->i64(), sr = r->i64();
                long long rres = 0;
                if (b.op == OpKind::Plus)
                    overflow = __builtin_add_overflow(sl, sr, &rres);
                else if (b.op == OpKind::Minus)
                    overflow = __builtin_sub_overflow(sl, sr, &rres);
                else
                    overflow = __builtin_mul_overflow(sl, sr, &rres);
                if (!overflow && bits < 64 &&
                    (rres < smin || rres > smax)) overflow = true;
                res = (unsigned long long)rres & mask;
            } else {
                unsigned long long ul = l->u & mask, ur = r->u & mask;
                if (b.op == OpKind::Plus)
                    overflow = __builtin_add_overflow(ul, ur, &res);
                else if (b.op == OpKind::Minus)
                    overflow = __builtin_sub_overflow(ul, ur, &res);
                else
                    overflow = __builtin_mul_overflow(ul, ur, &res);
                if (!overflow && bits < 64 && res > umax) overflow = true;
                res &= mask;
            }
            if (overflow) overflow_warn();
            return make_int(res, is_signed, bits);
        }
        case OpKind::Div:
            if (is_signed) {
                long long a = l->i64(), c = r->i64();
                long long mn = bits >= 64 ? LLONG_MIN : -(1ll << (bits - 1));
                if (c == 0) { sink.error(loc, "constant division by zero"); return std::nullopt; }
                // INT_MIN / -1 在 C++ 里是未定义行为（x86 上直接 SIGFPE），
                // 目标语义也不接受这个溢出结果。
                if (a == mn && c == -1) {
                    sink.error(loc, "constant division overflows");
                    return std::nullopt;
                }
                return make_int((unsigned long long)(a / c) & mask, true, bits);
            }
            if ((r->u & mask) == 0) {
                sink.error(loc, "constant division by zero");
                return std::nullopt;
            }
            return make_int((l->u & mask) / (r->u & mask), false, bits);
        case OpKind::Mod:
            if (is_signed) {
                long long a = l->i64(), c = r->i64();
                if (c == 0) { sink.error(loc, "constant modulo by zero"); return std::nullopt; }
                if (bits >= 64 && a == LLONG_MIN && c == -1)
                    return make_int(0, true, bits);
                return make_int((unsigned long long)(a % c) & mask, true, bits);
            }
            if ((r->u & mask) == 0) {
                sink.error(loc, "constant modulo by zero");
                return std::nullopt;
            }
            return make_int((l->u & mask) % (r->u & mask), false, bits);
        case OpKind::Shl: {
            long long sh = r->i64();
            if (sh < 0 || sh >= bits) { sink.warn(loc, "shift amount out of range in constant expression"); return make_int(0, is_signed, bits); }
            return make_int(l->u << sh, is_signed, bits);
        }
        case OpKind::Shr: {
            long long sh = r->i64();
            if (sh < 0 || sh >= bits) { sink.warn(loc, "shift amount out of range in constant expression"); return make_int(0, is_signed, bits); }
            if (is_signed) return make_int((unsigned long long)(l->i64() >> sh), true, bits);
            return make_int(l->u >> sh, false, bits);
        }
        case OpKind::BitAnd: return make_int(l->u & r->u, is_signed, bits);
        case OpKind::BitOr:  return make_int(l->u | r->u, is_signed, bits);
        case OpKind::BitXor: return make_int(l->u ^ r->u, is_signed, bits);
        default:
            (void)umax;
            return std::nullopt;
    }
}

static std::optional<ConstValue> eval_expr(Expr* e, ConstEvalSink& sink, int depth) {
    if (!e || depth > 64) return std::nullopt;

    if (auto* lit = e->get_literal())     return eval_literal(*lit, e->type, e->loc, sink);
    if (auto* un  = e->get_unary())       return eval_unary(*un, e->loc, sink, depth);
    if (auto* bin = e->get_binary())      return eval_binary(*bin, e->loc, sink, depth);
    if (auto* ter = e->get_ternary()) {
        auto c = eval_expr(ter->cond, sink, depth + 1);
        if (!c) return std::nullopt;
        return eval_expr(c->truthy() ? ter->then_expr : ter->else_expr, sink, depth + 1);
    }
    if (auto* cast = e->get_cast()) {
        auto v = eval_expr(cast->operand, sink, depth + 1);
        if (!v) return std::nullopt;
        return eval_cast(*v, cast->target_type, sink, e->loc);
    }
    if (e->is_identifier() || e->is_member()) return eval_symbol_value(e, sink, depth);
    if (e->is_null()) return make_null();
    return std::nullopt;
}

std::optional<ConstValue> eval_const(Expr* e, ConstEvalSink& sink, int depth) {
    return eval_expr(e, sink, depth);
}

} // namespace cx
