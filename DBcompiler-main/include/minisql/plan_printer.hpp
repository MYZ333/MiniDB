#pragma once

// 将逻辑计划打印为稳定的文本树，供课程演示、调试和快照测试使用。
#include "minisql/plan.hpp"

namespace minisql {

// 输入应来自成功的 buildPlan；不执行计划、不读取 Catalog，不输出指针地址。
// 此输出是可读调试格式，不是可反序列化协议或 SQL 源码。
std::string formatPlan(const LogicalPlan& plan);

} // namespace minisql
