#include "../include/parser.h"

#include <vector>
#include <string>
#include <string_view>

namespace cx {
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
    Token assign = lexer_.next_token();
    if (assign.kind != TokenKind::Assign) {
        add_error("expected '=' after alias name");
        return;
    }
    std::vector<Token> target;
    for(;;) {
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
    bool ltu = false; // 记录上一个token是否是unexpected，如果是则不连续报token unexpected
    while(!is_eof()){
        bool u = parse_using_namespace(); // 避免namespace decl识别报错，必须在前
        if (u){
            admit();
            ltu = false;
            continue;
        }
        if (curr().kind == TokenKind::Import ||
        (curr().kind == TokenKind::No && peek().kind == TokenKind::Import)){
            Import* i = parse_import();
            if (i){
                admit();
                ltu = false;
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
                ltu = false;
                continue;
            }
        }
        if (!ltu) {
            add_error("unexpected token " + curr().quote());
            ltu = true;
        }
        advance();
    }
    return &cu_;
}

// using namespace <path> —— 文件顶层 using 指令（hoisted），须在 parse_decl 之前尝试，
// 否则会被 parse_namespace_decl 误当作 brace 报错
bool Parser::parse_using_namespace() {
    if (curr().kind == TokenKind::Using) {
        advance();
        if (curr().kind != TokenKind::Namespace) {
            add_error("expected 'namespace' after 'using'");
            return true;
        }
        Location loc = get_loc();
        advance();
        Expr* path = parse_type();
        if (!path) {
            add_error("expected namespace path after 'using namespace'");
        }
        cu_.add_using_namespace(path, loc);
        return true;
    }
    // 已废弃：namespace <path>:
    if (curr().kind != TokenKind::Namespace) return false;
    if (!peek(1).is_ident()) return false;
    if (peek(2).kind == TokenKind::BraceL) return false;
    Location loc = get_loc();
    advance();
    std::vector<std::string_view> parts;
    while (curr().is_ident()) {
        parts.push_back(curr().get_val());
        advance();
        if (curr().kind == TokenKind::ColonColon) {
            advance();
            continue;
        }
        break;
    }
    if (curr().kind == TokenKind::Colon) advance();
    Expr* path = nullptr;
    for (std::string_view part : parts) {
        if (!path) {
            path = cu_.add_node<Expr>(loc, Expr::Identifier{part});
        } else {
            path = cu_.add_node<Expr>(loc, Expr::Member{path, part});
        }
        path->is_type = true;
    }
    cu_.add_using_namespace(path, loc);
    add_warning("'namespace <path>:' is deprecated, use 'using namespace <path>'");
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
        bool is_str = (curr().kind == TokenKind::StringLiteral);
        std::unique_ptr<Import> result = std::make_unique<Import>(
            get_loc(), curr().get_val(), no, is_str
        );
        advance();
        Import* raw = result.get();
        cu_.add_node(std::move(result));
        cu_.imports.push_back(raw);
        return raw;
    }
    add_error("expected filename or \"filename\"");
    return nullptr;
}

Decl* Parser::parse_decl() {
    if (!curr().is_word() && curr().kind != TokenKind::At
        && curr().kind != TokenKind::Dollar
        && curr().kind != TokenKind::BracketL
        && curr().kind != TokenKind::Hash) return nullptr;
    SpecList ml = parse_specs();
    auto sp = point();
    Location loc = get_loc();
    // '#mods @Class {}' 修饰注入：可逆探测，非 '@' 时退回交给类型解析
    if (curr().kind == TokenKind::Hash) {
        std::vector<Expr*> injs;
        while (curr().kind == TokenKind::Hash) {
            Expr* mod = parse_modification();
            if (!mod) break;
            injs.push_back(mod);
        }
        if (curr().kind == TokenKind::At) {
            return parse_class_decl(ml, loc, std::move(injs));
        }
        restore(sp);
    }
    // 易于确定的优先级更高
    if (curr().kind == TokenKind::At) return parse_class_decl(ml, loc);
    if (curr().kind == TokenKind::Enum) return parse_enum_decl(ml, loc);
    if (curr().kind == TokenKind::Keep) return parse_keep_decl(ml, loc);
    if (curr().kind == TokenKind::Operator) return parse_operator_overload(ml, loc);
    if (curr().kind == TokenKind::Conversion) return parse_conversion_decl(ml, loc);
    if (curr().kind == TokenKind::Extern) return parse_extern_decl(ml, loc);
    if (Decl* d = parse_namespace_decl(ml, loc)) return d; restore(sp);
    if (Decl* d = parse_func_decl(ml, loc)) return d; restore(sp);
    if (Decl* d = parse_var_decl(ml, loc)) { return d; } restore(sp);
    if (Decl* d = parse_special_method_decl(ml, loc)) return d; restore(sp);
    return nullptr;
}

// 第三层函数负责读取、确认和报错
// 无法确认的函数只能进行可逆操作
SpecList Parser::parse_specs() {
    SpecList ml;
    for(;;){
        if (curr().is_spec()) {
            ml.set(token_to_spec(
                curr().kind
            ));
            walk();
        } else {
            break;
        }
    }
    return ml;
}

Expr* Parser::parse_modification() {
    if (curr().kind != TokenKind::Hash) return nullptr;
    Location loc = get_loc();
    walk();
    if (curr().is_type()) {
        PrimitiveTypeKind kind = token_to_primitive_type(curr().kind);
        walk();
        Expr* mod = cu_.add_node<Expr>(loc, Expr::Primitive{kind});
        mod->is_type = true;
        return mod;
    }
    if (!curr().is_ident()) {
        add_error("expected identifier or base type after '#'");
        return nullptr;
    }
    std::string_view name = curr().get_val();
    walk();

    Expr* mod = cu_.add_node<Expr>(loc, Expr::Identifier{name});
    mod->is_type = true;
    return mod;
}

// 函数的识别：名称后接参数括号
Func* Parser::parse_func_decl(SpecList &ml, Location &loc) {
    if (!curr().is_ident()) return nullptr;
    if (peek().kind != TokenKind::ParenL
        && peek().kind != TokenKind::BraceL) return nullptr;
    std::string_view name = curr().get_val();
    walk();
    if (curr().kind != TokenKind::ParenL) return nullptr;
    bool variadic = false;
    std::vector<VarList*> params = parse_value_params(&variadic);
    admit();
    std::vector<VarList*> returns;
    for (;;) {
        if (curr().kind == TokenKind::BraceL) break;
        SpecList ml = parse_specs();
        Location loc = get_loc();
        VarList* r = parse_var_decl(ml, loc);
        if (r) {
            returns.push_back(r);
        } else {
            break;
        }
    }
    Stmt* body = parse_block_stmt();
    if (!body) add_error("expected function body");
    Func* f = cu_.add_node<Func>(
            loc, std::move(name),
            std::move(ml),
            std::move(params),
            std::move(returns),
            body
    );
    f->is_variadic = variadic;
    return f;
}

// 变量的识别：类型或推导符加标识符
VarList* Parser::parse_var_decl(SpecList &ml, Location &loc) {
    Expr* type;
    auto sp = point();
    if (curr().kind == TokenKind::Let) {
        add_error("'let' is not implemented yet");
        type = cu_.add_node<Expr>(get_loc(), Expr::Inferred{});
        advance();
    } else {
        type = parse_type();
        if (!type) return nullptr;
    }
    std::vector<VarItem*> vars;
    for (;;) {
        if (curr().is_ident()) {
            std::string_view name = curr().get_val();
            Expr* init = nullptr;
            walk();
            // 后缀数组维度：T name[d1][d2] 等价于 T[d1][d2] name。
            // 与前缀形式混写（T[a] name[b]）没有唯一解释，直接报错
            Expr* vtype = type;
            bool mixed_dim = type && type->is_array_type() &&
                             curr().kind == TokenKind::BracketL;
            if (mixed_dim)
                add_error("cannot combine prefix and postfix array dimensions");
            while (curr().kind == TokenKind::BracketL) {
                Location aloc = get_loc();
                advance();
                Expr* len = nullptr;
                if (curr().kind != TokenKind::BracketR) {
                    len = parse_array_length();
                    if (!len) {
                        add_error("expected array length expression");
                        break;
                    }
                }
                if (curr().kind != TokenKind::BracketR) {
                    add_error("expected ']' after array length");
                    break;
                }
                advance();
                // 混用已报错，后缀维度只消费不再叠加，避免级联错误
                if (!mixed_dim)
                    vtype = append_array_dim(aloc, vtype, len,
                        len ? ArrayKind::Fixed : ArrayKind::Infer);
            }
            if (curr().kind == TokenKind::Assign) {
                walk();
                init = parse_expr();
                if (!init) {
                    add_error("expected expression");
                    continue;
                }
            } else if(type->is_inferred()) {
                add_error("can not infer type");
            }
            vars.push_back(cu_.add_node<VarItem>(get_loc(), name, init, vtype));
        } else if (vars.empty()) {
            restore(sp);
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

Class* Parser::parse_class_decl(SpecList &ml, Location &loc, std::vector<Expr*> injs) {
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
    SpecList nml = parse_specs();
    ml.merge(nml);
    // 尾随注入：#mod 出现在类名之后、成员之前（如 `@b #qux { }`）
    std::vector<Expr*> trailing;
    while (curr().kind == TokenKind::Hash) {
        Expr* mod = parse_modification();
        if (!mod) break;
        trailing.push_back(mod);
    }
    std::vector<Decl*> members = parse_members();
    Class* raw = cu_.add_node<Class>(
        loc, std::move(name), std::move(ml), std::move(members),
        std::move(injs), std::move(trailing)
    );
    return raw;
}

Namespace* Parser::parse_namespace_decl(SpecList &ml, Location &loc) {
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
    SpecList default_specs;    // 修饰符指令（pub: / spec ...: / spec no:）作用于后续成员
    if (curr().kind == TokenKind::BraceL) {
        advance();
    }
    while (curr().kind != TokenKind::BraceR && !is_eof()) {
        if (parse_modifier_directive(default_specs)) {
            admit();
            continue;
        }
        if (Decl* d = parse_decl()) {
            d->specs.merge(default_specs);
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

bool Parser::parse_modifier_directive(SpecList& defaults) {
    // pub:
    if (curr().kind == TokenKind::Pub && peek().kind == TokenKind::Colon) {
        defaults.set(SpecKind::Pub);
        advance(2);
        consume_statement_end();
        return true;
    }
    // priv:
    if (curr().kind == TokenKind::Priv && peek().kind == TokenKind::Colon) {
        defaults.del(SpecKind::Pub);
        advance(2);
        consume_statement_end();
        return true;
    }
    // spec: 清空默认修饰符
    // spec <spec>...: 修改默认修饰符
    if (curr().kind == TokenKind::Spec) {
        advance();
        auto sp = point();
        if (curr().kind == TokenKind::Colon) {
            defaults = SpecList();
            advance();
            consume_statement_end();
            return true;
        }

        SpecList w = parse_specs();
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

Operator* Parser::parse_operator_overload(SpecList &ml, Location &loc) {
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
    if (curr().kind != TokenKind::ParenL) return cu_.add_node<Operator>();
    std::vector<VarList*> params = parse_value_params();
    admit();
    std::vector<VarList*> returns;
    for (;;) {
        SpecList ml = parse_specs();
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
            std::move(params),
            std::move(returns),
            body
    );
}

OpKind Parser::parse_operator() {
    auto sp = point();
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

SpecialMethod* Parser::parse_special_method_decl(SpecList &ml, Location &loc) {
    TokenKind tok_kind = curr().kind;
    SpecialMethod::Kind kind;
    if (tok_kind == TokenKind::Init){
        advance();
        std::vector<VarList*> params = parse_value_params();
        admit();
        Stmt* b = parse_block_stmt();
        if (!b) {
            add_error("expected INIT body");
        }
        return cu_.add_node<SpecialMethod>(
            loc, SpecialMethod::Kind::Init, std::move(ml), b,
            std::move(params)
        );
    } else if (tok_kind == TokenKind::Drop){
        kind = SpecialMethod::Kind::Drop;
    } else if (tok_kind == TokenKind::Ref){
        kind = SpecialMethod::Kind::Ref;
    } else if (tok_kind == TokenKind::Deref){
        kind = SpecialMethod::Kind::Deref;
    } else return nullptr;
    advance();
    Stmt* b = parse_block_stmt();
    if (!b) {
        add_error("expected method body");
    }
    return cu_.add_node<SpecialMethod>(
        loc, kind, std::move(ml), b
    );
}

Conversion* Parser::parse_conversion_decl(SpecList &ml, Location &loc) {
    if (curr().kind != TokenKind::Conversion) return nullptr;
    advance();
    if (curr().kind != TokenKind::ParenL) {
        add_error("expected '(' after conversion");
        return cu_.add_node<Conversion>();
    }
    std::vector<VarList*> params = parse_value_params();
    admit();
    std::vector<VarList*> returns;
    for (;;) {
        SpecList rml = parse_specs();
        Location rloc = get_loc();
        VarList* r = parse_var_decl(rml, rloc);
        if (r) returns.push_back(r); else break;
    }
    Stmt* body = parse_block_stmt();
    if (!body) add_error("expected conversion body");
    return cu_.add_node<Conversion>(loc, "conversion", std::move(ml),
        std::move(params), std::move(returns), body);
}

Extern* Parser::parse_extern_decl(SpecList &ml, Location &loc) {
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
    for (Decl* m : members) {
        if (!m) continue;
        if (m->kind == DeclKind::Func ||
            m->kind == DeclKind::Operator ||
            m->kind == DeclKind::Conversion) {
            static_cast<Func*>(m)->is_extern_decl = true;
        }
    }
    return cu_.add_node<Extern>(
        loc,
        lib_name,
        std::move(ml),
        std::move(members)
    );
}

Enum* Parser::parse_enum_decl(SpecList& ml, Location& loc) {
    if (curr().kind != TokenKind::Enum) return nullptr;
    advance();
    if (!curr().is_ident()) {
        add_error("expected enum name after 'enum'");
        return cu_.add_node<Enum>();
    }
    std::string_view name = curr().get_val();
    advance();
    std::vector<EnumVariant*> variants;
    if (curr().kind != TokenKind::BraceL) {
        add_error("expected '{' after enum name");
        return cu_.add_node<Enum>(loc, name, std::move(ml), std::move(variants));
    }
    advance();
    while (curr().is_ident()) {
        variants.push_back(cu_.add_node<EnumVariant>(get_loc(), curr().get_val()));
        advance();
    }
    if (curr().kind == TokenKind::BraceR) {
        advance();
    } else {
        add_error("expected terminating '}'");
    }
    return cu_.add_node<Enum>(loc, name, std::move(ml), std::move(variants));
}

Keep* Parser::parse_keep_decl(SpecList& ml, Location& loc) {
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
    if (curr().kind == TokenKind::BraceL) {
        result = parse_block_stmt();
    }else if (curr().kind == TokenKind::If) {
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
    }else if (curr().kind == TokenKind::Keep){
        result = parse_keep_stmt();
    }else {
    {
        auto sp = point();
        // 变量申明语句（含元类型别名 `type T = <meta-expr>` → T 为 MetaType 值变量）
        Location loc = get_loc();
        SpecList ml = parse_specs();
        Location l = get_loc();
        VarList* vl = parse_var_decl(ml,l);
        if (vl) {
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
    
    // 处理后缀链（类型后缀按类型解释）
    left = parse_postfix(left, left->is_type);
    if (!left) return nullptr;
    
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
        Expr* target = parse_type();
        if (curr().kind != TokenKind::ParenL) {
            return target;
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

    if (curr().kind == TokenKind::Null) {
        advance();
        return cu_.add_node<Expr>(loc, Expr::Null{});
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
    // spawn(call)：以新线程执行函数调用，类型继承被调函数的返回类型
    if (curr().kind == TokenKind::Spawn) {
        advance();
        if (curr().kind != TokenKind::ParenL) {
            add_error("expected '(' after 'spawn'");
            return nullptr;
        }
        advance();
        Expr* call = parse_expr();
        if (!call) add_error("expected call expression in 'spawn'");
        if (curr().kind != TokenKind::ParenR) {
            add_error("expected ')' after spawn call");
        } else {
            advance();
        }
        return cu_.add_node<Expr>(loc, Expr::Spawn{call});
    }
    // 块表达式（闭包）
    if (curr().kind == TokenKind::BraceL) {
        return parse_block_expr();
    }
    // 函数字面量：\ [捕获] (参数) 返回类型 { 函数体 }（除法是中缀，前缀位置出现 '\' 即闭包）
    if (curr().kind == TokenKind::Backslash) {
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

// 追加一维数组类型：先写的维度在外层，后写的在内层。
// int[A][B] 里 A 是最外层长度、B 是最内层长度；后缀声明 int a[A][B] 同理。
// 故新维度总是插入已有数组类型最内层 element 的位置
Expr* Parser::append_array_dim(const Location& loc, Expr* base,
                               Expr* len, ArrayKind kind) {
    // 递归到最内层再插入：新维度永远是最内层。
    // int a[A][B][C] 的三次追加依次得到 A->int、A->B->int、A->B->C->int，
    // 即书写顺序从外到内，与 C 的定长数组声明一致
    if (base && base->is_array_type()) {
        auto* a = base->get_array_type();
        a->element = append_array_dim(loc, a->element, len, kind);
        return base;
    }
    Expr* node = cu_.add_node<Expr>(loc, Expr::ArrayType{base, len, kind});
    node->is_type = true;
    return node;
}

// 统一后缀链：is_type 决定 [] 与 -*/-& 的解释，并决定可逆 walk 还是 advance
Expr* Parser::parse_postfix(Expr* left, bool is_type) {
    // 类型位置的可逆性由 parse_type 的 trial 守卫保证，advance 在 trial 中即 walk
    for (;;) {
        Location loc = get_loc();
        switch (curr().kind) {
            // 函数调用：foo(bar) / foo(bar)--{} / foo--{}
            case TokenKind::ParenL:
            case TokenKind::DoubleHyphen: {
                if (is_type) return left;
                std::vector<Expr*> args;
                if (curr().kind == TokenKind::ParenL) {
                    args = parse_list<Expr*>(
                        TokenKind::ParenL,
                        TokenKind::ParenR,
                        TokenKind::Comma,
                        [this] { return parse_expr(1); },
                        "argument"
                    ).value_or({});
                }
                while (curr().kind == TokenKind::DoubleHyphen) {
                    advance();  // 消费 '--'
                    if (curr().kind != TokenKind::BraceL) {
                        add_error("expected block statement after '--'");
                        break;
                    }
                    Expr* e = parse_block_expr();
                    if (!e) {
                        add_error("expected block statement after '--'");
                        break;
                    }
                    args.push_back(e);
                }
                left = cu_.add_node<Expr>(
                    loc,
                    Expr::Call{left, std::move(args)}
                );
                continue;
            }
            // 索引 vs 数组类型: []
            case TokenKind::BracketL: {
                advance();
                if (is_type) {
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
                    advance();
                    Expr* node = cu_.add_node<Expr>(loc, Expr::ArrayType{
                        left, length,
                        length ? ArrayKind::Fixed : ArrayKind::Infer});
                    node->is_type = true;
                    left = node;
                } else {
                    Expr* index = parse_expr();
                    if (!index) {
                        add_error("expected index expression");
                    }
                    if (curr().kind != TokenKind::BracketR) {
                        add_error("expected ']'");
                    } else {
                        advance();
                    }
                    left = cu_.add_node<Expr>(
                        loc,
                        Expr::Index{left, index}
                    );
                }
                continue;
            }
            // 所有权切片类型: T{} 或 T{n}
            // 与函数体起始的 '{' 区分：只有形如 {} 或 {长度} 的才当切片类型
            case TokenKind::BraceL: {
                if (!is_type) return left;
                if (peek().kind != TokenKind::BraceR && !peek().is_ident() &&
                    !peek().is_literal()) return left;
                advance();
                Expr* length = nullptr;
                if (curr().kind != TokenKind::BraceR) {
                    length = parse_array_length();
                    if (!length) {
                        add_error("expected slice length expression");
                        return left;
                    }
                }
                if (curr().kind != TokenKind::BraceR) {
                    add_error("expected '}' to close slice type");
                    return left;
                }
                advance();
                Expr* node = cu_.add_node<Expr>(loc,
                    Expr::ArrayType{left, length, ArrayKind::Slice});
                node->is_type = true;
                left = node;
                continue;
            }
            // 成员/作用域: . ::
            case TokenKind::Dot:
            case TokenKind::ColonColon: {
                advance();
                if (!curr().is_ident()) {
                    add_error("expected member name after '.' or '::'");
                    return left;
                }
                std::string_view member = curr().get_val();
                advance();
                Expr* node = cu_.add_node<Expr>(
                    loc,
                    Expr::Member{left, member}
                );
                node->is_type = left->is_type;
                left = node;
                continue;
            }
            // -* 解引用/指针，-& 取地址/引用
            case TokenKind::Minus: {
                if (peek().kind == TokenKind::Mul || peek().kind == TokenKind::BitAnd) {
                    advance();
                    if (is_type) {
                        while(curr().kind == TokenKind::Mul || curr().kind == TokenKind::BitAnd) {
                            Location uloc = get_loc();
                            Expr* node;
                            if (curr().kind == TokenKind::Mul) {
                                node = cu_.add_node<Expr>(uloc, Expr::Pointer{left});
                            } else {
                                node = cu_.add_node<Expr>(uloc, Expr::Reference{left});
                            }
                            walk();
                            left = node;
                            node->is_type = true;
                        }
                    } else {
                        while(curr().kind == TokenKind::Mul || curr().kind == TokenKind::BitAnd) {
                            left = make_unary(curr(), left, true);
                            advance();
                        }
                    }
                    continue;
                }
                if (is_type && peek().kind == TokenKind::BracketL) {
                    advance();
                    continue;
                }
                return left;
            }
            default:
                return left;
        }
    }
}

// 块表达式：语法可读，但不构造块节点（见 future.md 的闭包捕获）
// 返回占位表达式只为让解析继续，避免后续报一堆无意义的连锁错误；
// 已有 error，翻译阶段不会执行到
Expr* Parser::parse_block_expr() {
    Location loc = get_loc();
    errors_.push_back(Diagnostic(DiagLevel::Error,
        "block expression is not supported yet", loc));
    parse_block_stmt();
    return cu_.add_node<Expr>(loc, Expr::Inferred{});
}

// 函数字面量：\ (参数) 返回类型 { 函数体 }
// 语法可读并完整消费，但不构造函数字面量节点（见 future.md 的闭包捕获）
Expr* Parser::parse_func_literal() {
    Location loc = get_loc();
    walk();
    errors_.push_back(Diagnostic(DiagLevel::Error,
        "function literal is not supported yet", loc));
    parse_value_params();
    admit();
    for (;;) {
        SpecList ml = parse_specs();
        Location vloc = get_loc();
        if (!parse_var_decl(ml, vloc)) break;
    }
    parse_block_stmt();
    return cu_.add_node<Expr>(loc, Expr::Inferred{});
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

std::vector<Expr*> Parser::parse_type_list_in_paren() {
    return parse_list<Expr*>(
        TokenKind::ParenL,
        TokenKind::ParenR,
        TokenKind::Comma,
        [this] { return parse_type(); },
        "type"
    ).value_or({});
}

std::vector<VarList*> Parser::parse_value_params(bool* variadic) {
    if (variadic) *variadic = false;
    std::vector<VarList*> params;
    if (curr().kind != TokenKind::ParenL) return params;
    walk();
    if (curr().kind == TokenKind::ParenR) {
        walk();
        return params;
    }
    for (;;) {
        // 变参省略号：仅允许出现在形参表末尾
        if (curr().kind == TokenKind::Ellipsis) {
            if (variadic) *variadic = true;
            walk();
            if (curr().kind != TokenKind::ParenR)
                add_error("'...' must be the last parameter");
            break;
        }
        SpecList ml = parse_specs();
        Location loc = get_loc();
        VarList* elmt = parse_var_decl(ml, loc);
        if (elmt) {
            params.push_back(elmt);
        } else {
            add_error("expected parameter in '()'");
        }
        if (curr().kind == TokenKind::ParenR) break;
        if (curr().kind == TokenKind::Semicolon || curr().kind == TokenKind::Comma) {
            walk();
            if (curr().kind == TokenKind::ParenR) break;
            continue;
        }
        if (is_eof()) {
            add_error("unterminated '()'");
            break;
        }
        if (!elmt) walk();
    }
    if (curr().kind == TokenKind::ParenR) walk();
    return params;
}

// 类型解析

// 数组长度：只做可逆解析（walk），避免在探测性 parse_var_decl 失败后 restore() 过期
Expr* Parser::parse_array_length() {
    return parse_expr();
}

Expr* Parser::parse_type_atom() {
    Expr* t = nullptr;
    if (curr().is_type()) {
        t = parse_primitive_type();
    } else if (curr().kind == TokenKind::Dollar) {
        t = parse_func_type();
    } else if (curr().is_ident()) {
        t = parse_generic_type();
    }
    if (!t) return nullptr;
    // 后缀链与表达式共用；类型位置按类型解释
    return parse_postfix(t, true);
}

// 类型解析全程可逆：探测性 parse_decl 失败后 restore 必须仍然有效。
// 期间 advance 退化为 walk，退出时恢复。
Expr* Parser::parse_type() {
    bool saved = trial_;
    trial_ = true;
    Expr* t = parse_type_atom();
    trial_ = saved;
    return t;
}

Expr* Parser::parse_primitive_type() {
    if (!curr().is_type()) return nullptr;
    Location loc = get_loc();
    PrimitiveTypeKind kind = token_to_primitive_type(curr().kind);
    walk();
    Expr* t = cu_.add_node<Expr>(loc, Expr::Primitive{kind});
    t->is_type = true;
    return t;
}

Expr* Parser::parse_func_type() {
    if (curr().kind != TokenKind::Dollar) return nullptr;
    Location loc = get_loc();
    walk();

    std::vector<Expr*> param_types = parse_type_list_in_paren();

    Expr* return_type = parse_type();
    if (!return_type) {
        add_error("expected return type after $() or $");
    }

    Expr* t = cu_.add_node<Expr>(loc, Expr::FuncPtr{param_types, return_type});
    t->is_type = true;
    return t;
}

Expr* Parser::parse_generic_type() {
    if (!curr().is_ident()) return nullptr;
    Location loc = get_loc();
    std::string_view name = curr().get_val();
    walk();

    Expr* t = cu_.add_node<Expr>(loc, Expr::Identifier{name});
    t->is_type = true;
    return t;
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
        if (curr().kind == end) {
            walk();
            break;
        }
        // 逗号与 sep 均是强制分隔符，允许省略
        if (curr().kind == sep || curr().kind == TokenKind::Comma) {
            walk();
            if (curr().kind == end) {
                walk();
                break;
            }
            continue;
        }
        if (is_eof()) {
            add_error("unterminated '" +
                token_kind_to_string(start) +
                token_kind_to_string(end) +
                "'");
            break;
        }
        if (!elmt) {
            walk();
        }
    }
    return elements;
}

std::string_view Parser::parse_code() {
    use_alias_ = false;
    if (curr().kind != TokenKind::RawCode) {
        add_error("expected code block");
        use_alias_ = true;
        return "";
    }
    std::string_view code = curr().val;
    advance();
    use_alias_ = true;
    return code;
}

} // namespace cx