# 优化测试

optimizer_tests.cpp 的 24 组测试使用真实 SQL 编译链路检查优化行为。
覆盖算术边界、短路求值、运行时错误位置、恒真 Filter 消除、RowId、旧行赋值、
输出顺序/重复列、JOIN/GROUP/ORDER 遍历、原树不可变、重复优化稳定，以及基本无效计划诊断。
其中一组遍历 49 种确定性表达式组合。

reference_evaluator.hpp 是独立测试辅助，不调用优化器的常量计算模块。
它对内存行比较优化前后的 SELECT/UPDATE/DELETE 结果、影响行数和除零错误位置；
算术操作数限制在 ±2^30 内，避免参考实现自身溢出。INT64 极值由测试中的
明确预期向量单独检查，不用这个有限域求值器证明全范围算术。
它没有存储、事务、持久化功能，也不是后续执行层的公共接口。

运行 `bash scripts/check.sh`，或 CMake 构建后执行 `ctest --test-dir build --output-on-failure`。
讲解见 [优化代码讲解](../../docs/optimizer-walkthrough.md)。

当前普通严格警告与 UBSan 构建均为 24 组通过、0 失败。
可在支持 UBSan 的 GCC/Clang 环境从仓库根目录复现边界运算检查：

```bash
mkdir -p build/sanitized
c++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -g -O1 \
  -fsanitize=undefined -fno-sanitize-recover=undefined -Iinclude \
  tests/optimizer/optimizer_tests.cpp src/lexer/lexer.cpp src/parser/parser.cpp \
  src/catalog/memory_catalog.cpp src/semantic/type_rules.cpp src/semantic/analyzer.cpp \
  src/planner/plan_builder.cpp src/planner/plan_printer.cpp \
  src/optimizer/constant_fold.cpp src/optimizer/optimizer.cpp \
  -o build/sanitized/optimizer_tests
./build/sanitized/optimizer_tests
```
