// 内存 Catalog 的写入口与不可变快照实现。分析器只看得到 CatalogSnapshot。
#include "minisql/memory_catalog.hpp"

#include <unordered_set>
#include <utility>

namespace minisql {
namespace {

// 私有具体快照，调用方只能通过只读虚接口访问。
class MemorySnapshot final : public CatalogSnapshot {
public:
    using Tables = std::unordered_map<std::string, std::shared_ptr<const TableSchema>>;
    MemorySnapshot(CatalogVersion version, Tables tables)
        : version_(version), tables_(std::move(tables)) {}
    CatalogVersion version() const noexcept override { return version_; }
    std::shared_ptr<const TableSchema> findTable(std::string_view normalized_name) const override {
        const auto found = tables_.find(std::string(normalized_name));
        return found == tables_.end() ? nullptr : found->second;
    }
private:
    const CatalogVersion version_;
    const Tables tables_;
};

Diagnostic schemaError(ErrorCode code, std::string message) {
    // 显式注册属于执行/测试侧；没有 SQL 来源，所以不伪造源码位置。
    return {DiagnosticStage::Execution, code, std::move(message), std::nullopt};
}
} // namespace

Result<std::shared_ptr<const TableSchema>> MemoryCatalog::createTable(
    std::string table_name, const std::vector<ColumnSpec>& columns) {
    table_name = normalizeName(std::move(table_name));
    if (tables_.count(table_name) != 0) {
        return schemaError(ErrorCode::TableAlreadyExists, "table '" + table_name + "' already exists");
    }
    if (columns.empty()) {
        return schemaError(ErrorCode::EmptyColumnList, "table must have at least one column");
    }
    // 在局部结构里完成校验；任何返回错误都不会留下半张表。
    std::unordered_set<std::string> names;
    std::vector<ColumnSchema> fields;
    for (const auto& column : columns) {
        auto name = normalizeName(column.name);
        if (!names.insert(name).second) {
            return schemaError(ErrorCode::DuplicateColumn, "duplicate column '" + column.name + "'");
        }
        if (column.type != DataType::Int && column.type != DataType::Varchar) {
            return schemaError(ErrorCode::UnsupportedType, "table columns support only INT and VARCHAR");
        }
        fields.push_back({ColumnId{static_cast<std::uint64_t>(fields.size()) + 1},
                          std::move(name), column.type});
    }
    auto schema = std::make_shared<const TableSchema>(TableSchema{
        TableId{next_table_id_}, table_name, std::move(fields)});
    tables_.emplace(table_name, schema);
    ++next_table_id_;
    ++version_;
    return schema;
}

std::shared_ptr<const CatalogSnapshot> MemoryCatalog::snapshot() const {
    return std::make_shared<const MemorySnapshot>(version_, tables_);
}

} // namespace minisql
