#include "go2cpp/channel.hpp"
#include "go2cpp/control_flow.hpp"
#include "test_support.hpp"

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>
#include <memory>
#include <vector>

void run_control_flow_tests() {
    go2cpp_tests::announce("defer/panic/recover");
    using namespace go2cpp;

    std::vector<int> order;
    int evaluated = 3;
    {
        defer first([&] { order.push_back(1); });
        defer second([&] { order.push_back(2); });
        defer captured([&](int value) { order.push_back(value); }, evaluated);
        evaluated = 9;
    }
    GO2CPP_CHECK(order.size() == 3);
    GO2CPP_CHECK(order[0] == 3);
    GO2CPP_CHECK(order[1] == 2);
    GO2CPP_CHECK(order[2] == 1);

    int run_count = 0;
    {
        defer once([&] { ++run_count; });
        once.run_now();
        once.run_now();
        GO2CPP_CHECK(!once.active());
    }
    GO2CPP_CHECK(run_count == 1);

    bool dismissed_ran = false;
    {
        defer dismissed([&] { dismissed_ran = true; });
        dismissed.dismiss();
    }
    GO2CPP_CHECK(!dismissed_ran);

    bool unwound_ran = false;
    try {
        defer unwound([&] { unwound_ran = true; });
        throw std::runtime_error("ordinary scope unwind");
    } catch (const std::runtime_error&) {
        GO2CPP_CHECK(unwound_ran);
    }

    panic pa;
    recover rec(pa);
    GO2CPP_CHECK(!rec.active());
    GO2CPP_CHECK(!rec());
    bool recovered_in_defer = false;
    PanicInfo recovered_info;
    {
        defer boundary([&] {
            const auto value = rec.take();
            recovered_in_defer = value.has_value();
            if (value.has_value()) {
                recovered_info = *value;
            }
        });
        pa.call("explicit failure", 17);
        GO2CPP_CHECK(pa.active());
        GO2CPP_CHECK(!rec.take().has_value());
    }
    GO2CPP_CHECK(recovered_in_defer);
    GO2CPP_CHECK(!pa.active());
    GO2CPP_CHECK(pa.recovered());
    GO2CPP_CHECK(recovered_info.message == "explicit failure");
    GO2CPP_CHECK(recovered_info.code == 17);

    panic callable_panic;
    recover callable_recover(callable_panic);
    {
        defer boundary([&] {
            GO2CPP_CHECK(callable_recover());
        });
        callable_panic.call(std::string("cleared"), 3);
    }
    GO2CPP_CHECK(callable_panic.recovered());
    GO2CPP_CHECK(!callable_panic.info().valid());

    ClearDeferException();
    {
        defer catches_throw([] { throw std::runtime_error("defer callback"); });
    }
    GO2CPP_CHECK(LastDeferException() != nullptr);
    bool ordinary_exception_seen = false;
    try {
        throw std::logic_error("ordinary exception");
    } catch (const std::logic_error&) {
        ordinary_exception_seen = true;
    }
    GO2CPP_CHECK(ordinary_exception_seen);
    ClearDeferException();
    GO2CPP_CHECK(LastDeferException() == nullptr);

    panic separate;
    recover separate_recover(separate);
    std::atomic<bool> other_thread_saw_active{true};
    std::thread worker([&] {
        other_thread_saw_active.store(separate_recover.active(),
                                       std::memory_order_release);
        GO2CPP_CHECK(!separate_recover());
    });
    worker.join();
    GO2CPP_CHECK(!other_thread_saw_active.load(std::memory_order_acquire));

    auto typed_channel = MakeChannel<std::string>(1);
    GO2CPP_CHECK(typed_channel->Send("41").Ok());
    const auto selected = Select({RecvCase(typed_channel)});
    GO2CPP_CHECK(selected.selected);
    GO2CPP_CHECK(selected.TypedValue<std::string>().value_or("") == "41");
    auto caster = MakeCaster<std::string, int>(
        [](const std::string& value) { return std::stoi(value); });
    const auto converted = selected.Cast<int>(caster);
    GO2CPP_CHECK(converted.has_value() && *converted == 41);

    SelectProbe legacy_probe;
    legacy_probe.ready = true;
    legacy_probe.ok = true;
    GO2CPP_CHECK(legacy_probe.SetAny(std::any(std::string("8"))));
    const auto legacy_value = legacy_probe.typed_value.As<std::string>();
    GO2CPP_CHECK(legacy_value.has_value() && *legacy_value == "8");
    class StackCaster final : public SelectCaster {
    public:
        SelectValue Cast(const SelectValue& source) const noexcept override {
            const auto value = source.As<std::string>();
            if (!value.has_value()) {
                return {};
            }
            return SelectValue::From(std::stoi(*value));
        }
    } stack_caster;
    SelectResult legacy_result{0, true, true, std::move(legacy_probe.value),
                               ChannelStatus::kReady, {},
                               std::move(legacy_probe.typed_value)};
    const auto stack_converted = legacy_result.Cast<int>(stack_caster);
    GO2CPP_CHECK(stack_converted.has_value() && *stack_converted == 8);

    class ThrowingCaster final : public SelectCaster {
    public:
        SelectValue Cast(const SelectValue&) const override {
            throw std::runtime_error("caster failure");
        }
    } throwing_caster;
    GO2CPP_CHECK(!legacy_result.Cast<int>(throwing_caster).has_value());

    auto move_only = SelectValue::From(std::make_unique<int>(9));
    GO2CPP_CHECK(move_only.Get<std::unique_ptr<int>>() != nullptr);
    const auto moved = move_only.Take<std::unique_ptr<int>>();
    GO2CPP_CHECK(moved.has_value() && **moved == 9);

    SelectResult move_only_result{
        0, true, true, {}, ChannelStatus::kReady, {},
        SelectValue::From(std::make_unique<int>(10))};
    GO2CPP_CHECK(!move_only_result.Value<std::unique_ptr<int>>().has_value());
    const auto moved_result = move_only_result.TakeValue<std::unique_ptr<int>>();
    GO2CPP_CHECK(moved_result.has_value() && **moved_result == 10);

    // 内建 Channel Select 对 move-only case 明确返回错误，不能静默登记
    // 一个无法安全回滚的异步等待；普通 Send/Recv 仍支持该类型。
    auto move_channel = MakeChannel<std::unique_ptr<int>>(0);
    const auto move_recv_select = Select({RecvCase(move_channel)});
    GO2CPP_CHECK(move_recv_select.status == ChannelStatus::kInvalid);
    const auto move_send_select =
        Select({SendCase(move_channel, std::make_unique<int>(11))});
    GO2CPP_CHECK(move_send_select.status == ChannelStatus::kInvalid);
}
