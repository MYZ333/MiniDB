# Catalog 实现预留目录（B）

公共只读接口已经定义于 `include/minisql/catalog.hpp`。
后续在此实现用于测试的内存 Catalog、模式快照和表结构查询，并把 `.cpp`
显式加入 CMake 的 `minisql_backend` 目标。目前没有 Catalog 实现源文件。

语义分析只依赖 CatalogSnapshot；真实元数据的存储和建表提交由数据库执行层负责。
不要在 analyze 中自动创建表，也不要让 A 的 Parser 依赖 Catalog。
