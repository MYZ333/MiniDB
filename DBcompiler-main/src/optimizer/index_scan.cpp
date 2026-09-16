// 索引选择：只把单表 Filter->SeqScan 中可由现有单列 INT 索引缩小的数据源替换为 IndexScan。
#include "plan_rules.hpp"

#include <type_traits>
#include <utility>

namespace minisql::optimizer_detail {
namespace {

Diagnostic invalid(std::string message) {
    return {DiagnosticStage::Plan, ErrorCode::InvalidPlan, std::move(message), std::nullopt};
}

template <typename T>
PlanPtr rebuild(const PlanPtr& original, T op) {
    return std::make_shared<const PlanNode>(PlanNode{
        std::move(op), original->output, original->carries_row_id});
}

bool sameColumn(const BoundColumnRef& left, const BoundColumnRef& right) {
    return left.table_id.value == right.table_id.value &&
           left.column_id.value == right.column_id.value &&
           left.relation_id == right.relation_id &&
           left.ordinal == right.ordinal &&
           left.type == right.type;
}

std::optional<std::int64_t> intLiteral(const BoundExprPtr& expression) {
    if (!expression || expression->type != DataType::Int) return std::nullopt;
    const auto* literal = std::get_if<BoundLiteral>(&expression->node);
    if (!literal) return std::nullopt;
    if (const auto* value = std::get_if<std::int64_t>(&literal->value)) return *value;
    return std::nullopt;
}

const BoundColumnRef* columnRef(const BoundExprPtr& expression) {
    if (!expression || expression->type != DataType::Int) return nullptr;
    return std::get_if<BoundColumnRef>(&expression->node);
}

BinaryOp reverse(BinaryOp op) {
    switch (op) {
    case BinaryOp::Less: return BinaryOp::Greater;
    case BinaryOp::LessEqual: return BinaryOp::GreaterEqual;
    case BinaryOp::Greater: return BinaryOp::Less;
    case BinaryOp::GreaterEqual: return BinaryOp::LessEqual;
    default: return op;
    }
}

struct Candidate {
    BoundColumnRef column;
    std::shared_ptr<const IndexSchema> index;
    BinaryOp op = BinaryOp::Equal;
    std::int64_t value = 0;
};

std::shared_ptr<const IndexSchema> indexFor(const SeqScanPlan& scan,
                                            const BoundColumnRef& column) {
    if (!scan.table || column.table_id.value != scan.table->id.value ||
        column.relation_id != scan.relation_id || column.ordinal >= scan.table->columns.size())
        return nullptr;
    for (const auto& index : scan.table->indexes) {
        if (index.column_id.value != column.column_id.value ||
            index.key_type != DataType::Int || !index.unique)
            continue;
        return std::make_shared<const IndexSchema>(index);
    }
    return nullptr;
}

std::optional<Candidate> comparisonCandidate(const BoundExprPtr& expression,
                                             const SeqScanPlan& scan) {
    if (!expression) return std::nullopt;
    const auto* binary = std::get_if<BoundBinary>(&expression->node);
    if (!binary) return std::nullopt;
    if (binary->op != BinaryOp::Equal && binary->op != BinaryOp::Less &&
        binary->op != BinaryOp::LessEqual && binary->op != BinaryOp::Greater &&
        binary->op != BinaryOp::GreaterEqual)
        return std::nullopt;

    if (const auto* column = columnRef(binary->left)) {
        if (auto value = intLiteral(binary->right)) {
            auto index = indexFor(scan, *column);
            if (index) return Candidate{*column, std::move(index), binary->op, *value};
        }
    }
    if (const auto* column = columnRef(binary->right)) {
        if (auto value = intLiteral(binary->left)) {
            auto index = indexFor(scan, *column);
            if (index) return Candidate{*column, std::move(index), reverse(binary->op), *value};
        }
    }
    return std::nullopt;
}

void flattenAnd(const BoundExprPtr& expression, std::vector<BoundExprPtr>& out,
                std::size_t depth = 0) {
    if (!expression || depth >= 256) return;
    const auto* binary = std::get_if<BoundBinary>(&expression->node);
    if (binary && binary->op == BinaryOp::And) {
        flattenAnd(binary->left, out, depth + 1);
        flattenAnd(binary->right, out, depth + 1);
        return;
    }
    out.push_back(expression);
}

void tightenLower(std::optional<IndexRangeBound>& lower, IndexRangeBound candidate) {
    if (!lower || candidate.value > lower->value ||
        (candidate.value == lower->value && !candidate.inclusive && lower->inclusive))
        lower = candidate;
}

void tightenUpper(std::optional<IndexRangeBound>& upper, IndexRangeBound candidate) {
    if (!upper || candidate.value < upper->value ||
        (candidate.value == upper->value && !candidate.inclusive && upper->inclusive))
        upper = candidate;
}

std::optional<IndexScanPlan> indexScanFor(const FilterPlan& filter,
                                          const SeqScanPlan& scan) {
    std::vector<BoundExprPtr> predicates;
    flattenAnd(filter.predicate, predicates);
    std::optional<Candidate> chosen;
    for (const auto& predicate : predicates) {
        auto candidate = comparisonCandidate(predicate, scan);
        if (candidate) {
            chosen = std::move(candidate);
            break;
        }
    }
    if (!chosen) return std::nullopt;

    std::optional<IndexRangeBound> lower;
    std::optional<IndexRangeBound> upper;
    for (const auto& predicate : predicates) {
        auto candidate = comparisonCandidate(predicate, scan);
        if (!candidate || !sameColumn(candidate->column, chosen->column)) continue;
        switch (candidate->op) {
        case BinaryOp::Equal:
            lower = IndexRangeBound{candidate->value, true};
            upper = IndexRangeBound{candidate->value, true};
            break;
        case BinaryOp::Greater:
            tightenLower(lower, IndexRangeBound{candidate->value, false});
            break;
        case BinaryOp::GreaterEqual:
            tightenLower(lower, IndexRangeBound{candidate->value, true});
            break;
        case BinaryOp::Less:
            tightenUpper(upper, IndexRangeBound{candidate->value, false});
            break;
        case BinaryOp::LessEqual:
            tightenUpper(upper, IndexRangeBound{candidate->value, true});
            break;
        default:
            break;
        }
    }
    return IndexScanPlan{scan.table, chosen->index, scan.relation_id, scan.relation_name,
                         lower, upper, scan.columns};
}

Result<PlanPtr> chooseNode(const PlanPtr& plan, std::size_t depth) {
    if (!plan) return invalid("index scan rule found a missing plan node");
    if (depth >= 256) return invalid("plan exceeds 256 levels during index scan rule");
    return std::visit([&](const auto& op) -> Result<PlanPtr> {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, CreateTablePlan> ||
                      std::is_same_v<T, CreateIndexPlan> ||
                      std::is_same_v<T, AlterTablePlan> ||
                      std::is_same_v<T, DropTablePlan> ||
                      std::is_same_v<T, DropIndexPlan> ||
                      std::is_same_v<T, InsertPlan> ||
                      std::is_same_v<T, SeqScanPlan> ||
                      std::is_same_v<T, IndexScanPlan> ||
                      std::is_same_v<T, EmptyResultPlan>) {
            return plan;
        } else if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            auto left_result = chooseNode(op.left, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&left_result)) return *error;
            auto right_result = chooseNode(op.right, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&right_result)) return *error;
            auto left = std::get<PlanPtr>(std::move(left_result));
            auto right = std::get<PlanPtr>(std::move(right_result));
            if (left == op.left && right == op.right) return plan;
            return rebuild(plan, NestedLoopJoinPlan{left, right, op.predicate, op.type});
        } else if constexpr (std::is_same_v<T, SetOperationPlan>) {
            auto left_result = chooseNode(op.left, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&left_result)) return *error;
            auto right_result = chooseNode(op.right, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&right_result)) return *error;
            auto left = std::get<PlanPtr>(std::move(left_result));
            auto right = std::get<PlanPtr>(std::move(right_result));
            if (left == op.left && right == op.right) return plan;
            return rebuild(plan, SetOperationPlan{left, right, op.op, op.all});
        } else {
            auto child_result = chooseNode(op.input, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child_result)) return *error;
            auto input = std::get<PlanPtr>(std::move(child_result));
            if constexpr (std::is_same_v<T, FilterPlan>) {
                if (const auto* scan = std::get_if<SeqScanPlan>(&input->node)) {
                    auto index_scan = indexScanFor(op, *scan);
                    if (index_scan) {
                        auto scan_node = std::make_shared<const PlanNode>(PlanNode{
                            std::move(*index_scan), input->output, true});
                        return rebuild(plan, FilterPlan{op.predicate, scan_node});
                    }
                }
                if (input == op.input) return plan;
                return rebuild(plan, FilterPlan{op.predicate, input});
            } else if constexpr (std::is_same_v<T, DerivedTablePlan>) {
                if (input == op.input) return plan;
                return rebuild(plan, DerivedTablePlan{input, op.table, op.relation_id,
                                                      op.relation_name});
            } else if constexpr (std::is_same_v<T, GroupByPlan>) {
                if (input == op.input) return plan;
                return rebuild(plan, GroupByPlan{op.keys, input});
            } else if constexpr (std::is_same_v<T, AggregatePlan>) {
                if (input == op.input) return plan;
                return rebuild(plan, AggregatePlan{op.group_keys, op.items, op.order_by, input,
                    op.having, op.distinct, op.limit, op.offset});
            } else if constexpr (std::is_same_v<T, SortPlan>) {
                if (input == op.input) return plan;
                return rebuild(plan, SortPlan{op.items, input, op.expression_items});
            } else if constexpr (std::is_same_v<T, ProjectPlan>) {
                if (input == op.input) return plan;
                return rebuild(plan, ProjectPlan{op.columns, input, op.expressions,
                                                 op.distinct, op.limit, op.offset});
            } else if constexpr (std::is_same_v<T, UpdatePlan>) {
                if (input == op.input) return plan;
                return rebuild(plan, UpdatePlan{op.table, op.assignments, input});
            } else if constexpr (std::is_same_v<T, DeletePlan>) {
                if (input == op.input) return plan;
                return rebuild(plan, DeletePlan{op.table, input});
            } else {
                if (input == op.input) return plan;
                return rebuild(plan, ExplainPlan{input, op.analyze});
            }
        }
    }, plan->node);
}

} // namespace

Result<PlanPtr> chooseIndexScans(const PlanPtr& plan) {
    return chooseNode(plan, 0);
}

} // namespace minisql::optimizer_detail
