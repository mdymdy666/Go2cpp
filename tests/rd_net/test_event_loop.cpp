#include "go2cpp/rd_net/event_loop.hpp"

/// @file test_event_loop.cpp
/// @brief REQ-007 验收：单 tick 顺序、定时器精度（p50/p99/max）、高负载下定时器、
///        Post 跨线程 1e5 保序、单 tick 每 socket ≤64 包、CancelTimer / Stop / 无泄漏。

#if defined(_WIN32)
// Windows 无 dirent.h / procfs：句柄数走 GetProcessHandleCount，
// 常驻内存走 GetProcessMemoryInfo（psapi）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// NOMINMAX：windows.h 默认定义 min/max 宏，会破坏 std::min / std::max。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#if defined(_MSC_VER)
#pragma comment(lib, "psapi.lib")
#endif
#else
#include <dirent.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

int g_checks = 0;
int g_failures = 0;

#define CHECK_TRUE(cond)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                      \
    } while (0)

struct TimerStats {
    std::vector<long long> errors_us;

    void Sort() { std::sort(errors_us.begin(), errors_us.end()); }

    long long Percentile(double fraction) const {
        if (errors_us.empty()) {
            return 0;
        }
        const auto last = static_cast<double>(errors_us.size() - 1);
        const auto index = static_cast<std::size_t>(last * fraction);
        return errors_us[index];
    }

    long long Max() const { return errors_us.empty() ? 0 : errors_us.back(); }
};

std::unique_ptr<rd::net::EventLoop> MakeLoop() {
    auto created = rd::net::EventLoop::Create();
    if (!created.ok()) {
        std::printf("[FAIL] EventLoop::Create status=%s errno=%d\n",
                    rd::net::IoStatusName(created.status()), created.system_error());
        ++g_failures;
        return nullptr;
    }
    return std::move(created.value());
}

#if defined(_WIN32)
/// @brief 当前进程打开的句柄数（仅用于「前后不变」比较）。失败返回 -1。
///
/// @note Windows 没有 /proc/self/fd（REQ-010）：改用 GetProcessHandleCount
///       取进程全部句柄数。口径比 Linux 的 fd 计数宽（含非 socket 句柄），
///       但本测试只比较前后差值，同样能抓住「每轮泄漏一个句柄」。
int CountOpenFds() {
    DWORD count = 0;
    if (::GetProcessHandleCount(::GetCurrentProcess(), &count) == 0) {
        return -1;
    }
    return static_cast<int>(count);
}

/// @brief 当前进程常驻内存（KB）。失败返回 -1。
/// @note Windows 无 /proc/self/status，改用 GetProcessMemoryInfo().WorkingSetSize。
long ReadVmRssKb() {
    PROCESS_MEMORY_COUNTERS counters{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters,
                               static_cast<DWORD>(sizeof(counters))) == 0) {
        return -1;
    }
    return static_cast<long>(counters.WorkingSetSize / 1024u);
}
#else
long ReadVmRssKb() {
    std::FILE* file = std::fopen("/proc/self/status", "r");
    if (file == nullptr) {
        return -1;
    }
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            if (std::sscanf(line + 6, "%ld", &kb) != 1) {
                kb = -1;
            }
            break;
        }
    }
    std::fclose(file);
    return kb;
}

/// @brief 枚举 /proc/self/fd 统计打开的 fd 数（含目录本身之外的全部条目）。
int CountOpenFds() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    while (dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] != '.') {
            ++count;
        }
    }
    ::closedir(dir);
    return count;
}
#endif

/// @brief 只统计包数的协议（负载测试用）。
struct CountingProtocol : rd::net::Protocol {
    std::size_t received{0};
    void OnDatagram(rd::net::ByteView, const rd::net::Endpoint&) override { ++received; }
};

/// @brief 记录每 tick 包数分布的协议（公平性测试用）。
struct PerTickProtocol : rd::net::Protocol {
    rd::net::EventLoop* loop{nullptr};
    std::size_t count{0};
    std::uint64_t first_tick{0};
    std::uint64_t first_tick_count{0};
    std::uint64_t max_per_tick{0};
    std::uint64_t current_tick{0};
    std::size_t current_count{0};
    std::function<void()> on_packet;

    void OnAttached(rd::net::EventLoop& owner) override { loop = &owner; }

    void OnDatagram(rd::net::ByteView, const rd::net::Endpoint&) override {
        const std::uint64_t tick = loop->tick_count();
        if (tick != current_tick) {
            if (current_tick != 0 && current_count > max_per_tick) {
                max_per_tick = current_count;
            }
            current_tick = tick;
            current_count = 0;
        }
        ++current_count;
        if (first_tick == 0) {
            first_tick = tick;
            first_tick_count = 1;
        } else if (tick == first_tick) {
            ++first_tick_count;
        }
        ++count;
        if (on_packet) {
            on_packet();
        }
    }

    void FlushStats() {
        if (current_count > max_per_tick) {
            max_per_tick = current_count;
        }
    }
};

// ---------------------------------------------------------------------------
// Test 1: 单 tick 回调顺序固定 = Post 任务 → 到期定时器 → IO 事件
// ---------------------------------------------------------------------------
struct OrderProtocol : rd::net::Protocol {
    rd::net::EventLoop* loop{nullptr};
    std::vector<int>* order{nullptr};
    std::uint64_t io_tick{0};

    void OnAttached(rd::net::EventLoop& owner) override { loop = &owner; }

    void OnDatagram(rd::net::ByteView, const rd::net::Endpoint&) override {
        order->push_back(2);
        io_tick = loop->tick_count();
    }
};

void TestTickOrder() {
    std::unique_ptr<rd::net::EventLoop> loop = MakeLoop();
    if (!loop) {
        return;
    }
    auto sock_open = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    auto sender_open = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    CHECK_TRUE(sock_open.ok());
    CHECK_TRUE(sender_open.ok());
    if (!sock_open.ok() || !sender_open.ok()) {
        return;
    }
    rd::net::DatagramSocket sock = std::move(sock_open.value());
    rd::net::DatagramSocket sender = std::move(sender_open.value());

    std::vector<int> order;
    OrderProtocol handler;
    handler.order = &order;
    CHECK_TRUE(loop->AttachDatagram(sock, &handler));

    const std::uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const auto sent =
        sender.SendTo(rd::net::ByteView{payload, sizeof(payload)}, sock.local_endpoint(), nullptr);
    CHECK_TRUE(sent == rd::net::IoStatus::kOk);

    std::uint64_t post_tick = 0;
    std::uint64_t timer_tick = 0;
    loop->Post([&order, &post_tick, &loop] {
        order.push_back(0);
        post_tick = loop->tick_count();
    });
    loop->ScheduleAfter(std::chrono::milliseconds(0), [&order, &timer_tick, &loop] {
        order.push_back(1);
        timer_tick = loop->tick_count();
    });
    loop->ScheduleAfter(std::chrono::milliseconds(60), [&loop] { loop->Stop(); });
    loop->Run();

    CHECK_TRUE(order.size() == 3);
    CHECK_TRUE(order.size() >= 3 && order[0] == 0 && order[1] == 1 && order[2] == 2);
    CHECK_TRUE(post_tick == 1);
    CHECK_TRUE(timer_tick == 1);
    CHECK_TRUE(handler.io_tick == 1);
    std::printf("[1] tick order: size=%zu seq=", order.size());
    for (int value : order) {
        std::printf("%d", value);
    }
    std::printf(" same_tick(post/timer/io)=%llu/%llu/%llu\n",
                static_cast<unsigned long long>(post_tick),
                static_cast<unsigned long long>(timer_tick),
                static_cast<unsigned long long>(handler.io_tick));
}

// ---------------------------------------------------------------------------
// Test 2: 1000 个定时器在 1 秒内到期，误差 < 5ms
// ---------------------------------------------------------------------------
void TestTimerAccuracy() {
    std::unique_ptr<rd::net::EventLoop> loop = MakeLoop();
    if (!loop) {
        return;
    }
    constexpr int kTimerCount = 1000;
    TimerStats stats;
    stats.errors_us.reserve(kTimerCount);
    int fired = 0;
    for (int i = 0; i < kTimerCount; ++i) {
        const auto delay = std::chrono::milliseconds(i);
        const auto expected = Clock::now() + delay;
        loop->ScheduleAfter(delay, [&stats, &fired, &loop, expected] {
            const auto now = Clock::now();
            stats.errors_us.push_back(
                std::chrono::duration_cast<std::chrono::microseconds>(now - expected).count());
            ++fired;
            if (fired == kTimerCount) {
                loop->Stop();
            }
        });
    }
    const auto begin = Clock::now();
    loop->Run();
    const long long elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count();
    stats.Sort();
    std::printf("[2] timers: count=%d fired=%d elapsed=%lldms p50=%lldus p99=%lldus max=%lldus\n",
                kTimerCount, fired, elapsed_ms, stats.Percentile(0.50), stats.Percentile(0.99),
                stats.Max());
    CHECK_TRUE(fired == kTimerCount);
    CHECK_TRUE(stats.Max() < 5000);
    CHECK_TRUE(elapsed_ms >= 990);
    CHECK_TRUE(elapsed_ms <= 1100);
}

// ---------------------------------------------------------------------------
// Test 3: socket A 疯狂收包时，1ms 定时器仍准时
// ---------------------------------------------------------------------------
void TestTimerUnderSocketLoad() {
    std::unique_ptr<rd::net::EventLoop> loop = MakeLoop();
    if (!loop) {
        return;
    }
    rd::net::SocketOptions options;
    options.recv_buffer_bytes = 8u * 1024u * 1024u;
    auto a_open = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0}, options);
    auto b_open = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0}, options);
    CHECK_TRUE(a_open.ok());
    CHECK_TRUE(b_open.ok());
    if (!a_open.ok() || !b_open.ok()) {
        return;
    }
    rd::net::DatagramSocket socket_a = std::move(a_open.value());
    rd::net::DatagramSocket socket_b = std::move(b_open.value());
    CountingProtocol handler_a;
    CountingProtocol handler_b;
    CHECK_TRUE(loop->AttachDatagram(socket_a, &handler_a));
    CHECK_TRUE(loop->AttachDatagram(socket_b, &handler_b));

    const rd::net::Endpoint endpoint_a = socket_a.local_endpoint();
    const rd::net::Endpoint endpoint_b = socket_b.local_endpoint();
    std::atomic<bool> start_sender{false};
    std::atomic<bool> stop_sender{false};
    std::atomic<long long> sent{0};
    std::atomic<bool> sender_failed{false};

    std::thread sender([&] {
        auto opened = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
        if (!opened.ok()) {
            sender_failed.store(true, std::memory_order_release);
            return;
        }
        rd::net::DatagramSocket snd = std::move(opened.value());
        std::uint8_t payload[16];
        std::memset(payload, 0x5A, sizeof(payload));
        while (!start_sender.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        long long number = 0;
        while (!stop_sender.load(std::memory_order_relaxed)) {
            const auto status =
                snd.SendTo(rd::net::ByteView{payload, sizeof(payload)}, endpoint_a, nullptr);
            if (status == rd::net::IoStatus::kWouldBlock) {
                std::this_thread::yield();
                continue;
            }
            if (status != rd::net::IoStatus::kOk) {
                break;
            }
            ++number;
            sent.store(number, std::memory_order_relaxed);
            if ((number & 0xFFF) == 0) {
                snd.SendTo(rd::net::ByteView{payload, sizeof(payload)}, endpoint_b, nullptr);
            }
        }
    });

    constexpr int kRounds = 500;
    TimerStats stats;
    stats.errors_us.reserve(kRounds);
    Clock::time_point expected{};
    std::function<void()> tick_fn;
    tick_fn = [&] {
        const auto now = Clock::now();
        stats.errors_us.push_back(
            std::chrono::duration_cast<std::chrono::microseconds>(now - expected).count());
        if (static_cast<int>(stats.errors_us.size()) >= kRounds) {
            stop_sender.store(true, std::memory_order_relaxed);
            loop->Stop();
            return;
        }
        expected = Clock::now() + std::chrono::milliseconds(1);
        loop->ScheduleAfter(std::chrono::milliseconds(1), tick_fn);
    };
    expected = Clock::now() + std::chrono::milliseconds(1);
    loop->ScheduleAfter(std::chrono::milliseconds(1), tick_fn);

    start_sender.store(true, std::memory_order_release);
    const auto begin = Clock::now();
    loop->Run();
    stop_sender.store(true, std::memory_order_relaxed);
    sender.join();
    const long long elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count();
    stats.Sort();
    std::printf(
        "[3] load: rounds=%d elapsed=%lldms sent=%lld a_rx=%zu b_rx=%zu "
        "p50=%lldus p99=%lldus max=%lldus\n",
        kRounds, elapsed_ms, sent.load(), handler_a.received, handler_b.received,
        stats.Percentile(0.50), stats.Percentile(0.99), stats.Max());
    CHECK_TRUE(!sender_failed.load());
    CHECK_TRUE(static_cast<int>(stats.errors_us.size()) == kRounds);
    CHECK_TRUE(stats.Max() < 5000);
    CHECK_TRUE(sent.load() >= 100000);
}

// ---------------------------------------------------------------------------
// Test 4: Post 从另一线程 1e5 次，全部执行且保序
// ---------------------------------------------------------------------------
void TestPostOrdering() {
    std::unique_ptr<rd::net::EventLoop> loop = MakeLoop();
    if (!loop) {
        return;
    }
    constexpr int kTasks = 100000;
    std::vector<int> executed;
    executed.reserve(kTasks);
    int out_of_order = 0;
    std::thread producer([&] {
        for (int i = 0; i < kTasks; ++i) {
            loop->Post([&executed, &out_of_order, &loop, i, kTasks] {
                if (static_cast<int>(executed.size()) != i) {
                    ++out_of_order;
                }
                executed.push_back(i);
                if (static_cast<int>(executed.size()) == kTasks) {
                    loop->Stop();
                }
            });
        }
    });
    const auto begin = Clock::now();
    loop->Run();
    producer.join();
    const long long elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count();
    bool ordered = true;
    for (std::size_t i = 0; i < executed.size(); ++i) {
        if (executed[i] != static_cast<int>(i)) {
            ordered = false;
            break;
        }
    }
    std::printf("[4] post: tasks=%d executed=%zu out_of_order=%d ordered=%s elapsed=%lldms\n",
                kTasks, executed.size(), out_of_order, ordered ? "yes" : "no", elapsed_ms);
    CHECK_TRUE(static_cast<int>(executed.size()) == kTasks);
    CHECK_TRUE(out_of_order == 0);
    CHECK_TRUE(ordered);
}

// ---------------------------------------------------------------------------
// Test 5: 单 tick 每 socket ≤64 包，两个 socket 同 tick 都被处理（不饿死）
// ---------------------------------------------------------------------------
void TestPerSocketFairness() {
    std::unique_ptr<rd::net::EventLoop> loop = MakeLoop();
    if (!loop) {
        return;
    }
    rd::net::SocketOptions options;
    options.recv_buffer_bytes = 8u * 1024u * 1024u;
    auto a_open = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0}, options);
    auto b_open = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0}, options);
    auto sender_open = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    CHECK_TRUE(a_open.ok());
    CHECK_TRUE(b_open.ok());
    CHECK_TRUE(sender_open.ok());
    if (!a_open.ok() || !b_open.ok() || !sender_open.ok()) {
        return;
    }
    rd::net::DatagramSocket socket_a = std::move(a_open.value());
    rd::net::DatagramSocket socket_b = std::move(b_open.value());
    rd::net::DatagramSocket sender = std::move(sender_open.value());

    PerTickProtocol handler_a;
    PerTickProtocol handler_b;
    CHECK_TRUE(loop->AttachDatagram(socket_a, &handler_a));
    CHECK_TRUE(loop->AttachDatagram(socket_b, &handler_b));

    std::uint8_t payload[16];
    std::memset(payload, 0x33, sizeof(payload));
    const rd::net::Endpoint endpoint_a = socket_a.local_endpoint();
    std::size_t sent_a = 0;
    for (int i = 0; i < 1000; ++i) {
        for (int attempt = 0; attempt < 1000; ++attempt) {
            const auto status = sender.SendTo(rd::net::ByteView{payload, sizeof(payload)},
                                              endpoint_a, nullptr);
            if (status == rd::net::IoStatus::kOk) {
                ++sent_a;
                break;
            }
            if (status != rd::net::IoStatus::kWouldBlock) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    const auto b_status = sender.SendTo(rd::net::ByteView{payload, sizeof(payload)},
                                        socket_b.local_endpoint(), nullptr);
    CHECK_TRUE(b_status == rd::net::IoStatus::kOk);

    bool timed_out = false;
    handler_a.on_packet = [&] {
        if (handler_a.count >= sent_a) {
            loop->Stop();
        }
    };
    loop->ScheduleAfter(std::chrono::milliseconds(80), [&] {
        timed_out = true;
        loop->Stop();
    });
    loop->Run();
    handler_a.FlushStats();
    handler_b.FlushStats();

    std::printf(
        "[5] fairness: sent_a=%zu a_rx=%zu a_first_tick=%llu a_first_tick_pkts=%llu "
        "a_max_per_tick=%llu b_rx=%zu b_first_tick=%llu same_tick=%s timed_out=%s\n",
        sent_a, handler_a.count, static_cast<unsigned long long>(handler_a.first_tick),
        static_cast<unsigned long long>(handler_a.first_tick_count),
        static_cast<unsigned long long>(handler_a.max_per_tick), handler_b.count,
        static_cast<unsigned long long>(handler_b.first_tick),
        handler_b.first_tick == handler_a.first_tick ? "yes" : "no", timed_out ? "yes" : "no");
    CHECK_TRUE(handler_a.max_per_tick <= 64);
    CHECK_TRUE(handler_a.first_tick_count == 64);
    CHECK_TRUE(handler_a.count >= 64);
    CHECK_TRUE(handler_b.count == 1);
    CHECK_TRUE(handler_b.first_tick == handler_a.first_tick);
}

// ---------------------------------------------------------------------------
// Test 6: CancelTimer 不再触发；Stop() 后 Run 返回；200 轮无 fd / RSS 泄漏
// ---------------------------------------------------------------------------
void TestCancelStopAndLeak() {
    {
        std::unique_ptr<rd::net::EventLoop> loop = MakeLoop();
        if (!loop) {
            return;
        }
        constexpr int kTimers = 100;
        std::vector<int> fired;
        std::vector<std::uint64_t> ids;
        ids.reserve(kTimers);
        for (int i = 0; i < kTimers; ++i) {
            ids.push_back(loop->ScheduleAfter(std::chrono::milliseconds(5),
                                              [&fired, i] { fired.push_back(i); }));
        }
        int cancelled = 0;
        for (int i = 0; i < kTimers; i += 2) {
            if (loop->CancelTimer(ids[static_cast<std::size_t>(i)])) {
                ++cancelled;
            }
        }
        loop->ScheduleAfter(std::chrono::milliseconds(30), [&loop] { loop->Stop(); });
        loop->Run();

        int odd_fired = 0;
        int even_fired = 0;
        for (int value : fired) {
            if ((value % 2) == 0) {
                ++even_fired;
            } else {
                ++odd_fired;
            }
        }
        std::printf("[6a] cancel: scheduled=%d cancelled=%d fired=%zu odd=%d even=%d\n", kTimers,
                    cancelled, fired.size(), odd_fired, even_fired);
        CHECK_TRUE(cancelled == 50);
        CHECK_TRUE(even_fired == 0);
        CHECK_TRUE(odd_fired == 50);
    }

    {
        std::unique_ptr<rd::net::EventLoop> loop = MakeLoop();
        if (!loop) {
            return;
        }
        bool late_fired = false;
        loop->ScheduleAfter(std::chrono::milliseconds(1000), [&late_fired] { late_fired = true; });
        std::thread stopper([&loop] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            loop->Stop();
        });
        const auto begin = Clock::now();
        loop->Run();
        const long long run_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count();
        stopper.join();
        std::printf("[6b] stop: run=%lldms late_timer_fired=%s\n", run_ms,
                    late_fired ? "yes" : "no");
        CHECK_TRUE(!late_fired);
        CHECK_TRUE(run_ms >= 15);
        CHECK_TRUE(run_ms <= 300);
    }

    const int fds_before = CountOpenFds();
    const long rss_before = ReadVmRssKb();
    int rounds = 0;
    for (int round = 0; round < 200; ++round) {
        std::unique_ptr<rd::net::EventLoop> loop = MakeLoop();
        if (!loop) {
            break;
        }
        for (int i = 0; i < 8; ++i) {
            const auto delay = std::chrono::milliseconds(i % 3);
            loop->ScheduleAfter(delay, [&loop, i] {
                if (i == 7) {
                    loop->Stop();
                }
            });
        }
        loop->Run();
        ++rounds;
    }
    const long rss_after = ReadVmRssKb();
    const int fds_after = CountOpenFds();
    const long rss_delta = rss_after - rss_before;
    std::printf("[6c] leak: rounds=%d fd=%d->%d rss=%ldKB->%ldKB delta=%ldKB\n", rounds, fds_before,
                fds_after, rss_before, rss_after, rss_delta);
    CHECK_TRUE(rounds == 200);
    CHECK_TRUE(fds_after - fds_before <= 1);
#if !defined(__SANITIZE_ADDRESS__)
    // ASAN 自带 quarantine 与影子内存，RSS 不可比；泄漏交给 LeakSanitizer 判定。
    CHECK_TRUE(rss_delta <= 4096);
#endif
}

}  // namespace

int main() {
    std::printf("== REQ-007 event loop tests ==\n");
    TestTickOrder();
    TestTimerAccuracy();
    TestTimerUnderSocketLoad();
    TestPostOrdering();
    TestPerSocketFairness();
    TestCancelStopAndLeak();
    std::printf("== checks=%d failures=%d ==\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
