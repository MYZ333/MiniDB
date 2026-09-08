// 骨架联调检查：确保 A/B 的公共接口可以共同编译、链接，且占位入口不会假报成功。
// 后续实现对应模块时，应把占位断言替换成真正的模块行为测试。
#include "minisql/compiler.hpp"
#include "minisql/lexer.hpp"
#include "minisql/parser.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace minisql;

// 空快照只是接口替身；调用次数用于检查占位入口不会访问或修改 Catalog。
class EmptyCatalog final : public CatalogSnapshot {
public:
    CatalogVersion version() const noexcept override { return 0; }
    std::shared_ptr<const TableSchema> findTable(std::string_view) const override {
        ++lookups;
        return nullptr;
    }
    mutable unsigned lookups = 0;
};

template <typename T>
void expectPlaceholder(const Result<T>& result, DiagnosticStage stage) {
    const auto* error = std::get_if<Diagnostic>(&result);
    if (!error || error->code != ErrorCode::NotImplemented || error->stage != stage) {
        throw std::runtime_error("scaffold entry point unexpectedly reports real results");
    }
}
} // namespace

int main() {
    try {
        // 分别调用四个入口，不把 lex 的错误作为 parse 的输入继续传递。
        expectPlaceholder(lex("SELECT * FROM student;"), DiagnosticStage::Lexical);
        const TokenStream tokens{{TokenKind::EndOfInput, "", {{0, 1, 1}, {0, 1, 1}}}};
        expectPlaceholder(parse(tokens), DiagnosticStage::Syntax);
        const Statement ast{CreateTableStmt{{"student", std::nullopt},
            {{{"id", std::nullopt}, DataType::Int, std::nullopt}}}, std::nullopt};
        EmptyCatalog catalog;
        expectPlaceholder(analyze(ast, catalog), DiagnosticStage::Semantic);
        const BoundStatement bound{0, BoundCreateTable{"student", {{"id", DataType::Int}}}};
        expectPlaceholder(buildPlan(bound), DiagnosticStage::Plan);
        if (catalog.lookups != 0) {
            throw std::runtime_error("placeholder unexpectedly queries catalog");
        }
        std::cout << "Scaffold interfaces link; all four entry points report NotImplemented.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
