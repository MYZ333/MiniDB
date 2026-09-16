// 验证注册失败的原子性、模式 ID 和快照生命周期；不测试数据库记录执行。
#include "minisql/memory_catalog.hpp"
#include "../test_support.hpp"

using namespace minisql;
using namespace minisql::test;

int main() {
    Suite suite;
    suite.run("snapshot isolation and normalized schema", [] {
        MemoryCatalog catalog;
        auto before = catalog.snapshot();
        auto table = value(catalog.createTable("StUdEnT", {{"ID", DataType::Int}, {"Name", DataType::Varchar}}));
        auto after = catalog.snapshot();
        check(before->version() == 0 && !before->findTable("student"), "old snapshot changed");
        check(after->version() == 1 && after->findTable("student") == table, "new snapshot misses schema");
        check(table->name == "student" && table->columns[0].name == "id" && table->columns[1].name == "name", "normalization/order mismatch");
        check(table->columns[0].id.value != table->columns[1].id.value, "column IDs must be unique");
    });
    suite.run("duplicate table leaves version and ID allocation unchanged", [] {
        MemoryCatalog catalog;
        auto first = value(catalog.createTable("Student", {{"id", DataType::Int}}));
        failure(catalog.createTable("STUDENT", {{"other", DataType::Int}}), ErrorCode::TableAlreadyExists, DiagnosticStage::Execution);
        check(catalog.snapshot()->version() == 1, "failed registration changed version");
        auto next = value(catalog.createTable("course", {{"id", DataType::Int}}));
        check(next->id.value == first->id.value + 1, "failed registration consumed an ID");
        check(catalog.snapshot()->findTable("student") == first, "original schema was replaced");
    });
    suite.run("invalid schema never publishes a partial table", [] {
        MemoryCatalog catalog;
        failure(catalog.createTable("bad", {}), ErrorCode::EmptyColumnList, DiagnosticStage::Execution);
        failure(catalog.createTable("bad", {{"id", DataType::Int}, {"ID", DataType::Int}}), ErrorCode::DuplicateColumn, DiagnosticStage::Execution);
        failure(catalog.createTable("bad", {{"nothing", DataType::Null}}), ErrorCode::UnsupportedType, DiagnosticStage::Execution);
        check(catalog.snapshot()->version() == 0 && !catalog.snapshot()->findTable("bad"), "invalid table leaked");
        check(value(catalog.createTable("ok", {{"id", DataType::Int}}))->id.value == 1, "errors consumed table IDs");
    });
    suite.run("BOOL and FLOAT schemas are publishable", [] {
        MemoryCatalog catalog;
        const auto table = value(catalog.createTable("metrics",
            {{"active", DataType::Bool}, {"score", DataType::Float}}));
        check(table->columns[0].type == DataType::Bool &&
              table->columns[1].type == DataType::Float, "extended types were not retained");
    });
    suite.run("column constraints survive registration and invalid defaults are atomic", [] {
        MemoryCatalog catalog;
        ColumnSpec key{"id", DataType::Int};
        key.primary_key = true;
        ColumnSpec name{"name", DataType::Varchar};
        name.varchar_length = 4;
        name.not_null = true;
        name.default_value = ScalarValue{std::string{"Ann"}};
        const auto table = value(catalog.createTable("account", {key, name}));
        check(table->columns[0].primary_key && table->columns[0].not_null &&
              table->columns[0].unique && table->columns[1].varchar_length == 4 &&
              std::get<std::string>(*table->columns[1].default_value) == "Ann",
              "constraint metadata was not retained");
        name.default_value = ScalarValue{std::string{"TooLong"}};
        failure(catalog.createTable("bad", {name}), ErrorCode::TypeMismatch,
                DiagnosticStage::Execution);
        check(catalog.snapshot()->version() == 1 && !catalog.snapshot()->findTable("bad"),
              "invalid default published a partial schema");
    });
    suite.run("DROP validates atomically and advances version once", [] {
        MemoryCatalog catalog;
        const auto first = value(catalog.createTable("first", {{"id", DataType::Int}}));
        value(catalog.createTable("second", {{"id", DataType::Int}}));
        const auto before = catalog.snapshot();
        failure(catalog.dropTables({"first", "missing"}, false), ErrorCode::TableNotFound,
                DiagnosticStage::Execution);
        check(catalog.snapshot()->version() == 2 && catalog.snapshot()->findTable("first"),
              "failed DROP changed the catalog");
        check(value(catalog.dropTables({"FIRST", "missing"}, true)) == 1,
              "IF EXISTS removed count mismatch");
        check(catalog.snapshot()->version() == 3 && !catalog.snapshot()->findTable("first") &&
              before->findTable("first") == first, "DROP version or snapshot isolation mismatch");
        check(value(catalog.dropTables({"missing"}, true)) == 0 &&
              catalog.snapshot()->version() == 3, "missing-only DROP changed version");
    });
    suite.run("IF NOT EXISTS and composite constraints preserve catalog state", [] {
        MemoryCatalog catalog;
        const std::vector<TableConstraintSpec> constraints{
            {true, {0, 1}}, {false, {0, 2}}};
        const auto table = value(catalog.createTable("enrollment",
            {{"student_id", DataType::Int}, {"course_id", DataType::Int},
             {"grade", DataType::Int}}, constraints));
        check(table->table_constraints.size() == 2 && table->columns[0].not_null &&
              table->columns[1].not_null, "composite constraints were not retained");
        const auto existing = value(catalog.createTable(
            "ENROLLMENT", {{"ignored", DataType::Int}}, {}, true));
        check(existing == table && catalog.snapshot()->version() == 1,
              "IF NOT EXISTS changed or replaced the catalog");
    });
    suite.run("ALTER publishes immutable schemas and stable IDs", [] {
        MemoryCatalog catalog;
        const auto original = value(catalog.createTable("student",
            {{"id", DataType::Int}, {"name", DataType::Varchar}}));
        ColumnSpec age{"age", DataType::Int};
        age.default_value = ScalarValue{std::int64_t{18}};
        const auto added = value(catalog.addColumn("student", age));
        check(added->id.value == original->id.value && added->columns[0].id.value ==
              original->columns[0].id.value && added->columns[2].name == "age",
              "ADD changed stable identities");
        const auto renamedColumn = value(catalog.renameColumn("student", "name", "full_name"));
        check(renamedColumn->columns[1].name == "full_name" &&
              original->columns[1].name == "name", "RENAME COLUMN mutated an old snapshot");
        const auto dropped = value(catalog.dropColumn("student", "age"));
        check(dropped->columns.size() == 2, "DROP COLUMN did not update the schema");
        const auto renamedTable = value(catalog.renameTable("student", "person"));
        check(renamedTable->id.value == original->id.value &&
              !catalog.snapshot()->findTable("student") &&
              catalog.snapshot()->findTable("person") == renamedTable &&
              catalog.snapshot()->version() == 5,
              "RENAME TABLE changed ID, index, or version incorrectly");
    });
    suite.run("snapshot outlives mutable catalog", [] {
        std::shared_ptr<const CatalogSnapshot> snapshot;
        {
            MemoryCatalog temporary;
            value(temporary.createTable("student", {{"id", DataType::Int}}));
            snapshot = temporary.snapshot();
        }
        check(snapshot->findTable("student")->columns[0].name == "id", "dangling snapshot");
    });
    return suite.finish();
}
