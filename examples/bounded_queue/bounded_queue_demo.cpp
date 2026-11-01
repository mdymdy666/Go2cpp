// REQ-004 有界队列 / 背压：可编译运行的示例。
//
// 三个场景：
//   A. TrySend + 慢消费者：满时返回 kWouldBlock（= false），业务自己丢；
//   B. 阻塞 Send + 慢消费者：背压把压力反传给生产者，零丢弃；
//   C. RSS 对比：有界 Channel 的峰值内存远低于无界 std::deque。
//
// 编译运行：examples/bounded_queue/build_and_run.sh [--items N]
// （纯示例，不进入库目标，也不改任何现有构建文件）

#include "go2cpp/channel.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Job {
  std::uint64_t id = 0;
  std::uint64_t payload = 0;
};

std::uint64_t ReadRssBytes() {
#if defined(__linux__)
  std::FILE* file = std::fopen("/proc/self/statm", "r");
  if (file == nullptr) {
    return 0;
  }
  unsigned long long total_pages = 0;
  unsigned long long resident_pages = 0;
  const int parsed =
      std::fscanf(file, "%llu %llu", &total_pages, &resident_pages);
  std::fclose(file);
  if (parsed != 2) {
    return 0;
  }
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(resident_pages) *
         static_cast<std::uint64_t>(page_size);
#else
  return 0;
#endif
}

double Mib(std::uint64_t bytes) {
  return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

double SecondsSince(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

void PrintHeader(const char* title) {
  std::printf("\n== %s ==\n", title);
}

// ---------------------------------------------------------------------------
// 场景 A：TrySend + 慢消费者 —— 满即丢弃，绝不阻塞生产者
// ---------------------------------------------------------------------------
void ScenarioTrySendDrop(std::uint64_t items, std::size_t capacity) {
  PrintHeader("A. TrySend + 慢消费者：满时丢弃（不阻塞）");
  auto ch = go2cpp::MakeChannel<Job>(capacity);
  std::atomic<std::uint64_t> sent{0};
  std::atomic<std::uint64_t> dropped{0};
  std::atomic<std::uint64_t> consumed{0};

  const auto start = Clock::now();
  std::thread consumer([&] {
    std::uint64_t count = 0;
    while (true) {
      const auto result = ch->Recv();
      if (!result.ok) {
        break;
      }
      ++count;
      // 模拟慢消费者：每 256 条处理 200µs。
      if (count % 256 == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    }
    consumed.store(count, std::memory_order_release);
  });

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < items; ++i) {
      const auto result = ch->TrySend(Job{i, i});
      if (result.Ok()) {
        sent.fetch_add(1, std::memory_order_relaxed);
      } else if (result.status == go2cpp::ChannelStatus::kWouldBlock) {
        // 满：业务侧自行决定丢弃 / 降级 / 重试（这里选择丢弃并计数）。
        dropped.fetch_add(1, std::memory_order_relaxed);
      } else {
        // kClosed：消费者已经关闭，停止生产。
        break;
      }
    }
    ch->Close();
  });

  producer.join();
  consumer.join();
  const double seconds = SecondsSince(start);

  std::printf("  capacity=%zu items=%llu sent=%llu dropped=%llu consumed=%llu\n",
              capacity, static_cast<unsigned long long>(items),
              static_cast<unsigned long long>(sent.load()),
              static_cast<unsigned long long>(dropped.load()),
              static_cast<unsigned long long>(consumed.load()));
  std::printf("  elapsed=%.3fs drop_rate=%.2f%% 生产者从未被阻塞\n", seconds,
              items == 0 ? 0.0
                         : 100.0 * static_cast<double>(dropped.load()) /
                               static_cast<double>(items));
}

// ---------------------------------------------------------------------------
// 场景 B：阻塞 Send + 慢消费者 —— 背压反传到生产者，零丢弃
// ---------------------------------------------------------------------------
void ScenarioBlockingBackpressure(std::uint64_t items, std::size_t capacity) {
  PrintHeader("B. 阻塞 Send + 慢消费者：背压（零丢弃）");
  auto ch = go2cpp::MakeChannel<Job>(capacity);
  std::atomic<std::uint64_t> sent{0};
  std::atomic<std::uint64_t> consumed{0};

  const auto start = Clock::now();
  std::thread consumer([&] {
    std::uint64_t count = 0;
    while (true) {
      const auto result = ch->Recv();
      if (!result.ok) {
        break;
      }
      ++count;
      if (count % 256 == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    }
    consumed.store(count, std::memory_order_release);
  });

  double send_blocked_seconds = 0.0;
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < items; ++i) {
      const auto send_start = Clock::now();
      const auto result = ch->Send(Job{i, i});
      send_blocked_seconds += SecondsSince(send_start);
      if (!result.Ok()) {
        break;
      }
      sent.fetch_add(1, std::memory_order_relaxed);
    }
    ch->Close();
  });

  producer.join();
  consumer.join();
  const double seconds = SecondsSince(start);

  std::printf("  capacity=%zu items=%llu sent=%llu consumed=%llu\n", capacity,
              static_cast<unsigned long long>(items),
              static_cast<unsigned long long>(sent.load()),
              static_cast<unsigned long long>(consumed.load()));
  std::printf("  elapsed=%.3fs Send累计耗时=%.3fs（被消费者限速）\n", seconds,
              send_blocked_seconds);
}

// ---------------------------------------------------------------------------
// 场景 C：RSS 对比 —— 有界 Channel vs 无界 std::deque
// ---------------------------------------------------------------------------
class UnboundedQueue {
 public:
  void Push(Job job) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back(job);
  }

  bool Pop(Job* out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty()) {
      return false;
    }
    *out = m_queue.front();
    m_queue.pop_front();
    return true;
  }

  bool Empty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.empty();
  }

 private:
  mutable std::mutex m_mutex;
  std::deque<Job> m_queue;
};

// 有界队列：快生产者 + 慢消费者，采样 RSS 峰值。
struct RssSample {
  std::uint64_t baseline = 0;
  std::uint64_t peak = 0;
};

RssSample BoundedPeakRss(std::uint64_t items, std::size_t capacity) {
  auto ch = go2cpp::MakeChannel<Job>(capacity);
  RssSample sample;
  sample.baseline = ReadRssBytes();
  sample.peak = sample.baseline;

  std::thread consumer([&] {
    std::uint64_t count = 0;
    while (true) {
      const auto result = ch->Recv();
      if (!result.ok) {
        break;
      }
      if (++count % 128 == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
  });

  for (std::uint64_t i = 0; i < items; ++i) {
    (void)ch->Send(Job{i, i});
    if (i % 100'000ULL == 0) {
      const std::uint64_t rss = ReadRssBytes();
      if (rss > sample.peak) {
        sample.peak = rss;
      }
    }
  }
  ch->Close();
  consumer.join();
  return sample;
}

// 无界队列：同一生产/消费速率，采样 RSS 峰值。
RssSample UnboundedPeakRss(std::uint64_t items) {
  UnboundedQueue queue;
  RssSample sample;
  sample.baseline = ReadRssBytes();
  sample.peak = sample.baseline;
  std::atomic<bool> done{false};

  std::thread consumer([&] {
    std::uint64_t count = 0;
    Job job;
    while (!done.load(std::memory_order_acquire) || !queue.Empty()) {
      if (queue.Pop(&job)) {
        if (++count % 128 == 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
      } else {
        std::this_thread::yield();
      }
    }
  });

  for (std::uint64_t i = 0; i < items; ++i) {
    queue.Push(Job{i, i});
    if (i % 100'000ULL == 0) {
      const std::uint64_t rss = ReadRssBytes();
      if (rss > sample.peak) {
        sample.peak = rss;
      }
    }
    if (i % 512 == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
  }
  done.store(true, std::memory_order_release);
  consumer.join();
  return sample;
}

void ScenarioRss(std::uint64_t items, std::size_t capacity) {
  PrintHeader("C. RSS 对比：有界 Channel vs 无界 std::deque");
  const RssSample bounded = BoundedPeakRss(items, capacity);
  const RssSample unbounded = UnboundedPeakRss(items);

  std::printf("  items=%llu\n", static_cast<unsigned long long>(items));
  std::printf("  有界 Channel(capacity=%zu): baseline=%.2f MiB peak=%.2f MiB "
              "growth=+%.2f MiB\n",
              capacity, Mib(bounded.baseline), Mib(bounded.peak),
              Mib(bounded.peak - bounded.baseline));
  std::printf("  无界 deque            : baseline=%.2f MiB peak=%.2f MiB "
              "growth=+%.2f MiB\n",
              Mib(unbounded.baseline), Mib(unbounded.peak),
              Mib(unbounded.peak - unbounded.baseline));
  std::printf("  结论：有界队列的内存只与 capacity 有关，与消息总量无关。\n");
}

int PrintUsage(const char* program) {
  std::printf("usage: %s [--items N] [--scenario a|b|c|all]\n", program);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t items = 2'000'000;
  std::string scenario = "all";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--items") == 0 && i + 1 < argc) {
      items = std::strtoull(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
      scenario = argv[++i];
    } else if (std::strcmp(argv[i], "--help") == 0) {
      return PrintUsage(argv[0]);
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return PrintUsage(argv[0]);
    }
  }

  constexpr std::size_t kCapacity = 256;
  std::printf("[bounded_queue_demo] items=%llu capacity=%zu scenario=%s\n",
              static_cast<unsigned long long>(items), kCapacity,
              scenario.c_str());

  if (scenario == "all" || scenario == "a") {
    ScenarioTrySendDrop(items, kCapacity);
  }
  if (scenario == "all" || scenario == "b") {
    ScenarioBlockingBackpressure(items, kCapacity);
  }
  if (scenario == "all" || scenario == "c") {
    ScenarioRss(items, 1024);
  }
  return 0;
}
