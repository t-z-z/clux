#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "diagnostic.h"
#include "translator_mangle.h"

namespace cx {

class IRWriter;

class Translator {
public:
    Translator(CompUnit& cu, IRWriter& ir) : cu_(cu), ir_(ir) {}

    void run();
    void emit_decl(Decl* d);

    const std::vector<Diagnostic>& diagnostics() const { return diags_; }
    bool has_error() const { return has_error_; }

private:
    void error(const Location& loc, std::string msg);

    CompUnit& cu_;
    IRWriter& ir_;
    Mangler mangler_;
    bool uses_alwaysinline_ = false;
    std::vector<Diagnostic> diags_;
    bool has_error_ = false;

    struct Field { VarItem* var; int index; std::string ir_type; };
    struct ClassInfo {
        std::string llvm_name;
        // 存放实例字节数的全局常量，由 LLVM 常量表达式算出真实布局
        std::string size_global;
        std::vector<Field> fields;
        std::unordered_map<std::string, int> field_index;
        // static 字段：存储在模块级全局变量，不进实例 layout
        std::vector<Field> static_fields;
        std::unordered_map<const VarItem*, std::string> static_global_of;
        std::unordered_map<std::string, std::string> static_global_by_name;
    };
    std::unordered_map<Class*, ClassInfo> classes_;
    // static 字段变量 → 模块级全局符号名
    std::unordered_map<const VarItem*, std::string> static_global_;

    struct GlobalVar {
        VarItem* var;
        std::string sym;      // LLVM 全局符号名（不含 @）
        std::string ir_type;
        bool is_extern = false;   // extern 声明：由外部库定义，此处只声明不初始化
    };
    // 顶层 / 命名空间 / extern 内的 VarList：模块级变量
    std::vector<GlobalVar> globals_;
    std::unordered_map<const VarItem*, std::string> global_of_;
    void collect_globals_decl(Decl* d, const std::string& prefix,
                              bool is_extern = false);

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
        // 正在生成 INIT 方法体：此处的引用字段赋值是绑定，其余位置是写穿
        bool in_init = false;
        std::string ret_type;
        std::string exit_label;
        std::unordered_map<const VarItem*, std::string> slots;
        // 引用变量与引用字段：其槽存被引用对象的地址
        std::unordered_set<const VarItem*> ref_bound;
        bool term = false;
        // 无返回变量的 main：签名是 i32 但体内按 void 走，裸 return 需补 i32 0
        bool plain_main = false;
        std::vector<CleanupEntry> drops;
        int scope_depth = 0;
        // 类返回：返回变量 r 即 sret 目标对象，r 的值是 %ret 指针
        const VarItem* ret_obj_var = nullptr;
        std::string ret_obj_ptr;
        Class* ret_obj_cls = nullptr;
    };
    Ctx ctx_;
    std::vector<LoopCtx> loops_;
    // 翻译器合成的辅助函数编号：spawn 包装函数共用
    int aux_fn_counter_ = 0;

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

    void emit_stmt(Stmt* s);
    void emit_var_decl(VarList* vl);
    bool emit_class_var(VarList* vl, VarItem* vi, Class* cls);
    // 定长数组无初值：分配 backing 并以 {ptr,i32} 存入槽
    bool emit_array_noinit(VarItem* vi, Expr* vtype, const std::string& slot);
    // 由指针构造定长数组：int[N] s = ptr
    bool emit_ptr_to_array(Expr* vtype, Expr* ptr, const std::string& slot);
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
    // 逐字段拷贝 src 所指对象到 dst 所指对象
    void emit_fields_copy(Class* cls, const std::string& src,
                          const std::string& dst);
    // 构造时为实例字段写入声明处默认值（INIT body 之前）
    void emit_field_defaults(Class* cls, const std::string& slot);
    // 模块入口为所有 static 字段写入声明处默认值
    void emit_static_inits();
    // 模块入口为所有模块级变量写入声明处默认值
    void emit_global_inits();
    // 写入单个字段的声明处默认值：引用字段的初值即绑定点，存地址而非值
    void emit_field_default(const std::string& dst_addr, const Field& f);
    // 字段是否有需要写入的声明处默认值：无初值的引用字段写 null
    static bool field_has_default(const Field& f);
    // 合成操作符的函数体 IR：解包 __value -> 运算 -> 打包返回
    void emit_synth_operator(Func& f, Class* cls, VarItem* ret_var);
    // 合成操作符的函数体 IR：解包 __value -> 运算 -> 打包返回
    Class* as_wrap_class(Expr* t);
    static std::string base_ir_of(PrimitiveTypeKind k);
    // 类实例的字节大小（供 @malloc 分配）
    std::string emit_size_of(const std::string& llvm_name);
    // 单个元素的字节数
    static long type_bytes_of(const std::string& t);
    std::string eval_cond(Expr* e);
    std::string to_bool(Expr* e, const std::string& v);
    std::string eval_lvalue(Expr* e);
    // 被引用对象的地址：引用绑定、引用形参传参、取地址用
    std::string eval_ref_addr(Expr* e);
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
    std::string eval_spawn(Expr* e, Expr::Spawn* s);
    std::string call_func_ptr(Expr* e, Expr::Call* c, Expr::FuncPtr* fp);
    int array_length_of(Expr* arrtype);

    std::string data_ptr_of(Expr* e);
    bool is_ref_item(const VarItem* vi, Expr** target_out) const;
    // 成员表达式的存储地址：static 字段为全局符号，实例字段为 getelementptr
    std::string field_addr_of(Expr* e);
    // 引用字段的槽存被引用对象的地址，取其 lvalue 需先取出该地址
    std::string field_lvalue(Expr* e, const std::string& slot_addr);
    // 引用字段绑定：仅 INIT 内对字段的直接赋值是绑定，返回被绑定对象的地址
    std::string bind_ref_field(Expr* target, Expr* value);
    std::string cast_value(const std::string& v,
                           const std::string& src, const std::string& dst,
                           bool src_signed = true, bool dst_signed = true);
    std::string emit_operator_call(Expr* e, Operator* op, Expr* lhs, Expr* rhs);
    std::string emit_conversion_call(Expr* e, Conversion* cf);
    void emit_declare(Func& f);
};

} // namespace cx
