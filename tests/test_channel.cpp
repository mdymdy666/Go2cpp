#include "go2cpp/channel.hpp"
#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

template <typename T, typename = void>
struct has_send : std::false_type {};

template <typename T>
struct has_send<T, std::void_t<decltype(std::declval<T&>().Send(1))>>
    : std::true_type {};

template <typename T, typename = void>
struct has_recv : std::false_type {};

template <typename T>
struct has_recv<T, std::void_t<decltype(std::declval<T&>().TryRecv())>>
    : std::true_type {};

template <typename T, typename = void>
struct has_close : std::false_type {};

template <typename T>
struct has_close<T, std::void_t<decltype(std::declval<T&>().Close())>>
    : std::true_type {};

static_assert(has_send<go2cpp::SendOnlyChannel<int>>::value);
static_assert(has_close<go2cpp::SendOnlyChannel<int>>::value);
static_assert(!has_recv<go2cpp::SendOnlyChannel<int>>::value);
static_assert(has_recv<go2cpp::RecvOnlyChannel<int>>::value);
static_assert(!has_send<go2cpp::RecvOnlyChannel<int>>::value);
static_assert(!has_close<go2cpp::RecvOnlyChannel<int>>::value);

}  // namespace

void run_channel_tests() {
    go2cpp_tests::announce("channels, close, select and cancellation");
    using namespace go2cpp;
    using namespace std::chrono_literals;

    auto buffered = MakeChannel<int>(3);
    GO2CPP_CHECK(buffered->Capacity() == 3);
    GO2CPP_CHECK(buffered->TrySend(1).Ok());
    GO2CPP_CHECK(buffered->TrySend(2).Ok());
    GO2CPP_CHECK(buffered->TrySend(3).Ok());
    GO2CPP_CHECK(buffered->TrySend(4).status == ChannelStatus::kWouldBlock);
    GO2CPP_CHECK(buffered->Len() == 3);
    GO2CPP_CHECK(buffered->TryRecv().ValueOrDefault() == 1);
    GO2CPP_CHECK(buffered->TryRecv().ValueOrDefault() == 2);
    GO2CPP_CHECK(buffered->TryRecv().ValueOrDefault() == 3);

    auto rendezvous = MakeChannel<int>(0);
    std::atomic<bool> received{false};
    std::thread receiver([&] {
        const auto result = rendezvous->RecvFor(500ms);
        received.store(result.Ok() && result.ValueOrDefault() == 9,
                       std::memory_order_release);
    });
    std::this_thread::sleep_for(2ms);
    GO2CPP_CHECK(rendezvous->SendFor(9, 500ms).Ok());
    receiver.join();
    GO2CPP_CHECK(received.load(std::memory_order_acquire));

    auto close_channel = MakeChannel<int>(1);
    GO2CPP_CHECK(close_channel->Send(7).Ok());
    GO2CPP_CHECK(close_channel->Close().Ok());
    const auto drained = close_channel->Recv();
    GO2CPP_CHECK(drained.Ok() && drained.ValueOrDefault() == 7);
    const auto closed = close_channel->RecvFor(20ms);
    GO2CPP_CHECK(closed.Closed());
    GO2CPP_CHECK(closed.ValueOrDefault() == 0);
    GO2CPP_CHECK(close_channel->Close().status == ChannelStatus::kAlreadyClosed);
    GO2CPP_CHECK(close_channel->Send(8).Closed());

    const auto nil_send = SendChecked<int>({}, 1);
    GO2CPP_CHECK(nil_send.status == ChannelStatus::kNil);
    const auto nil_recv = RecvChecked<int>({});
    GO2CPP_CHECK(nil_recv.status == ChannelStatus::kNil);
    const auto nil_select = Select({RecvCase(ChannelPtr<int>{})});
    GO2CPP_CHECK(nil_select.selected &&
                 nil_select.status == ChannelStatus::kNil);
    const auto empty_select = Select({});
    GO2CPP_CHECK(!empty_select.selected &&
                 empty_select.status == ChannelStatus::kInvalid);
    const auto inert_select = Select({SelectCase{}});
    GO2CPP_CHECK(!inert_select.selected &&
                 inert_select.status == ChannelStatus::kInvalid);

    auto timeout_channel = MakeChannel<int>(0);
    const auto timeout_recv = timeout_channel->RecvFor(5ms);
    GO2CPP_CHECK(timeout_recv.status == ChannelStatus::kTimedOut);
    const auto timeout_send = timeout_channel->SendFor(1, 5ms);
    GO2CPP_CHECK(timeout_send.status == ChannelStatus::kTimedOut);

    auto cancel_channel = MakeChannel<int>(0);
    auto context_pair = WithCancel(Background());
    std::atomic<ChannelStatus> canceled_status{ChannelStatus::kInvalid};
    std::thread blocked([&] {
        canceled_status.store(cancel_channel->Recv(context_pair.first).status,
                              std::memory_order_release);
    });
    std::this_thread::sleep_for(2ms);
    context_pair.second();
    blocked.join();
    GO2CPP_CHECK(canceled_status.load(std::memory_order_acquire) ==
                 ChannelStatus::kCancelled);

    auto selected = MakeChannel<int>(1);
    GO2CPP_CHECK(selected->Send(42).Ok());
    const auto select_result = Select({RecvCase(selected), DefaultCase()});
    GO2CPP_CHECK(select_result.selected && select_result.index == 0);
    GO2CPP_CHECK(select_result.Value<int>().value_or(0) == 42);

    // Direct operations give an already-done context priority, while select
    // treats ready channel cases as competing alternatives.
    auto priority_channel = MakeChannel<int>(1);
    GO2CPP_CHECK(priority_channel->Send(5).Ok());
    auto priority_context = WithCancel(Background());
    priority_context.second();
    GO2CPP_CHECK(priority_channel->Recv(priority_context.first).Cancelled());
    const auto ready_over_cancel =
        Select({RecvCase(priority_channel)}, priority_context.first, 5ms);
    GO2CPP_CHECK(ready_over_cancel.selected && ready_over_cancel.ok);
    GO2CPP_CHECK(ready_over_cancel.Value<int>().value_or(0) == 5);

    auto empty = MakeChannel<int>(0);
    const auto default_result = Select({RecvCase(empty), DefaultCase()});
    GO2CPP_CHECK(default_result.selected && default_result.index == 1);

    auto select_timeout = MakeChannel<int>(0);
    const auto timed_select = Select({RecvCase(select_timeout)}, {}, 5ms);
    GO2CPP_CHECK(timed_select.status == ChannelStatus::kTimedOut);

    // An armed receive must rendezvous with an ordinary sender, and the
    // symmetric send case must rendezvous with an ordinary receiver.
    auto armed_receive_channel = MakeChannel<int>(0);
    std::atomic<bool> receive_select_started{false};
    SelectResult receive_select_result;
    std::thread receive_select_thread([&] {
        receive_select_started.store(true, std::memory_order_release);
        receive_select_result =
            Select({RecvCase(armed_receive_channel)}, {}, 1s);
    });
    while (!receive_select_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(2ms);
    GO2CPP_CHECK(armed_receive_channel->SendFor(73, 1s).Ok());
    receive_select_thread.join();
    GO2CPP_CHECK(receive_select_result.selected &&
                 receive_select_result.Value<int>().value_or(0) == 73);

    auto armed_send_channel = MakeChannel<int>(0);
    std::atomic<bool> send_select_started{false};
    SelectResult send_select_result;
    std::thread send_select_thread([&] {
        send_select_started.store(true, std::memory_order_release);
        send_select_result = Select({SendCase(armed_send_channel, 74)}, {}, 1s);
    });
    while (!send_select_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(2ms);
    const auto ordinary_receive = armed_send_channel->RecvFor(74ms);
    send_select_thread.join();
    GO2CPP_CHECK(ordinary_receive.Ok() && ordinary_receive.ValueOrDefault() == 74);
    GO2CPP_CHECK(send_select_result.selected && send_select_result.ok);

    // A full buffered channel keeps a select sender queued. Draining the
    // existing value must refill the slot from that sender and preserve FIFO.
    auto full_buffer_select_channel = MakeChannel<int>(1);
    GO2CPP_CHECK(full_buffer_select_channel->Send(81).Ok());
    std::atomic<bool> full_send_started{false};
    SelectResult full_send_result;
    std::thread full_send_thread([&] {
        full_send_started.store(true, std::memory_order_release);
        full_send_result =
            Select({SendCase(full_buffer_select_channel, 82)}, {}, 1s);
    });
    while (!full_send_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(2ms);
    const auto full_buffer_first = full_buffer_select_channel->RecvFor(1s);
    full_send_thread.join();
    GO2CPP_CHECK(full_buffer_first.Ok() &&
                 full_buffer_first.ValueOrDefault() == 81);
    GO2CPP_CHECK(full_send_result.selected && full_send_result.ok);
    const auto full_buffer_refill = full_buffer_select_channel->TryRecv();
    GO2CPP_CHECK(full_buffer_refill.Ok() &&
                 full_buffer_refill.ValueOrDefault() == 82);

    // Two independent selects on the same unbuffered channel must pair rather
    // than polling forever. Each side has a bounded timeout as a watchdog.
    auto select_pair_channel = MakeChannel<int>(0);
    std::atomic<int> pair_ready{0};
    SelectResult pair_receive_result;
    SelectResult pair_send_result;
    std::thread pair_receiver([&] {
        pair_ready.fetch_add(1, std::memory_order_acq_rel);
        pair_receive_result = Select({RecvCase(select_pair_channel)}, {}, 1s);
    });
    std::thread pair_sender([&] {
        pair_ready.fetch_add(1, std::memory_order_acq_rel);
        pair_send_result = Select({SendCase(select_pair_channel, 75)}, {}, 1s);
    });
    while (pair_ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    pair_receiver.join();
    pair_sender.join();
    GO2CPP_CHECK(pair_receive_result.selected && pair_receive_result.ok &&
                 pair_receive_result.Value<int>().value_or(0) == 75);
    GO2CPP_CHECK(pair_send_result.selected && pair_send_result.ok);

    // A single select containing both directions of the same channel must not
    // match its own pending nodes. It should time out and clean them up.
    auto self_select_channel = MakeChannel<int>(0);
    const auto self_select_result =
        Select({RecvCase(self_select_channel), SendCase(self_select_channel, 76)},
               {}, 10ms);
    GO2CPP_CHECK(self_select_result.status == ChannelStatus::kTimedOut);
    GO2CPP_CHECK(self_select_channel->TrySend(77).status ==
                 ChannelStatus::kWouldBlock);

    // Closing an armed case wakes it, and cancellation removes its waiter so
    // a later ordinary operation observes the channel normally.
    auto close_select_channel = MakeChannel<int>(0);
    SelectResult close_select_result;
    std::thread close_select_thread([&] {
        close_select_result = Select({RecvCase(close_select_channel)}, {}, 1s);
    });
    std::this_thread::sleep_for(2ms);
    GO2CPP_CHECK(close_select_channel->Close().Ok());
    close_select_thread.join();
    GO2CPP_CHECK(close_select_result.selected &&
                 close_select_result.status == ChannelStatus::kClosed);

    auto cancelled_select_channel = MakeChannel<int>(1);
    auto select_context_pair = WithCancel(Background());
    SelectResult cancelled_select_result;
    std::thread cancelled_select_thread([&] {
        cancelled_select_result =
            Select({RecvCase(cancelled_select_channel)}, select_context_pair.first,
                   1s);
    });
    std::this_thread::sleep_for(2ms);
    select_context_pair.second();
    cancelled_select_thread.join();
    GO2CPP_CHECK(cancelled_select_result.status == ChannelStatus::kCancelled);
    GO2CPP_CHECK(cancelled_select_channel->TrySend(78).Ok());

    auto directional_channel = MakeChannel<int>(1);
    auto send_only = AsSendOnly(directional_channel);
    auto recv_only = AsRecvOnly(directional_channel);
    GO2CPP_CHECK(send_only.Send(79).Ok());
    const auto directional_receive = recv_only.TryRecv();
    GO2CPP_CHECK(directional_receive.Ok() &&
                 directional_receive.ValueOrDefault() == 79);
    auto directional_select_channel = MakeChannel<int>(1);
    auto directional_select_sender = AsSendOnly(directional_select_channel);
    auto directional_select_receiver = AsRecvOnly(directional_select_channel);
    const auto directional_select_send =
        Select({SendCase(directional_select_sender, 80)}, {}, 20ms);
    GO2CPP_CHECK(directional_select_send.selected &&
                 directional_select_send.ok);
    const auto directional_select_recv =
        Select({RecvCase(directional_select_receiver)}, {}, 20ms);
    GO2CPP_CHECK(directional_select_recv.selected &&
                 directional_select_recv.ok &&
                 directional_select_recv.Value<int>().value_or(0) == 80);

    auto stress = MakeChannel<int>(8);
    constexpr int producer_count = 4;
    constexpr int values_per_producer = 50;
    std::atomic<int> consumed{0};
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    for (int p = 0; p < producer_count; ++p) {
        producers.emplace_back([stress, p] {
            for (int i = 0; i < values_per_producer; ++i) {
                const auto result = stress->SendFor(
                    p * values_per_producer + i, 2s);
                GO2CPP_CHECK(result.Ok());
            }
        });
    }
    for (int c = 0; c < 3; ++c) {
        consumers.emplace_back([stress, &consumed] {
            for (;;) {
                const auto result = stress->RecvFor(2s);
                if (result.Closed()) {
                    return;
                }
                if (result.Ok()) {
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    GO2CPP_CHECK(false);
                    return;
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    stress->Close();
    for (auto& consumer : consumers) {
        consumer.join();
    }
    GO2CPP_CHECK(consumed.load(std::memory_order_relaxed) ==
                 producer_count * values_per_producer);

    // A channel wait in a managed G must release the only M. These checks use
    // one processor/worker deliberately: a native condition_variable wait
    // would deadlock the sender forever.
    SchedulerConfig managed_config;
    managed_config.processor_count = 1;
    managed_config.max_workers = 1;
    managed_config.idle_wait = 1ms;
    Scheduler managed(managed_config);
    managed.start();

    auto managed_rendezvous = MakeChannel<int>(0);
    std::atomic<bool> managed_recv_started{false};
    std::atomic<bool> managed_recv_done{false};
    std::atomic<int> managed_value{0};
    auto managed_receiver = managed.spawn([&] {
        managed_recv_started.store(true, std::memory_order_release);
        const auto result = managed_rendezvous->Recv();
        if (result.Ok()) {
            managed_value.store(result.ValueOrDefault(), std::memory_order_release);
        }
        managed_recv_done.store(result.Ok(), std::memory_order_release);
    });
    const auto managed_start_deadline = std::chrono::steady_clock::now() + 1s;
    while (!managed_recv_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < managed_start_deadline) {
        std::this_thread::yield();
    }
    GO2CPP_CHECK(managed_recv_started.load(std::memory_order_acquire));
    while (managed_receiver->state() != GState::kWaiting &&
           std::chrono::steady_clock::now() < managed_start_deadline) {
        std::this_thread::yield();
    }
    auto managed_sender = managed.spawn([&] {
        GO2CPP_CHECK(managed_rendezvous->Send(91).Ok());
    });
    GO2CPP_CHECK(managed_receiver->wait_for(1s));
    GO2CPP_CHECK(managed_sender->wait_for(1s));
    GO2CPP_CHECK(managed_recv_done.load(std::memory_order_acquire));
    GO2CPP_CHECK(managed_value.load(std::memory_order_acquire) == 91);

    auto managed_buffer = MakeChannel<int>(2);
    std::atomic<int> managed_sum{0};
    auto managed_buffer_sender = managed.spawn([&] {
        for (int value = 1; value <= 4; ++value) {
            GO2CPP_CHECK(managed_buffer->SendFor(value, 1s).Ok());
        }
    });
    auto managed_buffer_receiver = managed.spawn([&] {
        for (int i = 0; i != 4; ++i) {
            const auto result = managed_buffer->RecvFor(1s);
            GO2CPP_CHECK(result.Ok());
            managed_sum.fetch_add(result.ValueOrDefault(),
                                  std::memory_order_relaxed);
        }
    });
    GO2CPP_CHECK(managed_buffer_sender->wait_for(2s));
    GO2CPP_CHECK(managed_buffer_receiver->wait_for(2s));
    GO2CPP_CHECK(managed_sum.load(std::memory_order_relaxed) == 10);

    auto managed_timeout_channel = MakeChannel<int>(0);
    std::atomic<ChannelStatus> managed_timeout{ChannelStatus::kInvalid};
    auto timeout_task = managed.spawn([&] {
        managed_timeout.store(managed_timeout_channel->RecvFor(25ms).status,
                              std::memory_order_release);
    });
    GO2CPP_CHECK(timeout_task->wait_for(1s));
    GO2CPP_CHECK(managed_timeout.load(std::memory_order_acquire) ==
                 ChannelStatus::kTimedOut);

    auto managed_cancel_channel = MakeChannel<int>(0);
    auto managed_cancel_context = WithCancel(Background());
    std::atomic<ChannelStatus> managed_cancel_status{ChannelStatus::kInvalid};
    auto cancel_task = managed.spawn([&] {
        managed_cancel_status.store(
            managed_cancel_channel->Recv(managed_cancel_context.first).status,
            std::memory_order_release);
    });
    while (cancel_task->state() != GState::kWaiting &&
           std::chrono::steady_clock::now() < managed_start_deadline) {
        std::this_thread::yield();
    }
    managed_cancel_context.second();
    GO2CPP_CHECK(cancel_task->wait_for(1s));
    GO2CPP_CHECK(managed_cancel_status.load(std::memory_order_acquire) ==
                 ChannelStatus::kCancelled);

    auto managed_select_channel = MakeChannel<int>(0);
    std::atomic<bool> managed_select_ok{false};
    auto managed_select_receiver = managed.spawn([&] {
        const auto result = Select({RecvCase(managed_select_channel)}, {}, 1s);
        managed_select_ok.store(result.selected && result.ok &&
                                     result.Value<int>().value_or(0) == 93,
                                 std::memory_order_release);
    });
    while (managed_select_receiver->state() != GState::kWaiting &&
           std::chrono::steady_clock::now() < managed_start_deadline) {
        std::this_thread::yield();
    }
    auto managed_select_sender = managed.spawn([&] {
        GO2CPP_CHECK(managed_select_channel->Send(93).Ok());
    });
    GO2CPP_CHECK(managed_select_receiver->wait_for(1s));
    GO2CPP_CHECK(managed_select_sender->wait_for(1s));
    GO2CPP_CHECK(managed_select_ok.load(std::memory_order_acquire));

    // Shutdown must wake a channel-blocked G and let its stack unwind before
    // the scheduler returns; this guards against suspended-capture leaks.
    auto shutdown_channel = MakeChannel<int>(0);
    std::atomic<bool> shutdown_returned{false};
    auto shutdown_task = managed.spawn([&] {
        const auto result = shutdown_channel->Recv();
        shutdown_returned.store(result.Cancelled(), std::memory_order_release);
    });
    while (shutdown_task->state() != GState::kWaiting &&
           std::chrono::steady_clock::now() < managed_start_deadline) {
        std::this_thread::yield();
    }
    managed.shutdown();
    GO2CPP_CHECK(shutdown_task->state() == GState::kCancelled ||
                 shutdown_task->state() == GState::kDead);
    GO2CPP_CHECK(shutdown_returned.load(std::memory_order_acquire));
}
