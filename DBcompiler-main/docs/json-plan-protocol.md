# JSON 执行计划协议 1

`minisql_plan_json` 从标准输入读取 SQL，输出一份 UTF-8 JSON 文档。Java 引擎只依赖
该文档，不链接或反序列化 C++ 内存对象。

```json
{
  "protocolVersion": 1,
  "plans": [
    { "catalogVersion": 0, "root": { "type": "CreateTable" } }
  ]
}
```

每个计划执行前都必须比较 `catalogVersion`。同一输入中，导出器会在 CREATE 的计划
导出后模拟 Catalog 注册，因此后续 INSERT、SELECT、UPDATE、DELETE 可引用新表；
Java 引擎也必须在成功 CREATE 后以同样的表 ID、列 ID 和版本规则更新目录。

节点类型为 `CreateTable`、`Insert`、`SeqScan`、`Filter`、`Project`、`Update` 和
`Delete`。表对象含 `id`、`name`、`columns`，列引用含 `tableId`、`columnId`、
`ordinal`、`type`。表达式以 `kind: column|literal|unary|binary` 表示，运算名称与
C++ 的 `UnaryOp`、`BinaryOp` 枚举一致。字符串、整数和内部 BOOL 分别使用 JSON
string、number、boolean；表达式附带可选 `span` 以便 Java 报告 SQL 行列。

`carriesRowId` 为 true 时，Java 存储适配层必须让扫描结果携带稳定 RowId；UPDATE
和 DELETE 使用该 RowId 定位原记录，不能按业务列值猜测记录身份。
