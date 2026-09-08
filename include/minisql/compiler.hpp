#pragma once

// B 的公共入口。当前 src/semantic 和 src/planner 提供可链接的占位定义，
// 返回 NotImplemented；后续保持签名不变，逐步替换为真正的算法。
#include "minisql/ast.hpp"
#include "minisql/plan.hpp"

namespace minisql {

// 不修改 AST 或 Catalog；首个语义错误通过 Result 返回。
Result<BoundStatement> analyze(const Statement& statement,
                               const CatalogSnapshot& catalog);

// 输入必须是通过语义分析的绑定结果，不再次执行名称解析。
Result<LogicalPlan> buildPlan(const BoundStatement& statement);

} // namespace minisql
