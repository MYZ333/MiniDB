# B：计划与打印行为测试

plan_tests.cpp 的 16 个用例大部分通过真实 analyze 后调用 buildPlan，检查五类树结构、
输出模式、版本和修改计划行标识。少数手工绑定结果用于验证非连续 ID 和错误结构诊断。
打印测试对 SELECT/UPDATE 等计划使用固定预期文本，检查重复构建稳定性和字符串转义。
运行 `bash scripts/check.sh`，或用 CMake 构建后运行 CTest。
本模块不执行 CRUD，不验证存储层的记录读写结果。
