# MiniSQL 第一阶段文法（接口版本 0.1）

本文由 B 维护，供 A 的 Lexer/Parser、B 的语义分析以及执行层共同使用。
当前五类语句的词法、语法、语义和逻辑计划已合入并通过 SQL→Plan 联调；执行层仍待接入。

## 1. 词法约定

- 关键字和不带引号的标识符按 ASCII 大小写不敏感匹配；保留原文用于诊断。
- 标识符：`[A-Za-z_][A-Za-z0-9_]*`；不支持带引号名称、限定列名和别名。
- 整数 token：`[0-9]+`。负号是独立 token，值范围为有符号 64 位整数。
  Parser 将紧邻语法意义上的负号和整数字面量组合为负整数，允许
  `-9223372036854775808`；超出范围报告 Syntax / IntegerOutOfRange。
  负号与数字之间允许空白；`-column` 仍构造一元表达式。
- 字符串用单引号包围，`''` 解码为一个单引号；反斜杠没有特殊含义。
  第一阶段不允许字符串跨行。字符串值保持大小写和 UTF-8 字节内容。
- 跳过空白、`--` 行注释和不嵌套的 `/* ... */` 块注释。
- 支持运算符 `= != < <= > >= + - * /`；不支持 `==`、`<>`。
- 不支持浮点数、NULL、DEFAULT、SQL 布尔字面量、VARCHAR 长度参数。
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
column_def  = name, (INT | VARCHAR) ;
insert      = INSERT, INTO, name, [ "(", names, ")" ],
              VALUES, "(", literal, { ",", literal }, ")" ;
select      = SELECT, ("*" | names), FROM, name, [where] ;
update      = UPDATE, name, SET, assignment, { ",", assignment }, [where] ;
delete      = DELETE, FROM, name, [where] ;
assignment  = name, "=", expr ;
where       = WHERE, expr ;
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
primary     = name | INTEGER | STRING | "(", expr, ")" ;
literal     = ["-"], INTEGER | STRING ;
name        = IDENTIFIER ;
```

表达式中的负号统一由 `unary` 消费，INSERT 的负数由 `literal` 消费，避免
两个产生式争用负号。`unary` 的负号后直接遇到 INTEGER token 时构造带符号
LiteralExpr，并在应用符号后检查范围；遇到其他操作数时构造 UnaryExpr。
这样既保留一元运算节点，也能正确表示 INT64_MIN。

优先级从高到低：一元负号、乘除、加减、比较、NOT、AND、OR。
二元算术运算左结合；NOT 和一元负号右结合；比较不能连写。
例如 `NOT age > 18` 为 `NOT (age > 18)`，`a < b < c` 是语法错误。
这是对 PPT 第 16 页文法与优先级文字不一致的明确取舍。

递归下降的 statement 分支分别以 CREATE / INSERT / SELECT / UPDATE /
DELETE 开始；where 的 FIRST 为 WHERE，FOLLOW 为分号；not_expr 的 FIRST
包括 NOT、负号、标识符、整数、字符串和左括号。表达式用分层函数体现优先级。

## 3. 语义限制

- INT 使用 int64_t；VARCHAR 使用 std::string；BOOL 仅用于表达式结果。
- CREATE 至少一列；表名和列名不得重复；CREATE 编译不修改 Catalog。
- INSERT 仅单行字面量，必须提供全部列。允许重排列顺序；省略列清单时按表顺序。
- SELECT 仅选择列或 `*`，保留显式列顺序和重复列；不支持选择列表表达式。
- UPDATE 目标列不得重复；全部右侧表达式读取同一条更新前记录。
- UPDATE/DELETE 省略 WHERE 时影响全部行；WHERE 必须为 BOOL。
- INT 支持加减乘除、负号和全部比较；VARCHAR 仅支持等于和不等于；
  BOOL 仅支持 AND/OR/NOT。不做隐式类型转换。
- 整数除法向零截断；除零和溢出由执行层报告运行时错误。
- 表达式先计算左侧；AND/OR 从左向右短路。规则优化必须保留可达错误及其位置，
  不会提前报告常量除零/溢出。优化产生的内部 BOOL 常量不扩展 SQL 字面量语法。
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
```

预期结构见 `docs/interfaces.md` 和 `examples/contracts.cpp`。
新增语法必须同步修改本文、共享类型、语义规则和相应用例，再通知 A 与执行层。

## 5. 变更记录

- 0.1：约定五类语句、整数算术、完整 INSERT 列覆盖、更新前值赋值语义。
- 实现进度：完成五类语句语义、深度防护和计划生成；语言范围未扩展，文法版本仍为 0.1。
- A 合并进度：Lexer/Parser 实现接入；补充 Parser 的 EOF、位置和深度契约回归。
- 优化进度：增加安全常量折叠、布尔化简及恒真 Filter 消除；EBNF 和语言版本不变。
