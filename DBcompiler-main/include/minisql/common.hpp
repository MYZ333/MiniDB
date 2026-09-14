#pragma once

// 公共基础类型：各层使用同一套类型、位置和错误规则，避免接口含义漂移。
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace minisql {

// Null 仅表示 NULL 字面量的内部类型，不能声明为表列类型。
enum class DataType { Int, Varchar, Bool, Float, Null };

struct SourcePosition {
    std::size_t offset = 0; // 从 0 开始的 UTF-8 字节偏移。
    std::size_t line = 1;
    std::size_t column = 1; // 从 1 开始的字节列号。
};

struct SourceSpan {
    SourcePosition begin;
    SourcePosition end; // 左闭右开；不包含 end 指向的字节。
};

// nullopt 表示手工构造节点时未知位置；A 的实际解析结果应有位置。
using SourceLocation = std::optional<SourceSpan>;

struct Identifier {
    std::string text; // 原始拼写，用于诊断；不要由 Parser 提前转小写。
    SourceLocation span;
};

// 仅按 ASCII 转换，不依赖进程 locale，也不修改字符串常量。
inline std::string normalizeName(std::string name) {
    for (char& ch : name) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return name;
}

// NULL 用于存储、聚合和空值判定；普通运算的三值逻辑留给后续执行契约。
struct NullValue {};
inline bool operator==(NullValue, NullValue) noexcept { return true; }
inline bool operator!=(NullValue, NullValue) noexcept { return false; }
using LiteralValue = std::variant<std::int64_t, double, std::string, bool, NullValue>;
using ScalarValue = std::variant<std::int64_t, double, std::string, bool, NullValue>;

// IsNull/IsNotNull 接受任意类型，返回非空 BOOL，贯通 A/B 与执行层。
enum class UnaryOp { Negate, Not, IsNull, IsNotNull };
enum class SortDirection { Asc, Desc };
enum class AggregateKind { Count, Sum, Avg, Min, Max };
enum class BinaryOp {
    Add, Subtract, Multiply, Divide,
    Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
    And, Or,
    Like // A 解析 SQL LIKE；B 后续定义字符串匹配语义。
};

enum class DiagnosticStage { Lexical, Syntax, Semantic, Plan, Execution };

// 枚举名称是稳定错误码；message 可改进，但调用方不应通过匹配消息判断错误。
enum class ErrorCode {
    InvalidCharacter, UnterminatedString, UnterminatedComment,
    UnexpectedToken, IntegerOutOfRange,
    TableAlreadyExists, TableNotFound, DuplicateColumn, ColumnNotFound,
    UnsupportedType, EmptyColumnList, ValueCountMismatch, MissingInsertColumn,
    TypeMismatch, InvalidOperandType, WhereNotBoolean, DuplicateAssignment,
    InvalidBoundStatement, CatalogVersionMismatch, DivisionByZero, IntegerOverflow,
    NotImplemented, // 骨架入口专用：模块未实现，不表示用户的 SQL 有错。
    InvalidAst, ExpressionTooDeep, // 防御手工/外部 AST 的空子节点和过深嵌套。
    InvalidPlan, // 优化入口发现缺失子节点或不满足基本结构约定的计划。
    UnsupportedFeature, // 已识别但当前阶段尚未定义行为的语法。
    AmbiguousColumn, DuplicateTable, InvalidGrouping, JoinConditionNotBoolean
};

struct Diagnostic {
    DiagnosticStage stage;
    ErrorCode code;
    std::string message;
    SourceLocation span;
};

// 成功值和错误互斥，不能同时返回半成品与错误。
template <typename T>
using Result = std::variant<T, Diagnostic>;

// 尚未分配 ID 的建表字段定义，供绑定结果和 CreateTable 计划共用。
struct ColumnSpec {
    std::string name; // 已归一化。
    DataType type;
    std::optional<std::int64_t> varchar_length = {}; // 仅 VARCHAR(n) 使用；nullopt 表示未声明长度。
};

} // namespace minisql
