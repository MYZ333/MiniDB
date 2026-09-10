# MiniDB

一个面向课程实训的小型数据库系统原型。项目采用“C++ 编译 SQL、Java 执行计划、Java
存储接口、Web 展示结果”的分层设计，当前已实现从 SQL 脚本到查询结果的完整演示链路。

> 当前版本使用内存记录存储，重点验证执行引擎逻辑；页式存储、缓冲池和重启持久化将由
> Java 存储系统接入后补齐。

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
                                RecordStore（当前为内存实现）
```

- C++ 编译器负责 SQL 的词法、语法、语义、逻辑计划和基础优化，并输出 JSON 计划。
- Java 执行引擎负责 CreateTable、Insert、SeqScan、Filter、Project、Update、Delete
  的实际执行、表达式求值、运行期错误和结果集生成。
- `RecordStore` 是执行层与存储层的边界；后续页式存储只需实现该接口，不需改动执行器。
- Web 演示台负责 SQL 编辑、结果表格、错误定位、执行历史和 JSON 计划展示。

## 快速启动

前置环境：Java 17+、Maven、CMake，以及 Visual Studio 2022 C++ Build Tools。

直接双击根目录的 `run_minidb_web.bat`，或在普通命令行中运行：

```bat
E:\MiniDB\run_minidb_web.bat
```

脚本会构建 C++ 与 Java 项目，然后启动本地服务并打开：<http://localhost:8080>。
保持脚本窗口运行；关闭窗口即停止服务。

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
UPDATE student SET age = age + 1 WHERE id = 1;
DELETE FROM student WHERE id = 1;
```

- 数据类型：`INT`、`VARCHAR`
- 表达式：整数加减乘除、一元负号、比较、`NOT` / `AND` / `OR`
- 查询：单表 `SELECT`、`SELECT *`、指定列和 `WHERE`
- 错误处理：词法/语法/语义错误，以及除零、整数溢出、类型不匹配等执行期错误

暂不支持 JOIN、聚合、ORDER BY、GROUP BY、索引、事务和并发控制。

## 数据与持久化说明

当前 `InMemoryRecordStore` 在 Java 进程内保存表结构和记录，并为每行分配稳定的
`RowId`，用于精确 UPDATE 与 DELETE。Web 页面每次“运行全部”都会从空数据库回放完整
脚本，因此数据不会写入磁盘，也不能跨重启保留。

存储系统接入后，将实现页号/槽号形式的 RowId 映射、记录序列化、页读写、缓冲池和
持久化 Catalog。

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
java --add-modules jdk.httpserver -ea -cp "target/classes;target/test-classes" minidb.WebServerTest
```

## 后续工作

1. 接入 Java 页式存储系统，实现页分配、读写与 Row/Page 映射。
2. 将系统目录持久化为特殊表，使表定义能够跨重启恢复。
3. 接入缓冲池与 LRU/FIFO 替换策略，补齐命中统计和页替换日志。
