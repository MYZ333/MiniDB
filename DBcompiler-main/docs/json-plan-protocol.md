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

每个计划执行前都必须比较 `catalogVersion`。同一输入中，导出器会在 CREATE/DROP 的计划
导出后模拟 Catalog 变更，因此后续语句看到正确模式；Java 引擎必须采用相同的版本规则。
`EXPLAIN ANALYZE` 包裹 CREATE/DROP 时也按实际执行处理该变更，普通 EXPLAIN 不改变 Catalog。

节点类型为 `CreateTable`、`DropTable`、`Insert`、`SeqScan`、`NestedLoopJoin`、`Filter`、
`GroupBy`、`Aggregate`、`Sort`、`Project`、`Update`、`Delete` 和 `Explain`。表对象含 `id`、`name`、`columns`，列引用含 `tableId`、`columnId`、
`relationId`、`ordinal`、`type`。表列还可含 `varcharLength`、`primaryKey`、`notNull`、
`unique`、`defaultValue` 和 `hasDefault`；后一个字段用于区分“没有默认值”和 `DEFAULT NULL`。
表达式以 `kind: column|literal|unary|binary|aggregate` 表示，运算名称与
C++ 的 `UnaryOp`、`BinaryOp` 枚举一致。字符串、整数/浮点、BOOL、NULL 分别使用 JSON
string、number、boolean、null；表达式附带可选 `span` 以便 Java 报告 SQL 行列。

- `DropTable` 使用 `tableNames` 和 `ifExists`。至少删除一张表时目录版本增加一次。
- `Insert` 的 `rows` 是完整记录二维数组；`values` 保留第一行以兼容旧消费者。
- `NestedLoopJoin` 使用 `left`、`right`、`predicate` 和 `joinType`，输出顺序为左列后接右列。
  joinType 为 INNER/LEFT/RIGHT/FULL；旧计划缺失时按 INNER 处理。
- `SeqScan` 的 `relationId` 和 `relationName` 标识一次 FROM/JOIN 出现。同一物理表
  自连接时 table.id 相同，但 relationId 不同；列引用必须按 relationId 定位关系实例。
- `GroupBy` 使用 `keys` 和 `input`，当前表示按键去重。
- `Sort` 使用有序 `items` 和 `input`；每项用 `kind` 区分 `column` 或 `expression`，
  并包含 `ASC`/`DESC` direction。
- `Project` 可用 `expressions` 计算输出；空数组时沿用 `columns`。它还携带
  `distinct`、`limit` 和 `offset`，执行顺序为投影、去重、分页。
- `Explain` 使用 `analyze: boolean` 和 `input: <statement-root>`。根节点的 output 固定为
  `[ {"name":"QUERY PLAN","type":"VARCHAR"} ]`，`carriesRowId` 为 false。

这些字段是协议 1 的向后兼容扩展：旧计划缺少 relationId 时，Java 引擎回退到 tableId。
当前 Java 引擎已执行全部上述节点，并以 Project.output 或 Aggregate.output 中的名称展示列别名。
旧引擎不能执行新增 Aggregate/Explain 节点；含聚合或 EXPLAIN SQL 需同步更新编译器和引擎。

`carriesRowId` 为 true 时，Java 存储适配层必须让扫描结果携带稳定 RowId；UPDATE
和 DELETE 使用该 RowId 定位原记录，不能按业务列值猜测记录身份。

## Aggregate 根节点

- `groupKeys`：有序列引用数组，空数组表示全表聚合。
- `items`：最终输出顺序。普通分组列为 `{"kind":"column","column":<ref>}`；
  聚合为 `{"kind":"aggregate","function":"SUM","argument":<ref>,"type":"INT","span":<span>}`。
  一般聚合表达式用 `{"kind":"expression","expression":<expr>}`。
  function 取 COUNT/SUM/AVG/MIN/MAX；仅 COUNT(*) 的 argument 为 JSON null。
- `orderBy`：有序排序项。输出别名为 `{"kind":"output","ordinal":1,"direction":"DESC"}`，
  ordinal 从 0 开始；隐藏分组键为 `{"kind":"group","column":<ref>,"direction":"ASC"}`。
- `having`：可空聚合表达式，按组求值且仅 TRUE 保留。
- `distinct`、`limit`、`offset`：聚合输出排序后的去重和分页设置。
- `input`：明细输入；`output`：与 items 对齐的最终列名和类型；`carriesRowId` 为 false。

Java 按 groupKeys 组建分组，按 items 计算结果，再执行 orderBy。无 GROUP BY 的空输入
必须保留一个空分组；COUNT 为 0，其余为 null。分组空输入返回零行。所有带参数聚合忽略 null。
MIN/MAX 沿用排序类型规则；整数 SUM 溢出报 IntegerOverflow，浮点累加非有限值报 FloatOverflow，
错误位置使用聚合项 span。NULL 排序保持 ASC 最后、DESC 最前。

## 空值判定与 A 新语法

Unary 表达式的 op 新增 IsNull 和 IsNotNull。两者先求值 operand，再检查其值是否为 null，
返回 BOOL，适用于任何已绑定类型。<>、BETWEEN、IN 使用既有二元比较/逻辑 JSON 节点；
UPDATE/DELETE 的表别名在 B 绑定时消解，仍用 relationId=0 传递行身份。
LIKE 使用 `BinaryOp::Like`，`%` 匹配任意 Unicode 码点序列，`_` 匹配一个码点。
执行表达式以 JSON/Java null 表示 SQL UNKNOWN；筛选类算子只接受 TRUE。

## Explain 根节点

Java 引擎将 Explain.input 按前序遍历转换为单列文本树。普通 EXPLAIN 只读取节点属性，
不调用任何目标算子。ANALYZE 模式下，目标根经过原 `executeNode`，关系子节点经过
原 `readInput`，因此统计反映真实执行路径。每行统计形式为：

```text
Filter [(student.age > 18)] (actual rows=2 time=0.125 ms loops=1)
```

- `actual rows` 是该算子累计输出行数；修改根节点使用 affectedRows。
- `time` 是累计墙钟毫秒，包含调用子算子的时间，保留三位小数。
- `loops` 是该 JSON 节点对象的调用次数；当前物化子输入实现通常为 1。
- ANALYZE 执行 DDL/DML 时保留副作用。目标报错时直接返回原 EngineException，不生成不完整报告。
