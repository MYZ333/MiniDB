# MiniDB Java 数据库引擎

本模块不调用 C++ 对象。它只读取 `protocolVersion: 1` 的 JSON 执行计划，执行
CREATE、ALTER、DROP、INSERT、SELECT、UPDATE 和 DELETE。当前 `InMemoryRecordStore` 是存储系统
完成前的替身；后续 Java 页式存储只需实现 `RecordStore`。
引擎还支持 Explain 计划：普通模式返回算子树，ANALYZE 模式执行目标并采集实际行数、
包含子算子的耗时和调用次数。

执行器支持 `INT/FLOAT/VARCHAR/BOOL` 和可存储的 `NULL`，并能执行 SeqScan、EmptyResult、
DerivedTable、NestedLoopJoin、Filter、GroupBy、Aggregate、Sort、Project 和 SetOperation。GroupBy 用于纯分组去重，
Aggregate 执行 COUNT/SUM/AVG/MIN/MAX、HAVING、聚合表达式以及聚合后排序。
Project/Aggregate 处理 DISTINCT 和 LIMIT/OFFSET；NestedLoopJoin 支持四种连接类型。
写入边界检查 VARCHAR(n)、PRIMARY KEY、NOT NULL、UNIQUE，并保证批量 INSERT 和 UPDATE
在约束失败时不留下部分结果。新增执行路径支持复合主键/唯一约束、ALTER 数据迁移、关联子查询、
标量子查询基数检查、CASE 短路，以及 UNION/INTERSECT/EXCEPT 的集合与 ALL 语义。
内部算子通过带列身份的 `PlanRow` 传行，因此 JOIN 后按
`relationId + tableId + columnId` 精确取列，并支持同一物理表自连接。
SeqScan 接受编译器导出的精确列集合，只物化投影、筛选、连接、分组和排序真正依赖的列；
缺失该字段的旧计划仍扫描全列，空集合可为 COUNT(*) 或 DELETE 只产生行数和 RowId。
EmptyResult 固定产生零行，并保留列身份供外连接补 NULL；EXPLAIN ANALYZE 可以直接显示
被恒假条件消除的扫描没有执行。

构建 Java 引擎：

```powershell
mvn '-Dmaven.repo.local=target/maven-repo' '-Dmaven.test.skip=true' package
```

完整链路：先由 C++ 编译器生成 JSON，再交给 Java 引擎执行。

```powershell
Get-Content demo.sql | .\build\minisql_plan_json | java -jar .\minidb-engine\target\minidb-engine-1.0.0.jar
```

运行无第三方依赖的集成测试：

```powershell
mvn '-Dmaven.repo.local=target/maven-repo' test-compile
java -ea -cp "target/classes;target/test-classes" minidb.EngineTest
```

高级算子的跨语言回归需从仓库根目录运行：

```bash
bash scripts/check_advanced_execution.sh
```

实现讲解见 [高级查询执行器](docs/advanced-query-execution.md)。
EXPLAIN 的运行时采样与副作用边界见
[EXPLAIN ANALYZE 实现讲解](../DBcompiler-main/docs/explain-analyze-walkthrough.md)。
扫描列契约、谓词下推和列裁剪数据流见
[规则优化代码讲解](../DBcompiler-main/docs/optimizer-walkthrough.md)，具体 SQL、运行行数和
优化前后对比见[规则优化效果演示](../DBcompiler-main/docs/optimizer-demo.md)。

## Web 演示台

在工作区根目录双击 `run_minidb_web.bat`。它会构建 C++ 计划导出器和 Java JAR，
然后启动本机服务；浏览器访问 <http://localhost:8080>。

页面一次运行完整 SQL 脚本，每次运行都从空的内存数据库回放脚本。这样与当前 C++ 编译器
的 Catalog 行为一致；重启或刷新页面不会获得磁盘持久化数据。

手工启动时需提供 C++ 导出器路径：

```powershell
java --add-modules jdk.httpserver "-Dminidb.compiler.path=E:\MiniDB\DBcompiler-main\build\Debug\minisql_plan_json.exe" -jar target\minidb-engine-1.0.0.jar web
```
