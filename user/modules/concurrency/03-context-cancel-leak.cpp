/**
 * @file 03-context-cancel-leak.cpp
 * @brief 异常场景 03：`WithCancel` 之后忘记调用 `cancel()`，导致 goroutine 泄漏。
 *
 * ## 场景
 * 派生一个可取消 Context，把 `ctx->Done()` 交给后台 goroutine 当作退出信号，
 * 然后忘记（或异常路径跳过）调用 `cancel()`。goroutine 会永远 park 在
 * `Done()` 上，任务不进入终态、栈和捕获的对象都不释放——这就是 Go 里最
 * 常见的 goroutine 泄漏形态。
 *
 * ## 默认行为（不写任何额外代码时）
 * - `WithCancel` 只做两件事：建子节点、返回 `CancelFunc`
 *   （include/go2cpp/concurrency/context.hpp:230），**不会**注册任何自动
 *   取消逻辑；只有 `Cancel()`（同文件 :304）或父级取消才会让
 *   `Done()` 关闭。
 * - 因此「作用域结束 / 对象析构」都不等于取消：等待 `ctx->Done()` 的
 *   goroutine 会一直 park，直到 `Scheduler::shutdown()` 把未完成的 G
 *   统一取消（Go 的 `main` 返回语义）。
 * - `WithTimeout` / `WithDeadline` 是例外：它们自带定时器，到点自动
 *   `CancelDeadline()`，不依赖调用方。
 *
 * ## 可选策略
 * A. RAII guard：析构时调用 `cancel()`，即 Go `defer cancel()` 的 C++ 等价物；
 * B. `WithTimeout`：给子树一个上限，到点自动取消，调用方即使漏掉也不会永久泄漏；
 * C. 主动检查 `ctx->Done()` / `IsDone()` / `Err()`：让等待方在取消后走
 *    明确的退出分支，而不是只靠「永远 Wait」；
 * D. 在等待处同时监听第二个退出信号（业务 channel 关闭 / 停止标志），
 *    避免把生命周期完全押在一个 Context 上。
 *
 * ## 如何选择
 * - 短生命周期请求：A（每个 `WithCancel` 后面紧跟一个 guard）；
 * - 外部调用 / 网络等待：B（有上限，超时自动收）；
 * - 后台循环：C（每轮检查，不依赖别人记得取消）；
 * - 多来源退出（停机 + 取消 + 队列关闭）：D。
 *
 * @return 全部断言通过返回 0，否则返回 1。
 */

#include "go2cpp/runtime.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <utility>

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

/**
 * @brief `defer cancel()` 的 C++ 等价物：析构时取消，不可复制。
 *
 * @details 只持有一个 `CancelFunc`；`Release()` 用于「正常路径已经把
 *   所有权交出去」时跳过析构取消。`CancelFunc` 本身幂等，重复调用安全。
 */
class CancelGuard final {
 public:
  /// @param cancel [in] 要接管所有权的取消函数；可为空（此时析构不做事）。
  explicit CancelGuard(CancelFunc cancel) : m_cancel(std::move(cancel)) {}
  ~CancelGuard() {
    if (m_cancel) {
      m_cancel();
    }
  }

  CancelGuard(const CancelGuard&) = delete;
  CancelGuard& operator=(const CancelGuard&) = delete;

  /// @brief 放弃析构时取消（例如取消权已经转交给别处）。
  void Release() noexcept { m_cancel = nullptr; }

 private:
  CancelFunc m_cancel;
};

/// @brief 默认路径：忘记 cancel，等待 Done 的 goroutine 一直不退出。
void ScenarioForgotCancel(Scheduler& scheduler) {
  std::clog << "[场景1][默认] WithCancel 后忘记 cancel：goroutine 永久 park\n";
  auto pair = WithCancel(Background());
  std::atomic<bool> exited{false};

  auto task = scheduler.Go([ctx = pair.first, &exited] {
    ctx->Done().Wait();  // 没人取消就一直等
    exited.store(true);
  });

  std::this_thread::sleep_for(200ms);
  std::cout << "  200ms 后 goroutine 已退出=" << exited.load()
            << "，ctx->IsDone()=" << pair.first->IsDone() << "（预期 0 0）\n";
  Check(!exited.load(), "未 cancel 时等待 Done 的 goroutine 不应结束");
  Check(!pair.first->IsDone(), "默认 Context 不会自动取消");

  // 收尾：真实项目里漏掉这一行，这个 G 会一直挂到 scheduler.shutdown()。
  pair.second();
  Check(task->wait_for(2s), "cancel 后等待中的 goroutine 应立即被唤醒");
  Check(exited.load(), "cancel 后 goroutine 应退出");
}

/// @brief 策略 A：RAII guard 在作用域结束时自动 cancel。
void ScenarioRaiiGuard(Scheduler& scheduler) {
  std::clog << "[场景2][可选] CancelGuard：析构即 cancel（defer cancel 等价物）\n";
  auto pair = WithCancel(Background());
  std::atomic<bool> exited{false};
  std::shared_ptr<Task> task;

  {
    CancelGuard guard(pair.second);  // 离开作用域自动取消
    task = scheduler.Go([ctx = pair.first, &exited] {
      ctx->Done().Wait();
      exited.store(true);
    });
    std::this_thread::sleep_for(50ms);
    Check(!exited.load(), "guard 作用域内尚未取消，goroutine 仍应在等待");
  }  // guard 析构 → cancel()

  Check(pair.first->IsDone(), "guard 析构后 Context 应已取消");
  Check(task->wait_for(2s), "guard 析构后 goroutine 应被唤醒并结束");
  Check(exited.load(), "guard 析构后 goroutine 应退出");
  std::cout << "  guard 析构后 IsDone=" << pair.first->IsDone()
            << "，goroutine 已退出=" << exited.load() << "（预期 1 1）\n";
}

/// @brief 策略 B：WithTimeout 到点自动取消，不依赖调用方记得 cancel。
void ScenarioWithTimeout(Scheduler& scheduler) {
  std::clog << "[场景3][可选] WithTimeout：到点自动取消，无需手动 cancel\n";
  auto pair = WithTimeout(Background(), 100ms);
  std::atomic<bool> exited{false};

  auto task = scheduler.Go([ctx = pair.first, &exited] {
    ctx->Done().Wait();
    exited.store(true);
  });

  Check(task->wait_for(2s), "超时到点后等待中的 goroutine 应结束");
  Check(pair.first->IsDone(), "WithTimeout 到点后 Context 应处于已取消状态");
  Check(Is(pair.first->Err(), DeadlineExceededError()),
        "自动取消的原因应是 DeadlineExceeded");
  std::cout << "  到点后 exited=" << exited.load()
            << "，Err 是 DeadlineExceeded="
            << Is(pair.first->Err(), DeadlineExceededError()) << "（预期 1 1）\n";
  pair.second();  // 幂等清理：提前释放定时器资源
}

/// @brief 策略 C：调用方主动检查 Done/Err，走明确的退出分支。
void ScenarioCheckDone() {
  std::clog << "[场景4][可选] 主动检查 Done()/Err()，不等别人替你收尾\n";
  auto pair = WithCancel(Background());
  pair.second();  // 模拟父级或外部取消

  const bool done = pair.first->Done().WaitFor(100ms);
  const bool canceled = Is(pair.first->Err(), CanceledError());
  std::cout << "  Done().WaitFor=" << done << "，Err 是 CanceledError="
            << canceled << "（预期 1 1）\n";
  Check(done, "取消后 Done() 应立刻完成");
  Check(canceled, "主动取消的原因应是 Canceled");
}

}  // namespace

/// @brief 演示 WithCancel 泄漏的默认行为与三种可选收尾策略。
/// @return 全部断言通过返回 0，否则返回 1。
int main() {
  Scheduler scheduler(2);
  scheduler.start();

  ScenarioForgotCancel(scheduler);
  ScenarioRaiiGuard(scheduler);
  ScenarioWithTimeout(scheduler);
  ScenarioCheckDone();

  scheduler.shutdown();

  if (g_failures == 0) {
    std::clog << "[03-context-cancel-leak] 全部通过\n";
    return 0;
  }
  std::cerr << "[03-context-cancel-leak] 失败 " << g_failures << " 项\n";
  return 1;
}
