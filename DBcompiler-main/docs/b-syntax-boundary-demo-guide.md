# B 模块支持的 SQL、边界与演示案例

这份文档用于自己学习和现场展示。它不逐条复述文法，而是回答三个更实际的问题：

1. 一条 SQL 进入 B 模块后，哪些写法可以继续生成计划？
2. 写到边界时，系统会得到什么结果？
3. 哪些案例最适合演示正确性、异常处理和优化效果？

完整文法仍以 [grammar.md](../grammar.md) 为准。本篇中的“支持”表示 SQL 不只可以被 A
解析，还已经接通 B 的语义分析、计划生成、JSON 协议和 Java 执行引擎。

## 1. 先理解系统如何处理一条 SQL

```text
SQL 文本
  ↓
A：词法、语法分析，生成 AST
  ↓
B：查表查列、检查类型和作用域，生成 BoundStatement
  ↓
B：生成逻辑计划并进行规则优化
  ↓
JSON 计划
  ↓
Java 执行引擎和存储
```

失败发生在哪一层，含义不同：

| 阶段 | 典型情况 | 系统如何处理 |
|---|---|---|
| 词法或语法 | 少分号、关键字顺序错误、`SELECT FROM` | 返回 Lexical/Syntax 诊断，不进入 B |
| B 语义分析 | 表不存在、列有歧义、WHERE 不是 BOOL | 返回 Semantic 诊断，不生成半成品计划 |
| B 计划生成 | 外部程序传入损坏的 Bound 节点 | 返回 Plan/InvalidBoundStatement |
| 执行 | 除零、数据溢出、唯一约束冲突 | 返回 ExecutionError，写操作按整条语句保持原子性 |

B 的原则是：能在没有数据的情况下确定的错误尽早拒绝；依赖实际记录和求值路径的错误留到执行时。
例如 `age + 'x'` 一定类型错误，会在 B 被拒绝；`10 / score` 是否除零取决于记录，只能在执行时判断。

所有 SELECT 当前都必须带 FROM，每条语句必须用分号结束。未加引号的名称不区分 ASCII 大小写，
且只允许字母、数字和下划线；当前没有双引号标识符。字符串用单引号，内部单引号写成 `''`。
INT 是有符号 64 位整数；FLOAT 必须写成类似 `1.5` 的形式，暂不接受 `.5`、`5.` 或指数写法。

## 2. 当前可完整执行的语法

### 2.1 建表和改表

支持：

- `CREATE TABLE` 和 `CREATE TABLE IF NOT EXISTS`
- `INT`、`FLOAT`、`BOOL`、`VARCHAR`、`VARCHAR(n)`
- `PRIMARY KEY`、`NOT NULL`、`UNIQUE`、`DEFAULT`
- 表级复合 `PRIMARY KEY(a, b)` 和 `UNIQUE(a, b)`
- `ALTER TABLE ... ADD [COLUMN] ...`
- `ALTER TABLE ... DROP [COLUMN] ...`
- `ALTER TABLE ... RENAME TO ...`
- `ALTER TABLE ... RENAME COLUMN ... TO ...`
- `DROP TABLE [IF EXISTS] table1, table2`

主要边界：

| 边界 | 边界内行为 | 超出边界后的处理 |
|---|---|---|
| 一张表至少一列 | 正常创建 | 空列清单在语法或 Catalog 检查中拒绝 |
| 一张表只能有一个主键 | 单列或复合主键均可 | 第二个主键返回约束错误 |
| `VARCHAR(n)` 按 Unicode 码点计数 | 长度不超过 n 可写入 | 超长值返回 ConstraintViolation |
| ADD 到已有非空表 | 有 DEFAULT 或允许 NULL 时迁移旧行 | 新列 NOT NULL 且无默认值时拒绝，原表不变 |
| DROP COLUMN | 普通非约束列可以删除 | 最后一列或复合约束成员不能删除 |
| RENAME | 保留表 ID、列 ID、RowId | 新名字冲突时拒绝，原模式不变 |

`IF NOT EXISTS` 命中已有表时是成功的空操作：不会用新定义覆盖旧表，也不会增加 CatalogVersion。

### 2.2 增删改

支持：

- 单行或多行 `INSERT ... VALUES`
- INSERT 显式列清单和列顺序重排
- 省略列时填 DEFAULT 或 NULL
- `UPDATE ... SET ... [WHERE ...]`
- `DELETE ... [WHERE ...]`
- UPDATE/DELETE 表别名

主要边界：

- INSERT 的值只能是字面量，暂不支持 `INSERT ... SELECT` 和 VALUES 中的表达式或 `DEFAULT` 关键字。
- 一批多行 INSERT 会先全部校验。中间任何一行违反约束时，整批都不写入。
- UPDATE 的所有右值读取“更新前的同一行”。`SET a=b, b=a` 能交换两列。
- 同一条 UPDATE 不能重复修改同一目标列。
- 省略 WHERE 表示影响全部行；WHERE 存在时必须是 BOOL。
- UPDATE/DELETE 依靠内部 RowId 定位记录。RowId 不作为普通查询列暴露。

### 2.3 SELECT、连接和聚合

支持：

- `SELECT *`、列、计算表达式、重复输出列和 `AS` 别名
- `WHERE`、`DISTINCT`、`LIMIT/OFFSET`
- INNER、LEFT、RIGHT、FULL JOIN，以及多表左深连接
- 表别名、自连接、限定列名
- `GROUP BY`、`HAVING`
- `COUNT`、`SUM`、`AVG`、`MIN`、`MAX`
- `ORDER BY` 列、输出别名或表达式，支持 ASC/DESC
- FROM 或 JOIN 中带别名的派生表
- `UNION/INTERSECT/EXCEPT` 及对应的 ALL

主要边界：

| 功能 | 当前规则 |
|---|---|
| 未限定列名 | 只命中一个可见关系时成功；命中多个时报 AmbiguousColumn |
| 表别名 | 声明别名后，应使用别名限定；原表名被隐藏 |
| JOIN ON | 必须得到 BOOL；只有 TRUE 算匹配，FALSE 和 NULL 都不匹配 |
| GROUP BY | 普通输出列必须属于分组键；不允许重复分组键 |
| 聚合 | 支持顶层聚合和聚合结果算术；暂不支持聚合 DISTINCT、COUNT(1)、嵌套聚合 |
| ORDER BY | 可以使用隐藏源列、表达式或唯一输出别名；同名别名会产生歧义 |
| LIMIT/OFFSET | 只接受非负整数字面量 |
| 派生表 | 必须有别名，输出列名必须唯一；当前不支持 LATERAL 外层引用 |
| 集合运算 | 两侧列数和对应类型必须相同；最终列名采用左分支 |

排序中的 NULL 规则固定为：ASC 时 NULL 在最后，DESC 时 NULL 在最前。同一排序键的行之间没有
额外稳定顺序；演示需要固定顺序时，应再加一个唯一键。

### 2.4 表达式、NULL、CASE 和子查询

支持：

- `+ - * /`、`= != <> < <= > >=`
- `AND OR NOT`，按照 NOT、AND、OR 的优先级解析
- `IS NULL`、`IS NOT NULL`
- `LIKE/NOT LIKE`，其中 `%` 匹配任意串，`_` 匹配一个 Unicode 码点
- `BETWEEN/NOT BETWEEN`
- 字面量列表和子查询形式的 `IN/NOT IN`
- `EXISTS/NOT EXISTS`
- 单列标量子查询
- 搜索型 CASE 和简单型 CASE
- 关联子查询

没有隐式数值转换：INT 只能和 INT 做普通算术，FLOAT 只能和 FLOAT 做普通算术。
VARCHAR 和 BOOL 支持相等/不等判断；VARCHAR 另支持 LIKE。类型不同会在 B 阶段拒绝。

NULL 使用 SQL 三值逻辑：

| 表达式 | 结果 |
|---|---|
| `NULL = NULL` | UNKNOWN，而不是 TRUE |
| `NULL IS NULL` | TRUE |
| `TRUE AND NULL` | UNKNOWN |
| `FALSE AND NULL` | FALSE |
| `TRUE OR NULL` | TRUE |
| WHERE 得到 NULL | 该行不保留 |

`IN (subquery)` 的特殊边界是：如果没有找到相等值，但子查询里出现 NULL，则结果为 UNKNOWN；
空子查询返回 FALSE。标量子查询零行返回 NULL，正好一行返回该值，多于一行返回
`ScalarSubqueryCardinality`，不会偷偷取第一行。

CASE 按 WHEN 的书写顺序短路，只求值命中的 THEN。搜索型 CASE 的 WHEN 必须为 BOOL；简单型
CASE 的 WHEN 必须和 CASE 操作数同类型；所有非 NULL 结果分支必须是同一类型。

### 2.5 EXPLAIN 和 EXPLAIN ANALYZE

- `EXPLAIN statement` 展示优化后的计划，不执行目标语句。
- `EXPLAIN ANALYZE statement` 真正执行目标，同时展示 `actual rows`、`time` 和 `loops`。
- 对 SELECT，ANALYZE 能直观看到每个算子前后的行数变化。
- 对 CREATE/INSERT/UPDATE/DELETE/DROP，ANALYZE 会产生真实副作用。
- 目标执行报错时，ANALYZE 直接返回原错误，不伪造成功报告。
- 不支持嵌套 EXPLAIN。

演示时不要对不希望修改的数据运行 `EXPLAIN ANALYZE UPDATE/DELETE`。

### 2.6 明确还不在范围内的功能

下面这些写法没有对应的完整 B 计划和执行语义：

- `WITH` 公共表表达式、窗口函数和递归查询
- `INSERT ... SELECT`、SELECT without FROM
- 子查询派生表的 LATERAL 外层引用
- 聚合 `DISTINCT`、`COUNT(1)`、聚合参数表达式和嵌套聚合
- `FOREIGN KEY`、`CHECK`、`CREATE INDEX`、视图和触发器
- `ALTER COLUMN TYPE`、增加或删除表约束
- 事务、锁、用户权限和并发控制语句
- 隐式类型转换，以及 DATE、DECIMAL、BLOB 等类型
- 基于索引的扫描、代价估算和连接顺序选择

这些输入通常由 A 返回 `UnexpectedToken`；如果 A 已经能构造某种扩展 AST、但 B 尚未定义语义，
B 会返回 `UnsupportedFeature` 或更具体的语义错误。系统不会把未知功能忽略后继续执行。

## 3. 常见异常怎样反映

下面的错误 SQL 应分别执行，因为编译器采用“遇到当前首个错误就返回”的方式。

| 示例 | 阶段 | 预期反映 |
|---|---|---|
| `SELECT * FROM missing;` | B | TableNotFound |
| `SELECT id FROM a JOIN b ON a.id=b.id;` | B | 两表都有 id 时为 AmbiguousColumn |
| `SELECT * FROM t WHERE 1;` | B | WhereNotBoolean |
| `SELECT 1 + 'x' FROM t;` | B | InvalidOperandType |
| `SELECT name FROM t GROUP BY id;` | B | InvalidGrouping |
| `SELECT id FROM a UNION SELECT name FROM b;` | B | 集合列类型不兼容 |
| `SELECT (SELECT value FROM score) FROM people;` | 执行 | 子查询多行时 ScalarSubqueryCardinality |
| `INSERT INTO t VALUES (1), (1);` | 执行 | 主键/唯一约束冲突，整批不写入 |
| `SELECT 1/0 FROM t;` | 执行 | 有输入行且表达式可达时 DivisionByZero |
| 超过 256 层括号或 NOT | A 或 B 防御检查 | ExpressionTooDeep |

错误信息包含阶段、稳定错误码和源码位置。B 不通过返回空计划或默认值来掩盖错误。

## 4. 展示案例一：新语法正常工作

下面是最短的综合演示。它同时展示复合约束、CASE、关联 EXISTS 和派生表：

```sql
CREATE TABLE student(
  id INT PRIMARY KEY,
  name VARCHAR(20) NOT NULL,
  age INT
);
CREATE TABLE score(
  student_id INT,
  course_id INT,
  value INT,
  UNIQUE(student_id, course_id)
);
INSERT INTO student VALUES (1,'Alice',20),(2,'Bob',17),(3,'Cara',22);
INSERT INTO score VALUES (1,10,90),(1,11,70),(2,10,95),(3,10,85);

SELECT s.name,
       CASE WHEN s.age >= 18 THEN 'adult' ELSE 'minor' END AS kind
FROM student s
WHERE EXISTS (
  SELECT * FROM score sc
  WHERE sc.student_id=s.id AND sc.value>=80
)
ORDER BY s.id;

SELECT d.name
FROM (SELECT name, age FROM student WHERE age>=18) d
WHERE d.age<22;
```

第一条查询应返回 Alice/adult、Bob/minor、Cara/adult；第二条只返回 Alice。

## 5. 展示案例二：约束失败且不留下半成品数据

```sql
CREATE TABLE account(id INT PRIMARY KEY, name VARCHAR(5) NOT NULL);
INSERT INTO account VALUES (1,'Amy');

-- 将这一条作为单独的失败操作执行，并在调用方捕获错误。
INSERT INTO account VALUES (2,'Bob'),(1,'Again');

-- 捕获错误后，在同一个 DatabaseEngine 实例中查询。
SELECT * FROM account;
```

第二批 INSERT 会因主键 1 重复而返回 `ConstraintViolation`。该批的第一行 `(2,'Bob')` 也不会
留下，因此调用方捕获异常后，用同一个引擎实例查询时仍只有 `(1,'Amy')`。命令行 `Main` 遇错会
终止，自动化测试或应用层可以捕获 `EngineException` 后继续检查状态。如果把名字改成超过 5 个
Unicode 码点，也会在写入前被拒绝。

这个案例说明“检查约束”不是只在编译时保存几个标志，执行层确实针对最终表状态做原子检查。

## 6. 展示案例三：NULL、IN 和标量子查询边界

```sql
CREATE TABLE outer_t(id INT PRIMARY KEY);
CREATE TABLE inner_t(value INT);
INSERT INTO outer_t VALUES (1),(2);
INSERT INTO inner_t VALUES (1),(NULL);

SELECT id FROM outer_t WHERE id IN (SELECT value FROM inner_t);
SELECT id FROM outer_t WHERE id NOT IN (SELECT value FROM inner_t);
SELECT (SELECT value FROM inner_t) AS only_value FROM outer_t WHERE id=1;
```

- 第一条只返回 1。
- 第二条不返回 2，因为 `2 NOT IN (1, NULL)` 是 UNKNOWN，不是 TRUE。
- 第三条返回 `ScalarSubqueryCardinality`，因为 inner_t 有两行。

这个案例适合说明系统实现的是 SQL 三值逻辑，而不是把 NULL 当成普通 Java/C++ 值。

## 7. 展示案例四：谓词下推和列裁剪

准备两张表：student 有 3 行，score 有 5 行，两张表各带一个完全无关的 `unused` 列。执行：

```sql
EXPLAIN ANALYZE
SELECT s.name, sc.value
FROM opt_student AS s
JOIN opt_score AS sc ON s.id=sc.student_id
WHERE s.age>=18 AND sc.value>=80
ORDER BY s.name;
```

优化前的形状是：

```text
Filter[age>=18 AND value>=80]
  Join
    SeqScan student[id,name,age,unused]
    SeqScan score[id,student_id,value,unused]
```

优化后两个单表条件分别进入 JOIN 两侧，扫描列也缩小：

```text
Join
  Filter[age>=18]
    SeqScan student[id,name,age]
  Filter[value>=80]
    SeqScan score[student_id,value]
```

按仓库固定测试数据：

| 指标 | 优化前 | 优化后 |
|---|---:|---:|
| 嵌套循环候选组合 | 3 × 5 = 15 | 2 × 4 = 8 |
| 两侧扫描列数 | 4 + 4 | 3 + 2 |
| 物化业务值个数 | 32 | 19 |

结果仍是 Alice/90 和 Cara/85。这个案例同时说明优化减少行数和行宽，并且没有改变结果。

## 8. 展示案例五：零列扫描和空结果传播

```sql
EXPLAIN ANALYZE SELECT COUNT(*) AS total FROM opt_student;
EXPLAIN ANALYZE SELECT name FROM opt_student WHERE 1=0;
EXPLAIN ANALYZE SELECT COUNT(*) AS total FROM opt_student WHERE 1=0;
```

应观察到：

1. `COUNT(*)` 的 SeqScan 显示 `columns=<none>`。它只数行，不物化任何业务列。
2. 普通恒假查询的输入变成 EmptyResult，不再出现 SeqScan，但 Project 仍保留结果列名。
3. 恒假条件下的全局 COUNT 仍保留 Aggregate，返回一行 0，而不是零行。

第三点是重要边界：优化器不能看到“输入为空”就连聚合一起删除。

## 9. 展示案例六：优化不能隐藏运行期错误

```sql
EXPLAIN
SELECT s.name
FROM opt_student s
JOIN opt_score sc ON s.id/0=sc.student_id
WHERE 1=0;
```

虽然 WHERE 恒假，计划仍保留 JOIN。按当前执行顺序，JOIN ON 的除零在上层 Filter 前可达；如果
直接把整棵树改成 EmptyResult，就会错误地隐藏 `DivisionByZero`。

另一个方向的例子是：

```sql
UPDATE opt_student SET age=1/0 WHERE 1=0;
```

它应成功并报告影响 0 行，因为 RHS 只对真正要更新的记录求值。优化后的 Update 根仍在，输入变为
EmptyResult。两个案例放在一起，可以说明优化器同时保护“应该报的错”和“不应该求值的表达式”。

## 10. 如何直接复现实验

在仓库根目录运行：

```bash
bash DBcompiler-main/scripts/check.sh
bash scripts/check_advanced_execution.sh
java -cp minidb-engine/target/classes minidb.Main \
  < minidb-engine/target/optimizer-rules-plan.json
```

完整的新语法 SQL 在
[a-extension-features.sql](../../minidb-engine/src/test/resources/a-extension-features.sql)，优化展示 SQL 在
[optimizer-rules.sql](../../minidb-engine/src/test/resources/optimizer-rules.sql)。现场演示优先选择第 7～9 节，
因为它们既有可见的计划变化，也能说明 B 模块没有为了缩短计划而破坏异常和边界语义。
