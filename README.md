# MiniSQL 编译器项目骨架（A + B）

本项目采用 **C++17 + CMake**，按照 `grammar.md` 和接口契约组织两人的开发。
语言目标为 CREATE TABLE、INSERT、SELECT、UPDATE、DELETE，以及基础条件和整数算术。
当前具备共享类型、分模块构建、入口占位、手工示例和联调检查；**尚不能编译或执行 SQL**。

## 1. 目录结构

```text
DBcompiler/
├── CMakeLists.txt             # 构建目标及 CTest 注册
├── grammar.md                 # 语言文法、优先级、语义限制，A/B 同步维护
├── docs/
│   └── interfaces.md          # Token/AST/Catalog/Bound/Plan 契约
├── include/minisql/           # 跨模块公共头文件
│   ├── common.hpp            # 基础类型、源码范围、诊断、名字归一化
│   ├── token.hpp             # A：Token 种类、词素、位置
│   ├── lexer.hpp             # A：lex() 声明
│   ├── parser.hpp            # A：parse() 声明
│   ├── ast.hpp               # A/B：语句与表达式 AST
│   ├── catalog.hpp           # B：只读 Catalog 快照接口
│   ├── bound.hpp             # B：名称绑定和类型检查后的结构
│   ├── plan.hpp              # B：逻辑算子及输出模式
│   └── compiler.hpp          # B：analyze()/buildPlan() 声明
├── src/
│   ├── lexer/lexer.cpp       # A：词法实现入口，当前占位
│   ├── parser/parser.cpp     # A：语法实现入口，当前占位
│   ├── catalog/README.md     # B：内存 Catalog 实现预留说明
│   ├── semantic/analyzer.cpp # B：语义实现入口，当前占位
│   └── planner/plan_builder.cpp # B：计划实现入口，当前占位
├── app/main.cpp              # A/B：未来的流程编排入口，当前只显示骨架状态
├── examples/contracts.cpp    # 手工构造五类 AST、绑定结果和计划
├── tests/
│   ├── lexer/                # A：词法测试预留说明
│   ├── parser/               # A：语法测试预留说明
│   ├── semantic/             # B：语义测试预留说明
│   ├── planner/              # B：计划测试预留说明
│   └── integration/scaffold_smoke.cpp # A/B：当前接口链接和占位诊断检查
├── scripts/check.sh          # 无 CMake 时的编译及检查脚本
└── build/                    # 本地构建产物，已忽略
```

`include/` 放双方需要引用的契约，`src/` 放实现。后续只服务某一模块的辅助类
或私有头文件就近放在该模块目录，避免所有细节都成为公共接口。
预留目录目前只有说明文件，不代表已有对应算法或行为测试。

## 2. 两人分工

| 工作 | A：词法与语法 | B：语义与计划 |
|---|---|---|
| 输入处理 | lex：关键字、注释、转义、位置、EOF | 不处理字符流 |
| 语法结构 | parse：五类语句、表达式优先级、多语句、AST | 使用 A 提供的 AST，不自行解析 SQL |
| 名称与类型 | 保留名称原文和源码范围 | Catalog 查询、表列绑定、类型检查、INSERT 重排、UPDATE 规则 |
| 计划生成 | 提供准确的 AST | 构造 CreateTable/Insert/SeqScan/Filter/Project/Update/Delete |
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
                             └─ 外部执行引擎 → 存储
```

四个入口均返回 `Result<T>`，即成功值或 Diagnostic，调用者遇错应停止该条流程。
当前四个入口返回 `NotImplemented`，它表示模块未开发，不表示用户 SQL 不合法。
parse 支持多语句是接口目标，当前没有实际解析能力。
CREATE 不在编译时修改 Catalog；未来由上层按“编译→执行成功→刷新快照→下一条”驱动。

| CMake 目标 | 内容 | 依赖 |
|---|---|---|
| minisql_contracts | 共享头文件 INTERFACE 库 | 无 |
| minisql_frontend | A 的 Lexer、Parser 静态库 | contracts |
| minisql_backend | B 的 Semantic、Planner 静态库 | contracts |
| minisql | 联调应用入口 | frontend + backend |
| contracts_example | 手工结构示例 | contracts |
| scaffold_smoke | 占位接口链接检查，仅 BUILD_TESTING 开启时构建 | frontend + backend |

A 与 B 的库互不依赖，因此可以独立实现和测试。真实 Catalog 实现加入后归入
backend；目前通过 CatalogSnapshot 抽象接口和测试替身连接，不创建空的算法库。

## 4. 构建和运行

需要 C++17 编译器和 CMake 3.16 及以上：

```bash
cmake -S . -B build
cmake --build build
(cd build && ctest --output-on-failure)
./build/minisql
./build/contracts_example
```

默认开启 BUILD_TESTING；`-DBUILD_TESTING=OFF` 可关闭测试目标。
以上可执行路径适用于默认单配置构建；多配置生成器使用对应 Debug/Release 子目录。
minisql 目前只显示开发状态，传入参数会拒绝并返回 2，不接受 SQL 文件或标准输入。

环境没有 CMake 时，在仓库目录运行：

```bash
bash scripts/check.sh
```

该脚本用 c++ 和 ar 分别构建 A/B 静态库，验证九个公共头文件可独立包含，
再编译运行应用、手工示例和联调检查。产物位于 `build/direct/`，不会覆盖
CMake 构建文件；可通过 CXX/AR 环境变量指定工具路径。

当前环境未安装 CMake；验证使用上述脚本，CMake/CTest 路径尚未在本机验证。
手工示例与占位检查仅验证接口骨架，不能代表解析、语义检查或 CRUD 已通过测试。

## 5. 开发与阅读顺序

1. 共同阅读 [grammar.md](grammar.md) 和 [接口文档](docs/interfaces.md)，先理解范围、
   输入输出、错误、名称大小写和源码位置规则。
2. A 从 token/lexer 开始，再实现 parser 与 AST；不需要等待 B 或真实数据库。
3. B 从 catalog/common 开始，使用手工 AST 实现 analyze，再实现 buildPlan，
   不需要等待 A 的 Parser；参考 examples/contracts.cpp 中的五组对应结构。
4. 每完成一个模块，把对应占位入口替换为算法，在所属 tests 目录增加行为测试，
   显式更新 CMake；若新增源文件，也同步 scripts/check.sh 的构建清单。
5. 同时替换 scaffold_smoke 中该模块的 NotImplemented 断言，防止功能完成后
   旧占位检查反而要求它继续失败。A/B 都完成后将其升级为真正的端到端测试。
6. 联调阶段再给 app 增加文件/标准输入、诊断展示和逐阶段输出，不把算法塞入 main。

讲解代码时沿三个结构层次展开：**AST 是“SQL 写了什么”，Bound 是“名称指向谁、
类型是什么”，Plan 是“执行层需要做什么”**。`variant` 区分节点种类，
`shared_ptr<const T>` 共享只读子树，SourceSpan 帮助把错误定位回 SQL。

建议先读 demonstrateSelect，再读 demonstrateInsert 和 demonstrateUpdate：
SELECT 展示扫描→过滤→投影的数据流；INSERT 展示输入列到表列的重排；
UPDATE 展示对更新前值的引用，以及修改操作为什么必须携带内部行标识。
所有新增模块均带中文注释；后续实现继续同步说明代码组织、设计原因和验证结果。
