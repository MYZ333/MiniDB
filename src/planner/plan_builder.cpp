// B 负责：将已通过语义检查的结构转换为扫描、过滤、投影和修改算子。
#include "minisql/compiler.hpp"

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

DataType valueType(const ScalarValue& value) {
    if (std::holds_alternative<std::int64_t>(value)) return DataType::Int;
    if (std::holds_alternative<std::string>(value)) return DataType::Varchar;
    return DataType::Bool;
}

std::optional<Diagnostic> checkExpr(const BoundExprPtr& expr, const TableSchema& table,
                                    std::size_t depth = 0) {
    if (!expr) return invalid("required bound expression is missing");
    if (depth >= 256) return invalid("bound expression exceeds 256 levels", expr->span);
    return std::visit([&](const auto& node) -> std::optional<Diagnostic> {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, BoundColumnRef>) {
            if (!validRef(node, table) || node.type != expr->type)
                return invalid("bound column does not match target schema", expr->span);
        } else if constexpr (std::is_same_v<T, BoundLiteral>) {
            if (valueType(node.value) != expr->type)
                return invalid("bound literal type does not match value", expr->span);
        } else if constexpr (std::is_same_v<T, BoundUnary>) {
            return checkExpr(node.operand, table, depth + 1);
        } else {
            if (auto error = checkExpr(node.left, table, depth + 1)) return error;
            return checkExpr(node.right, table, depth + 1);
        }
        return std::nullopt;
    }, expr->node);
}

std::optional<Diagnostic> validate(const BoundStatement& statement) {
    return std::visit([](const auto& stmt) -> std::optional<Diagnostic> {
        using T = std::decay_t<decltype(stmt)>;
        if constexpr (std::is_same_v<T, BoundCreateTable>) {
            if (stmt.table_name.empty() || stmt.columns.empty()) return invalid("CREATE requires a name and columns");
        } else {
            if (!stmt.table || stmt.table->columns.empty()) return invalid("target table schema is missing or empty");
            if constexpr (std::is_same_v<T, BoundInsert>) {
                if (stmt.values.size() != stmt.table->columns.size()) return invalid("INSERT values must cover all columns");
                for (std::size_t i = 0; i < stmt.values.size(); ++i) {
                    if (valueType(stmt.values[i]) != stmt.table->columns[i].type)
                        return invalid("INSERT value type does not match schema");
                }
            } else {
                if (stmt.where) {
                    if (stmt.where->type != DataType::Bool) return invalid("WHERE must be BOOL", stmt.where->span);
                    if (auto error = checkExpr(stmt.where, *stmt.table)) return error;
                }
                if constexpr (std::is_same_v<T, BoundSelect>) {
                    if (stmt.columns.empty()) return invalid("SELECT requires output columns");
                    for (const auto& ref : stmt.columns) {
                        if (!validRef(ref, *stmt.table)) return invalid("SELECT column does not match target schema");
                    }
                } else if constexpr (std::is_same_v<T, BoundUpdate>) {
                    if (stmt.assignments.empty()) return invalid("UPDATE requires assignments");
                    std::unordered_set<std::size_t> seen;
                    for (const auto& assignment : stmt.assignments) {
                        if (!validRef(assignment.target, *stmt.table) || !seen.insert(assignment.target.ordinal).second)
                            return invalid("UPDATE target is invalid or duplicated");
                        if (auto error = checkExpr(assignment.value, *stmt.table)) return error;
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
            std::vector<OutputColumn> output;
            for (const auto& column : stmt.columns)
                output.push_back({stmt.table->columns[column.ordinal].name, column.type});
            return node(ProjectPlan{stmt.columns, source(stmt.table, stmt.where, false)}, std::move(output));
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
