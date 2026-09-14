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
| src/optimizer/optimizer.cpp | 流水线入口和基础树改写 | optimizeExpr → optimizeNode → optimizePlan |
| src/optimizer/plan_rules.hpp | 私有规则接口 | 规则不进入公共 API，由入口固定排序 |
| src/optimizer/predicate_pushdown.cpp | 谓词下推 | 拆 AND、识别关系实例、保护外连接与错误顺序 |
| src/optimizer/empty_result.cpp | 空结果传播 | 消除安全的恒假输入，保留布局并守住语句根边界 |
| src/optimizer/column_pruning.cpp | 列裁剪 | 从根反向收集列依赖，重建 SeqScan 输出模式 |
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
    SeqScan[student]                  SeqScan[student; columns=name,age]
```

这是简写示意，实际 formatPlan 还会显示列类型、CatalogVersion 和 row_id。
如果条件只有 `1=1`，optimizeNode 会删除整个 Filter，Project 直接连接 SeqScan。
如果条件是 `1=0` 且输入可安全跳过，Filter 和扫描会被一个 EmptyResult 叶节点替换；
Project 仍保留最终列名，因此查询返回结构正确的零行结果。

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

## 6. 谓词下推如何组织

`pushNode` 先递归优化子节点。当它遇到 `Filter → NestedLoopJoin` 时，`flattenAnd`
按原有从左到右顺序拆出合取项，`collectRefs` 收集每项使用的表和关系实例 ID，
`relationOccurs` 再判断引用只属于左输入还是右输入。条件被包装成新的 Filter 后继续调用
`pushNode`，因此多表左深连接中的条件能够逐层靠近自己的 SeqScan。

下推矩阵由连接的 NULL 扩展语义决定：

| JOIN 类型 | 左侧 WHERE 条件 | 右侧 WHERE 条件 |
|---|---|---|
| INNER | 下推 | 下推 |
| LEFT | 下推 | 保留在 JOIN 上方 |
| RIGHT | 保留在 JOIN 上方 | 下推 |
| FULL | 保留 | 保留 |

例如 LEFT JOIN 的右侧条件若提前过滤，原本“匹配后被 WHERE 删除”的左行可能变成一条
NULL 扩展行，结果会变化。常量条件没有关系归属，跨表条件同时引用两侧，它们也留在原位。

`safeToMove` 把含加减乘除、一元取负和聚合的表达式视为可能报错。JOIN ON 可能报错时不做
下推；WHERE 中出现危险合取项后，不再把后续条件提前。这样不会让提前筛选跳过原本可达的
除零或溢出。例如 `id/0=1 AND score>60` 的右侧条件必须留在错误之后。

## 7. 空结果为什么不等于简单删除 Filter

`empty_result.cpp` 在谓词下推之后遍历计划。`Filter[FALSE]` 可以确定没有输出行，但执行器原本
会先执行 Filter 的输入；如果输入 JOIN ON 中存在 `id/0`，直接替换会把应当发生的除零错误隐藏。
因此 `safeToSkip` 只允许跳过扫描、EmptyResult，以及条件和排序表达式均不会报错的 Filter、
Join、GroupBy、Sort。算术、一元取负、聚合和修改节点都作为保守边界。

连接按以下规则传播空结果：

| JOIN 类型 | 可以判空的条件 |
|---|---|
| INNER | 任一输入为空，或 ON 恒假 |
| LEFT | 左输入为空 |
| RIGHT | 右输入为空 |
| FULL | 两个输入都为空 |

若判空会跳过另一个可能报错的输入，则保留原 JOIN。LEFT 的右侧为空、RIGHT 的左侧为空、FULL
只有一侧为空时也不能判空，因为保留侧仍会产生 NULL 扩展行。

EmptyResult 是叶节点，却不能只存一个“空”标志。`columns` 保存有序 BoundColumnRef，
`relations` 保存表模式和关系实例；RIGHT/FULL JOIN 可以据此为已经消失的输入构造类型和身份正确的
NULL 行。Sort 和纯 GroupBy 可以继续折叠，Project、Aggregate、Update/Delete、Explain 保留：
Project 提供结果列名，全局 Aggregate 必须产生 `COUNT(*)=0`，修改根必须返回影响行数 0。
对于 `LIMIT 0`，Project 根仍保留，但无风险输入可直接换成 EmptyResult；若投影含除法等
可能报错的计算，则继续读取输入并维持当前执行器“先投影、后分页”的顺序。

## 8. 列裁剪如何组织

`pruneNode(plan, required)` 是从根向叶的依赖传递。`required` 保存执行当前节点之后仍需读取的
`BoundColumnRef`，以 `table_id + relation_id + column_id + ordinal + type` 去重：

- Project 从最终列或计算表达式开始收集。
- Sort、Filter 分别加入排序键和条件引用。
- Join 加入 ON 引用，再用关系实例把集合分到左右子树。
- GroupBy 保留全部分组键；Aggregate 加入分组、输出聚合参数、HAVING 和聚合排序依赖。
- SeqScan 按表模式顺序与 required 求交，重建 `columns` 和 `output`。

`columns=nullopt` 表示全列，便于兼容旧计划；`columns=[]` 表示零业务列。例如
`SELECT COUNT(*) FROM student` 仍需为每条存储记录产生一个逻辑输入行，但不需要读取任何
业务值。RowId 不占 `output`，所以无条件 DELETE 同样可以零列扫描。UPDATE 当前要复制未修改列
并对最终整行检查约束，因此明确请求所有列，避免把优化变成不完整写回。

Java 的 `scanLayout` 是这项契约的执行边界。它对每个列引用与运行时 Catalog 做一致性检查，
再用原表 ordinal 从完整 StoredRow 取出所需值。表达式求值通过 `ColumnSlot` 在裁剪后的布局中
按身份找列，不把原表 ordinal 误当成紧凑数组下标。`layoutFor` 复用同一函数，保证外连接构造
NULL 行时使用完全相同的布局。EXPLAIN 会显示 `columns=<all>`、精确列名或 `<none>`。

## 9. 如何验证等价性

测试从 SQL 经真实 lex/parse/analyze/buildPlan 得到输入，再调用 optimizePlan。
除了比较树结构，还用独立参考求值器在多行和空表上运行前后计划，比较：

- SELECT 的值、顺序和重复列。
- UPDATE/DELETE 后的记录、修改目标和影响行数，以及 UPDATE 读取旧行的规则。
- 可达除零错误的错误码和源码位置，以及短路后不应发生的错误。

参考求值器不调用 constant_fold，避免两边共享同一个错误算法。
它把算术操作数限制在 ±2^30 内，不模拟全范围溢出；INT64 边界通过独立明确的
预期向量验证。49 种确定性表达式组合额外覆盖列值参与除法及除数为零的情况。
新增结构测试还覆盖 INNER 双侧下推、LEFT/FULL 边界、危险表达式顺序、查询/DML 裁剪、
恒假 Filter/JOIN 与安全 LIMIT 0 传播、危险表达式保留、EmptyResult 身份校验和重复优化固定点。
根目录的 `scripts/check_advanced_execution.sh` 会从真实 SQL 导出优化 JSON，
由 Java 验证下推后的行数、扫描列名、零列 COUNT(*) 和最终查询结果。参考求值器仍主要负责
单表常量优化；跨表和紧凑运行时布局由这组端到端测试负责。
