// A 的 AST 级常量折叠：不查询 Catalog、不做名称/类型检查，也不修改输入树。
#include "minisql/ast_optimizer.hpp"

#include <cmath>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace minisql {
namespace {
ExprPtr makeExpr(std::variant<IdentifierExpr, LiteralExpr, UnaryExpr, BinaryExpr, AggregateCall> node,
                 SourceLocation span) {
    return std::make_shared<const Expr>(Expr{std::move(node), std::move(span)});
}

ExprPtr literal(LiteralValue value, SourceLocation span) {
    return makeExpr(LiteralExpr{std::move(value)}, std::move(span));
}

const LiteralValue* literalValue(const ExprPtr& expression) {
    if (!expression) return nullptr;
    const auto* node = std::get_if<LiteralExpr>(&expression->node);
    return node ? &node->value : nullptr;
}

std::optional<bool> boolean(const ExprPtr& expression) {
    const auto* value = literalValue(expression);
    if (!value) return std::nullopt;
    if (const auto* result = std::get_if<bool>(value)) return *result;
    return std::nullopt;
}

std::optional<std::int64_t> integerBinary(BinaryOp op, std::int64_t a, std::int64_t b) {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    switch (op) {
    case BinaryOp::Add:
        if ((b > 0 && a > maximum - b) || (b < 0 && a < minimum - b)) return std::nullopt;
        return a + b;
    case BinaryOp::Subtract:
        if ((b > 0 && a < minimum + b) || (b < 0 && a > maximum + b)) return std::nullopt;
        return a - b;
    case BinaryOp::Multiply:
        if (a > 0 && ((b > 0 && a > maximum / b) || (b < 0 && b < minimum / a))) return std::nullopt;
        if (a < 0 && ((b > 0 && a < minimum / b) || (b < 0 && a < maximum / b))) return std::nullopt;
        return a * b;
    case BinaryOp::Divide:
        if (b == 0 || (a == minimum && b == -1)) return std::nullopt;
        return a / b;
    default: return std::nullopt;
    }
}

std::optional<LiteralValue> foldUnary(UnaryOp op, const LiteralValue& value) {
    if (op == UnaryOp::Not) {
        if (const auto* item = std::get_if<bool>(&value)) return LiteralValue{!*item};
    } else if (op == UnaryOp::IsNull) {
        return LiteralValue{std::holds_alternative<NullValue>(value)};
    } else if (op == UnaryOp::IsNotNull) {
        return LiteralValue{!std::holds_alternative<NullValue>(value)};
    } else if (const auto* item = std::get_if<std::int64_t>(&value)) {
        if (*item != std::numeric_limits<std::int64_t>::min()) return LiteralValue{-*item};
    } else if (const auto* item = std::get_if<double>(&value)) {
        if (std::isfinite(-*item)) return LiteralValue{-*item};
    }
    return std::nullopt;
}

template <typename T>
std::optional<LiteralValue> compare(BinaryOp op, const T& a, const T& b, bool ordered) {
    switch (op) {
    case BinaryOp::Equal: return LiteralValue{a == b};
    case BinaryOp::NotEqual: return LiteralValue{a != b};
    case BinaryOp::Less: if (ordered) return LiteralValue{a < b}; break;
    case BinaryOp::LessEqual: if (ordered) return LiteralValue{a <= b}; break;
    case BinaryOp::Greater: if (ordered) return LiteralValue{a > b}; break;
    case BinaryOp::GreaterEqual: if (ordered) return LiteralValue{a >= b}; break;
    default: break;
    }
    return std::nullopt;
}

std::optional<LiteralValue> foldBinary(BinaryOp op, const LiteralValue& left,
                                       const LiteralValue& right) {
    if (const auto* a = std::get_if<std::int64_t>(&left)) {
        if (const auto* b = std::get_if<std::int64_t>(&right)) {
            if (auto value = integerBinary(op, *a, *b)) return LiteralValue{*value};
            return compare(op, *a, *b, true);
        }
    } else if (const auto* a = std::get_if<double>(&left)) {
        if (const auto* b = std::get_if<double>(&right)) {
            if (op == BinaryOp::Add || op == BinaryOp::Subtract ||
                op == BinaryOp::Multiply || op == BinaryOp::Divide) {
                if (op == BinaryOp::Divide && *b == 0.0) return std::nullopt;
                const double value = op == BinaryOp::Add ? *a + *b :
                    op == BinaryOp::Subtract ? *a - *b :
                    op == BinaryOp::Multiply ? *a * *b : *a / *b;
                if (std::isfinite(value)) return LiteralValue{value};
                return std::nullopt;
            }
            return compare(op, *a, *b, true);
        }
    } else if (const auto* a = std::get_if<std::string>(&left)) {
        if (const auto* b = std::get_if<std::string>(&right)) return compare(op, *a, *b, false);
    } else if (const auto* a = std::get_if<bool>(&left)) {
        if (const auto* b = std::get_if<bool>(&right)) {
            if (op == BinaryOp::And) return LiteralValue{*a && *b};
            if (op == BinaryOp::Or) return LiteralValue{*a || *b};
            return compare(op, *a, *b, false);
        }
    }
    // NULL 和混合数值类型需要 B 的类型/空值语义，A 不擅自折叠。
    return std::nullopt;
}

ExprPtr optimizeBinary(const ExprPtr& original, const BinaryExpr& node) {
    ExprPtr left = optimizeAstExpression(node.left);
    const auto left_bool = boolean(left);
    // 只有左侧已经决定短路结果时，右侧才确实不可达。
    if ((node.op == BinaryOp::And && left_bool == false) ||
        (node.op == BinaryOp::Or && left_bool == true)) return literal(*left_bool, original->span);
    ExprPtr right = optimizeAstExpression(node.right);
    if (const auto* a = literalValue(left)) {
        if (const auto* b = literalValue(right)) {
            if (auto result = foldBinary(node.op, *a, *b)) return literal(*result, original->span);
        }
    }
    const auto right_bool = boolean(right);
    if ((node.op == BinaryOp::And && left_bool == true) ||
        (node.op == BinaryOp::Or && left_bool == false)) return right;
    if ((node.op == BinaryOp::And && right_bool == true) ||
        (node.op == BinaryOp::Or && right_bool == false)) return left;
    return makeExpr(BinaryExpr{node.op, std::move(left), std::move(right), node.operator_span},
                    original->span);
}

ExprPtr optimizeWhere(const ExprPtr& where) {
    ExprPtr result = optimizeAstExpression(where);
    return boolean(result) == true ? nullptr : result;
}

SelectList optimizeSelectList(const SelectList& columns) {
    if (!std::holds_alternative<std::vector<SelectItem>>(columns)) return columns;
    auto items = std::get<std::vector<SelectItem>>(columns);
    for (auto& item : items) {
        if (auto* expr = std::get_if<ExprPtr>(&item)) {
            *expr = optimizeAstExpression(*expr);
        }
    }
    return SelectList{std::move(items)};
}
} // namespace

ExprPtr optimizeAstExpression(const ExprPtr& expression) {
    if (!expression) return nullptr;
    return std::visit([&](const auto& node) -> ExprPtr {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, IdentifierExpr> || std::is_same_v<T, LiteralExpr> ||
                      std::is_same_v<T, AggregateCall>) {
            return expression;
        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
            ExprPtr operand = optimizeAstExpression(node.operand);
            if (const auto* value = literalValue(operand)) {
                if (auto folded = foldUnary(node.op, *value)) return literal(*folded, expression->span);
            }
            if (operand == node.operand) return expression;
            return makeExpr(UnaryExpr{node.op, std::move(operand), node.operator_span}, expression->span);
        } else return optimizeBinary(expression, node);
    }, expression->node);
}

Statement optimizeAstStatement(const Statement& statement) {
    Statement result = statement;
    std::visit([&](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, SelectStmt>) {
            auto copy = node;
            copy.columns = optimizeSelectList(node.columns);
            copy.where = optimizeWhere(node.where);
            copy.having = optimizeWhere(node.having);
            for (auto& join : copy.joins) join.on = optimizeAstExpression(join.on);
            for (auto& item : copy.order_by) {
                if (item.expression) item.expression = optimizeAstExpression(item.expression);
            }
            result.node = std::move(copy);
        } else if constexpr (std::is_same_v<T, UpdateStmt>) {
            auto copy = node;
            copy.where = optimizeWhere(node.where);
            for (auto& assignment : copy.assignments)
                assignment.value = optimizeAstExpression(assignment.value);
            result.node = std::move(copy);
        } else if constexpr (std::is_same_v<T, DeleteStmt>) {
            auto copy = node;
            copy.where = optimizeWhere(node.where);
            result.node = std::move(copy);
        } else if constexpr (std::is_same_v<T, DropTableStmt>) {
            result.node = node;
        } else if constexpr (std::is_same_v<T, ExplainStmt>) {
            auto copy = node;
            // 对目标语句使用同一 AST 优化入口，包装层只决定是否执行。
            std::visit([&](const auto& target) {
                const auto optimized = optimizeAstStatement(Statement{target, statement.span});
                using Target = std::decay_t<decltype(target)>;
                copy.target = std::get<Target>(optimized.node);
            }, node.target);
            result.node = std::move(copy);
        }
    }, statement.node);
    return result;
}

std::vector<Statement> optimizeAstStatements(const std::vector<Statement>& statements) {
    std::vector<Statement> result;
    result.reserve(statements.size());
    for (const auto& statement : statements) result.push_back(optimizeAstStatement(statement));
    return result;
}

} // namespace minisql
