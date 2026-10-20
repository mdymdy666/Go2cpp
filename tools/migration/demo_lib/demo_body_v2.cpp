/**
 * @file demo_body_v2.cpp
 * @brief demo 主体（SO v2）：符号名相同、代码布局相同、加载地址不同。
 *
 * ## 为什么函数体必须与 v1 一致
 *
 * 符号化恢复只能保证「符号名 → 新基址」这一层；栈上保存的是
 * `migration_demo_body + 偏移`，偏移的语义依赖**函数内指令布局**。若 v2
 * 改了函数体，同一个偏移在 v2 里会落到不同指令上，恢复必然崩溃。因此
 * 「换 SO 版本」验证的是：同一份代码、不同加载地址（ASLR / 不同 SO 文件）
 * 时仍能按符号解析成功。
 *
 * 版本差异通过一个独立的导出数据符号体现，不改变 `migration_demo_body`
 * 的指令布局。
 */
#include "migration/runtime.hpp"
#include "migration/symbol_table.hpp"

#include <cstddef>
#include <cstdint>

/// v2 独有：让 SO 内容与 v1 不同（build/加载地址不同），但不影响 body 布局。
extern "C" const char migration_demo_version[] = "v2";

extern "C" void migration_demo_body(migration::FiberCtl* self) {
    auto* const channel = reinterpret_cast<migration::ChannelCtl*>(self->channel);

    // 与 v1 完全一致的栈上数据布局与代码序列。
    volatile unsigned char payload[64 * 1024];
    for (std::size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = static_cast<unsigned char>((i * 31U + 7U) & 0xFFU);
    }
    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < sizeof(payload); ++i) checksum += payload[i];
    self->arg = checksum;

    migration::Log("fiber#%llu [demo SO] 启动，准备接收（payload=%zu 字节，校验和=%llu）\n",
                   static_cast<unsigned long long>(self->id), sizeof(payload),
                   static_cast<unsigned long long>(checksum));

    const std::int64_t first = migration::ChannelRecv(channel, self);
    migration::Log("fiber#%llu [demo SO] 继续执行，收到消息 #1 = %lld\n",
                   static_cast<unsigned long long>(self->id),
                   static_cast<long long>(first));
    self->result = first;

    std::uint64_t restored = 0;
    for (std::size_t i = 0; i < sizeof(payload); ++i) restored += payload[i];
    if (restored != checksum) {
        migration::Log("fiber#%llu [demo SO] payload 校验失败：%llu != %llu\n",
                       static_cast<unsigned long long>(self->id),
                       static_cast<unsigned long long>(restored),
                       static_cast<unsigned long long>(checksum));
        self->state = static_cast<std::uint32_t>(migration::FiberState::kFailed);
        return;
    }
    migration::Log("fiber#%llu [demo SO] 栈上 payload 校验通过（%llu）\n",
                   static_cast<unsigned long long>(self->id),
                   static_cast<unsigned long long>(restored));

    const std::int64_t second = migration::ChannelRecv(channel, self);
    migration::Log("fiber#%llu [demo SO] 继续执行，收到消息 #2 = %lld，主体结束\n",
                   static_cast<unsigned long long>(self->id),
                   static_cast<long long>(second));
    self->result = second;
}

MIGRATION_SYMBOL(migration_demo_body);
