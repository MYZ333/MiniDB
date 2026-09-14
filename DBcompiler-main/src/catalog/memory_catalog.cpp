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

DataType scalarType(const ScalarValue& value) {
    if (std::holds_alternative<std::int64_t>(value)) return DataType::Int;
    if (std::holds_alternative<double>(value)) return DataType::Float;
    if (std::holds_alternative<std::string>(value)) return DataType::Varchar;
    if (std::holds_alternative<bool>(value)) return DataType::Bool;
    return DataType::Null;
}

std::size_t utf8Length(const std::string& value) {
    std::size_t count = 0;
    for (unsigned char byte : value) if ((byte & 0xc0u) != 0x80u) ++count;
    return count;
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
    bool has_primary_key = false;
    for (const auto& column : columns) {
        auto name = normalizeName(column.name);
        if (!names.insert(name).second) {
            return schemaError(ErrorCode::DuplicateColumn, "duplicate column '" + column.name + "'");
        }
        if (column.type == DataType::Null) {
            return schemaError(ErrorCode::UnsupportedType, "NULL is not a declarable column type");
        }
        if (column.varchar_length &&
            (column.type != DataType::Varchar || *column.varchar_length <= 0)) {
            return schemaError(ErrorCode::UnsupportedType,
                               "VARCHAR length must be a positive integer");
        }
        if (column.primary_key && has_primary_key)
            return schemaError(ErrorCode::InvalidAst,
                               "table may contain only one PRIMARY KEY column");
        has_primary_key = has_primary_key || column.primary_key;
        if (column.default_value) {
            const auto actual = scalarType(*column.default_value);
            if ((column.not_null || column.primary_key) && actual == DataType::Null)
                return schemaError(ErrorCode::TypeMismatch,
                                   "NOT NULL column cannot default to NULL");
            if (actual != DataType::Null && actual != column.type)
                return schemaError(ErrorCode::TypeMismatch,
                                   "DEFAULT value type does not match column");
            if (column.varchar_length && actual == DataType::Varchar &&
                utf8Length(std::get<std::string>(*column.default_value)) >
                    static_cast<std::size_t>(*column.varchar_length))
                return schemaError(ErrorCode::TypeMismatch,
                                   "DEFAULT exceeds VARCHAR length");
        }
        fields.push_back({ColumnId{static_cast<std::uint64_t>(fields.size()) + 1},
                          std::move(name), column.type, column.varchar_length,
                          column.primary_key, column.not_null || column.primary_key,
                          column.unique || column.primary_key, column.default_value});
    }
    auto schema = std::make_shared<const TableSchema>(TableSchema{
        TableId{next_table_id_}, table_name, std::move(fields)});
    tables_.emplace(table_name, schema);
    ++next_table_id_;
    ++version_;
    return schema;
}

Result<std::size_t> MemoryCatalog::dropTables(
    const std::vector<std::string>& table_names, bool if_exists) {
    std::unordered_set<std::string> normalized;
    for (const auto& raw : table_names) {
        auto name = normalizeName(raw);
        if (!normalized.insert(name).second)
            return schemaError(ErrorCode::DuplicateTable,
                               "duplicate table '" + raw + "' in DROP TABLE");
        if (!if_exists && tables_.count(name) == 0)
            return schemaError(ErrorCode::TableNotFound,
                               "table '" + raw + "' does not exist");
    }
    std::size_t removed = 0;
    for (const auto& name : normalized) removed += tables_.erase(name);
    if (removed != 0) ++version_;
    return removed;
}

std::shared_ptr<const CatalogSnapshot> MemoryCatalog::snapshot() const {
    return std::make_shared<const MemorySnapshot>(version_, tables_);
}

} // namespace minisql
