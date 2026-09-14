# 模块接口契约 0.14

本文定义 A、B、Catalog 与执行层的衔接。当前 MemoryCatalog、六类基础语句与 EXPLAIN 语义分析、
逻辑计划生成、规则优化和文本打印已实现；A version2 的扩展 lex/parse 与 AST 展示优化已合入。
本仓库通过 `app/plan_json.cpp` 把计划交给相邻 Java 引擎，具体协议见 json-plan-protocol.md。

## 数据流和所有权

```text
A: SQL → Token → Statement(AST)
                         ↓ analyze(ast, catalog)
B:                 BoundStatement
                         ↓ buildPlan(bound)
                     LogicalPlan
                         ↓ optimizePlan(plan)，可选
                     LogicalPlan
                         ↓
执行层: 扫描/过滤/写入 → 存储层
```

- AST 保留原始名称与源码范围，不含数据库内部 ID。B 不修改 AST。
- BoundStatement 将每个名称换成表 ID、列 ID、列序号；每个表达式有确定类型。
- Plan 仅保存执行信息；表达式携带源码范围供运行时报错。
- 递归树通过 `shared_ptr<const T>` 共享只读子节点，避免复制整棵树。
  必需子节点不得为空；可选 WHERE 用空指针表示。发布后不得保留可变别名。
- `std::variant` 表示互斥节点种类，用 `std::visit` 分派。节点种类新增时应
  同步检查所有 visitor；不要依赖 variant 的整数下标跨模块通信。
- 这是同一工程/同一工具链下的源码级 C++ 接口，不承诺跨进程或二进制 ABI。

## A 内部：Lexer → Parser

```cpp
Result<TokenStream> lex(std::string_view sql);
Result<std::vector<Statement>> parse(const TokenStream& tokens);
```

`token.hpp` 定义文法中的关键字、标识符、整数、浮点数、字符串、操作符和分隔符。
Token 持有原始词素副本和必填范围；关键字种类已区分，但词素不转小写。
字符串词素包含原始引号与转义；Parser 负责解码字符串和有符号 64 位数值。
负号始终是单独 token，点号独立为 Dot；具体解析约定见 grammar.md。

lex 仅在调用期间借用输入，成功结果以唯一 EOF 结尾；EOF 原文为空、范围
为输入结束处的零长度范围。空 SQL 得到仅 EOF 的 TokenStream。
parse 接受符合此约定的 TokenStream；仅 EOF 返回空语句列表。
两者首版均遇首错返回 Diagnostic，不返回部分结果；后续错误恢复另行扩展接口。
AST 自持字符串与子节点，不依赖 TokenStream 的生命周期。

上述约定现已实现。parse 在入口检查唯一且末尾的 EOF，不接受 EOF 后还有其他 Token。
Parser 内部使用 Diagnostic 异常快速退出递归，在 run 边界捕获并转换为 Result，
调用方仍通过统一 Result 接口获取语法错误。
NOT/负号/括号的递归嵌套最多 256 层，生成 AST 的单条路径最多 256 个节点；
超过时返回 Syntax / ExpressionTooDeep，防止输入在到达 B 前先耗尽栈。

## A → B：AST

`ast.hpp` 定义 CreateTableStmt、DropTableStmt、InsertStmt、SelectStmt、UpdateStmt、DeleteStmt
和 ExplainStmt。ExplainTarget 只允许前六类基础语句，所以语法层不能嵌套 EXPLAIN。
Statement 保存整条语句范围；Identifier 保存原始拼写及精确范围。
一元/二元表达式另存运算符范围，便于把类型错误定位到操作符。

SELECT 使用 `variant<AllColumns, vector<Identifier>, vector<SelectItem>>`。
旧的纯列清单保留源码兼容；SelectItem 统一采用 A 的
variant<Identifier, AggregateCall, ExprPtr>，所有输出别名平行保存在 column_aliases。
AggregateCall 使用 AggregateFunction 枚举，argument 为 variant<AllColumns, Identifier>；
只有 COUNT(*) 使用 AllColumns。B 显式映射为 AggregateKind，不依赖枚举整数或 variant 下标。
Expr 增加 AggregateCall 分支；B 允许它出现在 SELECT、HAVING 和 ORDER BY 表达式中。
WHERE 和 JOIN ON 中的聚合调用返回 InvalidGrouping。
INSERT 使用 optional 列清单区分省略和显式给定，显式清单不得为空。
SelectStmt 追加 group_by、order_by、joins、FROM 表别名和与选择列平行的 column_aliases，
并为旧的三字段聚合初始化提供空默认值。JoinClause 可携带右表别名。
限定名由 Parser 合并为 `table.column` 的 Identifier.text；原始范围覆盖整个限定名。
字面量允许 int64_t/double/string/bool/NullValue；DataType 增加 Float，Null 仅为内部字面量类型。

A 的展示优化接口声明于 ast_optimizer.hpp：

```cpp
ExprPtr optimizeAstExpression(const ExprPtr&);
Statement optimizeAstStatement(const Statement&);
std::vector<Statement> optimizeAstStatements(const std::vector<Statement>&);
```

它用于 CLI 展示，不访问 Catalog，不代替 analyze。由于语义分析前尚不知道列是否存在，
FALSE AND unknown_column 等短路改写可能删除未绑定分支，所以编译流程必须把原 AST 交给
analyze；具有完整绑定信息的执行优化使用 B 的 optimizePlan。

A 负责语法合法性、数值解码及源码范围，B 负责表列存在性及类型规则。
完整语言约定见 `grammar.md`。Token 在 A 的独立头文件中定义，B 不依赖 Lexer。

## Catalog → B：只读模式快照

`CatalogSnapshot::findTable(normalized_name)` 返回共享只读 TableSchema，
不存在返回空指针。调用者用 `normalizeName` 进行 ASCII 小写归一化。
TableSchema 中表名/列名已归一化，列列表为建表顺序，ID 在快照中稳定。
列 ID 只要求在所属表内唯一，表 ID 在 Catalog 内唯一。

快照对象在一次分析中不可变化；version 必须标识完整模式版本。
B 把 version 写入绑定结果，再由计划生成传入 LogicalPlan。
执行层必须在执行期间固定该模式版本，版本不符则拒绝计划并请求重新编译。

`MemoryCatalog` 已提供可独立使用的内存实现（不保存记录、不持久化）：

```cpp
Result<std::shared_ptr<const TableSchema>> createTable(
    std::string table_name, const std::vector<ColumnSpec>& columns);
Result<std::size_t> dropTables(
    const std::vector<std::string>& table_names, bool if_exists);
std::shared_ptr<const CatalogSnapshot> snapshot() const;
```

createTable 要求名称符合 grammar.md 的基本标识符规则；它统一大小写、检查重复表/列、
空列定义、非法类型、VARCHAR 长度和默认值约束，失败不修改模式、不消耗 ID。显式注册无源码来源，失败使用
Execution 阶段诊断及空范围；这不代表完整执行引擎已经实现。
成功注册分配表 ID（从 1 开始）、表内列 ID（从 1 开始）并递增模式版本。
snapshot 复制名称索引并共享只读模式，旧快照不会看到新注册表，也可以比容器活得更久。
版本仅在同一 Catalog 实例的历史中比较；该内存容器供第一阶段单线程使用。
dropTables 在无 IF EXISTS 时先验证全部名字再修改，IF EXISTS 忽略缺失表；实际删除非空时
版本只递增一次。快照共享的旧 TableSchema 仍可被既有只读计划安全持有。

CREATE 产生建表描述，不预分配表列 ID，不调用 createTable。
成功执行后，由执行层负责修改真实 Catalog 并递增版本。
因此多语句由上层按“编译→执行成功→新快照→下一条”驱动。

## B 的调用接口和错误

```cpp
Result<BoundStatement> analyze(const Statement&, const CatalogSnapshot&);
Result<LogicalPlan> buildPlan(const BoundStatement&);
Result<LogicalPlan> optimizePlan(const LogicalPlan&); // 声明于 optimizer.hpp。
std::string formatPlan(const LogicalPlan&); // 声明于 plan_printer.hpp。
```

Result 为 `variant<T, Diagnostic>`，错误时不返回半成品；使用 `get_if` 或
`holds_alternative` 检查结果。首版每条语句仅报告首个语义错误。
NotImplemented 保留为后续开发状态错误码，当前基础语句及 EXPLAIN 的四个入口不再返回占位结果。
各类语句的 analyze/buildPlan 均返回真实结果或诊断；表达式支持 INT/FLOAT 同类型
算术与比较、VARCHAR/BOOL 判等、LIKE、空值判定以及 AND/OR/NOT。
Diagnostic 包含阶段、稳定错误码、可读消息、SourceSpan。
范围按字节、从 1 开始的行列、左闭右开定义；未知范围用 optional 表示，
禁止把未知范围伪装成第 1 行。正常 A 输入必须提供真实范围。

诊断按确定顺序返回：先检查表，再按 SQL 顺序检查列；INSERT 再检查值数、
默认值/必填列和逐值类型；SELECT 继续检查 WHERE、GROUP/HAVING/ORDER。表达式按左子树、右子树、
当前操作符的顺序检查；静态检查不会因为 AND/OR 的运行时短路而跳过某一子树。
UPDATE 先查表，再按赋值顺序检查目标列、重复目标、RHS 表达式及类型，最后检查 WHERE；
DELETE 查表后检查 WHERE。空赋值列表/空 RHS 属于外部 AST 结构错误，报 InvalidAst。
SELECT 按 FROM、各 JOIN 表和 ON、投影、WHERE、GROUP BY、HAVING、ORDER BY 的顺序绑定。
UPDATE/DELETE 与 SELECT 共用布尔条件检查，WHERE 省略合法，存在时必须为 BOOL。
优先使用名称/操作符/值自身范围，缺失时回退到表达式或语句范围，最终仍可为空。
必需表达式子节点为空时报 InvalidAst；表达式路径超过 256 个节点时报 ExpressionTooDeep。
绑定只推导类型，不求值，所以类型合法的除零或溢出表达式保留给执行层报告。
INSERT 的 NULL 仅受列 NOT NULL/PRIMARY KEY 约束；执行层以 null 表示 SQL UNKNOWN 并执行三值逻辑。
NULL 字面量直接参与需要两个同类型操作数的表达式仍会报 InvalidOperandType。限定名按关系有效名称解析；声明表别名后必须以
别名限定。未限定列名在全部可见关系实例中查找，命中多个实例时返回 AmbiguousColumn。
同一物理表可用不同别名自连接；重复关系名返回 DuplicateTable。选择列别名决定 Project
输出名称，并可由 ORDER BY 引用；同名输出别名被引用时返回 AmbiguousColumn。
JOIN ON 必须为 BOOL，否则返回 JoinConditionNotBoolean。聚合外的 SELECT/HAVING/ORDER 列
必须属于分组键，重复键或不满足约束返回 InvalidGrouping。

绑定结果约束：全部列引用已解析；WHERE/JOIN ON 为 BOOL；运算符合法；INSERT 值按
表列顺序重排并补齐 DEFAULT/NULL；SELECT 的星号已按可见表顺序展开；JOIN 表保持 SQL 顺序；GROUP/ORDER
键保存稳定的关系实例 ID、表 ID、列 ID、ordinal 和类型；UPDATE 目标唯一且赋值类型匹配。
buildPlan 只接受符合这些约束的结果，不再按名字查询 Catalog。

buildPlan 对目标模式、值数/值类型、relation ID、列 ID/ordinal、WHERE/JOIN ON 类型、关系名重复、
GROUP 投影约束、分组键重复、排序键可见性、赋值重复、
表达式空指针和深度进行附加检查，失败返回 Plan / InvalidBoundStatement。
它不会重新推导每个操作符的类型，前置条件仍是输入来自成功的 analyze。
生成期间保留绑定表达式的只读指针和 Catalog 版本，不计算表达式或写入元数据。

## B → 执行层：计划

| AST | 绑定结果 | 计划结构 |
|---|---|---|
| CreateTableStmt | 规范化名称、列定义 | CreateTable |
| DropTableStmt | 规范化表名、IF EXISTS | DropTable |
| InsertStmt | 表模式、按模式顺序的多行值 | Insert |
| SelectStmt | 展开列、JOIN/WHERE/GROUP/ORDER | Project → [Sort] → [GroupBy] → [Filter] → {NestedLoopJoin} → SeqScan |
| SelectStmt（含聚合） | 分组键、聚合项、最终输出名、聚合后排序 | Aggregate → [Filter] → {NestedLoopJoin} → SeqScan |
| UpdateStmt | 目标列、已定型 RHS、可选 BOOL 条件 | Update → [Filter] → SeqScan |
| DeleteStmt | 表模式、可选 BOOL 条件 | Delete → [Filter] → SeqScan |
| ExplainStmt | 已绑定目标语句、analyze 标志 | Explain → 目标根计划 |

PlanNode 的 output 是有序业务列模式，carries_row_id 是内部行标识属性。
buildPlan 产生的 SeqScan 初始输出全表列，并携带 relation_id/relation_name；优化后还可通过
`columns` 保存精确扫描列。Filter 保留优化后子节点的模式和行标识。
Project 输出选择表达式及列别名，可有重复名称，并处理 DISTINCT 和 OFFSET/LIMIT。
NestedLoopJoin 按 join type 执行 INNER/LEFT/RIGHT/FULL：仅 ON 为 TRUE 的组合行匹配，
外连接未匹配侧填 NULL；输出业务列始终是左模式后接右模式。多个 JOIN 按 SQL 顺序形成左深树。
自连接的多个扫描共享 table_id，但 relation_id 不同；执行层按 relation_id 和 column_id 定位值。
GroupBy 用于没有聚合调用的查询，按 keys 去重，输出恰好为分组键顺序；分组比较中两个 NULL
属于同一组。Sort 保留输入模式，按 items 顺序比较，ASC/DESC 分别表示升/降序；ASC 把
NULL 放在非 NULL 之后，DESC 把 NULL 放在非 NULL 之前，相同键之间的最终顺序未定义。
Sort 位于 Project 下方，因此能读取未投影的隐藏列和计算排序表达式。
非空值只在同一列类型内比较：INT/FLOAT 按数值，VARCHAR 按原始字节字典序，BOOL 按
FALSE 小于 TRUE；模式已固定列类型，因此 Sort 不执行跨类型转换。
Update/Delete 的输入必须带行标识；根节点业务输出为空。
RowId 的具体存储格式留给执行/存储层，B 只声明是否需要传递，不假定页号或槽号。

执行结果约定：SELECT 返回按 output 排列的记录；CREATE/DROP 返回成功状态；
INSERT/UPDATE/DELETE 返回影响行数（不作为 PlanNode.output 的业务列）。
普通 EXPLAIN 返回单列 `QUERY PLAN` 文本树且不调用目标计划；EXPLAIN ANALYZE
先执行目标计划，再在各行附加 actual rows/time/loops。其中 time 是包含子算子的累计墙钟时间。
INSERT 可多行；UPDATE/DELETE 按唯一行标识定位目标记录。
UPDATE 全部 RHS 在写入前求值，例如 SET a=b,b=a 交换旧值。
BoundUpdate/UpdatePlan 中每个 RHS 都是对原表列的引用；生成器不会把前一个赋值
替换进后一个 RHS。执行层必须先对旧记录计算全部新值，再一次性写回。
表达式按左子节点先求值；AND/OR 从左向右短路。执行层检查整数溢出、INT/FLOAT 除零；
字符串按值判等；LIKE 中 `%` 匹配任意码点序列、`_` 匹配一个码点。
表达式使用 SQL 三值逻辑，Filter/Join/HAVING 只保留 TRUE。

当前生成器固定使用上述树结构，SELECT * 也保留 Project；省略对应子句才省略相应算子。
恒真/恒假条件在 buildPlan 输出中保留，需显式调用 optimizePlan 优化。未优化 SeqScan 读取全部列；
优化后的 `columns=nullopt` 仍表示全列，有值时表示精确列集合，空集合表示不物化业务列。
修改输入行标识通过 Filter 原样传递；Project 和修改根不暴露内部行标识。

## B 内部：规则优化

optimizePlan 接受成功 analyze → buildPlan 得到的计划，也接受自身的成功输出。
buildPlan 不自动调用优化器，调用方可以保存并打印前后两个计划。
优化器不读 Catalog、不执行记录、不改变输入树；复用未改动的只读节点，
只为改动的表达式及其祖先创建新节点。Catalog 版本、输出列顺序/重复列、
表列 ID/ordinal、UPDATE 的旧行引用以及修改所需的 RowId 均保留。

当前流水线依次执行安全常量折叠与布尔化简、谓词下推、列裁剪：

- 常量 INT/FLOAT 算术/比较、字符串和 BOOL 判等、BOOL 逻辑运算可折叠。
  SQL 语法已支持 TRUE/FALSE 字面量；NULL 不参与折叠。
- 整数运算先检查边界，除零、溢出则保留原运算及操作符范围，交给执行层按需求值时报错。
- AND/OR 遵守左到右短路；FALSE AND x、TRUE OR x 可直接化简。
  TRUE AND x、FALSE OR x、x AND TRUE、x OR FALSE 可替换为 x。
  x AND FALSE、x OR TRUE 保留左侧求值，避免吞掉错误；不重排谓词。
- Filter 的条件折叠为 TRUE 后用输入节点替换；FALSE Filter 保留。优化器递归穿过
  GroupBy/Aggregate/Sort 和 Explain 的目标计划，并折叠 NestedLoopJoin 的 ON 表达式，
  但不删除恒真 JOIN 或改变连接顺序。
- 谓词下推只拆分 WHERE 的 AND 合取项。INNER 可推向任一单侧输入；LEFT 只推左侧，RIGHT
  只推右侧，FULL 不推。常量、跨关系条件留在原位。含算术/取负的条件或 ON 可能报运行期错误，
  优化器保留其求值顺序；危险合取项之后的条件也不提前。
- 列裁剪从根向下传递必需的 `BoundColumnRef`，将投影、Filter、JOIN ON、分组、聚合、HAVING
  和排序依赖合并后，在 SeqScan 按原模式顺序输出唯一列。COUNT(*) 可输出零业务列；DELETE
  仅需条件列和独立 RowId；UPDATE 为旧行复制和最终约束校验保留完整表列。
- 不删除 Update/Delete 根，不选择索引，也不基于统计信息改变连接顺序或算法。

入口附加检查空节点、访问路径深度（最多 256 层）、Filter 的 BOOL 条件及输出/RowId
透传、修改输入的 RowId。失败返回 Plan / InvalidPlan，不返回部分优化结果。
这些检查不是完整计划验证或重新类型检查；被短路跳过的子树不会被遍历。
调用方必须遵守上述有效输入前置条件，不能用此入口验证任意手工构造的计划。

## 计划文本展示

formatPlan 消费成功 buildPlan 或 optimizePlan 产生的计划，输出确定的 UTF-8 调试文本：首行为模式版本，
后续以两空格缩进表示父子关系，每个节点显示参数、output 和 row_id。
列名从计划自带模式读取，不访问 Catalog；表达式使用括号保留结构。
字符串引号翻倍，换行/制表符/反斜杠显示为转义文本。
该格式用于 C++ 阅读和测试，不是 SQL 源码或可反序列化协议。SQL EXPLAIN
最终面向用户的 `QUERY PLAN` 文本由 Java 执行层从同一 JSON 计划树生成，ANALYZE 统计也只能在该层填充。

## 示例及限制

`examples/contracts.cpp` 手工创建五组 AST、绑定结果和计划，对结构约束做检查，
打印每类的对接摘要。它没有调用 analyze/buildPlan，不是编译器或执行器。
其中 CREATE 使用建表前快照；其余语句使用模拟建表成功后的固定快照。
示例涵盖 INSERT 重排、SELECT 过滤、UPDATE 旧列值表达式和 DELETE 行标识。

`examples/semantic.cpp` 则调用真实 analyze：分析 CREATE → 显式注册模式 →
分析 INSERT/SELECT。演示产生绑定结果，不构造计划、不写入或查询数据记录。

`examples/plans.cpp` 贯通基础手工 AST → analyze → buildPlan → formatPlan，
仅在 CREATE 之后显式注册测试模式。INSERT/UPDATE/DELETE 不修改记录，SELECT 不返回数据行。

`examples/optimizer.cpp` 使用真实 SQL 串联 lex/parse/analyze/buildPlan/optimizePlan，
打印优化前后文本树。测试专用参考求值器在 tests/optimizer 下，不属于产品执行接口。

`tests/integration/scaffold_smoke.cpp` 已改为真实 SQL → lex → parse → analyze → buildPlan
兼容性测试，CREATE 后由测试驱动显式注册模式；普通库调用没有自动注册副作用。
`minisql` 命令保留 A 的标准输入→Token/AST 调试行为，完整库链路与命令行展示范围分别验收。

## 聚合绑定和计划补充

BoundSelect.aggregate_items 非空表示聚合查询，按最终 SELECT 顺序保存 BoundColumnRef、
BoundAggregate 或 BoundExprPtr。后者让 `SUM(id)+1` 这类表达式保留完整树。
aggregate_order_by 的 key 为输出序号（别名）、分组列引用或聚合表达式。
output_names 必须与 aggregate_items 等长；旧 columns 只保留该查询中的普通列引用，
不再用作聚合查询的输出布局。普通查询继续使用 columns/order_by。

AggregatePlan 保存 group_keys/items/order_by/having/input 以及 distinct/limit/offset，是查询根节点，负责分组、聚合、
HAVING、最终投影、排序、去重及分页。它直接读取 JOIN/Filter 后的明细，不经过旧 GroupBy 去重，避免丢失
重复输入行。其 output 保存最终名字和类型，carries_row_id=false。优化器可优化其输入，
但不能因输入为空而删掉全表 Aggregate：全表空输入仍须输出 COUNT=0 的一行。
详细类型、NULL、空输入和支持范围以 grammar.md 0.25 为准；JSON 字段见 json-plan-protocol.md。

## 维护责任

- B：共享类型、本文、文法语义约定；维护语义分析、计划生成与优化。
- A：遵循文法生成 AST，保留源码位置，提交 SQL→AST 联调测试。
- Catalog/执行层：遵循模式、版本、行标识和执行规则。
- 0.1：定义 AST、Catalog、绑定结果和计划的初版契约。
- 0.2：补充 A 的 Token、lex/parse 接口；B 原签名保持不变，补上占位实现；
  新增 NotImplemented 错误码。grammar.md 的语言范围仍为 0.1，没有新增 SQL 语法。
- 0.3：增加 MemoryCatalog；实现 CREATE/INSERT/SELECT 语义；增加 InvalidAst、
  ExpressionTooDeep 错误码和确定的诊断顺序。原 AST、Bound 和 analyze 签名保持不变。
- 0.4：完成 UPDATE/DELETE 语义、五类逻辑计划生成；新增 formatPlan 展示接口。
  复用现有 InvalidBoundStatement 错误码，AST/Bound/Plan 数据结构和语法范围保持不变。
- 0.5：合入 A 的 Lexer/Parser/调试入口与测试；修复 EOF 处块注释、逻辑操作符位置，
  增加 EOF 及 Parser 深度检查；公共签名、共享类型和 MiniSQL 子集保持一致。
- 0.6：增加 optimizePlan 及 InvalidPlan，明确安全常量折叠、短路化简和恒真 Filter 消除。
  原入口及 AST/Bound/Plan 结构不变，语言文法仍为 0.1。
- 0.7：合入 A version2 的 FLOAT/BOOL/NULL、限定名、JOIN/GROUP/ORDER 和 AST 展示优化。
  B 完成标量类型与单表限定名适配；尚无计划契约的扩展子句统一返回 UnsupportedFeature。
- 0.8：B 增加多表作用域、歧义诊断、BoundJoin/BoundOrderBy，以及 NestedLoopJoin、
  GroupBy、Sort 计划节点；明确无聚合分组、隐藏排序列和执行层行布局契约。
- 0.9：增加表/列别名和 ORDER BY 输出别名；以 relation_id 区分同一物理表的自连接实例，
  JSON 协议保持版本 1 并为旧计划保留 tableId 回退。

- 0.10：添加聚合 AST/Bound/AggregatePlan，定义聚合后投影排序和空输入规则。

- 0.11：整合 feature-zhangbo，统一 A 的新 AST 和 B Aggregate；IS NULL/IS NOT NULL 返回 BOOL，
  UPDATE/DELETE 别名通过单表作用域绑定且 relation_id=0；未实现的 AST 标记显式返回 UnsupportedFeature。
- 0.12：完成上述 A 扩展的 B 侧实现。Bound/Plan/JSON 保存 SELECT/HAVING/ORDER 表达式、
  DISTINCT/分页、外连接类型、约束元数据、多行 INSERT 和 DROP；Java 引擎实现执行与写入原子检查。
- 0.13：新增 ExplainStmt/BoundExplain/ExplainPlan 端到端契约。普通 EXPLAIN 仅展示优化计划；
  ANALYZE 通过 Java 统一分派点执行目标并采集 actual rows、包含子树的 time 和 loops。
- 0.14：SeqScanPlan 新增可选精确列集合；优化器加入外连接安全的谓词下推和自顶向下列依赖
  裁剪。JSON/Java 保持缺失或 null 表示全列，并支持空数组的零业务列扫描。
