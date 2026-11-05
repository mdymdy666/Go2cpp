/**
 * @file 02-blocking-in-thread.cpp
 * @brief 异常场景 02：在普通线程（非 Fiber）里使用 Channel / Mutex。
 *
 * ## 场景
 * 把 Go 风格的阻塞代码直接搬进 `std::thread`，或在单线程里写
 * 「先 `Send` 再 `Recv`」。Go 的 runtime 会把阻塞的 goroutine 挂起、把
 * 线程让给别人；普通线程没有这层调度，阻塞就是**真的把 OS 线程卡住**。
 *
 * ## 默认行为（不写任何额外代码时）
 * - 普通线程的 park 走 `ParkingCondition` 的 native 路径：
 *   自旋一小会儿后进入 futex 等待，阻塞整个 OS 线程
 *   （src/concurrency/parking_condition.cpp:407 `WaitNative`；
 *   底层 futex 系统调用在同文件 :83）。
 * - 因此单线程「先 `Send` 后 `Recv`」**必然自死锁**：无缓冲 Channel 的
 *   `Send`（include/go2cpp/concurrency/channel.hpp:1626）要等到有接收者，
 *   而唯一的线程正卡在 `Send` 里，永远走不到下一行 `Recv`。
 *   本文件不执行真正的死锁，改用 `SendFor` 超时证明「它会一直等」。
 * - 有缓冲但已满时同理：`Send` 一样会 park 到有消费者为止。
 * - Mutex 也一样：普通线程 `Lock()` 拿不到锁就阻塞线程；
 *   它不会像 Fiber 那样让出 M。
 *
 * ## 可选策略
 * A. 改在 `go()` / `Scheduler::Go()` 里跑：阻塞只 park Fiber、让出 M，
 *    同一 P 上的其他 G 继续执行；
 * B. 带缓冲 Channel + `TrySend`：发送不阻塞，队列满时由调用方决定
 *    （丢弃 / 退避 / 扩大缓冲）；
 * C. 显式 `BlockingRegion`：声明「接下来会阻塞 OS 线程」，让 sysmon
 *    补一个 M 接管当前 P（适合必须调用未 Hook 的第三方阻塞函数）；
 * D. 给等待加超时：`SendFor`/`RecvFor`/`TrySend`/`TryRecv`，至少不会
 *    永久自死锁。
 *
 * ## 如何选择
 * - 服务端业务逻辑：A（保持同步写法，又不占线程）；
 * - 热路径上的「能发就发」：B；
 * - 绕不开的原生阻塞调用：C（并确认 `active()==true`）；
 * - 边界/清理路径：D（宁可返回失败，也不要挂住调用线程）。
 *
 * @return 全部断言通过返回 0，否则返回 1。
 */

#include "go2cpp/go.hpp"
#include "go2cpp/runtime.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace go2cpp;
using namespace std::chrono_literals;

int g_failures = 0;

/// @brief 记录一条断言，失败时打印并递增全局失败计数。
bool Check(bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "  [失败] " << what << '\n';
    ++g_failures;
  }
  return condition;
}

/// @brief 场景 1：普通线程的 Recv 会把整个 OS 线程阻塞住。
void ScenarioNativeThreadBlocks() {
  std::clog << "[场景1][默认] std::thread 里的 Recv 阻塞整个 OS 线程\n";
  auto ch = MakeChannel<int>(0);
  std::atomic<long long> waited_ms{0};
  std::atomic<bool> got_expected{false};

  std::thread worker([&] {
    const auto begin = std::chrono::steady_clock::now();
    const ChannelRecvResult<int> got = ch->Recv();
    waited_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - begin)
                        .count());
    got_expected.store(got.Ok() && got.value.has_value() && *got.value == 42);
  });

  // 故意让接收者先等 120ms：这段时间 worker 线程被 futex 挂起，不做事。
  std::this_thread::sleep_for(120ms);
  const ChannelSendResult sent = ch->Send(42);
  worker.join();

  std::cout << "  接收线程阻塞了 " << waited_ms.load()
            << "ms（预期 >= 120），收到 42=" << got_expected.load() << '\n';
  Check(sent.Ok(), "主线程 Send 应完成交接");
  Check(got_expected.load(), "接收线程应拿到 42");
  Check(waited_ms.load() >= 100, "Recv 应真实阻塞 OS 线程约 120ms");
}

/// @brief 场景 2：单线程「先 Send 后 Recv」会自死锁——用超时安全演示。
void ScenarioSingleThreadSelfDeadlock() {
  std::clog << "[场景2][默认] 单线程先 Send 后 Recv：无接收者，永远等不到\n";
  auto solo = MakeChannel<int>(0);
  const auto begin = std::chrono::steady_clock::now();
  const ChannelSendResult result = solo->SendFor(1, 150ms);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - begin)
                           .count();

  std::cout << "  SendFor(150ms).status = " << ChannelStatusName(result.status)
            << "，实际等待 " << elapsed << "ms（预期 kTimedOut / >= 150）\n";
  Check(result.status == ChannelStatus::kTimedOut,
        "无接收者时 SendFor 应超时返回");
  std::clog << "  若换成 Send：这里永远回不来，下一行 Recv 永远执行不到 —— 自死锁。\n";
}

/// @brief 策略 A：同样的交接放进 go()，阻塞只 park Fiber、不占线程。
void ScenarioRunInFiber(Scheduler& scheduler) {
  std::clog << "[场景3][可选] 放进 go()：Send 只 park Fiber，主线程立刻可继续\n";
  auto ch = MakeChannel<int>(0);
  auto sender = scheduler.Go([ch] { (void)ch->Send(9); });

  const ChannelRecvResult<int> got = ch->Recv();
  Check(got.Ok() && got.value.has_value() && *got.value == 9,
        "Fiber 发送方与线程接收方应完成交接");
  Check(sender->wait_for(2s), "发送 Fiber 应正常结束");
  std::cout << "  收到 " << (got.value ? std::to_string(*got.value) : "<无>")
            << "，发送 Fiber 已结束\n";
}

/// @brief 策略 B：带缓冲 + TrySend，发送路径不阻塞。
void ScenarioBufferedTrySend() {
  std::clog << "[场景4][可选] 带缓冲 + TrySend：发不进去就立刻返回\n";
  auto ch = MakeChannel<int>(2);
  const ChannelSendResult first = ch->TrySend(1);
  const ChannelSendResult second = ch->TrySend(2);
  const ChannelSendResult overflow = ch->TrySend(3);

  std::cout << "  第 1/2 次 TrySend: " << ChannelStatusName(first.status) << "/"
            << ChannelStatusName(second.status) << "；第 3 次: "
            << ChannelStatusName(overflow.status) << "（预期 kReady/kReady/kWouldBlock）\n";
  Check(first.Ok() && second.Ok(), "缓冲未满时 TrySend 应成功");
  Check(overflow.status == ChannelStatus::kWouldBlock,
        "缓冲满时 TrySend 应返回 kWouldBlock");
}

/// @brief 策略 C：BlockingRegion 声明原生阻塞，让调度器补 M。
void ScenarioBlockingRegion(Scheduler& scheduler) {
  std::clog << "[场景5][可选] BlockingRegion：显式声明「要占住 OS 线程」\n";
  std::atomic<bool> active{false};
  auto task = scheduler.Go([&active] {
    BlockingRegion region;  // 析构时离开阻塞区
    active.store(region.active());
  });
  Check(task->wait_for(2s), "BlockingRegion 场景的 Fiber 应正常结束");
  std::cout << "  region.active() = " << active.load()
            << "（预期 1：受管 Fiber 内可声明阻塞区）\n";
  Check(active.load(), "Fiber 内 BlockingRegion 应处于激活状态");
}

}  // namespace

/// @brief 演示普通线程使用 Channel 的默认阻塞行为与四种可选策略。
/// @return 全部断言通过返回 0，否则返回 1。
int main() {
  ScenarioNativeThreadBlocks();
  ScenarioSingleThreadSelfDeadlock();

  // 复用进程级默认调度器（首次创建时 P=2），不再自建实例。
  Scheduler& scheduler = default_scheduler(2);
  ScenarioRunInFiber(scheduler);
  ScenarioBufferedTrySend();
  ScenarioBlockingRegion(scheduler);
  shutdown_default_scheduler();

  if (g_failures == 0) {
    std::clog << "[02-blocking-in-thread] 全部通过\n";
    return 0;
  }
  std::cerr << "[02-blocking-in-thread] 失败 " << g_failures << " 项\n";
  return 1;
}
