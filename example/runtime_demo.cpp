#include "go2cpp/channel.hpp"
#include "go2cpp/context.hpp"
#include "go2cpp/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <iostream>

int main() {
    using namespace std::chrono_literals;
    using namespace go2cpp;

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

    scheduler.spawn([&] {
        const auto result = channel->Recv(cancel_context_pair.first);
        if (result.Ok()) {
            received.store(result.ValueOrDefault(), std::memory_order_release);
        }
    });
    scheduler.spawn([&] {
        (void)channel->Send(42, cancel_context_pair.first);
    });

    const auto receive_deadline = std::chrono::steady_clock::now() + 2s;
    while (received.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < receive_deadline) {
        if (context_pair.first->Done().WaitFor(10ms)) {
            break;
        }
    }
    channel->Close();
    const auto context_value = value_context->Value(demo_key).value_or(-1);
    cancel_context_pair.second();
    const bool context_cancelled = cancel_context_pair.first->IsDone() &&
                                   static_cast<bool>(cancel_context_pair.first->Err());

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
              << " received=" << received.load() << '\n';
    std::cout << "context_value=" << context_value
              << " context_cancelled=" << (context_cancelled ? "true" : "false")
              << '\n';
    std::cout << "error=" << ErrorMessage(error) << '\n';
    std::cout << "error_is_root=" << (Is(error, root_error) ? "true" : "false")
              << '\n';

    context_pair.second();
    scheduler.shutdown();
    return received.load(std::memory_order_acquire) == 42 &&
                   context_value == 7 && context_cancelled &&
                   Is(error, root_error) &&
                   select_result.selected && select_result.ok &&
                   select_result.Value<int>().value_or(0) == 7
               ? 0
               : 1;
}
