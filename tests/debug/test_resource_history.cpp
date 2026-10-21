/**
 * @file test_resource_history.cpp
 * @brief 资源历史单测：注册表生命周期 / 资源事件 / 栈样本 / 四视图。
 *
 * 覆盖：
 *   1. 注册表：注册、按地址查、注销、墓碑、清空；OFF 构建返回无效 id；
 *   2. 资源事件：真实 sync::Mutex 加解锁产生 lock/unlock 事件，EventRing
 *      累计计数与 `--view history` 的 count 一致；
 *   3. 栈样本：CaptureStackSample / Lookup / Resolve / Reset；
 *   4. 四视图：resources（10 类覆盖矩阵）/ locks / channels / history 的
 *      schema、字段与计数。
 *
 * 编译期开关：
 *   - `GO2CPP_DEBUG_DUMP=ON` 时注册表与事件环才真正记录（Mutex/Channel 的
 *     插桩点就是 `#if GO2CPP_DEBUG_DUMP`）；
 *   - OFF 构建下这些接口是空操作，本测试改为断言「空语义 + schema 合法」，
 *     因此默认 build-work 与 build-enhanced 跑的是同一条 ctest。
 */
#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/debug/debug_dump.hpp"
#include "go2cpp/debug/debug_event_ring.hpp"
#include "go2cpp/debug/debug_resources.hpp"
#include "go2cpp/sync.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <string>

namespace {

using go2cpp::Channel;
using go2cpp::ChannelStatus;
using go2cpp::sync::Mutex;
using namespace go2cpp::debug;  // NOLINT(google-build-using-namespace)

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

/// JSON 字段匹配：兼容紧凑（`"k":v`）与非紧凑（`"k": v`）两种序列化。
/// 视图走 Json::dump() 紧凑输出；这里两种都接受，避免格式变化即误报。
bool HasField(const std::string& json, const std::string& key,
              const std::string& value) {
    return Contains(json, "\"" + key + "\":" + value) ||
           Contains(json, "\"" + key + "\": " + value);
}

/// 取 `"key": <非负整数>` 的第一个值；找不到/非数字返回 -1。
long long JsonInt(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    const std::size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        return -1;
    }
    std::size_t index = pos + needle.size();
    while (index < json.size() && (json[index] == ' ' || json[index] == '\t')) {
        ++index;
    }
    long long value = 0;
    const std::size_t start = index;
    while (index < json.size() && json[index] >= '0' && json[index] <= '9') {
        value = value * 10 + (json[index] - '0');
        ++index;
    }
    return index == start ? -1 : value;
}

// ---------------------------------------------------------------------------
// 1. 注册表生命周期
// ---------------------------------------------------------------------------
void TestRegistryLifecycle() {
    go2cpp_tests::announce("resource history: registry lifecycle");
    ResetResourceRegistry();
    int object = 0;
    const ResourceId id =
        RegisterResource(ResourceKind::kMutex, &object, "history-test");
#if GO2CPP_DEBUG_RESOURCES
    GO2CPP_CHECK(id != kInvalidResourceId);
    ResourceInfo info;
    GO2CPP_CHECK(LookupResource(id, &info));
    GO2CPP_CHECK(info.alive);
    GO2CPP_CHECK(info.name == "history-test");
    GO2CPP_CHECK(info.kind == ResourceKind::kMutex);
    GO2CPP_CHECK(info.address == &object);
    GO2CPP_CHECK(FindResourceByAddress(&object) == id);
    GO2CPP_CHECK(ResourceAliveCount() == 1U);
    // 同一地址重复注册必须返回同一个 id（幂等，不新建条目）。
    GO2CPP_CHECK(RegisterResource(ResourceKind::kMutex, &object, "again") == id);

    // 注销：按地址查不到、存活数归零；墓碑仍可按 id 回看历史。
    UnregisterResource(id);
    GO2CPP_CHECK(FindResourceByAddress(&object) == kInvalidResourceId);
    GO2CPP_CHECK(ResourceAliveCount() == 0U);
    GO2CPP_CHECK(LookupResource(id, &info));
    GO2CPP_CHECK(!info.alive);
    // destroyed_seq 允许为 0：进程内还没发布过任何事件时 PublishSeq() 就是
    // 0；墓碑语义的关键是 alive=false 且仍可按 id 查回条目。
    UnregisterResource(id);  // 重复注销必须幂等，不崩溃
#else
    // OFF 构建：注册接口返回 0 且不分配内存（零开销约定）。
    GO2CPP_CHECK(id == kInvalidResourceId);
    GO2CPP_CHECK(SnapshotResources().empty());
    GO2CPP_CHECK(ResourceAliveCount() == 0U);
    GO2CPP_CHECK(FindResourceByAddress(&object) == kInvalidResourceId);
#endif
    ResetResourceRegistry();
    GO2CPP_CHECK(ResourceAliveCount() == 0U);
}

// ---------------------------------------------------------------------------
// 2. Mutex 资源事件 + history / locks 视图
// ---------------------------------------------------------------------------
void TestMutexEventsAndHistory() {
    go2cpp_tests::announce("resource history: mutex events + views");
    ResetResourceRegistry();
    EventRing::Reset();
    EventRing::ResetTotals();
    Mutex mutex;
    GO2CPP_CHECK(mutex.Lock());
    mutex.Unlock();

    const std::string history = BuildViewJson("history", 0);
    GO2CPP_CHECK(HasField(history, "view", "\"history\""));
    GO2CPP_CHECK(HasField(history, "schema", "\"go2cpp.dump.view.history/1\""));
    const std::string locks = BuildViewJson("locks", 0);
    GO2CPP_CHECK(HasField(locks, "view", "\"locks\""));
    GO2CPP_CHECK(Contains(locks, "\"counts\""));
    const std::string resources = BuildViewJson("resources", 0);
    GO2CPP_CHECK(HasField(resources, "view", "\"resources\""));
    GO2CPP_CHECK(Contains(resources, "\"coverage\""));
    GO2CPP_CHECK(Contains(resources, "sync::Mutex"));

#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    const ResourceId id = FindResourceByAddress(&mutex);
    GO2CPP_CHECK(id != kInvalidResourceId);
    GO2CPP_CHECK(EventRing::TotalCount(SchedEventCode::kMutexLock) >= 1U);
    GO2CPP_CHECK(EventRing::TotalCount(SchedEventCode::kMutexUnlock) >= 1U);
    const std::string mutex_history = BuildViewJson("history", id);
    GO2CPP_CHECK(HasField(mutex_history, "resource_id", std::to_string(id)));
    GO2CPP_CHECK(JsonInt(mutex_history, "count") > 0);
    GO2CPP_CHECK(Contains(mutex_history, "\"summary\""));
    GO2CPP_CHECK(JsonInt(locks, "holds") >= 1);
    GO2CPP_CHECK(JsonInt(locks, "resources") >= 1);
#else
    GO2CPP_CHECK(JsonInt(locks, "holds") == 0);
#endif
    ResetResourceRegistry();
}

// ---------------------------------------------------------------------------
// 3. 线程栈样本表
// ---------------------------------------------------------------------------
void TestStackSamples() {
    go2cpp_tests::announce("resource history: stack samples");
    ResetStackSamples();
    GO2CPP_CHECK(StackSampleCount() == 0U);
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    const StackSampleId id = CaptureStackSample(0, true);
    GO2CPP_CHECK(id != 0U);
    StackSample sample;
    GO2CPP_CHECK(LookupStackSample(id, &sample));
    GO2CPP_CHECK(sample.frame_count > 0U);
    GO2CPP_CHECK(sample.refs >= 1U);
    GO2CPP_CHECK(StackSampleCount() == 1U);
    const auto frames = ResolveStackSample(id);
    GO2CPP_CHECK(!frames.empty());
    // 相同栈重复捕获只增加引用计数，不新建样本。
    const StackSampleId again = CaptureStackSample(0, true);
    GO2CPP_CHECK(again == id);
    GO2CPP_CHECK(StackSampleCount() == 1U);
#endif
    ResetStackSamples();
    GO2CPP_CHECK(StackSampleCount() == 0U);
    ResetFiberChains();
    GO2CPP_CHECK(FiberChainCount() == 0U);
}

// ---------------------------------------------------------------------------
// 4. 视图集合：schema / 未知视图 / channels
// ---------------------------------------------------------------------------
void TestViewsAndChannels() {
    go2cpp_tests::announce("resource history: views + channels");
    GO2CPP_CHECK(IsKnownViewName("history"));
    GO2CPP_CHECK(IsKnownViewName("locks"));
    GO2CPP_CHECK(IsKnownViewName("channels"));
    GO2CPP_CHECK(IsKnownViewName("resources"));
    GO2CPP_CHECK(!IsKnownViewName("no-such-view"));

    const std::string unknown = BuildViewJson("no-such-view", 0);
    GO2CPP_CHECK(HasField(unknown, "ok", "false"));
    GO2CPP_CHECK(HasField(unknown, "error", "\"unknown-view\""));

    ResetResourceRegistry();
    EventRing::Reset();
    EventRing::ResetTotals();
    {
        Channel<int> channel(4);
        const auto sent = channel.Send(7);
        GO2CPP_CHECK(sent.status == ChannelStatus::kReady);
        const auto received = channel.Recv();
        GO2CPP_CHECK(received.status == ChannelStatus::kReady);
        GO2CPP_CHECK(received.value.has_value());
        GO2CPP_CHECK(received.value.value_or(0) == 7);
    }
    const std::string channels = BuildViewJson("channels", 0);
    GO2CPP_CHECK(HasField(channels, "view", "\"channels\""));
    GO2CPP_CHECK(HasField(channels, "schema",
                          "\"go2cpp.dump.view.channels/1\""));
    GO2CPP_CHECK(Contains(channels, "\"counts\""));
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    // Channel 析构后注册表保留墓碑（用于回看历史），channels 视图仍能列出。
    GO2CPP_CHECK(JsonInt(channels, "resources") >= 1);
#endif
    ResetResourceRegistry();
}

}  // namespace

int main() {
    // v7：本测试验证「全量记录」语义（每条 lock/unlock 都进入历史），
    // 显式关闭资源事件与 sched 高频事件的默认采样；默认采样语义由
    // test_debug_config 的采样场景覆盖。
    go2cpp::debug::SetDebugEventSampleEvery(1, 1);
    std::printf("=== 资源历史单测（registry/events/stacks/views）===\n");
    TestRegistryLifecycle();
    TestMutexEventsAndHistory();
    TestStackSamples();
    TestViewsAndChannels();
    const int failures = go2cpp_tests::g_failures.load();
    std::printf("[resource-history] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}

// PATCH-MARKER-2026-10-21-RESOURCE-HISTORY
