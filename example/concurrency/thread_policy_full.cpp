#include "go2cpp/thread_policy.hpp"
#include "go2cpp/runtime.hpp"

#include <atomic>
#include <iostream>

// thread_policy 全面版：同一个组件的 7 个使用场景。
//   1  默认状态              普通线程 = kUnmanaged + kInherit
//   2  ScopedThreadParticipation  作用域内提升为 kGmpEligible，退出后恢复
//   3  ScopedThreadHookMode       作用域内临时禁用 / 启用 Hook，退出后恢复
//   4  SetCurrentThreadHookMode   直接设置（无自动恢复），需手工还原
//   5  嵌套守卫              内层退出恢复外层状态，而不是恢复默认值
//   6  HookAllowed 合并语义  进程级开关与线程级覆盖的组合结果
//   7  Scheduler worker 内   runtime_worker 为真、participates_in_gmp 为真
//
// 说明：策略是线程局部的（TLS），守卫必须在同一 OS 线程内构造与析构。

namespace {

using go2cpp::CurrentThreadHookMode;
using go2cpp::CurrentThreadPolicy;
using go2cpp::ScopedThreadHookMode;
using go2cpp::ScopedThreadParticipation;
using go2cpp::SetCurrentThreadHookMode;
using go2cpp::ThreadHookMode;
using go2cpp::ThreadParticipationMode;

bool scenario_default_state() {
    std::clog << "[full][场景1] 默认状态\n";
    const auto snapshot = CurrentThreadPolicy();
    const bool ok = snapshot.participation == ThreadParticipationMode::kUnmanaged &&
                    snapshot.hook == ThreadHookMode::kInherit &&
                    !snapshot.runtime_worker &&
                    snapshot.scheduler == nullptr &&
                    !snapshot.participates_in_gmp();
    std::cout << "  participation=" << static_cast<int>(snapshot.participation)
              << " hook=" << static_cast<int>(snapshot.hook)
              << " runtime_worker=" << snapshot.runtime_worker
              << " scheduler=nullptr " << (snapshot.scheduler == nullptr)
              << "（预期 kUnmanaged kInherit 0 1）\n";
    return ok;
}

bool scenario_scoped_participation() {
    std::clog << "[full][场景2] ScopedThreadParticipation 提升与恢复\n";
    const auto before = CurrentThreadPolicy();
    go2cpp::Scheduler scheduler(1);
    bool inside_ok = false;
    bool gmp_inside = false;
    {
        // ★ 守卫必须传入调度器身份：participates_in_gmp() 要求 scheduler 非空。
        ScopedThreadParticipation scope(scheduler,
                                        ThreadParticipationMode::kGmpEligible);
        const auto inside = CurrentThreadPolicy();
        inside_ok = inside.participation == ThreadParticipationMode::kGmpEligible &&
                    inside.scheduler == &scheduler;
        gmp_inside = inside.participates_in_gmp();
    }
    const auto after = CurrentThreadPolicy();
    const bool restored = after.participation == before.participation &&
                          !after.participates_in_gmp();
    std::cout << "  作用域内提升=" << inside_ok
              << " 参与 GMP=" << gmp_inside
              << " 退出后恢复=" << restored << "（预期 1 1 1）\n";
    return inside_ok && gmp_inside && restored;
}

bool scenario_scoped_hook_mode() {
    std::clog << "[full][场景3] ScopedThreadHookMode 禁用与恢复\n";
    const auto before = CurrentThreadHookMode();
    bool disabled_inside = false;
    bool enabled_inside = false;
    {
        ScopedThreadHookMode disable_scope(ThreadHookMode::kDisabled);
        disabled_inside = (CurrentThreadHookMode() == ThreadHookMode::kDisabled);
    }
    {
        ScopedThreadHookMode enable_scope(ThreadHookMode::kEnabled);
        enabled_inside = (CurrentThreadHookMode() == ThreadHookMode::kEnabled);
    }
    const auto after = CurrentThreadHookMode();
    const bool restored = (after == before);
    std::cout << "  禁用生效=" << disabled_inside << " 启用生效=" << enabled_inside
              << " 退出后恢复=" << restored << "（预期 1 1 1）\n";
    return disabled_inside && enabled_inside && restored;
}

bool scenario_direct_set() {
    std::clog << "[full][场景4] SetCurrentThreadHookMode 直接设置（无自动恢复）\n";
    const auto before = CurrentThreadHookMode();
    SetCurrentThreadHookMode(ThreadHookMode::kDisabled);
    const auto changed = CurrentThreadHookMode();
    // 直接设置没有 RAII 保护，作用域结束也不会自动还原，必须手工恢复
    const bool stayed = (changed == ThreadHookMode::kDisabled);
    SetCurrentThreadHookMode(before);                 // 手工还原
    const auto restored = CurrentThreadHookMode();
    std::cout << "  设置后=" << static_cast<int>(changed)
              << " 未被自动还原=" << stayed
              << " 手工还原后=" << static_cast<int>(restored)
              << "（预期 kDisabled=1 1 原值）\n";
    return stayed && restored == before;
}

bool scenario_nested_guards() {
    std::clog << "[full][场景5] 嵌套守卫：内层退出恢复外层状态\n";
    bool outer_seen = false;
    bool inner_seen = false;
    bool back_to_outer = false;
    bool back_to_default = false;
    {
        ScopedThreadHookMode outer(ThreadHookMode::kEnabled);
        outer_seen = (CurrentThreadHookMode() == ThreadHookMode::kEnabled);
        {
            ScopedThreadHookMode inner(ThreadHookMode::kDisabled);
            inner_seen = (CurrentThreadHookMode() == ThreadHookMode::kDisabled);
        }
        // 关键：内层退出后应恢复成"外层设置的 kEnabled"，而不是默认值
        back_to_outer = (CurrentThreadHookMode() == ThreadHookMode::kEnabled);
    }
    back_to_default = (CurrentThreadHookMode() == ThreadHookMode::kInherit);
    std::cout << "  外层=" << outer_seen << " 内层=" << inner_seen
              << " 回到外层=" << back_to_outer << " 最后回默认=" << back_to_default
              << "（预期 1 1 1 1）\n";
    return outer_seen && inner_seen && back_to_outer && back_to_default;
}

bool scenario_hook_allowed_merge() {
    std::clog << "[full][场景6] HookAllowed：进程级开关与线程级覆盖的合并\n";
    using go2cpp::thread_policy::detail::HookAllowed;
    const auto before = CurrentThreadHookMode();

    // kInherit：跟随进程级开关
    SetCurrentThreadHookMode(ThreadHookMode::kInherit);
    const bool inherit_on = HookAllowed(true);
    const bool inherit_off = HookAllowed(false);

    // kDisabled：无论进程级是否开启，本线程都不走 Hook
    SetCurrentThreadHookMode(ThreadHookMode::kDisabled);
    const bool disabled_on = HookAllowed(true);
    const bool disabled_off = HookAllowed(false);

    // kEnabled：本线程显式进入 Hook 路径 —— 线程级覆盖优先于进程级开关，
    // 因此即使进程级为 false，结果仍然是允许（这一条是我首版预期写错的地方）。
    SetCurrentThreadHookMode(ThreadHookMode::kEnabled);
    const bool enabled_on = HookAllowed(true);
    const bool enabled_off = HookAllowed(false);

    SetCurrentThreadHookMode(before);
    // 合并语义汇总：
    //   kInherit  跟随进程级开关        → (true,false) → (1,0)
    //   kDisabled 线程级禁用，压过进程级 → (false,false) → (0,0)
    //   kEnabled  线程级启用，压过进程级 → (true,true)  → (1,1)  ★ 注意第二个也是 1
    const bool ok = inherit_on && !inherit_off && !disabled_on && !disabled_off &&
                    enabled_on && enabled_off;
    std::cout << "  inherit(" << inherit_on << "," << inherit_off << ") disabled("
              << disabled_on << "," << disabled_off << ") enabled(" << enabled_on
              << "," << enabled_off << ")（预期 (1,0) (0,0) (1,1)）\n";
    return ok;
}

bool scenario_runtime_worker() {
    std::clog << "[full][场景7] Scheduler worker 内：runtime_worker 与参与者身份\n";
    using namespace go2cpp;
    Scheduler scheduler(2);
    scheduler.start();

    std::atomic<bool> worker_flag{false};
    std::atomic<bool> gmp_flag{false};
    std::atomic<bool> hook_disabled{false};
    auto task = scheduler.spawn([&] {
        const auto snapshot = CurrentThreadPolicy();
        worker_flag.store(snapshot.runtime_worker);
        gmp_flag.store(snapshot.participates_in_gmp());
        // 在 worker 内临时禁用 Hook，不影响其他 worker
        ScopedThreadHookMode scope(ThreadHookMode::kDisabled);
        hook_disabled.store(CurrentThreadHookMode() == ThreadHookMode::kDisabled);
    });
    task->wait();
    scheduler.shutdown();

    // 主线程不受 worker 内守卫的影响（TLS 隔离）
    const auto main_snapshot = CurrentThreadPolicy();
    const bool isolated = main_snapshot.hook == ThreadHookMode::kInherit &&
                          !main_snapshot.runtime_worker;
    std::cout << "  worker 内 runtime_worker=" << worker_flag.load()
              << " participates_in_gmp=" << gmp_flag.load()
              << " 可临时禁用 Hook=" << hook_disabled.load()
              << " 主线程未受影响=" << isolated << "（预期 1 1 1 1）\n";
    return worker_flag.load() && gmp_flag.load() && hook_disabled.load() && isolated;
}

}  // namespace

/**
 * @brief thread_policy 全面版：7 个场景，覆盖查询、守卫、嵌套与 worker 身份。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_default_state() && ok;
    ok = scenario_scoped_participation() && ok;
    ok = scenario_scoped_hook_mode() && ok;
    ok = scenario_direct_set() && ok;
    ok = scenario_nested_guards() && ok;
    ok = scenario_hook_allowed_merge() && ok;
    ok = scenario_runtime_worker() && ok;

    std::clog << (ok ? "[full] 7 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}