#include "../include/inject.h"

#include "../include/sem_types.h"
#include "../include/semantic.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace cx {

namespace {

// 模板深度：进入模板或模板体内的容器时 +1，返回路径不必各自维护
struct TemplateDepth {
    int& d;
    bool entered;
    TemplateDepth(int& depth, bool enter) : d(depth), entered(enter) {
        if (entered) ++d;
    }
    ~TemplateDepth() { if (entered) --d; }
};

std::vector<std::string> decl_key_names(const Decl* d) {
    std::vector<std::string> names;
    if (!d) return names;
    if (auto* vl = dynamic_cast<const VarList*>(d)) {
        for (auto* v : vl->vars) {
            if (v) names.push_back(std::string(v->name));
        }
        return names;
    }
    // 特殊方法没有名字：REF/DEREF/DROP 来自多个来源时全部保留，
    // 由钩子身份去重（见 expand_class），不参与按名字的覆盖
    if (d->kind == DeclKind::SpecialMethod) {
        auto* sm = static_cast<const SpecialMethod*>(d);
        // 基础类型派发的 INIT(T) 以承载类型作键：同一个类被重复展开时
        // 不会叠加多份同签名的合成构造
        if (sm->kind == SpecialMethod::Kind::Init && sm->synthesized) {
            Type* pt = nullptr;
            if (!sm->params.empty() && sm->params[0] &&
                !sm->params[0]->vars.empty()) {
                VarItem* vi = sm->params[0]->vars[0];
                pt = vi ? (vi->type ? vi->type : sm->params[0]->type) : nullptr;
            }
            return {"__INIT_" + describe_type(pt)};
        }
        return names;
    }
    if (!d->name.empty()) names.push_back(std::string(d->name));
    return names;
}

// 注入来源里哪些成员参与合并：INIT / conversion / operator 只有主类自身的
// 才起效；REF/DEREF/DROP 是生命周期钩子，所有来源的都生效。
// 基础类型来源派发的运算符（synthesized）必须合并，包装类靠它们参与运算；
// 派发的 INIT(T) 同理，包装类靠它用承载类型显式构造
bool merges_from_source(const Decl* d) {
    if (!d) return false;
    switch (d->kind) {
        case DeclKind::SpecialMethod: {
            auto* sm = static_cast<const SpecialMethod*>(d);
            return sm->kind != SpecialMethod::Kind::Init || sm->synthesized;
        }
        case DeclKind::Conversion:
            return false;
        case DeclKind::Operator:
            return static_cast<const Operator*>(d)->synthesized;
        default:
            return true;
    }
}

}

void InjectionExpander::expand_all() {
    Scope* root = symbols_.root();
    for (Decl* decl : cu_.top_decls) {
        expand_decl(decl, root);
    }
}


void InjectionExpander::expand_decl(Decl* decl, Scope* parent_scope) {
    if (!decl) return;
    switch (decl->kind) {
        case DeclKind::Class:
            expand_class(static_cast<Class*>(decl), parent_scope);
            break;
        case DeclKind::Namespace: {
            auto* ns = static_cast<Namespace*>(decl);
            Scope* ns_scope = ns->scope ? ns->scope : parent_scope;
            for (Decl* m : ns->members) {
                expand_decl(m, ns_scope);
            }
            break;
        }
        case DeclKind::Extern: {
            auto* ext = static_cast<Extern*>(decl);
            Scope* ext_scope = ext->scope ? ext->scope : parent_scope;
            for (Decl* m : ext->members) {
                expand_decl(m, ext_scope);
            }
            break;
        }
        default:
            break;
    }
}


Class* InjectionExpander::make_base_source(PrimitiveTypeKind base,
                                        const Location& loc, Class* main) {
    auto* synth = cu_.add_node<Class>(loc, "__base", SpecList{},
        std::vector<Decl*>{});
    synth->base_source = base;

    auto mk_type = [&]() {
        return cu_.add_node<Type>(loc, Type::Primitive{base});
    };
    // 派发的运算符返回主类而不是承载类型：num + 1 仍是 num，与类的值语义一致
    auto mk_main_type = [&]() {
        return cu_.add_node<Type>(loc, Type::Identifier{main->name});
    };
    auto mk_param = [&](std::string_view nm, Type* t) {
        auto* vi = cu_.add_node<VarItem>(loc, nm, nullptr, t);
        return cu_.add_node<VarList>(loc, SpecList{}, t,
            std::vector<VarItem*>{vi});
    };

    // 承载字段 __value：类布局首字段，不可直接访问
    Type* vt = mk_type();
    auto* vvi = cu_.add_node<VarItem>(loc, "__value", nullptr, vt);
    // 承载字段没有声明处初值：由包装/解包路径或派发的 INIT(T) 直接写入
    SpecList carrier_specs;
    carrier_specs.set(SpecKind::Uninit);
    synth->members.push_back(cu_.add_node<VarList>(loc, carrier_specs, vt,
        std::vector<VarItem*>{vvi}));

    // 模板体内的类还没有实例名，运算符返回类型无从落笔；实例化时会重新展开
    // 注入并生成完整成员，模板里只留承载字段与 base_source 标记
    if (template_depth_ > 0) return synth;

    // 构造：INIT(T) 把实参写进承载字段，包装类才能用承载类型显式构造
    auto* init = cu_.add_node<SpecialMethod>(loc, SpecialMethod::Kind::Init,
        SpecList{}, nullptr,
        std::vector<VarList*>{mk_param("value", mk_type())});
    init->synthesized = true;
    synth->members.push_back(init);

    bool is_bool = (base == PrimitiveTypeKind::Bool);

    // 二元运算：bool 只派发逻辑/位逻辑，数值类型派发算术/位/移位
    std::vector<OpKind> bin_ops;
    if (is_bool) {
        bin_ops = { OpKind::And, OpKind::Or,
                    OpKind::BitAnd, OpKind::BitOr, OpKind::BitXor };
    } else {
        bin_ops = { OpKind::Plus, OpKind::Minus, OpKind::Mul, OpKind::Div,
                    OpKind::Mod, OpKind::BitOr, OpKind::BitAnd, OpKind::BitXor,
                    OpKind::Shl, OpKind::Shr };
    }
    for (OpKind op : bin_ops) {
        auto* op_node = cu_.add_node<Operator>(loc, op, opkind_to_sv(op),
            SpecList{},
            std::vector<VarList*>{mk_param("other", mk_type())},
            std::vector<VarList*>{mk_param("r", mk_main_type())},
            nullptr);
        op_node->synthesized = true;
        synth->members.push_back(op_node);
    }

    // 二元比较：returns=[bool]
    std::vector<OpKind> cmp_ops = { OpKind::Equal, OpKind::NotEqual };
    if (!is_bool) {
        cmp_ops.push_back(OpKind::Less);
        cmp_ops.push_back(OpKind::Greater);
        cmp_ops.push_back(OpKind::LessEqual);
        cmp_ops.push_back(OpKind::GreaterEqual);
    }
    for (OpKind op : cmp_ops) {
        Type* bt = cu_.add_node<Type>(loc, Type::Primitive{PrimitiveTypeKind::Bool});
        auto* op_node = cu_.add_node<Operator>(loc, op, opkind_to_sv(op),
            SpecList{},
            std::vector<VarList*>{mk_param("other", mk_type())},
            std::vector<VarList*>{mk_param("r", bt)},
            nullptr);
        op_node->synthesized = true;
        op_node->ret_via_reg = true;
        synth->members.push_back(op_node);
    }

    // 复合赋值运算符：原地修改 self.__value，无返回值
    std::vector<OpKind> assign_ops;
    if (is_bool) {
        assign_ops = { OpKind::AndAssign, OpKind::OrAssign, OpKind::XorAssign };
    } else {
        assign_ops = { OpKind::PlusAssign, OpKind::MinusAssign, OpKind::MulAssign,
                       OpKind::DivAssign, OpKind::ModAssign,
                       OpKind::AndAssign, OpKind::OrAssign, OpKind::XorAssign,
                       OpKind::ShlAssign, OpKind::ShrAssign };
    }
    for (OpKind op : assign_ops) {
        auto* op_node = cu_.add_node<Operator>(loc, op, opkind_to_sv(op),
            SpecList{},
            std::vector<VarList*>{mk_param("other", mk_type())},
            std::vector<VarList*>{},
            nullptr);
        op_node->synthesized = true;
        synth->members.push_back(op_node);
    }

    // 一元操作符：params=[]（self 隐含）
    auto make_unary = [&](OpKind op, Type* rt, bool via_reg) {
        auto* op_node = cu_.add_node<Operator>(loc, op, opkind_to_sv(op),
            SpecList{},
            std::vector<VarList*>{},
            std::vector<VarList*>{mk_param("r", rt)},
            nullptr);
        op_node->synthesized = true;
        op_node->ret_via_reg = via_reg;
        synth->members.push_back(op_node);
    };
    if (is_bool) {
        Type* bt = cu_.add_node<Type>(loc, Type::Primitive{PrimitiveTypeKind::Bool});
        make_unary(OpKind::Not, bt, true);
    } else {
        make_unary(OpKind::Minus, mk_main_type(), false);
        Type* bt = cu_.add_node<Type>(loc, Type::Primitive{PrimitiveTypeKind::Bool});
        make_unary(OpKind::Not, bt, true);
        make_unary(OpKind::BitNot, mk_main_type(), false);
    }

    return synth;
}


void InjectionExpander::expand_class(Class* cls, Scope* parent_scope) {
    if (!cls) return;

    auto it = expand_state_.find(cls);
    if (it != expand_state_.end()) {
        if (it->second == ExpandState::InProgress) {
            error_(cls->loc, "injection cycle detected involving class '" +
                  std::string(cls->name) + "'");
        }
        return;
    }

    // 模板体内的注入（模板自身与嵌在模板里的类）等实例化后各展开一次：
    // 实参可能引用本类的泛型形参，此刻还没有对应类型
    TemplateDepth depth(template_depth_, is_generic_decl(cls));

    // 类内部的类未在 P0 注册，这里补注册类名与 scope
    if (!cls->scope && parent_scope) {
        std::string name(cls->name);
        Symbol* existing = parent_scope->lookup_member(name);
        Scope* cls_scope = parent_scope->get_or_create_child(name);
        if (existing) {
            error_(cls->loc, "redefinition of class '" + name + "'");
        } else {
            Symbol sym(SymbolKind::Class, cls);
            sym.specs = &cls->specs;
            sym.scope = cls_scope;
            sym.node = cls;
            parent_scope->insert_member(name, std::move(sym));
        }
        cls->scope = cls_scope;
    }

    expand_state_[cls] = ExpandState::InProgress;

    // 来源解析不进入类自身 scope，从所在 scope 的父链查找
    Scope* search_scope = cls->scope ? cls->scope->parent : nullptr;

    // 按名字取注入来源的类，非类来源报错
    auto lookup_class = [&](Type* mod, Symbol** sym_out) -> Class* {
        if (sym_out) *sym_out = nullptr;
        auto* id = mod ? mod->get_identifier() : nullptr;
        if (!id) {
            if (mod) error_(mod->loc, "cannot inject from '" +
                             type_name_string(mod) + "': not a class");
            return nullptr;
        }
        Symbol* sym = SymbolTable::lookup_member_up(search_scope, std::string(id->name));
        Decl* d = sym ? sym->get_decl() : nullptr;
        if (!d || d->kind != DeclKind::Class) {
            error_(mod->loc, "cannot inject from '" + std::string(id->name) +
                  "': not a class");
            return nullptr;
        }
        if (sym_out) *sym_out = sym;
        return static_cast<Class*>(d);
    };

    auto resolve = [&](Type* mod) -> Class* {
        if (!mod) return nullptr;
        if (auto* p = mod->get_primitive())
            return make_base_source(p->kind, mod->loc, cls);
        if (auto* g = mod->get_generic_type()) {
            // 模板体内的实参还是形参名，此处解析不出类型，交给实例化时结算
            if (template_depth_ > 0) return nullptr;
            Symbol* sym = nullptr;
            Class* tmpl = lookup_class(g->base, &sym);
            if (!tmpl) return nullptr;
            if (!is_generic_decl(tmpl)) {
                error_(mod->loc, "cannot inject from '" + std::string(tmpl->name) +
                      "': it is not generic, drop '-<...>'");
                return nullptr;
            }
            if (!generic_source_) return nullptr;
            return generic_source_(*tmpl, g->args, sym, search_scope, mod->loc);
        }
        Class* c = lookup_class(mod, nullptr);
        // 泛型类必须给类型实参，否则来源无从确定
        if (c && is_generic_decl(c)) {
            error_(mod->loc, "cannot inject from generic '" + std::string(c->name) +
                  "': write '" + std::string(c->name) + "-<...>'");
            return nullptr;
        }
        return c;
    };

    // 来源按优先级从高到低：前置注入（左→右）、基类自身、尾随注入（左→右）
    std::vector<Class*> pre_sources;
    for (Type* mod : cls->injects) {
        if (Class* c = resolve(mod)) {
            expand_class(c, c->scope ? c->scope->parent : nullptr);
            pre_sources.push_back(c);
        }
    }
    std::vector<Class*> post_sources;
    for (Type* mod : cls->trailing_injects) {
        if (Class* c = resolve(mod)) {
            expand_class(c, c->scope ? c->scope->parent : nullptr);
            post_sources.push_back(c);
        }
    }

    std::vector<Class*> sources;
    for (Class* c : pre_sources) sources.push_back(c);
    sources.push_back(cls);
    for (Class* c : post_sources) sources.push_back(c);

    // malloc 类的 free 是内置方法，且是唯一的堆释放入口：只有它会调用
    // @free，成员 free 覆盖它就等于让实例无法释放。因此无条件让内置
    // free 胜出。被丢弃的 free 分两种情形：
    //  - 尾随注入（#mod 在类名之后）优先级本就低于类自身，spec 中
    //    "malloc rcInt #rc" 正是靠这个顺序让内置 free 覆盖 rc 的 free，
    //    属预期行为，不告警；
    //  - 前置注入（#mod 在类名之前）或类自身声明，优先级高于内置 free，
    //    作者大概无意让空实现盖掉释放路径，告警提示。
    bool is_malloc = cls->specs.has(SpecKind::Malloc);
    if (is_malloc) {
        for (size_t si = 0; si < sources.size(); ++si) {
            Class* src = sources[si];
            bool overrides_builtin = src == cls || si < pre_sources.size();
            if (!overrides_builtin) continue;
            for (Decl* m : src->members) {
                if (!m) continue;
                std::vector<std::string> keys = decl_key_names(m);
                if (std::find(keys.begin(), keys.end(), "free") == keys.end())
                    continue;
                warn_(m->loc,
                      src == cls
                          ? std::string("'free' is a built-in method of malloc "
                                        "classes; the declaration is ignored and "
                                        "the built-in free is used")
                          : "'free' of '" + std::string(src->name) +
                            "' is injected before '" +
                            std::string(cls->name) +
                            "' and would override its built-in free; the "
                            "built-in free is used instead");
                break;
            }
        }
    }

    // 主类里 no 修饰的操作符即使优先级低于前置注入，也不能被基础类型自动
    // 派发的同名操作符覆盖
    std::unordered_set<std::string> no_keys;
    for (Decl* m : cls->members) {
        if (!m || !m->specs.has(SpecKind::No)) continue;
        for (const std::string& n : decl_key_names(m)) no_keys.insert(n);
    }

    // 同一 Class 被不同模块的 expander 各展开一次是安全的：合并按名字去重，
    // 第二遍的 sources 已含首遍的合并结果，index 占位后成员集合不再变化。
    // 钩子没有名字，另按最初声明去重
    std::vector<Decl*> result;
    std::unordered_map<std::string, size_t> index;
    std::unordered_map<const SpecialMethod*, size_t> hook_index;
    for (Class* src : sources) {
        bool from_main = (src == cls);
        for (Decl* m : src->members) {
            if (!m) continue;
            if (!from_main && !merges_from_source(m)) continue;
            bool claimed = false;
            for (const std::string& n : decl_key_names(m)) {
                if (is_malloc && n == "free") { claimed = true; break; }
                if (!from_main && !no_keys.empty() &&
                    m->kind == DeclKind::Operator &&
                    static_cast<Operator*>(m)->synthesized &&
                    no_keys.count(n)) {
                    claimed = true;
                    break;
                }
                if (index.count(n)) { claimed = true; break; }
            }
            if (claimed) continue;
            // 基类自身成员保持原指针，注入来源的成员克隆，避免共享 AST 节点
            Decl* placed = from_main ? m : cu_.clone_decl(m);
            if (placed && placed->kind == DeclKind::SpecialMethod) {
                auto* hook = static_cast<SpecialMethod*>(placed);
                if (hook->kind != SpecialMethod::Kind::Init) {
                    if (!hook->origin) hook->origin = hook;
                    auto [it, inserted] =
                        hook_index.emplace(hook->origin, result.size());
                    if (!inserted) {
                        // 重复展开：用主类成员表里的克隆件顶替来源类的原件
                        result[it->second] = hook;
                        continue;
                    }
                }
            }
            for (const std::string& n : decl_key_names(placed)) {
                index[n] = result.size();
            }
            result.push_back(placed);
        }
    }
    cls->members = std::move(result);

    // 基础类型来源标记传播：取第一个非 Void 的来源
    if (cls->base_source == PrimitiveTypeKind::Void) {
        for (Class* src : sources) {
            if (src->base_source != PrimitiveTypeKind::Void) {
                cls->base_source = src->base_source;
                break;
            }
        }
    }

    if (cls->scope) {
        symbols_.push_scope(std::string(cls->name));
        for (Decl* m : cls->members) {
            collect_(m, false);
        }
        symbols_.pop_scope();

        // 成员里的容器递归展开
        for (Decl* m : cls->members) {
            if (!m) continue;
            if (m->kind == DeclKind::Class) {
                expand_class(static_cast<Class*>(m), cls->scope);
            } else if (m->kind == DeclKind::Namespace ||
                       m->kind == DeclKind::Extern) {
                expand_decl(m, cls->scope);
            }
        }
    }

    expand_state_[cls] = ExpandState::Done;
}

} // namespace cx
