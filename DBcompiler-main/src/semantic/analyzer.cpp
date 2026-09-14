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

DataType literalType(const LiteralValue& value) {
    if (std::holds_alternative<std::int64_t>(value)) return DataType::Int;
    if (std::holds_alternative<double>(value)) return DataType::Float;
    if (std::holds_alternative<std::string>(value)) return DataType::Varchar;
    if (std::holds_alternative<bool>(value)) return DataType::Bool;
    return DataType::Null;
}

ScalarValue scalar(const LiteralValue& value) {  //把literal value转成scalar value
    return std::visit([](const auto& item) -> ScalarValue { return item; }, value);
}

struct RelationBinding {
    std::shared_ptr<const TableSchema> table;
    std::string name;
    std::uint64_t id;
};

BoundColumnRef columnRef(const RelationBinding& relation, std::size_t index) {
    const auto& field = relation.table->columns[index];
    return {relation.table->id, field.id, index, field.type, relation.id};
}

using BindingScope = std::vector<RelationBinding>;

BindingScope singleTableScope(const std::shared_ptr<const TableSchema>& table) {
    return BindingScope{{table, table->name, 0}};
}

bool sameColumn(const BoundColumnRef& left, const BoundColumnRef& right) {
    return left.table_id.value == right.table_id.value &&
           left.column_id.value == right.column_id.value &&
           left.relation_id == right.relation_id;
}

bool containsColumn(const std::vector<BoundColumnRef>& columns, const BoundColumnRef& target) {
    for (const auto& column : columns) if (sameColumn(column, target)) return true;
    return false;
}

std::optional<AggregateKind> aggregateKind(const std::string& name) {
    if (name == "count") return AggregateKind::Count;
    if (name == "sum") return AggregateKind::Sum;
    if (name == "avg") return AggregateKind::Avg;
    if (name == "min") return AggregateKind::Min;
    if (name == "max") return AggregateKind::Max;
    return std::nullopt;
}

std::string aggregateName(AggregateKind kind) {
    switch (kind) {
        case AggregateKind::Count: return "count";
        case AggregateKind::Sum: return "sum";
        case AggregateKind::Avg: return "avg";
        case AggregateKind::Min: return "min";
        case AggregateKind::Max: return "max";
    }
    return "aggregate";
}

// 限定名精确选择表；非限定名在全部可见表中查找，命中多次必须报歧义。
Result<BoundColumnRef> resolveColumn(const Identifier& name, const BindingScope& scope,
                                    SourceLocation fallback) {
    auto normalized = normalizeName(name.text);
    std::optional<std::string> qualifier;
    if (const auto dot = normalized.find('.'); dot != std::string::npos) {
        if (normalized.find('.', dot + 1) != std::string::npos) {
            return error(ErrorCode::ColumnNotFound,
                "qualified column '" + name.text + "' has too many name parts",
                location(name.span, fallback));
        }
        qualifier = normalized.substr(0, dot);
        normalized = normalized.substr(dot + 1);
    }

    std::optional<BoundColumnRef> match;
    for (const auto& relation : scope) {
        if (qualifier && relation.name != *qualifier) continue;
        for (std::size_t index = 0; index < relation.table->columns.size(); ++index) {
            if (relation.table->columns[index].name != normalized) continue;
            if (match) {
                return error(ErrorCode::AmbiguousColumn,
                    "column '" + name.text + "' is ambiguous; qualify it with a table alias",
                    location(name.span, fallback));
            }
            match = columnRef(relation, index);
        }
    }
    if (match) return *match;
    return error(ErrorCode::ColumnNotFound,
        "column '" + name.text + "' does not exist in the visible tables",
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
            if (column.type == DataType::Null) {
                return error(ErrorCode::UnsupportedType, "NULL is not a declarable column type",
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
                auto resolved = resolveColumn(name, singleTableScope(table), statement_span_);
                if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
                auto ref = std::get<BoundColumnRef>(resolved);
                if (!seen.insert(ref.ordinal).second) {
                    return error(ErrorCode::DuplicateColumn, "duplicate INSERT column '" + name.text + "'",
                                 location(name.span, statement_span_));
                }
                targets.push_back(ref);
            }
        } else {
            const auto scope = singleTableScope(table);
            for (std::size_t i = 0; i < table->columns.size(); ++i)
                targets.push_back(columnRef(scope.front(), i));
        }
        if (targets.size() != stmt.values.size()) {
            return error(ErrorCode::ValueCountMismatch, "INSERT has " + std::to_string(targets.size()) +
                " columns but " + std::to_string(stmt.values.size()) + " values", statement_span_);
        }
        if (targets.size() != table->columns.size()) {
            return error(ErrorCode::MissingInsertColumn,
                         "INSERT must provide all columns; DEFAULT is not supported", statement_span_);
        }

        // 确认完整覆盖后再分配输出，按 ordinal 写入而不是按 SQL 输入顺序追加。
        std::vector<ScalarValue> values(table->columns.size());
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const auto actual = literalType(stmt.values[i].value);
            // 当前模式统一允许空值；NULL 没有自己的列类型，不参与普通表达式运算。
            if (actual != DataType::Null && targets[i].type != actual) {
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
        // 外部调用方也可以用 SelectItem 表示纯列查询；统一到原列清单路径。
        if (const auto* items = std::get_if<std::vector<SelectItem>>(&stmt.columns)) {
            bool has_aggregate = false;
            for (const auto& item : *items)
                has_aggregate |= std::holds_alternative<AggregateCall>(item.value);
            if (!has_aggregate && stmt.column_aliases.empty()) {
                auto plain = stmt;
                std::vector<Identifier> names;
                for (const auto& item : *items) {
                    names.push_back(std::get<Identifier>(item.value));
                    plain.column_aliases.push_back(item.alias);
                }
                plain.columns = std::move(names);
                return bindStatement(plain);
            }
        }
        if (stmt.table_alias && stmt.table_alias->text.find('.') != std::string::npos)
            return error(ErrorCode::InvalidAst, "table alias must be a simple identifier",
                         location(stmt.table_alias->span, statement_span_));
        auto lookup = findTable(stmt.table);
        if (const auto* failure = std::get_if<Diagnostic>(&lookup)) return *failure;
        auto table = std::get<std::shared_ptr<const TableSchema>>(lookup);
        const std::string relation_name = stmt.table_alias
            ? normalizeName(stmt.table_alias->text) : table->name;
        BindingScope scope{{table, relation_name, 1}};
        std::vector<BoundJoin> joins;
        for (const auto& join : stmt.joins) {
            if (join.alias && join.alias->text.find('.') != std::string::npos)
                return error(ErrorCode::InvalidAst, "JOIN alias must be a simple identifier",
                             location(join.alias->span, location(join.span, statement_span_)));
            auto joined_lookup = findTable(join.table);
            if (const auto* failure = std::get_if<Diagnostic>(&joined_lookup)) return *failure;
            auto joined = std::get<std::shared_ptr<const TableSchema>>(joined_lookup);
            const std::string joined_name = join.alias
                ? normalizeName(join.alias->text) : joined->name;
            for (const auto& visible : scope) {
                if (visible.name == joined_name) {
                    return error(ErrorCode::DuplicateTable,
                        "relation name '" + joined_name + "' occurs more than once",
                        location(join.alias ? join.alias->span : join.table.span,
                                 location(join.span, statement_span_)));
                }
            }
            const std::uint64_t relation_id = scope.size() + 1;
            scope.push_back({joined, joined_name, relation_id});
            auto condition = bindBoolean(join.on, scope, "JOIN ON", ErrorCode::JoinConditionNotBoolean);
            if (const auto* failure = std::get_if<Diagnostic>(&condition)) return *failure;
            joins.push_back({std::move(joined), std::get<BoundExprPtr>(std::move(condition)),
                             joined_name, relation_id});
        }
        std::vector<BoundColumnRef> columns;
        std::vector<std::string> output_names;
        std::vector<std::pair<std::string, BoundColumnRef>> output_aliases;
        std::vector<BoundAggregateItem> aggregate_items;
        std::vector<std::pair<std::string, std::size_t>> aggregate_aliases;
        if (std::holds_alternative<AllColumns>(stmt.columns)) {
            if (!stmt.column_aliases.empty())
                return error(ErrorCode::InvalidAst,
                             "SELECT * cannot carry column aliases", statement_span_);
            for (const auto& visible : scope) {
                for (std::size_t i = 0; i < visible.table->columns.size(); ++i) {
                    columns.push_back(columnRef(visible, i));
                    output_names.push_back(visible.table->columns[i].name);
                }
            }
        } else if (const auto* names = std::get_if<std::vector<Identifier>>(&stmt.columns)) {
            if (names->empty()) {
                return error(ErrorCode::EmptyColumnList, "SELECT column list must not be empty", statement_span_);
            }
            if (!stmt.column_aliases.empty() && stmt.column_aliases.size() != names->size())
                return error(ErrorCode::InvalidAst,
                             "SELECT aliases must match the selected columns", statement_span_);
            for (std::size_t i = 0; i < names->size(); ++i) {
                const auto& name = (*names)[i];
                auto resolved = resolveColumn(name, scope, statement_span_);
                if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
                auto ref = std::get<BoundColumnRef>(resolved);
                columns.push_back(ref);
                if (!stmt.column_aliases.empty() && stmt.column_aliases[i]) {
                    if (stmt.column_aliases[i]->text.find('.') != std::string::npos)
                        return error(ErrorCode::InvalidAst,
                            "column alias must be a simple identifier",
                            location(stmt.column_aliases[i]->span, statement_span_));
                    auto alias = normalizeName(stmt.column_aliases[i]->text);
                    output_names.push_back(alias);
                    output_aliases.push_back({std::move(alias), ref});
                } else {
                    output_names.push_back(
                        scope[ref.relation_id - 1].table->columns[ref.ordinal].name);
                }
            }
        } else {
            const auto& items = std::get<std::vector<SelectItem>>(stmt.columns);
            if (items.empty())
                return error(ErrorCode::EmptyColumnList, "SELECT column list must not be empty", statement_span_);
            if (!stmt.column_aliases.empty())
                return error(ErrorCode::InvalidAst,
                             "rich SELECT items carry their aliases directly", statement_span_);
            for (const auto& item : items) {
                const std::size_t output_ordinal = aggregate_items.size();
                std::string default_name;
                if (const auto* name = std::get_if<Identifier>(&item.value)) {
                    auto resolved = resolveColumn(*name, scope, statement_span_);
                    if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
                    auto ref = std::get<BoundColumnRef>(resolved);
                    columns.push_back(ref);
                    aggregate_items.push_back({ref});
                    default_name = scope[ref.relation_id - 1].table->columns[ref.ordinal].name;
                } else {
                    const auto& call = std::get<AggregateCall>(item.value);
                    const auto kind = aggregateKind(normalizeName(call.function.text));
                    if (!kind) {
                        return error(ErrorCode::UnsupportedFeature,
                            "unsupported aggregate function '" + call.function.text + "'",
                            location(call.function.span, location(call.span, statement_span_)));
                    }
                    std::optional<BoundColumnRef> argument;
                    if (call.count_star == call.argument.has_value())
                        return error(ErrorCode::InvalidAst,
                            "aggregate requires exactly one column argument or '*'",
                            location(call.span, statement_span_));
                    if (call.argument) {
                        auto resolved = resolveColumn(*call.argument, scope, call.span);
                        if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
                        argument = std::get<BoundColumnRef>(resolved);
                    }
                    if (call.count_star && *kind != AggregateKind::Count) {
                        return error(ErrorCode::InvalidOperandType,
                            aggregateName(*kind) + " does not accept '*'", location(call.span, statement_span_));
                    }
                    if (!call.count_star && !argument) {
                        return error(ErrorCode::InvalidAst, "aggregate argument is missing",
                                     location(call.span, statement_span_));
                    }
                    DataType result_type = DataType::Int;
                    if (*kind == AggregateKind::Avg) result_type = DataType::Float;
                    else if (*kind != AggregateKind::Count) result_type = argument->type;
                    if ((*kind == AggregateKind::Sum || *kind == AggregateKind::Avg) &&
                        argument->type != DataType::Int && argument->type != DataType::Float) {
                        return error(ErrorCode::InvalidOperandType,
                            aggregateName(*kind) + " expects an INT or FLOAT column",
                            location(call.argument->span, location(call.span, statement_span_)));
                    }
                    aggregate_items.push_back({BoundAggregate{*kind, argument, result_type,
                                                              location(call.span, statement_span_)}});
                    default_name = aggregateName(*kind) + "(" +
                        (call.count_star ? "*" : normalizeName(call.argument->text)) + ")";
                }
                if (item.alias) {
                    if (item.alias->text.find('.') != std::string::npos)
                        return error(ErrorCode::InvalidAst, "column alias must be a simple identifier",
                                     location(item.alias->span, statement_span_));
                    auto alias = normalizeName(item.alias->text);
                    output_names.push_back(alias);
                    aggregate_aliases.push_back({std::move(alias), output_ordinal});
                } else output_names.push_back(std::move(default_name));
            }
        }
        auto predicate = bindWhere(stmt.where, scope);
        if (const auto* failure = std::get_if<Diagnostic>(&predicate)) return *failure;

        std::vector<BoundColumnRef> group_by;
        for (const auto& name : stmt.group_by) {
            auto resolved = resolveColumn(name, scope, statement_span_);
            if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
            auto ref = std::get<BoundColumnRef>(resolved);
            if (containsColumn(group_by, ref)) {
                return error(ErrorCode::InvalidGrouping,
                             "duplicate GROUP BY column '" + name.text + "'",
                             location(name.span, statement_span_));
            }
            group_by.push_back(ref);
        }
        if (!aggregate_items.empty()) {
            for (const auto& item : aggregate_items) {
                if (const auto* ref = std::get_if<BoundColumnRef>(&item.value)) {
                    if (!containsColumn(group_by, *ref)) {
                        return error(ErrorCode::InvalidGrouping,
                            "a selected non-aggregate column must occur in GROUP BY", statement_span_);
                    }
                }
            }
        } else if (!group_by.empty()) {
            for (const auto& column : columns) {
                if (!containsColumn(group_by, column)) {
                    return error(ErrorCode::InvalidGrouping,
                        "every selected non-aggregate column must occur in GROUP BY",
                        statement_span_);
                }
            }
        }

        std::vector<BoundOrderBy> order_by;
        std::vector<BoundAggregateOrder> aggregate_order_by;
        for (const auto& item : stmt.order_by) {
            if (!aggregate_items.empty()) {
                std::optional<std::size_t> alias_match;
                const auto order_name = normalizeName(item.column.text);
                if (order_name.find('.') == std::string::npos) {
                    for (const auto& alias : aggregate_aliases) {
                        if (alias.first != order_name) continue;
                        if (alias_match) {
                            return error(ErrorCode::AmbiguousColumn,
                                "ORDER BY alias '" + item.column.text + "' is ambiguous",
                                location(item.column.span, item.span));
                        }
                        alias_match = alias.second;
                    }
                }
                if (alias_match) {
                    aggregate_order_by.push_back({*alias_match, item.direction});
                    continue;
                }
                auto resolved = resolveColumn(item.column, scope, item.span);
                if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
                auto ref = std::get<BoundColumnRef>(resolved);
                if (!containsColumn(group_by, ref)) {
                    return error(ErrorCode::InvalidGrouping,
                        "ORDER BY source column must occur in GROUP BY in an aggregate query",
                        location(item.column.span, item.span));
                }
                aggregate_order_by.push_back({ref, item.direction});
                continue;
            }
            std::optional<BoundColumnRef> alias_match;
            const auto order_name = normalizeName(item.column.text);
            if (order_name.find('.') == std::string::npos) {
                for (const auto& alias : output_aliases) {
                    if (alias.first != order_name) continue;
                    if (alias_match) {
                        return error(ErrorCode::AmbiguousColumn,
                            "ORDER BY alias '" + item.column.text + "' is ambiguous",
                            location(item.column.span, item.span));
                    }
                    alias_match = alias.second;
                }
            }
            BoundColumnRef ref;
            if (alias_match) {
                ref = *alias_match;
            } else {
                auto resolved = resolveColumn(item.column, scope, item.span);
                if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
                ref = std::get<BoundColumnRef>(resolved);
            }
            if (!group_by.empty() && !containsColumn(group_by, ref)) {
                return error(ErrorCode::InvalidGrouping,
                    "ORDER BY column must occur in GROUP BY in a grouped query",
                    location(item.column.span, item.span));
            }
            order_by.push_back({ref, item.direction});
        }
        return success(BoundSelect{std::move(table), std::move(columns),
                                   std::get<BoundExprPtr>(std::move(predicate)),
                                   std::move(joins), std::move(group_by), std::move(order_by),
                                   relation_name, 1, std::move(output_names),
                                   std::move(aggregate_items), std::move(aggregate_order_by)});
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
            auto resolved = resolveColumn(assignment.target, singleTableScope(table), span);
            if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
            auto target = std::get<BoundColumnRef>(resolved);
            if (!targets.insert(target.ordinal).second) {
                return error(ErrorCode::DuplicateAssignment,
                    "duplicate assignment to column '" + assignment.target.text + "'",
                    location(assignment.target.span, span));
            }
            // 始终绑定到同一份原表模式，不用先前赋值替换 RHS 中的列引用。
            auto expression = bindExpr(assignment.value, singleTableScope(table), 0, span);
            if (const auto* failure = std::get_if<Diagnostic>(&expression)) return *failure;
            auto rhs = std::get<BoundExprPtr>(std::move(expression));
            if (rhs->type != target.type) {
                return error(ErrorCode::TypeMismatch,
                    table->name + "." + table->columns[target.ordinal].name + " expects " +
                    typeName(target.type) + ", but " + typeName(rhs->type) + " found", rhs->span);
            }
            assignments.push_back({target, std::move(rhs)});
        }
        auto predicate = bindWhere(stmt.where, singleTableScope(table));
        if (const auto* failure = std::get_if<Diagnostic>(&predicate)) return *failure;
        return success(BoundUpdate{std::move(table), std::move(assignments),
                                   std::get<BoundExprPtr>(std::move(predicate))});
    }

    Result<BoundStatement> bindStatement(const DeleteStmt& stmt) {
        auto lookup = findTable(stmt.table);
        if (const auto* failure = std::get_if<Diagnostic>(&lookup)) return *failure;
        auto table = std::get<std::shared_ptr<const TableSchema>>(lookup);
        auto predicate = bindWhere(stmt.where, singleTableScope(table));
        if (const auto* failure = std::get_if<Diagnostic>(&predicate)) return *failure;
        return success(BoundDelete{std::move(table), std::get<BoundExprPtr>(std::move(predicate))});
    }

    // SELECT/UPDATE/DELETE 共用：没有 WHERE 合法；有条件则必须推导为 BOOL。
    Result<BoundExprPtr> bindBoolean(const ExprPtr& expression, const BindingScope& scope,
                                    const char* clause, ErrorCode type_error) {
        if (!expression) return error(ErrorCode::InvalidAst,
                                      std::string(clause) + " requires an expression", statement_span_);
        auto result = bindExpr(expression, scope, 0, statement_span_);
        if (const auto* failure = std::get_if<Diagnostic>(&result)) return *failure;
        auto predicate = std::get<BoundExprPtr>(std::move(result));
        if (predicate->type != DataType::Bool) {
            return error(type_error, std::string(clause) + " expects BOOL, but " +
                typeName(predicate->type) + " found", location(expression->span, statement_span_));
        }
        return predicate;
    }

    Result<BoundExprPtr> bindWhere(const ExprPtr& where, const BindingScope& scope) {
        if (!where) return BoundExprPtr{};
        return bindBoolean(where, scope, "WHERE", ErrorCode::WhereNotBoolean);
    }

    Result<BoundExprPtr> bindExpr(const ExprPtr& expr, const BindingScope& scope,
                                 std::size_t depth, SourceLocation fallback) {
        if (!expr) return error(ErrorCode::InvalidAst, "required expression child is missing", fallback);
        const auto span = location(expr->span, fallback);
        // 手工 AST 也可能过深；限制递归深度，不让非法输入耗尽 C++ 调用栈。
        if (depth >= 256) return error(ErrorCode::ExpressionTooDeep, "expression exceeds 256 levels", span);

        return std::visit([&](const auto& node) -> Result<BoundExprPtr> {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, IdentifierExpr>) {
                auto resolved = resolveColumn(node.name, scope, span);
                if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
                auto ref = std::get<BoundColumnRef>(resolved);
                return std::make_shared<const BoundExpr>(BoundExpr{ref, ref.type, span});
            } else if constexpr (std::is_same_v<T, LiteralExpr>) {
                return std::make_shared<const BoundExpr>(BoundExpr{
                    BoundLiteral{scalar(node.value)}, literalType(node.value), span});
            } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                const auto op_span = location(node.operator_span, span);
                auto operand = bindExpr(node.operand, scope, depth + 1, op_span);
                if (const auto* failure = std::get_if<Diagnostic>(&operand)) return *failure;
                auto child = std::get<BoundExprPtr>(std::move(operand));
                auto type = unaryResult(node.op, child->type);
                if (!type) return error(ErrorCode::InvalidOperandType,
                    std::string("operator '") + operatorName(node.op) + "' cannot be applied to " + typeName(child->type), op_span);
                return std::make_shared<const BoundExpr>(BoundExpr{
                    BoundUnary{node.op, child, op_span}, *type, span});
            } else {
                const auto op_span = location(node.operator_span, span);
                auto left = bindExpr(node.left, scope, depth + 1, op_span);
                if (const auto* failure = std::get_if<Diagnostic>(&left)) return *failure;
                auto right = bindExpr(node.right, scope, depth + 1, op_span);
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
