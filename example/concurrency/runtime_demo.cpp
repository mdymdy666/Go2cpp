#include "go2cpp/channel.hpp"
#include "go2cpp/context.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <iostream>

/**
 * @brief 演示 Runtime、Context、Channel、Timer 和 Select 的组合用法。
 * @details 该示例按运行时初始化、任务提交、超时取消、Channel 关闭和资源
 *          回收的顺序组织，可作为完整程序的最小模板。
 * @return 所有运行时操作完成时返回 0。
 */
int main() {
    using namespace std::chrono_literals;
    using namespace go2cpp;

    std::clog << "[runtime][场景1] GMP + Context.Value + Channel 成功传递\n";
    SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    config.local_queue_limit = 8;
    Scheduler scheduler(config);
    scheduler.start();

    auto context_pair = WithTimeout(Background(), 2s);
    ContextKey<int> demo_key("demo-number");
    const auto value_context = WithValue(context_pair.first, demo_key, 7);
    auto cancel_context_pair = WithCancel(value_context);
    auto channel = MakeChannel<int>(1);
    std::atomic<int> received{0};

    auto receiver_task = scheduler.spawn([&] {
        const auto result = channel->Recv(cancel_context_pair.first);
        if (result.Ok()) {
            received.store(result.ValueOrDefault(), std::memory_order_release);
            std::clog << "[runtime][场景1] 收到 channel value="
                      << result.ValueOrDefault() << '\n';
        }
    });
    auto sender_task = scheduler.spawn([&] {
        const auto result = channel->Send(42, cancel_context_pair.first);
        std::clog << "[runtime][场景1] 发送状态="
                  << static_cast<int>(result.status) << '\n';
    });

    const auto receive_deadline = std::chrono::steady_clock::now() + 2s;
    while (received.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < receive_deadline) {
        if (context_pair.first->Done().WaitFor(10ms)) {
            break;
        }
    }
    const bool tasks_joined = receiver_task->wait_for(2s) &&
                              sender_task->wait_for(2s);
    channel->Close();
    const auto context_value = value_context->Value(demo_key).value_or(-1);
    cancel_context_pair.second();
    const bool context_cancelled = cancel_context_pair.first->IsDone() &&
                                   static_cast<bool>(cancel_context_pair.first->Err());

    std::clog << "[runtime][场景2] Select 超时与 Context 取消路径\n";
    auto timeout_channel = MakeChannel<int>(0);
    const auto timeout_select = Select({RecvCase(timeout_channel)},
                                       context_pair.first, 20ms);
    const bool timeout_ok = !timeout_select.selected &&
                            timeout_select.status == ChannelStatus::kTimedOut;
    std::clog << "[runtime][场景2] select status="
              << static_cast<int>(timeout_select.status)
              << "（预期 kTimedOut）\n";

    std::clog << "[runtime][场景3] 错误路径：关闭 Channel 后发送必须失败\n";
    auto closed_channel = MakeChannel<int>(1);
    closed_channel->Close();
    const auto closed_send = closed_channel->Send(9);
    const bool closed_ok = closed_send.Closed();
    std::clog << "[runtime][场景3] send_after_close status="
              << static_cast<int>(closed_send.status) << '\n';

    auto select_channel = MakeChannel<int>(1);
    auto send_view = AsSendOnly(select_channel);
    auto recv_view = AsRecvOnly(select_channel);
    send_view.Send(7);
    const auto select_result = Select({RecvCase(select_channel), DefaultCase()});
    const auto direction_probe = recv_view.TryRecv();
    std::cout << "select_index=" << select_result.index
              << " select_value=" << select_result.Value<int>().value_or(0)
              << " recv_view_status="
              << static_cast<int>(direction_probe.status) << '\n';

    const auto root_error = NewError("root cause");
    const auto error = Wrap(root_error, "demo operation");
    std::cout << "GMP workers=" << scheduler.worker_count()
              << " P=" << scheduler.processor_count()
              << " received=" << received.load()
              << " tasks_joined=" << tasks_joined << '\n';
    std::cout << "context_value=" << context_value
              << " context_cancelled=" << (context_cancelled ? "true" : "false")
              << '\n';
    std::cout << "error=" << ErrorMessage(error) << '\n';
    std::cout << "error_is_root=" << (Is(error, root_error) ? "true" : "false")
              << '\n';

    context_pair.second();
    scheduler.shutdown();
    return tasks_joined && received.load(std::memory_order_acquire) == 42 &&
                   context_value == 7 && context_cancelled &&
                   timeout_ok && closed_ok &&
                   Is(error, root_error) &&
                   select_result.selected && select_result.ok &&
                   select_result.Value<int>().value_or(0) == 7
               ? 0
               : 1;
}
