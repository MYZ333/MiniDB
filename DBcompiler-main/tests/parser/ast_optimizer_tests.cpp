// A 的 AST 优化回归：检查纯语法改写、短路安全和新增 SELECT 子句遍历。
#include "minisql/ast_optimizer.hpp"
#include "minisql/lexer.hpp"
#include "minisql/parser.hpp"
#include "../test_support.hpp"

using namespace minisql;
using namespace minisql::test;

namespace {
Statement parsed(const std::string& sql) {
    const auto statements = value(parse(value(lex(sql))));
    check(statements.size() == 1, "one statement expected");
    return statements[0];
}

const SelectStmt& selected(const Statement& statement) {
    return std::get<SelectStmt>(statement.node);
}
} // namespace

int main() {
    Suite suite;
    suite.run("AST constants and true WHERE fold", [] {
        const auto after = optimizeAstStatement(parsed(
            "SELECT * FROM t WHERE 1=1 AND score>10.0+8.5;"));
        const auto& where = selected(after).where;
        const auto& comparison = std::get<BinaryExpr>(where->node);
        const auto& result = std::get<LiteralExpr>(comparison.right->node).value;
        check(comparison.op == BinaryOp::Greater && std::get<double>(result) == 18.5,
              "FLOAT constant or TRUE identity did not fold");
        check(selected(optimizeAstStatement(parsed("SELECT * FROM t WHERE TRUE;"))).where == nullptr,
              "true WHERE was not removed");
    });
    suite.run("AST short circuit keeps reachable runtime errors", [] {
        auto after = optimizeAstStatement(parsed("SELECT * FROM t WHERE FALSE AND 1/0=1;"));
        check(std::get<bool>(std::get<LiteralExpr>(selected(after).where->node).value) == false,
              "left short circuit did not fold");
        after = optimizeAstStatement(parsed("SELECT * FROM t WHERE 1/0=1 AND FALSE;"));
        check(std::holds_alternative<BinaryExpr>(selected(after).where->node),
              "right absorbing constant hid reachable division");
    });
    suite.run("AST JOIN ON and UPDATE RHS are traversed", [] {
        auto after = optimizeAstStatement(parsed(
            "SELECT t.id FROM t JOIN u ON TRUE AND t.id=u.id WHERE TRUE;"));
        const auto& select = selected(after);
        check(select.where == nullptr && select.joins.size() == 1 &&
              std::get<BinaryExpr>(select.joins[0].on->node).op == BinaryOp::Equal,
              "JOIN ON or WHERE optimization mismatch");
        after = optimizeAstStatement(parsed("UPDATE t SET id=10+8;"));
        const auto& value_expr = std::get<UpdateStmt>(after.node).assignments[0].value;
        check(std::get<std::int64_t>(std::get<LiteralExpr>(value_expr->node).value) == 18,
              "UPDATE RHS was not folded");
    });
    suite.run("AST optimizer does not fold NULL or mixed numeric types", [] {
        for (const auto* sql : {"SELECT * FROM t WHERE NULL=NULL;",
                                "SELECT * FROM t WHERE 1+2.0=3.0;"}) {
            const auto optimized = optimizeAstStatement(parsed(sql));
            const auto& where = selected(optimized).where;
            check(std::holds_alternative<BinaryExpr>(where->node),
                  "AST optimizer guessed semantic conversion or NULL behavior");
        }
    });
    suite.run("AST optimizer folds literal IS NULL checks", [] {
        auto after = optimizeAstStatement(parsed("SELECT * FROM t WHERE NULL IS NULL;"));
        check(selected(after).where == nullptr, "TRUE IS NULL result should remove WHERE");
        after = optimizeAstStatement(parsed("SELECT * FROM t WHERE 1 IS NULL;"));
        check(std::get<bool>(std::get<LiteralExpr>(selected(after).where->node).value) == false,
              "non-null literal IS NULL did not fold to FALSE");
        after = optimizeAstStatement(parsed("SELECT * FROM t WHERE 'x' IS NOT NULL;"));
        check(selected(after).where == nullptr, "literal IS NOT NULL TRUE should remove WHERE");
    });
    suite.run("AST optimizer leaves LIKE semantics to B", [] {
        const auto after = optimizeAstStatement(parsed("SELECT * FROM t WHERE 'Alice' LIKE 'A%';"));
        check(std::get<BinaryExpr>(selected(after).where->node).op == BinaryOp::Like,
              "LIKE was folded before B defines pattern semantics");
    });
    suite.run("AST optimizer traverses HAVING", [] {
        const auto after = optimizeAstStatement(parsed(
            "SELECT active FROM t GROUP BY active HAVING TRUE AND active = TRUE;"));
        const auto& having = selected(after).having;
        check(having != nullptr && std::get<BinaryExpr>(having->node).op == BinaryOp::Equal,
              "HAVING constant identity was not folded");
    });
    suite.run("AST optimizer traverses SELECT expression items without folding aggregates", [] {
        const auto after = optimizeAstStatement(parsed(
            "SELECT 10 + 8 AS folded, COUNT(*) + 1 AS count_plus_one FROM t;"));
        const auto& items = std::get<std::vector<SelectItem>>(selected(after).columns);
        const auto& folded = std::get<ExprPtr>(items[0]);
        check(std::get<std::int64_t>(std::get<LiteralExpr>(folded->node).value) == 18,
              "SELECT expression item was not folded");
        const auto& aggregate_add = std::get<BinaryExpr>(std::get<ExprPtr>(items[1])->node);
        check(std::holds_alternative<AggregateCall>(aggregate_add.left->node),
              "aggregate expression was lost during optimization");
    });
    suite.run("AST optimizer traverses ORDER BY expressions", [] {
        const auto after = optimizeAstStatement(parsed(
            "SELECT id FROM t ORDER BY 10 + 8 DESC;"));
        const auto& order = selected(after).order_by[0];
        check(order.expression &&
                  std::get<std::int64_t>(std::get<LiteralExpr>(order.expression->node).value) == 18,
              "ORDER BY expression was not folded");
    });
    suite.run("AST optimizer traverses an EXPLAIN target", [] {
        const auto after = optimizeAstStatement(parsed(
            "EXPLAIN ANALYZE SELECT id FROM t WHERE TRUE AND id>10+8;"));
        const auto& explain = std::get<ExplainStmt>(after.node);
        const auto& select = std::get<SelectStmt>(explain.target);
        const auto& comparison = std::get<BinaryExpr>(select.where->node);
        check(explain.analyze && comparison.op == BinaryOp::Greater &&
              std::get<std::int64_t>(
                  std::get<LiteralExpr>(comparison.right->node).value) == 18,
              "EXPLAIN target expression was not optimized");
    });
    return suite.finish();
}
