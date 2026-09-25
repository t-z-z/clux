#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ast.h"

namespace cx {

class IRWriter;

class Translator {
public:
    Translator(CompUnit& cu, IRWriter& ir) : cu_(cu), ir_(ir) {}

    void run();
    void emit_decl(Decl* d);

private:
    CompUnit& cu_;
    IRWriter& ir_;
    std::unordered_map<Decl*, std::string> mangled_;

    struct Field { VarItem* var; int index; std::string ir_type; };
    struct ClassInfo {
        std::string llvm_name;
        std::vector<Field> fields;
        std::unordered_map<std::string, int> field_index;
    };
    std::unordered_map<Class*, ClassInfo> classes_;

    struct LoopCtx { std::string break_label, continue_label; int scope_depth; };
    struct CleanupEntry {
        enum Kind { Drop, Deref };
        Kind        kind = Drop;
        std::string slot;         // Drop: 实例指针槽；Deref: 指针变量槽
        std::string drop_fn;      // Drop 用
        Class*      cls = nullptr; // Deref 用
        int         scope_depth = 0;
    };

    struct Ctx {
        Func* fn = nullptr;
        Class* current_class = nullptr;
        std::string self_ptr;
        std::string ret_type;
        std::string exit_label;
        std::unordered_map<const VarItem*, std::string> slots;
        bool term = false;
        std::vector<CleanupEntry> drops;
        int scope_depth = 0;
    };
    Ctx ctx_;
    std::vector<LoopCtx> loops_;
    int closure_counter_ = 0;

    void collect_mangled();
    void walk_mangled(Decl* d, const std::string& prefix);
    void collect_classes();
    void collect_classes_decl(Decl* d);
    std::string mangled_of(Decl* d) const;
    std::string special_name(SpecialMethod* sm);
    std::string conversion_name(Conversion* cf);
    std::string type_key(Expr* t);

    SpecialMethod* find_special(Class* cls, SpecialMethod::Kind k) const;
    Class*         pointee_class(Expr* t);
    void           emit_guard_call(SpecialMethod* sm, const std::string& val);
    void           emit_bind_ref(Class* cls, const std::string& val,
                                 SpecialMethod::Kind k);
    void           register_deref(const std::string& slot, Class* cls);

    std::string map_type(Expr* t);
    Class* as_class(Expr* t);

    void emit_instr(const std::string& s);
    void emit_term(const std::string& s);
    void open_block(const std::string& label);

    void emit_func(Func& f, Class* cls);
    void emit_special(SpecialMethod& sm, Class* cls);
    void emit_class(Class* c);
    void emit_drops_to(int depth);

    void emit_stmt(Stmt* s);
    void emit_var_decl(VarList* vl);
    void emit_if(Stmt::If& i);
    void emit_while(Stmt::While& w);
    void emit_for(Stmt::For& f);
    void emit_return(Stmt::Return& r);
    void emit_break(Stmt::Break& b);
    void emit_continue(Stmt::Continue& c);
    void emit_flag(Stmt::Flag& f);
    void emit_goto(Stmt::Goto& g);

    std::string eval(Expr* e);
    std::string eval_cond(Expr* e);
    std::string to_bool(Expr* e, const std::string& v);
    std::string eval_lvalue(Expr* e);
    std::string eval_literal(Expr::Literal* lit);
    std::string eval_identifier(Expr* e, Expr::Identifier* id);
    std::string eval_binary(Expr* e, Expr::Binary* b);
    std::string eval_unary(Expr* e, Expr::Unary* u);
    std::string eval_assign(Expr::Assign* a);
    std::string eval_call(Expr* e, Expr::Call* c);
    std::string eval_index(Expr* e, Expr::Index* ix);
    std::string eval_member(Expr* e, Expr::Member* m);
    std::string eval_array(Expr* e, Expr::Array* arr);
    std::string build_string_slice(Expr* lit);
    std::string eval_cast(Expr* e, Expr::Cast* c);
    std::string eval_ternary(Expr* e, Expr::Ternary* t);
    std::string eval_closure(Expr* e, Expr::Closure* c);
    std::string eval_spawn(Expr* e, Expr::Spawn* s);
    std::string emit_closure_fn(Expr::Closure* c);
    std::string call_func_ptr(Expr* e, Expr::Call* c, Expr::FuncPtr* fp);
    int array_length_of(Expr* arrtype);

    std::string data_ptr_of(Expr* e);
    std::string cast_value(const std::string& v,
                           const std::string& src, const std::string& dst);
    std::string emit_operator_call(Expr* e, Operator* op, Expr* lhs, Expr* rhs);
    std::string emit_conversion_call(Expr* e, Conversion* cf);
    void emit_declare(Func& f);
};

} // namespace cx
