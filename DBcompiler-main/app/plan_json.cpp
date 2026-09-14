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

const char* unaryName(UnaryOp op) {
    switch (op) {
    case UnaryOp::Negate: return "Negate";
    case UnaryOp::Not: return "Not";
    case UnaryOp::IsNull: return "IsNull";
    case UnaryOp::IsNotNull: return "IsNotNull";
    }
    return "Unknown";
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
    case BinaryOp::Like: return "Like";
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
        out << ",\"varcharLength\":";
        if (column.varchar_length) out << *column.varchar_length; else out << "null";
        out << ",\"primaryKey\":" << (column.primary_key ? "true" : "false")
            << ",\"notNull\":" << (column.not_null ? "true" : "false")
            << ",\"unique\":" << (column.unique ? "true" : "false")
            << ",\"defaultValue\":";
        if (column.default_value) scalarJson(out, *column.default_value); else out << "null";
        out << ",\"hasDefault\":" << (column.default_value ? "true" : "false");
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
        } else if constexpr (std::is_same_v<T, BoundBinary>) {
            out << ",\"kind\":\"binary\",\"op\":";
            stringJson(out, binaryName(node.op));
            out << ",\"left\":";
            exprJson(out, node.left, depth + 1);
            out << ",\"right\":";
            exprJson(out, node.right, depth + 1);
        } else {
            out << ",\"kind\":\"aggregate\",\"function\":";
            stringJson(out, aggregateName(node.kind));
            out << ",\"argument\":";
            if (node.argument) refJson(out, *node.argument); else out << "null";
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
                out << ",\"varcharLength\":";
                if (node.columns[i].varchar_length) out << *node.columns[i].varchar_length;
                else out << "null";
                out << ",\"primaryKey\":" << (node.columns[i].primary_key ? "true" : "false")
                    << ",\"notNull\":" << (node.columns[i].not_null ? "true" : "false")
                    << ",\"unique\":" << (node.columns[i].unique ? "true" : "false")
                    << ",\"defaultValue\":";
                if (node.columns[i].default_value) scalarJson(out, *node.columns[i].default_value);
                else out << "null";
                out << ",\"hasDefault\":" << (node.columns[i].default_value ? "true" : "false");
                out << '}';
            }
            out << ']';
        } else if constexpr (std::is_same_v<T, DropTablePlan>) {
            out << "\"type\":\"DropTable\",\"tableNames\":[";
            for (std::size_t i = 0; i < node.table_names.size(); ++i) {
                if (i) out << ',';
                stringJson(out, node.table_names[i]);
            }
            out << "],\"ifExists\":" << (node.if_exists ? "true" : "false");
        } else if constexpr (std::is_same_v<T, InsertPlan>) {
            out << "\"type\":\"Insert\",\"table\":";
            tableJson(out, node.table);
            out << ",\"values\":[";
            for (std::size_t i = 0; i < node.values.size(); ++i) { if (i) out << ','; scalarJson(out, node.values[i]); }
            out << "],\"rows\":[";
            const auto rows = node.rows.empty()
                ? std::vector<std::vector<ScalarValue>>{node.values} : node.rows;
            for (std::size_t row = 0; row < rows.size(); ++row) {
                if (row) out << ',';
                out << '[';
                for (std::size_t i = 0; i < rows[row].size(); ++i) {
                    if (i) out << ',';
                    scalarJson(out, rows[row][i]);
                }
                out << ']';
            }
            out << ']';
        } else if constexpr (std::is_same_v<T, SeqScanPlan>) {
            out << "\"type\":\"SeqScan\",\"table\":";
            tableJson(out, node.table);
            out << ",\"relationId\":" << node.relation_id << ",\"relationName\":";
            stringJson(out, node.relation_name);
            out << ",\"columns\":";
            if (!node.columns) out << "null";
            else {
                out << '[';
                for (std::size_t i = 0; i < node.columns->size(); ++i) {
                    if (i) out << ',';
                    refJson(out, (*node.columns)[i]);
                }
                out << ']';
            }
        } else if constexpr (std::is_same_v<T, EmptyResultPlan>) {
            out << "\"type\":\"EmptyResult\",\"columns\":[";
            for (std::size_t i = 0; i < node.columns.size(); ++i) {
                if (i) out << ',';
                refJson(out, node.columns[i]);
            }
            out << "],\"relations\":[";
            for (std::size_t i = 0; i < node.relations.size(); ++i) {
                if (i) out << ',';
                out << "{\"table\":";
                tableJson(out, node.relations[i].table);
                out << ",\"relationId\":" << node.relations[i].relation_id
                    << ",\"relationName\":";
                stringJson(out, node.relations[i].relation_name);
                out << '}';
            }
            out << ']';
        } else if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            out << "\"type\":\"NestedLoopJoin\",\"predicate\":";
            exprJson(out, node.predicate, depth + 1);
            out << ",\"joinType\":";
            stringJson(out, joinName(node.type));
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
        } else if constexpr (std::is_same_v<T, AggregatePlan>) {
            out << "\"type\":\"Aggregate\",\"groupKeys\":[";
            for (std::size_t i = 0; i < node.group_keys.size(); ++i) {
                if (i) out << ',';
                refJson(out, node.group_keys[i]);
            }
            out << "],\"items\":[";
            for (std::size_t i = 0; i < node.items.size(); ++i) {
                if (i) out << ',';
                std::visit([&](const auto& item) {
                    using I = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<I, BoundColumnRef>) {
                        out << "{\"kind\":\"column\",\"column\":";
                        refJson(out, item);
                        out << '}';
                    } else if constexpr (std::is_same_v<I, BoundAggregate>) {
                        out << "{\"kind\":\"aggregate\",\"function\":";
                        stringJson(out, aggregateName(item.kind));
                        out << ",\"argument\":";
                        if (item.argument) refJson(out, *item.argument); else out << "null";
                        out << ",\"type\":";
                        stringJson(out, typeName(item.type));
                        out << ",\"span\":";
                        spanJson(out, item.span);
                        out << '}';
                    } else {
                        out << "{\"kind\":\"expression\",\"expression\":";
                        exprJson(out, item, depth + 1);
                        out << '}';
                    }
                }, node.items[i].value);
            }
            out << "],\"orderBy\":[";
            for (std::size_t i = 0; i < node.order_by.size(); ++i) {
                if (i) out << ',';
                out << '{';
                if (const auto* ordinal = std::get_if<std::size_t>(&node.order_by[i].key)) {
                    out << "\"kind\":\"output\",\"ordinal\":" << *ordinal;
                } else if (const auto* ref = std::get_if<BoundColumnRef>(&node.order_by[i].key)) {
                    out << "\"kind\":\"group\",\"column\":";
                    refJson(out, *ref);
                } else {
                    out << "\"kind\":\"expression\",\"expression\":";
                    exprJson(out, std::get<BoundExprPtr>(node.order_by[i].key), depth + 1);
                }
                out << ",\"direction\":\""
                    << (node.order_by[i].direction == SortDirection::Asc ? "ASC" : "DESC")
                    << "\"}";
            }
            out << "],\"having\":";
            if (node.having) exprJson(out, node.having, depth + 1); else out << "null";
            out << ",\"distinct\":" << (node.distinct ? "true" : "false")
                << ",\"limit\":";
            if (node.limit) out << *node.limit; else out << "null";
            out << ",\"offset\":" << node.offset << ",\"input\":";
            nodeJson(out, node.input, depth + 1);
        } else if constexpr (std::is_same_v<T, SortPlan>) {
            out << "\"type\":\"Sort\",\"items\":[";
            for (std::size_t i = 0; i < node.items.size(); ++i) {
                if (i) out << ',';
                out << "{\"kind\":\"column\",\"column\":";
                refJson(out, node.items[i].column);
                out << ",\"direction\":\""
                    << (node.items[i].direction == SortDirection::Asc ? "ASC" : "DESC")
                    << "\"}";
            }
            for (std::size_t i = 0; i < node.expression_items.size(); ++i) {
                if (i || !node.items.empty()) out << ',';
                const auto& item = node.expression_items[i];
                out << '{';
                if (const auto* ref = std::get_if<BoundColumnRef>(&item.key)) {
                    out << "\"kind\":\"column\",\"column\":";
                    refJson(out, *ref);
                } else {
                    out << "\"kind\":\"expression\",\"expression\":";
                    exprJson(out, std::get<BoundExprPtr>(item.key), depth + 1);
                }
                out << ",\"direction\":\""
                    << (item.direction == SortDirection::Asc ? "ASC" : "DESC") << "\"}";
            }
            out << "],\"input\":";
            nodeJson(out, node.input, depth + 1);
        } else if constexpr (std::is_same_v<T, ProjectPlan>) {
            out << "\"type\":\"Project\",\"columns\":[";
            for (std::size_t i = 0; i < node.columns.size(); ++i) { if (i) out << ','; refJson(out, node.columns[i]); }
            out << "],\"expressions\":[";
            for (std::size_t i = 0; i < node.expressions.size(); ++i) {
                if (i) out << ',';
                exprJson(out, node.expressions[i], depth + 1);
            }
            out << "],\"distinct\":" << (node.distinct ? "true" : "false")
                << ",\"limit\":";
            if (node.limit) out << *node.limit; else out << "null";
            out << ",\"offset\":" << node.offset << ",\"input\":";
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
        } else if constexpr (std::is_same_v<T, DeletePlan>) {
            out << "\"type\":\"Delete\",\"table\":";
            tableJson(out, node.table);
            out << ",\"input\":";
            nodeJson(out, node.input, depth + 1);
        } else {
            out << "\"type\":\"Explain\",\"analyze\":"
                << (node.analyze ? "true" : "false") << ",\"input\":";
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
        const BoundStatement& bound_statement = std::get<BoundStatement>(bound);
        // 编译脚本时模拟后续可见的 Catalog；只有 ANALYZE 才会执行被包装的 DDL。
        const BoundStatement* effect = &bound_statement;
        if (const auto* explain = std::get_if<BoundExplain>(&effect->node)) {
            effect = explain->analyze ? explain->target.get() : nullptr;
        }
        if (effect && std::get_if<BoundCreateTable>(&effect->node)) {
            const auto* create = std::get_if<BoundCreateTable>(&effect->node);
            auto registered = catalog.createTable(create->table_name, create->columns);
            if (const auto* error = std::get_if<Diagnostic>(&registered)) { printDiagnostic(*error); return 1; }
        } else if (effect && std::get_if<BoundDropTable>(&effect->node)) {
            const auto* drop = std::get_if<BoundDropTable>(&effect->node);
            auto removed = catalog.dropTables(drop->table_names, drop->if_exists);
            if (const auto* error = std::get_if<Diagnostic>(&removed)) { printDiagnostic(*error); return 1; }
        }
    }
    std::cout << "]}" << '\n';
    return 0;
}
