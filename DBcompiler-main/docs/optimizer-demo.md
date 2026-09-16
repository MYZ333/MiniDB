# 规则优化效果演示

本文用当前仓库可以直接运行的 SQL 展示 B 模块的四阶段计划优化：常量折叠、谓词下推、
空结果传播和列裁剪。重点观察计划形态、算子实际行数和扫描列数。`EXPLAIN ANALYZE` 中的
耗时会受机器、JVM 预热和数据规模影响，所以示例省略 `time`，保留可稳定复现的
`actual rows`、算子位置和列集合。

实现原理见[规则优化代码讲解](optimizer-walkthrough.md)，完整端到端测试 SQL 见
[optimizer-rules.sql](../../minidb-engine/src/test/resources/optimizer-rules.sql)，断言见
[OptimizerRulesEngineTest.java](../../minidb-engine/src/test/java/minidb/OptimizerRulesEngineTest.java)。

## 1. 复现方法

在仓库根目录执行：

```bash
# 构建 C++ 编译器，并运行包含优化器在内的全部 C++ 测试。
bash DBcompiler-main/scripts/check.sh

# 查看常量折叠、Filter 删除和列裁剪的前后计划。
DBcompiler-main/build/direct/optimizer_example

# 生成真实优化 JSON，并运行 Java 端到端回归。
bash scripts/check_advanced_execution.sh

# 再次执行优化展示脚本，直接查看 EXPLAIN ANALYZE 和查询结果。
java -cp minidb-engine/target/classes minidb.Main \
  < minidb-engine/target/optimizer-rules-plan.json
```

最后一条命令应依次展示联合优化、零列扫描、空结果传播、外连接和空写操作案例。
当前自动化结果为 C++ 优化器测试 34 组通过，Java 的 `OptimizerRulesEngineTest` 通过。

## 2. 测试数据

联合优化使用两张各含一个无关列的表：

```sql
CREATE TABLE opt_student(
    id INT PRIMARY KEY,
    name VARCHAR(20),
    age INT,
    unused VARCHAR(20)
);
CREATE TABLE opt_score(
    id INT PRIMARY KEY,
    student_id INT,
    value INT,
    unused VARCHAR(20)
);

INSERT INTO opt_student VALUES
    (1, 'Alice', 20, 'left-a'),
    (2, 'Bob',   17, 'left-b'),
    (3, 'Cara',  22, 'left-c');
INSERT INTO opt_score VALUES
    (11, 1, 90, 'right-a'),
    (12, 1, 70, 'right-b'),
    (13, 2, 95, 'right-c'),
    (14, 3, 85, 'right-d'),
    (15, 4, 99, 'right-e');
```

`opt_student` 有 3 行、4 列，`opt_score` 有 5 行、4 列。后面的数量对比均基于这组固定数据。

## 3. 案例一：常量折叠与恒真 Filter 删除

这一案例由 `optimizer_example` 在内部注册 `student(id, name, age)` 表模式，不依赖第二节的测试数据。
执行：

```sql
SELECT name FROM student WHERE 1=1 AND age>10+8;
```

`optimizer_example` 给出的真实前后计划为：

```text
Before:
CatalogVersion: 1
Project[student.name] output=[name:VARCHAR] row_id=no
  Filter[((1 = 1) AND (student.age > (10 + 8)))] output=[id:INT, name:VARCHAR, age:INT] row_id=no
    SeqScan[student#1] output=[id:INT, name:VARCHAR, age:INT] row_id=no

After:
CatalogVersion: 1
Project[student.name] output=[name:VARCHAR] row_id=no
  Filter[(student.age > 18)] output=[name:VARCHAR, age:INT] row_id=no
    SeqScan[student#1; columns=student.name, student.age] output=[name:VARCHAR, age:INT] row_id=no
```

这里连续发生三次改写：`1=1` 折叠为 TRUE，`10+8` 折叠为 18，`TRUE AND x` 化简为 `x`。
列裁剪又把扫描列从 `id、name、age` 缩减为 `name、age`。如果条件只有 `WHERE 1=1`，
整个 Filter 会被删除：

```text
Before: Project -> Filter[1 = 1] -> SeqScan[id, name, age]
After:  Project -> SeqScan[name]
```

这个案例适合展示优化器如何先减小表达式，再减小算子树和行宽，同时保持结果列名与类型不变。

## 4. 案例二：JOIN 谓词下推与列裁剪

执行：

```sql
EXPLAIN ANALYZE
SELECT s.name, sc.value
FROM opt_student AS s
JOIN opt_score AS sc ON s.id=sc.student_id
WHERE s.age>=18 AND sc.value>=80
ORDER BY s.name;
```

计划生成器最初把 WHERE 放在 JOIN 上方，并让两个 SeqScan 提供整表列：

```text
Project[name, value]
  Sort[name]
    Filter[age >= 18 AND value >= 80]
      NestedLoopJoin[id = student_id]
        SeqScan[s: id, name, age, unused]
        SeqScan[sc: id, student_id, value, unused]
```

优化后的真实 `EXPLAIN ANALYZE` 关键输出为：

```text
Project [name, value] (actual rows=2 loops=1)
  Sort [opt_student.name ASC] (actual rows=2 loops=1)
    NestedLoopJoin [INNER; opt_student.id = opt_score.student_id]
      (actual rows=2 loops=1)
      Filter [opt_student.age >= 18] (actual rows=2 loops=1)
        SeqScan [opt_student AS s;
          columns=opt_student.id, opt_student.name, opt_student.age]
          (actual rows=3 loops=1)
      Filter [opt_score.value >= 80] (actual rows=4 loops=1)
        SeqScan [opt_score AS sc;
          columns=opt_score.student_id, opt_score.value]
          (actual rows=5 loops=1)
```

两个只引用单侧关系的条件被分别推到 JOIN 左右输入。当前执行器采用嵌套循环连接，因此连接条件的
候选组合从未下推时的 `3×5=15` 次降到 `2×4=8` 次，减少约 46.7%。列裁剪删除了两个
`unused` 列和不参与任何上层计算的 `opt_score.id`：

| 指标 | 优化前 | 优化后 | 变化 |
|---|---:|---:|---:|
| JOIN 候选组合 | 15 | 8 | 减少 7，约 46.7% |
| 两个扫描输出列数 | 4 + 4 | 3 + 2 | 减少 3 列 |
| 本案例物化的业务值 | `3×4 + 5×4 = 32` | `3×3 + 5×2 = 19` | 减少 13，约 40.6% |

“物化的业务值”是按当前内存执行器逐行、逐列取值计算的数量，用于说明行宽变化，不是字节级
内存或磁盘 I/O 测量。查询结果保持为：

```text
name  | value
Alice | 90
Cara  | 85
```

## 5. 案例三：COUNT(*) 的零列扫描

执行：

```sql
EXPLAIN ANALYZE SELECT COUNT(*) AS total FROM opt_student;
```

输出为：

```text
Aggregate [group=<all>; output=total] (actual rows=1 loops=1)
  SeqScan [opt_student; columns=<none>] (actual rows=3 loops=1)

total
3
```

`COUNT(*)` 只需要知道输入有多少行，不读取任何业务列。因此 SeqScan 仍产生 3 个逻辑行，
但 `columns=<none>` 表示不从每条存储记录物化 `id、name、age、unused`。按本数据集计算，
业务值物化数量从 `3×4=12` 降到 0。这里不能把扫描改为空结果，否则 COUNT 会从 3 错成 0。

## 6. 案例四：恒假条件与 LIMIT 0 消除扫描

执行：

```sql
EXPLAIN ANALYZE SELECT name FROM opt_student WHERE 1=0;
EXPLAIN ANALYZE SELECT name FROM opt_student LIMIT 0;
```

两条语句的输入都被改为 `EmptyResult`：

```text
Project [name] (actual rows=0 loops=1)
  EmptyResult [columns=opt_student.name] (actual rows=0 loops=1)

Project [name; limit=0] (actual rows=0 loops=1)
  EmptyResult [columns=opt_student.name] (actual rows=0 loops=1)
```

计划中不再出现 SeqScan，执行层不访问存储记录。Project 仍保留，因为即使结果为零行，客户端
仍需获得名为 `name` 的结果列。EmptyResult 保存列身份和关系来源，使上层算子仍能验证布局。

全局聚合还有额外语义边界：

```sql
EXPLAIN ANALYZE
SELECT COUNT(*) AS total FROM opt_student WHERE 1=0;
```

```text
Aggregate [group=<all>; output=total] (actual rows=1 loops=1)
  EmptyResult [columns=<none>] (actual rows=0 loops=1)

total
0
```

输入虽然为零行，Aggregate 不能删除，因为全局 `COUNT(*)` 必须产生一行 0。

## 7. 案例五：空结果经过外连接仍保持正确语义

执行：

```sql
EXPLAIN ANALYZE
SELECT kept.name
FROM opt_student AS s
JOIN opt_score AS sc ON 1=0
RIGHT JOIN opt_student AS kept ON sc.student_id=kept.id
ORDER BY kept.id;
```

内层恒假 INNER JOIN 被折叠，但外层 RIGHT JOIN 保留：

```text
Project [name] (actual rows=3 loops=1)
  Sort [opt_student.id ASC] (actual rows=3 loops=1)
    NestedLoopJoin [RIGHT; opt_score.student_id = opt_student.id]
      (actual rows=3 loops=1)
      EmptyResult [columns=opt_score.student_id] (actual rows=0 loops=1)
      SeqScan [opt_student AS kept;
        columns=opt_student.id, opt_student.name] (actual rows=3 loops=1)
```

```text
name
Alice
Bob
Cara
```

RIGHT JOIN 要保留右表的所有行，所以不能把整个外连接继续折叠为空。EmptyResult 中保留的
`opt_score.student_id` 身份让执行器能为左侧构造正确的 NULL 布局。这一案例可以同时展示
优化收益和外连接语义保护。

## 8. 案例六：优化不能隐藏运行期错误

执行下面的普通 EXPLAIN：

```sql
EXPLAIN
SELECT s.name
FROM opt_student AS s
JOIN opt_score AS sc ON s.id/0=sc.student_id
WHERE 1=0;
```

优化后的真实计划仍保留 Filter 和 JOIN：

```text
Project [name]
  Filter [FALSE]
    NestedLoopJoin [INNER; (opt_student.id / 0) = opt_score.student_id]
      SeqScan [opt_student AS s; columns=opt_student.id, opt_student.name]
      SeqScan [opt_score AS sc; columns=opt_score.student_id]
```

执行器会先执行 JOIN，再执行上层 Filter，因此 JOIN 条件中的除零原本可达。如果仅看到
`WHERE 1=0` 就把整棵输入改为 EmptyResult，查询会从“报告 `DivisionByZero`”错误地变成“返回
零行”。`safeToSkip` 会把含除法、可能溢出的算术和一元取负视为危险边界，所以保留原求值顺序。
将 `EXPLAIN` 去掉并执行该查询，会得到：

```text
ExecutionError [DivisionByZero]: division by zero
```

错误输出还会带 SQL 源码行列；具体位置取决于查询在输入脚本中的位置。

相反，下面的 UPDATE 不会计算右侧表达式，因为恒假筛选后没有待更新行：

```sql
UPDATE opt_student SET age=1/0 WHERE 1=0;
DELETE FROM opt_student WHERE 1=0;
SELECT COUNT(*) AS rows_after_empty_dml FROM opt_student;
```

```text
UPDATE affected=0
DELETE affected=0
rows_after_empty_dml
3
```

Update/Delete 根节点始终保留，EmptyResult 只替换其输入。这既避免无效扫描，也保留了命令类型、
影响行数和不可达表达式不求值的行为。

## 9. 展示时如何概括

可以按“表达式、行数、列数、语义边界”四步讲解：

1. 常量折叠先把条件变简单，并删除恒真 Filter。
2. 谓词下推在 JOIN 前减少行数，本案例把候选组合从 15 降到 8。
3. 列裁剪减少每行携带的数据，本案例物化业务值从 32 降到 19，COUNT(*) 降到零列。
4. 空结果传播完全跳过确定无输出的扫描，但保留聚合、外连接、写操作和运行期错误语义。

这些优化不依赖统计信息，属于确定性的规则优化。测试同时比较优化前后查询结果、DML 影响行数、
错误类型和源码位置，避免只验证“计划看起来更短”而没有验证语义等价。
