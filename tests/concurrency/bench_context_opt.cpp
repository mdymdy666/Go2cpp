/**
 * @file bench_context_opt.cpp
 * @brief Context/DoneSignal 成本拆解基准（独立可执行，不进入 ctest）。
 *
 * 覆盖任务要求的三组数据：
 *  1. Context 创建：Background / WithCancel / WithTimeout / WithValue；
 *  2. 热路径检查：IsDone（未取消/已取消）/ Done().IsDone / Deadline / Err /
 *     ContextPtr 拷贝 / Now；
 *  3. 取消传播与唤醒：回调注册/注销、取消链（深度 1/64/1024）、
 *     已取消 Wait 快路径、跨线程 Wait 唤醒端到端；
 *  4. Channel Send/Recv 带 Context 与不带 Context 的成本差。
 *
 * 构建（不修改 CMake）：
 *   c++ -O3 -DNDEBUG -std=c++17 -Iinclude tests/concurrency/bench_context_opt.cpp \
 *       -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
 *       -lgo2cpp_channel -lgo2cpp_sync -lgo2cpp_future -lgo2cpp_context \
 *       -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_error -lgo2cpp_control_flow \
 *       -lgo2cpp_log -lgo2cpp_debug_event -lgo2cpp_debug -lpthread -o /tmp/bench_context
 */
#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/concurrency/future.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace go2cpp;
using Clock = std::chrono::steady_clock;

volatile std::uint64_t g_sink = 0;

inline void Sink(std::uint64_t value) noexcept {
    g_sink = g_sink + value;
}

inline std::uint64_t NowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

/// @brief 运行 f 若干次并返回 ns/op；f 接收迭代序号，便于避免常量折叠。
template <typename F>
double BenchNs(std::uint64_t iterations, F&& f) {
    const std::uint64_t warmup = iterations / 10 + 1;
    for (std::uint64_t i = 0; i < warmup; ++i) {
        f(i);
    }
    const auto start = NowNs();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        f(i);
    }
    const auto elapsed = NowNs() - start;
    return static_cast<double>(elapsed) / static_cast<double>(iterations);
}

void Print(const char* name, double ns) {
    std::printf("%-46s %10.2f ns/op\n", name, ns);
}

// ---------------------------------------------------------------------------
// 1. 创建成本
// ---------------------------------------------------------------------------
void BenchConstruction() {
    const auto root = Background();
    Print("Background() shared_ptr copy", BenchNs(2000000, [&](std::uint64_t) {
              auto value = Background();
              Sink(reinterpret_cast<std::uintptr_t>(value.get()));
          }));

    Print("WithCancel + cancel + destroy", BenchNs(300000, [&](std::uint64_t) {
              auto pair = WithCancel(root);
              pair.second();
              Sink(reinterpret_cast<std::uintptr_t>(pair.first.get()));
          }));

    Print("WithCancel create + destroy (no cancel)", BenchNs(300000, [&](std::uint64_t) {
              auto pair = WithCancel(root);
              Sink(reinterpret_cast<std::uintptr_t>(pair.first.get()));
          }));

    Print("WithTimeout(1h) + cancel + destroy", BenchNs(100000, [&](std::uint64_t) {
              auto pair = WithTimeout(root, std::chrono::hours(1));
              pair.second();
              Sink(reinterpret_cast<std::uintptr_t>(pair.first.get()));
          }));

    Print("WithDeadline(far) + cancel + destroy", BenchNs(100000, [&](std::uint64_t) {
              auto pair = WithDeadline(root, Clock::now() + std::chrono::hours(1));
              pair.second();
              Sink(reinterpret_cast<std::uintptr_t>(pair.first.get()));
          }));

    ContextKey<int> key("bench-key");
    Print("WithValue(typed) + destroy", BenchNs(200000, [&](std::uint64_t i) {
              auto value = WithValue(root, key, static_cast<int>(i));
              Sink(reinterpret_cast<std::uintptr_t>(value.get()));
          }));
}

// ---------------------------------------------------------------------------
// 2. 检查成本
// ---------------------------------------------------------------------------
void BenchChecks() {
    const auto root = Background();
    const auto live = WithCancel(root).first;
    auto canceled_pair = WithCancel(root);
    const auto canceled = canceled_pair.first;
    canceled_pair.second();
    const auto deadline = WithTimeout(root, std::chrono::hours(1)).first;
    const auto no_deadline = WithCancel(root).first;

    Print("IsDone() live (false)", BenchNs(50000000, [&](std::uint64_t) {
              Sink(live->IsDone() ? 1U : 0U);
          }));

    Print("IsDone() canceled (true)", BenchNs(50000000, [&](std::uint64_t) {
              Sink(canceled->IsDone() ? 1U : 0U);
          }));

    Print("Done().IsDone() live", BenchNs(50000000, [&](std::uint64_t) {
              Sink(live->Done().IsDone() ? 1U : 0U);
          }));

    Print("ContextPtr copy/destroy", BenchNs(50000000, [&](std::uint64_t) {
              ContextPtr copy = live;
              Sink(reinterpret_cast<std::uintptr_t>(copy.get()));
          }));

    Print("Deadline() no deadline", BenchNs(20000000, [&](std::uint64_t) {
              const auto value = no_deadline->Deadline();
              Sink(value.has_value() ? 1U : 0U);
          }));

    Print("Deadline() with deadline", BenchNs(20000000, [&](std::uint64_t) {
              const auto value = deadline->Deadline();
              Sink(value.has_value() ? 1U : 0U);
          }));

    Print("HasDeadline()", BenchNs(20000000, [&](std::uint64_t) {
              Sink(deadline->HasDeadline() ? 1U : 0U);
          }));

    Print("Err() canceled", BenchNs(20000000, [&](std::uint64_t) {
              Sink(canceled->Err() ? 1U : 0U);
          }));

    Print("steady_clock::now() baseline", BenchNs(20000000, [&](std::uint64_t) {
              Sink(static_cast<std::uint64_t>(Clock::now().time_since_epoch().count()));
          }));

    Print("Context::Now()", BenchNs(20000000, [&](std::uint64_t) {
              Sink(static_cast<std::uint64_t>(Context::Now().time_since_epoch().count()));
          }));

    // 空 ContextPtr 快路径：Channel 每次 Send/Recv 的入口判断。
    const ContextPtr empty;
    Print("null ContextPtr check", BenchNs(50000000, [&](std::uint64_t) {
              Sink((!empty || empty->IsDone()) ? 1U : 0U);
          }));
}

// ---------------------------------------------------------------------------
// 3. 回调与取消传播
// ---------------------------------------------------------------------------
void BenchCallbacks() {
    const auto root = Background();
    const auto live = WithCancel(root).first;

    Print("Done().AddCallback + RemoveCallback", BenchNs(300000, [&](std::uint64_t) {
              const auto id = live->Done().AddCallback([] {});
              live->Done().RemoveCallback(id);
              Sink(id);
          }));

    Print("Done().AddCallback only (leak until cancel)", BenchNs(100000, [&](std::uint64_t) {
              const auto id = live->Done().AddCallback([] {});
              Sink(id);
          }));

    for (int depth : {1, 64, 1024}) {
        const std::string label = "cancel chain depth " + std::to_string(depth);
        const double ns = BenchNs(depth >= 1024 ? 2000 : 20000, [&](std::uint64_t) {
            auto head_pair = WithCancel(root);
            std::vector<ContextPtr> nodes;
            nodes.reserve(static_cast<std::size_t>(depth));
            auto leaf = head_pair.first;
            for (int i = 1; i < depth; ++i) {
                auto child = WithCancel(leaf);
                nodes.push_back(leaf);
                leaf = child.first;
            }
            const auto start = NowNs();
            head_pair.second();
            const auto elapsed = NowNs() - start;
            Sink(elapsed);
        });
        Print(label.c_str(), ns);
    }

    for (int callbacks : {1, 64}) {
        const std::string label =
            "cancel with " + std::to_string(callbacks) + " Done callbacks";
        const double ns = BenchNs(20000, [&](std::uint64_t) {
            auto pair = WithCancel(root);
            std::vector<DoneSignal::CallbackId> ids;
            ids.reserve(static_cast<std::size_t>(callbacks));
            for (int i = 0; i < callbacks; ++i) {
                ids.push_back(pair.first->Done().AddCallback([] {}));
            }
            const auto start = NowNs();
            pair.second();
            const auto elapsed = NowNs() - start;
            Sink(elapsed + ids.size());
        });
        Print(label.c_str(), ns);
    }

    auto done_pair = WithCancel(root);
    done_pair.second();
    const auto done = done_pair.first;
    Print("Done().Wait() already canceled", BenchNs(5000000, [&](std::uint64_t) {
              done->Done().Wait();
              Sink(1);
          }));
    Print("Done().WaitFor(1ms) already canceled", BenchNs(5000000, [&](std::uint64_t) {
              Sink(done->Done().WaitFor(std::chrono::milliseconds(1)) ? 1U : 0U);
          }));
}

/**
 * @brief 跨线程 Wait 唤醒端到端延迟。
 *
 * 一个常驻等待线程每轮等待新 Context；主线程在确认「已进入等待」后取消并
 * 等待确认。时间包含两次线程间可见性传递，因此是**端到端**唤醒延迟，
 * 比纯 Context 内部开销大，报告里单独标注。
 */
void BenchCrossThreadWakeup() {
    constexpr int kIterations = 3000;
    std::atomic<const ContextPtr*> slot{nullptr};
    std::atomic<std::uint64_t> request{0};
    std::atomic<std::uint64_t> acked{0};
    std::atomic<bool> stop{false};

    std::thread waiter([&] {
        std::uint64_t seen = 0;
        while (!stop.load(std::memory_order_acquire)) {
            const std::uint64_t current = request.load(std::memory_order_acquire);
            const ContextPtr* target = slot.load(std::memory_order_acquire);
            if (current == seen || target == nullptr) {
                std::this_thread::yield();
                continue;
            }
            seen = current;
            (*target)->Done().Wait();
            acked.store(current, std::memory_order_release);
        }
    });

    std::uint64_t total = 0;
    for (int i = 0; i < kIterations; ++i) {
        auto pair = WithCancel(Background());
        const ContextPtr* target = &pair.first;
        slot.store(target, std::memory_order_release);
        const std::uint64_t epoch =
            request.fetch_add(1, std::memory_order_acq_rel) + 1;
        // 等等待线程拿到本轮 Context 并进入 Wait 之前，不可避免有调度抖动；
        // 用固定 20us 让等待线程先跑到 Wait 上，主线程再取消。
        const auto armed_deadline = Clock::now() + std::chrono::milliseconds(5);
        while (slot.load(std::memory_order_acquire) != nullptr &&
               Clock::now() < armed_deadline) {
            // 等待线程不会清空 slot；这里只做极短预热自旋。
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(20));
        const auto start = NowNs();
        pair.second();
        while (acked.load(std::memory_order_acquire) != epoch) {
            std::this_thread::yield();
        }
        total += NowNs() - start;
        slot.store(nullptr, std::memory_order_release);
    }
    stop.store(true, std::memory_order_release);
    waiter.join();
    std::printf("%-46s %10.2f us/op\n",
                "Wait cancel wakeup cross-thread (e2e)", static_cast<double>(total) / kIterations / 1000.0);
}

// ---------------------------------------------------------------------------
// 4. Channel 成本拆解
// ---------------------------------------------------------------------------
void BenchChannel() {
    Channel<int> buffered(8);
    const auto root = Background();
    const auto live = WithCancel(root).first;
    const ContextPtr none;

    Print("Channel cap8 Send+Recv no context", BenchNs(2000000, [&](std::uint64_t i) {
              (void)buffered.Send(static_cast<int>(i));
              auto result = buffered.Recv();
              Sink(result.value.has_value() ? 1U : 0U);
          }));

    Print("Channel cap8 Send+Recv live context", BenchNs(2000000, [&](std::uint64_t i) {
              (void)buffered.Send(static_cast<int>(i), live);
              auto result = buffered.Recv(live);
              Sink(result.value.has_value() ? 1U : 0U);
          }));

    Print("Channel cap8 TrySend+TryRecv", BenchNs(5000000, [&](std::uint64_t i) {
              (void)buffered.TrySend(static_cast<int>(i));
              auto result = buffered.TryRecv();
              Sink(result.value.has_value() ? 1U : 0U);
          }));

    // 无缓冲 ping-pong：两个线程各持一端，每轮一次 Send + Recv。
    // 带 context 时阻塞路径还会注册/注销一个 Done 回调。
    auto ping = MakeChannel<int>(0);
    auto pong = MakeChannel<int>(0);
    auto run_pingpong = [&](const ContextPtr& context) {
        std::atomic<bool> ready{false};
        std::thread peer([&] {
            ready.store(true, std::memory_order_release);
            for (;;) {
                auto value = ping->Recv(context);
                if (!value.value.has_value()) {
                    return;
                }
                if (*value.value < 0) {
                    return;
                }
                (void)pong->Send(*value.value, context);
            }
        });
        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        constexpr int kRounds = 50000;
        const auto start = NowNs();
        for (int i = 0; i < kRounds; ++i) {
            (void)ping->Send(i, context);
            auto reply = pong->Recv(context);
            Sink(reply.value.has_value() ? 1U : 0U);
        }
        const auto elapsed = NowNs() - start;
        (void)ping->Send(-1, context);
        peer.join();
        return static_cast<double>(elapsed) / kRounds;
    };

    Print("Channel unbuffered ping-pong no context", run_pingpong(none));
    Print("Channel unbuffered ping-pong live context", run_pingpong(live));
}

}  // namespace

int main() {
    std::printf("== Context 性能基准（Release, %u threads）==\n",
                std::thread::hardware_concurrency());
    BenchConstruction();
    BenchChecks();
    BenchCallbacks();
    BenchChannel();
    BenchCrossThreadWakeup();
    std::printf("sink=%llu\n", static_cast<unsigned long long>(g_sink));
    return 0;
}
