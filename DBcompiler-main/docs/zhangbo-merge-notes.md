# feature-zhangbo 与 B 聚合分支整合

> 本文记录 0.22 合并当时的状态。其“仅解析”限制已由 B 0.23 实现取代；
> 当前范围见 [grammar.md](../grammar.md)，代码阅读见 [A 扩展功能实现讲解](remaining-features-walkthrough.md)。

本次以 B 的 945d0aa 为基线，合并 A 的 feature-zhangbo（f32ac98，包含 e6d63f3）。
使用双亲 merge commit 保留两人的提交来源。工作目录为 MiniDB/DBcompiler-main，
执行适配位于相邻 minidb-engine。

## 合入内容和可执行范围

A 的 Lexer/Parser 新增语法、错误信息、Token 原文和位置提示，以及 AST 展示优化和测试全部保留。
B 原有 CRUD、JOIN、自连接、分组排序及 COUNT/SUM/AVG/MIN/MAX 的执行能力保留。
新增可执行语法包括 <>、BETWEEN/NOT BETWEEN、字面量 IN/NOT IN、INNER JOIN、
UPDATE/DELETE 别名及 IS NULL/IS NOT NULL。

DISTINCT、HAVING、LIMIT/OFFSET、外连接、一般 SELECT/ORDER BY 表达式、LIKE、
VARCHAR 长度与列约束、多行 INSERT、DROP TABLE 目前只解析。
B 会返回 UnsupportedFeature；不能忽略这些字段后执行原计划。
具体边界和 NULL 限制见 grammar.md 0.22。

## 冲突如何解决、代码如何阅读

1. **ast.hpp / parser.cpp**：采用 A 的新 AST。
   SelectItem 是 variant<Identifier, AggregateCall, ExprPtr>，
   别名统一保存在 SelectStmt.column_aliases；聚合参数是 AllColumns 或 Identifier。
   原 B 的带 alias 的 SelectItem 结构不再作为公共接口，手工构造 AST 的测试已迁移。
2. **semantic/analyzer.cpp**：增加聚合枚举映射和顶层项归一化。
   (COUNT(*)) 与 (age) 在 A 中是 ExprPtr，B 将其还原成聚合调用/列名，
   然后使用既有绑定流程。SUM(age)+1 等计算表达式明确拒绝。
   AggregateFunction 显式映射到 AggregateKind，绑定后的计划和 JSON 聚合字段保持一致。
3. **semantic/analyzer.cpp 的语句入口**：检查暂未支持的字段。
   特别检查 limit/offset 的 optional 是否存在，所以 LIMIT 0 也不会漏掉；
   多行 INSERT 不会只写入第一行，LEFT/RIGHT/FULL 不会被当成内连接。
   A 的 UPDATE/DELETE 别名作用域被保留，最终列身份仍为 relation_id=0。
4. **type_rules.cpp / plan_printer.cpp / app/plan_json.cpp**：
   IS NULL 与 IS NOT NULL 对任意类型返回 BOOL；打印和 JSON 明确区分这些运算，
   避免旧 visitor 把新的一元运算显示成负号。LIKE 有独立名称但执行入口明确拒绝。
5. **DatabaseEngine.java**：新增两个一元执行分支，求值后直接比较是否为 null。
   BETWEEN/IN/<> 复用原比较和逻辑计划；没有新增执行算子。

例如：

```sql
SELECT (COUNT(*)) AS rows, SUM(age) AS total
FROM student WHERE age IS NOT NULL;
```

A 保留列/函数/别名及源码范围；B 将 COUNT 包装归一化，绑定 age 和 BOOL 条件，
生成 Aggregate → Filter(IsNotNull) → SeqScan；Java 先过滤，再计算聚合。
这也验证了 A 的语法扩展与 B 的第三部分能在同一条 SQL 中组合使用。

## 验证

- 编译器目录执行 bash scripts/check.sh：独立头文件、警告即错误编译、全部 A/B 模块测试和示例。
- MiniDB 根目录执行 bash scripts/check_advanced_execution.sh：真实 SQL → C++ JSON → Java。
- scaffold_smoke 新增聚合 AST 兼容、20 条仅解析语法的明确拒绝、标量语法与 DML 别名测试。
- advanced-query.sql 新增 DML 别名、IN/BETWEEN/<>、INNER JOIN、NULL 判定及括号聚合的结果断言。
- 保留聚合空输入、全 NULL、自连接、排序、溢出行列和旧 CRUD 回归。

A 的两份扩展说明保留为交付历史，并添加指向最新契约的提示；
当前功能状态以 grammar.md、interfaces.md 和 json-plan-protocol.md 为准。
