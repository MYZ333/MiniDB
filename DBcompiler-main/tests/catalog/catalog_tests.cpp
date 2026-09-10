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
