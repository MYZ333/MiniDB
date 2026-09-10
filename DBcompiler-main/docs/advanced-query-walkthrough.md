# B 高级查询代码讲解：JOIN、GROUP BY 与 ORDER BY

这一部分把 A 已生成的 `SelectStmt` 扩展结构接入 B。代码仍分为三层：

```text
SelectStmt
  → analyzer.cpp：解析表列并检查语义
  → BoundSelect：只保存稳定 ID 和已定型表达式
  → plan_builder.cpp：构造可由执行层消费的算子树
```

## 1. 公共结构

`bound.hpp` 的 `BoundJoin` 保存右表模式和 BOOL 类型的 ON 表达式，`BoundOrderBy`
保存排序列引用与 ASC/DESC。`BoundSelect` 按 SQL 顺序保存 joins、group_by 和 order_by。
所有列都使用 `BoundColumnRef`，其中 table_id/column_id 表示身份，ordinal 只用于读取
所属表模式中的列。计划生成阶段不再按字符串查 Catalog。

`plan.hpp` 增加三个节点：

- `NestedLoopJoinPlan` 有 left、right 两个输入和 predicate；
- `GroupByPlan` 有 keys 和一个 input；
- `SortPlan` 有有序 items 和一个 input。

这些节点只描述逻辑工作，不包含循环、哈希表或比较器等执行代码。

## 2. 多表名称如何绑定

`analyzer.cpp` 用 `BindingScope` 保存当前可见表。开始时只有 FROM 表；每处理一个 JOIN，
先检查右表没有重复，再把它加入作用域，然后绑定 ON。因此第二个 JOIN 的 ON 可以引用
FROM 表、第一个 JOIN 表和刚加入的第二个右表。

`resolveColumn` 处理两种名称：

- `score.value` 先按 `score` 限定表，再查 `value`；
- `value` 遍历全部可见表，恰好命中一次才成功，命中多次返回 AmbiguousColumn。

SELECT 星号按作用域顺序展开，所以输出顺序为 FROM 表的全部列，随后是每个 JOIN 表的
全部列。显式选择列仍保留用户写下的顺序和重复项。JOIN ON 和 WHERE 都通过统一的
布尔绑定辅助函数检查，但使用不同错误码，便于调用方解释错误来源。

当前语法没有表别名。`student JOIN student` 无法区分两个实例，因此语义阶段直接返回
DuplicateTable；加入别名后需要把作用域键从表名改为“表实例名”。

## 3. 分组和排序规则

语法尚未支持 COUNT、SUM 等聚合表达式，因此 `GROUP BY` 的当前含义是按键去重。
分析器要求每个投影列都出现在分组键中。这样 GroupBy 输出只需保留键列，后续 Project
一定还能找到自己的输入。重复分组键没有额外意义，会返回 InvalidGrouping。

普通查询可以按未投影列排序，例如：

```sql
SELECT name FROM student ORDER BY age DESC;
```

Sort 在 Project 之前执行，此时 age 仍在扫描输出中。分组后只有键列存在，因此分组查询的
ORDER BY 也必须使用分组键。排序项目保持 SQL 顺序，执行层先比较第一项，相等时继续比较
下一项，并分别服从 ASC/DESC。
分组时两个 NULL 键属于同一组；排序时 ASC 把 NULL 放在最后，DESC 把 NULL 放在最前。

## 4. 计划树如何构造

`plan_builder.cpp` 的 `selectSource` 自底向上建立输入：

```text
Project                  最后裁剪并排列 SELECT 输出
  Sort                   可选；此时隐藏排序列仍存在
    GroupBy              可选；按键去重并只输出键
      Filter             可选；WHERE 在分组前筛行
        NestedLoopJoin   每个 JOIN 增加一层左深节点
          左输入
          右表 SeqScan
```

每个 NestedLoopJoin 的输出模式是 `left.output + right.output`。表达式求值不能仅按显示列名
定位，因为两表都可能有 id；执行层应使用 BoundColumnRef 的表 ID、列 ID 和各输入布局映射。
当前计划选择嵌套循环只为建立清晰的首版接口，未来可由优化器替换为哈希连接。

`validate` 防御手工构造的 BoundSelect：检查右表、BOOL ON、可见列身份、重复表、重复分组键，
以及投影/排序的分组约束。真实调用仍应先 analyze，再 buildPlan。

## 5. 打印和优化

`plan_printer.cpp` 的 `collectTables` 同时递归 JOIN 左右分支，再用稳定 ID 把表达式恢复成
`student.id` 形式。`printNode` 对二叉 JOIN 分别打印 left/right，对 GroupBy、Sort 等一元
节点打印 input。因此打印结果可直接检查真实树形，而不依赖节点地址。

`optimizer.cpp` 会递归优化 JOIN 两侧并折叠 ON 中的常量表达式，也会穿过 GroupBy 和 Sort。
它不改变 JOIN 顺序，不删除恒真 JOIN，也不选择物理算法。恒真 WHERE Filter 可以照常删除，
其上方的 GroupBy、Sort、Project 会用新的只读子节点重建；再次优化会复用整棵树。

## 6. 测试入口

- `tests/semantic/semantic_tests.cpp`：限定名、歧义列、重复表、BOOL ON、星号展开、分组约束和隐藏排序列；
- `tests/planner/plan_tests.cpp`：计划节点顺序、JOIN 输出拼接、防御性校验和文本打印；
- `tests/integration/scaffold_smoke.cpp`：真实 SQL 从 Lexer/Parser 进入 B；
- `tests/optimizer/optimizer_tests.cpp`：高级节点递归改写、幂等性和无效节点诊断。

运行 `bash scripts/check.sh` 可在没有 CMake 的环境完成严格警告构建和全部回归。
