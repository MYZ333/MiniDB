#pragma once

// B → 执行层：算子树只描述要执行的工作，本头文件不包含算子执行算法。
#include "minisql/bound.hpp"

namespace minisql {

struct OutputColumn {
    std::string name;
    DataType type;
};

struct PlanNode;
using PlanPtr = std::shared_ptr<const PlanNode>;

struct CreateTablePlan {
    std::string table_name;
    std::vector<ColumnSpec> columns;
};
struct DropTablePlan {
    std::vector<std::string> table_names;
    bool if_exists = false;
};
struct InsertPlan {
    std::shared_ptr<const TableSchema> table;
    std::vector<ScalarValue> values;
    std::vector<std::vector<ScalarValue>> rows = {};
};
struct SeqScanPlan {
    std::shared_ptr<const TableSchema> table;
    std::uint64_t relation_id = 0;
    std::string relation_name = {};
};
// 所有连接的输出布局都固定为左输入列后接右输入列；外连接缺失侧填 NULL。
struct NestedLoopJoinPlan {
    PlanPtr left;
    PlanPtr right;
    BoundExprPtr predicate;
    JoinType type = JoinType::Inner;
};
struct FilterPlan {
    BoundExprPtr predicate;
    PlanPtr input;
};
// 无聚合调用的查询使用 GroupBy 按 keys 去重并仅输出这些键。
struct GroupByPlan {
    std::vector<BoundColumnRef> keys;
    PlanPtr input;
};
// Aggregate 是完整的聚合输出边界：分组、聚合、最终投影和聚合后排序在此完成。
struct AggregatePlan {
    std::vector<BoundColumnRef> group_keys;
    std::vector<BoundAggregateItem> items;
    std::vector<BoundAggregateOrder> order_by;
    PlanPtr input;
    BoundExprPtr having = {};
    bool distinct = false;
    std::optional<std::int64_t> limit = {};
    std::int64_t offset = 0;
};
// 多键稳定优先级由 items 顺序表达；相同键行之间不保证稳定排序。
struct SortPlan {
    std::vector<BoundOrderBy> items;
    PlanPtr input;
    std::vector<BoundExpressionOrder> expression_items = {};
};
struct ProjectPlan {
    std::vector<BoundColumnRef> columns;
    PlanPtr input;
    std::vector<BoundExprPtr> expressions = {};
    bool distinct = false;
    std::optional<std::int64_t> limit = {};
    std::int64_t offset = 0;
};
struct UpdatePlan {
    std::shared_ptr<const TableSchema> table;
    std::vector<BoundAssignment> assignments;
    PlanPtr input; // 必须携带行标识；所有 RHS 先求值，再统一写回。
};
struct DeletePlan {
    std::shared_ptr<const TableSchema> table;
    PlanPtr input; // 必须携带行标识，不能通过业务值猜测是哪一条记录。
};

struct PlanNode {
    std::variant<CreateTablePlan, DropTablePlan, InsertPlan, SeqScanPlan, NestedLoopJoinPlan,
                 FilterPlan, GroupByPlan, AggregatePlan, SortPlan, ProjectPlan,
                 UpdatePlan, DeletePlan> node;
    std::vector<OutputColumn> output; // 有序业务列；修改类根节点为空。
    bool carries_row_id = false; // 内部行标识不占用 output 的业务列。
};

struct LogicalPlan {
    CatalogVersion catalog_version; // 执行前校验，并在执行期间固定此模式版本。
    PlanPtr root;
};

} // namespace minisql
