// B 负责：将已通过语义检查的结构转换为扫描、过滤、投影和修改算子。
#include "minisql/compiler.hpp"

#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace minisql {
namespace {

Diagnostic invalid(std::string message, SourceLocation span = std::nullopt) {
    return {DiagnosticStage::Plan, ErrorCode::InvalidBoundStatement, std::move(message), span};
}

// 公共绑定结构可以被手工构造：检查指针、序号和身份，防止生成悬空/越界计划。
// 这不是第二次语义分析，不按名字查 Catalog，也不重新推导操作符类型。
bool validRef(const BoundColumnRef& ref, const TableSchema& table) {
    return ref.table_id.value == table.id.value && ref.ordinal < table.columns.size() &&
           ref.column_id.value == table.columns[ref.ordinal].id.value &&
           ref.type == table.columns[ref.ordinal].type;
}

using Tables = std::vector<std::shared_ptr<const TableSchema>>;

bool validRef(const BoundColumnRef& ref, const Tables& tables) {
    for (const auto& table : tables) if (table && validRef(ref, *table)) return true;
    return false;
}

bool sameRef(const BoundColumnRef& left, const BoundColumnRef& right) {
    return left.table_id.value == right.table_id.value &&
           left.column_id.value == right.column_id.value &&
           left.ordinal == right.ordinal && left.type == right.type;
}

bool containsRef(const std::vector<BoundColumnRef>& refs, const BoundColumnRef& target) {
    for (const auto& ref : refs) if (sameRef(ref, target)) return true;
    return false;
}

DataType valueType(const ScalarValue& value) {
    if (std::holds_alternative<std::int64_t>(value)) return DataType::Int;
    if (std::holds_alternative<double>(value)) return DataType::Float;
    if (std::holds_alternative<std::string>(value)) return DataType::Varchar;
    if (std::holds_alternative<bool>(value)) return DataType::Bool;
    return DataType::Null;
}

std::optional<Diagnostic> checkExpr(const BoundExprPtr& expr, const Tables& tables,
                                    std::size_t depth = 0) {
    if (!expr) return invalid("required bound expression is missing");
    if (depth >= 256) return invalid("bound expression exceeds 256 levels", expr->span);
    return std::visit([&](const auto& node) -> std::optional<Diagnostic> {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, BoundColumnRef>) {
            if (!validRef(node, tables) || node.type != expr->type)
                return invalid("bound column does not match target schema", expr->span);
        } else if constexpr (std::is_same_v<T, BoundLiteral>) {
            if (valueType(node.value) != expr->type)
                return invalid("bound literal type does not match value", expr->span);
        } else if constexpr (std::is_same_v<T, BoundUnary>) {
            return checkExpr(node.operand, tables, depth + 1);
        } else {
            if (auto error = checkExpr(node.left, tables, depth + 1)) return error;
            return checkExpr(node.right, tables, depth + 1);
        }
        return std::nullopt;
    }, expr->node);
}

std::optional<Diagnostic> validate(const BoundStatement& statement) {
    return std::visit([](const auto& stmt) -> std::optional<Diagnostic> {
        using T = std::decay_t<decltype(stmt)>;
        if constexpr (std::is_same_v<T, BoundCreateTable>) {
            if (stmt.table_name.empty() || stmt.columns.empty()) return invalid("CREATE requires a name and columns");
        } else if constexpr (std::is_same_v<T, BoundSelect>) {
            if (!stmt.table || stmt.table->columns.empty()) return invalid("base table schema is missing or empty");
            Tables tables{stmt.table};
            for (const auto& join : stmt.joins) {
                if (!join.table || join.table->columns.empty() || !join.on || join.on->type != DataType::Bool)
                    return invalid("JOIN requires a table and BOOL predicate");
                for (const auto& table : tables) {
                    if (table->id.value == join.table->id.value) return invalid("JOIN table is duplicated");
                }
                tables.push_back(join.table);
                if (auto error = checkExpr(join.on, tables)) return error;
            }
            if (stmt.where) {
                if (stmt.where->type != DataType::Bool) return invalid("WHERE must be BOOL", stmt.where->span);
                if (auto error = checkExpr(stmt.where, tables)) return error;
            }
            if (stmt.columns.empty()) return invalid("SELECT requires output columns");
            for (const auto& ref : stmt.columns)
                if (!validRef(ref, tables)) return invalid("SELECT column does not match visible schemas");
            std::vector<BoundColumnRef> checked_groups;
            for (const auto& ref : stmt.group_by) {
                if (!validRef(ref, tables)) return invalid("GROUP BY column does not match visible schemas");
                if (containsRef(checked_groups, ref)) return invalid("GROUP BY column is duplicated");
                checked_groups.push_back(ref);
            }
            if (!stmt.group_by.empty()) {
                for (const auto& ref : stmt.columns) {
                    if (!containsRef(stmt.group_by, ref))
                        return invalid("SELECT column must occur in GROUP BY");
                }
            }
            for (const auto& item : stmt.order_by) {
                if (!validRef(item.column, tables)) return invalid("ORDER BY column does not match visible schemas");
                if (!stmt.group_by.empty() && !containsRef(stmt.group_by, item.column))
                    return invalid("ORDER BY column must occur in GROUP BY");
            }
        } else {
            if (!stmt.table || stmt.table->columns.empty()) return invalid("target table schema is missing or empty");
            if constexpr (std::is_same_v<T, BoundInsert>) {
                if (stmt.values.size() != stmt.table->columns.size()) return invalid("INSERT values must cover all columns");
                for (std::size_t i = 0; i < stmt.values.size(); ++i) {
                    if (valueType(stmt.values[i]) != DataType::Null &&
                        valueType(stmt.values[i]) != stmt.table->columns[i].type)
                        return invalid("INSERT value type does not match schema");
                }
            } else {
                if (stmt.where) {
                    if (stmt.where->type != DataType::Bool) return invalid("WHERE must be BOOL", stmt.where->span);
                    if (auto error = checkExpr(stmt.where, Tables{stmt.table})) return error;
                }
                if constexpr (std::is_same_v<T, BoundUpdate>) {
                    if (stmt.assignments.empty()) return invalid("UPDATE requires assignments");
                    std::unordered_set<std::size_t> seen;
                    for (const auto& assignment : stmt.assignments) {
                        if (!validRef(assignment.target, *stmt.table) || !seen.insert(assignment.target.ordinal).second)
                            return invalid("UPDATE target is invalid or duplicated");
                        if (auto error = checkExpr(assignment.value, Tables{stmt.table})) return error;
                        if (assignment.target.type != assignment.value->type)
                            return invalid("UPDATE value type does not match target", assignment.value->span);
                    }
                }
            }
        }
        return std::nullopt;
    }, statement.node);
}

template <typename T>
PlanPtr node(T op, std::vector<OutputColumn> output = {}, bool row_id = false) {
    return std::make_shared<const PlanNode>(PlanNode{std::move(op), std::move(output), row_id});
}

// 所有查询/修改共用同一条输入流水线；首版扫描全表列，不做列裁剪。
PlanPtr source(const std::shared_ptr<const TableSchema>& table, const BoundExprPtr& where,
               bool row_id) {
    std::vector<OutputColumn> output;
    for (const auto& column : table->columns) output.push_back({column.name, column.type});
    auto input = node(SeqScanPlan{table}, output, row_id);
    if (where) input = node(FilterPlan{where, input}, output, row_id);
    return input;
}

std::vector<OutputColumn> scanOutput(const std::shared_ptr<const TableSchema>& table) {
    std::vector<OutputColumn> output;
    for (const auto& column : table->columns) output.push_back({column.name, column.type});
    return output;
}

const ColumnSchema& schemaColumn(const BoundColumnRef& ref, const Tables& tables) {
    for (const auto& table : tables) {
        if (table->id.value == ref.table_id.value) return table->columns[ref.ordinal];
    }
    throw std::logic_error("validated column reference has no table");
}

std::vector<OutputColumn> referencedOutput(const std::vector<BoundColumnRef>& refs,
                                           const Tables& tables) {
    std::vector<OutputColumn> output;
    for (const auto& ref : refs) {
        const auto& column = schemaColumn(ref, tables);
        output.push_back({column.name, ref.type});
    }
    return output;
}

PlanPtr selectSource(const BoundSelect& stmt, Tables& tables) {
    auto input = node(SeqScanPlan{stmt.table}, scanOutput(stmt.table));
    for (const auto& join : stmt.joins) {
        auto right = node(SeqScanPlan{join.table}, scanOutput(join.table));
        auto output = input->output;
        output.insert(output.end(), right->output.begin(), right->output.end());
        input = node(NestedLoopJoinPlan{input, right, join.on}, std::move(output));
        tables.push_back(join.table);
    }
    if (stmt.where) input = node(FilterPlan{stmt.where, input}, input->output);
    if (!stmt.group_by.empty())
        input = node(GroupByPlan{stmt.group_by, input}, referencedOutput(stmt.group_by, tables));
    if (!stmt.order_by.empty()) input = node(SortPlan{stmt.order_by, input}, input->output);
    return input;
}
} // namespace

Result<LogicalPlan> buildPlan(const BoundStatement& statement) {
    if (auto error = validate(statement)) return *error;
    auto root = std::visit([](const auto& stmt) -> PlanPtr {
        using T = std::decay_t<decltype(stmt)>;
        if constexpr (std::is_same_v<T, BoundCreateTable>) {
            return node(CreateTablePlan{stmt.table_name, stmt.columns});
        } else if constexpr (std::is_same_v<T, BoundInsert>) {
            return node(InsertPlan{stmt.table, stmt.values});
        } else if constexpr (std::is_same_v<T, BoundSelect>) {
            Tables tables{stmt.table};
            auto input = selectSource(stmt, tables);
            return node(ProjectPlan{stmt.columns, input}, referencedOutput(stmt.columns, tables));
        } else if constexpr (std::is_same_v<T, BoundUpdate>) {
            // 修改操作的输入携带行标识，根只返回影响行数（执行结果，不是业务列）。
            return node(UpdatePlan{stmt.table, stmt.assignments, source(stmt.table, stmt.where, true)});
        } else {
            return node(DeletePlan{stmt.table, source(stmt.table, stmt.where, true)});
        }
    }, statement.node);
    return LogicalPlan{statement.catalog_version, std::move(root)};
}

} // namespace minisql
