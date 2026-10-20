/**
 * @file demo_body_v1.cpp
 * @brief demo 主体（SO v1）：用于验证「同一符号名、不同 SO 加载地址」的恢复。
 *
 * 编译成 `libmigration_demo_v1.so`，由 `migrate` 在运行期 dlopen。SO 内用
 * `MIGRATION_SYMBOL` 把导出函数注册进主程序的符号表；dump 时快照只记录
 * `migration_demo_body + 偏移`，恢复进程加载 v2 后按名字解析到新地址。
 *
 * 注意：v1 与 v2 的 `migration_demo_body` **指令布局必须一致**，否则栈上
 * 返回地址的偏移在新版本里语义改变（见 docs 的限制说明）。
 */
#include "migration/runtime.hpp"
#include "migration/symbol_table.hpp"

#include <cstddef>
#include <cstdint>

extern "C" void migration_demo_body(migration::FiberCtl* self) {
    auto* const channel = reinterpret_cast<migration::ChannelCtl*>(self->channel);

    // 栈上 64 KiB 可校验数据：证明栈内容随快照完整迁移。
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
