# MiniSQL 文法（接口版本 0.28）

本文由 B 维护，供 A 的 Lexer/Parser、B 的语义分析以及执行层共同使用。
已整合 feature-zhangbo 的语法扩展与 B 的聚合实现。下列 EBNF 描述 A 能解析的范围，
第 3 节记录完整执行边界，调用方应以该节作为可执行 SQL 的依据。

## 1. 词法约定

- 关键字和不带引号的标识符按 ASCII 大小写不敏感匹配；保留原文用于诊断。
- 标识符：`[A-Za-z_][A-Za-z0-9_]*`；限定名写作 `name.name`，AST 保留完整文本。
  别名为单段标识符，支持 `AS alias` 和省略 AS 的 `alias`；不支持带引号名称。
- 整数 token：`[0-9]+`。负号是独立 token，值范围为有符号 64 位整数。
  Parser 将紧邻语法意义上的负号和整数字面量组合为负整数，允许
  `-9223372036854775808`；超出范围报告 Syntax / IntegerOutOfRange。
  负号与数字之间允许空白；`-column` 仍构造一元表达式。
- 浮点 token：`[0-9]+\.[0-9]+`，小数点两侧都必须有数字；不支持指数、`.5` 或 `5.`。
- 字符串用单引号包围，`''` 解码为一个单引号；反斜杠没有特殊含义。
  第一阶段不允许字符串跨行。字符串值保持大小写和 UTF-8 字节内容。
- 跳过空白、`--` 行注释和不嵌套的 `/* ... */` 块注释。
- 支持运算符 `= != <> < <= > >= + - * /`；不支持 `==`。
- 支持 TRUE/FALSE、NULL、BOOL/FLOAT、`VARCHAR(n)`、列级和表级约束、ALTER TABLE、CREATE/DROP INDEX、INSERT 多行 VALUES、DROP TABLE、JOIN/ON、派生表、子查询、UNION/INTERSECT/EXCEPT、CASE、SELECT 表达式项、SELECT DISTINCT、GROUP BY、HAVING、ORDER BY 表达式 ASC/DESC、LIMIT/OFFSET、AS、IS、LIKE/NOT LIKE、BETWEEN/NOT BETWEEN、IN/NOT IN、COUNT/SUM/AVG/MIN/MAX 以及 EXPLAIN/EXPLAIN ANALYZE 关键字。
- INSERT 值位置不支持 `DEFAULT` 关键字；省略列时由 B 写入列 DEFAULT 或 NULL。
- COUNT/SUM/AVG/MIN/MAX 由 A 识别为关键字，不能再作为未加引号的普通表名、列名或别名。
- 每条语句必须以分号结束；空输入合法；单独的空分号不是语句。
- 源码位置：字节偏移从 0 开始，行列从 1 开始；列也按字节计算。
  LF、单独 CR 换行，CRLF 作为一个换行，制表符占一列；范围为左闭右开。

## 2. EBNF

大写单词为关键字，双引号为终结符，`[ ]` 表示可选，`{ }` 表示重复。

```ebnf
program     = { statement } ;
statement   = (explain | base_statement), ";" ;
explain     = EXPLAIN, [ANALYZE], base_statement ;
base_statement = create | alter | drop | insert | select | update | delete ;
create      = create_table | create_index ;
create_table = CREATE, TABLE, [IF, NOT, EXISTS], name, "(", table_element,
               { ",", table_element }, ")" ;
create_index = CREATE, INDEX, name, ON, name, "(", name, ")" ;
table_element = column_def | table_constraint ;
column_def  = name, type, { column_constraint } ;
column_constraint = PRIMARY, KEY | NOT, NULL | UNIQUE | DEFAULT, literal ;
table_constraint = (PRIMARY, KEY | UNIQUE), "(", names, ")" ;
type        = INT | VARCHAR, ["(", INTEGER, ")"] | BOOL | FLOAT ;
alter       = ALTER, TABLE, name,
              ( ADD, [COLUMN], column_def
              | DROP, [COLUMN], name
              | RENAME, TO, name
              | RENAME, COLUMN, name, TO, name ) ;
drop        = drop_table | drop_index ;
drop_table  = DROP, TABLE, [IF, EXISTS], names ;
drop_index  = DROP, INDEX, [IF, EXISTS], name ;
insert      = INSERT, INTO, name, [ "(", names, ")" ],
              VALUES, value_row, { ",", value_row } ;
value_row   = "(", literal, { ",", literal }, ")" ;
select      = select_core, { set_operator, [ALL], select_core } ;
select_core = SELECT, [DISTINCT], ("*" | select_items), FROM, table_ref,
              { join }, [where], [group_by], [having], [order_by], [limit] ;
set_operator = UNION | INTERSECT | EXCEPT ;
update      = UPDATE, table_ref, SET, assignment, { ",", assignment }, [where] ;
delete      = DELETE, FROM, table_ref, [where] ;
assignment  = name, "=", expr ;
join        = [join_type], JOIN, table_ref, ON, expr ;
join_type   = INNER | LEFT, [OUTER] | RIGHT, [OUTER] | FULL, [OUTER] ;
table_ref   = name, [ alias ] | "(", select, ")", alias ;
select_items = select_item, { ",", select_item } ;
select_item = expr, [ alias ] ;
aggregate_call = aggregate_func, "(", ("*" | name), ")" ;
aggregate_func = COUNT | SUM | AVG | MIN | MAX ;
alias       = [ AS ], IDENTIFIER ;
where       = WHERE, expr ;
group_by    = GROUP, BY, names ;
having      = HAVING, expr ;
order_by    = ORDER, BY, order_item, { ",", order_item } ;
order_item  = expr, [ASC | DESC] ;
limit       = LIMIT, INTEGER, [OFFSET, INTEGER] ;
names       = name, { ",", name } ;
expr        = or_expr ;
or_expr     = and_expr, { OR, and_expr } ;
and_expr    = not_expr, { AND, not_expr } ;
not_expr    = NOT, not_expr | comparison ;
comparison  = additive, [ (comp_op, additive) | (LIKE, additive) |
                          (NOT, LIKE, additive) |
                          (BETWEEN, additive, AND, additive) |
                          (NOT, BETWEEN, additive, AND, additive) |
                          (IN, "(", literal, { ",", literal }, ")") |
                          (NOT, IN, "(", literal, { ",", literal }, ")") |
                          (IN, "(", select, ")") |
                          (NOT, IN, "(", select, ")") |
                          (IS, [NOT], NULL) ] ;
comp_op     = "=" | "!=" | "<>" | "<" | "<=" | ">" | ">=" ;
additive    = term, { ("+" | "-"), term } ;
term        = unary, { ("*" | "/"), unary } ;
unary       = "-", unary | primary ;
primary     = aggregate_call | case_expr | EXISTS, "(", select, ")" |
              name | INTEGER | FLOAT_LITERAL | STRING | TRUE | FALSE | NULL |
              "(", select, ")" | "(", expr, ")" ;
case_expr   = CASE, [expr], WHEN, expr, THEN, expr,
              { WHEN, expr, THEN, expr }, [ELSE, expr], END ;
literal     = ["-"], (INTEGER | FLOAT_LITERAL) | STRING | TRUE | FALSE | NULL ;
name        = IDENTIFIER, { ".", IDENTIFIER } ;
```

表达式中的负号统一由 `unary` 消费，INSERT 的负数由 `literal` 消费，避免
两个产生式争用负号。`unary` 的负号后直接遇到 INTEGER token 时构造带符号
LiteralExpr；FLOAT_LITERAL 同样可带负号。整数在应用符号后检查范围；遇到其他操作数时构造 UnaryExpr。
这样既保留一元运算节点，也能正确表示 INT64_MIN。

优先级从高到低：一元负号、乘除、加减、比较、NOT、AND、OR。
二元算术运算左结合；NOT 和一元负号右结合；比较不能连写。
例如 `NOT age > 18` 为 `NOT (age > 18)`，`a < b < c` 是语法错误。
这是对 PPT 第 16 页文法与优先级文字不一致的明确取舍。

递归下降的 statement 可以以 EXPLAIN 开始，base_statement 分支分别以 CREATE / ALTER /
DROP / INSERT / SELECT / UPDATE / DELETE 开始；where 的 FIRST 为 WHERE；SELECT 中其后允许
GROUP/HAVING/ORDER/LIMIT、集合操作符或分号。not_expr 的 FIRST 包括 NOT、EXISTS、CASE、负号、
标识符、整数、浮点数、字符串、TRUE/FALSE、NULL 和左括号。
SELECT 子句顺序固定为 JOIN → WHERE → GROUP BY → HAVING → ORDER BY → LIMIT/OFFSET。

## 3. 语义限制与执行支持

| 功能 | A 解析 | B 绑定、计划与 Java 执行 |
|---|---|---|
| 基础增删改查、表/列别名、UPDATE/DELETE 别名 | 支持 | 支持 |
| JOIN、INNER JOIN、自连接、纯 GROUP BY、列/别名 ORDER BY | 支持 | 支持 |
| COUNT/SUM/AVG/MIN/MAX 顶层 SELECT 项（可带括号） | 支持 | 支持 |
| <>、BETWEEN/NOT BETWEEN、字面量 IN/NOT IN | 支持 | 复用已有比较与逻辑表达式；受既有类型和 NULL 限制 |
| IS NULL / IS NOT NULL | 支持 | 支持，任意类型及 NULL 字面量均返回 BOOL |
| DISTINCT、HAVING、LIMIT/OFFSET、LEFT/RIGHT/FULL JOIN | 支持 | 支持 |
| SELECT 计算表达式、聚合结果算术、ORDER BY 表达式 | 支持 | 支持 |
| LIKE/NOT LIKE、VARCHAR(n)、列级 PRIMARY KEY/NOT NULL/UNIQUE/DEFAULT | 支持 | 支持 |
| 多行 INSERT、DROP TABLE（含 IF EXISTS 和多表名） | 支持 | 支持 |
| CREATE INDEX、DROP INDEX、DROP INDEX IF EXISTS | 支持 | 支持编译器侧计划和 JSON；执行层需实现索引元数据变更 |
| CREATE IF NOT EXISTS、表级复合 PRIMARY KEY/UNIQUE | 支持 | 支持，Catalog 与写入约束生效 |
| ALTER TABLE ADD/DROP/RENAME | 支持 | 支持，保留表/行 ID 并迁移已有记录 |
| IN/NOT IN、EXISTS/NOT EXISTS、标量子查询 | 支持 | 支持，含关联子查询和 SQL 三值逻辑 |
| FROM/JOIN 派生表 | 支持 | 支持，子查询输出重新绑定为别名关系 |
| UNION/INTERSECT/EXCEPT 及 ALL | 支持 | 支持，ALL 按重复次数执行 |
| 搜索型与简单型 CASE | 支持 | 支持，按 WHEN 顺序短路求值 |
| EXPLAIN / EXPLAIN ANALYZE | 支持包裹六类基础语句 | 展示优化后计划；ANALYZE 额外执行并采样 |
| 谓词下推、空结果传播与列裁剪 | 不改变 SQL 文法 | B 改写逻辑计划，Java 执行精简后的算子和扫描布局 |

B 把上述字段显式保存在 Bound 和 LogicalPlan 中；JSON 执行计划再把它们传给 Java。
因此 LIMIT 不会被忽略，外连接不会退化为内连接，多行 INSERT 和建表约束也不会丢失。

- INT 使用 int64_t，FLOAT 使用 double，VARCHAR 使用 std::string，BOOL 使用 bool。
  CREATE 至少一列，表名和列名不得重复，编译不修改 Catalog。
- INSERT 可一次绑定和执行多行，显式列允许重排。省略的列依次使用 DEFAULT、NULL；
  NOT NULL/PRIMARY KEY 列既无输入又无 DEFAULT 时报告 MissingInsertColumn。
  A 的 rows 保存所有行且 values 保存首行用于源码兼容；B 和执行层以 rows 为准并原子检查整批约束。
- SELECT 保留输出顺序和重复项。星号按 FROM、各 JOIN 表和表内模式顺序展开。
  含表达式或聚合的清单使用 vector<SelectItem>，别名平行保存在 column_aliases。
- JOIN 按 SQL 顺序构造左深连接树，ON 必须为 BOOL。INNER/LEFT/RIGHT/FULL 均保留连接类型，
  外连接未匹配侧按固定输入布局补 NULL。声明表别名后应使用别名限定列；
  自连接的不同扫描使用不同 relation_id。可见关系名不得重复，非限定列有歧义时报错。
- 无聚合函数时 GROUP BY 按键去重；有聚合时计算每组聚合值。普通投影列必须属于分组键，
  分组键不能重复，两个 NULL 键归为同组。无 GROUP BY 的聚合查询不能混入普通列。
- COUNT(*) 计入全部行，COUNT(column) 忽略 NULL，均返回 INT。
  SUM 接受 INT/FLOAT 并保持类型；AVG 接受 INT/FLOAT 并返回 FLOAT；MIN/MAX 支持四种列类型并保持类型。
  带参数聚合均忽略 NULL。空输入或全 NULL 参数的 COUNT 为 0，其余聚合为 NULL。
  全表空输入聚合仍输出一行，有 GROUP BY 的空输入输出零行。
- SUM(INT) 检查 64 位溢出；FLOAT 累加产生非有限值时报 FloatOverflow。
  AVG 使用 double 累加再除以非 NULL 数量，有浮点舍入误差，累加溢出也报 FloatOverflow。
- HAVING 在分组形成后求值，可包含聚合表达式；结果只有 TRUE 的分组被保留。
  SELECT 和 ORDER BY 可以把聚合调用作为表达式叶节点，例如 `SUM(amount)+1`。
- ORDER BY 允许隐藏源列、计算表达式；聚合/分组查询中聚合外的列须是分组键，
  亦可引用唯一输出别名或直接使用 `ORDER BY SUM(amount)`。
  多键按书写顺序比较，ASC NULL 最后，DESC NULL 最前，同值行最终顺序未定义。
  INT/FLOAT 按数值比较，VARCHAR 按 UTF-8 字节，BOOL 为 FALSE 小于 TRUE。
  WHERE、ON、GROUP BY 不能引用 SELECT 输出别名。
- UPDATE 的所有 RHS 读取同一条更新前记录，目标列不得重复。UPDATE/DELETE 的表别名
  替代物理表名参与限定列解析，行标识与存储身份保持不变；省略 WHERE 影响全部行。
- INT/FLOAT 支持同类型算术与比较，不做隐式转换；VARCHAR/BOOL 支持等于和不等于。
  BETWEEN 展开为比较与 AND，IN 展开为等值与 OR，NOT 版本再包裹 NOT。
- LIKE 的两个操作数必须是 VARCHAR；`%` 匹配任意 Unicode 码点序列，`_` 匹配一个码点，
  当前文法没有 ESCAPE 子句。NOT LIKE 由 NOT 包裹 LIKE 实现。
- 执行表达式用 Java null 表示 SQL UNKNOWN：普通算术、比较和 LIKE 任一操作数为 NULL 时返回 NULL，
  AND/OR/NOT 使用三值逻辑，WHERE/ON/HAVING 只保留 TRUE。NULL 字面量在编译期仍不能直接参与
  需要确定同类型操作数的普通运算；检查空值使用 IS NULL/IS NOT NULL。
- DISTINCT 在最终投影后去重，两个相同位置的 NULL 视为相等；随后应用 OFFSET/LIMIT。
- VARCHAR(n) 按 Unicode 码点数限制。PRIMARY KEY 等价于 NOT NULL + UNIQUE，一张表只允许一个；
  表级 PRIMARY KEY/UNIQUE 可以包含多列；复合主键成员均为 NOT NULL，复合 UNIQUE 的任一成员
  为 NULL 时允许重复。INSERT 与 UPDATE 在写入前针对最终表状态检查，失败不留下部分写入。
- CREATE TABLE IF NOT EXISTS 在表已存在时不改变模式和 CatalogVersion。ALTER ADD 为已有行写入
  DEFAULT 或 NULL，并重新检查约束；DROP 不允许删除最后一列或表级约束成员；RENAME 保留表 ID、
  列 ID 和已有 RowId。每个成功改变模式的 ALTER 使 CatalogVersion 增加一次。
- 第一版索引只支持 `CREATE INDEX index_name ON table_name(column_name)`，不支持
  `CREATE UNIQUE INDEX`、复合索引、非唯一索引或非 INT 键。B 要求目标列为 INT 且
  NOT NULL 或 PRIMARY KEY；导出计划固定 `keyType=INT`、`unique=true`。索引名在全库唯一。
  同一 SQL 脚本中，导出器按 `max(existing indexId)+1` 预测新 indexId，并在 CREATE INDEX
  计划导出后模拟索引可见性；Java 端必须采用同样规则才能让随后 SELECT 稳定引用该索引。
  DROP INDEX 无 IF EXISTS 时要求索引存在；带 IF EXISTS 时缺失索引导出 no-op DropIndex。
- IN 子查询和标量子查询必须返回一列，集合运算两侧必须列数及逐列类型相同。标量子查询零行
  返回 NULL，多于一行报告 ScalarSubqueryCardinality。EXISTS 只检查是否有行；NOT EXISTS 不受
  输出 NULL 影响。IN 无匹配但发生 NULL 比较时返回 UNKNOWN，空子查询返回 FALSE。
- 关联子查询可以引用所有可见外层关系；内层同名列优先，限定名可显式访问外层别名。派生表
  当前不是 LATERAL，不能引用外层关系，而且必须有别名，其输出列名在派生关系内必须唯一。
- UNION 默认去重、UNION ALL 保留两侧全部行；INTERSECT/EXCEPT 默认集合语义，ALL 版本按
  两侧重复计数求最小值或做减法。集合比较把同位置 NULL 视为相等，最终列名取左分支。
- 搜索型 CASE 的 WHEN 必须为 BOOL；简单型 CASE 的操作数与 WHEN 值类型一致；所有 THEN/ELSE
  非 NULL 结果类型必须一致。省略 ELSE 等价于 NULL，按书写顺序求值且只执行命中的结果表达式。
- DROP TABLE 可带多个名字。无 IF EXISTS 时先验证全部表再删除；IF EXISTS 忽略缺失表。
  至少删除一张表时 CatalogVersion 只增加一次，存储中的记录随表删除。
  DROP INDEX 仅接受单个索引名；成功删除一个索引时 CatalogVersion 增加一次。
- EXPLAIN 将一条基础语句绑定并优化后包装为 ExplainPlan，返回单列 `QUERY PLAN`
  文本树，不执行目标语句。EXPLAIN ANALYZE 执行相同的目标计划，并为每个算子
  输出累计实际行数、包含子算子的耗时和调用次数。它对 INSERT/UPDATE/DELETE/CREATE/DROP
  具有真实副作用；执行失败时返回原执行错误。EXPLAIN 不能嵌套 EXPLAIN。
- WHERE/ON 必须为 BOOL；AND/OR 从左到右短路。优化不得吞掉可达的除零、溢出或其源码位置。
  整数除法向零截断。聚合 DISTINCT、COUNT(1)、聚合参数算术和嵌套调用仍不在 A 的文法中。
- 优化器把 WHERE 的 AND 合取项按关系实例拆分。INNER JOIN 可下推左右两侧条件；LEFT 只下推
  左侧，RIGHT 只下推右侧，FULL 不下推。跨关系条件、常量条件和不满足安全条件的表达式保留原位。
  含算术/取负的 ON 或合取项视为可能产生运行期错误，不能通过改写改变其原有求值可达性。
- 列裁剪从最终投影反向加入 Filter、JOIN、GROUP/HAVING、ORDER BY 和表达式依赖；SeqScan
  按原表模式顺序只物化这些列。UPDATE 因整行写回和约束检查保留全列，RowId 独立于业务列；
  DELETE 只保留条件列，COUNT(*) 可使用零业务列扫描。
- 索引选择发生在谓词下推和空结果传播之后、列裁剪之前。当前仅在单表 `Filter -> SeqScan`
  中，从 AND 合取项按书写顺序选择第一个可用的单列 INT 索引范围；支持 `=`、`<`、`<=`、`>`、
  `>=` 以及 Parser 展开的 BETWEEN。原 Filter 始终保留在 IndexScan 上方用于完整谓词校验。
  `<>`、OR、NULL 比较、列列比较、表达式比较、JOIN 条件、派生表和子查询不会触发 IndexScan。
- 恒假 Filter 和恒假 INNER JOIN 在其输入不会产生副作用或运行期错误时改写为 EmptyResult。
  INNER 任一侧为空、LEFT 左侧为空、RIGHT 右侧为空、FULL 两侧均为空时可以继续传播；另一侧
  可能报错时保持原算子。Project、全局 Aggregate、UPDATE/DELETE 和 Explain 根边界不会删除，
  从而保留查询列名、空输入 COUNT(*)、修改影响行数和计划展示语义。
  `LIMIT 0` 的普通查询在输入及投影表达式均不会报错时同样跳过输入。

实现限制：语义分析接受的表达式单条路径最多 256 个 AST 节点（根计为第 1 层），
超过时报告 Semantic / ExpressionTooDeep，避免递归耗尽调用栈。这是资源限制，
不改变 EBNF 的优先级或结合性；缺失必需子节点的外部 AST 报 InvalidAst。
Parser 在构造 AST 时执行同一高度限制，另限制括号/NOT/负号递归嵌套不超过 256 层；
解析阶段超限报告 Syntax / ExpressionTooDeep，正常 SQL 输入会先在该阶段被拒绝。

## 4. 联调用例

可完整执行的示例：

```sql
CREATE TABLE student(id INT PRIMARY KEY, name VARCHAR(20) UNIQUE, age INT DEFAULT 18);
INSERT INTO student(id, name) VALUES (1, 'Alice'), (2, 'Bob');
UPDATE student s SET s.age=s.age+1 WHERE s.id=1;
SELECT name FROM student WHERE age IS NULL;
SELECT (COUNT(*)) AS rows, SUM(age) AS total FROM student;
SELECT age, COUNT(*) AS rows, SUM(id)+1 FROM student
  GROUP BY age HAVING COUNT(*)>0 ORDER BY SUM(id) DESC LIMIT 10;
SELECT DISTINCT s.name FROM student s LEFT JOIN student t ON s.id=t.id
  WHERE s.name LIKE 'A%' ORDER BY s.age+1;
SELECT s.name FROM student s
  WHERE EXISTS (SELECT * FROM score x WHERE x.student_id=s.id);
SELECT d.name FROM (SELECT name, age FROM student WHERE age>=18) d WHERE d.age<30;
SELECT id FROM student UNION ALL SELECT student_id FROM score;
SELECT CASE WHEN age>=18 THEN 'adult' ELSE 'minor' END FROM student;
ALTER TABLE student ADD COLUMN nickname VARCHAR(20) DEFAULT 'unknown';
EXPLAIN SELECT name FROM student WHERE age >= 18 ORDER BY id;
EXPLAIN ANALYZE SELECT age, COUNT(*) FROM student GROUP BY age;
DELETE FROM student s WHERE s.id <> 1;
DROP TABLE IF EXISTS student;
```

当前文法之外的示例：

```sql
SELECT COUNT(DISTINCT age) FROM student; -- 聚合函数内部 DISTINCT 尚未定义
INSERT INTO student VALUES (DEFAULT, 'Carol', 20); -- 值位置 DEFAULT 尚未定义
SELECT * FROM student OFFSET 2; -- OFFSET 当前必须跟在 LIMIT 后
```

预期结构见 `docs/interfaces.md` 和 `examples/contracts.cpp`。
新增语法必须同步修改本文、共享类型、语义规则和相应用例，再通知 A 与执行层。

## 5. 变更记录

- 0.1：约定五类语句、整数算术、完整 INSERT 列覆盖、更新前值赋值语义。
- 0.2：新增 ORDER BY、BOOL/FLOAT、TRUE/FALSE 和 NULL。
- 0.3：新增 GROUP BY。
- 0.4：新增 JOIN/ON 和限定列名；合入时保留 EOF、位置和深度防护。
- 0.5：B 完成多表名称绑定、NestedLoopJoin、无聚合 GroupBy 和 Sort 计划契约。
- 0.6：增加显式/隐式表别名和列别名，以 relationId 区分自连接实例，并允许 ORDER BY 输出别名。
- 0.7：新增 `<>`、`VARCHAR(n)`、`IS NULL` / `IS NOT NULL`、`LIMIT/OFFSET`、`LIKE` 的 A 侧语法与 AST 表示；B 侧语义和执行适配后续完成。
- 0.8：新增 `COUNT/SUM/AVG/MIN/MAX` 聚合函数的 A 侧 SELECT 项语法与 AST 表示；B 侧绑定、聚合计划和执行适配后续完成。
- 0.9：新增 `HAVING` 子句的 A 侧语法与 AST 字段；B 侧分组后过滤、聚合表达式绑定和执行适配后续完成。
- 0.10：新增 `INNER/LEFT/RIGHT/FULL [OUTER] JOIN` 的 A 侧语法与 JoinType AST 表示；B 侧外连接语义和执行适配后续完成。
- 0.11：新增 `SELECT DISTINCT` 的 A 侧语法与 AST 标记；B 侧去重计划和执行适配后续完成。
- 0.12：新增 `NOT LIKE` 的 A 侧语法；AST 复用 `UnaryOp::Not` 包裹 `BinaryOp::Like`，不新增公共操作符。
- 0.13：新增 `BETWEEN/NOT BETWEEN` 的 A 侧语法；AST 展开为已有比较、AND 和 NOT 表达式，不新增公共操作符。
- 0.14：新增字面量列表版 `IN/NOT IN` 的 A 侧语法；AST 展开为已有等值比较、OR 和 NOT 表达式，不新增公共操作符。
- 0.15：新增 SELECT 表达式项的 A 侧语法与 AST 表示；B 侧适配详见 `docs/select_expression_items_B_adaptation.md`。
- 0.16：新增列级 `PRIMARY KEY`、`NOT NULL`、`UNIQUE`、`DEFAULT literal` 的 A 侧语法与 AST 字段；B 侧 Catalog 和执行语义后续适配。
- 0.17：新增聚合调用作为普通表达式节点的 A 侧表示；B 当前显式返回 UnsupportedFeature，完整聚合绑定和计划后续适配。
- 0.18：新增 ORDER BY 表达式的 A 侧语法与 AST 字段；B 当前显式返回 UnsupportedFeature，表达式排序计划后续适配。
- 0.19：新增 INSERT 多行 VALUES 的 A 侧语法与 AST 字段；B 当前显式返回 UnsupportedFeature，多行插入绑定、计划和执行后续适配。
- 0.20：新增 DROP TABLE / DROP TABLE IF EXISTS / 多表名 DROP 的 A 侧语法与 AST 字段；B 当前显式返回 UnsupportedFeature，Catalog 删除语义后续适配。
- 0.21：新增 UPDATE/DELETE 目标表别名的 A 侧语法与 AST 字段；B 侧名称绑定已按单表别名作用域适配。
- 优化进度：A 提供展示用 AST 优化；B 提供绑定后计划优化，两者接口分离。

- 0.22：整合 A 0.7–0.21 与 B 聚合分支（原 B 文法 0.7）。统一新 SelectItem/聚合 AST，保留聚合执行，接通 IS NULL 与 DML 别名，显式拒绝仅解析扩展。
- 0.23：B 完成 A 侧剩余扩展：计算/聚合表达式、表达式排序、HAVING、DISTINCT、LIMIT/OFFSET、
  LIKE、外连接、多行 INSERT、DROP TABLE，以及 VARCHAR 长度和列约束；同步扩展 JSON 与 Java 执行层。
- 0.24：新增 EXPLAIN/EXPLAIN ANALYZE。AST、Bound 和 Plan 使用显式包装节点；Java 按真实
  算子调用路径采集 actual rows/time/loops，并明确 ANALYZE 修改类语句的副作用。
- 0.25：B 新增保持外连接与运行期错误语义的谓词下推，以及覆盖投影、条件、连接、分组、
  聚合和排序依赖的列裁剪；SeqScan 精确列集合贯通 JSON、Java 执行和 EXPLAIN 展示。
- 0.26：新增 EmptyResult 逻辑算子和安全空结果传播。空节点保留列身份及关系来源，支持外连接
  NULL 扩展；Java 可执行并在 EXPLAIN ANALYZE 中显示零行且不访问被消除的扫描。
- 0.27：合并 A 的 CREATE IF NOT EXISTS、表级约束、ALTER、子查询、派生表、三类集合运算和
  CASE；B 完成名称与类型绑定、计划节点、JSON 协议、Java 执行、模式迁移和端到端回归。
- 0.28：新增编译器侧 CREATE/DROP INDEX、Catalog 索引快照、CreateIndex/DropIndex/IndexScan
  计划节点和 JSON 导出；第一版仅支持单列唯一 NOT NULL INT 索引。
