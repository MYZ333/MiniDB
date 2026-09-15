// A 负责：按 grammar.md 实现递归下降、优先级、多语句和 AST 构造。
#include "minisql/parser.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#include <type_traits>
#include <unordered_map>

namespace minisql {
namespace {

bool isComparison(TokenKind kind) {
    return kind == TokenKind::Equal || kind == TokenKind::NotEqual ||
           kind == TokenKind::Less || kind == TokenKind::LessEqual ||
           kind == TokenKind::Greater || kind == TokenKind::GreaterEqual ||
           kind == TokenKind::Like;
}

bool isAggregateFunction(TokenKind kind) {
    return kind == TokenKind::Count || kind == TokenKind::Sum ||
           kind == TokenKind::Avg || kind == TokenKind::Min ||
           kind == TokenKind::Max;
}

bool isSetOperator(TokenKind kind) {
    return kind == TokenKind::Union || kind == TokenKind::Intersect ||
           kind == TokenKind::Except;
}

SourceLocation locationOf(const Token& token) { return token.span; }

SourceLocation merge(SourceLocation left, SourceLocation right) {
    if (!left) {
        return right;
    }
    if (!right) {
        return left;
    }
    return SourceSpan{left->begin, right->end};
}

bool sameLocation(const SourceLocation& left, const SourceLocation& right) {
    if (!left || !right) {
        return !left && !right;
    }
    return left->begin.offset == right->begin.offset && left->end.offset == right->end.offset;
}

std::string tokenName(TokenKind kind) {
    switch (kind) {
    case TokenKind::EndOfInput: return "end of input";
    case TokenKind::Identifier: return "identifier";
    case TokenKind::Integer: return "integer";
    case TokenKind::FloatLiteral: return "float";
    case TokenKind::String: return "string";
    case TokenKind::Create: return "CREATE";
    case TokenKind::Table: return "TABLE";
    case TokenKind::Index: return "INDEX";
    case TokenKind::Alter: return "ALTER";
    case TokenKind::Add: return "ADD";
    case TokenKind::Column: return "COLUMN";
    case TokenKind::Rename: return "RENAME";
    case TokenKind::To: return "TO";
    case TokenKind::Drop: return "DROP";
    case TokenKind::If: return "IF";
    case TokenKind::Exists: return "EXISTS";
    case TokenKind::Insert: return "INSERT";
    case TokenKind::Into: return "INTO";
    case TokenKind::Values: return "VALUES";
    case TokenKind::Select: return "SELECT";
    case TokenKind::Distinct: return "DISTINCT";
    case TokenKind::Union: return "UNION";
    case TokenKind::Intersect: return "INTERSECT";
    case TokenKind::Except: return "EXCEPT";
    case TokenKind::All: return "ALL";
    case TokenKind::From: return "FROM";
    case TokenKind::Where: return "WHERE";
    case TokenKind::Having: return "HAVING";
    case TokenKind::Explain: return "EXPLAIN";
    case TokenKind::Analyze: return "ANALYZE";
    case TokenKind::Update: return "UPDATE";
    case TokenKind::Set: return "SET";
    case TokenKind::Delete: return "DELETE";
    case TokenKind::Join: return "JOIN";
    case TokenKind::Inner: return "INNER";
    case TokenKind::Left: return "LEFT";
    case TokenKind::Right: return "RIGHT";
    case TokenKind::Full: return "FULL";
    case TokenKind::Outer: return "OUTER";
    case TokenKind::On: return "ON";
    case TokenKind::Group: return "GROUP";
    case TokenKind::Order: return "ORDER";
    case TokenKind::By: return "BY";
    case TokenKind::Asc: return "ASC";
    case TokenKind::Desc: return "DESC";
    case TokenKind::As: return "AS";
    case TokenKind::Is: return "IS";
    case TokenKind::Limit: return "LIMIT";
    case TokenKind::Offset: return "OFFSET";
    case TokenKind::Primary: return "PRIMARY";
    case TokenKind::Key: return "KEY";
    case TokenKind::Unique: return "UNIQUE";
    case TokenKind::Default: return "DEFAULT";
    case TokenKind::Int: return "INT";
    case TokenKind::Varchar: return "VARCHAR";
    case TokenKind::Bool: return "BOOL";
    case TokenKind::Float: return "FLOAT";
    case TokenKind::Null: return "NULL";
    case TokenKind::True: return "TRUE";
    case TokenKind::False: return "FALSE";
    case TokenKind::And: return "AND";
    case TokenKind::Or: return "OR";
    case TokenKind::Not: return "NOT";
    case TokenKind::Like: return "LIKE";
    case TokenKind::Between: return "BETWEEN";
    case TokenKind::In: return "IN";
    case TokenKind::Case: return "CASE";
    case TokenKind::When: return "WHEN";
    case TokenKind::Then: return "THEN";
    case TokenKind::Else: return "ELSE";
    case TokenKind::End: return "END";
    case TokenKind::Count: return "COUNT";
    case TokenKind::Sum: return "SUM";
    case TokenKind::Avg: return "AVG";
    case TokenKind::Min: return "MIN";
    case TokenKind::Max: return "MAX";
    case TokenKind::Equal: return "=";
    case TokenKind::NotEqual: return "!=";
    case TokenKind::Less: return "<";
    case TokenKind::LessEqual: return "<=";
    case TokenKind::Greater: return ">";
    case TokenKind::GreaterEqual: return ">=";
    case TokenKind::Plus: return "+";
    case TokenKind::Minus: return "-";
    case TokenKind::Star: return "*";
    case TokenKind::Slash: return "/";
    case TokenKind::LeftParen: return "(";
    case TokenKind::RightParen: return ")";
    case TokenKind::Comma: return ",";
    case TokenKind::Dot: return ".";
    case TokenKind::Semicolon: return ";";
    }
    return "token";
}

std::string tokenDescription(const Token& token) {
    std::string result = tokenName(token.kind);
    if (token.kind != TokenKind::EndOfInput && !token.lexeme.empty()) {
        result += " \"";
        result += token.lexeme;
        result += "\"";
    }
    return result;
}

std::string joinExpectations(const std::vector<std::string>& expectations) {
    if (expectations.empty()) {
        return "valid syntax";
    }
    if (expectations.size() == 1) {
        return expectations.front();
    }
    std::string result;
    for (std::size_t i = 0; i < expectations.size(); ++i) {
        if (i > 0) {
            result += (i + 1 == expectations.size()) ? " or " : ", ";
        }
        result += expectations[i];
    }
    return result;
}

AggregateFunction aggregateFunctionFor(TokenKind kind) {
    switch (kind) {
    case TokenKind::Count: return AggregateFunction::Count;
    case TokenKind::Sum: return AggregateFunction::Sum;
    case TokenKind::Avg: return AggregateFunction::Avg;
    case TokenKind::Min: return AggregateFunction::Min;
    case TokenKind::Max: return AggregateFunction::Max;
    default: throw Diagnostic{DiagnosticStage::Syntax, ErrorCode::UnexpectedToken,
                              "token is not an aggregate function", std::nullopt};
    }
}

SetOperator setOperatorFor(TokenKind kind) {
    switch (kind) {
    case TokenKind::Union: return SetOperator::Union;
    case TokenKind::Intersect: return SetOperator::Intersect;
    case TokenKind::Except: return SetOperator::Except;
    default: throw Diagnostic{DiagnosticStage::Syntax, ErrorCode::UnexpectedToken,
                              "token is not a set operator", std::nullopt};
    }
}

BinaryOp binaryOpFor(TokenKind kind) {
    switch (kind) {
    case TokenKind::Plus: return BinaryOp::Add;
    case TokenKind::Minus: return BinaryOp::Subtract;
    case TokenKind::Star: return BinaryOp::Multiply;
    case TokenKind::Slash: return BinaryOp::Divide;
    case TokenKind::Equal: return BinaryOp::Equal;
    case TokenKind::NotEqual: return BinaryOp::NotEqual;
    case TokenKind::Less: return BinaryOp::Less;
    case TokenKind::LessEqual: return BinaryOp::LessEqual;
    case TokenKind::Greater: return BinaryOp::Greater;
    case TokenKind::GreaterEqual: return BinaryOp::GreaterEqual;
    case TokenKind::And: return BinaryOp::And;
    case TokenKind::Or: return BinaryOp::Or;
    case TokenKind::Like: return BinaryOp::Like;
    default: throw Diagnostic{DiagnosticStage::Syntax, ErrorCode::UnexpectedToken,
                              "token is not a binary operator", std::nullopt};
    }
}

struct TypeSpec {
    DataType type;
    std::optional<std::int64_t> varchar_length = {};
    SourceLocation span;
};

std::int64_t parseIntegerMagnitude(const Token& token, bool negative) {
    const std::string& text = token.lexeme;
    const std::string limit = negative ? "9223372036854775808" : "9223372036854775807";
    std::size_t first_digit = text.find_first_not_of('0');
    std::string significant = first_digit == std::string::npos ? "0" : text.substr(first_digit);
    if (significant.size() > limit.size() ||
        (significant.size() == limit.size() && significant > limit)) {
        throw Diagnostic{DiagnosticStage::Syntax, ErrorCode::IntegerOutOfRange,
                         "integer literal is out of int64 range", token.span};
    }

    std::uint64_t magnitude = 0;
    for (char ch : significant) {
        magnitude = magnitude * 10 + static_cast<unsigned>(ch - '0');
    }
    if (negative && magnitude == 9223372036854775808ull) {
        return std::numeric_limits<std::int64_t>::min();
    }
    const auto value = static_cast<std::int64_t>(magnitude);
    return negative ? -value : value;
}

double parseFloatValue(const Token& token, bool negative) {
    try {
        const double value = std::stod(token.lexeme);
        return negative ? -value : value;
    } catch (const std::out_of_range&) {
        throw Diagnostic{DiagnosticStage::Syntax, ErrorCode::IntegerOutOfRange,
                         "floating-point literal is out of range", token.span};
    }
}

std::string decodeString(const Token& token) {
    std::string decoded;
    for (std::size_t i = 1; i + 1 < token.lexeme.size(); ++i) {
        if (token.lexeme[i] == '\'' && i + 1 < token.lexeme.size() - 1 &&
            token.lexeme[i + 1] == '\'') {
            decoded.push_back('\'');
            ++i;
        } else {
            decoded.push_back(token.lexeme[i]);
        }
    }
    return decoded;
}

class Parser {
public:
    explicit Parser(const TokenStream& tokens) : tokens_(tokens) {}

    Result<std::vector<Statement>> run() {
        try {
            if (tokens_.empty()) {
                return syntaxError("input must end with EOF token", std::nullopt);
            }
            // 对接契约要求唯一 EOF 位于末尾，不能悄悄忽略 EOF 后面的输入。
            if (tokens_.back().kind != TokenKind::EndOfInput) {
                return syntaxError("input must end with EOF token", tokens_.back().span);
            }
            for (std::size_t i = 0; i + 1 < tokens_.size(); ++i) {
                if (tokens_[i].kind == TokenKind::EndOfInput) {
                    return syntaxError("EOF token must appear only at the end", tokens_[i].span);
                }
            }

            std::vector<Statement> statements;
            while (!check(TokenKind::EndOfInput)) {
                statements.push_back(statement());
            }
            return statements;
        } catch (const Diagnostic& diagnostic) {
            return diagnostic;
        }
    }

private:
    // 限制 NOT、负号、括号的递归嵌套；异常退出时也自动恢复计数。
    class NestingGuard {
    public:
        NestingGuard(std::size_t& depth, SourceLocation span) : depth_(depth) {
            if (depth_ >= 256) {
                throw Diagnostic{DiagnosticStage::Syntax, ErrorCode::ExpressionTooDeep,
                                 "expression nesting exceeds 256 levels", span};
            }
            ++depth_;
        }
        ~NestingGuard() { --depth_; }
        NestingGuard(const NestingGuard&) = delete;
        NestingGuard& operator=(const NestingGuard&) = delete;
    private:
        std::size_t& depth_;
    };

    // 循环解析的 a+a+... 也能形成深树。构造时记录高度，在树过深前拒绝，
    // 避免后续分析、打印或 shared_ptr 释放深链时耗尽栈；不改变公共 AST。
    ExprPtr makeExpr(Expr expr) {
        const auto depth = std::visit([&](const auto& node) -> std::size_t {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, UnaryExpr>) {
                return depths_.at(node.operand.get()) + 1;
            } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                const auto left = depths_.at(node.left.get());
                const auto right = depths_.at(node.right.get());
                return (left > right ? left : right) + 1;
            } else if constexpr (std::is_same_v<T, InSubqueryExpr>) {
                return depths_.at(node.value.get()) + 1;
            } else if constexpr (std::is_same_v<T, CaseExpr>) {
                std::size_t child_depth = node.operand ? depths_.at(node.operand.get()) : 0;
                for (const auto& branch : node.branches) {
                    const auto condition = depths_.at(branch.condition.get());
                    const auto result = depths_.at(branch.result.get());
                    child_depth = std::max(child_depth, std::max(condition, result));
                }
                if (node.else_result) {
                    child_depth = std::max(child_depth, depths_.at(node.else_result.get()));
                }
                return child_depth + 1;
            } else return 1;
        }, expr.node);
        if (depth > 256) {
            throw Diagnostic{DiagnosticStage::Syntax, ErrorCode::ExpressionTooDeep,
                             "expression AST exceeds 256 levels", expr.span};
        }
        auto result = std::make_shared<const Expr>(std::move(expr));
        depths_[result.get()] = depth;
        return result;
    }

    const Token& current() const {
        if (position_ < tokens_.size()) {
            return tokens_[position_];
        }
        return tokens_.back();
    }

    const Token& previous() const { return tokens_[position_ - 1]; }

    bool check(TokenKind kind) const { return current().kind == kind; }

    bool match(TokenKind kind) {
        if (!check(kind)) {
            return false;
        }
        ++position_;
        return true;
    }

    const Token& consume(TokenKind kind, const std::string& expectation) {
        if (check(kind)) {
            ++position_;
            return previous();
        }
        throw unexpected({expectation});
    }

    const Token& consume(TokenKind kind, const std::vector<std::string>& expectations) {
        if (check(kind)) {
            ++position_;
            return previous();
        }
        throw unexpected(expectations);
    }

    Diagnostic unexpected(const std::vector<std::string>& expectations) const {
        return syntaxError("unexpected " + tokenDescription(current()) + ", expected " +
                               joinExpectations(expectations),
                          locationOf(current()));
    }

    Diagnostic syntaxError(std::string message, SourceLocation span) const {
        return Diagnostic{DiagnosticStage::Syntax, ErrorCode::UnexpectedToken,
                          std::move(message), std::move(span)};
    }

    // 基础语句不消费分号，EXPLAIN 因此能复用同一组完整的语法分支。
    ExplainTarget baseStatement() {
        if (match(TokenKind::Create)) {
            return createStatement();
        } else if (match(TokenKind::Alter)) {
            return alterStatement();
        } else if (match(TokenKind::Drop)) {
            return dropStatement();
        } else if (match(TokenKind::Insert)) {
            return insertStatement();
        } else if (match(TokenKind::Select)) {
            return selectStatement();
        } else if (match(TokenKind::Update)) {
            return updateStatement();
        } else if (match(TokenKind::Delete)) {
            return deleteStatement();
        }
        throw unexpected({"CREATE", "ALTER", "DROP", "INSERT", "SELECT", "UPDATE", "DELETE"});
    }

    Statement statement() {
        depths_.clear(); // 只需保留正在解析语句的高度元数据。
        const SourceLocation start = locationOf(current());
        Statement result;
        if (match(TokenKind::Explain)) {
            const bool analyze = match(TokenKind::Analyze);
            result.node = ExplainStmt{baseStatement(), analyze};
        } else {
            auto base = baseStatement();
            std::visit([&](auto node) { result.node = std::move(node); }, std::move(base));
        }
        const Token& semicolon = consume(TokenKind::Semicolon, "';'");
        result.span = merge(start, locationOf(semicolon));
        return result;
    }

    Identifier identifier() {
        const Token& first = consume(TokenKind::Identifier, "identifier");
        std::string text = first.lexeme;
        SourceLocation span = first.span;
        // A 将限定名保留为一段原文，B 再按点分离限定符并做名称绑定。
        while (match(TokenKind::Dot)) {
            const Token& part = consume(TokenKind::Identifier, "identifier after '.'");
            text += "." + part.lexeme;
            span = merge(span, part.span);
        }
        return Identifier{std::move(text), span};
    }

    // 别名只能是一段普通标识符，不能写成带点限定名。
    Identifier simpleIdentifier() {
        const Token& token = consume(TokenKind::Identifier, "alias");
        return Identifier{token.lexeme, token.span};
    }

    std::optional<Identifier> optionalAlias() {
        if (match(TokenKind::As)) return simpleIdentifier();
        if (check(TokenKind::Identifier)) return simpleIdentifier();
        return std::nullopt;
    }

    TypeSpec typeName() {
        if (match(TokenKind::Int)) {
            return TypeSpec{DataType::Int, std::nullopt, locationOf(previous())};
        }
        if (match(TokenKind::Varchar)) {
            const Token& varchar = previous();
            SourceLocation end = locationOf(varchar);
            std::optional<std::int64_t> length;
            if (match(TokenKind::LeftParen)) {
                const Token& size = consume(TokenKind::Integer, "positive integer length");
                const auto parsed = parseIntegerMagnitude(size, false);
                if (parsed <= 0) {
                    throw syntaxError("VARCHAR length must be a positive integer",
                                      locationOf(size));
                }
                length = parsed;
                const Token& right = consume(TokenKind::RightParen, "')'");
                end = locationOf(right);
            }
            return TypeSpec{DataType::Varchar, length, merge(locationOf(varchar), end)};
        }
        if (match(TokenKind::Bool)) return TypeSpec{DataType::Bool, std::nullopt, locationOf(previous())};
        if (match(TokenKind::Float)) return TypeSpec{DataType::Float, std::nullopt, locationOf(previous())};
        throw unexpected({"INT", "VARCHAR", "BOOL", "FLOAT"});
    }

    ExplainTarget createStatement() {
        if (match(TokenKind::Index)) {
            return createIndexStatement();
        }
        consume(TokenKind::Table, std::vector<std::string>{"TABLE", "INDEX"});
        const bool if_not_exists = createIfNotExists();
        CreateTableStmt stmt{identifier(), {}};
        stmt.if_not_exists = if_not_exists;
        consume(TokenKind::LeftParen, "'('");
        createTableItem(stmt);
        while (match(TokenKind::Comma)) {
            createTableItem(stmt);
        }
        consume(TokenKind::RightParen, "')'");
        return stmt;
    }

    CreateIndexStmt createIndexStatement() {
        Identifier index = identifier();
        consume(TokenKind::On, "ON");
        Identifier table = identifier();
        consume(TokenKind::LeftParen, "'('");
        Identifier column = identifier();
        if (match(TokenKind::Comma)) {
            throw syntaxError("CREATE INDEX supports exactly one column in this version",
                              locationOf(previous()));
        }
        consume(TokenKind::RightParen, "')'");
        return CreateIndexStmt{std::move(index), std::move(table), std::move(column)};
    }

    bool createIfNotExists() {
        if (!match(TokenKind::If)) {
            return false;
        }
        consume(TokenKind::Not, "NOT after IF");
        consume(TokenKind::Exists, "EXISTS after IF NOT");
        return true;
    }

    void createTableItem(CreateTableStmt& stmt) {
        if (check(TokenKind::Primary) || check(TokenKind::Unique)) {
            stmt.table_constraints.push_back(tableConstraint());
            return;
        }
        stmt.columns.push_back(columnDefinition());
    }

    TableConstraint tableConstraint() {
        const SourceLocation start = locationOf(current());
        TableConstraintKind kind = TableConstraintKind::Unique;
        if (match(TokenKind::Primary)) {
            kind = TableConstraintKind::PrimaryKey;
            consume(TokenKind::Key, "KEY after PRIMARY");
        } else {
            consume(TokenKind::Unique, "UNIQUE");
        }
        consume(TokenKind::LeftParen, "'(' after table constraint");
        std::vector<Identifier> columns = names();
        const Token& right = consume(TokenKind::RightParen, "')'");
        return TableConstraint{kind, std::move(columns), merge(start, locationOf(right))};
    }

    AlterTableStmt alterStatement() {
        consume(TokenKind::Table, "TABLE");
        Identifier table = identifier();
        if (match(TokenKind::Add)) {
            const bool explicit_column = match(TokenKind::Column);
            ColumnDefinition column = columnDefinition();
            return AlterTableStmt{std::move(table),
                                  AlterAddColumn{std::move(column), explicit_column}};
        }
        if (match(TokenKind::Drop)) {
            const bool explicit_column = match(TokenKind::Column);
            Identifier column = identifier();
            return AlterTableStmt{std::move(table),
                                  AlterDropColumn{std::move(column), explicit_column}};
        }
        if (match(TokenKind::Rename)) {
            if (match(TokenKind::Column)) {
                Identifier old_name = identifier();
                consume(TokenKind::To, "TO after renamed column");
                Identifier new_name = identifier();
                return AlterTableStmt{std::move(table),
                                      AlterRenameColumn{std::move(old_name), std::move(new_name)}};
            }
            consume(TokenKind::To, "TO after RENAME");
            Identifier new_name = identifier();
            return AlterTableStmt{std::move(table), AlterRenameTable{std::move(new_name)}};
        }
        throw unexpected({"ADD", "DROP", "RENAME"});
    }

    ExplainTarget dropStatement() {
        if (match(TokenKind::Index)) {
            return dropIndexStatement();
        }
        consume(TokenKind::Table, std::vector<std::string>{"TABLE", "INDEX"});
        DropTableStmt stmt{{}, false};
        if (match(TokenKind::If)) {
            consume(TokenKind::Exists, "EXISTS after IF");
            stmt.if_exists = true;
        }
        stmt.tables = names();
        return stmt;
    }

    DropIndexStmt dropIndexStatement() {
        DropIndexStmt stmt{{}, false};
        if (match(TokenKind::If)) {
            consume(TokenKind::Exists, "EXISTS after IF");
            stmt.if_exists = true;
        }
        stmt.index = identifier();
        return stmt;
    }

    ColumnDefinition columnDefinition() {
        const SourceLocation start = locationOf(current());
        Identifier name = identifier();
        const TypeSpec type = typeName();
        ColumnDefinition column{std::move(name), type.type, merge(start, type.span),
                                type.varchar_length};
        columnConstraints(column);
        column.span = merge(start, previous().span);
        return column;
    }

    void columnConstraints(ColumnDefinition& column) {
        while (check(TokenKind::Primary) || check(TokenKind::Not) ||
               check(TokenKind::Unique) || check(TokenKind::Default)) {
            if (match(TokenKind::Primary)) {
                const Token& primary = previous();
                consume(TokenKind::Key, "KEY after PRIMARY");
                if (column.primary_key) {
                    throw syntaxError("duplicate PRIMARY KEY constraint", locationOf(primary));
                }
                column.primary_key = true;
            } else if (match(TokenKind::Not)) {
                const Token& not_token = previous();
                consume(TokenKind::Null, "NULL after NOT");
                if (column.not_null) {
                    throw syntaxError("duplicate NOT NULL constraint", locationOf(not_token));
                }
                column.not_null = true;
            } else if (match(TokenKind::Unique)) {
                const Token& unique = previous();
                if (column.unique) {
                    throw syntaxError("duplicate UNIQUE constraint", locationOf(unique));
                }
                column.unique = true;
            } else {
                const Token& default_token = consume(TokenKind::Default, "DEFAULT");
                if (column.default_value) {
                    throw syntaxError("duplicate DEFAULT constraint", locationOf(default_token));
                }
                column.default_value = literal();
            }
        }
    }

    InsertStmt insertStatement() {
        consume(TokenKind::Into, "INTO");
        InsertStmt stmt{identifier(), std::nullopt, {}};
        if (match(TokenKind::LeftParen)) {
            std::vector<Identifier> columns;
            columns.push_back(identifier());
            while (match(TokenKind::Comma)) {
                columns.push_back(identifier());
            }
            consume(TokenKind::RightParen, "')'");
            stmt.columns = std::move(columns);
        }
        consume(TokenKind::Values, "VALUES");
        std::vector<std::vector<LocatedLiteral>> rows;
        rows.push_back(insertValueRow());
        while (match(TokenKind::Comma)) {
            rows.push_back(insertValueRow());
        }
        stmt.values = rows.front(); // 兼容旧 B：单行读取路径仍可使用 values。
        stmt.rows = std::move(rows);
        return stmt;
    }

    std::vector<LocatedLiteral> insertValueRow() {
        consume(TokenKind::LeftParen, "'('");
        if (check(TokenKind::RightParen)) {
            throw unexpected({"literal value"});
        }
        std::vector<LocatedLiteral> row;
        row.push_back(literal());
        while (match(TokenKind::Comma)) {
            row.push_back(literal());
        }
        consume(TokenKind::RightParen, "')'");
        return row;
    }

    LocatedLiteral literal() {
        bool negative = false;
        SourceLocation start = locationOf(current());
        if (match(TokenKind::Minus)) {
            negative = true;
        }
        if (match(TokenKind::Integer)) {
            const Token& token = previous();
            return LocatedLiteral{parseIntegerMagnitude(token, negative),
                                  merge(start, locationOf(token))};
        }
        if (match(TokenKind::FloatLiteral)) {
            const Token& token = previous();
            return LocatedLiteral{parseFloatValue(token, negative), merge(start, locationOf(token))};
        }
        if (!negative && match(TokenKind::String)) {
            const Token& token = previous();
            return LocatedLiteral{decodeString(token), token.span};
        }
        if (!negative && match(TokenKind::True)) return LocatedLiteral{true, previous().span};
        if (!negative && match(TokenKind::False)) return LocatedLiteral{false, previous().span};
        if (!negative && match(TokenKind::Null)) return LocatedLiteral{NullValue{}, previous().span};
        throw unexpected({"integer", "float", "string", "TRUE", "FALSE", "NULL"});
    }

    SelectStmt selectStatement() {
        SelectStmt stmt = selectCore();
        while (isSetOperator(current().kind)) {
            const Token& op = current();
            ++position_;
            const bool all = match(TokenKind::All);
            const SourceLocation operator_span = all
                ? merge(locationOf(op), locationOf(previous()))
                : locationOf(op);
            consume(TokenKind::Select, "SELECT after set operator");
            auto query = std::make_shared<const SelectStmt>(selectCore());
            stmt.set_operations.push_back(SetOperation{
                setOperatorFor(op.kind), all, std::move(query), operator_span});
        }
        return stmt;
    }

    SelectStmt selectCore() {
        const bool distinct = match(TokenKind::Distinct);
        auto [columns, aliases] = selectList();
        consume(TokenKind::From, "FROM");
        TableRef from = tableRef();
        Identifier legacy_table = from.table;
        auto legacy_alias = from.alias;
        SelectStmt stmt{std::move(legacy_table), std::move(columns), nullptr, {}, {}, {},
                        std::move(legacy_alias), std::move(aliases)};
        stmt.from = std::move(from);
        stmt.distinct = distinct;
        selectTailClauses(stmt);
        rejectMisorderedSelectClause();
        return stmt;
    }

    std::pair<SelectList, std::vector<std::optional<Identifier>>> selectList() {
        if (match(TokenKind::Star)) {
            return {AllColumns{locationOf(previous())}, {}};
        }

        std::vector<SelectItem> selected;
        std::vector<std::optional<Identifier>> aliases;
        selected.push_back(selectItem());
        bool has_extended_item = !std::holds_alternative<Identifier>(selected.back());
        aliases.push_back(optionalAlias());
        while (match(TokenKind::Comma)) {
            selected.push_back(selectItem());
            has_extended_item = has_extended_item ||
                                !std::holds_alternative<Identifier>(selected.back());
            aliases.push_back(optionalAlias());
        }
        if (has_extended_item) {
            return {SelectList{std::move(selected)}, std::move(aliases)};
        }

        std::vector<Identifier> names_only;
        names_only.reserve(selected.size());
        for (auto& item : selected) {
            names_only.push_back(std::get<Identifier>(std::move(item)));
        }
        return {SelectList{std::move(names_only)}, std::move(aliases)};
    }

    void selectTailClauses(SelectStmt& stmt) {
        while (startsJoinClause()) stmt.joins.push_back(joinClause());
        if (match(TokenKind::Where)) {
            stmt.where = expression();
        }
        if (match(TokenKind::Group)) stmt.group_by = groupByList();
        if (match(TokenKind::Having)) stmt.having = expression();
        if (match(TokenKind::Order)) stmt.order_by = orderByList();
        limitClause(stmt);
    }

    void limitClause(SelectStmt& stmt) {
        if (match(TokenKind::Limit)) {
            stmt.limit = nonNegativeInteger("LIMIT");
            if (match(TokenKind::Offset)) {
                stmt.offset = nonNegativeInteger("OFFSET");
            }
        }
    }

    SelectItem selectItem() {
        if (check(TokenKind::Star)) {
            throw unexpected({"column name", "aggregate function", "expression"});
        }
        ExprPtr expr = expression();
        if (const auto* identifier = std::get_if<IdentifierExpr>(&expr->node)) {
            if (sameLocation(identifier->name.span, expr->span)) {
                return identifier->name;
            }
        }
        if (const auto* aggregate = std::get_if<AggregateCall>(&expr->node)) {
            if (sameLocation(aggregate->span, expr->span)) {
                return *aggregate;
            }
        }
        return expr;
    }

    AggregateCall aggregateCall() {
        const Token& function = current();
        ++position_;
        consume(TokenKind::LeftParen, "'('");
        std::variant<AllColumns, Identifier> argument;
        if (match(TokenKind::Star)) {
            const Token& star = previous();
            if (function.kind != TokenKind::Count) {
                throw syntaxError(tokenName(function.kind) + "(*) is not supported, expected column name",
                                  locationOf(star));
            }
            argument = AllColumns{locationOf(star)};
        } else {
            argument = identifier();
        }
        const Token& right = consume(TokenKind::RightParen, "')'");
        return AggregateCall{aggregateFunctionFor(function.kind), std::move(argument),
                             merge(locationOf(function), locationOf(right))};
    }

    std::int64_t nonNegativeInteger(const char* clause) {
        const Token& token = consume(TokenKind::Integer,
                                     {std::string{clause} + " non-negative integer value"});
        return parseIntegerMagnitude(token, false);
    }

    bool startsJoinClause() const {
        return check(TokenKind::Join) || check(TokenKind::Inner) || check(TokenKind::Left) ||
               check(TokenKind::Right) || check(TokenKind::Full);
    }

    TableRef tableRef() {
        const SourceLocation start = locationOf(current());
        if (match(TokenKind::LeftParen)) {
            consume(TokenKind::Select, "SELECT in derived table");
            auto query = std::make_shared<const SelectStmt>(selectStatement());
            const Token& right = consume(TokenKind::RightParen, "')' after derived table subquery");
            auto alias = optionalAlias();
            if (!alias) {
                throw syntaxError("derived table requires an alias, expected AS alias or alias",
                                  locationOf(right));
            }
            const SourceLocation span = merge(start, alias->span);
            return TableRef{Identifier{}, std::move(query), std::move(alias), span};
        }

        Identifier table = identifier();
        auto alias = optionalAlias();
        const SourceLocation span = alias ? merge(table.span, alias->span) : table.span;
        return TableRef{table, nullptr, std::move(alias), span};
    }

    JoinClause joinClause() {
        const Token& start = current();
        JoinType type = JoinType::Inner;
        if (match(TokenKind::Join)) {
            type = JoinType::Inner;
        } else if (match(TokenKind::Inner)) {
            consume(TokenKind::Join, "JOIN");
        } else if (match(TokenKind::Left)) {
            type = JoinType::Left;
            match(TokenKind::Outer);
            consume(TokenKind::Join, "JOIN");
        } else if (match(TokenKind::Right)) {
            type = JoinType::Right;
            match(TokenKind::Outer);
            consume(TokenKind::Join, "JOIN");
        } else {
            consume(TokenKind::Full, "FULL");
            type = JoinType::Full;
            match(TokenKind::Outer);
            consume(TokenKind::Join, "JOIN");
        }
        TableRef source = tableRef();
        Identifier legacy_table = source.table;
        auto legacy_alias = source.alias;
        consume(TokenKind::On, "ON");
        ExprPtr on = expression();
        const SourceLocation span = merge(start.span, on->span);
        JoinClause join{std::move(legacy_table), std::move(on), span, std::move(legacy_alias), type};
        join.source = std::move(source);
        return join;
    }

    void rejectMisorderedSelectClause() const {
        if (check(TokenKind::Join) || check(TokenKind::Inner) || check(TokenKind::Left) ||
            check(TokenKind::Right) || check(TokenKind::Full) || check(TokenKind::Where) ||
            check(TokenKind::Group) || check(TokenKind::Having) || check(TokenKind::Order) ||
            check(TokenKind::Limit) || check(TokenKind::Offset)) {
            throw syntaxError("unexpected " + tokenDescription(current()) +
                                  " in SELECT clause order, expected JOIN -> WHERE -> GROUP BY -> HAVING -> ORDER BY -> LIMIT/OFFSET",
                              locationOf(current()));
        }
    }

    std::vector<Identifier> groupByList() {
        consume(TokenKind::By, "BY");
        return names();
    }

    std::vector<OrderByItem> orderByList() {
        consume(TokenKind::By, "BY");
        std::vector<OrderByItem> result{orderByItem()};
        while (match(TokenKind::Comma)) result.push_back(orderByItem());
        return result;
    }

    OrderByItem orderByItem() {
        const SourceLocation start = locationOf(current());
        ExprPtr expr = expression();
        Identifier column;
        if (const auto* identifier = std::get_if<IdentifierExpr>(&expr->node)) {
            if (sameLocation(identifier->name.span, expr->span)) {
                column = identifier->name;
                expr = nullptr;
            }
        }
        SortDirection direction = SortDirection::Asc;
        SourceLocation end = expr ? expr->span : column.span;
        if (match(TokenKind::Asc)) end = previous().span;
        else if (match(TokenKind::Desc)) {
            direction = SortDirection::Desc;
            end = previous().span;
        }
        return OrderByItem{std::move(column), direction, merge(start, end), std::move(expr)};
    }

    UpdateStmt updateStatement() {
        UpdateStmt stmt{identifier(), {}, nullptr};
        stmt.table_alias = optionalAlias();
        consume(TokenKind::Set, "SET");
        stmt.assignments.push_back(assignment());
        while (match(TokenKind::Comma)) {
            stmt.assignments.push_back(assignment());
        }
        if (match(TokenKind::Where)) {
            stmt.where = expression();
        }
        return stmt;
    }

    Assignment assignment() {
        const SourceLocation start = locationOf(current());
        Identifier target = identifier();
        consume(TokenKind::Equal, "'='");
        ExprPtr value = expression();
        SourceLocation span = merge(start, value->span);
        return Assignment{std::move(target), std::move(value), std::move(span)};
    }

    DeleteStmt deleteStatement() {
        consume(TokenKind::From, "FROM");
        DeleteStmt stmt{identifier(), nullptr};
        stmt.table_alias = optionalAlias();
        if (match(TokenKind::Where)) {
            stmt.where = expression();
        }
        return stmt;
    }

    std::vector<Identifier> names() {
        std::vector<Identifier> result;
        result.push_back(identifier());
        while (match(TokenKind::Comma)) {
            result.push_back(identifier());
        }
        return result;
    }

    ExprPtr expression() { return orExpression(); }

    ExprPtr orExpression() {
        ExprPtr expr = andExpression();
        while (match(TokenKind::Or)) {
            const Token& op = previous(); // 先固定操作符，再解析会改变游标的右侧。
            auto right = andExpression();
            expr = binaryExpression(BinaryOp::Or, std::move(expr), std::move(right), op);
        }
        return expr;
    }

    ExprPtr andExpression() {
        ExprPtr expr = notExpression();
        while (match(TokenKind::And)) {
            const Token& op = previous();
            auto right = notExpression();
            expr = binaryExpression(BinaryOp::And, std::move(expr), std::move(right), op);
        }
        return expr;
    }

    ExprPtr notExpression() {
        if (match(TokenKind::Not)) {
            const Token& op = previous();
            if (check(TokenKind::Exists)) {
                return existsSubquery(&op);
            }
            const NestingGuard guard(nesting_, op.span);
            ExprPtr operand = notExpression();
            return makeExpr(Expr{
                UnaryExpr{UnaryOp::Not, operand, op.span}, merge(locationOf(op), operand->span)});
        }
        return comparison();
    }

    ExprPtr comparison() {
        ExprPtr expr = additive();
        if (isComparison(current().kind)) {
            const Token& op = current();
            ++position_;
            expr = binaryExpression(binaryOpFor(op.kind), std::move(expr), additive(), op);
            if (isComparison(current().kind)) {
                throw syntaxError("comparison operators cannot be chained", locationOf(current()));
            }
        } else if (match(TokenKind::Not)) {
            const Token& not_token = previous();
            if (match(TokenKind::Like)) {
                const Token& like_token = previous();
                ExprPtr like = binaryExpression(BinaryOp::Like, std::move(expr), additive(), like_token);
                const SourceLocation span = like->span;
                expr = makeExpr(Expr{UnaryExpr{UnaryOp::Not, std::move(like), not_token.span},
                                     span});
            } else if (match(TokenKind::Between)) {
                expr = betweenExpression(std::move(expr), previous(), &not_token);
            } else if (match(TokenKind::In)) {
                expr = inExpression(std::move(expr), previous(), &not_token);
            } else {
                throw unexpected({"LIKE after NOT", "BETWEEN after NOT", "IN after NOT"});
            }
        } else if (match(TokenKind::Between)) {
            expr = betweenExpression(std::move(expr), previous(), nullptr);
        } else if (match(TokenKind::In)) {
            expr = inExpression(std::move(expr), previous(), nullptr);
        } else if (match(TokenKind::Is)) {
            const Token& is_token = previous();
            const bool negated = match(TokenKind::Not);
            const SourceLocation operator_span = negated
                ? merge(locationOf(is_token), locationOf(previous()))
                : locationOf(is_token);
            const Token& null_token = consume(TokenKind::Null, "NULL");
            const SourceLocation span = merge(expr->span, locationOf(null_token));
            expr = makeExpr(Expr{
                UnaryExpr{negated ? UnaryOp::IsNotNull : UnaryOp::IsNull,
                          std::move(expr), operator_span},
                span});
        }
        return expr;
    }

    ExprPtr betweenExpression(ExprPtr value, const Token& between_token,
                              const Token* not_token) {
        ExprPtr lower = additive();
        const Token& and_token = consume(TokenKind::And, "AND in BETWEEN expression");
        ExprPtr upper = additive();
        ExprPtr lower_bound = binaryExpression(BinaryOp::GreaterEqual, value, std::move(lower),
                                               between_token);
        ExprPtr upper_bound = binaryExpression(BinaryOp::LessEqual, std::move(value),
                                               std::move(upper), between_token);
        ExprPtr combined = binaryExpression(BinaryOp::And, std::move(lower_bound),
                                            std::move(upper_bound), and_token);
        if (!not_token) {
            return combined;
        }
        const SourceLocation span = combined->span;
        return makeExpr(Expr{UnaryExpr{UnaryOp::Not, std::move(combined), not_token->span},
                             span});
    }

    ExprPtr literalExpression() {
        LocatedLiteral value = literal();
        return makeExpr(Expr{LiteralExpr{std::move(value.value)}, value.span});
    }

    ExprPtr caseExpression() {
        const Token& case_token = consume(TokenKind::Case, "CASE");
        ExprPtr operand = nullptr;
        if (!check(TokenKind::When)) {
            operand = expression();
        }

        std::vector<CaseWhenClause> branches;
        while (check(TokenKind::When)) {
            const SourceLocation when_span = locationOf(consume(TokenKind::When, "WHEN"));
            ExprPtr condition = expression();
            consume(TokenKind::Then, "THEN after CASE WHEN condition");
            ExprPtr result = expression();
            const SourceLocation branch_span = merge(when_span, result->span);
            branches.push_back(CaseWhenClause{
                std::move(condition), std::move(result), branch_span});
        }
        if (branches.empty()) {
            throw unexpected({"WHEN"});
        }

        ExprPtr else_result = nullptr;
        if (match(TokenKind::Else)) {
            else_result = expression();
        }
        const Token& end_token = consume(TokenKind::End, "END after CASE expression");
        const SourceLocation span = merge(locationOf(case_token), locationOf(end_token));
        return makeExpr(Expr{
            CaseExpr{std::move(operand), std::move(branches), std::move(else_result), span},
            span});
    }

    ExprPtr existsSubquery(const Token* not_token) {
        const SourceLocation start = not_token ? locationOf(*not_token) : locationOf(current());
        const Token& exists_token = consume(TokenKind::Exists, "EXISTS");
        consume(TokenKind::LeftParen, "'(' after EXISTS");
        consume(TokenKind::Select, "SELECT in EXISTS subquery");
        auto query = std::make_shared<const SelectStmt>(selectStatement());
        const Token& right = consume(TokenKind::RightParen, "')' after subquery");
        const SourceLocation operator_span = not_token
            ? merge(locationOf(*not_token), locationOf(exists_token))
            : locationOf(exists_token);
        return makeExpr(Expr{
            ExistsSubqueryExpr{std::move(query), not_token != nullptr, operator_span},
            merge(start, locationOf(right))});
    }

    ExprPtr scalarSubquery(SourceLocation start) {
        consume(TokenKind::Select, "SELECT in scalar subquery");
        auto query = std::make_shared<const SelectStmt>(selectStatement());
        const Token& right = consume(TokenKind::RightParen, "')' after scalar subquery");
        const SourceLocation span = merge(start, locationOf(right));
        return makeExpr(Expr{ScalarSubqueryExpr{std::move(query), span}, span});
    }

    ExprPtr inExpression(ExprPtr value, const Token& in_token, const Token* not_token) {
        consume(TokenKind::LeftParen, "'('");
        if (match(TokenKind::Select)) {
            auto query = std::make_shared<const SelectStmt>(selectStatement());
            const Token& right = consume(TokenKind::RightParen, "')' after subquery");
            const SourceLocation operator_span = not_token
                ? merge(locationOf(*not_token), locationOf(in_token))
                : locationOf(in_token);
            const SourceLocation span = merge(value->span, locationOf(right));
            return makeExpr(Expr{
                InSubqueryExpr{std::move(value), std::move(query), not_token != nullptr,
                               operator_span},
                span});
        }
        if (check(TokenKind::RightParen)) {
            throw unexpected({"literal value"});
        }

        ExprPtr result = binaryExpression(BinaryOp::Equal, value, literalExpression(), in_token);
        while (match(TokenKind::Comma)) {
            ExprPtr item = binaryExpression(BinaryOp::Equal, value, literalExpression(), in_token);
            result = binaryExpression(BinaryOp::Or, std::move(result), std::move(item), in_token);
        }
        consume(TokenKind::RightParen, "')'");
        if (!not_token) {
            return result;
        }
        const SourceLocation span = result->span;
        return makeExpr(Expr{UnaryExpr{UnaryOp::Not, std::move(result), not_token->span},
                             span});
    }

    ExprPtr additive() {
        ExprPtr expr = term();
        while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
            const Token& op = current();
            ++position_;
            expr = binaryExpression(binaryOpFor(op.kind), std::move(expr), term(), op);
        }
        return expr;
    }

    ExprPtr term() {
        ExprPtr expr = unary();
        while (check(TokenKind::Star) || check(TokenKind::Slash)) {
            const Token& op = current();
            ++position_;
            expr = binaryExpression(binaryOpFor(op.kind), std::move(expr), unary(), op);
        }
        return expr;
    }

    ExprPtr unary() {
        if (match(TokenKind::Minus)) {
            const Token& op = previous();
            const NestingGuard guard(nesting_, op.span);
            if (match(TokenKind::Integer)) {
                const Token& integer = previous();
                return makeExpr(Expr{
                    LiteralExpr{parseIntegerMagnitude(integer, true)},
                    merge(locationOf(op), locationOf(integer))});
            }
            if (match(TokenKind::FloatLiteral)) {
                const Token& number = previous();
                return makeExpr(Expr{LiteralExpr{parseFloatValue(number, true)},
                                     merge(locationOf(op), locationOf(number))});
            }
            ExprPtr operand = unary();
            return makeExpr(Expr{
                UnaryExpr{UnaryOp::Negate, operand, op.span}, merge(locationOf(op), operand->span)});
        }
        return primary();
    }

    ExprPtr primary() {
        if (check(TokenKind::Case)) {
            return caseExpression();
        }
        if (check(TokenKind::Exists)) {
            return existsSubquery(nullptr);
        }
        if (isAggregateFunction(current().kind)) {
            AggregateCall aggregate = aggregateCall();
            const SourceLocation span = aggregate.span;
            return makeExpr(Expr{std::move(aggregate), span});
        }
        if (check(TokenKind::Identifier)) {
            Identifier name = identifier();
            const SourceLocation span = name.span;
            return makeExpr(Expr{IdentifierExpr{std::move(name)}, span});
        }
        if (match(TokenKind::Integer)) {
            const Token& token = previous();
            return makeExpr(Expr{
                LiteralExpr{parseIntegerMagnitude(token, false)}, token.span});
        }
        if (match(TokenKind::FloatLiteral)) {
            const Token& token = previous();
            return makeExpr(Expr{LiteralExpr{parseFloatValue(token, false)}, token.span});
        }
        if (match(TokenKind::String)) {
            const Token& token = previous();
            return makeExpr(Expr{LiteralExpr{decodeString(token)}, token.span});
        }
        if (match(TokenKind::True)) return makeExpr(Expr{LiteralExpr{true}, previous().span});
        if (match(TokenKind::False)) return makeExpr(Expr{LiteralExpr{false}, previous().span});
        if (match(TokenKind::Null)) return makeExpr(Expr{LiteralExpr{NullValue{}}, previous().span});
        if (match(TokenKind::LeftParen)) {
            const SourceLocation start = locationOf(previous());
            if (check(TokenKind::Select)) {
                return scalarSubquery(start);
            }
            const NestingGuard guard(nesting_, start);
            ExprPtr expr = expression();
            const Token& right = consume(TokenKind::RightParen, "')'");
            return makeExpr(Expr{expr->node, merge(start, locationOf(right))});
        }
        throw unexpected({"CASE", "identifier", "integer", "float", "string", "TRUE", "FALSE",
                          "NULL", "'-'", "'('"});
    }

    ExprPtr binaryExpression(BinaryOp op, ExprPtr left, ExprPtr right, const Token& token) {
        SourceLocation span = merge(left->span, right->span);
        return makeExpr(Expr{
            BinaryExpr{op, std::move(left), std::move(right), token.span},
            std::move(span)});
    }

    const TokenStream& tokens_;
    std::size_t position_ = 0;
    std::size_t nesting_ = 0;
    std::unordered_map<const Expr*, std::size_t> depths_;
};

} // namespace

Result<std::vector<Statement>> parse(const TokenStream& tokens) {
    Parser parser(tokens);
    return parser.run();
}

} // namespace minisql
