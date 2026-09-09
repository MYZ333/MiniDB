#pragma once

// 语义模块私有规则表：只回答输入类型是否合法及结果类型，不求表达式值。
#include "minisql/common.hpp"

namespace minisql::semantic_detail {
std::optional<DataType> unaryResult(UnaryOp op, DataType operand);
std::optional<DataType> binaryResult(BinaryOp op, DataType left, DataType right);
const char* typeName(DataType type);
const char* operatorName(UnaryOp op);
const char* operatorName(BinaryOp op);
} // namespace minisql::semantic_detail
