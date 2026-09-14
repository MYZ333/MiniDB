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
    // COUNT(*) 使用 AllColumns；其他聚合只接受单个列名，B 检查类型和分组语义。
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
    // B 在 SELECT/HAVING/ORDER BY 中把调用绑定成 BoundAggregate 表达式叶节点。
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
    bool primary_key = false; // B 将其提升为 NOT NULL + UNIQUE；当前不自动建索引。
    bool not_null = false; // B/Catalog 保存，执行层在写入前检查。
    bool unique = false; // NULL 不参与唯一值冲突检查。
    std::optional<LocatedLiteral> default_value = {}; // B 检查类型并为省略的 INSERT 列填值。
};

struct CreateTableStmt {
    Identifier table;
    std::vector<ColumnDefinition> columns;
};

struct DropTableStmt {
    std::vector<Identifier> tables;
    bool if_exists = false; // true 时 B/Catalog/执行层忽略不存在的表。
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
    ExprPtr expression = nullptr; // 非空表示 ORDER BY 表达式；B 绑定为计算排序键。
};

struct JoinClause {
    Identifier table;
    ExprPtr on;
    SourceLocation span;
    std::optional<Identifier> alias = {}; // 关系实例名；省略时使用真实表名。
    JoinType type = JoinType::Inner; // B 原样传入计划，外连接缺失侧由执行层补 NULL。
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
    // 与显式 columns 一一对应（包括 SelectItem）；nullopt 沿用列名或函数展示名。
    std::vector<std::optional<Identifier>> column_aliases = {};
    // nullopt 表示未声明；B 在最终投影/聚合后应用 OFFSET 和 LIMIT。
    std::optional<std::int64_t> limit = {};
    std::optional<std::int64_t> offset = {};
    ExprPtr having = nullptr; // B 在分组上下文绑定，执行层仅保留 TRUE 的分组。
    bool distinct = false; // 最终输出去重，两个相同位置的 NULL 视为相等。
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

// EXPLAIN 只包裹一条基础语句，禁止继续嵌套 EXPLAIN，避免产生含糊的执行语义。
using ExplainTarget = std::variant<CreateTableStmt, DropTableStmt, InsertStmt, SelectStmt,
                                   UpdateStmt, DeleteStmt>;

struct ExplainStmt {
    ExplainTarget target;
    bool analyze = false; // true 时执行目标计划并收集每个算子的实际行数和耗时。
};

struct Statement {
    std::variant<CreateTableStmt, DropTableStmt, InsertStmt, SelectStmt, UpdateStmt, DeleteStmt,
                 ExplainStmt> node;
    SourceLocation span;
};

} // namespace minisql
