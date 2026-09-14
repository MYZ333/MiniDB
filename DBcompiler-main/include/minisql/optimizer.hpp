#pragma once

// B 的规则式优化入口：buildPlan 后显式调用，保留原计划供比较和执行等价性验证。
#include "minisql/plan.hpp"

namespace minisql {

// 前置条件：输入来自成功的语义分析和 buildPlan（或本函数上一次成功输出）。
// 常量折叠、保守布尔化简、谓词下推和列裁剪；保持模式版本、根输出和行标识。
// 不执行 SQL、不访问 Catalog、不改变原树；基本结构错误返回 Plan / InvalidPlan。
Result<LogicalPlan> optimizePlan(const LogicalPlan& plan);

} // namespace minisql
