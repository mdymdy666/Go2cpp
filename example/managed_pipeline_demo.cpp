#include "go2cpp/channel.hpp"
#include "go2cpp/context.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <iostream>

/**
 * @brief 演示带超时 Context 的无缓冲 Channel 生产者/消费者管线。
 * @details 消费者在 Context 允许期间读取并累加数据，生产者发送完毕后关闭
 *          Channel；任一端失败都会通过结果状态报告给主线程。
 * @return 管线完成且总和为 6 时返回 0。
 */
int main() {
    using namespace std::chrono_literals;
    go2cpp::Scheduler scheduler(1);
    auto context = go2cpp::WithTimeout(go2cpp::Background(), 2s);
    auto channel = go2cpp::MakeChannel<int>(0);
    std::atomic<int> sum{0};
    std::atomic<bool> success{true};
    scheduler.Start();
    auto consumer = scheduler.Go([&] {
        for (;;) {
            const auto result = channel->Recv(context.first);
            if (!result.Ok()) {
                break;
            }
            sum.fetch_add(result.ValueOrDefault());
        }
    });
    auto producer = scheduler.Go([&] {
        for (int value = 1; value <= 3; ++value) {
            if (!channel->Send(value, context.first).Ok()) {
                success.store(false);
                break;
            }
        }
        channel->Close();
    });
    const bool joined = producer->wait_for(3s) && consumer->wait_for(3s);
    context.second();
    scheduler.Shutdown();
    std::cout << "P=1 unbuffered pipeline sum=" << sum.load()
              << " context-done=" << context.first->Done().IsDone() << '\n';
    return joined && success.load() && sum.load() == 6 ? 0 : 1;
}
