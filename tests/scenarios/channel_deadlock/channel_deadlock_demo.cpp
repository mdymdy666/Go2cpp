/**
 * @file channel_deadlock_demo.cpp
 * @brief 场景：两个 Fiber 在无缓冲 channel 上互等，用 select 语义对比四种写法。
 *
 * ## 场景（同一份「互等」协议）
 *
 * 两个无缓冲 channel `chX` / `chY`，两个 Fiber：
 *
 * ```text
 *   Fiber A: 等 chX（B 本应在完成本端工作后发 chX）
 *   Fiber B: 等 chY（A 本应在完成本端工作后发 chY）
 *   协议约定：A 收到 chX 后向 chY 发送，B 收到 chY 后向 chX 发送。
 *   但双方都在等对方的第一条消息 —— 发送点永远到不了。
 * ```
 *
 * 注意：本场景**不依赖 Doctor**，也不调用 ctx.cancel / force-wake。
 * 解除互等完全靠 select 语义本身。
 *
 * ## 四种写法（--mode）
 *
 * 1. `baseline`：A `chX->Recv()`、B `chY->Recv()` 纯阻塞。
 *    构造成功后进程永久卡住，作为死锁对照；只能由外部 SIGTERM 结束。
 * 2. `timeout`：`Select({RecvCase(ch), AfterCase(200ms)})`。
 *    双方在 200ms 后从定时器分支退出，不永久死锁；记录超时耗时。
 * 3. `multi`：`Select({RecvCase(自己的入站), SendCase(对端的入站, token)})`。
 *    每一方都同时等「收」和「发」两个 channel 操作；A 的 send 与 B 的 recv
 *    通过 select 的 TrySelectPair 配对，双方都完成。
 *
 *    **为什么不是纯 recv 的两路 select？** 双方都不发送时，
 *    `Select({RecvCase(chX), RecvCase(chY)})` 只是把同一个「无人发送」的
 *    等待登记到两个 channel 上，仍然是死锁（两个接收队列都为空，
 *    没有任何 case 会就绪）。真正能破互等的是**可完成的替代分支**——
 *    本场景用 SendCase 提供它。README / 报告里对此有专门说明。
 * 4. `default`：`Select({RecvCase(ch), DefaultCase()})` 非阻塞尝试；
 *    没有就绪数据时走降级路径，记录「无就绪」次数后退出，不 park。
 *
 * ## 可复现性
 *
 * `--repro N` 对 timeout / multi / default 连续跑 N 轮（baseline 只允许 1 轮，
 * 因为它的语义就是永久卡住）。每轮新建 channel，输出 ROUND / REPRO 汇总。
 *
 * ## 输出约定
 *
 * stdout 是机器可读阶段行：
 * - `DEADLOCK_READY` / `STUCK`：baseline 确认双方已 park；
 * - `STILL_STUCK`：baseline 每 250ms 心跳，证明窗口内没有进展；
 * - `RESULT`：单轮结果（pass / stuck / 耗时 / 角色）；
 * - `REPRO`：N 轮汇总（轮数、成功数、平均/最大耗时）；
 * - `BASELINE_RESULT`：baseline 对照结果；
 * - `FAIL`：构造失败。
 *
 * stderr 是人体可读细节（每个 Fiber 的分支选择与耗时）。
 *
 * 退出码：0 = 满足本模式断言；2 = 参数错误；4 = 断言失败。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using namespace go2cpp;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// 基础工具
// ---------------------------------------------------------------------------

volatile std::sig_atomic_t g_stop = 0;

void OnSignal(int /*signal*/) { g_stop = 1; }

std::uint64_t CurrentGid() {
  Task* task = Scheduler::current_task_ptr();
  return task == nullptr ? 0U : static_cast<std::uint64_t>(task->id());
}

long long NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

long long ElapsedMs(long long start_us) {
  return (NowUs() - start_us) / 1000;
}

long long SteadyUs(std::chrono::steady_clock::time_point point) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             point.time_since_epoch())
      .count();
}

const char* StatusName(ChannelStatus status) {
  switch (status) {
    case ChannelStatus::kReady: return "ready";
    case ChannelStatus::kWouldBlock: return "would-block";
    case ChannelStatus::kClosed: return "closed";
    case ChannelStatus::kAlreadyClosed: return "already-closed";
    case ChannelStatus::kCancelled: return "cancelled";
    case ChannelStatus::kDeadlineExceeded: return "deadline-exceeded";
    case ChannelStatus::kTimedOut: return "timed-out";
    case ChannelStatus::kNil: return "nil";
    case ChannelStatus::kInvalid: return "invalid";
  }
  return "unknown";
}

bool Terminal(const std::shared_ptr<Task>& task) {
  if (!task) {
    return true;
  }
  const GState state = task->state();
  return state == GState::kDead || state == GState::kCancelled ||
         state == GState::kFailed;
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

// ---------------------------------------------------------------------------
// 配置与每轮状态
// ---------------------------------------------------------------------------

struct Config {
  std::string mode{"timeout"};
  int repro{1};
  int timeout_ms{200};
  int hold_ms{60000};
  int default_retries{3};
  int default_interval_ms{20};
  int wait_ms{15000};
};

/// 一个 Fiber 的结果。用独立原子字段而不是结构体整体交换，
/// 保证主线程任何时刻读到的字段都不会撕裂。
struct FiberResult {
  std::atomic<bool> finished{false};
  std::atomic<bool> completed{false};   ///< 分支成功完成（而非异常/无结果）
  std::atomic<bool> timed_out{false};   ///< timeout 模式：选中了定时器分支
  std::atomic<bool> fallback{false};    ///< default 模式：走了降级路径
  std::atomic<int> index{-1};           ///< select 选中的 case 下标
  std::atomic<int> status{static_cast<int>(ChannelStatus::kInvalid)};
  /// 1=recv 2=timer 3=send 5=fallback；0=未分类。
  std::atomic<int> kind{0};
  std::atomic<int> no_ready{0};         ///< default 模式：DefaultCase 命中次数
  std::atomic<long long> elapsed_us{0};
  std::atomic<std::uint64_t> gid{0};
};

struct RoundState {
  ChannelPtr<int> chx;
  ChannelPtr<int> chy;
  FiberResult a;
  FiberResult b;
};

struct RoundStats {
  bool pass{false};
  bool stuck_observed{false};
  int stuck_before{0};
  int stuck_after{0};
  long long a_elapsed_ms{0};
  long long b_elapsed_ms{0};
  std::string detail;
};

// ---------------------------------------------------------------------------
// Fiber 主体：四种写法
// ---------------------------------------------------------------------------

/// baseline：纯阻塞 Recv，永远等不到对端发送。
void RunBlocking(const std::shared_ptr<RoundState>& state, bool is_a) {
  auto& result = is_a ? state->a : state->b;
  result.gid.store(CurrentGid(), std::memory_order_release);
  const auto start = NowUs();
  const auto recv = is_a ? state->chx->Recv() : state->chy->Recv();
  // 只有 channel 被关闭或调度器取消时才会走到这里；baseline 的期望是永不返回。
  result.elapsed_us.store(NowUs() - start, std::memory_order_release);
  result.index.store(0, std::memory_order_release);
  result.status.store(static_cast<int>(recv.status),
                      std::memory_order_release);
  result.kind.store(1, std::memory_order_release);
  result.completed.store(recv.status == ChannelStatus::kReady,
                         std::memory_order_release);
  result.finished.store(true, std::memory_order_release);
}

/// timeout：`select { case <-ch: ...; case <-time.After(200ms): ... }`。
void RunTimeout(const std::shared_ptr<RoundState>& state, bool is_a,
                int timeout_ms) {
  auto& result = is_a ? state->a : state->b;
  const auto& channel = is_a ? state->chx : state->chy;
  result.gid.store(CurrentGid(), std::memory_order_release);
  const auto start = NowUs();
  const auto selected =
      Select({RecvCase(channel),
              AfterCase(std::chrono::milliseconds(timeout_ms))});
  result.elapsed_us.store(NowUs() - start, std::memory_order_release);
  result.index.store(static_cast<int>(selected.index),
                     std::memory_order_release);
  result.status.store(static_cast<int>(selected.status),
                      std::memory_order_release);
  if (selected.selected && selected.index == 0) {
    result.kind.store(1, std::memory_order_release);
    result.completed.store(selected.status == ChannelStatus::kReady,
                           std::memory_order_release);
    std::fprintf(stderr,
                 "[%s] RECV gid=%llu status=%s elapsed_ms=%lld\n",
                 is_a ? "A" : "B",
                 static_cast<unsigned long long>(result.gid.load()),
                 StatusName(selected.status), ElapsedMs(start));
  } else if (selected.selected && selected.index == 1) {
    result.kind.store(2, std::memory_order_release);
    result.timed_out.store(true, std::memory_order_release);
    result.completed.store(true, std::memory_order_release);
    std::fprintf(stderr,
                 "[%s] TIMEOUT gid=%llu timeout_ms=%d elapsed_ms=%lld\n",
                 is_a ? "A" : "B",
                 static_cast<unsigned long long>(result.gid.load()),
                 timeout_ms, ElapsedMs(start));
  } else {
    std::fprintf(stderr,
                 "[%s] TIMEOUT-SELECT-UNEXPECTED gid=%llu selected=%d "
                 "index=%zu status=%s\n",
                 is_a ? "A" : "B",
                 static_cast<unsigned long long>(result.gid.load()),
                 selected.selected ? 1 : 0, selected.index,
                 StatusName(selected.status));
  }
  result.finished.store(true, std::memory_order_release);
}

/// multi：`select { case <-自己的入站: ...; case 对端的入站 <- token: ... }`。
///
/// 两个 select 的 send/recv 会通过 Channel 的 select 配对逻辑
/// （`SelectWaitState::TrySelectPair`）互相完成，因此双方都能退出。
void RunMulti(const std::shared_ptr<RoundState>& state, bool is_a, int token) {
  auto& result = is_a ? state->a : state->b;
  const auto& recv_channel = is_a ? state->chx : state->chy;
  const auto& send_channel = is_a ? state->chy : state->chx;
  result.gid.store(CurrentGid(), std::memory_order_release);
  const auto start = NowUs();
  const auto selected =
      Select({RecvCase(recv_channel), SendCase(send_channel, token)});
  result.elapsed_us.store(NowUs() - start, std::memory_order_release);
  result.index.store(static_cast<int>(selected.index),
                     std::memory_order_release);
  result.status.store(static_cast<int>(selected.status),
                      std::memory_order_release);
  if (selected.selected && selected.index == 0) {
    result.kind.store(1, std::memory_order_release);
    result.completed.store(selected.status == ChannelStatus::kReady,
                           std::memory_order_release);
  } else if (selected.selected && selected.index == 1) {
    result.kind.store(3, std::memory_order_release);
    result.completed.store(selected.status == ChannelStatus::kReady,
                           std::memory_order_release);
  }
  const char* role = !selected.selected
                         ? "none"
                         : (selected.index == 0 ? "recv" : "send");
  std::fprintf(stderr,
               "[%s] MULTI gid=%llu selected=%d index=%zu role=%s "
               "status=%s elapsed_ms=%lld\n",
               is_a ? "A" : "B",
               static_cast<unsigned long long>(result.gid.load()),
               selected.selected ? 1 : 0, selected.index, role,
               StatusName(selected.status), ElapsedMs(start));
  result.finished.store(true, std::memory_order_release);
}

/// default：`select { case <-ch: ...; default: 降级 }`。
/// 每次循环都是一次非阻塞尝试；没有就绪数据就计数并走降级路径。
void RunDefault(const std::shared_ptr<RoundState>& state, bool is_a,
                int retries, int interval_ms) {
  auto& result = is_a ? state->a : state->b;
  const auto& channel = is_a ? state->chx : state->chy;
  result.gid.store(CurrentGid(), std::memory_order_release);
  const auto start = NowUs();
  bool got_value = false;
  for (int attempt = 0; attempt < retries; ++attempt) {
    const auto selected = Select({RecvCase(channel), DefaultCase()});
    if (selected.selected && selected.index == 0 &&
        (selected.status == ChannelStatus::kReady ||
         selected.status == ChannelStatus::kClosed)) {
      got_value = true;
      result.index.store(0, std::memory_order_release);
      result.status.store(static_cast<int>(selected.status),
                          std::memory_order_release);
      result.kind.store(1, std::memory_order_release);
      result.completed.store(true, std::memory_order_release);
      break;
    }
    // DefaultCase 胜出：本路无就绪数据。
    result.no_ready.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[%s] NO_READY gid=%llu attempt=%d status=%s\n",
                 is_a ? "A" : "B",
                 static_cast<unsigned long long>(result.gid.load()),
                 attempt + 1, StatusName(selected.status));
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
  result.elapsed_us.store(NowUs() - start, std::memory_order_release);
  if (!got_value) {
    result.kind.store(5, std::memory_order_release);
    result.fallback.store(true, std::memory_order_release);
    result.completed.store(true, std::memory_order_release);
    std::fprintf(stderr,
                 "[%s] FALLBACK gid=%llu no_ready=%d elapsed_ms=%lld\n",
                 is_a ? "A" : "B",
                 static_cast<unsigned long long>(result.gid.load()),
                 result.no_ready.load(), ElapsedMs(start));
  }
  result.finished.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// 单轮执行（timeout / multi / default）
// ---------------------------------------------------------------------------

RoundStats RunSelectRound(Scheduler& scheduler, const Config& config,
                          int round) {
  RoundStats stats;
  auto state = std::make_shared<RoundState>();
  state->chx = MakeChannel<int>(0);
  state->chy = MakeChannel<int>(0);

  std::shared_ptr<Task> task_a;
  std::shared_ptr<Task> task_b;
  if (config.mode == "timeout") {
    task_a = scheduler.spawn(
        [state, config] { RunTimeout(state, true, config.timeout_ms); });
    task_b = scheduler.spawn(
        [state, config] { RunTimeout(state, false, config.timeout_ms); });
  } else if (config.mode == "multi") {
    task_a =
        scheduler.spawn([state, round] { RunMulti(state, true, round); });
    task_b =
        scheduler.spawn([state, round] { RunMulti(state, false, round); });
  } else {
    task_a = scheduler.spawn([state, config] {
      RunDefault(state, true, config.default_retries,
                 config.default_interval_ms);
    });
    task_b = scheduler.spawn([state, config] {
      RunDefault(state, false, config.default_retries,
                 config.default_interval_ms);
    });
  }
  if (!task_a || !task_b) {
    stats.detail = "spawn-failed";
    std::printf("FAIL mode=%s round=%d reason=spawn-failed\n",
                config.mode.c_str(), round);
    return stats;
  }

  // timeout 模式在 200ms 内一定能观察到双方 park 的瞬间；记录下来作为
  // 「先互等、再超时退出」的证据。multi/default 不需要这个窗口。
  if (config.mode == "timeout") {
    const bool parked = WaitUntil(
        [&state] {
          const auto sx = state->chx->DebugSnapshot();
          const auto sy = state->chy->DebugSnapshot();
          return sx.recv_waiters >= 1U && sy.recv_waiters >= 1U;
        },
        1s);
    stats.stuck_observed = parked;
    if (parked) {
      const auto sx = state->chx->DebugSnapshot();
      const auto sy = state->chy->DebugSnapshot();
      stats.stuck_before =
          static_cast<int>(sx.recv_waiters + sy.recv_waiters);
      std::printf(
          "STUCK mode=timeout round=%d stuck=%d chX.recv_waiters=%zu "
          "chY.recv_waiters=%zu timeout_ms=%d\n",
          round, stats.stuck_before, sx.recv_waiters, sy.recv_waiters,
          config.timeout_ms);
      std::fflush(stdout);
    }
  }

  const bool finished = WaitUntil(
      [&state] {
        return state->a.finished.load(std::memory_order_acquire) &&
               state->b.finished.load(std::memory_order_acquire);
      },
      std::chrono::milliseconds(config.wait_ms));
  const bool tasks_done = WaitUntil(
      [&task_a, &task_b] { return Terminal(task_a) && Terminal(task_b); },
      1s);

  const auto sx = state->chx->DebugSnapshot();
  const auto sy = state->chy->DebugSnapshot();
  stats.stuck_after = static_cast<int>(sx.recv_waiters + sy.recv_waiters);
  stats.a_elapsed_ms = state->a.elapsed_us.load() / 1000;
  stats.b_elapsed_ms = state->b.elapsed_us.load() / 1000;

  const long refs_x = state->chx.use_count();
  const long refs_y = state->chy.use_count();
  const bool alive_a =
      state->a.gid.load() != 0U &&
      scheduler.IsTaskAlive(state->a.gid.load());
  const bool alive_b =
      state->b.gid.load() != 0U &&
      scheduler.IsTaskAlive(state->b.gid.load());
  const bool channels_clean = sx.recv_waiters == 0U && sy.recv_waiters == 0U &&
                              sx.send_waiters == 0U && sy.send_waiters == 0U;

  bool mode_ok = false;
  if (config.mode == "timeout") {
    const long long expected_us =
        static_cast<long long>(config.timeout_ms) * 1000;
    const bool a_time_ok =
        state->a.elapsed_us.load() >= expected_us - 20000 &&
        state->a.elapsed_us.load() <= expected_us + 2000000;
    const bool b_time_ok =
        state->b.elapsed_us.load() >= expected_us - 20000 &&
        state->b.elapsed_us.load() <= expected_us + 2000000;
    mode_ok = state->a.timed_out.load() && state->b.timed_out.load() &&
              a_time_ok && b_time_ok;
  } else if (config.mode == "multi") {
    const int sends = (state->a.kind.load() == 3 ? 1 : 0) +
                      (state->b.kind.load() == 3 ? 1 : 0);
    const int recvs = (state->a.kind.load() == 1 ? 1 : 0) +
                      (state->b.kind.load() == 1 ? 1 : 0);
    mode_ok = state->a.completed.load() && state->b.completed.load() &&
              sends == 1 && recvs == 1;
  } else {
    mode_ok = state->a.fallback.load() && state->b.fallback.load() &&
              state->a.no_ready.load() == config.default_retries &&
              state->b.no_ready.load() == config.default_retries;
  }

  stats.pass = finished && tasks_done && channels_clean && !alive_a &&
               !alive_b && stats.stuck_after == 0 && mode_ok &&
               refs_x == 1 && refs_y == 1;

  std::printf(
      "RESULT mode=%s round=%d pass=%d finished=%d tasks_done=%d "
      "stuck_observed=%d stuck_before=%d stuck_after=%d "
      "a_finished=%d a_completed=%d a_timed_out=%d a_fallback=%d "
      "a_index=%d a_status=%s a_kind=%d a_no_ready=%d a_elapsed_ms=%lld "
      "a_elapsed_us=%lld "
      "b_finished=%d b_completed=%d b_timed_out=%d b_fallback=%d "
      "b_index=%d b_status=%s b_kind=%d b_no_ready=%d b_elapsed_ms=%lld "
      "b_elapsed_us=%lld "
      "recv_waiters_x=%zu recv_waiters_y=%zu send_waiters_x=%zu "
      "send_waiters_y=%zu refs_x=%ld refs_y=%ld alive_a=%d alive_b=%d "
      "chx_closed=%d chy_closed=%d\n",
      config.mode.c_str(), round, stats.pass ? 1 : 0, finished ? 1 : 0,
      tasks_done ? 1 : 0, stats.stuck_observed ? 1 : 0, stats.stuck_before,
      stats.stuck_after, state->a.finished.load() ? 1 : 0,
      state->a.completed.load() ? 1 : 0,
      state->a.timed_out.load() ? 1 : 0,
      state->a.fallback.load() ? 1 : 0, state->a.index.load(),
      StatusName(static_cast<ChannelStatus>(state->a.status.load())),
      state->a.kind.load(), state->a.no_ready.load(), stats.a_elapsed_ms,
      state->a.elapsed_us.load(),
      state->b.finished.load() ? 1 : 0,
      state->b.completed.load() ? 1 : 0,
      state->b.timed_out.load() ? 1 : 0,
      state->b.fallback.load() ? 1 : 0, state->b.index.load(),
      StatusName(static_cast<ChannelStatus>(state->b.status.load())),
      state->b.kind.load(), state->b.no_ready.load(), stats.b_elapsed_ms,
      state->b.elapsed_us.load(),
      sx.recv_waiters, sy.recv_waiters, sx.send_waiters, sy.send_waiters,
      refs_x, refs_y, alive_a ? 1 : 0, alive_b ? 1 : 0,
      sx.closed ? 1 : 0, sy.closed ? 1 : 0);
  std::fflush(stdout);
  if (!stats.pass) {
    std::printf("FAIL mode=%s round=%d reason=assertion-failed\n",
                config.mode.c_str(), round);
    std::fflush(stdout);
  }
  return stats;
}

// ---------------------------------------------------------------------------
// baseline：纯阻塞互等（对照组）
// ---------------------------------------------------------------------------

int RunBaseline(Scheduler& scheduler, const Config& config) {
  auto state = std::make_shared<RoundState>();
  state->chx = MakeChannel<int>(0);
  state->chy = MakeChannel<int>(0);

  auto task_a = scheduler.spawn([state] { RunBlocking(state, true); });
  auto task_b = scheduler.spawn([state] { RunBlocking(state, false); });
  if (!task_a || !task_b) {
    std::printf("FAIL mode=baseline reason=spawn-failed\n");
    return 4;
  }

  const bool parked = WaitUntil(
      [&state] {
        const auto sx = state->chx->DebugSnapshot();
        const auto sy = state->chy->DebugSnapshot();
        return sx.recv_waiters >= 1U && sy.recv_waiters >= 1U &&
               sx.select_receivers == 0U && sy.select_receivers == 0U;
      },
      5s);
  if (!parked) {
    const auto sx = state->chx->DebugSnapshot();
    const auto sy = state->chy->DebugSnapshot();
    std::printf(
        "FAIL mode=baseline reason=not-parked chX.recv_waiters=%zu "
        "chY.recv_waiters=%zu\n",
        sx.recv_waiters, sy.recv_waiters);
    std::fflush(stdout);
    return 4;
  }

  const auto parked_at = std::chrono::steady_clock::now();
  const long long parked_us = SteadyUs(parked_at);
  const auto sx0 = state->chx->DebugSnapshot();
  const auto sy0 = state->chy->DebugSnapshot();
  std::printf(
      "DEADLOCK_READY mode=baseline stuck=%zu gid_a=%llu gid_b=%llu "
      "chX.recv_waiters=%zu chX.select_receivers=%zu "
      "chY.recv_waiters=%zu chY.select_receivers=%zu hold_ms=%d\n",
      sx0.recv_waiters + sy0.recv_waiters,
      static_cast<unsigned long long>(state->a.gid.load()),
      static_cast<unsigned long long>(state->b.gid.load()),
      sx0.recv_waiters, sx0.select_receivers, sy0.recv_waiters,
      sy0.select_receivers, config.hold_ms);
  std::fflush(stdout);

  int heartbeats = 0;
  int stuck = static_cast<int>(sx0.recv_waiters + sy0.recv_waiters);
  while (!g_stop && (NowUs() - parked_us) / 1000 < config.hold_ms) {
    std::this_thread::sleep_for(250ms);
    const auto sx = state->chx->DebugSnapshot();
    const auto sy = state->chy->DebugSnapshot();
    stuck = static_cast<int>(sx.recv_waiters + sy.recv_waiters);
    std::printf(
        "STILL_STUCK elapsed_ms=%lld stuck=%d chX.recv_waiters=%zu "
        "chY.recv_waiters=%zu\n",
        (NowUs() - parked_us) / 1000, stuck, sx.recv_waiters,
        sy.recv_waiters);
    std::fflush(stdout);
    ++heartbeats;
    if (stuck != 2) {
      break;
    }
  }

  const auto sx = state->chx->DebugSnapshot();
  const auto sy = state->chy->DebugSnapshot();
  stuck = static_cast<int>(sx.recv_waiters + sy.recv_waiters);
  const bool deadlock = stuck == 2 && !Terminal(task_a) && !Terminal(task_b);
  std::printf(
      "BASELINE_RESULT mode=baseline deadlock=%d stuck=%d recovered=0 "
      "held_ms=%lld heartbeats=%d stopped=%d a_alive=%d b_alive=%d pass=%d\n",
      deadlock ? 1 : 0, stuck, (NowUs() - parked_us) / 1000, heartbeats,
      g_stop ? 1 : 0, Terminal(task_a) ? 0 : 1,
      Terminal(task_b) ? 0 : 1, deadlock ? 1 : 0);
  std::fflush(stdout);
  return deadlock ? 0 : 4;
}

// ---------------------------------------------------------------------------
// 参数解析
// ---------------------------------------------------------------------------

bool ParseArgs(int argc, char** argv, Config* config) {
  for (int index = 1; index < argc; ++index) {
    std::string argument = argv[index];
    std::string inline_value;
    const std::size_t equals = argument.find('=');
    if (equals != std::string::npos) {
      inline_value = argument.substr(equals + 1);
      argument = argument.substr(0, equals);
    }
    const auto next = [&]() -> std::string {
      return index + 1 < argc ? argv[++index] : std::string{};
    };
    const auto value = [&]() -> std::string {
      return inline_value.empty() ? next() : inline_value;
    };
    if (argument == "--mode") {
      config->mode = value();
    } else if (argument == "--repro") {
      config->repro = std::atoi(value().c_str());
    } else if (argument == "--timeout-ms") {
      config->timeout_ms = std::atoi(value().c_str());
    } else if (argument == "--hold-ms") {
      config->hold_ms = std::atoi(value().c_str());
    } else if (argument == "--default-retries") {
      config->default_retries = std::atoi(value().c_str());
    } else if (argument == "--default-interval-ms") {
      config->default_interval_ms = std::atoi(value().c_str());
    } else if (argument == "--wait-ms") {
      config->wait_ms = std::atoi(value().c_str());
    } else if (argument == "--help" || argument == "-h") {
      std::printf(
          "usage: %s [--mode=baseline|timeout|multi|default] [--repro N]\n"
          "          [--timeout-ms N] [--hold-ms N] [--default-retries N]\n"
          "          [--default-interval-ms N] [--wait-ms N]\n",
          argv[0]);
      return false;
    } else {
      std::fprintf(stderr, "[demo] unknown argument: %s\n", argv[index]);
      return false;
    }
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  Config config;
  if (!ParseArgs(argc, argv, &config)) {
    return 2;
  }
  if (config.mode != "baseline" && config.mode != "timeout" &&
      config.mode != "multi" && config.mode != "default") {
    std::fprintf(stderr, "[demo] bad --mode: %s\n", config.mode.c_str());
    return 2;
  }
  if (config.repro < 1) {
    std::fprintf(stderr, "[demo] --repro must be >= 1\n");
    return 2;
  }
  if (config.mode == "baseline" && config.repro != 1) {
    std::fprintf(stderr,
                 "[demo] baseline 只允许 --repro 1（它的语义就是永久卡住）\n");
    return 2;
  }

  std::signal(SIGTERM, OnSignal);
  std::signal(SIGINT, OnSignal);

  SchedulerConfig scheduler_config;
  scheduler_config.processor_count = 2;
  scheduler_config.min_workers = 2;
  scheduler_config.max_workers = 4;
  scheduler_config.collect_metrics = true;
  Scheduler scheduler(scheduler_config);
  scheduler.start();

  std::printf("READY pid=%d mode=%s repro=%d timeout_ms=%d\n",
              static_cast<int>(::getpid()), config.mode.c_str(),
              config.repro, config.timeout_ms);
  std::fflush(stdout);

  int exit_code = 0;
  if (config.mode == "baseline") {
    exit_code = RunBaseline(scheduler, config);
  } else {
    int passed = 0;
    int failed = 0;
    long long total_a_ms = 0;
    long long total_b_ms = 0;
    long long min_ms = 0;
    long long max_ms = 0;
    for (int round = 1; round <= config.repro; ++round) {
      const RoundStats stats = RunSelectRound(scheduler, config, round);
      if (stats.pass) {
        ++passed;
      } else {
        ++failed;
      }
      total_a_ms += stats.a_elapsed_ms;
      total_b_ms += stats.b_elapsed_ms;
      const long long round_max =
          std::max(stats.a_elapsed_ms, stats.b_elapsed_ms);
      const long long round_min =
          std::min(stats.a_elapsed_ms, stats.b_elapsed_ms);
      if (round == 1 || round_max > max_ms) {
        max_ms = round_max;
      }
      if (round == 1 || round_min < min_ms) {
        min_ms = round_min;
      }
      std::printf(
          "ROUND mode=%s round=%d pass=%d a_elapsed_ms=%lld "
          "b_elapsed_ms=%lld\n",
          config.mode.c_str(), round, stats.pass ? 1 : 0,
          stats.a_elapsed_ms, stats.b_elapsed_ms);
      std::fflush(stdout);
    }
    const double avg_a =
        config.repro > 0 ? static_cast<double>(total_a_ms) / config.repro : 0.0;
    const double avg_b =
        config.repro > 0 ? static_cast<double>(total_b_ms) / config.repro : 0.0;
    std::printf(
        "REPRO mode=%s rounds=%d passed=%d failed=%d avg_a_ms=%.2f "
        "avg_b_ms=%.2f min_ms=%lld max_ms=%lld\n",
        config.mode.c_str(), config.repro, passed, failed, avg_a, avg_b,
        min_ms, max_ms);
    std::fflush(stdout);
    exit_code = failed == 0 ? 0 : 4;
  }

  scheduler.shutdown();
  return exit_code;
}
