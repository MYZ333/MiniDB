# MiniSQL 文法（接口版本 0.22）

本文由 B 维护，供 A 的 Lexer/Parser、B 的语义分析以及执行层共同使用。
已整合 feature-zhangbo 的语法扩展与 B 的聚合实现。下列 EBNF 描述 A 能解析的范围，
第 3 节区分完整执行支持和仅解析支持，调用方应以第 3 节作为可执行 SQL 的依据。

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
- 支持 TRUE/FALSE、NULL、BOOL/FLOAT、`VARCHAR(n)`、列级 PRIMARY KEY/NOT NULL/UNIQUE/DEFAULT、INSERT 多行 VALUES、DROP TABLE、JOIN/ON、INNER/LEFT/RIGHT/FULL OUTER JOIN、SELECT 表达式项、SELECT DISTINCT、GROUP BY、HAVING、ORDER BY 表达式 ASC/DESC、LIMIT/OFFSET、AS、IS、LIKE/NOT LIKE、BETWEEN/NOT BETWEEN、IN/NOT IN 和 COUNT/SUM/AVG/MIN/MAX 关键字。
- INSERT 值位置不支持 `DEFAULT` 关键字。DROP TABLE、外连接和多行 INSERT 仅支持解析，B 返回 UnsupportedFeature。
- COUNT/SUM/AVG/MIN/MAX 由 A 识别为关键字，不能再作为未加引号的普通表名、列名或别名。
- 每条语句必须以分号结束；空输入合法；单独的空分号不是语句。
- 源码位置：字节偏移从 0 开始，行列从 1 开始；列也按字节计算。
  LF、单独 CR 换行，CRLF 作为一个换行，制表符占一列；范围为左闭右开。

## 2. EBNF

大写单词为关键字，双引号为终结符，`[ ]` 表示可选，`{ }` 表示重复。

```ebnf
program     = { statement } ;
statement   = (create | drop | insert | select | update | delete), ";" ;
create      = CREATE, TABLE, name, "(", column_def,
              { ",", column_def }, ")" ;
column_def  = name, type, { column_constraint } ;
column_constraint = PRIMARY, KEY | NOT, NULL | UNIQUE | DEFAULT, literal ;
type        = INT | VARCHAR, ["(", INTEGER, ")"] | BOOL | FLOAT ;
drop        = DROP, TABLE, [IF, EXISTS], names ;
insert      = INSERT, INTO, name, [ "(", names, ")" ],
              VALUES, value_row, { ",", value_row } ;
value_row   = "(", literal, { ",", literal }, ")" ;
select      = SELECT, [DISTINCT], ("*" | select_items), FROM, table_ref,
              { join }, [where], [group_by], [having], [order_by], [limit] ;
update      = UPDATE, table_ref, SET, assignment, { ",", assignment }, [where] ;
delete      = DELETE, FROM, table_ref, [where] ;
assignment  = name, "=", expr ;
join        = [join_type], JOIN, table_ref, ON, expr ;
join_type   = INNER | LEFT, [OUTER] | RIGHT, [OUTER] | FULL, [OUTER] ;
table_ref   = name, [ alias ] ;
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
                          (IS, [NOT], NULL) ] ;
comp_op     = "=" | "!=" | "<>" | "<" | "<=" | ">" | ">=" ;
additive    = term, { ("+" | "-"), term } ;
term        = unary, { ("*" | "/"), unary } ;
unary       = "-", unary | primary ;
primary     = aggregate_call | name | INTEGER | FLOAT_LITERAL | STRING | TRUE | FALSE | NULL | "(", expr, ")" ;
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

递归下降的 statement 分支分别以 CREATE / DROP / INSERT / SELECT / UPDATE /
DELETE 开始；where 的 FIRST 为 WHERE；SELECT 中其后允许 GROUP/HAVING/ORDER/LIMIT 或分号。not_expr 的 FIRST
包括 NOT、负号、标识符、整数、浮点数、字符串、TRUE/FALSE、NULL 和左括号。
SELECT 子句顺序固定为 JOIN → WHERE → GROUP BY → HAVING → ORDER BY → LIMIT/OFFSET。

## 3. 语义限制与执行支持

| 功能 | A 解析 | B 绑定、计划与 Java 执行 |
|---|---|---|
| 基础增删改查、表/列别名、UPDATE/DELETE 别名 | 支持 | 支持 |
| JOIN、INNER JOIN、自连接、纯 GROUP BY、列/别名 ORDER BY | 支持 | 支持 |
| COUNT/SUM/AVG/MIN/MAX 顶层 SELECT 项（可带括号） | 支持 | 支持 |
| <>、BETWEEN/NOT BETWEEN、字面量 IN/NOT IN | 支持 | 复用已有比较与逻辑表达式；受既有类型和 NULL 限制 |
| IS NULL / IS NOT NULL | 支持 | 支持，任意类型及 NULL 字面量均返回 BOOL |
| DISTINCT、HAVING、LIMIT/OFFSET、外连接 | 支持 | UnsupportedFeature |
| SELECT 计算表达式、聚合结果算术、ORDER BY 表达式 | 支持 | UnsupportedFeature；SELECT 括号包裹单列/聚合除外 |
| LIKE/NOT LIKE、VARCHAR(n)、列级约束和 DEFAULT | 支持 | UnsupportedFeature |
| 多行 INSERT、DROP TABLE | 支持 | UnsupportedFeature |

B 对未支持的标记在生成计划前明确拒绝，不会忽略 LIMIT、把外连接当内连接执行，
也不会只插入多行 INSERT 的第一行或丢弃建表约束。零值 LIMIT/OFFSET 同样检查。

- INT 使用 int64_t，FLOAT 使用 double，VARCHAR 使用 std::string，BOOL 使用 bool。
  CREATE 至少一列，表名和列名不得重复，编译不修改 Catalog。
- INSERT 当前执行单行字面量，必须覆盖所有列；允许重排列顺序，NULL 可写入任意列。
  A 的 rows 保存所有行且 values 保存首行用于源码兼容；B 优先读取 rows，超过一行拒绝。
- SELECT 保留输出顺序和重复项。星号按 FROM、各 JOIN 表和表内模式顺序展开。
  含表达式或聚合的清单使用 vector<SelectItem>，别名平行保存在 column_aliases。
- JOIN 按 SQL 顺序构造左深内连接树，ON 必须为 BOOL。声明表别名后应使用别名限定列；
  自连接的不同扫描使用不同 relation_id。可见关系名不得重复，非限定列有歧义时报错。
- 无聚合函数时 GROUP BY 按键去重；有聚合时计算每组聚合值。普通投影列必须属于分组键，
  分组键不能重复，两个 NULL 键归为同组。无 GROUP BY 的聚合查询不能混入普通列。
- COUNT(*) 计入全部行，COUNT(column) 忽略 NULL，均返回 INT。
  SUM 接受 INT/FLOAT 并保持类型；AVG 接受 INT/FLOAT 并返回 FLOAT；MIN/MAX 支持四种列类型并保持类型。
  带参数聚合均忽略 NULL。空输入或全 NULL 参数的 COUNT 为 0，其余聚合为 NULL。
  全表空输入聚合仍输出一行，有 GROUP BY 的空输入输出零行。
- SUM(INT) 检查 64 位溢出；FLOAT 累加产生非有限值时报 FloatOverflow。
  AVG 使用 double 累加再除以非 NULL 数量，有浮点舍入误差，累加溢出也报 FloatOverflow。
- ORDER BY 允许隐藏源列；聚合/分组查询中隐藏列须是分组键，亦可引用唯一输出别名。
  聚合结果请使用 SELECT 别名排序，直接 ORDER BY SUM(amount) 当前仍返回 UnsupportedFeature。
  多键按书写顺序比较，ASC NULL 最后，DESC NULL 最前，同值行最终顺序未定义。
  INT/FLOAT 按数值比较，VARCHAR 按 UTF-8 字节，BOOL 为 FALSE 小于 TRUE。
  WHERE、ON、GROUP BY 不能引用 SELECT 输出别名。
- UPDATE 的所有 RHS 读取同一条更新前记录，目标列不得重复。UPDATE/DELETE 的表别名
  替代物理表名参与限定列解析，行标识与存储身份保持不变；省略 WHERE 影响全部行。
- INT/FLOAT 支持同类型算术与比较，不做隐式转换；VARCHAR/BOOL 支持等于和不等于。
  BETWEEN 展开为比较与 AND，IN 展开为等值与 OR，NOT 版本再包裹 NOT。
  NULL 三值逻辑尚未实现：NULL 字面量参与普通运算会被拒绝，nullable 列参与普通比较
  仍遵循旧执行行为；不能把 IN/NOT IN 在 NULL 上解释为标准三值逻辑。
  检查空值请使用 IS NULL/IS NOT NULL；这两个运算不会把 NULL 转为数字或布尔值。
- WHERE/ON 必须为 BOOL；AND/OR 从左到右短路。优化不得吞掉可达的除零、溢出或其源码位置。
  整数除法向零截断。聚合 DISTINCT、COUNT(1)、聚合参数算术和嵌套调用仍不在 A 的文法中。

实现限制：语义分析接受的表达式单条路径最多 256 个 AST 节点（根计为第 1 层），
超过时报告 Semantic / ExpressionTooDeep，避免递归耗尽调用栈。这是资源限制，
不改变 EBNF 的优先级或结合性；缺失必需子节点的外部 AST 报 InvalidAst。
Parser 在构造 AST 时执行同一高度限制，另限制括号/NOT/负号递归嵌套不超过 256 层；
解析阶段超限报告 Syntax / ExpressionTooDeep，正常 SQL 输入会先在该阶段被拒绝。

## 4. 联调用例

可完整执行的示例：

```sql
CREATE TABLE student(id INT, name VARCHAR, age INT);
INSERT INTO student VALUES (1, 'Alice', 20);
INSERT INTO student VALUES (2, 'Bob', NULL);
UPDATE student s SET s.age=s.age+1 WHERE s.id=1;
SELECT name FROM student WHERE age IS NULL;
SELECT (COUNT(*)) AS rows, SUM(age) AS total FROM student;
SELECT age, COUNT(*) AS rows FROM student GROUP BY age ORDER BY rows DESC;
SELECT s.name FROM student s INNER JOIN student t ON s.id=t.id WHERE s.id IN (1,2);
DELETE FROM student s WHERE s.id <> 1;
```

下列可解析，但编译执行时明确返回 UnsupportedFeature：

```sql
SELECT DISTINCT age FROM student;
SELECT COUNT(*) FROM student HAVING COUNT(*) > 1;
SELECT * FROM student LIMIT 0;
SELECT age+1 FROM student;
CREATE TABLE account(id INT PRIMARY KEY, name VARCHAR(20));
INSERT INTO student VALUES (3,'c',20),(4,'d',21);
DROP TABLE IF EXISTS student;
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
