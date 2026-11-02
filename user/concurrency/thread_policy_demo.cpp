#include "go2cpp/runtime.hpp"
#include "go2cpp/thread_policy.hpp"

#include <iostream>

/**
 * @brief thread_policy 简单版：查询当前线程策略、临时改变它、观察恢复。
 * @details ① CurrentThreadPolicy —— 读取当前线程的调度参与与 Hook 模式；
 *          ② ScopedThreadParticipation —— 在作用域内把线程标记为可参与 GMP；
 *          ③ ScopedThreadHookMode —— 在作用域内临时禁用 Hook；
 *          两个守卫都是 RAII：析构时恢复进入前的状态。
 * @return 全部校验通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;

    bool ok = true;

    // ① 默认状态：普通线程不参与 GMP，Hook 模式为 kInherit
    std::clog << "[thread_policy][场景1] 默认状态\n";
    const auto plain = CurrentThreadPolicy();
    std::cout << "  participation=" << static_cast<int>(plain.participation)
              << " hook=" << static_cast<int>(plain.hook)
              << " runtime_worker=" << plain.runtime_worker
              << " participates_in_gmp=" << plain.participates_in_gmp()
              << "（预期 kUnmanaged kInherit 0 0）\n";
    ok = ok && plain.participation == ThreadParticipationMode::kUnmanaged &&
         plain.hook == ThreadHookMode::kInherit && !plain.runtime_worker &&
         !plain.participates_in_gmp();

    // ② 作用域内绑定调度器并标记为可参与 GMP
    std::clog << "[thread_policy][场景2] ScopedThreadParticipation\n";
    Scheduler scheduler(1);
    {
        // ★ 该守卫必须传入调度器身份（指针或引用）：
        //   participates_in_gmp() 要求 scheduler != nullptr，
        //   只设 participation 而不绑定调度器仍然不算"参与 GMP"。
        ScopedThreadParticipation scope(scheduler,
                                        ThreadParticipationMode::kGmpEligible);
        const auto inside = CurrentThreadPolicy();
        std::cout << "  作用域内 scheduler 已绑定=" << (inside.scheduler == &scheduler)
                  << " participates_in_gmp=" << inside.participates_in_gmp()
                  << "（预期 1 1）\n";
        ok = ok && inside.participation == ThreadParticipationMode::kGmpEligible &&
             inside.scheduler == &scheduler && inside.participates_in_gmp();
    }
    const auto after_scope = CurrentThreadPolicy();
    std::cout << "  作用域后 participation="
              << static_cast<int>(after_scope.participation)
              << " participates_in_gmp=" << after_scope.participates_in_gmp()
              << "（预期 kUnmanaged=0 0）\n";
    ok = ok && after_scope.participation == ThreadParticipationMode::kUnmanaged &&
         !after_scope.participates_in_gmp();

    // ③ 作用域内临时禁用 Hook
    std::clog << "[thread_policy][场景3] ScopedThreadHookMode\n";
    const auto before = CurrentThreadHookMode();
    {
        ScopedThreadHookMode scope(ThreadHookMode::kDisabled);
        const auto inside = CurrentThreadHookMode();
        std::cout << "  作用域内 hook=" << static_cast<int>(inside) << "（预期 kDisabled=1）\n";
        ok = ok && inside == ThreadHookMode::kDisabled;
    }
    const auto restored = CurrentThreadHookMode();
    std::cout << "  作用域后 hook=" << static_cast<int>(restored)
              << "（应恢复到进入前的 " << static_cast<int>(before) << "）\n";
    ok = ok && restored == before;

    std::clog << (ok ? "[thread_policy] 全部通过\n" : "[thread_policy] 有失败\n");
    return ok ? 0 : 1;
}