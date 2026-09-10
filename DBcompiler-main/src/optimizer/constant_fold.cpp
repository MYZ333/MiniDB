// 用可移植 C++17 的边界判断实现安全常量计算，先判断再运算，避免有符号整数 UB。
#include "constant_fold.hpp"

#include <cmath>
#include <limits>

namespace minisql::optimizer_detail {
namespace {
constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
constexpr auto maximum = std::numeric_limits<std::int64_t>::max();

std::optional<ScalarValue> integers(BinaryOp op, std::int64_t a, std::int64_t b) {
    switch (op) {
    case BinaryOp::Add:
        if ((b > 0 && a > maximum - b) || (b < 0 && a < minimum - b)) return std::nullopt;
        return ScalarValue{a + b};
    case BinaryOp::Subtract:
        if ((b > 0 && a < minimum + b) || (b < 0 && a > maximum + b)) return std::nullopt;
        return ScalarValue{a - b};
    case BinaryOp::Multiply:
        // 按符号分别比较商，不能先计算 a*b 再看结果是否越界。
        if (a > 0) {
            if ((b > 0 && a > maximum / b) || (b < 0 && b < minimum / a)) return std::nullopt;
        } else if (a < 0) {
            if ((b > 0 && a < minimum / b) || (b < 0 && a < maximum / b)) return std::nullopt;
        }
        return ScalarValue{a * b};
    case BinaryOp::Divide:
        if (b == 0 || (a == minimum && b == -1)) return std::nullopt;
        return ScalarValue{a / b}; // C++17 的整数除法向零截断，与契约一致。
    case BinaryOp::Equal: return ScalarValue{a == b};
    case BinaryOp::NotEqual: return ScalarValue{a != b};
    case BinaryOp::Less: return ScalarValue{a < b};
    case BinaryOp::LessEqual: return ScalarValue{a <= b};
    case BinaryOp::Greater: return ScalarValue{a > b};
    case BinaryOp::GreaterEqual: return ScalarValue{a >= b};
    default: return std::nullopt;
    }
}
} // namespace

std::optional<ScalarValue> foldUnary(UnaryOp op, const ScalarValue& operand) {
    if (op == UnaryOp::Negate) {
        if (const auto* value = std::get_if<std::int64_t>(&operand)) {
            if (*value != minimum) return ScalarValue{-*value};
        }
        if (const auto* value = std::get_if<double>(&operand)) {
            const double result = -*value;
            if (std::isfinite(result)) return ScalarValue{result};
        }
    } else if (op == UnaryOp::Not) {
        if (const auto* value = std::get_if<bool>(&operand)) return ScalarValue{!*value};
    }
    return std::nullopt;
}

std::optional<ScalarValue> foldBinary(BinaryOp op, const ScalarValue& left, const ScalarValue& right) {
    if (const auto* a = std::get_if<std::int64_t>(&left)) {
        if (const auto* b = std::get_if<std::int64_t>(&right)) return integers(op, *a, *b);
    } else if (const auto* a = std::get_if<double>(&left)) {
        if (const auto* b = std::get_if<double>(&right)) {
            double result = 0.0;
            switch (op) {
            case BinaryOp::Add: result = *a + *b; break;
            case BinaryOp::Subtract: result = *a - *b; break;
            case BinaryOp::Multiply: result = *a * *b; break;
            case BinaryOp::Divide:
                if (*b == 0.0) return std::nullopt;
                result = *a / *b;
                break;
            case BinaryOp::Equal: return ScalarValue{*a == *b};
            case BinaryOp::NotEqual: return ScalarValue{*a != *b};
            case BinaryOp::Less: return ScalarValue{*a < *b};
            case BinaryOp::LessEqual: return ScalarValue{*a <= *b};
            case BinaryOp::Greater: return ScalarValue{*a > *b};
            case BinaryOp::GreaterEqual: return ScalarValue{*a >= *b};
            default: return std::nullopt;
            }
            if (std::isfinite(result)) return ScalarValue{result};
        }
    } else if (const auto* a = std::get_if<std::string>(&left)) {
        if (const auto* b = std::get_if<std::string>(&right)) {
            if (op == BinaryOp::Equal) return ScalarValue{*a == *b};
            if (op == BinaryOp::NotEqual) return ScalarValue{*a != *b};
        }
    } else if (const auto* a = std::get_if<bool>(&left)) {
        if (const auto* b = std::get_if<bool>(&right)) {
            if (op == BinaryOp::Equal) return ScalarValue{*a == *b};
            if (op == BinaryOp::NotEqual) return ScalarValue{*a != *b};
            if (op == BinaryOp::And) return ScalarValue{*a && *b};
            if (op == BinaryOp::Or) return ScalarValue{*a || *b};
        }
    }
    return std::nullopt;
}

} // namespace minisql::optimizer_detail
