# EXPLAIN ANALYZE 实现讲解

EXPLAIN ANALYZE 横跨 A、B 和 Java 执行层。阅读时先抓住一个原则：它包裹现有计划，
不复制 SELECT、UPDATE 或 DELETE 的绑定和执行逻辑。

## 1. 三层包装结构

`ast.hpp` 的 ExplainStmt 保存 ExplainTarget 和 analyze 开关。ExplainTarget 只含六类基础
语句，从类型上禁止嵌套 EXPLAIN。Parser 的 `baseStatement()` 不消费分号，普通语句和
EXPLAIN 目标因此共用同一组分支。

`bound.hpp` 的 BoundExplain 持有完整 BoundStatement。Analyzer 先用原来的 `bindStatement`
重载绑定目标，再加包装；表不存在、类型不匹配、分组不合法等规则不会因
为 EXPLAIN 而被跳过。

`plan.hpp` 的 ExplainPlan 持有一棵 PlanPtr 目标树。buildPlan 递归生成该树，再把包装
根的输出固定为单个 VARCHAR 列 `QUERY PLAN`。optimizer 先优化 ExplainPlan.input，所以用户
看到的是实际交给引擎的优化后树。

```text
EXPLAIN ANALYZE SELECT ...
  -> ExplainStmt(target, analyze=true)
  -> BoundExplain(bound target, analyze=true)
  -> ExplainPlan(optimized target, analyze=true)
  -> JSON Explain(input, analyze=true)
```

## 2. Catalog 副作用

`app/plan_json.cpp` 会为一整段 SQL 脚本模拟 Catalog 版本变化。对普通 EXPLAIN CREATE/DROP，
导出器不更新模拟 Catalog；对 EXPLAIN ANALYZE CREATE/DROP，它按内部语句更新。这与
Java 引擎的真实执行副作用一致，保证后续语句的 catalogVersion 正确。

## 3. 运行时采样

`DatabaseEngine.executeNode` 是语句根分派点，`readInput` 是关系算子分派点。Profiler
只在这两个入口包围原调用，因此不需要在 scan/filter/join/sort 等每个方法内复制
计时代码。IdentityHashMap 以解析后的 JSON Map 对象身份对应统计，避免结构相同的
两个扫描节点被误合并。

```text
executeNode(Explain)
  -> validateExplainTree               # 副作用前校验展示树
  -> activeProfiler = new Profiler
  -> executeNode(target root)           # 记录根节点
       -> readInput(child)              # 递归记录每个子算子
  -> appendExplainLines                 # 前序输出并填充统计
```

actual rows 表示算子的输出行数，修改根使用 affectedRows。time 从算子进入到返回计时，
因此包含子算子时间。loops 是同一节点对象被调用的次数。当前引擎会先物化子输入，
所以通常为 1；以后改为拉取式迭代器后，loops 仍能表达重复调用。

## 4. 副作用与错误

EXPLAIN 只调用树描述函数，不进入目标 `executeNode`。EXPLAIN ANALYZE 必须真实调用目标，
因此 INSERT、UPDATE、DELETE、CREATE 和 DROP 会修改数据或模式。若目标返回除零、约束违反等
EngineException，包装层直接传递原错误，不把失败执行伪装成完整分析报告。

## 5. 测试和演示

`minidb-engine/src/test/resources/explain-analyze.sql` 是可直接用于答辩的脚本。它先对
CREATE 比较 EXPLAIN 和 EXPLAIN ANALYZE，然后展示 Project/Sort/Filter/SeqScan 的行数变化，
最后验证普通 EXPLAIN 不修改记录、ANALYZE 会修改记录。

```bash
bash scripts/check_advanced_execution.sh
java -cp minidb-engine/target/classes minidb.Main \
  < minidb-engine/target/explain-analyze-plan.json
```

答辩时可以解释：SeqScan 输出 3 行，Filter 只输出 2 行，表明条件过滤确实发生；
Project 仍输出 2 行，说明它改变列布局而未进一步减少行。
