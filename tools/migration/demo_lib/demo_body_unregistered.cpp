/**
 * @file demo_body_unregistered.cpp
 * @brief v3 反例：挂起调用位于未注册的匿名静态函数里。
 *
 * `migration_demo_body` 是导出的（`dladdr` 能解析），但真正的挂起发生在
 * 匿名命名空间的 `DeepPark` 内。`DeepPark` 的返回地址会出现在
 * `ChannelRecv` 帧上，却既不在符号注册表里、也不在动态符号表里 —— dump
 * 必须拒绝，并给出「请用 MIGRATION_SYMBOL / MIGRATION_SYMBOL_FN 注册」的
 * 诊断（见 `SymbolTable::DiagnoseUnresolved`）。
 */
#include "migration/runtime.hpp"

#include <cstdint>

namespace {

__attribute__((noinline)) void DeepPark(migration::FiberCtl* self) {
    auto* const channel = reinterpret_cast<migration::ChannelCtl*>(self->channel);
    const std::int64_t value = migration::ChannelRecv(channel, self);
    self->result = value;
}

}  // namespace

extern "C" __attribute__((noinline)) void migration_demo_body(
    migration::FiberCtl* self) {
    DeepPark(self);
}
