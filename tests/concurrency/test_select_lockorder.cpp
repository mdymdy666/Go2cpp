// Select lockorder（对齐 Go selectgo 的 sellock）专项测试。
//
// 覆盖点：
// - L1 `SelectLockSet` 的排序、去重、RAII 释放与多线程反向登记压力；
// - L2 两个原生线程以相反 case 顺序 select 同一组 channel（锁序死锁的
//      经典形状）；
// - L3 两个 Fiber 上的同一场景；
// - L4 同一个 channel 在一个 select 中同时作为 recv/send（锁去重，不能自锁）；
// - L5 随机 case 组合 / 顺序 / default / AfterCase 的多 Fiber 压力；
// - L6 select 进行中关闭 channel + 取消 context 的拆除路径；
// - L7 原生线程与 Fiber 混合收发；
// - L8 三 channel、相反 case 顺序的长期配对。
//
// 所有阻塞场景都带看门狗：超时即打印 FAIL 并 `std::_Exit(2)`，避免测试进程
// 挂死 CI 而不是给出可读的失败。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/runtime.hpp"

using namespace go2cpp;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", name);
  std::fflush(stdout);
  if (!ok) {
    ++g_failures;
  }
}

// 看门狗：场景超过 limit 未析构即认定挂起。线程只读 m_done 原子量。
class Watchdog final {
 public:
  explicit Watchdog(std::chrono::seconds limit, const char* name)
      : m_limit(limit), m_name(name), m_thread([this] { Run(); }) {}

  ~Watchdog() {
    m_done.store(true);
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  Watchdog(const Watchdog&) = delete;
  Watchdog& operator=(const Watchdog&) = delete;

 private:
  void Run() {
    const auto deadline = std::chrono::steady_clock::now() + m_limit;
    while (!m_done.load(std::memory_order_relaxed)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        std::printf("[FAIL] %s：超过 %lld 秒未完成，判定死锁/挂起\n", m_name,
                    static_cast<long long>(m_limit.count()));
        std::fflush(stdout);
        std::_Exit(2);
      }
      std::this_thread::sleep_for(10ms);
    }
  }

  std::chrono::seconds m_limit;
  const char* m_name;
  std::atomic<bool> m_done{false};
  std::thread m_thread;
};

// ---------------------------------------------------------------- L1 锁集单元

std::vector<const void*> g_lock_log;

void LockLogged(const void* key) noexcept {
  try {
    g_lock_log.push_back(key);
  } catch (...) {
  }
  auto* mutex = const_cast<std::mutex*>(static_cast<const std::mutex*>(key));
  try {
    mutex->lock();
  } catch (...) {
  }
}

void UnlockLogged(const void* key) noexcept {
  auto* mutex = const_cast<std::mutex*>(static_cast<const std::mutex*>(key));
  try {
    mutex->unlock();
  } catch (...) {
  }
}

detail::SelectLockEntry MakeEntry(const std::mutex* mutex) {
  return detail::SelectLockEntry{mutex, &LockLogged, &UnlockLogged};
}

// 多线程压力用：不写共享日志（g_lock_log 非线程安全），只做加解锁。
void LockQuiet(const void* key) noexcept {
  auto* mutex = const_cast<std::mutex*>(static_cast<const std::mutex*>(key));
  try {
    mutex->lock();
  } catch (...) {
  }
}

void UnlockQuiet(const void* key) noexcept {
  auto* mutex = const_cast<std::mutex*>(static_cast<const std::mutex*>(key));
  try {
    mutex->unlock();
  } catch (...) {
  }
}

detail::SelectLockEntry MakeQuietEntry(const std::mutex* mutex) {
  return detail::SelectLockEntry{mutex, &LockQuiet, &UnlockQuiet};
}

void TestLockSetOrdering() {
  std::mutex m1;
  std::mutex m2;
  std::mutex m3;
  std::vector<const void*> expected{&m1, &m2, &m3};
  std::sort(expected.begin(), expected.end());

  // 两种相反的登记顺序，加锁顺序必须都等于按地址升序。
  std::vector<const void*> orders[2];
  bool all_held = true;
  for (int variant = 0; variant < 2; ++variant) {
    g_lock_log.clear();
    {
      detail::SelectLockSet locks;
      if (variant == 0) {
        locks.Add(MakeEntry(&m3));
        locks.Add(MakeEntry(&m1));
        locks.Add(MakeEntry(&m2));
      } else {
        locks.Add(MakeEntry(&m2));
        locks.Add(MakeEntry(&m3));
        locks.Add(MakeEntry(&m1));
      }
      locks.Acquire();
      orders[variant] = g_lock_log;
      all_held = all_held && locks.LockedCount() == 3;
    }
  }
  Check(all_held, "L1.1 三个 channel 锁全部持有");
  Check(orders[0] == expected && orders[1] == expected,
        "L1.2 相反登记顺序得到同一加锁顺序（lockorder 全序）");

  // 同一把锁重复登记必须去重，否则自旋锁不可重入会自锁。
  {
    g_lock_log.clear();
    detail::SelectLockSet locks;
    locks.Add(MakeEntry(&m1));
    locks.Add(MakeEntry(&m1));
    locks.Acquire();
    Check(locks.Size() == 1 && locks.LockedCount() == 1,
          "L1.3 同一 channel 重复 case 只加一次锁");
  }

  // RAII：作用域结束后锁必须已经释放。
  bool reacquired = false;
  {
    detail::SelectLockSet locks;
    locks.Add(MakeEntry(&m2));
    locks.Acquire();
  }
  {
    std::unique_lock<std::mutex> probe(m2, std::try_to_lock);
    reacquired = probe.owns_lock();
  }
  Check(reacquired, "L1.4 析构后逆序释放全部锁（RAII）");

  // 多线程以相反顺序反复登记/加锁/释放：未排序实现会在此挂起。
  {
    Watchdog watchdog(30s, "L1.5 反向登记压力");
    std::atomic<int> completed{0};
    auto worker = [&](bool reversed) {
      for (int i = 0; i < 20000; ++i) {
        detail::SelectLockSet locks;
        if (!reversed) {
          locks.Add(MakeQuietEntry(&m1));
          locks.Add(MakeQuietEntry(&m2));
        } else {
          locks.Add(MakeQuietEntry(&m2));
          locks.Add(MakeQuietEntry(&m1));
        }
        locks.Acquire();
        locks.Release();
        completed.fetch_add(1, std::memory_order_relaxed);
      }
    };
    std::thread first(worker, false);
    std::thread second(worker, true);
    first.join();
    second.join();
    Check(completed.load() == 40000, "L1.5 两线程反向登记 2 万轮不死锁");
  }
}

// ------------------------------------------------------------ L2 原生线程反向

void TestNativeReversedOrder() {
  Watchdog watchdog(40s, "L2 native 反向 select");
  constexpr int kRounds = 5000;
  auto first = MakeChannel<int>(0);
  auto second = MakeChannel<int>(0);
  std::atomic<int> completed{0};
  std::atomic<int> failed{0};

  auto worker = [&](bool reversed) {
    for (int i = 0; i < kRounds; ++i) {
      std::vector<SelectCase> cases;
      if (!reversed) {
        cases.push_back(RecvCase(first));
        cases.push_back(SendCase(second, i));
      } else {
        cases.push_back(RecvCase(second));
        cases.push_back(SendCase(first, i));
      }
      const auto result = Select(cases);
      if (!result.selected || result.status != ChannelStatus::kReady) {
        failed.fetch_add(1, std::memory_order_relaxed);
      }
      completed.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::thread first_worker(worker, false);
  std::thread second_worker(worker, true);
  first_worker.join();
  second_worker.join();
  Check(failed.load() == 0 && completed.load() == kRounds * 2,
        "L2 原生线程以相反 case 顺序 select 同一组 channel 不死锁");
}

// ---------------------------------------------------------------- L3 Fiber 反向

void TestFiberReversedOrder(Scheduler& scheduler) {
  Watchdog watchdog(40s, "L3 fiber 反向 select");
  constexpr int kRounds = 5000;
  auto first = MakeChannel<int>(0);
  auto second = MakeChannel<int>(0);
  auto completed = std::make_shared<std::atomic<int>>(0);
  auto failed = std::make_shared<std::atomic<int>>(0);

  auto make_worker = [&](bool reversed) {
    return go(scheduler, [first, second, reversed, completed, failed] {
      for (int i = 0; i < kRounds; ++i) {
        std::vector<SelectCase> cases;
        if (!reversed) {
          cases.push_back(RecvCase(first));
          cases.push_back(SendCase(second, i));
        } else {
          cases.push_back(RecvCase(second));
          cases.push_back(SendCase(first, i));
        }
        const auto result = Select(cases);
        if (!result.selected || result.status != ChannelStatus::kReady) {
          failed->fetch_add(1, std::memory_order_relaxed);
        }
        completed->fetch_add(1, std::memory_order_relaxed);
      }
    });
  };

  auto first_worker = make_worker(false);
  auto second_worker = make_worker(true);
  first_worker->wait();
  second_worker->wait();
  Check(failed->load() == 0 && completed->load() == kRounds * 2,
        "L3 Fiber 以相反 case 顺序 select 同一组 channel 不死锁");
}

// ------------------------------------------------------ L4 同一 channel 双 case

void TestSameChannelTwice(Scheduler& scheduler) {
  Watchdog watchdog(40s, "L4 同 channel 双 case");
  constexpr int kRounds = 3000;
  auto channel = MakeChannel<int>(0);
  auto completed = std::make_shared<std::atomic<int>>(0);

  auto make_worker = [&](bool reversed) {
    return go(scheduler, [channel, reversed, completed] {
      for (int i = 0; i < kRounds; ++i) {
        std::vector<SelectCase> cases;
        if (!reversed) {
          cases.push_back(RecvCase(channel));
          cases.push_back(SendCase(channel, i));
        } else {
          cases.push_back(SendCase(channel, i));
          cases.push_back(RecvCase(channel));
        }
        const auto result = Select(cases);
        if (result.selected && result.status == ChannelStatus::kReady) {
          completed->fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  };

  auto first = make_worker(false);
  auto second = make_worker(true);
  first->wait();
  second->wait();
  Check(completed->load() == kRounds * 2,
        "L4 同一 channel 的 recv/send case 去重后不死锁且全部成功");
}

// ------------------------------------------------------------ L5 随机 select

void TestRandomSelectStress(Scheduler& scheduler) {
  Watchdog watchdog(60s, "L5 随机 select 压力");
  std::vector<ChannelPtr<int>> channels;
  for (std::size_t capacity : {0u, 1u, 4u, 16u}) {
    channels.push_back(MakeChannel<int>(capacity));
  }
  constexpr int kWorkers = 6;
  constexpr int kIters = 1500;
  auto selected = std::make_shared<std::atomic<long>>(0);
  auto timed_out = std::make_shared<std::atomic<long>>(0);
  auto failed = std::make_shared<std::atomic<long>>(0);
  auto finished = std::make_shared<std::atomic<int>>(0);

  for (int worker = 0; worker < kWorkers; ++worker) {
    go(scheduler, [channels, worker, selected, timed_out, failed, finished] {
      std::mt19937 rng(static_cast<unsigned>(0x5EED + worker));
      std::vector<std::size_t> index(channels.size());
      for (std::size_t i = 0; i < index.size(); ++i) {
        index[i] = i;
      }
      for (int iter = 0; iter < kIters; ++iter) {
        std::shuffle(index.begin(), index.end(), rng);
        const std::size_t count = 2 + (rng() % (index.size() - 1));
        std::vector<SelectCase> cases;
        cases.reserve(count + 2);
        for (std::size_t i = 0; i < count; ++i) {
          const auto slot = index[i];
          if ((rng() & 1u) != 0) {
            cases.push_back(RecvCase(channels[slot]));
          } else {
            cases.push_back(SendCase(channels[slot], iter));
          }
        }
        if (rng() % 8 == 0) {
          cases.push_back(DefaultCase());
        } else {
          cases.push_back(AfterCase(2ms));
        }
        std::shuffle(cases.begin(), cases.end(), rng);
        const auto result =
            Select(cases, {}, std::optional<ContextDuration>(200ms));
        if (result.selected) {
          selected->fetch_add(1, std::memory_order_relaxed);
        } else if (result.status == ChannelStatus::kTimedOut ||
                   result.status == ChannelStatus::kDeadlineExceeded) {
          timed_out->fetch_add(1, std::memory_order_relaxed);
        } else {
          failed->fetch_add(1, std::memory_order_relaxed);
        }
      }
      finished->fetch_add(1, std::memory_order_relaxed);
    });
  }

  // 主线程等待全部 worker 结束（有看门狗兜底，不会永久阻塞）。
  const auto wait_deadline = std::chrono::steady_clock::now() + 50s;
  while (finished->load(std::memory_order_relaxed) != kWorkers &&
         std::chrono::steady_clock::now() < wait_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  Check(finished->load() == kWorkers, "L5.1 随机 select worker 全部结束");
  Check(failed->load() == 0, "L5.2 随机 select 无非法状态");
  Check(selected->load() + timed_out->load() ==
            static_cast<long>(kWorkers) * kIters,
        "L5.3 随机 select 全部落定（选中或超时）");
}

// -------------------------------------------------------------- L6 关闭拆除

void TestCloseTeardown(Scheduler& scheduler) {
  Watchdog watchdog(40s, "L6 select 拆除");
  constexpr int kRounds = 40;
  for (int round = 0; round < kRounds; ++round) {
    auto channel = MakeChannel<int>(0);
    auto pair = Context::WithCancel(Context::Background());
    auto context = pair.first;
    auto cancel = pair.second;
    auto returned = std::make_shared<std::atomic<int>>(0);
    constexpr int kWatchers = 4;
    for (int i = 0; i < kWatchers; ++i) {
      go(scheduler, [channel, context, returned] {
        std::vector<SelectCase> cases{RecvCase(channel), DoneCase(context)};
        const auto result = Select(cases);
        if (result.selected) {
          returned->fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
    std::this_thread::sleep_for(2ms);
    (void)channel->Close();
    cancel();
    // 等所有 watcher 返回：用计数器自旋（有看门狗兜底）。
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (returned->load() < kWatchers &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(1ms);
    }
    const auto snapshot = channel->DebugSnapshot();
    if (returned->load() != kWatchers ||
        snapshot.select_receivers != 0 || snapshot.select_senders != 0) {
      Check(false, "L6.1 关闭/取消后所有 select 返回且不残留注册节点");
      return;
    }
  }
  Check(true, "L6.1 关闭/取消后所有 select 返回且不残留注册节点");
}

// ---------------------------------------------------------- L7 线程/Fiber 混合

void TestNativeFiberMixed(Scheduler& scheduler) {
  Watchdog watchdog(40s, "L7 线程+Fiber 混合");
  constexpr int kMessages = 3000;
  auto channel = MakeChannel<int>(0);
  auto received = std::make_shared<std::atomic<int>>(0);

  auto consumer = [&](bool secondary) {
    return go(scheduler, [channel, received, secondary] {
      for (;;) {
        std::vector<SelectCase> cases{RecvCase(channel), AfterCase(5ms)};
        const auto result = Select(cases);
        if (result.selected && result.status == ChannelStatus::kReady &&
            result.index == 0) {
          received->fetch_add(1, std::memory_order_relaxed);
        }
        if (received->load(std::memory_order_relaxed) >= kMessages) {
          return;
        }
        (void)secondary;
      }
    });
  };

  auto first = consumer(false);
  auto second = consumer(true);
  std::thread producer([channel] {
    for (int i = 0; i < kMessages; ++i) {
      (void)channel->Send(i);
    }
  });
  producer.join();
  first->wait();
  second->wait();
  Check(received->load() >= kMessages,
        "L7 原生线程发送 + 两个 Fiber select 接收全部完成");
}

// ---------------------------------------------------------- L8 三 channel 反向

void TestThreeChannelReversed() {
  Watchdog watchdog(40s, "L8 三 channel 反向");
  constexpr int kRounds = 3000;
  auto deps = MakeChannel<int>(0);
  auto orders = MakeChannel<int>(0);
  auto acks = MakeChannel<int>(0);
  std::atomic<int> completed{0};

  auto coordinator = [&] {
    for (int i = 0; i < kRounds; ++i) {
      std::vector<SelectCase> cases;
      cases.push_back(RecvCase(deps));
      cases.push_back(RecvCase(orders));
      cases.push_back(SendCase(acks, i));
      const auto result = Select(cases);
      if (result.selected && result.status == ChannelStatus::kReady) {
        completed.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };
  auto worker = [&] {
    for (int i = 0; i < kRounds; ++i) {
      std::vector<SelectCase> cases;
      // 与 coordinator 完全相反的 case 顺序。
      cases.push_back(RecvCase(acks));
      cases.push_back(SendCase(orders, i));
      cases.push_back(SendCase(deps, i));
      const auto result = Select(cases);
      if (result.selected && result.status == ChannelStatus::kReady) {
        completed.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  std::thread first(coordinator);
  std::thread second(worker);
  first.join();
  second.join();
  Check(completed.load() == kRounds * 2,
        "L8 三 channel、相反 case 顺序长期配对无死锁");
}

}  // namespace

int main() {
  std::printf("== select lockorder tests ==\n");
  TestLockSetOrdering();
  TestNativeReversedOrder();

  Scheduler scheduler(4);
  scheduler.start();
  TestFiberReversedOrder(scheduler);
  TestSameChannelTwice(scheduler);
  TestRandomSelectStress(scheduler);
  TestCloseTeardown(scheduler);
  TestNativeFiberMixed(scheduler);
  TestThreeChannelReversed();

  std::printf("[select_lockorder] failures=%d\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
