# B 第三部分代码讲解：修改语义与逻辑计划

本次完成 UPDATE/DELETE 语义、五类计划生成和计划打印。到此 B 的基础流程为：

```text
手工 AST → analyze → BoundStatement → buildPlan → LogicalPlan → formatPlan
```

A 已接入，SQL 来源的完整调用见 tests/integration/scaffold_smoke.cpp；
本篇的教学演示仍使用手工 AST。执行层仍需实现真正的算子和记录读写。

## 1. 代码组织与阅读顺序

| 顺序 | 文件/函数 | 关注点 |
|---|---|---|
| 1 | `src/semantic/analyzer.cpp` 的 bindWhere | 三类带 WHERE 语句复用同一布尔检查 |
| 2 | 同文件的 UPDATE/DELETE bindStatement | 目标列、RHS、重复赋值、旧值引用 |
| 3 | `src/planner/plan_builder.cpp` 的 buildPlan/source | 按语句类型构造算子树、传播输出模式 |
| 4 | 同文件的 validate/checkExpr | 防御外部手工绑定结构的空指针和错误身份 |
| 5 | `src/planner/plan_printer.cpp` | 递归打印实际对象，使用计划自带表结构 |
| 6 | `examples/plans.cpp` | 五类语句贯通真实 B 流程的运行示例 |

analyze/buildPlan 的签名及 AST、Bound、Plan 数据布局都没有改变。
新增公共接口仅是 plan_printer.hpp 中的 `std::string formatPlan(const LogicalPlan&)`。

## 2. UPDATE：为什么要先保留旧列引用

处理流程为：查表 → 逐个检查赋值目标 → 检查重复目标 → 绑定 RHS → 检查赋值类型
→ 检查 WHERE → 构造 BoundUpdate。空赋值列表或缺失 RHS 是 InvalidAst。

目标通过 resolveColumn 解析，使用 ordinal 判断重复，因此 `age` 和 `AGE`
是同一个目标。RHS 使用现有 bindExpr，可以包含列、常量、算术及其他受支持表达式，
最后必须与目标列类型完全相同。例如 BOOL 不能赋给 INT。

对于 `SET id = age, age = id`，第一个 RHS 绑定原 age，第二个 RHS 绑定原 id。
分析器不修改表模式，也不会把“第一个赋值的新值”替换进第二个表达式。
计划只是保存这两个原列引用。**执行层必须先读取旧记录、计算所有 RHS，再统一写回。**
当前测试证明的是引用和计划结构正确，尚未通过真实记录执行来验证交换结果。

DELETE 更简单：查表、绑定 WHERE、返回 BoundDelete。
SELECT/UPDATE/DELETE 共用 bindWhere；没有 WHERE 返回空指针，有 WHERE 必须为 BOOL。
UPDATE 和 DELETE 省略 WHERE 表示针对整张表，生成器不会自动加限制条件。

## 3. buildPlan：一条语句如何变成树

buildPlan 先做基本绑定结构检查，再用 std::visit 根据语句种类构造根节点。
私有 node 辅助函数统一创建 shared_ptr<const PlanNode>。

| 绑定语句 | 固定计划结构 |
|---|---|
| BoundCreateTable | CreateTable |
| BoundInsert | Insert |
| BoundSelect | Project → 可选 Sort → 可选 GroupBy → 可选 Filter → Join/SeqScan |
| BoundUpdate | Update → 可选 Filter → SeqScan |
| BoundDelete | Delete → 可选 Filter → SeqScan |

source 先创建 SeqScan，输出为所有表列；存在 WHERE 才包一层 Filter。
Filter 使用相同输出模式，不能丢掉过滤或修改所需的列。
Project 按已绑定的选择列创建输出模式，保留顺序和重复列。列名用 ordinal 从
计划自带模式中读取，列身份仍保留 TableId/ColumnId，不能把 ID 当作数组下标。

INSERT 直接保存语义阶段已重排的值。CREATE 保存名称和列定义，不分配 ID，
不注册表。所有计划都原样携带 Catalog 版本和只读表达式引用。

buildPlan 不自动优化：SELECT * 仍有 Project，恒真条件仍有 Filter。每条生成规则
直接对应语义结构；现可显式调用 optimizePlan 对照前后变化，见 [优化代码讲解](optimizer-walkthrough.md)。
JOIN/GROUP/ORDER 的多表布局和约束另见 [高级查询代码讲解](advanced-query-walkthrough.md)。

## 4. 行标识怎样传递

UPDATE/DELETE 调用 source 时将 row_id 设为 true：SeqScan 必须提供内部行标识，
Filter 把它和记录一起传递。修改根消费它来定位原记录，但根不输出业务列或 RowId。
影响行数由执行结果返回，不能误塞进 PlanNode.output。

SELECT 的扫描不要求 RowId，Project 只返回业务列。
具体行标识是页号/槽号还是其他格式由执行和存储层决定，B 不规定其物理表示。

```text
Update                   消费原记录与内部行标识
  Filter                 保留满足条件的记录及其行标识
    SeqScan              提供全表列和内部行标识
```

树按根到叶打印；数据由扫描产生，再向上流动。

## 5. 检查和打印为什么与语义分开

BoundStatement 是公开结构，可以被测试或外部代码手工构造。validate/checkExpr
检查缺失模式、越界 ordinal、错误表列 ID、空表达式、值类型等，失败返回
Plan / InvalidBoundStatement。它不按名字查询 Catalog，也不重做运算符类型推导；
正确用法仍然是把 analyze 的成功结果传给 buildPlan。

formatPlan 只做展示：从实际节点读取名字、表达式、输出列和 RowId 属性。
表达式带括号，字符串单引号翻倍，换行与反斜杠显示为转义文本，输出不包含指针地址。
因此同一语句重复生成计划可以得到完全相同的文本，便于答辩和测试。
此调试格式不是序列化协议，也没有添加 SQL EXPLAIN 语法。

## 6. 运行与验收

```bash
bash scripts/check.sh
./build/direct/plans_example
```

例如 `UPDATE student SET age = age + 1 WHERE id = 1` 的实际输出为：

```text
CatalogVersion: 1
Update[student#1; student.age = (student.age + 1); values=old-row] output=[] row_id=no
  Filter[(student.id = 1)] output=[id:INT, name:VARCHAR, age:INT] row_id=yes
    SeqScan[student#1] output=[id:INT, name:VARCHAR, age:INT] row_id=yes
```

当前通过 63 个行为用例：Catalog 5 个、语义 38 个、计划/打印 20 个。
另有接口联调、手工结构示例、语义示例和五类计划演示。
直接构建使用 C++17 严格警告，并逐个检查 11 个公共头文件；环境无可运行的 CMake，
因此尚未在本机验证 CMake/CTest 路径。A 合并后另增加词法/语法测试和 12 个
真实 SQL 兼容性用例，验证解析、位置及完整流程，详见 a-merge-notes.md。
