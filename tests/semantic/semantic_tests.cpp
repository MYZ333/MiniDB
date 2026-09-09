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

InsertStmt insert() {
    return {id("student"), std::nullopt,
        {{std::int64_t{1}, span(30)}, {std::string{"Alice"}, span(33, 7)}, {std::int64_t{20}, span(42, 2)}}};
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
    suite.run("CREATE validates columns and rejects BOOL", [] {
        Fixture f;
        failure(f.analyzeNode(CreateTableStmt{id("newtable"), {}}), ErrorCode::EmptyColumnList);
        auto e = failure(f.analyzeNode(CreateTableStmt{id("newtable"),
            {{id("id"), DataType::Int, {}}, {id("ID", span(25, 2)), DataType::Int, {}}}}), ErrorCode::DuplicateColumn);
        check(e.span->begin.offset == 25, "duplicate should locate second column");
        failure(f.analyzeNode(CreateTableStmt{id("newtable"), {{id("flag"), DataType::Bool, span(20, 4)}}}), ErrorCode::UnsupportedType);
    });
    suite.run("INSERT omitted list uses schema order", [] {
        Fixture f;
        auto binding = value(f.analyzeNode(insert()));
        const auto& row = std::get<BoundInsert>(binding.node);
        check(std::get<std::int64_t>(row.values[0]) == 1 && std::get<std::string>(row.values[1]) == "Alice", "wrong row mapping");
        check(binding.catalog_version == 1, "missing catalog version");
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
    suite.run("INSERT distinguishes empty list, count mismatch and missing coverage", [] {
        Fixture f; auto ast = insert();
        ast.columns = std::vector<Identifier>{};
        failure(f.analyzeNode(ast), ErrorCode::EmptyColumnList);
        ast.columns = std::nullopt; ast.values.pop_back();
        failure(f.analyzeNode(ast), ErrorCode::ValueCountMismatch);
        ast.columns = std::vector<Identifier>{id("id"), id("name")};
        failure(f.analyzeNode(ast), ErrorCode::MissingInsertColumn);
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
    suite.run("VARCHAR only supports equality and inequality", [] {
        Fixture f;
        for (auto op : {BinaryOp::Equal, BinaryOp::NotEqual}) value(f.where(bin(op, col("name"), text("Alice"))));
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
    suite.run("BOOL is required for logical operators and WHERE", [] {
        Fixture f;
        for (auto op : {BinaryOp::And, BinaryOp::Or}) failure(f.where(bin(op, num(1), truth())), ErrorCode::InvalidOperandType);
        failure(f.where(bin(BinaryOp::Equal, truth(), truth())), ErrorCode::InvalidOperandType);
        auto e = failure(f.where(col("age", span(33, 3))), ErrorCode::WhereNotBoolean);
        check(e.span->begin.offset == 33, "WHERE type error location");
        failure(f.where(text("abc")), ErrorCode::WhereNotBoolean);
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
    return suite.finish();
}
