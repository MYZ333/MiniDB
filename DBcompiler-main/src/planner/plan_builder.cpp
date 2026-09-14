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

struct Relation {
    std::shared_ptr<const TableSchema> table;
    std::uint64_t id;
    std::string name;
};
using Relations = std::vector<Relation>;

bool validRef(const BoundColumnRef& ref, const Relations& relations) {
    for (const auto& relation : relations)
        if (relation.table && relation.id == ref.relation_id &&
            validRef(ref, *relation.table)) return true;
    return false;
}

bool sameRef(const BoundColumnRef& left, const BoundColumnRef& right) {
    return left.table_id.value == right.table_id.value &&
           left.column_id.value == right.column_id.value &&
           left.ordinal == right.ordinal && left.type == right.type &&
           left.relation_id == right.relation_id;
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

std::size_t utf8Length(const std::string& value) {
    std::size_t count = 0;
    for (unsigned char byte : value) if ((byte & 0xc0u) != 0x80u) ++count;
    return count;
}

std::optional<Diagnostic> checkExpr(const BoundExprPtr& expr, const Relations& relations,
                                    std::size_t depth = 0) {
    if (!expr) return invalid("required bound expression is missing");
    if (depth >= 256) return invalid("bound expression exceeds 256 levels", expr->span);
    return std::visit([&](const auto& node) -> std::optional<Diagnostic> {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, BoundColumnRef>) {
            if (!validRef(node, relations) || node.type != expr->type)
                return invalid("bound column does not match target schema", expr->span);
        } else if constexpr (std::is_same_v<T, BoundLiteral>) {
            if (valueType(node.value) != expr->type)
                return invalid("bound literal type does not match value", expr->span);
        } else if constexpr (std::is_same_v<T, BoundUnary>) {
            return checkExpr(node.operand, relations, depth + 1);
        } else if constexpr (std::is_same_v<T, BoundBinary>) {
            if (auto error = checkExpr(node.left, relations, depth + 1)) return error;
            return checkExpr(node.right, relations, depth + 1);
        } else {
            if (node.argument && !validRef(*node.argument, relations))
                return invalid("aggregate argument does not match visible schemas", node.span);
            if (!node.argument && node.kind != AggregateKind::Count)
                return invalid("only COUNT may omit its argument", node.span);
            if (node.type != expr->type)
                return invalid("aggregate expression result type is inconsistent", node.span);
        }
        return std::nullopt;
    }, expr->node);
}

std::optional<Diagnostic> validate(const BoundStatement& statement) {
    return std::visit([&](const auto& stmt) -> std::optional<Diagnostic> {
        using T = std::decay_t<decltype(stmt)>;
        if constexpr (std::is_same_v<T, BoundCreateTable>) {
            if (stmt.table_name.empty() || stmt.columns.empty()) return invalid("CREATE requires a name and columns");
            std::unordered_set<std::string> names;
            bool has_primary_key = false;
            for (const auto& column : stmt.columns) {
                if (column.name.empty() || !names.insert(column.name).second)
                    return invalid("CREATE column names must be non-empty and unique");
                if (column.type == DataType::Null)
                    return invalid("NULL is not a declarable column type");
                if (column.varchar_length &&
                    (column.type != DataType::Varchar || *column.varchar_length <= 0))
                    return invalid("VARCHAR length must be positive");
                if (column.primary_key && has_primary_key)
                    return invalid("CREATE permits only one PRIMARY KEY column");
                has_primary_key = has_primary_key || column.primary_key;
                if (!column.default_value) continue;
                const auto type = valueType(*column.default_value);
                if ((column.not_null || column.primary_key) && type == DataType::Null)
                    return invalid("NOT NULL column cannot default to NULL");
                if (type != DataType::Null && type != column.type)
                    return invalid("DEFAULT value type does not match column");
                if (column.varchar_length && type == DataType::Varchar &&
                    utf8Length(std::get<std::string>(*column.default_value)) >
                        static_cast<std::size_t>(*column.varchar_length))
                    return invalid("DEFAULT exceeds VARCHAR length");
            }
        } else if constexpr (std::is_same_v<T, BoundDropTable>) {
            if (stmt.table_names.empty()) return invalid("DROP requires table names");
            std::unordered_set<std::string> names;
            for (const auto& name : stmt.table_names)
                if (name.empty() || !names.insert(name).second)
                    return invalid("DROP table names must be non-empty and unique");
        } else if constexpr (std::is_same_v<T, BoundExplain>) {
            if (!stmt.target) return invalid("EXPLAIN target is missing");
            if (stmt.target->catalog_version != statement.catalog_version)
                return invalid("EXPLAIN target CatalogVersion is inconsistent");
            if (std::holds_alternative<BoundExplain>(stmt.target->node))
                return invalid("nested EXPLAIN is not supported");
            return validate(*stmt.target);
        } else if constexpr (std::is_same_v<T, BoundSelect>) {
            if (!stmt.table || stmt.table->columns.empty()) return invalid("base table schema is missing or empty");
            Relations relations{{stmt.table, stmt.relation_id,
                                 stmt.relation_name.empty() ? stmt.table->name : stmt.relation_name}};
            for (const auto& join : stmt.joins) {
                if (!join.table || join.table->columns.empty() || !join.on || join.on->type != DataType::Bool)
                    return invalid("JOIN requires a table and BOOL predicate");
                const std::string name =
                    join.relation_name.empty() ? join.table->name : join.relation_name;
                for (const auto& relation : relations) {
                    if (relation.id == join.relation_id || relation.name == name)
                        return invalid("JOIN relation identity or name is duplicated");
                }
                relations.push_back({join.table, join.relation_id, name});
                if (auto error = checkExpr(join.on, relations)) return error;
            }
            if (stmt.where) {
                if (stmt.where->type != DataType::Bool) return invalid("WHERE must be BOOL", stmt.where->span);
                if (auto error = checkExpr(stmt.where, relations)) return error;
            }
            if (stmt.columns.empty() && stmt.aggregate_items.empty() && stmt.projection_expressions.empty())
                return invalid("SELECT requires output columns");
            const auto output_count = !stmt.aggregate_items.empty() ? stmt.aggregate_items.size() :
                !stmt.projection_expressions.empty() ? stmt.projection_expressions.size() : stmt.columns.size();
            if (!stmt.output_names.empty() && stmt.output_names.size() != output_count)
                return invalid("SELECT output names do not match columns");
            for (const auto& ref : stmt.columns)
                if (!validRef(ref, relations)) return invalid("SELECT column does not match visible schemas");
            for (const auto& expression : stmt.projection_expressions)
                if (auto error = checkExpr(expression, relations)) return error;
            std::vector<BoundColumnRef> checked_groups;
            for (const auto& ref : stmt.group_by) {
                if (!validRef(ref, relations)) return invalid("GROUP BY column does not match visible schemas");
                if (containsRef(checked_groups, ref)) return invalid("GROUP BY column is duplicated");
                checked_groups.push_back(ref);
            }
            if (!stmt.aggregate_items.empty()) {
                if (stmt.output_names.size() != stmt.aggregate_items.size() || !stmt.order_by.empty())
                    return invalid("aggregate requires output names and post-aggregate ordering");
                for (const auto& item : stmt.aggregate_items) {
                    if (const auto* ref = std::get_if<BoundColumnRef>(&item.value)) {
                        if (!validRef(*ref, relations) || !containsRef(stmt.group_by, *ref))
                            return invalid("aggregate output column must occur in GROUP BY");
                    } else if (const auto* aggregate_ptr = std::get_if<BoundAggregate>(&item.value)) {
                        const auto& aggregate = *aggregate_ptr;
                        if (aggregate.argument && !validRef(*aggregate.argument, relations))
                            return invalid("aggregate argument does not match visible schemas", aggregate.span);
                        if (!aggregate.argument && aggregate.kind != AggregateKind::Count)
                            return invalid("only COUNT may omit its argument", aggregate.span);
                        const auto kind = aggregate.kind;
                        if (kind != AggregateKind::Count && kind != AggregateKind::Sum &&
                            kind != AggregateKind::Avg && kind != AggregateKind::Min &&
                            kind != AggregateKind::Max)
                            return invalid("unknown aggregate kind", aggregate.span);
                        const auto argument_type = aggregate.argument
                            ? aggregate.argument->type : DataType::Int;
                        if ((kind == AggregateKind::Sum || kind == AggregateKind::Avg) &&
                            argument_type != DataType::Int && argument_type != DataType::Float)
                            return invalid("numeric aggregate requires INT or FLOAT", aggregate.span);
                        const auto expected = kind == AggregateKind::Count ? DataType::Int :
                            kind == AggregateKind::Avg ? DataType::Float : argument_type;
                        if (aggregate.type != expected)
                            return invalid("aggregate result type is inconsistent", aggregate.span);
                    } else {
                        if (auto error = checkExpr(std::get<BoundExprPtr>(item.value), relations)) return error;
                    }
                }
                for (const auto& item : stmt.aggregate_order_by) {
                    if (const auto* ordinal = std::get_if<std::size_t>(&item.key)) {
                        if (*ordinal >= stmt.aggregate_items.size())
                            return invalid("aggregate ORDER BY output ordinal is out of range");
                    } else {
                        if (const auto* ref = std::get_if<BoundColumnRef>(&item.key)) {
                            if (!validRef(*ref, relations) || !containsRef(stmt.group_by, *ref))
                                return invalid("aggregate ORDER BY column must occur in GROUP BY");
                        } else if (auto error = checkExpr(std::get<BoundExprPtr>(item.key), relations)) return error;
                    }
                }
            } else if (!stmt.group_by.empty()) {
                for (const auto& ref : stmt.columns) {
                    if (!containsRef(stmt.group_by, ref))
                        return invalid("SELECT column must occur in GROUP BY");
                }
            }
            for (const auto& item : stmt.order_by) {
                if (!validRef(item.column, relations)) return invalid("ORDER BY column does not match visible schemas");
                if (!stmt.group_by.empty() && !containsRef(stmt.group_by, item.column))
                    return invalid("ORDER BY column must occur in GROUP BY");
            }
            for (const auto& item : stmt.expression_order_by) {
                if (const auto* ref = std::get_if<BoundColumnRef>(&item.key)) {
                    if (!validRef(*ref, relations)) return invalid("ORDER BY column does not match visible schemas");
                } else if (auto error = checkExpr(std::get<BoundExprPtr>(item.key), relations)) return error;
            }
            if (stmt.having) {
                if (stmt.having->type != DataType::Bool) return invalid("HAVING must be BOOL", stmt.having->span);
                if (auto error = checkExpr(stmt.having, relations)) return error;
            }
            if (stmt.limit && *stmt.limit < 0) return invalid("LIMIT must be non-negative");
            if (stmt.offset < 0) return invalid("OFFSET must be non-negative");
        } else {
            if (!stmt.table || stmt.table->columns.empty()) return invalid("target table schema is missing or empty");
            if constexpr (std::is_same_v<T, BoundInsert>) {
                const auto rows = stmt.rows.empty()
                    ? std::vector<std::vector<ScalarValue>>{stmt.values} : stmt.rows;
                if (rows.empty()) return invalid("INSERT requires rows");
                for (const auto& row : rows) {
                    if (row.size() != stmt.table->columns.size())
                        return invalid("INSERT values must cover all columns");
                    for (std::size_t i = 0; i < row.size(); ++i) {
                        const auto type = valueType(row[i]);
                        const auto& column = stmt.table->columns[i];
                        if (type == DataType::Null && column.not_null)
                            return invalid("INSERT cannot store NULL in a NOT NULL column");
                        if (type != DataType::Null && type != column.type)
                            return invalid("INSERT value type does not match schema");
                        if (column.varchar_length && type == DataType::Varchar &&
                            utf8Length(std::get<std::string>(row[i])) >
                                static_cast<std::size_t>(*column.varchar_length))
                            return invalid("INSERT value exceeds VARCHAR length");
                    }
                }
            } else {
                if (stmt.where) {
                    if (stmt.where->type != DataType::Bool) return invalid("WHERE must be BOOL", stmt.where->span);
                    if (auto error = checkExpr(stmt.where,
                        Relations{{stmt.table, 0, stmt.table->name}})) return error;
                }
                if constexpr (std::is_same_v<T, BoundUpdate>) {
                    if (stmt.assignments.empty()) return invalid("UPDATE requires assignments");
                    std::unordered_set<std::size_t> seen;
                    for (const auto& assignment : stmt.assignments) {
                        if (!validRef(assignment.target, *stmt.table) || !seen.insert(assignment.target.ordinal).second)
                            return invalid("UPDATE target is invalid or duplicated");
                        if (auto error = checkExpr(assignment.value,
                            Relations{{stmt.table, 0, stmt.table->name}})) return error;
                        if (assignment.value->type == DataType::Null &&
                            stmt.table->columns[assignment.target.ordinal].not_null)
                            return invalid("UPDATE cannot store NULL in a NOT NULL column",
                                           assignment.value->span);
                        if (assignment.value->type != DataType::Null &&
                            assignment.target.type != assignment.value->type)
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

const ColumnSchema& schemaColumn(const BoundColumnRef& ref, const Relations& relations) {
    for (const auto& relation : relations) {
        if (relation.id == ref.relation_id &&
            relation.table->id.value == ref.table_id.value)
            return relation.table->columns[ref.ordinal];
    }
    throw std::logic_error("validated column reference has no table");
}

std::vector<OutputColumn> referencedOutput(const std::vector<BoundColumnRef>& refs,
                                           const Relations& relations,
                                           const std::vector<std::string>& names = {}) {
    std::vector<OutputColumn> output;
    for (std::size_t i = 0; i < refs.size(); ++i) {
        const auto& column = schemaColumn(refs[i], relations);
        output.push_back({names.empty() ? column.name : names[i], refs[i].type});
    }
    return output;
}

PlanPtr selectSource(const BoundSelect& stmt, Relations& relations) {
    auto input = node(SeqScanPlan{stmt.table, stmt.relation_id,
        stmt.relation_name.empty() ? stmt.table->name : stmt.relation_name}, scanOutput(stmt.table));
    for (const auto& join : stmt.joins) {
        auto right = node(SeqScanPlan{join.table, join.relation_id,
            join.relation_name.empty() ? join.table->name : join.relation_name},
            scanOutput(join.table));
        auto output = input->output;
        output.insert(output.end(), right->output.begin(), right->output.end());
        input = node(NestedLoopJoinPlan{input, right, join.on, join.type}, std::move(output));
        relations.push_back({join.table, join.relation_id,
            join.relation_name.empty() ? join.table->name : join.relation_name});
    }
    if (stmt.where) input = node(FilterPlan{stmt.where, input}, input->output);
    // 聚合节点必须看到过滤后的明细行，分组和排序由 Aggregate 自己完成。
    if (!stmt.aggregate_items.empty()) return input;
    if (!stmt.group_by.empty())
        input = node(GroupByPlan{stmt.group_by, input}, referencedOutput(stmt.group_by, relations));
    if (!stmt.order_by.empty() || !stmt.expression_order_by.empty())
        input = node(SortPlan{stmt.order_by, input, stmt.expression_order_by}, input->output);
    return input;
}
} // namespace

Result<LogicalPlan> buildPlan(const BoundStatement& statement) {
    if (auto error = validate(statement)) return *error;
    if (const auto* explain = std::get_if<BoundExplain>(&statement.node)) {
        auto target = buildPlan(*explain->target);
        if (const auto* error = std::get_if<Diagnostic>(&target)) return *error;
        auto input = std::get<LogicalPlan>(std::move(target)).root;
        // EXPLAIN 本身是一个查询根，其单列文本由执行层生成。
        return LogicalPlan{statement.catalog_version,
            node(ExplainPlan{std::move(input), explain->analyze}, {{"QUERY PLAN", DataType::Varchar}})};
    }
    auto root = std::visit([](const auto& stmt) -> PlanPtr {
        using T = std::decay_t<decltype(stmt)>;
        if constexpr (std::is_same_v<T, BoundCreateTable>) {
            return node(CreateTablePlan{stmt.table_name, stmt.columns});
        } else if constexpr (std::is_same_v<T, BoundDropTable>) {
            return node(DropTablePlan{stmt.table_names, stmt.if_exists});
        } else if constexpr (std::is_same_v<T, BoundInsert>) {
            return node(InsertPlan{stmt.table, stmt.values, stmt.rows});
        } else if constexpr (std::is_same_v<T, BoundSelect>) {
            Relations relations{{stmt.table, stmt.relation_id,
                stmt.relation_name.empty() ? stmt.table->name : stmt.relation_name}};
            auto input = selectSource(stmt, relations);
            if (!stmt.aggregate_items.empty()) {
                std::vector<OutputColumn> output;
                for (std::size_t i = 0; i < stmt.aggregate_items.size(); ++i) {
                    const auto type = std::visit([](const auto& item) {
                        using I = std::decay_t<decltype(item)>;
                        if constexpr (std::is_same_v<I, BoundExprPtr>) return item->type;
                        else return item.type;
                    }, stmt.aggregate_items[i].value);
                    output.push_back({stmt.output_names[i], type});
                }
                return node(AggregatePlan{stmt.group_by, stmt.aggregate_items,
                                          stmt.aggregate_order_by, input, stmt.having,
                                          stmt.distinct, stmt.limit, stmt.offset}, std::move(output));
            }
            std::vector<OutputColumn> output;
            if (!stmt.projection_expressions.empty()) {
                for (std::size_t i = 0; i < stmt.projection_expressions.size(); ++i)
                    output.push_back({stmt.output_names[i], stmt.projection_expressions[i]->type});
            } else output = referencedOutput(stmt.columns, relations, stmt.output_names);
            return node(ProjectPlan{stmt.columns, input, stmt.projection_expressions,
                                    stmt.distinct, stmt.limit, stmt.offset}, std::move(output));
        } else if constexpr (std::is_same_v<T, BoundUpdate>) {
            // 修改操作的输入携带行标识，根只返回影响行数（执行结果，不是业务列）。
            return node(UpdatePlan{stmt.table, stmt.assignments, source(stmt.table, stmt.where, true)});
        } else if constexpr (std::is_same_v<T, BoundDelete>) {
            return node(DeletePlan{stmt.table, source(stmt.table, stmt.where, true)});
        } else return nullptr; // BoundExplain 已在访问 variant 前递归生成。
    }, statement.node);
    return LogicalPlan{statement.catalog_version, std::move(root)};
}

} // namespace minisql
