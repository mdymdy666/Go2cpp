/**
 * @file stress_top_opt.cpp
 * @brief 并发 Top 优化点（#1 current_task_ptr、#2 WakeNodeLocked seq、
 *        #3 WaitNodeBase::task 零拷贝、#8 AddCancelCallback SBO）的独立压测。
 *
 * ## 为什么独立编译
 * 本文件不注册 CMake（避免改动构建描述），由命令直接编译：
 *   g++ -O2 -std=c++17 -I include tests/concurrency/stress_top_opt.cpp \
 *       .build-bento-wsl/libgo2cpp_*.so .build-bento-wsl/libgo2cpp_reflect.a \
 *       -Wl,-rpath,.build-bento-wsl -pthread -o /tmp/stress_top_opt
 *
 * ## 覆盖
 * - A1/A2/A3：1P1C 线程 send/recv，cap0/cap64/cap1024，严格顺序校验；
 * - B1/B2/B3：1P1C Fiber（同一 Scheduler）同上；
 * - C：Select + Close 并发（批量等待者，累计 1e7 次 select+close）；
 * - D：带 Context 的取消回调（AddCancelCallback 路径，累计 1e7 次）；
 * - E：Fiber × 线程混合（1 Fiber 生产者 + 1 线程生产者 + 1 线程消费者）。
 *
 * ## 断言
 * - 每个场景都校验「无丢失、无重复、顺序正确、状态正确」；
 * - 全局 watchdog：60 秒无进展直接 abort（证明无死锁）。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/context.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using go2cpp::ChannelStatus;
using go2cpp::Context;
using go2cpp::ContextPtr;
using go2cpp::MakeChannel;
using go2cpp::Scheduler;
using go2cpp::SchedulerConfig;

std::atomic<std::uint64_t> g_progress{0};
std::atomic<bool> g_stop{false};
std::atomic<std::uint64_t> g_failures{0};

/// @brief 跑满 idle_seconds 无进展就判定死锁并 abort。
void WatchdogMain(int idle_seconds) {
  std::uint64_t last = g_progress.load(std::memory_order_relaxed);
  int idle = 0;
  while (!g_stop.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(1s);
    const std::uint64_t now = g_progress.load(std::memory_order_relaxed);
    if (now == last) {
      if (++idle >= idle_seconds) {
        std::fprintf(stderr, "[stress] WATCHDOG: %d 秒无进展，判定死锁，abort\n",
                     idle_seconds);
        std::fflush(stderr);
        std::abort();
      }
    } else {
      idle = 0;
      last = now;
    }
  }
}

/// @brief 每 N 次操作推进一次进度（避免把 atomic 开销算进被测路径）。
inline void Bump(std::uint64_t step) {
  if ((step & 1023) == 0) {
    g_progress.fetch_add(1, std::memory_order_relaxed);
  }
}

void Report(const char* label, std::uint64_t total, double ns_per_op,
            std::uint64_t bad, const char* unit) {
  std::printf("  %-28s total=%-12llu %8.2f ns/op (%s) bad=%llu\n", label,
              static_cast<unsigned long long>(total), ns_per_op, unit,
              static_cast<unsigned long long>(bad));
  std::fflush(stdout);
  if (bad != 0) {
    g_failures.fetch_add(1, std::memory_order_relaxed);
  }
}

std::uint64_t ReadRssKb() {
  std::FILE* file = std::fopen("/proc/self/status", "r");
  if (file == nullptr) {
    return 0;
  }
  char line[256];
  std::uint64_t kb = 0;
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    if (std::strncmp(line, "VmRSS:", 6) == 0) {
      std::sscanf(line, "VmRSS: %llu",
                  reinterpret_cast<unsigned long long*>(&kb));
      break;
    }
  }
  std::fclose(file);
  return kb;
}

// ---------------------------------------------------------------------------
// A. 线程 1P1C：严格顺序
// ---------------------------------------------------------------------------
bool ThreadPair(std::size_t cap, std::uint64_t total, const char* label) {
  auto channel = MakeChannel<std::uint64_t>(cap);
  std::atomic<std::uint64_t> send_bad{0};
  std::atomic<std::uint64_t> recv_bad{0};
  std::atomic<bool> start{false};

  std::thread producer([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    std::uint64_t local_bad = 0;
    for (std::uint64_t i = 0; i < total; ++i) {
      if (channel->Send(i).status != ChannelStatus::kReady) {
        ++local_bad;
      }
      Bump(i);
    }
    send_bad.store(local_bad, std::memory_order_relaxed);
  });
  std::thread consumer([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    std::uint64_t expect = 0;
    std::uint64_t local_bad = 0;
    while (expect < total) {
      auto result = channel->Recv();
      if (result.status != ChannelStatus::kReady || !result.value.has_value()) {
        ++local_bad;
        break;
      }
      if (*result.value != expect) {
        ++local_bad;
        break;
      }
      ++expect;
      Bump(expect);
    }
    recv_bad.store(local_bad, std::memory_order_relaxed);
  });

  const auto start_time = Clock::now();
  start.store(true, std::memory_order_release);
  producer.join();
  consumer.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - start_time);
  const std::uint64_t bad = send_bad.load() + recv_bad.load();
  Report(label, total,
         static_cast<double>(elapsed.count()) / static_cast<double>(total), bad,
         "thread 1P1C");
  return bad == 0;
}

// ---------------------------------------------------------------------------
// B. Fiber 1P1C：严格顺序
// ---------------------------------------------------------------------------
bool FiberPair(std::size_t cap, std::uint64_t total, const char* label) {
  auto channel = MakeChannel<std::uint64_t>(cap);
  SchedulerConfig config;
  config.processor_count = 1;
  config.max_workers = 1;
  config.idle_wait = 1ms;
  Scheduler scheduler(config);
  scheduler.start();

  std::atomic<std::uint64_t> send_bad{0};
  std::atomic<std::uint64_t> recv_bad{0};
  const auto start_time = Clock::now();
  auto producer = scheduler.spawn([&channel, total, &send_bad] {
    std::uint64_t local_bad = 0;
    for (std::uint64_t i = 0; i < total; ++i) {
      if (channel->Send(i).status != ChannelStatus::kReady) {
        ++local_bad;
      }
      Bump(i);
    }
    send_bad.store(local_bad, std::memory_order_relaxed);
  });
  auto consumer = scheduler.spawn([&channel, total, &recv_bad] {
    std::uint64_t expect = 0;
    std::uint64_t local_bad = 0;
    while (expect < total) {
      auto result = channel->Recv();
      if (result.status != ChannelStatus::kReady || !result.value.has_value() ||
          *result.value != expect) {
        ++local_bad;
        break;
      }
      ++expect;
      Bump(expect);
    }
    recv_bad.store(local_bad, std::memory_order_relaxed);
  });
  const bool ok = producer->wait_for(600s) && consumer->wait_for(600s);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - start_time);
  scheduler.shutdown();
  const std::uint64_t bad = send_bad.load() + recv_bad.load() + (ok ? 0 : 1);
  Report(label, total,
         static_cast<double>(elapsed.count()) / static_cast<double>(total), bad,
         "fiber 1P1C");
  return bad == 0;
}

// ---------------------------------------------------------------------------
// C. Select + Close 并发（批量等待者，累计 total_ops 次）
// ---------------------------------------------------------------------------
bool SelectClose(std::uint64_t total_ops, std::uint64_t batch) {
  SchedulerConfig config;
  config.processor_count = 4;
  config.max_workers = 4;
  config.idle_wait = 1ms;
  Scheduler scheduler(config);
  scheduler.start();

  std::atomic<std::uint64_t> bad{0};
  std::atomic<std::uint64_t> closed_seen{0};
  const std::uint64_t rounds = total_ops / batch;
  const auto start_time = Clock::now();
  for (std::uint64_t round = 0; round < rounds; ++round) {
    std::atomic<std::uint64_t> entered{0};
    std::vector<std::shared_ptr<go2cpp::Channel<int>>> channels(batch);
    std::vector<std::shared_ptr<go2cpp::Task>> waiters;
    waiters.reserve(batch);
    for (auto& channel : channels) {
      channel = MakeChannel<int>(0);
    }
    for (std::size_t i = 0; i < batch; ++i) {
      auto channel = channels[i];
      waiters.push_back(scheduler.spawn([channel, &entered, &bad,
                                         &closed_seen] {
        entered.fetch_add(1, std::memory_order_relaxed);
        auto result =
            go2cpp::Select({go2cpp::RecvCase(channel)}, {}, std::nullopt);
        if (result.status == ChannelStatus::kClosed) {
          closed_seen.fetch_add(1, std::memory_order_relaxed);
        } else {
          bad.fetch_add(1, std::memory_order_relaxed);
        }
      }));
    }
    while (entered.load(std::memory_order_acquire) < batch) {
      std::this_thread::yield();
    }
    for (auto& channel : channels) {
      channel->Close();
    }
    for (auto& waiter : waiters) {
      if (!waiter->wait_for(10s)) {
        bad.fetch_add(1, std::memory_order_relaxed);
      }
    }
    Bump(round);
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - start_time);
  scheduler.shutdown();
  const std::uint64_t bad_count = bad.load();
  Report("select+close", rounds * batch,
         static_cast<double>(elapsed.count()) /
             static_cast<double>(rounds * batch),
         bad_count, "op");
  std::printf("  select+close: closed_seen=%llu rounds=%llu batch=%llu\n",
              static_cast<unsigned long long>(closed_seen.load()),
              static_cast<unsigned long long>(rounds),
              static_cast<unsigned long long>(batch));
  if (closed_seen.load() != rounds * batch) {
    return false;
  }
  return bad_count == 0;
}

// ---------------------------------------------------------------------------
// D. 带 Context 的取消回调（AddCancelCallback 路径，批量，累计 total_ops 次）
// ---------------------------------------------------------------------------
bool CancelCallbacks(std::uint64_t total_ops, std::uint64_t batch) {
  SchedulerConfig config;
  config.processor_count = 4;
  config.max_workers = 4;
  config.idle_wait = 1ms;
  Scheduler scheduler(config);
  scheduler.start();

  const std::uint64_t rss_before = ReadRssKb();
  std::atomic<std::uint64_t> bad{0};
  std::atomic<std::uint64_t> cancelled{0};
  const std::uint64_t rounds = total_ops / batch;
  const auto start_time = Clock::now();
  for (std::uint64_t round = 0; round < rounds; ++round) {
    std::atomic<std::uint64_t> entered{0};
    std::vector<std::shared_ptr<go2cpp::Channel<int>>> channels(batch);
    std::vector<ContextPtr> contexts;
    std::vector<std::function<void()>> cancels;
    std::vector<std::shared_ptr<go2cpp::Task>> waiters;
    contexts.reserve(batch);
    cancels.reserve(batch);
    waiters.reserve(batch);
    for (std::size_t i = 0; i < batch; ++i) {
      channels[i] = MakeChannel<int>(0);
      auto context_and_cancel = Context::WithCancel(nullptr);
      contexts.push_back(context_and_cancel.first);
      cancels.push_back(std::move(context_and_cancel.second));
    }
    for (std::size_t i = 0; i < batch; ++i) {
      auto channel = channels[i];
      ContextPtr context = contexts[i];
      waiters.push_back(
          scheduler.spawn([channel, context, &entered, &bad, &cancelled] {
            entered.fetch_add(1, std::memory_order_relaxed);
            auto result = channel->Recv(context);
            if (result.status == ChannelStatus::kCancelled) {
              cancelled.fetch_add(1, std::memory_order_relaxed);
            } else {
              bad.fetch_add(1, std::memory_order_relaxed);
            }
          }));
    }
    while (entered.load(std::memory_order_acquire) < batch) {
      std::this_thread::yield();
    }
    for (auto& cancel : cancels) {
      cancel();
    }
    for (auto& waiter : waiters) {
      if (!waiter->wait_for(10s)) {
        bad.fetch_add(1, std::memory_order_relaxed);
      }
    }
    Bump(round);
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - start_time);
  scheduler.shutdown();
  const std::uint64_t rss_after = ReadRssKb();
  const std::uint64_t bad_count = bad.load();
  Report("cancel-callback", rounds * batch,
         static_cast<double>(elapsed.count()) /
             static_cast<double>(rounds * batch),
         bad_count, "op");
  std::printf("  cancel-callback: cancelled=%llu rss_before=%lluKB "
              "rss_after=%lluKB\n",
              static_cast<unsigned long long>(cancelled.load()),
              static_cast<unsigned long long>(rss_before),
              static_cast<unsigned long long>(rss_after));
  if (rss_after > rss_before + 65536) {
    std::fprintf(stderr, "  cancel-callback: RSS 增长超过 64MB，疑似泄漏\n");
    return false;
  }
  return bad_count == 0 && cancelled.load() == rounds * batch;
}

// ---------------------------------------------------------------------------
// E. Fiber × 线程混合：1 个 Fiber 生产者 + 1 个线程生产者 + 1 个线程消费者
// ---------------------------------------------------------------------------
bool MixedFiberThread(std::uint64_t total) {
  constexpr std::uint64_t kProducers = 2;
  const std::uint64_t per_producer = total / kProducers;
  auto channel = MakeChannel<std::uint64_t>(1024);
  std::atomic<std::uint64_t> bad{0};
  std::atomic<std::uint64_t> started{0};

  SchedulerConfig config;
  config.processor_count = 1;
  config.max_workers = 1;
  config.idle_wait = 1ms;
  Scheduler scheduler(config);
  scheduler.start();

  const auto start_time = Clock::now();
  auto fiber_producer = scheduler.spawn([&, per_producer] {
    started.fetch_add(1, std::memory_order_release);
    for (std::uint64_t i = 0; i < per_producer; ++i) {
      if (channel->Send((0ULL << 56) | i).status != ChannelStatus::kReady) {
        bad.fetch_add(1, std::memory_order_relaxed);
      }
      Bump(i);
    }
  });
  std::thread thread_producer([&] {
    started.fetch_add(1, std::memory_order_release);
    for (std::uint64_t i = 0; i < per_producer; ++i) {
      if (channel->Send((1ULL << 56) | i).status != ChannelStatus::kReady) {
        bad.fetch_add(1, std::memory_order_relaxed);
      }
      Bump(i);
    }
  });
  std::thread consumer([&] {
    std::vector<std::uint64_t> expect(kProducers, 0);
    std::uint64_t received = 0;
    while (received < total) {
      auto result = channel->Recv();
      if (result.status != ChannelStatus::kReady || !result.value.has_value()) {
        bad.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      const std::uint64_t value = *result.value;
      const std::uint64_t p = value >> 56;
      if (p >= kProducers || (value & 0x00FFFFFFFFFFFFFFULL) != expect[p]) {
        bad.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      ++expect[p];
      ++received;
      Bump(received);
    }
  });
  while (started.load(std::memory_order_acquire) < kProducers) {
    std::this_thread::yield();
  }
  const bool ok = fiber_producer->wait_for(600s);
  thread_producer.join();
  consumer.join();
  scheduler.shutdown();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - start_time);
  const std::uint64_t bad_count = bad.load();
  Report("mixed fiber+thread", total,
         static_cast<double>(elapsed.count()) / static_cast<double>(total),
         bad_count + (ok ? 0 : 1), "2P/1C");
  return bad_count == 0 && ok;
}

// ---------------------------------------------------------------------------
// F. 带 Context 的阻塞往返（隔离 AddCancelCallback 的注册/注销成本）
// ---------------------------------------------------------------------------
bool ContextRoundTrip(std::uint64_t rounds) {
  auto channel = MakeChannel<std::uint64_t>(0);
  SchedulerConfig config;
  config.processor_count = 1;
  config.max_workers = 1;
  config.idle_wait = 1ms;
  Scheduler scheduler(config);
  scheduler.start();

  auto context_and_cancel = Context::WithCancel(nullptr);
  ContextPtr context = context_and_cancel.first;
  std::atomic<std::uint64_t> bad{0};
  const auto start_time = Clock::now();
  auto producer = scheduler.spawn([&channel, &context, rounds, &bad] {
    for (std::uint64_t i = 0; i < rounds; ++i) {
      if (channel->Send(i, context).status != ChannelStatus::kReady) {
        bad.fetch_add(1, std::memory_order_relaxed);
      }
      Bump(i);
    }
  });
  auto consumer = scheduler.spawn([&channel, &context, rounds, &bad] {
    for (std::uint64_t i = 0; i < rounds; ++i) {
      auto result = channel->Recv(context);
      if (result.status != ChannelStatus::kReady || !result.value.has_value() ||
          *result.value != i) {
        bad.fetch_add(1, std::memory_order_relaxed);
      }
      Bump(i);
    }
  });
  const bool ok = producer->wait_for(600s) && consumer->wait_for(600s);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - start_time);
  scheduler.shutdown();
  const std::uint64_t bad_count = bad.load() + (ok ? 0 : 1);
  Report("context roundtrip", rounds,
         static_cast<double>(elapsed.count()) / static_cast<double>(rounds),
         bad_count, "fiber 1P1C +ctx");
  return bad_count == 0;
}

}  // namespace

int main(int argc, char** argv) {
  const bool quick = argc > 1 && std::strcmp(argv[1], "quick") == 0;
  const std::uint64_t stream_total = quick ? 2000000ULL : 100000000ULL;
  const std::uint64_t mixed_ops = quick ? 2000000ULL : 10000000ULL;
  const std::uint64_t select_ops = quick ? 100000ULL : 10000000ULL;
  const std::uint64_t cancel_ops = quick ? 100000ULL : 10000000ULL;
  const std::uint64_t batch = quick ? 16 : 128;

  std::printf("=== 并发 Top 优化点压测（%s）===\n", quick ? "quick" : "full");
  std::printf(
      "stream_total=%llu mixed=%llu select=%llu cancel=%llu batch=%llu\n\n",
      static_cast<unsigned long long>(stream_total),
      static_cast<unsigned long long>(mixed_ops),
      static_cast<unsigned long long>(select_ops),
      static_cast<unsigned long long>(cancel_ops),
      static_cast<unsigned long long>(batch));

  std::thread watchdog(WatchdogMain, quick ? 20 : 60);

  const char* scene_arg = argc > 2 ? argv[2] : "all";
  const bool all = std::strcmp(scene_arg, "all") == 0;
  auto run = [&](const char* name) {
    return all || std::strcmp(scene_arg, name) == 0;
  };

  bool ok = true;
  if (run("stream")) {
  std::printf("[A] 线程 1P1C send/recv\n");
  ok = ThreadPair(64, stream_total, "thread cap64") && ok;
  ok = ThreadPair(1024, stream_total, "thread cap1024") && ok;
  ok = ThreadPair(0, quick ? stream_total / 4 : 20000000ULL,
                  "thread cap0") && ok;

  std::printf("\n[B] Fiber 1P1C send/recv\n");
  ok = FiberPair(64, stream_total, "fiber cap64") && ok;
  ok = FiberPair(1024, stream_total, "fiber cap1024") && ok;
  ok = FiberPair(0, stream_total, "fiber cap0") && ok;
  }

  if (run("select")) {
  std::printf("\n[C] Select + Close 并发\n");
  ok = SelectClose(select_ops, batch) && ok;
  }

  if (run("cancel")) {
  std::printf("\n[D] 带 Context 的取消回调\n");
  ok = CancelCallbacks(cancel_ops, batch) && ok;
  }

  if (run("mixed")) {
  std::printf("\n[E] Fiber × 线程混合\n");
  ok = MixedFiberThread(mixed_ops) && ok;
  }

  if (run("roundtrip")) {
    std::printf("\n[F] 带 Context 的阻塞往返（AddCancelCallback 路径）\n");
    ok = ContextRoundTrip(quick ? 1000000ULL : 10000000ULL) && ok;
  }

  g_stop.store(true, std::memory_order_release);
  watchdog.join();

  std::printf("\n[汇总] %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
