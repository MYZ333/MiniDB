#pragma once

// A 的词法输出：Token 保存原始词素和位置，不解析表列含义。
#include "minisql/common.hpp"

namespace minisql {

// 第一阶段文法的全部终结符。关键字使用专门种类，Parser 无需比较字符串。
enum class TokenKind {
    EndOfInput,
    Identifier, Integer, FloatLiteral, String,
    Create, Table, Insert, Into, Values, Select, From, Where,
    Update, Set, Delete, Join, On, Group, Order, By, Asc, Desc,
    Int, Varchar, Bool, Float, Null, True, False, And, Or, Not,
    Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
    Plus, Minus, Star, Slash,
    LeftParen, RightParen, Comma, Dot, Semicolon
};

struct Token {
    TokenKind kind;
    std::string lexeme; // 拥有原文副本；字符串包含引号和原始转义，不提前解码。
    SourceSpan span; // 真实 Token 必须有位置，包含 EOF 的零长度范围。
};

// 成功的词法输出总以唯一 EndOfInput 结尾；空 SQL 也产生一个 EOF。
using TokenStream = std::vector<Token>;

} // namespace minisql
