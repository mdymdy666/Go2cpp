#include "go2cpp/runtime.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// fiber_local 全面版：同一个组件的 7 个使用场景。
//   1  按需构造            GetOrCreate 首次用参数就地构造
//   2  两个 Fiber 隔离      各自的 G 看到各自的值
//   3  park/resume 后存活   值属于 G 而不是某次执行片段；阻塞唤醒后仍在
//   4  Reset               清除后 TryGet 返回空，再次 GetOrCreate 重新构造
//   5  Fiber 外的后备值     在线程里使用时走线程局部后备（不是 Fiber 槽）
//   6  键唯一              每个 FiberLocalCache 实例分配单调唯一的 key_id
//   7  多实例互不干扰       两个不同的 FiberLocal 在同一次执行里各持一份
//
// ★ 重要语义：值属于「当前 Fiber（G）」而不是某个 M。
//   Scheduler 迁移 G 时值随 G 迁移；值在 Fiber trampoline 结束时销毁。
//   在 Fiber 外调用则使用线程局部后备值，于线程退出时销毁。

namespace {

using Ms = std::chrono::milliseconds;

bool scenario_get_or_create() {
    using namespace go2cpp;
    std::clog << "[full][场景1] GetOrCreate 按需就地构造\n";
    FiberLocal<std::string> cache;
    const auto* before = cache.TryGet();
    const auto& value = cache.GetOrCreate("initial");
    const auto* after = cache.TryGet();
    // 再次 GetOrCreate 应返回同一份，而不是重新构造
    const auto& again = cache.GetOrCreate("ignored");
    const bool same_object = (&again == &value);
    std::cout << "  创建前=" << (before == nullptr ? "nullptr" : "有值")
              << " 创建后=\"" << value << "\" 两次调用同一对象=" << same_object
              << "（预期 nullptr / initial / 1）\n";
    return before == nullptr && value == "initial" && after != nullptr && same_object;
}

bool scenario_two_fibers_isolated(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景2] 两个 Fiber 的值互相隔离\n";
    FiberLocal<std::string> cache;
    std::string seen_a;
    std::string seen_b;

    auto task_a = go(scheduler, [&] {
        cache.GetOrCreate("") = "alpha";
        // 让出后再次读取，值仍属于本 G
        std::this_thread::sleep_for(Ms(5));
        seen_a = cache.GetOrCreate("");
    });
    auto task_b = go(scheduler, [&] {
        cache.GetOrCreate("") = "beta";
        std::this_thread::sleep_for(Ms(5));
        seen_b = cache.GetOrCreate("");
    });
    task_a->wait();
    task_b->wait();

    std::cout << "  A=\"" << seen_a << "\" B=\"" << seen_b << "\"（预期 alpha / beta）\n";
    return seen_a == "alpha" && seen_b == "beta";
}

bool scenario_survives_park_resume(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景3] 经过 park/resume 后值仍然存在\n";
    FiberLocal<int> counter;
    auto channel = MakeChannel<int>(0);
    std::atomic<int> observed{-1};

    auto waiter = go(scheduler, [&] {
        counter.GetOrCreate(0) = 4242;
        // 阻塞在无缓冲 Channel 上：G 会被 park，唤醒时可能落在另一个 M 上。
        (void)channel->Recv();
        observed.store(counter.GetOrCreate(-1));   // 值应仍是 4242
    });
    auto sender = go(scheduler, [&] {
        std::this_thread::sleep_for(Ms(20));
        (void)channel->Send(1);
    });
    waiter->wait();
    sender->wait();

    std::cout << "  park/resume 后读到 " << observed.load() << "（预期 4242）\n";
    return observed.load() == 4242;
}

bool scenario_reset() {
    using namespace go2cpp;
    std::clog << "[full][场景4] Reset 后 TryGet 为空，可重新构造\n";
    FiberLocal<std::string> cache;
    cache.GetOrCreate("") = "first";
    const bool had = cache.TryGet() != nullptr;
    cache.Reset();
    const bool cleared = cache.TryGet() == nullptr;
    const auto& rebuilt = cache.GetOrCreate("second");
    const bool rebuilt_ok = rebuilt == "second";

    std::cout << "  Reset 前有值=" << had << " Reset 后为空=" << cleared
              << " 重建后=\"" << rebuilt << "\"（预期 1 1 second）\n";
    return had && cleared && rebuilt_ok;
}

bool scenario_thread_fallback() {
    using namespace go2cpp;
    std::clog << "[full][场景5] Fiber 之外使用线程局部后备值\n";
    FiberLocal<int> cache;
    // 主执行流不是 Fiber，走线程局部后备
    cache.GetOrCreate(0) = 11;

    int seen_in_thread = -1;
    std::thread worker([&] {
        // 新线程没有该键的值 —— 后备值是按线程隔离的
        const int* fresh = cache.TryGet();
        seen_in_thread = (fresh == nullptr) ? -1 : *fresh;
        cache.GetOrCreate(0) = 22;                 // 本线程自己的一份
    });
    worker.join();

    const int main_seen = cache.GetOrCreate(-1);    // 主线程仍是 11
    std::cout << "  新线程初次读到 " << seen_in_thread
              << " 主线程仍为 " << main_seen << "（预期 -1 11）\n";
    return seen_in_thread == -1 && main_seen == 11;
}

bool scenario_key_uniqueness() {
    using namespace go2cpp;
    std::clog << "[full][场景6] 每个实例分配单调唯一的 key_id\n";
    FiberLocal<int> first;
    FiberLocal<int> second;
    FiberLocal<int> third;
    const auto a = first.key_id();
    const auto b = second.key_id();
    const auto c = third.key_id();
    const bool ok = a != b && b != c && a != c && a < b && b < c;
    std::cout << "  key_id=" << a << "," << b << "," << c
              << " 严格递增且互不相同=" << ok << "（预期 1）\n";
    return ok;
}

bool scenario_multiple_instances(go2cpp::Scheduler& scheduler) {
    using namespace go2cpp;
    std::clog << "[full][场景7] 多个 FiberLocal 实例互不干扰\n";
    FiberLocal<int> numbers;
    FiberLocal<std::string> texts;
    std::atomic<int> read_number{-1};
    std::atomic<bool> read_text{false};

    auto task = go(scheduler, [&] {
        numbers.GetOrCreate(0) = 99;
        texts.GetOrCreate("") = "kept";
        // 两个槽独立存取
        read_number.store(numbers.GetOrCreate(-1));
        read_text.store(texts.GetOrCreate("?") == "kept");
    });
    task->wait();

    std::cout << "  数值槽=" << read_number.load()
              << " 文本槽正确=" << read_text.load() << "（预期 99 1）\n";
    return read_number.load() == 99 && read_text.load();
}

}  // namespace

/**
 * @brief fiber_local 全面版：7 个场景，覆盖隔离、迁移存活、重置与线程后备。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;

    Scheduler scheduler(4);
    scheduler.start();

    bool ok = true;
    ok = scenario_get_or_create() && ok;
    ok = scenario_two_fibers_isolated(scheduler) && ok;
    ok = scenario_survives_park_resume(scheduler) && ok;
    ok = scenario_reset() && ok;
    ok = scenario_thread_fallback() && ok;
    ok = scenario_key_uniqueness() && ok;
    ok = scenario_multiple_instances(scheduler) && ok;

    scheduler.shutdown();
    std::clog << (ok ? "[full] 7 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
