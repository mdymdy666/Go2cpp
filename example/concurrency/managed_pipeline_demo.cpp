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
    std::clog << "[pipeline][场景1] 无缓冲 Channel 成功传递并在关闭后结束\n";
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
                std::clog << "[pipeline][场景1] consumer 收到状态="
                          << static_cast<int>(result.status) << '\n';
                break;
            }
            std::clog << "[pipeline][场景1] consumer 收到="
                      << result.ValueOrDefault() << '\n';
            sum.fetch_add(result.ValueOrDefault());
        }
    });
    auto producer = scheduler.Go([&] {
        for (int value = 1; value <= 3; ++value) {
            if (!channel->Send(value, context.first).Ok()) {
                std::clog << "[pipeline][场景1] producer 发送失败 value="
                          << value << '\n';
                success.store(false);
                break;
            }
        }
        channel->Close();
    });
    const bool joined = producer->wait_for(3s) && consumer->wait_for(3s);
    context.second();

    const bool success_path = joined && success.load() && sum.load() == 6;
    std::clog << "[pipeline][场景1] joined=" << joined
              << " sum=" << sum.load() << " success=" << success_path
              << '\n';

    std::clog << "[pipeline][场景2] 无发送者时由 Context 超时取消接收\n";
    auto cancel_context = go2cpp::WithTimeout(go2cpp::Background(), 30ms);
    auto cancel_channel = go2cpp::MakeChannel<int>(0);
    std::atomic<go2cpp::ChannelStatus> cancel_status{
        go2cpp::ChannelStatus::kInvalid};
    auto cancelled_consumer = scheduler.Go([&] {
        const auto result = cancel_channel->Recv(cancel_context.first);
        cancel_status.store(result.status, std::memory_order_release);
        std::clog << "[pipeline][场景2] consumer 返回 status="
                  << static_cast<int>(result.status) << '\n';
    });
    const bool cancelled_joined = cancelled_consumer->wait_for(2s);
    cancel_context.second();
    const auto observed_cancel = cancel_status.load(std::memory_order_acquire);
    const bool cancel_path = cancelled_joined &&
                             (observed_cancel == go2cpp::ChannelStatus::kCancelled ||
                              observed_cancel == go2cpp::ChannelStatus::kDeadlineExceeded ||
                              observed_cancel == go2cpp::ChannelStatus::kTimedOut);

    std::clog << "[pipeline][场景3] 错误路径：关闭后发送必须失败而不是阻塞\n";
    auto closed_channel = go2cpp::MakeChannel<int>(1);
    closed_channel->Close();
    const auto closed_send = closed_channel->Send(9);
    const bool closed_path = closed_send.Closed();
    std::clog << "[pipeline][场景3] send_after_close status="
              << static_cast<int>(closed_send.status) << '\n';

    scheduler.Shutdown();
    std::cout << "P=1 unbuffered pipeline sum=" << sum.load()
              << " success=" << success_path
              << " cancel=" << cancel_path
              << " closed_send=" << closed_path << '\n';
    return success_path && cancel_path && closed_path ? 0 : 1;
}
