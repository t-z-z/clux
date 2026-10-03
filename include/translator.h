#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "translator_mangle.h"

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
    Mangler mangler_;
    bool uses_alwaysinline_ = false;

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
        // 已绑定的引用变量：其槽存被引用对象的地址
        std::unordered_set<const VarItem*> ref_bound;
        // 引用变量 → 其绑定的作用域深度，离开作用域时遗忘绑定
        std::unordered_map<const VarItem*, int> ref_slots;
        bool term = false;
        std::vector<CleanupEntry> drops;
        int scope_depth = 0;
    };
    Ctx ctx_;
    std::vector<LoopCtx> loops_;
    int closure_counter_ = 0;

    void collect_classes();
    void collect_classes_decl(Decl* d);

    SpecialMethod* find_special(Class* cls, SpecialMethod::Kind k) const;
    Class*         pointee_class(Expr* t);
    void           emit_guard_call(SpecialMethod* sm, const std::string& val);
    void           emit_bind_ref(Class* cls, const std::string& val,
                                 SpecialMethod::Kind k);
    void           register_deref(const std::string& slot, Class* cls, int depth = -1);

    std::string map_type(Expr* t);

    void emit_instr(const std::string& s);
    void emit_term(const std::string& s);
    void open_block(const std::string& label);

    void emit_func(Func& f, Class* cls);
    void emit_special(SpecialMethod& sm, Class* cls);
    void emit_class(Class* c);
    void emit_drops_to(int depth);
    void           emit_scope_cleanup(int depth);

    void emit_stmt(Stmt* s);
    void emit_var_decl(VarList* vl);
    bool emit_class_var(VarList* vl, VarItem* vi, Class* cls);
    bool emit_array_noinit(VarItem* vi, Expr* vtype, const std::string& slot);
    bool emit_array_string_init(VarItem* vi, const std::string& slot);
    void emit_if(Stmt::If& i);
    void emit_while(Stmt::While& w);
    void emit_for(Stmt::For& f);
    void emit_return(Stmt::Return& r);
    void emit_break(Stmt::Break& b);
    void emit_continue(Stmt::Continue& c);
    void emit_flag(Stmt::Flag& f);
    void emit_goto(Stmt::Goto& g);

    std::string eval(Expr* e);
    std::string eval_converted(Expr* e);
    // 该表达式是否需要走 eval_converted（conversion 函数或包装转换）
    static bool needs_convert(Expr* e);
    // 包装类隐式转换的 IR：解包 -> 数值 cast -> 打包
    std::string emit_wrap_convert(Expr* e);
    // 类拷贝构造：新建实例并逐字段复制
    std::string emit_class_copy(Class* cls, const std::string& src);
    // 合成操作符的函数体 IR：解包 __value -> 运算 -> 打包返回
    void emit_synth_operator(Func& f, Class* cls, VarItem* ret_var);
    // 合成操作符的函数体 IR：解包 __value -> 运算 -> 打包返回
    Class* as_wrap_class(Expr* t);
    static std::string base_ir_of(PrimitiveTypeKind k);
    // 类实例的字节大小（供 @malloc 分配）
    size_t type_size(const std::string& llvm_name) const;
    std::string eval_cond(Expr* e);
    std::string to_bool(Expr* e, const std::string& v);
    std::string eval_lvalue(Expr* e);
    std::string eval_literal(Expr::Literal* lit);
    std::string eval_identifier(Expr* e, Expr::Identifier* id);
    std::string eval_binary(Expr* e, Expr::Binary* b);
    std::string eval_unary(Expr* e, Expr::Unary* u);
    std::string eval_assign(Expr* e, Expr::Assign* a);
    std::string eval_call(Expr* e, Expr::Call* c);
    std::string eval_free_builtin(Expr* e, Expr::Call* c);
    std::string eval_default_ctor(Expr* e, Expr::Call* c);
    std::string eval_ctor_call(Expr* e, Expr::Call* c, SpecialMethod* sm);
    std::string eval_plain_call(Expr* e, Expr::Call* c, Func* fn);
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
    bool is_ref_item(const VarItem* vi, Expr** target_out) const;
    bool is_ref_expr(Expr* e, Expr** target_out) const;
    std::string cast_value(const std::string& v,
                           const std::string& src, const std::string& dst,
                           bool src_signed = true, bool dst_signed = true);
    std::string emit_operator_call(Expr* e, Operator* op, Expr* lhs, Expr* rhs);
    std::string emit_conversion_call(Expr* e, Conversion* cf);
    void emit_declare(Func& f);
};

} // namespace cx
