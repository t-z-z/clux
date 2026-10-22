#pragma once

#include <vector>
#include <utility>
#include <string>

#include "token.h"
#include "ast.h"
#include "lexer.h"
#include "diagnostic.h"

namespace cx {

class Parser {
public:
    Parser(Lexer &lexer, CompUnit &cu, StringPool &pool): lexer_(lexer), cu_(cu), pool_(pool) {
        dq_.push_back(get_token());
    } 
    CompUnit* parse();
    inline bool has_error() const { return !errors_.empty(); }
    const std::vector<Diagnostic>& errors() const { return errors_; }
    inline bool has_warning() const { return !warnings_.empty(); }
    const std::vector<Diagnostic>& warnings() const { return warnings_; }
    inline const Location& get_loc()  { return curr().loc; }
private:
    Lexer &lexer_;
    CompUnit &cu_;
    std::vector<Diagnostic> errors_;
    std::vector<Diagnostic> warnings_;
    // token 缓冲与游标。解析期间只追加、不删除已读入的 token，
    // 因此 point/restore 永远有效：回退只是把游标移回去。
    std::deque<Token> dq_;
    size_t walk_pos_ = 0;
    // 探针深度：>0 时只做前瞻，不产出诊断
    size_t probe_ = 0;
    StringPool& pool_;
    struct DefineDecl{
        std::string_view name;
        std::vector<Token> alias;
        // 覆盖者的位置
        size_t cvr_lv = ~size_t(0);
        size_t cvr_id = ~size_t(0);
    };
    // define 替换的作用域层级
    std::vector<std::vector<DefineDecl>> defines_;
    // 类体嵌套深度：extern 块只允许出现在顶层与命名空间里
    size_t class_depth_ = 0;
    // define 替换缓冲；泛型闭合符裂解后的余部也排在这里
    std::deque<Token> pending_;
    bool use_define_ = true;
    // 泛型嵌套深度：'-<' 进层，闭合 '>' 出层，决定 get_token 是否裂解
    size_t generic_depth_ = 0;

    inline void add_error(const std::string &msg) {
        errors_.push_back(Diagnostic(DiagLevel::Error, msg, get_loc()));
    }

    inline void add_error_at(const Location &loc, const std::string &msg) {
        errors_.push_back(Diagnostic(DiagLevel::Error, msg, loc));
    }

    inline void add_warning(const std::string &msg) {
        warnings_.push_back(Diagnostic(DiagLevel::Warning, msg, get_loc()));
    }

    // 语法期望失败：主错误只陈述"在等什么"与"遇到什么"，
    // 已经看懂的结构单独作为 note，两者不混在一句里。
    void fail_expected(const std::string& expected, const std::string& understood = "");
    // 无法归类的 token：给出"在某结构之后"的上下文
    void fail_unexpected(const std::string& previous);

    // 结构描述：报错时说明 parser 已经读懂了什么
    std::string describe_type(const Type* t) const;
    std::string describe_expr_short(const Expr* e) const;
    std::string describe_decl(const Decl* d) const;

    // 从外向内查找 define 替换解开链式替换
    std::optional<std::vector<Token>> expand_define(const Token& name) const;

    void scan_define_decl();

    // 泛型闭合符裂解：嵌套内以 '>' 起始的操作符先出闭合 '>'，余部排回 pending_ 重派
    Token generic_split(Token tok);

    inline Token get_token() {
        // 从 pending 队列中取 Token
        if (!pending_.empty()) {
            Token tok = std::move(pending_.front());
            pending_.pop_front();
            return generic_split(std::move(tok));
        }
        Token tok = lexer_.next_token();
        if (tok.kind == TokenKind::Define) {
            scan_define_decl();
            return get_token();
        }
        // 处理 define 替换
        if (tok.is_word()) {
            const auto target = expand_define(tok);
            if (target) {
                pending_.insert(pending_.end(), target->begin(), target->end());
                return get_token();
            }
        }
        // 维护 define 作用域（'{}' 进出）
        if (tok.kind == TokenKind::BraceL) {
            defines_.push_back({});
        } else if (tok.kind == TokenKind::BraceR) {
            if (!defines_.empty()) defines_.pop_back();
        }

        return generic_split(std::move(tok));
    }

    // 可逆前进：缓冲只追加不删除，restore 总能把游标移回来
    inline void walk(const int &step) {
        walk_pos_ += step;
        while (walk_pos_ >= dq_.size()) {
            if (!dq_.empty() && dq_.back().kind == TokenKind::Eof) {
                walk_pos_ = dq_.size() - 1;
                return;
            }
            dq_.push_back(get_token());
        }
    }
    // 内联函数参数含有默认值，且调用处没有参数时，调试时会跳转到对应位置进行预预览
    // 非常影响调试观感，也没有找到vscode的对应设置，于是用重载来避免
    inline void walk(){
        walk(1);
    }
    // 前进，与 walk 同义。保留它是为了让"提交读取"与"探测读取"在调用处可读。
    inline void advance(const int &step) {
        walk(step);
    }
    inline void advance(){
        advance(1);
    }
    inline const Token& peek(size_t offset) {
        offset += walk_pos_;
        while (offset >= dq_.size()) {
            if (!dq_.empty() && dq_.back().kind == TokenKind::Eof) return dq_.back();
            dq_.push_back(get_token());
        }
        return dq_[offset];
    }
    inline const Token& peek(){
        return peek(1);
    }
    // 检查点：记录游标位置、已生成节点数、诊断数与类体深度
    struct Mark {
        size_t walk_pos;
        size_t nodes;
        size_t error;
        // 类体嵌套深度随解析前进，回退 token 位置须一并回退
        size_t class_depth;
    };
    inline void restore(const Mark & m) {
        walk_pos_ = m.walk_pos;
        class_depth_ = m.class_depth;
        if (cu_.nodes.size() > m.nodes) cu_.nodes.resize(m.nodes);
        if (errors_.size() > m.error) errors_.resize(m.error);
    }
    inline Token& curr() {
        if (walk_pos_ >= dq_.size()) {
            dq_.push_back(get_token());
        }
        return dq_[walk_pos_];
    }
    inline Mark point() const {
        return Mark{walk_pos_, cu_.nodes.size(), errors_.size(), class_depth_};
    }
    // 探针：只读 token 的前瞻。期间不产出诊断，离开作用域自动回退。
    // 只用于回溯代价大的歧义点（如 `T{}` 是类体还是切片类型）。
    struct Probe {
        Parser& p;
        Mark m;
        explicit Probe(Parser& parser) : p(parser), m(parser.point()) { ++p.probe_; }
        ~Probe() {
            p.restore(m);
            --p.probe_;
        }
        Probe(const Probe&) = delete;
        Probe& operator=(const Probe&) = delete;
    };
    inline bool is_eof() {
        return curr().kind == TokenKind::Eof;
    }

    
    // 声明头部形态：只用于"名字/类型开头"的歧义，关键字声明由 parse_decl 直接派发
    enum class HeadShape : uint8_t { Unknown, Func, Class, Var };
    HeadShape scan_decl_head();
    // 跳过配平的 `-<...>`，进入前 curr 必须是 GenericStart
    void skip_generic_args();
    // 跳过一或多个 `#修饰`，返回是否至少跳过一个
    bool skip_modifier_chain();

    // 尝试分析，失败返回空指针
    Decl* parse_decl();
    std::vector<Decl*> parse_members();
    bool parse_modifier_directive(SpecList& defaults);
    // 当前 '{' 是切片长度括号而非类体（回溯代价大的歧义点，用探针判定）
    bool is_slice_brace();
    // expect_name：形态已确定为变量声明，缺名字要报错
    VarList* parse_var_decl(SpecList &ml, Location &loc, bool expect_name = false);
    // 类型已解析后的变量名、后缀维度与初值
    VarList* parse_var_decl_body(SpecList &ml, Location &loc, Type* type);
    Decl* parse_typedef_decl();
    Decl* parse_exprdef_decl();
    Func* parse_func_decl(SpecList &ml, Location &loc);
    Class* parse_class_decl(SpecList &ml, Location &loc);
    Namespace* parse_namespace_decl(SpecList &ml, Location &loc);
    Extern* parse_extern_decl(SpecList &ml, Location &loc);
    Operator* parse_operator_overload(SpecList &ml, Location &loc);
    SpecialMethod* parse_special_method_decl(SpecList &ml, Location &loc);
    Conversion* parse_conversion_decl(SpecList &ml, Location &loc);
    Enum* parse_enum_decl(SpecList &ml, Location &loc);
    Keep* parse_keep_decl(SpecList &ml, Location &loc);

    Import* parse_import();
    bool parse_using_namespace();

    Stmt* parse_stmt();
    Stmt* parse_block_stmt();
    Stmt* parse_if_stmt();
    Stmt* parse_while_stmt();
    Stmt* parse_for_stmt();
    Stmt* parse_return_stmt();
    Stmt* parse_break_stmt();
    Stmt* parse_continue_stmt();
    Stmt* parse_label_stmt();
    Stmt* parse_goto_stmt();
    Stmt* parse_asm_stmt();
    Stmt* parse_keep_stmt();

    // 语句分析辅助函数
    bool is_statement_end();
    void consume_statement_end();
    void skip_to_next_statement();

    Expr* parse_expr(int min_prec = 0);
    int get_precedence(TokenKind kind);
    bool is_infix_op(TokenKind kind);
    bool is_right_assoc(TokenKind kind);
    bool is_prefix_op(TokenKind kind);
    bool is_statement_boundary();
    Expr* parse_prefix();
    Expr* parse_postfix_expr(Expr* left);
    Type* parse_postfix_type(Type* left);
    Type* append_array_dim(const Location& loc, Type* base,
                            Expr* len, ArrayKind kind);
    Expr* parse_block_expr();
    Expr* parse_func_literal();
    Expr* parse_literal();
    Expr* make_unary(Token op, Expr* operand, bool is_postfix);
    Expr* make_binary(Expr* left, Token op, Token op2);
    Expr* make_binary(Expr* left, Token op, Expr* right);

    std::vector<VarList*> parse_value_params(bool* variadic = nullptr);    // (int a ...)

    Type* parse_type();
    Type* parse_type_atom();
    Expr* parse_array_length();
    Type* parse_primitive_type();
    Type* parse_func_type();     // $int(int)
    Type* parse_named_type();    // List

    // 泛型：形参表 `foo-<T, U>` 与实参表 `List-<int>`
    std::vector<TypeParam*> parse_type_params();
    // 泛型实参表 `-<T, U>`：base 在类型位置得到 Type::GenericType，
    // 在值位置得到 Expr::GenericType，两者实参都是类型
    std::vector<Type*> parse_generic_args();
    Type* parse_type_args(Type* base);
    Expr* parse_expr_args(Expr* base);

    // 其他辅助函数
    std::vector<Type*> parse_type_list_in_paren();
    std::string_view parse_code();
    // 以下均可逆
    SpecList parse_specs();
    OpKind parse_operator();
    Type* parse_modification();
    template<typename T, typename F>
    std::optional<std::vector<T>> parse_list(
        TokenKind start,
        TokenKind end,
        TokenKind sep,
        F parse_element,
        const std::string& elmt_name
    );

};

}// namespace cx