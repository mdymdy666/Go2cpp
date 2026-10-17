#include "go2cpp/runtime.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

// Channel 全面版：同一个组件的 6 个使用场景。
//   1  无缓冲 = 同步交接（发送方要等到接收方取走）
//   2  有缓冲 + 生产者/消费者 + Close 收尾
//   3  select 多路复用（多个就绪 case 中挑一个）
//   4  select + DefaultCase（非阻塞探测）
//   5  select + 超时（没有对端时应及时返回）
//   6  Context 取消传播到 Recv（取消后不再永久等待）
//
// 每个场景独立成段，互不依赖；校验失败只记 ok=false，最后统一报告。


namespace {

using namespace std::chrono_literals;

bool scenario_unbuffered(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景1] 无缓冲 Channel：发送方阻塞到接收方取走\n";
    auto ch = MakeChannel<std::string>(0);
    auto sender = go(scheduler, [ch] { (void)ch->Send("handoff"); });
    // 无缓冲下 Recv 会 park 当前执行流，直到 sender 完成交接
    const auto got = ch->Recv();
    sender->wait();
    if (!got) {
        std::clog << "  失败：没有取到值\n";
        return false;
    }
    std::cout << "  交接内容: " << *got.value << '\n';
    return *got.value == "handoff";
}

bool scenario_buffered_pipeline(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景2] 有缓冲 Channel：生产者/消费者 + Close 收尾\n";
    auto queue = MakeChannel<int>(4);
    constexpr int kItems = 10;

    auto producer = go(scheduler, [queue] {
        for (int i = 1; i <= kItems; ++i) {
            (void)queue->Send(i);      // 缓冲满时自动 park
        }
        queue->Close();                // 关闭即"上游结束"
    });

    int sum = 0;
    int count = 0;
    // Close 之后 Recv 返回 Closed()，operator bool 为 false，循环自然结束
    while (const auto got = queue->Recv()) {
        sum += *got.value;
        ++count;
    }
    producer->wait();
    std::cout << "  消费 " << count << " 项，合计 " << sum << "（预期 10 项 / 55）\n";
    return count == kItems && sum == 55;
}

bool scenario_select_multiplex() {
    using namespace go2cpp;
    std::clog << "[full][场景3] select：多路复用，命中就绪的那一路\n";
    auto fast = MakeChannel<int>(1);
    auto slow = MakeChannel<int>(1);
    (void)slow->Send(200);             // 只有 slow 就绪

    const auto result = Select({RecvCase(fast), RecvCase(slow)}, {}, 100ms);
    if (!result.selected || !result.ok) {
        std::clog << "  失败：没有命中\n";
        return false;
    }
    std::cout << "  命中 index=" << result.index << "（预期 1 = slow）\n";
    return result.index == 1;
}

bool scenario_select_default() {
    using namespace go2cpp;
    std::clog << "[full][场景4] select + DefaultCase：非阻塞探测\n";
    auto empty = MakeChannel<int>(1);
    const auto result = Select({RecvCase(empty), DefaultCase()}, {}, 0ms);
    const bool hit_default = result.selected && result.index == 1;
    std::cout << "  命中 default=" << hit_default << "（预期 1）\n";
    return hit_default;
}

bool scenario_select_timeout() {
    using namespace go2cpp;
    std::clog << "[full][场景5] select + 超时：没有发送者时及时返回\n";
    auto lonely = MakeChannel<int>(0);
    const auto result = Select({RecvCase(lonely)}, {}, 30ms);
    const bool timed_out = !result.selected &&
                           result.status == ChannelStatus::kTimedOut;
    std::clog << "  selected=" << result.selected
              << " status=" << static_cast<int>(result.status)
              << "（预期 kTimedOut）\n";
    return timed_out;
}

bool scenario_cancellation(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景6] Context 取消传播到 Recv：不再永久等待\n";
    auto ch = MakeChannel<int>(0);
    auto pair = WithCancel(Background());
    const auto ctx = pair.first;
    const auto cancel = pair.second;

    auto canceller = go(scheduler, [cancel] {
        std::this_thread::sleep_for(20ms);
        cancel();
    });

    const auto got = ch->Recv(ctx);    // 取消后应返回 Cancelled 而不是一直挂着
    canceller->wait();
    const bool cancelled = got.Cancelled();
    std::clog << "  Cancelled=" << cancelled
              << " status=" << static_cast<int>(got.status) << '\n';
    return cancelled;
}

}  // namespace

/**
 * @brief Channel 全面版：6 个使用场景，覆盖交接、缓冲、多路复用、超时与取消。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;
    using namespace std::chrono_literals;

    Scheduler scheduler(4);
    scheduler.start();

    bool ok = true;
    ok = scenario_unbuffered(scheduler) && ok;
    ok = scenario_buffered_pipeline(scheduler) && ok;
    ok = scenario_select_multiplex() && ok;
    ok = scenario_select_default() && ok;
    ok = scenario_select_timeout() && ok;
    ok = scenario_cancellation(scheduler) && ok;

    scheduler.shutdown();
    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}