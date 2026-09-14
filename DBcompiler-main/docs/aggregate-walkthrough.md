# 第三部分：聚合函数实现讲解

本次完成 COUNT、SUM、AVG、MIN、MAX，贯通 SQL、C++ 语义分析和计划、JSON、Java 执行。
先读下面的例子，再按模块阅读代码。

```sql
CREATE TABLE sales(dept VARCHAR, amount INT);
INSERT INTO sales VALUES ('a', 10);
INSERT INTO sales VALUES ('a', 20);
INSERT INTO sales VALUES ('b', NULL);
SELECT dept, COUNT(*) AS rows, SUM(amount) AS total
FROM sales GROUP BY dept ORDER BY total DESC;
```

结果按 DESC 的 NULL 在最前规则为：

| dept | rows | total |
|---|---:|---:|
| b | 1 | NULL |
| a | 2 | 30 |

## 1. 代码如何组织

| 阅读位置 | 输入和输出 | 主要职责与分工 |
|---|---|---|
| include/minisql/ast.hpp、src/parser/parser.cpp | SQL Token → SelectItem/AggregateCall | A 接口的必要适配：识别括号、参数、星号、别名和源码范围 |
| include/minisql/bound.hpp、src/semantic/analyzer.cpp | AST + Catalog → BoundSelect | B：检查函数、绑定参数列、推导类型、验证 GROUP BY 和 ORDER BY |
| include/minisql/plan.hpp、src/planner/plan_builder.cpp | BoundSelect → AggregatePlan | B：在 JOIN/Filter 的明细输出上建立聚合边界，声明最终输出 |
| src/optimizer/optimizer.cpp、src/planner/plan_printer.cpp | 计划 → 优化后计划/文本 | B：递归优化输入，保留聚合边界，展示函数、分组和排序 |
| app/plan_json.cpp | C++ 计划 → JSON | B 与执行层的桥接：输出已绑定参数及稳定字段 |
| ../minidb-engine/src/main/java/minidb/DatabaseEngine.java | JSON → QueryResult | 执行适配：形成分组、计算聚合值、投影、排序 |

旧 vector<Identifier> AST 保持可用。只有含聚合调用的 SQL 使用新的 vector<SelectItem>；
外部手工构造的纯列 SelectItem 也会转回普通 SELECT 路径。
COUNT 等函数名仍是普通 Identifier Token，因此无需增加五个关键字。

## 2. B 如何绑定

示例中的 SUM(amount) 被绑定为“Sum + 确定的 amount 列引用 + INT 结果类型”。
列引用保存 table_id、column_id、ordinal、type 和 relation_id。
relation_id 区分自连接中同一物理表的两次扫描，SUM(l.amount) 和 SUM(r.amount) 不会混淆。

COUNT(*) 的 argument 为空，COUNT(amount) 则保存列引用。这一区别决定是否跳过 NULL。
BoundSelect.aggregate_items 按最终 SELECT 顺序保存普通分组列或聚合描述；
output_names 保存 dept、rows、total。ORDER BY total 绑定为输出序号 2，
执行阶段直接读取第 3 个输出值，无需再次查找字符串名字。

普通列必须出现在 GROUP BY 中。没有 GROUP BY 的 SUM(amount) 合法，
但 dept,SUM(amount) 非法，因为全表合为一组后无法确定唯一的 dept。
SUM/AVG 只接受 INT/FLOAT；COUNT 返回 INT，AVG 返回 FLOAT，其余保留参数类型。

## 3. 为什么新增 Aggregate 节点

```text
Aggregate[group=dept; items=dept, COUNT(*), SUM(amount); order=output#2 DESC]
  SeqScan[sales]
```

有 JOIN 和 WHERE 时，它们位于 Aggregate 下方，先产生过滤后的明细行。
旧 GroupBy 会去重并只保留分组键，无法再求 SUM；Aggregate 因此直接读取明细。
它统一执行分组、聚合、最终投影和聚合后排序，保留隐藏分组键供 ORDER BY 使用。
当前是面向课程项目的组合算子，后续可以拆成独立的聚合、表达式投影和排序算子。

## 4. Java 如何执行

aggregate 先使用 LinkedHashMap 按分组键列表归类，每个键映射到该组的明细行列表。
列表相等比较让两个 NULL 键归入同组。随后按 SELECT 项调用 aggregateValue，
最后 compareAggregateRows 按各排序项依次比较结果。

- COUNT(*) 对每行加一；COUNT(column) 跳过 NULL 后加一。
- SUM(INT) 用 Math.addExact 检查 64 位溢出；SUM(FLOAT) 检查结果是否有限。
- AVG 用 double 累加非 NULL 值并除以数量；可能有浮点舍入误差，累加非有限值报错。
- MIN/MAX 跳过 NULL，复用已有同类型排序比较。

全表聚合预先创建一个空组，所以空表上的 COUNT(*) 得到 0，其余聚合得到 NULL。
带 GROUP BY 时不预建组，空输入返回零行。全 NULL 参数与空输入也要区分：
一行 NULL 的 COUNT(*)=1，COUNT(column)=0。

实现会物化输入并保存各组的明细行引用，内存用量随输入增长；
当前没有流式聚合、磁盘溢写或聚合 DISTINCT。

## 5. 测试与边界

在编译器目录执行 `bash scripts/check.sh` 验证 A/B 的完整测试、独立头文件和命令行构建。
在 MiniDB 根目录执行 `bash scripts/check_advanced_execution.sh`，
用真实 C++ 导出器生成 JSON，交给 Java 验证普通查询兼容性和聚合结果。
aggregate-query.sql 覆盖五类函数、NULL、空表、全 NULL、排序和自连接；
aggregate-overflow.sql 验证整数 SUM 溢出的 SQL 行列。

grammar.md 0.7 定义语言范围，interfaces.md 定义 C++ 接口，
json-plan-protocol.md 定义跨语言字段。暂不支持 HAVING、聚合 DISTINCT、
COUNT(1)、函数参数算术、嵌套聚合和直接 ORDER BY SUM(amount)；
排序聚合结果请使用 SELECT 别名。普通表达式尚未实现 NULL 三值逻辑。

答辩时可这样说明：A 负责识别聚合调用的语法，B 负责确认函数与列合法并生成带类型的计划；
执行层在扫描和过滤之后按分组键归类，计算每组的聚合结果，再按输出别名排序。
