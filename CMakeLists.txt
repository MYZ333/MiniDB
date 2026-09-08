cmake_minimum_required(VERSION 3.16)
project(minisql_compiler LANGUAGES CXX)

# 公共契约由 A/B 共用；INTERFACE 目标传递头文件路径和语言版本。
add_library(minisql_contracts INTERFACE)
target_include_directories(minisql_contracts INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(minisql_contracts INTERFACE cxx_std_17)

# A 的实现库：后续在对应源文件中替换占位入口，无需修改公共签名。
add_library(minisql_frontend STATIC src/lexer/lexer.cpp src/parser/parser.cpp)
target_link_libraries(minisql_frontend PUBLIC minisql_contracts)

# B 的实现库：后续 Catalog 源文件也加入此目标。
add_library(minisql_backend STATIC src/semantic/analyzer.cpp src/planner/plan_builder.cpp)
target_link_libraries(minisql_backend PUBLIC minisql_contracts)

# 应用只负责未来的流程编排，不存放词法、语法或语义算法。
add_executable(minisql app/main.cpp)
target_link_libraries(minisql PRIVATE minisql_frontend minisql_backend)

# 手工构造五类契约示例；这是结构校验，不是 SQL 编译器入口。
add_executable(contracts_example examples/contracts.cpp)
target_link_libraries(contracts_example PRIVATE minisql_contracts)

# 所有编译目标采用一致警告设置，便于两人尽早发现接口和类型问题。
function(minisql_warnings target)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
foreach(target minisql_frontend minisql_backend minisql contracts_example)
    minisql_warnings(${target})
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_executable(scaffold_smoke tests/integration/scaffold_smoke.cpp)
    target_link_libraries(scaffold_smoke PRIVATE minisql_frontend minisql_backend)
    minisql_warnings(scaffold_smoke)
    add_test(NAME scaffold_smoke COMMAND scaffold_smoke)
    add_test(NAME contracts_example COMMAND contracts_example)
endif()
