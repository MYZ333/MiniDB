// 列裁剪：从根节点反向收集列依赖，让 SeqScan 只物化投影、条件和连接所需列。
#include "plan_rules.hpp"

#include <type_traits>
#include <utility>

namespace minisql::optimizer_detail {
namespace {

using RequiredColumns = std::vector<BoundColumnRef>;

Diagnostic invalid(std::string message) {
    return {DiagnosticStage::Plan, ErrorCode::InvalidPlan, std::move(message), std::nullopt};
}

bool sameRef(const BoundColumnRef& left, const BoundColumnRef& right) {
    return left.table_id.value == right.table_id.value &&
           left.column_id.value == right.column_id.value &&
           left.relation_id == right.relation_id && left.ordinal == right.ordinal &&
           left.type == right.type;
}

bool contains(const RequiredColumns& columns, const BoundColumnRef& target) {
    for (const auto& column : columns) if (sameRef(column, target)) return true;
    return false;
}

void require(RequiredColumns& columns, const BoundColumnRef& column) {
    if (!contains(columns, column)) columns.push_back(column);
}

void require(RequiredColumns& columns, const BoundExprPtr& expression,
             std::size_t depth = 0) {
    if (!expression || depth >= 256) return;
    std::visit([&](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, BoundColumnRef>) require(columns, node);
        else if constexpr (std::is_same_v<T, BoundAggregate>) {
            if (node.argument) require(columns, *node.argument);
        } else if constexpr (std::is_same_v<T, BoundUnary>) {
            require(columns, node.operand, depth + 1);
        } else if constexpr (std::is_same_v<T, BoundBinary>) {
            require(columns, node.left, depth + 1);
            require(columns, node.right, depth + 1);
        } else if constexpr (std::is_same_v<T, BoundCase>) {
            require(columns, node.operand, depth + 1);
            for (const auto& branch : node.branches) {
                require(columns, branch.condition, depth + 1);
                require(columns, branch.result, depth + 1);
            }
            require(columns, node.else_result, depth + 1);
        } else if constexpr (std::is_same_v<T, BoundInSubquery>) {
            require(columns, node.value, depth + 1);
            for (const auto& ref : node.correlated_columns) require(columns, ref);
        } else if constexpr (std::is_same_v<T, BoundExistsSubquery> ||
                             std::is_same_v<T, BoundScalarSubquery>) {
            for (const auto& ref : node.correlated_columns) require(columns, ref);
        }
    }, expression->node);
}

bool relationOccurs(const PlanPtr& plan, const BoundColumnRef& ref,
                    std::size_t depth = 0) {
    if (!plan || depth >= 256) return false;
    return std::visit([&](const auto& op) -> bool {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, SeqScanPlan>) {
            return op.table && op.table->id.value == ref.table_id.value &&
                   op.relation_id == ref.relation_id;
        } else if constexpr (std::is_same_v<T, IndexScanPlan>) {
            return op.table && op.table->id.value == ref.table_id.value &&
                   op.relation_id == ref.relation_id;
        } else if constexpr (std::is_same_v<T, EmptyResultPlan>) {
            for (const auto& column : op.columns)
                if (sameRef(column, ref)) return true;
            return false;
        } else if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            return relationOccurs(op.left, ref, depth + 1) ||
                   relationOccurs(op.right, ref, depth + 1);
        } else if constexpr (std::is_same_v<T, DerivedTablePlan>) {
            return op.table && op.table->id.value == ref.table_id.value &&
                   op.relation_id == ref.relation_id;
        } else if constexpr (std::is_same_v<T, SetOperationPlan>) {
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

bool sameOutput(const std::vector<OutputColumn>& left,
                const std::vector<OutputColumn>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (left[i].name != right[i].name || left[i].type != right[i].type) return false;
    return true;
}

bool sameSelection(const std::optional<RequiredColumns>& left,
                   const std::optional<RequiredColumns>& right) {
    if (left.has_value() != right.has_value()) return false;
    if (!left) return true;
    if (left->size() != right->size()) return false;
    for (std::size_t i = 0; i < left->size(); ++i)
        if (!sameRef((*left)[i], (*right)[i])) return false;
    return true;
}

bool sameRefs(const RequiredColumns& left, const RequiredColumns& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (!sameRef(left[i], right[i])) return false;
    return true;
}

std::vector<OutputColumn> concatenate(const PlanPtr& left, const PlanPtr& right) {
    auto output = left->output;
    output.insert(output.end(), right->output.begin(), right->output.end());
    return output;
}

template <typename T>
PlanPtr rebuild(const PlanPtr&, T operation,
                std::vector<OutputColumn> output, bool carries_row_id) {
    return std::make_shared<const PlanNode>(PlanNode{
        std::move(operation), std::move(output), carries_row_id});
}

BoundColumnRef tableColumn(const SeqScanPlan& scan, std::size_t ordinal) {
    const auto& column = scan.table->columns[ordinal];
    return {scan.table->id, column.id, ordinal, column.type, scan.relation_id};
}

Result<PlanPtr> pruneNode(const PlanPtr& plan, RequiredColumns required,
                          std::size_t depth) {
    if (!plan) return invalid("column pruning found a missing plan node");
    if (depth >= 256) return invalid("plan exceeds 256 levels during column pruning");
    return std::visit([&](const auto& op) -> Result<PlanPtr> {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, CreateTablePlan> ||
                      std::is_same_v<T, CreateIndexPlan> ||
                      std::is_same_v<T, AlterTablePlan> ||
                      std::is_same_v<T, DropTablePlan> ||
                      std::is_same_v<T, DropIndexPlan> ||
                      std::is_same_v<T, InsertPlan>) return plan;
        else if constexpr (std::is_same_v<T, SeqScanPlan> ||
                           std::is_same_v<T, IndexScanPlan>) {
            if (!op.table) return invalid("scan table is missing during column pruning");
            RequiredColumns selected;
            std::vector<OutputColumn> output;
            for (std::size_t ordinal = 0; ordinal < op.table->columns.size(); ++ordinal) {
                const auto& column = op.table->columns[ordinal];
                const auto ref = BoundColumnRef{
                    op.table->id, column.id, ordinal, column.type, op.relation_id};
                if (!contains(required, ref)) continue;
                selected.push_back(ref);
                output.push_back({op.table->columns[ordinal].name,
                                  op.table->columns[ordinal].type});
            }
            for (const auto& ref : required) {
                if (ref.table_id.value == op.table->id.value &&
                    ref.relation_id == op.relation_id && !contains(selected, ref))
                    return invalid("scan required column does not match its table schema");
            }
            // 全列仍用 nullopt，保持老协议和未裁剪计划的紧凑表示。
            std::optional<RequiredColumns> selection = selected.size() == op.table->columns.size()
                ? std::nullopt : std::optional<RequiredColumns>{selected};
            if (sameSelection(op.columns, selection) && sameOutput(plan->output, output))
                return plan;
            if constexpr (std::is_same_v<T, SeqScanPlan>) {
                return rebuild(plan, SeqScanPlan{op.table, op.relation_id, op.relation_name,
                                                 std::move(selection)},
                               std::move(output), plan->carries_row_id);
            } else {
                return rebuild(plan, IndexScanPlan{op.table, op.index, op.relation_id,
                                                   op.relation_name, op.lower, op.upper,
                                                   std::move(selection)},
                               std::move(output), plan->carries_row_id);
            }
        } else if constexpr (std::is_same_v<T, EmptyResultPlan>) {
            if (op.columns.size() != plan->output.size())
                return invalid("EmptyResult columns do not match output metadata");
            RequiredColumns selected;
            std::vector<OutputColumn> output;
            for (std::size_t index = 0; index < op.columns.size(); ++index) {
                if (!contains(required, op.columns[index])) continue;
                selected.push_back(op.columns[index]);
                output.push_back(plan->output[index]);
            }
            if (sameRefs(op.columns, selected) && sameOutput(plan->output, output))
                return plan;
            return rebuild(plan, EmptyResultPlan{std::move(selected), op.relations},
                           std::move(output), plan->carries_row_id);
        } else if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            require(required, op.predicate);
            RequiredColumns left_required, right_required;
            for (const auto& ref : required) {
                const bool left = relationOccurs(op.left, ref);
                const bool right = relationOccurs(op.right, ref);
                if (left == right)
                    return invalid("JOIN column dependency is missing or ambiguous");
                require(left ? left_required : right_required, ref);
            }
            auto left_result = pruneNode(op.left, std::move(left_required), depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&left_result)) return *error;
            auto right_result = pruneNode(op.right, std::move(right_required), depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&right_result)) return *error;
            auto left = std::get<PlanPtr>(std::move(left_result));
            auto right = std::get<PlanPtr>(std::move(right_result));
            auto output = concatenate(left, right);
            if (left == op.left && right == op.right && sameOutput(plan->output, output))
                return plan;
            return rebuild(plan, NestedLoopJoinPlan{left, right, op.predicate, op.type},
                           std::move(output), plan->carries_row_id);
        } else if constexpr (std::is_same_v<T, DerivedTablePlan>) {
            // 派生表的列身份与子查询内部身份不同，保持完整子查询输出并在边界重标记。
            auto child = pruneNode(op.input, {}, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child)) return *error;
            auto input = std::get<PlanPtr>(std::move(child));
            if (input == op.input) return plan;
            return rebuild(plan, DerivedTablePlan{input, op.table, op.relation_id,
                op.relation_name}, plan->output, plan->carries_row_id);
        } else if constexpr (std::is_same_v<T, SetOperationPlan>) {
            auto left_result = pruneNode(op.left, {}, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&left_result)) return *error;
            auto right_result = pruneNode(op.right, {}, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&right_result)) return *error;
            auto left = std::get<PlanPtr>(std::move(left_result));
            auto right = std::get<PlanPtr>(std::move(right_result));
            if (left == op.left && right == op.right) return plan;
            return rebuild(plan, SetOperationPlan{left, right, op.op, op.all},
                           plan->output, plan->carries_row_id);
        } else if constexpr (std::is_same_v<T, FilterPlan>) {
            require(required, op.predicate);
            auto child = pruneNode(op.input, std::move(required), depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child)) return *error;
            auto input = std::get<PlanPtr>(std::move(child));
            if (input == op.input && sameOutput(plan->output, input->output)) return plan;
            return rebuild(plan, FilterPlan{op.predicate, input}, input->output,
                           input->carries_row_id);
        } else if constexpr (std::is_same_v<T, SortPlan>) {
            for (const auto& item : op.items) require(required, item.column);
            for (const auto& item : op.expression_items) {
                if (const auto* ref = std::get_if<BoundColumnRef>(&item.key)) require(required, *ref);
                else require(required, std::get<BoundExprPtr>(item.key));
            }
            auto child = pruneNode(op.input, std::move(required), depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child)) return *error;
            auto input = std::get<PlanPtr>(std::move(child));
            if (input == op.input && sameOutput(plan->output, input->output)) return plan;
            return rebuild(plan, SortPlan{op.items, input, op.expression_items},
                           input->output, input->carries_row_id);
        } else if constexpr (std::is_same_v<T, GroupByPlan>) {
            RequiredColumns keys;
            for (const auto& key : op.keys) require(keys, key);
            auto child = pruneNode(op.input, std::move(keys), depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child)) return *error;
            auto input = std::get<PlanPtr>(std::move(child));
            if (input == op.input) return plan;
            return rebuild(plan, GroupByPlan{op.keys, input}, plan->output,
                           plan->carries_row_id);
        } else if constexpr (std::is_same_v<T, AggregatePlan>) {
            RequiredColumns dependencies;
            for (const auto& key : op.group_keys) require(dependencies, key);
            for (const auto& item : op.items) {
                std::visit([&](const auto& value) {
                    using I = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<I, BoundColumnRef>) require(dependencies, value);
                    else if constexpr (std::is_same_v<I, BoundAggregate>) {
                        if (value.argument) require(dependencies, *value.argument);
                    } else require(dependencies, value);
                }, item.value);
            }
            for (const auto& item : op.order_by) {
                if (const auto* ref = std::get_if<BoundColumnRef>(&item.key)) require(dependencies, *ref);
                else if (const auto* expression = std::get_if<BoundExprPtr>(&item.key))
                    require(dependencies, *expression);
            }
            require(dependencies, op.having);
            auto child = pruneNode(op.input, std::move(dependencies), depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child)) return *error;
            auto input = std::get<PlanPtr>(std::move(child));
            if (input == op.input) return plan;
            return rebuild(plan, AggregatePlan{op.group_keys, op.items, op.order_by, input,
                op.having, op.distinct, op.limit, op.offset}, plan->output,
                plan->carries_row_id);
        } else if constexpr (std::is_same_v<T, ProjectPlan>) {
            RequiredColumns dependencies;
            if (op.expressions.empty()) {
                for (const auto& column : op.columns) require(dependencies, column);
            } else {
                for (const auto& expression : op.expressions) require(dependencies, expression);
            }
            auto child = pruneNode(op.input, std::move(dependencies), depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child)) return *error;
            auto input = std::get<PlanPtr>(std::move(child));
            if (input == op.input) return plan;
            return rebuild(plan, ProjectPlan{op.columns, input, op.expressions,
                op.distinct, op.limit, op.offset}, plan->output, plan->carries_row_id);
        } else if constexpr (std::is_same_v<T, UpdatePlan>) {
            RequiredColumns dependencies;
            // 当前更新器从旧行复制未修改列并执行整行约束检查，因此保留全列。
            for (std::size_t ordinal = 0; ordinal < op.table->columns.size(); ++ordinal)
                require(dependencies, BoundColumnRef{op.table->id,
                    op.table->columns[ordinal].id, ordinal,
                    op.table->columns[ordinal].type, 0});
            auto child = pruneNode(op.input, std::move(dependencies), depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child)) return *error;
            auto input = std::get<PlanPtr>(std::move(child));
            if (input == op.input) return plan;
            return rebuild(plan, UpdatePlan{op.table, op.assignments, input},
                           plan->output, plan->carries_row_id);
        } else if constexpr (std::is_same_v<T, DeletePlan>) {
            auto child = pruneNode(op.input, {}, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child)) return *error;
            auto input = std::get<PlanPtr>(std::move(child));
            if (input == op.input) return plan;
            return rebuild(plan, DeletePlan{op.table, input}, plan->output,
                           plan->carries_row_id);
        } else {
            auto child = pruneNode(op.input, {}, depth + 1);
            if (const auto* error = std::get_if<Diagnostic>(&child)) return *error;
            auto input = std::get<PlanPtr>(std::move(child));
            if (input == op.input) return plan;
            return rebuild(plan, ExplainPlan{input, op.analyze}, plan->output,
                           plan->carries_row_id);
        }
    }, plan->node);
}

} // namespace

Result<PlanPtr> pruneColumns(const PlanPtr& plan) {
    return pruneNode(plan, {}, 0);
}

} // namespace minisql::optimizer_detail
