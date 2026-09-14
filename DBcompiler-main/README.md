# MiniSQL 编译器项目骨架（A + B）

本项目采用 **C++17 + CMake**，按照 `grammar.md` 和接口契约组织两人的开发。
语言目标为 CREATE TABLE、DROP TABLE、INSERT、SELECT、UPDATE、DELETE，以及条件和标量表达式。
已合入团队成员的 A version2，实现扩展 Lexer、Parser、AST 优化展示和前端调试入口；结合本地 B，
六类语句已通过 **SQL → Token → AST → 语义分析 → 逻辑计划** 联调。
现已增加 B 的规则优化：安全常量折叠、布尔化简、恒真 Filter 消除，并提供前后计划对照。
JOIN、GROUP BY 与 COUNT/SUM/AVG/MIN/MAX、多列 ORDER BY、表/列别名和自连接已完成绑定、计划生成、打印、
优化遍历及 JSON 导出。本目录不读写数据库记录；仓库相邻的 `minidb-engine` 通过 JSON
消费增删改查与高级查询计划。两次合并范围见
[A 第一版合并说明](docs/a-merge-notes.md)和 [A version2 合并说明](docs/a-version2-merge-notes.md)。

已整合 feature-zhangbo：语法与 B 聚合 AST 已统一，新增空值判定和 DML 别名执行。
[整合说明与阅读顺序](docs/zhangbo-merge-notes.md)解释接口冲突的解决方式；
[grammar.md 0.23](grammar.md)给出当前完整执行边界。

## 1. 目录结构

```text
DBcompiler/
├── CMakeLists.txt             # 构建目标及 CTest 注册
├── grammar.md                 # 语言文法、优先级、语义限制，A/B 同步维护
├── docs/
│   ├── interfaces.md          # Token/AST/Catalog/Bound/Plan 契约
│   ├── semantic-walkthrough.md # 名称绑定和类型检查的阅读指南
│   ├── planner-walkthrough.md # UPDATE/DELETE、计划生成与打印讲解
│   ├── advanced-query-walkthrough.md # JOIN/GROUP/ORDER 的绑定与计划讲解
│   ├── aggregate-walkthrough.md # 第三部分：聚合函数的数据流、空输入和执行讲解
│   ├── remaining-features-walkthrough.md # A 扩展在 B/JSON/Java 中的完整数据流
│   ├── optimizer-walkthrough.md # 安全常量计算、树改写与等价性验证讲解
│   ├── json-plan-protocol.md # C++ 到 Java 的 JSON 计划字段约定
│   └── a-merge-notes.md      # A 来源、兼容修复、测试结果与阅读顺序
├── include/minisql/           # 跨模块公共头文件
│   ├── common.hpp            # 基础类型、源码范围、诊断、名字归一化
│   ├── token.hpp             # A：Token 种类、词素、位置
│   ├── lexer.hpp             # A：lex() 声明
│   ├── parser.hpp            # A：parse() 声明
│   ├── ast_optimizer.hpp     # A：展示用 AST 常量折叠接口
│   ├── ast.hpp               # A/B：语句与表达式 AST
│   ├── catalog.hpp           # B：只读 Catalog 快照接口
│   ├── memory_catalog.hpp    # B：显式注册表、获取不可变快照
│   ├── bound.hpp             # B：名称绑定和类型检查后的结构
│   ├── plan.hpp              # B：逻辑算子及输出模式
│   ├── plan_printer.hpp      # B：formatPlan() 文本展示接口
│   ├── optimizer.hpp         # B：optimizePlan() 规则优化接口
│   └── compiler.hpp          # B：analyze()/buildPlan() 声明
├── src/
│   ├── lexer/lexer.cpp       # A：Token、注释、转义、源码位置
│   ├── parser/
│   │   ├── parser.cpp       # A：扩展递归下降、表达式和深度限制
│   │   └── ast_optimizer.cpp # A：语义分析前的展示用 AST 改写
│   ├── catalog/memory_catalog.cpp # B：模式校验、ID 分配、快照实现
│   ├── semantic/
│   │   ├── analyzer.cpp     # B：六类语句绑定、名称解析和类型/分组检查
│   │   └── type_rules.hpp/.cpp # B：私有表达式类型规则
│   ├── planner/
│   │   ├── plan_builder.cpp # B：绑定结果检查、构造逻辑算子树
│   │   └── plan_printer.cpp # B：稳定文本树、表达式及输出模式展示
│   └── optimizer/
│       ├── constant_fold.hpp/.cpp # B：私有安全常量计算
│       └── optimizer.cpp    # B：表达式化简、计划改写、保留原树
├── app/main.cpp              # A 调试入口：Token、原 AST、优化 AST 展示
├── app/plan_json.cpp         # 完整编译流程及 JSON 计划导出入口
├── examples/contracts.cpp    # 手工构造基础 AST、绑定结果和计划
├── examples/semantic.cpp     # 手工 AST 调用真实 analyze 的演示
├── examples/plans.cpp        # 基础 AST → 真实语义分析 → 真实计划生成 → 打印
├── examples/optimizer.cpp    # 真实 SQL 编译 → 优化前后计划对照
├── tests/
│   ├── lexer/lexer_tests.cpp # A：词法及扩展 Token 回归
│   ├── parser/               # A：语法与 9 组 AST 优化测试
│   ├── catalog/catalog_tests.cpp # B：5 个模式/快照行为用例
│   ├── semantic/semantic_tests.cpp # B：语义行为用例
│   ├── test_support.hpp      # 测试断言和手工 AST 辅助，不属于产品 API
│   ├── planner/plan_tests.cpp # B：23 个计划结构与打印用例
│   ├── optimizer/            # B：25 组优化测试及独立参考求值器
│   └── integration/scaffold_smoke.cpp # 17 组真实 SQL → Plan/诊断兼容用例
├── scripts/check.sh          # 无 CMake 时的编译及检查脚本
└── build/                    # 本地构建产物，已忽略
```

`include/` 放双方需要引用的契约，`src/` 放实现。后续只服务某一模块的辅助类
或私有头文件就近放在该模块目录，避免所有细节都成为公共接口。
团队原始交付副本保留在 `A version/DBcompiler/` 和 `A_version2/DBcompiler/` 供溯源，
已从主工程 Git 跟踪和构建中排除。
没有将副本中的嵌套 .git、IDE 缓存或 Windows Zone.Identifier 文件合入代码目录。

## 2. 两人分工

| 工作 | A：词法与语法 | B：语义与计划 |
|---|---|---|
| 输入处理 | lex：关键字、注释、转义、位置、EOF | 不处理字符流 |
| 语法结构 | parse：六类语句、聚合调用、别名、JOIN/GROUP/ORDER、表达式优先级、多语句、AST | 使用 A 提供的 AST，检查聚合类型与分组约束 |
| 名称与类型 | 保留名称原文和源码范围 | Catalog 查询、关系实例/表列绑定、类型检查、INSERT 重排、UPDATE 规则 |
| 计划生成 | 提供准确的 AST | 构造增删改查及 NestedLoopJoin/GroupBy/Aggregate/Sort 计划 |
| 规则优化 | 展示用 AST 折叠，维护原始/优化 AST 对照 | 绑定后计划折叠、布尔化简、恒真 Filter 消除和等价性测试 |
| 错误与测试 | 词法/语法诊断，lexer/parser 测试 | 语义/计划诊断，semantic/planner 测试 |
| 文档 | 文法语法部分、Token 与 AST 接口 | 文法语义部分、Catalog/Bound/Plan 接口；B 汇总维护文档 |
| 联调 | 与 B 共建 SQL→AST→计划用例，维护 app 和 integration | 与 A 协同，负责向执行层说明计划契约 |

AST、common、grammar.md、接口文档及 CMake 为共同协作文件。
改动前说明接口影响，改动时同步类型、文档、样例和测试；不要各自定义第二套 AST。
执行引擎、存储和持久化不属于本次骨架，只通过 Catalog 与计划契约对接。

## 3. 数据流与构建关系

```text
SQL
 └─ A: lex(sql) → TokenStream
        └─ A: parse(tokens) → vector<Statement>
               └─ B: analyze(statement, catalog) → BoundStatement
                      └─ B: buildPlan(bound) → LogicalPlan
                             └─ B: optimizePlan(plan) → LogicalPlan（可选）
                                    └─ 外部执行引擎 → 存储
```

五个入口均返回 `Result<T>`，即成功值或 Diagnostic，调用者遇错应停止该条流程。
lex、parse、analyze 与 buildPlan 均已实现六类语句的对应阶段。
optimizePlan 显式调用，保持版本、输出和 RowId；除零/溢出运算保留给执行层按需求值时报错。
`formatPlan(plan)` 返回可读文本树，展示算子参数、业务输出列、模式版本和行标识属性。
parse 支持多语句，按文法处理优先级，保留实际源码范围。
CREATE 不在编译时修改 Catalog；未来由上层按“编译→执行成功→刷新快照→下一条”驱动。

| CMake 目标 | 内容 | 依赖 |
|---|---|---|
| minisql_contracts | 共享头文件 INTERFACE 库 | 无 |
| minisql_frontend | A 的 Lexer、Parser 静态库 | contracts |
| minisql_backend | B 的 MemoryCatalog、类型规则、Semantic、Planner、Optimizer 静态库 | contracts |
| minisql | A 的 Token/AST 调试入口，当前尚未从 main 调用 B | frontend + backend |
| minisql_plan_json | SQL→优化计划→JSON，供 Java 引擎调用 | frontend + backend |
| contracts_example | 手工结构示例 | contracts |
| semantic_example | 调用真实语义分析的演示 | backend |
| plans_example | 基础语句的 B 侧完整流程和计划打印 | backend |
| optimizer_example | 真实 SQL 编译和优化前后计划打印 | frontend + backend |
| lexer_tests / parser_tests / ast_optimizer_tests | A 的模块测试，仅 BUILD_TESTING 开启时构建 | frontend |
| scaffold_smoke | 真实 SQL→计划/诊断的 14 个联调用例，仅 BUILD_TESTING 开启时构建 | frontend + backend |
| catalog_tests / semantic_tests / plan_tests | 模式、语义、计划行为测试，仅 BUILD_TESTING 开启时构建 | backend |
| optimizer_tests | 优化边界和前后等价性测试，仅 BUILD_TESTING 开启时构建 | frontend + backend |

A 与 B 的库互不依赖，因此可以独立实现和测试。MemoryCatalog 已归入 backend；
语义分析仍只依赖 CatalogSnapshot，可替换为外部数据库的模式快照。
BOOL/FLOAT/NULL、约束 DDL、多行 INSERT、查询表达式、内外连接、GROUP/HAVING、
DISTINCT、ORDER 和 LIMIT 已贯通 B 与 JSON。相邻 Java 引擎执行全部这些节点和字段。

## 4. 构建和运行

需要 C++17 编译器和 CMake 3.16 及以上：

```bash
cmake -S . -B build
cmake --build build
(cd build && ctest --output-on-failure)
printf 'SELECT name FROM student WHERE age > 18;\n' | ./build/minisql
./build/contracts_example
./build/semantic_example
./build/plans_example
./build/optimizer_example
printf 'CREATE TABLE t(id INT); SELECT * FROM t;' | ./build/minisql_plan_json
```

默认开启 BUILD_TESTING；`-DBUILD_TESTING=OFF` 可关闭测试目标。
以上可执行路径适用于默认单配置构建；多配置生成器使用对应 Debug/Release 子目录。
minisql 从标准输入读取 SQL，默认打印 Token Stream、AST 和 Optimized AST；
`--raw-only` 只打印原 AST，`--optimized-only` 只打印优化 AST。
不接收文件参数；无效或多余参数返回 2，词法/语法错误返回 1，成功返回 0。
此命令仅调试前端，不检查表是否存在；完整 A/B 调用见 tests/integration/scaffold_smoke.cpp。

环境没有 CMake 时，在仓库目录运行：

```bash
bash scripts/check.sh
```

该脚本用 c++ 和 ar 分别构建 A/B 静态库和 JSON 导出器，验证全部公共头文件可独立包含，
再编译运行前端空输入检查、示例、A 的词法/语法/AST 优化测试以及 B 的 Catalog、
语义、计划、优化和 SQL 联调用例。
产物位于 `build/direct/`，不会覆盖
CMake 构建文件；可通过 CXX/AR 环境变量指定工具路径。

当前环境未安装 CMake；验证使用上述脚本，CMake/CTest 路径尚未在本机验证。
本次直接编译使用 `-Wall -Wextra -Wpedantic -Werror`；上述测试全部通过。
测试包含快照隔离、失败注册无副作用、名称大小写、列重排、表达式类型、错误位置及深度边界。
新增用例包含 UPDATE 旧值引用、重复赋值、DELETE 条件、修改计划行标识、
输出模式、外部非连续列 ID、无效绑定结果诊断和确定性打印。
新增联调用例验证真实 SQL 的扩展类型、限定名、JOIN/GROUP/ORDER 计划、源码范围和六类语句。
联调驱动显式注册 CREATE 模式，不执行数据库 CRUD。
优化测试另用测试专用参考求值器比较 SELECT/UPDATE/DELETE 的记录结果、影响行数及错误位置，
包含 49 种确定性表达式组合；INT64 边界使用明确预期用例。参考求值器不是产品执行引擎。
当前 24 组计划优化测试及其编译链路通过了 UBSan 检查；复现命令见
[优化测试说明](tests/optimizer/README.md)。

## 5. 开发与阅读顺序

1. 共同阅读 [grammar.md](grammar.md) 和 [接口文档](docs/interfaces.md)，先理解范围、
   输入输出、错误、名称大小写和源码位置规则。
2. A 按 token → Lexer::run → Parser::statement → 分层表达式函数 → ast_optimizer 阅读，
   version2 的接口取舍及兼容修复见 [合并说明](docs/a-version2-merge-notes.md)。
3. B 先按 memory_catalog → type_rules → analyzer 阅读 [语义代码讲解](docs/semantic-walkthrough.md)，
   再按 bindWhere → UPDATE/DELETE → buildPlan → formatPlan 阅读 [计划代码讲解](docs/planner-walkthrough.md)。
   JOIN/GROUP/ORDER 按 resolveColumn → bindStatement(SelectStmt) → selectSource → printNode 阅读
   [高级查询代码讲解](docs/advanced-query-walkthrough.md)。
   A 扩展功能按公共结构 → analyzer → plan_builder/optimizer → JSON/Java 阅读
   [剩余功能实现讲解](docs/remaining-features-walkthrough.md)。
   优化部分按 constant_fold → optimizeExpr → optimizeNode 阅读 [优化代码讲解](docs/optimizer-walkthrough.md)。
4. 新增功能时在所属 tests 目录增加行为测试，显式更新 CMake；若新增源文件，
   同步 scripts/check.sh 的构建清单。
5. scaffold_smoke 已升级为真实 SQL 联调。语法、类型或计划规则变更应同时覆盖
   独立模块测试和 SQL→Plan 测试，避免仅靠手工 AST 掩盖跨模块问题。
6. 当前 app 保留 A 的前端调试行为；后续应用层接入 B 时明确 Catalog 来源和执行模式，
   算法继续放在库中。

讲解代码时沿三个结构层次展开：**AST 是“SQL 写了什么”，Bound 是“名称指向谁、
类型是什么”，Plan 是“执行层需要做什么”**。`variant` 区分节点种类，
`shared_ptr<const T>` 共享只读子树，SourceSpan 帮助把错误定位回 SQL。

建议先读 demonstrateSelect，再读 demonstrateInsert 和 demonstrateUpdate：
SELECT 展示扫描→过滤→投影的数据流；INSERT 展示输入列到表列的重排；
UPDATE 展示对更新前值的引用，以及修改操作为什么必须携带内部行标识。
所有新增模块均带中文注释；后续实现继续同步说明代码组织、设计原因和验证结果。

A/B 的基础编译链路已联调；后续与执行层确认快照版本、旧值赋值及行标识协议。
基础规则优化已实现；列裁剪、空结果算子和代价优化可作为后续进阶功能。
