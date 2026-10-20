/**
 * @file demo_body_lambda.cpp
 * @brief v3 lambda demo：Fiber 入口是一个**无捕获 lambda**。
 *
 * 关键点：
 * - lambda 对象 `g_lambda_body` 本身被 `MIGRATION_SYMBOL_FN` 注册：内部同时
 *   注册转换函数 `_FUN` 和真正的 `operator()`（栈帧上出现的是后者）；
 * - 真正调用走 `volatile` 函数指针 `g_lambda_entry`，保证代码是独立出线
 *   函数，不会被内联进 wrapper；
 * - 挂起点用 0x00030001 / 0x00030002，和 safepoint demo 区分。
 *
 * 本目标额外关闭了 IPA 克隆（`-fno-ipa-cp/-sra/-icf`）：`.constprop/.isra`
 * 克隆会让栈上的实际函数与注册地址不一致（见 docs/fiber-migration.md §8）。
 *
 * 若把 `MIGRATION_SYMBOL_FN(g_lambda_body)` 注释掉，dump 会在帧链上遇到
 * 未注册的 lambda 符号并拒绝 —— 这正是「需要注册」诊断要展示的行为。
 */
#include "migration/runtime.hpp"
#include "migration/safepoints.hpp"
#include "migration/symbol_table.hpp"

#include <cstddef>
#include <cstdint>

extern "C" const char migration_demo_version[] = "lambda-v1";

namespace {

constexpr std::uint32_t kSafepointLambdaRecv1 = 0x00030001;
constexpr std::uint32_t kSafepointLambdaRecv2 = 0x00030002;

using BodyFn = void (*)(migration::FiberCtl*);

/// lambda 对象本身：`MIGRATION_SYMBOL_FN` 需要闭包类型才能取 operator()。
auto g_lambda_body = [](migration::FiberCtl* self) {
    auto* const channel = reinterpret_cast<migration::ChannelCtl*>(self->channel);

    volatile unsigned char payload[8 * 1024];
    for (std::size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = static_cast<unsigned char>((i * 13U + 5U) & 0xFFU);
    }
    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < sizeof(payload); ++i) checksum += payload[i];
    self->arg = checksum;

    migration::Log("fiber#%llu [lambda] 启动，准备接收（payload=%zu 字节）\n",
                   static_cast<unsigned long long>(self->id), sizeof(payload));

    std::int64_t first = 0;
    MIGRATION_RECV(kSafepointLambdaRecv1, first, channel, self);
    migration::Log("fiber#%llu [lambda] 继续执行，收到消息 #1 = %lld\n",
                   static_cast<unsigned long long>(self->id),
                   static_cast<long long>(first));
    self->result = first;

    std::uint64_t restored = 0;
    for (std::size_t i = 0; i < sizeof(payload); ++i) restored += payload[i];
    if (restored != checksum) {
        migration::Log("fiber#%llu [lambda] payload 校验失败：%llu != %llu\n",
                       static_cast<unsigned long long>(self->id),
                       static_cast<unsigned long long>(restored),
                       static_cast<unsigned long long>(checksum));
        self->state = static_cast<std::uint32_t>(migration::FiberState::kFailed);
        return;
    }
    migration::Log("fiber#%llu [lambda] 栈上 payload 校验通过（%llu）\n",
                   static_cast<unsigned long long>(self->id),
                   static_cast<unsigned long long>(restored));

    std::int64_t second = 0;
    MIGRATION_RECV(kSafepointLambdaRecv2, second, channel, self);
    migration::Log("fiber#%llu [lambda] 继续执行，收到消息 #2 = %lld，主体结束\n",
                   static_cast<unsigned long long>(self->id),
                   static_cast<long long>(second));
    self->result = second;
};

/// volatile 函数指针：调用点无法被去虚拟化，lambda 代码必然独立成函数。
BodyFn volatile g_lambda_entry = +g_lambda_body;

}  // namespace

extern "C" __attribute__((noinline)) void migration_demo_body(
    migration::FiberCtl* self) {
    g_lambda_entry(self);
}

MIGRATION_SYMBOL(migration_demo_body);
MIGRATION_SYMBOL_FN(g_lambda_body);
