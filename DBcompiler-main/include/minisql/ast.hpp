#pragma once

// A → B：AST 表示 SQL 的语法结构，不负责确定名称指向哪个数据库对象。
#include "minisql/common.hpp"

#include <memory>

namespace minisql {

struct Expr;
struct SelectStmt;
using ExprPtr = std::shared_ptr<const Expr>;
using SelectStmtPtr = std::shared_ptr<const SelectStmt>;

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

struct InSubqueryExpr {
    ExprPtr value;
    SelectStmtPtr query;
    bool negated = false; // 表示 NOT IN；A 不判断相关/非相关，B 后续绑定作用域。
    SourceLocation operator_span; // 指向 IN 或 NOT IN，便于 B 后续定位诊断。
};

struct ExistsSubqueryExpr {
    SelectStmtPtr query;
    bool negated = false; // 表示 NOT EXISTS；A 不判断相关/非相关，B 后续绑定作用域。
    SourceLocation operator_span; // 指向 EXISTS 或 NOT EXISTS，便于 B 后续定位诊断。
};

struct ScalarSubqueryExpr {
    SelectStmtPtr query;
    SourceLocation span; // 整个 (SELECT ...)；B 后续检查单行单列和返回类型。
};

struct CaseWhenClause {
    ExprPtr condition; // 搜索型 CASE 为布尔条件；简单型 CASE 为 WHEN 后的匹配值表达式。
    ExprPtr result;
    SourceLocation span; // 从 WHEN 到 THEN 结果表达式结束。
};

struct CaseExpr {
    ExprPtr operand = nullptr; // 非空表示简单型 CASE operand WHEN value THEN ...；空表示 CASE WHEN condition THEN ...
    std::vector<CaseWhenClause> branches;
    ExprPtr else_result = nullptr; // 空表示没有 ELSE，B 后续决定隐式 NULL 语义。
    SourceLocation span;
};

struct Expr {
    // AggregateCall 允许 A 表达 HAVING COUNT(*) > 0、SELECT COUNT(*) + 1 等聚合表达式；
    // B 可先显式报 UnsupportedFeature，再逐步实现绑定和聚合计划。
    std::variant<IdentifierExpr, LiteralExpr, UnaryExpr, BinaryExpr, AggregateCall, InSubqueryExpr, ExistsSubqueryExpr, ScalarSubqueryExpr, CaseExpr> node;
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

enum class TableConstraintKind { PrimaryKey, Unique };

struct TableConstraint {
    TableConstraintKind kind;
    std::vector<Identifier> columns;
    SourceLocation span; // 表级约束整体范围；具体列名位置保存在 columns 内。
};

struct CreateTableStmt {
    Identifier table;
    std::vector<ColumnDefinition> columns;
    std::vector<TableConstraint> table_constraints = {}; // 表级 PRIMARY KEY/UNIQUE；B 后续接 Catalog 约束。
    bool if_not_exists = false; // A 只保留 IF NOT EXISTS；B 后续决定已存在时的 no-op 表示。
};

struct AlterAddColumn {
    ColumnDefinition column;
    bool column_keyword = false; // 记录是否显式写了 COLUMN；B 通常只需读取 column。
};

struct AlterDropColumn {
    Identifier column;
    bool column_keyword = false; // 记录是否显式写了 COLUMN；B 通常只需读取 column。
};

struct AlterRenameTable {
    Identifier new_name;
};

struct AlterRenameColumn {
    Identifier old_name;
    Identifier new_name;
};

using AlterTableAction = std::variant<AlterAddColumn, AlterDropColumn, AlterRenameTable, AlterRenameColumn>;

struct AlterTableStmt {
    Identifier table;
    AlterTableAction action; // ALTER TABLE 的具体动作；B 后续逐个接 Catalog 变更语义。
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

struct TableRef {
    Identifier table; // 普通表名；派生表时为空，B 应读取 subquery 和 alias。
    SelectStmtPtr subquery = nullptr; // 非空表示 FROM/JOIN 中的派生表子查询。
    std::optional<Identifier> alias = {}; // 派生表必须有别名；普通表仍可省略。
    SourceLocation span; // 整个 table_ref 范围，便于 B 对派生表报 UnsupportedFeature。
};

struct OrderByItem {
    Identifier column;
    SortDirection direction = SortDirection::Asc;
    SourceLocation span;
    ExprPtr expression = nullptr; // 非空表示 ORDER BY 表达式；B 当前可先显式 UnsupportedFeature。
};

enum class JoinType { Inner, Left, Right, Full };
enum class SetOperator { Union, Intersect, Except };

struct SetOperation {
    SetOperator op = SetOperator::Union;
    bool all = false; // true 表示保留重复行；false 表示去重集合运算，B 后续决定去重计划。
    SelectStmtPtr query;
    SourceLocation operator_span; // 指向 UNION/INTERSECT/EXCEPT，便于 B 对集合运算定位诊断。
};

struct JoinClause {
    Identifier table;
    ExprPtr on;
    SourceLocation span;
    std::optional<Identifier> alias = {}; // 关系实例名；省略时使用真实表名。
    JoinType type = JoinType::Inner; // A 只记录连接种类；B 后续决定外连接计划和 NULL 补齐语义。
    TableRef source = {}; // 新 table_ref 表示；旧 table/alias 字段继续保留以兼容 B。
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
    TableRef from = {}; // 新 FROM table_ref 表示；旧 table/table_alias 字段继续保留以兼容 B。
    std::vector<SetOperation> set_operations = {}; // UNION/INTERSECT/EXCEPT；B 后续适配列数、类型和集合语义。
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
    std::variant<CreateTableStmt, AlterTableStmt, DropTableStmt, InsertStmt, SelectStmt, UpdateStmt, DeleteStmt> node;
    SourceLocation span;
};

} // namespace minisql
