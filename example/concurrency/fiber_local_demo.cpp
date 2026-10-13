#include "go2cpp/runtime.hpp"

#include <iostream>
#include <string>

/**
 * @brief fiber_local 简单版：Fiber 维度的局部变量。
 * @details FiberLocal<T> 类似 thread_local，但作用域是「当前 Fiber（G）」：
 *          ① GetOrCreate —— 取值，不存在时按参数就地构造；
 *          ② 两个 Fiber 各持一份，互不可见；
 *          ③ Reset + TryGet —— 清除后 TryGet 返回空指针。
 * @return 全部校验通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;

    Scheduler scheduler(2);
    scheduler.start();
    bool ok = true;

    // 本示例的键；放在 main 作用域，确保在所有 Fiber 结束前一直存活。
    FiberLocal<std::string> cache;

    // ① 初次访问按参数构造
    std::clog << "[fiber_local][场景1] GetOrCreate\n";
    const std::string* first = cache.TryGet();
    const std::string& created = cache.GetOrCreate("默认值");
    std::cout << "  创建前 TryGet=" << (first == nullptr ? "nullptr" : "有值")
              << " 创建后=\"" << created << "\"（预期 nullptr / 默认值）\n";
    ok = ok && first == nullptr && created == "默认值";

    // ② 两个 Fiber 各持一份
    std::clog << "[fiber_local][场景2] 两个 Fiber 的值互相独立\n";
    std::string seen_a;
    std::string seen_b;
    auto task_a = go(scheduler, [&] {
        cache.GetOrCreate("") = "A";
        seen_a = cache.GetOrCreate("");
    });
    auto task_b = go(scheduler, [&] {
        cache.GetOrCreate("") = "B";
        seen_b = cache.GetOrCreate("");
    });
    task_a->wait();
    task_b->wait();
    std::cout << "  Fiber A 看到 \"" << seen_a << "\"，Fiber B 看到 \"" << seen_b
              << "\"（预期 A / B）\n";
    ok = ok && seen_a == "A" && seen_b == "B";

    // ③ Reset 之后 TryGet 返回空
    std::clog << "[fiber_local][场景3] Reset 与 TryGet\n";
    cache.Reset();
    const std::string* after_reset = cache.TryGet();
    std::cout << "  Reset 后 TryGet=" << (after_reset == nullptr ? "nullptr" : "有值")
              << "（预期 nullptr）\n";
    ok = ok && after_reset == nullptr;

    scheduler.shutdown();
    std::clog << (ok ? "[fiber_local] 全部通过\n" : "[fiber_local] 有失败\n");
    return ok ? 0 : 1;
}
