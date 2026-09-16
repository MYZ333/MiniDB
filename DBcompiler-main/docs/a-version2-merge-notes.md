# A version2 合并说明

合并日期：2026-09-10。

本次输入是 `A_version2/DBcompiler` 完整副本，内部含独立 `.git`、Visual Studio 缓存和
多套 Windows 构建产物。主工程只整合源码、接口意图和测试场景；`A_version2/` 已加入
根 `.gitignore`，不会成为嵌套仓库或上传约 80 MB 的本机构建文件。

## 合入的 A 功能

- Lexer 新增 FLOAT 字面量、BOOL/FLOAT/TRUE/FALSE/NULL、JOIN/ON、GROUP BY、
  ORDER BY、ASC/DESC 和点号 Token。
- Parser 新增浮点/布尔/NULL 字面量、BOOL/FLOAT 列定义、限定名、JoinClause、
  group_by 和 order_by；子句顺序为 JOIN → WHERE → GROUP BY → ORDER BY。
- 前端 CLI 默认展示 Token、原 AST 和 Optimized AST，并支持 `--raw-only`、
  `--optimized-only`。
- A 的 AST 常量折叠独立为 `ast_optimizer.hpp` 与 `src/parser/ast_optimizer.cpp`。

## 没有直接覆盖的内容

A version2 基于提交 4a7fe6d，而主工程已在 9173adb 上完成 B 实现和第一轮兼容修复。
因此逐项移植新增逻辑，保留以下主线行为：

- 块注释闭合符恰好位于 EOF 时仍合法。
- Parser 要求唯一 EOF 位于 TokenStream 末尾。
- 括号、NOT、负号嵌套与最终 AST 高度都限制为 256。
- AND/OR 的 operator_span 固定为真实操作符位置。
- B 的 `optimizer.hpp` 和 optimizePlan 继续负责绑定后计划优化。

A 副本也定义了同名 optimizer.hpp，若直接复制会覆盖 B API。因此展示优化改名为
optimizeAstExpression/optimizeAstStatement/optimizeAstStatements，并归入 frontend。
CLI 使用它展示语法树；正式编译仍把原 AST 传给 analyze，再对计划调用 optimizePlan。

AST 优化器在没有 Catalog 的阶段可能通过短路删除未绑定分支，例如
`FALSE AND missing=1`。所以它不能作为语义检查的输入，也不能证明 SQL 合法。
实现仍保留 A 的演示目标，并修复了副本中 `x AND FALSE`、`x OR TRUE` 可能吞掉左侧
运行时错误的问题。NULL 和 INT/FLOAT 混合表达式不会在 A 层猜测语义。

## B 兼容范围

| A version2 AST | 当前 B 行为 |
|---|---|
| BOOL/FLOAT 建表、字面量和同类型运算 | 完成绑定、计划、打印及常量折叠 |
| INSERT NULL | 允许写入任意列；记录层需保存空值 |
| 单表 `table.column` | 校验限定符后绑定到原 table/column ID |
| NULL 普通表达式 | InvalidOperandType；尚无三值逻辑 |
| JOIN | 多表作用域、歧义检查和 NestedLoopJoin 计划已完成 |
| GROUP BY | 无聚合函数时按键去重，生成 GroupBy 计划 |
| ORDER BY | 支持隐藏排序列及 ASC/DESC，生成 Sort 计划 |

上述三项在后续 B 开发中已由真实绑定和计划替换最初的 UnsupportedFeature 边界；
执行算法仍由执行层按 interfaces.md 的行布局契约实现。

## 验证

`bash scripts/check.sh` 使用 C++17 和 `-Wall -Wextra -Wpedantic -Werror` 构建全部模块。
合并后通过 A 的词法/语法测试、4 组 AST 优化测试、14 组真实 SQL A/B 联调、
5 组 Catalog、38 组语义、20 组计划和 24 组绑定后优化测试。
两套优化器还分别通过 UBSan 运行，未报告有符号溢出等未定义行为。

阅读顺序建议为 token.hpp → ast.hpp → lexer.cpp → parser.cpp → ast_optimizer.cpp，
再查看 analyzer.cpp 中的 literalType、resolveColumn 和 SelectStmt 扩展边界。
