// 直接调用真实 analyze 的行为测试：无需 A 的 Parser 或数据库执行器。
#include "minisql/memory_catalog.hpp"
#include "../test_support.hpp"

#include <limits>

using namespace minisql;
using namespace minisql::test;

namespace {
struct Fixture {
    MemoryCatalog catalog;
    Fixture() {
        value(catalog.createTable("student", {{"id", DataType::Int}, {"name", DataType::Varchar}, {"age", DataType::Int}}));
    }
    template <typename T>
    Result<BoundStatement> analyzeNode(T node) {
        auto snapshot = catalog.snapshot();
        return analyze(Statement{std::move(node), span(0, 80)}, *snapshot);
    }
    Result<BoundStatement> where(ExprPtr predicate) {
        return analyzeNode(SelectStmt{id("student"), AllColumns{}, std::move(predicate)});
    }
};

struct MultiTableFixture {
    MemoryCatalog catalog;
    MultiTableFixture() {
        value(catalog.createTable("student", {{"id", DataType::Int}, {"name", DataType::Varchar},
                                               {"age", DataType::Int}}));
        value(catalog.createTable("score", {{"id", DataType::Int}, {"student_id", DataType::Int},
                                             {"value", DataType::Int}}));
    }
    Result<BoundStatement> analyzeSelect(SelectStmt select) {
        return analyze(Statement{std::move(select), span(0, 100)}, *catalog.snapshot());
    }
};

InsertStmt insert() {
    return {id("student"), std::nullopt,
        {{std::int64_t{1}, span(30)}, {std::string{"Alice"}, span(33, 7)}, {std::int64_t{20}, span(42, 2)}}};
}

// 聚合测试按 A 的新 AST 构造，别名与 SELECT 项平行保存在 column_aliases。
SelectItem selected(std::string name) { return id(std::move(name)); }

SelectItem aggregate(std::string function, std::optional<std::string> argument, bool star = false) {
    const auto kind = function == "COUNT" ? AggregateFunction::Count :
        function == "SUM" ? AggregateFunction::Sum : function == "AVG" ? AggregateFunction::Avg :
        function == "MIN" ? AggregateFunction::Min : function == "MAX" ? AggregateFunction::Max :
        static_cast<AggregateFunction>(-1);
    std::variant<AllColumns, Identifier> parameter = AllColumns{};
    if (!star && argument) parameter = id(*argument);
    return AggregateCall{kind, std::move(parameter), {}};
}
} // namespace

int main() {
    Suite suite;
    suite.run("CREATE normalizes without mutating AST or catalog", [] {
        MemoryCatalog catalog;
        auto snapshot = catalog.snapshot();
        Statement ast{CreateTableStmt{id("Student"), {{id("ID"), DataType::Int, {}}}}, span(0, 30)};
        auto bound = value(analyze(ast, *snapshot));
        const auto& create = std::get<BoundCreateTable>(bound.node);
        check(create.table_name == "student" && create.columns[0].name == "id", "CREATE names not normalized");
        check(std::get<CreateTableStmt>(ast.node).table.text == "Student", "AST was mutated");
        check(!catalog.snapshot()->findTable("student") && bound.catalog_version == 0, "analysis registered table");
    });
    suite.run("CREATE reports duplicate table at original name", [] {
        Fixture f;
        auto e = failure(f.analyzeNode(CreateTableStmt{id("STUDENT", span(13, 7)), {{id("id"), DataType::Int, {}}}}), ErrorCode::TableAlreadyExists);
        check(e.span->begin.offset == 13, "wrong table location");
    });
    suite.run("CREATE validates columns and accepts BOOL FLOAT", [] {
        Fixture f;
        failure(f.analyzeNode(CreateTableStmt{id("newtable"), {}}), ErrorCode::EmptyColumnList);
        auto e = failure(f.analyzeNode(CreateTableStmt{id("newtable"),
            {{id("id"), DataType::Int, {}}, {id("ID", span(25, 2)), DataType::Int, {}}}}), ErrorCode::DuplicateColumn);
        check(e.span->begin.offset == 25, "duplicate should locate second column");
        const auto created = value(f.analyzeNode(CreateTableStmt{id("newtable"),
            {{id("flag"), DataType::Bool, span(20, 4)},
             {id("score"), DataType::Float, span(30, 5)}}}));
        const auto& columns = std::get<BoundCreateTable>(created.node).columns;
        check(columns[0].type == DataType::Bool && columns[1].type == DataType::Float,
              "extended CREATE types lost");
    });
    suite.run("CREATE IF NOT EXISTS and table constraints bind explicit DDL metadata", [] {
        Fixture f;
        CreateTableStmt create_if_missing{id("newtable"), {{id("id"), DataType::Int, {}}}};
        create_if_missing.if_not_exists = true;
        value(f.analyzeNode(create_if_missing));

        CreateTableStmt create_existing{id("student", span(20, 7)), {{id("id"), DataType::Int, {}}}};
        create_existing.if_not_exists = true;
        const auto existing = value(f.analyzeNode(create_existing));
        check(std::get<BoundCreateTable>(existing.node).if_not_exists,
              "CREATE IF NOT EXISTS flag was lost");

        CreateTableStmt constrained{id("constrained"),
            {{id("id"), DataType::Int, {}}, {id("code"), DataType::Int, {}}}};
        constrained.table_constraints.push_back(
            {TableConstraintKind::PrimaryKey, {id("id", span(42, 2))}, span(30, 15)});
        const auto bound = value(f.analyzeNode(constrained));
        const auto& create = std::get<BoundCreateTable>(bound.node);
        check(create.table_constraints.size() == 1 &&
              create.table_constraints[0].primary_key &&
              create.table_constraints[0].columns == std::vector<std::size_t>{0} &&
              create.columns[0].not_null,
              "table constraint binding or PRIMARY KEY nullability was lost");
    });
    suite.run("ALTER TABLE binds target schema and action payload", [] {
        Fixture f;
        AlterTableStmt alter{id("student", span(12, 7)),
            AlterAddColumn{{id("email"), DataType::Varchar, span(30, 13)}, true}};
        const auto bound = value(f.analyzeNode(std::move(alter)));
        const auto& operation = std::get<BoundAlterTable>(bound.node);
        const auto& add = std::get<BoundAlterAddColumn>(operation.action);
        check(operation.table->name == "student" && add.column.name == "email" &&
              add.column.type == DataType::Varchar,
              "ALTER TABLE ADD binding lost schema or column metadata");
    });
    suite.run("DROP TABLE binds names and IF EXISTS policy", [] {
        Fixture f;
        const auto bound = value(f.analyzeNode(
            DropTableStmt{{id("STUDENT", span(11, 7)), id("missing")}, true}));
        const auto& drop = std::get<BoundDropTable>(bound.node);
        check(drop.if_exists && drop.table_names == std::vector<std::string>({"student", "missing"}),
              "DROP names or IF EXISTS flag were lost");
        failure(f.analyzeNode(DropTableStmt{{id("missing")}, false}), ErrorCode::TableNotFound);
    });
    suite.run("INSERT omitted list uses schema order", [] {
        Fixture f;
        auto binding = value(f.analyzeNode(insert()));
        const auto& row = std::get<BoundInsert>(binding.node);
        check(std::get<std::int64_t>(row.values[0]) == 1 && std::get<std::string>(row.values[1]) == "Alice", "wrong row mapping");
        check(binding.catalog_version == 1, "missing catalog version");
    });
    suite.run("INSERT rows keeps compatibility and binds every row", [] {
        Fixture f;
        auto ast = insert();
        ast.rows = {ast.values};
        value(f.analyzeNode(ast));

        ast.rows.push_back({{std::int64_t{2}, span(50)}, {std::string{"Bob"}, span(53, 5)},
                            {std::int64_t{18}, span(60, 2)}});
        const auto bound = value(f.analyzeNode(std::move(ast)));
        const auto& insert = std::get<BoundInsert>(bound.node);
        check(insert.rows.size() == 2 && std::get<std::int64_t>(insert.rows[1][0]) == 2,
              "multi-row INSERT payload was lost");
    });
    suite.run("INSERT reorders explicit mixed-case columns", [] {
        Fixture f;
        auto ast = insert();
        ast.columns = std::vector<Identifier>{id("NAME"), id("Age"), id("ID")};
        ast.values = {{std::string{"Alice"}, {}}, {std::int64_t{20}, {}}, {std::int64_t{1}, {}}};
        const auto bound = value(f.analyzeNode(ast));
        const auto& row = std::get<BoundInsert>(bound.node);
        check(std::get<std::int64_t>(row.values[0]) == 1 && std::get<std::string>(row.values[1]) == "Alice" &&
              std::get<std::int64_t>(row.values[2]) == 20, "reordered values wrong");
        check(std::get<std::string>(ast.values[0].value) == "Alice", "AST values changed");
    });
    suite.run("INSERT rejects nonexistent table and column", [] {
        Fixture f;
        auto ast = insert(); ast.table = id("missing", span(12, 7));
        auto e = failure(f.analyzeNode(ast), ErrorCode::TableNotFound);
        check(e.span->begin.offset == 12, "missing table position");
        ast.table = id("student"); ast.columns = std::vector<Identifier>{id("score", span(20, 5))};
        e = failure(f.analyzeNode(ast), ErrorCode::ColumnNotFound);
        check(e.span->begin.offset == 20, "missing column position");
    });
    suite.run("INSERT rejects duplicate columns before count check", [] {
        Fixture f; auto ast = insert();
        ast.columns = std::vector<Identifier>{id("id"), id("ID", span(22, 2))};
        auto e = failure(f.analyzeNode(ast), ErrorCode::DuplicateColumn);
        check(e.span->begin.offset == 22, "duplicate INSERT column location");
    });
    suite.run("INSERT distinguishes empty list and fills omitted nullable columns", [] {
        Fixture f; auto ast = insert();
        ast.columns = std::vector<Identifier>{};
        failure(f.analyzeNode(ast), ErrorCode::EmptyColumnList);
        ast.columns = std::nullopt; ast.values.pop_back();
        failure(f.analyzeNode(ast), ErrorCode::ValueCountMismatch);
        ast.columns = std::vector<Identifier>{id("id"), id("name")};
        const auto bound = value(f.analyzeNode(ast));
        check(std::holds_alternative<NullValue>(std::get<BoundInsert>(bound.node).values[2]),
              "omitted nullable column must be filled with NULL");
    });
    suite.run("INSERT type error points to value after reordering", [] {
        Fixture f; auto ast = insert();
        ast.columns = std::vector<Identifier>{id("name"), id("age"), id("id")};
        ast.values = {{std::string{"Alice"}, {}}, {std::int64_t{20}, {}}, {std::string{"bad"}, span(60, 5)}};
        auto e = failure(f.analyzeNode(ast), ErrorCode::TypeMismatch);
        check(e.span->begin.offset == 60 && e.message.find("student.id expects INT") != std::string::npos, "type mismatch lacks target/value location");
    });
    suite.run("INSERT preserves INT64 boundaries and string bytes", [] {
        Fixture f; auto ast = insert();
        ast.values = {{std::numeric_limits<std::int64_t>::min(), {}}, {std::string{"Tom's 书"}, {}},
                      {std::numeric_limits<std::int64_t>::max(), {}}};
        const auto result = value(f.analyzeNode(ast));
        const auto& row = std::get<BoundInsert>(result.node);
        check(std::get<std::int64_t>(row.values[0]) == std::numeric_limits<std::int64_t>::min() &&
              std::get<std::int64_t>(row.values[2]) == std::numeric_limits<std::int64_t>::max() &&
              std::get<std::string>(row.values[1]) == "Tom's 书", "literal contents changed");
    });
    suite.run("SELECT star expands in schema order", [] {
        Fixture f;
        const auto bound = value(f.analyzeNode(SelectStmt{id("STUDENT"), AllColumns{}, nullptr}));
        const auto& select = std::get<BoundSelect>(bound.node);
        check(select.columns.size() == 3 && !select.where, "bad star or absent WHERE");
        for (std::size_t i = 0; i < 3; ++i) {
            check(select.columns[i].ordinal == i && select.columns[i].column_id.value == select.table->columns[i].id.value,
                  "wrong bound column identity");
        }
    });
    suite.run("SELECT retains duplicate output columns and filters on hidden column", [] {
        Fixture f;
        const auto bound = value(f.analyzeNode(SelectStmt{id("student"), std::vector<Identifier>{id("Name"), id("id"), id("NAME")},
            bin(BinaryOp::Greater, col("AGE"), num(18))}));
        const auto& select = std::get<BoundSelect>(bound.node);
        check(select.columns.size() == 3 && select.columns[0].ordinal == 1 && select.columns[2].ordinal == 1, "output order/dedup error");
        const auto& comparison = std::get<BoundBinary>(select.where->node);
        check(std::get<BoundColumnRef>(comparison.left->node).ordinal == 2, "WHERE column incorrectly bound to output index");
    });
    suite.run("SELECT rejects missing table, empty output and missing projection", [] {
        Fixture f;
        failure(f.analyzeNode(SelectStmt{id("missing"), AllColumns{}, nullptr}), ErrorCode::TableNotFound);
        failure(f.analyzeNode(SelectStmt{id("student"), std::vector<Identifier>{}, nullptr}), ErrorCode::EmptyColumnList);
        auto e = failure(f.analyzeNode(SelectStmt{id("student"), std::vector<Identifier>{id("score", span(7, 5))}, nullptr}), ErrorCode::ColumnNotFound);
        check(e.span->begin.column == 8 && e.span->end.offset == 12, "SELECT diagnostic range is not half-open");
    });
    suite.run("JOIN binds qualified columns across both table schemas", [] {
        MultiTableFixture f;
        SelectStmt select{id("student"),
            std::vector<Identifier>{id("student.name"), id("score.value")},
            bin(BinaryOp::Greater, col("score.value"), num(60))};
        select.joins.push_back({id("score"),
            bin(BinaryOp::Equal, col("student.id"), col("score.student_id")), {}});
        const auto bound = value(f.analyzeSelect(std::move(select)));
        const auto& query = std::get<BoundSelect>(bound.node);
        check(query.joins.size() == 1 && query.columns.size() == 2,
              "JOIN binding shape mismatch");
        check(query.columns[0].table_id.value == query.table->id.value &&
              query.columns[1].table_id.value == query.joins[0].table->id.value,
              "qualified columns resolved to wrong tables");
        check(query.joins[0].on->type == DataType::Bool && query.where->type == DataType::Bool,
              "JOIN ON or WHERE lost BOOL type");
    });
    suite.run("JOIN star expands all tables and ambiguous names are rejected", [] {
        MultiTableFixture f;
        SelectStmt star{id("student"), AllColumns{}, nullptr};
        star.joins.push_back({id("score"),
            bin(BinaryOp::Equal, col("student.id"), col("score.student_id")), {}});
        const auto star_bound = value(f.analyzeSelect(star));
        const auto& query = std::get<BoundSelect>(star_bound.node);
        check(query.columns.size() == 6 && query.columns[3].table_id.value == query.joins[0].table->id.value,
              "JOIN star did not preserve table and schema order");

        star.columns = std::vector<Identifier>{id("id", span(7, 2))};
        auto error = failure(f.analyzeSelect(star), ErrorCode::AmbiguousColumn);
        check(error.span->begin.offset == 7, "ambiguous column location mismatch");
        star.columns = std::vector<Identifier>{id("student.id")};
        star.joins[0].on = col("student.id", span(40, 10));
        error = failure(f.analyzeSelect(star), ErrorCode::JoinConditionNotBoolean);
        check(error.span->begin.offset == 40, "JOIN ON type location mismatch");
    });
    suite.run("self JOIN uses distinct alias relation identities", [] {
        Fixture f;
        SelectStmt select{id("student"),
            std::vector<Identifier>{id("e.name"), id("m.name")}, nullptr};
        select.table_alias = id("e");
        select.column_aliases = {id("employee_name"), id("manager_name")};
        select.order_by = {{id("employee_name"), SortDirection::Desc, {}}};
        select.joins.push_back({id("student"),
            bin(BinaryOp::Equal, col("e.id"), col("m.age")), {}, id("m")});
        const auto bound = value(f.analyzeNode(std::move(select)));
        const auto& query = std::get<BoundSelect>(bound.node);
        check(query.relation_name == "e" && query.relation_id == 1 &&
              query.joins[0].relation_name == "m" &&
              query.joins[0].relation_id == 2,
              "relation aliases or instance IDs were lost");
        check(query.columns[0].table_id.value == query.columns[1].table_id.value &&
              query.columns[0].relation_id != query.columns[1].relation_id,
              "self JOIN columns do not distinguish relation instances");
        check(query.output_names[0] == "employee_name" &&
              query.output_names[1] == "manager_name", "column aliases were not bound");
        check(query.order_by[0].column.relation_id == query.columns[0].relation_id &&
              query.order_by[0].column.column_id.value == query.columns[0].column_id.value,
              "ORDER BY output alias did not resolve to its source column");
    });
    suite.run("aliases hide physical names and duplicate relation names fail", [] {
        MultiTableFixture f;
        SelectStmt hidden{id("student"), std::vector<Identifier>{id("student.name")}, nullptr};
        hidden.table_alias = id("s");
        failure(f.analyzeSelect(std::move(hidden)), ErrorCode::ColumnNotFound);

        SelectStmt duplicate{id("student"), AllColumns{}, nullptr};
        duplicate.table_alias = id("x");
        duplicate.joins.push_back({id("score"), truth(), {}, id("X", span(25, 1))});
        const auto error = failure(f.analyzeSelect(std::move(duplicate)), ErrorCode::DuplicateTable);
        check(error.span->begin.offset == 25, "duplicate alias location mismatch");
    });
    suite.run("GROUP BY enforces key-only projection and ordering", [] {
        MultiTableFixture f;
        SelectStmt valid{id("student"), std::vector<Identifier>{id("name")}, nullptr};
        valid.group_by = {id("name")};
        valid.order_by = {{id("name"), SortDirection::Desc, {}}};
        const auto grouped_bound = value(f.analyzeSelect(valid));
        const auto& grouped = std::get<BoundSelect>(grouped_bound.node);
        check(grouped.group_by.size() == 1 && grouped.order_by.size() == 1 &&
              grouped.order_by[0].direction == SortDirection::Desc,
              "GROUP BY or ORDER BY binding mismatch");

        valid.columns = std::vector<Identifier>{id("age")};
        failure(f.analyzeSelect(valid), ErrorCode::InvalidGrouping);
        valid.columns = std::vector<Identifier>{id("name")};
        valid.order_by = {{id("age"), SortDirection::Asc, span(55, 3)}};
        failure(f.analyzeSelect(valid), ErrorCode::InvalidGrouping);
        valid.order_by.clear();
        valid.group_by.push_back(id("NAME", span(70, 4)));
        const auto error = failure(f.analyzeSelect(valid), ErrorCode::InvalidGrouping);
        check(error.span->begin.offset == 70, "duplicate GROUP BY key location mismatch");
    });
    suite.run("aggregate functions bind result types aliases and post-group ordering", [] {
        MultiTableFixture f;
        SelectStmt select{id("student"), std::vector<SelectItem>{
            selected("age"), aggregate("COUNT", std::nullopt, true),
            aggregate("SUM", "id"), aggregate("AVG", "age"),
            aggregate("MIN", "name"), aggregate("MAX", "name")}, nullptr};
        select.column_aliases = {std::nullopt, id("rows"), id("total"), std::nullopt, std::nullopt, std::nullopt};
        select.group_by = {id("age")};
        select.order_by = {{id("total"), SortDirection::Desc, {}},
                           {id("age"), SortDirection::Asc, {}}};
        const auto bound = value(f.analyzeSelect(std::move(select)));
        const auto& query = std::get<BoundSelect>(bound.node);
        check(query.aggregate_items.size() == 6 && query.aggregate_order_by.size() == 2,
              "aggregate items or ordering were lost");
        const auto& count = std::get<BoundAggregate>(query.aggregate_items[1].value);
        const auto& average = std::get<BoundAggregate>(query.aggregate_items[3].value);
        check(count.kind == AggregateKind::Count && !count.argument && count.type == DataType::Int,
              "COUNT(*) binding mismatch");
        check(average.kind == AggregateKind::Avg && average.type == DataType::Float,
              "AVG result type mismatch");
        check(std::get<std::size_t>(query.aggregate_order_by[0].key) == 2 &&
              std::holds_alternative<BoundColumnRef>(query.aggregate_order_by[1].key),
              "aggregate alias or hidden group ordering did not bind");
    });
    suite.run("aggregate semantic rules reject invalid functions arguments and grouping", [] {
        Fixture f;
        failure(f.analyzeNode(SelectStmt{id("student"), std::vector<SelectItem>{
            aggregate("SUM", "name")}, nullptr}), ErrorCode::InvalidOperandType);
        failure(f.analyzeNode(SelectStmt{id("student"), std::vector<SelectItem>{
            aggregate("MEDIAN", "age")}, nullptr}), ErrorCode::UnsupportedFeature);
        failure(f.analyzeNode(SelectStmt{id("student"), std::vector<SelectItem>{
            aggregate("MAX", std::nullopt, true)}, nullptr}), ErrorCode::InvalidOperandType);

        SelectStmt ungrouped{id("student"), std::vector<SelectItem>{
            selected("name"), aggregate("COUNT", std::nullopt, true)}, nullptr};
        failure(f.analyzeNode(std::move(ungrouped)), ErrorCode::InvalidGrouping);
        SelectStmt bad_order{id("student"), std::vector<SelectItem>{
            aggregate("COUNT", std::nullopt, true)}, nullptr};
        bad_order.order_by = {{id("age"), SortDirection::Asc, {}}};
        failure(f.analyzeNode(std::move(bad_order)), ErrorCode::InvalidGrouping);
        failure(f.analyzeNode(SelectStmt{id("student"), std::vector<SelectItem>{
            aggregate("COUNT", "missing")}, nullptr}), ErrorCode::ColumnNotFound);
        SelectStmt ambiguous{id("student"), std::vector<SelectItem>{
            aggregate("COUNT", std::nullopt, true),
            aggregate("SUM", "age")}, nullptr};
        ambiguous.column_aliases = {id("total"), id("total")};
        ambiguous.order_by = {{id("total"), SortDirection::Asc, {}}};
        failure(f.analyzeNode(std::move(ambiguous)), ErrorCode::AmbiguousColumn);
        // Rich AST with only columns must retain ordinary SELECT semantics.
        const auto plain = value(f.analyzeNode(SelectStmt{id("student"),
            std::vector<SelectItem>{selected("name")}, nullptr}));
        check(std::get<BoundSelect>(plain.node).aggregate_items.empty(),
              "rich plain SELECT was incorrectly treated as global aggregation");
    });
    suite.run("ORDER BY permits a hidden column and retains item order", [] {
        Fixture f;
        SelectStmt select{id("student"), std::vector<Identifier>{id("name")}, nullptr};
        select.order_by = {{id("age"), SortDirection::Desc, {}},
                           {id("id"), SortDirection::Asc, {}}};
        const auto ordered_bound = value(f.analyzeNode(select));
        const auto& ordered = std::get<BoundSelect>(ordered_bound.node);
        check(ordered.columns.size() == 1 && ordered.order_by.size() == 2 &&
              ordered.order_by[0].column.ordinal == 2 && ordered.order_by[1].column.ordinal == 0,
              "hidden ORDER BY column or item order was lost");
    });
    suite.run("ORDER BY expressions bind as typed sort keys", [] {
        Fixture f;
        SelectStmt select{id("student"), std::vector<Identifier>{id("name")}, nullptr};
        select.order_by = {{id(""), SortDirection::Desc, span(40, 7),
                            bin(BinaryOp::Add, col("age"), num(1), span(40, 7))}};
        const auto bound = value(f.analyzeNode(std::move(select)));
        const auto& query = std::get<BoundSelect>(bound.node);
        check(query.order_by.empty() && query.expression_order_by.size() == 1 &&
              std::get<BoundExprPtr>(query.expression_order_by[0].key)->type == DataType::Int,
              "ORDER BY expression type or direction was lost");
    });
    suite.run("nested expressions receive types without AST mutation", [] {
        Fixture f;
        auto sum = bin(BinaryOp::Add, col("AGE"), num(1));
        auto predicate = bin(BinaryOp::Or,
            un(UnaryOp::Not, bin(BinaryOp::Greater, sum, num(18))),
            bin(BinaryOp::And, truth(), bin(BinaryOp::Equal, col("name"), text("Alice"))));
        const auto bound = value(f.where(predicate));
        const auto& root = std::get<BoundSelect>(bound.node).where;
        check(root->type == DataType::Bool, "OR result not typed");
        const auto& logical = std::get<BoundBinary>(root->node);
        const auto& negation = std::get<BoundUnary>(logical.left->node);
        const auto& comparison = std::get<BoundBinary>(negation.operand->node);
        check(comparison.left->type == DataType::Int, "sum not typed INT");
        check(std::get<IdentifierExpr>(std::get<BinaryExpr>(sum->node).left->node).name.text == "AGE", "AST name changed");
    });
    suite.run("all integer arithmetic and comparison operators are accepted", [] {
        Fixture f;
        for (auto op : {BinaryOp::Add, BinaryOp::Subtract, BinaryOp::Multiply, BinaryOp::Divide}) {
            value(f.where(bin(BinaryOp::Equal, bin(op, col("age"), num(2)), num(20))));
        }
        for (auto op : {BinaryOp::Equal, BinaryOp::NotEqual, BinaryOp::Less, BinaryOp::LessEqual, BinaryOp::Greater, BinaryOp::GreaterEqual}) {
            value(f.where(bin(op, un(UnaryOp::Negate, col("age")), num(0))));
        }
    });
    suite.run("VARCHAR supports equality and LIKE but not ordering or arithmetic", [] {
        Fixture f;
        for (auto op : {BinaryOp::Equal, BinaryOp::NotEqual, BinaryOp::Like})
            value(f.where(bin(op, col("name"), text("Alice"))));
        for (auto op : {BinaryOp::Less, BinaryOp::Add, BinaryOp::Divide}) {
            failure(f.where(bin(op, col("name"), text("x"))), ErrorCode::InvalidOperandType);
        }
    });
    suite.run("invalid operator reports precise location and operand types", [] {
        Fixture f;
        auto e = failure(f.where(bin(BinaryOp::Add, col("age"), text("abc"), span(10))), ErrorCode::InvalidOperandType);
        check(e.span->begin.column == 11 && e.message.find("INT and VARCHAR") != std::string::npos, "missing operator diagnostic");
        e = failure(f.where(un(UnaryOp::Not, col("age"), span(6, 3))), ErrorCode::InvalidOperandType);
        check(e.span->begin.offset == 6, "NOT location mismatch");
        failure(f.where(un(UnaryOp::Negate, text("abc"))), ErrorCode::InvalidOperandType);
    });
    suite.run("BOOL logical operators equality and WHERE rules", [] {
        Fixture f;
        for (auto op : {BinaryOp::And, BinaryOp::Or}) failure(f.where(bin(op, num(1), truth())), ErrorCode::InvalidOperandType);
        value(f.where(bin(BinaryOp::Equal, truth(), truth())));
        auto e = failure(f.where(col("age", span(33, 3))), ErrorCode::WhereNotBoolean);
        check(e.span->begin.offset == 33, "WHERE type error location");
        failure(f.where(text("abc")), ErrorCode::WhereNotBoolean);
    });
    suite.run("aggregate expressions remain forbidden in WHERE", [] {
        Fixture f;
        auto aggregate = std::make_shared<const Expr>(Expr{
            AggregateCall{AggregateFunction::Count, AllColumns{span(15, 1)}, span(10, 8)},
            span(10, 8)});
        auto e = failure(f.where(bin(BinaryOp::Greater, aggregate, num(0), span(19))),
                         ErrorCode::InvalidGrouping);
        check(e.message.find("only allowed") != std::string::npos,
              "aggregate clause diagnostic changed");
    });
    suite.run("IN subqueries bind value query and boolean result", [] {
        Fixture f;
        auto query = std::make_shared<const SelectStmt>(
            SelectStmt{id("student"), std::vector<Identifier>{id("id")}, nullptr});
        auto predicate = std::make_shared<const Expr>(Expr{
            InSubqueryExpr{col("id"), query, false, span(20, 2)}, span(10, 35)});
        const auto bound = value(f.where(predicate));
        const auto& select = std::get<BoundSelect>(bound.node);
        const auto& in = std::get<BoundInSubquery>(select.where->node);
        check(select.where->type == DataType::Bool && in.query && !in.negated &&
              std::get<BoundColumnRef>(in.value->node).ordinal == 0,
              "IN subquery binding is incomplete");
    });
    suite.run("EXISTS subqueries bind as boolean expressions", [] {
        Fixture f;
        auto query = std::make_shared<const SelectStmt>(
            SelectStmt{id("student"), AllColumns{}, nullptr});
        auto predicate = std::make_shared<const Expr>(Expr{
            ExistsSubqueryExpr{query, false, span(20, 6)}, span(20, 35)});
        const auto bound = value(f.where(predicate));
        const auto& exists = std::get<BoundExistsSubquery>(
            std::get<BoundSelect>(bound.node).where->node);
        check(exists.query && !exists.negated,
              "EXISTS subquery binding is incomplete");
    });
    suite.run("derived tables expose synthetic aliased schemas", [] {
        Fixture f;
        auto query = std::make_shared<const SelectStmt>(
            SelectStmt{id("student"), std::vector<Identifier>{id("id")}, nullptr});
        SelectStmt select{id(""), AllColumns{}, nullptr};
        select.from = TableRef{Identifier{}, query, id("d"), span(14, 35)};
        const auto bound = value(f.analyzeNode(select));
        const auto& derived = std::get<BoundSelect>(bound.node);
        check(derived.source_query && derived.relation_name == "d" &&
              derived.table->columns.size() == 1 &&
              derived.table->columns[0].name == "id",
              "derived table did not expose its output schema");
    });
    suite.run("scalar subqueries bind one-column result types", [] {
        Fixture f;
        auto query = std::make_shared<const SelectStmt>(
            SelectStmt{id("student"), std::vector<Identifier>{id("age")}, nullptr});
        auto predicate = bin(BinaryOp::Greater, col("age"),
                             std::make_shared<const Expr>(Expr{
                                 ScalarSubqueryExpr{query, span(20, 30)}, span(20, 30)}));
        const auto bound = value(f.where(predicate));
        const auto& comparison = std::get<BoundBinary>(
            std::get<BoundSelect>(bound.node).where->node);
        const auto& scalar = std::get<BoundScalarSubquery>(comparison.right->node);
        check(scalar.query && comparison.right->type == DataType::Int,
              "scalar subquery type or query was lost");
    });
    suite.run("set operations bind every compatible branch", [] {
        Fixture f;
        SelectStmt select{id("student"), std::vector<Identifier>{id("id")}, nullptr};
        select.set_operations.push_back(SetOperation{
            SetOperator::Intersect, true,
            std::make_shared<const SelectStmt>(
                SelectStmt{id("student"), std::vector<Identifier>{id("id")}, nullptr}),
            span(20, 9)});
        const auto bound = value(f.analyzeNode(select));
        const auto& operation = std::get<BoundSelect>(bound.node).set_operations[0];
        check(operation.op == SetOperator::Intersect && operation.all && operation.query,
              "set operation branch or modifier was lost");
    });
    suite.run("CASE expressions bind branches and infer result type", [] {
        Fixture f;
        auto case_expr = std::make_shared<const Expr>(Expr{
            CaseExpr{nullptr,
                     {CaseWhenClause{bin(BinaryOp::Greater, col("age"), num(18)),
                                     truth(), span(10, 20)}},
                     bin(BinaryOp::NotEqual, num(1), num(1)),
                     span(5, 30)},
            span(5, 30)});
        const auto bound = value(f.where(case_expr));
        const auto& expression = std::get<BoundSelect>(bound.node).where;
        const auto& case_node = std::get<BoundCase>(expression->node);
        check(expression->type == DataType::Bool && case_node.branches.size() == 1 &&
              case_node.else_result,
              "CASE branch or inferred type was lost");
    });
    suite.run("semantic analysis checks both logical branches and left error first", [] {
        Fixture f;
        auto e = failure(f.where(bin(BinaryOp::Or, truth(), col("missing", span(40, 7)))), ErrorCode::ColumnNotFound);
        check(e.span->begin.offset == 40, "short circuit incorrectly hid semantic error");
        e = failure(f.where(bin(BinaryOp::Equal, col("left_missing", span(5)), col("right_missing", span(20)))), ErrorCode::ColumnNotFound);
        check(e.span->begin.offset == 5, "left-first diagnostics changed");
    });
    suite.run("runtime arithmetic errors are not evaluated during binding", [] {
        Fixture f;
        value(f.where(bin(BinaryOp::Equal, bin(BinaryOp::Divide, num(1), num(0)), num(0))));
        value(f.where(bin(BinaryOp::Equal, bin(BinaryOp::Add, num(std::numeric_limits<std::int64_t>::max()), num(1)), num(0))));
    });
    suite.run("null required child returns InvalidAst with fallback location", [] {
        Fixture f;
        auto e = failure(f.where(bin(BinaryOp::Equal, nullptr, num(1), span(20))), ErrorCode::InvalidAst);
        check(e.span->begin.offset == 20, "invalid child lacks parent location");
        failure(f.where(un(UnaryOp::Not, nullptr)), ErrorCode::InvalidAst);
    });
    suite.run("expression depth boundary prevents runaway recursion", [] {
        Fixture f; auto expr = truth();
        for (int i = 0; i < 254; ++i) expr = un(UnaryOp::Not, expr);
        value(f.where(expr)); // 254 NOT + comparison + literal = 256 层。
        failure(f.where(un(UnaryOp::Not, expr)), ErrorCode::ExpressionTooDeep);
    });
    suite.run("old snapshot and bound schema remain valid after new registration", [] {
        Fixture f;
        auto old = f.catalog.snapshot();
        Statement ast{SelectStmt{id("student"), AllColumns{}, nullptr}, {}};
        const auto bound = value(analyze(ast, *old));
        value(f.catalog.createTable("course", {{"id", DataType::Int}}));
        check(old->version() == 1 && !old->findTable("course"), "snapshot changed");
        check(bound.catalog_version == 1 && std::get<BoundSelect>(bound.node).table->name == "student", "bound schema invalidated");
        check(f.catalog.snapshot()->version() == 2, "new version missing");
    });
    suite.run("UPDATE binds integer arithmetic and string assignment", [] {
        Fixture f;
        const auto result = value(f.analyzeNode(UpdateStmt{id("STUDENT"),
            {{id("Age"), bin(BinaryOp::Add, col("age"), num(1)), {}}, {id("NAME"), text("Bob"), {}}},
            bin(BinaryOp::Equal, col("id"), num(1))}));
        const auto& update = std::get<BoundUpdate>(result.node);
        check(update.assignments[0].target.ordinal == 2 && update.assignments[0].value->type == DataType::Int,
              "arithmetic assignment not bound");
        check(update.assignments[1].target.ordinal == 1 && update.assignments[1].value->type == DataType::Varchar,
              "string assignment not bound");
        check(update.where->type == DataType::Bool && result.catalog_version == 1, "missing WHERE type/version");
    });
    suite.run("UPDATE swap retains original column references and AST", [] {
        Fixture f;
        UpdateStmt ast{id("student"), {{id("id"), col("AGE"), {}}, {id("age"), col("ID"), {}}}, nullptr};
        const auto result = value(f.analyzeNode(ast));
        const auto& update = std::get<BoundUpdate>(result.node);
        check(std::get<BoundColumnRef>(update.assignments[0].value->node).ordinal == 2 &&
              std::get<BoundColumnRef>(update.assignments[1].value->node).ordinal == 0,
              "later assignment substituted earlier assignment instead of old column");
        check(std::get<IdentifierExpr>(ast.assignments[1].value->node).name.text == "ID", "UPDATE AST mutated");
        check(f.catalog.snapshot()->version() == 1, "UPDATE analysis mutated catalog");
    });
    suite.run("UPDATE and DELETE require existing table", [] {
        Fixture f;
        auto e = failure(f.analyzeNode(UpdateStmt{id("missing", span(7, 7)), {{id("id"), num(1), {}}}, nullptr}), ErrorCode::TableNotFound);
        check(e.span->begin.offset == 7, "UPDATE table diagnostic location");
        e = failure(f.analyzeNode(DeleteStmt{id("missing", span(12, 7)), nullptr}), ErrorCode::TableNotFound);
        check(e.span->begin.offset == 12, "DELETE table diagnostic location");
    });
    suite.run("UPDATE detects repeated assignment ignoring name case", [] {
        Fixture f;
        auto e = failure(f.analyzeNode(UpdateStmt{id("student"),
            {{id("age"), num(20), {}}, {id("AGE", span(32, 3)), num(21), {}}}, nullptr}), ErrorCode::DuplicateAssignment);
        check(e.span->begin.offset == 32, "duplicate must point to second target");
    });
    suite.run("UPDATE resolves target and RHS names separately", [] {
        Fixture f;
        auto e = failure(f.analyzeNode(UpdateStmt{id("student"), {{id("score", span(20, 5)), num(1), {}}}, nullptr}), ErrorCode::ColumnNotFound);
        check(e.span->begin.offset == 20, "target column location");
        e = failure(f.analyzeNode(UpdateStmt{id("student"), {{id("age"), col("score", span(26, 5)), {}}}, nullptr}), ErrorCode::ColumnNotFound);
        check(e.span->begin.offset == 26, "RHS column location");
    });
    suite.run("UPDATE type mismatch locates RHS and rejects BOOL assignment", [] {
        Fixture f;
        auto e = failure(f.analyzeNode(UpdateStmt{id("student"), {{id("age"), lit(std::string{"abc"}, span(25, 5)), {}}}, nullptr}), ErrorCode::TypeMismatch);
        check(e.span->begin.offset == 25 && e.message.find("student.age expects INT") != std::string::npos, "assignment type diagnostic");
        failure(f.analyzeNode(UpdateStmt{id("student"), {{id("age"), truth(), {}}}, nullptr}), ErrorCode::TypeMismatch);
        const auto nullable = value(f.analyzeNode(
            UpdateStmt{id("student"), {{id("name"), lit(NullValue{}), {}}}, nullptr}));
        check(std::get<BoundUpdate>(nullable.node).assignments[0].value->type == DataType::Null,
              "nullable UPDATE should retain the NULL literal");
    });
    suite.run("UPDATE and DELETE share boolean WHERE checking", [] {
        Fixture f;
        auto e = failure(f.analyzeNode(UpdateStmt{id("student"), {{id("age"), num(1), {}}}, col("id", span(38, 2))}), ErrorCode::WhereNotBoolean);
        check(e.span->begin.offset == 38, "UPDATE WHERE location");
        failure(f.analyzeNode(DeleteStmt{id("student"), col("id")}), ErrorCode::WhereNotBoolean);
        failure(f.analyzeNode(DeleteStmt{id("student"), bin(BinaryOp::Add, col("name"), num(1))}), ErrorCode::InvalidOperandType);
    });
    suite.run("UPDATE and DELETE resolve WHERE against full table", [] {
        Fixture f;
        auto condition = bin(BinaryOp::And, bin(BinaryOp::Greater, col("age"), num(18)),
                             un(UnaryOp::Not, bin(BinaryOp::Equal, col("name"), text("Tom"))));
        value(f.analyzeNode(UpdateStmt{id("student"), {{id("id"), num(10), {}}}, condition}));
        const auto result = value(f.analyzeNode(DeleteStmt{id("STUDENT"), condition}));
        check(std::get<BoundDelete>(result.node).where->type == DataType::Bool, "DELETE compound condition not typed");
        failure(f.analyzeNode(UpdateStmt{id("student"), {{id("id"), num(10), {}}}, col("missing")}), ErrorCode::ColumnNotFound);
        failure(f.analyzeNode(DeleteStmt{id("student"), col("missing")}), ErrorCode::ColumnNotFound);
    });
    suite.run("UPDATE and DELETE table aliases qualify the single table scope", [] {
        Fixture f;
        UpdateStmt update{id("student"),
            {{id("s.age"), bin(BinaryOp::Add, col("s.age"), num(1)), {}}},
            bin(BinaryOp::Equal, col("s.id"), num(1))};
        update.table_alias = id("s");
        const auto bound_update = value(f.analyzeNode(update));
        const auto& updated = std::get<BoundUpdate>(bound_update.node);
        check(updated.assignments[0].target.ordinal == 2 &&
                  std::get<BoundBinary>(updated.assignments[0].value->node).left->type == DataType::Int,
              "UPDATE alias-qualified target or RHS did not bind");

        DeleteStmt deletion{id("student"), bin(BinaryOp::Equal, col("s.id"), num(1))};
        deletion.table_alias = id("s");
        value(f.analyzeNode(deletion));

        value(f.analyzeNode(DeleteStmt{id("student"),
            bin(BinaryOp::Equal, col("student.id"), num(1))}));
        DeleteStmt hidden_physical{id("student"),
            bin(BinaryOp::Equal, col("student.id", span(20, 10)), num(1))};
        hidden_physical.table_alias = id("s");
        auto e = failure(f.analyzeNode(std::move(hidden_physical)), ErrorCode::ColumnNotFound);
        check(e.span->begin.offset == 20, "alias should hide physical table qualifier");
    });
    suite.run("UPDATE and DELETE without WHERE mean all rows", [] {
        Fixture f;
        const auto update = value(f.analyzeNode(UpdateStmt{id("student"), {{id("age"), num(0), {}}}, nullptr}));
        const auto deletion = value(f.analyzeNode(DeleteStmt{id("student"), nullptr}));
        check(!std::get<BoundUpdate>(update.node).where && !std::get<BoundDelete>(deletion.node).where, "absent WHERE became a predicate");
    });
    suite.run("UPDATE rejects empty assignment list and null RHS", [] {
        Fixture f;
        failure(f.analyzeNode(UpdateStmt{id("student"), {}, nullptr}), ErrorCode::InvalidAst);
        auto e = failure(f.analyzeNode(UpdateStmt{id("student"), {{id("age"), nullptr, span(20, 6)}}, nullptr}), ErrorCode::InvalidAst);
        check(e.span->begin.offset == 20, "null RHS must use assignment range");
    });
    suite.run("EXPLAIN preserves target binding and ANALYZE mode", [] {
        Fixture f;
        ExplainStmt explain{SelectStmt{id("student"), std::vector<Identifier>{id("name")},
            bin(BinaryOp::Greater, col("age"), num(18))}, true};
        const auto bound = value(f.analyzeNode(std::move(explain)));
        const auto& wrapper = std::get<BoundExplain>(bound.node);
        check(wrapper.analyze && wrapper.target &&
              std::holds_alternative<BoundSelect>(wrapper.target->node),
              "EXPLAIN target did not pass through semantic analysis");
        ExplainStmt missing{SelectStmt{id("missing"), AllColumns{}, nullptr}, false};
        failure(f.analyzeNode(std::move(missing)), ErrorCode::TableNotFound);
    });
    return suite.finish();
}
