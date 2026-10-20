/**
 * @file symbol_conflict.cpp
 * @brief 反例：把两个不同函数注册成同一个符号名。
 *
 * 只在 `migrate_conflict` 目标里链接。dump 侧的符号唯一性检查会拒绝这种
 * 配置，并打印两个地址 —— 因为「同名符号 → 一个地址」是按名字恢复的前提。
 */
#include "migration/symbol_table.hpp"

#include <cstdint>

namespace {

__attribute__((noinline)) void ConflictFnA() {}
__attribute__((noinline)) void ConflictFnB() {}

// 同一个名字、两个地址 → SymbolTable 记录 conflict。
[[maybe_unused]] const ::migration::SymbolRegistrar kConflictA(
    "MigrationConflictDemo", reinterpret_cast<const void*>(&ConflictFnA), 0);
[[maybe_unused]] const ::migration::SymbolRegistrar kConflictB(
    "MigrationConflictDemo", reinterpret_cast<const void*>(&ConflictFnB), 0);

}  // namespace
