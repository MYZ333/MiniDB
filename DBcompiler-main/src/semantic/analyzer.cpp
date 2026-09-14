// B：将 AST 的名称解析为具体表列，并为每个表达式推导类型。
// 六类基础语句共享名字解析、表达式类型规则和布尔子句检查；
// EXPLAIN 复用完整基础绑定结果。
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

BindingScope singleTableScope(const std::shared_ptr<const TableSchema>& table,
                              const std::optional<Identifier>& alias) {
    return BindingScope{{table, alias ? normalizeName(alias->text) : table->name, 0}};
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

bool containsAggregate(const BoundExprPtr& expression) {
    if (!expression) return false;
    return std::visit([&](const auto& node) -> bool {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, BoundAggregate>) return true;
        else if constexpr (std::is_same_v<T, BoundUnary>) return containsAggregate(node.operand);
        else if constexpr (std::is_same_v<T, BoundBinary>)
            return containsAggregate(node.left) || containsAggregate(node.right);
        else return false;
    }, expression->node);
}

// 聚合函数之外出现的列必须由 GROUP BY 唯一确定。
bool groupingCompatible(const BoundExprPtr& expression,
                        const std::vector<BoundColumnRef>& group_by) {
    if (!expression) return true;
    return std::visit([&](const auto& node) -> bool {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, BoundColumnRef>) return containsColumn(group_by, node);
        else if constexpr (std::is_same_v<T, BoundAggregate> || std::is_same_v<T, BoundLiteral>) return true;
        else if constexpr (std::is_same_v<T, BoundUnary>) return groupingCompatible(node.operand, group_by);
        else return groupingCompatible(node.left, group_by) && groupingCompatible(node.right, group_by);
    }, expression->node);
}

std::size_t utf8Length(const std::string& value) {
    std::size_t count = 0;
    for (unsigned char byte : value) if ((byte & 0xc0u) != 0x80u) ++count;
    return count;
}

// A 使用语法枚举，B 使用计划枚举；逐项映射，不依赖枚举整数值。
std::optional<AggregateKind> aggregateKind(AggregateFunction function) {
    switch (function) {
    case AggregateFunction::Count: return AggregateKind::Count;
    case AggregateFunction::Sum: return AggregateKind::Sum;
    case AggregateFunction::Avg: return AggregateKind::Avg;
    case AggregateFunction::Min: return AggregateKind::Min;
    case AggregateFunction::Max: return AggregateKind::Max;
    }
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
        bool has_primary_key = false;
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
            if (column.varchar_length &&
                (column.type != DataType::Varchar || *column.varchar_length <= 0)) {
                return error(ErrorCode::UnsupportedType,
                    "VARCHAR length must be a positive integer", location(column.span, statement_span_));
            }
            if (column.primary_key && has_primary_key)
                return error(ErrorCode::InvalidAst,
                    "table may contain only one PRIMARY KEY column",
                    location(column.span, statement_span_));
            has_primary_key = has_primary_key || column.primary_key;
            std::optional<ScalarValue> default_value;
            if (column.default_value) {
                const auto actual = literalType(column.default_value->value);
                if ((column.not_null || column.primary_key) && actual == DataType::Null)
                    return error(ErrorCode::TypeMismatch,
                        "NOT NULL column cannot default to NULL", column.default_value->span);
                if (actual != DataType::Null && actual != column.type)
                    return error(ErrorCode::TypeMismatch,
                        std::string("DEFAULT expects ") + typeName(column.type) + ", but " +
                        typeName(actual) + " found", column.default_value->span);
                if (column.varchar_length && actual == DataType::Varchar &&
                    utf8Length(std::get<std::string>(column.default_value->value)) >
                        static_cast<std::size_t>(*column.varchar_length))
                    return error(ErrorCode::TypeMismatch,
                        "DEFAULT exceeds VARCHAR length", column.default_value->span);
                default_value = scalar(column.default_value->value);
            }
            columns.push_back({std::move(normalized), column.type, column.varchar_length,
                               column.primary_key, column.not_null || column.primary_key,
                               column.unique || column.primary_key, std::move(default_value)});
        }
        // 只有描述，没有注册动作，也不分配数据库 ID。
        return success(BoundCreateTable{normalizeName(stmt.table.text), std::move(columns)});
    }

    Result<BoundStatement> bindStatement(const DropTableStmt& stmt) {
        if (stmt.tables.empty())
            return error(ErrorCode::InvalidAst, "DROP TABLE requires at least one table", statement_span_);
        std::unordered_set<std::string> seen;
        std::vector<std::string> names;
        for (const auto& table : stmt.tables) {
            auto name = normalizeName(table.text);
            if (!seen.insert(name).second)
                return error(ErrorCode::DuplicateTable,
                    "duplicate table '" + table.text + "' in DROP TABLE", table.span);
            if (!stmt.if_exists && !catalog_.findTable(name))
                return error(ErrorCode::TableNotFound,
                    "table '" + table.text + "' does not exist", table.span);
            names.push_back(std::move(name));
        }
        return success(BoundDropTable{std::move(names), stmt.if_exists});
    }

    Result<BoundStatement> bindStatement(const ExplainStmt& stmt) {
        // EXPLAIN 不放宽目标 SQL 的语义规则；先完整绑定，再加上解释属性。
        auto target = std::visit([this](const auto& node) {
            return bindStatement(node);
        }, stmt.target);
        if (const auto* failure = std::get_if<Diagnostic>(&target)) return *failure;
        auto bound = std::make_shared<const BoundStatement>(
            std::get<BoundStatement>(std::move(target)));
        return success(BoundExplain{std::move(bound), stmt.analyze});
    }

    Result<BoundStatement> bindStatement(const InsertStmt& stmt) {
        auto lookup = findTable(stmt.table);
        if (const auto* failure = std::get_if<Diagnostic>(&lookup)) return *failure;
        auto table = std::get<std::shared_ptr<const TableSchema>>(lookup);
        const std::vector<std::vector<LocatedLiteral>> input_rows = stmt.rows.empty()
            ? std::vector<std::vector<LocatedLiteral>>{stmt.values} : stmt.rows;
        if (input_rows.empty())
            return error(ErrorCode::ValueCountMismatch, "INSERT requires at least one row", statement_span_);
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
        std::vector<std::vector<ScalarValue>> rows;
        for (const auto& input_values : input_rows) {
            if (targets.size() != input_values.size())
                return error(ErrorCode::ValueCountMismatch,
                    "INSERT has " + std::to_string(targets.size()) + " columns but " +
                    std::to_string(input_values.size()) + " values", statement_span_);
            std::vector<ScalarValue> values(table->columns.size(), NullValue{});
            std::vector<bool> provided(table->columns.size(), false);
            for (std::size_t i = 0; i < targets.size(); ++i) {
                const auto ordinal = targets[i].ordinal;
                const auto& field = table->columns[ordinal];
                const auto actual = literalType(input_values[i].value);
                if (actual == DataType::Null && field.not_null)
                    return error(ErrorCode::TypeMismatch,
                        table->name + "." + field.name + " does not allow NULL", input_values[i].span);
                if (actual != DataType::Null && field.type != actual)
                    return error(ErrorCode::TypeMismatch,
                        table->name + "." + field.name + " expects " + typeName(field.type) +
                        ", but " + typeName(actual) + " found", input_values[i].span);
                if (field.varchar_length && actual == DataType::Varchar &&
                    utf8Length(std::get<std::string>(input_values[i].value)) >
                        static_cast<std::size_t>(*field.varchar_length))
                    return error(ErrorCode::TypeMismatch,
                        table->name + "." + field.name + " exceeds VARCHAR length", input_values[i].span);
                values[ordinal] = scalar(input_values[i].value);
                provided[ordinal] = true;
            }
            for (std::size_t ordinal = 0; ordinal < table->columns.size(); ++ordinal) {
                if (provided[ordinal]) continue;
                const auto& field = table->columns[ordinal];
                if (field.default_value) values[ordinal] = *field.default_value;
                else if (field.not_null)
                    return error(ErrorCode::MissingInsertColumn,
                        "INSERT omits required column '" + field.name + "'", statement_span_);
            }
            rows.push_back(std::move(values));
        }
        return success(BoundInsert{table, rows.front(), std::move(rows)});
    }

    Result<BoundStatement> bindStatement(const SelectStmt& stmt) {
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
                             joined_name, relation_id, join.type});
        }

        std::vector<BoundColumnRef> columns;
        std::vector<BoundExprPtr> projection_expressions;
        std::vector<std::string> output_names;
        std::vector<std::pair<std::string, std::size_t>> output_aliases;
        std::vector<BoundAggregateItem> aggregate_items;
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
                    output_aliases.push_back({std::move(alias), i});
                } else {
                    output_names.push_back(
                        scope[ref.relation_id - 1].table->columns[ref.ordinal].name);
                }
            }
        } else {
            const auto& items = std::get<std::vector<SelectItem>>(stmt.columns);
            if (items.empty())
                return error(ErrorCode::EmptyColumnList, "SELECT column list must not be empty", statement_span_);
            if (!stmt.column_aliases.empty() && stmt.column_aliases.size() != items.size())
                return error(ErrorCode::InvalidAst,
                             "SELECT aliases must match selected items", statement_span_);
            for (std::size_t output_ordinal = 0; output_ordinal < items.size(); ++output_ordinal) {
                const auto& item = items[output_ordinal];
                std::string default_name;
                BoundExprPtr expression;
                if (const auto* name = std::get_if<Identifier>(&item)) {
                    auto resolved = resolveColumn(*name, scope, statement_span_);
                    if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
                    auto ref = std::get<BoundColumnRef>(resolved);
                    columns.push_back(ref);
                    expression = std::make_shared<const BoundExpr>(BoundExpr{ref, ref.type, name->span});
                    default_name = scope[ref.relation_id - 1].table->columns[ref.ordinal].name;
                } else if (const auto* call = std::get_if<AggregateCall>(&item)) {
                    auto source = std::make_shared<const Expr>(Expr{*call, call->span});
                    auto bound = bindExpr(source, scope, 0, statement_span_, true);
                    if (const auto* failure = std::get_if<Diagnostic>(&bound)) return *failure;
                    expression = std::get<BoundExprPtr>(std::move(bound));
                    const auto& aggregate = std::get<BoundAggregate>(expression->node);
                    default_name = aggregateName(aggregate.kind) + std::string{"("} +
                        (aggregate.argument ? scope[aggregate.argument->relation_id - 1]
                            .table->columns[aggregate.argument->ordinal].name : "*") + ")";
                } else {
                    const auto& source = std::get<ExprPtr>(item);
                    auto bound = bindExpr(source, scope, 0, statement_span_, true);
                    if (const auto* failure = std::get_if<Diagnostic>(&bound)) return *failure;
                    expression = std::get<BoundExprPtr>(std::move(bound));
                    // 括号只改变解析路径，不应把 `(age)` 或 `(COUNT(*))` 的默认列名变成 exprN。
                    if (const auto* ref = std::get_if<BoundColumnRef>(&expression->node)) {
                        default_name = scope[ref->relation_id - 1].table->columns[ref->ordinal].name;
                    } else if (const auto* aggregate = std::get_if<BoundAggregate>(&expression->node)) {
                        default_name = aggregateName(aggregate->kind) + std::string{"("} +
                            (aggregate->argument ? scope[aggregate->argument->relation_id - 1]
                                .table->columns[aggregate->argument->ordinal].name : "*") + ")";
                    } else default_name = "expr" + std::to_string(output_ordinal + 1);
                }
                projection_expressions.push_back(expression);
                const auto alias_name = stmt.column_aliases.empty()
                    ? std::optional<Identifier>{} : stmt.column_aliases[output_ordinal];
                if (alias_name) {
                    if (alias_name->text.find('.') != std::string::npos)
                        return error(ErrorCode::InvalidAst, "column alias must be a simple identifier",
                                     location(alias_name->span, statement_span_));
                    auto alias = normalizeName(alias_name->text);
                    output_names.push_back(alias);
                    output_aliases.push_back({std::move(alias), output_ordinal});
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
        BoundExprPtr having;
        if (stmt.having) {
            auto result = bindBoolean(stmt.having, scope, "HAVING",
                                      ErrorCode::WhereNotBoolean, true);
            if (const auto* failure = std::get_if<Diagnostic>(&result)) return *failure;
            having = std::get<BoundExprPtr>(std::move(result));
        }

        struct PendingOrder {
            std::optional<std::size_t> output_ordinal;
            BoundExprPtr expression;
            SortDirection direction;
            SourceLocation span;
        };
        std::vector<PendingOrder> pending_order;
        bool aggregate_query = having && containsAggregate(having);
        for (const auto& expression : projection_expressions)
            aggregate_query = aggregate_query || containsAggregate(expression);
        // 旧列清单没有 BoundExpr；在需要统一处理时按列引用构造只读表达式。
        auto projectedExpression = [&](std::size_t ordinal) -> BoundExprPtr {
            if (!projection_expressions.empty()) return projection_expressions[ordinal];
            const auto& ref = columns[ordinal];
            return std::make_shared<const BoundExpr>(BoundExpr{ref, ref.type, statement_span_});
        };
        std::vector<BoundOrderBy> order_by;
        std::vector<BoundAggregateOrder> aggregate_order_by;
        for (const auto& item : stmt.order_by) {
            PendingOrder pending{std::nullopt, nullptr, item.direction, item.span};
            if (item.expression) {
                auto bound = bindExpr(item.expression, scope, 0, item.span, true);
                if (const auto* failure = std::get_if<Diagnostic>(&bound)) return *failure;
                pending.expression = std::get<BoundExprPtr>(std::move(bound));
            } else {
                std::optional<std::size_t> alias_match;
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
                if (alias_match) {
                    pending.output_ordinal = *alias_match;
                } else {
                    auto resolved = resolveColumn(item.column, scope, item.span);
                    if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
                    auto ref = std::get<BoundColumnRef>(resolved);
                    pending.expression = std::make_shared<const BoundExpr>(
                        BoundExpr{ref, ref.type, item.column.span});
                }
            }
            if (pending.expression) aggregate_query = aggregate_query || containsAggregate(pending.expression);
            pending_order.push_back(std::move(pending));
        }

        if (having) aggregate_query = true; // HAVING always runs after forming groups.
        std::vector<BoundExpressionOrder> expression_order_by;
        if (aggregate_query) {
            std::vector<BoundExprPtr> selected;
            if (!projection_expressions.empty()) selected = projection_expressions;
            else for (std::size_t i = 0; i < columns.size(); ++i) selected.push_back(projectedExpression(i));
            for (const auto& expression : selected) {
                if (!groupingCompatible(expression, group_by))
                    return error(ErrorCode::InvalidGrouping,
                        "non-aggregate SELECT columns must occur in GROUP BY", expression->span);
                if (const auto* ref = std::get_if<BoundColumnRef>(&expression->node))
                    aggregate_items.push_back({*ref});
                else if (const auto* aggregate = std::get_if<BoundAggregate>(&expression->node))
                    aggregate_items.push_back({*aggregate});
                else aggregate_items.push_back({expression});
            }
            if (!groupingCompatible(having, group_by))
                return error(ErrorCode::InvalidGrouping,
                    "non-aggregate HAVING columns must occur in GROUP BY", having->span);
            for (const auto& pending : pending_order) {
                if (pending.output_ordinal) {
                    aggregate_order_by.push_back({*pending.output_ordinal, pending.direction});
                } else {
                    if (!groupingCompatible(pending.expression, group_by))
                        return error(ErrorCode::InvalidGrouping,
                            "non-aggregate ORDER BY columns must occur in GROUP BY", pending.span);
                    if (const auto* ref = std::get_if<BoundColumnRef>(&pending.expression->node))
                        aggregate_order_by.push_back({*ref, pending.direction});
                    else aggregate_order_by.push_back({pending.expression, pending.direction});
                }
            }
        } else {
            if (!group_by.empty()) {
                if (!projection_expressions.empty()) {
                    for (const auto& expression : projection_expressions)
                        if (!groupingCompatible(expression, group_by))
                            return error(ErrorCode::InvalidGrouping,
                                "SELECT expression columns must occur in GROUP BY", expression->span);
                } else for (const auto& column : columns)
                    if (!containsColumn(group_by, column))
                        return error(ErrorCode::InvalidGrouping,
                            "every selected column must occur in GROUP BY", statement_span_);
            }
            bool simple_order = projection_expressions.empty();
            for (const auto& pending : pending_order) {
                auto expression = pending.output_ordinal
                    ? projectedExpression(*pending.output_ordinal) : pending.expression;
                if (!group_by.empty() && !groupingCompatible(expression, group_by))
                    return error(ErrorCode::InvalidGrouping,
                        "ORDER BY expression columns must occur in GROUP BY", pending.span);
                if (simple_order) {
                    if (const auto* ref = std::get_if<BoundColumnRef>(&expression->node)) {
                        order_by.push_back({*ref, pending.direction});
                        continue;
                    }
                    simple_order = false;
                    for (const auto& old : order_by) expression_order_by.push_back({old.column, old.direction});
                    order_by.clear();
                }
                expression_order_by.push_back({expression, pending.direction});
            }
        }
        return success(BoundSelect{std::move(table), std::move(columns),
                                   std::get<BoundExprPtr>(std::move(predicate)),
                                   std::move(joins), std::move(group_by), std::move(order_by),
                                   relation_name, 1, std::move(output_names),
                                   std::move(aggregate_items), std::move(aggregate_order_by),
                                   std::move(projection_expressions),
                                   std::move(expression_order_by), std::move(having), stmt.distinct,
                                   stmt.limit, stmt.offset.value_or(0)});
    }

    Result<BoundStatement> bindStatement(const UpdateStmt& stmt) {
        auto lookup = findTable(stmt.table);
        if (const auto* failure = std::get_if<Diagnostic>(&lookup)) return *failure;
        auto table = std::get<std::shared_ptr<const TableSchema>>(lookup);
        if (stmt.table_alias && stmt.table_alias->text.find('.') != std::string::npos)
            return error(ErrorCode::InvalidAst, "UPDATE table alias must be a simple identifier",
                         location(stmt.table_alias->span, statement_span_));
        // A 支持 UPDATE 目标表别名；B 在单表作用域中用别名替代物理表名做限定名解析。
        const auto scope = singleTableScope(table, stmt.table_alias);
        if (stmt.assignments.empty()) {
            return error(ErrorCode::InvalidAst, "UPDATE requires at least one assignment", statement_span_);
        }
        std::unordered_set<std::size_t> targets;
        std::vector<BoundAssignment> assignments;
        for (const auto& assignment : stmt.assignments) {
            const auto span = location(assignment.span, statement_span_);
            auto resolved = resolveColumn(assignment.target, scope, span);
            if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
            auto target = std::get<BoundColumnRef>(resolved);
            if (!targets.insert(target.ordinal).second) {
                return error(ErrorCode::DuplicateAssignment,
                    "duplicate assignment to column '" + assignment.target.text + "'",
                    location(assignment.target.span, span));
            }
            // 始终绑定到同一份原表模式，不用先前赋值替换 RHS 中的列引用。
            auto expression = bindExpr(assignment.value, scope, 0, span);
            if (const auto* failure = std::get_if<Diagnostic>(&expression)) return *failure;
            auto rhs = std::get<BoundExprPtr>(std::move(expression));
            const auto& target_column = table->columns[target.ordinal];
            if (rhs->type == DataType::Null && target_column.not_null) {
                return error(ErrorCode::TypeMismatch,
                    table->name + "." + target_column.name + " does not allow NULL", rhs->span);
            }
            if (rhs->type != DataType::Null && rhs->type != target.type) {
                return error(ErrorCode::TypeMismatch,
                    table->name + "." + target_column.name + " expects " +
                    typeName(target.type) + ", but " + typeName(rhs->type) + " found", rhs->span);
            }
            assignments.push_back({target, std::move(rhs)});
        }
        auto predicate = bindWhere(stmt.where, scope);
        if (const auto* failure = std::get_if<Diagnostic>(&predicate)) return *failure;
        return success(BoundUpdate{std::move(table), std::move(assignments),
                                   std::get<BoundExprPtr>(std::move(predicate))});
    }

    Result<BoundStatement> bindStatement(const DeleteStmt& stmt) {
        auto lookup = findTable(stmt.table);
        if (const auto* failure = std::get_if<Diagnostic>(&lookup)) return *failure;
        auto table = std::get<std::shared_ptr<const TableSchema>>(lookup);
        if (stmt.table_alias && stmt.table_alias->text.find('.') != std::string::npos)
            return error(ErrorCode::InvalidAst, "DELETE table alias must be a simple identifier",
                         location(stmt.table_alias->span, statement_span_));
        // A 支持 DELETE 目标表别名；WHERE 中的限定列使用该单表作用域解析。
        const auto scope = singleTableScope(table, stmt.table_alias);
        auto predicate = bindWhere(stmt.where, scope);
        if (const auto* failure = std::get_if<Diagnostic>(&predicate)) return *failure;
        return success(BoundDelete{std::move(table), std::get<BoundExprPtr>(std::move(predicate))});
    }

    // SELECT/UPDATE/DELETE 共用：没有 WHERE 合法；有条件则必须推导为 BOOL。
    Result<BoundExprPtr> bindBoolean(const ExprPtr& expression, const BindingScope& scope,
                                    const char* clause, ErrorCode type_error,
                                    bool allow_aggregate = false) {
        if (!expression) return error(ErrorCode::InvalidAst,
                                      std::string(clause) + " requires an expression", statement_span_);
        auto result = bindExpr(expression, scope, 0, statement_span_, allow_aggregate);
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
                                 std::size_t depth, SourceLocation fallback,
                                 bool allow_aggregate = false) {
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
                auto operand = bindExpr(node.operand, scope, depth + 1, op_span, allow_aggregate);
                if (const auto* failure = std::get_if<Diagnostic>(&operand)) return *failure;
                auto child = std::get<BoundExprPtr>(std::move(operand));
                auto type = unaryResult(node.op, child->type);
                if (!type) return error(ErrorCode::InvalidOperandType,
                    std::string("operator '") + operatorName(node.op) + "' cannot be applied to " + typeName(child->type), op_span);
                return std::make_shared<const BoundExpr>(BoundExpr{
                    BoundUnary{node.op, child, op_span}, *type, span});
            } else if constexpr (std::is_same_v<T, AggregateCall>) {
                if (!allow_aggregate)
                    return error(ErrorCode::InvalidGrouping,
                        "aggregate functions are only allowed in SELECT, HAVING and ORDER BY", span);
                const auto kind = aggregateKind(node.function);
                if (!kind)
                    return error(ErrorCode::UnsupportedFeature, "aggregate function is not supported", span);
                std::optional<BoundColumnRef> argument;
                if (const auto* name = std::get_if<Identifier>(&node.argument)) {
                    auto resolved = resolveColumn(*name, scope, span);
                    if (const auto* failure = std::get_if<Diagnostic>(&resolved)) return *failure;
                    argument = std::get<BoundColumnRef>(resolved);
                } else if (*kind != AggregateKind::Count) {
                    return error(ErrorCode::InvalidOperandType,
                        aggregateName(*kind) + " does not accept '*'", span);
                }
                DataType result_type = DataType::Int;
                if (*kind == AggregateKind::Avg) result_type = DataType::Float;
                else if (*kind != AggregateKind::Count) result_type = argument->type;
                if ((*kind == AggregateKind::Sum || *kind == AggregateKind::Avg) &&
                    argument->type != DataType::Int && argument->type != DataType::Float)
                    return error(ErrorCode::InvalidOperandType,
                        aggregateName(*kind) + " expects an INT or FLOAT column", span);
                BoundAggregate aggregate{*kind, argument, result_type, span};
                return std::make_shared<const BoundExpr>(BoundExpr{aggregate, result_type, span});
            } else {
                const auto op_span = location(node.operator_span, span);
                auto left = bindExpr(node.left, scope, depth + 1, op_span, allow_aggregate);
                if (const auto* failure = std::get_if<Diagnostic>(&left)) return *failure;
                auto right = bindExpr(node.right, scope, depth + 1, op_span, allow_aggregate);
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
