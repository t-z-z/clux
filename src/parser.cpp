#include "../include/parser.h"

#include <vector>
#include <string>
#include <string_view>

namespace vo {
    // 从外向内查找别名解开链式别名
std::optional<std::vector<Token>> Parser::lookup_alias_expended(const Token& name) const {
    if (aliases_.empty() || !use_alias_) return std::nullopt;
    std::vector<Token> r = {name};
    for (size_t i = 0; i < aliases_.size(); ++i) {
        const auto& level = aliases_[i];
        for (size_t j = 0; j < r.size(); ++j) {
            if (!r[j].is_word()) continue;
            for (size_t k = 0; k < level.size(); ++k) {
                if (level[k].name == r[j].get_val() 
                    && !( // 覆盖它的别名如果存在则不触发此别名
                        level[k].cvr_lv < aliases_.size()
                        && level[k].cvr_id < aliases_[level[k].cvr_lv].size()
                        && level[k].name == aliases_[level[k].cvr_lv][level[k].cvr_id].name)
                ) {
                    r.erase(r.begin() + j);
                    r.insert(r.begin() + j, level[k].alias.begin(), level[k].alias.end());
                    j += level[k].alias.size();
                }
            }
        }
    }
    return r;
}
void Parser::scan_alias_decl() {
    if (!use_alias_) return;
    Token name_tok = get_token();
    if (!name_tok.is_word()) {
        add_error("expected word after alias");
        return;
    }
    Token assign = lexer_.next_token();
    if (assign.kind != TokenKind::Assign) {
        add_error("expected '=' after alias name");
        return;
    }
    std::vector<Token> target;
    while (true) {
        Token t = get_token();
        if (t.kind == TokenKind::Colon || t.kind == TokenKind::Eof) break;
        target.push_back(t);
    }
    if (target.size() == 1 && target[0].get_val() == name_tok.get_val()) {
        add_warning("the alias is the same as the original name");
    }
    // 覆盖最靠近的别名
    for (size_t i = aliases_.size(); i-- > 0; ) {
        auto& level = aliases_[i];
        for (size_t j = level.size(); j-- > 0; ) {
            if (level[j].name == name_tok.get_val()) {
                level[j].cvr_lv = aliases_.size() -1;
                level[j].cvr_id = aliases_.back().size();
                return;
            }
        }
    }
    if (aliases_.empty()) aliases_.push_back({});
    aliases_.back().push_back({name_tok.get_val(), std::move(target), ~size_t(0), ~size_t(0)});
}
// 申明解析部分

// 顶层函数不负责退回
CompUnit* Parser::parse() {
    while(!is_eof()){
        bool u = parse_using_namespace(); // 避免namespace decl识别报错，必须在前
        if (u){
            admit();
            continue;
        }
        if (curr().kind == TokenKind::Import){
            Import* i = parse_import();
            if (i){
                admit();
                continue;
            }
        } else {
            Decl* d = parse_decl();
            if (d){
                admit();
                cu_.top_decls.push_back(d);
                if (d->kind == DeclKind::Class) {
                    if (cu_.is_type_name(d->name)) {
                        add_error("mutiple definition of class "+ std::string(d->name));
                    } else if (!d->name.empty()) {
                        cu_.register_type(d->name, d);
                    }
                }
                continue;
            }
        }

        add_error("unexpected token " + curr().quote());
        advance();
    }
    return &cu_;
}

// namespace <name>: —— 文件顶层 using 指令（hoisted），须在 parse_decl 之前尝试，
// 否则会被 parse_namespace_decl 误当作 brace 形式在 ':' 处报错
bool Parser::parse_using_namespace() {
    if (curr().kind != TokenKind::Namespace) return false;
    if (!peek(1).is_ident()) return false;
    if (peek(2).kind == TokenKind::BraceL) return false;
    advance();
    Location loc = get_loc();
    std::vector<std::string_view> path;
    while(curr().is_ident()) {
        path.push_back(curr().get_val());
        advance();
        if (curr().kind == TokenKind::ColonColon) advance();
        else if (curr().kind == TokenKind::Colon) {
            advance();
            break;
        }
    }
    cu_.add_using_namespace(path, loc);
    return true;
}

// 第二层函数负责在读取失败时退回
// import只有能不能读取两种状态，不需要退回
Import* Parser::parse_import() {
    bool no = false;
    if (curr().kind == TokenKind::No) {
        if(peek().kind == TokenKind::Import){
            advance();
            no = true;
        }
    }
    if (curr().kind != TokenKind::Import) return nullptr;
    advance();
    if (curr().is_ident() || curr().kind == TokenKind::StringLiteral){
        std::unique_ptr<Import> result = std::make_unique<Import>(
            get_loc(), curr().get_val(), no
        );
        advance();
        Import* raw = result.get();
        cu_.add_node(std::move(result));
        return raw;
    }
    add_error("expected filename or \"filename\"");
    return nullptr;
}

Decl* Parser::parse_decl() {
    if (!curr().is_word() && curr().kind != TokenKind::At
        && curr().kind != TokenKind::Dollar
        && curr().kind != TokenKind::BracketL) return nullptr;
    ModfList ml = parse_modfs();
    size_t sp = point();
    Location loc = get_loc();
    // 易于确定的优先级更高
    if (curr().kind == TokenKind::At) return parse_class_decl(ml, loc);
    if (curr().kind == TokenKind::Enum) return parse_enum_decl(ml, loc);
    if (curr().kind == TokenKind::Operator) return parse_operator_overload(ml, loc);
    if (curr().kind == TokenKind::Extern) return parse_extern_decl(ml, loc);
    if (Decl* d = parse_namespace_decl(ml, loc)) return d; restore(sp);
    if (Decl* d = parse_func_decl(ml, loc)) return d; restore(sp);
    if (Decl* d = parse_var_decl(ml, loc)) { return d; } restore(sp);
    if (Decl* d = parse_special_method_decl(ml, loc)) return d; restore(sp);
    return nullptr;
}

// 第三层函数负责读取、确认和报错
// 无法确认的函数只能进行可逆操作
ModfList Parser::parse_modfs() {
    ModfList ml;
    for(;;){
        if (curr().is_modf()) {
            ml.set(token_to_modf(
                curr().kind
            ));
            walk();
        } else if (curr().kind == TokenKind::Hash) {
            ml.add_custom(parse_modification());
        } else {
            break;
        }
    }
    return ml;
}

Modification* Parser::parse_modification() {
    if (curr().kind != TokenKind::Hash) return nullptr;
    const Location& loc = get_loc();
    walk();
    if (!curr().is_ident()) {
        add_error("expected identifier after '#'");
        return nullptr;
    }
    std::string_view name = curr().get_val();
    walk();
    std::vector<ASTNode*> args = parse_template_args();

    return cu_.add_node<Modification>(loc, name, args);
}

// 函数的识别：名称后接参数括号
Func* Parser::parse_func_decl(ModfList &ml, Location &loc) {
    if (!curr().is_ident()) return nullptr;
    if (peek().kind != TokenKind::GenericStart
        && peek().kind != TokenKind::ParenL
        && peek().kind != TokenKind::BraceL) return nullptr;
    std::string_view name = curr().get_val();
    walk();
    std::vector<TemplateParam> tpls = parse_template_params();
    if (curr().kind != TokenKind::ParenL) return nullptr;
    std::vector<VarList*> params = parse_value_params();
    admit();
    std::vector<VarList*> returns;
    for (;;) {
        ModfList ml = parse_modfs();
        Location loc = get_loc();
        VarList* r = parse_var_decl(ml, loc);
        if (r) {
            returns.push_back(r);
        } else {
            break;
        }
    }
    Stmt* body = parse_block_stmt();
    if (!body) {
        add_error("expected function body");
    }
    return cu_.add_node<Func>(
            loc, std::move(name),
            std::move(ml),
            std::move(tpls),
            std::move(params),
            std::move(returns),
            body
    );
}

// 变量的识别：类型或推导符加标识符
VarList* Parser::parse_var_decl(ModfList &ml, Location &loc) {
    Type* type;
    if (curr().kind == TokenKind::Let) {
        type = cu_.add_node<Type>(get_loc(), Type::Inferred{});
        advance();
    } else {
        type = parse_type();
        if (!type) return nullptr;
    }
    std::vector<VarItem> vars;
    for (;;) {
        if (curr().is_ident()) {
            std::string_view name = curr().get_val();
            Expr* init = nullptr;
            walk();
            if (curr().kind == TokenKind::Assign) {
                walk();
                init = parse_expr();
                if (!init) {
                    add_error("expected expression");
                    continue;
                }
            }else if(type->is_inferred()) {
                add_error("can not infer type");
            }
            vars.push_back(VarItem(name, init));
        } else if (vars.empty()) {
            return nullptr;
        } else {
            break;
        }
        if (curr().kind != TokenKind::Comma) break;
        advance();
    }
    return cu_.add_node<VarList>(
        loc, ml, type, vars
    );
}

Class* Parser::parse_class_decl(ModfList &ml, Location &loc) {
    if (curr().kind != TokenKind::At) return nullptr;
    advance();
    std::string_view name;
    if (!curr().is_ident()) {
        add_error("expected class name after @");
        // 语义上不允许，语法上仍需要解析匿名类
    }else {
        name = curr().get_val();
        advance();
    }
    std::vector<TemplateParam> tpls = parse_template_params();
    ModfList nml = parse_modfs();
    ml.merge(nml);
    std::vector<Decl*> members = parse_members();
    Class* raw = cu_.add_node<Class>(
        loc, std::move(name), std::move(ml), std::move(members), std::move(tpls)
    );
    return raw;
}

Namespace* Parser::parse_namespace_decl(ModfList &ml, Location &loc) {
    if (curr().kind != TokenKind::Namespace) return nullptr;
    advance();
    if (!curr().is_ident()) {
        add_error("expected namespace name after 'namespace'");
        return cu_.add_node<Namespace>();
    }
    std::string_view name = curr().val;
    advance();
    std::vector<Decl*> members = parse_members();
    return cu_.add_node<Namespace>(
        loc, std::move(name), ml, members
    );
}

std::vector<Decl*> Parser::parse_members() {
    std::vector<Decl*> members;
    ModfList default_modfs;    // 修饰符指令（pub: / modf ...: / modf no:）作用于后续成员
    if (curr().kind == TokenKind::BraceL) {
        advance();
    }
    while (curr().kind != TokenKind::BraceR && !is_eof()) {
        if (parse_modifier_directive(default_modfs)) {
            admit();
            continue;
        }
        if (Decl* d = parse_decl()) {
            d->modfs.merge(default_modfs);
            members.push_back(d);
            admit();
        } else {
            add_error("unexpected token: " + curr().quote());
            advance();
        }
        if (is_eof()) {
            add_warning("unterminated container, expected '}'");
            break;
        }
    }
    if (curr().kind == TokenKind::BraceR) {
        advance(); // 消费 '}'
    }
    return members;
}

bool Parser::parse_modifier_directive(ModfList& defaults) {
    // pub:
    if (curr().kind == TokenKind::Pub && peek().kind == TokenKind::Colon) {
        defaults.set(ModfKind::Pub);
        advance(2);
        consume_statement_end();
        return true;
    }
    // priv:
    if (curr().kind == TokenKind::Priv && peek().kind == TokenKind::Colon) {
        defaults.del(ModfKind::Pub);
        advance(2);
        consume_statement_end();
        return true;
    }
    // modf: 清空默认修饰符
    // modf <modf>...: 修改默认修饰符
    if (curr().kind == TokenKind::Modf) {
        advance();
        size_t sp = point();
        if (curr().kind == TokenKind::Colon) {
            defaults = ModfList();
            advance();
            consume_statement_end();
            return true;
        }

        ModfList w = parse_modfs();
        if (curr().kind == TokenKind::Colon) {
            defaults = w;
            advance();
            consume_statement_end();
            return true;
        }
        restore(sp);
    }
    return false;
}

Operator* Parser::parse_operator_overload(ModfList &ml, Location &loc) {
    if (curr().kind != TokenKind::Operator) return nullptr;
    advance();
    std::string_view name = curr().get_val();
    OpKind kind = parse_operator();
    if (kind == OpKind::Unknown) {
        if (curr().kind == TokenKind::ParenL
                && peek().kind == TokenKind::ParenR) {
            kind = OpKind::Paren;
            advance(2);
            name = "()";
        }else if (curr().kind == TokenKind::BracketL
                && peek().kind == TokenKind::BracketR) {
            kind = OpKind::Index;
            advance(2);
            name = "[]";
        }else {
            add_error("expected operator after \"operator\"");
        }
    }
    std::vector<TemplateParam> tpls = parse_template_params();
    if (curr().kind != TokenKind::ParenL) return cu_.add_node<Operator>();
    std::vector<VarList*> params = parse_value_params();
    admit();
    std::vector<VarList*> returns;
    for (;;) {
        ModfList ml = parse_modfs();
        Location loc = get_loc();
        VarList* r = parse_var_decl(ml, loc);
        if (r) {
            returns.push_back(r);
        } else {
            break;
        }
    }
    Stmt* body = parse_block_stmt();
    if (!body) {
        add_error("expected operator overload body");
    }
    return cu_.add_node<Operator>(
            loc, kind,
            name,
            std::move(ml),
            std::move(tpls),
            std::move(params),
            std::move(returns),
            body
    );
}

OpKind Parser::parse_operator() {
    size_t sp = point();
    TokenKind tk = curr().kind;
    walk();
    switch (tk) {
        #define X(name, str) \
            case TokenKind::name: return OpKind::name;
        OP_KIND
        #undef X
        default: 
            restore(sp);
            return OpKind::Unknown;  // fallback
    }
}

SpecialMethod* Parser::parse_special_method_decl(ModfList &ml, Location &loc) {
    TokenKind tok_kind = curr().kind;
    SpecialMethod::Kind kind = SpecialMethod::Kind::Conversion;
    if (tok_kind == TokenKind::Drop){
        kind = SpecialMethod::Kind::Drop;
    } else if (tok_kind == TokenKind::Ref){
        kind = SpecialMethod::Kind::Ref;
    } else if (tok_kind == TokenKind::Deref){
        kind = SpecialMethod::Kind::Deref;
    } else if (tok_kind == TokenKind::Access){
        kind = SpecialMethod::Kind::Access;
    } else if (tok_kind == TokenKind::Conversion){
        advance();
        ModfList m = ModfList();
        VarList* vl = parse_var_decl(m, loc);
        if (!vl) return nullptr;
        Stmt* s = parse_block_stmt();
        if (!s) {
            add_error("expected conversion method body");
        }
        return cu_.add_node<SpecialMethod>(
            loc, kind, std::move(ml), s, vl
        );
    } else return nullptr;
    Stmt* b = parse_block_stmt();
    if (!b) {
        add_error("expected method body");
    }
    return cu_.add_node<SpecialMethod>(
        loc, kind, std::move(ml), b
    );
}

Extern* Parser::parse_extern_decl(ModfList &ml, Location &loc) {
    if (curr().kind != TokenKind::Extern) return nullptr;
    advance();
    
    if (curr().kind != TokenKind::StringLiteral) {
        add_error("expected library name string after 'extern'");
        return cu_.add_node<Extern>();
    }
    std::string_view lib_name = curr().get_val();
    advance();
    
    if (curr().kind != TokenKind::BraceL) {
        add_error("expected '{' after extern library name");
        return cu_.add_node<Extern>();
    }
    
    std::vector<Decl*> members = parse_members();
    return cu_.add_node<Extern>(
        loc,
        lib_name,
        std::move(ml),
        std::move(members)
    );
}

Enum* Parser::parse_enum_decl(ModfList& ml, Location& loc) {
    if (curr().kind != TokenKind::Enum) return nullptr;
    advance();
    if (curr().kind == TokenKind::BraceL) {
        advance();
        std::vector<Token> vars;
        while(curr().is_ident()) {
            vars.push_back(curr());
        }
        if (curr().kind == TokenKind::BraceR) {
            advance();
        } else {
            add_error("expected terminating '}");
        }
        return cu_.add_node<Enum>(loc, std::move(vars), std::move(ml));
    }
    if (curr().is_ident()) {
        return cu_.add_node<Enum>(loc, std::vector<Token>{curr()}, std::move(ml));
    }
    add_error("expected variant of enum");
    return cu_.add_node<Enum>();
}

Keep* Parser::parse_keep_decl(ModfList& ml, Location& loc) {
    if (curr().kind != TokenKind::Keep) return nullptr;
    advance();
    std::string_view code = parse_code();
    return cu_.add_node<Keep>(loc, code, std::move(ml));
}

// 语句解析部分

bool Parser::is_statement_end() {
    if (is_eof()) return true;
    if (curr().kind == TokenKind::Semicolon) return true;
    if (curr().kind == TokenKind::BraceR) return true;
    return false;
}
void Parser::consume_statement_end() {
    if (curr().kind == TokenKind::Semicolon) {
        advance();
        return;
    }
    // 如果遇到操作符，说明语句未正确结束
    // 注意，一定要用ast的分类器而不是token的方法，token的方法会包含}符而导致错误
    if (token_to_op(curr().kind) != OpKind::Unknown) {
        add_error("unexpected operator at end of statement: " + curr().quote());
        skip_to_next_statement();
        return;
    }
    // 换行符已在词法分析中忽略
}
void Parser::skip_to_next_statement() {
    // 换行不一定结束语句，不换行不一定没结束语句
    while (!is_eof()) {
        const Token &pre= curr();
        advance();

        if (pre.kind == TokenKind::Semicolon) break;

        if (pre.is_literal() || 
            pre.is_word() ||
            pre.kind == TokenKind::ParenR ||
            pre.kind == TokenKind::BraceR ||
            pre.kind == TokenKind::BracketR
        ) {
            const Token &cur = curr();
            if (cur.is_literal() || 
                cur.is_word() ||
                cur.kind == TokenKind::ParenL ||
                cur.kind == TokenKind::BraceL
            ) {
                break;
            }
        }
    }
}

Stmt* Parser::parse_stmt() {
    Stmt* result = nullptr;
    if (curr().kind == TokenKind::Semicolon) {
        // 空块
        result = cu_.add_node<Stmt>(
            get_loc(),
            Stmt::Block{{}}
        );
        advance();
        return result;
    }
    // 块语句
    if (curr().kind == TokenKind::BraceL) {
        result = parse_block_stmt();
    }else
    // 控制流关键字
    if (curr().kind == TokenKind::If) {
        result = parse_if_stmt();
    }else if (curr().kind == TokenKind::While) {
        result = parse_while_stmt();
    }else if (curr().kind == TokenKind::For) {
        result = parse_for_stmt();
    }else if (curr().kind == TokenKind::Return) {
        result = parse_return_stmt();
    }else if (curr().kind == TokenKind::Break) {
        result = parse_break_stmt();
    }else if (curr().kind == TokenKind::Continue) {
        result = parse_continue_stmt();
    }else if (curr().kind == TokenKind::Flag) {
        result = parse_flag_stmt();
    }else if (curr().kind == TokenKind::ToFlag) {
        result = parse_goto_stmt();
    }else if (curr().kind == TokenKind::Asm) {
        result = parse_asm_stmt();
    }else if (curr().kind == TokenKind::Spawn){
        result = parse_spawn_stmt();
    }else if (curr().kind == TokenKind::Wait){
        result = parse_wait_stmt();
    }else if (curr().kind == TokenKind::Keep){
        result = parse_keep_stmt();
    }else {
    {
        size_t sp = point();
        // 变量申明语句（含元类型别名 `type T = <meta-expr>` → T 为 MetaType 值变量）
        Location loc = get_loc();
        ModfList ml = parse_modfs();
        Location l = get_loc();
        VarList* vl = parse_var_decl(ml,l);
        if (vl) {
            // 形如 `data[0] = 5` / `foo(1)` 会被 parse_generic_type 误读成
            // `Custom(data) data` 变量声明：类型为裸自定义名且名称后紧跟后缀 token 时回退
            // 需要修复 parse_generic_type 解决 标记
            result = cu_.add_node<Stmt>(loc, Stmt::VarDecl{vl});
            goto End;
        } else {
            restore(sp);
        }
    }{
        // 表达式语句
        Expr* expr = parse_expr();
        if (!expr) {
            add_error("expected statement");
            goto End;
        }
        result = cu_.add_node<Stmt>(
            expr->loc,
            Stmt::ExprStmt{expr}
        );
        goto End;
    }
    }
    End:
    consume_statement_end();
    return result;
}

Stmt* Parser::parse_block_stmt() {
    if (curr().kind != TokenKind::BraceL) return nullptr;
    Location loc = get_loc();
    advance();
    
    std::vector<Stmt*> stmts;
    
    while (!is_eof() && curr().kind != TokenKind::BraceR) {
        Stmt* s = parse_stmt();
        if (s) {
            stmts.push_back(s);
        } else {
            // 解析失败，跳过错误 token
            add_error("unexpected token in block: " + curr().quote());
            advance();
        }
    }
    
    if (curr().kind != TokenKind::BraceR) {
        add_error("expected '}' to close block");
        return nullptr;
    }
    advance(); // 消费 '}'
    
    return cu_.add_node<Stmt>(
        loc,
        Stmt::Block{stmts}
    );
}

Stmt* Parser::parse_if_stmt() {
    if (curr().kind != TokenKind::If) return nullptr;
    Location loc = get_loc();
    advance();

    Expr* cond = parse_expr();
    if (!cond) {
        add_error("expected condition after 'if'");
        // 创建占位条件
        cond = cu_.add_node<Expr>(
            loc, Expr::Literal{LiteralKind::Bool, "false"}
        );
    }

    Stmt* then_branch = parse_stmt();
    if (!then_branch) {
        add_error("expected statement after 'if' condition");
        // 创建空块
        then_branch = cu_.add_node<Stmt>(
            loc, Stmt::Block{{}}
        );
    }

    Stmt* else_branch = nullptr;
    if (curr().kind == TokenKind::Else) {
        advance();
        else_branch = parse_stmt();
        if (!else_branch) {
            add_error("expected statement after 'else'");
            else_branch = cu_.add_node<Stmt>(
                loc, Stmt::Block{{}}
            );
        }
    }

    return cu_.add_node<Stmt>(
        loc, Stmt::If{cond, then_branch, else_branch}
    );
}

Stmt* Parser::parse_while_stmt() {
    if (curr().kind != TokenKind::While) return nullptr;
    Location loc = get_loc();
    advance();
    
    Expr* cond = parse_expr();
    if (!cond) {
        add_error("expected condition after 'while'");
        cond = cu_.add_node<Expr>(
            loc, Expr::Literal{LiteralKind::Bool, "true"}
        );
    }
    Stmt* body = parse_stmt();
    if (!body) {
        add_error("expected body statement after 'while' condition");
        body = cu_.add_node<Stmt>(
            loc,
            Stmt::Block{{}}
        );
    }

    return cu_.add_node<Stmt>(
        loc, Stmt::While{cond, body}
    );
}

Stmt* Parser::parse_for_stmt() {
    if (curr().kind != TokenKind::For) return nullptr;
    Location loc = get_loc();
    advance();
    // 检查是否为 for-in: var in collection
    if (curr().is_ident() && peek(1).kind == TokenKind::In) {
        Token var_tok = curr();
        advance(); // 消费变量名
        advance(); // 消费 'in'
        
        Expr* collection = parse_expr();
        if (!collection) {
            add_error("expected collection expression after 'in'");
            collection = cu_.add_node<Expr>(
                loc, Expr::Identifier{}
            );
        }
        Stmt* body = parse_stmt();
        if (!body) {
            add_error("expected body statement for for-in loop");
            body = cu_.add_node<Stmt>(
                loc, Stmt::Block{}
            );
        }
        return cu_.add_node<Stmt>(
            loc, Stmt::ForIn{var_tok, collection, body}
        );
    }
    // 常规 for 循环: for (init; cond; next) 或 for init; cond; next
    if (curr().kind == TokenKind::ParenL) {
        advance();
    }
    Stmt* init = nullptr;
    if (curr().kind != TokenKind::Semicolon) {
        init = parse_stmt();
        if (!init) {
            add_error("expected init statement in for loop");
        }
    }

    Expr* cond = nullptr;
    if (curr().kind != TokenKind::Semicolon) {
        cond = parse_expr();
        if (!cond) {
            add_error("expected condition in for loop");
        }
    }

    consume_statement_end();

    Stmt* next = nullptr;
    if (curr().kind != TokenKind::ParenR) {
        next = parse_stmt();
        if (!next) {
            add_error("expected next statement in for loop");
        }
    }

    if (curr().kind == TokenKind::ParenR) {
        advance();
    }

    Stmt* body = parse_stmt();
    if (!body) {
        add_error("expected body statement for for loop");
    }
    return cu_.add_node<Stmt>(
        loc, Stmt::For{init, cond, next, body}
    );
}

Stmt* Parser::parse_return_stmt() {
    if (curr().kind != TokenKind::Return) return nullptr;
    Location loc = get_loc();
    advance();
    return cu_.add_node<Stmt>(
        loc,
        Stmt::Return{}
    );
}
Stmt* Parser::parse_break_stmt() {
    if (curr().kind != TokenKind::Break) return nullptr;
    Location loc = get_loc();
    advance();
    return cu_.add_node<Stmt>(
        loc,
        Stmt::Break{}
    );
}

Stmt* Parser::parse_continue_stmt() {
    if (curr().kind != TokenKind::Continue) return nullptr;
    Location loc = get_loc();
    advance();
    return cu_.add_node<Stmt>(
        loc,
        Stmt::Continue{}
    );
}

Stmt* Parser::parse_flag_stmt() {
    if (curr().kind != TokenKind::Flag) return nullptr;
    Location loc = get_loc();
    advance();
    
    if (!curr().is_ident()) {
        add_error("expected label name after 'flag'");
        return nullptr;
    }
    std::string_view label = curr().get_val();
    advance();
    return cu_.add_node<Stmt>(
        loc,
        Stmt::Flag{label}
    );
}

Stmt* Parser::parse_goto_stmt() {
    if (curr().kind != TokenKind::ToFlag) return nullptr;
    Location loc = get_loc();
    advance();
    
    if (!curr().is_ident()) {
        add_error("expected label name after 'toflag'");
        return nullptr;
    }
    std::string_view label = curr().get_val();
    advance();

    return cu_.add_node<Stmt>(
        loc,
        Stmt::Goto{label}
    );
}

Stmt* Parser::parse_asm_stmt() {
    if (curr().kind != TokenKind::Asm) return nullptr;
    Location loc = get_loc();
    advance();
    std::string_view code = parse_code();
    return cu_.add_node<Stmt>(
        loc,
        Stmt::Asm{code}
    );
}

Stmt* Parser::parse_keep_stmt() {
    if (curr().kind != TokenKind::Keep) return nullptr;
    Location loc = get_loc();
    advance();
    std::string_view code = parse_code();
    return cu_.add_node<Stmt>(
        loc,
        Stmt::Keep{code}
    );
}

Stmt* Parser::parse_spawn_stmt() {
    Location loc = get_loc();
    advance();

    Stmt* body = parse_block_stmt();
    if (!body) {
        add_error("expected block after 'spawn'");
        return nullptr;
    }

    return cu_.add_node<Stmt>(
        loc,
        Stmt::Spawn{body}
    );
}

Stmt* Parser::parse_wait_stmt() {
    Location loc = get_loc();
    advance();

    if (curr().kind != TokenKind::ParenL) {
        add_error("expected '(' after 'wait'");
        return nullptr;
    }
    advance();

    Expr* condition = parse_expr();

    if (curr().kind != TokenKind::ParenR) {
        add_error("expected ')' after wait condition");
        return nullptr;
    }
    advance();

    return cu_.add_node<Stmt>(
        loc,
        Stmt::Wait{condition}
    );
}

// 表达式解析部分

int Parser::get_precedence(TokenKind kind) {
    switch (kind) {
        case TokenKind::Assign:
        case TokenKind::PlusAssign:
        case TokenKind::MinusAssign:
        case TokenKind::MulAssign:
        case TokenKind::DivAssign:
        case TokenKind::ModAssign:
        case TokenKind::AndAssign:
        case TokenKind::OrAssign:
        case TokenKind::XorAssign:
        case TokenKind::ShlAssign:
        case TokenKind::ShrAssign:  return 3;
        case TokenKind::Question:   return 4;  // ?:
        case TokenKind::Or:         return 5;  // ||
        case TokenKind::And:        return 6;  // &&
        case TokenKind::BitOr:      return 7;
        case TokenKind::BitXor:     return 8;
        case TokenKind::BitAnd:     return 9;
        case TokenKind::Equal:
        case TokenKind::NotEqual:   return 10;
        case TokenKind::Less:
        case TokenKind::Greater:
        case TokenKind::LessEqual:
        case TokenKind::GreaterEqual:return 11;
        case TokenKind::Shl:
        case TokenKind::Shr:        return 13;
        case TokenKind::Plus:
        case TokenKind::Minus:      return 14;
        case TokenKind::Mul:
        case TokenKind::Div:
        case TokenKind::Mod:        return 15;
        default:                    return 0;
    }
}

bool Parser::is_infix_op(TokenKind kind) {
    return get_precedence(kind) > 0;
}

bool Parser::is_right_assoc(TokenKind kind) {
    switch (kind) {
        case TokenKind::Assign:
        case TokenKind::PlusAssign:
        case TokenKind::MinusAssign:
        case TokenKind::MulAssign:
        case TokenKind::DivAssign:
        case TokenKind::ModAssign:
        case TokenKind::AndAssign:
        case TokenKind::OrAssign:
        case TokenKind::XorAssign:
        case TokenKind::ShlAssign:
        case TokenKind::ShrAssign:
        case TokenKind::Question:   // ?: 也是右结合
            return true;
        default:
            return false;
    }
}

bool Parser::is_prefix_op(TokenKind kind) {
    switch (kind) {
        case TokenKind::Plus:      // +
        case TokenKind::Minus:     // -
        case TokenKind::Not:       // !
        case TokenKind::BitNot:    // ~
            return true;
        default:
            return false;
    }
}

bool Parser::is_postfix_op(TokenKind kind) {
    switch (kind) {
        case TokenKind::ParenL:     // ()
        case TokenKind::BracketL:   // []
        case TokenKind::Dot:        // .
        case TokenKind::ColonColon: // ::
        case TokenKind::GenericStart: // f-<T>(args)
        case TokenKind::Minus:      // -* -&
            return true;
        default:
            return false;
    }
}

bool Parser::is_statement_boundary() {
    if (is_eof()) return true;
    if (curr().kind == TokenKind::Semicolon) return true;
    if (curr().kind == TokenKind::BraceR) return true;
    if (curr().kind == TokenKind::BraceL) return true;
    
    // 语句关键字
    switch (curr().kind) {
        case TokenKind::If:
        case TokenKind::While:
        case TokenKind::For:
        case TokenKind::Return:
        case TokenKind::Break:
        case TokenKind::Continue:
        case TokenKind::Flag:
        case TokenKind::ToFlag:
        case TokenKind::Asm:
        case TokenKind::Keep:
            return true;
        default:
            break;
    }
    
    // 类型名开头可能是变量声明
    if (curr().is_type()) return true;
    
    return false;
}

// pratt算法
Expr* Parser::parse_expr(int min_prec) {
    // 解析前缀
    Expr* left = parse_prefix();
    if (!left) {
        return nullptr;
    }
    
    // 处理后缀操作符
    while (is_postfix_op(curr().kind)) {
        left = parse_postfix(left);
        if (!left) return nullptr;
    }
    
    // 处理中缀操作符
    for(;;) {
        Token op = curr();
        int prec = get_precedence(op.kind);
        
        if (prec < min_prec || !is_infix_op(op.kind)) break;
        
        advance(); // 消费操作符
        
        // 三目运算符 cond ? then : else（右结合）
        if (op.kind == TokenKind::Question) {
            Expr* then_expr = parse_expr(0);
            if (!then_expr) {
                add_error("expected expression after '?'");
                continue;
            }
            if (curr().kind != TokenKind::Colon) {
                add_error("expected ':' in ternary expression");
            } else {
                advance();
            }
            Expr* else_expr = parse_expr(prec);
            if (!else_expr) {
                add_error("expected expression after ':'");
            }
            while (is_postfix_op(curr().kind)) {
                else_expr = parse_postfix(else_expr);
                if (!else_expr) return nullptr;
            }
            left = cu_.add_node<Expr>(op.loc, Expr::Ternary{left, then_expr, else_expr});
            continue;
        }
        
        // 处理右结合性
        int next_prec = is_right_assoc(op.kind) ? prec : prec + 1;
        Expr* right = parse_expr(next_prec);
        if (!right) {
            add_error("expected expression after " + op.quote());
            // 尝试跳过操作符继续
            if (!is_statement_boundary()) {
                advance();
                continue;
            }
            return left;
        }
        
        // 处理后缀在右操作数上
        while (is_postfix_op(curr().kind)) {
            right = parse_postfix(right);
            if (!right) return nullptr;
        }
        
        // 构建中缀节点
        left = make_binary(left, op, right);
    }
    
    return left;
}

// 前缀解析
Expr* Parser::parse_prefix() {
    Location loc = get_loc();
    
    // 字面量
    if (curr().is_literal()) {
        return parse_literal();
    }

    // 内置类型转换函数：typename(值) 或 识别原始类型为泛型参数表达式
    if (curr().is_type()) {
        Location c_loc = get_loc();
        Type* target = parse_primitive_type();
        if (curr().kind != TokenKind::ParenL) {
            return cu_.add_node<Expr>(c_loc, Expr::PrimitiveType{target});
        }
        advance();
        Expr* operand = parse_expr();
        if (!operand) {
            add_error("expected expression in type conversion");
        }
        if (curr().kind != TokenKind::ParenR) {
            add_error("expected ')'");
        } else {
            advance();
        }
        return cu_.add_node<Expr>(c_loc, Expr::Cast{target, operand});
    }
    
    // 标识符
    if (curr().is_ident()) {
        std::string_view name = curr().get_val();
        advance();
        return cu_.add_node<Expr>(
            loc,
            Expr::Identifier{name}
        );
    }
    
    // 括号表达式
    if (curr().kind == TokenKind::ParenL) {
        advance();
        Expr* inner = parse_expr();
        if (!inner) {
            add_error("expected expression after '('");
            return nullptr;
        }
        if (curr().kind != TokenKind::ParenR) {
            add_error("expected ')'");
            return nullptr;
        } else {
            advance();
        }
        return inner;
    }
    // 块表达式（闭包）
    if (curr().kind == TokenKind::BraceL) {
        return parse_block_expr();
    }
    // 函数字面量：/ (参数) 返回类型 { 函数体 }（除法是中缀，前缀位置出现 '/' 即闭包）
    if (curr().kind == TokenKind::Div) {
        return parse_func_literal();
    }
    // 前缀操作符
    if (is_prefix_op(curr().kind)) {
        Token op = curr();
        int prec = get_precedence(op.kind);
        advance();
        Expr* operand = parse_expr(prec + 1);
        if (!operand) {
            add_error("expected expression after " + op.quote());
            return nullptr;
        }
        return make_unary(op, operand, false);
    }
    // 数组字面量
    if (curr().kind == TokenKind::BracketL) {
        Location inner_loc = get_loc();
        advance();
        std::vector<Expr*> members;
        Expr* fill_value = nullptr;
        if (curr().kind != TokenKind::BracketR) {
            for (;;) {
                Expr* el = parse_expr(1);
                if (!el) {
                    add_error("expected element expression in array literal");
                    break;
                }
                if (curr().kind == TokenKind::Ellipsis) {
                    advance();
                    fill_value = el;
                    break;
                }
                members.push_back(el);
                if (curr().kind == TokenKind::Comma) {
                    advance();
                } else if (curr().kind == TokenKind::BracketR) {
                    break;
                }
            }
        }
        if (curr().kind == TokenKind::BracketR) {
            advance();
        } else {
            add_error("expected ']' to close array literal");
        }
        return cu_.add_node<Expr>(
            inner_loc,
            Expr::Array{std::move(members), fill_value}
        );
    }
    return nullptr;
}

// 后缀解析
Expr* Parser::parse_postfix(Expr* left) {
    Location loc = get_loc();
    
    switch (curr().kind) {
        // 函数调用：foo-<bar>(baz)--{}
        case TokenKind::GenericStart:
        case TokenKind::ParenL:
        case TokenKind::DoubleHyphen: {
            std::vector<ASTNode*> tpl_args = parse_template_args();
            std::vector<Expr*> args = parse_list<Expr*>(
                TokenKind::ParenL,
                TokenKind::ParenR,
                TokenKind::Comma,
                [this] { return parse_expr(1); },
                "argument"
            ).value_or({});
            while(curr().kind == TokenKind::DoubleHyphen) {
                if (curr().kind == TokenKind::BraceL) {
                    Expr* e = parse_block_expr();
                    if (!e) {
                        add_error("expected block statement after '--'");
                    }
                    args.push_back(e);
                } else break;
            }
            return cu_.add_node<Expr>(
                loc,
                Expr::Call{left, tpl_args, args}
            );
        }
        // 索引: []
        case TokenKind::BracketL: {
            advance();
            Expr* index = parse_expr();
            if (!index) {
                add_error("expected index expression");
            }
            if (curr().kind != TokenKind::BracketR) {
                add_error("expected ']'");
            } else {
                advance();
            }
            return cu_.add_node<Expr>(
                loc,
                Expr::Index{left, index}
            );
        }
        // 成员访问: .
        case TokenKind::Dot: {
            advance();
            if (!curr().is_ident()) {
                add_error("expected member name after '.'");
                return left;
            }
            std::string_view member = curr().get_val();
            advance();
            return cu_.add_node<Expr>(
                loc,
                Expr::Member{left, member}
            );
        }
        // 作用域: ::
        case TokenKind::ColonColon: {
            advance();
            if (!curr().is_ident()) {
                add_error("expected member name after '::'");
                return left;
            }
            std::string_view member = curr().get_val();
            advance();
            return cu_.add_node<Expr>(
                loc,
                Expr::Member{left, member}
            );
        }
        // -&取地址/-*解引用
        case TokenKind::Minus: {
            if (peek().kind == TokenKind::Mul || peek().kind == TokenKind::BitAnd) {
                advance();
                while(peek().kind == TokenKind::Mul || peek().kind == TokenKind::BitAnd) {
                    left = make_unary(curr(), left, true);
                    advance();
                }
                return left;
            }
        }
        default:
            return left;
    }
}

// 块表达式（闭包）
Expr* Parser::parse_block_expr() {
    Stmt* block = parse_block_stmt();
    if (!block) return nullptr;
    return cu_.add_node<Expr>(
        block->loc,
        Expr::Closure{
            std::vector<VarList*>{},
            std::vector<VarList*>{},
            block,
            std::vector<Token>{}
        }
    );
}

// 函数字面量：/ [捕获] (参数) 返回类型 { 函数体 }
Expr* Parser::parse_func_literal() {
    Location loc = get_loc();
    walk();
    std::vector<Token> capture;
    if (curr().kind == TokenKind::BracketL) {
        while (curr().kind != TokenKind::BraceR) {
            if (curr().is_ident()) {
                capture.push_back(curr());
            } else if (is_eof()) {
                add_error("unterminated capture list");
            } else {
                add_error("expected identifier");
            }
        }
    }
    std::vector<VarList*> params = parse_value_params();
    admit();
    std::vector<VarList*> returns;
    for (;;) {
        ModfList ml = parse_modfs();
        Location loc = get_loc();
        VarList* r = parse_var_decl(ml, loc);
        if (r) {
            returns.push_back(r);
        } else {
            break;
        }
    }
    Stmt* body = parse_block_stmt();
    if (!body) {
        add_error("expected function body");
    }
    return cu_.add_node<Expr>(
        loc,
        Expr::Closure{std::move(params), returns, body, capture}
    );
}

// 字面量解析
Expr* Parser::parse_literal() {
    Location loc = get_loc();
    LiteralKind kind;
    std::string_view value = curr().get_val();
    
    switch (curr().kind) {
        #define X(L,K) case TokenKind::L: kind = LiteralKind::K; break;
        LITERAL_MAPPING
        #undef X
        default:
            add_error("expected literal");
            return nullptr;
    }
    
    advance();
    return cu_.add_node<Expr>(
        loc,
        Expr::Literal{kind, std::move(value)}
    );
}

Expr* Parser::make_unary(Token op, Expr* operand, bool is_postfix) {
    OpKind op_kind = token_to_op(op.kind);
    if (!is_unary(op_kind)) {    
        add_error("unknown unary operator: " + op.quote());
        return operand;
    }
    return cu_.add_node<Expr>(
        op.loc,
        Expr::Unary{op_kind, operand, is_postfix}
    );
}

// 构造二元表达式
Expr* Parser::make_binary(Expr* left, Token op, Expr* right) {
    OpKind op_kind = token_to_op(op.kind);
    
    if (is_assign(op_kind)) {
        return cu_.add_node<Expr>(
            op.loc,
            Expr::Assign{left, op_kind, right}
        );
    }

    return cu_.add_node<Expr>(
        op.loc,
        Expr::Binary{left, op_kind, right}
    );
}

std::vector<Type*> Parser::parse_type_list_in_paren() {
    return parse_list<Type*>(
        TokenKind::ParenL,
        TokenKind::ParenR,
        TokenKind::Comma,
        [this] { return parse_type(); },
        "type"
    ).value_or({});
}

std::vector<VarList*> Parser::parse_value_params() {
    return parse_list<VarList*>(
        TokenKind::ParenL,
        TokenKind::ParenR,
        TokenKind::Semicolon,
        [this] {
            ModfList ml = parse_modfs();
            Location loc = get_loc();
            return parse_var_decl(ml, loc);
        },
        "parameter"
    ).value_or({});
}

// 类型解析

// 数组长度：只做可逆解析（walk），避免在探测性 parse_var_decl 失败后 restore() 过期
Expr* Parser::parse_array_length() {
    if (curr().kind == TokenKind::IntLiteral
        || curr().kind == TokenKind::UIntLiteral
        || curr().kind == TokenKind::Int8Literal
        || curr().kind == TokenKind::ShortLiteral
        || curr().kind == TokenKind::LongLiteral
        || curr().kind == TokenKind::FloatLiteral
        || curr().kind == TokenKind::DoubleLiteral) {
        Location loc = get_loc();
        LiteralKind kind;
        switch (curr().kind) {
            case TokenKind::Int8Literal: kind = LiteralKind::Int8; break;
            case TokenKind::ShortLiteral: kind = LiteralKind::Short; break;
            case TokenKind::IntLiteral: kind = LiteralKind::Int; break;
            case TokenKind::LongLiteral: kind = LiteralKind::Long; break;
            case TokenKind::UIntLiteral: kind = LiteralKind::UInt; break;
            default: kind = LiteralKind::Int; break;
        }
        std::string_view value = curr().get_val();
        walk();
        return cu_.add_node<Expr>(loc, Expr::Literal{kind, value});
    }
    if (curr().is_ident()) {
        Location loc = get_loc();
        std::string_view name = curr().get_val();
        walk();
        return cu_.add_node<Expr>(loc, Expr::Identifier{name});
    }
    return nullptr;
}

Type* Parser::parse_type() {
    Type* t = nullptr;

    if (curr().is_type()) {
        t = parse_primitive_type();
    } else if (curr().kind == TokenKind::Dollar) {
        t = parse_func_type();
    } else if (curr().kind == TokenKind::BracketL) {
        // 前缀定长数组 [N]T：先读长度，再读元素类型
        // 只用 walk() 保持可逆：parse_stmt 会探测性尝试 parse_var_decl，失败后 restore()
        Location aloc = get_loc();
        walk();
        Expr* length = nullptr;
        if (curr().kind != TokenKind::BracketR) {
            length = parse_array_length();
            if (!length) {
                add_error("expected array length expression");
                return nullptr;
            }
        }
        if (curr().kind != TokenKind::BracketR) {
            add_error("expected ']' after array length");
            return nullptr;
        }
        walk();
        Type* elem = parse_type();
        if (!elem) {
            add_error("expected element type after ']'");
            return nullptr;
        }
        t = cu_.add_node<Type>(aloc, Type::Array{elem, length});
    } else if (curr().is_ident()) {
        t = parse_generic_type();
    }

    if (!t) {
        return nullptr;
    }

    // 后缀类型修饰：T-* 指针、T-& 引用、T[] 长度推导数组、T[N] 定长数组
    for (;;) {
        if (curr().kind == TokenKind::Minus) {
            walk();
        }
        if(curr().kind == TokenKind::Mul) {
            Location ploc = get_loc();
            walk();
            t = cu_.add_node<Type>(ploc, Type::Pointer{t});
        } else if (curr().kind == TokenKind::BitAnd) {
            Location loc = get_loc();
            walk();
            t = cu_.add_node<Type>(loc, Type::Reference{t});
        } else if (curr().kind == TokenKind::BracketL) {
            Location loc = get_loc();
            walk();
            Expr* length = nullptr;
            if (curr().kind != TokenKind::BracketR) {
                length = parse_array_length();
                if (!length) {
                    add_error("expected array length expression");
                    return nullptr;
                }
                if (curr().kind != TokenKind::BracketR) {
                    add_error("expected ']' after array length");
                    return nullptr;
                }
            }
            walk();
            t = cu_.add_node<Type>(loc, Type::Array{t, length});
        } else {
            break;
        }
    }

    return t;
}

Type* Parser::parse_primitive_type() {
    if (!curr().is_type()) return nullptr;
    Location loc = get_loc();
    PrimitiveTypeKind kind = token_to_primitive_type(curr().kind);
    walk();
    return cu_.add_node<Type>(loc, Type::Primitive{kind});
}

Type* Parser::parse_func_type() {
    if (curr().kind != TokenKind::Dollar) return nullptr;
    Location loc = get_loc();
    walk();

    std::vector<Type*> param_types = parse_type_list_in_paren();

    Type* return_type = parse_type();
    if (!return_type) {
        add_error("expected return type after $()");
    }

    return cu_.add_node<Type>(loc, Type::Func{param_types, return_type});
}

Type* Parser::parse_generic_type() {
    if (!curr().is_ident()) return nullptr;
    Location loc = get_loc();
    std::string_view name = curr().get_val();
    walk();

    if (curr().kind == TokenKind::GenericStart) {
        std::vector<ASTNode*> args = parse_template_args();
        return cu_.add_node<Type>(loc, Type::Custom{name, std::move(args)});
    }

    return cu_.add_node<Type>(loc, Type::Custom{name, {}});
}

template<typename T, typename F>
std::optional<std::vector<T>> Parser::parse_list(
    TokenKind start,
    TokenKind end,
    TokenKind sep,
    F parse_element,
    const std::string& elmt_name
) {
    std::vector<T> elements;
    if (curr().kind != start) {
        return std::nullopt;
    }
    walk();
    if (curr().kind == end) {
        walk();
        return elements;
    }
    for (;;) {
        T elmt = parse_element();
        if (elmt) {
            elements.push_back(elmt);
        } else {
            add_error("expected " +
                elmt_name +
                " in '" +
                token_kind_to_string(start) +
                token_kind_to_string(end) +
                "'"
            );
        }
        if (curr().kind == sep) {
            walk();
        } else if(curr().kind == end) {
            walk();
            break;
        } else if (is_eof()) {
            add_error("unterminated '" +
                token_kind_to_string(start) +
                token_kind_to_string(end) +
                "'");
            break;
        } else if (!elmt) {
            walk();
        }
    }
    return elements;
}

std::string_view Parser::parse_code() {
    use_alias_ = false;
    if (curr().kind != TokenKind::BackQuote) {
        add_error("expected '`'");
    }
    advance();
    if (curr().kind == TokenKind::BackQuote) {
        advance();
        use_alias_ = true;
        return "";
    }
    size_t start = lexer_.get_pos();
    while (!is_eof() && curr().kind != TokenKind::BackQuote) {
        advance();
    }
    if (is_eof()) {
        add_error("unterminated code block (missing closing '`')");
        use_alias_ = true;
        return "";
    }
    size_t end = lexer_.get_pos();
    advance();
    std::string_view code = lexer_.get_source_range(start,end);
    use_alias_ = true;
    return code;
}

std::vector<TemplateParam> Parser::parse_template_params() {
    std::vector<TemplateParam> tpls;
    if (curr().kind != TokenKind::GenericStart) {
        return {};
    }
    walk();
    if (curr().kind == TokenKind::Greater) {
        walk();
        return {};
    }
    for (;;) {
        if (curr().is_ident()) {
            std::string_view name = curr().get_val();
            ASTNode* dft = nullptr;
            if (curr().kind == TokenKind::Assign) {
                dft = parse_expr();
            }
            tpls.push_back({name, dft});
        }
        if (curr().kind == TokenKind::Comma) {
            walk();
        } else if(curr().kind == TokenKind::Greater) {
            walk();
            break;
        } else if (is_eof()) {
            add_error("unterminated template parameter list");
            break;
        }
    }
    return tpls;
}

std::vector<ASTNode*> Parser::parse_template_args() {
    return parse_list<ASTNode*>(
        TokenKind::GenericStart,
        TokenKind::Greater,
        TokenKind::Comma,
        [this]{
            return parse_expr();
        },
        "template argument"
    ).value_or({});
}

} // namespace vo