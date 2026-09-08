# 模块接口契约 0.2

本文先于算法实现定义 A、B、Catalog 与执行层的衔接。当前实现的是共享数据
结构和可编译示例；lex/parse/analyze/buildPlan 均有可链接的占位定义，
统一返回 NotImplemented，尚未实现编译算法。

## 数据流和所有权

```text
A: SQL → Token → Statement(AST)
                         ↓ analyze(ast, catalog)
B:                 BoundStatement
                         ↓ buildPlan(bound)
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

`token.hpp` 定义文法中的关键字、标识符、整数、字符串、操作符和分隔符。
Token 持有原始词素副本和必填范围；关键字种类已区分，但词素不转小写。
字符串词素包含原始引号与转义；Parser 负责解码字符串和有符号 64 位数值。
负号始终是单独 token，具体解析约定见 grammar.md。

lex 仅在调用期间借用输入，成功结果以唯一 EOF 结尾；EOF 原文为空、范围
为输入结束处的零长度范围。空 SQL 得到仅 EOF 的 TokenStream。
parse 接受符合此约定的 TokenStream；仅 EOF 返回空语句列表。
两者首版均遇首错返回 Diagnostic，不返回部分结果；后续错误恢复另行扩展接口。
AST 自持字符串与子节点，不依赖 TokenStream 的生命周期。

上述为实现后的约定；当前所有输入均返回 NotImplemented（包括空输入）。

## A → B：AST

`ast.hpp` 定义 CreateTableStmt、InsertStmt、SelectStmt、UpdateStmt、DeleteStmt。
Statement 保存整条语句范围；Identifier 保存原始拼写及精确范围。
一元/二元表达式另存运算符范围，便于把类型错误定位到操作符。

SELECT 使用 `variant<AllColumns, vector<Identifier>>` 区分星号和列清单。
INSERT 使用 optional 列清单区分省略和显式给定，显式清单不得为空。
字面量只允许 int64_t/string；语法阶段没有 BOOL 字面量。

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

本阶段只提供抽象接口，示例用固定快照演示接入；内存 Catalog 实现在下一阶段。
CREATE 产生建表描述，不预分配表列 ID，不调用 createTable。
成功执行后，由执行层负责修改真实 Catalog 并递增版本。
因此多语句由上层按“编译→执行成功→新快照→下一条”驱动。

## B 的调用接口和错误

```cpp
Result<BoundStatement> analyze(const Statement&, const CatalogSnapshot&);
Result<LogicalPlan> buildPlan(const BoundStatement&);
```

Result 为 `variant<T, Diagnostic>`，错误时不返回半成品；使用 `get_if` 或
`holds_alternative` 检查结果。首版每条语句仅报告首个语义错误。
当前占位入口返回所属阶段的 NotImplemented；这是开发状态，不是非法 SQL 诊断。
Diagnostic 包含阶段、稳定错误码、可读消息、SourceSpan。
范围按字节、从 1 开始的行列、左闭右开定义；未知范围用 optional 表示，
禁止把未知范围伪装成第 1 行。正常 A 输入必须提供真实范围。

绑定结果约束：全部列引用已解析；WHERE 为 BOOL；运算符合法；INSERT 值按
表列顺序重排；SELECT 的星号已展开；UPDATE 目标唯一且赋值类型匹配。
buildPlan 只接受符合这些约束的结果，不再按名字查询 Catalog。

## B → 执行层：计划

| AST | 绑定结果 | 计划结构 |
|---|---|---|
| CreateTableStmt | 规范化名称、列定义 | CreateTable |
| InsertStmt | 表模式、按模式顺序的值 | Insert |
| SelectStmt | 展开且绑定的列、可选 BOOL 条件 | Project → [Filter] → SeqScan |
| UpdateStmt | 目标列、已定型 RHS、可选 BOOL 条件 | Update → [Filter] → SeqScan |
| DeleteStmt | 表模式、可选 BOOL 条件 | Delete → [Filter] → SeqScan |

PlanNode 的 output 是有序业务列模式，carries_row_id 是内部行标识属性。
SeqScan 第一阶段输出全表列；Filter 保留子节点的模式和行标识。
Project 输出选择列，可有重复名称，丢弃内部行标识。
Update/Delete 的输入必须带行标识；根节点业务输出为空。
RowId 的具体存储格式留给执行/存储层，B 只声明是否需要传递，不假定页号或槽号。

执行结果约定：SELECT 返回按 output 排列的记录；CREATE 返回成功状态；
INSERT/UPDATE/DELETE 返回影响行数（不作为 PlanNode.output 的业务列）。
INSERT 单行；UPDATE/DELETE 按唯一行标识定位目标记录。
UPDATE 全部 RHS 在写入前求值，例如 SET a=b,b=a 交换旧值。
表达式按左子节点先求值；AND/OR 从左向右短路。执行层检查整数溢出、除零；
字符串按原始字节判等。不支持 NULL，暂不存在三值逻辑。

## 五类示例及限制

`examples/contracts.cpp` 手工创建五组 AST、绑定结果和计划，对结构约束做检查，
打印每类的对接摘要。它没有调用 analyze/buildPlan，不是编译器或执行器。
其中 CREATE 使用建表前快照；其余语句使用模拟建表成功后的固定快照。
示例涵盖 INSERT 重排、SELECT 过滤、UPDATE 旧列值表达式和 DELETE 行标识。

## 维护责任

- B：共享类型、本文、文法语义约定；后续实现分析和计划生成。
- A：遵循文法生成 AST，保留源码位置，提交 SQL→AST 联调测试。
- Catalog/执行层：遵循模式、版本、行标识和执行规则。
- 0.1：定义 AST、Catalog、绑定结果和计划的初版契约。
- 0.2：补充 A 的 Token、lex/parse 接口；B 原签名保持不变，补上占位实现；
  新增 NotImplemented 错误码。grammar.md 的语言范围仍为 0.1，没有新增 SQL 语法。
