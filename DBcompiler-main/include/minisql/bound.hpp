#pragma once

// B 内部中间结果：名称已绑定，类型已检查，计划生成无需再查名称。
#include "minisql/catalog.hpp"

namespace minisql {

struct BoundColumnRef {
    TableId table_id;
    ColumnId column_id;
    std::size_t ordinal; // 在原表模式中的序号，不是 Project 输出的位置。
    DataType type;
};

struct BoundExpr;
using BoundExprPtr = std::shared_ptr<const BoundExpr>;

struct BoundLiteral { ScalarValue value; };
struct BoundUnary {
    UnaryOp op;
    BoundExprPtr operand;
    SourceLocation operator_span;
};
struct BoundBinary {
    BinaryOp op;
    BoundExprPtr left;
    BoundExprPtr right;
    SourceLocation operator_span;
};

struct BoundExpr {
    std::variant<BoundColumnRef, BoundLiteral, BoundUnary, BoundBinary> node;
    DataType type; // 每个表达式均有确定类型，运算结果也不例外。
    SourceLocation span;
};

struct BoundCreateTable {
    std::string table_name;
    std::vector<ColumnSpec> columns; // ID 留给执行建表的 Catalog 分配。
};

struct BoundInsert {
    std::shared_ptr<const TableSchema> table;
    std::vector<ScalarValue> values; // 已按表列顺序重排，数量等于全部表列数。
};

struct BoundJoin {
    std::shared_ptr<const TableSchema> table;
    BoundExprPtr on; // 加入当前表后绑定；必须为 BOOL。
};

struct BoundOrderBy {
    BoundColumnRef column; // 可为未投影的列；Sort 在 Project 前读取它。
    SortDirection direction; // 每个排序键独立指定升序或降序。
};

struct BoundSelect {
    std::shared_ptr<const TableSchema> table;
    std::vector<BoundColumnRef> columns; // 星号已展开，显式重复列保留。
    BoundExprPtr where; // 若存在则必须为 BOOL。
    std::vector<BoundJoin> joins = {}; // 按 SQL 顺序保存，生成左深连接树。
    std::vector<BoundColumnRef> group_by = {}; // 无聚合阶段仅允许投影这些键。
    std::vector<BoundOrderBy> order_by = {}; // 项目顺序就是多键比较优先级。
};

struct BoundAssignment {
    BoundColumnRef target;
    BoundExprPtr value; // 读取更新前记录，类型必须与 target 相同。
};

struct BoundUpdate {
    std::shared_ptr<const TableSchema> table;
    std::vector<BoundAssignment> assignments;
    BoundExprPtr where;
};

struct BoundDelete {
    std::shared_ptr<const TableSchema> table;
    BoundExprPtr where;
};

struct BoundStatement {
    CatalogVersion catalog_version;
    std::variant<BoundCreateTable, BoundInsert, BoundSelect, BoundUpdate, BoundDelete> node;
};

} // namespace minisql
