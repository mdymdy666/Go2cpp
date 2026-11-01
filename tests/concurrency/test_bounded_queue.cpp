// REQ-004 有界队列 / 背压：专项测试（独立可执行）。
//
// 设计意图：
//   1. 锁死 `TrySend` 在满时返回 `kWouldBlock`（= false 语义）而不是阻塞；
//   2. 用阻塞 `Send` / `Recv` 跑 1e8 条（默认 4 条流水线），逐条校验序号，
//      证明「不丢、不重、不乱序」；
//   3. 采样 RSS，证明有界队列的内存不随消息数增长；
//   4. 覆盖 cap==0 rendezvous、关闭后 TrySend、TrySend 丢弃守恒等边界。
//
// 编译运行：tests/concurrency/run_bounded_queue_test.sh
//   （默认不进入 CMake/ctest，与本目录 build_and_run_tests.sh 的惯例一致）

#include "go2cpp/channel.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using go2cpp::ChannelStatus;

constexpr std::uint64_t kDefaultNoLossItems = 100'000'000ULL;

struct Item {
  std::uint64_t producer = 0;
  std::uint64_t seq = 0;
};

struct Job {
  std::uint64_t id = 0;
};

std::uint64_t ReadRssBytes() {
#if defined(__linux__)
  std::FILE* file = std::fopen("/proc/self/statm", "r");
  if (file == nullptr) {
    return 0;
  }
  unsigned long long total_pages = 0;
  unsigned long long resident_pages = 0;
  const int parsed = std::fscanf(file, "%llu %llu", &total_pages,
                                 &resident_pages);
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

std::string FormatBytes(std::uint64_t bytes) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.2f MiB",
                static_cast<double>(bytes) / (1024.0 * 1024.0));
  return std::string(buffer);
}

double SecondsSince(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

// ---------------------------------------------------------------------------
// 用例 1：容量语义 —— 满时 Send 阻塞 / TrySend 返回 kWouldBlock
// ---------------------------------------------------------------------------
void TestCapacitySemantics() {
  go2cpp_tests::announce("bounded_queue: capacity semantics");
  auto ch = go2cpp::MakeChannel<int>(4);
  GO2CPP_REQUIRE(ch->Capacity() == 4);

  for (int i = 0; i < 4; ++i) {
    const auto result = ch->TrySend(i);
    GO2CPP_CHECK(result.Ok());
    GO2CPP_CHECK(result.status == ChannelStatus::kReady);
  }
  GO2CPP_CHECK(ch->Len() == 4);

  // 满：TrySend 不阻塞，返回 kWouldBlock —— 这就是「false」语义。
  const auto full = ch->TrySend(99);
  GO2CPP_CHECK(!full.Ok());
  GO2CPP_CHECK(full.status == ChannelStatus::kWouldBlock);
  GO2CPP_CHECK(!full.Closed());
  GO2CPP_CHECK(ch->Len() == 4);

  // 满时 Send 必须阻塞，直到消费者腾出空间。
  std::atomic<bool> send_returned{false};
  std::thread sender([&] {
    const auto result = ch->Send(100);
    GO2CPP_CHECK(result.Ok());
    send_returned.store(true, std::memory_order_release);
  });
  GO2CPP_REQUIRE_EVENTUALLY(ch->DebugSnapshot().send_waiters >= 1,
                            std::chrono::seconds(2));
  GO2CPP_CHECK(!send_returned.load(std::memory_order_acquire));

  // 取走一条，被阻塞的 Send 才能落位。
  const auto first = ch->Recv();
  GO2CPP_REQUIRE(first.ok);
  GO2CPP_CHECK(*first.value == 0);
  GO2CPP_JOIN_WITH_WATCHDOG(sender, std::chrono::seconds(2));
  GO2CPP_CHECK(send_returned.load(std::memory_order_acquire));

  // 排空：[1, 2, 3, 100]
  std::vector<int> drained;
  while (true) {
    const auto result = ch->TryRecv();
    if (!result.ok) {
      GO2CPP_CHECK(result.status == ChannelStatus::kWouldBlock);
      break;
    }
    drained.push_back(*result.value);
  }
  GO2CPP_REQUIRE(drained.size() == 4);
  GO2CPP_CHECK(drained[0] == 1);
  GO2CPP_CHECK(drained[1] == 2);
  GO2CPP_CHECK(drained[2] == 3);
  GO2CPP_CHECK(drained[3] == 100);
  GO2CPP_CHECK(ch->Len() == 0);

  // 关闭后 TrySend 返回 kClosed；缓冲已空时 Recv 返回 kClosed。
  ch->Close();
  const auto closed_send = ch->TrySend(7);
  GO2CPP_CHECK(closed_send.status == ChannelStatus::kClosed);
  GO2CPP_CHECK(closed_send.Closed());
  GO2CPP_CHECK(!closed_send.Ok());
  const auto closed_recv = ch->Recv();
  GO2CPP_CHECK(closed_recv.status == ChannelStatus::kClosed);
  GO2CPP_CHECK(!closed_recv.ok);

  // cap==0：无接收者时 TrySend 必然 kWouldBlock；有接收者挂起时可交接。
  auto rendezvous = go2cpp::MakeChannel<int>(0);
  GO2CPP_CHECK(rendezvous->Capacity() == 0);
  const auto nobody = rendezvous->TrySend(1);
  GO2CPP_CHECK(nobody.status == ChannelStatus::kWouldBlock);
  std::atomic<bool> got{false};
  std::thread receiver([&] {
    const auto result = rendezvous->Recv();
    GO2CPP_CHECK(result.ok);
    GO2CPP_CHECK(result.value.has_value() && *result.value == 42);
    got.store(true, std::memory_order_release);
  });
  GO2CPP_REQUIRE_EVENTUALLY(rendezvous->DebugSnapshot().recv_waiters >= 1,
                            std::chrono::seconds(2));
  const auto handed = rendezvous->TrySend(42);
  GO2CPP_CHECK(handed.Ok());
  GO2CPP_JOIN_WITH_WATCHDOG(receiver, std::chrono::seconds(2));
  GO2CPP_CHECK(got.load(std::memory_order_acquire));
}

// ---------------------------------------------------------------------------
// 用例 2：TrySend 满时丢弃 —— 计数守恒（快生产者 / 慢消费者）
// ---------------------------------------------------------------------------
void TestDropAccounting(std::uint64_t total) {
  go2cpp_tests::announce("bounded_queue: TrySend drop accounting");
  constexpr std::size_t kCapacity = 256;
  auto ch = go2cpp::MakeChannel<Job>(kCapacity);

  std::atomic<std::uint64_t> sent{0};
  std::atomic<std::uint64_t> dropped{0};
  std::atomic<std::uint64_t> received{0};
  std::atomic<bool> ordered{true};

  const auto start = Clock::now();
  std::thread consumer([&] {
    std::uint64_t last_id = 0;
    std::uint64_t count = 0;
    bool first = true;
    while (true) {
      const auto result = ch->Recv();
      if (!result.ok) {
        break;
      }
      // TrySend 会丢，因此序号允许跳号，但必须严格递增（不接受重复/乱序）。
      if (!first && result.value->id <= last_id) {
        ordered.store(false, std::memory_order_relaxed);
      }
      last_id = result.value->id;
      first = false;
      ++count;
      if (count % 200 == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
    }
    received.store(count, std::memory_order_release);
  });

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < total; ++i) {
      const auto result = ch->TrySend(Job{i});
      if (result.Ok()) {
        sent.fetch_add(1, std::memory_order_relaxed);
      } else if (result.status == ChannelStatus::kWouldBlock) {
        dropped.fetch_add(1, std::memory_order_relaxed);
      } else {
        GO2CPP_CHECK(false);
      }
    }
    ch->Close();
  });

  GO2CPP_JOIN_WITH_WATCHDOG(producer, std::chrono::seconds(60));
  GO2CPP_JOIN_WITH_WATCHDOG(consumer, std::chrono::seconds(60));

  const double seconds = SecondsSince(start);
  std::printf(
      "  total=%llu sent=%llu dropped=%llu received=%llu elapsed=%.3fs\n",
      static_cast<unsigned long long>(total),
      static_cast<unsigned long long>(sent.load()),
      static_cast<unsigned long long>(dropped.load()),
      static_cast<unsigned long long>(received.load()), seconds);

  GO2CPP_CHECK(sent.load() + dropped.load() == total);
  GO2CPP_CHECK(received.load() == sent.load());
  GO2CPP_CHECK(ordered.load());
  GO2CPP_CHECK(dropped.load() > 0);  // 满路径必须被真实走到
  GO2CPP_CHECK(ch->IsClosed());
}

// ---------------------------------------------------------------------------
// 用例 3：1e8 条阻塞 send/recv 无丢失（多流水线并行）
// ---------------------------------------------------------------------------
struct LaneResult {
  std::uint64_t sent = 0;
  std::uint64_t received = 0;
  std::uint64_t checksum = 0;
  bool ordered = true;
  bool send_ok = true;
};

void TestNoLossBlocking(std::uint64_t total_items, std::size_t lanes,
                        std::size_t capacity) {
  go2cpp_tests::announce("bounded_queue: 1e8-style no-loss blocking");
  GO2CPP_REQUIRE(lanes > 0);
  GO2CPP_REQUIRE(total_items % lanes == 0);
  const std::uint64_t per_lane = total_items / lanes;

  std::vector<go2cpp::ChannelPtr<Item>> channels;
  channels.reserve(lanes);
  for (std::size_t i = 0; i < lanes; ++i) {
    channels.push_back(go2cpp::MakeChannel<Item>(capacity));
  }
  std::vector<LaneResult> results(lanes);

  const auto start = Clock::now();
  std::vector<std::thread> producers;
  std::vector<std::thread> consumers;
  producers.reserve(lanes);
  consumers.reserve(lanes);

  for (std::size_t lane = 0; lane < lanes; ++lane) {
    go2cpp::Channel<Item>* ch = channels[lane].get();
    LaneResult* out = &results[lane];
    producers.emplace_back([ch, out, lane, per_lane] {
      for (std::uint64_t i = 0; i < per_lane; ++i) {
        const auto result =
            ch->Send(Item{static_cast<std::uint64_t>(lane), i});
        if (!result.Ok()) {
          out->send_ok = false;
          return;
        }
      }
      out->sent = per_lane;
      ch->Close();
    });
    consumers.emplace_back([ch, out] {
      std::uint64_t expected = 0;
      std::uint64_t sum = 0;
      bool ordered = true;
      while (true) {
        const auto result = ch->Recv();
        if (!result.ok) {
          break;
        }
        if (result.value->seq != expected) {
          ordered = false;
        }
        expected = result.value->seq + 1;
        sum += result.value->seq;
      }
      out->received = expected;
      out->checksum = sum;
      out->ordered = ordered;
    });
  }

  for (auto& thread : producers) {
    GO2CPP_JOIN_WITH_WATCHDOG(thread, std::chrono::seconds(600));
  }
  for (auto& thread : consumers) {
    GO2CPP_JOIN_WITH_WATCHDOG(thread, std::chrono::seconds(600));
  }

  const double seconds = SecondsSince(start);
  std::uint64_t total_sent = 0;
  std::uint64_t total_received = 0;
  for (std::size_t lane = 0; lane < lanes; ++lane) {
    const LaneResult& r = results[lane];
    const std::uint64_t expected_sum = per_lane * (per_lane - 1) / 2;
    GO2CPP_CHECK(r.send_ok);
    GO2CPP_CHECK(r.sent == per_lane);
    GO2CPP_CHECK(r.received == per_lane);
    GO2CPP_CHECK(r.ordered);
    GO2CPP_CHECK(r.checksum == expected_sum);
    total_sent += r.sent;
    total_received += r.received;
  }
  std::printf(
      "  lanes=%zu capacity=%zu total=%llu sent=%llu received=%llu "
      "elapsed=%.3fs rate=%.2f Mmsg/s\n",
      lanes, capacity, static_cast<unsigned long long>(total_items),
      static_cast<unsigned long long>(total_sent),
      static_cast<unsigned long long>(total_received), seconds,
      static_cast<double>(total_items) / seconds / 1e6);
  GO2CPP_CHECK(total_sent == total_items);
  GO2CPP_CHECK(total_received == total_items);
}

// ---------------------------------------------------------------------------
// 用例 4：RSS 稳定 —— 有界队列的内存不随消息数增长
// ---------------------------------------------------------------------------
void TestRssStable(std::uint64_t items, std::size_t capacity) {
  go2cpp_tests::announce("bounded_queue: RSS stability");
  auto ch = go2cpp::MakeChannel<Item>(capacity);

  const std::uint64_t baseline = ReadRssBytes();
  std::atomic<std::uint64_t> peak_rss{baseline};
  std::atomic<std::uint64_t> peak_len{0};
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
      if (count % 512 == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(30));
      }
    }
    consumed.store(count, std::memory_order_release);
  });

  // 生产者每 100 万条采样一次：观察队列长度与 RSS 的峰值。
  for (std::uint64_t i = 0; i < items; ++i) {
    const auto result = ch->Send(Item{0, i});
    GO2CPP_CHECK(result.Ok());
    if (i % 1'000'000ULL == 0) {
      const std::uint64_t len = ch->Len();
      const std::uint64_t rss = ReadRssBytes();
      if (len > peak_len.load(std::memory_order_relaxed)) {
        peak_len.store(len, std::memory_order_relaxed);
      }
      if (rss > peak_rss.load(std::memory_order_relaxed)) {
        peak_rss.store(rss, std::memory_order_relaxed);
      }
    }
  }
  ch->Close();
  GO2CPP_JOIN_WITH_WATCHDOG(consumer, std::chrono::seconds(600));

  const double seconds = SecondsSince(start);
  const std::uint64_t growth =
      peak_rss.load() > baseline ? peak_rss.load() - baseline : 0;
  std::printf(
      "  items=%llu capacity=%zu consumed=%llu peak_len=%llu "
      "baseline=%s peak_rss=%s growth=%s elapsed=%.3fs\n",
      static_cast<unsigned long long>(items), capacity,
      static_cast<unsigned long long>(consumed.load()),
      static_cast<unsigned long long>(peak_len.load()),
      FormatBytes(baseline).c_str(), FormatBytes(peak_rss.load()).c_str(),
      FormatBytes(growth).c_str(), seconds);

  GO2CPP_CHECK(consumed.load() == items);
  GO2CPP_CHECK(peak_len.load() <= capacity);
  if (baseline != 0) {
    // 有界队列本身只有 capacity 个槽位；RSS 的增长只应来自线程栈与
    // 分配器元数据。阈值取 32 MiB，远小于同样条数的无界队列。
    GO2CPP_CHECK(growth < 32ULL * 1024ULL * 1024ULL);
  }
}

// ---------------------------------------------------------------------------
// 用例 5：多生产者 + 单消费者，关闭后消费者能把缓冲排空
// ---------------------------------------------------------------------------
void TestMultiProducerClose() {
  go2cpp_tests::announce("bounded_queue: multi-producer + close drain");
  constexpr std::size_t kProducers = 4;
  constexpr std::uint64_t kPerProducer = 250'000;
  constexpr std::size_t kCapacity = 1024;
  auto ch = go2cpp::MakeChannel<Item>(kCapacity);

  std::vector<std::uint64_t> last_seen(kProducers, 0);
  std::vector<bool> seen_any(kProducers, false);
  std::atomic<bool> ordered{true};
  std::atomic<std::uint64_t> received{0};

  std::thread consumer([&] {
    std::uint64_t count = 0;
    while (true) {
      const auto result = ch->Recv();
      if (!result.ok) {
        break;
      }
      const std::size_t producer = static_cast<std::size_t>(result.value->producer);
      GO2CPP_REQUIRE(producer < kProducers);
      if (seen_any[producer]) {
        if (result.value->seq != last_seen[producer] + 1) {
          ordered.store(false, std::memory_order_relaxed);
        }
      } else if (result.value->seq != 0) {
        ordered.store(false, std::memory_order_relaxed);
      }
      last_seen[producer] = result.value->seq;
      seen_any[producer] = true;
      ++count;
    }
    received.store(count, std::memory_order_release);
  });

  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (std::size_t producer = 0; producer < kProducers; ++producer) {
    producers.emplace_back([ch = ch.get(), producer] {
      for (std::uint64_t i = 0; i < kPerProducer; ++i) {
        const auto result = ch->Send(
            Item{static_cast<std::uint64_t>(producer), i});
        GO2CPP_CHECK(result.Ok());
      }
    });
  }
  for (auto& thread : producers) {
    GO2CPP_JOIN_WITH_WATCHDOG(thread, std::chrono::seconds(120));
  }
  ch->Close();
  GO2CPP_JOIN_WITH_WATCHDOG(consumer, std::chrono::seconds(120));

  GO2CPP_CHECK(ordered.load());
  GO2CPP_CHECK(received.load() == kProducers * kPerProducer);
  GO2CPP_CHECK(ch->IsClosed());
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t no_loss_items = kDefaultNoLossItems;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--items") == 0 && i + 1 < argc) {
      no_loss_items = std::strtoull(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--help") == 0) {
      std::printf("usage: %s [--items N]  (default N=%llu)\n", argv[0],
                  static_cast<unsigned long long>(kDefaultNoLossItems));
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return 2;
    }
  }

  std::printf("[bounded_queue] items=%llu\n",
              static_cast<unsigned long long>(no_loss_items));

  TestCapacitySemantics();
  TestDropAccounting(2'000'000);
  TestMultiProducerClose();
  TestNoLossBlocking(no_loss_items, 4, 1024);
  TestRssStable(10'000'000, 1024);

  const int failures =
      go2cpp_tests::g_failures.load(std::memory_order_relaxed);
  if (failures != 0) {
    std::printf("[bounded_queue] FAILED: %d check(s)\n", failures);
    return 1;
  }
  std::printf("[bounded_queue] PASSED\n");
  return 0;
}
