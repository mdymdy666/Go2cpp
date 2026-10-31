/**
 * @file test_error_local_thread.cpp
 * @brief `ErrorLocal` 默认线程后端（不 include Fiber 后端）的验收测试。
 *
 * @details 不链接 Fiber 运行时的程序必须保持纯线程语义：
 *  1. 后端类型是 kThread；
 *  2. 值只在本线程可见，线程之间互不影响；
 *  3. Reset 只清当前线程，GetOrCreate 返回同一对象。
 *
 * 本文件刻意不 include `go2cpp/error/error_fiber.hpp`：一旦 include，
 * 静态初始化会安装 Fiber 后端，测的就不是默认线程后端了。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/error/error_local.hpp"

#include <atomic>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<int> g_failures{0};
std::atomic<int> g_checks{0};

void Check(bool condition, const char* expression, int line) {
    g_checks.fetch_add(1);
    if (!condition) {
        g_failures.fetch_add(1);
        std::cerr << __FILE__ << ':' << line << ": CHECK failed: " << expression
                  << '\n';
    }
}

} // namespace

#define CHECK(condition) Check((condition), #condition, __LINE__)

int main() {
    using go2cpp::error::CurrentErrorLocalBackend;
    using go2cpp::error::ErrorLocal;
    using go2cpp::error::ErrorLocalBackendKind;

    CHECK(CurrentErrorLocalBackend() == ErrorLocalBackendKind::kThread);

    ErrorLocal<int> slot;
    int& first = slot.GetOrCreate(7);
    first = 11;
    CHECK(&slot.GetOrCreate() == &first); // 同一线程返回同一对象
    CHECK(*slot.TryGet() == 11);

    std::atomic<int> worker_value{-1};
    std::atomic<bool> worker_absent{false};
    std::thread worker([&] {
        worker_absent.store(slot.TryGet() == nullptr, std::memory_order_release);
        worker_value.store(slot.GetOrCreate(3), std::memory_order_release);
        CHECK(*slot.TryGet() == 3);
        slot.Reset();
        CHECK(slot.TryGet() == nullptr);
    });
    worker.join();

    CHECK(worker_absent.load(std::memory_order_acquire));
    CHECK(worker_value.load(std::memory_order_acquire) == 3);
    CHECK(*slot.TryGet() == 11); // 工作线程的 Reset 不影响本线程
    slot.Reset();
    CHECK(slot.TryGet() == nullptr);

    // 槽位 ID 单调分配、对象销毁后仍不复用（诊断属性）。
    const auto first_id = slot.SlotId();
    ErrorLocal<int> other;
    CHECK(other.SlotId() > first_id);
    CHECK(CurrentErrorLocalBackend() == ErrorLocalBackendKind::kThread);

    const int failures = g_failures.load();
    std::cout << "=== " << (failures == 0 ? "PASSED" : "FAILED") << ": "
              << g_checks.load() << " checks, " << failures << " failures ===\n";
    return failures == 0 ? 0 : 1;
}
