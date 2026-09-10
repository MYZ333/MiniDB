# 团队 A 第一部分：本地合并与兼容性记录

## 1. 合并范围

来源为 `A version/DBcompiler/`，采用文件级合并，保留当前本地未提交的 B 工作。
原始交付副本保留用于对照，没有把其中的嵌套 .git、.vs 或 Zone.Identifier 合入主工程。

合入五个文件的新增实现：

- `src/lexer/lexer.cpp`：实际词法分析器。
- `src/parser/parser.cpp`：实际递归下降解析器。
- `app/main.cpp`：标准输入 SQL 的 Token/AST 调试入口。
- `tests/lexer/lexer_tests.cpp` 和 `tests/parser/parser_tests.cpp`：A 的模块测试。

交付副本中的 Token/AST/lex/parse 公共类型与本地一致。它的 B 文件、common 错误码、
README 和接口文档基于早期骨架；本次保留本地 B 实现及较新的契约，按新增内容合并
CMake 和检查脚本。文本统一采用 LF，原始副本不修改。

## 2. 兼容修复及原因

| 问题 | 修复 | 验证 |
|---|---|---|
| `/* comment */` 恰好在 EOF 闭合，仍因 atEnd 被误报未闭合 | 增加明确 closed 标志，区分是否消费到闭合符 | 纯注释、语句尾注释、后续未闭合注释 |
| AND/OR 的 previous() 与右侧解析位于同一函数调用参数中 | 先保存操作符，再解析右侧 | AST 范围及 B 的实际操作符错误位置 |
| Parser 遇到提前的 EOF 会忽略后续 Token，缺 EOF 的输入不符合假设 | 入口验证唯一且末尾的 EOF | 空流、缺 EOF、重复 EOF、EOF 后有 Token |
| 深括号/NOT/负号可在 B 的深度检查前耗尽 Parser 栈，长加法链也能形成深树 | 嵌套计数 RAII 守卫 + 构造时记录 AST 高度，限 256 | 256/257 边界、300 NOT、长左结合链、失败后再次解析 |

操作符位置问题与 C++ 参数求值顺序有关，在某些编译器上原实现可能恰好给出正确位置，
修复后行为与求值顺序无关。没有更改逻辑运算的优先级。

Parser 的 depths_ 是私有“节点地址 → 树高”元数据，不增加公共 AST 字段。
每次构造节点由子节点高度计算父高度；每条语句开始清空索引。
NestingGuard 在进入递归分支时递增计数，退出（包括异常）时递减，防止计数泄露。
原 Parser 内部使用 Diagnostic 异常退出递归，run 捕获后仍返回公共 Result 接口。

## 3. 合并后的代码组织

```text
SQL → Lexer::run → TokenStream → Parser::run/statement → AST
    → analyze（本地 B）→ BoundStatement → buildPlan（本地 B）→ LogicalPlan
```

Lexer 负责原始词素与源码范围，Parser 解码字面量并按优先级构造 AST。
B 使用同一 AST，把名称绑定到表列并生成计划，不需要适配器或第二套结构。

app/main.cpp 保留 A 的调试行为，输出 Token 和 AST，不调用 B，不验证表是否存在。
完整链路在 tests/integration/scaffold_smoke.cpp 中验证。测试遇 CREATE 后显式注册
内存模式，模拟执行层的元数据更新；这不是在 analyze/buildPlan 中自动建表。

## 4. 已完成验证

执行 `bash scripts/check.sh`，C++17 严格警告编译（包括 -Werror），结果如下：

| 测试范围 | 数量与结果 |
|---|---|
| A 词法 | 6 组通过，包含原有用例与合并回归 |
| A 语法 | 9 组通过，包含原有用例与合并回归 |
| SQL→Plan 兼容性 | 12 个用例通过 |
| B Catalog | 原有 4 个用例通过 |
| B 语义 | 原有 33 个用例通过 |
| B 计划/打印 | 原有 16 个用例通过 |
| 公共头文件/示例 | 11 个头文件独立包含通过，三个原有示例通过 |

另使用 subprocess 检查 CLI：正常 SQL 返回 0，词法/语法错误返回 1，错误参数返回 2。
所有检查均在当前 Linux 环境用 c++ 运行；环境无可运行的 CMake，CMake/CTest 路径
已同步但未在本机运行，也未宣称完成 MSVC/Clang 的跨编译器验证。

联调覆盖五类语句、优先级、精确源码位置、CRLF、字符串解码、INT64 极值、
名字大小写、重复输出列、INSERT 重排、UPDATE 原值引用、DELETE 无条件扫描、
阶段错误分类和 CREATE 快照转换。没有执行存储读写或测试实际查询结果。

## 5. 本地复现

```bash
bash scripts/check.sh
printf 'SELECT name FROM student WHERE age > 18;\n' | ./build/direct/minisql
./build/direct/scaffold_smoke
```

第一条构建并回归 A/B；第二条观察真实 SQL 的 Token/AST；第三条运行完整链路的
12 个兼容性用例。后续扩展优先同步 grammar.md、接口契约、双方模块测试及这些联调用例。
