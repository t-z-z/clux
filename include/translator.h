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

// 字段类型取自 VarItem 或其所在 VarList，故解析后存在 Field 上统一使用
struct TranslatorField { VarItem* var; int index; Type* type; std::string ir_type; };
struct TranslatorClass {
    std::string llvm_name;
    std::vector<TranslatorField> fields;
    // 实例字段按 VarItem 身份索引；__value 是 inject 合成的承载字段
    std::unordered_map<const VarItem*, int> field_index;
    int value_index = -1;
    // static 字段：存储在模块级全局变量，不进实例 layout。
    // 其 LLVM 符号名由 Mangler::field_of 统一分配
    std::vector<TranslatorField> static_fields;
    // extern "LLVMIR" 块内的类：布局由 keep`` 的 IR 给出，译者为它发类型
    bool from_keep_ir = false;
};

// 跨模块共享的翻译期表。AST 里的 Decl* 在各模块之间是同一批对象，
// 引用别的模块声明的符号/类时必须查到真名与真布局，故整张表共用：
// Mangler::of 的兜底会退化成裸声明名，类查不到则实例直接变 null。
// 由前端统一建一份，各 Translator 共用。
struct TranslatorTables {
    Mangler mangler;
    // 模块级变量的 LLVM 全局符号名
    std::unordered_map<const VarItem*, std::string> global_names;
    std::unordered_map<Class*, TranslatorClass> classes;
    // 类按声明顺序登记：类结构体类型要按内联字段依赖拓扑发射，
    // unordered_map 的遍历顺序不稳定，故另存一份有序表
    std::vector<Class*> class_order;
};

class Translator {
public:
    Translator(CompUnit& cu, IRWriter& ir, TranslatorTables* shared = nullptr)
        : cu_(cu), ir_(ir), tables_(shared ? shared : &own_tables_),
          mangler_(tables_->mangler), classes_(tables_->classes),
          class_order_(tables_->class_order),
          global_names_(tables_->global_names) {}

    // 登记本单元的符号名与类布局。跨模块编译时须先对所有单元调用 collect
    // 再逐个 emit_class_decls / run
    void collect();
    // 全部单元的类型与 static 字段全局只发一次，须在所有 collect 之后、run 之前调
    void emit_class_decls();
    void run();
    void emit_decl(Decl* d);

    const std::vector<Diagnostic>& diagnostics() const { return diags_; }
    bool has_error() const { return has_error_; }

private:
    void error(const Location& loc, std::string msg);

    CompUnit& cu_;
    IRWriter& ir_;
    bool uses_alwaysinline_ = false;
    std::vector<Diagnostic> diags_;
    bool has_error_ = false;

    TranslatorTables own_tables_;
    TranslatorTables* tables_;
    Mangler& mangler_;
    std::unordered_map<Class*, TranslatorClass>& classes_;
    std::vector<Class*>& class_order_;
    std::unordered_map<const VarItem*, std::string>& global_names_;
    // 本单元声明的类：static 字段默认值只在本模块入口写一次
    std::vector<Class*> own_classes_;
    // 特殊方法内出现过裸 free() 的类：这些类的实例由钩子自行释放，
    // a.free() 触发完钩子后不再重复调用 free。类的发射早于所有函数，故此处可查
    std::unordered_set<Class*> self_free_cls_;

    struct GlobalVar {
        VarItem* var;
        std::string sym;      // LLVM 全局符号名（不含 @）
        std::string ir_type;
        bool is_extern = false;   // extern 声明：由外部库定义，此处只声明不初始化
        bool skip_emit = false;   // extern "LLVMIR"：实现由 keep`` 给出，两样都不发
    };
    // 顶层 / 命名空间 / extern 内的 VarList：模块级变量
    std::vector<GlobalVar> globals_;
    void collect_globals_decl(Decl* d, const std::string& prefix,
                              ExternLang lang = ExternLang::None);

    struct LoopCtx { std::string break_label, continue_label; int scope_depth; };
    struct CleanupEntry {
        // Drop/Deref: 类实例与指针变量的析构；Restore: 回收栈上的切片存储
        enum Kind { Drop, Deref, Restore };
        Kind        kind = Drop;
        std::string slot;         // Drop: 实例指针槽；Deref: 指针变量槽
        std::string drop_fn;      // Drop 用
        std::string ptr;          // Restore: llvm.stacksave 的返回值
        // Deref 用：从宿主实例逐级 getelementptr 到目标指针字段。
        // 空路径表示清理指针变量本身（槽里直接就是那个指针）
        std::vector<std::pair<std::string, int>> path;
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
        // 本函数内按 malloc 分配实例的类变量：赋值时副本同样要落在堆上，
        // 否则 free() 会拿到栈指针
        std::unordered_set<const VarItem*> malloc_vars;
        int scope_depth = 0;
        // 类返回：返回变量 r 即 sret 目标对象，r 的值是 %ret 指针
        const VarItem* ret_obj_var = nullptr;
        std::string ret_obj_ptr;
        Class* ret_obj_cls = nullptr;
        // 引用返回变量：对其赋值是绑定，存被引用对象的地址
        const VarItem* ret_ref_var = nullptr;
    };
    Ctx ctx_;
    std::vector<LoopCtx> loops_;
    // 翻译器合成的辅助函数编号：spawn 包装函数共用
    int aux_fn_counter_ = 0;

    void collect_classes();
    void collect_classes_decl(Decl* d, bool from_keep_ir = false);
    // 每个类的内联类字段所指向的类
    std::unordered_map<Class*, std::vector<Class*>> inline_deps() const;
    // 按内联字段依赖拓扑发射全部类结构体类型
    void emit_class_types();

    SpecialMethod* find_special(Class* cls, SpecialMethod::Kind k) const;
    Class*         pointee_class(Type* t);
    void           emit_guard_call(SpecialMethod* sm, const std::string& val);
    void           emit_bind_ref(Class* cls, const std::string& val,
                                 SpecialMethod::Kind k);
    void           register_deref(const std::string& slot, Class* cls, int depth = -1);
    // 对象销毁时清理其指针字段：字段指向的类有 DEREF 才登记。
    // 内联类字段逐级下钻，path 记录从宿主实例到目标指针字段的导航
    void           register_field_derefs(const std::string& slot, Class* cls,
                                         int depth = -1,
                                         std::vector<std::pair<std::string, int>> path = {});

    std::string map_type(Type* t);

    void emit_instr(const std::string& s);
    void emit_term(const std::string& s);
    void open_block(const std::string& label);
    // 函数体内的 keep：保留代码原样落进当前基本块
    void emit_keep(std::string_view code);

    void emit_func(Func& f, Class* cls);
    void emit_special(SpecialMethod& sm, Class* cls);
    void emit_class(Class* c);
    void emit_drops_to(int depth);

    void emit_stmt(Stmt* s);
    void emit_var_decl(VarList* vl);
    bool emit_class_var(VarList* vl, VarItem* vi, Class* cls);
    // 定长数组无初值：分配 backing 并以 {ptr,i32} 存入槽
    bool emit_array_noinit(VarItem* vi, Type* vtype, const std::string& slot);
    // 由指针构造定长数组：int[N] s = ptr
    bool emit_ptr_to_array(Type* vtype, Expr* ptr, const std::string& slot);
    bool emit_array_string_init(VarItem* vi, const std::string& slot);
    void emit_if(Stmt::If& i);
    void emit_while(Stmt::While& w);
    void emit_for(Stmt::For& f);
    void emit_return(Stmt::Return& r);
    void emit_break(Stmt::Break& b);
    void emit_continue(Stmt::Continue& c);
    void emit_label(Stmt::Label& f);
    void emit_goto(Stmt::Goto& g);

    std::string eval(Expr* e);
    std::string eval_converted(Expr* e);
    // 该表达式是否需要走 eval_converted（conversion 函数或包装转换）
    static bool needs_convert(Expr* e);
    // 包装类隐式转换的 IR：解包 -> 数值 cast -> 打包
    std::string emit_wrap_convert(Expr* e);
    // 类拷贝构造：新建实例并逐字段复制
    // heap 为真时副本分配在堆上并按 malloc 实例触发 REF
    std::string emit_class_copy(Class* cls, const std::string& src,
                                bool heap = false);
    std::string emit_malloc_inst(Class* cls);
    // 把 C(...) 的 INIT 就地发到已分配好的 slot 上
    void emit_ctor_into(Expr::Call* c, SpecialMethod* sm, Class* cls,
                        const std::string& slot);
    // 逐字段拷贝 src 所指对象到 dst 所指对象
    // copy_holds 为真表示复制出新的一份持有（指针字段各计一次 REF）；
    // 为假表示只是把已构造好的对象搬进 dst 所指存储（如返回值落到调用方
    // 的 sret 槽），不产生新的持有
    void emit_fields_copy(Class* cls, const std::string& src,
                          const std::string& dst, bool copy_holds);
    // 构造时为实例字段写入声明处默认值（INIT body 之前）
    void emit_field_defaults(Class* cls, const std::string& slot);
    // 模块入口为所有 static 字段写入声明处默认值
    void emit_static_inits();
    // 模块入口为所有模块级变量写入声明处默认值
    void emit_global_inits();
    // 写入单个字段的声明处默认值：引用字段的初值即绑定点，存地址而非值
    void emit_field_default(const std::string& dst_addr, const std::string& inst_addr,
                          Class* cls, const TranslatorField& f);
    // 字段初值绑定到本类另一字段时，从实例存储算它的地址
    std::string field_addr_in(Class* cls, const std::string& inst_addr, Expr* src);
    // 字段是否有需要写入的声明处默认值：无初值的引用字段写 null
    static bool field_has_default(const TranslatorField& f);
    // 合成操作符的函数体 IR：解包 __value -> 运算 -> 打包返回
    void emit_synth_operator(Func& f, Class* cls, VarItem* ret_var);
    // 合成操作符的函数体 IR：解包 __value -> 运算 -> 打包返回
    Class* as_wrap_class(Type* t);
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
    std::string call_func_ptr(Expr* e, Expr::Call* c, Type::FuncPtr* fp);
    int array_length_of(Type* arrtype);

    // 定长数组 T[N] 在 IR 里是真正的 [N x T]，值统一用存储地址表示，
    // 故 map_type 给 ptr；object_type 给出 alloca / load / store 用的真实类型
    bool is_fixed_array(Type* t) const;
    std::string object_type(Type* t);
    // 入口块 alloca：长度是编译期常量的存储（定长数组、编译期长度的切片），
    // 整个函数只分配一次，循环里复用同一块，不会逐轮吃栈
    std::string entry_alloca(const std::string& ir_type);
    // 运行期长度的存储：栈上 alloca，并用 stacksave/stackrestore 在作用域
    // 结束时回收（alloca 本身只在函数返回时释放，循环里会逐轮吃栈）
    std::string alloc_slice_storage(Type* elem_type, const std::string& count);
    // 数组/切片/字符串的长度：编译期已知返回常量文本，否则返回运行期值
    std::string length_value_of(Expr* e);
    // 拷贝 count 个元素（count 为 i64 值或整数字面量）
    void copy_elements(const std::string& dst, const std::string& src,
                       Type* elem_type, const std::string& count);
    // 两个长度取较小者：常量文本直接比，运行期部分用 icmp/select
    std::string min_count(const std::string& a, const std::string& b);
    std::string min_with_const(const std::string& v, long long k);
    // 把初值内容拷进已有存储，长度取目标与初值的较小者
    void copy_init_into(const std::string& backing, Type* elem_type,
                        const std::string& len64, Expr* init);
    // 定长数组初始化：把初值元素写入已分配的存储
    void emit_fixed_array_init(const std::string& slot, Type* elem_type, int n,
                               Expr* init);
    // 按目标数组类型求值：字面量按目标的元素类型构造
    std::string eval_as_array(Expr* e, Type* target);
    // 所有权切片初始化：按长度分配存储，拷贝初值内容，存入 {ptr,len}
    void emit_slice_init(VarList* vl, const std::string& slot, Type* vtype,
                         Expr* init);

    std::string data_ptr_of(Expr* e);
    bool is_ref_item(const VarItem* vi, Type** target_out) const;
    // 成员表达式的存储地址：static 字段为全局符号，实例字段为 getelementptr
    std::string field_addr_of(Expr* e);
    // 引用字段的槽存被引用对象的地址，取其 lvalue 需先取出该地址
    std::string field_lvalue(Expr* e, const std::string& slot_addr);
    // 字段读取：slot_addr 为字段槽地址，返回字段的值
    std::string eval_field_value(Expr* e, const std::string& slot_addr);
    // 类左值 = 类右值：副本另行分配存储后逐字段复制，源对象保持不变
    std::string assign_class_value(Expr* target, Expr* value);
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
