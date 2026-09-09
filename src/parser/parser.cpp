// A 负责：按 grammar.md 实现递归下降、优先级、多语句和 AST 构造。
#include "minisql/parser.hpp"

#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <type_traits>
#include <unordered_map>

namespace minisql {
namespace {

bool isComparison(TokenKind kind) {
    return kind == TokenKind::Equal || kind == TokenKind::NotEqual ||
           kind == TokenKind::Less || kind == TokenKind::LessEqual ||
           kind == TokenKind::Greater || kind == TokenKind::GreaterEqual;
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

std::string tokenName(TokenKind kind) {
    switch (kind) {
    case TokenKind::EndOfInput: return "end of input";
    case TokenKind::Identifier: return "identifier";
    case TokenKind::Integer: return "integer";
    case TokenKind::String: return "string";
    case TokenKind::Create: return "CREATE";
    case TokenKind::Table: return "TABLE";
    case TokenKind::Insert: return "INSERT";
    case TokenKind::Into: return "INTO";
    case TokenKind::Values: return "VALUES";
    case TokenKind::Select: return "SELECT";
    case TokenKind::From: return "FROM";
    case TokenKind::Where: return "WHERE";
    case TokenKind::Update: return "UPDATE";
    case TokenKind::Set: return "SET";
    case TokenKind::Delete: return "DELETE";
    case TokenKind::Int: return "INT";
    case TokenKind::Varchar: return "VARCHAR";
    case TokenKind::And: return "AND";
    case TokenKind::Or: return "OR";
    case TokenKind::Not: return "NOT";
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
    case TokenKind::Semicolon: return ";";
    }
    return "token";
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
    default: throw Diagnostic{DiagnosticStage::Syntax, ErrorCode::UnexpectedToken,
                              "token is not a binary operator", std::nullopt};
    }
}

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
        throw syntaxError("unexpected " + tokenName(current().kind) + ", expected " + expectation,
                          locationOf(current()));
    }

    Diagnostic syntaxError(std::string message, SourceLocation span) const {
        return Diagnostic{DiagnosticStage::Syntax, ErrorCode::UnexpectedToken,
                          std::move(message), std::move(span)};
    }

    Statement statement() {
        depths_.clear(); // 只需保留正在解析语句的高度元数据。
        const SourceLocation start = locationOf(current());
        Statement result;
        if (match(TokenKind::Create)) {
            result.node = createStatement();
        } else if (match(TokenKind::Insert)) {
            result.node = insertStatement();
        } else if (match(TokenKind::Select)) {
            result.node = selectStatement();
        } else if (match(TokenKind::Update)) {
            result.node = updateStatement();
        } else if (match(TokenKind::Delete)) {
            result.node = deleteStatement();
        } else {
            throw syntaxError("unexpected " + tokenName(current().kind) +
                                  ", expected statement keyword",
                              locationOf(current()));
        }
        const Token& semicolon = consume(TokenKind::Semicolon, "';'");
        result.span = merge(start, locationOf(semicolon));
        return result;
    }

    Identifier identifier() {
        const Token& token = consume(TokenKind::Identifier, "identifier");
        return Identifier{token.lexeme, token.span};
    }

    DataType typeName() {
        if (match(TokenKind::Int)) {
            return DataType::Int;
        }
        if (match(TokenKind::Varchar)) {
            return DataType::Varchar;
        }
        throw syntaxError("unexpected " + tokenName(current().kind) + ", expected INT or VARCHAR",
                          locationOf(current()));
    }

    CreateTableStmt createStatement() {
        consume(TokenKind::Table, "TABLE");
        CreateTableStmt stmt{identifier(), {}};
        consume(TokenKind::LeftParen, "'('");
        stmt.columns.push_back(columnDefinition());
        while (match(TokenKind::Comma)) {
            stmt.columns.push_back(columnDefinition());
        }
        consume(TokenKind::RightParen, "')'");
        return stmt;
    }

    ColumnDefinition columnDefinition() {
        const SourceLocation start = locationOf(current());
        Identifier name = identifier();
        const DataType type = typeName();
        return ColumnDefinition{std::move(name), type, merge(start, locationOf(previous()))};
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
        consume(TokenKind::LeftParen, "'('");
        stmt.values.push_back(literal());
        while (match(TokenKind::Comma)) {
            stmt.values.push_back(literal());
        }
        consume(TokenKind::RightParen, "')'");
        return stmt;
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
        if (!negative && match(TokenKind::String)) {
            const Token& token = previous();
            return LocatedLiteral{decodeString(token), token.span};
        }
        throw syntaxError("unexpected " + tokenName(current().kind) +
                              ", expected integer or string literal",
                          locationOf(current()));
    }

    SelectStmt selectStatement() {
        SelectList columns;
        if (match(TokenKind::Star)) {
            columns = AllColumns{locationOf(previous())};
        } else {
            columns = names();
        }
        consume(TokenKind::From, "FROM");
        SelectStmt stmt{identifier(), std::move(columns), nullptr};
        if (match(TokenKind::Where)) {
            stmt.where = expression();
        }
        return stmt;
    }

    UpdateStmt updateStatement() {
        UpdateStmt stmt{identifier(), {}, nullptr};
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
        }
        return expr;
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
            ExprPtr operand = unary();
            return makeExpr(Expr{
                UnaryExpr{UnaryOp::Negate, operand, op.span}, merge(locationOf(op), operand->span)});
        }
        return primary();
    }

    ExprPtr primary() {
        if (match(TokenKind::Identifier)) {
            const Token& token = previous();
            return makeExpr(Expr{
                IdentifierExpr{Identifier{token.lexeme, token.span}}, token.span});
        }
        if (match(TokenKind::Integer)) {
            const Token& token = previous();
            return makeExpr(Expr{
                LiteralExpr{parseIntegerMagnitude(token, false)}, token.span});
        }
        if (match(TokenKind::String)) {
            const Token& token = previous();
            return makeExpr(Expr{LiteralExpr{decodeString(token)}, token.span});
        }
        if (match(TokenKind::LeftParen)) {
            const SourceLocation start = locationOf(previous());
            const NestingGuard guard(nesting_, start);
            ExprPtr expr = expression();
            const Token& right = consume(TokenKind::RightParen, "')'");
            return makeExpr(Expr{expr->node, merge(start, locationOf(right))});
        }
        throw syntaxError("unexpected " + tokenName(current().kind) +
                              ", expected identifier, literal, '-' or '('",
                          locationOf(current()));
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
