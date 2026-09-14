// 谓词下推：将安全的 WHERE 合取项移到 JOIN 输入，减少连接前的行数。
#include "plan_rules.hpp"

#include <type_traits>
#include <utility>

namespace minisql::optimizer_detail {
namespace {

Diagnostic invalid(std::string message, SourceLocation span = std::nullopt) {
    return {DiagnosticStage::Plan, ErrorCode::InvalidPlan, std::move(message), span};
}

bool sameRef(const BoundColumnRef& left, const BoundColumnRef& right) {
    return left.table_id.value == right.table_id.value &&
           left.column_id.value == right.column_id.value &&
           left.relation_id == right.relation_id;
}

void addRef(std::vector<BoundColumnRef>& refs, const BoundColumnRef& ref) {
    for (const auto& present : refs) if (sameRef(present, ref)) return;
    refs.push_back(ref);
}

void collectRefs(const BoundExprPtr& expression, std::vector<BoundColumnRef>& refs,
                 std::size_t depth = 0) {
    if (!expression || depth >= 256) return;
    std::visit([&](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, BoundColumnRef>) addRef(refs, node);
        else if constexpr (std::is_same_v<T, BoundAggregate>) {
            if (node.argument) addRef(refs, *node.argument);
        } else if constexpr (std::is_same_v<T, BoundUnary>) {
            collectRefs(node.operand, refs, depth + 1);
        } else if constexpr (std::is_same_v<T, BoundBinary>) {
            collectRefs(node.left, refs, depth + 1);
            collectRefs(node.right, refs, depth + 1);
        }
    }, expression->node);
}

// 项目约定运行期除零/溢出错误可观测。含算术或取负的条件不移动，
// 避免空 JOIN 或短路求值场景中错误的可达性改变。
bool safeToMove(const BoundExprPtr& expression, std::size_t depth = 0) {
    if (!expression || depth >= 256) return false;
    return std::visit([&](const auto& node) -> bool {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, BoundColumnRef> ||
                      std::is_same_v<T, BoundLiteral>) return true;
        else if constexpr (std::is_same_v<T, BoundAggregate>) return false;
        else if constexpr (std::is_same_v<T, BoundUnary>) {
            return node.op != UnaryOp::Negate && safeToMove(node.operand, depth + 1);
        } else {
            const bool arithmetic = node.op == BinaryOp::Add || node.op == BinaryOp::Subtract ||
                node.op == BinaryOp::Multiply || node.op == BinaryOp::Divide;
            return !arithmetic && safeToMove(node.left, depth + 1) &&
                   safeToMove(node.right, depth + 1);
        }
    }, expression->node);
}

void flattenAnd(const BoundExprPtr& expression, std::vector<BoundExprPtr>& conjuncts) {
    if (const auto* binary = std::get_if<BoundBinary>(&expression->node);
        binary && binary->op == BinaryOp::And) {
        flattenAnd(binary->left, conjuncts);
        flattenAnd(binary->right, conjuncts);
    } else conjuncts.push_back(expression);
}

BoundExprPtr combineAnd(const std::vector<BoundExprPtr>& conjuncts,
                        const BoundExprPtr& original) {
    if (conjuncts.empty()) return nullptr;
    BoundExprPtr result = conjuncts.front();
    SourceLocation operator_span = original->span;
    if (const auto* binary = std::get_if<BoundBinary>(&original->node))
        operator_span = binary->operator_span;
    for (std::size_t i = 1; i < conjuncts.size(); ++i) {
        result = std::make_shared<const BoundExpr>(BoundExpr{
            BoundBinary{BinaryOp::And, result, conjuncts[i], operator_span},
            DataType::Bool, original->span});
    }
    return result;
}

bool relationOccurs(const PlanPtr& plan, const BoundColumnRef& ref,
                    std::size_t depth = 0) {
    if (!plan || depth >= 256) return false;
    return std::visit([&](const auto& op) -> bool {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, SeqScanPlan>) {
            return op.table && op.table->id.value == ref.table_id.value &&
                   op.relation_id == ref.relation_id;
        } else if constexpr (std::is_same_v<T, EmptyResultPlan>) {
            for (const auto& column : op.columns)
                if (sameRef(column, ref)) return true;
            return false;
        } else if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            return relationOccurs(op.left, ref, depth + 1) ||
                   relationOccurs(op.right, ref, depth + 1);
        } else if constexpr (std::is_same_v<T, FilterPlan> ||
                             std::is_same_v<T, GroupByPlan> ||
                             std::is_same_v<T, AggregatePlan> ||
                             std::is_same_v<T, SortPlan> ||
                             std::is_same_v<T, ProjectPlan> ||
                             std::is_same_v<T, UpdatePlan> ||
                             std::is_same_v<T, DeletePlan> ||
                             std::is_same_v<T, ExplainPlan>) {
            return relationOccurs(op.input, ref, depth + 1);
        } else return false;
    }, plan->node);
}

bool allIn(const std::vector<BoundColumnRef>& refs, const PlanPtr& plan) {
    if (refs.empty()) return false; // 常量谓词保留原位，避免改变空输入语义。
    for (const auto& ref : refs) if (!relationOccurs(plan, ref)) return false;
    return true;
}

bool mayPushLeft(JoinType type) {
    return type == JoinType::Inner || type == JoinType::Left;
}

bool mayPushRight(JoinType type) {
    return type == JoinType::Inner || type == JoinType::Right;
}

template <typename T>
PlanPtr replaceInput(const PlanPtr& original, T operation) {
    return std::make_shared<const PlanNode>(PlanNode{
        std::move(operation), original->output, original->carries_row_id});
}

PlanPtr filter(BoundExprPtr predicate, PlanPtr input) {
    return std::make_shared<const PlanNode>(PlanNode{
        FilterPlan{std::move(predicate), input}, input->output, input->carries_row_id});
}

Result<PlanPtr> pushNode(const PlanPtr& plan, std::size_t depth) {
    if (!plan) return invalid("predicate pushdown found a missing plan node");
    if (depth >= 256) return invalid("plan exceeds 256 levels during predicate pushdown");
    return std::visit([&](const auto& op) -> Result<PlanPtr> {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, CreateTablePlan> ||
                      std::is_same_v<T, DropTablePlan> ||
                      std::is_same_v<T, InsertPlan> ||
                      std::is_same_v<T, SeqScanPlan> ||
                      std::is_same_v<T, EmptyResultPlan>) return plan;
        else if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            auto left_result = pushNode(op.left, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&left_result)) return *error;
            auto right_result = pushNode(op.right, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&right_result)) return *error;
            auto left = std::get<PlanPtr>(std::move(left_result));
            auto right = std::get<PlanPtr>(std::move(right_result));
            if (left == op.left && right == op.right) return plan;
            return replaceInput(plan, NestedLoopJoinPlan{left, right, op.predicate, op.type});
        } else {
            auto child_result = pushNode(op.input, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child_result)) return *error;
            auto input = std::get<PlanPtr>(std::move(child_result));
            if constexpr (std::is_same_v<T, FilterPlan>) {
                PlanPtr current = input == op.input
                    ? plan : replaceInput(plan, FilterPlan{op.predicate, input});
                const auto* join = std::get_if<NestedLoopJoinPlan>(&input->node);
                // 下推会减少 ON 的求值次数；若 ON 自身可能报算术错误，则保持原顺序。
                if (!join || !safeToMove(join->predicate)) return current;

                std::vector<BoundExprPtr> conjuncts;
                flattenAnd(op.predicate, conjuncts);
                std::vector<BoundExprPtr> left_items, right_items, remaining;
                bool passed_unsafe_item = false;
                for (const auto& conjunct : conjuncts) {
                    // 危险项之后的条件不能提前，否则可能跳过原本先发生的运行期错误。
                    if (passed_unsafe_item || !safeToMove(conjunct)) {
                        passed_unsafe_item = true;
                        remaining.push_back(conjunct);
                        continue;
                    }
                    std::vector<BoundColumnRef> refs;
                    collectRefs(conjunct, refs);
                    const bool left_only = allIn(refs, join->left);
                    const bool right_only = allIn(refs, join->right);
                    if (left_only && !right_only && mayPushLeft(join->type))
                        left_items.push_back(conjunct);
                    else if (right_only && !left_only && mayPushRight(join->type))
                        right_items.push_back(conjunct);
                    else remaining.push_back(conjunct);
                }
                if (left_items.empty() && right_items.empty()) return current;

                PlanPtr left = join->left;
                if (!left_items.empty()) {
                    auto pushed = pushNode(filter(combineAnd(left_items, op.predicate), left),
                                           depth + 1);
                    if (const auto* error = std::get_if<Diagnostic>(&pushed)) return *error;
                    left = std::get<PlanPtr>(std::move(pushed));
                }
                PlanPtr right = join->right;
                if (!right_items.empty()) {
                    auto pushed = pushNode(filter(combineAnd(right_items, op.predicate), right),
                                           depth + 1);
                    if (const auto* error = std::get_if<Diagnostic>(&pushed)) return *error;
                    right = std::get<PlanPtr>(std::move(pushed));
                }
                auto rewritten_join = std::make_shared<const PlanNode>(PlanNode{
                    NestedLoopJoinPlan{left, right, join->predicate, join->type},
                    input->output, input->carries_row_id});
                auto predicate = combineAnd(remaining, op.predicate);
                return predicate ? filter(std::move(predicate), rewritten_join) : rewritten_join;
            } else if constexpr (std::is_same_v<T, GroupByPlan>) {
                return input == op.input ? plan :
                    replaceInput(plan, GroupByPlan{op.keys, input});
            } else if constexpr (std::is_same_v<T, AggregatePlan>) {
                return input == op.input ? plan : replaceInput(plan, AggregatePlan{
                    op.group_keys, op.items, op.order_by, input, op.having,
                    op.distinct, op.limit, op.offset});
            } else if constexpr (std::is_same_v<T, SortPlan>) {
                return input == op.input ? plan : replaceInput(plan,
                    SortPlan{op.items, input, op.expression_items});
            } else if constexpr (std::is_same_v<T, ProjectPlan>) {
                return input == op.input ? plan : replaceInput(plan, ProjectPlan{
                    op.columns, input, op.expressions, op.distinct, op.limit, op.offset});
            } else if constexpr (std::is_same_v<T, UpdatePlan>) {
                return input == op.input ? plan : replaceInput(plan,
                    UpdatePlan{op.table, op.assignments, input});
            } else if constexpr (std::is_same_v<T, DeletePlan>) {
                return input == op.input ? plan : replaceInput(plan, DeletePlan{op.table, input});
            } else {
                return input == op.input ? plan : replaceInput(plan,
                    ExplainPlan{input, op.analyze});
            }
        }
    }, plan->node);
}

} // namespace

Result<PlanPtr> pushDownPredicates(const PlanPtr& plan) {
    return pushNode(plan, 0);
}

} // namespace minisql::optimizer_detail
