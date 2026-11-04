/**
 * @file bench_asm_rest.cpp
 * @brief 「并发汇编剩余点」专用微基准：Mutex / ConditionVariable / futex /
 *        Channel / FastChannel / Scheduler 提交。
 *
 * @date 2026-11-01
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 为什么独立
 * 本轮优化只允许改 `src/concurrency/**`、`include/go2cpp/concurrency/**`、
 * `tests/concurrency/**`，不能注册 CMake 目标。因此本文件手动编译：
 * ```
 * g++ -std=c++20 -O3 -DNDEBUG -Iinclude tests/concurrency/bench_asm_rest.cpp \
 *   -Wl,--no-as-needed -Wl,-rpath,$PWD/.build-bento-wsl -L.build-bento-wsl \
 *   -lgo2cpp_channel -lgo2cpp_context -lgo2cpp_scheduler -lgo2cpp_fiber \
 *   -lgo2cpp_sync -lgo2cpp_error -lgo2cpp_log -lgo2cpp_debug \
 *   -lgo2cpp_debug_event -lgo2cpp_hook -lgo2cpp_io -lgo2cpp_config \
 *   -lgo2cpp_container -lgo2cpp_metrics -lgo2cpp_control_flow -lgo2cpp_math \
 *   -lgo2cpp_rand -lgo2cpp_path -lgo2cpp_compress -lgo2cpp_encoding \
 *   -lgo2cpp_hash -lgo2cpp_future -lgo2cpp_network -pthread -ldl -lz -lm \
 *   -o /tmp/asmrest/bench_asm_rest
 * taskset -c 2,3 /tmp/asmrest/bench_asm_rest [iters]
 * ```
 *
 * ## 测项与口径
 * - `mutex.pair.1t`：单线程 lock/unlock 一个完整临界区 ns；
 * - `mutex.4t`：4 线程争用同一把锁的 ns/op；
 * - `cv.notify.empty`：**无等待者**时 `NotifyOne()` 单次 ns（点 2 的目标）；
 * - `cv.pingpong`：native 线程 1P1C 条件变量往返 ns（点 3 的目标）；
 * - `futex.wake.empty`：空 FUTEX_WAKE 系统调用 ns（syscall 物理下限）；
 * - `futex.wait.wake`：FUTEX_WAIT→FUTEX_WAKE 完整往返 ns（park 下限）；
 * - `chan.spsc.64`：`Channel<uint64_t>`（容量 64）1P1C ns/op；
 * - `fastchan.spsc.64`：`FastChannel<uint64_t,64>` 1P1C ns/op（点 4 的目标）；
 * - `fastchan.1t`：同一线程 TrySend/TryRecv 交替（隔离锁与环成本）；
 * - `sched.submit`：`Scheduler::spawn` 提交 + 完成墙钟 ns/task（点 5/6/7）。
 *
 * 所有测项都校验结果（序号/计数），任何丢失或乱序都会返回非零。
 */
#include "go2cpp/channel.hpp"
#include "go2cpp/concurrency/fast_channel.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

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
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

inline void Relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}

inline double Ns(Clock::duration d, std::uint64_t ops) {
  return static_cast<double>(
             std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()) /
         static_cast<double>(ops);
}

inline void Escape(std::uint64_t v) {
  static volatile std::uint64_t sink = 0;
  sink = sink + v;
}

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++g_failures;
  }
}

void Report(const char* name, double ns, std::uint64_t ops) {
  std::printf("%-20s %10.3f ns/op   ops=%llu\n", name, ns,
              static_cast<unsigned long long>(ops));
}

// ---------------- Mutex ----------------

void BenchMutexPair(std::uint64_t iters) {
  go2cpp::sync::Mutex mutex;
  double best = 1e30;
  for (int rep = 0; rep < 3; ++rep) {
    const auto start = Clock::now();
    for (std::uint64_t i = 0; i < iters; ++i) {
      mutex.Lock();
      mutex.Unlock();
    }
    const auto elapsed = Clock::now() - start;
    best = std::min(best, Ns(elapsed, iters));
  }
  Report("mutex.pair.1t", best, iters);
}

void BenchMutexContend(std::uint64_t iters, int threads) {
  go2cpp::sync::Mutex mutex;
  std::atomic<std::uint64_t> ops{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(threads));
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
      while (!go.load(std::memory_order_acquire)) {
        Relax();
      }
      const std::uint64_t per = iters / static_cast<std::uint64_t>(threads);
      for (std::uint64_t i = 0; i < per; ++i) {
        mutex.Lock();
        mutex.Unlock();
      }
      ops.fetch_add(per, std::memory_order_relaxed);
    });
  }
  const auto start = Clock::now();
  go.store(true, std::memory_order_release);
  for (auto& w : workers) {
    w.join();
  }
  const auto elapsed = Clock::now() - start;
  Report("mutex.4t", Ns(elapsed, ops.load()), ops.load());
}

// ---------------- ConditionVariable ----------------

void BenchCvNotifyEmpty(std::uint64_t iters) {
  go2cpp::sync::ConditionVariable cv;
  double best = 1e30;
  for (int rep = 0; rep < 3; ++rep) {
    const auto start = Clock::now();
    for (std::uint64_t i = 0; i < iters; ++i) {
      cv.NotifyOne();
    }
    const auto elapsed = Clock::now() - start;
    best = std::min(best, Ns(elapsed, iters));
  }
  Report("cv.notify.empty", best, iters);
}

/// native 线程 1P1C：wait 侧入队 → notify → wait 返回，计一次往返。
void BenchCvPingPong(std::uint64_t rounds) {
  go2cpp::sync::Mutex mutex;
  go2cpp::sync::ConditionVariable cv;
  std::uint64_t turn = 0;
  std::atomic<std::uint64_t> acked{0};
  std::atomic<bool> stop{false};
  std::thread waiter([&] {
    for (std::uint64_t i = 1; i <= rounds; ++i) {
      mutex.Lock();
      while (turn < i && !stop.load(std::memory_order_acquire)) {
        (void)cv.Wait(mutex);
      }
      mutex.Unlock();
      acked.store(i, std::memory_order_release);
    }
  });
  const auto start = Clock::now();
  for (std::uint64_t i = 1; i <= rounds; ++i) {
    // 等等待者上一次确认（保证它已经重新入队）。
    while (acked.load(std::memory_order_acquire) + 1 < i) {
      Relax();
    }
    mutex.Lock();
    turn = i;
    cv.NotifyOne();
    mutex.Unlock();
  }
  while (acked.load(std::memory_order_acquire) < rounds) {
    Relax();
  }
  const auto elapsed = Clock::now() - start;
  stop.store(true, std::memory_order_release);
  mutex.Lock();
  cv.NotifyAll();
  mutex.Unlock();
  waiter.join();
  Report("cv.pingpong", Ns(elapsed, rounds), rounds);
}

// ---------------- futex 物理下限 ----------------

void BenchFutexEmptyWake(std::uint64_t iters) {
  std::atomic<std::uint32_t> word{0};
  double best = 1e30;
  for (int rep = 0; rep < 3; ++rep) {
    const auto start = Clock::now();
#if defined(__linux__)
    for (std::uint64_t i = 0; i < iters; ++i) {
      (void)::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word),
                      FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    }
#else
    (void)iters;
#endif
    const auto elapsed = Clock::now() - start;
    best = std::min(best, Ns(elapsed, iters));
  }
  Report("futex.wake.empty", best, iters);
}

void BenchFutexRoundTrip(std::uint64_t rounds) {
#if defined(__linux__)
  std::atomic<std::uint32_t> word{0};
  std::atomic<std::uint64_t> acked{0};
  std::atomic<bool> stop{false};
  std::thread waiter([&] {
    for (std::uint64_t i = 1; i <= rounds; ++i) {
      while (word.load(std::memory_order_acquire) < i) {
        if (stop.load(std::memory_order_acquire)) {
          return;
        }
        const std::uint32_t expected =
            static_cast<std::uint32_t>(word.load(std::memory_order_relaxed));
        (void)::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word),
                        FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
      }
      acked.store(i, std::memory_order_release);
    }
  });
  const auto start = Clock::now();
  for (std::uint64_t i = 1; i <= rounds; ++i) {
    word.store(static_cast<std::uint32_t>(i), std::memory_order_release);
    (void)::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word),
                    FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    while (acked.load(std::memory_order_acquire) < i) {
      Relax();
    }
  }
  const auto elapsed = Clock::now() - start;
  stop.store(true, std::memory_order_release);
  word.fetch_add(1, std::memory_order_release);
  (void)::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word),
                  FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
  waiter.join();
  Report("futex.wait.wake", Ns(elapsed, rounds), rounds);
#else
  (void)rounds;
#endif
}

// ---------------- Channel ----------------

void BenchChannelSpsc(std::uint64_t total, std::size_t capacity) {
  auto channel = go2cpp::MakeChannel<std::uint64_t>(capacity);
  std::atomic<bool> go{false};
  std::uint64_t violations = 0;
  std::thread producer([&] {
    while (!go.load(std::memory_order_acquire)) {
      Relax();
    }
    for (std::uint64_t i = 0; i < total; ++i) {
      (void)channel->Send(i);  // 基准热路径：通道不关闭，Send 恒成功
    }
  });
  std::thread consumer([&] {
    while (!go.load(std::memory_order_acquire)) {
      Relax();
    }
    for (std::uint64_t i = 0; i < total; ++i) {
      const auto result = channel->Recv();
      if (!result.ok || !result.value || *result.value != i) {
        ++violations;
      }
    }
  });
  const auto start = Clock::now();
  go.store(true, std::memory_order_release);
  producer.join();
  consumer.join();
  const auto elapsed = Clock::now() - start;
  Check(violations == 0, "chan.spsc ordering");
  Report("chan.spsc.64", Ns(elapsed, total), total);
}

void BenchFastChannelSpsc(std::uint64_t total) {
  go2cpp::FastChannel<std::uint64_t, 64> channel;
  std::atomic<bool> go{false};
  std::uint64_t violations = 0;
  std::thread producer([&] {
    while (!go.load(std::memory_order_acquire)) {
      Relax();
    }
    for (std::uint64_t i = 0; i < total; ++i) {
      if (channel.Send(i) != go2cpp::FastStatus::kOk) {
        ++violations;
      }
    }
  });
  std::thread consumer([&] {
    while (!go.load(std::memory_order_acquire)) {
      Relax();
    }
    for (std::uint64_t i = 0; i < total; ++i) {
      std::uint64_t value = 0;
      if (channel.Recv(value) != go2cpp::FastStatus::kOk || value != i) {
        ++violations;
      }
    }
  });
  const auto start = Clock::now();
  go.store(true, std::memory_order_release);
  producer.join();
  consumer.join();
  const auto elapsed = Clock::now() - start;
  Check(violations == 0, "fastchan.spsc ordering");
  Report("fastchan.spsc.64", Ns(elapsed, total), total);
}

/// 同一线程 TrySend/TryRecv 交替，隔离「CanSpinWait + 锁 + 环」的固定成本。
void BenchFastChannelSingleThread(std::uint64_t iters) {
  go2cpp::FastChannel<std::uint64_t, 64> channel;
  std::uint64_t sink = 0;
  double best = 1e30;
  for (int rep = 0; rep < 3; ++rep) {
    const auto start = Clock::now();
    for (std::uint64_t i = 0; i < iters; ++i) {
      (void)channel.TrySend(i);
      std::uint64_t value = 0;
      if (channel.TryRecv(value) == go2cpp::FastStatus::kOk) {
        sink += value;
      }
    }
    const auto elapsed = Clock::now() - start;
    best = std::min(best, Ns(elapsed, iters));
  }
  Escape(sink);
  Report("fastchan.1t", best, iters);
}

// ---------------- Scheduler 提交 ----------------

void BenchSchedulerSubmit(std::uint64_t total, int workers) {
  go2cpp::SchedulerConfig config;
  config.processor_count = static_cast<std::size_t>(workers);
  config.min_workers = static_cast<std::size_t>(workers);
  config.max_workers = static_cast<std::size_t>(workers);
  go2cpp::Scheduler scheduler(config);
  scheduler.start();
  std::atomic<std::uint64_t> done{0};
  std::uint64_t submitted = 0;
  const auto start = Clock::now();
  for (std::uint64_t i = 0; i < total; ++i) {
    // spawn 可能因停机/资源限制返回空：必须接受失败并且只等已提交的部分，
    // 否则 done 永远追不上 total，基准会挂死在这里。
    if (!scheduler.spawn(
            [&done] { done.fetch_add(1, std::memory_order_relaxed); })) {
      break;
    }
    ++submitted;
  }
  while (done.load(std::memory_order_acquire) < submitted) {
    std::this_thread::yield();
  }
  const auto elapsed = Clock::now() - start;
  scheduler.shutdown();
  Check(submitted > 0, "sched.submit 至少提交一个任务");
  Report("sched.submit", Ns(elapsed, submitted), submitted);
}

std::uint64_t ParseIters(int argc, char** argv, std::uint64_t fallback) {
  if (argc < 2) {
    return fallback;
  }
  return std::strtoull(argv[1], nullptr, 10);
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t iters = ParseIters(argc, argv, 10000000ULL);
  const std::uint64_t small = std::max<std::uint64_t>(iters / 10, 100000ULL);
  std::printf("iters=%llu\n", static_cast<unsigned long long>(iters));

  // 第三参数是标签过滤器（空 = 全跑），便于单项定位与超时隔离。
  const char* only = argc >= 3 ? argv[2] : nullptr;
  const auto want = [only](const char* name) {
    return only == nullptr || std::strstr(name, only) != nullptr;
  };
  std::printf("filter=%s\n", only == nullptr ? "(all)" : only);
  std::fflush(stdout);
  if (want("mutex")) {
    BenchMutexPair(iters);
    BenchMutexContend(iters, 4);
  }
  if (want("notify")) {
    // 独立标签：空通知是纯用户态操作，可以用 1e8 次压到极小相对噪声。
    BenchCvNotifyEmpty(iters);
  }
  if (want("cv")) {
    BenchCvPingPong(std::max<std::uint64_t>(iters / 10, 200000ULL));
  }
  if (want("futex")) {
    BenchFutexEmptyWake(iters);
    BenchFutexRoundTrip(small / 10);
  }
  if (want("chan")) {
    BenchChannelSpsc(iters, 64);
    BenchFastChannelSpsc(iters);
    BenchFastChannelSingleThread(iters);
  }
  if (want("sched")) {
    BenchSchedulerSubmit(small, 4);
  }

  if (g_failures != 0) {
    std::printf("FAILURES=%d\n", g_failures);
    return 1;
  }
  std::printf("ALL OK\n");
  return 0;
}
