/**
 * @file 01-send-on-closed.cpp
 * @brief 异常场景 01：向已关闭的 Channel 发送消息。
 *
 * ## 场景
 * 关闭方（通常是上游生产者或超时清理逻辑）已经 `Close()`，另一个生产者
 * 仍持有同一个 `Channel<T>` 并继续 `Send`。Go 里这是
 * `panic: send on closed channel`，也是服务端最常见的崩溃来源之一：
 * 关闭与发送发生在两个执行流，编译器无法替你把关。
 *
 * ## 默认行为（不写任何额外代码时）
 * - `Channel<T>::Send` 在加锁后发现 `m_closed` 立即返回
 *   `ChannelSendResult{ChannelStatus::kClosed}`，**不 panic、不抛异常**
 *   （关闭判定：include/go2cpp/concurrency/channel.hpp:2894；
 *   入口：同文件 :1626）。
 * - `ChannelSendResult` 标了 `[[nodiscard]]`（channel.hpp:521）：
 *   `Send`/`SendFor`/`TrySend` 的返回值不能静默丢弃。若写成
 *   `ch->Send(v);`，`-Wall` 会给出 `-Wunused-result` 告警——关闭后的
 *   `kClosed` 不会被伪装成发送成功。
 * - `Close()` 是幂等的：重复关闭返回 `kAlreadyClosed`（channel.hpp:1830），
 *   不会抛异常。
 * - 关闭只阻止后续 `Send`；缓冲区里已有的值仍能被 `Recv` 读完
 *   （channel.hpp:3381 附近的关闭分支）。
 *
 * ## 可选策略
 * A. 检查返回值并记录（默认推荐）：`if (!r.Ok()) log(...)`；
 * B. `SendOrThrow`（channel.hpp:1679）：关闭时抛 `std::logic_error`
 *    （消息固定为 "send on closed channel"），对齐 Go 的 panic，
 *    适合「关闭后再发就是 bug」的新代码；
 * C. 关闭前排空（drain）：`Close()` 后循环 `Recv()` 读完剩余值，
 *    或在生产者确认全部 `Send` 完成后再 `Close()`；
 * D. `SendFor` 给发送加超时，避免关闭竞态下无限阻塞。
 *
 * ## 如何选择
 * - 库代码 / 跨模块边界：选 A —— 不抛异常，把「要不要失败」交给调用方；
 * - 内部不变量「关闭后不可能再发」：选 B —— 让 bug 在第一次出现时暴露；
 * - 生产者/消费者收尾：选 C —— 关闭表示「不会再有新值」，不是「丢弃旧值」；
 * - 关闭时机不确定、又不能无限等：选 D（`SendFor` 或 `TrySend`）。
 *
 * @return 全部断言通过返回 0，否则返回 1。
 */

#include "go2cpp/runtime.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace go2cpp;

int g_failures = 0;

/// @brief 记录一条断言，失败时打印并递增全局失败计数。
bool Check(bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "  [失败] " << what << '\n';
    ++g_failures;
  }
  return condition;
}

/// @brief 策略 A：检查 Send 返回值，不抛异常（默认路径）。
void ScenarioCheckReturnValue() {
  std::clog << "[场景1][默认] 关闭后 Send 返回 kClosed，不 panic、不抛异常\n";
  auto ch = MakeChannel<int>(1);

  const ChannelSendResult before_close = ch->Send(1);
  Check(before_close.Ok(), "Close 之前的 Send 应成功");

  const ChannelCloseResult closed = ch->Close();
  Check(closed.Ok(), "首次 Close 应成功");

  // [[nodiscard]]：必须把返回值接住并显式判断。
  const ChannelSendResult after_close = ch->Send(2);
  std::cout << "  Send(2).status = " << ChannelStatusName(after_close.status)
            << "（预期 kClosed）\n";
  Check(after_close.status == ChannelStatus::kClosed,
        "关闭后 Send 应返回 kClosed");
  Check(!after_close.Ok(), "kClosed 不能被当成发送成功");
  if (after_close.error) {
    std::cout << "  错误对象可读：" << after_close.error->Message() << '\n';
  }
}

/// @brief 策略 B：SendOrThrow 把「关闭后再发」升级成异常。
void ScenarioSendOrThrow() {
  std::clog << "[场景2][可选] SendOrThrow：关闭即抛 std::logic_error\n";
  auto ch = MakeChannel<int>(1);
  ch->Close();

  bool caught = false;
  try {
    ch->SendOrThrow(7);
  } catch (const std::logic_error& error) {
    caught = true;
    std::cout << "  捕获 logic_error: " << error.what()
              << "（预期 send on closed channel）\n";
    Check(std::string(error.what()) == "send on closed channel",
          "异常消息应与 Go 的 panic 文本一致");
  } catch (const std::exception& error) {
    std::cerr << "  捕获到非预期异常: " << error.what() << '\n';
  }
  Check(caught, "SendOrThrow 在已关闭 Channel 上应抛异常");
}

/// @brief 策略 C：关闭后排空缓冲区，旧值不丢。
void ScenarioDrainBeforeExit() {
  std::clog << "[场景3][可选] 关闭后 drain：缓冲区里的旧值仍能读完\n";
  auto ch = MakeChannel<int>(3);
  for (int value = 10; value <= 30; value += 10) {
    const ChannelSendResult sent = ch->Send(value);
    Check(sent.Ok(), "缓冲未满时 Send 应成功");
  }
  ch->Close();

  int sum = 0;
  int count = 0;
  while (const auto got = ch->Recv()) {
    sum += *got.value;
    ++count;
  }
  std::cout << "  drain 收到 " << count << " 项，合计 " << sum
            << "（预期 3 项 / 60）\n";
  Check(count == 3, "Close 后应能取回全部缓冲值");
  Check(sum == 60, "Close 不应丢弃缓冲区里的值");

  // 排空之后再取：kClosed 作为「流结束」的稳定信号。
  const ChannelRecvResult<int> end = ch->Recv();
  Check(end.status == ChannelStatus::kClosed,
        "缓冲区空且已关闭时 Recv 应返回 kClosed");
  std::cout << "  排空后再次 Recv: " << ChannelStatusName(end.status) << '\n';
}

}  // namespace

/// @brief 演示向已关闭 Channel 发送的默认处理与三种可选策略。
/// @return 全部断言通过返回 0，否则返回 1。
int main() {
  ScenarioCheckReturnValue();
  ScenarioSendOrThrow();
  ScenarioDrainBeforeExit();

  if (g_failures == 0) {
    std::clog << "[01-send-on-closed] 全部通过\n";
    return 0;
  }
  std::cerr << "[01-send-on-closed] 失败 " << g_failures << " 项\n";
  return 1;
}
