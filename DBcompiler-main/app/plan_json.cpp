// 跨语言边界：将 C++ LogicalPlan 导出为 JSON，供 Java 数据库引擎读取。
#include "minisql/compiler.hpp"
#include "minisql/lexer.hpp"
#include "minisql/memory_catalog.hpp"
#include "minisql/optimizer.hpp"
#include "minisql/parser.hpp"

#include <iostream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <type_traits>

namespace {
using namespace minisql;

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

const char* unaryName(UnaryOp op) { return op == UnaryOp::Not ? "Not" : "Negate"; }

const char* binaryName(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "Add";
    case BinaryOp::Subtract: return "Subtract";
    case BinaryOp::Multiply: return "Multiply";
    case BinaryOp::Divide: return "Divide";
    case BinaryOp::Equal: return "Equal";
    case BinaryOp::NotEqual: return "NotEqual";
    case BinaryOp::Less: return "Less";
    case BinaryOp::LessEqual: return "LessEqual";
    case BinaryOp::Greater: return "Greater";
    case BinaryOp::GreaterEqual: return "GreaterEqual";
    case BinaryOp::And: return "And";
    case BinaryOp::Or: return "Or";
    }
    return "Unknown";
}

void stringJson(std::ostream& out, const std::string& value) {
    out << '"';
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) out << "\\u00" << hex[ch >> 4] << hex[ch & 0x0f];
            else out << static_cast<char>(ch);
        }
    }
    out << '"';
}

void scalarJson(std::ostream& out, const ScalarValue& value) {
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::string>) stringJson(out, item);
        else if constexpr (std::is_same_v<T, bool>) out << (item ? "true" : "false");
        else if constexpr (std::is_same_v<T, NullValue>) out << "null";
        else if constexpr (std::is_same_v<T, double>)
            out << std::setprecision(std::numeric_limits<double>::max_digits10) << item;
        else out << item;
    }, value);
}

void spanJson(std::ostream& out, const SourceLocation& span) {
    if (!span) { out << "null"; return; }
    out << "{\"begin\":{\"offset\":" << span->begin.offset << ",\"line\":" << span->begin.line
        << ",\"column\":" << span->begin.column << "},\"end\":{\"offset\":" << span->end.offset
        << ",\"line\":" << span->end.line << ",\"column\":" << span->end.column << "}}";
}

void tableJson(std::ostream& out, const std::shared_ptr<const TableSchema>& table) {
    if (!table) { out << "null"; return; }
    out << "{\"id\":" << table->id.value << ",\"name\":";
    stringJson(out, table->name);
    out << ",\"columns\":[";
    for (std::size_t i = 0; i < table->columns.size(); ++i) {
        if (i) out << ',';
        const auto& column = table->columns[i];
        out << "{\"id\":" << column.id.value << ",\"name\":";
        stringJson(out, column.name);
        out << ",\"type\":";
        stringJson(out, typeName(column.type));
        out << '}';
    }
    out << "]}";
}

void refJson(std::ostream& out, const BoundColumnRef& ref) {
    out << "{\"tableId\":" << ref.table_id.value << ",\"columnId\":" << ref.column_id.value
        << ",\"relationId\":" << ref.relation_id
        << ",\"ordinal\":" << ref.ordinal << ",\"type\":";
    stringJson(out, typeName(ref.type));
    out << '}';
}

void exprJson(std::ostream& out, const BoundExprPtr& expr, std::size_t depth = 0) {
    if (!expr || depth >= 256) { out << "null"; return; }
    out << "{\"type\":";
    stringJson(out, typeName(expr->type));
    out << ",\"span\":";
    spanJson(out, expr->span);
    std::visit([&](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, BoundColumnRef>) {
            out << ",\"kind\":\"column\",\"column\":";
            refJson(out, node);
        } else if constexpr (std::is_same_v<T, BoundLiteral>) {
            out << ",\"kind\":\"literal\",\"value\":";
            scalarJson(out, node.value);
        } else if constexpr (std::is_same_v<T, BoundUnary>) {
            out << ",\"kind\":\"unary\",\"op\":";
            stringJson(out, unaryName(node.op));
            out << ",\"operand\":";
            exprJson(out, node.operand, depth + 1);
        } else {
            out << ",\"kind\":\"binary\",\"op\":";
            stringJson(out, binaryName(node.op));
            out << ",\"left\":";
            exprJson(out, node.left, depth + 1);
            out << ",\"right\":";
            exprJson(out, node.right, depth + 1);
        }
    }, expr->node);
    out << '}';
}

void outputJson(std::ostream& out, const std::vector<OutputColumn>& output) {
    out << '[';
    for (std::size_t i = 0; i < output.size(); ++i) {
        if (i) out << ',';
        out << "{\"name\":";
        stringJson(out, output[i].name);
        out << ",\"type\":";
        stringJson(out, typeName(output[i].type));
        out << '}';
    }
    out << ']';
}

void nodeJson(std::ostream& out, const PlanPtr& plan, std::size_t depth = 0) {
    if (!plan || depth >= 256) { out << "null"; return; }
    out << '{';
    std::visit([&](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, CreateTablePlan>) {
            out << "\"type\":\"CreateTable\",\"tableName\":";
            stringJson(out, node.table_name);
            out << ",\"columns\":[";
            for (std::size_t i = 0; i < node.columns.size(); ++i) {
                if (i) out << ',';
                out << "{\"name\":";
                stringJson(out, node.columns[i].name);
                out << ",\"type\":";
                stringJson(out, typeName(node.columns[i].type));
                out << '}';
            }
            out << ']';
        } else if constexpr (std::is_same_v<T, InsertPlan>) {
            out << "\"type\":\"Insert\",\"table\":";
            tableJson(out, node.table);
            out << ",\"values\":[";
            for (std::size_t i = 0; i < node.values.size(); ++i) { if (i) out << ','; scalarJson(out, node.values[i]); }
            out << ']';
        } else if constexpr (std::is_same_v<T, SeqScanPlan>) {
            out << "\"type\":\"SeqScan\",\"table\":";
            tableJson(out, node.table);
            out << ",\"relationId\":" << node.relation_id << ",\"relationName\":";
            stringJson(out, node.relation_name);
        } else if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            out << "\"type\":\"NestedLoopJoin\",\"predicate\":";
            exprJson(out, node.predicate, depth + 1);
            out << ",\"left\":";
            nodeJson(out, node.left, depth + 1);
            out << ",\"right\":";
            nodeJson(out, node.right, depth + 1);
        } else if constexpr (std::is_same_v<T, FilterPlan>) {
            out << "\"type\":\"Filter\",\"predicate\":";
            exprJson(out, node.predicate, depth + 1);
            out << ",\"input\":";
            nodeJson(out, node.input, depth + 1);
        } else if constexpr (std::is_same_v<T, GroupByPlan>) {
            out << "\"type\":\"GroupBy\",\"keys\":[";
            for (std::size_t i = 0; i < node.keys.size(); ++i) {
                if (i) out << ',';
                refJson(out, node.keys[i]);
            }
            out << "],\"input\":";
            nodeJson(out, node.input, depth + 1);
        } else if constexpr (std::is_same_v<T, SortPlan>) {
            out << "\"type\":\"Sort\",\"items\":[";
            for (std::size_t i = 0; i < node.items.size(); ++i) {
                if (i) out << ',';
                out << "{\"column\":";
                refJson(out, node.items[i].column);
                out << ",\"direction\":\""
                    << (node.items[i].direction == SortDirection::Asc ? "ASC" : "DESC")
                    << "\"}";
            }
            out << "],\"input\":";
            nodeJson(out, node.input, depth + 1);
        } else if constexpr (std::is_same_v<T, ProjectPlan>) {
            out << "\"type\":\"Project\",\"columns\":[";
            for (std::size_t i = 0; i < node.columns.size(); ++i) { if (i) out << ','; refJson(out, node.columns[i]); }
            out << "],\"input\":";
            nodeJson(out, node.input, depth + 1);
        } else if constexpr (std::is_same_v<T, UpdatePlan>) {
            out << "\"type\":\"Update\",\"table\":";
            tableJson(out, node.table);
            out << ",\"assignments\":[";
            for (std::size_t i = 0; i < node.assignments.size(); ++i) {
                if (i) out << ',';
                out << "{\"target\":";
                refJson(out, node.assignments[i].target);
                out << ",\"value\":";
                exprJson(out, node.assignments[i].value, depth + 1);
                out << '}';
            }
            out << "],\"input\":";
            nodeJson(out, node.input, depth + 1);
        } else {
            out << "\"type\":\"Delete\",\"table\":";
            tableJson(out, node.table);
            out << ",\"input\":";
            nodeJson(out, node.input, depth + 1);
        }
    }, plan->node);
    out << ",\"output\":";
    outputJson(out, plan->output);
    out << ",\"carriesRowId\":" << (plan->carries_row_id ? "true" : "false") << '}';
}

void printDiagnostic(const Diagnostic& error) {
    std::cerr << "Compilation failed: " << error.message;
    if (error.span) std::cerr << " (line " << error.span->begin.line << ", column " << error.span->begin.column << ')';
    std::cerr << '\n';
}
} // namespace

int main() {
    // JSON 数字必须使用点号，不受操作系统区域设置影响。
    std::cout.imbue(std::locale::classic());
    std::ostringstream input;
    input << std::cin.rdbuf();
    auto tokens = lex(input.str());
    if (const auto* error = std::get_if<Diagnostic>(&tokens)) { printDiagnostic(*error); return 1; }
    auto statements = parse(std::get<TokenStream>(tokens));
    if (const auto* error = std::get_if<Diagnostic>(&statements)) { printDiagnostic(*error); return 1; }
    MemoryCatalog catalog;
    std::cout << "{\"protocolVersion\":1,\"plans\":[";
    bool first = true;
    for (const Statement& statement : std::get<std::vector<Statement>>(statements)) {
        auto bound = analyze(statement, *catalog.snapshot());
        if (const auto* error = std::get_if<Diagnostic>(&bound)) { printDiagnostic(*error); return 1; }
        auto plan = buildPlan(std::get<BoundStatement>(bound));
        if (const auto* error = std::get_if<Diagnostic>(&plan)) { printDiagnostic(*error); return 1; }
        auto optimized = optimizePlan(std::get<LogicalPlan>(plan));
        if (const auto* error = std::get_if<Diagnostic>(&optimized)) { printDiagnostic(*error); return 1; }
        const LogicalPlan& logical_plan = std::get<LogicalPlan>(optimized);
        std::ostringstream root;
        root.imbue(std::locale::classic());
        nodeJson(root, logical_plan.root);
        if (!first) std::cout << ',';
        std::cout << "{\"catalogVersion\":" << logical_plan.catalog_version << ",\"root\":" << root.str() << '}';
        first = false;
        if (const auto* create = std::get_if<BoundCreateTable>(&std::get<BoundStatement>(bound).node)) {
            auto registered = catalog.createTable(create->table_name, create->columns);
            if (const auto* error = std::get_if<Diagnostic>(&registered)) { printDiagnostic(*error); return 1; }
        }
    }
    std::cout << "]}" << '\n';
    return 0;
}
