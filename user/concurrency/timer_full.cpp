#include "go2cpp/runtime.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

// timer 全面版：同一个组件的 6 个使用场景。
//   1  after 一次性任务        延时到点后执行一次
//   2  多个定时器按到期顺序    10/20/30ms 三个任务，回调顺序应与延时一致
//   3  Cancel 取消未领取任务   取消后回调不再执行
//   4  Active / RunCount       观测句柄生命周期与已执行次数
//   5  与 WaitGroup 配合       等一批定时任务全部完成
//   6  定时器 vs Context 超时  两者语义不同：一个触发动作，一个取消等待
//
// 说明：after() 的回调由运行时投递执行；本示例用轮询 + sleep 等待结果，
// 与既有 timer_task_demo.cpp 的做法一致。
//
// ★★ 关键语义：Timer 句柄析构会取消尚未领取的任务。
//    必须把 after() 返回的句柄保存下来，否则写成 (void)after(...)
//    会让临时句柄立即析构，定时器永不触发（本示例首版就栽在这里）。

namespace {

using Ms = std::chrono::milliseconds;

// 轮询等待谓词成立；超时返回 false。
template <typename Predicate>
bool wait_for(Predicate predicate, Ms timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(Ms(1));
    }
    return true;
}

bool scenario_after_once() {
    using namespace go2cpp;
    std::clog << "[full][场景1] after 一次性任务\n";
    std::atomic<int> fired{0};
    auto timer = after(Ms(20), [&fired] { fired.fetch_add(1); });

    const bool ran = wait_for([&fired] { return fired.load() == 1; }, Ms(1000));
    // 再等一会儿，确认只执行一次
    std::this_thread::sleep_for(Ms(40));
    const bool once = fired.load() == 1;
    std::cout << "  已执行=" << ran << " 恰好一次=" << once
              << " RunCount=" << timer.RunCount() << "（预期 1 1 1）\n";
    return ran && once;
}

bool scenario_ordering() {
    using namespace go2cpp;
    std::clog << "[full][场景2] 多个定时器按到期时间顺序执行\n";
    std::vector<int> order;
    std::atomic<int> done{0};
    // ★ 必须持有 Timer 句柄：句柄析构会取消尚未执行的任务。
    //   写成 (void)after(...) 会让临时句柄立刻析构，定时器永不触发。
    std::vector<core::Timer> timers;
    timers.push_back(after(Ms(60), [&] { order.push_back(30); done.fetch_add(1); }));
    timers.push_back(after(Ms(20), [&] { order.push_back(10); done.fetch_add(1); }));
    timers.push_back(after(Ms(40), [&] { order.push_back(20); done.fetch_add(1); }));

    const bool all = wait_for([&done] { return done.load() == 3; }, Ms(2000));
    const bool ordered = order.size() == 3 && order[0] == 10 &&
                         order[1] == 20 && order[2] == 30;
    std::cout << "  三个都执行=" << all << " 顺序=" << (order.size() > 0 ? order[0] : -1)
              << "," << (order.size() > 1 ? order[1] : -1)
              << "," << (order.size() > 2 ? order[2] : -1)
              << "（预期 1 10,20,30）\n";
    return all && ordered;
}

bool scenario_cancel() {
    using namespace go2cpp;
    std::clog << "[full][场景3] Cancel 取消尚未领取的任务\n";
    std::atomic<bool> fired{false};
    auto timer = after(Ms(120), [&fired] { fired.store(true); });
    const bool active_before = timer.Active();
    timer.Cancel();
    const bool active_after = timer.Active();

    std::this_thread::sleep_for(Ms(220));      // 超过原定延时
    std::cout << "  取消前 Active=" << active_before
              << " 取消后 Active=" << active_after
              << " 是否执行=" << fired.load() << "（预期 1 0 0）\n";
    return active_before && !active_after && !fired.load();
}

bool scenario_run_count() {
    using namespace go2cpp;
    std::clog << "[full][场景4] Active / RunCount 观测生命周期\n";
    std::atomic<int> fired{0};
    auto timer = after(Ms(20), [&fired] { fired.fetch_add(1); });

    const bool before_ok = timer.Active() && timer.RunCount() == 0;
    const bool ran = wait_for([&fired] { return fired.load() == 1; }, Ms(1000));
    const std::size_t after_count = timer.RunCount();
    std::cout << "  执行前 Active=1 且 RunCount=0 → " << before_ok
              << " 执行后 RunCount=" << after_count << "（预期 1 1）\n";
    return before_ok && ran && after_count == 1;
}

bool scenario_with_wait_group() {
    using namespace go2cpp;
    std::clog << "[full][场景5] 与 WaitGroup 配合：等一批定时任务完成\n";
    Scheduler scheduler(3);
    scheduler.start();
    sync::WaitGroup group;
    std::atomic<int> finished{0};
    constexpr int kTimers = 4;

    std::vector<core::Timer> timers;
    for (int i = 0; i < kTimers; ++i) {
        group.Add(1);
        // 同上：句柄必须活到回调被领取为止
        timers.push_back(after(Ms(20 + i * 10), [&finished, &group] {
            finished.fetch_add(1);
            group.Done();
        }));
    }
    const bool waited = group.WaitFor(Ms(2000));
    scheduler.shutdown();
    std::cout << "  WaitFor 成功=" << waited
              << " 完成数=" << finished.load() << "（预期 1 " << kTimers << "）\n";
    return waited && finished.load() == kTimers;
}

bool scenario_timer_vs_context_timeout() {
    using namespace go2cpp;
    std::clog << "[full][场景6] 定时器触发动作 vs Context 超时取消等待\n";
    // 定时器：到点执行一个动作，不改变别的等待
    std::atomic<bool> action_ran{false};
    auto action_timer = after(Ms(20), [&action_ran] { action_ran.store(true); });
    (void)action_timer;                        // 保住句柄，任务才会触发
    const bool action_ok = wait_for([&action_ran] { return action_ran.load(); }, Ms(1000));

    // Context 超时：到点取消等待，Err() 变成 DeadlineExceededError
    auto pair = WithTimeout(Background(), Ms(20));
    const bool timed_out = pair.first->Done().WaitFor(Ms(1000)) &&
                           Is(pair.first->Err(), DeadlineExceededError());
    (void)action_ok;
    std::cout << "  定时器动作已执行=" << action_ok
              << " Context 超时并带 DeadlineExceeded=" << timed_out
              << "（预期 1 1）\n";
    std::cout << "  ★ 区别：定时器是「到点做事」，Context 超时是「到点放弃等待」\n";
    return action_ok && timed_out;
}

}  // namespace

/**
 * @brief timer 全面版：6 个场景，覆盖一次性/顺序/取消/观测/汇合与语义对比。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_after_once() && ok;
    ok = scenario_ordering() && ok;
    ok = scenario_cancel() && ok;
    ok = scenario_run_count() && ok;
    ok = scenario_with_wait_group() && ok;
    ok = scenario_timer_vs_context_timeout() && ok;

    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}