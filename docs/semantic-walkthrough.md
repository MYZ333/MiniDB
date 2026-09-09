# B 第二部分代码讲解：从 AST 到绑定结果

本文主要讲解第二部分的 CREATE、INSERT、SELECT 语义代码。教学示例手工构造 AST；
A 现已合入，可从真实 SQL 构造同一结构。输出是 BoundStatement。后续已完成 UPDATE/DELETE 和计划生成，
对应内容见 [第三部分讲解](planner-walkthrough.md)；本文的 semantic_example 仍只演示语义分析。

## 1. 三个模块如何协作

| 模块 | 关键代码 | 负责的问题 |
|---|---|---|
| 内存 Catalog | `src/catalog/memory_catalog.cpp` | 模式里有哪些表列？如何获得稳定快照？ |
| 类型规则 | `src/semantic/type_rules.cpp` | 这个操作符接受哪些类型，结果是什么类型？ |
| 语义分析器 | `src/semantic/analyzer.cpp` | AST 名称指向哪一列，整个语句是否合法？ |

只有 memory_catalog.hpp 是新增公共接口。type_rules.hpp 是语义模块的私有头文件，
Parser 或计划生成器无需包含它。analyze 的公共签名、AST 与 Bound 数据布局都保持不变。

## 2. Catalog 为什么要区分容器与快照

MemoryCatalog 是可显式注册表的容器；MemorySnapshot 是它产生的只读视图，
具体类藏在 cpp 中，对外只暴露已有 CatalogSnapshot 接口。

createTable 先归一化名称，再检查表名、空列、重复列、列类型。全部通过后才
创建 TableSchema、加入索引、分配下一表 ID 并递增版本。错误返回不改变容器。

snapshot 复制“名称 → 模式指针”的索引，模式本身由 shared_ptr<const TableSchema>
共享，不复制所有列定义。新表注册只改变容器的新索引，旧快照不会突然出现新表。
即使容器销毁，已有快照和绑定结果仍持有有效模式。

讲解时可以举这个例子：版本 0 的空快照 → 注册 student → 新快照版本 1。
老快照仍是空的。analyze(CREATE) 不调用注册，因此分析成功后版本仍是 0。

## 3. analyze 的入口如何分派

每次 analyze 创建一个局部 Analyzer，保存只读 Catalog 引用和语句范围。
run 用 std::visit 观察 Statement 当前是哪种 variant，再调用对应的
bindStatement 重载。分析器没有跨语句的可变状态。

三个辅助函数值得先看：

- findTable：归一化原始表名，查 Catalog；不存在时定位到原始名称。
- resolveColumn：在表模式中查列，返回 TableId、ColumnId、ordinal 和类型。
  ordinal 是表内记录位置，不能用它代替 ColumnId，也不能用输出列位置代替它。
- success：把语义结果和 Catalog 版本装进 BoundStatement。

Result 是成功值与 Diagnostic 的 variant。每次调用子步骤后先检查错误，再取成功值；
任一步失败立即返回，不向调用方暴露部分绑定结果。预期 SQL 错误不使用异常控制流程。

## 4. 三类语句逐步讲解

**CREATE**：检查表是否已存在 → 检查列定义和重名 → 生成规范化 ColumnSpec。
此时不分配数据库 ID，因为数据库还没有实际创建表。内存 Catalog 也会校验定义，
是因为注册 API 可以被测试或未来执行层独立调用，不能假定所有调用都已经过 analyze。

**INSERT**：先查表，把 SQL 列清单转换为目标列引用；省略清单时使用所有表列。
检查重复列、数量、完整覆盖，再检查每个值类型。最后执行
`values[targets[i].ordinal] = scalar(stmt.values[i].value)`。

例如模式为 `(id, name, age)`，输入为 `(name, age, id)`：目标 ordinal 是
`[1, 2, 0]`，输入值 `['Alice', 20, 1]` 最终变成 `[1, 'Alice', 20]`。
这种重排发生在绑定阶段，后续执行层直接按模式顺序写入。

**SELECT**：展开星号或逐项绑定选择列，保留原顺序和重复项，再分析 WHERE。
过滤表达式通过共用的 bindWhere 检查完整表模式，因此 `SELECT name ... WHERE age > 18` 合法，
age 不必出现在输出列表中。WHERE 缺省是空指针，存在时必须被推导为 BOOL。

## 5. bindExpr 怎样检查表达式

bindExpr 按 AST 的结构递归，不重新计算优先级；优先级结构应由 A 的 Parser 保证。

1. 名称节点调用 resolveColumn，产生带 ID 和类型的 BoundColumnRef。
2. 字面量节点保留原值，根据 int64_t/string 确定 INT/VARCHAR。
3. 一元节点先绑定操作数，再用 unaryResult 检查 NOT 或负号。
4. 二元节点先绑定左侧、再绑定右侧，最后用 binaryResult 检查两个类型。

例如 `age + 1 > 18`：age 与 1 均为 INT，加法结果为 INT；与 18 比较得到 BOOL，
因此能够作为 WHERE。`age + 'abc'` 则在加号处报告 InvalidOperandType，
消息说明 INT 和 VARCHAR 不支持相加。

这里仅推导类型，不做常量折叠，也不执行运算。`1 / 0 = 0` 类型合法，除零由
执行层负责；OR 左侧即使是恒真，右侧不存在的列也必须在静态检查时报告。

每个 BoundExpr 都保留范围；操作符另保留精确位置。输入没有细位置时回退到
父表达式/语句位置，不凭空生成第 1 行。空的必需子节点返回 InvalidAst；
超过 256 层返回 ExpressionTooDeep。两项用于防御手工或外部 AST 的错误结构。

## 6. 怎样运行并解释证据

```bash
bash scripts/check.sh
./build/direct/semantic_example
```

semantic_example 按顺序执行真实语义分析和显式模式注册，输出应包含：

```text
CREATE analyzed: student; catalog version still 0
INSERT bound in schema order: 1, Alice, 20
SELECT bound: output ordinal 1; WHERE column ID 3; predicate BOOL=1
```

4 个 Catalog 用例检查模式规范化、快照隔离、失败注册原子性和生命周期。
语义测试现扩展至 33 个用例，检查五类语句的成功/失败、类型规则、准确范围、深度边界和无副作用。
test_support.hpp 只是测试辅助；实际算法位于 src 中，测试通过公共 analyze 调用它。

当前环境没有 CMake，已用直接构建脚本通过严格警告编译及全部用例；CMake 配置
已同步加入源文件和测试目标，但尚未在本机验证。UPDATE/DELETE 现已复用
resolveColumn、bindExpr 和类型规则，五类计划生成见第三部分讲解。
