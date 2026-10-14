#include "../include/inject.h"

#include "../include/sem_types.h"
#include "../include/semantic.h"

#include <algorithm>
#include <string>
#include <vector>

namespace cx {

namespace {
std::vector<std::string> decl_key_names(const Decl* d) {
    std::vector<std::string> names;
    if (!d) return names;
    if (auto* vl = dynamic_cast<const VarList*>(d)) {
        for (auto* v : vl->vars) {
            if (v) names.push_back(std::string(v->name));
        }
        return names;
    }
    if (auto* sm = dynamic_cast<const SpecialMethod*>(d)) {
        switch (sm->kind) {
            case SpecialMethod::Kind::Drop:   names.push_back("DROP"); break;
            case SpecialMethod::Kind::Ref:    names.push_back("REF"); break;
            case SpecialMethod::Kind::Deref:  names.push_back("DEREF"); break;
            default: break;
        }
        return names;
    }
    if (!d->name.empty()) names.push_back(std::string(d->name));
    return names;
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
                                        const Location& loc) {
    auto* synth = cu_.add_node<Class>(loc, "__base", SpecList{},
        std::vector<Decl*>{});
    synth->base_source = base;

    auto mk_type = [&]() {
        return cu_.add_node<Type>(loc, Type::Primitive{base});
    };
    auto mk_param = [&](std::string_view nm, Type* t) {
        auto* vi = cu_.add_node<VarItem>(loc, nm, nullptr, t);
        return cu_.add_node<VarList>(loc, SpecList{}, t,
            std::vector<VarItem*>{vi});
    };

    // 承载字段 __value：类布局首字段，不可直接访问
    Type* vt = mk_type();
    auto* vvi = cu_.add_node<VarItem>(loc, "__value", nullptr, vt);
    synth->members.push_back(cu_.add_node<VarList>(loc, SpecList{}, vt,
        std::vector<VarItem*>{vvi}));

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
            std::vector<VarList*>{mk_param("r", mk_type())},
            nullptr);
        op_node->synthesized = true;
        op_node->ret_via_reg = true;
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
    auto make_unary = [&](OpKind op, PrimitiveTypeKind ret_kind) {
        Type* rt = cu_.add_node<Type>(loc, Type::Primitive{ret_kind});
        auto* op_node = cu_.add_node<Operator>(loc, op, opkind_to_sv(op),
            SpecList{},
            std::vector<VarList*>{},
            std::vector<VarList*>{mk_param("r", rt)},
            nullptr);
        op_node->synthesized = true;
        op_node->ret_via_reg = true;
        synth->members.push_back(op_node);
    };
    if (is_bool) {
        make_unary(OpKind::Not, PrimitiveTypeKind::Bool);
    } else {
        make_unary(OpKind::Minus, base);
        make_unary(OpKind::Not, PrimitiveTypeKind::Bool);
        make_unary(OpKind::BitNot, base);
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

    auto resolve = [&](Type* mod) -> Class* {
        if (!mod) return nullptr;
        if (auto* p = mod->get_primitive())
            return make_base_source(p->kind, mod->loc);
        // `#foo-<int>` 解析成泛型应用；泛型注入尚未实现，宁可报错也不静默丢弃
        if (mod->is_generic_type()) {
            error_(mod->loc, "cannot inject from generic '" +
                  type_name_string(mod->get_generic_type()->base) +
                  "': generics are not supported yet");
            return nullptr;
        }
        auto* id = mod->get_identifier();
        if (!id) return nullptr;
        Symbol* sym = SymbolTable::lookup_member_up(search_scope, std::string(id->name));
        Decl* d = sym ? sym->get_decl() : nullptr;
        if (!d || d->kind != DeclKind::Class) {
            error_(mod->loc, "cannot inject from '" + std::string(id->name) +
                  "': not a class");
            return nullptr;
        }
        return static_cast<Class*>(d);
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

    // 同一 Class 被不同模块的 expander 各展开一次是安全的：合并按名字去重，
    // 第二遍的 sources 已含首遍的合并结果，index 占位后成员集合不再变化
    std::vector<Decl*> result;
    std::unordered_map<std::string, size_t> index;
    for (Class* src : sources) {
        for (Decl* m : src->members) {
            if (!m) continue;
            bool claimed = false;
            for (const std::string& n : decl_key_names(m)) {
                if (is_malloc && n == "free") { claimed = true; break; }
                if (index.count(n)) { claimed = true; break; }
            }
            if (claimed) continue;
            // 基类自身成员保持原指针，注入来源的成员克隆，避免共享 AST 节点
            Decl* placed = (src == cls) ? m : cu_.clone_decl(m);
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
