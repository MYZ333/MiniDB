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
"$compiler" "${flags[@]}" -c src/semantic/analyzer.cpp -o build/direct/analyzer.o
"$compiler" "${flags[@]}" -c src/planner/plan_builder.cpp -o build/direct/plan_builder.o
"$archiver" rcs build/direct/libminisql_frontend.a build/direct/lexer.o build/direct/parser.o
"$archiver" rcs build/direct/libminisql_backend.a build/direct/analyzer.o build/direct/plan_builder.o
libraries=(build/direct/libminisql_frontend.a build/direct/libminisql_backend.a)

"$compiler" "${flags[@]}" app/main.cpp "${libraries[@]}" -o build/direct/minisql
"$compiler" "${flags[@]}" examples/contracts.cpp -o build/direct/contracts_example
"$compiler" "${flags[@]}" tests/integration/scaffold_smoke.cpp "${libraries[@]}" -o build/direct/scaffold_smoke
./build/direct/minisql
./build/direct/contracts_example
./build/direct/scaffold_smoke
