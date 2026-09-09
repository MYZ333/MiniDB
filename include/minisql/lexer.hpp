#pragma once

// A / Lexer 入口：字符流 → 有位置的 Token Stream。
#include "minisql/token.hpp"

#include <string_view>

namespace minisql {

// 仅在调用期间借用 SQL；成功返回的 Token 拥有词素，不依赖输入生命周期。
// 词法错误返回首个 Diagnostic；支持注释、转义、大小写与字节行列位置。
Result<TokenStream> lex(std::string_view sql);

} // namespace minisql
