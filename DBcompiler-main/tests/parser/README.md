# A：语法测试

parser_tests.cpp 由团队 A 版本合入。覆盖五类语句、扩展标量、限定名、JOIN、
GROUP BY、ORDER BY、优先级、INT64 边界和语法错误；保留 AND/OR 操作符范围、
EOF 协议、资源深度、算术左结合和 AST 所有权检查。ast_optimizer_tests.cpp
另用 4 组测试检查展示优化、短路边界、JOIN ON 和混合类型。
已注册到 CMake/CTest 以及 scripts/check.sh；与 B 的完整兼容性检查见 integration。
