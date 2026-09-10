# 模块接口契约 0.8

本文定义 A、B、Catalog 与执行层的衔接。当前 MemoryCatalog、五类语句语义分析、
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

`ast.hpp` 定义 CreateTableStmt、InsertStmt、SelectStmt、UpdateStmt、DeleteStmt。
Statement 保存整条语句范围；Identifier 保存原始拼写及精确范围。
一元/二元表达式另存运算符范围，便于把类型错误定位到操作符。

SELECT 使用 `variant<AllColumns, vector<Identifier>>` 区分星号和列清单。
INSERT 使用 optional 列清单区分省略和显式给定，显式清单不得为空。
SelectStmt 追加 group_by、order_by、joins，并为旧的三字段聚合初始化提供空默认值。
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
std::shared_ptr<const CatalogSnapshot> snapshot() const;
```

createTable 要求名称符合 grammar.md 的基本标识符规则；它统一大小写、检查重复表/列、
空列定义和非法列类型，失败不修改模式、不消耗 ID。显式注册无源码来源，失败使用
Execution 阶段诊断及空范围；这不代表完整执行引擎已经实现。
成功注册分配表 ID（从 1 开始）、表内列 ID（从 1 开始）并递增模式版本。
snapshot 复制名称索引并共享只读模式，旧快照不会看到新注册表，也可以比容器活得更久。
版本仅在同一 Catalog 实例的历史中比较；该内存容器供第一阶段单线程使用。

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
NotImplemented 保留为后续开发状态错误码，当前五类语句的四个入口不再返回占位结果。
五类基础语句的 analyze/buildPlan 均返回真实结果或诊断；表达式支持 INT/FLOAT 同类型
算术与比较、VARCHAR/BOOL 判等，以及 AND/OR/NOT。
Diagnostic 包含阶段、稳定错误码、可读消息、SourceSpan。
范围按字节、从 1 开始的行列、左闭右开定义；未知范围用 optional 表示，
禁止把未知范围伪装成第 1 行。正常 A 输入必须提供真实范围。

诊断按确定顺序返回：先检查表，再按 SQL 顺序检查列；INSERT 再检查值数、
完整列覆盖、逐值类型；SELECT 最后检查 WHERE。表达式按左子树、右子树、
当前操作符的顺序检查；静态检查不会因为 AND/OR 的运行时短路而跳过某一子树。
UPDATE 先查表，再按赋值顺序检查目标列、重复目标、RHS 表达式及类型，最后检查 WHERE；
DELETE 查表后检查 WHERE。空赋值列表/空 RHS 属于外部 AST 结构错误，报 InvalidAst。
SELECT 按 FROM、各 JOIN 表和 ON、投影、WHERE、GROUP BY、ORDER BY 的顺序绑定。
UPDATE/DELETE 与 SELECT 共用布尔条件检查，WHERE 省略合法，存在时必须为 BOOL。
优先使用名称/操作符/值自身范围，缺失时回退到表达式或语句范围，最终仍可为空。
必需表达式子节点为空时报 InvalidAst；表达式路径超过 256 个节点时报 ExpressionTooDeep。
绑定只推导类型，不求值，所以类型合法的除零或溢出表达式保留给执行层报告。
INSERT 的 NULL 允许写入任意类型列，执行层需为记录提供空值表示；NULL 参与表达式时
因尚无三值逻辑而报 InvalidOperandType。限定名按表名解析；未限定列名在全部可见表中
查找，命中多个表时返回 AmbiguousColumn。同表重复 JOIN 在别名功能加入前返回 DuplicateTable。
JOIN ON 必须为 BOOL，否则返回 JoinConditionNotBoolean。无聚合 GROUP BY 要求所有投影列
和排序列都属于分组键，重复键或不满足约束返回 InvalidGrouping。

绑定结果约束：全部列引用已解析；WHERE/JOIN ON 为 BOOL；运算符合法；INSERT 值按
表列顺序重排；SELECT 的星号已按可见表顺序展开；JOIN 表保持 SQL 顺序；GROUP/ORDER
键保存稳定的表 ID、列 ID、ordinal 和类型；UPDATE 目标唯一且赋值类型匹配。
buildPlan 只接受符合这些约束的结果，不再按名字查询 Catalog。

buildPlan 对目标模式、值数/值类型、列 ID/ordinal、WHERE/JOIN ON 类型、JOIN 表重复、
GROUP 投影约束、分组键重复、排序键可见性、赋值重复、
表达式空指针和深度进行附加检查，失败返回 Plan / InvalidBoundStatement。
它不会重新推导每个操作符的类型，前置条件仍是输入来自成功的 analyze。
生成期间保留绑定表达式的只读指针和 Catalog 版本，不计算表达式或写入元数据。

## B → 执行层：计划

| AST | 绑定结果 | 计划结构 |
|---|---|---|
| CreateTableStmt | 规范化名称、列定义 | CreateTable |
| InsertStmt | 表模式、按模式顺序的值 | Insert |
| SelectStmt | 展开列、JOIN/WHERE/GROUP/ORDER | Project → [Sort] → [GroupBy] → [Filter] → {NestedLoopJoin} → SeqScan |
| UpdateStmt | 目标列、已定型 RHS、可选 BOOL 条件 | Update → [Filter] → SeqScan |
| DeleteStmt | 表模式、可选 BOOL 条件 | Delete → [Filter] → SeqScan |

PlanNode 的 output 是有序业务列模式，carries_row_id 是内部行标识属性。
SeqScan 第一阶段输出全表列；Filter 保留子节点的模式和行标识。
Project 输出选择列，可有重复名称，丢弃内部行标识。
NestedLoopJoin 执行内连接：对左输入的每行依次扫描右输入，仅输出 ON 为 TRUE 的组合行；
输出业务列是左模式后接右模式。多个 JOIN 按 SQL 顺序形成左深树，当前不选择其他连接算法。
GroupBy 在聚合函数尚未加入时按 keys 去重，输出恰好为分组键顺序；分组比较中两个 NULL
属于同一组。Sort 保留输入模式，按 items 顺序比较，ASC/DESC 分别表示升/降序；ASC 把
NULL 放在非 NULL 之后，DESC 把 NULL 放在非 NULL 之前，相同键之间的最终顺序未定义。
Sort 位于 Project 下方，因此能读取未投影的隐藏排序列。
非空值只在同一列类型内比较：INT/FLOAT 按数值，VARCHAR 按原始字节字典序，BOOL 按
FALSE 小于 TRUE；模式已固定列类型，因此 Sort 不执行跨类型转换。
Update/Delete 的输入必须带行标识；根节点业务输出为空。
RowId 的具体存储格式留给执行/存储层，B 只声明是否需要传递，不假定页号或槽号。

执行结果约定：SELECT 返回按 output 排列的记录；CREATE 返回成功状态；
INSERT/UPDATE/DELETE 返回影响行数（不作为 PlanNode.output 的业务列）。
INSERT 单行；UPDATE/DELETE 按唯一行标识定位目标记录。
UPDATE 全部 RHS 在写入前求值，例如 SET a=b,b=a 交换旧值。
BoundUpdate/UpdatePlan 中每个 RHS 都是对原表列的引用；生成器不会把前一个赋值
替换进后一个 RHS。执行层必须先对旧记录计算全部新值，再一次性写回。
表达式按左子节点先求值；AND/OR 从左向右短路。执行层检查整数溢出、INT/FLOAT 除零；
字符串按原始字节判等。记录层需要保存 INSERT NULL，但表达式暂不存在三值逻辑。

当前生成器固定使用上述树结构，SELECT * 也保留 Project；省略对应子句才省略相应算子。
恒真/恒假条件在 buildPlan 输出中保留，需显式调用 optimizePlan 优化。SeqScan 读取全部列，修改输入行标识通过 Filter
原样传递；Project 和修改根不暴露内部行标识。

## B 内部：规则优化

optimizePlan 接受成功 analyze → buildPlan 得到的计划，也接受自身的成功输出。
buildPlan 不自动调用优化器，调用方可以保存并打印前后两个计划。
优化器不读 Catalog、不执行记录、不改变输入树；复用未改动的只读节点，
只为改动的表达式及其祖先创建新节点。Catalog 版本、输出列顺序/重复列、
表列 ID/ordinal、UPDATE 的旧行引用以及修改所需的 RowId 均保留。

首版包含安全常量折叠、布尔化简和恒真 Filter 消除：

- 常量 INT/FLOAT 算术/比较、字符串和 BOOL 判等、BOOL 逻辑运算可折叠。
  SQL 语法已支持 TRUE/FALSE 字面量；NULL 不参与折叠。
- 整数运算先检查边界，除零、溢出则保留原运算及操作符范围，交给执行层按需求值时报错。
- AND/OR 遵守左到右短路；FALSE AND x、TRUE OR x 可直接化简。
  TRUE AND x、FALSE OR x、x AND TRUE、x OR FALSE 可替换为 x。
  x AND FALSE、x OR TRUE 保留左侧求值，避免吞掉错误；不重排谓词。
- Filter 的条件折叠为 TRUE 后用输入节点替换；FALSE Filter 保留。优化器递归穿过
  GroupBy/Sort，并折叠 NestedLoopJoin 的 ON 表达式，但不删除恒真 JOIN 或改变连接顺序。
  不删除 Update/Delete 根，不进行列裁剪、索引选择或代价优化。

入口附加检查空节点、访问路径深度（最多 256 层）、Filter 的 BOOL 条件及输出/RowId
透传、修改输入的 RowId。失败返回 Plan / InvalidPlan，不返回部分优化结果。
这些检查不是完整计划验证或重新类型检查；被短路跳过的子树不会被遍历。
调用方必须遵守上述有效输入前置条件，不能用此入口验证任意手工构造的计划。

## 计划文本展示

formatPlan 消费成功 buildPlan 或 optimizePlan 产生的计划，输出确定的 UTF-8 调试文本：首行为模式版本，
后续以两空格缩进表示父子关系，每个节点显示参数、output 和 row_id。
列名从计划自带模式读取，不访问 Catalog；表达式使用括号保留结构。
字符串引号翻倍，换行/制表符/反斜杠显示为转义文本。
该格式用于阅读和测试，不是 SQL 源码或可反序列化协议；也不是 SQL EXPLAIN 语法支持。

## 五类示例及限制

`examples/contracts.cpp` 手工创建五组 AST、绑定结果和计划，对结构约束做检查，
打印每类的对接摘要。它没有调用 analyze/buildPlan，不是编译器或执行器。
其中 CREATE 使用建表前快照；其余语句使用模拟建表成功后的固定快照。
示例涵盖 INSERT 重排、SELECT 过滤、UPDATE 旧列值表达式和 DELETE 行标识。

`examples/semantic.cpp` 则调用真实 analyze：分析 CREATE → 显式注册模式 →
分析 INSERT/SELECT。演示产生绑定结果，不构造计划、不写入或查询数据记录。

`examples/plans.cpp` 贯通五类手工 AST → analyze → buildPlan → formatPlan，
仅在 CREATE 之后显式注册测试模式。INSERT/UPDATE/DELETE 不修改记录，SELECT 不返回数据行。

`examples/optimizer.cpp` 使用真实 SQL 串联 lex/parse/analyze/buildPlan/optimizePlan，
打印优化前后文本树。测试专用参考求值器在 tests/optimizer 下，不属于产品执行接口。

`tests/integration/scaffold_smoke.cpp` 已改为真实 SQL → lex → parse → analyze → buildPlan
兼容性测试，CREATE 后由测试驱动显式注册模式；普通库调用没有自动注册副作用。
`minisql` 命令保留 A 的标准输入→Token/AST 调试行为，完整库链路与命令行展示范围分别验收。

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
