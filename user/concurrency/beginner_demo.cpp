#include "go2cpp/runtime.hpp"

#include <chrono>
#include <iostream>
#include <string>

/**
 * @brief 演示新手友好的 go、Channel 和 SelectLoop 基本用法。
 * @details 先创建并启动 Scheduler，再用 go(scheduler, callback) 提交 Fiber；
 *          同一个示例还展示默认调度器和无缓冲 Channel 的收发流程。
 * @return 所有 Fiber 正常完成时返回 0。
 */
int main() {
    using namespace go2cpp;
    using namespace std::chrono_literals;

    std::clog << "[beginner][场景1] 显式 Scheduler + go() + Channel\n";
    // 只需一个 Scheduler，就可以用 go() 提交 Fiber；等待仍然是可回收的。
    Scheduler scheduler(2);
    scheduler.start();
    auto task = go(scheduler, [] {
        std::clog << "[beginner][场景1] Fiber 已在 Scheduler 中执行\n";
    });
    task->wait();

    auto channel = MakeChannel<int>(0);
    SelectLoop select;
    select.bind(RecvCase(channel), [](const SelectResult& result) {
        if (const auto value = result.Value<int>()) {
            std::cout << "收到: " << *value << '\n';
        }
    });
    auto sender = go(scheduler, [channel] {
        std::clog << "[beginner][场景1] 发送 2026\n";
        (void)channel->Send(2026);
    });
    (void)select.work({}, 1s);
    sender->wait();

    std::clog << "[beginner][场景2] Select 超时：没有发送者时应及时返回\n";
    auto timeout_channel = MakeChannel<int>(0);
    const auto timeout_result = Select({RecvCase(timeout_channel)}, {}, 20ms);
    const bool timeout_ok = !timeout_result.selected &&
                            timeout_result.status == ChannelStatus::kTimedOut;
    std::clog << "[beginner][场景2] selected=" << timeout_result.selected
              << " status=" << static_cast<int>(timeout_result.status)
              << "（预期 kTimedOut）\n";

    std::clog << "[beginner][场景3] 错误路径：nil Channel 不会永久等待\n";
    ChannelPtr<int> nil_channel;
    const auto nil_result = Select({RecvCase(nil_channel)}, {}, 20ms);
    const bool nil_ok = nil_result.selected &&
                        nil_result.status == ChannelStatus::kNil;
    std::clog << "[beginner][场景3] selected=" << nil_result.selected
              << " status=" << static_cast<int>(nil_result.status)
              << "（预期 kNil）\n";

    scheduler.shutdown();

    // 不想管理 Scheduler 时可以使用进程级默认调度器；结束前显式关闭它。
    std::clog << "[beginner][场景4] 进程级默认 Scheduler 的 go() 入口\n";
    auto default_task = go([] { std::clog << "[beginner][场景4] 默认任务完成\n"; });
    default_task->wait();
    shutdown_default_scheduler();
    return timeout_ok && nil_ok ? 0 : 1;
}
