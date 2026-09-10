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

using Tables = std::vector<std::shared_ptr<const TableSchema>>;

void addTable(Tables& tables, const std::shared_ptr<const TableSchema>& table) {
    if (!table) return;
    for (const auto& present : tables) if (present->id.value == table->id.value) return;
    tables.push_back(table);
}

// 从计划自身收集模式，不回查 Catalog；JOIN 的左右两棵子树都要遍历。
void collectTables(const PlanPtr& plan, Tables& tables, std::size_t depth = 0) {
    if (!plan || depth >= 256) return;
    std::visit([&](const auto& op) {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            collectTables(op.left, tables, depth + 1);
            collectTables(op.right, tables, depth + 1);
        } else if constexpr (std::is_same_v<T, FilterPlan> ||
                             std::is_same_v<T, GroupByPlan> ||
                             std::is_same_v<T, SortPlan> ||
                             std::is_same_v<T, ProjectPlan>) {
            collectTables(op.input, tables, depth + 1);
        } else if constexpr (!std::is_same_v<T, CreateTablePlan>) addTable(tables, op.table);
    }, plan->node);
}

std::string columnName(const BoundColumnRef& ref, const Tables& tables) {
    for (const auto& table : tables) {
        if (ref.table_id.value == table->id.value && ref.ordinal < table->columns.size() &&
            ref.column_id.value == table->columns[ref.ordinal].id.value) {
            return table->name + "." + table->columns[ref.ordinal].name;
        }
    }
    return "table#" + std::to_string(ref.table_id.value) + ".column#" + std::to_string(ref.column_id.value);
}

std::string expression(const BoundExprPtr& expr, const Tables& tables,
                       std::size_t depth = 0) {
    if (!expr) return "<missing-expression>";
    if (depth >= 256) return "<depth-limit>";
    return std::visit([&](const auto& node) -> std::string {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, BoundColumnRef>) return columnName(node, tables);
        else if constexpr (std::is_same_v<T, BoundLiteral>) return literal(node.value);
        else if constexpr (std::is_same_v<T, BoundUnary>)
            return std::string("(") + (node.op == UnaryOp::Not ? "NOT " : "-") +
                   expression(node.operand, tables, depth + 1) + ")";
        else
            return "(" + expression(node.left, tables, depth + 1) + " " + opName(node.op) + " " +
                   expression(node.right, tables, depth + 1) + ")";
    }, expr->node);
}

void printNode(std::ostream& out, const PlanPtr& plan, std::size_t depth) {
    out << std::string(depth * 2, ' ');
    if (!plan) { out << "<missing-plan>\n"; return; }
    if (depth >= 256) { out << "<depth-limit>\n"; return; }
    Tables tables;
    collectTables(plan, tables);
    PlanPtr input;
    PlanPtr left;
    PlanPtr right;
    std::visit([&](const auto& op) {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, CreateTablePlan>) {
            out << "CreateTable[" << op.table_name << "; ";
            for (std::size_t i = 0; i < op.columns.size(); ++i) {
                if (i) out << ", ";
                out << op.columns[i].name << ':' << typeName(op.columns[i].type);
            }
        } else if constexpr (std::is_same_v<T, InsertPlan>) {
            out << "Insert[" << tableName(op.table) << "; values=(";
            for (std::size_t i = 0; i < op.values.size(); ++i) {
                if (i) out << ", ";
                out << literal(op.values[i]);
            }
            out << ')';
        } else if constexpr (std::is_same_v<T, SeqScanPlan>) {
            out << "SeqScan[" << tableName(op.table);
        } else if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            left = op.left;
            right = op.right;
            out << "NestedLoopJoin[" << expression(op.predicate, tables);
        } else {
            input = op.input;
            if constexpr (std::is_same_v<T, FilterPlan>) {
                out << "Filter[" << expression(op.predicate, tables);
            } else if constexpr (std::is_same_v<T, GroupByPlan>) {
                out << "GroupBy[";
                for (std::size_t i = 0; i < op.keys.size(); ++i) {
                    if (i) out << ", ";
                    out << columnName(op.keys[i], tables);
                }
            } else if constexpr (std::is_same_v<T, SortPlan>) {
                out << "Sort[";
                for (std::size_t i = 0; i < op.items.size(); ++i) {
                    if (i) out << ", ";
                    out << columnName(op.items[i].column, tables) << ' '
                        << (op.items[i].direction == SortDirection::Asc ? "ASC" : "DESC");
                }
            } else if constexpr (std::is_same_v<T, ProjectPlan>) {
                out << "Project[";
                for (std::size_t i = 0; i < op.columns.size(); ++i) {
                    if (i) out << ", ";
                    out << columnName(op.columns[i], tables);
                }
            } else if constexpr (std::is_same_v<T, UpdatePlan>) {
                out << "Update[" << tableName(op.table) << "; ";
                for (std::size_t i = 0; i < op.assignments.size(); ++i) {
                    if (i) out << ", ";
                    out << columnName(op.assignments[i].target, tables) << " = "
                        << expression(op.assignments[i].value, tables);
                }
                out << "; values=old-row";
            } else {
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
