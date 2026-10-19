#include "go2cpp/channel.hpp"
#include "go2cpp/core/timer.hpp"
#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <random>
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
static_assert(noexcept(std::declval<go2cpp::Channel<int>&>().Close()));
static_assert(std::is_nothrow_destructible<go2cpp::Channel<int>>::value);

}  // namespace

namespace {

// 定义在本文件末尾（同一个匿名命名空间）。保持"测试只改一个文件"的约定：
// DoneCase / AfterCase 的场景与既有 channel 用例共享同一个模块与同一个 ctest 项。
void RunSelectHelperCaseTests();

}  // namespace

/**
 * @brief 验证无缓冲/有缓冲 Channel、关闭、取消和 select 行为。
 * @return 无；所有结果通过 GO2CPP_CHECK 记录。
 */
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
    GO2CPP_JOIN_WITH_WATCHDOG(receiver, 3s);
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
    // 注册等待节点失败时必须立即返回错误，不能在没有任何已注册事件的
    // 状态下永久等待。
    SelectCase failing_arm;
    failing_arm.probe = [] { return SelectProbe{}; };
    failing_arm.arm_checked =
        [](const std::shared_ptr<detail::SelectWaitState>&, std::size_t) {
            return false;
        };
    const auto failed_select = Select({failing_arm});
    GO2CPP_CHECK(!failed_select.selected &&
                 failed_select.status == ChannelStatus::kInvalid);

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
    GO2CPP_JOIN_WITH_WATCHDOG(blocked, 3s);
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
    GO2CPP_REQUIRE_EVENTUALLY(
        receive_select_started.load(std::memory_order_acquire), 3s);
    std::this_thread::sleep_for(2ms);
    GO2CPP_CHECK(armed_receive_channel->SendFor(73, 1s).Ok());
    GO2CPP_JOIN_WITH_WATCHDOG(receive_select_thread, 3s);
    GO2CPP_CHECK(receive_select_result.selected &&
                 receive_select_result.Value<int>().value_or(0) == 73);

    auto armed_send_channel = MakeChannel<int>(0);
    std::atomic<bool> send_select_started{false};
    SelectResult send_select_result;
    std::thread send_select_thread([&] {
        send_select_started.store(true, std::memory_order_release);
        send_select_result = Select({SendCase(armed_send_channel, 74)}, {}, 1s);
    });
    GO2CPP_REQUIRE_EVENTUALLY(
        send_select_started.load(std::memory_order_acquire), 3s);
    std::this_thread::sleep_for(2ms);
    const auto ordinary_receive = armed_send_channel->RecvFor(74ms);
    GO2CPP_JOIN_WITH_WATCHDOG(send_select_thread, 3s);
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
    GO2CPP_REQUIRE_EVENTUALLY(
        full_send_started.load(std::memory_order_acquire), 3s);
    std::this_thread::sleep_for(2ms);
    const auto full_buffer_first = full_buffer_select_channel->RecvFor(1s);
    GO2CPP_JOIN_WITH_WATCHDOG(full_send_thread, 3s);
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
    GO2CPP_REQUIRE_EVENTUALLY(
        pair_ready.load(std::memory_order_acquire) == 2, 3s);
    GO2CPP_JOIN_WITH_WATCHDOG(pair_receiver, 3s);
    GO2CPP_JOIN_WITH_WATCHDOG(pair_sender, 3s);
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
    GO2CPP_JOIN_WITH_WATCHDOG(close_select_thread, 3s);
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
    GO2CPP_JOIN_WITH_WATCHDOG(cancelled_select_thread, 3s);
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
                if (!result.Ok()) {
                    go2cpp_tests::watchdog_abort("channel producer", __FILE__,
                                                 __LINE__);
                }
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
                    go2cpp_tests::watchdog_abort("channel consumer", __FILE__,
                                                 __LINE__);
                }
            }
        });
    }
    for (auto& producer : producers) {
        GO2CPP_JOIN_WITH_WATCHDOG(producer, 5s);
    }
    stress->Close();
    for (auto& consumer : consumers) {
        GO2CPP_JOIN_WITH_WATCHDOG(consumer, 5s);
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
        go2cpp_tests::yield_for_watchdog();
    }
    GO2CPP_CHECK(managed_recv_started.load(std::memory_order_acquire));
    while (managed_receiver->state() != GState::kWaiting &&
           std::chrono::steady_clock::now() < managed_start_deadline) {
        go2cpp_tests::yield_for_watchdog();
    }
    auto managed_sender = managed.spawn([&] {
        GO2CPP_CHECK(managed_rendezvous->Send(91).Ok());
    });
    GO2CPP_REQUIRE(managed_receiver->wait_for(1s));
    GO2CPP_REQUIRE(managed_sender->wait_for(1s));
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
    GO2CPP_REQUIRE(managed_buffer_sender->wait_for(2s));
    GO2CPP_REQUIRE(managed_buffer_receiver->wait_for(2s));
    GO2CPP_CHECK(managed_sum.load(std::memory_order_relaxed) == 10);

    auto managed_timeout_channel = MakeChannel<int>(0);
    std::atomic<ChannelStatus> managed_timeout{ChannelStatus::kInvalid};
    auto timeout_task = managed.spawn([&] {
        managed_timeout.store(managed_timeout_channel->RecvFor(25ms).status,
                              std::memory_order_release);
    });
    GO2CPP_REQUIRE(timeout_task->wait_for(1s));
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
        go2cpp_tests::yield_for_watchdog();
    }
    managed_cancel_context.second();
    GO2CPP_REQUIRE(cancel_task->wait_for(1s));
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
        go2cpp_tests::yield_for_watchdog();
    }
    auto managed_select_sender = managed.spawn([&] {
        GO2CPP_CHECK(managed_select_channel->Send(93).Ok());
    });
    GO2CPP_REQUIRE(managed_select_receiver->wait_for(1s));
    GO2CPP_REQUIRE(managed_select_sender->wait_for(1s));
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
        go2cpp_tests::yield_for_watchdog();
    }
    managed.shutdown();
    GO2CPP_CHECK(shutdown_task->state() == GState::kCancelled ||
                 shutdown_task->state() == GState::kDead);
    GO2CPP_CHECK(shutdown_returned.load(std::memory_order_acquire));

    // 环形缓冲专项：索引回绕、容量边界与随机对拍（对齐 Go hchan 的
    // buf/sendx/recvx/qcount 模型）。替换 deque 后这些是最容易出错的地方。
    {
        // 场景 1：容量 2 反复回绕 200 轮，验证 FIFO 与索引回绕。
        auto ring = MakeChannel<int>(2);
        bool ring_ok = true;
        for (int round = 0; round < 200; ++round) {
            ring_ok = ring_ok && ring->Send(round * 2).Ok();
            ring_ok = ring_ok && ring->Send(round * 2 + 1).Ok();
            ring_ok = ring_ok && ring->Recv().ValueOrDefault() == round * 2;
            ring_ok = ring_ok && ring->Recv().ValueOrDefault() == round * 2 + 1;
        }
        GO2CPP_CHECK(ring_ok);
        GO2CPP_CHECK(ring->Len() == 0);

        // 场景 2：容量 1 的满/空交替（最容易暴露 head 前进与计数错误）。
        auto single = MakeChannel<int>(1);
        bool single_ok = true;
        for (int i = 0; i < 1000; ++i) {
            single_ok = single_ok && single->TrySend(i).Ok();
            single_ok = single_ok &&
                        single->TrySend(-1).status == ChannelStatus::kWouldBlock;
            single_ok = single_ok && single->TryRecv().ValueOrDefault() == i;
            single_ok = single_ok &&
                        single->TryRecv().status == ChannelStatus::kWouldBlock;
        }
        GO2CPP_CHECK(single_ok);

        // 场景 3：容量取非 2 的幂（7），强制回绕不整齐；2 万个值跨线程传递
        // 并校验总和，覆盖大数据量与生产者/消费者并行两种压力。
        constexpr int kRingTotal = 20000;
        auto bulk = MakeChannel<int>(7);
        std::atomic<long long> bulk_sum{0};
        std::thread bulk_producer([&] {
            for (int i = 0; i < kRingTotal; ++i) {
                bulk->Send(i);
            }
        });
        std::thread bulk_consumer([&] {
            long long local = 0;
            for (int i = 0; i < kRingTotal; ++i) {
                local += bulk->Recv().ValueOrDefault();
            }
            bulk_sum.store(local, std::memory_order_relaxed);
        });
        GO2CPP_JOIN_WITH_WATCHDOG(bulk_producer, 10s);
        GO2CPP_JOIN_WITH_WATCHDOG(bulk_consumer, 10s);
        GO2CPP_CHECK(bulk_sum.load(std::memory_order_relaxed) ==
                     static_cast<long long>(kRingTotal) * (kRingTotal - 1) / 2);
        GO2CPP_CHECK(bulk->Len() == 0);

        // 场景 4：随机操作与 std::deque 参考模型对拍 —— 通道的输出序列必须
        // 与参考队列完全一致（2 万步，容量 5）。
        std::mt19937 rng(20261015);
        auto model_channel = MakeChannel<int>(5);
        std::deque<int> model;
        int next_value = 0;
        bool model_ok = true;
        for (int step = 0; step < 20000; ++step) {
            const bool do_send = (rng() % 2U) == 0U;
            if (do_send && model.size() < 5) {
                const int value = next_value++;
                model_ok = model_ok && model_channel->TrySend(value).Ok();
                model.push_back(value);
            } else if (!model.empty()) {
                const int expected = model.front();
                model.pop_front();
                model_ok = model_ok &&
                           model_channel->TryRecv().ValueOrDefault() == expected;
            }
        }
        GO2CPP_CHECK(model_ok);
        GO2CPP_CHECK(model_channel->Len() == model.size());
        while (!model.empty()) {
            GO2CPP_CHECK(model_channel->TryRecv().ValueOrDefault() == model.front());
            model.pop_front();
        }
        GO2CPP_CHECK(model_channel->Len() == 0);
    }

    // DoneCase（等价 `case <-ctx.Done()`）与 AfterCase（等价
    // `case <-time.After(d)`）专项：场景、泄漏与大数据量。放在最后执行，
    // 避免统计 TimerService 待处理数时受到前面用例残留定时器的干扰。
    RunSelectHelperCaseTests();
}

namespace {

using SteadyClock = std::chrono::steady_clock;

// 把 steady_clock 差值换算成毫秒（浮点）。统一计时口径，便于断言
// "耗时约等于 deadline" 并把大数据量用例的真实耗时打进 ctest 输出。
double ElapsedMs(SteadyClock::time_point start) {
    return std::chrono::duration<double, std::milli>(SteadyClock::now() - start)
        .count();
}

void ReportPerf(const char* label, double elapsed_ms) {
    std::cout << "[perf] " << label << ": " << elapsed_ms << " ms" << '\n';
}

// T1：DoneCase 单独使用，ctx 先取消 —— 必须立刻返回 kCancelled。
void SelectHelperT1DoneOnly() {
    using namespace go2cpp;
    auto pair = WithCancel(Background());
    pair.second();
    const auto start = SteadyClock::now();
    const auto result = Select({DoneCase(pair.first)});
    const double elapsed = ElapsedMs(start);
    GO2CPP_CHECK(result.selected);
    GO2CPP_CHECK(result.index == 0);
    GO2CPP_CHECK(result.status == ChannelStatus::kCancelled);
    GO2CPP_CHECK(!result.ok);
    GO2CPP_CHECK(result.error != nullptr);
    GO2CPP_CHECK(elapsed < 50.0);
    ReportPerf("T1 DoneCase(已取消 ctx) 返回耗时", elapsed);
}

// T2：DoneCase + RecvCase，select 已挂起后先 cancel、再发送 —— 选中 Done，
// 且这次发送不能被 select 吞掉。
void SelectHelperT2DoneWinsOverRecv() {
    using namespace go2cpp;
    using namespace std::chrono_literals;
    auto channel = MakeChannel<int>(1);
    auto pair = WithCancel(Background());
    std::atomic<bool> started{false};
    SelectResult result;
    std::thread worker([&] {
        started.store(true, std::memory_order_release);
        result = Select({RecvCase(channel), DoneCase(pair.first)}, {}, 2s);
    });
    GO2CPP_REQUIRE_EVENTUALLY(started.load(std::memory_order_acquire), 3s);
    std::this_thread::sleep_for(5ms);
    pair.second();
    GO2CPP_JOIN_WITH_WATCHDOG(worker, 3s);
    GO2CPP_CHECK(result.selected);
    GO2CPP_CHECK(result.index == 1);
    GO2CPP_CHECK(result.status == ChannelStatus::kCancelled);
    GO2CPP_CHECK(channel->TrySend(11).Ok());
    GO2CPP_CHECK(channel->TryRecv().ValueOrDefault() == 11);
}

// T3：DoneCase + RecvCase，先发送（select 已挂起）后 cancel —— 选中 Recv，
// 之后的取消不得改写已经完成的选择。
void SelectHelperT3RecvWinsOverDone() {
    using namespace go2cpp;
    using namespace std::chrono_literals;
    auto channel = MakeChannel<int>(0);
    auto pair = WithCancel(Background());
    std::atomic<bool> started{false};
    SelectResult result;
    std::thread worker([&] {
        started.store(true, std::memory_order_release);
        result = Select({RecvCase(channel), DoneCase(pair.first)}, {}, 2s);
    });
    GO2CPP_REQUIRE_EVENTUALLY(started.load(std::memory_order_acquire), 3s);
    std::this_thread::sleep_for(5ms);
    GO2CPP_CHECK(channel->SendFor(21, 2s).Ok());
    GO2CPP_JOIN_WITH_WATCHDOG(worker, 3s);
    pair.second();
    GO2CPP_CHECK(result.selected);
    GO2CPP_CHECK(result.index == 0);
    GO2CPP_CHECK(result.ok);
    GO2CPP_CHECK(result.Value<int>().value_or(0) == 21);
}

// T4：AfterCase 单独使用 —— index=0，耗时约等于 deadline。
void SelectHelperT4AfterCaseAlone() {
    using namespace go2cpp;
    using namespace std::chrono_literals;
    const auto start = SteadyClock::now();
    const auto result = Select({AfterCase(30ms)});
    const double elapsed = ElapsedMs(start);
    GO2CPP_CHECK(result.selected);
    GO2CPP_CHECK(result.index == 0);
    GO2CPP_CHECK(result.ok);
    GO2CPP_CHECK(result.status == ChannelStatus::kReady);
    GO2CPP_CHECK(elapsed >= 20.0);
    GO2CPP_CHECK(elapsed < 2000.0);
    ReportPerf("T4 AfterCase(30ms) 实际等待", elapsed);
}

// T5：AfterCase + RecvCase，timer 先到 —— 选中 timer（index=0）。
void SelectHelperT5TimerWinsOverRecv() {
    using namespace go2cpp;
    using namespace std::chrono_literals;
    auto channel = MakeChannel<int>(0);
    const auto start = SteadyClock::now();
    const auto result = Select({AfterCase(40ms), RecvCase(channel)});
    const double elapsed = ElapsedMs(start);
    GO2CPP_CHECK(result.selected);
    GO2CPP_CHECK(result.index == 0);
    GO2CPP_CHECK(result.status == ChannelStatus::kReady);
    GO2CPP_CHECK(elapsed >= 30.0);
    GO2CPP_CHECK(elapsed < 2000.0);
    ReportPerf("T5 AfterCase(40ms) vs RecvCase，timer 先到", elapsed);
}

// T6：AfterCase + RecvCase，数据先到 —— 选中 Recv，且 timer 被 disarm 取消
// （用 TimerService 的待处理数验证，不依赖内部字段）。
void SelectHelperT6RecvWinsTimerDisarmed() {
    using namespace go2cpp;
    using namespace std::chrono_literals;
    auto& timers = core::TimerService::Default();
    const auto baseline = timers.PendingCount();
    auto channel = MakeChannel<int>(0);
    // case 对象故意保持存活到函数结束：这样 disarm 必须真正撤销定时器，而不能
    // 依赖 case 析构时的副作用（真实代码里 case 表通常是复用、长期存活的）。
    const SelectCase after = AfterCase(10s);
    const SelectCase recv = RecvCase(channel);
    std::atomic<bool> started{false};
    SelectResult result;
    std::thread worker([&] {
        started.store(true, std::memory_order_release);
        result = Select({after, recv}, {}, 2s);
    });
    GO2CPP_REQUIRE_EVENTUALLY(started.load(std::memory_order_acquire), 3s);
    // 已 arm：10s 定时器必须真的排进了默认 TimerService。用轮询而不是固定
    // sleep，避免慢机器/插桩环境下"还没 arm 就断言"的假失败。
    GO2CPP_REQUIRE_EVENTUALLY(timers.PendingCount() >= baseline + 1, 2s);
    GO2CPP_CHECK(channel->SendFor(31, 2s).Ok());
    GO2CPP_JOIN_WITH_WATCHDOG(worker, 3s);
    GO2CPP_CHECK(result.selected);
    GO2CPP_CHECK(result.index == 1);
    GO2CPP_CHECK(result.ok);
    GO2CPP_CHECK(result.Value<int>().value_or(0) == 31);
    // 数据先到时定时器必须被 disarm 取消，否则每次 select 泄漏一个 timer。
    GO2CPP_CHECK(timers.PendingCount() <= baseline);
}

// T7：RecvCase + DoneCase + AfterCase 三合一，分别验证三种"先到"。
void SelectHelperT7ThreeWay() {
    using namespace go2cpp;
    using namespace std::chrono_literals;
    {
        auto channel = MakeChannel<int>(1);
        GO2CPP_CHECK(channel->Send(41).Ok());
        auto live = WithCancel(Background());
        const auto result =
            Select({RecvCase(channel), DoneCase(live.first), AfterCase(10s)});
        GO2CPP_CHECK(result.selected);
        GO2CPP_CHECK(result.index == 0);
        GO2CPP_CHECK(result.ok);
        GO2CPP_CHECK(result.Value<int>().value_or(0) == 41);
    }
    {
        auto channel = MakeChannel<int>(0);
        auto pair = WithCancel(Background());
        pair.second();
        const auto result =
            Select({RecvCase(channel), DoneCase(pair.first), AfterCase(10s)});
        GO2CPP_CHECK(result.selected);
        GO2CPP_CHECK(result.index == 1);
        GO2CPP_CHECK(result.status == ChannelStatus::kCancelled);
        GO2CPP_CHECK(!result.ok);
    }
    {
        auto channel = MakeChannel<int>(0);
        auto live = WithCancel(Background());
        const auto start = SteadyClock::now();
        const auto result =
            Select({RecvCase(channel), DoneCase(live.first), AfterCase(25ms)});
        const double elapsed = ElapsedMs(start);
        GO2CPP_CHECK(result.selected);
        GO2CPP_CHECK(result.index == 2);
        GO2CPP_CHECK(result.status == ChannelStatus::kReady);
        GO2CPP_CHECK(elapsed >= 15.0);
        GO2CPP_CHECK(elapsed < 2000.0);
        ReportPerf("T7(c) 三合一 timer 先到", elapsed);
    }
}

// T8：1000 次 Select(DoneCase+RecvCase+DefaultCase)，不取消也不发数据 ——
// 全部走 default，且不注册任何定时器（PendingCount 不增长）。
void SelectHelperT8DefaultNotHijacked() {
    using namespace go2cpp;
    auto& timers = core::TimerService::Default();
    const auto baseline = timers.PendingCount();
    auto idle = MakeChannel<int>(0);
    auto live = WithCancel(Background());
    const SelectCase done = DoneCase(live.first);
    const SelectCase recv = RecvCase(idle);
    const SelectCase fallback = DefaultCase();
    std::size_t defaults = 0;
    const auto start = SteadyClock::now();
    for (int i = 0; i < 1000; ++i) {
        const auto result = Select({recv, done, fallback});
        if (result.selected && result.index == 2) {
            ++defaults;
        }
    }
    const double elapsed = ElapsedMs(start);
    GO2CPP_CHECK(defaults == 1000);
    GO2CPP_CHECK(timers.PendingCount() == baseline);
    ReportPerf("T8 1000x Select(Done+Recv+Default) 全走 default", elapsed);
}

// T9：DoneCase 连续 10000 次。(a) 同一个已取消 ctx 立刻命中；
// (b) 活跃 ctx 上 10000 次 arm/disarm 后取消，保留状态不得被回调改写 ——
// 直接证明 disarm 真的 RemoveCallback 了。
void SelectHelperT9DoneCaseRepeated() {
    using namespace go2cpp;
    auto pair = WithCancel(Background());
    pair.second();
    const SelectCase done = DoneCase(pair.first);
    std::size_t cancelled = 0;
    const auto start = SteadyClock::now();
    for (int i = 0; i < 10000; ++i) {
        const auto result = Select({done});
        if (result.selected && result.index == 0 &&
            result.status == ChannelStatus::kCancelled) {
            ++cancelled;
        }
    }
    const double elapsed = ElapsedMs(start);
    GO2CPP_CHECK(cancelled == 10000);
    ReportPerf("T9(a) 10000x Select(已取消 DoneCase)", elapsed);

    auto live = WithCancel(Background());
    const SelectCase live_done = DoneCase(live.first);
    std::vector<std::shared_ptr<detail::SelectWaitState>> kept;
    kept.reserve(64);
    const auto arm_start = SteadyClock::now();
    for (int i = 0; i < 10000; ++i) {
        auto state = std::make_shared<detail::SelectWaitState>();
        live_done.arm(state, 0);
        live_done.disarm(state);
        if (i >= 10000 - 64) {
            kept.push_back(std::move(state));
        }
    }
    const double arm_elapsed = ElapsedMs(arm_start);
    live.second();
    std::size_t fired = 0;
    for (const auto& state : kept) {
        if (state->IsSelected()) {
            ++fired;
        }
    }
    GO2CPP_CHECK(fired == 0);
    ReportPerf("T9(b) 10000x DoneCase arm/disarm", arm_elapsed);
}

// T10：AfterCase 连续 10000 次且每次立即 disarm —— 每次都要真的排程再取消，
// TimerService 待处理数必须回到基线（无 timer 泄漏）。
void SelectHelperT10AfterCaseRepeated() {
    using namespace go2cpp;
    auto& timers = core::TimerService::Default();
    const auto baseline = timers.PendingCount();
    const SelectCase after = AfterCase(SteadyClock::now() + std::chrono::seconds(30));
    bool armed_visible = false;
    bool disarmed_visible = false;
    const auto start = SteadyClock::now();
    for (int i = 0; i < 10000; ++i) {
        auto state = std::make_shared<detail::SelectWaitState>();
        after.arm(state, 0);
        if (i == 0) {
            armed_visible = timers.PendingCount() >= baseline + 1;
        }
        after.disarm(state);
        if (i == 0) {
            disarmed_visible = timers.PendingCount() <= baseline;
        }
    }
    const double elapsed = ElapsedMs(start);
    GO2CPP_CHECK(armed_visible);
    GO2CPP_CHECK(disarmed_visible);
    GO2CPP_CHECK(timers.PendingCount() <= baseline);
    ReportPerf("T10 10000x AfterCase arm+立即 disarm", elapsed);
}

// T11：DoneCase 与 DefaultCase 同在，ctx 未取消 —— 返回 default。
void SelectHelperT11DoneWithDefault() {
    using namespace go2cpp;
    auto live = WithCancel(Background());
    const auto result = Select({DoneCase(live.first), DefaultCase()});
    GO2CPP_CHECK(result.selected);
    GO2CPP_CHECK(result.index == 1);
    GO2CPP_CHECK(result.status == ChannelStatus::kReady);
    GO2CPP_CHECK(!live.first->IsDone());
}

// B1：100 万次"先 send 再 select(Recv+Default)"，全部取到值并打印耗时。
void SelectHelperB1MillionSelect() {
    using namespace go2cpp;
    constexpr int kTotal = 1000000;
    auto channel = MakeChannel<int>(1);
    std::size_t received = 0;
    const auto start = SteadyClock::now();
    for (int i = 0; i < kTotal; ++i) {
        if (!channel->TrySend(i).Ok()) {
            break;
        }
        const auto result = Select({RecvCase(channel), DefaultCase()});
        if (result.selected && result.index == 0 &&
            result.Value<int>().value_or(-1) == i) {
            ++received;
        }
    }
    const double elapsed = ElapsedMs(start);
    GO2CPP_CHECK(received == static_cast<std::size_t>(kTotal));
    GO2CPP_CHECK(channel->Len() == 0);
    ReportPerf("B1 100 万次 Send+Select(Recv+Default)", elapsed);
}

// B2：1000 个 RecvCase 放进一次 Select，只有第 1000 个有数据。
void SelectHelperB2ThousandCases() {
    using namespace go2cpp;
    constexpr int kCases = 1000;
    std::vector<ChannelPtr<int>> channels;
    channels.reserve(kCases);
    std::vector<SelectCase> cases;
    cases.reserve(kCases);
    for (int i = 0; i < kCases; ++i) {
        channels.push_back(MakeChannel<int>(1));
        cases.push_back(RecvCase(channels.back()));
    }
    GO2CPP_CHECK(channels.back()->TrySend(777).Ok());
    const auto start = SteadyClock::now();
    const auto result = Select(cases);
    const double elapsed = ElapsedMs(start);
    GO2CPP_CHECK(result.selected);
    GO2CPP_CHECK(result.index == static_cast<std::size_t>(kCases - 1));
    GO2CPP_CHECK(result.Value<int>().value_or(0) == 777);
    ReportPerf("B2 1000 个 RecvCase（第 1000 个有数据）", elapsed);
}

// B3：10 万个 AfterCase 逐个创建并立即 disarm —— TimerService 待处理数不增长。
void SelectHelperB3HundredThousandTimers() {
    using namespace go2cpp;
    auto& timers = core::TimerService::Default();
    const auto baseline = timers.PendingCount();
    constexpr int kTotal = 100000;
    const auto start = SteadyClock::now();
    for (int i = 0; i < kTotal; ++i) {
        auto state = std::make_shared<detail::SelectWaitState>();
        const SelectCase after =
            AfterCase(SteadyClock::now() + std::chrono::seconds(5));
        after.arm(state, 0);
        after.disarm(state);
    }
    const double elapsed = ElapsedMs(start);
    GO2CPP_CHECK(timers.PendingCount() <= baseline);
    ReportPerf("B3 10 万次 AfterCase 创建+立即 disarm", elapsed);
}

// B4：8 个线程各 1 万次 Select(Recv+Done)，一半已取消、一半有数据；另外每
// 线程 100 次"先挂起再取消"的真实阻塞路径。
void SelectHelperB4ConcurrentSelect() {
    using namespace go2cpp;
    using namespace std::chrono_literals;
    constexpr int kWorkers = 8;
    constexpr int kPerWorker = 10000;
    constexpr int kBlockedPerWorker = 100;
    std::atomic<std::size_t> cancelled_hits{0};
    std::atomic<std::size_t> recv_hits{0};
    std::atomic<std::size_t> blocked_cancelled{0};
    std::atomic<std::size_t> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    const auto start = SteadyClock::now();
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([w, &cancelled_hits, &recv_hits,
                              &blocked_cancelled, &failures] {
            auto cancelled_pair = WithCancel(Background());
            cancelled_pair.second();
            auto live_pair = WithCancel(Background());
            auto channel = MakeChannel<int>(1);
            const SelectCase cancelled_done = DoneCase(cancelled_pair.first);
            const SelectCase live_done = DoneCase(live_pair.first);
            for (int i = 0; i < kPerWorker; ++i) {
                if ((i % 2) == 0) {
                    const auto result =
                        Select({RecvCase(channel), cancelled_done});
                    if (result.selected && result.index == 1 &&
                        result.status == ChannelStatus::kCancelled) {
                        cancelled_hits.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    const int value = w * kPerWorker + i;
                    if (!channel->TrySend(value).Ok()) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    const auto result = Select({RecvCase(channel), live_done});
                    if (result.selected && result.index == 0 && result.ok &&
                        result.Value<int>().value_or(-1) == value) {
                        recv_hits.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            // 真实阻塞路径：先 arm，再由伴随线程取消，覆盖 Done 回调唤醒。
            for (int i = 0; i < kBlockedPerWorker; ++i) {
                auto pair = WithCancel(Background());
                std::atomic<bool> entering{false};
                std::thread canceller([&pair, &entering] {
                    while (!entering.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    std::this_thread::sleep_for(200us);
                    pair.second();
                });
                entering.store(true, std::memory_order_release);
                const auto result =
                    Select({RecvCase(channel), DoneCase(pair.first)}, {}, 2s);
                canceller.join();
                if (result.selected && result.index == 1 &&
                    result.status == ChannelStatus::kCancelled) {
                    blocked_cancelled.fetch_add(1, std::memory_order_relaxed);
                } else {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        GO2CPP_JOIN_WITH_WATCHDOG(worker, 30s);
    }
    const double elapsed = ElapsedMs(start);
    GO2CPP_CHECK(failures.load() == 0);
    GO2CPP_CHECK(cancelled_hits.load() ==
                 static_cast<std::size_t>(kWorkers * (kPerWorker / 2)));
    GO2CPP_CHECK(recv_hits.load() ==
                 static_cast<std::size_t>(kWorkers * (kPerWorker / 2)));
    GO2CPP_CHECK(blocked_cancelled.load() ==
                 static_cast<std::size_t>(kWorkers * kBlockedPerWorker));
    ReportPerf("B4 8 线程 x 10000 次 Select(Recv+Done) + 阻塞取消", elapsed);
}

// R1：随机化回归 —— 每轮随机只让一个事件源就绪，断言选中的下标与状态正确。
void SelectHelperR1RandomSources() {
    using namespace go2cpp;
    std::mt19937 rng(20261019);
    std::size_t recv_hits = 0;
    std::size_t done_hits = 0;
    std::size_t timer_hits = 0;
    std::size_t mismatches = 0;
    const auto far = SteadyClock::now() + std::chrono::seconds(60);
    const auto past = SteadyClock::now() - std::chrono::seconds(1);
    const auto start = SteadyClock::now();
    for (int i = 0; i < 3000; ++i) {
        const unsigned kind = rng() % 3U;
        auto channel = MakeChannel<int>(1);
        auto pair = WithCancel(Background());
        auto live = WithCancel(Background());
        if (kind == 0U) {
            GO2CPP_CHECK(channel->TrySend(7).Ok());
            const auto result =
                Select({RecvCase(channel), DoneCase(live.first), AfterCase(far)});
            if (result.selected && result.index == 0 &&
                result.Value<int>().value_or(0) == 7) {
                ++recv_hits;
            } else {
                ++mismatches;
            }
        } else if (kind == 1U) {
            pair.second();
            const auto result =
                Select({RecvCase(channel), DoneCase(pair.first), AfterCase(far)});
            if (result.selected && result.index == 1 &&
                result.status == ChannelStatus::kCancelled) {
                ++done_hits;
            } else {
                ++mismatches;
            }
        } else {
            const auto result =
                Select({RecvCase(channel), DoneCase(live.first), AfterCase(past)});
            if (result.selected && result.index == 2 &&
                result.status == ChannelStatus::kReady) {
                ++timer_hits;
            } else {
                ++mismatches;
            }
        }
    }
    const double elapsed = ElapsedMs(start);
    GO2CPP_CHECK(mismatches == 0);
    GO2CPP_CHECK(recv_hits + done_hits + timer_hits == 3000);
    ReportPerf("R1 3000 轮随机事件源 select", elapsed);
}

void RunSelectHelperCaseTests() {
    go2cpp_tests::announce(
        "select helper cases: DoneCase / AfterCase (T1-T11, B1-B4, R1)");
    SelectHelperT1DoneOnly();
    SelectHelperT2DoneWinsOverRecv();
    SelectHelperT3RecvWinsOverDone();
    SelectHelperT4AfterCaseAlone();
    SelectHelperT5TimerWinsOverRecv();
    SelectHelperT6RecvWinsTimerDisarmed();
    SelectHelperT7ThreeWay();
    SelectHelperT8DefaultNotHijacked();
    SelectHelperT9DoneCaseRepeated();
    SelectHelperT10AfterCaseRepeated();
    SelectHelperT11DoneWithDefault();
    SelectHelperB1MillionSelect();
    SelectHelperB2ThousandCases();
    SelectHelperB3HundredThousandTimers();
    SelectHelperB4ConcurrentSelect();
    SelectHelperR1RandomSources();
    std::cout << "[perf] select helper cases finished" << '\n';
}

}  // namespace
