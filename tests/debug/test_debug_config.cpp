/**
 * @file test_debug_config.cpp
 * @brief 调试粒度配置（DebugConfig）测试。
 *
 * 覆盖：
 *   场景 1 事件码 → 类别映射（每个区间抽一个 + Doctor 不受控 + 未知码）；
 *   场景 2 `ParseCategories` 的白名单语义（未列出的类别关闭）；
 *   场景 3 别名（mutex/channel/pc 等映射到 primitives）；
 *   场景 4 大小写/空白/空 token/非法 token 的容错；
 *   场景 5 `Set/Get/Reset` 往返与 `LoadFromEnv` 幂等；
 *   场景 6 事件宏的类别过滤（scheduler/park 关闭时计数为 0，mutex 仍记录）；
 *   场景 7 `sample_shift` 的额外降采样（1 次/2^n 变为 0 次）；
 *   场景 8 `stacks/chains` 开关与类别位互不串扰。
 *
 * 说明：场景 6/7 依赖 `GO2CPP_DEBUG_DUMP=ON`（宏会展开为记录调用）；
 * OFF 构建只验证配置结构本身，不产生任何事件。
 */

#include "go2cpp/debug/debug_config.hpp"
#include "go2cpp/debug/debug_event_ring.hpp"
#include "test_support.hpp"

namespace {

using go2cpp::debug::DebugCategory;
using go2cpp::debug::DebugCategoryOf;
using go2cpp::debug::DebugConfig;
using go2cpp::debug::SchedEventCode;

bool Has(const DebugConfig& config, DebugCategory category) {
    switch (category) {
        case DebugCategory::kLifecycle: return config.lifecycle;
        case DebugCategory::kSched: return config.sched;
        case DebugCategory::kPark: return config.park;
        case DebugCategory::kPrimitives: return config.primitives;
        case DebugCategory::kTimer: return config.timer;
        case DebugCategory::kIo: return config.io;
        case DebugCategory::kStacks: return config.stacks;
        case DebugCategory::kChains: return config.chains;
        default: return false;
    }
}

void CheckOnly(const DebugConfig& config, DebugCategory only) {
    const DebugCategory all[] = {
        DebugCategory::kLifecycle, DebugCategory::kSched,
        DebugCategory::kPark,      DebugCategory::kPrimitives,
        DebugCategory::kTimer,     DebugCategory::kIo,
        DebugCategory::kStacks,    DebugCategory::kChains};
    for (const DebugCategory category : all) {
        GO2CPP_CHECK(Has(config, category) == (category == only));
    }
}

}  // namespace

void run_debug_config_tests() {
    go2cpp_tests::announce("debug_config");

    // 场景 1：类别映射。
    {
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kParkEnter) ==
                     DebugCategory::kPark);
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kParkCaptured) ==
                     DebugCategory::kPark);
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kWakeEnqueued) ==
                     DebugCategory::kPark);
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kEnqueue) ==
                     DebugCategory::kSched);
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kStealOk) ==
                     DebugCategory::kSched);
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kSysmonPass) ==
                     DebugCategory::kSched);
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kIoWait) ==
                     DebugCategory::kIo);
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kGCreated) ==
                     DebugCategory::kLifecycle);
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kSchedulerStop) ==
                     DebugCategory::kLifecycle);
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kMutexLock) ==
                     DebugCategory::kPrimitives);
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kChannelSelect) ==
                     DebugCategory::kPrimitives);
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kPCNotify) ==
                     DebugCategory::kPrimitives);
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kWaitGroupDone) ==
                     DebugCategory::kPrimitives);
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kTimerArm) ==
                     DebugCategory::kTimer);
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kTimerCallback) ==
                     DebugCategory::kTimer);
        // Doctor 事件不受开关控制：Enabled(kNone) 恒为 true。
        GO2CPP_CHECK(DebugCategoryOf(SchedEventCode::kDiagnosticForceWake) ==
                     DebugCategory::kNone);
        GO2CPP_CHECK(DebugConfig::Enabled(DebugCategory::kNone));
    }

    // 场景 2/3/4：解析白名单、别名、容错。
    {
        DebugConfig config;
        GO2CPP_CHECK(DebugConfig::ParseCategories("mutex", &config));
        CheckOnly(config, DebugCategory::kPrimitives);

        GO2CPP_CHECK(DebugConfig::ParseCategories(
            "lifecycle,sched,park,timer,io", &config));
        GO2CPP_CHECK(!Has(config, DebugCategory::kPrimitives));
        GO2CPP_CHECK(Has(config, DebugCategory::kLifecycle));
        GO2CPP_CHECK(Has(config, DebugCategory::kSched));
        GO2CPP_CHECK(Has(config, DebugCategory::kPark));
        GO2CPP_CHECK(Has(config, DebugCategory::kTimer));
        GO2CPP_CHECK(Has(config, DebugCategory::kIo));

        // 大小写不敏感 + 空白 + 连续逗号 + 尾逗号。
        GO2CPP_CHECK(DebugConfig::ParseCategories(
            " MUTEX ,  Channel ,, pc ,", &config));
        CheckOnly(config, DebugCategory::kPrimitives);

        // all / none / 空串。
        GO2CPP_CHECK(DebugConfig::ParseCategories("all", &config));
        for (const DebugCategory category :
             {DebugCategory::kLifecycle, DebugCategory::kSched,
              DebugCategory::kPark, DebugCategory::kPrimitives,
              DebugCategory::kTimer, DebugCategory::kIo,
              DebugCategory::kStacks, DebugCategory::kChains}) {
            GO2CPP_CHECK(Has(config, category));
        }
        GO2CPP_CHECK(DebugConfig::ParseCategories("none", &config));
        for (const DebugCategory category :
             {DebugCategory::kLifecycle, DebugCategory::kSched,
              DebugCategory::kPark, DebugCategory::kPrimitives,
              DebugCategory::kTimer, DebugCategory::kIo,
              DebugCategory::kStacks, DebugCategory::kChains}) {
            GO2CPP_CHECK(!Has(config, category));
        }
        GO2CPP_CHECK(DebugConfig::ParseCategories(nullptr, &config));
        GO2CPP_CHECK(Has(config, DebugCategory::kPrimitives));

        // 非法 token：整体返回 false，但合法 token 仍然生效。
        GO2CPP_CHECK(!DebugConfig::ParseCategories("mutex,bogus,timer", &config));
        GO2CPP_CHECK(Has(config, DebugCategory::kPrimitives));
        GO2CPP_CHECK(Has(config, DebugCategory::kTimer));
        GO2CPP_CHECK(!Has(config, DebugCategory::kSched));
    }

    // 场景 5：Set/Get/Reset 与 LoadFromEnv 幂等。
    {
        DebugConfig config;
        config.lifecycle = true;
        config.sched = false;
        config.park = false;
        config.primitives = true;
        config.timer = false;
        config.io = false;
        config.stacks = false;
        config.chains = false;
        config.sample_shift = 2;
        DebugConfig::Set(config);
        const DebugConfig current = DebugConfig::Get();
        GO2CPP_CHECK(current.lifecycle);
        GO2CPP_CHECK(!current.sched);
        GO2CPP_CHECK(!current.park);
        GO2CPP_CHECK(current.primitives);
        GO2CPP_CHECK(!current.timer);
        GO2CPP_CHECK(!current.io);
        GO2CPP_CHECK(!current.stacks);
        GO2CPP_CHECK(!current.chains);
        GO2CPP_CHECK(current.sample_shift == 2U);
        GO2CPP_CHECK(DebugConfig::SampleShift() == 2U);
        GO2CPP_CHECK(DebugConfig::Enabled(DebugCategory::kPrimitives));
        GO2CPP_CHECK(!DebugConfig::Enabled(DebugCategory::kSched));
        // 已显式 Set 后，LoadFromEnv 不得覆盖（幂等语义）。
        GO2CPP_CHECK(DebugConfig::LoadFromEnv());
        GO2CPP_CHECK(!DebugConfig::Get().sched);

        DebugConfig::Reset();
        const DebugConfig reset = DebugConfig::Get();
        GO2CPP_CHECK(reset.lifecycle && reset.sched && reset.park &&
                     reset.primitives && reset.timer && reset.io &&
                     reset.stacks && reset.chains);
        GO2CPP_CHECK(reset.sample_shift == 0U);

        // 超大 sample_shift 被截断到上限（16）。
        config.sample_shift = 999;
        DebugConfig::Set(config);
        GO2CPP_CHECK(DebugConfig::SampleShift() == 16U);
        DebugConfig::Reset();
    }

#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    // 场景 6：事件宏过滤（只开 primitives）。
    {
        DebugConfig config;
        config.lifecycle = false;
        config.sched = false;
        config.park = false;
        config.primitives = true;
        config.timer = false;
        config.io = false;
        DebugConfig::Set(config);
        go2cpp::debug::EventRing::Reset();
        go2cpp::debug::EventRing::ResetTotals();

        GO2CPP_DEBUG_EVENT(kSchedulerStart, 1, 2);
        GO2CPP_DEBUG_EVENT(kEnqueue, 2, 0);
        GO2CPP_DEBUG_EVENT(kParkEnter, 3, 1);
        GO2CPP_DEBUG_EVENT(kWakeEnqueued, 3, 1);
        GO2CPP_DEBUG_EVENT(kGCreated, 4, 0);
        GO2CPP_DEBUG_EVENT(kIoWait, 5, 7);
        GO2CPP_DEBUG_EVENT(kTimerArm, 6, 100);
        GO2CPP_DEBUG_EVENT(kMutexLock, 7, 0);
        GO2CPP_DEBUG_EVENT(kChannelSend, 8, 0);

        GO2CPP_CHECK(
            go2cpp::debug::EventRing::TotalCount(SchedEventCode::kMutexLock) == 1U);
        GO2CPP_CHECK(go2cpp::debug::EventRing::TotalCount(
                         SchedEventCode::kChannelSend) == 1U);
        GO2CPP_CHECK(
            go2cpp::debug::EventRing::TotalCount(SchedEventCode::kSchedulerStart) ==
            0U);
        GO2CPP_CHECK(go2cpp::debug::EventRing::TotalCount(
                         SchedEventCode::kEnqueue) == 0U);
        GO2CPP_CHECK(go2cpp::debug::EventRing::TotalCount(
                         SchedEventCode::kParkEnter) == 0U);
        GO2CPP_CHECK(go2cpp::debug::EventRing::TotalCount(
                         SchedEventCode::kWakeEnqueued) == 0U);
        GO2CPP_CHECK(go2cpp::debug::EventRing::TotalCount(
                         SchedEventCode::kGCreated) == 0U);
        GO2CPP_CHECK(
            go2cpp::debug::EventRing::TotalCount(SchedEventCode::kIoWait) == 0U);
        GO2CPP_CHECK(
            go2cpp::debug::EventRing::TotalCount(SchedEventCode::kTimerArm) == 0U);
        GO2CPP_CHECK(go2cpp::debug::EventRing::TotalCountAll() == 2U);

        DebugConfig::Reset();
        go2cpp::debug::EventRing::ResetTotals();
    }

    // 场景 7：sample_shift 的额外降采样。
    {
        DebugConfig config;
        config.sample_shift = 16;  // 1/65536
        DebugConfig::Set(config);
        go2cpp::debug::EventRing::ResetTotals();
        for (int index = 0; index < 1000; ++index) {
            GO2CPP_DEBUG_EVENT_SAMPLED(kMutexFastLock, 1, 0, 1);
        }
        GO2CPP_CHECK(go2cpp::debug::EventRing::TotalCount(
                         SchedEventCode::kMutexFastLock) == 0U);

        config.sample_shift = 0;
        DebugConfig::Set(config);
        go2cpp::debug::EventRing::ResetTotals();
        for (int index = 0; index < 1000; ++index) {
            GO2CPP_DEBUG_EVENT_SAMPLED(kMutexFastLock, 1, 0, 1);
        }
        GO2CPP_CHECK(go2cpp::debug::EventRing::TotalCount(
                         SchedEventCode::kMutexFastLock) == 1000U);
        DebugConfig::Reset();
        go2cpp::debug::EventRing::ResetTotals();
    }
#endif  // GO2CPP_DEBUG_DUMP

    // 场景 8：stacks/chains 是独立位，不影响行为类别。
    {
        DebugConfig config;
        config.stacks = false;
        config.chains = false;
        config.primitives = true;
        config.sched = true;
        config.park = false;
        DebugConfig::Set(config);
        GO2CPP_CHECK(!DebugConfig::Enabled(DebugCategory::kStacks));
        GO2CPP_CHECK(!DebugConfig::Enabled(DebugCategory::kChains));
        GO2CPP_CHECK(DebugConfig::Enabled(DebugCategory::kPrimitives));
        GO2CPP_CHECK(DebugConfig::Enabled(DebugCategory::kSched));
        GO2CPP_CHECK(!DebugConfig::Enabled(DebugCategory::kPark));
        DebugConfig::Reset();
    }
}
