#pragma once

// B 的公共入口。CREATE/DROP/INSERT/SELECT/UPDATE/DELETE 均生成真实绑定和计划。
#include "minisql/ast.hpp"
#include "minisql/plan.hpp"

namespace minisql {

// 不修改 AST 或 Catalog；首个语义错误通过 Result 返回。
Result<BoundStatement> analyze(const Statement& statement,
                               const CatalogSnapshot& catalog);

// 输入必须是通过语义分析的绑定结果，不再次执行名称解析；
// 附加检查指针、列身份和基本结构，失败返回 Plan / InvalidBoundStatement。
Result<LogicalPlan> buildPlan(const BoundStatement& statement);

} // namespace minisql
