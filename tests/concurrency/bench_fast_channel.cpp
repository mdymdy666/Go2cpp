/**
 * @file bench_fast_channel.cpp
 * @brief FastChannel / FastSpscChannel 数据面基准（1P1C、单线程快路径、
 *        Fiber 1P1C），同场对照 Go `chan`（见 tests/concurrency/go_channel_bench.go）。
 *
 * 口径：
 *  - `1P1C`：一个生产者线程 + 一个消费者线程（`taskset -c 2,3` 绑定同一物理核
 *    的两个 SMT 线程），逐条 `Send/Recv`，200k 条 × N 轮取中位数；每轮都校验
 *    序号，任何丢失/重复/乱序都会计入 `bad` 并让程序返回非零。
 *  - `Fiber 1P1C`：两个受管 Fiber 在同一 Scheduler（1 P / 1 M）上跑同样的收发，
 *    对应 Go 的「两个 goroutine 同 P 交替」场景。
 *  - `单线程快路径`：同一线程 TryRecv+TrySend 交替，隔离锁/ring 的固定成本。
 *
 * 构建（CMake target `go2cpp_fast_channel_bench`）：
 *   cmake --build .build-bento-wsl --target go2cpp_fast_channel_bench -j
 * 运行：
 *   taskset -c 2,3 .build-bento-wsl/go2cpp_fast_channel_bench [total] [reps]
 *
 * Go 对照：
 *   cd tests/concurrency && GO111MODULE=off go build -o /tmp/go_channel_bench go_channel_bench.go
 *   for c in 0 64 1024; do taskset -c 2,3 /tmp/go_channel_bench -cap $c -n 200000 -reps 5; done
 */

#include "go2cpp/concurrency/fast_channel.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using go2cpp::FastStatus;

/// @brief 防止编译器把结果优化掉。
inline void Escape(std::uint64_t value) {
  static volatile std::uint64_t sink = 0;
  sink = sink + value;
}

/// @brief 自旋起跑门：计时不含线程创建开销。
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
  double Median() const {
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    return sorted[sorted.size() / 2];
  }
};

void PrintRow(const char* label, const Samples& samples,
              std::uint64_t bad) {
  std::printf("  %-26s median %8.2f  min %8.2f  ns/op  bad=%llu\n", label,
              samples.Median(), samples.Min(),
              static_cast<unsigned long long>(bad));
  std::fflush(stdout);
}

/// @brief 1P1C：阻塞 Send/Recv，返回 ns/消息。
template <typename ChannelT>
double RunThreadPair(ChannelT& channel, std::uint64_t total,
                     std::atomic<std::uint64_t>* bad) {
  StartGate gate;
  std::atomic<std::uint64_t> errors{0};
  std::atomic<std::uint64_t> checksum{0};
  std::thread producer([&] {
    gate.Wait();
    for (std::uint64_t i = 0; i < total; ++i) {
      if (channel.Send(i) != FastStatus::kOk) {
        errors.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });
  std::thread consumer([&] {
    gate.Wait();
    std::uint64_t sum = 0;
    std::uint64_t local_bad = 0;
    for (std::uint64_t i = 0; i < total; ++i) {
      std::uint64_t value = 0;
      if (channel.Recv(value) != FastStatus::kOk || value != i) {
        ++local_bad;
        continue;
      }
      sum += value;
    }
    checksum.store(sum, std::memory_order_relaxed);
    errors.fetch_add(local_bad, std::memory_order_relaxed);
  });
  const auto start = Clock::now();
  gate.Open();
  producer.join();
  consumer.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - start);
  Escape(checksum.load(std::memory_order_relaxed));
  bad->fetch_add(errors.load(std::memory_order_relaxed),
                 std::memory_order_relaxed);
  return static_cast<double>(elapsed.count()) / static_cast<double>(total);
}

/// @brief Fiber 1P1C：两个 Fiber 在同一 Scheduler（1P/1M）上收发。
template <typename ChannelT>
double RunFiberPair(ChannelT& channel, std::uint64_t total,
                    std::atomic<std::uint64_t>* bad) {
  go2cpp::SchedulerConfig config;
  config.processor_count = 1;
  config.max_workers = 1;
  config.idle_wait = 1ms;
  go2cpp::Scheduler scheduler(config);
  scheduler.start();
  std::atomic<std::uint64_t> errors{0};
  const auto start = Clock::now();
  auto producer = scheduler.spawn([&channel, total] {
    for (std::uint64_t i = 0; i < total; ++i) {
      channel.Send(i);
    }
  });
  auto consumer = scheduler.spawn([&channel, total, &errors] {
    for (std::uint64_t i = 0; i < total; ++i) {
      std::uint64_t value = 0;
      if (channel.Recv(value) != FastStatus::kOk || value != i) {
        errors.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });
  const bool ok = producer->wait_for(120s) && consumer->wait_for(120s);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - start);
  scheduler.shutdown();
  if (!ok) {
    std::fprintf(stderr, "[bench] fiber pair timed out\n");
    std::abort();
  }
  bad->fetch_add(errors.load(std::memory_order_relaxed),
                 std::memory_order_relaxed);
  return static_cast<double>(elapsed.count()) / static_cast<double>(total);
}

/// @brief 单线程快路径：TryRecv + TrySend 交替（ring 恒非空/非满）。
template <typename ChannelT>
double RunSingleThreadTry(ChannelT& channel) {
  constexpr std::uint64_t kIters = 2000000;
  for (std::uint64_t i = 0; i < 1024; ++i) {
    channel.TrySend(i);
  }
  const auto start = Clock::now();
  std::uint64_t sum = 0;
  for (std::uint64_t i = 0; i < kIters; ++i) {
    std::uint64_t value = 0;
    channel.TryRecv(value);
    channel.TrySend(value + 1);
    sum += value;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - start);
  Escape(sum);
  return static_cast<double>(elapsed.count()) /
         static_cast<double>(kIters);
}

template <typename ChannelT>
void BenchPair(const char* label, std::uint64_t total, int reps,
               std::atomic<std::uint64_t>* bad) {
  ChannelT channel;
  Samples samples;
  for (int i = 0; i < reps; ++i) {
    samples.Add(RunThreadPair(channel, total, bad));
  }
  PrintRow(label, samples, bad->load());
}

template <typename ChannelT>
void BenchFiberPair(const char* label, std::uint64_t total, int reps,
                    std::atomic<std::uint64_t>* bad) {
  ChannelT channel;
  Samples samples;
  for (int i = 0; i < reps; ++i) {
    samples.Add(RunFiberPair(channel, total, bad));
  }
  PrintRow(label, samples, bad->load());
}

template <typename ChannelT>
void BenchTry(const char* label) {
  ChannelT channel;
  Samples samples;
  for (int i = 0; i < 3; ++i) {
    samples.Add(RunSingleThreadTry(channel));
  }
  PrintRow(label, samples, 0);
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t total =
      argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
  const int reps = argc > 2 ? std::atoi(argv[2]) : 5;
  std::printf("=== FastChannel bench: total=%llu reps=%d ===\n",
              static_cast<unsigned long long>(total), reps);

  std::atomic<std::uint64_t> bad{0};
  std::printf("[线程 1P1C，taskset 2,3]\n");
  BenchPair<go2cpp::FastChannel<std::uint64_t, 0>>("fast-cap0", total, reps,
                                                   &bad);
  BenchPair<go2cpp::FastChannel<std::uint64_t, 64>>("fast-cap64", total, reps,
                                                    &bad);
  BenchPair<go2cpp::FastChannel<std::uint64_t, 1024>>("fast-cap1024", total,
                                                      reps, &bad);
  BenchPair<go2cpp::FastSpscChannel<std::uint64_t, 64>>("spsc-cap64", total,
                                                        reps, &bad);
  BenchPair<go2cpp::FastSpscChannel<std::uint64_t, 1024>>("spsc-cap1024", total,
                                                          reps, &bad);
  BenchPair<go2cpp::FastSpscChannel<std::uint64_t, 256>>("spsc-cap256", total,
                                                         reps, &bad);
  BenchPair<go2cpp::FastSpscChannel<std::uint64_t, 16>>("spsc-cap16", total,
                                                        reps, &bad);

  std::printf("[Fiber 1P1C，1P/1M]\n");
  BenchFiberPair<go2cpp::FastChannel<std::uint64_t, 0>>(
      "fiber-fast-cap0", total, reps, &bad);
  BenchFiberPair<go2cpp::FastChannel<std::uint64_t, 64>>(
      "fiber-fast-cap64", total, reps, &bad);
  BenchFiberPair<go2cpp::FastChannel<std::uint64_t, 1024>>(
      "fiber-fast-cap1024", total, reps, &bad);
  BenchFiberPair<go2cpp::FastSpscChannel<std::uint64_t, 64>>(
      "fiber-spsc-cap64", total, reps, &bad);

  std::printf("[单线程快路径]\n");
  BenchTry<go2cpp::FastSpscChannel<std::uint64_t, 1024>>("spsc-try-1T");
  BenchTry<go2cpp::FastChannel<std::uint64_t, 1024>>("fast-try-1T");

  std::printf("[汇总] bad=%llu\n",
              static_cast<unsigned long long>(bad.load()));
  return bad.load() == 0 ? 0 : 1;
}
