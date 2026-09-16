// 优化流水线：表达式化简、谓词下推、空结果传播，再按依赖裁剪扫描列。
#include "minisql/optimizer.hpp"
#include "constant_fold.hpp"
#include "plan_rules.hpp"

#include <type_traits>
#include <utility>

namespace minisql {
namespace {
Diagnostic invalid(std::string message, SourceLocation span = std::nullopt) {
    return {DiagnosticStage::Plan, ErrorCode::InvalidPlan, std::move(message), span};
}

const ScalarValue* literalValue(const BoundExprPtr& expr) {
    if (const auto* literal = std::get_if<BoundLiteral>(&expr->node)) return &literal->value;
    return nullptr;
}

std::optional<bool> boolean(const BoundExprPtr& expr) {
    if (expr->type == DataType::Bool) {
        if (const auto* value = literalValue(expr)) {
            if (const auto* result = std::get_if<bool>(value)) return *result;
        }
    }
    return std::nullopt;
}

BoundExprPtr constant(const BoundExprPtr& original, ScalarValue value) {
    // 新常量表示整个旧表达式，保留其源码范围；被保留的危险运算仍保存原操作符位置。
    return std::make_shared<const BoundExpr>(BoundExpr{BoundLiteral{std::move(value)}, original->type, original->span});
}

Result<BoundExprPtr> optimizeExpr(const BoundExprPtr& expr, std::size_t depth = 0) {
    if (!expr) return invalid("required expression is missing");
    if (depth >= 256) return invalid("expression exceeds 256 levels", expr->span);
    return std::visit([&](const auto& node) -> Result<BoundExprPtr> {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, BoundColumnRef> || std::is_same_v<T, BoundLiteral> ||
                      std::is_same_v<T, BoundAggregate>) {
            // 聚合叶节点由 AggregatePlan 按分组求值，不能在普通常量折叠阶段展开。
            return expr;
        } else if constexpr (std::is_same_v<T, BoundUnary>) {
            auto result = optimizeExpr(node.operand, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&result)) return *error;
            auto child = std::get<BoundExprPtr>(std::move(result));
            if (const auto* value = literalValue(child)) {
                if (auto folded = optimizer_detail::foldUnary(node.op, *value)) return constant(expr, *folded);
            }
            if (child == node.operand) return expr;
            return std::make_shared<const BoundExpr>(BoundExpr{
                BoundUnary{node.op, child, node.operator_span}, expr->type, expr->span});
        } else if constexpr (std::is_same_v<T, BoundBinary>) {
            if (!node.left || !node.right) return invalid("binary expression child is missing", expr->span);
            auto left = optimizeExpr(node.left, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&left)) return *error;
            auto lhs = std::get<BoundExprPtr>(std::move(left));
            const auto lhs_bool = boolean(lhs);
            // 左侧决定短路时不需要访问右侧。保留运行时的从左向右求值约定。
            if ((node.op == BinaryOp::And && lhs_bool && !*lhs_bool) ||
                (node.op == BinaryOp::Or && lhs_bool && *lhs_bool)) {
                return constant(expr, ScalarValue{*lhs_bool});
            }
            auto right = optimizeExpr(node.right, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&right)) return *error;
            auto rhs = std::get<BoundExprPtr>(std::move(right));
            if (const auto* a = literalValue(lhs)) {
                if (const auto* b = literalValue(rhs)) {
                    if (auto folded = optimizer_detail::foldBinary(node.op, *a, *b)) return constant(expr, *folded);
                }
            }
            const auto rhs_bool = boolean(rhs);
            // TRUE AND x / FALSE OR x，以及 x AND TRUE / x OR FALSE 都保持 x 被求值。
            if ((node.op == BinaryOp::And && lhs_bool && *lhs_bool) ||
                (node.op == BinaryOp::Or && lhs_bool && !*lhs_bool)) return rhs;
            if ((node.op == BinaryOp::And && rhs_bool && *rhs_bool) ||
                (node.op == BinaryOp::Or && rhs_bool && !*rhs_bool)) return lhs;
            // 不将 x AND FALSE、x OR TRUE 直接折叠，因为 x 的除零/溢出不能被隐藏。
            if (lhs == node.left && rhs == node.right) return expr;
            return std::make_shared<const BoundExpr>(BoundExpr{
                BoundBinary{node.op, lhs, rhs, node.operator_span}, expr->type, expr->span});
        } else if constexpr (std::is_same_v<T, BoundCase>) {
            auto operand = node.operand;
            bool changed = false;
            if (operand) {
                auto optimized = optimizeExpr(operand, depth + 1);
                if (const auto* error = std::get_if<Diagnostic>(&optimized)) return *error;
                operand = std::get<BoundExprPtr>(std::move(optimized));
                changed = operand != node.operand;
            }
            auto branches = node.branches;
            for (auto& branch : branches) {
                auto condition = optimizeExpr(branch.condition, depth + 1);
                if (const auto* error = std::get_if<Diagnostic>(&condition)) return *error;
                auto result = optimizeExpr(branch.result, depth + 1);
                if (const auto* error = std::get_if<Diagnostic>(&result)) return *error;
                auto new_condition = std::get<BoundExprPtr>(std::move(condition));
                auto new_result = std::get<BoundExprPtr>(std::move(result));
                changed = changed || new_condition != branch.condition || new_result != branch.result;
                branch.condition = std::move(new_condition);
                branch.result = std::move(new_result);
            }
            auto else_result = node.else_result;
            if (else_result) {
                auto optimized = optimizeExpr(else_result, depth + 1);
                if (const auto* error = std::get_if<Diagnostic>(&optimized)) return *error;
                else_result = std::get<BoundExprPtr>(std::move(optimized));
                changed = changed || else_result != node.else_result;
            }
            if (!changed) return expr;
            return std::make_shared<const BoundExpr>(BoundExpr{
                BoundCase{operand, std::move(branches), else_result}, expr->type, expr->span});
        } else if constexpr (std::is_same_v<T, BoundInSubquery>) {
            auto value = optimizeExpr(node.value, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&value)) return *error;
            auto optimized = std::get<BoundExprPtr>(std::move(value));
            if (optimized == node.value) return expr;
            auto copy = node;
            copy.value = std::move(optimized);
            return std::make_shared<const BoundExpr>(BoundExpr{
                std::move(copy), expr->type, expr->span});
        } else if constexpr (std::is_same_v<T, BoundExistsSubquery> ||
                             std::is_same_v<T, BoundScalarSubquery>) {
            // 子计划由外围优化流水线独立处理；标量表达式层保留其绑定边界。
            return expr;
        } else {
            return invalid("unknown expression node", expr->span);
        }
    }, expr->node);
}

bool sameOutput(const PlanNode& left, const PlanNode& right) {
    if (left.carries_row_id != right.carries_row_id || left.output.size() != right.output.size()) return false;
    for (std::size_t i = 0; i < left.output.size(); ++i) {
        if (left.output[i].name != right.output[i].name || left.output[i].type != right.output[i].type) return false;
    }
    return true;
}

bool sameOutputTypes(const PlanNode& left, const PlanNode& right) {
    if (left.carries_row_id != right.carries_row_id ||
        left.output.size() != right.output.size()) return false;
    for (std::size_t i = 0; i < left.output.size(); ++i)
        if (left.output[i].type != right.output[i].type) return false;
    return true;
}

// JOIN 的记录布局是左列后接右列；优化只能改表达式，不能改变执行层依赖的槽位顺序。
bool isConcatenatedOutput(const PlanNode& join, const PlanNode& left, const PlanNode& right) {
    if (join.carries_row_id || left.carries_row_id || right.carries_row_id ||
        join.output.size() != left.output.size() + right.output.size()) return false;
    std::size_t output_index = 0;
    for (const auto* input : {&left, &right}) {
        for (const auto& column : input->output) {
            const auto& actual = join.output[output_index++];
            if (actual.name != column.name || actual.type != column.type) return false;
        }
    }
    return true;
}

template <typename T>
PlanPtr replace(const PlanPtr& original, T op) {
    return std::make_shared<const PlanNode>(PlanNode{std::move(op), original->output, original->carries_row_id});
}

Result<PlanPtr> optimizeNode(const PlanPtr& plan, std::size_t depth = 0) {
    if (!plan) return invalid("required plan node is missing");
    if (depth >= 256) return invalid("plan exceeds 256 levels");
    return std::visit([&](const auto& op) -> Result<PlanPtr> {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, CreateTablePlan> || std::is_same_v<T, CreateIndexPlan> ||
                      std::is_same_v<T, AlterTablePlan> ||
                      std::is_same_v<T, DropTablePlan> || std::is_same_v<T, DropIndexPlan> ||
                      std::is_same_v<T, InsertPlan> || std::is_same_v<T, SeqScanPlan> ||
                      std::is_same_v<T, IndexScanPlan> ||
                      std::is_same_v<T, EmptyResultPlan>) {
            return plan; // DDL、字面量 INSERT 和扫描没有可折叠的子表达式。
        } else if constexpr (std::is_same_v<T, ExplainPlan>) {
            if (plan->carries_row_id || plan->output.size() != 1 ||
                plan->output[0].name != "QUERY PLAN" ||
                plan->output[0].type != DataType::Varchar)
                return invalid("EXPLAIN output must be one VARCHAR column without RowId");
            if (op.input && std::holds_alternative<ExplainPlan>(op.input->node))
                return invalid("nested EXPLAIN is not supported");
            auto child = optimizeNode(op.input, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child)) return *error;
            auto input = std::get<PlanPtr>(std::move(child));
            if (input == op.input) return plan;
            return replace(plan, ExplainPlan{std::move(input), op.analyze});
        } else if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            auto left_result = optimizeNode(op.left, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&left_result)) return *error;
            auto right_result = optimizeNode(op.right, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&right_result)) return *error;
            if (!op.predicate || op.predicate->type != DataType::Bool)
                return invalid("NestedLoopJoin requires a BOOL predicate");
            auto expression = optimizeExpr(op.predicate);
            if (const auto* error = std::get_if<Diagnostic>(&expression)) return *error;
            auto left = std::get<PlanPtr>(std::move(left_result));
            auto right = std::get<PlanPtr>(std::move(right_result));
            auto predicate = std::get<BoundExprPtr>(std::move(expression));
            if (!isConcatenatedOutput(*plan, *left, *right))
                return invalid("NestedLoopJoin output must concatenate left and right metadata");
            if (left == op.left && right == op.right && predicate == op.predicate) return plan;
            return replace(plan, NestedLoopJoinPlan{left, right, predicate, op.type});
        } else if constexpr (std::is_same_v<T, SetOperationPlan>) {
            auto left_result = optimizeNode(op.left, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&left_result)) return *error;
            auto right_result = optimizeNode(op.right, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&right_result)) return *error;
            auto left = std::get<PlanPtr>(std::move(left_result));
            auto right = std::get<PlanPtr>(std::move(right_result));
            if (!sameOutputTypes(*left, *right) || !sameOutput(*plan, *left))
                return invalid("set operation inputs must have identical output metadata");
            if (left == op.left && right == op.right) return plan;
            return replace(plan, SetOperationPlan{left, right, op.op, op.all});
        } else if constexpr (std::is_same_v<T, DerivedTablePlan>) {
            auto child = optimizeNode(op.input, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child)) return *error;
            auto input = std::get<PlanPtr>(std::move(child));
            if (!op.table || input->output.size() != op.table->columns.size())
                return invalid("derived table output does not match its schema");
            if (input == op.input) return plan;
            return replace(plan, DerivedTablePlan{input, op.table, op.relation_id,
                                                  op.relation_name});
        } else {
            auto child = optimizeNode(op.input, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child)) return *error;
            auto input = std::get<PlanPtr>(std::move(child));
            if constexpr (std::is_same_v<T, FilterPlan>) {
                if (!op.predicate || op.predicate->type != DataType::Bool) return invalid("Filter requires a BOOL predicate");
                if (!sameOutput(*plan, *input)) return invalid("Filter output and RowId must match its input");
                auto expression = optimizeExpr(op.predicate);
                if (const auto* error = std::get_if<Diagnostic>(&expression)) return *error;
                auto predicate = std::get<BoundExprPtr>(std::move(expression));
                const auto value = boolean(predicate);
                if (value && *value) return input; // TRUE Filter 原样透传，直接用输入替代。
                // FALSE Filter 暂留给后续空结果规则；该规则还要判断输入能否安全跳过。
                if (input == op.input && predicate == op.predicate) return plan;
                return replace(plan, FilterPlan{predicate, input});
            } else if constexpr (std::is_same_v<T, UpdatePlan>) {
                if (!input->carries_row_id) return invalid("UPDATE input requires RowId");
                auto assignments = op.assignments;
                bool changed = input != op.input;
                for (auto& assignment : assignments) {
                    auto expression = optimizeExpr(assignment.value);
                    if (const auto* error = std::get_if<Diagnostic>(&expression)) return *error;
                    auto value = std::get<BoundExprPtr>(std::move(expression));
                    changed = changed || value != assignment.value;
                    assignment.value = std::move(value);
                }
                if (!changed) return plan;
                return replace(plan, UpdatePlan{op.table, std::move(assignments), input});
            } else if constexpr (std::is_same_v<T, DeletePlan>) {
                if (!input->carries_row_id) return invalid("DELETE input requires RowId");
                if (input == op.input) return plan;
                return replace(plan, DeletePlan{op.table, input});
            } else if constexpr (std::is_same_v<T, GroupByPlan>) {
                if (op.keys.empty() || plan->carries_row_id || input->carries_row_id ||
                    plan->output.size() != op.keys.size())
                    return invalid("GroupBy keys and output metadata are inconsistent");
                for (std::size_t i = 0; i < op.keys.size(); ++i) {
                    if (plan->output[i].type != op.keys[i].type)
                        return invalid("GroupBy output type does not match its key");
                }
                if (input == op.input) return plan;
                return replace(plan, GroupByPlan{op.keys, input});
            } else if constexpr (std::is_same_v<T, AggregatePlan>) {
                if (op.items.empty() || plan->carries_row_id || input->carries_row_id ||
                    plan->output.size() != op.items.size())
                    return invalid("Aggregate items and output metadata are inconsistent");
                auto items = op.items;
                auto order = op.order_by;
                auto having = op.having;
                bool changed = input != op.input;
                for (auto& item : items) {
                    if (auto* expression = std::get_if<BoundExprPtr>(&item.value)) {
                        auto optimized = optimizeExpr(*expression);
                        if (const auto* error = std::get_if<Diagnostic>(&optimized)) return *error;
                        auto value = std::get<BoundExprPtr>(std::move(optimized));
                        changed = changed || value != *expression;
                        *expression = std::move(value);
                    }
                }
                for (auto& item : order) {
                    if (auto* expression = std::get_if<BoundExprPtr>(&item.key)) {
                        auto optimized = optimizeExpr(*expression);
                        if (const auto* error = std::get_if<Diagnostic>(&optimized)) return *error;
                        auto value = std::get<BoundExprPtr>(std::move(optimized));
                        changed = changed || value != *expression;
                        *expression = std::move(value);
                    }
                }
                if (having) {
                    auto optimized = optimizeExpr(having);
                    if (const auto* error = std::get_if<Diagnostic>(&optimized)) return *error;
                    auto value = std::get<BoundExprPtr>(std::move(optimized));
                    changed = changed || value != having;
                    having = std::move(value);
                }
                if (!changed) return plan;
                return replace(plan, AggregatePlan{op.group_keys, std::move(items),
                    std::move(order), input, std::move(having), op.distinct, op.limit, op.offset});
            } else if constexpr (std::is_same_v<T, SortPlan>) {
                if ((op.items.empty() && op.expression_items.empty()) || !sameOutput(*plan, *input))
                    return invalid("Sort requires items and must preserve input metadata");
                auto expressions = op.expression_items;
                bool changed = input != op.input;
                for (auto& item : expressions) {
                    if (auto* expression = std::get_if<BoundExprPtr>(&item.key)) {
                        auto optimized = optimizeExpr(*expression);
                        if (const auto* error = std::get_if<Diagnostic>(&optimized)) return *error;
                        auto value = std::get<BoundExprPtr>(std::move(optimized));
                        changed = changed || value != *expression;
                        *expression = std::move(value);
                    }
                }
                if (!changed) return plan;
                return replace(plan, SortPlan{op.items, input, std::move(expressions)});
            } else {
                auto expressions = op.expressions;
                bool changed = input != op.input;
                for (auto& expression : expressions) {
                    auto optimized = optimizeExpr(expression);
                    if (const auto* error = std::get_if<Diagnostic>(&optimized)) return *error;
                    auto value = std::get<BoundExprPtr>(std::move(optimized));
                    changed = changed || value != expression;
                    expression = std::move(value);
                }
                if (!changed) return plan;
                return replace(plan, ProjectPlan{op.columns, input, std::move(expressions),
                                                 op.distinct, op.limit, op.offset});
            }
        }
    }, plan->node);
}
} // namespace

Result<LogicalPlan> optimizePlan(const LogicalPlan& plan) {
    auto simplified = optimizeNode(plan.root);
    if (const auto* error = std::get_if<Diagnostic>(&simplified)) return *error;

    // WHERE 合取项先靠近数据源，列裁剪才能同时看到投影、连接和下推条件的依赖。
    auto pushed = optimizer_detail::pushDownPredicates(
        std::get<PlanPtr>(std::move(simplified)));
    if (const auto* error = std::get_if<Diagnostic>(&pushed)) return *error;
    auto emptied = optimizer_detail::eliminateEmptyInputs(
        std::get<PlanPtr>(std::move(pushed)));
    if (const auto* error = std::get_if<Diagnostic>(&emptied)) return *error;
    auto indexed = optimizer_detail::chooseIndexScans(
        std::get<PlanPtr>(std::move(emptied)));
    if (const auto* error = std::get_if<Diagnostic>(&indexed)) return *error;
    auto pruned = optimizer_detail::pruneColumns(std::get<PlanPtr>(std::move(indexed)));
    if (const auto* error = std::get_if<Diagnostic>(&pruned)) return *error;
    return LogicalPlan{plan.catalog_version, std::get<PlanPtr>(std::move(pruned))};
}

} // namespace minisql
