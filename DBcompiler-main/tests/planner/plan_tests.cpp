// 从真实语义结果生成计划，验证执行契约；没有测试执行器，不声称查询/修改了数据。
#include "minisql/memory_catalog.hpp"
#include "minisql/plan_printer.hpp"
#include "../test_support.hpp"

using namespace minisql;
using namespace minisql::test;

namespace {
struct Fixture {
    MemoryCatalog catalog;
    Fixture() { value(catalog.createTable("student", {{"id", DataType::Int}, {"name", DataType::Varchar}, {"age", DataType::Int}})); }
    template <typename T>
    BoundStatement bind(T stmt) { return value(analyze(Statement{std::move(stmt), {}}, *catalog.snapshot())); }
    template <typename T>
    LogicalPlan compile(T stmt) { return value(buildPlan(bind(std::move(stmt)))); }
};

struct MultiTableFixture {
    MemoryCatalog catalog;
    MultiTableFixture() {
        value(catalog.createTable("student", {{"id", DataType::Int}, {"name", DataType::Varchar},
                                               {"age", DataType::Int}}));
        value(catalog.createTable("score", {{"id", DataType::Int}, {"student_id", DataType::Int},
                                             {"value", DataType::Int}}));
    }
    BoundStatement bind(SelectStmt select) {
        return value(analyze(Statement{std::move(select), {}}, *catalog.snapshot()));
    }
    LogicalPlan compile(SelectStmt select) { return value(buildPlan(bind(std::move(select)))); }
};

SelectStmt joinedQuery() {
    SelectStmt select{id("student"), std::vector<Identifier>{id("student.name")},
        bin(BinaryOp::Greater, col("score.value"), num(60))};
    select.joins.push_back({id("score"),
        bin(BinaryOp::Equal, col("student.id"), col("score.student_id")), {}});
    select.group_by = {id("student.name")};
    select.order_by = {{id("student.name"), SortDirection::Desc, {}}};
    return select;
}

UpdateStmt increment(ExprPtr where = nullptr) {
    return {id("student"), {{id("age"), bin(BinaryOp::Add, col("age"), num(1)), {}}}, std::move(where)};
}

SelectItem selected(std::string name) { return {id(std::move(name)), std::nullopt, {}}; }

SelectItem aggregate(std::string function, std::optional<std::string> argument,
                     bool star = false, std::optional<std::string> alias = std::nullopt) {
    return {AggregateCall{id(std::move(function)),
                          argument ? std::optional<Identifier>{id(*argument)} : std::nullopt,
                          star, {}},
            alias ? std::optional<Identifier>{id(*alias)} : std::nullopt, {}};
}

// 检查 Filter 保留完整输入模式和 RowId；两者对修改计划都很关键。
void checkFilteredInput(const PlanPtr& input, bool row_id) {
    const auto& filter = std::get<FilterPlan>(input->node);
    check(input->output.size() == 3 && input->output[2].name == "age", "filter lost full scan schema");
    check(input->carries_row_id == row_id && filter.input->carries_row_id == row_id, "row identity not preserved");
    check(std::holds_alternative<SeqScanPlan>(filter.input->node), "filter input is not a scan");
}
} // namespace

int main() {
    Suite suite;
    suite.run("CREATE plan is metadata only and preserves version", [] {
        MemoryCatalog catalog;
        auto bound = value(analyze(Statement{CreateTableStmt{id("Student"), {{id("ID"), DataType::Int, {}}}}, {}}, *catalog.snapshot()));
        auto plan = value(buildPlan(bound));
        const auto& op = std::get<CreateTablePlan>(plan.root->node);
        check(op.table_name == "student" && op.columns[0].name == "id", "wrong CREATE payload");
        check(plan.catalog_version == 0 && !catalog.snapshot()->findTable("student"), "planning changed catalog");
        check(plan.root->output.empty() && !plan.root->carries_row_id, "CREATE has unexpected row output");
    });
    suite.run("INSERT plan carries already reordered values", [] {
        Fixture f;
        auto plan = f.compile(InsertStmt{id("student"), std::vector<Identifier>{id("name"), id("age"), id("id")},
            {{std::string{"Alice"}, {}}, {std::int64_t{20}, {}}, {std::int64_t{1}, {}}}});
        const auto& op = std::get<InsertPlan>(plan.root->node);
        check(std::get<std::int64_t>(op.values[0]) == 1 && std::get<std::string>(op.values[1]) == "Alice" &&
              std::get<std::int64_t>(op.values[2]) == 20, "INSERT lost schema order");
        check(plan.root->output.empty() && !plan.root->carries_row_id, "INSERT output should be execution status only");
    });
    suite.run("SELECT Filter Project order and output dependencies", [] {
        Fixture f;
        auto plan = f.compile(SelectStmt{id("student"), std::vector<Identifier>{id("name")},
            bin(BinaryOp::Greater, col("age"), num(18))});
        const auto& project = std::get<ProjectPlan>(plan.root->node);
        checkFilteredInput(project.input, false);
        check(plan.root->output.size() == 1 && plan.root->output[0].name == "name" &&
              plan.root->output[0].type == DataType::Varchar, "wrong SELECT output schema");
        check(plan.catalog_version == 1 && !plan.root->carries_row_id, "SELECT metadata mismatch");
    });
    suite.run("SELECT star retains Project and omits absent Filter", [] {
        Fixture f;
        auto plan = f.compile(SelectStmt{id("student"), AllColumns{}, nullptr});
        const auto& project = std::get<ProjectPlan>(plan.root->node);
        check(std::holds_alternative<SeqScanPlan>(project.input->node), "spurious Filter");
        check(project.columns.size() == 3 && plan.root->output[2].name == "age", "star order mismatch");
    });
    suite.run("SELECT duplicate outputs keep caller order", [] {
        Fixture f;
        auto plan = f.compile(SelectStmt{id("student"), std::vector<Identifier>{id("age"), id("name"), id("AGE")}, nullptr});
        check(plan.root->output.size() == 3 && plan.root->output[0].name == "age" &&
              plan.root->output[1].name == "name" && plan.root->output[2].name == "age", "projection deduplicated or reordered");
    });
    suite.run("JOIN GROUP BY ORDER BY produce execution-order plan nodes", [] {
        MultiTableFixture f;
        const auto plan = f.compile(joinedQuery());
        const auto& project = std::get<ProjectPlan>(plan.root->node);
        const auto& sort = std::get<SortPlan>(project.input->node);
        const auto& group = std::get<GroupByPlan>(sort.input->node);
        const auto& filter = std::get<FilterPlan>(group.input->node);
        const auto& join = std::get<NestedLoopJoinPlan>(filter.input->node);
        check(std::holds_alternative<SeqScanPlan>(join.left->node) &&
              std::holds_alternative<SeqScanPlan>(join.right->node),
              "NestedLoopJoin children must be scans");
        check(join.left->output.size() == 3 && join.right->output.size() == 3 &&
              filter.input->output.size() == 6, "JOIN output must concatenate left and right schemas");
        check(group.keys.size() == 1 && std::holds_alternative<FilterPlan>(group.input->node) &&
              sort.items[0].direction == SortDirection::Desc,
              "GROUP or ORDER payload mismatch");
        check(plan.root->output.size() == 1 && plan.root->output[0].name == "name" &&
              !plan.root->carries_row_id, "advanced SELECT root metadata mismatch");
    });
    suite.run("aggregate query produces one typed root above filtered detail rows", [] {
        Fixture f;
        SelectStmt select{id("student"), std::vector<SelectItem>{
            selected("age"), aggregate("COUNT", std::nullopt, true, "rows"),
            aggregate("SUM", "id", false, "total"), aggregate("AVG", "age")},
            bin(BinaryOp::Greater, col("id"), num(0))};
        select.group_by = {id("age")};
        select.order_by = {{id("total"), SortDirection::Desc, {}}};
        const auto plan = f.compile(std::move(select));
        const auto& root = std::get<AggregatePlan>(plan.root->node);
        check(root.group_keys.size() == 1 && root.items.size() == 4 &&
              root.order_by.size() == 1 && std::holds_alternative<FilterPlan>(root.input->node),
              "Aggregate plan shape mismatch");
        check(plan.root->output[0].type == DataType::Int &&
              plan.root->output[1].name == "rows" &&
              plan.root->output[3].type == DataType::Float,
              "Aggregate output names or result types mismatch");
        check(formatPlan(plan).find(
            "Aggregate[group=student.age; items=student.age, COUNT(*), SUM(student.id), AVG(student.age); order=output#2 DESC]")
            != std::string::npos, "Aggregate plan printer omitted payload");
    });
    suite.run("self JOIN plan preserves relation instances and output aliases", [] {
        Fixture f;
        SelectStmt select{id("student"),
            std::vector<Identifier>{id("e.name"), id("m.name")}, nullptr};
        select.table_alias = id("e");
        select.column_aliases = {id("employee_name"), id("manager_name")};
        select.joins.push_back({id("student"),
            bin(BinaryOp::Equal, col("e.age"), col("m.id")), {}, id("m")});
        const auto plan = f.compile(std::move(select));
        const auto& project = std::get<ProjectPlan>(plan.root->node);
        const auto& join = std::get<NestedLoopJoinPlan>(project.input->node);
        const auto& left = std::get<SeqScanPlan>(join.left->node);
        const auto& right = std::get<SeqScanPlan>(join.right->node);
        check(left.table->id.value == right.table->id.value &&
              left.relation_id == 1 && right.relation_id == 2 &&
              left.relation_name == "e" && right.relation_name == "m",
              "self JOIN scans lost relation identity");
        check(project.columns[0].relation_id == 1 &&
              project.columns[1].relation_id == 2 &&
              plan.root->output[0].name == "employee_name" &&
              plan.root->output[1].name == "manager_name",
              "Project lost relation or output aliases");
        const auto printed = formatPlan(plan);
        check(printed.find("NestedLoopJoin[(e.age = m.id)]") != std::string::npos &&
              printed.find("SeqScan[student#1 AS e]") != std::string::npos &&
              printed.find("SeqScan[student#1 AS m]") != std::string::npos,
              "plan printer does not distinguish self JOIN aliases");
    });
    suite.run("ORDER BY hidden column runs before Project", [] {
        Fixture f;
        SelectStmt select{id("student"), std::vector<Identifier>{id("name")}, nullptr};
        select.order_by = {{id("age"), SortDirection::Desc, {}}};
        const auto plan = f.compile(std::move(select));
        const auto& project = std::get<ProjectPlan>(plan.root->node);
        const auto& sort = std::get<SortPlan>(project.input->node);
        check(sort.input->output.size() == 3 && project.input->output.size() == 3 &&
              plan.root->output.size() == 1, "hidden sort key was projected away too early");
    });
    suite.run("UPDATE filtered scan carries row identity", [] {
        Fixture f;
        auto bound = f.bind(increment(bin(BinaryOp::Equal, col("id"), num(1))));
        auto plan = value(buildPlan(bound));
        const auto& op = std::get<UpdatePlan>(plan.root->node);
        checkFilteredInput(op.input, true);
        check(op.assignments[0].target.ordinal == 2, "wrong UPDATE target");
        check(op.assignments[0].value == std::get<BoundUpdate>(bound.node).assignments[0].value, "planner rewrote RHS");
        check(plan.root->output.empty() && !plan.root->carries_row_id, "UPDATE root should not return row IDs");
    });
    suite.run("UPDATE swap keeps both old-row references", [] {
        Fixture f;
        auto plan = f.compile(UpdateStmt{id("student"), {{id("id"), col("age"), {}}, {id("age"), col("id"), {}}}, nullptr});
        const auto& op = std::get<UpdatePlan>(plan.root->node);
        check(std::get<BoundColumnRef>(op.assignments[0].value->node).ordinal == 2 &&
              std::get<BoundColumnRef>(op.assignments[1].value->node).ordinal == 0, "swap lost old-row binding");
        check(std::holds_alternative<SeqScanPlan>(op.input->node) && op.input->carries_row_id, "unfiltered UPDATE input mismatch");
    });
    suite.run("DELETE with and without WHERE carries row identity", [] {
        Fixture f;
        auto filtered = f.compile(DeleteStmt{id("student"), bin(BinaryOp::Equal, col("id"), num(1))});
        checkFilteredInput(std::get<DeletePlan>(filtered.root->node).input, true);
        auto all = f.compile(DeleteStmt{id("student"), nullptr});
        const auto& op = std::get<DeletePlan>(all.root->node);
        check(std::holds_alternative<SeqScanPlan>(op.input->node) && op.input->carries_row_id, "DELETE all rows lost RowId");
        check(all.root->output.empty() && !all.root->carries_row_id, "DELETE root output mismatch");
    });
    suite.run("planner uses IDs separately from ordinals", [] {
        auto table = std::make_shared<const TableSchema>(TableSchema{TableId{42}, "external",
            {{ColumnId{901}, "id", DataType::Int}, {ColumnId{17}, "name", DataType::Varchar}}});
        BoundStatement bound{9, BoundSelect{table, {{TableId{42}, ColumnId{17}, 1, DataType::Varchar}}, nullptr}};
        auto plan = value(buildPlan(bound));
        check(std::get<ProjectPlan>(plan.root->node).columns[0].column_id.value == 17 &&
              plan.root->output[0].name == "name" && plan.catalog_version == 9, "planner confused ID and ordinal");
    });
    suite.run("plan survives destruction of catalog and bound statement", [] {
        LogicalPlan plan;
        {
            Fixture f;
            plan = f.compile(SelectStmt{id("student"), AllColumns{}, nullptr});
        }
        const auto& scan = std::get<SeqScanPlan>(std::get<ProjectPlan>(plan.root->node).input->node);
        check(scan.table->columns[1].name == "name", "plan holds dangling schema");
    });
    suite.run("planner rejects missing table and invalid projection identities", [] {
        failure(buildPlan(BoundStatement{0, BoundDelete{nullptr, nullptr}}), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
        Fixture f;
        auto bound = f.bind(SelectStmt{id("student"), AllColumns{}, nullptr});
        auto& select = std::get<BoundSelect>(bound.node);
        const auto original = select.columns[0];
        select.columns[0].ordinal = 100;
        failure(buildPlan(bound), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
        select.columns[0] = original; select.columns[0].column_id.value = 999;
        failure(buildPlan(bound), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
        select.columns[0] = original; select.columns[0].table_id.value = 999;
        failure(buildPlan(bound), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
    });
    suite.run("planner rejects malformed INSERT and non-BOOL predicate", [] {
        Fixture f;
        auto table = f.catalog.snapshot()->findTable("student");
        failure(buildPlan(BoundStatement{1, BoundInsert{table, {std::int64_t{1}}}}), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
        failure(buildPlan(BoundStatement{1, BoundInsert{table, {std::int64_t{1}, std::int64_t{2}, std::int64_t{3}}}}), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
        auto expr = std::make_shared<const BoundExpr>(BoundExpr{BoundLiteral{std::int64_t{1}}, DataType::Int, span(10)});
        auto e = failure(buildPlan(BoundStatement{1, BoundDelete{table, expr}}), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
        check(e.span->begin.offset == 10, "invalid predicate location lost");
    });
    suite.run("planner rejects malformed aggregate output and result types", [] {
        Fixture f;
        auto bound = f.bind(SelectStmt{id("student"), std::vector<SelectItem>{
            aggregate("COUNT", std::nullopt, true, "rows")}, nullptr});
        auto& select = std::get<BoundSelect>(bound.node);
        select.output_names.clear();
        failure(buildPlan(bound), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
        select.output_names = {"rows"};
        auto& count = std::get<BoundAggregate>(select.aggregate_items[0].value);
        count.type = DataType::Float;
        failure(buildPlan(bound), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
        count.type = DataType::Int;
        select.aggregate_order_by = {{std::size_t{1}, SortDirection::Asc}};
        failure(buildPlan(bound), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
    });
    suite.run("planner rejects malformed UPDATE target and RHS", [] {
        Fixture f;
        auto bound = f.bind(increment());
        auto& update = std::get<BoundUpdate>(bound.node);
        auto assignment = update.assignments[0];
        update.assignments.push_back(assignment);
        failure(buildPlan(bound), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
        update.assignments = {assignment}; update.assignments[0].value = nullptr;
        failure(buildPlan(bound), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
        update.assignments[0].value = std::make_shared<const BoundExpr>(BoundExpr{
            BoundBinary{BinaryOp::Add, nullptr, nullptr, {}}, DataType::Int, {}});
        failure(buildPlan(bound), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
    });
    suite.run("planner defensively rejects malformed JOIN and grouping contracts", [] {
        MultiTableFixture f;
        auto bound = f.bind(joinedQuery());
        auto& select = std::get<BoundSelect>(bound.node);
        const auto group = select.group_by[0];
        select.group_by.push_back(group);
        failure(buildPlan(bound), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
        select.group_by = {group};
        select.columns[0] = select.joins[0].table
            ? BoundColumnRef{select.joins[0].table->id, select.joins[0].table->columns[2].id,
                             2, DataType::Int}
            : group;
        failure(buildPlan(bound), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
        select.columns = {group};
        select.joins[0].on = std::make_shared<const BoundExpr>(BoundExpr{
            BoundLiteral{std::int64_t{1}}, DataType::Int, span(40)});
        auto error = failure(buildPlan(bound), ErrorCode::InvalidBoundStatement, DiagnosticStage::Plan);
        check(!error.message.empty(), "malformed JOIN diagnostic missing");
    });
    suite.run("SELECT plan printer golden output and repeatability", [] {
        Fixture f;
        auto bound = f.bind(SelectStmt{id("student"), std::vector<Identifier>{id("name")}, bin(BinaryOp::Greater, col("age"), num(18))});
        const std::string expected =
            "CatalogVersion: 1\n"
            "Project[student.name] output=[name:VARCHAR] row_id=no\n"
            "  Filter[(student.age > 18)] output=[id:INT, name:VARCHAR, age:INT] row_id=no\n"
            "    SeqScan[student#1] output=[id:INT, name:VARCHAR, age:INT] row_id=no\n";
        check(formatPlan(value(buildPlan(bound))) == expected, "SELECT printed plan mismatch");
        check(formatPlan(value(buildPlan(bound))) == expected, "plan output depends on node addresses");
    });
    suite.run("UPDATE printer displays old-row expressions and row identity", [] {
        Fixture f;
        auto output = formatPlan(f.compile(increment()));
        const std::string expected =
            "CatalogVersion: 1\n"
            "Update[student#1; student.age = (student.age + 1); values=old-row] output=[] row_id=no\n"
            "  SeqScan[student#1] output=[id:INT, name:VARCHAR, age:INT] row_id=yes\n";
        check(output == expected, "UPDATE printed contract mismatch");
    });
    suite.run("advanced SELECT printer shows both JOIN branches and key directions", [] {
        MultiTableFixture f;
        const auto output = formatPlan(f.compile(joinedQuery()));
        check(output.find("Project[student.name]") != std::string::npos &&
              output.find("Sort[student.name DESC]") != std::string::npos &&
              output.find("GroupBy[student.name]") != std::string::npos &&
              output.find("NestedLoopJoin[(student.id = score.student_id)]") != std::string::npos &&
              output.find("  SeqScan[student#1]") != std::string::npos &&
              output.find("  SeqScan[score#2]") != std::string::npos,
              "advanced plan printer omitted an operator or JOIN branch");
    });
    suite.run("printer renders CREATE DELETE and escaped INSERT strings", [] {
        MemoryCatalog empty;
        const auto create = value(buildPlan(value(analyze(Statement{CreateTableStmt{id("t"), {{id("id"), DataType::Int, {}}}}, {}}, *empty.snapshot()))));
        check(formatPlan(create) == "CatalogVersion: 0\nCreateTable[t; id:INT] output=[] row_id=no\n", "CREATE print mismatch");
        Fixture f;
        auto insert = f.compile(InsertStmt{id("student"), std::nullopt,
            {{std::int64_t{1}, {}}, {std::string{"Tom's\n\\book"}, {}}, {std::int64_t{20}, {}}}});
        check(formatPlan(insert).find("values=(1, 'Tom''s\\n\\\\book', 20)") != std::string::npos, "string escaping mismatch");
        check(formatPlan(f.compile(DeleteStmt{id("student"), nullptr})).find("Delete[student#1] output=[] row_id=no\n") != std::string::npos, "DELETE print mismatch");
    });
    return suite.finish();
}
