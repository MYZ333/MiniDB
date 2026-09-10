#pragma once

// 无第三方依赖的测试辅助：失败抛出测试异常，逐用例报告；Release 也不跳过断言。
#include "minisql/compiler.hpp"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace minisql::test {
inline void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
T value(Result<T> result) {
    if (const auto* error = std::get_if<Diagnostic>(&result)) throw std::runtime_error(error->message);
    return std::get<T>(std::move(result));
}

template <typename T>
Diagnostic failure(Result<T> result, ErrorCode code,
                   DiagnosticStage stage = DiagnosticStage::Semantic) {
    const auto* error = std::get_if<Diagnostic>(&result);
    check(error != nullptr, "expected diagnostic, got success");
    check(error->code == code, "unexpected diagnostic code");
    check(error->stage == stage, "unexpected diagnostic stage");
    check(!error->message.empty(), "diagnostic needs an explanation");
    return *error;
}

// 手工 AST 使用单行示例范围；精确位置断言与 Parser 无关。
inline SourceSpan span(std::size_t offset, std::size_t length = 1) {
    return {{offset, 1, offset + 1}, {offset + length, 1, offset + length + 1}};
}
inline Identifier id(std::string name, SourceLocation at = std::nullopt) {
    return {std::move(name), at};
}
inline ExprPtr col(std::string name, SourceLocation at = std::nullopt) {
    return std::make_shared<const Expr>(Expr{IdentifierExpr{id(std::move(name), at)}, at});
}
inline ExprPtr lit(LiteralValue literal, SourceLocation at = std::nullopt) {
    return std::make_shared<const Expr>(Expr{LiteralExpr{std::move(literal)}, at});
}
inline ExprPtr num(std::int64_t n) { return lit(n); }
inline ExprPtr text(std::string s) { return lit(std::move(s)); }
inline ExprPtr bin(BinaryOp op, ExprPtr left, ExprPtr right, SourceLocation at = std::nullopt) {
    return std::make_shared<const Expr>(Expr{
        BinaryExpr{op, std::move(left), std::move(right), at}, at});
}
inline ExprPtr un(UnaryOp op, ExprPtr operand, SourceLocation at = std::nullopt) {
    return std::make_shared<const Expr>(Expr{UnaryExpr{op, std::move(operand), at}, at});
}
inline ExprPtr truth() { return bin(BinaryOp::Equal, num(1), num(1)); }

class Suite {
public:
    template <typename F>
    void run(const char* name, F test) {
        try { test(); ++passed_; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failed_; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    }
    int finish() const {
        std::cout << passed_ << " passed, " << failed_ << " failed\n";
        return failed_ == 0 ? 0 : 1;
    }
private:
    int passed_ = 0;
    int failed_ = 0;
};
} // namespace minisql::test
