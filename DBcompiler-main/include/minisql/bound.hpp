#pragma once

// B 内部中间结果：名称已绑定，类型已检查，计划生成无需再查名称。
#include "minisql/catalog.hpp"

namespace minisql {

struct BoundColumnRef {
    TableId table_id;
    ColumnId column_id;
    std::size_t ordinal; // 在原表模式中的序号，不是 Project 输出的位置。
    DataType type;
    std::uint64_t relation_id = 0; // 同一物理表的不同 FROM/JOIN 实例必须不同。
};

// 聚合叶节点可嵌入 SELECT/HAVING/ORDER BY 表达式。
struct BoundAggregate {
    AggregateKind kind;
    std::optional<BoundColumnRef> argument;
    DataType type;
    SourceLocation span;
};

struct BoundExpr;
struct BoundSelect;
struct PlanNode;
using BoundExprPtr = std::shared_ptr<const BoundExpr>;
using BoundSelectPtr = std::shared_ptr<const BoundSelect>;

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

struct BoundCaseWhen {
    BoundExprPtr condition;
    BoundExprPtr result;
};

struct BoundCase {
    BoundExprPtr operand;
    std::vector<BoundCaseWhen> branches;
    BoundExprPtr else_result;
};

// 子查询先保存绑定树；计划生成再填充 plan。correlated_columns 是外层列裁剪契约。
struct BoundInSubquery {
    BoundExprPtr value;
    BoundSelectPtr query;
    std::shared_ptr<const PlanNode> plan;
    std::vector<BoundColumnRef> correlated_columns;
    bool negated = false;
};

struct BoundExistsSubquery {
    BoundSelectPtr query;
    std::shared_ptr<const PlanNode> plan;
    std::vector<BoundColumnRef> correlated_columns;
    bool negated = false;
};

struct BoundScalarSubquery {
    BoundSelectPtr query;
    std::shared_ptr<const PlanNode> plan;
    std::vector<BoundColumnRef> correlated_columns;
};

struct BoundExpr {
    std::variant<BoundColumnRef, BoundLiteral, BoundUnary, BoundBinary, BoundAggregate,
                 BoundCase, BoundInSubquery, BoundExistsSubquery, BoundScalarSubquery> node;
    DataType type; // 每个表达式均有确定类型，运算结果也不例外。
    SourceLocation span;
};

struct BoundCreateTable {
    std::string table_name;
    std::vector<ColumnSpec> columns; // ID 留给执行建表的 Catalog 分配。
    std::vector<TableConstraintSpec> table_constraints = {};
    bool if_not_exists = false;
};

struct BoundCreateIndex {
    std::string index_name;
    std::shared_ptr<const TableSchema> table;
    BoundColumnRef column;
    IndexId predicted_index_id;
    DataType key_type = DataType::Int;
    bool unique = true;
    std::int64_t metadata_page_id = -1;
};

struct BoundAlterAddColumn { ColumnSpec column; };
struct BoundAlterDropColumn { std::size_t ordinal; std::string column_name; };
struct BoundAlterRenameTable { std::string new_name; };
struct BoundAlterRenameColumn { std::size_t ordinal; std::string new_name; };
using BoundAlterAction = std::variant<BoundAlterAddColumn, BoundAlterDropColumn,
    BoundAlterRenameTable, BoundAlterRenameColumn>;

struct BoundAlterTable {
    std::shared_ptr<const TableSchema> table;
    BoundAlterAction action;
};

struct BoundDropTable {
    std::vector<std::string> table_names;
    bool if_exists = false;
};

struct BoundDropIndex {
    std::string index_name;
    std::shared_ptr<const IndexSchema> index;
    bool if_exists = false;
};

struct BoundInsert {
    std::shared_ptr<const TableSchema> table;
    std::vector<ScalarValue> values; // 已按表列顺序重排，数量等于全部表列数。
    std::vector<std::vector<ScalarValue>> rows = {}; // 多行时保存全部完整记录。
};

struct BoundJoin {
    std::shared_ptr<const TableSchema> table;
    BoundExprPtr on; // 加入当前表后绑定；必须为 BOOL。
    std::string relation_name = {}; // 已归一化的表别名或真实表名。
    std::uint64_t relation_id = 0;
    JoinType type = JoinType::Inner;
    BoundSelectPtr subquery = {}; // 非空表示由派生表查询提供该关系。
};

struct BoundOrderBy {
    BoundColumnRef column; // 可为未投影的列；Sort 在 Project 前读取它。
    SortDirection direction; // 每个排序键独立指定升序或降序。
};

struct BoundExpressionOrder {
    std::variant<BoundColumnRef, BoundExprPtr> key;
    SortDirection direction;
};

struct BoundAggregateItem {
    std::variant<BoundColumnRef, BoundAggregate, BoundExprPtr> value;
};

// 聚合后排序可以引用 SELECT 输出别名，也可以引用未投影的分组键。
struct BoundAggregateOrder {
    std::variant<std::size_t, BoundColumnRef, BoundExprPtr> key; // size_t 是输出列序号。
    SortDirection direction;
};

struct BoundSetOperation {
    SetOperator op = SetOperator::Union;
    bool all = false;
    BoundSelectPtr query;
};

struct BoundSelect {
    std::shared_ptr<const TableSchema> table;
    std::vector<BoundColumnRef> columns; // 星号已展开，显式重复列保留。
    BoundExprPtr where; // 若存在则必须为 BOOL。
    std::vector<BoundJoin> joins = {}; // 按 SQL 顺序保存，生成左深连接树。
    std::vector<BoundColumnRef> group_by = {}; // 非聚合输出列必须属于这些键。
    std::vector<BoundOrderBy> order_by = {}; // 项目顺序就是多键比较优先级。
    std::string relation_name = {}; // FROM 表的别名或真实表名。
    std::uint64_t relation_id = 0;
    std::vector<std::string> output_names = {}; // SELECT 别名解析后的最终列名。
    // 非空表示查询含聚合函数；顺序与最终输出列和 output_names 一致。
    std::vector<BoundAggregateItem> aggregate_items = {};
    std::vector<BoundAggregateOrder> aggregate_order_by = {};
    std::vector<BoundExprPtr> projection_expressions = {};
    std::vector<BoundExpressionOrder> expression_order_by = {};
    BoundExprPtr having = {};
    bool distinct = false;
    std::optional<std::int64_t> limit = {};
    std::int64_t offset = 0;
    BoundSelectPtr source_query = {}; // FROM 派生表的已绑定查询。
    std::vector<BoundSetOperation> set_operations = {};
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

struct BoundStatement;

struct BoundExplain {
    // 使用只读指针保存完整目标语句，使其 CatalogVersion 和绑定结果都不可被包装层篡改。
    std::shared_ptr<const BoundStatement> target;
    bool analyze = false;
};

struct BoundStatement {
    CatalogVersion catalog_version;
    std::variant<BoundCreateTable, BoundCreateIndex, BoundAlterTable, BoundDropTable,
                 BoundDropIndex, BoundInsert, BoundSelect, BoundUpdate, BoundDelete,
                 BoundExplain> node;
};

} // namespace minisql
