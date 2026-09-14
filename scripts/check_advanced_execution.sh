#!/usr/bin/env bash
# End-to-end regression: real SQL -> C++ JSON plan -> Java advanced operators.
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
compiler_dir="$root_dir/DBcompiler-main"
engine_dir="$root_dir/minidb-engine"
build_dir="$compiler_dir/build/integration"
mkdir -p "$build_dir" "$engine_dir/target"

cxx="${CXX:-c++}"
flags=(-std=c++17 -Wall -Wextra -Wpedantic -Werror -I"$compiler_dir/include")
sources=(
    "$compiler_dir/app/plan_json.cpp"
    "$compiler_dir/src/lexer/lexer.cpp"
    "$compiler_dir/src/parser/parser.cpp"
    "$compiler_dir/src/parser/ast_optimizer.cpp"
    "$compiler_dir/src/semantic/analyzer.cpp"
    "$compiler_dir/src/semantic/type_rules.cpp"
    "$compiler_dir/src/catalog/memory_catalog.cpp"
    "$compiler_dir/src/planner/plan_builder.cpp"
    "$compiler_dir/src/planner/plan_printer.cpp"
    "$compiler_dir/src/optimizer/constant_fold.cpp"
    "$compiler_dir/src/optimizer/optimizer.cpp"
)

"$cxx" "${flags[@]}" "${sources[@]}" -o "$build_dir/minisql_plan_json"
"$build_dir/minisql_plan_json" \
    < "$engine_dir/src/test/resources/advanced-query.sql" \
    > "$engine_dir/target/advanced-query-plan.json"
"$build_dir/minisql_plan_json" \
    < "$engine_dir/src/test/resources/aggregate-query.sql" \
    > "$engine_dir/target/aggregate-query-plan.json"
"$build_dir/minisql_plan_json" \
    < "$engine_dir/src/test/resources/aggregate-overflow.sql" \
    > "$engine_dir/target/aggregate-overflow-plan.json"
"$build_dir/minisql_plan_json" \
    < "$engine_dir/src/test/resources/remaining-features.sql" \
    > "$engine_dir/target/remaining-features-plan.json"
"$build_dir/minisql_plan_json" \
    < "$engine_dir/src/test/resources/constraint-unique.sql" \
    > "$engine_dir/target/constraint-unique-plan.json"
"$build_dir/minisql_plan_json" \
    < "$engine_dir/src/test/resources/constraint-update.sql" \
    > "$engine_dir/target/constraint-update-plan.json"

mvn -q -f "$engine_dir/pom.xml" test-compile
java -ea -cp "$engine_dir/target/classes:$engine_dir/target/test-classes" minidb.EngineTest
java -ea -cp "$engine_dir/target/classes:$engine_dir/target/test-classes" \
    minidb.AdvancedQueryEngineTest "$engine_dir/target/advanced-query-plan.json"
java -ea -cp "$engine_dir/target/classes:$engine_dir/target/test-classes" \
    minidb.AggregateQueryEngineTest "$engine_dir/target/aggregate-query-plan.json" \
    "$engine_dir/target/aggregate-overflow-plan.json"
java -ea -cp "$engine_dir/target/classes:$engine_dir/target/test-classes" \
    minidb.RemainingFeaturesEngineTest \
    "$engine_dir/target/remaining-features-plan.json" \
    "$engine_dir/target/constraint-unique-plan.json" \
    "$engine_dir/target/constraint-update-plan.json"
