# MiniDB

一个面向课程实训的小型数据库系统原型。项目采用“C++ 编译 SQL、Java 执行计划、Java
存储接口、Web 展示结果”的分层设计，当前已实现从 SQL 脚本到查询结果的完整演示链路。

> 当前版本默认使用 Java 页式持久化存储；目录与记录会在正常关闭后恢复。

## 架构

```text
浏览器 Web 演示台
        │ SQL 脚本
        ▼
Java Web 服务（127.0.0.1）
        │ 调用计划导出器
        ▼
C++ SQL 编译器 ── JSON 逻辑执行计划 ──► Java 数据库引擎
                                            │
                                            ▼
                                PageRecordStore（页、缓冲池、数据文件）
```

- C++ 编译器负责 SQL 的词法、语法、语义、逻辑计划和规则优化，并输出 JSON 计划；
  编译侧支持约束 DDL、多行 INSERT、计算与聚合表达式、内外连接、分组、HAVING、
  DISTINCT、表达式排序、LIMIT/OFFSET、子查询、派生表、集合运算、CASE、ALTER TABLE 和
  EXPLAIN/EXPLAIN ANALYZE。
- Java 执行引擎负责 AlterTable、DerivedTable、SetOperation 等全部计划节点的实际执行、
  表模式和记录迁移、关联表达式求值、运行期错误和结果集生成。
- `RecordStore` 是执行层与存储层的边界；后续页式存储只需实现该接口，不需改动执行器。
- Web 工作台负责 SQL 编辑、结果表格、错误定位、JSON 计划和持久化状态展示；表目录可打开独立的只读表浏览器，查看字段、约束、索引与分页数据。

## 快速启动

前置环境：Java 17+、Maven、CMake，以及 Visual Studio 2022 C++ Build Tools。

直接双击根目录的 `run_minidb_web.bat`，或在普通命令行中运行：

```bat
E:\MiniDB\run_minidb_web.bat
```

脚本会构建 C++ 与 Java 项目，然后启动本地服务并打开：<http://localhost:8080>。
保持脚本窗口运行；关闭窗口即停止服务。

## Web 工作台与表浏览器

浏览器根路径 `<http://localhost:8080>` 提供 SQL Workbench，可编辑并运行完整 SQL 脚本，
查看命令影响行数、查询结果、错误位置与 JSON 执行计划。页面底部保留全部演示用例的用途、
预期结果和影响说明，点击用例只会填入 SQL，不会立即执行。

右侧表目录来自持久化 Catalog。点击表会打开独立页面
`<http://localhost:8080/table-browser.html?table=表名>`，该页面以只读方式显示：

- 分页数据预览，每页最多 100 行；
- 字段类型、VARCHAR 长度、主键、NOT NULL、UNIQUE 和 DEFAULT；
- 表级约束及索引定义。

Web API 同时提供 `GET /api/catalog` 与 `GET /api/tables/{name}?offset&limit`，供页面读取目录和表快照；
既有 `POST /api/execute` 保持不变。

也可使用命令行脚本运行根目录 `demo.sql`：

```bat
E:\MiniDB\run_minidb.bat
```

## 当前 SQL 范围

支持以分号分隔的多条 SQL：

```sql
CREATE TABLE student(id INT, name VARCHAR, age INT);
INSERT INTO student VALUES (1, 'Alice', 20);
SELECT name FROM student WHERE age >= 18;
EXPLAIN ANALYZE SELECT name FROM student WHERE age >= 18;
UPDATE student SET age = age + 1 WHERE id = 1;
DELETE FROM student WHERE id = 1;
```

- 编译器数据类型：`INT`、`VARCHAR(n)`、`BOOL`、`FLOAT`，INSERT 支持 `NULL` 和多行 VALUES
- 编译器表达式：同类型数值运算和比较、字符串/布尔判等、LIKE、空值判定和三值逻辑
- 编译器查询：表/列别名、内外连接、GROUP BY/HAVING、五类聚合、DISTINCT、表达式排序与分页
- 编译器扩展：关联/非关联子查询、FROM/JOIN 派生表、UNION/INTERSECT/EXCEPT 及 ALL、CASE
- 编译器 DDL：CREATE IF NOT EXISTS、列级/复合约束、ALTER TABLE ADD/DROP/RENAME
- 计划观测：EXPLAIN 展示优化后树，EXPLAIN ANALYZE 展示每个算子的实际行数、耗时和调用次数
- 计划优化：常量折叠、外连接安全的谓词下推、空结果传播，以及覆盖查询和 DML 依赖的列裁剪
- Java 引擎执行类型：`INT`、`FLOAT`、`VARCHAR`、`BOOL`，记录可保存 `NULL`
- Java 引擎高级查询：内外连接、分组/HAVING、计算投影、多列排序、DISTINCT、分页和规定的 NULL 顺序
- 错误处理：词法/语法/语义错误，以及除零、整数溢出、类型不匹配等执行期错误

feature-zhangbo 提供的扩展语法已接通 B 和 Java 执行层，包括子查询、派生表、集合运算、
CASE、ALTER TABLE，以及此前的 HAVING、DISTINCT、外连接和约束 DDL。
项目扩展还支持 EXPLAIN 和带真实执行统计的 EXPLAIN ANALYZE。
完整边界见 [文法支持表](DBcompiler-main/grammar.md)。聚合函数内部 DISTINCT、索引、事务和并发控制尚不支持。
整合过程与代码阅读指南见 [A+B 整合说明](DBcompiler-main/docs/zhangbo-merge-notes.md)。

## 数据与持久化说明

默认数据文件为 `data/minidb.db`，同目录的 `.alloc` 文件保存页分配信息。系统目录与
用户记录均经 BufferPool 写入固定 4 KB 页，行标识编码为页号和槽号；重启后可以直接执行
查询或写入。`-Dminidb.data.path=...`、`-Dminidb.buffer.frames=...` 和
`-Dminidb.buffer.policy=FIFO` 可改变路径、缓冲帧数和替换策略。运行
`java -jar minidb-engine.jar reset` 会显式清除默认数据文件。

## 验收状态与评分准备

**当前结论：**系统的核心功能已经较完整，能够演示 SQL 编译、Java 执行、页式持久化、
Web 和 EXPLAIN ANALYZE；但不能声明“全部验收通过”。按《大型平台软件设计实习评分-2026》
的要求，C++ 的 `parser_tests` 和 `scaffold_smoke` 仍会段错误，必须优先修复。

| 评分项 | 当前情况 | 结论 |
| --- | --- | --- |
| SQL 编译器，16 分 | 词法、语法、语义、Catalog、JSON 计划均有实现；支持基础 DDL/DML 与 JOIN、聚合、子查询、集合、CASE、ALTER、EXPLAIN ANALYZE。 | 功能范围较完整，但 `parser_tests` 段错误，不能称全绿。 |
| 存储系统，12 分 | 4 KB 页、磁盘文件、页分配/释放、LRU/FIFO、dirty/pin、PageGuard、重启读写、B+ 树自测均已覆盖。 | 基本完成。 |
| 数据库系统，12 分 | SeqScan/Filter/Insert/Update/Delete、目录与记录持久化、CLI、Web、C++ 到 JSON 到 Java 的链路均已实现。 | 基本完成。 |
| 模块完成度，15 分 | 代码、边界处理、测试和接口衔接较完整。 | 受 C++ 段错误影响，不宜按满分准备。 |
| 创新，10 分 | 高级 SQL、规则优化、EXPLAIN ANALYZE、Web 与缓存统计、B+ 树均可作为创新点。 | 有较大展示和拿分空间。 |
| 实验报告，10 分 | 尚未提供报告成品。 | 必须补齐个人模块、测试证据和问题总结。 |

已确认通过的回归包括 Java 基础引擎、页式 `PageRecordStore`、存储自测（含 B+ 树重启）和
Web API。Web 默认运行于持久化模式。C++ 的 Debug `ctest` 当前为 11 项中 9 项通过，失败项为
`parser_tests` 与 `scaffold_smoke`，两者均以段错误退出。

### 当前仍属部分完成的边界

- 未实现断电或崩溃恢复；当前保证正常关闭后的持久化，不包含 WAL。
- B+ 树是独立存储能力，尚未接入 `CREATE INDEX`、优化器选择和 `IndexScan`；不能称为“SQL 已使用索引”。
- 记录扩容更新会迁移记录，RowId 不保证稳定；索引接入前需先解决该问题。
- Maven 默认跳过许多以可执行 `main` 形式编写的测试；验收应使用统一脚本显式运行 C++、Java、存储和 Web 测试。

### 答辩优先级

1. 修复 `parser_tests`、`scaffold_smoke` 的段错误，使 C++ Debug `ctest` 达到 11/11 通过。
2. 增加一键验收脚本，运行 C++ CTest、Java 引擎/存储/Web 测试、持久化重启验证和 Web 演示 SQL。
3. 如有时间，优先把 B+ 树接入 SQL：`CREATE INDEX`、写入维护、等值/范围 `IndexScan`，并在 `EXPLAIN ANALYZE` 展示它。
4. 时间不足时，优先展示谓词下推和列裁剪的优化前后扫描行数与耗时，不建议冒险增加 WAL。

答辩可主打高级 SQL 全链路、页式持久化、LRU/FIFO、EXPLAIN ANALYZE、规则优化和 Web 可视化；
不要声称已实现事务、并发、WAL 或 SQL 索引扫描。

## 项目结构

```text
MiniDB/
├── DBcompiler-main/     # C++17 SQL 编译器与 JSON 计划导出器
├── minidb-engine/       # Java 执行引擎、RecordStore 与 Web 服务
├── demo.sql             # 可直接演示的完整 SQL 脚本
├── run_minidb.bat       # 命令行端到端运行脚本
└── run_minidb_web.bat   # 一键启动 Web 演示台
```

更具体的 C++ 编译器说明见 `DBcompiler-main/README.md`，Java 引擎和手工启动方式见
`minidb-engine/README.md`。

## 验证

Java 引擎与 Web 服务均有无第三方依赖的测试：

```powershell
cd E:\MiniDB\minidb-engine
mvn '-Dmaven.repo.local=target/maven-repo' test-compile
java --add-modules jdk.httpserver -ea -cp "target/classes;target/test-classes" minidb.EngineTest
java --add-modules jdk.httpserver -ea -cp "target/classes;target/test-classes" minidb.PageRecordStoreTest
java --add-modules jdk.httpserver -ea -cp "target/classes;target/test-classes" minidb.storage.StorageSelfTest
java --add-modules jdk.httpserver -ea -cp "target/classes;target/test-classes" minidb.WebServerTest
```

C++ 编译器在 `DBcompiler-main` 下运行以下命令：

```powershell
cmake --build .\build --config Debug
ctest --test-dir .\build -C Debug --output-on-failure
```

当前 CTest 有两项已知段错误，命令会以失败状态退出；修复前不得将它作为“全部通过”的证据。
Java 的测试中也有一部分以可执行 `main` 形式存在，Maven 的 Surefire 默认跳过它们，因此必须
显式运行上述四个 Java 命令。

Linux/WSL 下可运行跨语言高级查询回归：

```bash
bash scripts/check_advanced_execution.sh
```

该脚本用真实 SQL 构建 C++ JSON 计划，再由 Java 测试检查 JOIN、GROUP BY、ORDER BY、
FLOAT、BOOL、NULL、五类聚合、HAVING、DISTINCT、分页、LIKE、子查询、派生表、集合运算、
CASE、ALTER、DDL 约束、多行写入、失败原子性
以及 EXPLAIN ANALYZE 运行时统计、谓词下推、空结果传播、精确列扫描和零列 `COUNT(*)` 执行。

聚合代码的阅读顺序和答辩示例见 [聚合实现讲解](DBcompiler-main/docs/aggregate-walkthrough.md)。
EXPLAIN 的包装结构、采样点和演示脚本见 [EXPLAIN ANALYZE 实现讲解](DBcompiler-main/docs/explain-analyze-walkthrough.md)。
谓词下推的语义边界和列依赖传播见 [规则优化代码讲解](DBcompiler-main/docs/optimizer-walkthrough.md)；
可直接用于答辩的 SQL、前后计划和量化结果见
[规则优化效果演示](DBcompiler-main/docs/optimizer-demo.md)。

面向学习和答辩的两份简明材料：
[B 语法、边界与演示案例](DBcompiler-main/docs/b-syntax-boundary-demo-guide.md)汇总可执行语法、
异常输入和优化展示；[B 模块代码学习指南](DBcompiler-main/docs/b-module-study-guide.md)解释模块目标、
运行流程、核心结构、实现算法、模块交互和修改影响。

## 后续工作

1. 添加聚合函数内部 DISTINCT，并为连接和分组增加可替换的物理执行算法。
2. 为持久化 DDL 与多页写入增加 WAL 和断电恢复。
