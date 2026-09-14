// 第一阶段可运行教材：手工构造 AST、绑定结果和计划，展示三个接口层的区别。
// 注意：这里不解析 SQL、不调用 analyze/buildPlan，也不读写数据库记录。
#include "minisql/compiler.hpp"

#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
using namespace minisql;

// 示例用未知源码范围；真实 Parser 必须填入 SQL 对应的范围。
Identifier name(std::string text) { return {std::move(text), std::nullopt}; }

ExprPtr column(const std::string& text) {
    return std::make_shared<const Expr>(Expr{IdentifierExpr{name(text)}, std::nullopt});
}

ExprPtr integer(std::int64_t value) {
    return std::make_shared<const Expr>(Expr{LiteralExpr{value}, std::nullopt});
}

ExprPtr binary(BinaryOp op, ExprPtr left, ExprPtr right) {
    return std::make_shared<const Expr>(Expr{
        BinaryExpr{op, std::move(left), std::move(right), std::nullopt}, std::nullopt});
}

BoundExprPtr boundColumn(BoundColumnRef ref) {
    return std::make_shared<const BoundExpr>(BoundExpr{ref, ref.type, std::nullopt});
}

BoundExprPtr boundInteger(std::int64_t value) {
    return std::make_shared<const BoundExpr>(BoundExpr{
        BoundLiteral{value}, DataType::Int, std::nullopt});
}

// result_type 由示例显式提供；真正的语义分析器需要根据运算符推导并检查它。
BoundExprPtr boundBinary(BinaryOp op, BoundExprPtr left, BoundExprPtr right,
                         DataType result_type) {
    return std::make_shared<const BoundExpr>(BoundExpr{
        BoundBinary{op, std::move(left), std::move(right), std::nullopt},
        result_type, std::nullopt});
}

// 不用 assert，保证 Release 构建也会执行契约检查。
void require(bool condition, const char* explanation) {
    if (!condition) {
        throw std::runtime_error(explanation);
    }
}

// 固定数据夹具，仅证明 Catalog 接口可以被替换；它不是可变内存 Catalog。
class ExampleCatalog final : public CatalogSnapshot {
public:
    CatalogVersion version() const noexcept override { return 1; }

    std::shared_ptr<const TableSchema> findTable(std::string_view key) const override {
        return key == table_->name ? table_ : nullptr;
    }

private:
    const std::shared_ptr<const TableSchema> table_ =
        std::make_shared<const TableSchema>(TableSchema{
            TableId{100}, "student",
            {{ColumnId{1}, "id", DataType::Int},
             {ColumnId{2}, "name", DataType::Varchar},
             {ColumnId{3}, "age", DataType::Int}}});
};

BoundColumnRef ref(const TableSchema& table, std::size_t ordinal) {
    const auto& field = table.columns.at(ordinal);
    return {table.id, field.id, ordinal, field.type};
}

std::vector<OutputColumn> schema(const TableSchema& table) {
    std::vector<OutputColumn> result;
    for (const auto& field : table.columns) {
        result.push_back({field.name, field.type});
    }
    return result;
}

// 仅为展示手工算子树准备样板；没有从 AST 生成计划的逻辑。
PlanPtr scan(const std::shared_ptr<const TableSchema>& table, bool with_row_id) {
    return std::make_shared<const PlanNode>(PlanNode{
        SeqScanPlan{table}, schema(*table), with_row_id});
}

PlanPtr filter(BoundExprPtr condition, PlanPtr input) {
    auto output = input->output;
    const bool row_id = input->carries_row_id;
    return std::make_shared<const PlanNode>(PlanNode{
        FilterPlan{std::move(condition), std::move(input)}, std::move(output), row_id});
}

// 从实际计划节点读取名称和子节点，避免打印一条与对象无关的硬编码计划。
void printOutline(const PlanNode& plan) {
    std::visit([](const auto& op) {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, CreateTablePlan>) std::cout << "CreateTable";
        else if constexpr (std::is_same_v<T, DropTablePlan>) std::cout << "DropTable";
        else if constexpr (std::is_same_v<T, InsertPlan>) std::cout << "Insert";
        else if constexpr (std::is_same_v<T, SeqScanPlan>) std::cout << "SeqScan";
        else if constexpr (std::is_same_v<T, NestedLoopJoinPlan>) {
            std::cout << "NestedLoopJoin -> (";
            printOutline(*op.left);
            std::cout << ", ";
            printOutline(*op.right);
            std::cout << ")";
        }
        else {
            if constexpr (std::is_same_v<T, FilterPlan>) std::cout << "Filter";
            else if constexpr (std::is_same_v<T, GroupByPlan>) std::cout << "GroupBy";
            else if constexpr (std::is_same_v<T, SortPlan>) std::cout << "Sort";
            else if constexpr (std::is_same_v<T, ProjectPlan>) std::cout << "Project";
            else if constexpr (std::is_same_v<T, UpdatePlan>) std::cout << "Update";
            else if constexpr (std::is_same_v<T, DeletePlan>) std::cout << "Delete";
            std::cout << " -> ";
            printOutline(*op.input);
        }
    }, plan.node);
}

void show(const char* sql, const LogicalPlan& plan) {
    std::cout << sql << '\n' << "  manual plan (catalog version "
              << plan.catalog_version << "): ";
    printOutline(*plan.root);
    std::cout << '\n';
}

void demonstrateCreate() {
    Statement ast{CreateTableStmt{name("student"),
        {{name("id"), DataType::Int, std::nullopt},
         {name("name"), DataType::Varchar, std::nullopt},
         {name("age"), DataType::Int, std::nullopt}}}, std::nullopt};

    // 模拟语义分析已完成规范化与重名检查；CREATE 不分配数据库 ID。
    BoundStatement bound{0, BoundCreateTable{"student",
        {{"id", DataType::Int}, {"name", DataType::Varchar}, {"age", DataType::Int}}}};
    const auto& binding = std::get<BoundCreateTable>(bound.node);
    LogicalPlan plan{bound.catalog_version, std::make_shared<const PlanNode>(PlanNode{
        CreateTablePlan{binding.table_name, binding.columns}, {}, false})};

    require(std::get<CreateTableStmt>(ast.node).columns.size() == binding.columns.size(),
            "CREATE must retain every column definition");
    require(plan.root->output.empty(), "CREATE has no business output columns");
    show("CREATE TABLE student(id INT, name VARCHAR, age INT);", plan);
}

void demonstrateInsert(const ExampleCatalog& catalog) {
    auto table = catalog.findTable("student");
    Statement ast{InsertStmt{name("student"),
        std::vector<Identifier>{name("name"), name("age"), name("id")},
        {{std::string{"Alice"}, std::nullopt}, {std::int64_t{20}, std::nullopt},
         {std::int64_t{1}, std::nullopt}}}, std::nullopt};

    // SQL 的 name,age,id 顺序在绑定后变成模式的 id,name,age 顺序。
    BoundStatement bound{catalog.version(), BoundInsert{table,
        {std::int64_t{1}, std::string{"Alice"}, std::int64_t{20}}}};
    const auto& binding = std::get<BoundInsert>(bound.node);
    LogicalPlan plan{bound.catalog_version, std::make_shared<const PlanNode>(PlanNode{
        InsertPlan{binding.table, binding.values}, {}, false})};

    const auto& input = std::get<InsertStmt>(ast.node);
    const auto& op = std::get<InsertPlan>(plan.root->node);
    require(input.columns->front().text == "name", "AST retains SQL column order");
    require(std::get<std::int64_t>(op.values.at(0)) == 1 &&
            std::get<std::string>(op.values.at(1)) == "Alice" &&
            std::get<std::int64_t>(op.values.at(2)) == 20,
            "bound INSERT must use schema order");
    show("INSERT INTO student(name, age, id) VALUES ('Alice', 20, 1);", plan);
}

void demonstrateSelect(const ExampleCatalog& catalog) {
    auto table = catalog.findTable("student");
    Statement ast{SelectStmt{name("student"), std::vector<Identifier>{name("name")},
        binary(BinaryOp::Greater, column("age"), integer(18))}, std::nullopt};

    auto predicate = boundBinary(BinaryOp::Greater, boundColumn(ref(*table, 2)),
                                  boundInteger(18), DataType::Bool);
    BoundStatement bound{catalog.version(), BoundSelect{table, {ref(*table, 1)}, predicate}};
    const auto& binding = std::get<BoundSelect>(bound.node);
    auto input = filter(binding.where, scan(table, false));
    LogicalPlan plan{bound.catalog_version, std::make_shared<const PlanNode>(PlanNode{
        ProjectPlan{binding.columns, input}, {{"name", DataType::Varchar}}, false})};

    require(std::get<SelectStmt>(ast.node).where != nullptr, "SELECT retains WHERE");
    require(binding.where->type == DataType::Bool, "bound WHERE must be BOOL");
    require(input->output.size() == 3 && plan.root->output.size() == 1,
            "scan/filter retain age for filtering, project only outputs name");
    show("SELECT name FROM student WHERE age > 18;", plan);
}

void demonstrateUpdate(const ExampleCatalog& catalog) {
    auto table = catalog.findTable("student");
    Statement ast{UpdateStmt{name("student"),
        {{name("age"), binary(BinaryOp::Add, column("age"), integer(1)), std::nullopt}},
        binary(BinaryOp::Equal, column("id"), integer(1))}, std::nullopt};

    // RHS 保留列引用；不能在编译期把 age + 1 当作一个常量值。
    auto rhs = boundBinary(BinaryOp::Add, boundColumn(ref(*table, 2)),
                            boundInteger(1), DataType::Int);
    auto predicate = boundBinary(BinaryOp::Equal, boundColumn(ref(*table, 0)),
                                  boundInteger(1), DataType::Bool);
    BoundStatement bound{catalog.version(), BoundUpdate{table, {{ref(*table, 2), rhs}}, predicate}};
    const auto& binding = std::get<BoundUpdate>(bound.node);
    auto input = filter(binding.where, scan(table, true));
    LogicalPlan plan{bound.catalog_version, std::make_shared<const PlanNode>(PlanNode{
        UpdatePlan{table, binding.assignments, input}, {}, false})};

    require(std::get<UpdateStmt>(ast.node).assignments.size() == 1, "one UPDATE assignment");
    const auto& op = std::get<UpdatePlan>(plan.root->node);
    const auto& sum = std::get<BoundBinary>(op.assignments.front().value->node);
    require(std::get<BoundColumnRef>(sum.left->node).ordinal == 2,
            "UPDATE RHS reads original age column");
    require(op.input->carries_row_id, "UPDATE input requires row identity");
    show("UPDATE student SET age = age + 1 WHERE id = 1;", plan);
}

void demonstrateDelete(const ExampleCatalog& catalog) {
    auto table = catalog.findTable("student");
    Statement ast{DeleteStmt{name("student"),
        binary(BinaryOp::Equal, column("id"), integer(1))}, std::nullopt};
    auto predicate = boundBinary(BinaryOp::Equal, boundColumn(ref(*table, 0)),
                                  boundInteger(1), DataType::Bool);
    BoundStatement bound{catalog.version(), BoundDelete{table, predicate}};
    const auto& binding = std::get<BoundDelete>(bound.node);
    auto input = filter(binding.where, scan(table, true));
    LogicalPlan plan{bound.catalog_version, std::make_shared<const PlanNode>(PlanNode{
        DeletePlan{table, input}, {}, false})};

    require(std::get<DeleteStmt>(ast.node).where != nullptr, "DELETE retains WHERE");
    const auto& op = std::get<DeletePlan>(plan.root->node);
    require(op.input->carries_row_id && plan.root->output.empty(),
            "DELETE consumes row identity without business output");
    show("DELETE FROM student WHERE id = 1;", plan);
}

void demonstrateDiagnostic() {
    // SELECT score FROM student; 中 score 的字节范围为 [7,12)。
    Result<BoundStatement> failure = Diagnostic{DiagnosticStage::Semantic,
        ErrorCode::ColumnNotFound, "column 'score' does not exist in table 'student'",
        SourceSpan{{7, 1, 8}, {12, 1, 13}}};
    const auto* error = std::get_if<Diagnostic>(&failure);
    require(error != nullptr && error->span->begin.column == 8 &&
            error->span->end.offset == 12, "diagnostic preserves precise source range");
}
} // namespace

int main() {
    try {
        ExampleCatalog catalog;
        require(catalog.findTable(normalizeName("StUdEnT")) != nullptr,
                "name lookup uses shared ASCII normalization");
        require(catalog.findTable("missing") == nullptr, "missing table returns nullptr");
        demonstrateCreate();
        demonstrateInsert(catalog);
        demonstrateSelect(catalog);
        demonstrateUpdate(catalog);
        demonstrateDelete(catalog);
        demonstrateDiagnostic();
        require(catalog.version() == 1, "examples must not mutate catalog");
        std::cout << "Contract examples passed (manual structures; no SQL execution).\n";
    } catch (const std::exception& error) {
        std::cerr << "Contract example failed: " << error.what() << '\n';
        return 1;
    }
}
