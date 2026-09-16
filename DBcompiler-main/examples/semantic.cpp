// 可运行讲解：手工 AST → 真实 analyze → BoundStatement，尚未生成或执行计划。
#include "minisql/compiler.hpp"
#include "minisql/memory_catalog.hpp"

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
} // namespace

int main() {
    try {
        MemoryCatalog catalog;
        // 1. CREATE 只返回模式描述；Catalog 此时仍为空。
        Statement create{CreateTableStmt{{"Student", {}},
            {{{"id", {}}, DataType::Int, {}}, {{"name", {}}, DataType::Varchar, {}},
             {{"age", {}}, DataType::Int, {}}}}, {}};
        const auto created = unwrap(analyze(create, *catalog.snapshot()));
        const auto& definition = std::get<BoundCreateTable>(created.node);
        std::cout << "CREATE analyzed: " << definition.table_name
                  << "; catalog version still " << catalog.snapshot()->version() << '\n';

        // 2. 测试驱动显式注册模式，模拟执行层成功建表后的元数据更新。
        unwrap(catalog.createTable(definition.table_name, definition.columns));
        auto snapshot = catalog.snapshot();

        // 3. 输入 name,age,id，分析器真实执行重排，输出 id,name,age。
        Statement insert{InsertStmt{{"student", {}},
            std::vector<Identifier>{{"name", {}}, {"age", {}}, {"id", {}}},
            {{std::string{"Alice"}, {}}, {std::int64_t{20}, {}}, {std::int64_t{1}, {}}}}, {}};
        const auto inserted = unwrap(analyze(insert, *snapshot));
        const auto& values = std::get<BoundInsert>(inserted.node).values;
        std::cout << "INSERT bound in schema order: " << std::get<std::int64_t>(values[0])
                  << ", " << std::get<std::string>(values[1]) << ", " << std::get<std::int64_t>(values[2]) << '\n';

        // 4. WHERE age > 18：IdentifierExpr 被绑定成列 ID，比较表达式被定型为 BOOL。
        auto age = std::make_shared<const Expr>(Expr{IdentifierExpr{{"age", {}}}, {}});
        auto limit = std::make_shared<const Expr>(Expr{LiteralExpr{std::int64_t{18}}, {}});
        auto predicate = std::make_shared<const Expr>(Expr{BinaryExpr{BinaryOp::Greater, age, limit, {}}, {}});
        Statement select{SelectStmt{{"student", {}}, std::vector<Identifier>{{"name", {}}}, predicate}, {}};
        const auto selected = unwrap(analyze(select, *snapshot));
        const auto& query = std::get<BoundSelect>(selected.node);
        const auto& comparison = std::get<BoundBinary>(query.where->node);
        const auto& bound_age = std::get<BoundColumnRef>(comparison.left->node);
        std::cout << "SELECT bound: output ordinal " << query.columns[0].ordinal
                  << "; WHERE column ID " << bound_age.column_id.value
                  << "; predicate BOOL=" << (query.where->type == DataType::Bool) << '\n';
        std::cout << "Metadata only: no records inserted or queried.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
