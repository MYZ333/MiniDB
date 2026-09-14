# SELECT 表达式项 B 侧适配说明

> 本文保留 A 分支交付时的设计与历史记录。整合后的支持状态以 [grammar.md 0.22](../grammar.md) 和 [本次整合说明](zhangbo-merge-notes.md) 为准；顶层聚合已接通 B，部分扩展仍明确拒绝执行。

本文档单独说明 A 新增的 SELECT 表达式项改动，供 B 适配语义分析、计划生成和执行层时参考。

## 1. 新增语法

A 现在支持在 SELECT 列表中书写普通表达式：

```sql
SELECT age + 1 AS next_age FROM student;
SELECT score * 1.1 adjusted_score FROM metrics;
SELECT 10 AS constant_value FROM t;
SELECT active = TRUE AS is_active FROM metrics;
SELECT (score) AS grouped_score FROM metrics;
```

原有语法仍然保留：

```sql
SELECT name FROM student;
SELECT * FROM student;
SELECT active, COUNT(*) FROM metrics GROUP BY active;
```

## 2. A 侧 AST 表示

`include/minisql/ast.hpp` 中的 `SelectItem` 已从：

```cpp
using SelectItem = std::variant<Identifier, AggregateCall>;
```

扩展为：

```cpp
using SelectItem = std::variant<Identifier, AggregateCall, ExprPtr>;
```

`SelectList` 仍然是：

```cpp
using SelectList = std::variant<
    AllColumns,
    std::vector<Identifier>,
    std::vector<SelectItem>
>;
```

为了降低对 B 的即时影响，A 保留旧分支：

- `SELECT name, age FROM student;`
  - 仍使用 `std::vector<Identifier>`。
- `SELECT age + 1 AS next_age FROM student;`
  - 使用 `std::vector<SelectItem>`，其中表达式项为 `ExprPtr`。
- `SELECT active, COUNT(*) FROM metrics GROUP BY active;`
  - 继续使用 `std::vector<SelectItem>`，其中普通列为 `Identifier`，聚合为 `AggregateCall`。

## 3. B 侧需要适配的位置

### 3.1 语义分析

`src/semantic/analyzer.cpp` 当前主要处理：

- `AllColumns`
- `std::vector<Identifier>`

B 需要新增处理：

- `std::vector<SelectItem>`

建议处理方式：

1. 遍历 `std::vector<SelectItem>`。
2. 对 `Identifier`：
   - 沿用现有列绑定逻辑。
3. 对 `ExprPtr`：
   - 复用已有 WHERE / UPDATE SET 表达式绑定逻辑。
   - 生成一个可作为输出列的表达式绑定结果。
4. 对 `AggregateCall`：
   - 沿用或新增聚合绑定逻辑。

### 3.2 输出列名

`SelectStmt::column_aliases` 仍然和显式 SELECT 项一一对应。

建议命名规则：

- 如果有别名，输出列名使用别名。
- 如果是裸列名 `Identifier`，输出列名使用原列名或现有 B 规则。
- 如果是表达式且没有别名：
  - 可以先使用表达式文本占位名，例如 `expr1`、`expr2`。
  - 或者要求表达式项必须有别名，由 B 报语义错误。

为了课程项目实现简单，建议先允许无别名表达式，并生成稳定名称：

```text
expr1
expr2
...
```

### 3.3 类型检查

表达式项应复用已有表达式类型规则：

- `age + 1` 返回 INT。
- `score * 1.1` 返回 FLOAT。
- `active = TRUE` 返回 BOOL。
- 字符串字面量返回 VARCHAR。
- NULL 字面量返回 NULL 或由 B 定义的兼容类型。

如果表达式里引用了不存在的列、类型不兼容或操作符不支持，应由 B 的语义阶段报错。

### 3.4 计划生成

当前 `ProjectPlan` 可能只支持列引用输出。SELECT 表达式项需要 B 选择一种方案：

方案 A：扩展 ProjectPlan

- 让 ProjectPlan 支持输出表达式列表。
- 每个输出项包含：
  - BoundExpr
  - 输出列名
  - 输出类型

方案 B：新增 EvalProjectPlan

- 保留旧 ProjectPlan 给纯列投影。
- 新增表达式投影计划节点处理 `ExprPtr` / BoundExpr。

课程项目中建议优先方案 A，因为更直接。

### 3.5 执行层

执行层需要在输出每一行时计算表达式项：

- 对普通列：读取列值。
- 对表达式：按当前行计算 BoundExpr。
- 对聚合：后续按聚合适配方案处理。

表达式计算应复用 WHERE / UPDATE SET 已有表达式执行逻辑，避免重复实现。

## 4. 与 GROUP BY / 聚合的关系

A 不做 GROUP BY 合法性检查。

B 后续建议规则：

- 如果 SELECT 项是非聚合表达式，则表达式中引用的列都应满足 GROUP BY 规则。
- `SELECT age + 1 FROM t GROUP BY age;` 可以合法。
- `SELECT age + score FROM t GROUP BY age;` 应该报分组错误，除非 B 定义了其他规则。
- 聚合函数表达式化尚未完成，当前 `COUNT(*) + 1` 仍不是 A 支持范围。

## 5. 当前 A 侧边界

当前 A 支持：

- 算术表达式 SELECT 项。
- 比较表达式 SELECT 项。
- 字面量 SELECT 项。
- 括号表达式 SELECT 项。
- 输出别名。

当前 A 暂不支持：

- 函数调用表达式，例如 `LOWER(name)`。
- `CASE WHEN`。
- 子查询表达式。

## 6. 聚合表达式化补充

A 现在已经把聚合调用纳入普通表达式 AST：

```cpp
struct Expr {
    std::variant<IdentifierExpr, LiteralExpr, UnaryExpr, BinaryExpr, AggregateCall> node;
    SourceLocation span;
};
```

因此以下语法可以被 Lexer/Parser 接受：

```sql
SELECT COUNT(*) + 1 AS count_plus_one FROM metrics;
SELECT active FROM metrics GROUP BY active HAVING COUNT(*) > 0;
```

### 6.1 当前 B 侧最小处理

`src/semantic/analyzer.cpp` 已添加带注释的 `AggregateCall` 分支。

当前行为是：

- B 能编译通过。
- B 在语义分析阶段遇到聚合表达式时返回 `UnsupportedFeature`。
- B 不会把 `AggregateCall` 误当成 `BinaryExpr` 访问 `left/right/op`。

这是过渡状态，不代表 B 已经支持聚合执行。

### 6.2 B 后续完整适配建议

B 后续需要新增绑定层表达式，例如：

```cpp
struct BoundAggregate {
    AggregateFunction function;
    std::variant<AllColumns, BoundColumnRef> argument;
    SourceLocation span;
};
```

或者设计等价结构。

语义分析建议分阶段处理：

1. 在 SELECT / HAVING / ORDER BY 允许聚合表达式。
2. 在 WHERE / JOIN ON 禁止聚合表达式。
3. 推导聚合返回类型：
   - `COUNT` 返回 INT。
   - `SUM(INT)` 返回 INT。
   - `SUM(FLOAT)` 返回 FLOAT。
   - `AVG` 返回 FLOAT。
   - `MIN/MAX` 返回参数类型。
4. 检查聚合参数类型：
   - `COUNT(*)` 不绑定具体列。
   - `COUNT(column)` 可接受 B 定义的任意可计数类型。
   - `SUM/AVG` 建议只接受数值列。
   - `MIN/MAX` 可以先接受数值列，后续再扩展 VARCHAR。
5. 检查 GROUP BY：
   - 非聚合表达式引用的列需要满足 GROUP BY 规则。
   - 聚合表达式参数不需要出现在 GROUP BY 中。

计划层建议新增或扩展聚合计划节点：

- 输入：JOIN/WHERE 后的数据。
- 分组键：GROUP BY 列表；没有 GROUP BY 但有聚合时使用单组聚合。
- 聚合列表：SELECT、HAVING、ORDER BY 中出现的聚合调用。
- HAVING：在聚合后过滤。
- Project：在聚合后计算最终输出表达式。

执行层建议：

- 复用已有表达式执行器计算普通表达式。
- 聚合表达式由聚合算子提供结果值。
- `COUNT(*)` 统计输入行数。
- NULL 处理规则由 B 统一定义。

## 7. 对现有 B 的兼容性

普通列查询仍使用旧分支：

```cpp
std::vector<Identifier>
```

所以现有 B 对以下 SQL 的处理路径不需要立刻变化：

```sql
SELECT name FROM student;
SELECT id, name FROM student;
SELECT * FROM student;
```

只有出现表达式项或聚合项时，B 才会看到：

```cpp
std::vector<SelectItem>
```

B 在完成适配前，可以对 `std::vector<SelectItem>` 统一返回 `UnsupportedFeature`，但建议错误信息明确写出：

```text
SELECT expression or aggregate items are not supported by semantic analysis yet
```

## 8. 建议测试

B 适配后建议至少补充以下测试：

```sql
SELECT age + 1 AS next_age FROM student;
SELECT score * 1.1 AS adjusted_score FROM metrics;
SELECT active = TRUE AS is_active FROM metrics;
SELECT 10 AS constant_value FROM student;
SELECT age + 1 FROM student;
SELECT missing + 1 FROM student;
SELECT name + 1 FROM student;
SELECT COUNT(*) + 1 AS count_plus_one FROM student;
SELECT age FROM student HAVING COUNT(*) > 0;
SELECT age FROM student WHERE COUNT(*) > 0;
```

如果 B 暂时要求表达式必须带别名，则对应补充：

```sql
SELECT age + 1 FROM student;
```

应报语义错误。

## 9. ORDER BY 表达式补充

A 现在支持 ORDER BY 表达式语法：

```sql
SELECT name FROM metrics ORDER BY score + 1 DESC;
SELECT active FROM metrics GROUP BY active ORDER BY COUNT(*) DESC;
```

AST 表示方式：

```cpp
struct OrderByItem {
    Identifier column;
    SortDirection direction = SortDirection::Asc;
    SourceLocation span;
    ExprPtr expression = nullptr;
};
```

兼容策略：

- 普通 `ORDER BY age DESC` 仍然使用旧的 `column` 字段，`expression == nullptr`。
- 表达式排序项使用 `expression` 字段。
- `column` 字段在表达式排序项中不承载有效列名。

### 9.1 当前 B 侧最小处理

`src/semantic/analyzer.cpp` 已添加带注释的检查：

- 如果 `OrderByItem::expression` 非空，当前返回 `UnsupportedFeature`。
- 这样避免 B 按旧 `column` 字段继续绑定，产生误导性列不存在错误。

当前 B 还没有真正支持表达式排序。

### 9.2 B 后续完整适配建议

B 后续可以把 `BoundOrderBy` 从只保存列引用：

```cpp
struct BoundOrderBy {
    BoundColumnRef column;
    SortDirection direction;
};
```

扩展为表达式排序项，例如：

```cpp
struct BoundOrderBy {
    BoundExprPtr expression;
    SortDirection direction;
};
```

或者新增 variant，同时保留列引用快速路径。

语义规则建议：

- ORDER BY 表达式最终类型必须可比较。
- ORDER BY 表达式可以引用 FROM/JOIN 可见列。
- ORDER BY 表达式是否允许引用 SELECT 输出别名，需要和现有别名规则统一。
- 分组查询中，ORDER BY 非聚合表达式引用的列应满足 GROUP BY 规则。
- ORDER BY 聚合表达式依赖前面“聚合表达式化”的完整 B 适配。

计划和执行建议：

- SortPlan 需要能计算排序表达式，或在 Sort 前增加表达式投影。
- 如果表达式排序项未出现在最终 SELECT 输出中，它仍是 hidden sort key。
- Project 应在 Sort 后执行，避免 hidden sort key 泄漏到最终输出。
