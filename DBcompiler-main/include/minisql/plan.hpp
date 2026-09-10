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
struct InsertPlan {
    std::shared_ptr<const TableSchema> table;
    std::vector<ScalarValue> values;
};
struct SeqScanPlan { std::shared_ptr<const TableSchema> table; };
struct FilterPlan {
    BoundExprPtr predicate;
    PlanPtr input;
};
struct ProjectPlan {
    std::vector<BoundColumnRef> columns;
    PlanPtr input;
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
    std::variant<CreateTablePlan, InsertPlan, SeqScanPlan, FilterPlan,
                 ProjectPlan, UpdatePlan, DeletePlan> node;
    std::vector<OutputColumn> output; // 有序业务列；修改类根节点为空。
    bool carries_row_id = false; // 内部行标识不占用 output 的业务列。
};

struct LogicalPlan {
    CatalogVersion catalog_version; // 执行前校验，并在执行期间固定此模式版本。
    PlanPtr root;
};

} // namespace minisql
