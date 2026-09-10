# A：语法测试

parser_tests.cpp 由团队 A 版本合入，共 9 组测试。保留五类语句、空输入、星号、
优先级、INT64 边界和语法错误；补充 AND/OR 操作符范围、EOF 协议、资源深度、
算术左结合和 AST 所有权检查。语法错误测试必须先通过词法阶段。
已注册到 CMake/CTest 以及 scripts/check.sh；与 B 的完整兼容性检查见 integration。
