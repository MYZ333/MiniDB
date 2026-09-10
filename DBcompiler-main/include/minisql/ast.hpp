#pragma once

// A → B：AST 表示 SQL 的语法结构，不负责确定名称指向哪个数据库对象。
#include "minisql/common.hpp"

#include <memory>

namespace minisql {

struct Expr;
using ExprPtr = std::shared_ptr<const Expr>;

struct IdentifierExpr { Identifier name; };
struct LiteralExpr { LiteralValue value; };

struct UnaryExpr {
    UnaryOp op;
    ExprPtr operand;
    SourceLocation operator_span; // 诊断指向 NOT 或负号。
};

struct BinaryExpr {
    BinaryOp op;
    ExprPtr left;
    ExprPtr right;
    SourceLocation operator_span; // 诊断指向具体操作符，而非整个 WHERE。
};

struct Expr {
    std::variant<IdentifierExpr, LiteralExpr, UnaryExpr, BinaryExpr> node;
    SourceLocation span;
};

struct ColumnDefinition {
    Identifier name;
    DataType type;
    SourceLocation span;
};

struct CreateTableStmt {
    Identifier table;
    std::vector<ColumnDefinition> columns;
};

// INSERT 只允许字面量；单独保存每个值的位置，便于定位类型不匹配。
struct LocatedLiteral {
    LiteralValue value;
    SourceLocation span;
};

struct InsertStmt {
    Identifier table;
    std::optional<std::vector<Identifier>> columns; // nullopt：省略列清单。
    std::vector<LocatedLiteral> values;
};

struct AllColumns { SourceLocation span; };
using SelectList = std::variant<AllColumns, std::vector<Identifier>>;

struct SelectStmt {
    Identifier table;
    SelectList columns;
    ExprPtr where; // 空指针：没有 WHERE。其余表达式子节点必须非空。
};

struct Assignment {
    Identifier target;
    ExprPtr value;
    SourceLocation span;
};

struct UpdateStmt {
    Identifier table;
    std::vector<Assignment> assignments;
    ExprPtr where;
};

struct DeleteStmt {
    Identifier table;
    ExprPtr where;
};

struct Statement {
    std::variant<CreateTableStmt, InsertStmt, SelectStmt, UpdateStmt, DeleteStmt> node;
    SourceLocation span;
};

} // namespace minisql
