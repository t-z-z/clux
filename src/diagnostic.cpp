#include "../include/diagnostic.h"
#include <iostream>
#include <cstdio>

namespace vo{

// 当前用全局函数实现（后续替换为Consumer模式）
void emit_diagnostic(const Diagnostic& diag) {
    const char *level_str = "";
    const char *color = "";
    switch (diag.lvl) {
        case DiagLevel::Error:   level_str = "error"; color = "\033[31m"; break;
        case DiagLevel::Warning: level_str = "warning"; color = "\033[33m"; break;
        case DiagLevel::Note:    level_str = "note"; color = "\033[36m"; break;
        case DiagLevel::Help:    level_str = "help"; color = "\033[32m"; break;
    }
    
    // 基础输出：彩色 + 位置
    std::cout << color << "[" << level_str << "] " 
              << diag.loc.filename << ":" 
              << diag.loc.lin << ":" 
              << diag.loc.col 
              << "\033[0m: " << diag.msg << std::endl;
    
    // 如果有次级位置，简单打印（已预留）
    for (const auto& sec : diag.secondary_locations) {
        std::cout << "  --> " << sec.filename << ":" << sec.lin << std::endl;
    }
    
    // 如果有建议，打印（已预留）
    if (diag.suggestion) {
        std::cout << "  help: " << *diag.suggestion << std::endl;
    }
}

}// namespace vo