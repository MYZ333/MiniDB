// A 负责：按 grammar.md 实现递归下降、优先级、多语句和 AST 构造。
#include "minisql/parser.hpp"

namespace minisql {

Result<std::vector<Statement>> parse(const TokenStream& /*tokens*/) {
    // TODO(A)：语法检查属于这里，表是否存在和表达式类型交给 B。
    return Diagnostic{DiagnosticStage::Syntax, ErrorCode::NotImplemented,
                      "Parser is not implemented yet", std::nullopt};
}

} // namespace minisql
