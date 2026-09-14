#pragma once

// Catalog 是编译器的符号表：B 只读表结构，不读写数据库记录。
#include "minisql/common.hpp"

#include <memory>
#include <string_view>

namespace minisql {

// 包装为不同类型，防止把表 ID 当作列 ID 传递。
struct TableId { std::uint64_t value; };
struct ColumnId { std::uint64_t value; };
using CatalogVersion = std::uint64_t;

struct ColumnSchema {
    ColumnId id;
    std::string name; // 已归一化。
    DataType type;
    std::optional<std::int64_t> varchar_length = {}; // 仅 VARCHAR(n) 使用；nullopt 表示未声明长度。
    bool primary_key = false;
    bool not_null = false;
    bool unique = false;
    std::optional<ScalarValue> default_value = {};
};

struct TableSchema {
    TableId id;
    std::string name; // 已归一化。
    std::vector<ColumnSchema> columns; // 建表顺序，也是扫描记录的列顺序。
};

// 返回共享只读模式，保证 BoundStatement/Plan 可以持有安全的生命周期。
class CatalogSnapshot {
public:
    virtual ~CatalogSnapshot() = default;
    virtual CatalogVersion version() const noexcept = 0;
    virtual std::shared_ptr<const TableSchema> findTable(
        std::string_view normalized_name) const = 0; // 不存在返回 nullptr。
};

} // namespace minisql
