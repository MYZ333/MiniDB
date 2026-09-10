// A 阶段调试入口：从标准输入读取 SQL，输出 Token Stream 和 AST。
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
    case TokenKind::String: return "String";
    case TokenKind::Create: return "Create";
    case TokenKind::Table: return "Table";
    case TokenKind::Insert: return "Insert";
    case TokenKind::Into: return "Into";
    case TokenKind::Values: return "Values";
    case TokenKind::Select: return "Select";
    case TokenKind::From: return "From";
    case TokenKind::Where: return "Where";
    case TokenKind::Update: return "Update";
    case TokenKind::Set: return "Set";
    case TokenKind::Delete: return "Delete";
    case TokenKind::Int: return "Int";
    case TokenKind::Varchar: return "Varchar";
    case TokenKind::And: return "And";
    case TokenKind::Or: return "Or";
    case TokenKind::Not: return "Not";
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
    default: return "Error";
    }
}

std::string typeName(DataType type) {
    switch (type) {
    case DataType::Int: return "INT";
    case DataType::Varchar: return "VARCHAR";
    case DataType::Bool: return "BOOL";
    }
    return "TYPE";
}

std::string unaryName(UnaryOp op) {
    return op == UnaryOp::Negate ? "Negate" : "Not";
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
    }
    return "Binary";
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
        } else {
            std::cout << item;
        }
    }, value);
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

void printStatement(const Statement& statement, int index) {
    std::cout << "Statement " << index << '\n';
    std::visit([](const auto& stmt) {
        using T = std::decay_t<decltype(stmt)>;
        if constexpr (std::is_same_v<T, CreateTableStmt>) {
            printIndent(1);
            std::cout << "CreateTable " << stmt.table.text << '\n';
            for (const auto& column : stmt.columns) {
                printIndent(2);
                std::cout << "Column " << column.name.text << " " << typeName(column.type)
                          << '\n';
            }
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
            printIndent(2);
            std::cout << "Values";
            for (const auto& value : stmt.values) {
                std::cout << " ";
                printLiteral(value.value);
            }
            std::cout << '\n';
        } else if constexpr (std::is_same_v<T, SelectStmt>) {
            printIndent(1);
            std::cout << "Select from " << stmt.table.text << '\n';
            printIndent(2);
            std::cout << "Columns";
            if (std::holds_alternative<AllColumns>(stmt.columns)) {
                std::cout << " *";
            } else {
                for (const auto& column : std::get<std::vector<Identifier>>(stmt.columns)) {
                    std::cout << " " << column.text;
                }
            }
            std::cout << '\n';
            printWhere(stmt.where, 2);
        } else if constexpr (std::is_same_v<T, UpdateStmt>) {
            printIndent(1);
            std::cout << "Update " << stmt.table.text << '\n';
            for (const auto& assignment : stmt.assignments) {
                printIndent(2);
                std::cout << "Set " << assignment.target.text << '\n';
                printExpr(assignment.value, 3);
            }
            printWhere(stmt.where, 2);
        } else if constexpr (std::is_same_v<T, DeleteStmt>) {
            printIndent(1);
            std::cout << "Delete from " << stmt.table.text << '\n';
            printWhere(stmt.where, 2);
        }
    }, statement.node);
}

void printAst(const std::vector<Statement>& statements) {
    std::cout << "AST\n";
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

int main(int argc, char*[]) {
    if (argc != 1) {
        std::cerr << "Usage: minisql < input.sql\n";
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
    printAst(std::get<std::vector<Statement>>(parsed));
    return 0;
}
