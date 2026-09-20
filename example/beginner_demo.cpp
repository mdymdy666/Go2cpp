#include "go2cpp/runtime.hpp"

#include <chrono>
#include <iostream>

int main() {
    using namespace go2cpp;
    using namespace std::chrono_literals;

    // 只需一个 Scheduler，就可以用 go() 提交 Fiber；等待仍然是可回收的。
    Scheduler scheduler(2);
    scheduler.start();
    auto task = go(scheduler, [] { std::cout << "显式调度器中的 go 任务\n"; });
    task->wait();

    auto channel = MakeChannel<int>(0);
    SelectLoop select;
    select.bind(RecvCase(channel), [](const SelectResult& result) {
        if (const auto value = result.Value<int>()) {
            std::cout << "收到: " << *value << '\n';
        }
    });
    auto sender = go(scheduler, [channel] {
        (void)channel->Send(2026);
    });
    (void)select.work({}, 1s);
    sender->wait();
    scheduler.shutdown();

    // 不想管理 Scheduler 时可以使用进程级默认调度器；结束前显式关闭它。
    auto default_task = go([] { std::cout << "默认调度器中的 go 任务\n"; });
    default_task->wait();
    shutdown_default_scheduler();
    return 0;
}
