// A 负责：关键字、常量、注释、转义、多字符操作符以及源码位置追踪。
#include "minisql/lexer.hpp"

namespace minisql {

Result<TokenStream> lex(std::string_view /*sql*/) {
    // TODO(A)：实现后用 TokenStream 或真实词法诊断替换此占位结果。
    return Diagnostic{DiagnosticStage::Lexical, ErrorCode::NotImplemented,
                      "Lexer is not implemented yet", std::nullopt};
}

} // namespace minisql
