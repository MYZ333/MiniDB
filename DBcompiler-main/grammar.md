# MiniSQL 文法（接口版本 0.21）

本文由 B 维护，供 A 的 Lexer/Parser、B 的语义分析以及执行层共同使用。
五类基础语句及 A version2 扩展语法已合入。B 已支持扩展标量类型、限定名、
内连接、无聚合分组、多列排序、表/列别名；执行层已接入这些查询算子。

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
- INSERT 值位置不支持 `DEFAULT` 关键字。DROP TABLE、聚合函数、外连接和多行 INSERT 目前由 A 解析为 AST，B 的完整语义、计划和执行适配后续完成。
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
DELETE 开始；where 的 FIRST 为 WHERE，FOLLOW 为分号；not_expr 的 FIRST
包括 NOT、负号、标识符、整数、浮点数、字符串、TRUE/FALSE、NULL 和左括号。
SELECT 子句顺序固定为 JOIN → WHERE → GROUP BY → HAVING → ORDER BY → LIMIT/OFFSET。

## 3. 语义限制

- INT 使用 int64_t，FLOAT 使用 double，VARCHAR 使用 std::string，BOOL 使用 bool。
  `VARCHAR(n)` 的 n 必须是正整数；A 在 AST 中保留长度，长度约束由 B/执行层后续决定。
- 列级 PRIMARY KEY、NOT NULL、UNIQUE 和 DEFAULT 由 A 保存在 ColumnDefinition 中；
  A 不检查默认值类型、不维护索引、不修改 Catalog，具体约束语义由 B/Catalog/执行层决定。
- CREATE 至少一列；表名和列名不得重复；CREATE 编译不修改 Catalog。
- A 支持 DROP TABLE，可选 `IF EXISTS`，以及逗号分隔的多个表名。B 适配前，
  DROP TABLE 在语义阶段显式报 UnsupportedFeature，不会修改 Catalog。
- A 支持 INSERT 单行和多行字面量；`InsertStmt::values` 保留第一行用于兼容旧接口，
  `InsertStmt::rows` 保存全部行。B 适配前，多行 INSERT 在语义阶段显式报 UnsupportedFeature。
  INSERT 必须提供全部列。允许重排列顺序；省略列清单时按表顺序。
- SELECT 可选择列、表达式、聚合项或 `*`，保留显式项目顺序和重复项；选择项可声明输出别名。`*` 按
  FROM 表、随后各 JOIN 表的 SQL 顺序展开，并在每张表内保持模式列顺序。
  为兼容 B，普通列清单仍保留为旧的 `std::vector<Identifier>` AST 分支；出现表达式项或聚合项时使用 `std::vector<SelectItem>`。
- SELECT DISTINCT 由 A 保存为 `SelectStmt::distinct`。B 适配前，该标记只保证
  Parser/AST 输出正确；去重计划、NULL 比较和排序稳定性由 B 后续定义。
- A 支持记录 INNER/LEFT/RIGHT/FULL 连接类型；B 适配前，外连接只保证 Parser/AST 输出正确。
  JOIN 按书写顺序构造左深树；ON 可引用当前已加入的所有关系实例且
  必须为 BOOL。表声明别名后，限定列必须使用别名；不同别名允许同一物理表自连接。
  可见关系名不得重复，未限定列名命中多个关系实例时报 AmbiguousColumn。
- A 支持聚合调用表达式：`COUNT(*)`、`COUNT(column)`、`SUM(column)`、
  `AVG(column)`、`MIN(column)`、`MAX(column)`。除 `COUNT(*)` 外，`*` 不能作为聚合参数。
  聚合调用可出现在 SELECT 表达式项和 HAVING 等表达式位置。B 完整适配前，
  这些查询只保证 Lexer/Parser/AST 输出正确，不保证语义分析、计划生成或执行。
- GROUP BY 在 B 完成聚合适配前仍按无聚合分组处理，含义为按键去重。每个非聚合 SELECT 列都
  必须出现在分组键中，分组键不得重复；`SELECT *` 也受同一规则约束。两个 NULL 键归入同一组。
- HAVING 使用现有表达式文法，A 将其保存在 `SelectStmt::having` 中。B 适配前，
  HAVING 只保证 Parser/AST 输出正确；聚合函数作为 HAVING 表达式参数的形式后续再扩展。
- ORDER BY 可包含未出现在 SELECT 中的隐藏列或表达式，默认 ASC，支持显式 ASC/DESC，并按项目
  顺序确定同值行的后续排序键。ASC 的 NULL 在最后，DESC 的 NULL 在最前；分组查询中的
  排序列必须属于分组键。ORDER BY 可以引用唯一的 SELECT 输出别名；WHERE、JOIN ON 和
  GROUP BY 不可引用输出别名。B 完整适配前，ORDER BY 表达式会在语义阶段显式报 UnsupportedFeature。
- UPDATE 目标列不得重复；全部右侧表达式读取同一条更新前记录。
- UPDATE/DELETE 省略 WHERE 时影响全部行；WHERE 必须为 BOOL。
- UPDATE/DELETE 支持目标表别名。声明别名后，限定列名应使用别名；
  物理表名不再作为限定符参与该语句的列解析。未声明别名时仍可使用物理表名限定列。
- INT/FLOAT 分别支持同类型加减乘除、负号、全部比较和 BETWEEN/NOT BETWEEN；VARCHAR 支持等于/不等于、LIKE 和 NOT LIKE；
  BOOL 支持等于、不等于、AND/OR/NOT。不做 INT/FLOAT 隐式转换。
- INSERT 的 NULL 可写入任意列；`IS NULL` / `IS NOT NULL` 返回 BOOL。
  普通表达式中的 NULL 暂无三值逻辑，使用其他运算符会被语义阶段拒绝。
- 单表语句接受 `table.column`，限定符必须与目标表匹配；多表查询可用限定名消除歧义。
- 整数除法向零截断；除零和溢出由执行层报告运行时错误。
- 表达式先计算左侧；AND/OR 从左向右短路。规则优化必须保留可达错误及其位置，
  不会提前报告常量除零/溢出。TRUE/FALSE 现在也是合法 SQL 字面量。
- 表或列不存在、INSERT 数量/覆盖/类型错误、赋值类型错误属于语义错误。

实现限制：语义分析接受的表达式单条路径最多 256 个 AST 节点（根计为第 1 层），
超过时报告 Semantic / ExpressionTooDeep，避免递归耗尽调用栈。这是资源限制，
不改变 EBNF 的优先级或结合性；缺失必需子节点的外部 AST 报 InvalidAst。
Parser 在构造 AST 时执行同一高度限制，另限制括号/NOT/负号递归嵌套不超过 256 层；
解析阶段超限报告 Syntax / ExpressionTooDeep，正常 SQL 输入会先在该阶段被拒绝。

## 4. 联调用例

```sql
CREATE TABLE student(id INT, name VARCHAR, age INT);
CREATE TABLE account(id INT PRIMARY KEY, name VARCHAR(20) NOT NULL UNIQUE DEFAULT 'guest');
DROP TABLE IF EXISTS old_student;
INSERT INTO student(name, age, id) VALUES ('Alice', 20, 1);
INSERT INTO student(id, name, age) VALUES (1, 'Alice', 20), (2, 'Bob', 18);
SELECT name FROM student WHERE age > 18;
UPDATE student SET age = age + 1 WHERE id = 1;
DELETE FROM student WHERE id = 1;
UPDATE student AS s SET s.age = s.age + 1 WHERE s.id = 1;
DELETE FROM student s WHERE s.id = 1;
CREATE TABLE metrics(active BOOL, score FLOAT);
SELECT metrics.score FROM metrics WHERE metrics.active=TRUE AND metrics.score>60.5;
SELECT student.name FROM student
JOIN score ON student.id=score.student_id
WHERE score.value>60
GROUP BY student.name
ORDER BY student.name DESC;
SELECT student.name, score.value
FROM student LEFT OUTER JOIN score ON student.id=score.student_id;
SELECT e.name AS employee_name, m.name manager_name
FROM employee AS e JOIN employee m ON e.manager_id=m.id
ORDER BY employee_name;
SELECT * FROM student ORDER BY id DESC LIMIT 10 OFFSET 20;
SELECT name FROM metrics ORDER BY score + 1 DESC;
SELECT DISTINCT active FROM metrics;
SELECT age + 1 AS next_age, score * 1.1 AS adjusted_score FROM metrics;
SELECT active, COUNT(*), AVG(score) AS avg_score
FROM metrics
GROUP BY active
HAVING active = TRUE;
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
