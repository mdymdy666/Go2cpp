#include "go2cpp/runtime.hpp"

#include <chrono>
#include <iostream>
#include <string>

/**
 * @brief runtime 简单版：只包含一个聚合头，就能同时使用多个子系统。
 * @details runtime.hpp 本身没有 API，它是"一次包含全部公开模块"的便捷入口
 *          （面向转译程序；大工程仍可按模块分别包含以缩小依赖面）。
 *          本示例只用它一个头，演示三个子系统协同：
 *          ① Scheduler + go 提交 Fiber；
 *          ② Channel 在 Fiber 间传递数据；
 *          ③ Context 取消驱动退出。
 * @return 全部校验通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;
    using Ms = std::chrono::milliseconds;
    bool ok = true;

    // ① Scheduler + go
    std::clog << "[runtime][场景1] Scheduler + go\n";
    Scheduler scheduler(2);
    scheduler.start();
    std::atomic<int> ran{0};
    auto task = go(scheduler, [&ran] { ran.fetch_add(1); });
    ok = task->wait() && ok;
    std::cout << "  Fiber 执行次数=" << ran.load() << "（预期 1）\n";

    // ② Channel 传递数据
    std::clog << "[runtime][场景2] Channel 传值\n";
    auto channel = MakeChannel<std::string>(0);
    std::string received;
    auto sender = go(scheduler, [channel] { (void)channel->Send("payload"); });
    if (const auto message = channel->Recv()) {
        received = *message.value;
    }
    sender->wait();
    std::cout << "  收到=\"" << received << "\"（预期 payload）\n";
    ok = ok && received == "payload";

    // ③ Context 取消驱动退出
    std::clog << "[runtime][场景3] Context 取消\n";
    auto pair = WithTimeout(Background(), Ms(20));
    const bool done = pair.first->Done().WaitFor(Ms(1000));
    const bool deadline = Is(pair.first->Err(), DeadlineExceededError());
    std::cout << "  超时完成=" << done << " 是 DeadlineExceeded=" << deadline
              << "（预期 1 1）\n";
    ok = ok && done && deadline;

    scheduler.shutdown();
    std::clog << (ok ? "[runtime] 全部通过\n" : "[runtime] 有失败\n");
    return ok ? 0 : 1;
}
