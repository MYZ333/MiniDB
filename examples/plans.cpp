// 五类语句的 B 侧完整演示：手工 AST → analyze → buildPlan → 文本树。
// SQL 字符串仅用作讲解标签；保留手工 AST 以展示接口，真实 SQL 流程见 optimizer.cpp。
#include "minisql/compiler.hpp"
#include "minisql/memory_catalog.hpp"
#include "minisql/plan_printer.hpp"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using namespace minisql;

template <typename T>
T unwrap(Result<T> result) {
    if (const auto* error = std::get_if<Diagnostic>(&result)) throw std::runtime_error(error->message);
    return std::get<T>(std::move(result));
}

ExprPtr column(std::string name) {
    return std::make_shared<const Expr>(Expr{IdentifierExpr{{std::move(name), {}}}, {}});
}
ExprPtr integer(std::int64_t value) {
    return std::make_shared<const Expr>(Expr{LiteralExpr{value}, {}});
}
ExprPtr binary(BinaryOp op, ExprPtr left, ExprPtr right) {
    return std::make_shared<const Expr>(Expr{BinaryExpr{op, std::move(left), std::move(right), {}}, {}});
}

BoundStatement show(const char* sql, Statement ast, const CatalogSnapshot& catalog) {
    auto bound = unwrap(analyze(ast, catalog));
    const auto plan = unwrap(buildPlan(bound));
    std::cout << "[manual AST] " << sql << '\n' << formatPlan(plan) << '\n';
    return bound;
}
} // namespace

int main() {
    try {
        MemoryCatalog catalog;
        auto before = catalog.snapshot();
        auto created = show("CREATE TABLE student(id INT, name VARCHAR, age INT);",
            Statement{CreateTableStmt{{"student", {}},
                {{{"id", {}}, DataType::Int, {}}, {{"name", {}}, DataType::Varchar, {}},
                 {{"age", {}}, DataType::Int, {}}}}, {}}, *before);

        // 编译不会建表；测试驱动显式模拟执行层的模式注册，再获取新快照。
        const auto& definition = std::get<BoundCreateTable>(created.node);
        unwrap(catalog.createTable(definition.table_name, definition.columns));
        auto snapshot = catalog.snapshot();
        show("INSERT INTO student(name, age, id) VALUES ('Alice', 20, 1);",
            Statement{InsertStmt{{"student", {}},
                std::vector<Identifier>{{"name", {}}, {"age", {}}, {"id", {}}},
                {{std::string{"Alice"}, {}}, {std::int64_t{20}, {}}, {std::int64_t{1}, {}}}}, {}}, *snapshot);
        show("SELECT name FROM student WHERE age > 18;",
            Statement{SelectStmt{{"student", {}}, std::vector<Identifier>{{"name", {}}},
                binary(BinaryOp::Greater, column("age"), integer(18))}, {}}, *snapshot);
        show("UPDATE student SET age = age + 1 WHERE id = 1;",
            Statement{UpdateStmt{{"student", {}},
                {{{"age", {}}, binary(BinaryOp::Add, column("age"), integer(1)), {}}},
                binary(BinaryOp::Equal, column("id"), integer(1))}, {}}, *snapshot);
        show("DELETE FROM student WHERE id = 1;",
            Statement{DeleteStmt{{"student", {}}, binary(BinaryOp::Equal, column("id"), integer(1))}, {}}, *snapshot);
        std::cout << "Five statement plans built; no database records were executed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
