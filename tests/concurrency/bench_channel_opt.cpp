/**
 * @file bench_channel_opt.cpp
 * @brief Channel 单生产者/单消费者吞吐、单线程快路径与 ping-pong 延迟基准。
 *
 * ## 为什么独立于 benchmarks/
 * 本文件由 channel 性能优化任务引入，约束只允许改动
 * `include/go2cpp/concurrency/channel.hpp`、`src/concurrency/channel*` 与
 * `tests/concurrency/**`，因此不能把基准注册进 `benchmarks/` 与 CMake。
 * 采用手动编译（命令见下），保证同一份代码可用于「优化前 / 优化后」对比。
 *
 * ## 编译与运行
 * ```
 * g++ -std=c++20 -O3 -DNDEBUG -Iinclude tests/concurrency/bench_channel_opt.cpp \
 *     -L.build-bento-wsl \
 *     -lgo2cpp_channel -lgo2cpp_context -lgo2cpp_scheduler -lgo2cpp_fiber \
 *     -lgo2cpp_error -lgo2cpp_log -pthread \
 *     -o .build-bento-wsl/bench_channel_opt
 * LD_LIBRARY_PATH=$PWD/.build-bento-wsl .build-bento-wsl/bench_channel_opt [total] [reps] [mode]
 * ```
 *
 * `mode`：`all`（默认）| `micro` | `spsc` | `pingpong` | `empty`。
 *
 * ## 测例
 * 0. 单线程快路径：同一线程 Send/Recv 交替（缓冲恒非空/非满）——隔离出
 *    「锁 + 环形缓冲 + 返回值构造」的固定成本，不含跨核唤醒；
 * 1. SPSC 吞吐：1 生产者 + 1 消费者传 total 个 int，容量 0/64/1024；
 * 2. ping-pong 单条延迟：两个通道来回，容量 0/1/64，报单程延迟（往返/2）；
 * 3. 空通道探测：TryRecv / TrySend 在空/满通道上的单次成本。
 *
 * 所有测例都逐条校验序号：任何丢失/重复/乱序都会让基准以非零码退出。
 */

#include "go2cpp/channel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// 防止编译器把结果优化掉。
inline void Escape(std::uint64_t value) {
  static volatile std::uint64_t sink = 0;
  sink = sink + value;
}

/// 自旋起跑门，让计时不含线程创建开销。
class StartGate {
 public:
  void Wait() {
    while (!m_open.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }
  void Open() { m_open.store(true, std::memory_order_release); }

 private:
  std::atomic<bool> m_open{false};
};

struct Samples {
  std::vector<double> values;

  void Add(double value) { values.push_back(value); }
  double Min() const { return *std::min_element(values.begin(), values.end()); }
  double Max() const { return *std::max_element(values.begin(), values.end()); }
  double Median() const {
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    return sorted[sorted.size() / 2];
  }
};

void PrintRow(const std::string& label, const Samples& samples) {
  std::printf("  %-42s min %9.1f  median %9.1f  max %9.1f  ns/op\n",
              label.c_str(), samples.Min(), samples.Median(), samples.Max());
}

/// 1 生产者 + 1 消费者，返回每操作纳秒数；order_violations 累加语义错误数。
double Spsc(std::size_t capacity, std::uint64_t total,
            std::atomic<std::uint64_t>* order_violations) {
  auto channel = go2cpp::MakeChannel<std::uint64_t>(capacity);
  StartGate gate;
  std::atomic<std::uint64_t> checksum{0};
  std::atomic<std::uint64_t> violations{0};
  std::thread producer([&] {
    gate.Wait();
    for (std::uint64_t i = 0; i < total; ++i) {
      channel->Send(i);
    }
  });
  std::thread consumer([&] {
    gate.Wait();
    std::uint64_t local_sum = 0;
    std::uint64_t local_violations = 0;
    for (std::uint64_t i = 0; i < total; ++i) {
      const auto result = channel->Recv();
      if (!result.ok) {
        ++local_violations;
        continue;
      }
      const std::uint64_t value = *result.value;
      if (value != i) {
        ++local_violations;
      }
      local_sum += value;
    }
    checksum.store(local_sum, std::memory_order_relaxed);
    violations.store(local_violations, std::memory_order_relaxed);
  });
  const auto started = Clock::now();
  gate.Open();
  producer.join();
  consumer.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - started);
  Escape(checksum.load(std::memory_order_relaxed));
  order_violations->fetch_add(violations.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
  return static_cast<double>(elapsed.count()) / static_cast<double>(total);
}

/// 两个通道来回 ping-pong，返回**单程**延迟（往返 / 2）。
double PingPong(std::size_t capacity, std::uint64_t rounds,
                std::atomic<std::uint64_t>* order_violations) {
  auto to_b = go2cpp::MakeChannel<std::uint64_t>(capacity);
  auto to_a = go2cpp::MakeChannel<std::uint64_t>(capacity);
  std::atomic<std::uint64_t> violations{0};
  std::thread responder([&] {
    std::uint64_t local_violations = 0;
    for (std::uint64_t i = 0; i < rounds; ++i) {
      const auto got = to_b->Recv();
      if (!got.ok || *got.value != i) {
        ++local_violations;
      }
      to_a->Send(i);
    }
    violations.store(local_violations, std::memory_order_relaxed);
  });
  std::uint64_t local_violations = 0;
  const auto started = Clock::now();
  for (std::uint64_t i = 0; i < rounds; ++i) {
    to_b->Send(i);
    const auto got = to_a->Recv();
    if (!got.ok || *got.value != i) {
      ++local_violations;
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - started);
  responder.join();
  order_violations->fetch_add(local_violations +
                                  violations.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
  return static_cast<double>(elapsed.count()) /
         (2.0 * static_cast<double>(rounds));
}

/// 单线程 Send/Recv 交替：缓冲恒非满（先填一个种子），隔离固定成本。
double MicroPair(std::size_t capacity, std::uint64_t total,
                 std::atomic<std::uint64_t>* order_violations) {
  auto channel = go2cpp::MakeChannel<std::uint64_t>(capacity);
  channel->Send(0);
  std::uint64_t violations = 0;
  std::uint64_t checksum = 0;
  const auto started = Clock::now();
  for (std::uint64_t i = 1; i <= total; ++i) {
    channel->Send(i);
    const auto got = channel->Recv();
    if (!got.ok || *got.value != i - 1) {
      ++violations;
    } else {
      checksum += *got.value;
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - started);
  Escape(checksum);
  order_violations->fetch_add(violations, std::memory_order_relaxed);
  // 每次迭代 1 次 Send + 1 次 Recv。
  return static_cast<double>(elapsed.count()) /
         (2.0 * static_cast<double>(total));
}

/// 纯快路径 SPSC：双方只用 TrySend/TryRecv 轮询重试，**永不 park**。
/// 用来把「跨核锁成本」与「park/futex 成本」分离：如果它与普通 SPSC 接近，
/// 说明瓶颈在锁的 cacheline 传递；如果远低于普通 SPSC，说明瓶颈在 park。
double TrySpsc(std::size_t capacity, std::uint64_t total,
               std::atomic<std::uint64_t>* order_violations) {
  auto channel = go2cpp::MakeChannel<std::uint64_t>(capacity);
  StartGate gate;
  std::atomic<std::uint64_t> violations{0};
  std::atomic<std::uint64_t> checksum{0};
  std::thread producer([&] {
    gate.Wait();
    for (std::uint64_t i = 0; i < total; ++i) {
      while (!channel->TrySend(i).Ok()) {
#if defined(__x86_64__)
        __builtin_ia32_pause();
#endif
      }
    }
  });
  std::thread consumer([&] {
    gate.Wait();
    std::uint64_t local_sum = 0;
    std::uint64_t local_violations = 0;
    for (std::uint64_t i = 0; i < total; ++i) {
      for (;;) {
        auto result = channel->TryRecv();
        if (result.ok) {
          if (*result.value != i) {
            ++local_violations;
          }
          local_sum += *result.value;
          break;
        }
#if defined(__x86_64__)
        __builtin_ia32_pause();
#endif
      }
    }
    checksum.store(local_sum, std::memory_order_relaxed);
    violations.store(local_violations, std::memory_order_relaxed);
  });
  const auto started = Clock::now();
  gate.Open();
  producer.join();
  consumer.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - started);
  Escape(checksum.load(std::memory_order_relaxed));
  order_violations->fetch_add(violations.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
  return static_cast<double>(elapsed.count()) / static_cast<double>(total);
}

#if !defined(GO2CPP_BENCH_NO_BATCH)
/// 批量 API 的 SPSC 吞吐：生产者/消费者各以 batch 条为一批。
double BatchSpsc(std::size_t capacity, std::size_t batch, std::uint64_t total,
                 std::atomic<std::uint64_t>* order_violations) {
  auto channel = go2cpp::MakeChannel<std::uint64_t>(capacity);
  StartGate gate;
  std::atomic<std::uint64_t> violations{0};
  std::atomic<std::uint64_t> checksum{0};
  std::thread producer([&] {
    gate.Wait();
    std::vector<std::uint64_t> values(batch);
    std::uint64_t next = 0;
    while (next < total) {
      const std::size_t n = static_cast<std::size_t>(
          std::min<std::uint64_t>(batch, total - next));
      for (std::size_t k = 0; k < n; ++k) {
        values[k] = next + k;
      }
      std::size_t offset = 0;
      while (offset < n) {
        const std::size_t sent =
            channel->TrySendBatch(values.data() + offset, n - offset);
        if (sent == 0) {
#if defined(__x86_64__)
          __builtin_ia32_pause();
#endif
          continue;
        }
        offset += sent;
      }
      next += n;
    }
  });
  std::thread consumer([&] {
    gate.Wait();
    std::vector<std::uint64_t> buffer(batch);
    std::uint64_t expect = 0;
    std::uint64_t local_sum = 0;
    std::uint64_t local_violations = 0;
    while (expect < total) {
      const std::size_t got =
          channel->TryRecvBatch(buffer.data(), buffer.size());
      if (got == 0) {
#if defined(__x86_64__)
        __builtin_ia32_pause();
#endif
        continue;
      }
      for (std::size_t k = 0; k < got; ++k) {
        if (buffer[k] != expect) {
          ++local_violations;
        }
        local_sum += buffer[k];
        ++expect;
      }
    }
    checksum.store(local_sum, std::memory_order_relaxed);
    violations.store(local_violations, std::memory_order_relaxed);
  });
  const auto started = Clock::now();
  gate.Open();
  producer.join();
  consumer.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - started);
  Escape(checksum.load(std::memory_order_relaxed));
  order_violations->fetch_add(violations.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
  return static_cast<double>(elapsed.count()) / static_cast<double>(total);
}

#endif  // GO2CPP_BENCH_NO_BATCH

/// 空通道上的非阻塞探测成本。
double EmptyTryRecv(std::size_t capacity, std::uint64_t iterations,
                    std::atomic<std::uint64_t>* order_violations) {
  auto channel = go2cpp::MakeChannel<std::uint64_t>(capacity);
  std::uint64_t hits = 0;
  const auto started = Clock::now();
  for (std::uint64_t i = 0; i < iterations; ++i) {
    if (channel->TryRecv().ok) {
      ++hits;
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - started);
  Escape(hits);
  order_violations->fetch_add(hits, std::memory_order_relaxed);
  return static_cast<double>(elapsed.count()) /
         static_cast<double>(iterations);
}

/// 已满缓冲上的非阻塞发送成本。
double FullTrySend(std::size_t capacity, std::uint64_t iterations,
                   std::atomic<std::uint64_t>* order_violations) {
  auto channel = go2cpp::MakeChannel<std::uint64_t>(capacity);
  for (std::size_t i = 0; i < capacity; ++i) {
    channel->Send(0);
  }
  std::uint64_t hits = 0;
  const auto started = Clock::now();
  for (std::uint64_t i = 0; i < iterations; ++i) {
    if (channel->TrySend(i).Ok()) {
      ++hits;
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - started);
  Escape(hits);
  order_violations->fetch_add(hits, std::memory_order_relaxed);
  return static_cast<double>(elapsed.count()) /
         static_cast<double>(iterations);
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t total = 1000000;
  int reps = 3;
  std::string mode = "all";
  if (argc > 1) {
    const auto parsed = std::strtoull(argv[1], nullptr, 10);
    if (parsed > 0) total = parsed;
  }
  if (argc > 2) {
    const int parsed = std::atoi(argv[2]);
    if (parsed > 0) reps = parsed;
  }
  if (argc > 3) {
    mode = argv[3];
  }
  const bool all = mode == "all";
  const std::uint64_t ping_rounds = std::min<std::uint64_t>(total, 200000);

  std::printf("=== Channel SPSC 性能基准 ===\n");
  std::printf("参数: total=%llu reps=%d ping-pong rounds=%llu mode=%s\n\n",
              static_cast<unsigned long long>(total), reps,
              static_cast<unsigned long long>(ping_rounds), mode.c_str());

  std::atomic<std::uint64_t> violations{0};

  if (all || mode == "micro") {
    std::printf("[0] 单线程 Send/Recv 快路径（无跨核唤醒）\n");
    for (const std::size_t capacity : {std::size_t{64}, std::size_t{1024}}) {
      Samples samples;
      for (int i = 0; i < reps; ++i) {
        samples.Add(MicroPair(capacity, total / 2, &violations));
      }
      PrintRow("Channel cap=" + std::to_string(capacity), samples);
    }
    std::printf("\n");
  }

  #if !defined(GO2CPP_BENCH_NO_BATCH)
  if (all || mode == "batch") {
    std::printf("[1c] 批量 API（TrySendBatch/TryRecvBatch）SPSC\n");
    for (const std::size_t capacity : {std::size_t{64}, std::size_t{1024}}) {
      for (const std::size_t batch : {std::size_t{8}, std::size_t{32}}) {
        Samples samples;
        for (int i = 0; i < reps; ++i) {
          samples.Add(BatchSpsc(capacity, batch, total, &violations));
        }
        PrintRow("Channel cap=" + std::to_string(capacity) + " batch=" +
                     std::to_string(batch),
                 samples);
      }
    }
    std::printf("\n");
  }
  #endif  // GO2CPP_BENCH_NO_BATCH

  if (all || mode == "tryspsc") {
    std::printf("[1b] 纯快路径 SPSC（TrySend/TryRecv 轮询，永不 park）\n");
    for (const std::size_t capacity : {std::size_t{64}, std::size_t{1024}}) {
      Samples samples;
      for (int i = 0; i < reps; ++i) {
        samples.Add(TrySpsc(capacity, total, &violations));
      }
      PrintRow("Channel cap=" + std::to_string(capacity), samples);
    }
    std::printf("\n");
  }

  if (all || mode == "spsc") {
    std::printf("[1] SPSC 吞吐（1 生产 + 1 消费）\n");
    for (const std::size_t capacity : {std::size_t{0}, std::size_t{64},
                                       std::size_t{1024}}) {
      Samples samples;
      for (int i = 0; i < reps; ++i) {
        samples.Add(Spsc(capacity, total, &violations));
      }
      PrintRow("Channel cap=" + std::to_string(capacity), samples);
    }
    std::printf("\n");
  }

  if (all || mode == "pingpong") {
    std::printf("[2] ping-pong 单程延迟（两个通道来回）\n");
    for (const std::size_t capacity : {std::size_t{0}, std::size_t{1},
                                       std::size_t{64}}) {
      Samples samples;
      for (int i = 0; i < reps; ++i) {
        samples.Add(PingPong(capacity, ping_rounds, &violations));
      }
      PrintRow("Channel cap=" + std::to_string(capacity), samples);
    }
    std::printf("\n");
  }

  if (all || mode == "empty") {
    std::printf("[3] 空通道 / 满缓冲的非阻塞探测成本\n");
    {
      Samples samples;
      for (int i = 0; i < reps; ++i) {
        samples.Add(EmptyTryRecv(0, total, &violations));
      }
      PrintRow("TryRecv cap=0 空", samples);
    }
    {
      Samples samples;
      for (int i = 0; i < reps; ++i) {
        samples.Add(EmptyTryRecv(1024, total, &violations));
      }
      PrintRow("TryRecv cap=1024 空", samples);
    }
    {
      Samples samples;
      for (int i = 0; i < reps; ++i) {
        samples.Add(FullTrySend(1024, total, &violations));
      }
      PrintRow("TrySend cap=1024 满", samples);
    }
    std::printf("\n");
  }

  const auto bad = violations.load(std::memory_order_relaxed);
  std::printf("语义校验: 乱序/丢失/重复/意外命中 = %llu%s\n",
              static_cast<unsigned long long>(bad),
              bad == 0 ? "（通过）" : "（失败）");
  return bad == 0 ? 0 : 1;
}
