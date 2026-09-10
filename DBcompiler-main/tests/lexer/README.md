# A：词法测试

lexer_tests.cpp 由团队 A 版本合入，覆盖关键字/字符串/位置/操作符/错误检查，
以及 FLOAT、BOOL、NULL、JOIN/GROUP/ORDER、限定名点号、EOF 闭合注释和 CRLF。
它直接调用 lex，不依赖 B。
已注册到 CMake/CTest 以及 scripts/check.sh。
