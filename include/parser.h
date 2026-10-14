#pragma once

#include <vector>
#include <utility>

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
    std::deque<Token> dq_;
    size_t walk_pos_ = 0;
    // trial 模式：advance 退化为 walk，用于探测性/类型解析，
    // 保证 restore(point) 仍能回退。由 parse_type 的守卫设置。
    bool trial_ = false;
    StringPool& pool_;
    struct AliasDecl{
        std::string_view name;
        std::vector<Token> alias;
        // 覆盖者的位置
        size_t cvr_lv = ~size_t(0);
        size_t cvr_id = ~size_t(0);
    };
    // alias 别名作用域层级
    std::vector<std::vector<AliasDecl>> aliases_;
    // alias 别名展开缓冲；泛型闭合符裂解后的余部也排在这里
    std::deque<Token> pending_;
    bool use_alias_ = true;
    // 泛型嵌套深度：'-<' 进层，闭合 '>' 出层，决定 get_token 是否裂解
    size_t generic_depth_ = 0;

    inline void add_error(const std::string &msg) {
        errors_.push_back(Diagnostic(DiagLevel::Error, msg, get_loc()));
    }

    inline void add_warning(const std::string &msg) {
        warnings_.push_back(Diagnostic(DiagLevel::Warning, msg, get_loc()));
    }

    // 从外向内查找别名解开链式别名
    std::optional<std::vector<Token>> lookup_alias_expended(const Token& name) const;

    void scan_alias_decl();

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
        if (tok.kind == TokenKind::Alias) {
            scan_alias_decl();
            return get_token();
        }
        // 处理 alias 别名展开
        if (tok.is_word()) {
            const auto target = lookup_alias_expended(tok);
            if (target) {
                pending_.insert(pending_.end(), target->begin(), target->end());
                return get_token();
            }
        }
        // 维护别名作用域（'{}' 进出）
        if (tok.kind == TokenKind::BraceL) {
            aliases_.push_back({});
        } else if (tok.kind == TokenKind::BraceR) {
            if (!aliases_.empty()) aliases_.pop_back();
        }

        return generic_split(std::move(tok));
    }

    // 可逆前进
    inline void walk(const int &step) {
        walk_pos_ += step;
        while(walk_pos_ >= dq_.size()) {
            dq_.push_back(get_token());
        }
    }
    // 内联函数参数含有默认值，且调用处没有参数时，调试时会跳转到对应位置进行预预览
    // 非常影响调试观感，也没有找到vscode的对应设置，于是用重载来避免
    inline void walk(){
        walk(1);
    }
    // 不可逆前进
    inline void advance(const int &step) {
        if (trial_) { walk(step); return; }
        walk_pos_ += step;
        if(walk_pos_ < dq_.size()) {
            if(walk_pos_ > 0) {
                dq_.erase(dq_.begin(), dq_.begin() + walk_pos_);
            }
        }else {
            size_t n = dq_.size();
            dq_.clear();
            while(walk_pos_ > n){
                get_token();
                ++n;
            }
            dq_.push_back(get_token());
        }
        walk_pos_ = 0;
    }
    inline void advance(){
        advance(1);
    }
    inline const Token& peek(size_t offset) {
        offset += walk_pos_;
        if(offset < dq_.size()) {
            return dq_[offset];
        }else {
            while(offset >= dq_.size()) {
                dq_.push_back(get_token());
            }
            return dq_.back();
        }
    }
    inline const Token& peek(){
        return peek(1);
    }
    // 检查点：同时记录 token 位置与已生成节点数量，支持多层嵌套回退
    struct Mark {
        size_t walk_pos;
        size_t nodes;
        size_t error;
        // 泛型嵌套深度随 token 流前进，回退 token 位置须一并回退
        size_t generic_depth;
    };
    inline void restore(const Mark & m) {
        walk_pos_ = m.walk_pos;
        generic_depth_ = m.generic_depth;
        if (cu_.nodes.size() > m.nodes) cu_.nodes.resize(m.nodes);
        if (errors_.size() > m.error) errors_.resize(m.error);
    }
    inline void admit() {
        if(walk_pos_ > 0) {
            dq_.erase(dq_.begin(), dq_.begin() + walk_pos_);
        }
        walk_pos_ = 0;
    }
    inline Token& curr() {
        if (walk_pos_ >= dq_.size()) {
            dq_.push_back(get_token());
        }
        return dq_[walk_pos_];
    }
    inline Mark point() const {
        return Mark{walk_pos_, cu_.nodes.size(), errors_.size(), generic_depth_};
    }
    inline bool is_eof() {
        return curr().kind == TokenKind::Eof;
    }

    
    // 尝试分析，失败返回空指针
    Decl* parse_decl();
    std::vector<Decl*> parse_members();
    bool parse_modifier_directive(SpecList& defaults);
    VarList* parse_var_decl(SpecList &ml, Location &loc);
    Func* parse_func_decl(SpecList &ml, Location &loc);
    Class* parse_class_decl(SpecList &ml, Location &loc, std::vector<Type*> injs = {});
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
    Stmt* parse_flag_stmt();
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
    Type* parse_func_type();     // $(int)int
    Type* parse_named_type();    // List

    // 泛型：形参表 `foo-<T, U>` 与实参表 `List-<int>`
    void parse_type_params(std::vector<TypeParam*>& out);
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