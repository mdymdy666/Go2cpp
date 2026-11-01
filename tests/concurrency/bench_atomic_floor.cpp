/**
 * @file bench_atomic_floor.cpp
 * @brief 调度器优化用的**物理下限**实测锚点（不是估算）。
 *
 * ## 为什么需要
 *
 * 「调度器还能不能再快」必须用同一台机器、同一编译选项下的原子指令
 * 成本做分母，而不是拿文档上的 20 cycles 估算。本基准给出两类数字：
 *
 * - **吞吐**：同线程连续 RMW 同一行（流水线化，~4 cycles/次）；
 * - **延迟 / 跨核**：依赖链上的 RMW、两个核交替 RMW 同一行、一次缓存行
 *   单向转让 —— 调度器的提交路径（CAS 返回值决定分支、生产者写/消费者读
 *   同一行）付的是这一类。
 *
 * 另有 `shared_ptr` 拷贝/析构（一次控制块 +1/-1）与 `memcpy` 64B 作对照。
 *
 * 全部取 N 次循环的 **min**（最干净样本），并同时报中位数。
 *
 * 构建/运行：
 *   g++ -O3 -DNDEBUG -std=c++17 -Iinclude tests/concurrency/bench_atomic_floor.cpp \
 *       -lpthread -o /tmp/bench_atomic_floor
 *   taskset -c 8,9 /tmp/bench_atomic_floor
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kInner = 1U << 15;  // 每次采样 32768 次操作
constexpr int kSamples = 41;              // 采样次数（取 min 与中位）

inline void KeepAlive() { asm volatile("" ::: "memory"); }

struct Stat {
    double min_ns;
    double p50_ns;
    double p90_ns;
};

template <typename Fn>
Stat Measure(Fn&& fn) {
    std::vector<double> ns;
    ns.reserve(kSamples);
    for (int s = 0; s < kSamples; ++s) {
        const auto begin = Clock::now();
        fn();
        const auto elapsed = std::chrono::duration<double, std::nano>(
                                 Clock::now() - begin)
                                 .count();
        ns.push_back(elapsed / static_cast<double>(kInner));
    }
    std::sort(ns.begin(), ns.end());
    return Stat{ns.front(), ns[ns.size() / 2], ns[ns.size() * 9 / 10]};
}

void Print(const char* name, const Stat& s) {
    std::printf("%-46s min %6.2f  p50 %6.2f  p90 %6.2f ns/op\n", name,
                s.min_ns, s.p50_ns, s.p90_ns);
}

struct alignas(64) Line {
    std::uint64_t words[8]{};
};

}  // namespace

int main() {
    std::printf("[floor] inner=%zu samples=%d\n", kInner, kSamples);

    std::atomic<std::uint32_t> a32{0};
    std::atomic<std::uint64_t> a64{0};
    std::atomic<bool> flag{false};
    std::uint64_t plain = 0;
    Line line;

    Print("plain add (loop baseline)",
          Measure([&] {
              for (std::size_t i = 0; i < kInner; ++i) {
                  plain += i;
              }
              KeepAlive();
          }));

    // 吞吐：独立 RMW（无依赖）。
    Print("lock xadd 32-bit throughput (independent)",
          Measure([&] {
              for (std::size_t i = 0; i < kInner; ++i) {
                  a32.fetch_add(1, std::memory_order_relaxed);
              }
          }));
    Print("lock xadd 64-bit throughput (independent)",
          Measure([&] {
              for (std::size_t i = 0; i < kInner; ++i) {
                  a64.fetch_add(1, std::memory_order_relaxed);
              }
          }));

    // 延迟：每次 RMW 的结果进入下一次的依赖链（等价 CAS 循环的返回依赖）。
    std::atomic<std::uint64_t> chain{0};
    Print("lock xadd 64-bit dependency chain",
          Measure([&] {
              std::uint64_t acc = 0;
              for (std::size_t i = 0; i < kInner; ++i) {
                  acc = chain.fetch_add(acc + 1, std::memory_order_acq_rel);
              }
              plain += acc;
              chain.store(0, std::memory_order_relaxed);
              KeepAlive();
          }));

    // 延迟：CAS 成功路径（状态迁移的实际形态）。
    Print("lock cmpxchg 64-bit dependency chain",
          Measure([&] {
              std::uint64_t expected = 0;
              std::uint64_t next = 0;
              for (std::size_t i = 0; i < kInner; ++i) {
                  expected = next;
                  if (!chain.compare_exchange_weak(
                          expected, next + 1, std::memory_order_acq_rel,
                          std::memory_order_relaxed)) {
                      next = expected;
                  } else {
                      next += 1;
                  }
              }
              chain.store(0, std::memory_order_relaxed);
              KeepAlive();
          }));

    Print("lock xchg 1-bit (spinlock acquire)",
          Measure([&] {
              for (std::size_t i = 0; i < kInner; ++i) {
                  (void)flag.exchange(true, std::memory_order_acquire);
                  flag.store(false, std::memory_order_release);
              }
          }));

    // 一次 shared_ptr 拷贝 + 析构 = 控制块上两次 RMW。
    {
        auto owner = std::make_shared<Line>();
        Print("shared_ptr copy+drop (2 control RMW)",
              Measure([&] {
                  for (std::size_t i = 0; i < kInner; ++i) {
                      std::shared_ptr<Line> copy = owner;
                      KeepAlive();
                      (void)copy;
                  }
              }));
    }

    // 一次缓存行搬运的参照。
    {
        Line src;
        Line dst;
        src.words[0] = 1;
        Print("memcpy 64B (one cache line)",
              Measure([&] {
                  for (std::size_t i = 0; i < kInner; ++i) {
                      std::memcpy(&dst, &src, sizeof(Line));
                      KeepAlive();
                  }
              }));
    }

    // 跨核：两颗核交替 RMW 同一行（依赖链：每次都要重新拿独占权）。
    {
        std::atomic<std::uint64_t> shared{0};
        std::atomic<bool> go{false};
        std::atomic<bool> done{false};
        constexpr std::size_t kRounds = kInner / 2;
        Print("cross-core lock xadd, same line (2 cores)",
              Measure([&] {
                  shared.store(0, std::memory_order_relaxed);
                  go.store(false, std::memory_order_relaxed);
                  done.store(false, std::memory_order_relaxed);
                  std::thread other([&] {
                      while (!go.load(std::memory_order_acquire)) {
                      }
                      for (std::size_t i = 0; i < kRounds; ++i) {
                          (void)shared.fetch_add(1,
                                                 std::memory_order_acq_rel);
                      }
                      done.store(true, std::memory_order_release);
                  });
                  go.store(true, std::memory_order_release);
                  for (std::size_t i = 0; i < kRounds; ++i) {
                      (void)shared.fetch_add(1, std::memory_order_acq_rel);
                  }
                  while (!done.load(std::memory_order_acquire)) {
                  }
                  other.join();
              }));
    }

    // 跨核：一次缓存行所有权转让的往返（生产者发、消费者收，再回到发方）。
    {
        std::atomic<std::uint32_t> ping{0};
        std::atomic<std::uint32_t> pong{0};
        constexpr std::size_t kRounds = kInner / 4;
        Print("cross-core cache-line handoff round trip",
              Measure([&] {
                  ping.store(0, std::memory_order_relaxed);
                  pong.store(0, std::memory_order_relaxed);
                  std::thread other([&] {
                      for (std::size_t i = 1; i <= kRounds; ++i) {
                          while (ping.load(std::memory_order_acquire) < i) {
                          }
                          pong.store(static_cast<std::uint32_t>(i),
                                     std::memory_order_release);
                      }
                  });
                  for (std::size_t i = 1; i <= kRounds; ++i) {
                      ping.store(static_cast<std::uint32_t>(i),
                                 std::memory_order_release);
                      while (pong.load(std::memory_order_acquire) < i) {
                      }
                  }
                  other.join();
              }));
    }

    std::printf("[floor] plain sink=%lu a32=%u a64=%lu\n",
                static_cast<unsigned long>(plain), a32.load(), a64.load());
    return 0;
}
