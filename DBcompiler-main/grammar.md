# MiniSQL 文法（接口版本 0.7）

本文由 B 维护，供 A 的 Lexer/Parser、B 的语义分析以及执行层共同使用。
五类基础语句及 A version2 扩展语法已合入。B 已支持扩展标量类型、限定名、
内连接、分组、COUNT/SUM/AVG/MIN/MAX、多列排序、表/列别名；执行层已接入这些查询算子。

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
- 支持运算符 `= != < <= > >= + - * /`；不支持 `==`、`<>`。
- 支持 TRUE/FALSE、NULL、BOOL/FLOAT、JOIN/ON、GROUP BY、ORDER BY ASC/DESC 和 AS 关键字。
- 聚合函数名作为普通标识符交给 B 检查。暂不支持 DEFAULT、VARCHAR 长度参数和外连接。
- 每条语句必须以分号结束；空输入合法；单独的空分号不是语句。
- 源码位置：字节偏移从 0 开始，行列从 1 开始；列也按字节计算。
  LF、单独 CR 换行，CRLF 作为一个换行，制表符占一列；范围为左闭右开。

## 2. EBNF

大写单词为关键字，双引号为终结符，`[ ]` 表示可选，`{ }` 表示重复。

```ebnf
program     = { statement } ;
statement   = (create | insert | select | update | delete), ";" ;
create      = CREATE, TABLE, name, "(", column_def,
              { ",", column_def }, ")" ;
column_def  = name, (INT | VARCHAR | BOOL | FLOAT) ;
insert      = INSERT, INTO, name, [ "(", names, ")" ],
              VALUES, "(", literal, { ",", literal }, ")" ;
select      = SELECT, ("*" | select_items), FROM, table_ref,
              { join }, [where], [group_by], [order_by] ;
update      = UPDATE, name, SET, assignment, { ",", assignment }, [where] ;
delete      = DELETE, FROM, name, [where] ;
assignment  = name, "=", expr ;
join        = JOIN, table_ref, ON, expr ;
table_ref   = name, [ alias ] ;
select_items = select_item, { ",", select_item } ;
select_item = (name | aggregate_call), [ alias ] ;
aggregate_call = IDENTIFIER, "(", (name | "*"), ")" ;
alias       = [ AS ], IDENTIFIER ;
where       = WHERE, expr ;
group_by    = GROUP, BY, names ;
order_by    = ORDER, BY, order_item, { ",", order_item } ;
order_item  = name, [ASC | DESC] ;
names       = name, { ",", name } ;
expr        = or_expr ;
or_expr     = and_expr, { OR, and_expr } ;
and_expr    = not_expr, { AND, not_expr } ;
not_expr    = NOT, not_expr | comparison ;
comparison  = additive, [comp_op, additive] ;
comp_op     = "=" | "!=" | "<" | "<=" | ">" | ">=" ;
additive    = term, { ("+" | "-"), term } ;
term        = unary, { ("*" | "/"), unary } ;
unary       = "-", unary | primary ;
primary     = name | INTEGER | FLOAT_LITERAL | STRING | TRUE | FALSE | NULL | "(", expr, ")" ;
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

递归下降的 statement 分支分别以 CREATE / INSERT / SELECT / UPDATE /
DELETE 开始；where 的 FIRST 为 WHERE，FOLLOW 为分号；not_expr 的 FIRST
包括 NOT、负号、标识符、整数、浮点数、字符串、TRUE/FALSE、NULL 和左括号。
SELECT 子句顺序固定为 JOIN → WHERE → GROUP BY → ORDER BY。

## 3. 语义限制

- INT 使用 int64_t，FLOAT 使用 double，VARCHAR 使用 std::string，BOOL 使用 bool。
- CREATE 至少一列；表名和列名不得重复；CREATE 编译不修改 Catalog。
- INSERT 仅单行字面量，必须提供全部列。允许重排列顺序；省略列清单时按表顺序。
- SELECT 可选择列、聚合调用或单独的 `*`，保留显式列顺序和重复列；选择列可声明输出别名。`*` 按
  FROM 表、随后各 JOIN 表的 SQL 顺序展开，并在每张表内保持模式列顺序。
- JOIN 是带 ON 的内连接，按书写顺序构造左深树；ON 可引用当前已加入的所有关系实例且
  必须为 BOOL。表声明别名后，限定列必须使用别名；不同别名允许同一物理表自连接。
  可见关系名不得重复，未限定列名命中多个关系实例时报 AmbiguousColumn。
- GROUP BY 在没有聚合调用时按键去重。每个 SELECT 非聚合列都必须出现在分组键中，
  分组键不得重复；`SELECT *` 也受同一规则约束。两个 NULL 键归入同一组。
- ORDER BY 可包含未出现在 SELECT 中的隐藏列，默认 ASC，支持显式 ASC/DESC，并按项目
  顺序确定同值行的后续排序键。ASC 的 NULL 在最后，DESC 的 NULL 在最前；分组查询中的
  排序列必须属于分组键。ORDER BY 可以引用唯一的 SELECT 输出别名；WHERE、JOIN ON 和
  GROUP BY 不可引用输出别名。聚合查询也允许 ORDER BY 聚合结果别名或隐藏分组键；
  不支持在 ORDER BY 直接写函数调用或位置序号。
- 聚合只允许 SELECT 中的 `COUNT(*)`、`COUNT(name)`、`SUM(name)`、`AVG(name)`、
  `MIN(name)`、`MAX(name)`；函数名大小写不敏感，参数可以是带关系别名的限定列名。
  `COUNT` 返回 INT；`SUM` 接受 INT/FLOAT 并保持参数类型；`AVG` 接受 INT/FLOAT 并返回 FLOAT；
  `MIN/MAX` 接受四种列类型并保持类型，比较顺序与 ORDER BY 一致。
- 所有带列参数的聚合忽略 NULL；COUNT(*) 计入每行。空输入或全 NULL 参数的 COUNT 为 0，
  其余聚合为 NULL。无 GROUP BY 的全表聚合即使输入为空也输出一行；有 GROUP BY 时空输入输出零行。
  无 GROUP BY 的聚合查询不能混入普通列。SUM(INT) 检查 64 位溢出，FLOAT 累加非有限值报 FloatOverflow。
  AVG 使用 double 累加再除以非 NULL 数量，结果存在浮点舍入误差，累加溢出也报 FloatOverflow。
- 暂不支持 HAVING、聚合 DISTINCT、COUNT(1)、嵌套聚合、聚合参数算术和聚合结果算术。
  WHERE/ON 的 NULL 比较仍沿用现有表达式限制；聚合忽略 NULL 不代表实现了 SQL 三值逻辑。
- UPDATE 目标列不得重复；全部右侧表达式读取同一条更新前记录。
- UPDATE/DELETE 省略 WHERE 时影响全部行；WHERE 必须为 BOOL。
- INT/FLOAT 分别支持同类型加减乘除、负号和全部比较；VARCHAR 支持等于/不等于；
  BOOL 支持等于、不等于、AND/OR/NOT。不做 INT/FLOAT 隐式转换。
- INSERT 的 NULL 可写入任意列；普通表达式中的 NULL 暂无三值逻辑，使用运算符会被语义阶段拒绝。
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
INSERT INTO student(name, age, id) VALUES ('Alice', 20, 1);
SELECT name FROM student WHERE age > 18;
UPDATE student SET age = age + 1 WHERE id = 1;
DELETE FROM student WHERE id = 1;
CREATE TABLE metrics(active BOOL, score FLOAT);
SELECT metrics.score FROM metrics WHERE metrics.active=TRUE AND metrics.score>60.5;
SELECT student.name FROM student
JOIN score ON student.id=score.student_id
WHERE score.value>60
GROUP BY student.name
ORDER BY student.name DESC;
SELECT e.name AS employee_name, m.name manager_name
FROM employee AS e JOIN employee m ON e.manager_id=m.id
ORDER BY employee_name;
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
- 0.7：增加五类聚合调用、全表/分组聚合、NULL/空输入规则、类型检查及聚合结果别名排序。
- 优化进度：A 提供展示用 AST 优化；B 提供绑定后计划优化，两者接口分离。
