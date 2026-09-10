#pragma once

// 测试专用参考求值器：不调用优化器的常量计算代码，不属于数据库执行引擎。
// 算术等价性测试只使用小整数；INT64 极值用 optimizer_tests 中的明确预期向量检查。
#include "minisql/plan.hpp"

#include <stdexcept>
#include <utility>

namespace minisql::test_reference {
using Row = std::vector<ScalarValue>;
using Rows = std::vector<Row>;

struct RuntimeFailure : std::runtime_error {
    ErrorCode code;
    SourceLocation span;
    RuntimeFailure(ErrorCode error, SourceLocation source)
        : std::runtime_error("reference runtime failure"), code(error), span(source) {}
};

inline void smallOperands(std::int64_t a, std::int64_t b = 0) {
    // 每个操作数限制在 ±2^30 内，和、差、积均能用 int64_t 精确表示。
    constexpr std::int64_t limit = std::int64_t{1} << 30;
    if (a < -limit || a > limit || b < -limit || b > limit)
        throw std::runtime_error("reference arithmetic only supports the small-value test domain");
}

inline ScalarValue evaluate(const BoundExprPtr& expression, const Row& row) {
    if (const auto* ref = std::get_if<BoundColumnRef>(&expression->node)) return row.at(ref->ordinal);
    if (const auto* literal = std::get_if<BoundLiteral>(&expression->node)) return literal->value;
    if (const auto* unary = std::get_if<BoundUnary>(&expression->node)) {
        const auto value = evaluate(unary->operand, row);
        if (unary->op == UnaryOp::Not) return !std::get<bool>(value);
        const auto integer = std::get<std::int64_t>(value);
        smallOperands(integer);
        return -integer;
    }
    const auto& binary = std::get<BoundBinary>(expression->node);
    const auto left = evaluate(binary.left, row);
    // 先左后右，只有确实需要右侧时才求值，独立检验优化器的短路规则。
    if (binary.op == BinaryOp::And && !std::get<bool>(left)) return false;
    if (binary.op == BinaryOp::Or && std::get<bool>(left)) return true;
    const auto right = evaluate(binary.right, row);
    if (binary.op == BinaryOp::And || binary.op == BinaryOp::Or) return std::get<bool>(right);
    if (binary.op == BinaryOp::Equal) return left == right;
    if (binary.op == BinaryOp::NotEqual) return left != right;
    const auto a = std::get<std::int64_t>(left);
    const auto b = std::get<std::int64_t>(right);
    switch (binary.op) {
    case BinaryOp::Less: return a < b;
    case BinaryOp::LessEqual: return a <= b;
    case BinaryOp::Greater: return a > b;
    case BinaryOp::GreaterEqual: return a >= b;
    default: break;
    }
    if (binary.op == BinaryOp::Divide && b == 0)
        throw RuntimeFailure(ErrorCode::DivisionByZero, binary.operator_span);
    smallOperands(a, b);
    switch (binary.op) {
    case BinaryOp::Add: return a + b;
    case BinaryOp::Subtract: return a - b;
    case BinaryOp::Multiply: return a * b;
    case BinaryOp::Divide: return a / b;
    default: throw std::runtime_error("unsupported reference expression");
    }
}

struct IndexedRow { std::size_t row_id; Row values; };

inline std::vector<IndexedRow> read(const PlanPtr& plan, const Rows& table) {
    if (std::holds_alternative<SeqScanPlan>(plan->node)) {
        std::vector<IndexedRow> result;
        for (std::size_t i = 0; i < table.size(); ++i) result.push_back({i, table[i]});
        return result;
    }
    if (const auto* filter = std::get_if<FilterPlan>(&plan->node)) {
        auto rows = read(filter->input, table);
        std::vector<IndexedRow> result;
        for (const auto& row : rows) {
            if (std::get<bool>(evaluate(filter->predicate, row.values))) result.push_back(row);
        }
        return result;
    }
    throw std::runtime_error("reference source expects Scan or Filter");
}

struct Outcome {
    Rows rows; // SELECT 是查询输出；UPDATE/DELETE 是修改后的整表。
    std::size_t affected = 0;
    std::optional<ErrorCode> error;
    SourceLocation error_span;
};

inline Outcome run(const LogicalPlan& plan, const Rows& original) {
    Outcome result;
    try {
        if (const auto* project = std::get_if<ProjectPlan>(&plan.root->node)) {
            for (const auto& row : read(project->input, original)) {
                Row output;
                for (const auto& column : project->columns) output.push_back(row.values.at(column.ordinal));
                result.rows.push_back(std::move(output));
            }
        } else if (const auto* update = std::get_if<UpdatePlan>(&plan.root->node)) {
            result.rows = original;
            for (const auto& row : read(update->input, original)) {
                Row updated = row.values;
                // 所有 RHS 都使用旧行，完成之后才写入测试容器。
                for (const auto& assignment : update->assignments)
                    updated.at(assignment.target.ordinal) = evaluate(assignment.value, row.values);
                result.rows[row.row_id] = std::move(updated);
                ++result.affected;
            }
        } else if (const auto* deletion = std::get_if<DeletePlan>(&plan.root->node)) {
            result.rows = original;
            std::vector<bool> removed(original.size(), false);
            for (const auto& row : read(deletion->input, original)) removed[row.row_id] = true;
            result.rows.clear();
            for (std::size_t i = 0; i < original.size(); ++i) {
                if (removed[i]) ++result.affected;
                else result.rows.push_back(original[i]);
            }
        } else throw std::runtime_error("reference runner only accepts SELECT/UPDATE/DELETE");
    } catch (const RuntimeFailure& error) {
        result.error = error.code;
        result.error_span = error.span;
    }
    return result;
}

inline bool sameSpan(const SourceLocation& a, const SourceLocation& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return a->begin.offset == b->begin.offset && a->end.offset == b->end.offset &&
           a->begin.line == b->begin.line && a->begin.column == b->begin.column &&
           a->end.line == b->end.line && a->end.column == b->end.column;
}

inline bool equivalent(const Outcome& a, const Outcome& b) {
    return a.rows == b.rows && a.affected == b.affected && a.error == b.error && sameSpan(a.error_span, b.error_span);
}

} // namespace minisql::test_reference
