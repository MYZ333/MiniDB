// 将规则集中维护，避免 Parser、Analyzer 各自猜测隐式类型转换。
#include "type_rules.hpp"

namespace minisql::semantic_detail {

std::optional<DataType> unaryResult(UnaryOp op, DataType operand) {
    // 空值判定接受任意已绑定类型，包括 NULL 字面量，始终返回 BOOL。
    if (op == UnaryOp::IsNull || op == UnaryOp::IsNotNull) return DataType::Bool;
    if (op == UnaryOp::Negate && (operand == DataType::Int || operand == DataType::Float)) return operand;
    if (op == UnaryOp::Not && operand == DataType::Bool) return DataType::Bool;
    return std::nullopt;
}

std::optional<DataType> binaryResult(BinaryOp op, DataType left, DataType right) {
    if (left != right) return std::nullopt; // 第一阶段不做任何隐式转换。
    switch (op) {
    case BinaryOp::Add: case BinaryOp::Subtract:
    case BinaryOp::Multiply: case BinaryOp::Divide:
        if (left == DataType::Int || left == DataType::Float) return left;
        break;
    case BinaryOp::Equal: case BinaryOp::NotEqual:
        if (left == DataType::Int || left == DataType::Float ||
            left == DataType::Varchar || left == DataType::Bool) return DataType::Bool;
        break;
    case BinaryOp::Less: case BinaryOp::LessEqual:
    case BinaryOp::Greater: case BinaryOp::GreaterEqual:
        if (left == DataType::Int || left == DataType::Float) return DataType::Bool;
        break;
    case BinaryOp::And: case BinaryOp::Or:
        if (left == DataType::Bool) return DataType::Bool;
        break;
    case BinaryOp::Like: break; // 语义入口显式拒绝尚未实现的匹配运算。
    }
    return std::nullopt;
}

const char* typeName(DataType type) {
    switch (type) {
    case DataType::Int: return "INT";
    case DataType::Varchar: return "VARCHAR";
    case DataType::Bool: return "BOOL";
    case DataType::Float: return "FLOAT";
    case DataType::Null: return "NULL";
    }
    return "UNKNOWN";
}

const char* operatorName(UnaryOp op) {
    switch (op) {
    case UnaryOp::Not: return "NOT";
    case UnaryOp::Negate: return "-";
    case UnaryOp::IsNull: return "IS NULL";
    case UnaryOp::IsNotNull: return "IS NOT NULL";
    }
    return "UNKNOWN";
}

const char* operatorName(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Subtract: return "-";
    case BinaryOp::Multiply: return "*";
    case BinaryOp::Divide: return "/";
    case BinaryOp::Equal: return "=";
    case BinaryOp::NotEqual: return "!=";
    case BinaryOp::Less: return "<";
    case BinaryOp::LessEqual: return "<=";
    case BinaryOp::Greater: return ">";
    case BinaryOp::GreaterEqual: return ">=";
    case BinaryOp::And: return "AND";
    case BinaryOp::Or: return "OR";
    case BinaryOp::Like: return "LIKE";
    }
    return "UNKNOWN";
}
} // namespace minisql::semantic_detail
