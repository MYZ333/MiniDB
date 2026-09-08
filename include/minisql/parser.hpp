#pragma once

// A / Parser 入口：Token Stream → 多条语句的 AST。
#include "minisql/ast.hpp"
#include "minisql/token.hpp"

namespace minisql {

// 输入必须以唯一 EOF 结束；AST 拥有名称和字面量，不引用 Token 中的内存。
// 仅 EOF 的输入返回空列表；缺分号等错误返回首个 Diagnostic，不返回部分 AST。
// 当前骨架尚未解析任何输入，统一返回 NotImplemented。
Result<std::vector<Statement>> parse(const TokenStream& tokens);

} // namespace minisql
