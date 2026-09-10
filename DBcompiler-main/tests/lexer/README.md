# A：词法测试

lexer_tests.cpp 由团队 A 版本合入，共 6 组测试：原有关键字/字符串/位置/操作符/错误检查，
另补 EOF 处闭合块注释和 CRLF/词素所有权回归。直接调用 lex，不依赖 B。
已注册到 CMake/CTest 以及 scripts/check.sh。
