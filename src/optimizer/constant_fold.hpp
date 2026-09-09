#pragma once

// 优化器私有工具：只计算常量，不依赖表、AST 或执行器。
#include "minisql/common.hpp"

namespace minisql::optimizer_detail {

// nullopt 表示类型不适用或计算会除零/溢出，调用方应保留原来的运算节点。
std::optional<ScalarValue> foldUnary(UnaryOp op, const ScalarValue& operand);
std::optional<ScalarValue> foldBinary(BinaryOp op, const ScalarValue& left, const ScalarValue& right);

} // namespace minisql::optimizer_detail
