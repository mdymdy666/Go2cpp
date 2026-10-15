/**
 * @file channel_vs_queue_benchmark.cpp
 * @brief Go2Cpp `Channel<T>` 与传统「线程 + 阻塞队列」的吞吐/延迟对比基准。
 *
 * 对比对象（三种实现，都是 FIFO）：
 *   A. `go2cpp::Channel<T>` —— 本项目的 Go 风格通道（无缓冲 / 有缓冲）
 *   B. `BoundedQueue<T>`    —— 教科书式双条件变量有界队列（`not_empty` + `not_full`，`notify_one`）
 *   C. `NaiveQueue<T>`      —— 常见但粗糙的写法（**单条件变量 + notify_all**，用来量化"惊群"代价）
 *
 * 测例：
 *   1. SPSC 吞吐    1 生产者 + 1 消费者，传 N 个 int
 *   2. MPMC 吞吐    4 生产者 + 4 消费者，合计 N 个 int
 *   3. ping-pong 延迟  一个值来回 N 次（测单次往返）
 *   4. 空通道探测成本 `TryRecv()` / `TryPop()` 在空队列上的单次开销
 *
 * 每个测例重复若干轮，输出 min / median / max —— **只报区间，不把最好成绩当结论**
 * （与 `docs/performance.md` 的阅读须知一致）。
 *
 * 编译：必须走 `build-bench`（`CMAKE_BUILD_TYPE=Release`）。本文件另由 CMake 加 `-O2`。
 * 用法：`./go2cpp_channel_vs_queue_benchmark [重复轮数]`
 */

#include "go2cpp/channel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// 参考实现 B：教科书式双条件变量有界队列
// ---------------------------------------------------------------------------
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : m_capacity(capacity) {}

  void Push(T value) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_not_full.wait(lock, [this] { return m_queue.size() < m_capacity; });
    m_queue.push_back(std::move(value));
    lock.unlock();
    m_not_empty.notify_one();
  }

  T Pop() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_not_empty.wait(lock, [this] { return !m_queue.empty(); });
    T value = std::move(m_queue.front());
    m_queue.pop_front();
    lock.unlock();
    m_not_full.notify_one();
    return value;
  }

  bool TryPop(T* out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty()) return false;
    *out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
  }

 private:
  std::size_t m_capacity;
  std::mutex m_mutex;
  std::condition_variable m_not_empty;
  std::condition_variable m_not_full;
  std::deque<T> m_queue;
};

// ---------------------------------------------------------------------------
// 参考实现 C：单条件变量 + notify_all（常见粗糙写法，量化惊群代价）
// ---------------------------------------------------------------------------
template <typename T>
class NaiveQueue {
 public:
  explicit NaiveQueue(std::size_t capacity) : m_capacity(capacity) {}

  void Push(T value) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this] { return m_queue.size() < m_capacity; });
    m_queue.push_back(std::move(value));
    lock.unlock();
    m_cv.notify_all();
  }

  T Pop() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this] { return !m_queue.empty(); });
    T value = std::move(m_queue.front());
    m_queue.pop_front();
    lock.unlock();
    m_cv.notify_all();
    return value;
  }

  bool TryPop(T* out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty()) return false;
    *out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
  }

 private:
  std::size_t m_capacity;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<T> m_queue;
};

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

/// 防止编译器把被传递的值优化掉。
inline void Escape(std::uint64_t value) {
  static volatile std::uint64_t sink = 0;
  sink = sink + value;
}

/// 自旋等待起跑信号，让计时不包含线程创建时间。
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

std::string HostDescription() {
  std::string description;
  std::FILE* file = std::fopen("/proc/cpuinfo", "r");
  if (file != nullptr) {
    char line[512];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
      const std::string text(line);
      if (text.rfind("model name", 0) == 0) {
        const auto colon = text.find(':');
        if (colon != std::string::npos) {
          description = text.substr(colon + 1);
          while (!description.empty() &&
                 (description.front() == ' ' || description.front() == '\t')) {
            description.erase(description.begin());
          }
          while (!description.empty() &&
                 (description.back() == '\n' || description.back() == ' ')) {
            description.pop_back();
          }
        }
        break;
      }
    }
    std::fclose(file);
  }
  const unsigned int hardware = std::thread::hardware_concurrency();
  description += "  (hardware_concurrency=" + std::to_string(hardware) + ")";
  return description;
}

struct Samples {
  std::vector<double> nanoseconds;  // 每轮的 ns/op

  void Add(double value) { nanoseconds.push_back(value); }

  double Min() const { return *std::min_element(nanoseconds.begin(), nanoseconds.end()); }
  double Max() const { return *std::max_element(nanoseconds.begin(), nanoseconds.end()); }
  double Median() const {
    std::vector<double> sorted = nanoseconds;
    std::sort(sorted.begin(), sorted.end());
    return sorted[sorted.size() / 2];
  }
};

// ---------------------------------------------------------------------------
// 测例实现
// ---------------------------------------------------------------------------

/// A：Channel 的 SPSC 吞吐。
double ChannelSpsc(go2cpp::ChannelPtr<int> channel, std::uint64_t total) {
  StartGate gate;
  std::atomic<std::uint64_t> sum{0};
  std::thread producer([&] {
    gate.Wait();
    for (std::uint64_t i = 0; i < total; ++i) {
      channel->Send(static_cast<int>(i));
    }
  });
  std::thread consumer([&] {
    gate.Wait();
    std::uint64_t local = 0;
    for (std::uint64_t i = 0; i < total; ++i) {
      const auto result = channel->Recv();
      local += static_cast<std::uint64_t>(result.ValueOrDefault());
    }
    sum.store(local, std::memory_order_relaxed);
  });
  const auto started = Clock::now();
  gate.Open();
  producer.join();
  consumer.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
  Escape(sum.load(std::memory_order_relaxed));
  return static_cast<double>(elapsed.count()) / static_cast<double>(total);
}

/// B/C：队列的 SPSC 吞吐。
template <typename Queue>
double QueueSpsc(Queue& queue, std::uint64_t total) {
  StartGate gate;
  std::atomic<std::uint64_t> sum{0};
  std::thread producer([&] {
    gate.Wait();
    for (std::uint64_t i = 0; i < total; ++i) {
      queue.Push(static_cast<int>(i));
    }
  });
  std::thread consumer([&] {
    gate.Wait();
    std::uint64_t local = 0;
    for (std::uint64_t i = 0; i < total; ++i) {
      local += static_cast<std::uint64_t>(queue.Pop());
    }
    sum.store(local, std::memory_order_relaxed);
  });
  const auto started = Clock::now();
  gate.Open();
  producer.join();
  consumer.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
  Escape(sum.load(std::memory_order_relaxed));
  return static_cast<double>(elapsed.count()) / static_cast<double>(total);
}

/// A：Channel 的 MPMC 吞吐。
double ChannelMpmc(go2cpp::ChannelPtr<int> channel, int producers, int consumers,
                   std::uint64_t per_producer) {
  StartGate gate;
  std::atomic<std::uint64_t> sum{0};
  std::vector<std::thread> threads;
  const std::uint64_t total = static_cast<std::uint64_t>(producers) * per_producer;
  const std::uint64_t per_consumer = total / static_cast<std::uint64_t>(consumers);
  for (int p = 0; p < producers; ++p) {
    threads.emplace_back([&, p] {
      gate.Wait();
      for (std::uint64_t i = 0; i < per_producer; ++i) {
        channel->Send(static_cast<int>(p * 1000 + static_cast<int>(i)));
      }
    });
  }
  for (int c = 0; c < consumers; ++c) {
    threads.emplace_back([&] {
      gate.Wait();
      std::uint64_t local = 0;
      for (std::uint64_t i = 0; i < per_consumer; ++i) {
        local += static_cast<std::uint64_t>(channel->Recv().ValueOrDefault());
      }
      sum.fetch_add(local, std::memory_order_relaxed);
    });
  }
  const auto started = Clock::now();
  gate.Open();
  for (auto& thread : threads) thread.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
  Escape(sum.load(std::memory_order_relaxed));
  return static_cast<double>(elapsed.count()) / static_cast<double>(total);
}

/// B/C：队列的 MPMC 吞吐。
template <typename Queue>
double QueueMpmc(Queue& queue, int producers, int consumers, std::uint64_t per_producer) {
  StartGate gate;
  std::atomic<std::uint64_t> sum{0};
  std::vector<std::thread> threads;
  const std::uint64_t total = static_cast<std::uint64_t>(producers) * per_producer;
  const std::uint64_t per_consumer = total / static_cast<std::uint64_t>(consumers);
  for (int p = 0; p < producers; ++p) {
    threads.emplace_back([&, p] {
      gate.Wait();
      for (std::uint64_t i = 0; i < per_producer; ++i) {
        queue.Push(static_cast<int>(p * 1000 + static_cast<int>(i)));
      }
    });
  }
  for (int c = 0; c < consumers; ++c) {
    threads.emplace_back([&] {
      gate.Wait();
      std::uint64_t local = 0;
      for (std::uint64_t i = 0; i < per_consumer; ++i) {
        local += static_cast<std::uint64_t>(queue.Pop());
      }
      sum.fetch_add(local, std::memory_order_relaxed);
    });
  }
  const auto started = Clock::now();
  gate.Open();
  for (auto& thread : threads) thread.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
  Escape(sum.load(std::memory_order_relaxed));
  return static_cast<double>(elapsed.count()) / static_cast<double>(total);
}

/// A：Channel 的 ping-pong 往返（无缓冲会合）。
double ChannelPingPong(go2cpp::ChannelPtr<int> to_b, go2cpp::ChannelPtr<int> to_a,
                       std::uint64_t rounds) {
  std::thread responder([&] {
    for (std::uint64_t i = 0; i < rounds; ++i) {
      to_b->Recv();
      to_a->Send(1);
    }
  });
  const auto started = Clock::now();
  for (std::uint64_t i = 0; i < rounds; ++i) {
    to_b->Send(1);
    to_a->Recv();
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
  responder.join();
  return static_cast<double>(elapsed.count()) / static_cast<double>(rounds);
}

/// B/C：队列的 ping-pong 往返（容量 1 近似会合）。
template <typename Queue>
double QueuePingPong(Queue& to_b, Queue& to_a, std::uint64_t rounds) {
  std::thread responder([&] {
    for (std::uint64_t i = 0; i < rounds; ++i) {
      to_b.Pop();
      to_a.Push(1);
    }
  });
  const auto started = Clock::now();
  for (std::uint64_t i = 0; i < rounds; ++i) {
    to_b.Push(1);
    to_a.Pop();
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
  responder.join();
  return static_cast<double>(elapsed.count()) / static_cast<double>(rounds);
}

/// A：空通道上的 TryRecv 成本。
double ChannelEmptyTryRecv(go2cpp::ChannelPtr<int> channel, std::uint64_t iterations) {
  std::uint64_t hits = 0;
  const auto started = Clock::now();
  for (std::uint64_t i = 0; i < iterations; ++i) {
    if (channel->TryRecv().Ok()) ++hits;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
  Escape(hits);
  return static_cast<double>(elapsed.count()) / static_cast<double>(iterations);
}

/// B/C：空队列上的 TryPop 成本。
template <typename Queue>
double QueueEmptyTryPop(Queue& queue, std::uint64_t iterations) {
  std::uint64_t hits = 0;
  int value = 0;
  const auto started = Clock::now();
  for (std::uint64_t i = 0; i < iterations; ++i) {
    if (queue.TryPop(&value)) ++hits;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
  Escape(hits + static_cast<std::uint64_t>(value));
  return static_cast<double>(elapsed.count()) / static_cast<double>(iterations);
}

void PrintRow(const std::string& label, const Samples& samples) {
  std::printf("  %-46s min %10.1f  median %10.1f  max %10.1f  ns/op\n", label.c_str(),
              samples.Min(), samples.Median(), samples.Max());
}

template <typename Body>
Samples RunCase(int repeats, Body&& body) {
  Samples samples;
  for (int i = 0; i < repeats; ++i) {
    samples.Add(body());
  }
  return samples;
}

}  // namespace

int main(int argc, char** argv) {
  int repeats = 3;
  if (argc > 1) {
    const int parsed = std::atoi(argv[1]);
    if (parsed > 0) repeats = parsed;
  }

  constexpr std::uint64_t kSpscTotal = 50000;
  constexpr std::uint64_t kMpmcPerProducer = 12500;
  constexpr std::uint64_t kPingPongRounds = 10000;
  constexpr std::uint64_t kTryIterations = 100000;

  std::printf("=== Go2Cpp Channel vs 传统线程+异步队列 基准 ===\n");
  std::printf("宿主: %s\n", HostDescription().c_str());
  std::printf("重复轮数: %d（每轮输出一次，报告 min/median/max 区间）\n", repeats);
  std::printf("参数: SPSC N=%llu | MPMC 4x4 合计=%llu | ping-pong 往返=%llu | 空探测=%llu\n\n",
              static_cast<unsigned long long>(kSpscTotal),
              static_cast<unsigned long long>(kMpmcPerProducer * 4),
              static_cast<unsigned long long>(kPingPongRounds),
              static_cast<unsigned long long>(kTryIterations));

  // ---------------- 1. SPSC 吞吐 ----------------
  std::printf("[1] SPSC 吞吐（1 生产 + 1 消费，%llu 个 int）\n",
              static_cast<unsigned long long>(kSpscTotal));
  {
    // A1 无缓冲
    auto samples = RunCase(repeats, [&] {
      return ChannelSpsc(go2cpp::MakeChannel<int>(0), kSpscTotal);
    });
    PrintRow("A1 Channel 无缓冲(0)", samples);

    // A2/A3 有缓冲
    for (const std::size_t capacity : {std::size_t{64}, std::size_t{1024}}) {
      auto buffered = RunCase(repeats, [&] {
        return ChannelSpsc(go2cpp::MakeChannel<int>(capacity), kSpscTotal);
      });
      PrintRow("A" + std::string(capacity == 64 ? "2" : "3") + " Channel 有缓冲(" +
                   std::to_string(capacity) + ")",
               buffered);
    }

    for (const std::size_t capacity : {std::size_t{1}, std::size_t{64}, std::size_t{1024}}) {
      auto queue_samples = RunCase(repeats, [&] {
        BoundedQueue<int> queue(capacity);
        return QueueSpsc(queue, kSpscTotal);
      });
      PrintRow("B  BoundedQueue 双cv(" + std::to_string(capacity) + ")", queue_samples);
    }

    for (const std::size_t capacity : {std::size_t{64}, std::size_t{1024}}) {
      auto naive_samples = RunCase(repeats, [&] {
        NaiveQueue<int> queue(capacity);
        return QueueSpsc(queue, kSpscTotal);
      });
      PrintRow("C  NaiveQueue 单cv+notify_all(" + std::to_string(capacity) + ")", naive_samples);
    }
  }

  // ---------------- 2. MPMC 吞吐 ----------------
  std::printf("\n[2] MPMC 吞吐（4 生产 + 4 消费，合计 %llu）\n",
              static_cast<unsigned long long>(kMpmcPerProducer * 4));
  {
    for (const std::size_t capacity : {std::size_t{0}, std::size_t{64}, std::size_t{1024}}) {
      auto samples = RunCase(repeats, [&] {
        return ChannelMpmc(go2cpp::MakeChannel<int>(capacity), 4, 4, kMpmcPerProducer);
      });
      PrintRow("A  Channel(" + std::to_string(capacity) + ")", samples);
    }
    for (const std::size_t capacity : {std::size_t{64}, std::size_t{1024}}) {
      auto samples = RunCase(repeats, [&] {
        BoundedQueue<int> queue(capacity);
        return QueueMpmc(queue, 4, 4, kMpmcPerProducer);
      });
      PrintRow("B  BoundedQueue 双cv(" + std::to_string(capacity) + ")", samples);
    }
    auto naive_samples = RunCase(repeats, [&] {
      NaiveQueue<int> queue(1024);
      return QueueMpmc(queue, 4, 4, kMpmcPerProducer);
    });
    PrintRow("C  NaiveQueue 单cv+notify_all(1024)", naive_samples);
  }

  // ---------------- 3. ping-pong 延迟 ----------------
  std::printf("\n[3] ping-pong 往返延迟（%llu 次）\n",
              static_cast<unsigned long long>(kPingPongRounds));
  {
    auto samples = RunCase(repeats, [&] {
      return ChannelPingPong(go2cpp::MakeChannel<int>(0), go2cpp::MakeChannel<int>(0),
                             kPingPongRounds);
    });
    PrintRow("A  Channel 无缓冲(0) 双向会合", samples);

    auto buffered = RunCase(repeats, [&] {
      return ChannelPingPong(go2cpp::MakeChannel<int>(1), go2cpp::MakeChannel<int>(1),
                             kPingPongRounds);
    });
    PrintRow("A  Channel 有缓冲(1) 双向", buffered);

    auto queue_samples = RunCase(repeats, [&] {
      BoundedQueue<int> to_b(1);
      BoundedQueue<int> to_a(1);
      return QueuePingPong(to_b, to_a, kPingPongRounds);
    });
    PrintRow("B  BoundedQueue(1) 双向", queue_samples);

    auto naive_samples = RunCase(repeats, [&] {
      NaiveQueue<int> to_b(1);
      NaiveQueue<int> to_a(1);
      return QueuePingPong(to_b, to_a, kPingPongRounds);
    });
    PrintRow("C  NaiveQueue(1) 双向", naive_samples);
  }

  // ---------------- 4. 空通道探测成本 ----------------
  std::printf("\n[4] 空通道探测成本（%llu 次 TryRecv / TryPop）\n",
              static_cast<unsigned long long>(kTryIterations));
  {
    auto samples = RunCase(repeats, [&] {
      return ChannelEmptyTryRecv(go2cpp::MakeChannel<int>(0), kTryIterations);
    });
    PrintRow("A  Channel::TryRecv() 空", samples);

    auto queue_samples = RunCase(repeats, [&] {
      BoundedQueue<int> queue(64);
      return QueueEmptyTryPop(queue, kTryIterations);
    });
    PrintRow("B  BoundedQueue::TryPop() 空", queue_samples);

    auto naive_samples = RunCase(repeats, [&] {
      NaiveQueue<int> queue(64);
      return QueueEmptyTryPop(queue, kTryIterations);
    });
    PrintRow("C  NaiveQueue::TryPop() 空", naive_samples);
  }

  std::printf("\n说明：所有数字均为 ns/op，来自 build-bench（Release, -O2）；\n");
  std::printf("      每项跑 %d 轮，上表给出 min/median/max —— 只作区间参考，\n", repeats);
  std::printf("      不得把最小轮次当保证，也不得与其他日期/宿主的结果横向比较。\n");
  return 0;
}
