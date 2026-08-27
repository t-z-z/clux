#include "../include/lexer.h"
#include "../include/token.h"
#include "../include/string_view_map.h"
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <stdexcept>
#include <cstdint>

namespace vo{

void Lexer::skip_space() {
    while (!is_eof()) {
        char ch = curr_char();
        if (isspace(ch)) {
            advance();
        } else if (ch == '/') {
            if (peek_char(1) == '/') {
                skip_comment();
            } else if (peek_char(1) == '*') {
                skip_comment();
            } else {
                break;
            }
        } else {
            break;
        }
    }
}

void Lexer::skip_comment() {
    if(curr_char()=='/' && peek_char()=='/') {
        advance(2);
        while(curr_char()!=EOF && curr_char()!='\n') {
            advance();
        }
    } else if(curr_char()=='/' && peek_char()=='*') {
        advance(2);
        while(curr_char()!=EOF) {
            if(curr_char()=='*' && peek_char()=='/') {
                advance(2);
                return;
            }
            advance();
        }
    } else if(curr_char()=='"' && peek_char()=='\"' && peek_char(2)=='\"') {
        advance(3);
        while(curr_char()!=EOF) {
            if(curr_char()=='"' && peek_char()=='\"' && peek_char(2)=='\"') {
                advance(3);
                return;
            }
            advance();
        }
    }
}
// 返回的token的val中不包含两个单引号
Token Lexer::read_char() {
    Location s_loc=get_location();
    advance();
    // 跳过第一个单引号
    size_t s_pos=pos_;
    std::string_view view(source_);
    size_t len=0;
    bool escape=false;
    //识别转义的单引号
    for(char c=curr_char(); c!='\''&&!escape; advance(),c=curr_char()){
        if(is_eof()){
            add_error("missing terminating \' character");
            break;
        }
        escape=(c=='\\');
        ++len;
    }
    std::string_view sub=view.substr(s_pos,len);
    std::string_view sv=translate_escape_sequences(sub);
    if(sv.size()>1) add_waring("multi-character char content");
    advance();
    //跳过末尾的单引号
    return Token(TokenKind::CharLiteral,sv,s_loc);
}

// 返回的token的val中不包含两个双引号
Token Lexer::read_string(){
    Location s_loc=get_location();
    //字符串的读取逻辑和字符基本相同
    advance();
    size_t s_pos=pos_;
    std::string_view view(source_);
    size_t len=0;
    bool escape=false;
    for(char c=curr_char(); c!='\"'||escape; advance(),c=curr_char()){
        if(is_eof()){
            add_error("missing terminating \" character");
            break;
        }
        escape=(c=='\\');
        ++len;
    }
    std::string_view sub=view.substr(s_pos,len);
    std::string_view sv=translate_escape_sequences(sub);
    advance();
    return Token(TokenKind::StringLiteral,sv,s_loc);
}

Token Lexer::read_number() {
    Location start_loc = get_location();
    size_t s_pos = pos_;
    bool is_float = false;
    int base = 10;

    // 检查进制前缀
    if (curr_char() == '0') {
        char next = peek_char();
        if (next == 'x' || next == 'X') {
            base = 16;
            advance(2);
            
            if (!isxdigit(curr_char())) {
                add_error("Expected hexadecimal digit after '0x'");
                return Token(TokenKind::Unknown, std::string_view(), start_loc);
            }
            while (isxdigit(curr_char())) {
                advance();
            }
        } else if (next == 'b' || next == 'B') {
            base = 2;
            advance(2);
            
            if (curr_char() != '0' && curr_char() != '1') {
                add_error("Expected binary digit after '0b'");
                return Token(TokenKind::Unknown, std::string_view(), start_loc);
            }
            while (curr_char() == '0' || curr_char() == '1') {
                advance();
            }
        } else if (next == 'o' || next == 'O') {
            base = 8;
            advance(2);
            
            if (curr_char() < '0' || curr_char() > '7') {
                add_error("Expected octal digit after '0o'");
                return Token(TokenKind::Unknown, std::string_view(), start_loc);
            }
            while (curr_char() >= '0' && curr_char() <= '7') {
                advance();
            }
        }
    }
    
    // 读取数字部分
    if (base == 10) {
        while (isdigit(curr_char())) {
            advance();
        }
    }
    
    // 检查是否有小数点
    if (curr_char() == '.' && peek_char(1) != '.') {
        is_float = true;
        advance();
        while (isdigit(curr_char())) {
            advance();
        }
    }
    
    // 检查指数部分 (e/E)
    if ((curr_char() == 'e' || curr_char() == 'E') && 
        (isdigit(peek_char()) || peek_char() == '+' || peek_char() == '-')) {
        is_float = true;
        advance();
        if (curr_char() == '+' || curr_char() == '-') {
            advance();
        }
        if (!isdigit(curr_char())) {
            add_error("Expected exponent digit after 'e'/'E'");
            return Token(TokenKind::Unknown, std::string_view(), start_loc);
        }
        while (isdigit(curr_char())) {
            advance();
        }
    }
    
    // 十六进制浮点数指数 (p/P)
    if (base == 16 && (curr_char() == 'p' || curr_char() == 'P') &&
        (isdigit(peek_char()) || peek_char() == '+' || peek_char() == '-')) {
        is_float = true;
        advance();
        if (curr_char() == '+' || curr_char() == '-') {
            advance();
        }
        if (!isdigit(curr_char())) {
            add_error("Expected exponent digit after 'p'/'P'");
            return Token(TokenKind::Unknown, std::string_view(), start_loc);
        }
        while (isdigit(curr_char())) {
            advance();
        }
    }
    
    // 解析后缀并确定 Token 类型
    TokenKind kind = TokenKind::Unknown;
    
    // 浮点后缀
    if (curr_char() == 'f' || curr_char() == 'F') {
        kind = TokenKind::FloatLiteral;
        advance();
        is_float = true;
    } else if (curr_char() == 'd' || curr_char() == 'D') {
        kind = TokenKind::DoubleLiteral;
        advance();
        is_float = true;
    } else if (curr_char() == 'h' || curr_char() == 'H') {
        kind = TokenKind::Float16Literal;
        advance();
        is_float = true;
    } else if (is_float) {
        kind = TokenKind::DoubleLiteral;
    } else {
        // 整数后缀解析（统一处理）
        int bit_width = 0;
        bool is_unsigned = false;
        bool is_signed = false;
        
        // 第一步：检测符号修饰符
        if (curr_char() == 'u' || curr_char() == 'U') {
            is_unsigned = true;
            advance();
        } else if (curr_char() == 'i' || curr_char() == 'I') {
            is_signed = true;
            advance();
        }
        
        // 第二步：检测位宽数字（i8, i16, u32, u64 等）
        if (isdigit(curr_char())) {
            std::string bits;
            while (isdigit(curr_char())) {
                bits += curr_char();
                advance();
            }
            bit_width = std::stoi(bits);
        } else {
            // 第三步：检测传统后缀（s, l, ll）
            if (curr_char() == 's' || curr_char() == 'S') {
                advance();
                bit_width = 16;  // short
            } else if (curr_char() == 'l' || curr_char() == 'L') {
                advance();
                // 检查 LL
                if (curr_char() == 'l' || curr_char() == 'L') {
                    advance();
                }
                bit_width = 64;  // long / long long
            }
        }
        
        // 第四步：确定 Token 类型
        if (bit_width == 0) {
            // 没有指定位宽，默认 int (32位)
            if (is_unsigned) {
                kind = TokenKind::UIntLiteral;
            } else {
                kind = TokenKind::IntLiteral;
            }
        } else {
            // 根据位宽和符号确定类型
            if (is_unsigned) {
                switch (bit_width) {
                    case 8:  kind = TokenKind::UInt8Literal; break;
                    case 16: kind = TokenKind::UShortLiteral; break;
                    case 32: kind = TokenKind::UIntLiteral; break;
                    case 64: kind = TokenKind::ULongLiteral; break;
                    case 128: kind = TokenKind::UInt128Literal; break;
                    default: 
                        add_error("Unsupported bit width: " + std::to_string(bit_width));
                        return Token(TokenKind::Unknown, std::string_view(), start_loc);
                }
            } else {
                switch (bit_width) {
                    case 8:  kind = TokenKind::Int8Literal; break;
                    case 16: kind = TokenKind::ShortLiteral; break;
                    case 32: kind = TokenKind::IntLiteral; break;
                    case 64: kind = TokenKind::LongLiteral; break;
                    case 128: kind = TokenKind::Int128Literal; break;
                    default:
                        add_error("Unsupported bit width: " + std::to_string(bit_width));
                        return Token(TokenKind::Unknown, std::string_view(), start_loc);
                }
            }
        }
        
        // 如果没有符号修饰，但有传统后缀
        if (!is_unsigned && !is_signed) {
            if (bit_width == 16) {
                kind = TokenKind::ShortLiteral;  // short
            } else if (bit_width == 64) {
                kind = TokenKind::LongLiteral;  // long, long long
            }
        }
    }
    
    std::string_view val = std::string_view(source_).substr(s_pos, pos_ - s_pos);
    return intern_token(kind, val, start_loc);
}

Token Lexer::read_operator() {
    Location start_loc = get_location();
    size_t s_pos = pos_;
    // 使用基于 string_view 的透明查找表（无需拷贝键）
    #define X(val,key) {key,TokenKind::val},
    static const svmap<TokenKind> map_3 = { OPERATOR3C };
    static const svmap<TokenKind> map_2 = { OPERATOR2C };
    static const svmap<TokenKind> map_1 = { OPERATOR1C };
    #undef X

    if(peek_char()!='\0'){
        std::string_view op2(source_.data() + s_pos, 2);
        if(peek_char(2)!='\0'){
            // 三字操作符
            std::string_view op3(source_.data() + s_pos, 3);
            svmap<TokenKind>::const_iterator it = map_3.find(op3); 
            if(it != map_3.end()){
                advance(3);
                return intern_token(it->second, op3, start_loc);
            }
        }
        // 二字操作符
        svmap<TokenKind>::const_iterator it = map_2.find(op2); 
        if(it != map_2.end()){
            advance(2);
            return intern_token(it->second, op2, start_loc);
        }
    }
    // 一字操作符
    std::string_view op1(source_.data() + s_pos, 1);
    svmap<TokenKind>::const_iterator it = map_1.find(op1); 
    if(it != map_1.end()){
        advance(1);
        return intern_token(it->second, op1, start_loc);
    }

    // 如果没有匹配到任何操作符，返回错误
    std::string msg = std::string("Unknown operator: '") + curr_char() + "'";
    add_error(msg);
    advance();
    return intern_token(TokenKind::Unknown, op1, start_loc);
}

Token Lexer::read_identifier() {
    Location start_loc = get_location();
    size_t s_pos = pos_;
    
    // 标识符可以以字母或下划线开头
    if (!isalpha(curr_char()) && curr_char() != '_') {
        add_error(std::string("Expected identifier, found '") + curr_char() + "'");
        std::string_view val(source_.data() + s_pos, 1);
        advance();
        return intern_token(TokenKind::Unknown, val, start_loc);
    }
    
    // 读取标识符：字母、数字、下划线
    while (isalnum(curr_char()) || curr_char() == '_') {
        advance();
    }
    
    std::string_view value = std::string_view(source_).substr(s_pos, pos_ - s_pos);
    
    // 使用 KEYWORD 宏构建透明查找表，统一检查关键字
    #define X(kind, str) {str, TokenKind::kind},
    static const svmap<TokenKind> keyword_map = { KEYWORD };
    #undef X
    auto kit = keyword_map.find(value);
    if (kit != keyword_map.end()) {
        return intern_token(kit->second, value, start_loc);
    }
    
    // 检查内置字面量
    if (value == "true" || value == "false") {
        return intern_token(TokenKind::BoolLiteral, value, start_loc);
    }
    
    // 如果不是关键字，就是普通标识符
    return intern_token(TokenKind::Identifier, value, start_loc);
}

Token Lexer::next_token() {
    skip_space();
    if (is_eof()) {
        return Token(TokenKind::Eof, "", get_location());
    }
    
    // 根据当前字符判断 token 类型
    char ch = curr_char();
    
    // 分支匹配：按优先级从高到低检查
    if (isdigit(ch)) {
        return read_number();          // 数字字面量
    } else if (isalpha(ch) || ch == '_') {
        return read_identifier();      // 标识符或关键字
    } else if (ch == '"') {
        return read_string();          // 字符串字面量
    } else if (ch == '\'') {
        return read_char();// 字符字面量
    } else {
        return read_operator();        // 操作符或分隔符
    }
}

//翻译支持：\n \r \t \\ \' \" \a \b \f \v \0 \ooo \xhh \uXXXX \UXXXXXXXX
std::string_view Lexer::translate_escape_sequences(const std::string_view& input) {
    std::string result;
    result.reserve(input.size());
    // 基本转义字符映射表
    static const std::unordered_map<char, char> basicEscapeMap = {
        {'n', '\n'}, {'r', '\r'}, {'t', '\t'}, {'\\', '\\'},
        {'\'', '\''}, {'\"', '\"'}, {'a', '\a'}, {'b', '\b'},
        {'f', '\f'}, {'v', '\v'}, {'0', '\0'}
    };
    for (size_t i = 0; i < input.length(); ++i) {
        char ch = input[i];
        if (ch=='\\' && i + 1 < input.length()) {
            char next = input[i + 1];
            // ===== 1. 处理基本转义字符 =====
            std::unordered_map<char, char>::const_iterator
                it = basicEscapeMap.find(next);
            if (it != basicEscapeMap.end()) {
                result.push_back(it->second);
                i++;
                continue;
            }
            // ===== 2. 处理八进制转义 \ooo =====
            if (next >= '0' && next <= '7') {
                int octalValue = 0;
                size_t j = i + 1;
                int count = 0;
                while (j < input.length() && count < 3 && 
                        input[j] >= '0' && input[j] <= '7') {
                    octalValue = octalValue * 8 + (input[j] - '0');
                    j++;
                    count++;
                }
                if (count > 0 && octalValue <= 255) {
                    result.push_back(static_cast<char>(octalValue));
                    i = j - 1;
                    continue;
                } else {
                    add_error("invalid octal escape sequence");
                }
            }
            // ===== 3. 处理十六进制转义 \xhh =====
            if (next=='x' && i + 2 < input.length()) {
                int hexValue = 0;
                size_t j = i + 2;
                int count = 0;
                while (j < input.length() && count < 2) {
                    char c = input[j];
                    if (c >= '0' && c <= '9') {
                        hexValue = hexValue * 16 + (c - '0');
                    } else if (c >= 'a' && c <= 'f') {
                        hexValue = hexValue * 16 + (c - 'a' + 10);
                    } else if (c >= 'A' && c <= 'F') {
                        hexValue = hexValue * 16 + (c - 'A' + 10);
                    } else {
                        break;
                    }
                    j++;
                    count++;
                }
                if (count > 0 && hexValue <= 255) {
                    result.push_back(static_cast<char>(hexValue));
                    i = j - 1;
                    continue;
                } else {
                    add_error("invalid hexadecimal escape sequence");
                }
            }
            // ===== 4. 处理 Unicode 转义 \uXXXX =====
            if (next=='u' && i + 5 < input.length()) {
                uint32_t unicodeValue = 0;
                bool valid = true;
                for (int k = 0; k < 4; ++k) {
                    char c = input[i + 2 + k];
                    if (c >= '0' && c <= '9') {
                        unicodeValue = unicodeValue * 16 + (c - '0');
                    } else if (c >= 'a' && c <= 'f') {
                        unicodeValue = unicodeValue * 16 + (c - 'a' + 10);
                    } else if (c >= 'A' && c <= 'F') {
                        unicodeValue = unicodeValue * 16 + (c - 'A' + 10);
                    } else {
                        valid = false;
                        break;
                    }
                }
                if (valid) {
                    // 检查是否为代理对（Surrogate Pair）
                    if (unicodeValue >= 0xD800 && unicodeValue <= 0xDFFF) {
                        add_error("invalid Unicode surrogate");
                    }
                    
                    // UTF-8 编码转换
                    if (unicodeValue <= 0x7F) {
                        result.push_back(static_cast<char>(unicodeValue));
                    } else if (unicodeValue <= 0x7FF) {
                        result.push_back(static_cast<char>(0xC0 | (unicodeValue >> 6)));
                        result.push_back(static_cast<char>(0x80 | (unicodeValue & 0x3F)));
                    } else if (unicodeValue <= 0xFFFF) {
                        result.push_back(static_cast<char>(0xE0 | (unicodeValue >> 12)));
                        result.push_back(static_cast<char>(0x80 | ((unicodeValue >> 6) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | (unicodeValue & 0x3F)));
                    }
                    i += 5;
                    continue;
                } else {
                    add_error("invalid Unicode escape sequence");
                }
            }
            // ===== 5. 处理扩展 Unicode 转义 \UXXXXXXXX =====
            if (next=='U' && i + 9 < input.length()) {
                uint32_t unicodeValue = 0;
                bool valid = true;
                for (int k = 0; k < 8; ++k) {
                    char c = input[i + 2 + k];
                    if (c >= '0' && c <= '9') {
                        unicodeValue = unicodeValue * 16 + (c - '0');
                    } else if (c >= 'a' && c <= 'f') {
                        unicodeValue = unicodeValue * 16 + (c - 'a' + 10);
                    } else if (c >= 'A' && c <= 'F') {
                        unicodeValue = unicodeValue * 16 + (c - 'A' + 10);
                    } else {
                        valid = false;
                        break;
                    }
                }
                if (valid && unicodeValue <= 0x10FFFF) {
                    // UTF-8 编码转换（支持最多4字节）
                    if (unicodeValue <= 0x7F) {
                        result.push_back(static_cast<char>(unicodeValue));
                    } else if (unicodeValue <= 0x7FF) {
                        result.push_back(static_cast<char>(0xC0 | (unicodeValue >> 6)));
                        result.push_back(static_cast<char>(0x80 | (unicodeValue & 0x3F)));
                    } else if (unicodeValue <= 0xFFFF) {
                        result.push_back(static_cast<char>(0xE0 | (unicodeValue >> 12)));
                        result.push_back(static_cast<char>(0x80 | ((unicodeValue >> 6) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | (unicodeValue & 0x3F)));
                    } else if (unicodeValue <= 0x10FFFF) {
                        result.push_back(static_cast<char>(0xF0 | (unicodeValue >> 18)));
                        result.push_back(static_cast<char>(0x80 | ((unicodeValue >> 12) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | ((unicodeValue >> 6) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | (unicodeValue & 0x3F)));
                    }
                    i += 9;
                    continue;
                } else {
                    add_error("invalid extended Unicode escape sequence");
                }
            }
            // ===== 6. 处理控制字符 \cX =====
            if (next=='c' && i + 2 < input.length()) {
                char control = input[i + 2];
                if (control >= 'A' && control <= 'Z') {
                    result.push_back(static_cast<char>(control - 'A' + 1));
                    i += 2;
                    continue;
                } else if (control >= 'a' && control <= 'z') {
                    result.push_back(static_cast<char>(control - 'a' + 1));
                    i += 2;
                    continue;
                } else {
                    add_error("invalid control character");
                }
            }
            // ===== 7. 处理八进制转义（带反斜杠和问号） =====
            if (next=='?') {
                result.push_back('?');
                i++;
                continue;
            }
            // ===== 8. 处理行继续符（反斜杠后跟换行） =====
            if (next=='\n') {
                // 忽略反斜杠和换行符（行继续）
                i++;
                continue;
            }
            // ===== 9. 未知转义序列 =====
            // 保留反斜杠和下一个字符
            result.push_back('\\');
            result.push_back(next);
            i++;
            continue;
        } else {
            result.push_back(ch);
        }
    }
    
    // 写入统一字符串池（替代原先 lexer 私有的 translated_strings_ 向量），
    // 返回的视图在整个编译期都有效
    return pool_.insert(result);
}

}//namespace vo
