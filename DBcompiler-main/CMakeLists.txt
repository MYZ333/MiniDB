cmake_minimum_required(VERSION 3.16)
project(minisql_compiler LANGUAGES CXX)

# 公共契约由 A/B 共用；INTERFACE 目标传递头文件路径和语言版本。
add_library(minisql_contracts INTERFACE)
target_include_directories(minisql_contracts INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(minisql_contracts INTERFACE cxx_std_17)

# A 的实现库：由团队版本合入 Lexer/Parser，保持原公共签名。
add_library(minisql_frontend STATIC
    src/lexer/lexer.cpp
    src/parser/parser.cpp
    src/parser/ast_optimizer.cpp)
target_link_libraries(minisql_frontend PUBLIC minisql_contracts)

# B 的实现库：内存元数据、语义分析、计划生成和规则优化。
add_library(minisql_backend STATIC
    src/catalog/memory_catalog.cpp
    src/semantic/type_rules.cpp
    src/semantic/analyzer.cpp
    src/planner/plan_builder.cpp
    src/planner/plan_printer.cpp
    src/optimizer/constant_fold.cpp
    src/optimizer/optimizer.cpp)
target_link_libraries(minisql_backend PUBLIC minisql_contracts)

# 应用只负责未来的流程编排，不存放词法、语法或语义算法。
add_executable(minisql app/main.cpp)
target_link_libraries(minisql PRIVATE minisql_frontend minisql_backend)

# 跨语言边界：导出稳定 JSON 计划，供仓库中的 Java 执行引擎读取。
add_executable(minisql_plan_json app/plan_json.cpp)
target_link_libraries(minisql_plan_json PRIVATE minisql_frontend minisql_backend)

# 手工构造五类契约示例；这是结构校验，不是 SQL 编译器入口。
add_executable(contracts_example examples/contracts.cpp)
target_link_libraries(contracts_example PRIVATE minisql_contracts)

# 真实语义分析演示，与完全手工构造的 contracts_example 分开。
add_executable(semantic_example examples/semantic.cpp)
target_link_libraries(semantic_example PRIVATE minisql_backend)

# B 侧完整流程演示：五类手工 AST 通过真实语义分析与计划生成。
add_executable(plans_example examples/plans.cpp)
target_link_libraries(plans_example PRIVATE minisql_backend)

# 真实 SQL 编译及优化前后对照，演示 A/B 串联。
add_executable(optimizer_example examples/optimizer.cpp)
target_link_libraries(optimizer_example PRIVATE minisql_frontend minisql_backend)

# 所有编译目标采用一致警告设置，便于两人尽早发现接口和类型问题。
function(minisql_warnings target)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
foreach(target minisql_frontend minisql_backend minisql minisql_plan_json contracts_example semantic_example plans_example optimizer_example)
    minisql_warnings(${target})
endforeach()

include(CTest)
if(BUILD_TESTING)
    # 合入 A 的模块测试，和本地 B 测试同时回归。
    add_executable(lexer_tests tests/lexer/lexer_tests.cpp)
    add_executable(parser_tests tests/parser/parser_tests.cpp)
    add_executable(ast_optimizer_tests tests/parser/ast_optimizer_tests.cpp)
    foreach(target lexer_tests parser_tests ast_optimizer_tests)
        target_link_libraries(${target} PRIVATE minisql_frontend)
        minisql_warnings(${target})
        add_test(NAME ${target} COMMAND ${target})
    endforeach()
    add_executable(scaffold_smoke tests/integration/scaffold_smoke.cpp)
    target_link_libraries(scaffold_smoke PRIVATE minisql_frontend minisql_backend)
    minisql_warnings(scaffold_smoke)
    add_test(NAME scaffold_smoke COMMAND scaffold_smoke)
    add_test(NAME contracts_example COMMAND contracts_example)
    add_executable(catalog_tests tests/catalog/catalog_tests.cpp)
    add_executable(semantic_tests tests/semantic/semantic_tests.cpp)
    add_executable(plan_tests tests/planner/plan_tests.cpp)
    foreach(target catalog_tests semantic_tests plan_tests)
        target_link_libraries(${target} PRIVATE minisql_backend)
        minisql_warnings(${target})
        add_test(NAME ${target} COMMAND ${target})
    endforeach()
    add_test(NAME plans_example COMMAND plans_example)
    add_executable(optimizer_tests tests/optimizer/optimizer_tests.cpp)
    target_link_libraries(optimizer_tests PRIVATE minisql_frontend minisql_backend)
    minisql_warnings(optimizer_tests)
    add_test(NAME optimizer_tests COMMAND optimizer_tests)
    add_test(NAME optimizer_example COMMAND optimizer_example)
endif()
