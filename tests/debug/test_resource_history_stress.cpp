/**
 * @file test_resource_history_stress.cpp
 * @brief 资源历史压力测试：10 类结构注册、四视图计数、按资源过滤、事件守恒。
 *
 * ## 为什么需要这个文件
 *
 * `tests/debug/test_resource_history.cpp` 覆盖了注册表生命周期、Mutex 事件、
 * 栈样本与四个视图的 schema，但它的规模是「每类一个对象、每个操作一次」。
 * 资源历史真正的风险在**规模化与并发**：
 *
 * - 10 类结构同时存活时，`--view resources` 的 `count/alive/by_kind` 是否
 *   逐类正确（而不是只对 Mutex 正确）；
 * - `--view history --resource <id>` 是否**只**返回该资源的事件（过滤按
 *   `resource_id` 精确匹配，不能让邻居资源的事件混进来）；
 * - 多线程 1000 次操作后，事件累计数是否与操作数**严格守恒**（丢事件会
 *   让诊断缺失关键证据，重复事件会让时间线噪声化）。
 *
 * ## 一个必须如实说明的语义
 *
 * 资源注册表的粒度是**原语对象**，不是「用户可见的结构类」：`RWMutex`
 * 内部持有一把 `Mutex` 和一个 `ConditionVariable`，`TimedMutex` /
 * `RecursiveMutex` 内部各有一把 `Mutex`，`Channel` 内部有一个
 * `ParkingCondition`，`WaitGroup` 内部有 `Mutex` + `ConditionVariable`。
 * 这些内部原语**同样注册**，因此「10 个用户结构」在视图里的条目数会多于
 * 10。本测试不做「条目数恰好等于 10」的错误断言，而是：
 *
 * - 逐类单独构造，断言**该类自己的 kind** 至少出现 1 条（精确验证插桩点）；
 * - 10 类同时构造时，断言 10 个 kind 全部出现且视图计数与注册表快照一致；
 * - `SpinMutex` / `FutexMutex` 在源码中明确不插桩（resources 视图 coverage
 *   标注 `instrumented=false`），测试用公开注册接口显式登记，验证外部
 *   注册同样能被视图统计。
 *
 * ## 覆盖
 *
 * 1. **逐类注册**：10 类各自单独构造 → 该 kind 出现 → 析构后存活数归零；
 * 2. **10 类同时存活**：`by_kind` 十类齐全、`count == alive ==` 注册表快照
 *    长度、每类真实操作一次后计数不变；
 * 3. **注销**：2 类外部注册手动注销（幂等），8 类离开作用域自动注销，最终
 *    `alive == 0`、视图保留墓碑 `count >= 10`；
 * 4. **history 过滤**：资源 A 20 个事件、资源 B 6 个事件，按 A 查询只返回
 *    20 条且每条 `resource_id == A`，按 B 查询只返回 6 条；
 * 5. **1000 次操作守恒**：4 线程 × 250 次随机 `Lock` / `LockFor`（每线程
 *    独享一把 Mutex），`kMutexLock` 与 `kMutexUnlock` 的累计增量都恰好 1000；
 * 6. **多线程过滤**：4 线程竞争同一把 Mutex 各 100 次，线程存活期间按该
 *    资源查询，所有事件条目的 `resource_id` 都等于该资源。
 *
 * ## 构建兼容
 *
 * `GO2CPP_DEBUG_DUMP=ON` 时执行全部计数断言；OFF 时注册接口按零开销约定
 * 返回 0、视图返回空数据，测试改为断言「空语义 + coverage 仍列出 10 类」。
 * 因此同一个源文件在默认构建与增强构建下都能跑通。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/core/futex_mutex.hpp"
#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/concurrency/locks.hpp"
#include "go2cpp/concurrency/spin_mutex.hpp"
#include "go2cpp/debug/debug_dump.hpp"
#include "go2cpp/debug/debug_event_ring.hpp"
#include "go2cpp/debug/debug_resources.hpp"
#include "go2cpp/sync.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

using go2cpp::Channel;
using go2cpp::core::FutexMutex;
using go2cpp::core::ParkingCondition;
using go2cpp::sync::ConditionVariable;
using go2cpp::sync::Mutex;
using go2cpp::sync::RWMutex;
using go2cpp::sync::RecursiveMutex;
using go2cpp::sync::SpinMutex;
using go2cpp::sync::TimedMutex;
using go2cpp::sync::WaitGroup;

using namespace go2cpp::debug;  // NOLINT(google-build-using-namespace)

/// `by_kind` / `ResourceKindName` 里的 10 个资源类别键。
const char* const kKindKeys[] = {
    "kMutex",            "kRwMutex",       "kRecursiveMutex",
    "kParkingCondition", "kChannel",       "kConditionVariable",
    "kWaitGroup",        "kTimedMutex",    "kSpinMutex",
    "kFutexMutex",
};

/// `resources` 视图 coverage 里的 10 个结构名（注意与 by_kind 命名不同）。
const char* const kCoverageKinds[] = {
    "sync::Mutex",       "sync::RWMutex",   "sync::RecursiveMutex",
    "core::ParkingCondition", "Channel<T>", "sync::ConditionVariable",
    "sync::WaitGroup",   "sync::TimedMutex", "sync::SpinMutex",
    "core::FutexMutex",
};

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

/// JSON 字段匹配：兼容紧凑（`"k":v`）与非紧凑（`"k": v`）两种序列化。
bool HasField(const std::string& json, const std::string& key,
              const std::string& value) {
    return Contains(json, "\"" + key + "\":" + value) ||
           Contains(json, "\"" + key + "\": " + value);
}

/// 取 `"key": <非负整数>` 的第一个值；找不到/非数字返回 -1。
long long JsonInt(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    std::size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        return -1;
    }
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
        ++pos;
    }
    long long value = 0;
    const std::size_t start = pos;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        value = value * 10 + (json[pos] - '0');
        ++pos;
    }
    return pos == start ? -1 : value;
}

/// 收集 JSON 中所有 `"resource_id": N` 的数值（root + 每条事件）。
std::vector<std::uint64_t> CollectResourceIds(const std::string& json) {
    const std::string needle = "\"resource_id\":";
    std::vector<std::uint64_t> ids;
    std::size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos) {
        pos += needle.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
            ++pos;
        }
        std::uint64_t value = 0;
        bool any = false;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            value = value * 10 + static_cast<std::uint64_t>(json[pos] - '0');
            ++pos;
            any = true;
        }
        if (any) {
            ids.push_back(value);
        }
    }
    return ids;
}

/// 统计注册表中指定 kind 的**存活**条目数。
std::size_t CountAliveKind(ResourceKind kind) {
    std::size_t count = 0;
    for (const auto& info : SnapshotResources()) {
        if (info.alive && info.kind == kind) {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// 1. 逐类注册：每类单独构造 → kind 出现 → 析构后存活归零
// ---------------------------------------------------------------------------
void TestEachKindRegisters() {
    go2cpp_tests::announce("resource-stress: per-kind registration");
    ResetResourceRegistry();

#if GO2CPP_DEBUG_RESOURCES
    {
        Mutex mutex;
        GO2CPP_CHECK(FindResourceByAddress(&mutex) != kInvalidResourceId);
        GO2CPP_CHECK(CountAliveKind(ResourceKind::kMutex) == 1U);
    }
    GO2CPP_CHECK(ResourceAliveCount() == 0U);

    {
        ConditionVariable condition_variable;
        GO2CPP_CHECK(FindResourceByAddress(&condition_variable) !=
                     kInvalidResourceId);
        GO2CPP_CHECK(CountAliveKind(ResourceKind::kConditionVariable) == 1U);
    }
    GO2CPP_CHECK(ResourceAliveCount() == 0U);

    {
        WaitGroup wait_group;
        GO2CPP_CHECK(FindResourceByAddress(&wait_group) !=
                     kInvalidResourceId);
        GO2CPP_CHECK(CountAliveKind(ResourceKind::kWaitGroup) == 1U);
    }
    GO2CPP_CHECK(ResourceAliveCount() == 0U);

    {
        RWMutex rw_mutex;
        GO2CPP_CHECK(FindResourceByAddress(&rw_mutex) != kInvalidResourceId);
        GO2CPP_CHECK(CountAliveKind(ResourceKind::kRwMutex) == 1U);
    }
    GO2CPP_CHECK(ResourceAliveCount() == 0U);

    {
        RecursiveMutex recursive_mutex;
        GO2CPP_CHECK(FindResourceByAddress(&recursive_mutex) !=
                     kInvalidResourceId);
        GO2CPP_CHECK(CountAliveKind(ResourceKind::kRecursiveMutex) == 1U);
    }
    GO2CPP_CHECK(ResourceAliveCount() == 0U);

    {
        TimedMutex timed_mutex;
        GO2CPP_CHECK(FindResourceByAddress(&timed_mutex) !=
                     kInvalidResourceId);
        GO2CPP_CHECK(CountAliveKind(ResourceKind::kTimedMutex) == 1U);
    }
    GO2CPP_CHECK(ResourceAliveCount() == 0U);

    {
        Channel<int> channel(2);
        GO2CPP_CHECK(FindResourceByAddress(&channel) != kInvalidResourceId);
        GO2CPP_CHECK(CountAliveKind(ResourceKind::kChannel) == 1U);
    }
    GO2CPP_CHECK(ResourceAliveCount() == 0U);

    {
        ParkingCondition parking_condition;
        GO2CPP_CHECK(FindResourceByAddress(&parking_condition) !=
                     kInvalidResourceId);
        GO2CPP_CHECK(CountAliveKind(ResourceKind::kParkingCondition) == 1U);
    }
    GO2CPP_CHECK(ResourceAliveCount() == 0U);
#endif

    // 这两类没有内置插桩：显式注册 → kind 出现 → 注销后归零。
    {
        SpinMutex spin_mutex;
        FutexMutex futex_mutex;
        // SpinMutex 的第一个成员就是内部 sync::Mutex，二者地址相同；内部
        // Mutex 构造时已按 kMutex 注册。注册表按地址唯一，先注销该地址上
        // 的内部条目，再按 kSpinMutex 显式登记，避免「同址不同 kind」。
        UnregisterResourceByAddress(&spin_mutex);
        const ResourceId spin_id = RegisterResource(ResourceKind::kSpinMutex,
                                                    &spin_mutex,
                                                    "stress-spin-mutex");
        const ResourceId futex_id = RegisterResource(ResourceKind::kFutexMutex,
                                                     &futex_mutex,
                                                     "stress-futex-mutex");
#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_CHECK(spin_id != kInvalidResourceId);
        GO2CPP_CHECK(futex_id != kInvalidResourceId);
        GO2CPP_CHECK(spin_id != futex_id);
        GO2CPP_CHECK(CountAliveKind(ResourceKind::kSpinMutex) == 1U);
        GO2CPP_CHECK(CountAliveKind(ResourceKind::kFutexMutex) == 1U);
        GO2CPP_CHECK(ResourceAliveCount() == 2U);
#else
        GO2CPP_CHECK(spin_id == kInvalidResourceId);
        GO2CPP_CHECK(futex_id == kInvalidResourceId);
        GO2CPP_CHECK(ResourceAliveCount() == 0U);
#endif
        UnregisterResource(spin_id);
        UnregisterResource(futex_id);
        UnregisterResource(spin_id);  // 重复注销必须幂等
    }
    GO2CPP_CHECK(ResourceAliveCount() == 0U);
    ResetResourceRegistry();
}

// ---------------------------------------------------------------------------
// 2. 10 类同时存活：by_kind 十类齐全 + 视图计数与注册表一致
// ---------------------------------------------------------------------------
void TestTenKindsTogether() {
    go2cpp_tests::announce(
        "resource-stress: 10 kinds alive + resources view counts");
    ResetResourceRegistry();
    EventRing::Reset();
    EventRing::ResetTotals();

    {
        Mutex mutex;
        ConditionVariable condition_variable;
        WaitGroup wait_group;
        RWMutex rw_mutex;
        RecursiveMutex recursive_mutex;
        TimedMutex timed_mutex;
        SpinMutex spin_mutex;
        FutexMutex futex_mutex;
        Channel<int> channel(2);
        ParkingCondition parking_condition;

        // SpinMutex 与内部 Mutex 同址：先注销内部条目再按 kSpinMutex 注册
        // （原因见 TestEachKindRegisters）。
        UnregisterResourceByAddress(&spin_mutex);
        const ResourceId spin_id = RegisterResource(ResourceKind::kSpinMutex,
                                                    &spin_mutex,
                                                    "stress-spin-mutex");
        const ResourceId futex_id = RegisterResource(ResourceKind::kFutexMutex,
                                                     &futex_mutex,
                                                     "stress-futex-mutex");

#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_CHECK(spin_id != kInvalidResourceId);
        GO2CPP_CHECK(futex_id != kInvalidResourceId);
        GO2CPP_CHECK(ResourceAliveCount() >= 10U);
        GO2CPP_CHECK(FindResourceByAddress(&mutex) != kInvalidResourceId);
        GO2CPP_CHECK(FindResourceByAddress(&condition_variable) !=
                     kInvalidResourceId);
        GO2CPP_CHECK(FindResourceByAddress(&wait_group) != kInvalidResourceId);
        GO2CPP_CHECK(FindResourceByAddress(&rw_mutex) != kInvalidResourceId);
        GO2CPP_CHECK(FindResourceByAddress(&recursive_mutex) !=
                     kInvalidResourceId);
        GO2CPP_CHECK(FindResourceByAddress(&timed_mutex) != kInvalidResourceId);
        GO2CPP_CHECK(FindResourceByAddress(&channel) != kInvalidResourceId);
        GO2CPP_CHECK(FindResourceByAddress(&parking_condition) !=
                     kInvalidResourceId);
        GO2CPP_CHECK(FindResourceByAddress(&spin_mutex) == spin_id);
        GO2CPP_CHECK(FindResourceByAddress(&futex_mutex) == futex_id);
#endif

        const std::size_t alive_entries = ResourceAliveCount();
        const std::size_t snapshot_entries = SnapshotResources().size();
        const std::string resources = BuildViewJson("resources", 0);
        GO2CPP_CHECK(HasField(resources, "view", "\"resources\""));
        GO2CPP_CHECK(HasField(resources, "schema",
                              "\"go2cpp.dump.view.resources/1\""));
        GO2CPP_CHECK(JsonInt(resources, "count") ==
                     static_cast<long long>(snapshot_entries));
        GO2CPP_CHECK(JsonInt(resources, "alive") ==
                     static_cast<long long>(alive_entries));
#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_CHECK(alive_entries >= 10U);
        // 十类 kind 全部出现，且 by_kind 的数字与注册表一致。
        for (const char* kind : kKindKeys) {
            const std::string expected = std::string("\"") + kind + "\":";
            GO2CPP_CHECK(Contains(resources, expected));
        }
        for (const auto& info : SnapshotResources()) {
            if (info.alive) {
                GO2CPP_CHECK(JsonInt(resources,
                                    ResourceKindName(info.kind)) >= 1);
            }
        }
#else
        GO2CPP_CHECK(alive_entries == 0U);
        GO2CPP_CHECK(snapshot_entries == 0U);
        GO2CPP_CHECK(JsonInt(resources, "count") == 0);
        GO2CPP_CHECK(JsonInt(resources, "alive") == 0);
#endif

        // coverage 在两种构建下都列出 10 类，并如实标注插桩状态。
        for (const char* kind : kCoverageKinds) {
            const std::string compact = std::string("\"kind\":\"") + kind +
                                        "\"";
            const std::string spaced = std::string("\"kind\": \"") + kind +
                                       "\"";
            GO2CPP_CHECK(Contains(resources, compact) ||
                         Contains(resources, spaced));
        }
        GO2CPP_CHECK(Contains(resources, "\"instrumented\":false"));

        // 每类做一次真实操作：注册数量与视图计数不因操作而变化。
        GO2CPP_CHECK(mutex.Lock());
        mutex.Unlock();
        rw_mutex.RLock();
        rw_mutex.RUnlock();
        recursive_mutex.Lock();
        recursive_mutex.Lock();
        recursive_mutex.Unlock();
        recursive_mutex.Unlock();
        GO2CPP_CHECK(timed_mutex.TryLock());
        timed_mutex.Unlock();
        spin_mutex.Lock();
        spin_mutex.Unlock();
        {
            std::lock_guard<FutexMutex> guard(futex_mutex);
        }
        condition_variable.NotifyOne();
        wait_group.Add(1);
        wait_group.Done();
        GO2CPP_CHECK(channel.Send(1).Ok());
        GO2CPP_CHECK(channel.Recv().Ok());
        parking_condition.notify_all();

        GO2CPP_CHECK(ResourceAliveCount() == alive_entries);
        GO2CPP_CHECK(JsonInt(BuildViewJson("resources", 0), "alive") ==
                     static_cast<long long>(alive_entries));

        // 手动注销 2 类外部注册（幂等），其余 8 类仍存活。
        UnregisterResource(spin_id);
        UnregisterResource(futex_id);
        UnregisterResource(spin_id);
#if GO2CPP_DEBUG_RESOURCES
        GO2CPP_CHECK(ResourceAliveCount() == alive_entries - 2U);
#endif
    }

    // 8 类离开作用域自动注销：alive 归零，墓碑仍可回看（count 不减少）。
#if GO2CPP_DEBUG_RESOURCES
    GO2CPP_CHECK(ResourceAliveCount() == 0U);
    const std::string after = BuildViewJson("resources", 0);
    GO2CPP_CHECK(JsonInt(after, "count") >= 10);
    GO2CPP_CHECK(JsonInt(after, "alive") == 0);
    GO2CPP_CHECK(JsonInt(after, "count") ==
                 static_cast<long long>(SnapshotResources().size()));
#endif
    ResetResourceRegistry();
    GO2CPP_CHECK(ResourceAliveCount() == 0U);
}

// ---------------------------------------------------------------------------
// 3. history 按资源过滤：A 的查询不能混入 B 的事件
// ---------------------------------------------------------------------------
void TestHistoryFiltering() {
    go2cpp_tests::announce("resource-stress: history filter by resource id");
    ResetResourceRegistry();
    EventRing::Reset();
    EventRing::ResetTotals();

    Mutex resource_a;
    Mutex resource_b;
    for (int iteration = 0; iteration < 10; ++iteration) {
        GO2CPP_CHECK(resource_a.Lock());
        resource_a.Unlock();
    }
    for (int iteration = 0; iteration < 3; ++iteration) {
        GO2CPP_CHECK(resource_b.Lock());
        resource_b.Unlock();
    }

#if GO2CPP_DEBUG_RESOURCES
    const ResourceId id_a = FindResourceByAddress(&resource_a);
    const ResourceId id_b = FindResourceByAddress(&resource_b);
    GO2CPP_REQUIRE(id_a != kInvalidResourceId);
    GO2CPP_REQUIRE(id_b != kInvalidResourceId);
    GO2CPP_REQUIRE(id_a != id_b);

    const std::string history_a = BuildViewJson("history", id_a);
    GO2CPP_CHECK(HasField(history_a, "view", "\"history\""));
    GO2CPP_CHECK(HasField(history_a, "schema",
                          "\"go2cpp.dump.view.history/1\""));
    GO2CPP_CHECK(JsonInt(history_a, "resource_id") ==
                 static_cast<long long>(id_a));
    GO2CPP_CHECK(JsonInt(history_a, "count") == 20);
    const auto ids_a = CollectResourceIds(history_a);
    GO2CPP_CHECK(ids_a.size() == 21U);  // root + 20 条事件
    for (const std::uint64_t id : ids_a) {
        GO2CPP_CHECK(id == id_a);
    }

    const std::string history_b = BuildViewJson("history", id_b);
    GO2CPP_CHECK(JsonInt(history_b, "resource_id") ==
                 static_cast<long long>(id_b));
    GO2CPP_CHECK(JsonInt(history_b, "count") == 6);
    const auto ids_b = CollectResourceIds(history_b);
    GO2CPP_CHECK(ids_b.size() == 7U);
    for (const std::uint64_t id : ids_b) {
        GO2CPP_CHECK(id == id_b);
    }

    // 未知资源：count=0、resource.known=false，不能伪造事件。
    const std::string missing = BuildViewJson("history", 9999999);
    GO2CPP_CHECK(JsonInt(missing, "count") == 0);
    GO2CPP_CHECK(Contains(missing, "\"known\":false"));
#else
    GO2CPP_CHECK(FindResourceByAddress(&resource_a) == kInvalidResourceId);
    GO2CPP_CHECK(JsonInt(BuildViewJson("history", 1), "count") == 0);
#endif
    ResetResourceRegistry();
}

// ---------------------------------------------------------------------------
// 4. 1000 次操作事件守恒（4 线程 × 250 次随机 Lock/LockFor）
// ---------------------------------------------------------------------------
void TestEventConservation() {
    go2cpp_tests::announce(
        "resource-stress: 1000 random ops x 4 threads event conservation");

    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 250;
    constexpr long long kExpected = kThreads * kOpsPerThread;

    EventRing::ResetTotals();
    const std::uint64_t lock_before =
        EventRing::TotalCount(SchedEventCode::kMutexLock);
    const std::uint64_t unlock_before =
        EventRing::TotalCount(SchedEventCode::kMutexUnlock);

    std::atomic<long long> completed{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([&, thread_index] {
            // 每线程独享一把 Mutex：无竞争时 Lock/LockFor 都只发一对
            // lock/unlock 事件，因此守恒关系可以精确断言 1:1。
            Mutex mutex;
            std::mt19937 random(0x9E3779B9U + thread_index);
            for (int iteration = 0; iteration < kOpsPerThread; ++iteration) {
                // 随机选择两种「都会产生 1 个 lock 事件」的获取方式；
                // TryLock 不在快路径插桩，不参与本守恒模型。
                if (random() % 2U == 0U) {
                    if (!mutex.Lock()) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                } else if (!mutex.LockFor(5ms)) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                mutex.Unlock();
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 60s);
    }

    GO2CPP_CHECK(failures.load(std::memory_order_acquire) == 0);
    GO2CPP_CHECK(completed.load(std::memory_order_acquire) == kExpected);

    const std::uint64_t locks =
        EventRing::TotalCount(SchedEventCode::kMutexLock) - lock_before;
    const std::uint64_t unlocks =
        EventRing::TotalCount(SchedEventCode::kMutexUnlock) - unlock_before;
#if GO2CPP_DEBUG_RESOURCES
    GO2CPP_CHECK(locks == static_cast<std::uint64_t>(kExpected));
    GO2CPP_CHECK(unlocks == static_cast<std::uint64_t>(kExpected));
    GO2CPP_CHECK(EventRing::TotalCountAll() >=
                 static_cast<std::uint64_t>(kExpected) * 2U);
#else
    // OFF 构建：事件环整体关闭，累计计数必须保持 0（零开销约定）。
    GO2CPP_CHECK(locks == 0U);
    GO2CPP_CHECK(unlocks == 0U);
    GO2CPP_CHECK(EventRing::TotalCountAll() == 0U);
#endif
    std::printf(
        "[resource] conservation: locks=%llu unlocks=%llu expected=%lld\n",
        static_cast<unsigned long long>(locks),
        static_cast<unsigned long long>(unlocks), kExpected);
}

// ---------------------------------------------------------------------------
// 5. 多线程共享资源：线程存活期间 history 过滤只返回该资源
// ---------------------------------------------------------------------------
void TestConcurrentHistoryFiltering() {
    go2cpp_tests::announce(
        "resource-stress: concurrent history filter (4 threads x 100)");
    ResetResourceRegistry();
    EventRing::Reset();

    Mutex shared;
    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 100;
    std::atomic<int> failures{0};
    std::atomic<int> done{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([&] {
            for (int iteration = 0; iteration < kOpsPerThread; ++iteration) {
                if (!shared.Lock()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                shared.Unlock();
            }
            done.fetch_add(1, std::memory_order_release);
            // 保持线程（及其事件环）存活，直到主线程完成查询：
            // EventRing 的线程环在线程退出时注销，退出后查询会看不到
            // 该线程的事件。
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
    }
    GO2CPP_REQUIRE_EVENTUALLY(done.load(std::memory_order_acquire) == kThreads,
                              30s);
    GO2CPP_CHECK(failures.load(std::memory_order_acquire) == 0);

#if GO2CPP_DEBUG_RESOURCES
    const ResourceId id = FindResourceByAddress(&shared);
    GO2CPP_REQUIRE(id != kInvalidResourceId);
    const std::string history = BuildViewJson("history", id);
    GO2CPP_CHECK(JsonInt(history, "count") > 0);
    const auto ids = CollectResourceIds(history);
    GO2CPP_CHECK(ids.size() >= 2U);  // root + 至少 1 条事件
    for (const std::uint64_t event_id : ids) {
        GO2CPP_CHECK(event_id == id);
    }
#else
    GO2CPP_CHECK(JsonInt(BuildViewJson("history", 1), "count") == 0);
#endif

    release.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        GO2CPP_JOIN_WITH_WATCHDOG(thread, 30s);
    }
    ResetResourceRegistry();
}

}  // namespace

int main() {
    // v7：压力测试验证「全量记录 + 1:1 守恒」，显式关闭默认采样。
    go2cpp::debug::SetDebugEventSampleEvery(1, 1);
    std::printf("=== 资源历史压力测试（10 类 / 过滤 / 事件守恒）===\n");
    const auto total_started = std::chrono::steady_clock::now();

    TestEachKindRegisters();
    TestTenKindsTogether();
    TestHistoryFiltering();
    TestEventConservation();
    TestConcurrentHistoryFiltering();

    const int failures = go2cpp_tests::g_failures.load();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      total_started)
            .count();
    std::printf("[resource-history-stress] failures=%d elapsed=%.3fs\n",
                failures, seconds);
    return failures == 0 ? 0 : 1;
}
