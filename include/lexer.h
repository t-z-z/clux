#pragma once
#include "token.h"
#include "diagnostic.h"
#include "string_pool.h"
#include <vector>
#include <string>
#include <string_view>

namespace vo {

class Lexer {
    public:
    // 统一字符串池由外部（编译驱动）持有，lexer 只引用它，
    // 这样转义后的字符串字面量与其他阶段共享同一份存储
    Lexer(const std::string &source, StringPool &pool, const std::string &filename="<input>"):
        source_(source), pool_(pool), filename_(filename){};

    Token next_token();
    std::string_view get_source_range(const size_t& start, const size_t& end) {
        return std::string_view(source_).substr(start, end - start);
    }

    inline bool has_error() const {return !errors_.empty();}
    const std::vector<Diagnostic> &errors() const {return errors_;}
    const std::string& get_filename() const {return filename_;}

    inline int get_line() const {return lin_;}
    inline int get_column() const {return col_;}
    inline size_t get_pos() const {return pos_;}
    inline Location get_location() const {return Location(lin_,col_,filename_);}
    
    std::string_view translate_escape_sequences(const std::string_view& input);
    inline bool is_eof(){
        return pos_>=source_.size();
    }
    // 将 token 值写入统一字符串池并返回稳定视图
    Token intern_token(TokenKind kind, std::string_view val, const Location &loc) {
        return Token(kind, pool_.insert(val), loc);
    }
    private:
    std::string source_;
    StringPool &pool_;
    std::string filename_;

    int lin_=1;
    int col_=1;
    size_t pos_=0;
    std::vector<Diagnostic> errors_;
    
    inline char curr_char() const {
        return source_[pos_];
    }

    inline char peek_char(int offset=1) const {
        if(pos_+offset<source_.size()) return source_[pos_+offset];
        return '\0';
    }

    inline void advance() {
        if (pos_<source_.size()) {
            if (source_[pos_]=='\n') ++lin_, col_=1;
            else ++col_;
            ++pos_;
        }
    }
    inline void advance(int step) {
        for (int i=0; i<step && pos_<source_.size();++i) advance();
    }
    inline bool check(const std::string &val){
        for(int i=0;i<val.size();++i) if(peek_char(i)!=val[i]) return false;
        return true;
    }
    void skip_space();
    void skip_comment();
    Token read_char();
    Token read_string();
    Token read_number();
    Token read_operator();
    Token read_identifier();
    inline void add_error(const std::string &msg)
        {errors_.push_back(Diagnostic(DiagLevel::Error,msg,get_location()));}
    inline void add_waring(const std::string &msg)
            {errors_.push_back(Diagnostic(DiagLevel::Warning,msg,get_location()));}
};


}//namespace vo
