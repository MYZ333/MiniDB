// 内存 Catalog 的写入口与不可变快照实现。分析器只看得到 CatalogSnapshot。
#include "minisql/memory_catalog.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace minisql {
namespace {

// 私有具体快照，调用方只能通过只读虚接口访问。
class MemorySnapshot final : public CatalogSnapshot {
public:
    using Tables = std::unordered_map<std::string, std::shared_ptr<const TableSchema>>;
    using Indexes = std::unordered_map<std::string, std::shared_ptr<const IndexSchema>>;
    MemorySnapshot(CatalogVersion version, Tables tables, Indexes indexes,
                   std::uint64_t next_index_id)
        : version_(version), tables_(std::move(tables)), indexes_(std::move(indexes)),
          next_index_id_(next_index_id) {}
    CatalogVersion version() const noexcept override { return version_; }
    std::shared_ptr<const TableSchema> findTable(std::string_view normalized_name) const override {
        const auto found = tables_.find(std::string(normalized_name));
        return found == tables_.end() ? nullptr : found->second;
    }
    std::shared_ptr<const IndexSchema> findIndex(std::string_view normalized_name) const override {
        const auto found = indexes_.find(std::string(normalized_name));
        return found == indexes_.end() ? nullptr : found->second;
    }
    IndexId nextIndexId() const noexcept override { return IndexId{next_index_id_}; }
private:
    const CatalogVersion version_;
    const Tables tables_;
    const Indexes indexes_;
    const std::uint64_t next_index_id_;
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

std::optional<std::size_t> columnOrdinal(const TableSchema& table, std::string_view name) {
    for (std::size_t i = 0; i < table.columns.size(); ++i)
        if (table.columns[i].name == name) return i;
    return std::nullopt;
}
} // namespace

Result<std::shared_ptr<const TableSchema>> MemoryCatalog::createTable(
    std::string table_name, const std::vector<ColumnSpec>& columns,
    const std::vector<TableConstraintSpec>& table_constraints, bool if_not_exists) {
    table_name = normalizeName(std::move(table_name));
    if (const auto existing = tables_.find(table_name); existing != tables_.end()) {
        if (if_not_exists) return existing->second;
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
    bool table_primary_key = has_primary_key;
    for (const auto& constraint : table_constraints) {
        if (constraint.columns.empty())
            return schemaError(ErrorCode::InvalidAst, "table constraint requires columns");
        if (constraint.primary_key && table_primary_key)
            return schemaError(ErrorCode::InvalidAst, "table may contain only one PRIMARY KEY");
        table_primary_key = table_primary_key || constraint.primary_key;
        std::unordered_set<std::size_t> members;
        for (const auto ordinal : constraint.columns) {
            if (ordinal >= fields.size() || !members.insert(ordinal).second)
                return schemaError(ErrorCode::InvalidAst,
                                   "table constraint column is invalid or duplicated");
            if (constraint.primary_key && !fields[ordinal].not_null)
                fields[ordinal].not_null = true;
        }
    }
    auto schema = std::make_shared<const TableSchema>(TableSchema{
        TableId{next_table_id_}, table_name, std::move(fields), table_constraints, {}});
    tables_.emplace(table_name, schema);
    ++next_table_id_;
    ++version_;
    return schema;
}

Result<std::shared_ptr<const TableSchema>> MemoryCatalog::addColumn(
    std::string table_name, const ColumnSpec& column) {
    table_name = normalizeName(std::move(table_name));
    const auto found = tables_.find(table_name);
    if (found == tables_.end())
        return schemaError(ErrorCode::TableNotFound, "table '" + table_name + "' does not exist");
    auto name = normalizeName(column.name);
    for (const auto& present : found->second->columns)
        if (present.name == name)
            return schemaError(ErrorCode::DuplicateColumn, "duplicate column '" + name + "'");
    if (column.primary_key) {
        for (const auto& present : found->second->columns)
            if (present.primary_key)
                return schemaError(ErrorCode::InvalidAst,
                                   "table may contain only one PRIMARY KEY");
        for (const auto& constraint : found->second->table_constraints)
            if (constraint.primary_key)
                return schemaError(ErrorCode::InvalidAst,
                                   "table may contain only one PRIMARY KEY");
    }
    if (column.type == DataType::Null ||
        (column.varchar_length &&
         (column.type != DataType::Varchar || *column.varchar_length <= 0)))
        return schemaError(ErrorCode::UnsupportedType, "invalid added column type");
    if (column.default_value) {
        const auto actual = scalarType(*column.default_value);
        if ((column.not_null || column.primary_key) && actual == DataType::Null)
            return schemaError(ErrorCode::TypeMismatch, "NOT NULL column cannot default to NULL");
        if (actual != DataType::Null && actual != column.type)
            return schemaError(ErrorCode::TypeMismatch, "DEFAULT value type does not match column");
        if (column.varchar_length && actual == DataType::Varchar &&
            utf8Length(std::get<std::string>(*column.default_value)) >
                static_cast<std::size_t>(*column.varchar_length))
            return schemaError(ErrorCode::TypeMismatch, "DEFAULT exceeds VARCHAR length");
    }
    auto fields = found->second->columns;
    std::uint64_t next_column_id = 1;
    for (const auto& field : fields) next_column_id = std::max(next_column_id, field.id.value + 1);
    fields.push_back({ColumnId{next_column_id}, std::move(name), column.type,
                      column.varchar_length, column.primary_key,
                      column.not_null || column.primary_key,
                      column.unique || column.primary_key, column.default_value});
    auto schema = std::make_shared<const TableSchema>(TableSchema{
        found->second->id, table_name, std::move(fields),
        found->second->table_constraints, found->second->indexes});
    found->second = schema;
    ++version_;
    return schema;
}

Result<std::shared_ptr<const TableSchema>> MemoryCatalog::dropColumn(
    std::string table_name, std::string column_name) {
    table_name = normalizeName(std::move(table_name));
    column_name = normalizeName(std::move(column_name));
    const auto found = tables_.find(table_name);
    if (found == tables_.end())
        return schemaError(ErrorCode::TableNotFound, "table '" + table_name + "' does not exist");
    std::size_t ordinal = found->second->columns.size();
    for (std::size_t i = 0; i < found->second->columns.size(); ++i)
        if (found->second->columns[i].name == column_name) ordinal = i;
    if (ordinal == found->second->columns.size())
        return schemaError(ErrorCode::ColumnNotFound, "column '" + column_name + "' does not exist");
    if (found->second->columns.size() == 1)
        return schemaError(ErrorCode::EmptyColumnList, "cannot drop the last table column");
    for (const auto& index : found->second->indexes)
        if (index.column_id.value == found->second->columns[ordinal].id.value)
            return schemaError(ErrorCode::UnsupportedIndex,
                               "cannot drop a column used by an index");
    for (const auto& constraint : found->second->table_constraints)
        for (const auto member : constraint.columns)
            if (member == ordinal)
                return schemaError(ErrorCode::InvalidAst,
                                   "cannot drop a column used by a table constraint");
    auto fields = found->second->columns;
    fields.erase(fields.begin() + static_cast<std::ptrdiff_t>(ordinal));
    auto constraints = found->second->table_constraints;
    for (auto& constraint : constraints)
        for (auto& member : constraint.columns) if (member > ordinal) --member;
    auto schema = std::make_shared<const TableSchema>(TableSchema{
        found->second->id, table_name, std::move(fields), std::move(constraints),
        found->second->indexes});
    found->second = schema;
    ++version_;
    return schema;
}

Result<std::shared_ptr<const TableSchema>> MemoryCatalog::renameTable(
    std::string table_name, std::string new_name) {
    table_name = normalizeName(std::move(table_name));
    new_name = normalizeName(std::move(new_name));
    const auto found = tables_.find(table_name);
    if (found == tables_.end())
        return schemaError(ErrorCode::TableNotFound, "table '" + table_name + "' does not exist");
    if (tables_.count(new_name))
        return schemaError(ErrorCode::TableAlreadyExists, "table '" + new_name + "' already exists");
    auto schema = std::make_shared<const TableSchema>(TableSchema{
        found->second->id, new_name, found->second->columns,
        found->second->table_constraints, found->second->indexes});
    tables_.erase(found);
    tables_.emplace(new_name, schema);
    ++version_;
    return schema;
}

Result<std::shared_ptr<const TableSchema>> MemoryCatalog::renameColumn(
    std::string table_name, std::string column_name, std::string new_name) {
    table_name = normalizeName(std::move(table_name));
    column_name = normalizeName(std::move(column_name));
    new_name = normalizeName(std::move(new_name));
    const auto found = tables_.find(table_name);
    if (found == tables_.end())
        return schemaError(ErrorCode::TableNotFound, "table '" + table_name + "' does not exist");
    auto fields = found->second->columns;
    std::size_t ordinal = fields.size();
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == new_name)
            return schemaError(ErrorCode::DuplicateColumn, "duplicate column '" + new_name + "'");
        if (fields[i].name == column_name) ordinal = i;
    }
    if (ordinal == fields.size())
        return schemaError(ErrorCode::ColumnNotFound, "column '" + column_name + "' does not exist");
    fields[ordinal].name = new_name;
    auto schema = std::make_shared<const TableSchema>(TableSchema{
        found->second->id, table_name, std::move(fields),
        found->second->table_constraints, found->second->indexes});
    found->second = schema;
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
    std::unordered_set<std::uint64_t> removed_table_ids;
    for (const auto& name : normalized) {
        if (const auto found = tables_.find(name); found != tables_.end())
            removed_table_ids.insert(found->second->id.value);
        removed += tables_.erase(name);
    }
    for (auto it = indexes_.begin(); it != indexes_.end();) {
        if (removed_table_ids.count(it->second->table_id.value)) it = indexes_.erase(it);
        else ++it;
    }
    if (removed != 0) ++version_;
    return removed;
}

Result<std::shared_ptr<const IndexSchema>> MemoryCatalog::createIndex(
    std::string index_name, std::string table_name, std::string column_name,
    DataType key_type, bool unique, std::int64_t metadata_page_id,
    std::optional<IndexId> forced_id) {
    index_name = normalizeName(std::move(index_name));
    table_name = normalizeName(std::move(table_name));
    column_name = normalizeName(std::move(column_name));
    if (indexes_.count(index_name))
        return schemaError(ErrorCode::DuplicateIndex,
                           "index '" + index_name + "' already exists");
    const auto table_found = tables_.find(table_name);
    if (table_found == tables_.end())
        return schemaError(ErrorCode::TableNotFound, "table '" + table_name + "' does not exist");
    auto table = table_found->second;
    auto ordinal = columnOrdinal(*table, column_name);
    if (!ordinal)
        return schemaError(ErrorCode::ColumnNotFound,
                           "column '" + column_name + "' does not exist");
    const auto& column = table->columns[*ordinal];
    if (column.type != DataType::Int || key_type != DataType::Int ||
        !(column.not_null || column.primary_key) || !unique)
        return schemaError(ErrorCode::UnsupportedIndex,
                           "only unique NOT NULL INT indexes are supported");
    const IndexId id = forced_id.value_or(IndexId{next_index_id_});
    auto index = std::make_shared<const IndexSchema>(IndexSchema{
        id, index_name, table->id, column.id, key_type, unique, metadata_page_id});
    auto indexes = table->indexes;
    indexes.push_back(*index);
    auto schema = std::make_shared<const TableSchema>(TableSchema{
        table->id, table->name, table->columns, table->table_constraints, std::move(indexes)});
    table_found->second = schema;
    indexes_.emplace(index_name, index);
    next_index_id_ = std::max(next_index_id_, id.value + 1);
    ++version_;
    return index;
}

Result<std::size_t> MemoryCatalog::dropIndex(std::string index_name, bool if_exists) {
    index_name = normalizeName(std::move(index_name));
    const auto found = indexes_.find(index_name);
    if (found == indexes_.end()) {
        if (if_exists) return std::size_t{0};
        return schemaError(ErrorCode::IndexNotFound,
                           "index '" + index_name + "' does not exist");
    }
    const auto index = found->second;
    for (auto& [name, table] : tables_) {
        if (table->id.value != index->table_id.value) continue;
        auto indexes = table->indexes;
        indexes.erase(std::remove_if(indexes.begin(), indexes.end(),
            [&](const IndexSchema& candidate) {
                return candidate.id.value == index->id.value;
            }), indexes.end());
        table = std::make_shared<const TableSchema>(TableSchema{
            table->id, table->name, table->columns, table->table_constraints,
            std::move(indexes)});
        break;
    }
    indexes_.erase(found);
    ++version_;
    return std::size_t{1};
}

std::shared_ptr<const CatalogSnapshot> MemoryCatalog::snapshot() const {
    return std::make_shared<const MemorySnapshot>(version_, tables_, indexes_, next_index_id_);
}

void MemoryCatalog::loadSnapshot(CatalogVersion version, std::uint64_t next_table_id,
                                 std::vector<TableSchema> tables,
                                 std::vector<IndexSchema> indexes) {
    tables_.clear();
    indexes_.clear();
    std::uint64_t max_index_id = 0;
    for (const auto& index : indexes) {
        max_index_id = std::max(max_index_id, index.id.value);
    }
    for (auto& table : tables) {
        table.indexes.clear();
        for (const auto& index : indexes)
            if (index.table_id.value == table.id.value) table.indexes.push_back(index);
        auto owned = std::make_shared<const TableSchema>(std::move(table));
        tables_.emplace(owned->name, std::move(owned));
    }
    for (auto& index : indexes) {
        auto owned = std::make_shared<const IndexSchema>(std::move(index));
        indexes_.emplace(owned->name, std::move(owned));
    }
    version_ = version;
    next_table_id_ = next_table_id;
    next_index_id_ = std::max<std::uint64_t>(1, max_index_id + 1);
}

} // namespace minisql
