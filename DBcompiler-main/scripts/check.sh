#!/usr/bin/env bash
# 无 CMake 环境的本地验证入口：构建静态库、应用、示例和骨架联调检查。
# 新增源文件后，应同步维护此清单和 CMakeLists.txt。
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
mkdir -p build/direct
compiler="${CXX:-c++}"
archiver="${AR:-ar}"
flags=(-std=c++17 -Wall -Wextra -Wpedantic -Werror -Iinclude)

# 每个公共头文件独立包含，避免误依赖其他头文件的包含顺序。
for header in include/minisql/*.hpp; do
    printf '#include "%s"\nint main() {}\n' "${header#include/}" |
        "$compiler" "${flags[@]}" -x c++ -fsyntax-only -
done

"$compiler" "${flags[@]}" -c src/lexer/lexer.cpp -o build/direct/lexer.o
"$compiler" "${flags[@]}" -c src/parser/parser.cpp -o build/direct/parser.o
"$compiler" "${flags[@]}" -c src/parser/ast_optimizer.cpp -o build/direct/ast_optimizer.o
"$compiler" "${flags[@]}" -c src/semantic/analyzer.cpp -o build/direct/analyzer.o
"$compiler" "${flags[@]}" -c src/semantic/type_rules.cpp -o build/direct/type_rules.o
"$compiler" "${flags[@]}" -c src/catalog/memory_catalog.cpp -o build/direct/memory_catalog.o
"$compiler" "${flags[@]}" -c src/planner/plan_builder.cpp -o build/direct/plan_builder.o
"$compiler" "${flags[@]}" -c src/planner/plan_printer.cpp -o build/direct/plan_printer.o
"$compiler" "${flags[@]}" -c src/optimizer/constant_fold.cpp -o build/direct/constant_fold.o
"$compiler" "${flags[@]}" -c src/optimizer/predicate_pushdown.cpp -o build/direct/predicate_pushdown.o
"$compiler" "${flags[@]}" -c src/optimizer/column_pruning.cpp -o build/direct/column_pruning.o
"$compiler" "${flags[@]}" -c src/optimizer/optimizer.cpp -o build/direct/optimizer.o
"$archiver" rcs build/direct/libminisql_frontend.a build/direct/lexer.o build/direct/parser.o build/direct/ast_optimizer.o
"$archiver" rcs build/direct/libminisql_backend.a build/direct/analyzer.o build/direct/type_rules.o build/direct/memory_catalog.o build/direct/plan_builder.o build/direct/plan_printer.o build/direct/constant_fold.o build/direct/predicate_pushdown.o build/direct/column_pruning.o build/direct/optimizer.o
libraries=(build/direct/libminisql_frontend.a build/direct/libminisql_backend.a)

"$compiler" "${flags[@]}" app/main.cpp "${libraries[@]}" -o build/direct/minisql
"$compiler" "${flags[@]}" app/plan_json.cpp "${libraries[@]}" -o build/direct/minisql_plan_json
"$compiler" "${flags[@]}" examples/contracts.cpp -o build/direct/contracts_example
"$compiler" "${flags[@]}" tests/integration/scaffold_smoke.cpp "${libraries[@]}" -o build/direct/scaffold_smoke
"$compiler" "${flags[@]}" examples/semantic.cpp "${libraries[@]}" -o build/direct/semantic_example
"$compiler" "${flags[@]}" tests/catalog/catalog_tests.cpp "${libraries[@]}" -o build/direct/catalog_tests
"$compiler" "${flags[@]}" tests/semantic/semantic_tests.cpp "${libraries[@]}" -o build/direct/semantic_tests
"$compiler" "${flags[@]}" tests/planner/plan_tests.cpp "${libraries[@]}" -o build/direct/plan_tests
"$compiler" "${flags[@]}" examples/plans.cpp "${libraries[@]}" -o build/direct/plans_example
"$compiler" "${flags[@]}" tests/lexer/lexer_tests.cpp "${libraries[@]}" -o build/direct/lexer_tests
"$compiler" "${flags[@]}" tests/parser/parser_tests.cpp "${libraries[@]}" -o build/direct/parser_tests
"$compiler" "${flags[@]}" tests/parser/ast_optimizer_tests.cpp "${libraries[@]}" -o build/direct/ast_optimizer_tests
"$compiler" "${flags[@]}" tests/optimizer/optimizer_tests.cpp "${libraries[@]}" -o build/direct/optimizer_tests
"$compiler" "${flags[@]}" examples/optimizer.cpp "${libraries[@]}" -o build/direct/optimizer_example
./build/direct/minisql < /dev/null
printf "CREATE TABLE t(id INT); INSERT INTO t VALUES (1); SELECT * FROM t;" |
    ./build/direct/minisql_plan_json > build/direct/plan.json
grep -q '"protocolVersion":1' build/direct/plan.json
grep -q '"type":"Project"' build/direct/plan.json
printf "CREATE TABLE s(id INT,name VARCHAR); CREATE TABLE x(sid INT); SELECT s.name FROM s JOIN x ON s.id=x.sid GROUP BY s.name ORDER BY s.name DESC;" |
    ./build/direct/minisql_plan_json > build/direct/advanced-plan.json
grep -q '"type":"NestedLoopJoin"' build/direct/advanced-plan.json
grep -q '"type":"GroupBy"' build/direct/advanced-plan.json
grep -q '"type":"Sort"' build/direct/advanced-plan.json
printf "CREATE TABLE employee(id INT,manager_id INT); SELECT e.id AS employee_id FROM employee AS e JOIN employee m ON e.manager_id=m.id ORDER BY employee_id;" |
    ./build/direct/minisql_plan_json > build/direct/alias-plan.json
grep -q '"relationName":"e"' build/direct/alias-plan.json
grep -q '"relationName":"m"' build/direct/alias-plan.json
grep -q '"name":"employee_id"' build/direct/alias-plan.json
printf "CREATE TABLE p(id INT,name VARCHAR,unused BOOL); SELECT name FROM p WHERE id=1; SELECT COUNT(*) FROM p;" |
    ./build/direct/minisql_plan_json > build/direct/pruned-plan.json
grep -q '"columns":\[' build/direct/pruned-plan.json
grep -q '"columns":\[\]' build/direct/pruned-plan.json
./build/direct/lexer_tests
./build/direct/parser_tests
./build/direct/ast_optimizer_tests
./build/direct/contracts_example
./build/direct/scaffold_smoke
./build/direct/semantic_example
./build/direct/catalog_tests
./build/direct/semantic_tests
./build/direct/plan_tests
./build/direct/plans_example
./build/direct/optimizer_tests
./build/direct/optimizer_example
