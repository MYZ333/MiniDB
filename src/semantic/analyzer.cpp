// B：将 AST 的名称解析为具体表列，并为每个表达式推导类型。
// 五类语句共享名字解析、表达式类型规则和 WHERE 检查。
#include "minisql/compiler.hpp"
#include "type_rules.hpp"

#include <type_traits>
#include <unordered_set>
#include <utility>

namespace minisql {
namespace {
using namespace semantic_detail;

Diagnostic error(ErrorCode code, std::string message, SourceLocation span) {
    return {DiagnosticStage::Semantic, code, std::move(message), span};
}

SourceLocation location(SourceLocation specific, SourceLocation fallback) {
    return specific ? specific : fallback;
}

DataType literalType(const LiteralValue& value) {  //返回常值的类型是int还是varchar
    return std::holds_alternative<std::int64_t>(value) ? DataType::Int : DataType::Varchar;
}

ScalarValue scalar(const LiteralValue& value) {  //把literal value转成scalar value
    return std::visit([](const auto& item) -> ScalarValue { return item; }, value);
}

BoundColumnRef columnRef(const TableSchema& table, std::size_t index) {
    const auto& field = table.columns[index];
    return {table.id, field.id, index, field.type};
}

// 以表定义顺序给出 ordinal；后续扫描记录同样采用该顺序。
Result<BoundColumnRef> resolveColumn(const Identifier& name, const TableSchema& table,
                                    SourceLocation fallback) {
    const auto normalized = normalizeName(name.text);
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
        if (table.columns[index].name == normalized) return columnRef(table, index);
    }
    return error(ErrorCode::ColumnNotFound,
        "column '" + name.text + "' does not exist in table '" + table.name + "'",
        location(name.span, fallback));
}

// 每次调用创建一个短生命周期分析器；不保存全局状态，也不修改调用方对象。
class Analyzer {
public:
    Analyzer(const CatalogSnapshot& catalog, SourceLocation statement_span)
        : catalog_(catalog), statement_span_(statement_span) {}

    Result<BoundStatement> run(const Statement& statement) {
        return std::visit([this](const auto& node) { return bindStatement(node); }, statement.node);
    }

private:
    const CatalogSnapshot& catalog_;
    SourceLocation statement_span_;

    template <typename T>
    Result<BoundStatement> success(T node) const {
        return BoundStatement{catalog_.version(), std::move(node)};
    }

    Result<std::shared_ptr<const TableSchema>> findTable(const Identifier& name) const {
        auto table = catalog_.findTable(normalizeName(name.text));
        if (table) return table;
        return error(ErrorCode::TableNotFound, "table '" + name.text + "' does not exist",
                     location(name.span, statement_span_));
    }

    Result<BoundStatement> bindStatement(const CreateTableStmt& stmt) {
        if (catalog_.findTable(normalizeName(stmt.table.text))) {
            return error(ErrorCode::TableAlreadyExists, "table '" + stmt.table.text + "' already exists",
                         location(stmt.table.span, statement_span_));
        }
        if (stmt.columns.empty()) {
            return error(ErrorCode::EmptyColumnList, "table must have at least one column", statement_span_);
        }
        std::unordered_set<std::string> names;
        std::vector<ColumnSpec> columns;
        for (const auto& column : stmt.columns) {
            auto normalized = normalizeName(column.name.text);
            if (!names.insert(normalized).second) {
                return error(ErrorCode::DuplicateColumn, "duplicate column '" + column.name.text + "'",
                             location(column.name.span, statement_span_));
            }
            if (column.type != DataType::Int && column.type != DataType::Varchar) {
                return error(ErrorCode::UnsupportedType, "table columns support only INT and VARCHAR",
                             location(column.span, location(column.name.span, statement_span_)));
            }
            columns.push_back({std::move(normalized), column.type});
        }
        // 只有描述，没有注册动作，也不分配数据库 ID。
        return success(BoundCreateTable{normalizeName(stmt.table.text), std::move(columns)});
    }

    Result<BoundStatement> bindStatement(const InsertStmt& stmt) {
        auto lookup = findTable(stmt.table);
        if (const auto* failure = std::get_if<Diagnostic>(&lookup)) return *failure;
        auto table = std::get<std::shared_ptr<const TableSchema>>(lookup);
        std::vector<BoundColumnRef> targets;
        if (stmt.columns) {
            if (stmt.columns->empty()) {
                return error(ErrorCode::EmptyColumnList, "INSERT column list must not be empty", statement_span_);
            }
            std::unordered_set<std::size_t> seen;
            for (const auto& name : *stmt.columns) {
                auto resolved = resolveColumn(name, *table, statement_span_);
                if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
                auto ref = std::get<BoundColumnRef>(resolved);
                if (!seen.insert(ref.ordinal).second) {
                    return error(ErrorCode::DuplicateColumn, "duplicate INSERT column '" + name.text + "'",
                                 location(name.span, statement_span_));
                }
                targets.push_back(ref);
            }
        } else {
            for (std::size_t i = 0; i < table->columns.size(); ++i) targets.push_back(columnRef(*table, i));
        }
        if (targets.size() != stmt.values.size()) {
            return error(ErrorCode::ValueCountMismatch, "INSERT has " + std::to_string(targets.size()) +
                " columns but " + std::to_string(stmt.values.size()) + " values", statement_span_);
        }
        if (targets.size() != table->columns.size()) {
            return error(ErrorCode::MissingInsertColumn,
                         "INSERT must provide all columns; NULL and DEFAULT are not supported", statement_span_);
        }

        // 确认完整覆盖后再分配输出，按 ordinal 写入而不是按 SQL 输入顺序追加。
        std::vector<ScalarValue> values(table->columns.size());
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const auto actual = literalType(stmt.values[i].value);
            if (targets[i].type != actual) {
                return error(ErrorCode::TypeMismatch,
                    table->name + "." + table->columns[targets[i].ordinal].name + " expects " +
                    typeName(targets[i].type) + ", but " + typeName(actual) + " found",
                    location(stmt.values[i].span, statement_span_));
            }
            values[targets[i].ordinal] = scalar(stmt.values[i].value);
        }
        return success(BoundInsert{std::move(table), std::move(values)});
    }

    Result<BoundStatement> bindStatement(const SelectStmt& stmt) {
        auto lookup = findTable(stmt.table);
        if (const auto* failure = std::get_if<Diagnostic>(&lookup)) return *failure;
        auto table = std::get<std::shared_ptr<const TableSchema>>(lookup);
        std::vector<BoundColumnRef> columns;
        if (std::holds_alternative<AllColumns>(stmt.columns)) {
            for (std::size_t i = 0; i < table->columns.size(); ++i) columns.push_back(columnRef(*table, i));
        } else {
            const auto& names = std::get<std::vector<Identifier>>(stmt.columns);
            if (names.empty()) {
                return error(ErrorCode::EmptyColumnList, "SELECT column list must not be empty", statement_span_);
            }
            for (const auto& name : names) {
                auto resolved = resolveColumn(name, *table, statement_span_);
                if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
                columns.push_back(std::get<BoundColumnRef>(resolved)); // 保留重复选择列。
            }
        }
        auto predicate = bindWhere(stmt.where, *table);
        if (const auto* failure = std::get_if<Diagnostic>(&predicate)) return *failure;
        return success(BoundSelect{std::move(table), std::move(columns),
                                   std::get<BoundExprPtr>(std::move(predicate))});
    }

    Result<BoundStatement> bindStatement(const UpdateStmt& stmt) {
        auto lookup = findTable(stmt.table);
        if (const auto* failure = std::get_if<Diagnostic>(&lookup)) return *failure;
        auto table = std::get<std::shared_ptr<const TableSchema>>(lookup);
        if (stmt.assignments.empty()) {
            return error(ErrorCode::InvalidAst, "UPDATE requires at least one assignment", statement_span_);
        }
        std::unordered_set<std::size_t> targets;
        std::vector<BoundAssignment> assignments;
        for (const auto& assignment : stmt.assignments) {
            const auto span = location(assignment.span, statement_span_);
            auto resolved = resolveColumn(assignment.target, *table, span);
            if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
            auto target = std::get<BoundColumnRef>(resolved);
            if (!targets.insert(target.ordinal).second) {
                return error(ErrorCode::DuplicateAssignment,
                    "duplicate assignment to column '" + assignment.target.text + "'",
                    location(assignment.target.span, span));
            }
            // 始终绑定到同一份原表模式，不用先前赋值替换 RHS 中的列引用。
            auto expression = bindExpr(assignment.value, *table, 0, span);
            if (const auto* failure = std::get_if<Diagnostic>(&expression)) return *failure;
            auto rhs = std::get<BoundExprPtr>(std::move(expression));
            if (rhs->type != target.type) {
                return error(ErrorCode::TypeMismatch,
                    table->name + "." + table->columns[target.ordinal].name + " expects " +
                    typeName(target.type) + ", but " + typeName(rhs->type) + " found", rhs->span);
            }
            assignments.push_back({target, std::move(rhs)});
        }
        auto predicate = bindWhere(stmt.where, *table);
        if (const auto* failure = std::get_if<Diagnostic>(&predicate)) return *failure;
        return success(BoundUpdate{std::move(table), std::move(assignments),
                                   std::get<BoundExprPtr>(std::move(predicate))});
    }

    Result<BoundStatement> bindStatement(const DeleteStmt& stmt) {
        auto lookup = findTable(stmt.table);
        if (const auto* failure = std::get_if<Diagnostic>(&lookup)) return *failure;
        auto table = std::get<std::shared_ptr<const TableSchema>>(lookup);
        auto predicate = bindWhere(stmt.where, *table);
        if (const auto* failure = std::get_if<Diagnostic>(&predicate)) return *failure;
        return success(BoundDelete{std::move(table), std::get<BoundExprPtr>(std::move(predicate))});
    }

    // SELECT/UPDATE/DELETE 共用：没有 WHERE 合法；有条件则必须推导为 BOOL。
    Result<BoundExprPtr> bindWhere(const ExprPtr& where, const TableSchema& table) {
        if (!where) return BoundExprPtr{};
        auto result = bindExpr(where, table, 0, statement_span_);
        if (const auto* failure = std::get_if<Diagnostic>(&result)) return *failure;
        auto predicate = std::get<BoundExprPtr>(std::move(result));
        if (predicate->type != DataType::Bool) {
            return error(ErrorCode::WhereNotBoolean, "WHERE expects BOOL, but " +
                std::string(typeName(predicate->type)) + " found", location(where->span, statement_span_));
        }
        return predicate;
    }

    Result<BoundExprPtr> bindExpr(const ExprPtr& expr, const TableSchema& table,
                                 std::size_t depth, SourceLocation fallback) {
        if (!expr) return error(ErrorCode::InvalidAst, "required expression child is missing", fallback);
        const auto span = location(expr->span, fallback);
        // 手工 AST 也可能过深；限制递归深度，不让非法输入耗尽 C++ 调用栈。
        if (depth >= 256) return error(ErrorCode::ExpressionTooDeep, "expression exceeds 256 levels", span);

        return std::visit([&](const auto& node) -> Result<BoundExprPtr> {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, IdentifierExpr>) {
                auto resolved = resolveColumn(node.name, table, span);
                if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
                auto ref = std::get<BoundColumnRef>(resolved);
                return std::make_shared<const BoundExpr>(BoundExpr{ref, ref.type, span});
            } else if constexpr (std::is_same_v<T, LiteralExpr>) {
                return std::make_shared<const BoundExpr>(BoundExpr{
                    BoundLiteral{scalar(node.value)}, literalType(node.value), span});
            } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                const auto op_span = location(node.operator_span, span);
                auto operand = bindExpr(node.operand, table, depth + 1, op_span);
                if (const auto* failure = std::get_if<Diagnostic>(&operand)) return *failure;
                auto child = std::get<BoundExprPtr>(std::move(operand));
                auto type = unaryResult(node.op, child->type);
                if (!type) return error(ErrorCode::InvalidOperandType,
                    std::string("operator '") + operatorName(node.op) + "' cannot be applied to " + typeName(child->type), op_span);
                return std::make_shared<const BoundExpr>(BoundExpr{
                    BoundUnary{node.op, child, op_span}, *type, span});
            } else {
                const auto op_span = location(node.operator_span, span);
                auto left = bindExpr(node.left, table, depth + 1, op_span);
                if (const auto* failure = std::get_if<Diagnostic>(&left)) return *failure;
                auto right = bindExpr(node.right, table, depth + 1, op_span);
                if (const auto* failure = std::get_if<Diagnostic>(&right)) return *failure;
                auto lhs = std::get<BoundExprPtr>(std::move(left));
                auto rhs = std::get<BoundExprPtr>(std::move(right));
                auto type = binaryResult(node.op, lhs->type, rhs->type);
                if (!type) return error(ErrorCode::InvalidOperandType,
                    std::string("operator '") + operatorName(node.op) + "' cannot be applied to " +
                    typeName(lhs->type) + " and " + typeName(rhs->type), op_span);
                return std::make_shared<const BoundExpr>(BoundExpr{
                    BoundBinary{node.op, lhs, rhs, op_span}, *type, span});
            }
        }, expr->node);
    }
};
} // namespace

Result<BoundStatement> analyze(const Statement& statement, const CatalogSnapshot& catalog) {
    return Analyzer(catalog, statement.span).run(statement);
}

} // namespace minisql
