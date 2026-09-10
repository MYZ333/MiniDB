# 规则优化代码讲解

这一部分由 B 负责，把已绑定、已生成的逻辑计划改写成计算更少的等价计划。
入口是 `optimizePlan(const LogicalPlan&)`，放在 buildPlan 之后。A version2 另有
展示用 optimizeAstStatements，详见 A version2 合并说明；正式编译仍使用本章的
绑定后计划优化。新增产品模块与测试辅助模块均有中文注释。

## 1. 按什么顺序读代码

| 文件 | 负责什么 | 阅读重点 |
|---|---|---|
| include/minisql/optimizer.hpp | 公共入口和前置条件 | 输入是通过语义检查的计划，输出仍是 Result |
| src/optimizer/constant_fold.hpp/.cpp | 安全计算纯常量 | 不访问树、不修改元数据；失败折叠返回 nullopt |
| src/optimizer/optimizer.cpp | 表达式与算子树改写 | optimizeExpr → optimizeNode → optimizePlan |
| examples/optimizer.cpp | 真实 SQL 的完整演示 | 串联 A/B 后保存和打印前后计划 |
| tests/optimizer/optimizer_tests.cpp | 边界、等价性与接口回归 | 真 SQL 输入；明确算术预期与独立求值对照 |
| tests/optimizer/reference_evaluator.hpp | 测试专用求值器 | 小整数计算、短路、旧行赋值和错误位置 |

先运行 `bash scripts/check.sh`，再运行 `./build/direct/optimizer_example`。
使用 CMake 构建时，对应程序为 `./build/optimizer_example`。

## 2. 从一个例子理解树的变化

```sql
SELECT name FROM student WHERE 1=1 AND age>10+8;
```

原始 Filter 表达式是 `(1 = 1) AND (student.age > (10 + 8))`。
optimizeExpr 先优化子表达式：`1=1` 变为内部 TRUE，`10+8` 变为 18；
随后 `TRUE AND x` 替换为 x，所以得到 `student.age > 18`。

```text
之前                              之后
Project[name]                     Project[name]
  Filter[1=1 AND age>10+8]           Filter[age>18]
    SeqScan[student]                  SeqScan[student]
```

这是简写示意，实际 formatPlan 还会显示列类型、CatalogVersion 和 row_id。
如果条件只有 `1=1`，optimizeNode 会删除整个 Filter，Project 直接连接 SeqScan。
如果条件是 `1=0`，则保留 Filter[FALSE]；第一版没有增加 EmptyResult 算子。

## 3. 常量折叠为什么需要单独一个模块

constant_fold 只接收操作符和 ScalarValue，因此不依赖 Parser、Catalog 或计划结构。
它也支持同类型 FLOAT 运算/比较及 BOOL 判等；非有限浮点结果和除零保持原表达式。
返回 `optional<ScalarValue>`：有值表示可以安全替换，没有值表示继续保留原运算。
它不返回“编译失败”，因为 `UPDATE ... SET age=1/0 WHERE 1=0` 不会计算 RHS。
此时提前报除零会改变程序行为。

C++ 有符号整数溢出是未定义行为，必须先检查，再做算术。
例如加法在 b 为正时检查 `a > INT64_MAX - b`，在 b 为负时检查
`a < INT64_MIN - b`；减法同样依据 b 的符号判断安全范围。
乘法按两个数的符号用除法边界检查，避免先乘出溢出的中间值。
除法单独拒绝除数零及 `INT64_MIN / -1`；一元取负拒绝 `-INT64_MIN`。
拒绝折叠后，原操作符和源码位置继续留在树中，未来执行层负责运行时检查。

## 4. 布尔规则必须考虑求值顺序

| 原表达式 | 改写 | 理由 |
|---|---|---|
| FALSE AND x | FALSE | 原本就不访问 x |
| TRUE OR x | TRUE | 原本就不访问 x |
| TRUE AND x / FALSE OR x | x | 两者都必须计算 x |
| x AND TRUE / x OR FALSE | x | 仍然计算 x，右侧常量无副作用 |
| x AND FALSE / x OR TRUE | 保留 | x 可能除零或溢出，不能跳过 |

optimizeExpr 先访问左侧。如果它已经确定短路结果，就返回常量而不访问右侧。
其他情况再优化右侧并应用规则。不会交换 AND/OR 两侧，也不做 `x*0 → 0`，
因此 `(1/0)*0` 中的错误仍然可达。语义分析在优化前已检查完整表达式，
短路不会绕过“未知列”或类型错误。

## 5. 如何保持计划契约

所有树节点通过 `shared_ptr<const T>` 发布。改写只创建新节点；如果子树没变，
就返回原指针。因此可以同时保存 before 和 after，用 formatPlan 对照。
第二次优化已稳定的结果直接复用原根节点，测试会检查这一点。

Filter 删除之前要检查自己的输出模式和 RowId 属性与输入相同。
Update/Delete 的输入必须携带 RowId，移除恒真 Filter 后仍连接带 RowId 的 SeqScan。
两个修改根不会删除，输出仍为空；影响行数属于执行结果。
UPDATE 逐个优化赋值 RHS，不改变目标列、赋值顺序或列引用：
`SET id=age,age=id+2*3` 只会把 `2*3` 折叠成 6，两个 RHS 仍读取旧行。

CatalogVersion、表模式、列 ID/ordinal、输出顺序和重复投影列均保持。
新字面量保留被替代表达式的范围；留下的运算保留 operator_span，运行时报错位置不变。
空节点、深度超限等基本结构异常返回 Plan / InvalidPlan，但入口不是完整计划验证器；
有效的 analyze/buildPlan 输出是必要前置条件。

## 6. 如何验证等价性

测试从 SQL 经真实 lex/parse/analyze/buildPlan 得到输入，再调用 optimizePlan。
除了比较树结构，还用独立参考求值器在多行和空表上运行前后计划，比较：

- SELECT 的值、顺序和重复列。
- UPDATE/DELETE 后的记录、修改目标和影响行数，以及 UPDATE 读取旧行的规则。
- 可达除零错误的错误码和源码位置，以及短路后不应发生的错误。

参考求值器不调用 constant_fold，避免两边共享同一个错误算法。
它把算术操作数限制在 ±2^30 内，不模拟全范围溢出；INT64 边界通过独立明确的
预期向量验证。49 种确定性表达式组合额外覆盖列值参与除法及除数为零的情况。
这些是编译器的优化正确性测试，不代表存储、事务或真实数据库执行引擎已经实现。

后续可以在保持这些约定的前提下扩展空结果算子、列裁剪，再结合新增语法考虑连接优化。
