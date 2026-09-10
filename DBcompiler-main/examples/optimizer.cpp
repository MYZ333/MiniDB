// 真实 SQL → A 前端 → B 语义与计划 → 规则优化，逐条展示前后文本树。
#include "minisql/compiler.hpp"
#include "minisql/lexer.hpp"
#include "minisql/memory_catalog.hpp"
#include "minisql/optimizer.hpp"
#include "minisql/parser.hpp"
#include "minisql/plan_printer.hpp"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
// 演示驱动把首个诊断转换为异常，在 main 统一显示；公共 API 仍使用 Result。
template <typename T>
T unwrap(minisql::Result<T> result) {
    if (const auto* error = std::get_if<minisql::Diagnostic>(&result)) {
        throw std::runtime_error(error->message);
    }
    return std::get<T>(std::move(result));
}
} // namespace

int main() {
    using namespace minisql;
    try {
        // 仅注册演示所需的表模式，不插入记录；编译与优化不会修改 Catalog。
        MemoryCatalog catalog;
        unwrap(catalog.createTable("student", {{"id", DataType::Int},
            {"name", DataType::Varchar}, {"age", DataType::Int}}));
        const auto snapshot = catalog.snapshot();
        for (const auto* sql : {
            "SELECT name FROM student WHERE 1=1 AND age>10+8;",
            "SELECT name FROM student WHERE 1=1;",
            "UPDATE student SET age=age+(2*3) WHERE 1=1;",
            "DELETE FROM student WHERE 1=0;"}) {
            const auto statements = unwrap(parse(unwrap(lex(sql))));
            const auto before = unwrap(buildPlan(unwrap(analyze(statements.at(0), *snapshot))));
            const auto after = unwrap(optimizePlan(before));
            std::cout << sql << "\nBefore:\n" << formatPlan(before)
                      << "After:\n" << formatPlan(after) << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
