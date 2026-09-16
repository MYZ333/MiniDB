// 空结果传播：用显式叶节点替代可安全跳过的恒假流水线，并保留列和关系布局。
#include "plan_rules.hpp"

#include <type_traits>
#include <utility>

namespace minisql::optimizer_detail {
namespace {

Diagnostic invalid(std::string message) {
    return {DiagnosticStage::Plan, ErrorCode::InvalidPlan, std::move(message), std::nullopt};
}

bool isEmpty(const PlanPtr& plan) {
    return plan && std::holds_alternative<EmptyResultPlan>(plan->node);
}

std::optional<bool> booleanLiteral(const BoundExprPtr& expression) {
    if (!expression || expression->type != DataType::Bool) return std::nullopt;
    const auto* literal = std::get_if<BoundLiteral>(&expression->node);
    if (!literal) return std::nullopt;
    if (const auto* value = std::get_if<bool>(&literal->value)) return *value;
    return std::nullopt;
}

// 返回关系算子的列身份顺序；EmptyResult 用它支持外连接生成 NULL 行。
Result<std::vector<BoundColumnRef>> outputRefs(const PlanPtr& plan,
                                               std::size_t depth = 0) {
    if (!plan) return invalid("empty-result analysis found a missing plan node");
    if (depth >= 256) return invalid("plan exceeds 256 levels during empty-result analysis");
    return std::visit([&](const auto& op) -> Result<std::vector<BoundColumnRef>> {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, EmptyResultPlan>) return op.columns;
        else if constexpr (std::is_same_v<T, SeqScanPlan>) {
            if (op.columns) return *op.columns;
            if (!op.table) return invalid("SeqScan table is missing during empty-result analysis");
            std::vector<BoundColumnRef> columns;
            for (std::size_t ordinal = 0; ordinal < op.table->columns.size(); ++ordinal) {
                const auto& column = op.table->columns[ordinal];
                columns.push_back({op.table->id, column.id, ordinal, column.type,
                                   op.relation_id});
            }
            return columns;
        } else if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            auto left_result = outputRefs(op.left, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&left_result)) return *error;
            auto right_result = outputRefs(op.right, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&right_result)) return *error;
            auto columns = std::get<std::vector<BoundColumnRef>>(std::move(left_result));
            auto right = std::get<std::vector<BoundColumnRef>>(std::move(right_result));
            columns.insert(columns.end(), right.begin(), right.end());
            return columns;
        } else if constexpr (std::is_same_v<T, DerivedTablePlan>) {
            if (!op.table) return invalid("DerivedTable schema is missing");
            std::vector<BoundColumnRef> columns;
            for (std::size_t ordinal = 0; ordinal < op.table->columns.size(); ++ordinal) {
                const auto& column = op.table->columns[ordinal];
                columns.push_back({op.table->id, column.id, ordinal, column.type,
                                   op.relation_id});
            }
            return columns;
        } else if constexpr (std::is_same_v<T, FilterPlan> ||
                             std::is_same_v<T, SortPlan>) {
            return outputRefs(op.input, depth + 1);
        } else if constexpr (std::is_same_v<T, GroupByPlan>) return op.keys;
        else return invalid("operator does not expose a relational column layout");
    }, plan->node);
}

void addRelation(std::vector<PlanRelation>& relations,
                 const std::shared_ptr<const TableSchema>& table,
                 std::uint64_t relation_id, const std::string& relation_name) {
    if (!table) return;
    for (const auto& present : relations)
        if (present.table && present.table->id.value == table->id.value &&
            present.relation_id == relation_id) return;
    relations.push_back({table, relation_id,
                         relation_name.empty() ? table->name : relation_name});
}

// 关系元数据只用于解释列名和验证布局，不会使被消除的子树继续执行。
void collectRelations(const PlanPtr& plan, std::vector<PlanRelation>& relations,
                      std::size_t depth = 0) {
    if (!plan || depth >= 256) return;
    std::visit([&](const auto& op) {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, SeqScanPlan>) {
            addRelation(relations, op.table, op.relation_id, op.relation_name);
        } else if constexpr (std::is_same_v<T, EmptyResultPlan>) {
            for (const auto& relation : op.relations)
                addRelation(relations, relation.table, relation.relation_id,
                            relation.relation_name);
        } else if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            collectRelations(op.left, relations, depth + 1);
            collectRelations(op.right, relations, depth + 1);
        } else if constexpr (std::is_same_v<T, DerivedTablePlan>) {
            addRelation(relations, op.table, op.relation_id, op.relation_name);
        } else if constexpr (std::is_same_v<T, FilterPlan> ||
                             std::is_same_v<T, GroupByPlan> ||
                             std::is_same_v<T, SortPlan>) {
            collectRelations(op.input, relations, depth + 1);
        }
    }, plan->node);
}

Result<PlanPtr> emptyLike(const PlanPtr& plan) {
    auto refs_result = outputRefs(plan);
    if (const auto* error = std::get_if<Diagnostic>(&refs_result)) return *error;
    auto columns = std::get<std::vector<BoundColumnRef>>(std::move(refs_result));
    if (columns.size() != plan->output.size())
        return invalid("EmptyResult columns must match output metadata");
    std::vector<PlanRelation> relations;
    collectRelations(plan, relations);
    return std::make_shared<const PlanNode>(PlanNode{
        EmptyResultPlan{std::move(columns), std::move(relations)},
        plan->output, plan->carries_row_id});
}

// 只有确定不会报运行期错误或产生副作用的输入才能被整棵跳过。
bool expressionCannotFail(const BoundExprPtr& expression, std::size_t depth = 0) {
    if (!expression || depth >= 256) return false;
    return std::visit([&](const auto& node) -> bool {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, BoundColumnRef> ||
                      std::is_same_v<T, BoundLiteral>) return true;
        else if constexpr (std::is_same_v<T, BoundAggregate> ||
                           std::is_same_v<T, BoundCase> ||
                           std::is_same_v<T, BoundInSubquery> ||
                           std::is_same_v<T, BoundExistsSubquery> ||
                           std::is_same_v<T, BoundScalarSubquery>) return false;
        else if constexpr (std::is_same_v<T, BoundUnary>) {
            return node.op != UnaryOp::Negate &&
                   expressionCannotFail(node.operand, depth + 1);
        } else if constexpr (std::is_same_v<T, BoundBinary>) {
            const bool arithmetic = node.op == BinaryOp::Add ||
                node.op == BinaryOp::Subtract || node.op == BinaryOp::Multiply ||
                node.op == BinaryOp::Divide;
            return !arithmetic && expressionCannotFail(node.left, depth + 1) &&
                   expressionCannotFail(node.right, depth + 1);
        }
    }, expression->node);
}

bool safeToSkip(const PlanPtr& plan, std::size_t depth = 0) {
    if (!plan || depth >= 256) return false;
    return std::visit([&](const auto& op) -> bool {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, SeqScanPlan> ||
                      std::is_same_v<T, EmptyResultPlan>) return true;
        else if constexpr (std::is_same_v<T, DerivedTablePlan>)
            return safeToSkip(op.input, depth + 1);
        else if constexpr (std::is_same_v<T, SetOperationPlan>)
            return safeToSkip(op.left, depth + 1) && safeToSkip(op.right, depth + 1);
        else if constexpr (std::is_same_v<T, FilterPlan>) {
            return expressionCannotFail(op.predicate) && safeToSkip(op.input, depth + 1);
        } else if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            return expressionCannotFail(op.predicate) &&
                   safeToSkip(op.left, depth + 1) && safeToSkip(op.right, depth + 1);
        } else if constexpr (std::is_same_v<T, GroupByPlan>) {
            return safeToSkip(op.input, depth + 1);
        } else if constexpr (std::is_same_v<T, SortPlan>) {
            for (const auto& item : op.expression_items)
                if (const auto* expression = std::get_if<BoundExprPtr>(&item.key);
                    expression && !expressionCannotFail(*expression)) return false;
            return safeToSkip(op.input, depth + 1);
        } else return false;
    }, plan->node);
}

bool projectionCannotFail(const ProjectPlan& project) {
    for (const auto& expression : project.expressions)
        if (!expressionCannotFail(expression)) return false;
    return true;
}

template <typename T>
PlanPtr replace(const PlanPtr& original, T operation) {
    return std::make_shared<const PlanNode>(PlanNode{
        std::move(operation), original->output, original->carries_row_id});
}

Result<PlanPtr> eliminateNode(const PlanPtr& plan, std::size_t depth) {
    if (!plan) return invalid("empty-result propagation found a missing plan node");
    if (depth >= 256) return invalid("plan exceeds 256 levels during empty-result propagation");
    return std::visit([&](const auto& op) -> Result<PlanPtr> {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, CreateTablePlan> ||
                      std::is_same_v<T, AlterTablePlan> ||
                      std::is_same_v<T, DropTablePlan> ||
                      std::is_same_v<T, InsertPlan> ||
                      std::is_same_v<T, SeqScanPlan>) return plan;
        else if constexpr (std::is_same_v<T, EmptyResultPlan>) {
            if (op.columns.size() != plan->output.size())
                return invalid("EmptyResult columns do not match output metadata");
            if (op.relations.empty())
                return invalid("EmptyResult requires retained relation metadata");
            for (const auto& relation : op.relations)
                if (!relation.table)
                    return invalid("EmptyResult relation table is missing");
            for (std::size_t index = 0; index < op.columns.size(); ++index) {
                const auto& ref = op.columns[index];
                bool matched = false;
                for (const auto& relation : op.relations) {
                    if (!relation.table || relation.table->id.value != ref.table_id.value ||
                        relation.relation_id != ref.relation_id) continue;
                    if (ref.ordinal >= relation.table->columns.size())
                        return invalid("EmptyResult column ordinal is outside its relation");
                    const auto& column = relation.table->columns[ref.ordinal];
                    if (column.id.value != ref.column_id.value || column.type != ref.type ||
                        plan->output[index].type != ref.type)
                        return invalid("EmptyResult column metadata does not match its relation");
                    matched = true;
                    break;
                }
                if (!matched)
                    return invalid("EmptyResult column has no matching relation metadata");
            }
            return plan;
        }
        else if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            auto left_result = eliminateNode(op.left, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&left_result)) return *error;
            auto right_result = eliminateNode(op.right, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&right_result)) return *error;
            auto left = std::get<PlanPtr>(std::move(left_result));
            auto right = std::get<PlanPtr>(std::move(right_result));
            PlanPtr current = left == op.left && right == op.right ? plan :
                replace(plan, NestedLoopJoinPlan{left, right, op.predicate, op.type});
            const bool left_empty = isEmpty(left);
            const bool right_empty = isEmpty(right);
            const bool false_on = booleanLiteral(op.predicate) == std::optional<bool>{false};
            bool empty = false;
            if (op.type == JoinType::Inner) {
                empty = (left_empty && safeToSkip(right)) ||
                        (right_empty && safeToSkip(left)) ||
                        (false_on && safeToSkip(left) && safeToSkip(right));
            } else if (op.type == JoinType::Left) {
                empty = left_empty && safeToSkip(right);
            } else if (op.type == JoinType::Right) {
                empty = right_empty && safeToSkip(left);
            } else empty = left_empty && right_empty;
            return empty ? emptyLike(current) : Result<PlanPtr>{current};
        } else if constexpr (std::is_same_v<T, CreateTablePlan> ||
                             std::is_same_v<T, CreateIndexPlan> ||
                             std::is_same_v<T, AlterTablePlan> ||
                             std::is_same_v<T, DropTablePlan> ||
                             std::is_same_v<T, DropIndexPlan> ||
                             std::is_same_v<T, InsertPlan> ||
                             std::is_same_v<T, SeqScanPlan> ||
                             std::is_same_v<T, IndexScanPlan> ||
                             std::is_same_v<T, EmptyResultPlan>) {
            return plan;
        } else if constexpr (std::is_same_v<T, SetOperationPlan>) {
            auto left_result = eliminateNode(op.left, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&left_result)) return *error;
            auto right_result = eliminateNode(op.right, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&right_result)) return *error;
            auto left = std::get<PlanPtr>(std::move(left_result));
            auto right = std::get<PlanPtr>(std::move(right_result));
            return left == op.left && right == op.right ? plan :
                replace(plan, SetOperationPlan{left, right, op.op, op.all});
        } else if constexpr (std::is_same_v<T, DerivedTablePlan>) {
            auto child_result = eliminateNode(op.input, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child_result)) return *error;
            auto input = std::get<PlanPtr>(std::move(child_result));
            if (input == op.input) return plan;
            return replace(plan, DerivedTablePlan{input, op.table, op.relation_id,
                                                  op.relation_name});
        } else {
            auto child_result = eliminateNode(op.input, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child_result)) return *error;
            auto input = std::get<PlanPtr>(std::move(child_result));
            if constexpr (std::is_same_v<T, FilterPlan>) {
                PlanPtr current = input == op.input ? plan :
                    replace(plan, FilterPlan{op.predicate, input});
                if (isEmpty(input)) return input;
                const bool false_filter =
                    booleanLiteral(op.predicate) == std::optional<bool>{false};
                return false_filter && safeToSkip(input) ? emptyLike(current) :
                    Result<PlanPtr>{current};
            } else if constexpr (std::is_same_v<T, GroupByPlan>) {
                PlanPtr current = input == op.input ? plan :
                    replace(plan, GroupByPlan{op.keys, input});
                return isEmpty(input) ? emptyLike(current) : Result<PlanPtr>{current};
            } else if constexpr (std::is_same_v<T, SortPlan>) {
                if (isEmpty(input)) return input;
                return input == op.input ? plan : replace(plan,
                    SortPlan{op.items, input, op.expression_items});
            } else if constexpr (std::is_same_v<T, AggregatePlan>) {
                return input == op.input ? plan : replace(plan, AggregatePlan{
                    op.group_keys, op.items, op.order_by, input, op.having,
                    op.distinct, op.limit, op.offset});
            } else if constexpr (std::is_same_v<T, ProjectPlan>) {
                PlanPtr current = input == op.input ? plan : replace(plan, ProjectPlan{
                    op.columns, input, op.expressions, op.distinct, op.limit, op.offset});
                // 当前执行器先计算投影再分页；仅无风险投影才允许 LIMIT 0 跳过输入。
                if (op.limit == std::optional<std::int64_t>{0} && !isEmpty(input) &&
                    projectionCannotFail(op) && safeToSkip(input)) {
                    auto empty = emptyLike(input);
                    if (const auto* error = std::get_if<Diagnostic>(&empty)) return *error;
                    return replace(current, ProjectPlan{op.columns,
                        std::get<PlanPtr>(std::move(empty)), op.expressions,
                        op.distinct, op.limit, op.offset});
                }
                return current;
            } else if constexpr (std::is_same_v<T, UpdatePlan>) {
                return input == op.input ? plan :
                    replace(plan, UpdatePlan{op.table, op.assignments, input});
            } else if constexpr (std::is_same_v<T, DeletePlan>) {
                return input == op.input ? plan : replace(plan, DeletePlan{op.table, input});
            } else {
                return input == op.input ? plan :
                    replace(plan, ExplainPlan{input, op.analyze});
            }
        }
    }, plan->node);
}

} // namespace

Result<PlanPtr> eliminateEmptyInputs(const PlanPtr& plan) {
    return eliminateNode(plan, 0);
}

} // namespace minisql::optimizer_detail
