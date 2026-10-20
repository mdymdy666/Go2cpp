/**
 * @file scenario_entry.cpp
 * @brief 3 producer → 1 consumer（96 次无缓冲交接）的**重放入口**。
 *
 * 通过 `RECORD_FIBER_ENTRY_NAMED` 编译期注册，`ReplayFileAuto()` / CLI
 * `replay play` 无需显式 factory 即可重跑。录制端不使用注册表（它跑真实
 * 调度器），但把本文件链接进录制程序也无副作用。
 */

#include "demo/scenario.hpp"

#include "replay/auto_entry.hpp"
#include "replay/sync_channel.hpp"

namespace {

void FaninEntry(std::uint32_t gid, go2cpp::replay::ReplaySource& source) {
    using namespace go2cpp::replay;
    ScopedGid guard(gid);
    SyncChannel<std::uint64_t> channel(demo::kChannelId, source);
    if (gid < demo::kProducers) {
        (void)demo::Produce(gid, channel);
    } else {
        (void)demo::Consume(channel, &demo::SharedResult());
    }
}

}  // namespace

RECORD_FIBER_ENTRY_NAMED("fanin-3x32", FaninEntry);
