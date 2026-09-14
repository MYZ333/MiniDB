# MiniSQL 语法扩展更新记录

> 本文保留 A 分支交付时的设计与历史记录。B 侧适配现已完成；当前状态以
> [grammar.md 0.23](../grammar.md) 和 [实现讲解](remaining-features-walkthrough.md) 为准。

本文档记录合并版 0.6 之后的语法扩展。后续每完成一项扩展，都只追加到这一份文档中。

## 2026-09-13：标准不等于操作符

### 新增内容

- 支持 SQL 标准写法 `<>`，语义等同于现有的不等于操作符 `!=`。
- Lexer 对 `!=` 和 `<>` 都输出现有的 `TokenKind::NotEqual`。
- Parser 无需新增逻辑，因为现有代码已经把 `TokenKind::NotEqual` 映射为 `BinaryOp::NotEqual`。

### 兼容性说明

- 未修改 AST。
- 未新增 TokenKind。
- 不需要 B 修改语义分析、计划生成、Catalog 或执行层。
- 原有 `!=` 行为不变。

### 验证

- 新增 Lexer 测试覆盖 `<>`。
- 新增 Parser 测试覆盖 `WHERE id <> 1`。

## 2026-09-13：VARCHAR 长度声明

### 新增内容

- `CREATE TABLE` 的列类型支持 `VARCHAR(n)`，其中 `n` 必须是正整数。
- 原有 `VARCHAR` 写法仍然合法，表示未声明长度。
- A 的 Parser 会把 `VARCHAR(n)` 的长度保存在 `ColumnDefinition::varchar_length` 中。
- A 的调试入口打印建表 AST 时会显示 `VARCHAR(n)`。

### 公共接口影响

- `ColumnDefinition` 新增 `varchar_length` 字段，用于把 A 解析出的长度交给 B。
- `ColumnSpec` 和 `ColumnSchema` 预留同名字段，方便 B 后续在语义分析、Catalog 和计划中保留长度。
- 新字段都有默认值，旧代码中只提供 `name` 和 `type` 的聚合初始化仍然可以继续编译。

### B 侧待适配

- B 后续需要在语义分析中从 `ColumnDefinition::varchar_length` 复制到 `ColumnSpec::varchar_length`。
- Catalog 后续需要从 `ColumnSpec::varchar_length` 复制到 `ColumnSchema::varchar_length`。
- 是否强制检查字符串实际长度由 B 和执行层决定；本次 A 只负责语法解析与 AST 保存。

### 验证

- 新增 Parser 测试覆盖 `VARCHAR(20)` 和普通 `VARCHAR`。
- 新增 Parser 错误测试覆盖 `VARCHAR()`、`VARCHAR(0)`、`VARCHAR(-1)`。

## 2026-09-13：IS NULL / IS NOT NULL

### 新增内容

- 支持表达式后缀 `IS NULL` 和 `IS NOT NULL`。
- Lexer 新增关键字 `IS`。
- Parser 将 `expr IS NULL` 构造为 `UnaryExpr{UnaryOp::IsNull, expr}`。
- Parser 将 `expr IS NOT NULL` 构造为 `UnaryExpr{UnaryOp::IsNotNull, expr}`。
- A 的 AST 优化器会安全折叠字面量上的 NULL 判断，例如 `NULL IS NULL`、`1 IS NULL`。

### 公共接口影响

- `TokenKind` 新增 `Is`。
- `UnaryOp` 新增 `IsNull` 和 `IsNotNull`。
- 这两个一元操作是给 B 后续语义分析和执行层适配的显式 AST 表示。

### B 侧待适配

- B 后续需要在类型规则中让 `IS NULL` / `IS NOT NULL` 返回 BOOL。
- B 后续需要在绑定表达式、计划输出和执行层中处理 `UnaryOp::IsNull` 与 `UnaryOp::IsNotNull`。
- 当前 A 只负责词法、语法、AST 输出和展示用常量折叠。

### 验证

- 新增 Lexer 测试覆盖 `IS`。
- 新增 Parser 测试覆盖 `score IS NULL` 和 `name IS NOT NULL`。
- 新增 Parser 错误测试覆盖缺少 `NULL` 的 `IS` 用法。
- 新增 AST optimizer 测试覆盖字面量 NULL 判断折叠。

## 2026-09-13：LIMIT / OFFSET

### 新增内容

- 支持 SELECT 末尾的 `LIMIT n`。
- 支持 `LIMIT n OFFSET m`。
- `LIMIT 0` 合法；`LIMIT` 和 `OFFSET` 的值必须是非负整数字面量。
- A 的 Parser 会把值保存在 `SelectStmt::limit` 和 `SelectStmt::offset` 中。
- A 的调试入口会打印 Limit 和 Offset。

### 公共接口影响

- `TokenKind` 新增 `Limit` 和 `Offset`。
- `SelectStmt` 新增 `limit` 和 `offset` 字段。
- 新字段都有默认值，旧代码中不填写 LIMIT/OFFSET 的手工 AST 仍然可以继续编译。

### B 侧待适配

- B 后续需要在语义分析中保留 `SelectStmt::limit` 和 `SelectStmt::offset`。
- 计划生成后续需要增加或复用限制行数的逻辑算子。
- 执行层后续需要按排序、分组、过滤后的结果顺序应用 OFFSET 和 LIMIT。

### 验证

- 新增 Lexer 测试覆盖 `LIMIT` 和 `OFFSET`。
- 新增 Parser 测试覆盖 `LIMIT 10 OFFSET 20` 与 `LIMIT 0`。
- 新增 Parser 错误测试覆盖缺少 LIMIT 值、负数 LIMIT、没有 LIMIT 的 OFFSET、缺少 OFFSET 值。

## 2026-09-13：LIKE

### 新增内容

- 支持表达式中的 `LIKE` 操作符，例如 `name LIKE 'A%'`。
- Lexer 新增关键字 `LIKE`。
- Parser 将 `left LIKE right` 构造为 `BinaryExpr{BinaryOp::Like, left, right}`。
- A 的 AST 优化器不会提前折叠 LIKE；模式匹配语义留给 B 和执行层定义。

### 公共接口影响

- `TokenKind` 新增 `Like`。
- `BinaryOp` 新增 `Like`，并在公共代码中加了注释说明该语义由 B 后续定义。

### B 侧待适配

- B 后续需要在类型规则中让 `VARCHAR LIKE VARCHAR` 返回 BOOL。
- B 后续需要在绑定表达式、计划输出和执行层中处理 `BinaryOp::Like`。
- 建议执行层先支持 `%` 和 `_` 两种通配符，暂不支持 ESCAPE。

### 验证

- 新增 Lexer 测试覆盖 `LIKE`。
- 新增 Parser 测试覆盖 `name LIKE 'A%'`。
- 新增 Parser 错误测试覆盖缺少 LIKE 右操作数。
- 新增 AST optimizer 测试，确保 LIKE 不被 A 提前折叠。

## 2026-09-13：聚合函数 SELECT 项

### 新增内容

- 支持在 SELECT 列表中解析聚合函数：
  - `COUNT(*)`
  - `COUNT(column)`
  - `SUM(column)`
  - `AVG(column)`
  - `MIN(column)`
  - `MAX(column)`
- 聚合函数可以和普通列混合出现，例如：
  - `SELECT active, COUNT(*), AVG(score) AS avg_score FROM metrics GROUP BY active;`
- 聚合项支持已有的输出别名语法：
  - `AS alias`
  - 省略 `AS` 的 `alias`
- A 只负责保留语法结构，不检查：
  - 聚合参数列是否存在。
  - 聚合参数类型是否允许。
  - 聚合列和非聚合列是否满足 GROUP BY 规则。
  - 查询是否需要聚合计划节点。

### 语法边界

- `COUNT(*)` 合法。
- `COUNT(column)` 合法。
- `SUM/AVG/MIN/MAX` 当前只接受列名参数，不接受 `*`。
- 聚合参数当前只接受单个列名，不接受表达式，例如暂不支持 `SUM(score + bonus)`。
- 聚合函数当前只作为 SELECT 项支持，不作为 WHERE、JOIN ON、UPDATE SET 等普通表达式支持。
- `COUNT()`、`SUM(*)`、缺少右括号等写法会在 A 的 Parser 阶段报语法错误。

### 公共接口影响

- `TokenKind` 新增：
  - `Count`
  - `Sum`
  - `Avg`
  - `Min`
  - `Max`
- `ast.hpp` 新增：
  - `enum class AggregateFunction { Count, Sum, Avg, Min, Max }`
  - `struct AggregateCall`
  - `using SelectItem = std::variant<Identifier, AggregateCall>`
- `SelectList` 从：
  - `std::variant<AllColumns, std::vector<Identifier>>`
  变为：
  - `std::variant<AllColumns, std::vector<Identifier>, std::vector<SelectItem>>`
- 为了降低对 B 的即时影响，A 保留了旧的 `std::vector<Identifier>` 分支：
  - 没有聚合函数的普通 SELECT 仍然使用旧分支。
  - 只要 SELECT 列表中出现任意聚合函数，整组 SELECT 项会使用新的 `std::vector<SelectItem>` 分支。
- `AggregateCall::argument` 使用：
  - `AllColumns` 表示 `COUNT(*)`。
  - `Identifier` 表示 `COUNT(column)`、`SUM(column)` 等列参数。

### B 侧待适配

- 语义分析需要在处理 `SelectStmt::columns` 时新增 `std::vector<SelectItem>` 分支。
- 对普通列项：
  - 可以沿用现有列绑定逻辑。
  - 输出别名仍从 `SelectStmt::column_aliases` 读取，索引和 SELECT 项一一对应。
- 对聚合项：
  - 需要绑定 `AggregateCall::argument` 中的列名。
  - `COUNT(*)` 不绑定具体列，但需要依赖当前输入行。
  - 需要检查 `SUM/AVG` 的参数类型，建议只允许数值类型。
  - `MIN/MAX` 是否允许 VARCHAR 由 B 决定；建议先允许可比较类型，或者先只允许数值类型以降低执行层复杂度。
- 建议返回类型：
  - `COUNT` 返回 `INT`。
  - `SUM(INT)` 返回 `INT`，`SUM(FLOAT)` 返回 `FLOAT`。
  - `AVG` 返回 `FLOAT`。
  - `MIN/MAX` 返回参数列类型。
- GROUP BY 规则建议：
  - 没有 GROUP BY 但出现聚合函数时，整张输入表作为一个分组。
  - SELECT 中的非聚合列必须出现在 GROUP BY 中。
  - GROUP BY 中的列不一定必须出现在 SELECT 中。
  - WHERE 在聚合前过滤，ORDER BY 在聚合后排序。
- 计划生成建议：
  - 在 Filter/JOIN 之后、Project/Sort 之前增加聚合或扩展 GroupBy 计划节点。
  - 如果没有 GROUP BY 但有聚合函数，也需要构造单组聚合计划。
  - Project 输出列需要支持普通 BoundColumnRef 和 BoundAggregate 两类来源。
- 执行层建议：
  - `COUNT(*)` 统计输入行数。
  - `COUNT(column)` 是否忽略 NULL 由 B 统一定义；建议按 SQL 常见语义忽略 NULL。
  - `SUM/AVG/MIN/MAX` 对 NULL 的处理也建议由 B 明确后统一执行。

### 验证

- 新增 Lexer 测试覆盖 `COUNT/SUM/AVG/MIN/MAX` 关键字。
- 新增 Parser 测试覆盖普通列与聚合项混合的 SELECT 列表。
- 新增 Parser 测试覆盖 `COUNT(*)`、`COUNT(column)`、`AVG/SUM/MIN/MAX(column)`。
- 新增 Parser 错误测试覆盖 `COUNT` 缺少括号、`COUNT()`、`SUM(*)` 和缺少右括号。

## 2026-09-13：HAVING 子句

### 新增内容

- 支持 SELECT 中的 `HAVING` 子句。
- 子句顺序为：
  - `JOIN`
  - `WHERE`
  - `GROUP BY`
  - `HAVING`
  - `ORDER BY`
  - `LIMIT/OFFSET`
- A 的 Parser 会把 `HAVING` 后的表达式保存到 `SelectStmt::having`。
- A 的 AST 优化器会遍历 `HAVING`，进行和 WHERE 相同的安全常量折叠，例如：
  - `HAVING TRUE AND active = TRUE` 会优化为 `HAVING active = TRUE`。
  - `HAVING TRUE` 会被消除为没有 HAVING。

### 当前边界

- `HAVING` 当前使用现有表达式文法。
- 因为聚合函数目前还不是普通表达式节点，所以本次不支持 `HAVING COUNT(*) > 0`。
- `HAVING` 的聚合表达式支持建议作为后续 B 适配前后的单独扩展项处理。
- A 不检查是否存在 GROUP BY，也不检查 HAVING 表达式是否为 BOOL；这些仍属于 B 的语义阶段。

### 公共接口影响

- `TokenKind` 新增 `Having`。
- `SelectStmt` 末尾新增：
  - `ExprPtr having = nullptr`
- 该字段放在结构体末尾并带默认值，用于保持旧的手写 AST 聚合初始化尽量不受影响。

### B 侧待适配

- 语义分析需要读取 `SelectStmt::having`。
- HAVING 应在分组/聚合之后绑定和检查。
- HAVING 表达式应要求最终类型为 BOOL。
- HAVING 中可以引用哪些名称需要 B 明确定义：
  - 是否允许引用 GROUP BY 列。
  - 是否允许引用 SELECT 输出别名。
  - 后续聚合函数表达式接入后，是否允许直接写 `HAVING COUNT(*) > 0`。
- 计划生成建议：
  - HAVING 对应的过滤节点应放在 GroupBy/Aggregate 之后。
  - 如果后续 Project 会改名或裁剪列，HAVING 应在最终 Project 之前执行。
- 执行层建议：
  - 按每个分组的聚合结果计算 HAVING。
  - HAVING 为 TRUE 的分组保留，为 FALSE 的分组丢弃。
  - NULL/三值逻辑如何处理由 B 和执行层统一决定。

### 验证

- 新增 Lexer 测试覆盖 `HAVING` 关键字。
- 新增 Parser 测试覆盖 `GROUP BY ... HAVING ... ORDER BY ... LIMIT ...` 的合法顺序。
- 新增 Parser 错误测试覆盖 `HAVING` 缺少表达式、`ORDER BY` 后错误出现 `HAVING`。
- 新增 AST optimizer 测试覆盖 HAVING 表达式遍历和常量折叠。

## 2026-09-13：外连接语法

### 新增内容

- 支持显式内连接：
  - `INNER JOIN`
- 支持外连接语法：
  - `LEFT JOIN`
  - `LEFT OUTER JOIN`
  - `RIGHT JOIN`
  - `RIGHT OUTER JOIN`
  - `FULL JOIN`
  - `FULL OUTER JOIN`
- 原有普通 `JOIN` 仍然表示内连接。
- A 会在 AST 的 `JoinClause` 中记录连接类型。

### 当前边界

- A 只负责识别连接类型并保存 AST。
- B 适配前，外连接查询不保证语义分析、计划生成或执行结果正确。
- `OUTER JOIN` 不能单独出现，必须写成 `LEFT/RIGHT/FULL OUTER JOIN`。
- `LEFT/RIGHT/FULL` 后面必须出现 `JOIN`，否则 Parser 报语法错误。
- 新增关键字 `INNER/LEFT/RIGHT/FULL/OUTER` 后，这些词不能再作为裸标识符直接使用。

### 公共接口影响

- `TokenKind` 新增：
  - `Inner`
  - `Left`
  - `Right`
  - `Full`
  - `Outer`
- `ast.hpp` 新增：
  - `enum class JoinType { Inner, Left, Right, Full }`
- `JoinClause` 末尾新增：
  - `JoinType type = JoinType::Inner`
- 该字段有默认值，并放在结构体末尾，用于保持旧的 `JoinClause` 构造代码尽量不受影响。

### B 侧待适配

- 语义分析需要读取 `JoinClause::type`。
- 名称绑定规则可以先沿用当前 JOIN 的可见关系规则：
  - 左输入和右输入都进入后续作用域。
  - ON 表达式可以引用当前已加入的关系。
- 计划生成需要根据 JoinType 选择不同逻辑计划：
  - `Inner` 可继续使用现有 NestedLoopJoin。
  - `Left` 需要保留左侧未匹配行，并为右侧列补 NULL。
  - `Right` 可以实现为交换输入后的 Left Join，也可以单独实现。
  - `Full` 需要保留两侧未匹配行，并为另一侧列补 NULL。
- 输出列类型需要考虑外连接引入的 NULL：
  - 当前类型系统没有 nullable 标记，B 可以先在执行层允许外连接补 NULL。
  - 如果后续要严格表达可空性，需要再扩展公共类型系统。
- 执行层建议：
  - ON 为 TRUE 时按笛卡尔匹配。
  - ON 为 FALSE 或 NULL 时视为不匹配。
  - 外连接补 NULL 的列顺序应保持现有 FROM/JOIN 展开顺序。

### 验证

- 新增 Lexer 测试覆盖 `INNER/LEFT/RIGHT/FULL/OUTER` 关键字。
- 新增 Parser 测试覆盖 `INNER JOIN`、`LEFT OUTER JOIN`、`RIGHT JOIN`、`FULL OUTER JOIN`。
- 新增 Parser 错误测试覆盖缺少 JOIN 的 `LEFT b ...` 和非法的单独 `OUTER JOIN`。

## 2026-09-14：SELECT DISTINCT

### 新增内容

- 支持 `SELECT DISTINCT ...`。
- 支持 `SELECT DISTINCT * FROM table;`。
- 支持 `SELECT DISTINCT column FROM table;`。
- A 会把是否声明 DISTINCT 保存到 `SelectStmt::distinct`。

### 当前边界

- A 只记录 DISTINCT 标记，不做去重逻辑。
- A 不判断 DISTINCT 是否和 GROUP BY、ORDER BY、LIMIT 或聚合函数组合合理。
- `DISTINCT` 只能出现在 `SELECT` 后、选择列表前。
- `SELECT name DISTINCT FROM table;` 和重复 `DISTINCT` 会在 Parser 阶段报语法错误。
- 新增关键字 `DISTINCT` 后，该词不能再作为裸标识符直接使用。

### 公共接口影响

- `TokenKind` 新增 `Distinct`。
- `SelectStmt` 末尾新增：
  - `bool distinct = false`
- 该字段带默认值并放在结构体末尾，用于保持旧的手写 AST 聚合初始化尽量不受影响。

### B 侧待适配

- 语义分析需要读取 `SelectStmt::distinct`。
- 计划生成建议在最终 Project 之后、ORDER BY/LIMIT 之前增加去重逻辑；如果 B 决定按 SQL 标准执行顺序，也可以明确为：
  - FROM/JOIN
  - WHERE
  - GROUP BY / Aggregate
  - HAVING
  - SELECT Project
  - DISTINCT
  - ORDER BY
  - LIMIT/OFFSET
- 执行层需要定义重复行判断：
  - 建议所有输出列值都相同才视为重复。
  - NULL 与 NULL 是否视为相同需要 B 明确；建议 DISTINCT 去重时把 NULL 与 NULL 视为相同。
- 如果 ORDER BY 引用隐藏列，DISTINCT 和隐藏列的交互需要 B 明确：
  - 可以先限制 DISTINCT 查询只能按输出列排序。
  - 或者保留隐藏排序列但只按输出列去重。

### 验证

- 新增 Lexer 测试覆盖 `DISTINCT` 关键字。
- 新增 Parser 测试覆盖 `SELECT DISTINCT *` 和 `SELECT DISTINCT column`。
- 新增 Parser 错误测试覆盖 DISTINCT 放错位置和重复 DISTINCT。

## 2026-09-14：Parser 错误提示优化

### 新增内容

- Parser 的语法错误消息现在会包含当前遇到的 Token 类型和原始词素，例如：
  - `unexpected identifier "BOGUS", expected CREATE, INSERT, SELECT, UPDATE or DELETE`
- expected 信息支持多个候选项，不再只能写成一个手工拼接字符串。
- SELECT 子句顺序错误会给出专门提示：
  - `JOIN -> WHERE -> GROUP BY -> HAVING -> ORDER BY -> LIMIT/OFFSET`
- 常见缺失项的提示更具体，例如：
  - SELECT 列表中错误出现 `*` 时提示期望列名或聚合函数。
  - `LIMIT` 后缺值时提示期望非负整数。
  - INSERT 字面量位置错误时列出可接受的字面量类型。

### 公共接口影响

- 未新增 AST 字段。
- 未新增 TokenKind。
- 未修改 B 的语义分析、计划生成、Catalog 或执行层代码。
- `Diagnostic::message` 的文本更详细；错误码仍然使用原有 `UnexpectedToken` 等稳定枚举。

### B 侧待适配

- 无强制适配项。
- 如果 B 或 UI 层有基于错误消息全文匹配的测试，建议改成匹配 `ErrorCode` 和关键片段；公共注释中已经说明 message 可以改进，不应依赖完整字符串。

### 验证

- 新增 Parser 测试检查错误消息中包含：
  - 当前 token 原始词素。
  - 多个 expected 候选。
  - SELECT 子句顺序提示。
  - 字面量 expected 列表。

## 2026-09-14：Parser SELECT 解析结构整理

### 新增内容

- 对 `src/parser/parser.cpp` 内部结构做了整理，降低 `selectStatement()` 的复杂度。
- `selectStatement()` 现在主要负责：
  - 读取 `DISTINCT` 标记。
  - 组装 `SelectStmt` 主体。
  - 调用子函数解析 SELECT 后续部分。
- 新增内部辅助函数：
  - `selectList()`：解析 `*`、普通列列表、聚合 SELECT 项和列别名。
  - `selectTailClauses()`：解析 FROM 之后的 `JOIN/WHERE/GROUP BY/HAVING/ORDER BY/LIMIT`。
  - `limitClause()`：集中解析 `LIMIT/OFFSET`。

### 公共接口影响

- 未新增 AST 字段。
- 未新增 TokenKind。
- 未修改文法能力。
- 未修改 B 的语义分析、计划生成、Catalog 或执行层代码。

### B 侧待适配

- 无强制适配项。
- 这是 A 内部可维护性调整，目的是让后续扩展 SELECT 表达式、聚合表达式和更多谓词时更容易定位改动范围。

### 验证

- 复用现有 Lexer、Parser、AST optimizer 和全量测试验证行为不变。

## 2026-09-14：NOT LIKE

### 新增内容

- 支持表达式中的 `NOT LIKE`：
  - `name NOT LIKE 'A%'`
- Parser 会把它构造为：
  - 外层 `UnaryExpr{UnaryOp::Not, ...}`
  - 内层 `BinaryExpr{BinaryOp::Like, left, right}`
- 这样可以复用已有 `NOT` 和 `LIKE` 表达式能力，不引入新的公共操作符。

### 当前边界

- `NOT` 后面必须紧跟 `LIKE`，例如 `name NOT 'A%'` 会报语法错误。
- `NOT LIKE` 的模式匹配规则仍然沿用 `LIKE`，由 B 和执行层后续定义。
- A 的 AST optimizer 不会提前折叠 LIKE，因此也不会提前折叠 NOT LIKE 的字符串匹配语义。

### 公共接口影响

- 未新增 TokenKind。
- 未新增 AST 字段。
- 未新增 BinaryOp 或 UnaryOp。
- 未修改 B 的语义分析、计划生成、Catalog 或执行层代码。

### B 侧待适配

- 如果 B 已经支持 `UnaryOp::Not` 和 `BinaryOp::Like`，则 `NOT LIKE` 不需要新增公共枚举分支。
- 语义分析只需要能处理 `NOT` 的操作数为 BOOL；`LIKE` 返回 BOOL 后，外层 NOT 即可成立。
- 计划输出和执行层可以按普通一元 NOT 包裹 LIKE 的表达式树处理。

### 验证

- 新增 Parser 测试覆盖 `name NOT LIKE 'B%'`。
- 新增 Parser 错误消息测试覆盖 `NOT` 后缺少 `LIKE`。

## 2026-09-14：BETWEEN / NOT BETWEEN

### 新增内容

- 支持表达式中的 `BETWEEN`：
  - `age BETWEEN 18 AND 30`
- 支持表达式中的 `NOT BETWEEN`：
  - `age NOT BETWEEN 10 AND 20`
- Parser 不新增专门的 Between AST 节点，而是展开为已有表达式：
  - `a BETWEEN b AND c` 展开为 `a >= b AND a <= c`
  - `a NOT BETWEEN b AND c` 展开为 `NOT (a >= b AND a <= c)`

### 当前边界

- `BETWEEN` 的上下界当前使用已有 `additive` 表达式层级。
- `BETWEEN` 中间必须出现 `AND`。
- `NOT` 后如果不是 `LIKE` 或 `BETWEEN`，Parser 会给出 expected 提示。
- A 不检查左右操作数类型是否可比较；类型规则仍由 B 决定。

### 公共接口影响

- `TokenKind` 新增 `Between`。
- 未新增 AST 字段。
- 未新增 BinaryOp 或 UnaryOp。
- 未修改 B 的语义分析、计划生成、Catalog 或执行层代码。

### B 侧待适配

- 如果 B 已经支持：
  - `BinaryOp::GreaterEqual`
  - `BinaryOp::LessEqual`
  - `BinaryOp::And`
  - `UnaryOp::Not`
  则 `BETWEEN/NOT BETWEEN` 不需要新增表达式种类。
- B 仍需按展开后的比较表达式做类型检查。
- `BETWEEN` 的 SQL 空值语义当前不会由 A 特殊处理，后续应跟普通比较和 AND/NOT 的 NULL 规则保持一致。

### 验证

- 新增 Lexer 测试覆盖 `BETWEEN` 关键字。
- 新增 Parser 测试覆盖 `age BETWEEN 18 AND 30` 的 AST 展开。
- 新增 Parser 测试覆盖 `age NOT BETWEEN 10 AND 20` 的外层 NOT。
- 新增 Parser 错误测试覆盖缺少 `AND` 和 `NOT` 后缺少 `LIKE/BETWEEN`。

## 2026-09-14：IN / NOT IN

### 新增内容

- 支持字面量列表版 `IN`：
  - `id IN (1, 2, 3)`
- 支持字面量列表版 `NOT IN`：
  - `name NOT IN ('Alice', 'Bob')`
- Parser 不新增专门的 In AST 节点，而是展开为已有表达式：
  - `a IN (x, y, z)` 展开为 `a = x OR a = y OR a = z`
  - `a NOT IN (x, y)` 展开为 `NOT (a = x OR a = y)`

### 当前边界

- 当前只支持字面量列表，不支持列名、表达式或子查询：
  - 暂不支持 `id IN (other_id)`。
  - 暂不支持 `id IN (1 + 2)`。
  - 暂不支持 `id IN (SELECT id FROM t)`。
- `IN ()` 空列表会报语法错误。
- 列表中逗号后必须继续出现字面量，`IN (1,)` 会报语法错误。
- A 不检查左侧表达式和列表字面量之间的类型兼容性；类型规则仍由 B 决定。

### 公共接口影响

- `TokenKind` 新增 `In`。
- 未新增 AST 字段。
- 未新增 BinaryOp 或 UnaryOp。
- 未修改 B 的语义分析、计划生成、Catalog 或执行层代码。

### B 侧待适配

- 如果 B 已经支持：
  - `BinaryOp::Equal`
  - `BinaryOp::Or`
  - `UnaryOp::Not`
  则 `IN/NOT IN` 不需要新增表达式种类。
- B 仍需按展开后的等值比较和 OR/NOT 表达式做类型检查。
- `IN` 中的 NULL 语义当前不会由 A 特殊处理，后续应跟普通等值比较和 OR/NOT 的 NULL 规则保持一致。
- 如果 B 后续要支持子查询版 IN，建议另行设计 AST，不建议继续展开为 OR。

### 验证

- 新增 Lexer 测试覆盖 `IN` 关键字。
- 新增 Parser 测试覆盖 `id IN (1, 2, 3)` 的 OR 展开。
- 新增 Parser 测试覆盖 `name NOT IN ('Alice', 'Bob')` 的外层 NOT。
- 新增 Parser 错误测试覆盖空列表、逗号后缺值和非字面量列表项。

## 2026-09-14：SELECT 表达式项

### 新增内容

- 支持 SELECT 列表中的表达式项，例如：
  - `SELECT age + 1 AS next_age FROM student;`
  - `SELECT score * 1.1 adjusted_score FROM metrics;`
  - `SELECT active = TRUE AS is_active FROM metrics;`
  - `SELECT 10 AS constant_value FROM t;`
- 普通列查询仍尽量保留旧 AST 分支，降低对 B 的即时影响。

### 公共接口影响

- `SelectItem` 新增 `ExprPtr` 分支。
- `SelectList` 仍保留 `AllColumns`、`std::vector<Identifier>` 和 `std::vector<SelectItem>` 三个分支。
- 没有新增 TokenKind。
- 没有修改 B 的语义分析、计划生成、Catalog 或执行层代码。

### B 侧待适配

- 详细适配说明单独放在 `docs/select_expression_items_B_adaptation.md`。

### 验证

- 新增 Parser 测试覆盖算术表达式、字面量表达式、比较表达式、括号表达式和输出别名。

## 2026-09-14：列级约束语法

### 新增内容

- 支持 CREATE TABLE 中的列级约束：
  - `PRIMARY KEY`
  - `NOT NULL`
  - `UNIQUE`
  - `DEFAULT literal`
- 示例：
  - `CREATE TABLE account(id INT PRIMARY KEY, name VARCHAR(20) NOT NULL UNIQUE DEFAULT 'guest');`
- A 会把这些标记保存到 `ColumnDefinition` 中。

### 当前边界

- 只支持列级约束，不支持表级约束：
  - 暂不支持 `PRIMARY KEY(id)`。
  - 暂不支持 `UNIQUE(name)`。
- `DEFAULT` 当前只接受字面量，复用 INSERT 的字面量范围。
- A 不检查默认值类型是否匹配列类型。
- A 不检查 PRIMARY KEY 是否隐含 NOT NULL。
- A 不检查一个表中是否出现多个 PRIMARY KEY。
- 重复写同一种列级约束会在 Parser 阶段报语法错误。

### 公共接口影响

- `TokenKind` 新增：
  - `Primary`
  - `Key`
  - `Unique`
  - `Default`
- `ColumnDefinition` 新增字段：
  - `bool primary_key = false`
  - `bool not_null = false`
  - `bool unique = false`
  - `std::optional<LocatedLiteral> default_value = {}`
- `LocatedLiteral` 被移动到 `ColumnDefinition` 之前定义，以便 DEFAULT 和 INSERT 共用。
- 新字段均带默认值，用于保持旧的手写 AST 初始化尽量不受影响。

### B 侧待适配

- 语义分析需要读取 `ColumnDefinition` 上的新字段。
- 建议 B 后续处理规则：
  - PRIMARY KEY 至少应隐含 NOT NULL 和 UNIQUE。
  - 一个表中最多允许一个 PRIMARY KEY。
  - UNIQUE 是否允许多个 NULL 由 B/Catalog/执行层统一定义。
  - DEFAULT 的类型需要与列类型兼容。
  - INSERT 省略列清单或未来支持缺省列时，可以使用 DEFAULT 值。
- Catalog 后续需要在 `ColumnSpec` / `ColumnSchema` 中保存对应约束，或者另建约束结构。
- 执行层后续需要在 INSERT/UPDATE 时检查 NOT NULL、UNIQUE/PRIMARY KEY，并填充 DEFAULT。

### 验证

- 新增 Lexer 测试覆盖 `PRIMARY/KEY/UNIQUE/DEFAULT` 关键字。
- 新增 Parser 测试覆盖 PRIMARY KEY、NOT NULL、UNIQUE、字符串/BOOL/FLOAT DEFAULT。
- 新增 Parser 错误测试覆盖 PRIMARY 缺 KEY、NOT 缺 NULL、DEFAULT 缺值、重复 PRIMARY KEY 和重复 DEFAULT。

## 2026-09-14：聚合调用表达式化

### 新增内容

- 聚合调用现在可以作为普通表达式节点出现。
- A 可以解析：
  - `SELECT COUNT(*) + 1 AS count_plus_one FROM metrics;`
  - `SELECT active FROM metrics GROUP BY active HAVING COUNT(*) > 0;`
- 裸聚合 SELECT 项仍尽量保留旧的 `SelectItem::AggregateCall` 表示，降低 B 的一次性适配压力。

### 公共接口影响

- `Expr::node` 新增 `AggregateCall` 分支。
- `SelectItem` 结构不变，仍然是 `Identifier / AggregateCall / ExprPtr`。
- 没有新增 TokenKind。

### B 侧当前处理

- `src/semantic/analyzer.cpp` 增加了带注释的 `AggregateCall` 分支。
- 当前 B 遇到聚合表达式会返回 `UnsupportedFeature`。
- 这只是为了保证新增公共 AST 后 B 不会编译失败，不代表 B 已经支持聚合绑定或执行。
- 完整适配方案见 `docs/select_expression_items_B_adaptation.md` 的“聚合表达式化补充”。

### 验证

- 新增 Parser 测试覆盖 `COUNT(*) + 1` SELECT 表达式项。
- 新增 Parser 测试覆盖 `HAVING COUNT(*) > 0`。

## 2026-09-14：ORDER BY 表达式

### 新增内容

- 支持 ORDER BY 中书写表达式：
  - `ORDER BY score + 1 DESC`
  - `ORDER BY COUNT(*) ASC`
- 普通列名排序仍保留旧字段表示。

### 公共接口影响

- `OrderByItem` 末尾新增：
  - `ExprPtr expression = nullptr`
- 没有新增 TokenKind。
- `column` 字段保留，旧的手工 AST 初始化仍尽量兼容。

### B 侧当前处理

- `src/semantic/analyzer.cpp` 增加了带注释的检查。
- 当前 B 遇到 `OrderByItem::expression` 非空时返回 `UnsupportedFeature`。
- 完整适配方案见 `docs/select_expression_items_B_adaptation.md` 的“ORDER BY 表达式补充”。

### 验证

- 新增 Parser 测试覆盖算术 ORDER BY 表达式和聚合 ORDER BY 表达式。
- 新增 AST optimizer 测试覆盖 ORDER BY 表达式遍历。
- 新增 semantic 测试确认 B 当前明确返回 `UnsupportedFeature`。

## 2026-09-14：INSERT 多行 VALUES

### 新增内容

- 支持一条 INSERT 中写多组 VALUES：
  - `INSERT INTO student VALUES (1, 'Alice', 20), (2, 'Bob', 18);`
  - `INSERT INTO student(id, name, age) VALUES (1, 'Alice', 20), (2, 'Bob', 18);`
- A 会保存每一行字面量，行内仍只接受已有 `literal` 范围：
  - 整数和负整数
  - 浮点数和负浮点数
  - 字符串
  - TRUE/FALSE
  - NULL
- A 不检查每行值数量是否一致，也不检查列数量是否匹配；这些仍属于 B 的语义阶段。

### 当前边界

- 不支持 `VALUES ()` 空行。
- 不支持 `INSERT ... DEFAULT VALUES`。
- 不支持值位置写 `DEFAULT` 关键字。
- 不支持 `INSERT ... SELECT ...`。
- 多行 INSERT 当前只保证 Lexer/Parser/AST 输出正确；B 完整适配前不会进入计划生成和执行。

### 公共接口影响

- `InsertStmt` 末尾新增：
  - `std::vector<std::vector<LocatedLiteral>> rows = {}`
- `InsertStmt::values` 保留，用于兼容 B 现有单行读取路径。
- A 的 Parser 对新 SQL 会同时写入：
  - `values = rows.front()`
  - `rows = 全部 VALUES 行`
- 如果外部旧代码手工构造 AST，只填写 `values` 而不填写 `rows`，B 当前仍按旧单行语义处理。

### B 侧当前处理

- `src/semantic/analyzer.cpp` 增加了带注释的兼容逻辑：
  - `rows.empty()` 时沿用 `values`。
  - `rows.size() == 1` 时读取 `rows.front()`，行为等同单行 INSERT。
  - `rows.size() > 1` 时返回 `UnsupportedFeature`。
- 这样可以避免 B 在尚未扩展 `BoundInsert` / `InsertPlan` 前只绑定第一行，造成静默丢数据。

### B 侧后续适配建议

- 语义分析：
  - 遍历 `InsertStmt::rows` 中每一行。
  - 每行都检查值数量、列覆盖和类型兼容。
  - 错误位置优先使用具体行中出错 `LocatedLiteral::span`。
  - 如果声明了显式列清单，每一行都按同一列映射重排到表模式顺序。
- Bound 层：
  - 可以把 `BoundInsert` 从单行 `values` 扩展为多行 `rows`。
  - 或新增 `BoundInsertRows`，让旧单行计划暂时不受影响。
- Plan 层：
  - 可以把 `InsertPlan` 扩展为多行值。
  - 执行层按行依次插入；如果后续支持事务，应明确某一行失败时是全部回滚还是保留前面成功行。
- Catalog/约束：
  - 后续接入 DEFAULT、NOT NULL、UNIQUE、PRIMARY KEY 时，多行内部也需要检查互相冲突。

### 验证

- 新增 Parser 测试覆盖省略列清单和显式列清单的多行 INSERT。
- 新增 Parser 测试覆盖多行中的字符串、NULL、负数字面量。
- 新增 Parser 错误测试覆盖缺少第一行、空行、逗号后缺行。
- 新增 semantic 测试确认单行 `rows` 兼容旧行为，且多行当前明确返回 `UnsupportedFeature`。

## 2026-09-14：DROP TABLE

### 新增内容

- 支持删除表语句的 A 侧语法：
  - `DROP TABLE student;`
  - `DROP TABLE IF EXISTS old_student;`
  - `DROP TABLE archive_2024, archive_2025;`
- A 会保存：
  - 是否声明 `IF EXISTS`。
  - 要删除的表名列表及每个表名的位置。
- A 不修改 Catalog，不检查表是否存在，也不决定 `IF EXISTS` 的运行语义。

### 当前边界

- 仅支持 `DROP TABLE`，不支持：
  - `DROP DATABASE`
  - `DROP INDEX`
  - `DROP VIEW`
  - `DROP TEMPORARY TABLE`
  - `CASCADE` / `RESTRICT`
- 表名仍使用现有 `name` 规则，支持限定名文本保留，但是否允许真正按多段名称删除由 B 决定。
- `IF` 后必须紧跟 `EXISTS`。
- 表名列表不能为空，逗号后必须继续出现表名。

### 公共接口影响

- `TokenKind` 新增：
  - `Drop`
  - `If`
  - `Exists`
- `ast.hpp` 新增：
  - `struct DropTableStmt`
- `Statement::node` 新增 `DropTableStmt` 分支。
- `DropTableStmt` 字段：
  - `std::vector<Identifier> tables`
  - `bool if_exists = false`
- 因为 `Statement` variant 增加了新分支，所有 `std::visit(Statement::node)` 的位置都需要能处理 `DropTableStmt`。

### B 侧当前处理

- `src/semantic/analyzer.cpp` 增加了带注释的 `bindStatement(const DropTableStmt&)`。
- 当前 B 遇到 DROP TABLE 会返回 `UnsupportedFeature`。
- 这样做是为了保证新增公共 AST 后工程可以编译，并且避免调用方误以为 analyze 已经删除 Catalog 中的表。

### B 侧后续适配建议

- 语义分析：
  - 检查 `DropTableStmt::tables` 是否为空；正常 Parser 不会生成空列表，但手工 AST 可能会。
  - 逐个规范化表名，并检查同一条 DROP 中是否重复写同一个表。
  - 未声明 `IF EXISTS` 且表不存在时返回 `TableNotFound`。
  - 声明 `IF EXISTS` 且表不存在时应按成功处理，或者返回一个不产生删除动作的 Bound 节点；具体由 B 统一定义。
- Bound 层：
  - 可以新增 `BoundDropTable`，保存规范化后的表名列表和 `if_exists`。
  - 如果需要精确删除已存在表，也可以保存 `TableId`，但删除动作通常还需要按名称检查当前 Catalog 版本。
- Plan/Catalog 层：
  - 可以新增 `DropTablePlan`，或让 DDL 语句直接走 Catalog 操作。
  - 需要定义删除表时对旧快照、已有 BoundStatement/Plan 的影响。当前快照模型适合“旧快照仍然只读有效，新 Catalog 版本删除表”。
  - 如果后续有索引、约束或外键，DROP TABLE 需要同步清理依赖对象或明确拒绝。
- 执行层：
  - 多表 DROP 建议按列表顺序处理。
  - 如果中途失败，需要定义前面已经删除的表是否回滚；没有事务时建议先在语义/计划阶段完成全部可行性检查。

### 验证

- 新增 Lexer 测试覆盖 `DROP`、`IF`、`EXISTS` 关键字。
- 新增 Parser 测试覆盖普通 DROP、`IF EXISTS` 和多表名列表。
- 新增 Parser 错误测试覆盖缺少 TABLE、缺少表名、`IF` 后缺少 `EXISTS`、逗号后缺少表名。
- 新增 semantic 测试确认 B 当前明确返回 `UnsupportedFeature`。

## 2026-09-14：UPDATE / DELETE 目标表别名

### 新增内容

- 支持 UPDATE 目标表声明别名：
  - `UPDATE student AS s SET s.age = s.age + 1 WHERE s.id = 1;`
  - `UPDATE student s SET age = age + 1 WHERE s.id = 1;`
- 支持 DELETE 目标表声明别名：
  - `DELETE FROM student AS s WHERE s.id = 1;`
  - `DELETE FROM student s WHERE s.id = 1;`
- 别名语法复用现有 `table_ref` 规则，支持 `AS alias` 和省略 `AS` 的 `alias`。

### 名字解析规则

- 未声明别名时，UPDATE/DELETE 仍按旧规则解析：
  - `age`
  - `student.age`
- 声明别名后，限定列应使用别名：
  - `s.age`
- 声明别名后，物理表名不再作为限定符参与当前语句解析：
  - `UPDATE student AS s SET student.age = 1;` 会在语义阶段报列不存在。
- 不带限定符的列名仍然可用，因为 UPDATE/DELETE 当前都是单表语句：
  - `UPDATE student AS s SET age = age + 1 WHERE id = 1;`

### 公共接口影响

- `UpdateStmt` 末尾新增：
  - `std::optional<Identifier> table_alias = {}`
- `DeleteStmt` 末尾新增：
  - `std::optional<Identifier> table_alias = {}`
- 字段放在结构体末尾并提供默认值，旧的手写 AST 聚合初始化仍可继续编译。
- 没有新增 TokenKind。
- 没有新增 Bound 或 Plan 结构。

### B 侧适配情况

- `src/semantic/analyzer.cpp` 已同步适配 UPDATE/DELETE 的单表作用域。
- 适配方式：
  - 如果 AST 带 `table_alias`，单表作用域中的关系名使用别名。
  - 如果 AST 不带 `table_alias`，继续使用表的物理名称。
  - assignment target、assignment RHS、WHERE 统一在同一个作用域里解析。
- 计划层不需要为别名单独扩字段，因为进入计划前已经绑定为稳定的 `BoundColumnRef`。

### 当前边界

- UPDATE/DELETE 仍然是单表语句。
- 不支持 `UPDATE ... FROM ...`。
- 不支持 `DELETE s FROM student s JOIN ...`。
- 不支持多表 UPDATE 或多表 DELETE。
- 手工 AST 如果把别名写成带点名称，会在语义阶段返回 `InvalidAst`。

### 验证

- 新增 Parser 测试覆盖 UPDATE/DELETE 的显式和隐式表别名。
- 新增 Parser 测试确认 alias-qualified target / WHERE 表达式保留。
- 新增 semantic 测试覆盖：
  - `s.age` 作为 UPDATE 目标列。
  - `s.age` 作为 UPDATE RHS。
  - `s.id` 作为 UPDATE/DELETE WHERE。
  - 未声明别名时 `student.id` 仍可绑定。
  - 声明别名后 `student.id` 被视为不可见限定名。
