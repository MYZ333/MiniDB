# A 扩展功能的 B 侧实现讲解

本次代码解决的是同一个接口问题：A 已经把语法信息放进 AST，如果 B 没有在 Bound、Plan 和
JSON 中逐层保留，执行层就会静默丢失语义。阅读时按下面的数据流走，不需要从一个大函数开始猜。

## 1. 公共结构

`common.hpp` 的 `ColumnSpec` 保存 VARCHAR 长度、PRIMARY KEY、NOT NULL、UNIQUE 和 DEFAULT。
`catalog.hpp` 的 `ColumnSchema` 是分配表列 ID 后的同一份元数据。`JoinType` 放在 common 中，
这样 AST、Bound 和 Plan 使用同一个枚举，不需要依赖枚举整数做转换。

`bound.hpp` 表示语义分析后的结果：

- `BoundExpr` 增加 `BoundAggregate`，所以聚合调用能嵌入算术、HAVING 和 ORDER BY；
- `BoundSelect` 分别保存普通投影表达式、排序表达式、HAVING、DISTINCT 和分页值；
- `BoundInsert::rows` 保存全部按表模式排好的记录，`values` 继续保存第一行以兼容旧代码；
- `BoundDropTable` 保存规范化后的表名和 IF EXISTS。

`plan.hpp` 只描述执行工作。Project 和 Aggregate 携带最终输出阶段的去重和分页设置，Sort
携带计算排序键，NestedLoopJoin 携带连接种类，DDL/DML 节点携带完整元数据和行数组。

## 2. 语义分析

`analyzer.cpp` 的 CREATE 分支检查约束组合和默认值类型，再生成 `ColumnSpec`。INSERT 先把每个
输入值映射到表列序号，然后为省略列填 DEFAULT 或 NULL；必填列没有值时停止编译。整个过程只
生成描述，不修改 Catalog。

SELECT 使用 `bindExpr(..., allow_aggregate)` 控制聚合函数只能出现在 SELECT、HAVING 和
ORDER BY。`groupingCompatible` 递归检查聚合函数之外的每个列引用是否属于 GROUP BY。
因此 `SUM(id)+1` 合法，而 `name+SUM(id)` 在没有按 name 分组时会得到 InvalidGrouping。

普通查询的表达式由 Project 求值，排序表达式由 Project 下方的 Sort 求值。聚合查询把选择项、
HAVING 和排序表达式都交给 Aggregate，使它们在同一个分组上下文中读取聚合状态。

## 3. 计划、优化和 JSON

`plan_builder.cpp` 先防御手工构造的 Bound 数据，再建立左深连接树和查询流水线。普通查询是
`Project -> Sort -> GroupBy -> Filter -> Join/Scan`；聚合查询用 Aggregate 包住过滤后的明细输入。
Project/Aggregate 的最终阶段按投影、排序、DISTINCT、OFFSET、LIMIT 的约定返回结果。

`optimizer.cpp` 递归优化新增表达式字段，重建节点时完整复制 join type、HAVING、DISTINCT 和分页
设置。`BoundAggregate` 是按组求值的叶节点，常量折叠器不能把它当普通二元表达式展开。

`app/plan_json.cpp` 是跨语言边界。新增字段全部显式写入 JSON；导出多语句脚本时，它在 CREATE
和 DROP 后同步修改编译期 MemoryCatalog，使下一条语句的 catalogVersion 与 Java 引擎一致。

## 4. Java 执行

`DatabaseEngine` 的普通 `evaluate` 按一条输入行求值，`evaluateAggregate` 按一个分组求值；
后者遇到 aggregate 叶节点时扫描该组记录。两条路径共享一元、二元运算实现，所以 NULL 三值逻辑、
溢出和 LIKE 规则一致。

外连接仍使用嵌套循环。匹配失败时，`layoutFor` 从计划恢复缺失侧列布局，`nullRowFor` 生成同样
数量的 NULL，再按“左列后接右列”合并，列引用身份不会改变。

写入先构造操作完成后的整表映像，再检查长度、非空和唯一约束。批量 INSERT 或 UPDATE 只有在
全部检查通过后才调用 RecordStore，因此后面一行失败不会留下前面几行。DROP 同时清除引擎目录和
RecordStore；为此存储接口增加 `dropTable(tableId)`。

## 5. 验证入口

`scripts/check_advanced_execution.sh` 从真实 SQL 生成 JSON，再运行 Java 断言。
`remaining-features.sql` 覆盖成功路径；`constraint-unique.sql` 和 `constraint-update.sql` 覆盖失败
原子性。C++ 的 `scripts/check.sh` 继续检查公共头文件独立包含、语义、计划、优化和旧接口兼容。
