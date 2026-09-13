# Java 高级查询执行器代码讲解

这一部分接收 B 计划生成器导出的 JSON，执行 JOIN、GROUP BY 和 ORDER BY。主调用链是：

```text
executeProgramJson
  -> executePlan
    -> Project
      -> readInput
        -> Sort / GroupBy / Filter / NestedLoopJoin / SeqScan
```

## 1. 为什么增加 PlanRow

存储层的 `StoredRow` 只有 `RowId + values`，适合单表读写。两个表连接后，左右表都可能
有 ordinal 为 0 的列，仅用数组下标无法判断表达式引用哪一列。

`DatabaseEngine` 内部因此增加两个只供执行算子使用的记录：

- `ColumnSlot` 保存 `tableId`、`columnId`、源表 ordinal 和类型；
- `PlanRow` 保存各源表 RowId、当前列布局和对应值。

`SeqScan` 根据 Catalog 创建布局。表达式读取列时，`columnSlot` 先用
`tableId + columnId` 查找，再检查 ordinal 和类型是否与 Catalog 一致。这样 JOIN 拼接
左右值数组后不会把两个表的同位置列混淆。存储层接口没有变化。

## 2. 五个输入算子

`readInput` 是统一分派入口：

- `scan` 把 `StoredRow` 转成 `PlanRow`，保留目标表 RowId；
- `filter` 对每行求 BOOL 谓词，只保留 TRUE；
- `nestedLoopJoin` 以左行为外层、右行为内层，拼接候选行后计算 ON；
- `groupBy` 用有序 Map 按键元组去重，并只输出分组键；
- `sort` 依次比较 ORDER BY 项，支持每项独立 ASC/DESC。

嵌套循环连接的时间复杂度是 `O(leftRows * rightRows)`。它适合第一版验证计划接口，
以后可以把同一节点实现替换成哈希连接，而不改变 C++ 计划结构。

GroupBy 目前没有聚合函数，因此每组保留首个键元组。Java List 的相等判断会把对应位置的
两个 null 判断为相等，符合“两个 NULL 属于同一组”的接口约定。

排序对 INT、FLOAT、VARCHAR、BOOL 分别使用同类型比较。ASC 把 NULL 放在最后，DESC
把 NULL 放在最前；多个排序项逐项比较。Sort 位于 Project 下方，所以可以读取最终结果中
未显示的隐藏排序列。

## 3. 类型和表达式

JSON 读取器支持整数、小数和指数形式。JSON 本身不会保留 SQL 的整数/浮点类型：
`95.0` 可能由导出器写成 `95`。执行器根据列模式或表达式的 `type` 字段，将 FLOAT
统一归一化为 Java `Double`，INT 保持 `Long`。

表达式求值支持 INT/FLOAT 同类型算术和数值比较、字符串/布尔判等、NOT、AND、OR。
AND/OR 保留短路求值；INT 运算检查溢出，INT/FLOAT 除法检查除零，非有限浮点结果返回
`FloatOverflow`。NULL 可写入任何列，但当前不会参与表达式三值逻辑。

## 4. UPDATE/DELETE 为什么仍然可用

`PlanRow.rowIds` 按 tableId 保存源行标识，Filter 会原样传递它。Update/Delete 从中取得
目标表 RowId 后调用 `RecordStore.replace/erase`。Update 的所有右值表达式都读取旧行，
最后一次性写回，因此 `SET a=b,b=a` 可以正确交换。

## 5. 测试

`src/test/resources/advanced-query.sql` 是固定 SQL 场景，
`AdvancedQueryEngineTest` 检查最终记录。仓库根目录
`scripts/check_advanced_execution.sh` 会编译真实 C++ 导出器、生成 JSON，再运行 Java
基础测试和高级查询测试，避免手写 JSON 掩盖跨模块接口不一致。
