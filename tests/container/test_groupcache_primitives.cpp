/**
 * @file test_groupcache_primitives.cpp
 * @brief groupcache 前置组件测试：singleflight + consistent hash。
 *
 * @details 覆盖：
 *   - singleflight：单 key 合并（100 并发，fn 执行 1 次、shared=99）、不同 key
 *     各自执行、异常广播给全部等待者且之后可重试、void 返回、Forget/Size；
 *   - consistent hash：空环、3 节点 1000 key 分布、Add 幂等、删除/新增节点
 *     的迁移比例与落点；
 *   - 压测：singleflight 1e5 次 Do、consistent hash 1e6 次 Get（ns/op）。
 *
 * 构建与运行：`tests/container/build_and_run_gc_primitives.sh`。
 */
#include "go2cpp/concurrency/singleflight.hpp"
#include "go2cpp/container/consistent_hash.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

std::atomic<int> g_failures{0};

void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        return;
    }
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "%s:%d: gc-primitives check failed: %s\n", file, line,
                 expression);
}

void Require(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "%s:%d: gc-primitives REQUIRE failed: %s\n", file, line,
                 expression);
    std::fflush(stderr);
    std::abort();
}

#define GC_CHECK(expression) \
    ::Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define GC_REQUIRE(expression) \
    ::Require(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

using Clock = std::chrono::steady_clock;

double NsPerOp(Clock::time_point start, std::size_t operations) {
    const double ns =
        std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    return ns / static_cast<double>(operations);
}

using go2cpp::concurrency::Group;
using go2cpp::container::Map;

// ---------------------------------------------------------------------------
// singleflight
// ---------------------------------------------------------------------------

void TestSingleflightBasics() {
    Group group;
    int calls = 0;
    int value = 0;

    const auto [result, shared, error] =
        group.Do("answer", [&calls, &value]() {
            ++calls;
            value = 42;
            return value;
        });
    GC_CHECK(!shared);
    GC_CHECK(error == nullptr);
    GC_CHECK(result.HasValue());
    GC_CHECK(result.Get<int>() == 42);
    GC_CHECK(calls == 1);
    GC_CHECK(value == 42);
    GC_CHECK(group.Size() == 0);

    // void 返回：结果为空、无错误、fn 确实执行。
    bool ran = false;
    const auto [void_result, void_shared, void_error] =
        group.Do("void", [&ran] { ran = true; });
    GC_CHECK(!void_shared);
    GC_CHECK(void_error == nullptr);
    GC_CHECK(!void_result.HasValue());
    GC_CHECK(ran);
    GC_CHECK(group.Size() == 0);
}

void TestSingleflightCoalesce() {
    constexpr int kThreads = 100;
    Group group;

    std::atomic<int> fn_calls{0};
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<int> shared_count{0};
    std::atomic<int> wrong_results{0};
    std::vector<int> results(kThreads, 0);
    std::vector<go2cpp::error::ErrorPtr> errors(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const auto [result, shared, error] = group.Do("hot-key", [&]() {
                fn_calls.fetch_add(1, std::memory_order_relaxed);
                // 留出足够窗口，让其余线程都进入本 key 的等待队列。
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                return 42;
            });
            errors[i] = error;
            if (shared) {
                shared_count.fetch_add(1, std::memory_order_relaxed);
            }
            results[i] = result.HasValue() ? result.Get<int>() : -1;
            if (results[i] != 42) {
                wrong_results.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) < kThreads) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    const int calls = fn_calls.load(std::memory_order_relaxed);
    const int merged = shared_count.load(std::memory_order_relaxed);
    std::printf(
        "[gc-primitives] singleflight 100 threads same key: fn_calls=%d "
        "shared=%d wrong_results=%d\n",
        calls, merged, wrong_results.load(std::memory_order_relaxed));
    GC_CHECK(calls == 1);
    GC_CHECK(merged == kThreads - 1);
    GC_CHECK(wrong_results.load(std::memory_order_relaxed) == 0);
    for (const auto& error : errors) {
        GC_CHECK(error == nullptr);
    }
    GC_CHECK(group.Size() == 0);
}

void TestSingleflightDistinctKeys() {
    constexpr int kThreads = 8;
    Group group;
    std::atomic<int> fn_calls{0};
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<int> results(kThreads, -1);
    std::vector<char> shared_flags(kThreads, 0);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            const std::string key = "key-" + std::to_string(i);
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const auto [result, shared, error] = group.Do(key, [&]() {
                fn_calls.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                return i;
            });
            GC_CHECK(error == nullptr);
            results[i] = result.HasValue() ? result.Get<int>() : -1;
            shared_flags[i] = shared ? 1 : 0;
        });
    }
    while (ready.load(std::memory_order_acquire) < kThreads) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    std::printf("[gc-primitives] singleflight distinct keys: fn_calls=%d\n",
                fn_calls.load(std::memory_order_relaxed));
    GC_CHECK(fn_calls.load(std::memory_order_relaxed) == kThreads);
    for (int i = 0; i < kThreads; ++i) {
        GC_CHECK(results[i] == i);
        GC_CHECK(shared_flags[i] == 0);
    }
    GC_CHECK(group.Size() == 0);
}

void TestSingleflightException() {
    constexpr int kThreads = 32;
    Group group;
    std::atomic<int> fn_calls{0};
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<int> with_error{0};
    std::atomic<int> with_result{0};
    std::atomic<int> bad_message{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const auto [result, shared, error] = group.Do("boom-key", [&]() -> int {
                fn_calls.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                throw std::runtime_error("boom");
            });
            if (error != nullptr) {
                with_error.fetch_add(1, std::memory_order_relaxed);
                if (error->Message().find("boom") == std::string::npos) {
                    bad_message.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (result.HasValue()) {
                with_result.fetch_add(1, std::memory_order_relaxed);
            }
            (void)shared;
        });
    }
    while (ready.load(std::memory_order_acquire) < kThreads) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    std::printf(
        "[gc-primitives] singleflight exception: fn_calls=%d with_error=%d\n",
        fn_calls.load(std::memory_order_relaxed),
        with_error.load(std::memory_order_relaxed));
    GC_CHECK(fn_calls.load(std::memory_order_relaxed) == 1);
    GC_CHECK(with_error.load(std::memory_order_relaxed) == kThreads);
    GC_CHECK(with_result.load(std::memory_order_relaxed) == 0);
    GC_CHECK(bad_message.load(std::memory_order_relaxed) == 0);
    GC_CHECK(group.Size() == 0);

    // 错误不缓存：同一个 key 可以再次执行并成功。
    const auto [result, shared, error] =
        group.Do("boom-key", [&fn_calls]() {
            fn_calls.fetch_add(1, std::memory_order_relaxed);
            return 7;
        });
    GC_CHECK(error == nullptr);
    GC_CHECK(!shared);
    GC_CHECK(result.HasValue() && result.Get<int>() == 7);
    GC_CHECK(fn_calls.load(std::memory_order_relaxed) == 2);
    GC_CHECK(group.Size() == 0);
}

void TestSingleflightForget() {
    Group group;
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::thread worker([&] {
        const auto [result, shared, error] = group.Do("forget-me", [&] {
            entered.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return 1;
        });
        GC_CHECK(error == nullptr);
        GC_CHECK(!shared);
        GC_CHECK(result.Get<int>() == 1);
    });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    GC_CHECK(group.Size() == 1);
    group.Forget("forget-me");
    GC_CHECK(group.Size() == 0);
    release.store(true, std::memory_order_release);
    worker.join();
    GC_CHECK(group.Size() == 0);
}

// ---------------------------------------------------------------------------
// consistent hash
// ---------------------------------------------------------------------------

void TestConsistentHashBasics() {
    Map empty;
    GC_CHECK(empty.Size() == 0);
    GC_CHECK(empty.Get("anything").empty());

    Map map(50);
    map.Add({"node-a", "node-b", "node-c"});
    GC_REQUIRE(map.Size() == 150);
    const std::string owner = map.Get("user:42");
    GC_CHECK(owner == "node-a" || owner == "node-b" || owner == "node-c");

    // Add 幂等：重复加入已有节点不改变环。
    const std::size_t before = map.Size();
    const std::string before_owner = map.Get("user:42");
    map.Add({"node-a", "node-b", "node-c"});
    GC_CHECK(map.Size() == before);
    GC_CHECK(map.Get("user:42") == before_owner);

    // 自定义 hash：全部 key 都落到同一个虚拟节点的后继上（仍应返回已加入节点）。
    Map identity(3, [](std::string_view data) {
        return static_cast<std::uint32_t>(data.size());
    });
    identity.Add({"x", "y"});
    GC_CHECK(identity.Get("abc") == "x" || identity.Get("abc") == "y");
}

void TestConsistentHashDistribution() {
    constexpr int kKeys = 1000;
    Map map(50);
    map.Add({"node-a", "node-b", "node-c"});

    std::vector<std::string> keys;
    keys.reserve(kKeys);
    int counts[3] = {0, 0, 0};
    const char* names[3] = {"node-a", "node-b", "node-c"};
    for (int i = 0; i < kKeys; ++i) {
        keys.push_back("key-" + std::to_string(i));
        const std::string owner = map.Get(keys.back());
        int index = -1;
        for (int n = 0; n < 3; ++n) {
            if (owner == names[n]) {
                index = n;
            }
        }
        GC_CHECK(index >= 0);
        if (index >= 0) {
            ++counts[index];
        }
    }
    std::printf(
        "[gc-primitives] consistent hash distribution (3 nodes, 1000 keys): "
        "a=%d b=%d c=%d\n",
        counts[0], counts[1], counts[2]);
    for (int n = 0; n < 3; ++n) {
        GC_CHECK(counts[n] >= 200);
        GC_CHECK(counts[n] <= 600);
    }
}

void TestConsistentHashMigration() {
    constexpr int kKeys = 1000;
    Map with_three(50);
    with_three.Add({"node-a", "node-b", "node-c"});
    Map with_two(50);
    with_two.Add({"node-a", "node-b"});
    Map with_four(50);
    with_four.Add({"node-a", "node-b", "node-c", "node-d"});

    int removed_moved = 0;
    int removed_wrong_target = 0;
    int removed_untouched_changed = 0;
    int added_moved = 0;
    int added_wrong_target = 0;
    int added_untouched_changed = 0;

    for (int i = 0; i < kKeys; ++i) {
        const std::string key = "key-" + std::to_string(i);
        const std::string owner3 = with_three.Get(key);
        const std::string owner2 = with_two.Get(key);
        const std::string owner4 = with_four.Get(key);

        if (owner2 != owner3) {
            ++removed_moved;
            if (owner2 == "node-c" || (owner2 != "node-a" && owner2 != "node-b")) {
                ++removed_wrong_target;
            }
            if (owner3 != "node-c") {
                ++removed_untouched_changed;
            }
        } else if (owner3 == "node-c") {
            // 删除 node-c 后，原本属于它的 key 必须迁移。
            ++removed_untouched_changed;
        }

        if (owner4 != owner3) {
            ++added_moved;
            if (owner4 != "node-d") {
                ++added_wrong_target;
            }
            if (owner3 == "node-d") {
                ++added_untouched_changed;
            }
        }
    }

    std::printf(
        "[gc-primitives] consistent hash migration: remove=%d/1000 "
        "(wrong_target=%d untouched_changed=%d), add=%d/1000 "
        "(wrong_target=%d untouched_changed=%d)\n",
        removed_moved, removed_wrong_target, removed_untouched_changed,
        added_moved, added_wrong_target, added_untouched_changed);
    // 删掉一个 3 节点中的节点：约 1/3 迁移，且只影响原本属于它的 key。
    GC_CHECK(removed_moved >= 100);
    GC_CHECK(removed_moved <= 600);
    GC_CHECK(removed_wrong_target == 0);
    GC_CHECK(removed_untouched_changed == 0);
    // 新增一个第 4 节点：约 1/4 迁移，且迁移的 key 全部落到新节点。
    GC_CHECK(added_moved >= 100);
    GC_CHECK(added_moved <= 600);
    GC_CHECK(added_wrong_target == 0);
    GC_CHECK(added_untouched_changed == 0);
}

// ---------------------------------------------------------------------------
// 压测
// ---------------------------------------------------------------------------

void BenchmarkSingleflight() {
    constexpr std::size_t kOps = 100000;
    Group group;
    std::size_t sink = 0;
    const auto start = Clock::now();
    for (std::size_t i = 0; i < kOps; ++i) {
        const auto [result, shared, error] =
            group.Do("hot-key", []() { return 1; });
        (void)shared;
        (void)error;
        sink += static_cast<std::size_t>(result.Get<int>());
    }
    const double ns = NsPerOp(start, kOps);
    std::printf(
        "[bench] singleflight Do x%zu (hot key, single thread): %.1f ns/op "
        "(sink=%zu)\n",
        kOps, ns, sink);
    GC_CHECK(sink == kOps);
}

void BenchmarkConsistentHash() {
    constexpr std::size_t kOps = 1000000;
    constexpr std::size_t kKeys = 1000;
    Map map(50);
    map.Add({"node-a", "node-b", "node-c"});
    std::vector<std::string> keys;
    keys.reserve(kKeys);
    for (std::size_t i = 0; i < kKeys; ++i) {
        keys.push_back("key-" + std::to_string(i));
    }

    std::size_t sink = 0;
    const auto start = Clock::now();
    for (std::size_t i = 0; i < kOps; ++i) {
        sink += map.Get(keys[i % kKeys]).size();
    }
    const double ns = NsPerOp(start, kOps);
    std::printf(
        "[bench] consistenthash Get x%zu (3 nodes, 1000 keys): %.1f ns/op "
        "(sink=%zu)\n",
        kOps, ns, sink);
    GC_CHECK(sink > 0);
}

}  // namespace

int main() {
    std::printf("[gc-primitives] === groupcache primitives tests ===\n");
    std::fflush(stdout);

    TestSingleflightBasics();
    TestSingleflightCoalesce();
    TestSingleflightDistinctKeys();
    TestSingleflightException();
    TestSingleflightForget();
    TestConsistentHashBasics();
    TestConsistentHashDistribution();
    TestConsistentHashMigration();
    BenchmarkSingleflight();
    BenchmarkConsistentHash();

    const int failures = g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::fprintf(stderr, "[gc-primitives] failures=%d\n", failures);
        return 1;
    }
    std::printf("[gc-primitives] all checks passed\n");
    return 0;
}
