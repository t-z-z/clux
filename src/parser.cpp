#include "../include/parser.h"
#include "token.h"

#include <vector>
#include <string>
#include <string_view>

namespace cx {
    // 从外向内查找 define 替换解开链式替换
std::optional<std::vector<Token>> Parser::expand_define(const Token& name) const {
    if (defines_.empty() || !use_define_) return std::nullopt;
    std::vector<Token> r = {name};
    for (size_t i = 0; i < defines_.size(); ++i) {
        const auto& level = defines_[i];
        for (size_t j = 0; j < r.size(); ++j) {
            if (!r[j].is_word()) continue;
            // 替换后 j 跳到插入段之后，可能已越界：插入的 token 由 get_token 重新派发再替换
            for (size_t k = 0; k < level.size() && j < r.size(); ++k) {
                if (level[k].name == r[j].get_val() 
                    && !( // 覆盖它的 define 如果存在则不触发此替换
                        level[k].cvr_lv < defines_.size()
                        && level[k].cvr_id < defines_[level[k].cvr_lv].size()
                        && level[k].name == defines_[level[k].cvr_lv][level[k].cvr_id].name)
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
// define foo = bar: foo 在本作用域内按 bar 的 token 替换，'}' 处失效
void Parser::scan_define_decl() {
    if (!use_define_) return;
    Token name_tok = get_token();
    Token assign = lexer_.next_token();
    if (assign.kind != TokenKind::Assign) {
        add_error("expected '=' after define name");
        return;
    }
    std::vector<Token> target;
    for(;;) {
        Token t = get_token();
        if (t.kind == TokenKind::Colon || t.kind == TokenKind::Eof) break;
        target.push_back(t);
    }
    if (target.size() == 1 && target[0].get_val() == name_tok.get_val()) {
        add_warning("the define is the same as the original name");
    }
    // 覆盖最靠近的 define
    for (size_t i = defines_.size(); i-- > 0; ) {
        auto& level = defines_[i];
        for (size_t j = level.size(); j-- > 0; ) {
            if (level[j].name == name_tok.get_val()) {
                level[j].cvr_lv = defines_.size() -1;
                level[j].cvr_id = defines_.back().size();
                return;
            }
        }
    }
    if (defines_.empty()) defines_.push_back({});
    defines_.back().push_back({name_tok.get_val(), std::move(target), ~size_t(0), ~size_t(0)});
}
// 申明解析部分

// 诊断部分

// 主错误只讲"在等什么"与"遇到什么"；已经看懂的结构单独作为 note，两者不混在一句里
void Parser::fail_expected(const std::string& expected, const std::string& understood) {
    if (probe_ > 0) return;
    add_error("expected " + expected + ", found " + curr().quote());
    if (!understood.empty()) {
        errors_.back().children.push_back(
            Diagnostic(DiagLevel::Note, "parser understood: " + understood, get_loc()));
    }
}

// 无法归类的 token：若前一个结构已提交，说明错误发生在它之后
void Parser::fail_unexpected(const std::string& previous) {
    if (probe_ > 0) return;
    std::string msg = "unexpected token " + std::string(curr().quote());
    if (!previous.empty()) msg += " after " + previous;
    add_error(msg);
}

// 只需短描述：字面量取原文，其余不展开表达式
std::string Parser::describe_expr_short(const Expr* e) const {
    if (!e) return "";
    if (const auto* l = e->get_literal()) return std::string(l->value);
    if (const auto* i = e->get_identifier()) return std::string(i->name);
    return "N";
}

std::string Parser::describe_type(const Type* t) const {
    if (!t) return "<unknown>";
    if (const auto* p = t->get_primitive()) {
        return token_kind_to_string(static_cast<TokenKind>(
            static_cast<int>(TokenKind::Int8) + static_cast<int>(p->kind)));
    }
    if (t->is_void()) return "void";
    if (t->is_null()) return "null";
    if (t->is_inferred()) return "<inferred>";
    if (t->is_string_literal_type()) return "<string literal>";
    if (const auto* p = t->get_pointer()) return describe_type(p->pointee) + "-*";
    if (const auto* r = t->get_reference()) return describe_type(r->target) + "-&";
    if (const auto* a = t->get_array_type()) {
        std::string len = describe_expr_short(a->length);
        switch (a->kind) {
            case ArrayKind::Fixed: return describe_type(a->element) + "[" + len + "]";
            case ArrayKind::Infer: return describe_type(a->element) + "[]";
            case ArrayKind::Slice: return describe_type(a->element) + "{" + len + "}";
        }
    }
    if (const auto* i = t->get_identifier()) return std::string(i->name);
    if (const auto* m = t->get_member())
        return describe_type(m->target) + "." + std::string(m->member);
    if (const auto* g = t->get_generic_type()) {
        std::string s = describe_type(g->base) + "-<";
        for (size_t k = 0; k < g->args.size(); ++k) {
            if (k) s += ", ";
            s += describe_type(g->args[k]);
        }
        return s + ">";
    }
    if (const auto* f = t->get_func_ptr()) {
        std::string s = "$" + describe_type(f->return_type) + "(";
        for (size_t k = 0; k < f->params.size(); ++k) {
            if (k) s += ", ";
            s += describe_type(f->params[k]);
        }
        return s + ")";
    }
    return "<type>";
}

std::string Parser::describe_decl(const Decl* d) const {
    if (!d) return "declaration";
    switch (d->kind) {
        case DeclKind::VarList: {
            const auto* vl = static_cast<const VarList*>(d);
            std::string s = "a variable declaration";
            if (!vl->vars.empty()) {
                s += " \"";
                for (size_t k = 0; k < vl->vars.size(); ++k) {
                    if (k) s += ", ";
                    s += std::string(vl->vars[k]->name);
                }
                s += "\"";
            }
            const Type* t = vl->type;
            const auto* a = t ? t->get_array_type() : nullptr;
            if (a && a->kind == ArrayKind::Slice)
                s += " of slice type " + describe_type(t);
            else if (t && !t->is_inferred())
                s += " of type " + describe_type(t);
            return s;
        }
        case DeclKind::Func: return "a function \"" + std::string(d->name) + "\"";
        case DeclKind::Operator: return "an operator overload";
        case DeclKind::Conversion: return "a conversion method";
        case DeclKind::SpecialMethod: return "a special method";
        case DeclKind::Class: return "a class \"" + std::string(d->name) + "\"";
        case DeclKind::Namespace: return "a namespace \"" + std::string(d->name) + "\"";
        case DeclKind::Extern: return "an extern block \"" + std::string(d->name) + "\"";
        case DeclKind::Enum: return "an enum \"" + std::string(d->name) + "\"";
        case DeclKind::TypeDef: return "a typedef \"" + std::string(d->name) + "\"";
        case DeclKind::ExprDef: return "an exprdef \"" + std::string(d->name) + "\"";
        case DeclKind::Keep: return "a keep block";
    }
    return "declaration";
}

// 顶层函数不负责退回
CompUnit* Parser::parse() {
    bool ltu = false; // 记录上一个token是否是unexpected，如果是则不连续报token unexpected
    std::string prev; // 上一个已提交的结构，用于"在某结构之后"的报错
    while(!is_eof()){
        size_t before = errors_.size();
        bool u = parse_using_namespace(); // 避免namespace decl识别报错，必须在前
        if (u){
            prev = "a using directive";
            ltu = false;
            continue;
        }
        if (curr().kind == TokenKind::Import ||
        (curr().kind == TokenKind::No && peek().kind == TokenKind::Import)){
            Import* i = parse_import();
            if (i){
                prev = "an import";
                ltu = false;
                continue;
            }
        } else {
            Decl* d = parse_decl();
            if (d){
                cu_.top_decls.push_back(d);
                if (d->kind == DeclKind::Class) {
                    if (cu_.is_type_name(d->name)) {
                        add_error("mutiple definition of class "+ std::string(d->name));
                    } else if (!d->name.empty()) {
                        cu_.register_type(d->name, d);
                    }
                }
                prev = describe_decl(d);
                ltu = false;
                continue;
            }
        }
        // 已经给出更具体的语法错误时不再兜底；紧随其后的残留 token 也一并静默，
        // 直到重新对上一个声明起点
        if (errors_.size() > before) {
            ltu = true;
        } else if (!ltu) {
            fail_unexpected(prev);
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
        Type* path = parse_type();
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
    Type* path = nullptr;
    for (std::string_view part : parts) {
        if (!path) {
            path = cu_.add_node<Type>(loc, Type::Identifier{part});
        } else {
            path = cu_.add_node<Type>(loc, Type::Member{path, part});
        }
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

// 声明的起始 token：词（关键字/标识符）、'$' 函数指针类型、
// '[' 前缀数组类型、'#' 修饰注入
static bool is_decl_start(const Token &tok) {
    return tok.is_word()
        || tok.kind == TokenKind::Dollar
        || tok.kind == TokenKind::Hash;
}
// 变量名之后仍属该变量声明的 token；其余（'{'、'('、'-<'、'.' 等）
// 说明该标识符是下一个声明的开头
static bool is_var_tail(const Token &tok) {
    return tok.kind == TokenKind::Assign || tok.kind == TokenKind::Comma
        || tok.kind == TokenKind::BracketL || tok.kind == TokenKind::Semicolon
        || tok.kind == TokenKind::BraceR || tok.kind == TokenKind::Eof;
}

Decl* Parser::parse_decl() {
    if (!is_decl_start(curr())) return nullptr;
    SpecList ml = parse_specs();
    Location loc = get_loc();

    // 关键字已经唯一确定形态，不再试探
    switch (curr().kind) {
        case TokenKind::TypeDef: return parse_typedef_decl();
        case TokenKind::ExprDef: return parse_exprdef_decl();
        case TokenKind::Enum: return parse_enum_decl(ml, loc);
        case TokenKind::Keep: return parse_keep_decl(ml, loc);
        case TokenKind::Operator: return parse_operator_overload(ml, loc);
        case TokenKind::Conversion: return parse_conversion_decl(ml, loc);
        case TokenKind::Extern: return parse_extern_decl(ml, loc);
        case TokenKind::Namespace: return parse_namespace_decl(ml, loc);
        case TokenKind::Init:
        case TokenKind::Drop:
        case TokenKind::Ref:
        case TokenKind::Deref:
            return parse_special_method_decl(ml, loc);
        default: break;
    }

    // 名字/类型开头：先看头部形态，再进入对应实现
    switch (scan_decl_head()) {
        case HeadShape::Func: return parse_func_decl(ml, loc);
        case HeadShape::Class: {
            Class* c = parse_class_decl(ml, loc);
            if (!c) fail_expected("'{' to open the class body");
            return c;
        }
        case HeadShape::Var: return parse_var_decl(ml, loc, true);
        default: return nullptr;
    }
}

// 跳过配平的 `-<...>`（含嵌套）。只读 token，不建节点。
void Parser::skip_generic_args() {
    if (curr().kind != TokenKind::GenericStart) return;
    int depth = 0;
    for (;;) {
        TokenKind k = curr().kind;
        if (k == TokenKind::GenericStart) ++depth;
        else if (k == TokenKind::Greater) --depth;
        else if (k == TokenKind::Eof) return;
        walk();
        if (depth <= 0) return;
    }
}

// 跳过一或多个 `#修饰`。返回是否至少跳过一个。
bool Parser::skip_modifier_chain() {
    bool any = false;
    while (curr().kind == TokenKind::Hash) {
        walk();
        if (curr().is_type()) walk();
        else if (curr().is_ident()) walk();
        else return any;
        if (curr().kind == TokenKind::GenericStart) skip_generic_args();
        any = true;
    }
    return any;
}

// 判定声明头部形态。只读 token，用于消解"类体 / 切片类型 / 函数 / 变量"的歧义。
// 关键字开头的声明已在 parse_decl 里定型，这里只处理名字与类型开头。
Parser::HeadShape Parser::scan_decl_head() {
    // 前置注入 `#a #b Name {`：注入链之后必须是类名，否则不是类声明
    if (curr().kind == TokenKind::Hash) {
        Probe probe(*this);
        if (!skip_modifier_chain()) return HeadShape::Unknown;
        return curr().is_ident() ? HeadShape::Class : HeadShape::Unknown;
    }
    if (curr().is_ident()) {
        Probe probe(*this);
        walk();
        if (curr().kind == TokenKind::GenericStart) skip_generic_args();
        // 类名之后允许修饰符与尾随注入（`Name #mod {`）
        while (curr().is_spec()) walk();
        if (curr().kind == TokenKind::Hash) skip_modifier_chain();
        if (curr().kind == TokenKind::ParenL) return HeadShape::Func;
        if (curr().kind == TokenKind::BraceL)
            return is_slice_brace() ? HeadShape::Var : HeadShape::Class;
        return HeadShape::Var;
    }
    // 类型开头只能是变量声明；缺名字的形态由变量分支报错。
    // `let` 也走变量分支，由它统一报“未实现”
    if (curr().is_type() || curr().kind == TokenKind::Dollar
        || curr().kind == TokenKind::Let)
        return HeadShape::Var;
    return HeadShape::Unknown;
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

Type* Parser::parse_modification() {
    if (curr().kind != TokenKind::Hash) return nullptr;
    Location loc = get_loc();
    walk();
    if (curr().is_type()) {
        PrimitiveTypeKind kind = token_to_primitive_type(curr().kind);
        walk();
        return cu_.add_node<Type>(loc, Type::Primitive{kind});
    }
    if (!curr().is_ident()) {
        add_error("expected identifier or base type after '#'");
        return nullptr;
    }
    std::string_view name = curr().get_val();
    walk();

    Type* mod = cu_.add_node<Type>(loc, Type::Identifier{name});
    // 泛型注入 `#foo-<int> bar`：实参表跟在修饰名之后
    if (curr().kind == TokenKind::GenericStart) {
        return parse_type_args(mod);
    }
    return mod;
}

// 函数的识别：名称后接泛型形参表（可选）与参数括号
Func* Parser::parse_func_decl(SpecList &ml, Location &loc) {
    if (!curr().is_ident()) return nullptr;
    std::string_view name = curr().get_val();
    walk();
    std::vector<TypeParam*> type_params = parse_type_params();
    if (curr().kind != TokenKind::ParenL) return nullptr;
    bool variadic = false;
    std::vector<VarList*> params = parse_value_params(&variadic);
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
    f->type_params = std::move(type_params);
    f->is_variadic = variadic;
    return f;
}

// 变量的识别：类型或推导符加标识符。
// 失败时游标停在出错处，不自行回退——回退由调用方按需负责。
VarList* Parser::parse_var_decl(SpecList &ml, Location &loc, bool expect_name) {
    bool is_let = curr().kind == TokenKind::Let;
    Type* type = nullptr;
    if (is_let) {
        add_error("'let' is not implemented yet");
        type = cu_.add_node<Type>(get_loc(), Type::Inferred{});
        advance();
    } else {
        type = parse_type();
        if (!type) return nullptr;
    }
    VarList* vl = parse_var_decl_body(ml, loc, type);
    // 形态已确定为变量声明时，失败点就是缺名字
    if (!vl && expect_name && !is_let)
        fail_expected("an identifier after the type \"" + describe_type(type) + "\"",
                      "a variable declaration");
    return vl;
}

// 类型已解析：读取变量名（可多个）、后缀维度与初值
VarList* Parser::parse_var_decl_body(SpecList &ml, Location &loc, Type* type) {
    std::vector<VarItem*> vars;
    for (;;) {
        if (curr().is_ident()) {
            std::string_view name = curr().get_val();
            Expr* init = nullptr;
            walk();
            // 后缀数组维度：T name[d1][d2] 等价于 T[d1][d2] name。
            // 与前缀形式混写（T[a] name[b]）没有唯一解释，直接报错
            Type* vtype = type;
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
                }
            } else if(type->is_inferred()) {
                add_error("can not infer type");
            }
            vars.push_back(cu_.add_node<VarItem>(get_loc(), name, init, vtype));
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

// typedef 名 = 类型：为类型取别名，右边就是一个类型，没有终止符
Decl* Parser::parse_typedef_decl() {
    Location loc = get_loc();
    advance();
    if (!curr().is_ident()) {
        add_error("expected identifier after 'typedef'");
        return nullptr;
    }
    std::string_view name = curr().get_val();
    advance();
    if (curr().kind != TokenKind::Assign) {
        add_error("expected '=' after typedef name");
        return nullptr;
    }
    advance();
    Type* target = parse_type();
    if (!target) {
        add_error("expected type after '=' in typedef");
        return nullptr;
    }
    return cu_.add_node<TypeDef>(loc, name, target);
}

// exprdef 名 = 表达式：为表达式取别名，右边就是一个表达式，没有终止符
Decl* Parser::parse_exprdef_decl() {
    Location loc = get_loc();
    advance();
    if (!curr().is_ident()) {
        add_error("expected identifier after 'exprdef'");
        return nullptr;
    }
    std::string_view name = curr().get_val();
    advance();
    if (curr().kind != TokenKind::Assign) {
        add_error("expected '=' after exprdef name");
        return nullptr;
    }
    advance();
    Expr* target = parse_expr();
    if (!target) {
        add_error("expected expression after '=' in exprdef");
        return nullptr;
    }
    return cu_.add_node<ExprDef>(loc, name, target);
}

// 当前 '{' 是否为切片类型的长度括号而非类体：只在 `T{n} 变量名` 中出现，
// 即 '}' 之后紧跟变量名，且该名字之后仍能续接变量声明；
// 否则它是新声明的开头，如空体类之后的函数声明
bool Parser::is_slice_brace() {
    // 与 parse_postfix_type 的判据一致：只有 {} 与 {长度} 才可能是切片
    if (peek().kind != TokenKind::BraceR && !peek().is_ident()
        && !peek().is_literal()) return false;
    Probe probe(*this);
    walk();
    if (curr().kind != TokenKind::BraceR && !parse_array_length()) return false;
    return curr().kind == TokenKind::BraceR && peek().is_ident()
        && (is_var_tail(peek(2)) || is_decl_start(peek(2)));
}

// 类声明：`['#' 修饰注入...] 名称 [泛型形参表] ['#' 修饰注入...] '{' 成员 '}'
// 调用前由 scan_decl_head 判定形态；头部再核对一次以免落到错误分支
Class* Parser::parse_class_decl(SpecList &ml, Location &loc) {
    std::vector<Type*> injs;
    while (curr().kind == TokenKind::Hash) {
        Type* mod = parse_modification();
        if (!mod) return nullptr;
        injs.push_back(mod);
    }
    if (!curr().is_ident()) {
        return nullptr;
    }
    std::string_view name = curr().get_val();
    walk();
    std::vector<TypeParam*> type_params = parse_type_params();
    SpecList nml = parse_specs();
    // 尾随注入：#mod 出现在类名之后、成员之前（如 `b #qux { }`）
    std::vector<Type*> trailing;
    while (curr().kind == TokenKind::Hash) {
        Type* mod = parse_modification();
        if (!mod) break;
        trailing.push_back(mod);
    }
    if (curr().kind != TokenKind::BraceL || is_slice_brace()) return nullptr;
    ml.merge(nml);
    ++class_depth_;
    std::vector<Decl*> members = parse_members();
    --class_depth_;
    Class* raw = cu_.add_node<Class>(
        loc, std::move(name), std::move(ml), std::move(members),
        std::move(injs), std::move(trailing)
    );
    raw->type_params = std::move(type_params);
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
    std::string prev;          // 上一个已提交的结构，用于"在某结构之后"的报错
    bool suppress = false;     // 刚报过具体错误，静默紧随的残留 token
    if (curr().kind == TokenKind::BraceL) {
        advance();
    }
    while (curr().kind != TokenKind::BraceR && !is_eof()) {
        size_t before = errors_.size();
        if (parse_modifier_directive(default_specs)) {
            prev = "a modifier directive";
            suppress = false;
            continue;
        }
        if (Decl* d = parse_decl()) {
            d->specs.merge(default_specs);
            members.push_back(d);
            prev = describe_decl(d);
            suppress = false;
        } else {
            if (errors_.size() > before) suppress = true;
            else if (!suppress) {
                fail_unexpected(prev);
                suppress = true;
            }
            advance();
        }
        if (is_eof()) break;   // 未闭合，交由循环后统一报错
    }
    if (curr().kind == TokenKind::BraceR) {
        advance(); // 消费 '}'
    } else {
        add_error("unterminated container, expected '}'");
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

// 库名最终以 "-l<name>" 交给链接器，故只放行常规库名字符：不含路径分隔符、
// 冒号、空格与前导 '-'，避免库名变成链接器参数之外的任意目标
static bool valid_extern_lib_name(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    char c0 = name.front();
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_'))
        return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '+' ||
                  c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// extern 里只有声明，没有定义体：描述定义的修饰（链接方式、存储、代码生成）
// 写在这里要么被静默忽略，要么与"外部符号"自相矛盾（static 尤其容易让人以为
// 拿到了内部链接的符号，实际仍然导出）。可见性与 const 是声明本身的属性，放行
static bool extern_def_spec(const SpecList& specs, std::string& out) {
    if (specs.has(SpecKind::Static)) { out = "static"; return true; }
    if (specs.has(SpecKind::Inline)) { out = "inline"; return true; }
    if (specs.has(SpecKind::Malloc)) { out = "malloc"; return true; }
    return false;
}

// extern 块内只允许 C 意义上的声明：函数声明与外部变量。其余成员要么是 clux
// 独有的构造（在外部符号空间里没有对应物），要么是容器（会让译者在发声明时
// 漏掉它，生成残缺甚至非法的 IR），一律报错。extern "LLVMIR" 另开一条路：
// 那里的一切实现都来自 keep`` 的 IR，类只是 clux 侧的类型名，不发射任何声明
static const char* extern_member_kind_error(DeclKind kind) {
    switch (kind) {
        case DeclKind::Class:
            return "'class' is not allowed inside an extern block: an external "
                   "type has no definition here, declare it in clux";
        case DeclKind::Enum:
            return "'enum' is not allowed inside an extern block: an external "
                   "enum has no definition here, declare it in clux";
        case DeclKind::Namespace:
            return "'namespace' is not allowed inside an extern block";
        case DeclKind::Extern:
            return "an extern block cannot be nested in another extern block";
        case DeclKind::Operator:
        case DeclKind::Conversion:
        case DeclKind::SpecialMethod:
            return "only function declarations are allowed inside an extern "
                   "block: an operator, conversion or special method is a "
                   "clux construct with no external counterpart";
        case DeclKind::Keep:
            return "'keep' is not allowed inside an extern block";
        case DeclKind::TypeDef:
        case DeclKind::ExprDef:
            return "an alias is not allowed inside an extern block";
        default: return nullptr;
    }
}

Extern* Parser::parse_extern_decl(SpecList &ml, Location &loc) {
    if (curr().kind != TokenKind::Extern) return nullptr;
    advance();

    if (curr().kind != TokenKind::StringLiteral) {
        add_error("expected library name string after 'extern'");
        return cu_.add_node<Extern>();
    }
    Location lib_loc = get_loc();
    std::string_view lib_name = curr().get_val();
    advance();

    if (class_depth_ > 0) {
        add_error_at(loc, "an extern block is not allowed inside a class: the "
                          "symbols it declares live in the external library's "
                          "namespace, not in the class");
    }
    std::string bad_spec;
    if (extern_def_spec(ml, bad_spec)) {
        add_error_at(loc, "'" + bad_spec + "' is not allowed on an extern "
                          "block: it declares external symbols, there is no "
                          "definition to attach the modifier to");
    }
    if (!valid_extern_lib_name(lib_name)) {
        add_error_at(lib_loc, "invalid library name '" + std::string(lib_name) +
                                  "': expected a name like m, z, stdc++, "
                                  "opengl32");
    }

    if (curr().kind != TokenKind::BraceL) {
        add_error("expected '{' after extern library name");
        return cu_.add_node<Extern>();
    }

    std::vector<Decl*> members = parse_members();
    // 块名即绑定目标：`c` / `C` 是 C 的语言标识，`LLVMIR` 的符号实现由 keep``
    // 提供，其余是待链接的库名。C 的调用约定推出来的约束只对前者生效
    ExternLang lang = extern_lang_of(lib_name);
    for (Decl* m : members) {
        if (!m) continue;
        // extern "LLVMIR" 里类只是类型名：布局与实现都在 keep`` 的 IR 中，
        // 译者为它不发任何东西，因此允许出现在块内。类体内只能有字段——
        // 方法体是 clux 代码，却不会被发射出去
        if (m->kind == DeclKind::Class && lang == ExternLang::LLVMIR) {
            for (Decl* cm : static_cast<Class*>(m)->members) {
                if (!cm || cm->kind == DeclKind::VarList) continue;
                add_error_at(cm->loc, "only field declarations are allowed in "
                                      "a class inside an extern \"LLVMIR\" "
                                      "block: its body is clux code, which is "
                                      "never emitted there");
            }
            continue;
        }
        if (const char* why = extern_member_kind_error(m->kind)) {
            add_error_at(m->loc, why);
            continue;
        }
        if (m->kind != DeclKind::Func && m->kind != DeclKind::VarList) continue;
        if (extern_def_spec(m->specs, bad_spec)) {
            add_error_at(m->loc, "'" + bad_spec + "' is not allowed on an "
                                 "extern declaration: it declares an external "
                                 "symbol, there is no definition to attach the "
                                 "modifier to");
        }
        if (m->kind != DeclKind::Func) continue;
        auto* f = static_cast<Func*>(m);
        f->extern_lang = lang;
        // extern 的符号名保持原样，泛型实例化出的多个实例会撞成同一个外部
        // 符号，且实例被当普通函数发射而不是声明
        if (!f->type_params.empty())
            add_error_at(m->loc, "a generic function is not allowed inside an "
                                 "extern block: every instance would claim "
                                 "the same external symbol");
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
        Location vloc = get_loc();
        std::string_view vname = curr().get_val();
        advance();
        Expr* init = nullptr;
        if (curr().kind == TokenKind::Assign) {
            advance();
            init = parse_expr();
            if (!init) add_error("expected expression after '='");
        }
        variants.push_back(cu_.add_node<EnumVariant>(vloc, vname, init));
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
    Location loc = get_loc();
    if (curr().kind == TokenKind::Semicolon) {
        // 空块
        advance();
        return cu_.add_node<Stmt>(loc, Stmt::Block{{}});
    }
    if (curr().kind == TokenKind::BraceL) return parse_block_stmt();

    // 关键字已经唯一确定形态
    switch (curr().kind) {
        case TokenKind::If:       return parse_if_stmt();
        case TokenKind::While:    return parse_while_stmt();
        case TokenKind::For:      return parse_for_stmt();
        case TokenKind::Return:   return parse_return_stmt();
        case TokenKind::Break:    return parse_break_stmt();
        case TokenKind::Continue: return parse_continue_stmt();
        case TokenKind::Label:    return parse_label_stmt();
        case TokenKind::Goto:     return parse_goto_stmt();
        case TokenKind::Asm:      return parse_asm_stmt();
        case TokenKind::Keep:     return parse_keep_stmt();
        case TokenKind::TypeDef:
        case TokenKind::ExprDef: {
            // 函数体内的 typedef / exprdef：作用域限于所在块，向下生效
            Decl* d = curr().kind == TokenKind::TypeDef
                ? parse_typedef_decl() : parse_exprdef_decl();
            return d ? cu_.add_node<Stmt>(loc, Stmt::Def{d}) : nullptr;
        }
        default: break;
    }

    // 修饰符与 let 只出现在声明里
    if (curr().kind == TokenKind::Let || curr().is_spec()) {
        SpecList ml = parse_specs();
        Location l = get_loc();
        VarList* vl = parse_var_decl(ml, l, true);
        if (!vl) return nullptr;
        Stmt* s = cu_.add_node<Stmt>(loc, Stmt::VarDecl{vl});
        consume_statement_end();
        return s;
    }

    // 头部判定：类型头后跟标识符是变量声明，否则是表达式语句。
    // 这是语句层唯一的歧义点，类型没有函数体，试探很浅。
    {
        Mark sp = point();
        Type* head = parse_type();
        if (head && curr().is_ident()) {
            SpecList ml;
            VarList* vl = parse_var_decl_body(ml, loc, head);
            if (!vl) {
                // 有标识符时必然产出变量；此处只作防御
                restore(sp);
                return nullptr;
            }
            Stmt* s = cu_.add_node<Stmt>(loc, Stmt::VarDecl{vl});
            consume_statement_end();
            return s;
        }
        restore(sp);
    }

    Expr* expr = parse_expr();
    if (!expr) {
        fail_expected("an expression");
        consume_statement_end();
        return nullptr;
    }
    Stmt* s = cu_.add_node<Stmt>(expr->loc, Stmt::ExprStmt{expr});
    consume_statement_end();
    return s;
}

Stmt* Parser::parse_block_stmt() {
    if (curr().kind != TokenKind::BraceL) return nullptr;
    Location loc = get_loc();
    advance();
    
    std::vector<Stmt*> stmts;
    
    while (!is_eof() && curr().kind != TokenKind::BraceR) {
        size_t before = errors_.size();
        Stmt* s = parse_stmt();
        if (s) {
            stmts.push_back(s);
        } else {
            // 已经给出更具体的语法错误时不再兜底；否则跳过错误 token
            if (errors_.size() == before)
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

Stmt* Parser::parse_label_stmt() {
    if (curr().kind != TokenKind::Label) return nullptr;
    Location loc = get_loc();
    advance();
    
    if (!curr().is_ident()) {
        add_error("expected label name after 'label'");
        return nullptr;
    }
    std::string_view label = curr().get_val();
    advance();
    return cu_.add_node<Stmt>(
        loc,
        Stmt::Label{label}
    );
}

Stmt* Parser::parse_goto_stmt() {
    if (curr().kind != TokenKind::Goto) return nullptr;
    Location loc = get_loc();
    advance();
    
    if (!curr().is_ident()) {
        add_error("expected label name after 'goto'");
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
        case TokenKind::Label:
        case TokenKind::Goto:
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
    
    // 处理值位置后缀链
    left = parse_postfix_expr(left);
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

    // 内置类型转换函数：typename(值)
    if (curr().is_type()) {
        Location c_loc = get_loc();
        Type* target = parse_type();
        if (curr().kind != TokenKind::ParenL) {
            // 类型在值位置不成表达式（Expr 不再承载类型节点），
            // 返回空指针，由调用方按缺表达式报错
            return nullptr;
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

    if (curr().kind == TokenKind::Dollar) {
        Location loc = get_loc();
        Type* ptrtype = parse_func_type();
        if (curr().kind != TokenKind::ParenL) {
            return nullptr;
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
        return cu_.add_node<Expr>(loc, Expr::Cast{ptrtype, operand});
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
Type* Parser::append_array_dim(const Location& loc, Type* base,
                               Expr* len, ArrayKind kind) {
    // 递归到最内层再插入：新维度永远是最内层。
    // int a[A][B][C] 的三次追加依次得到 A->int、A->B->int、A->B->C->int，
    // 即书写顺序从外到内，与 C 的定长数组声明一致
    if (base && base->is_array_type()) {
        auto* a = base->get_array_type();
        a->element = append_array_dim(loc, a->element, len, kind);
        return base;
    }
    return cu_.add_node<Type>(loc, Type::ArrayType{base, len, kind});
}

// 值位置的后缀链。类型位置的同形后缀见 parse_postfix_type，
// 两者的 [] 与 -*/-& 解释不同，改动须同步
Expr* Parser::parse_postfix_expr(Expr* left) {
    for (;;) {
        Location loc = get_loc();
        switch (curr().kind) {
            // 函数调用：foo(bar) / foo(bar)--{} / foo--{}
            case TokenKind::ParenL:
            case TokenKind::DoubleHyphen: {
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
            // 索引
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
                left = cu_.add_node<Expr>(
                    loc,
                    Expr::Index{left, index}
                );
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
                left = cu_.add_node<Expr>(
                    loc,
                    Expr::Member{left, member}
                );
                continue;
            }
            // 值位置的泛型应用 foo-<int>(1)：实参表与类型位置同形
            case TokenKind::GenericStart: {
                left = parse_expr_args(left);
                continue;
            }
            // -* 解引用，-& 取地址
            case TokenKind::Minus: {
                if (peek().kind == TokenKind::Mul || peek().kind == TokenKind::BitAnd) {
                    advance();
                    while(curr().kind == TokenKind::Mul || curr().kind == TokenKind::BitAnd) {
                        left = make_unary(curr(), left, true);
                        advance();
                    }
                    continue;
                }
                return left;
            }
            default:
                return left;
        }
    }
}

// 类型位置的后缀链。全程可逆：探测性 parse_var_decl 失败后 restore 必须仍然有效，
// 由 parse_type 的 trial 守卫保证（trial 期间 advance 退化为 walk）
Type* Parser::parse_postfix_type(Type* left) {
    for (;;) {
        Location loc = get_loc();
        switch (curr().kind) {
            // 函数调用不是类型后缀，交给值位置解析
            case TokenKind::ParenL:
            case TokenKind::DoubleHyphen:
                return left;
            // 数组类型: T[N] / T[]
            case TokenKind::BracketL: {
                advance();
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
                left = cu_.add_node<Type>(loc, Type::ArrayType{
                    left, length,
                    length ? ArrayKind::Fixed : ArrayKind::Infer});
                continue;
            }
            // 所有权切片类型: T{} 或 T{n}
            // 与函数体起始的 '{' 区分：只有形如 {} 或 {长度} 的才当切片类型
            case TokenKind::BraceL: {
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
                left = cu_.add_node<Type>(loc,
                    Type::ArrayType{left, length, ArrayKind::Slice});
                continue;
            }
            // 限定名: . ::
            case TokenKind::Dot:
            case TokenKind::ColonColon: {
                advance();
                if (!curr().is_ident()) {
                    add_error("expected member name after '.' or '::'");
                    return left;
                }
                std::string_view member = curr().get_val();
                advance();
                left = cu_.add_node<Type>(
                    loc,
                    Type::Member{left, member}
                );
                continue;
            }
            // 泛型实参 List-<int>：独立的 '-<' 符号
            case TokenKind::GenericStart: {
                left = parse_type_args(left);
                continue;
            }
            // -* 指针，-& 引用
            case TokenKind::Minus: {
                if (peek().kind == TokenKind::Mul || peek().kind == TokenKind::BitAnd) {
                    advance();
                    while(curr().kind == TokenKind::Mul || curr().kind == TokenKind::BitAnd) {
                        Location uloc = get_loc();
                        Type* node;
                        if (curr().kind == TokenKind::Mul) {
                            node = cu_.add_node<Type>(uloc, Type::Pointer{left});
                        } else {
                            node = cu_.add_node<Type>(uloc, Type::Reference{left});
                        }
                        walk();
                        left = node;
                    }
                    continue;
                }
                // `-[` 是前缀数组类型 T[a]b 的后缀续写
                if (peek().kind == TokenKind::BracketL) {
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

std::vector<Type*> Parser::parse_type_list_in_paren() {
    return parse_list<Type*>(
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

// 数组长度：表达式解析本身可逆（游标默认回退），无需额外守卫
Expr* Parser::parse_array_length() {
    return parse_expr();
}

Type* Parser::parse_type_atom() {
    Type* t = nullptr;
    if (curr().is_type()) {
        t = parse_primitive_type();
    } else if (curr().kind == TokenKind::Dollar) {
        t = parse_func_type();
    } else if (curr().is_ident()) {
        t = parse_named_type();
    }
    if (!t) return nullptr;
    return parse_postfix_type(t);
}

// 类型解析：游标默认可逆，探测失败后 restore 仍然有效
Type* Parser::parse_type() {
    return parse_type_atom();
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

    // 函数指针类型写成 $返回类型(参数类型表)，后缀直接接在返回类型之后
    Type* return_type = parse_type();
    if (!return_type) {
        add_error("expected return type after '$'");
        return nullptr;
    }

    std::vector<Type*> param_types = parse_type_list_in_paren();

    return cu_.add_node<Type>(loc, Type::FuncPtr{param_types, return_type});
}

Type* Parser::parse_named_type() {
    if (!curr().is_ident()) return nullptr;
    Location loc = get_loc();
    std::string_view name = curr().get_val();
    walk();
    return cu_.add_node<Type>(loc, Type::Identifier{name});
}

// 裂解余部重新派发：'>' 起始的操作符去掉首字符后只可能是这三种文本
static TokenKind operator_kind_of(std::string_view s) {
    if (s == ">")  return TokenKind::Greater;
    if (s == "=")  return TokenKind::Assign;
    if (s == ">=") return TokenKind::GreaterEqual;
    return TokenKind::Unknown;
}

// 泛型闭合符裂解：嵌套内以 '>' 起始的操作符（'>>'、'>='、'>>='）先出闭合符，
// 余部排回 pending_ 重新派发；嵌套外的 '>' 是比较运算，不动。
// 深度随 token 流前进，回退由 Mark 里记录的深度负责
Token Parser::generic_split(Token tok) {
    if (tok.kind == TokenKind::GenericStart) {
        ++generic_depth_;
        return tok;
    }
    if (generic_depth_ == 0 || !is_generic_close(tok.kind)) return tok;
    std::string_view rest = tok.val.substr(1);
    if (!rest.empty()) {
        Location rest_loc = tok.loc;
        rest_loc.col += 1;
        pending_.push_back(Token(operator_kind_of(rest), rest, rest_loc));
    }
    --generic_depth_;
    return Token(TokenKind::Greater, tok.val.substr(0, 1), tok.loc);
}

// 泛型形参表 `-<T, U>`：'-<' 已由调用方确认。全程可逆，供函数声明探测使用
std::vector<TypeParam*> Parser::parse_type_params() {
    return parse_list<TypeParam*>(
        TokenKind::GenericStart,
        TokenKind::Greater,
        TokenKind::Comma,
        [this]() -> TypeParam* {
            if (!curr().is_ident()) return nullptr;
            Location loc = get_loc();
            std::string_view name = curr().get_val();
            walk();
            TypeParam* tp = cu_.add_node<TypeParam>(loc, name);
            return tp;
        },
        "type parameter"
    ).value_or({});
}

// 泛型实参表 `-<T, U>`：类型位置与值位置语法同形，实参恒为类型
std::vector<Type*> Parser::parse_generic_args() {
    std::vector<Type*> args = parse_list<Type*>(
        TokenKind::GenericStart,
        TokenKind::Greater,
        TokenKind::Comma,
        [this] { return parse_type(); },
        "type argument"
    ).value_or({});
    // parse_list 正常结束时已消费 '>'；停在 EOF 说明未闭合，其内部已报错
    if (is_eof()) return args;
    return args;
}

Type* Parser::parse_type_args(Type* base) {
    std::vector<Type*> args = parse_generic_args();
    if (is_eof()) return base;
    return cu_.add_node<Type>(base->loc, Type::GenericType{base, std::move(args)});
}

Expr* Parser::parse_expr_args(Expr* base) {
    std::vector<Type*> args = parse_generic_args();
    if (is_eof()) return base;
    return cu_.add_node<Expr>(base->loc, Expr::GenericType{base, std::move(args)});
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
    use_define_ = false;
    if (curr().kind != TokenKind::RawCode) {
        add_error("expected code block");
        use_define_ = true;
        return "";
    }
    std::string_view code = curr().val;
    advance();
    use_define_ = true;
    return code;
}

} // namespace cx