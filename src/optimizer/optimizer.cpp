// 自底向上优化：先化简表达式，再改写算子。未改变的只读节点直接共享。
#include "minisql/optimizer.hpp"
#include "constant_fold.hpp"

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
        if constexpr (std::is_same_v<T, BoundColumnRef> || std::is_same_v<T, BoundLiteral>) {
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
        } else {
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

template <typename T>
PlanPtr replace(const PlanPtr& original, T op) {
    return std::make_shared<const PlanNode>(PlanNode{std::move(op), original->output, original->carries_row_id});
}

Result<PlanPtr> optimizeNode(const PlanPtr& plan, std::size_t depth = 0) {
    if (!plan) return invalid("required plan node is missing");
    if (depth >= 256) return invalid("plan exceeds 256 levels");
    return std::visit([&](const auto& op) -> Result<PlanPtr> {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, CreateTablePlan> || std::is_same_v<T, InsertPlan> || std::is_same_v<T, SeqScanPlan>) {
            return plan; // 第一阶段 INSERT 已是单行字面量，无需继续折叠。
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
                // FALSE Filter 仍保留；不新增空结果算子，不移除修改语句的根。
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
            } else {
                if (input == op.input) return plan;
                return replace(plan, ProjectPlan{op.columns, input});
            }
        }
    }, plan->node);
}
} // namespace

Result<LogicalPlan> optimizePlan(const LogicalPlan& plan) {
    auto result = optimizeNode(plan.root);
    if (const auto* error = std::get_if<Diagnostic>(&result)) return *error;
    return LogicalPlan{plan.catalog_version, std::get<PlanPtr>(std::move(result))};
}

} // namespace minisql
