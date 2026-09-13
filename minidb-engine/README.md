# MiniDB Java 数据库引擎

本模块不调用 C++ 对象。它只读取 `protocolVersion: 1` 的 JSON 执行计划，执行
CREATE、INSERT、SELECT、UPDATE 和 DELETE。当前 `InMemoryRecordStore` 是存储系统
完成前的替身；后续 Java 页式存储只需实现 `RecordStore`。

执行器支持 `INT/FLOAT/VARCHAR/BOOL` 和可存储的 `NULL`，并能执行 SeqScan、
NestedLoopJoin、Filter、GroupBy、Sort、Project。当前 GroupBy 按键去重，尚不计算聚合函数。
内部算子通过带列身份的 `PlanRow` 传行，因此 JOIN 后仍按 `tableId + columnId` 精确取列。

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

## Web 演示台

在工作区根目录双击 `run_minidb_web.bat`。它会构建 C++ 计划导出器和 Java JAR，
然后启动本机服务；浏览器访问 <http://localhost:8080>。

页面一次运行完整 SQL 脚本，每次运行都从空的内存数据库回放脚本。这样与当前 C++ 编译器
的 Catalog 行为一致；重启或刷新页面不会获得磁盘持久化数据。

手工启动时需提供 C++ 导出器路径：

```powershell
java --add-modules jdk.httpserver "-Dminidb.compiler.path=E:\MiniDB\DBcompiler-main\build\Debug\minisql_plan_json.exe" -jar target\minidb-engine-1.0.0.jar web
```
