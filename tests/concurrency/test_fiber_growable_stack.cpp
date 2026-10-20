/**
 * @file test_fiber_growable_stack.cpp
 * @brief 验证 Fiber 大栈预留 + 2KB 初始提交（归一化 1 页）+ SIGSEGV 按需增长。
 *
 * 覆盖：
 *   1. 默认配置（2048 初始 / 8MiB 预留）下普通 Fiber 与 Scheduler 任务完成；
 *   2. 深递归 256KiB / 1MiB（volatile 局部数组防止优化），不崩且结果正确；
 *   3. 增长后从深帧返回，外层帧局部变量值与地址不变（验证栈不搬家）；
 *   4. 4 个 worker、每个约 8 个 Fiber 并发增长，互不干扰；
 *   5. GO2CPP_GROWABLE_STACK=0 / reserve==initial 退化为固定栈，4KiB 简单
 *      Fiber 可运行；恢复环境后增长路径不误用固定栈缓存。
 *
 * 说明：本测试不 fork。增长失败会直接让测试进程崩溃，由 ctest 判失败；
 * 这正是我们要观察的硬失败，而不是被 try/catch 吞掉。
 */
#include "go2cpp/fiber.hpp"
#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using namespace std::chrono_literals;

volatile std::uint64_t g_sink = 0;

/**
 * @brief 每层消耗约 256 字节 volatile 局部的深递归。
 * @note 必须在递归调用之后继续使用 pad，避免被尾调用优化成循环；
 *       volatile 保证数组真实落在栈上，不会被优化掉。
 */
__attribute__((noinline)) std::uint64_t walk(std::size_t depth) {
    volatile char pad[256];
    pad[0] = static_cast<char>(depth & 0x7F);
    pad[255] = static_cast<char>((depth >> 7) & 0x7F);
    if (depth == 0) {
        return static_cast<std::uint64_t>(
            static_cast<unsigned char>(pad[0]) +
            static_cast<unsigned char>(pad[255]));
    }
    const std::uint64_t below = walk(depth - 1);
    return below + static_cast<std::uint64_t>(
                       static_cast<unsigned char>(pad[0]) +
                       static_cast<unsigned char>(pad[255]));
}

std::uint64_t expected_walk(std::size_t depth) {
    std::uint64_t sum = 0;
    for (std::size_t d = 0; d <= depth; ++d) {
        sum += static_cast<unsigned char>(d & 0x7F);
        sum += static_cast<unsigned char>((d >> 7) & 0x7F);
    }
    return sum;
}

std::size_t page_size() {
    const long raw = ::sysconf(_SC_PAGESIZE);
    return raw > 0 ? static_cast<std::size_t>(raw) : 4096U;
}

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

void restore_env(const char* name, const std::string& old_value, bool had_value) {
    if (had_value) {
        (void)::setenv(name, old_value.c_str(), 1);
    } else {
        (void)::unsetenv(name);
    }
}

void run_deep_case(std::size_t frames, const char* label) {
    const auto expected = expected_walk(frames);
    std::uint64_t result = 0;
    go2cpp::Fiber fiber([&] { result = walk(frames); }, 2048);
    GO2CPP_CHECK(fiber.stack_size() == page_size());
    GO2CPP_CHECK(fiber.resume());
    GO2CPP_CHECK(fiber.state() == go2cpp::FiberState::Completed);
    GO2CPP_CHECK(result == expected);
    g_sink += result;
    std::cout << "  " << label << ": frames=" << frames
              << " checksum=" << result << " (expected " << expected << ")\n";
}

void test_default_config_and_normal_fiber() {
    const std::size_t page = page_size();
    go2cpp::SchedulerConfig config;
    GO2CPP_CHECK(config.fiber_stack_size == 2048U);
    GO2CPP_CHECK(config.fiber_stack_reserve_size == 8U * 1024U * 1024U);
    GO2CPP_CHECK(config.fiber_stack_growable);

    // 2048 请求页对齐后是 4096；普通 Fiber 只使用远小于 4KiB 的栈。
    bool ran = false;
    go2cpp::Fiber normal([&] { ran = true; }, 2048);
    GO2CPP_CHECK(normal.stack_size() == page);
    GO2CPP_CHECK(normal.resume());
    GO2CPP_CHECK(normal.state() == go2cpp::FiberState::Completed);
    GO2CPP_CHECK(ran);

    // 调度器路径：spawn 用 SchedulerConfig 的 2048 填充 TaskOptions。
    go2cpp::SchedulerConfig scheduler_config;
    scheduler_config.processor_count = 2;
    scheduler_config.min_workers = 2;
    scheduler_config.max_workers = 2;
    go2cpp::Scheduler scheduler(scheduler_config);
    scheduler.start();
    std::atomic<int> completed{0};
    auto task = scheduler.spawn([&] { completed.fetch_add(1); });
    GO2CPP_REQUIRE(task->wait_for(5s));
    GO2CPP_CHECK(completed.load() == 1);
    scheduler.shutdown();
}

void test_deep_recursion_256k() {
    // 256 字节局部 + 调用开销，约 1024 层 ≈ 384KiB 实际栈占用。
    run_deep_case(1024, "256KiB-class");
}

void test_deep_recursion_1m() {
    // 约 4096 层 ≈ 1.5MiB 实际栈占用，触发多次 64KiB 增长。
    run_deep_case(4096, "1MiB-class");
}

void test_stack_address_stability() {
    constexpr std::uint64_t kMarker = 0x123456789ABCDEF0ULL;
    std::uintptr_t outer_address = 0;
    std::uint64_t observed = 0;
    std::uint64_t checksum = 0;

    go2cpp::Fiber fiber([&] {
        volatile std::uint64_t marker = kMarker;
        outer_address = reinterpret_cast<std::uintptr_t>(&marker);
        const std::uint64_t below = walk(4096);
        // 深递归已经让 committed 向下增长；外层帧的地址和值必须保持不变。
        observed = marker;
        GO2CPP_CHECK(reinterpret_cast<std::uintptr_t>(&marker) ==
                     outer_address);
        checksum = below;
    }, 2048);

    GO2CPP_CHECK(fiber.resume());
    GO2CPP_CHECK(fiber.state() == go2cpp::FiberState::Completed);
    GO2CPP_CHECK(observed == kMarker);
    GO2CPP_CHECK(checksum == expected_walk(4096));
    g_sink += observed;
    std::cout << "  stack-address-stable: outer=" << observed
              << " checksum=" << checksum << "\n";
}

void test_migrated_growth() {
    const auto expected = expected_walk(1024);
    std::atomic<bool> entered{false};
    std::uint64_t result = 0;
    go2cpp::Fiber migrated([&] {
        volatile int marker = 7;
        entered.store(true, std::memory_order_release);
        GO2CPP_CHECK(go2cpp::Fiber::Suspend(go2cpp::SuspendReason::Yield));
        // 这个 Fiber 的栈在主线程构造，恢复/增长发生在另一个 std::thread；
        // resume_locked 必须为后者补装 sigaltstack，否则深递归会缺少备用栈。
        result = walk(1024);
        GO2CPP_CHECK(marker == 7);
    }, 2048);

    GO2CPP_CHECK(migrated.resume());
    GO2CPP_REQUIRE_EVENTUALLY(
        entered.load(std::memory_order_acquire), 3s);
    std::thread worker([&] { GO2CPP_CHECK(migrated.resume()); });
    GO2CPP_JOIN_WITH_WATCHDOG(worker, 10s);
    GO2CPP_CHECK(migrated.state() == go2cpp::FiberState::Completed);
    GO2CPP_CHECK(result == expected);
    std::cout << "  migrated-growth: checksum=" << result << "\n";
}

void test_concurrent_growth() {
    constexpr int kWorkers = 4;
    constexpr int kTasks = kWorkers * 8;
    const auto expected = expected_walk(1024);
    go2cpp::SchedulerConfig config;
    config.processor_count = kWorkers;
    config.min_workers = kWorkers;
    config.max_workers = kWorkers;
    go2cpp::Scheduler scheduler(config);
    scheduler.start();

    std::atomic<int> completed{0};
    std::vector<std::shared_ptr<go2cpp::Task>> tasks;
    tasks.reserve(kTasks);
    for (int index = 0; index < kTasks; ++index) {
        tasks.push_back(scheduler.spawn([&, expected] {
            // 每个 G 独立预留在 4 个 M 上并发增长；TLS 当前栈必须互不串扰。
            if (walk(1024) == expected) {
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        }));
    }
    for (auto& task : tasks) {
        GO2CPP_REQUIRE(task->wait_for(15s));
    }
    GO2CPP_CHECK(completed.load() == kTasks);
    scheduler.shutdown();
    std::cout << "  concurrent: workers=" << kWorkers << " tasks=" << kTasks
              << " completed=" << completed.load() << "\n";
}

void test_fixed_fallback() {
    const char* const name = "GO2CPP_GROWABLE_STACK";
    const bool had_value = std::getenv(name) != nullptr;
    const std::string old_value = env_or_empty(name);

    // 1) 环境变量关闭：4KiB 固定栈（usable 4096 + 1 页 guard），简单 Fiber 可跑。
    GO2CPP_REQUIRE(::setenv(name, "0", 1) == 0);
    {
        bool ran = false;
        go2cpp::Fiber fixed([&] { ran = true; }, 4096);
        GO2CPP_CHECK(fixed.stack_size() == page_size());
        GO2CPP_CHECK(fixed.resume());
        GO2CPP_CHECK(fixed.state() == go2cpp::FiberState::Completed);
        GO2CPP_CHECK(ran);
    }

    // 2) reserve == 初始提交量：即使 growable=true 也退化为固定栈。
    {
        bool ran = false;
        go2cpp::Fiber fixed([&] { ran = true; }, 4096, 4096, true);
        GO2CPP_CHECK(fixed.resume());
        GO2CPP_CHECK(fixed.state() == go2cpp::FiberState::Completed);
        GO2CPP_CHECK(ran);
    }

    // 3) 恢复增长后，深递归仍可增长；缓存键 (usable, reserve) 保证不会
    //    误用上面固定栈（reserve=0）的池条目。
    GO2CPP_REQUIRE(::setenv(name, "1", 1) == 0);
    std::uint64_t result = 0;
    go2cpp::Fiber grown([&] { result = walk(1024); }, 2048);
    GO2CPP_CHECK(grown.resume());
    GO2CPP_CHECK(grown.state() == go2cpp::FiberState::Completed);
    GO2CPP_CHECK(result == expected_walk(1024));

    restore_env(name, old_value, had_value);
}

}  // namespace

void run_fiber_growable_stack_tests() {
    go2cpp_tests::announce(
        "fiber growable stack: reserve + 2KiB initial commit + SIGSEGV growth");
#if defined(__linux__) && defined(__x86_64__)
    test_default_config_and_normal_fiber();
    test_deep_recursion_256k();
    test_deep_recursion_1m();
    test_stack_address_stability();
    test_migrated_growth();
    test_concurrent_growth();
    test_fixed_fallback();
#else
    // 非 Linux/x86_64 或未启用原生上下文后端时，实现按设计退化为固定
    // 初始栈，没有 SIGSEGV 增长；深递归用例不适用。
    std::clog << "  [跳过] growable stack 需要 Linux/x86_64 原生上下文后端\n";
#endif
}
