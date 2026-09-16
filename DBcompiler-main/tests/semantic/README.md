# B：语义行为测试

semantic_tests.cpp 使用手工 AST 和 MemoryCatalog 调用真实 analyze。
当前 38 个用例覆盖五类语句、JOIN/GROUP/ORDER、多表歧义、表达式类型、源码范围、列重排、快照版本和防御性错误。
UPDATE 用例还检查旧值交换、重复赋值、目标/RHS 名称和类型；DELETE 覆盖可选 WHERE。
运行 `bash scripts/check.sh`，或用 CMake 构建后运行 CTest。
后续 A 接入时补充真实 SQL→AST→语义测试；此处仍可独立于 Parser 运行。
