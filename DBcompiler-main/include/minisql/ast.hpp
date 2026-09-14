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
struct AggregateCall {
    Identifier function; // B 归一化并检查 COUNT/SUM/AVG/MIN/MAX。
    std::optional<Identifier> argument; // COUNT(*) 用 nullopt + count_star。
    bool count_star = false;
    SourceLocation span;
};
struct SelectItem {
    std::variant<Identifier, AggregateCall> value;
    std::optional<Identifier> alias;
    SourceLocation span;
};
// vector<Identifier> 保留旧手工 AST 的源码兼容；Parser 遇聚合时使用 SelectItem。
using SelectList = std::variant<AllColumns, std::vector<Identifier>, std::vector<SelectItem>>;

struct OrderByItem {
    Identifier column;
    SortDirection direction = SortDirection::Asc;
    SourceLocation span;
};

struct JoinClause {
    Identifier table;
    ExprPtr on;
    SourceLocation span;
    std::optional<Identifier> alias = {}; // 关系实例名；省略时使用真实表名。
};

struct SelectStmt {
    Identifier table;
    SelectList columns;
    ExprPtr where; // 空指针：没有 WHERE。其余表达式子节点必须非空。
    // 默认值保持旧的三字段聚合初始化源码兼容。
    std::vector<Identifier> group_by = {};
    std::vector<OrderByItem> order_by = {};
    std::vector<JoinClause> joins = {};
    std::optional<Identifier> table_alias = {};
    // 与显式 columns 一一对应；nullopt 表示该项沿用模式列名。
    std::vector<std::optional<Identifier>> column_aliases = {};
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
