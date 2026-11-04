#include "go2cpp/runtime.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

// runtime 全面版：聚合头的 7 个使用场景。
//   runtime.hpp 自身没有 API —— 它是"一次包含全部公开模块"的便捷入口，
//   面向转译程序；大型工程仍可按模块分别包含以缩小依赖面。
//   因此本示例的重点不是重复单模块演示，而是「跨模块协同」：
//
//   1  多子系统并存        channel + future + context 同时可用
//   2  取消驱动的收敛      Context 取消 → 接收方退出 → Fiber 结束
//   3  提交与汇合          Scheduler + go + WaitGroup + Timer 协同
//   4  配置驱动运行时      IniFile 读出参数，直接影响调度器配置
//   5  Future + Context    等待被上下文取消打断
//   6  control_flow 作用域 在 Fiber 内用 defer 保证清理
//   7  fiber_local 隔离    多个 Fiber 各持一份，互不可见
//
// 每个场景独立成段；校验失败只记 ok=false，最后统一报告。

namespace {

using Ms = std::chrono::milliseconds;

bool scenario_multiple_subsystems(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景1] 一个聚合头同时用上 channel / future / context\n";
    // channel
    auto channel = MakeChannel<int>(1);
    (void)channel->Send(7);
    const auto got = channel->Recv();
    // future
    auto pair = MakePromise<int>();
    auto promise = std::make_shared<Promise<int>>(std::move(pair.first));
    const auto future = pair.second;
    auto worker = go(scheduler, [promise] { (void)promise->SetValue(9); });
    const auto result = future.GetResult();
    worker->wait();
    // context
    const auto root = Background();

    const bool ok = got && *got.value == 7 && result.Ok() && result.value &&
                    *result.value == 9 && !root->IsDone();
    std::cout << "  channel=" << (got ? *got.value : -1)
              << " future=" << (result.value ? *result.value : -1)
              << " context 未取消=" << !root->IsDone() << "（预期 7 9 1）\n";
    return ok;
}

bool scenario_cancel_driven_shutdown(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景2] 取消驱动收敛：Context → 接收方退出\n";
    auto channel = MakeChannel<int>(0);
    auto cancel_pair = WithCancel(Background());
    const auto context = cancel_pair.first;
    std::atomic<bool> exited{false};
    std::atomic<bool> cancelled{false};

    auto receiver = go(scheduler, [&] {
        // 无缓冲 channel 上没有发送者；上下文取消后 Recv 返回而不是永久等待
        const auto message = channel->Recv(context);
        cancelled.store(message.Cancelled());
        exited.store(true);
    });
    auto canceller = go(scheduler, [cancel = cancel_pair.second] {
        std::this_thread::sleep_for(Ms(30));
        cancel();
    });
    const bool joined = receiver->wait_for(std::chrono::seconds(2)) &&
                        canceller->wait_for(std::chrono::seconds(2));
    std::cout << "  接收方退出=" << exited.load() << " 判定为取消="
              << cancelled.load() << " 都结束=" << joined << "（预期 1 1 1）\n";
    return exited.load() && cancelled.load() && joined;
}

bool scenario_submit_and_join(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景3] 提交与汇合：WaitGroup + Timer\n";
    sync::WaitGroup group;
    std::atomic<int> finished{0};
    std::atomic<int> timer_fired{0};
    constexpr int kTasks = 5;

    for (int index = 0; index < kTasks; ++index) {
        group.Add(1);
        (void)go(scheduler, [&finished, &group] {
            finished.fetch_add(1);
            group.Done();
        });
    }
    // 定时器与任务并行推进，用句柄保证它不会被提前取消
    auto timer = after(Ms(10), [&timer_fired] { timer_fired.fetch_add(1); });
    (void)timer;

    const bool waited = group.WaitFor(std::chrono::seconds(2));
    const bool fired = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (timer_fired.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(Ms(1));
        }
        return timer_fired.load() == 1;
    }();
    std::cout << "  WaitGroup 汇合=" << waited << " 完成=" << finished.load()
              << " 定时器触发=" << fired << "（预期 1 " << kTasks << " 1）\n";
    return waited && finished.load() == kTasks && fired;
}

bool scenario_config_drives_runtime() {
    using namespace go2cpp;
    std::clog << "[full][场景4] 配置驱动运行时：INI 参数直接决定 P 数量\n";
    const std::string text = "[scheduler]\nprocessors = 3\n";
    config::IniFile ini;
    const bool parsed = ini.Parse(text);
    const auto value = ini.Get("scheduler", "processors", "1");
    const auto processors = static_cast<std::size_t>(std::stoul(value));

    Scheduler scheduler(processors);
    scheduler.Start();
    const auto actual = scheduler.processor_count();
    scheduler.Shutdown();

    const bool ok = parsed && processors == 3 && actual == 3;
    std::cout << "  配置读出=" << value << " 实际 P=" << actual
              << "（预期 3 3）\n";
    return ok;
}

bool scenario_future_cancelled(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景5] Future + Context：等待被取消打断\n";
    auto pair = MakePromise<int>();
    const auto future = pair.second;
    // 故意不完成，只保留 promise 让 Future 一直挂起
    auto promise = std::make_shared<Promise<int>>(std::move(pair.first));
    (void)promise;

    auto cancel_pair = WithCancel(Background());
    auto canceller = go(scheduler, [cancel = cancel_pair.second] {
        std::this_thread::sleep_for(Ms(20));
        cancel();
    });
    const auto status = future.Wait(cancel_pair.first);
    canceller->wait();
    const bool ok = status == FutureStatus::kCancelled;
    std::cout << "  status=" << FutureStatusName(status) << "（预期 kCancelled）\n";
    return ok;
}

bool scenario_defer_in_fiber(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景6] control_flow：Fiber 内用 defer 保证清理\n";
    std::vector<std::string> order;
    auto task = go(scheduler, [&order] {
        defer first([&order] { order.push_back("first"); });
        defer second([&order] { order.push_back("second"); });
        order.push_back("body");
    });
    task->wait();
    const bool ok = order.size() == 3 && order[0] == "body" &&
                    order[1] == "second" && order[2] == "first";
    std::cout << "  顺序=";
    for (const auto& entry : order) {
        std::cout << entry << " ";
    }
    std::cout << "（预期 body second first，即 LIFO）\n";
    return ok;
}

bool scenario_fiber_local_isolation(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景7] fiber_local：多个 Fiber 各持一份\n";
    FiberLocal<std::string> slot;
    std::string seen_a;
    std::string seen_b;
    auto task_a = go(scheduler, [&] {
        slot.GetOrCreate("") = "A";
        seen_a = slot.GetOrCreate("");
    });
    auto task_b = go(scheduler, [&] {
        slot.GetOrCreate("") = "B";
        seen_b = slot.GetOrCreate("");
    });
    task_a->wait();
    task_b->wait();
    const bool ok = seen_a == "A" && seen_b == "B";
    std::cout << "  A=\"" << seen_a << "\" B=\"" << seen_b
              << "\"（预期 A B，互相不可见）\n";
    return ok;
}

}  // namespace

/**
 * @brief runtime 全面版：聚合头的 7 个跨模块协同场景。
 * @details runtime.hpp 是便捷总头（自身无 API），本示例验证的是
 *          "一次包含即可协同使用多个子系统"。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;

    Scheduler scheduler(4);
    scheduler.start();

    bool ok = true;
    ok = scenario_multiple_subsystems(scheduler) && ok;
    ok = scenario_cancel_driven_shutdown(scheduler) && ok;
    ok = scenario_submit_and_join(scheduler) && ok;
    ok = scenario_config_drives_runtime() && ok;
    ok = scenario_future_cancelled(scheduler) && ok;
    ok = scenario_defer_in_fiber(scheduler) && ok;
    ok = scenario_fiber_local_isolation(scheduler) && ok;

    scheduler.shutdown();
    std::clog << (ok ? "[full] 7 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
