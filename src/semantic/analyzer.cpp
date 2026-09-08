// B 负责：只读 Catalog 查询、名字绑定、类型检查和五类语句的语义限制。
#include "minisql/compiler.hpp"

namespace minisql {

Result<BoundStatement> analyze(const Statement& /*statement*/,
                               const CatalogSnapshot& /*catalog*/) {
    // TODO(B)：返回定型的 BoundStatement；不修改 AST、Catalog 或数据库记录。
    return Diagnostic{DiagnosticStage::Semantic, ErrorCode::NotImplemented,
                      "Semantic analyzer is not implemented yet", std::nullopt};
}

} // namespace minisql
