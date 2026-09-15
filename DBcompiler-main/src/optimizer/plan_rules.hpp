#pragma once

// B 内部计划规则：不暴露到公共 include，由 optimizePlan 按固定顺序组合。
#include "minisql/plan.hpp"

namespace minisql::optimizer_detail {

Result<PlanPtr> pushDownPredicates(const PlanPtr& plan);
Result<PlanPtr> eliminateEmptyInputs(const PlanPtr& plan);
Result<PlanPtr> chooseIndexScans(const PlanPtr& plan);
Result<PlanPtr> pruneColumns(const PlanPtr& plan);

} // namespace minisql::optimizer_detail
