#pragma once
#include<string>
#include<ostream>
#include<string_view>

namespace cx{

#define MODIFIER \
    X(Const, "const") \
    X(Static, "static") \
    X(Pub, "pub") \
    X(Priv, "priv") \
    X(Inline, "inline") \
    X(Own, "own") \
    X(Readonly, "readonly") \
    X(Malloc, "malloc") \
    X(Alloca, "alloca") \
    X(Over, "over")

#define TYPE \
    X(Int8, "int8") \
    X(Short, "short") \
    X(Int, "int") \
    X(Long, "long") \
    X(Int128, "int128") \
    X(UInt8, "uint8") \
    X(UShort, "ushort") \
    X(UInt, "uint") \
    X(ULong, "ulong") \
    X(UInt128, "uint128") \
    X(Float16, "float16") \
    X(Float, "float") \
    X(Double, "double") \
    X(Char, "char") \
    X(UChar, "uchar") \
    X(Bool, "bool") \
    X(Type, "type") \
    X(Void, "void") \
    X(Enum, "enum")

#define KEYWORD \
    X(Let, "let") \
    X(Namespace, "namespace") \
    X(Using, "using") \
    X(Import, "import") \
    X(No, "no") \
    X(If, "if") \
    X(Else, "else") \
    X(While, "while") \
    X(For, "for") \
    X(Return, "return") \
    X(Break, "break") \
    X(Continue, "continue") \
    X(Spawn, "spawn") \
    X(Mem, "mem") \
    X(Extern, "extern") \
    X(Asm, "asm") \
    X(Keep, "keep") \
    X(Flag, "flag") \
    X(ToFlag, "toflag") \
    X(Modf, "modf") \
    X(Operator, "operator") \
    X(Drop, "DROP") \
    X(Ref, "REF") \
    X(Deref, "DEREF") \
    X(Access, "ACCESS") \
    X(Alias, "alias") \
    X(Conversion, "conversion") \
    X(Init, "INIT") \
    MODIFIER \
    TYPE

#define OPERATOR3C \
    X(ShlAssign, "<<=") \
    X(ShrAssign, ">>=") \
    X(Ellipsis, "...")

#define OPERATOR2C \
    X(PlusAssign, "+=") \
    X(MinusAssign, "-=") \
    X(MulAssign, "*=") \
    X(DivAssign, "/=") \
    X(ModAssign, "%=") \
    X(AndAssign, "&=") \
    X(OrAssign, "|=") \
    X(XorAssign, "^=") \
    X(LessEqual, "<=") \
    X(GreaterEqual, ">=") \
    X(Equal, "==") \
    X(NotEqual, "!=") \
    X(And, "&&") \
    X(Or, "||") \
    X(Arrow, "->") \
    X(Shl, "<<") \
    X(Shr, ">>") \
    X(ColonColon, "::") \
    X(DoubleHyphen, "--") \

#define OPERATOR1C \
    X(ParenL, "(") \
    X(ParenR, ")") \
    X(BraceL, "{") \
    X(BraceR, "}") \
    X(BracketL, "[") \
    X(BracketR, "]") \
    X(Colon, ":") \
    X(Semicolon, ";") \
    X(Comma, ",") \
    X(Dot, ".") \
    X(Assign, "=") \
    X(Plus, "+") \
    X(Minus, "-") \
    X(Mul, "*") \
    X(Div, "/") \
    X(Mod, "%") \
    X(Less, "<") \
    X(Greater, ">") \
    X(Not, "!") \
    X(BitAnd, "&") \
    X(BitOr, "|") \
    X(BitXor, "^") \
    X(BitNot, "~") \
    X(Question, "?") \
    X(Dollar, "$") \
    X(At, "@") \
    X(Hash, "#") \
    X(Backslash, "\\") \
    X(BackQuote, "`")

#define OPERATOR \
    OPERATOR1C \
    OPERATOR2C \
    OPERATOR3C

#define LITERAL \
    X(Int8Literal, "int8_literal") \
    X(ShortLiteral, "short_literal") \
    X(IntLiteral, "int_literal") \
    X(LongLiteral, "long_literal") \
    X(UInt8Literal, "uint8_literal") \
    X(Int128Literal, "int128_literal") \
    X(UShortLiteral, "ushort_literal") \
    X(UIntLiteral, "uint_literal") \
    X(ULongLiteral, "ulong_literal") \
    X(UInt128Literal, "uint128_literal") \
    X(Float16Literal, "float16_literal") \
    X(FloatLiteral, "float_literal") \
    X(DoubleLiteral, "double_literal") \
    X(CharLiteral, "character_literal") \
    X(StringLiteral, "string_literal") \
    X(BoolLiteral, "bool_literal")

#define TOKEN_KINDS \
    X(Eof, "eof") \
    \
    KEYWORD \
    \
    X(Identifier, "identifier") \
    X(RawCode, "raw code") \
    \
    OPERATOR \
    \
    X(Unknown, "")\
    \
    LITERAL \
    \
    X(LineComment, "line comment") \
    X(BlockComment, "block comment")

struct Location {
    int lin;
    int col;
    std::string_view filename;
    Location():lin(-1),col(-1),filename(""){};
    Location(const int &line,const int &colomn,std::string_view file):
    lin(line),col(colomn),filename(file){}
};

enum class TokenKind {
    #define X(Kind,str) Kind,
    TOKEN_KINDS
    #undef X
};

inline std::string token_kind_to_string(TokenKind kind) {
    switch(kind) {
        #define X(kind, str) case TokenKind::kind: return str;
        TOKEN_KINDS
        #undef X
    }
    return "invalid";
}

struct Token {
    TokenKind kind;
    std::string_view val;
    Location loc;
    Token():kind(TokenKind::Unknown),val(""),loc() {}
    Token(TokenKind token_kind, std::string_view value, const Location &location):
        kind(token_kind),val(value),loc(location) {}
    Token(TokenKind token_kind, std::string_view value, int line, int column, std::string_view filename="<input>"):
        kind(token_kind),val(value),loc(line,column,filename) {}
    inline std::string_view get_val() const {
        return val;
    }
    inline std::string quote() const {
        return "\"" + std::string(val) + "\"";
    }
    inline std::string to_string()const {
        std::string name=token_kind_to_string(kind);
        return "{" + name + " " + quote()+", (" + std::to_string(loc.lin) + ", " + std::to_string(loc.col) + ")}";
    }

    friend std::ostream &operator<<(std::ostream &os,const Token &token) {
        os<<token.to_string();
        return os;
    }
    inline bool is_op() const {
        return kind > TokenKind::Identifier && kind < TokenKind::Unknown;
    }
    inline bool is_modf() const {
        return kind >= TokenKind::Const && kind <= TokenKind::Over;
    }
    inline bool is_type() const {
        return kind >= TokenKind::Int8 && kind <= TokenKind::Enum;
    }
    inline bool is_ident() const {
        return kind == TokenKind::Identifier;
    }
    inline bool is_word() const {
        return kind > TokenKind::Eof && kind <= TokenKind::Identifier;
    }
    inline bool is_eof() const {
        return kind == TokenKind::Eof;
    }
    inline bool is_literal() const { 
        return kind > TokenKind::Unknown && kind < TokenKind::BlockComment;
    }
};



}// namespace cx