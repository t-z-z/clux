#include "compiler.h"

#include "string_pool.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"

namespace vo {

bool compile(const std::string& source, const std::string& filename) {
    // 统一字符串池：生命周期覆盖整个编译过程，lexer 仅引用它
    StringPool pool;
    Lexer lexer(source, pool, filename);
    CompUnit cu;
    Parser parser(lexer, cu, pool);
    CompUnit* pcu = parser.parse();
    if (!pcu) return false;
    if (parser.has_error()) {
        for (const Diagnostic& d : parser.errors()) {
            emit_diagnostic(d);
        }
        return false;
    }

    SemanticAnalyzer sem(*pcu);
    bool ok = sem.analyze();
    if (sem.has_error()) {
        for (const Diagnostic& d : sem.diagnostics()) {
            emit_diagnostic(d);
        }
        return false;
    }
    return ok;
}

} // namespace vo
