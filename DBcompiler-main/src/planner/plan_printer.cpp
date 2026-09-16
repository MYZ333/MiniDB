// 文本展示与执行解耦：递归打印实际计划节点，保留算子顺序、表达式和行标识属性。
#include "minisql/plan_printer.hpp"

#include <locale>
#include <limits>
#include <sstream>
#include <type_traits>

namespace minisql {
namespace {
const char* typeName(DataType type) {
    switch (type) {
    case DataType::Int: return "INT";
    case DataType::Varchar: return "VARCHAR";
    case DataType::Bool: return "BOOL";
    case DataType::Float: return "FLOAT";
    case DataType::Null: return "NULL";
    }
    return "UNKNOWN";
}

const char* opName(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Subtract: return "-";
    case BinaryOp::Multiply: return "*";
    case BinaryOp::Divide: return "/";
    case BinaryOp::Equal: return "=";
    case BinaryOp::NotEqual: return "!=";
    case BinaryOp::Less: return "<";
    case BinaryOp::LessEqual: return "<=";
    case BinaryOp::Greater: return ">";
    case BinaryOp::GreaterEqual: return ">=";
    case BinaryOp::And: return "AND";
    case BinaryOp::Or: return "OR";
    case BinaryOp::Like: return "LIKE";
    }
    return "UNKNOWN";
}

const char* aggregateName(AggregateKind kind) {
    switch (kind) {
    case AggregateKind::Count: return "COUNT";
    case AggregateKind::Sum: return "SUM";
    case AggregateKind::Avg: return "AVG";
    case AggregateKind::Min: return "MIN";
    case AggregateKind::Max: return "MAX";
    }
    return "UNKNOWN";
}

const char* joinName(JoinType type) {
    switch (type) {
    case JoinType::Inner: return "INNER";
    case JoinType::Left: return "LEFT";
    case JoinType::Right: return "RIGHT";
    case JoinType::Full: return "FULL";
    }
    return "UNKNOWN";
}

const char* setName(SetOperator op) {
    switch (op) {
    case SetOperator::Union: return "UNION";
    case SetOperator::Intersect: return "INTERSECT";
    case SetOperator::Except: return "EXCEPT";
    }
    return "UNKNOWN";
}

std::string literal(const ScalarValue& value) {
    return std::visit([](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::string>) {
            // 单引号翻倍，换行和反斜杠显式显示，防止值内容破坏计划树的行结构。
            std::string text = "'";
            for (char ch : item) {
                switch (ch) {
                case '\'': text += "''"; break;
                case '\\': text += "\\\\"; break;
                case '\n': text += "\\n"; break;
                case '\r': text += "\\r"; break;
                case '\t': text += "\\t"; break;
                default: text += ch;
                }
            }
            return text + "'";
        } else if constexpr (std::is_same_v<T, bool>) {
            return item ? "TRUE" : "FALSE";
        } else if constexpr (std::is_same_v<T, NullValue>) {
            return "NULL";
        } else if constexpr (std::is_same_v<T, double>) {
            std::ostringstream stream;
            stream.imbue(std::locale::classic());
            stream.precision(std::numeric_limits<double>::max_digits10);
            stream << item;
            return stream.str();
        } else {
            return std::to_string(item);
        }
    }, value);
}

std::string tableName(const std::shared_ptr<const TableSchema>& table) {
    return table ? table->name + "#" + std::to_string(table->id.value) : "<missing-table>";
}

struct Relation {
    std::shared_ptr<const TableSchema> table;
    std::uint64_t id;
    std::string name;
};
using Relations = std::vector<Relation>;

void addRelation(Relations& relations, const std::shared_ptr<const TableSchema>& table,
                 std::uint64_t id, std::string name) {
    if (!table) return;
    for (const auto& present : relations) if (present.id == id) return;
    relations.push_back({table, id, name.empty() ? table->name : std::move(name)});
}

// 从计划自身收集模式，不回查 Catalog；JOIN 的左右两棵子树都要遍历。
void collectRelations(const PlanPtr& plan, Relations& relations, std::size_t depth = 0) {
    if (!plan || depth >= 256) return;
    std::visit([&](const auto& op) {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, NestedLoopJoinPlan> ||
                      std::is_same_v<T, SetOperationPlan>) {
            collectRelations(op.left, relations, depth + 1);
            collectRelations(op.right, relations, depth + 1);
        } else if constexpr (std::is_same_v<T, DerivedTablePlan>) {
            addRelation(relations, op.table, op.relation_id, op.relation_name);
            collectRelations(op.input, relations, depth + 1);
        } else if constexpr (std::is_same_v<T, FilterPlan> ||
                             std::is_same_v<T, GroupByPlan> ||
                             std::is_same_v<T, AggregatePlan> ||
                             std::is_same_v<T, SortPlan> ||
                             std::is_same_v<T, ProjectPlan> ||
                             std::is_same_v<T, ExplainPlan>) {
            collectRelations(op.input, relations, depth + 1);
        } else if constexpr (std::is_same_v<T, SeqScanPlan>) {
            addRelation(relations, op.table, op.relation_id, op.relation_name);
        } else if constexpr (std::is_same_v<T, IndexScanPlan>) {
            addRelation(relations, op.table, op.relation_id, op.relation_name);
        } else if constexpr (std::is_same_v<T, EmptyResultPlan>) {
            for (const auto& relation : op.relations)
                addRelation(relations, relation.table, relation.relation_id,
                            relation.relation_name);
        } else if constexpr (!std::is_same_v<T, CreateTablePlan> &&
                             !std::is_same_v<T, CreateIndexPlan> &&
                             !std::is_same_v<T, DropTablePlan> &&
                             !std::is_same_v<T, DropIndexPlan> &&
                             !std::is_same_v<T, AlterTablePlan> &&
                             !std::is_same_v<T, EmptyResultPlan>) {
            addRelation(relations, op.table, 0, op.table ? op.table->name : std::string{});
        }
    }, plan->node);
}

std::string columnName(const BoundColumnRef& ref, const Relations& relations) {
    for (const auto& relation : relations) {
        const auto& table = relation.table;
        if (ref.relation_id == relation.id && ref.table_id.value == table->id.value &&
            ref.ordinal < table->columns.size() &&
            ref.column_id.value == table->columns[ref.ordinal].id.value) {
            return relation.name + "." + table->columns[ref.ordinal].name;
        }
    }
    return "table#" + std::to_string(ref.table_id.value) + ".column#" + std::to_string(ref.column_id.value);
}

std::string expression(const BoundExprPtr& expr, const Relations& relations,
                       std::size_t depth = 0) {
    if (!expr) return "<missing-expression>";
    if (depth >= 256) return "<depth-limit>";
    return std::visit([&](const auto& node) -> std::string {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, BoundColumnRef>) return columnName(node, relations);
        else if constexpr (std::is_same_v<T, BoundLiteral>) return literal(node.value);
        else if constexpr (std::is_same_v<T, BoundUnary>) {
            if (node.op == UnaryOp::IsNull || node.op == UnaryOp::IsNotNull)
                return "(" + expression(node.operand, relations, depth + 1) +
                    (node.op == UnaryOp::IsNull ? " IS NULL)" : " IS NOT NULL)");
            return std::string("(") + (node.op == UnaryOp::Not ? "NOT " : "-") +
                   expression(node.operand, relations, depth + 1) + ")";
        } else if constexpr (std::is_same_v<T, BoundBinary>)
            return "(" + expression(node.left, relations, depth + 1) + " " + opName(node.op) + " " +
                   expression(node.right, relations, depth + 1) + ")";
        else if constexpr (std::is_same_v<T, BoundAggregate>) {
            std::string result = aggregateName(node.kind);
            result += '(';
            result += node.argument ? columnName(*node.argument, relations) : "*";
            return result + ')';
        } else if constexpr (std::is_same_v<T, BoundCase>) {
            std::string result = "CASE";
            if (node.operand) result += " " + expression(node.operand, relations, depth + 1);
            for (const auto& branch : node.branches)
                result += " WHEN " + expression(branch.condition, relations, depth + 1) +
                          " THEN " + expression(branch.result, relations, depth + 1);
            if (node.else_result)
                result += " ELSE " + expression(node.else_result, relations, depth + 1);
            return result + " END";
        } else if constexpr (std::is_same_v<T, BoundInSubquery>) {
            return "(" + expression(node.value, relations, depth + 1) +
                   (node.negated ? " NOT IN (SUBQUERY))" : " IN (SUBQUERY))");
        } else if constexpr (std::is_same_v<T, BoundExistsSubquery>) {
            return node.negated ? "NOT EXISTS (SUBQUERY)" : "EXISTS (SUBQUERY)";
        } else {
            return "(SCALAR SUBQUERY)";
        }
    }, expr->node);
}

void printNode(std::ostream& out, const PlanPtr& plan, std::size_t depth) {
    out << std::string(depth * 2, ' ');
    if (!plan) { out << "<missing-plan>\n"; return; }
    if (depth >= 256) { out << "<depth-limit>\n"; return; }
    Relations relations;
    collectRelations(plan, relations);
    PlanPtr input;
    PlanPtr left;
    PlanPtr right;
    std::visit([&](const auto& op) {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, CreateTablePlan>) {
            out << "CreateTable[" << op.table_name << "; ";
            if (op.if_not_exists) out << "IF NOT EXISTS; ";
            for (std::size_t i = 0; i < op.columns.size(); ++i) {
                if (i) out << ", ";
                const auto& column = op.columns[i];
                out << column.name << ':' << typeName(column.type);
                if (column.varchar_length) out << '(' << *column.varchar_length << ')';
                if (column.primary_key) out << " PRIMARY KEY";
                else {
                    if (column.not_null) out << " NOT NULL";
                    if (column.unique) out << " UNIQUE";
                }
                if (column.default_value) out << " DEFAULT " << literal(*column.default_value);
            }
            for (const auto& constraint : op.table_constraints) {
                out << ", " << (constraint.primary_key ? "PRIMARY KEY(" : "UNIQUE(");
                for (std::size_t i = 0; i < constraint.columns.size(); ++i) {
                    if (i) out << ", ";
                    out << op.columns[constraint.columns[i]].name;
                }
                out << ')';
            }
        } else if constexpr (std::is_same_v<T, CreateIndexPlan>) {
            out << "CreateIndex[" << op.index_name << " ON " << tableName(op.table)
                << "." << columnName(op.column, relations) << "; "
                << typeName(op.key_type);
            if (op.unique) out << " UNIQUE";
        } else if constexpr (std::is_same_v<T, AlterTablePlan>) {
            out << "AlterTable[" << tableName(op.table) << "; ";
            std::visit([&](const auto& action) {
                using A = std::decay_t<decltype(action)>;
                if constexpr (std::is_same_v<A, BoundAlterAddColumn>)
                    out << "ADD COLUMN " << action.column.name;
                else if constexpr (std::is_same_v<A, BoundAlterDropColumn>)
                    out << "DROP COLUMN " << action.column_name;
                else if constexpr (std::is_same_v<A, BoundAlterRenameTable>)
                    out << "RENAME TO " << action.new_name;
                else out << "RENAME COLUMN #" << action.ordinal << " TO " << action.new_name;
            }, op.action);
        } else if constexpr (std::is_same_v<T, DropTablePlan>) {
            out << "DropTable[";
            if (op.if_exists) out << "IF EXISTS ";
            for (std::size_t i = 0; i < op.table_names.size(); ++i) {
                if (i) out << ", ";
                out << op.table_names[i];
            }
        } else if constexpr (std::is_same_v<T, DropIndexPlan>) {
            out << "DropIndex[";
            if (op.if_exists) out << "IF EXISTS ";
            out << op.index_name;
            if (op.index) out << "#" << op.index->id.value;
        } else if constexpr (std::is_same_v<T, InsertPlan>) {
            const auto rows = op.rows.empty()
                ? std::vector<std::vector<ScalarValue>>{op.values} : op.rows;
            out << "Insert[" << tableName(op.table) << "; rows=";
            for (std::size_t row = 0; row < rows.size(); ++row) {
                if (row) out << ", ";
                out << '(';
                for (std::size_t i = 0; i < rows[row].size(); ++i) {
                    if (i) out << ", ";
                    out << literal(rows[row][i]);
                }
                out << ')';
            }
        } else if constexpr (std::is_same_v<T, SeqScanPlan>) {
            out << "SeqScan[" << tableName(op.table);
            if (op.table && !op.relation_name.empty() && op.relation_name != op.table->name)
                out << " AS " << op.relation_name;
            if (op.columns) {
                out << "; columns=";
                if (op.columns->empty()) out << "<none>";
                for (std::size_t i = 0; i < op.columns->size(); ++i) {
                    if (i) out << ", ";
                    out << columnName((*op.columns)[i], relations);
                }
            }
        } else if constexpr (std::is_same_v<T, IndexScanPlan>) {
            out << "IndexScan[" << (op.index ? op.index->name : std::string{"<missing-index>"})
                << " ON " << tableName(op.table);
            if (op.table && !op.relation_name.empty() && op.relation_name != op.table->name)
                out << " AS " << op.relation_name;
            out << "; range=";
            if (op.lower) out << (op.lower->inclusive ? "[" : "(") << op.lower->value;
            else out << "(-inf";
            out << ", ";
            if (op.upper) out << op.upper->value << (op.upper->inclusive ? "]" : ")");
            else out << "+inf)";
            if (op.columns) {
                out << "; columns=";
                if (op.columns->empty()) out << "<none>";
                for (std::size_t i = 0; i < op.columns->size(); ++i) {
                    if (i) out << ", ";
                    out << columnName((*op.columns)[i], relations);
                }
            }
        } else if constexpr (std::is_same_v<T, EmptyResultPlan>) {
            out << "EmptyResult[columns=";
            if (op.columns.empty()) out << "<none>";
            for (std::size_t i = 0; i < op.columns.size(); ++i) {
                if (i) out << ", ";
                out << columnName(op.columns[i], relations);
            }
        } else if constexpr (std::is_same_v<T, DerivedTablePlan>) {
            input = op.input;
            out << "DerivedTable[" << op.relation_name;
        } else if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            left = op.left;
            right = op.right;
            out << "NestedLoopJoin[" << joinName(op.type) << "; "
                << expression(op.predicate, relations);
        } else if constexpr (std::is_same_v<T, SetOperationPlan>) {
            left = op.left;
            right = op.right;
            out << "SetOperation[" << setName(op.op);
            if (op.all) out << " ALL";
        } else {
            input = op.input;
            if constexpr (std::is_same_v<T, ExplainPlan>) {
                out << (op.analyze ? "Explain[ANALYZE" : "Explain[");
            } else if constexpr (std::is_same_v<T, FilterPlan>) {
                out << "Filter[" << expression(op.predicate, relations);
            } else if constexpr (std::is_same_v<T, GroupByPlan>) {
                out << "GroupBy[";
                for (std::size_t i = 0; i < op.keys.size(); ++i) {
                    if (i) out << ", ";
                    out << columnName(op.keys[i], relations);
                }
            } else if constexpr (std::is_same_v<T, AggregatePlan>) {
                out << "Aggregate[group=";
                if (op.group_keys.empty()) out << "<all>";
                for (std::size_t i = 0; i < op.group_keys.size(); ++i) {
                    if (i) out << ", ";
                    out << columnName(op.group_keys[i], relations);
                }
                out << "; items=";
                for (std::size_t i = 0; i < op.items.size(); ++i) {
                    if (i) out << ", ";
                    std::visit([&](const auto& item) {
                        using I = std::decay_t<decltype(item)>;
                        if constexpr (std::is_same_v<I, BoundColumnRef>) {
                            out << columnName(item, relations);
                        } else if constexpr (std::is_same_v<I, BoundAggregate>) {
                            out << aggregateName(item.kind) << '(';
                            if (item.argument) out << columnName(*item.argument, relations);
                            else out << '*';
                            out << ')';
                        } else out << expression(item, relations);
                    }, op.items[i].value);
                }
                if (!op.order_by.empty()) out << "; order=";
                for (std::size_t i = 0; i < op.order_by.size(); ++i) {
                    if (i) out << ", ";
                    if (const auto* ordinal = std::get_if<std::size_t>(&op.order_by[i].key))
                        out << "output#" << *ordinal;
                    else if (const auto* ref = std::get_if<BoundColumnRef>(&op.order_by[i].key))
                        out << columnName(*ref, relations);
                    else out << expression(std::get<BoundExprPtr>(op.order_by[i].key), relations);
                    out << ' ' << (op.order_by[i].direction == SortDirection::Asc ? "ASC" : "DESC");
                }
                if (op.having) out << "; having=" << expression(op.having, relations);
                if (op.distinct) out << "; DISTINCT";
                if (op.limit) out << "; limit=" << *op.limit;
                if (op.offset) out << "; offset=" << op.offset;
            } else if constexpr (std::is_same_v<T, SortPlan>) {
                out << "Sort[";
                for (std::size_t i = 0; i < op.items.size(); ++i) {
                    if (i) out << ", ";
                    out << columnName(op.items[i].column, relations) << ' '
                        << (op.items[i].direction == SortDirection::Asc ? "ASC" : "DESC");
                }
                for (std::size_t i = 0; i < op.expression_items.size(); ++i) {
                    if (i || !op.items.empty()) out << ", ";
                    const auto& item = op.expression_items[i];
                    if (const auto* ref = std::get_if<BoundColumnRef>(&item.key))
                        out << columnName(*ref, relations);
                    else out << expression(std::get<BoundExprPtr>(item.key), relations);
                    out << ' ' << (item.direction == SortDirection::Asc ? "ASC" : "DESC");
                }
            } else if constexpr (std::is_same_v<T, ProjectPlan>) {
                out << "Project[";
                const auto count = op.expressions.empty() ? op.columns.size() : op.expressions.size();
                for (std::size_t i = 0; i < count; ++i) {
                    if (i) out << ", ";
                    if (op.expressions.empty()) out << columnName(op.columns[i], relations);
                    else out << expression(op.expressions[i], relations);
                }
                if (op.distinct) out << "; DISTINCT";
                if (op.limit) out << "; limit=" << *op.limit;
                if (op.offset) out << "; offset=" << op.offset;
            } else if constexpr (std::is_same_v<T, UpdatePlan>) {
                out << "Update[" << tableName(op.table) << "; ";
                for (std::size_t i = 0; i < op.assignments.size(); ++i) {
                    if (i) out << ", ";
                    out << columnName(op.assignments[i].target, relations) << " = "
                        << expression(op.assignments[i].value, relations);
                }
                out << "; values=old-row";
            } else if constexpr (std::is_same_v<T, DeletePlan>) {
                out << "Delete[" << tableName(op.table);
            }
        }
    }, plan->node);
    out << "] output=[";
    for (std::size_t i = 0; i < plan->output.size(); ++i) {
        if (i) out << ", ";
        out << plan->output[i].name << ':' << typeName(plan->output[i].type);
    }
    out << "] row_id=" << (plan->carries_row_id ? "yes" : "no") << '\n';
    if (input) printNode(out, input, depth + 1);
    if (left) printNode(out, left, depth + 1);
    if (right) printNode(out, right, depth + 1);
}
} // namespace

std::string formatPlan(const LogicalPlan& plan) {
    std::ostringstream out;
    out.imbue(std::locale::classic()); // 输出不受调用方的数字分组 locale 影响。
    out << "CatalogVersion: " << plan.catalog_version << '\n';
    printNode(out, plan.root, 0);
    return out.str();
}

} // namespace minisql
