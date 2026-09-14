// 使用真实 SQL 编译得到原计划，再验证改写、整数边界及独立参考求值结果。
#include "minisql/lexer.hpp"
#include "minisql/parser.hpp"
#include "minisql/memory_catalog.hpp"
#include "minisql/optimizer.hpp"
#include "minisql/plan_printer.hpp"
#include "../test_support.hpp"
#include "reference_evaluator.hpp"

#include <limits>

using namespace minisql;
using namespace minisql::test;
namespace reference = minisql::test_reference;

namespace {
struct Fixture {
    MemoryCatalog catalog;
    Fixture() { value(catalog.createTable("student", {{"id", DataType::Int}, {"name", DataType::Varchar}, {"age", DataType::Int}})); }
    LogicalPlan compile(const std::string& sql) {
        const auto statements = value(parse(value(lex(sql))));
        check(statements.size() == 1, "one SQL statement expected");
        return value(buildPlan(value(analyze(statements[0], *catalog.snapshot()))));
    }
    LogicalPlan update(const std::string& expression) {
        return compile("UPDATE student SET age=" + expression + ";");
    }
};

BoundExprPtr rhs(const LogicalPlan& plan) { return std::get<UpdatePlan>(plan.root->node).assignments[0].value; }
const FilterPlan& selectFilter(const LogicalPlan& plan) {
    return std::get<FilterPlan>(std::get<ProjectPlan>(plan.root->node).input->node);
}
reference::Rows sampleRows() {
    return {{std::int64_t{1}, std::string{"Alice"}, std::int64_t{20}},
            {std::int64_t{2}, std::string{"Tom"}, std::int64_t{0}},
            {std::int64_t{3}, std::string{"Alice"}, std::int64_t{-2}},
            {std::int64_t{4}, std::string{"Bob"}, std::int64_t{18}}};
}
void expectEquivalent(const LogicalPlan& before, const reference::Rows& rows) {
    const auto after = value(optimizePlan(before));
    check(reference::equivalent(reference::run(before, rows), reference::run(after, rows)),
          "optimized result, effects or runtime error changed");
}
} // namespace

int main() {
    Suite suite;
    suite.run("PPT example folds constants and simplifies BOOL", [] {
        Fixture f;
        const auto before = f.compile("SELECT name FROM student WHERE 1=1 AND age>10+8;");
        const auto after = value(optimizePlan(before));
        const auto& comparison = std::get<BoundBinary>(selectFilter(after).predicate->node);
        check(comparison.op == BinaryOp::Greater && std::get<BoundColumnRef>(comparison.left->node).ordinal == 2,
              "PPT predicate lost comparison or binding");
        check(std::get<std::int64_t>(std::get<BoundLiteral>(comparison.right->node).value) == 18, "10+8 did not fold");
        check(formatPlan(before).find("AND") != std::string::npos && formatPlan(after).find("AND") == std::string::npos,
              "before/after difference missing");
        expectEquivalent(before, sampleRows());
    });
    suite.run("true Filter removal preserves output version and RowId", [] {
        Fixture f;
        for (const std::string sql : {"SELECT name,name FROM student WHERE 2=2;", "UPDATE student SET age=18 WHERE 2=2;", "DELETE FROM student WHERE 2=2;"}) {
            auto before = f.compile(sql);
            auto after = value(optimizePlan(before));
            check(after.catalog_version == before.catalog_version && after.root->output.size() == before.root->output.size(), "root metadata changed");
            check(formatPlan(after).find("Filter[") == std::string::npos, "true Filter not removed");
            if (const auto* update = std::get_if<UpdatePlan>(&after.root->node)) check(update->input->carries_row_id, "UPDATE lost RowId");
            if (const auto* deletion = std::get_if<DeletePlan>(&after.root->node)) check(deletion->input->carries_row_id, "DELETE lost RowId");
            expectEquivalent(before, sampleRows());
        }
    });
    suite.run("false Filter and DML roots remain present", [] {
        Fixture f;
        for (const std::string sql : {"SELECT * FROM student WHERE 1=0;", "UPDATE student SET age=1/0 WHERE 1=0;", "DELETE FROM student WHERE 1=0;"}) {
            const auto before = f.compile(sql);
            const auto after = value(optimizePlan(before));
            check(before.root->node.index() == after.root->node.index(), "DML/query root removed");
            check(formatPlan(after).find("Filter[FALSE]") != std::string::npos, "false Filter removed");
            expectEquivalent(before, sampleRows());
        }
    });
    suite.run("aggregate boundary survives filter optimization and empty input", [] {
        Fixture f;
        const auto before = f.compile("SELECT COUNT(*) AS rows FROM student WHERE 1=1;");
        const auto after = value(optimizePlan(before));
        const auto& aggregate = std::get<AggregatePlan>(after.root->node);
        check(std::holds_alternative<SeqScanPlan>(aggregate.input->node) &&
              after.root->output[0].name == "rows", "aggregate metadata lost on input rewrite");
        const auto empty = value(optimizePlan(
            f.compile("SELECT COUNT(*) FROM student WHERE 1=0;")));
        const auto& filtered = std::get<AggregatePlan>(empty.root->node);
        check(std::holds_alternative<FilterPlan>(filtered.input->node),
              "global aggregate over empty input must retain its result boundary");
    });
    suite.run("UPDATE RHS folds without replacing old-row references", [] {
        Fixture f;
        const auto before = f.compile("UPDATE student SET id=age+10+8,age=id+(2*3) WHERE 1=1;");
        const auto after = value(optimizePlan(before));
        const auto& assignments = std::get<UpdatePlan>(after.root->node).assignments;
        const auto& second = std::get<BoundBinary>(assignments[1].value->node);
        check(std::get<BoundColumnRef>(second.left->node).ordinal == 0 &&
              std::get<std::int64_t>(std::get<BoundLiteral>(second.right->node).value) == 6, "UPDATE binding/folding mismatch");
        expectEquivalent(before, sampleRows());
    });
    suite.run("constant arithmetic exact boundary results", [] {
        Fixture f;
        const auto min = std::numeric_limits<std::int64_t>::min();
        const auto max = std::numeric_limits<std::int64_t>::max();
        const std::vector<std::pair<std::string, std::int64_t>> cases{
            {"10+8",18},{"20-3",17},{"6*7",42},{"-7/2",-3},{"7/-2",-3},{"-7/-2",3},
            {"9223372036854775806+1",max},{"-9223372036854775807-1",min},
            {"-9223372036854775808-(-9223372036854775808)",0},
            {"9223372036854775807+(-9223372036854775808)",-1},
            {"-9223372036854775808*1",min},{"1*(-9223372036854775808)",min},
            {"0*(-9223372036854775808)",0},{"-3*(-4)",12},{"-3*4",-12},{"3*(-4)",-12},
            {"-9223372036854775808/1",min},{"-(-9223372036854775807)",max},
            {"3037000499*3037000499",9223372030926249001LL}};
        for (const auto& item : cases) {
            const auto after = value(optimizePlan(f.update(item.first)));
            const auto* literal = std::get_if<BoundLiteral>(&rhs(after)->node);
            check(literal && std::get<std::int64_t>(literal->value) == item.second, "constant arithmetic result wrong");
        }
    });
    suite.run("overflow and division by zero retain their expression and range", [] {
        Fixture f;
        for (const std::string expression : {
            "9223372036854775807+1", "-9223372036854775808+(-1)",
            "-9223372036854775808-1", "9223372036854775807-(-1)",
            "9223372036854775807*2", "-9223372036854775808*2", "2*(-9223372036854775808)",
            "-9223372036854775808*(-1)", "-1*(-9223372036854775808)",
            "3037000500*3037000500", "-9223372036854775808/(-1)", "-(-9223372036854775808)", "1/0"}) {
            const auto before = f.update(expression);
            const auto after = value(optimizePlan(before));
            check(rhs(after) == rhs(before), "unsafe constant was folded or its error range changed");
        }
    });
    suite.run("all constant comparisons and NOT fold", [] {
        Fixture f;
        for (const std::string expression : {"1=1", "1!=2", "1<2", "1<=1", "2>1", "2>=2", "NOT(1=0)", "'Tom''s'='Tom''s'", "'Alice'!='alice'"}) {
            const auto after = value(optimizePlan(f.compile("SELECT * FROM student WHERE " + expression + ";")));
            check(std::holds_alternative<SeqScanPlan>(std::get<ProjectPlan>(after.root->node).input->node), "true comparison not folded");
        }
        const auto after = value(optimizePlan(f.compile("SELECT * FROM student WHERE 'Alice'='alice';")));
        check(formatPlan(after).find("Filter[FALSE]") != std::string::npos, "string case changed");
    });
    suite.run("FLOAT and BOOL constants fold after binding", [] {
        MemoryCatalog catalog;
        value(catalog.createTable("metrics", {{"active", DataType::Bool},
                                               {"score", DataType::Float}}));
        const auto compile = [&](const std::string& sql) {
            const auto statements = value(parse(value(lex(sql))));
            return value(buildPlan(value(analyze(statements[0], *catalog.snapshot()))));
        };
        auto after = value(optimizePlan(compile(
            "SELECT score FROM metrics WHERE TRUE AND score>10.0+8.5;")));
        check(formatPlan(after).find("18.5") != std::string::npos,
              "FLOAT arithmetic was not folded");
        after = value(optimizePlan(compile("SELECT score FROM metrics WHERE TRUE=TRUE;")));
        check(formatPlan(after).find("Filter[") == std::string::npos,
              "BOOL equality or true Filter was not folded");
    });
    suite.run("safe BOOL identities preserve column evaluation", [] {
        Fixture f;
        for (const std::string expr : {"(1=1) AND age>0", "(1=0) OR age>0", "age>0 AND (1=1)", "age>0 OR (1=0)"}) {
            const auto before = f.compile("SELECT * FROM student WHERE " + expr + ";");
            const auto after = value(optimizePlan(before));
            check(std::get<BoundBinary>(selectFilter(after).predicate->node).op == BinaryOp::Greater, "safe BOOL identity not applied");
            expectEquivalent(before, sampleRows());
        }
    });
    suite.run("left short circuit safely drops unreachable division", [] {
        Fixture f;
        for (const std::string expr : {"1=0 AND 1/0=1", "1=1 OR 1/0=1"}) {
            const auto before = f.compile("SELECT * FROM student WHERE " + expr + ";");
            const auto after = value(optimizePlan(before));
            check(formatPlan(after).find(" / ") == std::string::npos, "unreachable RHS not removed");
            check(!reference::run(after, sampleRows()).error, "short circuit introduced error");
            expectEquivalent(before, sampleRows());
        }
    });
    suite.run("right absorbing constant cannot hide left runtime error", [] {
        Fixture f;
        for (const std::string expr : {"1/0=1 AND 1=0", "1/0=1 OR 1=1"}) {
            const auto before = f.compile("SELECT * FROM student WHERE " + expr + ";");
            const auto after = value(optimizePlan(before));
            check(formatPlan(after).find(" / ") != std::string::npos, "left runtime error incorrectly removed");
            check(reference::run(after, sampleRows()).error == ErrorCode::DivisionByZero, "expected error disappeared");
            expectEquivalent(before, sampleRows());
        }
    });
    suite.run("error-producing arithmetic is not erased by multiplication by zero", [] {
        Fixture f;
        const auto before = f.compile("SELECT * FROM student WHERE (1/0)*0=0;");
        const auto after = value(optimizePlan(before));
        check(formatPlan(after).find(" / ") != std::string::npos, "arithmetic identity hid division error");
        expectEquivalent(before, sampleRows());
    });
    suite.run("partial folding retains failing operator source position", [] {
        Fixture f;
        const auto before = f.compile("SELECT * FROM student\nWHERE (2+3)/(age-age)=0;");
        const auto after = value(optimizePlan(before));
        const auto expected = reference::run(before, sampleRows());
        const auto actual = reference::run(after, sampleRows());
        check(expected.error == ErrorCode::DivisionByZero && expected.error_span->begin.line == 2,
              "expected source error missing");
        check(reference::equivalent(expected, actual), "runtime diagnostic moved during partial fold");
    });
    suite.run("folded literals retain original expression span", [] {
        Fixture f;
        const auto before = f.update("(10+8)");
        const auto after = value(optimizePlan(before));
        check(reference::sameSpan(rhs(before)->span, rhs(after)->span), "folded value lost expression range");
    });
    suite.run("original plan immutable and optimization idempotent", [] {
        Fixture f;
        const auto before = f.compile("SELECT name,name FROM student WHERE 1=1 AND age>10+8;");
        const auto text = formatPlan(before);
        const auto after = value(optimizePlan(before));
        const auto again = value(optimizePlan(after));
        check(formatPlan(before) == text && before.root != after.root, "original tree mutated");
        check(again.root == after.root && formatPlan(again) == formatPlan(after), "optimizer did not reach a fixed point");
        check(after.root->output.size() == 2 && after.root->output[0].name == after.root->output[1].name, "duplicate output columns changed");
        check(f.catalog.snapshot()->version() == 1, "optimizer changed catalog");
    });
    suite.run("unaffected CREATE INSERT and SELECT nodes are reused", [] {
        Fixture f;
        for (const std::string sql : {"CREATE TABLE other(id INT);", "INSERT INTO student VALUES (1,'Alice',20);", "SELECT * FROM student;"}) {
            const auto before = f.compile(sql);
            const auto after = value(optimizePlan(before));
            check(before.root == after.root && before.catalog_version == after.catalog_version, "unchanged tree copied or altered");
        }
    });
    suite.run("optimizer traverses JOIN GROUP BY and ORDER BY nodes", [] {
        Fixture f;
        value(f.catalog.createTable("score", {{"student_id", DataType::Int},
                                               {"value", DataType::Int}}));
        const auto before = f.compile(
            "SELECT student.name FROM student "
            "JOIN score ON student.id=score.student_id AND 1=1 "
            "WHERE 2=2 GROUP BY student.name ORDER BY student.name DESC;");
        const auto after = value(optimizePlan(before));
        const auto& project = std::get<ProjectPlan>(after.root->node);
        const auto& sort = std::get<SortPlan>(project.input->node);
        const auto& group = std::get<GroupByPlan>(sort.input->node);
        const auto& join = std::get<NestedLoopJoinPlan>(group.input->node);
        check(std::holds_alternative<BoundBinary>(join.predicate->node) &&
              formatPlan(after).find("AND") == std::string::npos &&
              formatPlan(after).find("Filter[") == std::string::npos,
              "advanced-node traversal did not simplify predicates");
        const auto again = value(optimizePlan(after));
        check(again.root == after.root, "advanced optimized plan is not idempotent");
    });
    suite.run("SELECT equivalence on multiple predicates and rows", [] {
        Fixture f;
        for (const std::string predicate : {"1=1 AND age>10+8", "NOT(1=0) AND (id>1 OR 2=3)",
            "(age+2*3)>5 AND name!='Tom'", "1=0 OR (age/2<5 AND 1=1)", "age>0 OR 1=1", "age>0 AND 1=0"}) {
            const auto before = f.compile("SELECT name,id,name FROM student WHERE " + predicate + ";");
            expectEquivalent(before, sampleRows());
            expectEquivalent(before, {});
        }
    });
    suite.run("UPDATE and DELETE equivalence preserves rows and affected count", [] {
        Fixture f;
        for (const std::string sql : {"UPDATE student SET id=age,age=id+2*3 WHERE age>10+8 OR 1=0;",
            "UPDATE student SET age=10+8,name='Bob' WHERE 1=1;", "DELETE FROM student WHERE age>10+8 AND 1=1;",
            "DELETE FROM student WHERE 1=0;", "DELETE FROM student WHERE 1=1;"}) {
            const auto before = f.compile(sql);
            expectEquivalent(before, sampleRows());
            expectEquivalent(before, {});
        }
    });
    suite.run("runtime RHS errors remain deferred when no rows match", [] {
        Fixture f;
        const auto before = f.compile("UPDATE student SET age=1/0 WHERE 1=0;");
        const auto after = value(optimizePlan(before));
        check(!reference::run(after, sampleRows()).error, "optimization raised an unreachable RHS error");
        expectEquivalent(before, sampleRows());
        expectEquivalent(f.compile("UPDATE student SET age=(2+3)/0 WHERE id=1;"), sampleRows());
    });
    suite.run("deterministic expression matrix equivalence", [] {
        Fixture f;
        // 独立参考求值器在小整数域验证 49 种表达式组合；输入包含使除数为零的行。
        for (int a = -3; a <= 3; ++a) {
            for (int b = -3; b <= 3; ++b) {
                const auto sql = "SELECT id FROM student WHERE 1=0 OR ((age+(" + std::to_string(a) +
                    "+2))/(id+(" + std::to_string(b) + "))>0 AND 1=1);";
                expectEquivalent(f.compile(sql), sampleRows());
            }
        }
    });
    suite.run("invalid plan root input and Filter metadata return diagnostics", [] {
        failure(optimizePlan(LogicalPlan{0, nullptr}), ErrorCode::InvalidPlan, DiagnosticStage::Plan);
        Fixture f;
        auto before = f.compile("SELECT * FROM student WHERE 1=1;");
        const auto& project = std::get<ProjectPlan>(before.root->node);
        auto bad = *project.input;
        std::get<FilterPlan>(bad.node).input = nullptr;
        failure(optimizePlan(LogicalPlan{1, std::make_shared<const PlanNode>(bad)}), ErrorCode::InvalidPlan, DiagnosticStage::Plan);
        bad = *project.input; bad.carries_row_id = true;
        failure(optimizePlan(LogicalPlan{1, std::make_shared<const PlanNode>(bad)}), ErrorCode::InvalidPlan, DiagnosticStage::Plan);
        bad = *project.input; std::get<FilterPlan>(bad.node).predicate = nullptr;
        failure(optimizePlan(LogicalPlan{1, std::make_shared<const PlanNode>(bad)}), ErrorCode::InvalidPlan, DiagnosticStage::Plan);
    });
    suite.run("UPDATE missing RHS and missing row identity return diagnostics", [] {
        Fixture f;
        auto before = f.update("10+8");
        auto bad = *before.root;
        std::get<UpdatePlan>(bad.node).assignments[0].value = nullptr;
        failure(optimizePlan(LogicalPlan{1, std::make_shared<const PlanNode>(bad)}), ErrorCode::InvalidPlan, DiagnosticStage::Plan);
        bad = *before.root;
        auto scan = *std::get<UpdatePlan>(bad.node).input;
        scan.carries_row_id = false;
        std::get<UpdatePlan>(bad.node).input = std::make_shared<const PlanNode>(scan);
        failure(optimizePlan(LogicalPlan{1, std::make_shared<const PlanNode>(bad)}), ErrorCode::InvalidPlan, DiagnosticStage::Plan);
    });
    suite.run("malformed JOIN GROUP BY and ORDER BY plans return diagnostics", [] {
        Fixture f;
        const auto scan_plan = std::get<ProjectPlan>(f.compile("SELECT * FROM student;").root->node).input;
        auto truth_expr = std::make_shared<const BoundExpr>(BoundExpr{
            BoundLiteral{true}, DataType::Bool, {}});
        auto join = std::make_shared<const PlanNode>(PlanNode{
            NestedLoopJoinPlan{scan_plan, nullptr, truth_expr}, scan_plan->output, false});
        failure(optimizePlan(LogicalPlan{1, join}), ErrorCode::InvalidPlan, DiagnosticStage::Plan);
        auto group = std::make_shared<const PlanNode>(PlanNode{
            GroupByPlan{{}, scan_plan}, {}, false});
        failure(optimizePlan(LogicalPlan{1, group}), ErrorCode::InvalidPlan, DiagnosticStage::Plan);
        auto sort = std::make_shared<const PlanNode>(PlanNode{
            SortPlan{{}, scan_plan}, scan_plan->output, false});
        failure(optimizePlan(LogicalPlan{1, sort}), ErrorCode::InvalidPlan, DiagnosticStage::Plan);
    });
    return suite.finish();
}
