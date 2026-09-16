# B 模块代码学习指南

这份文档从“为什么需要 B”开始介绍代码。目标是看完后能够沿着一条 SQL 找到对应代码，并能解释
某处修改为什么可能影响其他模块。详细接口字段可以再查阅 [interfaces.md](interfaces.md)。

## 1. 本模块解决什么问题

A 模块只能回答“这段文字是不是一条符合文法的 SQL”，不能回答这些问题：

- `student.age` 指的是哪张表、哪一列？
- 两张表都有 id 时，裸写 `id` 是否有歧义？
- VARCHAR 能不能和 INT 相加？
- SELECT 中的普通列是否满足 GROUP BY 规则？
- UPDATE 执行时需要扫描哪些列，是否必须携带 RowId？
- WHERE 条件能否提前到 JOIN 之前，提前后会不会改变结果或错误行为？

B 模块位于 AST 和执行引擎之间，负责把“按名字描述的 SQL”变成“按稳定 ID 和类型描述的计划”。
它解决三类问题：

1. **语义分析**：解析表、列、别名和作用域，检查类型及 SQL 规则。
2. **计划生成**：确定扫描、过滤、连接、聚合、排序、投影和写入的执行顺序。
3. **计划优化**：在结果、异常和副作用不变的前提下，减少表达式计算、输入行数和扫描列数。

## 2. 整体设计思路和运行流程

可以把 B 理解为两次“翻译”和一次“改写”：

```text
                    CatalogSnapshot
                          ↓
AST ── analyze ──> BoundStatement ── buildPlan ──> LogicalPlan
                                                     ↓
                                               optimizePlan
                                                     ↓
                                              optimized plan
                                                     ↓
                                             JSON → Java 引擎
```

### 第一次翻译：AST → BoundStatement

AST 中保存的是用户原文，如 `s.age`。analyze 查询 Catalog 和当前作用域，把它变成类似下面的身份：

```text
relation_id = 7       本次查询中的关系实例
table_id    = 2       Catalog 中的物理表
column_id   = 3       表内稳定列 ID
ordinal     = 2       该列在表模式中的位置
type        = INT
```

这里同时保留 relation_id 和 table_id，是为了支持自连接。同一物理表的 `student s1` 和
`student s2` 拥有相同 table_id，但必须有不同 relation_id。

### 第二次翻译：BoundStatement → LogicalPlan

Bound 已经解决名字和类型，buildPlan 主要决定树形执行顺序。例如：

```sql
SELECT name FROM student WHERE age>=18 ORDER BY name;
```

会形成：

```text
Project[name]
  Sort[name]
    Filter[age >= 18]
      SeqScan[student]
```

执行从树叶向根进行：先扫描，再过滤，再排序，最后只输出 name。

### 计划改写：LogicalPlan → optimized LogicalPlan

优化器不访问真实记录，也不改变原计划对象。它依据已经绑定的类型和列身份，创建一棵等价的新树。
规则按固定顺序执行：

```text
安全常量折叠 → 谓词下推 → 空结果传播 → 列裁剪
```

顺序有意义。例如先把 `1=0` 折叠成 FALSE，空结果规则才能识别它；谓词下推完成后，列裁剪才能
在 JOIN 两侧分别计算真正需要的列。

## 3. 主要类、函数和数据结构

### 3.1 公共结果和错误

| 名称 | 作用 |
|---|---|
| `Result<T>` | 成功时保存 T，失败时保存 Diagnostic；不使用半成品结果 |
| `Diagnostic` | 保存阶段、错误码、可读信息和 SourceSpan |
| `SourceSpan` | 保存字节偏移与行列，使 B/执行期错误能指回 SQL 原文 |
| `ScalarValue` | INT/FLOAT/VARCHAR/BOOL/NULL 的统一值类型 |

### 3.2 Catalog 相关结构

| 名称 | 作用 |
|---|---|
| `CatalogSnapshot` | 一次编译期间使用的只读模式快照 |
| `TableSchema` | 表 ID、规范化表名、列和复合约束 |
| `ColumnSchema` | 列 ID、序号、类型和列约束 |
| `MemoryCatalog` | 测试及当前编译流程中的内存模式实现 |

快照设计避免分析过程中表结构突然变化。成功的 DDL 会发布一个新的不可变 TableSchema 并增加
CatalogVersion；旧计划仍能安全持有旧模式，但执行时版本不一致会被拒绝。

### 3.3 Bound 层结构

代码入口在 [bound.hpp](../include/minisql/bound.hpp) 和
[analyzer.cpp](../src/semantic/analyzer.cpp)。

| 名称 | 作用 |
|---|---|
| `BoundColumnRef` | 已解析列的 relation/table/column ID、ordinal 和类型 |
| `BoundExpr` | 已定型的字面量、列、运算、聚合、CASE 和子查询表达式 |
| `BoundSelect` | 已展开的输出、关系、过滤、分组、排序和集合分支 |
| `BoundInsert/Update/Delete` | 已按表模式整理的写操作信息 |
| `BoundCreateTable/BoundAlterTable` | 已规范化并检查过的 DDL 描述 |
| `BoundStatement` | 上述语句的 variant 总入口 |

子查询表达式还保存 `correlated_columns`。它不是把外层全部列都记录下来，而是遍历子查询，只收集
真正引用的外层列。这一点会直接影响列裁剪：外层扫描不能删掉子查询实际要读取的列。

### 3.4 Plan 层结构

代码入口在 [plan.hpp](../include/minisql/plan.hpp) 和
[plan_builder.cpp](../src/planner/plan_builder.cpp)。

| 节点 | 作用 |
|---|---|
| `SeqScanPlan` | 读取一张表；优化后可指定精确列集合 |
| `FilterPlan` | 只保留条件为 TRUE 的行 |
| `NestedLoopJoinPlan` | 执行 INNER/LEFT/RIGHT/FULL 连接 |
| `GroupByPlan/AggregatePlan` | 分组去重或计算聚合值 |
| `SortPlan` | 保存有序排序表达式和方向 |
| `ProjectPlan` | 形成最终列，处理 DISTINCT 和 LIMIT/OFFSET |
| `DerivedTablePlan` | 执行子查询并把输出重新标记为派生关系 |
| `SetOperationPlan` | 执行 UNION/INTERSECT/EXCEPT 和 ALL |
| `EmptyResultPlan` | 表示确定为零行、但仍保留列和关系布局的输入 |
| `Insert/Update/DeletePlan` | 执行写入；UPDATE/DELETE 输入要求 RowId |
| `ExplainPlan` | 包裹完整目标计划，决定是否真实执行并采样 |

`PlanNode.output` 是有序业务列布局，`carries_row_id` 是独立属性。RowId 不混进 SELECT 输出，
但 Filter 必须原样向 Update/Delete 传递它。

### 3.5 四个最重要的入口函数

```cpp
Result<BoundStatement> analyze(const Statement&, const CatalogSnapshot&);
Result<LogicalPlan> buildPlan(const BoundStatement&);
Result<LogicalPlan> optimizePlan(const LogicalPlan&);
std::string formatPlan(const LogicalPlan&);
```

- `analyze` 是 A、Catalog 与 B 的边界。
- `buildPlan` 是 Bound 和计划树的边界。
- `optimizePlan` 是未优化计划和执行计划的边界。
- `formatPlan` 只负责可读展示，不执行计划。

## 4. 核心算法和实现逻辑

### 4.1 名称解析和分层作用域

Analyzer 为 FROM 和每个 JOIN 建立关系作用域。解析列时遵循：

1. 限定名先匹配关系别名，再在该关系中查列。
2. 未限定名扫描当前层所有关系：零个命中是 UnknownColumn，多个命中是 AmbiguousColumn。
3. 子查询当前层未命中时，再向外层查找，从而支持关联子查询。
4. 派生表使用隔离作用域，不向外层查找，因为当前没有 LATERAL。

关系实例 ID 全局递增，避免嵌套查询或自连接中的列身份碰撞。

### 4.2 表达式绑定和类型检查

Analyzer 递归绑定表达式，先绑定子节点，再根据操作符检查类型并确定结果类型：

- INT 与 INT、FLOAT 与 FLOAT 可以算术运算，不做隐式转换。
- 比较、LIKE、逻辑运算各自有明确的输入类型要求。
- WHERE、JOIN ON、HAVING 最终必须是 BOOL。
- CASE 检查 WHEN 形式和所有结果分支类型。
- IN 和标量子查询必须正好输出一列。
- 集合运算检查列数和逐列类型。

绑定阶段不计算 `1/0`。它只确认两个操作数是 INT，把可能的 DivisionByZero 留给真正执行该路径时处理。

### 4.3 INSERT 的列重排和默认值补齐

显式 INSERT 列顺序可以和表模式不同。Analyzer 先建立“输入位置 → 表列序号”映射，再按表模式重建
每一行。未出现的列使用 DEFAULT；没有 DEFAULT 且允许 NULL 时补 NULL；必填列无法补齐时返回错误。

这样计划和执行层永远按照表模式读取 VALUES，不需要再次按字符串查列。

### 4.4 GROUP BY 和聚合检查

Analyzer 先识别 SELECT/HAVING/ORDER 中的聚合调用，再判断查询属于普通查询、纯 GROUP BY，还是
聚合查询。聚合外出现的列必须来自分组键；无 GROUP BY 的全局聚合不能混入普通列。

执行时 COUNT(*) 数全部行；带参数的聚合忽略 NULL。空输入全局聚合仍输出一行：COUNT 为 0，
其他聚合为 NULL；带 GROUP BY 的空输入输出零行。

### 4.5 子查询的计划和执行

Bound 子查询保存完整 BoundSelect。PlanBuilder 为它建立只读子计划，并放入表达式节点。Java 引擎
求值子查询前保存当前外层行，执行子计划时列查找先查内层布局，未命中再查外层行。

- EXISTS 只关心是否存在一行。
- IN 逐行比较并保留 NULL 导致的 UNKNOWN 状态。
- 标量子查询显式区分零行、一行和多行。

### 4.6 ALTER 的不可变模式更新

ALTER 不在原 TableSchema 上直接改字段，而是构造一个新模式。未改列保留 column_id，表改名仍保留
table_id，已有记录保留 RowId。执行引擎先构造迁移后的全部记录并检查约束，全部成功后才替换模式和数据。

这种写法使旧快照仍然有效，也防止 ADD COLUMN 迁移到一半失败后留下混合布局。

## 5. 几处优化具体是怎样实现的

优化器入口在 [optimizer.cpp](../src/optimizer/optimizer.cpp)，私有规则分别放在三个文件中。

### 5.1 安全常量折叠

`constant_fold.cpp` 只接收操作符和值，不依赖 Catalog。它计算 `10+8`、`1=1` 之类的纯常量，
并在计算前检查整数边界、除零和非有限浮点结果。

```text
1=1 AND age>10+8  →  age>18
```

遇到 `1/0` 或溢出时，它返回“不能折叠”，保留原表达式和源码位置。布尔化简还严格遵守从左到右短路：
`FALSE AND x` 可以直接变 FALSE，而 `x AND FALSE` 不能随便删掉 x，因为 x 可能报错。

### 5.2 谓词下推

[predicate_pushdown.cpp](../src/optimizer/predicate_pushdown.cpp) 把 Filter 中的 AND 按原顺序拆开，
收集每项引用的 relation_id，再判断它只属于 JOIN 左侧、右侧，还是跨表条件。

- INNER JOIN：左右单侧条件都可下推。
- LEFT JOIN：只下推左侧条件。
- RIGHT JOIN：只下推右侧条件。
- FULL JOIN：不下推 WHERE 条件。

含算术或一元负号的条件被视为可能报错，不移动；一个危险条件之后的条件也不提前，避免改变错误顺序。

### 5.3 空结果传播

[empty_result.cpp](../src/optimizer/empty_result.cpp) 识别恒假 Filter、恒假 INNER JOIN 和安全的
LIMIT 0，用 `EmptyResultPlan` 代替无需执行的输入。

EmptyResult 仍保存列身份和关系布局，因为外连接需要知道空侧应该补出哪些 NULL。规则还保留这些边界：

- Project 保留结果列名。
- 全局 Aggregate 保留空输入的单行结果。
- Update/Delete 根保留影响行数和语句类别。
- 如果被跳过的输入可能产生运行时错误，就不传播空结果。

### 5.4 列裁剪

[column_pruning.cpp](../src/optimizer/column_pruning.cpp) 从根向叶传递“上层还需要哪些列”：

1. Project 收集输出表达式依赖。
2. Sort、Filter 加入排序键和条件列。
3. Join 加入 ON 中的列，再按 relation_id 分到左右输入。
4. Aggregate 加入分组、聚合参数、HAVING 和排序依赖。
5. SeqScan 按原表顺序只输出依赖集合中的列。

`COUNT(*)` 不依赖业务列，因此可以得到 `columns=[]` 的零列扫描，但仍产生正确数量的逻辑行。
UPDATE 为复制旧行和检查最终约束保留全列；DELETE 可以只保留条件列和独立 RowId。

### 5.5 EXPLAIN ANALYZE 让优化效果可见

B 生成 ExplainPlan，并让优化器递归优化其目标计划。JSON 层传递 analyze 标志；Java 引擎在统一的
节点分派入口记录行数、耗时和调用次数。普通 EXPLAIN 不执行，ANALYZE 执行同一棵优化后计划。

因此展示时既能看到 `Filter` 移到了 JOIN 下方，也能看到它把 3 行变成 2 行、5 行变成 4 行。

## 6. 与其他模块怎样交互

### A → B

A 提供结构完整、带 SourceSpan 的 AST。B 不修改 AST，也不依赖 token 流。每当 A 给 AST variant
增加一种节点，B 的 Analyzer、AST visitor、PlanBuilder 和协议导出通常都要检查是否需要同步处理。

### Catalog → B

B 只读取 CatalogSnapshot，用名字找到稳定的表列模式。CREATE/ALTER/DROP 计划描述“想怎样修改”，
真正发布新版本由执行路径负责。多语句脚本必须按编译、执行、取得新快照、再编译下一条的顺序推进。

### B → JSON → 执行引擎

[plan_json.cpp](../app/plan_json.cpp) 是跨语言边界。它必须完整传递节点类型、输出布局、列身份、
表达式、CatalogVersion 和 RowId 属性。Java 引擎不再按 SQL 名字重新做语义推导，只验证计划契约并执行。

### B → 存储

B 不知道页号、槽号或磁盘格式，只通过计划声明 SeqScan 需要的列以及写操作需要 RowId。Java 执行层
把紧凑扫描布局映射到 StoredRow，再调用当前内存存储完成读取或原子写入。

## 7. 开发中遇到的主要问题及解决方法

| 问题 | 原因 | 解决方法 |
|---|---|---|
| A 新增 AST 后 B 编译或 visitor 漏分支 | variant 分支集合发生变化 | Bound/Plan 增加显式节点，所有 visitor 同步覆盖，并加端到端测试 |
| 自连接和嵌套查询列身份冲突 | 只用 table_id 无法区分同表实例 | 引入独立 relation_id，并在整个分析过程唯一分配 |
| 关联子查询导致外层列被错误裁掉 | 子计划会在运行时读取外层行 | 收集子查询真正引用的 correlated_columns，加入列裁剪依赖 |
| 派生表列无法继续按原表身份引用 | 子查询输出已经是新的关系边界 | 为派生表生成合成模式和关系 ID，执行时按位置重新标记输出 |
| NULL 被当作普通 false 或普通相等值 | SQL 使用三值逻辑 | 表达式返回 nullable 值，WHERE 只接受 TRUE；IN 单独记录 UNKNOWN |
| 外连接谓词下推改变保留行 | NULL 扩展侧不能任意提前过滤 | 按 INNER/LEFT/RIGHT/FULL 建立明确下推矩阵 |
| 恒假优化隐藏除零错误 | 原执行顺序中危险输入先于 Filter | `safeToMove/safeToSkip` 保守识别可能报错表达式 |
| 列裁剪后 ordinal 与紧凑数组下标混淆 | StoredRow 是全表布局，PlanRow 是裁剪布局 | 按 ColumnSlot 身份查值，扫描时再用原 ordinal 从存储取值 |
| ALTER 失败后容易留下半迁移状态 | 直接原地修改模式和记录 | 先构造新模式和新记录、整体校验，最后一次性发布 |
| C++ 计划通过 JSON 交给 Java 时字段漂移 | 两端独立语言和数据结构 | 维护 json-plan-protocol，并用真实 SQL 做跨语言集成测试 |

## 8. 修改某处代码可能产生什么影响

这是修改前最实用的检查表：

| 修改位置 | 可能影响 | 修改后至少检查 |
|---|---|---|
| `ast.hpp` 新增/调整节点 | Analyzer 和所有 AST visitor | Parser、语义测试、AST 优化 visitor |
| `BoundExpr/BoundStatement` | PlanBuilder、优化器引用收集、JSON | 语义测试和计划测试 |
| 类型规则 | WHERE/JOIN、CASE、集合运算、执行值类型 | 正常类型、NULL、混合类型错误 |
| 列解析或 relation_id | 自连接、关联子查询、谓词下推、列裁剪 | 同名列、自连接、嵌套作用域 |
| `PlanNode.output` | Java 行布局、Project、Join NULL 扩展 | 每个算子的列顺序和类型 |
| `carries_row_id` | UPDATE/DELETE 是否能定位行 | 有/无 WHERE 的更新和删除 |
| 新 Plan 节点 | 打印器、四个优化阶段、JSON、Java 分派 | EXPLAIN 和真实端到端执行 |
| JSON 字段名或枚举文本 | Java 协议解析 | C++ 导出 + Java 集成测试 |
| CatalogVersion 更新 | 多语句 DDL、旧计划执行 | IF EXISTS/IF NOT EXISTS 与 ALTER |
| 谓词下推安全条件 | 外连接结果和错误顺序 | LEFT/RIGHT/FULL、除零和溢出 |
| 空结果规则 | COUNT 空输入、外连接、DML 影响行数 | EmptyResult 边界用例 |
| 列裁剪依赖收集 | 运行时缺列或错读列 | JOIN、ORDER、HAVING、关联子查询、DML |

一个简单原则是：修改“节点结构”时沿整条流水线检查；修改“等价改写”时同时检查最终值、错误和副作用。

## 9. 建议的代码阅读顺序

第一次阅读不必从头看完大型 Analyzer。按一条简单 SELECT 逐层跟踪更容易：

1. 在 `bound.hpp` 看 BoundColumnRef、BoundExpr 和 BoundSelect。
2. 在 `analyzer.cpp` 找 SELECT 绑定、列解析和表达式绑定。
3. 在 `plan.hpp` 看 SeqScan、Filter、Project 的结构。
4. 在 `plan_builder.cpp` 看它们怎样从叶到根组成树。
5. 用 `formatPlan` 观察真实计划。
6. 再分别阅读四项优化，比较优化前后树。
7. 最后看 `plan_json.cpp` 和 Java 的 `executeNode/readInput`，理解计划怎样真正运行。

推荐用下面三条 SQL作为阅读入口：

```sql
SELECT name FROM student WHERE age>10+8;

SELECT s.name, sc.value
FROM student s JOIN score sc ON s.id=sc.student_id
WHERE s.age>=18 AND sc.value>=80;

SELECT name FROM student s
WHERE EXISTS (SELECT * FROM score sc WHERE sc.student_id=s.id);
```

它们依次覆盖基础绑定与常量折叠、关系身份与谓词下推、分层作用域与关联列裁剪。

## 10. 验证入口

```bash
# A/B/C++ 单元和计划测试
bash DBcompiler-main/scripts/check.sh

# 真实 SQL → C++ 计划 → JSON → Java 执行
bash scripts/check_advanced_execution.sh

# 查看便于展示的优化计划与实际行数
java -cp minidb-engine/target/classes minidb.Main \
  < minidb-engine/target/optimizer-rules-plan.json
```

只改 B 代码时也建议执行第二条。许多问题只有跨过 JSON 边界并在裁剪后的真实行布局上求值时才会出现。
