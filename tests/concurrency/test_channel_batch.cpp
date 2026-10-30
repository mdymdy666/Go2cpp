/**
 * @file test_channel_batch.cpp
 * @brief Channel 实验性批量 API（TrySendBatch/TryRecvBatch）的正确性测试。
 *
 * ## 为什么独立
 * `Channel` 的批量 API 是本次性能优化引入的实验性接口（见 channel.hpp 的
 * `TrySendBatch` / `TryRecvBatch`）。现有 ctest 用例只覆盖单条路径，本文件
 * 覆盖批量路径的边界、与单条路径混用、关闭、等待者回退、多生产者多消费者
 * 与随机压测。手动编译运行时需要定义 `GO2CPP_CHANNEL_BATCH_STANDALONE`：
 *
 * ```
 * g++ -std=c++20 -O2 -g -DGO2CPP_CHANNEL_BATCH_STANDALONE \
 *     -Iinclude tests/concurrency/test_channel_batch.cpp \
 *     -L.build-bento-wsl -lgo2cpp_channel -lgo2cpp_context \
 *     -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_error -lgo2cpp_log -pthread \
 *     -o .build-bento-wsl/test_channel_batch
 * LD_LIBRARY_PATH=$PWD/.build-bento-wsl .build-bento-wsl/test_channel_batch
 * ```
 */

#include "go2cpp/channel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using go2cpp::ChannelStatus;
using go2cpp::MakeChannel;

std::atomic<int> g_failures{0};

void Check(bool condition, const char* expression, const char* file, int line) {
  if (condition) {
    return;
  }
  g_failures.fetch_add(1, std::memory_order_relaxed);
  std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

/// 自旋等待条件成立（测试用，最多 2s）。
template <typename Fn>
bool WaitUntil(Fn&& predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::yield();
  }
  return predicate();
}

// ---------------------------------------------------------------------------
// 1. 基本顺序：批量发送/接收必须保持 FIFO，Len 精确。
// ---------------------------------------------------------------------------
void TestBasicOrder() {
  auto channel = MakeChannel<int>(64);
  std::vector<int> input{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  CHECK(channel->TrySendBatch(input.data(), input.size()) == 10U);
  CHECK(channel->Len() == 10U);

  std::vector<int> output(10, -1);
  CHECK(channel->TryRecvBatch(output.data(), output.size()) == 10U);
  CHECK(output == input);
  CHECK(channel->Len() == 0U);
  // 空通道上的批量接收返回 0，不阻塞。
  CHECK(channel->TryRecvBatch(output.data(), output.size()) == 0U);
}

// ---------------------------------------------------------------------------
// 2. 容量边界：一次批量超过容量时只写入容量条，再发返回 0。
// ---------------------------------------------------------------------------
void TestCapacityBoundary() {
  auto channel = MakeChannel<int>(4);
  std::vector<int> input{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  CHECK(channel->TrySendBatch(input.data(), input.size()) == 4U);
  CHECK(channel->Len() == 4U);
  CHECK(channel->TrySendBatch(input.data() + 4, 6) == 0U);

  std::vector<int> output(10, -1);
  CHECK(channel->TryRecvBatch(output.data(), output.size()) == 4U);
  CHECK(output[0] == 0 && output[1] == 1 && output[2] == 2 && output[3] == 3);
  CHECK(channel->TrySendBatch(input.data() + 4, 6) == 4U);
  std::fill(output.begin(), output.end(), -1);
  CHECK(channel->TryRecvBatch(output.data(), output.size()) == 4U);
  CHECK(output[0] == 4 && output[1] == 5 && output[2] == 6 && output[3] == 7);
}

// ---------------------------------------------------------------------------
// 3. 与单条 API 混用：FIFO 顺序与逐条调用完全一致。
// ---------------------------------------------------------------------------
void TestMixedSingleAndBatch() {
  auto channel = MakeChannel<int>(8);
  const int batch_values[] = {0, 1, 2, 3, 4};
  CHECK(channel->TrySendBatch(batch_values, 5) == 5U);
  CHECK(channel->TrySend(5).Ok());
  CHECK(channel->TrySend(6).Ok());
  CHECK(channel->Len() == 7U);

  CHECK(channel->TryRecv().ValueOrDefault() == 0);
  std::vector<int> output(10, -1);
  CHECK(channel->TryRecvBatch(output.data(), output.size()) == 6U);
  CHECK(output[0] == 1 && output[1] == 2 && output[2] == 3 && output[3] == 4 &&
        output[4] == 5 && output[5] == 6);
  CHECK(channel->TryRecvBatch(output.data(), output.size()) == 0U);
}

// ---------------------------------------------------------------------------
// 4. 关闭语义：关闭后批量发送返回 0；批量接收先排空缓冲，再返回 0。
// ---------------------------------------------------------------------------
void TestClosedChannel() {
  auto channel = MakeChannel<int>(8);
  const int values[] = {10, 11, 12};
  CHECK(channel->TrySendBatch(values, 3) == 3U);
  CHECK(channel->Close().Ok());
  CHECK(channel->TrySendBatch(values, 3) == 0U);

  std::vector<int> output(8, -1);
  CHECK(channel->TryRecvBatch(output.data(), output.size()) == 3U);
  CHECK(output[0] == 10 && output[1] == 11 && output[2] == 12);
  CHECK(channel->TryRecvBatch(output.data(), output.size()) == 0U);
  // 已关闭的通道上单条发送返回关闭状态，批量不会把它误当成成功。
  const auto send = channel->Send(13, {});
  CHECK(send.status == ChannelStatus::kClosed);
}

// ---------------------------------------------------------------------------
// 5. 等待者回退：有接收者 park 时批量发送返回 0（不抢占等待者），
//    随后单条 Send 必须完成交接。
// ---------------------------------------------------------------------------
void TestWaiterFallback() {
  auto channel = MakeChannel<int>(8);
  std::atomic<int> received{-1};
  std::thread receiver([&] {
    const auto result = channel->Recv();
    received.store(result.ok ? *result.value : -2,
                   std::memory_order_release);
  });
  CHECK(WaitUntil([&] {
    return channel->DebugSnapshot().recv_waiters > 0;
  }));
  const int values[] = {1, 2, 3};
  CHECK(channel->TrySendBatch(values, 3) == 0U);
  CHECK(channel->Send(42).Ok());
  receiver.join();
  CHECK(received.load(std::memory_order_acquire) == 42);
  // 等待者被单条 Send 交接后，缓冲仍为空。
  CHECK(channel->Len() == 0U);
}

// ---------------------------------------------------------------------------
// 6. MPMC：4 生产者 × 4 消费者，批量混合单条，校验不丢不重。
// ---------------------------------------------------------------------------
void TestMpmcBatch() {
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr std::uint64_t kPerProducer = 20000;
  constexpr std::uint64_t kTotal = kProducers * kPerProducer;

  auto channel = MakeChannel<std::uint64_t>(256);
  std::atomic<std::uint64_t> produced{0};
  std::atomic<std::uint64_t> consumed{0};
  std::atomic<std::uint64_t> duplicates{0};
  std::mutex collect_mutex;
  std::vector<std::uint64_t> collected;
  collected.reserve(kTotal);

  std::vector<std::thread> threads;
  for (int p = 0; p < kProducers; ++p) {
    threads.emplace_back([&, p] {
      std::mt19937 rng(static_cast<std::uint32_t>(p) * 7919U + 17U);
      std::uniform_int_distribution<int> batch_size(1, 24);
      std::uint64_t next = static_cast<std::uint64_t>(p) * kPerProducer;
      const std::uint64_t end = next + kPerProducer;
      std::vector<std::uint64_t> batch;
      while (next < end) {
        const std::uint64_t remaining = end - next;
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(
            static_cast<std::uint64_t>(batch_size(rng)), remaining));
        batch.resize(want);
        for (std::size_t i = 0; i < want; ++i) {
          batch[i] = next + i;
        }
        std::size_t offset = 0;
        while (offset < want) {
          const std::size_t sent =
              channel->TrySendBatch(batch.data() + offset, want - offset);
          if (sent == 0) {
            if (!channel->TrySend(batch[offset]).Ok()) {
              std::this_thread::yield();
              continue;
            }
            ++offset;
            produced.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
          offset += sent;
          produced.fetch_add(sent, std::memory_order_relaxed);
        }
        next += want;
      }
    });
  }
  for (int c = 0; c < kConsumers; ++c) {
    threads.emplace_back([&] {
      std::mt19937 rng(0xC0FFEEU + static_cast<std::uint32_t>(c));
      std::uniform_int_distribution<int> batch_size(1, 24);
      std::vector<std::uint64_t> batch(32);
      std::vector<std::uint64_t> local;
      local.reserve(kTotal / kConsumers);
      while (true) {
        const auto current = consumed.load(std::memory_order_relaxed);
        if (current >= kTotal) {
          break;
        }
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(
            static_cast<std::uint64_t>(batch_size(rng)),
            kTotal - current));
        std::size_t got = channel->TryRecvBatch(batch.data(), want);
        if (got == 0) {
          const auto single = channel->TryRecv();
          if (single.ok) {
            batch[0] = *single.value;
            got = 1;
          } else {
            std::this_thread::yield();
            continue;
          }
        }
        for (std::size_t i = 0; i < got; ++i) {
          local.push_back(batch[i]);
        }
        consumed.fetch_add(got, std::memory_order_relaxed);
      }
      std::lock_guard<std::mutex> lock(collect_mutex);
      collected.insert(collected.end(), local.begin(), local.end());
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  CHECK(produced.load(std::memory_order_relaxed) == kTotal);
  CHECK(consumed.load(std::memory_order_relaxed) == kTotal);
  CHECK(collected.size() == kTotal);
  std::sort(collected.begin(), collected.end());
  bool exact = collected.size() == kTotal;
  if (exact) {
    for (std::uint64_t i = 0; i < kTotal; ++i) {
      if (collected[i] != i) {
        exact = false;
        break;
      }
    }
  }
  if (!exact) {
    duplicates.fetch_add(1, std::memory_order_relaxed);
  }
  CHECK(exact);
}

// ---------------------------------------------------------------------------
// 7. 随机混合压测：随机容量、随机 batch、随机单条/批量混用。
// ---------------------------------------------------------------------------
void TestRandomStress() {
  std::mt19937 rng(20251030U);
  std::uniform_int_distribution<int> capacity_dist(1, 64);
  std::uniform_int_distribution<int> batch_dist(1, 16);
  std::uniform_int_distribution<int> mode_dist(0, 3);

  for (int round = 0; round < 20; ++round) {
    const std::size_t capacity =
        static_cast<std::size_t>(capacity_dist(rng));
    auto channel = MakeChannel<int>(capacity);
    constexpr int kItems = 2000;
    std::vector<int> received;
    received.reserve(kItems + 8);
    std::atomic<int> sent_total{0};

    std::thread producer([&] {
      int next = 0;
      while (next < kItems) {
        const std::size_t want = static_cast<std::size_t>(std::min<int>(
            batch_dist(rng), kItems - next));
        if (mode_dist(rng) == 0) {
          if (channel->TrySend(next).Ok()) {
            ++next;
            sent_total.fetch_add(1, std::memory_order_relaxed);
          } else {
            std::this_thread::yield();
          }
          continue;
        }
        std::vector<int> batch(want);
        for (std::size_t i = 0; i < want; ++i) {
          batch[i] = next + static_cast<int>(i);
        }
        std::size_t offset = 0;
        while (offset < want) {
          const std::size_t sent =
              channel->TrySendBatch(batch.data() + offset, want - offset);
          if (sent == 0) {
            if (!channel->TrySend(batch[offset]).Ok()) {
              std::this_thread::yield();
              continue;
            }
            sent_total.fetch_add(1, std::memory_order_relaxed);
            ++offset;
            continue;
          }
          sent_total.fetch_add(sent, std::memory_order_relaxed);
          offset += sent;
        }
        next += static_cast<int>(want);
      }
    });

    std::thread consumer([&] {
      std::vector<int> batch(16);
      while (true) {
        const auto got = channel->TryRecvBatch(batch.data(), batch.size());
        if (got > 0) {
          for (std::size_t i = 0; i < got; ++i) {
            received.push_back(batch[i]);
          }
          continue;
        }
        const auto single = channel->TryRecv();
        if (single.ok) {
          received.push_back(*single.value);
          continue;
        }
        if (channel->IsClosed() && received.size() >= kItems) {
          break;
        }
        std::this_thread::yield();
      }
    });

    // 等消费者把所有值取走后再关闭，避免关闭丢失在途值。
    CHECK(WaitUntil([&] { return received.size() >= kItems; }));
    producer.join();
    channel->Close();
    consumer.join();

    CHECK(sent_total.load(std::memory_order_relaxed) == kItems);
    CHECK(received.size() == static_cast<std::size_t>(kItems));
    bool ordered = received.size() == static_cast<std::size_t>(kItems);
    for (std::size_t i = 0; ordered && i < received.size(); ++i) {
      if (received[i] != static_cast<int>(i)) {
        ordered = false;
      }
    }
    CHECK(ordered);
  }
}

// ---------------------------------------------------------------------------
// 8. 真实场景：生产者批量生成、消费者批量处理、关闭排空（流水线）。
// ---------------------------------------------------------------------------
void TestPipelineScenario() {
  constexpr int kRecords = 5000;
  auto channel = MakeChannel<int>(128);
  std::atomic<std::uint64_t> sum{0};
  std::atomic<int> processed{0};
  std::atomic<bool> closed_seen{false};

  std::thread consumer([&] {
    std::vector<int> batch(16);
    while (true) {
      const auto got = channel->TryRecvBatch(batch.data(), batch.size());
      if (got > 0) {
        for (std::size_t i = 0; i < got; ++i) {
          sum.fetch_add(static_cast<std::uint64_t>(batch[i]),
                        std::memory_order_relaxed);
        }
        processed.fetch_add(static_cast<int>(got), std::memory_order_relaxed);
        continue;
      }
      if (channel->IsClosed()) {
        // 关闭后把剩余值排空，再退出。
        const auto drain = channel->TryRecvBatch(batch.data(), batch.size());
        if (drain == 0) {
          closed_seen.store(true, std::memory_order_relaxed);
          break;
        }
        for (std::size_t i = 0; i < drain; ++i) {
          sum.fetch_add(static_cast<std::uint64_t>(batch[i]),
                        std::memory_order_relaxed);
        }
        processed.fetch_add(static_cast<int>(drain), std::memory_order_relaxed);
        continue;
      }
      std::this_thread::yield();
    }
  });

  std::vector<int> batch(16);
  for (int next = 0; next < kRecords;) {
    const std::size_t want = static_cast<std::size_t>(
        std::min<int>(16, kRecords - next));
    for (std::size_t i = 0; i < want; ++i) {
      batch[i] = next + static_cast<int>(i);
    }
    std::size_t offset = 0;
    while (offset < want) {
      const std::size_t sent =
          channel->TrySendBatch(batch.data() + offset, want - offset);
      if (sent == 0) {
        if (!channel->TrySend(batch[offset]).Ok()) {
          std::this_thread::yield();
          continue;
        }
        ++offset;
        continue;
      }
      offset += sent;
    }
    next += static_cast<int>(want);
  }
  channel->Close();
  consumer.join();

  const std::uint64_t expected =
      static_cast<std::uint64_t>(kRecords - 1) * kRecords / 2U;
  CHECK(processed.load(std::memory_order_relaxed) == kRecords);
  CHECK(sum.load(std::memory_order_relaxed) == expected);
  CHECK(closed_seen.load(std::memory_order_relaxed));
}

}  // namespace

// CMake 已把本文件加入 go2cpp_tests（与 tests/test_main.cpp 共用进程），
// 因此默认不生成 main；需要独立运行时手动定义
// -DGO2CPP_CHANNEL_BATCH_STANDALONE 并链接 go2cpp_channel 等库。
#if defined(GO2CPP_CHANNEL_BATCH_STANDALONE)
#ifdef GO2CPP_CHANNEL_BATCH_STANDALONE
int main() {
  std::printf("[channel-batch] 开始批量 API 正确性测试\n");
  TestBasicOrder();
  TestCapacityBoundary();
  TestMixedSingleAndBatch();
  TestClosedChannel();
  TestWaiterFallback();
  TestMpmcBatch();
  TestRandomStress();
  TestPipelineScenario();
  const int failures = g_failures.load(std::memory_order_relaxed);
  std::printf("[channel-batch] failures=%d%s\n", failures,
              failures == 0 ? "（通过）" : "（失败）");
  return failures == 0 ? 0 : 1;
}
#endif  // GO2CPP_CHANNEL_BATCH_STANDALONE
#endif  // GO2CPP_CHANNEL_BATCH_STANDALONE
