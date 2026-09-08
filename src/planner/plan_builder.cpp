// B 负责：将已通过语义检查的结构转换为扫描、过滤、投影和修改算子。
#include "minisql/compiler.hpp"

namespace minisql {

Result<LogicalPlan> buildPlan(const BoundStatement& /*statement*/) {
    // TODO(B)：只消费绑定结果，不重新按字符串查列，不执行计划。
    return Diagnostic{DiagnosticStage::Plan, ErrorCode::NotImplemented,
                      "Plan builder is not implemented yet", std::nullopt};
}

} // namespace minisql
