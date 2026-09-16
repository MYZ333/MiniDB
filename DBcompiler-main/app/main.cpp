// A 阶段调试入口：从标准输入读取 SQL，输出 Token、原 AST 和可选的优化 AST。
#include "minisql/ast_optimizer.hpp"
#include "minisql/lexer.hpp"
#include "minisql/parser.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace {
using namespace minisql;

std::string tokenName(TokenKind kind) {
    switch (kind) {
    case TokenKind::EndOfInput: return "EndOfInput";
    case TokenKind::Identifier: return "Identifier";
    case TokenKind::Integer: return "Integer";
    case TokenKind::FloatLiteral: return "FloatLiteral";
    case TokenKind::String: return "String";
    case TokenKind::Create: return "Create";
    case TokenKind::Table: return "Table";
    case TokenKind::Alter: return "Alter";
    case TokenKind::Add: return "Add";
    case TokenKind::Column: return "Column";
    case TokenKind::Rename: return "Rename";
    case TokenKind::To: return "To";
    case TokenKind::Drop: return "Drop";
    case TokenKind::If: return "If";
    case TokenKind::Exists: return "Exists";
    case TokenKind::Insert: return "Insert";
    case TokenKind::Into: return "Into";
    case TokenKind::Values: return "Values";
    case TokenKind::Select: return "Select";
    case TokenKind::Distinct: return "Distinct";
    case TokenKind::Union: return "Union";
    case TokenKind::Intersect: return "Intersect";
    case TokenKind::Except: return "Except";
    case TokenKind::All: return "All";
    case TokenKind::From: return "From";
    case TokenKind::Where: return "Where";
    case TokenKind::Having: return "Having";
    case TokenKind::Explain: return "Explain";
    case TokenKind::Analyze: return "Analyze";
    case TokenKind::Update: return "Update";
    case TokenKind::Set: return "Set";
    case TokenKind::Delete: return "Delete";
    case TokenKind::Join: return "Join";
    case TokenKind::Inner: return "Inner";
    case TokenKind::Left: return "Left";
    case TokenKind::Right: return "Right";
    case TokenKind::Full: return "Full";
    case TokenKind::Outer: return "Outer";
    case TokenKind::On: return "On";
    case TokenKind::Group: return "Group";
    case TokenKind::Order: return "Order";
    case TokenKind::By: return "By";
    case TokenKind::Asc: return "Asc";
    case TokenKind::Desc: return "Desc";
    case TokenKind::As: return "As";
    case TokenKind::Is: return "Is";
    case TokenKind::Limit: return "Limit";
    case TokenKind::Offset: return "Offset";
    case TokenKind::Primary: return "Primary";
    case TokenKind::Key: return "Key";
    case TokenKind::Unique: return "Unique";
    case TokenKind::Default: return "Default";
    case TokenKind::Int: return "Int";
    case TokenKind::Varchar: return "Varchar";
    case TokenKind::Bool: return "Bool";
    case TokenKind::Float: return "Float";
    case TokenKind::Null: return "Null";
    case TokenKind::True: return "True";
    case TokenKind::False: return "False";
    case TokenKind::And: return "And";
    case TokenKind::Or: return "Or";
    case TokenKind::Not: return "Not";
    case TokenKind::Like: return "Like";
    case TokenKind::Between: return "Between";
    case TokenKind::In: return "In";
    case TokenKind::Case: return "Case";
    case TokenKind::When: return "When";
    case TokenKind::Then: return "Then";
    case TokenKind::Else: return "Else";
    case TokenKind::End: return "End";
    case TokenKind::Count: return "Count";
    case TokenKind::Sum: return "Sum";
    case TokenKind::Avg: return "Avg";
    case TokenKind::Min: return "Min";
    case TokenKind::Max: return "Max";
    case TokenKind::Equal: return "Equal";
    case TokenKind::NotEqual: return "NotEqual";
    case TokenKind::Less: return "Less";
    case TokenKind::LessEqual: return "LessEqual";
    case TokenKind::Greater: return "Greater";
    case TokenKind::GreaterEqual: return "GreaterEqual";
    case TokenKind::Plus: return "Plus";
    case TokenKind::Minus: return "Minus";
    case TokenKind::Star: return "Star";
    case TokenKind::Slash: return "Slash";
    case TokenKind::LeftParen: return "LeftParen";
    case TokenKind::RightParen: return "RightParen";
    case TokenKind::Comma: return "Comma";
    case TokenKind::Dot: return "Dot";
    case TokenKind::Semicolon: return "Semicolon";
    }
    return "Token";
}

std::string stageName(DiagnosticStage stage) {
    switch (stage) {
    case DiagnosticStage::Lexical: return "Lexical";
    case DiagnosticStage::Syntax: return "Syntax";
    case DiagnosticStage::Semantic: return "Semantic";
    case DiagnosticStage::Plan: return "Plan";
    case DiagnosticStage::Execution: return "Execution";
    }
    return "Unknown";
}

std::string errorName(ErrorCode code) {
    switch (code) {
    case ErrorCode::InvalidCharacter: return "InvalidCharacter";
    case ErrorCode::UnterminatedString: return "UnterminatedString";
    case ErrorCode::UnterminatedComment: return "UnterminatedComment";
    case ErrorCode::UnexpectedToken: return "UnexpectedToken";
    case ErrorCode::IntegerOutOfRange: return "IntegerOutOfRange";
    case ErrorCode::ExpressionTooDeep: return "ExpressionTooDeep";
    case ErrorCode::NotImplemented: return "NotImplemented";
    case ErrorCode::UnsupportedFeature: return "UnsupportedFeature";
    case ErrorCode::DuplicateIndex: return "DuplicateIndex";
    case ErrorCode::IndexNotFound: return "IndexNotFound";
    case ErrorCode::UnsupportedIndex: return "UnsupportedIndex";
    default: return "Error";
    }
}

std::string typeName(DataType type) {
    switch (type) {
    case DataType::Int: return "INT";
    case DataType::Varchar: return "VARCHAR";
    case DataType::Bool: return "BOOL";
    case DataType::Float: return "FLOAT";
    case DataType::Null: return "NULL";
    }
    return "TYPE";
}

std::string columnTypeName(const ColumnDefinition& column) {
    std::string name = typeName(column.type);
    if (column.type == DataType::Varchar && column.varchar_length) {
        name += "(" + std::to_string(*column.varchar_length) + ")";
    }
    return name;
}

std::string unaryName(UnaryOp op) {
    switch (op) {
    case UnaryOp::Negate: return "Negate";
    case UnaryOp::Not: return "Not";
    case UnaryOp::IsNull: return "IsNull";
    case UnaryOp::IsNotNull: return "IsNotNull";
    }
    return "Unary";
}

std::string binaryName(BinaryOp op) {
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
    return "Binary";
}

std::string aggregateName(AggregateFunction function) {
    switch (function) {
    case AggregateFunction::Count: return "COUNT";
    case AggregateFunction::Sum: return "SUM";
    case AggregateFunction::Avg: return "AVG";
    case AggregateFunction::Min: return "MIN";
    case AggregateFunction::Max: return "MAX";
    }
    return "AGG";
}

std::string joinTypeName(JoinType type) {
    switch (type) {
    case JoinType::Inner: return "Inner";
    case JoinType::Left: return "Left";
    case JoinType::Right: return "Right";
    case JoinType::Full: return "Full";
    }
    return "Join";
}

std::string setOperatorName(const SetOperation& operation) {
    switch (operation.op) {
    case SetOperator::Union: return operation.all ? "UnionAll" : "Union";
    case SetOperator::Intersect: return operation.all ? "IntersectAll" : "Intersect";
    case SetOperator::Except: return operation.all ? "ExceptAll" : "Except";
    }
    return "SetOperation";
}

std::string tableConstraintName(TableConstraintKind kind) {
    switch (kind) {
    case TableConstraintKind::PrimaryKey: return "PRIMARY KEY";
    case TableConstraintKind::Unique: return "UNIQUE";
    }
    return "CONSTRAINT";
}

std::string selectSourceName(const SelectStmt& stmt) {
    if (stmt.from.subquery) return "<derived>";
    if (!stmt.from.table.text.empty()) return stmt.from.table.text;
    return stmt.table.text;
}

std::string joinSourceName(const JoinClause& join) {
    if (join.source.subquery) return "<derived>";
    if (!join.source.table.text.empty()) return join.source.table.text;
    return join.table.text;
}

void printIndent(int indent) {
    for (int i = 0; i < indent; ++i) {
        std::cout << "  ";
    }
}

void printLiteral(const LiteralValue& value) {
    std::visit([](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::string>) {
            std::cout << "'" << item << "'";
        } else if constexpr (std::is_same_v<T, NullValue>) {
            std::cout << "NULL";
        } else if constexpr (std::is_same_v<T, bool>) {
            std::cout << (item ? "TRUE" : "FALSE");
        } else {
            std::cout << item;
        }
    }, value);
}

void printColumnDefinition(const ColumnDefinition& column) {
    std::cout << column.name.text << " " << columnTypeName(column);
    if (column.primary_key) std::cout << " PRIMARY KEY";
    if (column.not_null) std::cout << " NOT NULL";
    if (column.unique) std::cout << " UNIQUE";
    if (column.default_value) {
        std::cout << " DEFAULT ";
        printLiteral(column.default_value->value);
    }
}

void printAggregateCall(const AggregateCall& aggregate) {
    std::cout << aggregateName(aggregate.function) << "(";
    if (std::holds_alternative<AllColumns>(aggregate.argument)) {
        std::cout << "*";
    } else {
        std::cout << std::get<Identifier>(aggregate.argument).text;
    }
    std::cout << ")";
}

void printDiagnostic(const Diagnostic& diagnostic) {
    std::cerr << stageName(diagnostic.stage) << "Error";
    if (diagnostic.span) {
        std::cerr << " at line " << diagnostic.span->begin.line
                  << ", column " << diagnostic.span->begin.column;
    }
    std::cerr << " [" << errorName(diagnostic.code) << "]: "
              << diagnostic.message << '\n';
}

void printTokens(const TokenStream& tokens) {
    std::cout << "Token Stream\n";
    for (const Token& token : tokens) {
        std::cout << "  " << tokenName(token.kind) << " \"" << token.lexeme << "\""
                  << " (" << token.span.begin.line << "," << token.span.begin.column
                  << ")-(" << token.span.end.line << "," << token.span.end.column
                  << ")\n";
    }
}

void printExpr(const ExprPtr& expr, int indent) {
    if (!expr) {
        printIndent(indent);
        std::cout << "<none>\n";
        return;
    }
    std::visit([indent](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, IdentifierExpr>) {
            printIndent(indent);
            std::cout << "IdentifierExpr " << node.name.text << '\n';
        } else if constexpr (std::is_same_v<T, LiteralExpr>) {
            printIndent(indent);
            std::cout << "LiteralExpr ";
            printLiteral(node.value);
            std::cout << '\n';
        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
            printIndent(indent);
            std::cout << "UnaryExpr " << unaryName(node.op) << '\n';
            printExpr(node.operand, indent + 1);
        } else if constexpr (std::is_same_v<T, BinaryExpr>) {
            printIndent(indent);
            std::cout << "BinaryExpr " << binaryName(node.op) << '\n';
            printExpr(node.left, indent + 1);
            printExpr(node.right, indent + 1);
        } else if constexpr (std::is_same_v<T, AggregateCall>) {
            printIndent(indent);
            std::cout << "AggregateExpr ";
            printAggregateCall(node);
            std::cout << '\n';
        } else if constexpr (std::is_same_v<T, InSubqueryExpr>) {
            printIndent(indent);
            std::cout << (node.negated ? "NotInSubqueryExpr" : "InSubqueryExpr") << '\n';
            printExpr(node.value, indent + 1);
            printIndent(indent + 1);
            std::cout << "SubquerySelect from " << selectSourceName(*node.query) << '\n';
        } else if constexpr (std::is_same_v<T, ExistsSubqueryExpr>) {
            printIndent(indent);
            std::cout << (node.negated ? "NotExistsSubqueryExpr" : "ExistsSubqueryExpr") << '\n';
            printIndent(indent + 1);
            std::cout << "SubquerySelect from " << selectSourceName(*node.query) << '\n';
        } else if constexpr (std::is_same_v<T, ScalarSubqueryExpr>) {
            printIndent(indent);
            std::cout << "ScalarSubqueryExpr\n";
            printIndent(indent + 1);
            std::cout << "SubquerySelect from " << selectSourceName(*node.query) << '\n';
        } else if constexpr (std::is_same_v<T, CaseExpr>) {
            printIndent(indent);
            std::cout << "CaseExpr\n";
            if (node.operand) {
                printIndent(indent + 1);
                std::cout << "Operand\n";
                printExpr(node.operand, indent + 2);
            }
            for (const auto& branch : node.branches) {
                printIndent(indent + 1);
                std::cout << "When\n";
                printExpr(branch.condition, indent + 2);
                printIndent(indent + 1);
                std::cout << "Then\n";
                printExpr(branch.result, indent + 2);
            }
            if (node.else_result) {
                printIndent(indent + 1);
                std::cout << "Else\n";
                printExpr(node.else_result, indent + 2);
            }
        }
    }, expr->node);
}

void printWhere(const ExprPtr& where, int indent) {
    if (where) {
        printIndent(indent);
        std::cout << "Where\n";
        printExpr(where, indent + 1);
    }
}

void printExprInline(const ExprPtr& expr) {
    if (!expr) {
        std::cout << "<none>";
        return;
    }
    std::visit([](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, IdentifierExpr>) {
            std::cout << node.name.text;
        } else if constexpr (std::is_same_v<T, LiteralExpr>) {
            printLiteral(node.value);
        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
            std::cout << unaryName(node.op) << "(";
            printExprInline(node.operand);
            std::cout << ")";
        } else if constexpr (std::is_same_v<T, BinaryExpr>) {
            std::cout << "(";
            printExprInline(node.left);
            std::cout << " " << binaryName(node.op) << " ";
            printExprInline(node.right);
            std::cout << ")";
        } else if constexpr (std::is_same_v<T, AggregateCall>) {
            printAggregateCall(node);
        } else if constexpr (std::is_same_v<T, InSubqueryExpr>) {
            printExprInline(node.value);
            std::cout << (node.negated ? " NOT IN " : " IN ") << "(SELECT ...)";
        } else if constexpr (std::is_same_v<T, ExistsSubqueryExpr>) {
            std::cout << (node.negated ? "NOT EXISTS " : "EXISTS ") << "(SELECT ...)";
        } else if constexpr (std::is_same_v<T, ScalarSubqueryExpr>) {
            std::cout << "(SELECT ...)";
        } else if constexpr (std::is_same_v<T, CaseExpr>) {
            std::cout << "CASE ... END";
        }
    }, expr->node);
}

void printSelectItem(const SelectItem& item) {
    std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Identifier>) {
            std::cout << value.text;
        } else if constexpr (std::is_same_v<T, AggregateCall>) {
            printAggregateCall(value);
        } else {
            printExprInline(value);
        }
    }, item);
}

void printStatement(const Statement& statement, int index, bool print_header = true) {
    if (print_header) std::cout << "Statement " << index << '\n';
    std::visit([](const auto& stmt) {
        using T = std::decay_t<decltype(stmt)>;
        if constexpr (std::is_same_v<T, CreateTableStmt>) {
            printIndent(1);
            std::cout << "CreateTable";
            if (stmt.if_not_exists) std::cout << " IF NOT EXISTS";
            std::cout << " " << stmt.table.text << '\n';
            for (const auto& column : stmt.columns) {
                printIndent(2);
                std::cout << "Column ";
                printColumnDefinition(column);
                std::cout << '\n';
            }
            for (const auto& constraint : stmt.table_constraints) {
                printIndent(2);
                std::cout << "TableConstraint " << tableConstraintName(constraint.kind) << " (";
                for (std::size_t i = 0; i < constraint.columns.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::cout << constraint.columns[i].text;
                }
                std::cout << ")\n";
            }
        } else if constexpr (std::is_same_v<T, CreateIndexStmt>) {
            printIndent(1);
            std::cout << "CreateIndex " << stmt.index.text << " ON "
                      << stmt.table.text << "(" << stmt.column.text << ")\n";
        } else if constexpr (std::is_same_v<T, AlterTableStmt>) {
            printIndent(1);
            std::cout << "AlterTable " << stmt.table.text << '\n';
            std::visit([](const auto& action) {
                using Action = std::decay_t<decltype(action)>;
                if constexpr (std::is_same_v<Action, AlterAddColumn>) {
                    printIndent(2);
                    std::cout << "AddColumn";
                    if (action.column_keyword) std::cout << " COLUMN";
                    std::cout << " ";
                    printColumnDefinition(action.column);
                    std::cout << '\n';
                } else if constexpr (std::is_same_v<Action, AlterDropColumn>) {
                    printIndent(2);
                    std::cout << "DropColumn";
                    if (action.column_keyword) std::cout << " COLUMN";
                    std::cout << " " << action.column.text << '\n';
                } else if constexpr (std::is_same_v<Action, AlterRenameTable>) {
                    printIndent(2);
                    std::cout << "RenameTable TO " << action.new_name.text << '\n';
                } else if constexpr (std::is_same_v<Action, AlterRenameColumn>) {
                    printIndent(2);
                    std::cout << "RenameColumn " << action.old_name.text
                              << " TO " << action.new_name.text << '\n';
                }
            }, stmt.action);
        } else if constexpr (std::is_same_v<T, DropTableStmt>) {
            printIndent(1);
            std::cout << "DropTable";
            if (stmt.if_exists) std::cout << " IF EXISTS";
            for (const auto& table : stmt.tables) {
                std::cout << " " << table.text;
            }
            std::cout << '\n';
        } else if constexpr (std::is_same_v<T, DropIndexStmt>) {
            printIndent(1);
            std::cout << "DropIndex";
            if (stmt.if_exists) std::cout << " IF EXISTS";
            std::cout << " " << stmt.index.text << '\n';
        } else if constexpr (std::is_same_v<T, InsertStmt>) {
            printIndent(1);
            std::cout << "Insert " << stmt.table.text << '\n';
            if (stmt.columns) {
                printIndent(2);
                std::cout << "Columns";
                for (const auto& column : *stmt.columns) {
                    std::cout << " " << column.text;
                }
                std::cout << '\n';
            }
            if (stmt.rows.size() > 1) {
                printIndent(2);
                std::cout << "Rows\n";
                for (std::size_t row_index = 0; row_index < stmt.rows.size(); ++row_index) {
                    printIndent(3);
                    std::cout << "Row " << (row_index + 1);
                    for (const auto& value : stmt.rows[row_index]) {
                        std::cout << " ";
                        printLiteral(value.value);
                    }
                    std::cout << '\n';
                }
            } else {
                const auto& values = stmt.rows.empty() ? stmt.values : stmt.rows.front();
                printIndent(2);
                std::cout << "Values";
                for (const auto& value : values) {
                    std::cout << " ";
                    printLiteral(value.value);
                }
                std::cout << '\n';
            }
        } else if constexpr (std::is_same_v<T, SelectStmt>) {
            printIndent(1);
            std::cout << "Select";
            if (stmt.distinct) std::cout << " DISTINCT";
            std::cout << " from " << selectSourceName(stmt);
            const auto& table_alias = stmt.from.alias ? stmt.from.alias : stmt.table_alias;
            if (table_alias) std::cout << " AS " << table_alias->text;
            std::cout << '\n';
            if (stmt.from.subquery) {
                printIndent(2);
                std::cout << "DerivedTable\n";
                printIndent(3);
                std::cout << "SubquerySelect from " << selectSourceName(*stmt.from.subquery) << '\n';
            }
            printIndent(2);
            std::cout << "Columns";
            if (std::holds_alternative<AllColumns>(stmt.columns)) {
                std::cout << " *";
            } else if (std::holds_alternative<std::vector<Identifier>>(stmt.columns)) {
                const auto& columns = std::get<std::vector<Identifier>>(stmt.columns);
                for (std::size_t i = 0; i < columns.size(); ++i) {
                    std::cout << " " << columns[i].text;
                    if (!stmt.column_aliases.empty() && stmt.column_aliases[i])
                        std::cout << " AS " << stmt.column_aliases[i]->text;
                }
            } else {
                const auto& items = std::get<std::vector<SelectItem>>(stmt.columns);
                for (std::size_t i = 0; i < items.size(); ++i) {
                    std::cout << " ";
                    printSelectItem(items[i]);
                    if (!stmt.column_aliases.empty() && stmt.column_aliases[i])
                        std::cout << " AS " << stmt.column_aliases[i]->text;
                }
            }
            std::cout << '\n';
            if (!stmt.joins.empty()) {
                printIndent(2);
                std::cout << "Joins\n";
                for (const auto& join : stmt.joins) {
                    printIndent(3);
                    std::cout << joinTypeName(join.type) << "Join " << joinSourceName(join);
                    const auto& join_alias = join.source.alias ? join.source.alias : join.alias;
                    if (join_alias) std::cout << " AS " << join_alias->text;
                    std::cout << "\n";
                    if (join.source.subquery) {
                        printIndent(4);
                        std::cout << "DerivedTable\n";
                        printIndent(5);
                        std::cout << "SubquerySelect from "
                                  << selectSourceName(*join.source.subquery) << '\n';
                    }
                    printExpr(join.on, 4);
                }
            }
            printWhere(stmt.where, 2);
            if (!stmt.group_by.empty()) {
                printIndent(2);
                std::cout << "GroupBy";
                for (const auto& column : stmt.group_by) std::cout << " " << column.text;
                std::cout << '\n';
            }
            if (stmt.having) {
                printIndent(2);
                std::cout << "Having\n";
                printExpr(stmt.having, 3);
            }
            if (!stmt.order_by.empty()) {
                printIndent(2);
                std::cout << "OrderBy\n";
                for (const auto& item : stmt.order_by) {
                    printIndent(3);
                    if (item.expression) printExprInline(item.expression);
                    else std::cout << item.column.text;
                    std::cout << " "
                              << (item.direction == SortDirection::Asc ? "ASC" : "DESC") << '\n';
                }
            }
            if (stmt.limit) {
                printIndent(2);
                std::cout << "Limit " << *stmt.limit << '\n';
                if (stmt.offset) {
                    printIndent(2);
                    std::cout << "Offset " << *stmt.offset << '\n';
                }
            }
            if (!stmt.set_operations.empty()) {
                printIndent(2);
                std::cout << "SetOperations\n";
                for (const auto& operation : stmt.set_operations) {
                    printIndent(3);
                    std::cout << setOperatorName(operation) << '\n';
                    printIndent(4);
                    std::cout << "Select from " << selectSourceName(*operation.query) << '\n';
                }
            }
        } else if constexpr (std::is_same_v<T, UpdateStmt>) {
            printIndent(1);
            std::cout << "Update " << stmt.table.text;
            if (stmt.table_alias) std::cout << " AS " << stmt.table_alias->text;
            std::cout << '\n';
            for (const auto& assignment : stmt.assignments) {
                printIndent(2);
                std::cout << "Set " << assignment.target.text << '\n';
                printExpr(assignment.value, 3);
            }
            printWhere(stmt.where, 2);
        } else if constexpr (std::is_same_v<T, DeleteStmt>) {
            printIndent(1);
            std::cout << "Delete from " << stmt.table.text;
            if (stmt.table_alias) std::cout << " AS " << stmt.table_alias->text;
            std::cout << '\n';
            printWhere(stmt.where, 2);
        } else if constexpr (std::is_same_v<T, ExplainStmt>) {
            printIndent(1);
            std::cout << (stmt.analyze ? "ExplainAnalyze\n" : "Explain\n");
            std::visit([](const auto& target) {
                printStatement(Statement{target, {}}, 0, false);
            }, stmt.target);
        }
    }, statement.node);
}

void printAst(const std::vector<Statement>& statements, const char* title) {
    std::cout << title << '\n';
    if (statements.empty()) {
        std::cout << "  <empty>\n";
        return;
    }
    int index = 1;
    for (const Statement& statement : statements) {
        printStatement(statement, index++);
    }
}

} // namespace

enum class AstOutputMode { Both, RawOnly, OptimizedOnly };

int main(int argc, char* argv[]) {
    AstOutputMode mode = AstOutputMode::Both;
    if (argc == 2) {
        const std::string option = argv[1];
        if (option == "--raw-only") mode = AstOutputMode::RawOnly;
        else if (option == "--optimized-only") mode = AstOutputMode::OptimizedOnly;
        else {
            std::cerr << "Usage: minisql [--raw-only|--optimized-only] < input.sql\n";
            return 2;
        }
    } else if (argc != 1) {
        std::cerr << "Usage: minisql [--raw-only|--optimized-only] < input.sql\n";
        return 2;
    }

    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    const std::string sql = buffer.str();

    auto lexed = lex(sql);
    if (const auto* diagnostic = std::get_if<Diagnostic>(&lexed)) {
        printDiagnostic(*diagnostic);
        return 1;
    }
    const TokenStream& tokens = std::get<TokenStream>(lexed);
    printTokens(tokens);

    auto parsed = parse(tokens);
    if (const auto* diagnostic = std::get_if<Diagnostic>(&parsed)) {
        printDiagnostic(*diagnostic);
        return 1;
    }
    const auto& statements = std::get<std::vector<Statement>>(parsed);
    if (mode != AstOutputMode::OptimizedOnly) printAst(statements, "AST");
    if (mode != AstOutputMode::RawOnly)
        printAst(optimizeAstStatements(statements), "Optimized AST");
    return 0;
}
