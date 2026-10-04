#include "../include/ast.h"

namespace cx {

Expr* CompUnit::clone_type(Expr* t) {
    if (!t) return nullptr;
    Location loc = t->loc;
    if (auto* p = t->get_primitive()) return add_node<Expr>(loc, Expr::Primitive{p->kind});
    if (t->is_string_literal_type()) return add_node<Expr>(loc, Expr::StringLiteralType{});
    if (t->is_void()) return add_node<Expr>(loc, Expr::Void{});
    if (t->is_inferred()) return add_node<Expr>(loc, Expr::Inferred{});
    if (auto* p = t->get_pointer()) return add_node<Expr>(loc, Expr::Pointer{clone_type(p->pointee)});
    if (auto* r = t->get_reference()) return add_node<Expr>(loc, Expr::Reference{clone_type(r->target)});
    if (auto* a = t->get_array_type()) {
        return add_node<Expr>(loc, Expr::ArrayType{clone_type(a->element), clone_expr(a->length)});
    }
    if (auto* f = t->get_func_ptr()) {
        std::vector<Expr*> params;
        params.reserve(f->params.size());
        for (auto* p : f->params) params.push_back(clone_type(p));
        return add_node<Expr>(loc, Expr::FuncPtr{std::move(params), clone_type(f->return_type)});
    }
    if (t->is_type && (t->is_identifier() || t->is_member())) {
        Expr* n = nullptr;
        if (auto* id = t->get_identifier()) {
            n = add_node<Expr>(loc, Expr::Identifier{id->name});
        } else {
            auto* m = t->get_member();
            n = add_node<Expr>(loc, Expr::Member{clone_expr(m->target), m->member});
        }
        n->is_type = true;
        n->resolved_decl = t->resolved_decl;
        n->resolved_scope = t->resolved_scope;
        return n;
    }
    return add_node<Expr>(loc, t->data);
}

Expr* CompUnit::clone_expr(Expr* e) {
    if (!e) return nullptr;
    Location loc = e->loc;
    if (e->is_type_node()) return clone_type(e);
    if (auto* v = e->get_literal()) return add_node<Expr>(loc, Expr::Literal{*v});
    if (auto* v = e->get_identifier()) return add_node<Expr>(loc, Expr::Identifier{v->name});
    if (auto* v = e->get_binary()) {
        return add_node<Expr>(loc, Expr::Binary{
            clone_expr(v->left), v->op, clone_expr(v->right)});
    }
    if (auto* v = e->get_unary()) {
        return add_node<Expr>(loc, Expr::Unary{v->op, clone_expr(v->operand), v->is_postfix});
    }
    if (auto* v = e->get_assign()) {
        return add_node<Expr>(loc, Expr::Assign{clone_expr(v->target), v->op, clone_expr(v->value)});
    }
    if (auto* v = e->get_call()) {
        std::vector<Expr*> args;
        args.reserve(v->args.size());
        for (auto* a : v->args) args.push_back(clone_expr(a));
        return add_node<Expr>(loc, Expr::Call{clone_expr(v->callee), std::move(args)});
    }
    if (auto* v = e->get_ternary()) {
        return add_node<Expr>(loc, Expr::Ternary{
            clone_expr(v->cond), clone_expr(v->then_expr), clone_expr(v->else_expr)});
    }
    if (auto* v = e->get_index()) {
        return add_node<Expr>(loc, Expr::Index{clone_expr(v->target), clone_expr(v->index)});
    }
    if (auto* v = e->get_member()) {
        return add_node<Expr>(loc, Expr::Member{clone_expr(v->target), v->member});
    }
    if (auto* v = e->get_cast()) {
        return add_node<Expr>(loc, Expr::Cast{clone_type(v->target_type), clone_expr(v->operand)});
    }
    if (auto* v = e->get_array()) {
        std::vector<Expr*> members;
        members.reserve(v->members.size());
        for (auto* m : v->members) members.push_back(clone_expr(m));
        return add_node<Expr>(loc, Expr::Array{std::move(members), clone_expr(v->fill_value)});
    }
    if (auto* v = e->get_spawn()) {
        return add_node<Expr>(loc, Expr::Spawn{clone_expr(v->call)});
    }
    return add_node<Expr>(loc, e->data);
}

Stmt* CompUnit::clone_stmt(Stmt* s) {
    if (!s) return nullptr;
    Location loc = s->loc;
    if (auto* v = s->get_block()) {
        std::vector<Stmt*> stmts;
        stmts.reserve(v->stmts.size());
        for (auto* x : v->stmts) stmts.push_back(clone_stmt(x));
        return add_node<Stmt>(loc, Stmt::Block{std::move(stmts)});
    }
    if (auto* v = s->get_expr()) return add_node<Stmt>(loc, Stmt::ExprStmt{clone_expr(v->expr)});
    if (auto* v = s->get_if()) {
        return add_node<Stmt>(loc, Stmt::If{
            clone_expr(v->condition), clone_stmt(v->then_branch), clone_stmt(v->else_branch)});
    }
    if (auto* v = s->get_while()) {
        return add_node<Stmt>(loc, Stmt::While{clone_expr(v->condition), clone_stmt(v->body)});
    }
    if (auto* v = s->get_for()) {
        return add_node<Stmt>(loc, Stmt::For{
            clone_stmt(v->init), clone_expr(v->condition), clone_stmt(v->next), clone_stmt(v->body)});
    }
    if (s->is_return()) return add_node<Stmt>(loc, Stmt::Return{});
    if (s->is_break()) return add_node<Stmt>(loc, Stmt::Break{});
    if (s->is_continue()) return add_node<Stmt>(loc, Stmt::Continue{});
    if (auto* v = s->get_flag()) return add_node<Stmt>(loc, Stmt::Flag{v->label});
    if (auto* v = s->get_goto()) return add_node<Stmt>(loc, Stmt::Goto{v->label});
    if (auto* v = s->get_asm()) return add_node<Stmt>(loc, Stmt::Asm{v->code});
    if (auto* v = s->get_keep()) return add_node<Stmt>(loc, Stmt::Keep{v->tokens});
    if (auto* v = s->get_var_decl()) {
        return add_node<Stmt>(loc, Stmt::VarDecl{clone_varlist(v->vars)});
    }
    return add_node<Stmt>(loc, s->data);
}

VarList* CompUnit::clone_varlist(VarList* vl) {
    if (!vl) return nullptr;
    Expr* t = clone_type(vl->type);
    std::vector<VarItem*> vars;
    vars.reserve(vl->vars.size());
    for (auto* vi : vl->vars) {
        if (!vi) { vars.push_back(nullptr); continue; }
        vars.push_back(add_node<VarItem>(
            vi->loc, vi->name, clone_expr(vi->init), t));
    }
    return add_node<VarList>(vl->loc, vl->specs, t, std::move(vars));
}

Func* CompUnit::clone_func(Func* f) {
    std::vector<VarList*> params;
    params.reserve(f->params.size());
    for (auto* p : f->params) params.push_back(clone_varlist(p));
    std::vector<VarList*> returns;
    returns.reserve(f->returns.size());
    for (auto* r : f->returns) returns.push_back(clone_varlist(r));
    return add_node<Func>(f->loc, f->name, f->specs,
        std::move(params), std::move(returns), clone_stmt(f->body));
}

Decl* CompUnit::clone_decl(Decl* d) {
    if (!d) return nullptr;
    switch (d->kind) {
        case DeclKind::VarList:
            return clone_varlist(static_cast<VarList*>(d));
        case DeclKind::Func:
            return clone_func(static_cast<Func*>(d));
        case DeclKind::Operator: {
            auto* op = static_cast<Operator*>(d);
            Func* f = clone_func(op);
            auto* cloned = add_node<Operator>(op->loc, op->op, op->name,
                op->specs, f->params, f->returns, f->body);
            cloned->synthesized = op->synthesized;
            cloned->ret_via_reg = op->ret_via_reg;
            return cloned;
        }
        case DeclKind::SpecialMethod: {
            auto* sm = static_cast<SpecialMethod*>(d);
            std::vector<VarList*> params;
            params.reserve(sm->params.size());
            for (auto* p : sm->params) params.push_back(clone_varlist(p));
            return add_node<SpecialMethod>(sm->loc, sm->kind, sm->specs,
                clone_stmt(sm->body), std::move(params));
        }
        case DeclKind::Conversion: {
            auto* cf = static_cast<Conversion*>(d);
            std::vector<VarList*> params;
            for (auto* p : cf->params) params.push_back(clone_varlist(p));
            std::vector<VarList*> returns;
            for (auto* r : cf->returns) returns.push_back(clone_varlist(r));
            return add_node<Conversion>(cf->loc, cf->name, cf->specs,
                std::move(params), std::move(returns), clone_stmt(cf->body));
        }
        case DeclKind::Keep: {
            auto* k = static_cast<Keep*>(d);
            return add_node<Keep>(k->loc, k->code, k->specs);
        }
        case DeclKind::Class: {
            auto* c = static_cast<Class*>(d);
            std::vector<Decl*> members;
            members.reserve(c->members.size());
            for (auto* m : c->members) members.push_back(clone_decl(m));
            return add_node<Class>(c->loc, c->name, c->specs, std::move(members));
        }
        case DeclKind::Enum: {
            auto* e = static_cast<Enum*>(d);
            std::vector<EnumVariant*> variants;
            variants.reserve(e->variants.size());
            for (auto* v : e->variants) {
                variants.push_back(add_node<EnumVariant>(v->loc, v->name));
            }
            return add_node<Enum>(e->loc, e->name, e->specs, std::move(variants));
        }
        default:
            return d;
    }
}

}
