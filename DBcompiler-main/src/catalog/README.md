# Catalog 实现（B）

公共只读接口已经定义于 `include/minisql/catalog.hpp`。
`memory_catalog.cpp` 实现用于测试的内存 Catalog、模式快照和表结构查询，
已加入 CMake 的 `minisql_backend` 目标。写入口声明在 memory_catalog.hpp。

注册时先校验全部定义，成功后才发布不可变 TableSchema 并递增版本；snapshot
复制名称索引，因此后续注册不影响旧快照。快照通过 shared_ptr 保证模式生命周期。

语义分析只依赖 CatalogSnapshot；真实元数据的存储和建表提交由数据库执行层负责。
不要在 analyze 中自动创建表，也不要让 A 的 Parser 依赖 Catalog。
