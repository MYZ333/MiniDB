#pragma once

// A → B：AST 表示 SQL 的语法结构，不负责确定名称指向哪个数据库对象。
#include "minisql/common.hpp"

#include <memory>

namespace minisql {

struct Expr;
using ExprPtr = std::shared_ptr<const Expr>;

struct IdentifierExpr { Identifier name; };
struct LiteralExpr { LiteralValue value; };

struct AllColumns { SourceLocation span; };

enum class AggregateFunction { Count, Sum, Avg, Min, Max };

struct AggregateCall {
    AggregateFunction function;
    // COUNT(*) 使用 AllColumns；其他聚合暂只接受单个列名，B 后续负责类型和分组语义。
    std::variant<AllColumns, Identifier> argument;
    SourceLocation span;
};

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
    // AggregateCall 允许 A 表达 HAVING COUNT(*) > 0、SELECT COUNT(*) + 1 等聚合表达式；
    // B 可先显式报 UnsupportedFeature，再逐步实现绑定和聚合计划。
    std::variant<IdentifierExpr, LiteralExpr, UnaryExpr, BinaryExpr, AggregateCall> node;
    SourceLocation span;
};

// INSERT 和 DEFAULT 都只允许字面量；单独保存每个值的位置，便于定位类型不匹配。
struct LocatedLiteral {
    LiteralValue value;
    SourceLocation span;
};

struct ColumnDefinition {
    Identifier name;
    DataType type;
    SourceLocation span;
    std::optional<std::int64_t> varchar_length = {}; // 仅 VARCHAR(n) 使用；nullopt 表示未声明长度。
    bool primary_key = false; // A 只保留列级 PRIMARY KEY 语法；B 后续决定唯一性和索引语义。
    bool not_null = false; // A 不做 NULL 约束检查；B/Catalog/执行层后续适配。
    bool unique = false; // 列级 UNIQUE 语法标记；是否与 PRIMARY KEY 合并由 B 决定。
    std::optional<LocatedLiteral> default_value = {}; // DEFAULT 字面量；类型兼容性由 B 检查。
};

struct CreateTableStmt {
    Identifier table;
    std::vector<ColumnDefinition> columns;
};

struct DropTableStmt {
    std::vector<Identifier> tables;
    bool if_exists = false; // A 只保留 IF EXISTS 语法；B 后续决定缺表时是否忽略。
};

struct InsertStmt {
    Identifier table;
    std::optional<std::vector<Identifier>> columns; // nullopt：省略列清单。
    std::vector<LocatedLiteral> values; // 兼容旧 B：Parser 会同步保存第一行值。
    std::vector<std::vector<LocatedLiteral>> rows = {}; // INSERT 多行；空表示沿用旧 values 字段。
};

using SelectItem = std::variant<Identifier, AggregateCall, ExprPtr>;
// 保留 std::vector<Identifier> 旧分支以兼容 B；出现聚合函数或表达式项时使用 std::vector<SelectItem>。
using SelectList = std::variant<AllColumns, std::vector<Identifier>, std::vector<SelectItem>>;

struct OrderByItem {
    Identifier column;
    SortDirection direction = SortDirection::Asc;
    SourceLocation span;
    ExprPtr expression = nullptr; // 非空表示 ORDER BY 表达式；B 当前可先显式 UnsupportedFeature。
};

enum class JoinType { Inner, Left, Right, Full };

struct JoinClause {
    Identifier table;
    ExprPtr on;
    SourceLocation span;
    std::optional<Identifier> alias = {}; // 关系实例名；省略时使用真实表名。
    JoinType type = JoinType::Inner; // A 只记录连接种类；B 后续决定外连接计划和 NULL 补齐语义。
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
    // LIMIT/OFFSET 只保存语法值；B 后续决定计划与执行语义。nullopt 表示未声明。
    std::optional<std::int64_t> limit = {};
    std::optional<std::int64_t> offset = {};
    ExprPtr having = nullptr; // HAVING 在 GROUP BY 后过滤分组；B 后续负责聚合语义绑定。
    bool distinct = false; // SELECT DISTINCT 标记；B 后续决定去重计划与 NULL 比较规则。
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
    std::optional<Identifier> table_alias = {}; // UPDATE 目标表别名；声明后限定列应使用别名。
};

struct DeleteStmt {
    Identifier table;
    ExprPtr where;
    std::optional<Identifier> table_alias = {}; // DELETE 目标表别名；声明后限定列应使用别名。
};

struct Statement {
    std::variant<CreateTableStmt, DropTableStmt, InsertStmt, SelectStmt, UpdateStmt, DeleteStmt> node;
    SourceLocation span;
};

} // namespace minisql
