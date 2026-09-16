#pragma once

// A 的 AST 级展示优化：在语义分析前改写纯语法树，供 CLI 对比和模块测试。
// B 的 optimizer.hpp 则处理绑定后的 LogicalPlan，两者处在不同编译阶段。
#include "minisql/ast.hpp"

namespace minisql {

ExprPtr optimizeAstExpression(const ExprPtr& expression);
Statement optimizeAstStatement(const Statement& statement);
std::vector<Statement> optimizeAstStatements(const std::vector<Statement>& statements);

} // namespace minisql
