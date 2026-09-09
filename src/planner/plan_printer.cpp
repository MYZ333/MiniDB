// 文本展示与执行解耦：递归打印实际计划节点，保留算子顺序、表达式和行标识属性。
#include "minisql/plan_printer.hpp"

#include <locale>
#include <sstream>
#include <type_traits>

namespace minisql {
namespace {
const char* typeName(DataType type) {
    switch (type) {
    case DataType::Int: return "INT";
    case DataType::Varchar: return "VARCHAR";
    case DataType::Bool: return "BOOL";
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
        } else {
            return std::to_string(item);
        }
    }, value);
}

std::string tableName(const std::shared_ptr<const TableSchema>& table) {
    return table ? table->name + "#" + std::to_string(table->id.value) : "<missing-table>";
}

// 单表计划的模式已经存在于算子中，打印器无需重新查询 Catalog。
std::shared_ptr<const TableSchema> sourceTable(const PlanPtr& plan, std::size_t depth = 0) {
    if (!plan || depth >= 256) return nullptr;
    return std::visit([&](const auto& op) -> std::shared_ptr<const TableSchema> {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, CreateTablePlan>) return nullptr;
        else if constexpr (std::is_same_v<T, FilterPlan> || std::is_same_v<T, ProjectPlan>)
            return sourceTable(op.input, depth + 1);
        else return op.table;
    }, plan->node);
}

std::string columnName(const BoundColumnRef& ref, const std::shared_ptr<const TableSchema>& table) {
    if (table && ref.table_id.value == table->id.value && ref.ordinal < table->columns.size() &&
        ref.column_id.value == table->columns[ref.ordinal].id.value) {
        return table->name + "." + table->columns[ref.ordinal].name;
    }
    return "table#" + std::to_string(ref.table_id.value) + ".column#" + std::to_string(ref.column_id.value);
}

std::string expression(const BoundExprPtr& expr, const std::shared_ptr<const TableSchema>& table,
                       std::size_t depth = 0) {
    if (!expr) return "<missing-expression>";
    if (depth >= 256) return "<depth-limit>";
    return std::visit([&](const auto& node) -> std::string {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, BoundColumnRef>) return columnName(node, table);
        else if constexpr (std::is_same_v<T, BoundLiteral>) return literal(node.value);
        else if constexpr (std::is_same_v<T, BoundUnary>)
            return std::string("(") + (node.op == UnaryOp::Not ? "NOT " : "-") +
                   expression(node.operand, table, depth + 1) + ")";
        else
            return "(" + expression(node.left, table, depth + 1) + " " + opName(node.op) + " " +
                   expression(node.right, table, depth + 1) + ")";
    }, expr->node);
}

void printNode(std::ostream& out, const PlanPtr& plan, std::size_t depth) {
    out << std::string(depth * 2, ' ');
    if (!plan) { out << "<missing-plan>\n"; return; }
    if (depth >= 256) { out << "<depth-limit>\n"; return; }
    const auto table = sourceTable(plan);
    PlanPtr input;
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
        } else {
            input = op.input;
            if constexpr (std::is_same_v<T, FilterPlan>) {
                out << "Filter[" << expression(op.predicate, table);
            } else if constexpr (std::is_same_v<T, ProjectPlan>) {
                out << "Project[";
                for (std::size_t i = 0; i < op.columns.size(); ++i) {
                    if (i) out << ", ";
                    out << columnName(op.columns[i], table);
                }
            } else if constexpr (std::is_same_v<T, UpdatePlan>) {
                out << "Update[" << tableName(op.table) << "; ";
                for (std::size_t i = 0; i < op.assignments.size(); ++i) {
                    if (i) out << ", ";
                    out << columnName(op.assignments[i].target, table) << " = "
                        << expression(op.assignments[i].value, table);
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
