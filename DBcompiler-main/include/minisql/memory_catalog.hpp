#pragma once

// B 的内存元数据容器：供独立测试和未来执行层显式注册表，不存储数据记录。
#include "minisql/catalog.hpp"

#include <unordered_map>

namespace minisql {

class MemoryCatalog {
public:
    // 先完整校验，再分配 ID/更新版本；失败不改变模式。名称按 ASCII 归一化。
    // 此方法不是 analyze 的一部分，不代表执行了 CREATE SQL。
    Result<std::shared_ptr<const TableSchema>> createTable(
        std::string table_name, const std::vector<ColumnSpec>& columns);
    // 一个 DROP 语句至多递增一次版本；不存在且 if_exists=true 的名称被忽略。
    Result<std::size_t> dropTables(const std::vector<std::string>& table_names, bool if_exists);

    // 复制名称索引并共享不可变 TableSchema，旧快照不受后续建表影响。
    std::shared_ptr<const CatalogSnapshot> snapshot() const;

private:
    // 第一阶段单线程使用，不承诺并发注册/读取的线程安全。
    std::unordered_map<std::string, std::shared_ptr<const TableSchema>> tables_;
    CatalogVersion version_ = 0;
    std::uint64_t next_table_id_ = 1;
};

} // namespace minisql
