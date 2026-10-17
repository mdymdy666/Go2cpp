/**
 * @file test_hook_ops.cpp
 * @brief hook 每函数开关的语义测试。
 *
 * ## 覆盖
 *
 * 1. **硬拒绝**：`close`/`shutdown`/`fcntl`/`dup`/`dup2` 不允许被关闭，
 *    `SetOpEnabled(op, false)` 返回 false 且位图不变；
 * 2. **全关时保留不可关闭位**：`SetAllOpsEnabled(false)` 不能绕过上面的硬拒绝；
 * 3. **6 个新接线的操作可正常切换**：readv/writev/sendto/accept/recvfrom/sleep；
 * 4. **19 项全部有名字**（配置键与诊断依赖它）。
 *
 * ## 关于「关闭后确实不 park」
 *
 * 这个测试**没有**验证它，因为可靠地测「协程是否让出」需要精确的并发时序控制
 * （要区分「park 了但很快被唤醒」与「真的阻塞 worker」），在时间预算内写出来的
 * 测试很容易变成 flaky 而不是证据。
 *
 * 间接证据是静态可见的：每个接线函数的函数体第一段就是
 * `if (!OpEnabled(HookOp::kXxx)) { 直调原函数 }`，见 `src/network/hook.cpp`。
 * 真正的行为验证需要「1 个 worker + 一个心跳 Fiber + sleep」的组合，属后续工作。
 */
#include "go2cpp/network/hook.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

using go2cpp::hook::HookOp;
using go2cpp::hook::OpEnabled;
using go2cpp::hook::OpFromString;
using go2cpp::hook::SetAllOpsEnabled;
using go2cpp::hook::SetOpEnabled;
using go2cpp::hook::ToString;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "[hook-ops] FAILED: %s\n", what.c_str());
        ++g_failures;
    }
}

std::string Name(HookOp op) { return ToString(op) ? ToString(op) : "<null>"; }

// ---------------------------------------------------------------------------
// 场景 1：不可关闭的操作，硬拒绝
// ---------------------------------------------------------------------------
void ScenarioNonDisableable() {
    const HookOp ops[] = {HookOp::kClose, HookOp::kShutdown, HookOp::kFcntl,
                          HookOp::kDup, HookOp::kDup2};
    for (const auto op : ops) {
        const std::string name = Name(op);
        Check(OpEnabled(op), name + " 默认应启用");
        Check(!SetOpEnabled(op, false), name + " 关闭请求应被拒绝（返回 false）");
        Check(OpEnabled(op), name + " 被拒后仍应启用");
        Check(SetOpEnabled(op, true), name + " 重新启用应成功");
        Check(OpEnabled(op), name + " 重新启用后应为启用");
    }
    std::printf("[hook-ops] 硬拒绝: %zu 个操作均拒绝关闭\n", sizeof(ops) / sizeof(ops[0]));
}

// ---------------------------------------------------------------------------
// 场景 2：SetAllOpsEnabled(false) 不能绕过硬拒绝
// ---------------------------------------------------------------------------
void ScenarioSetAll() {
    SetAllOpsEnabled(true);
    Check(OpEnabled(HookOp::kRead), "全开后 read 应启用");

    SetAllOpsEnabled(false);
    const HookOp keep[] = {HookOp::kClose, HookOp::kShutdown, HookOp::kFcntl,
                           HookOp::kDup, HookOp::kDup2};
    for (const auto op : keep) {
        Check(OpEnabled(op), Name(op) + " 全关后仍应启用（不能被绕过）");
    }
    const HookOp closed[] = {HookOp::kRead, HookOp::kWrite, HookOp::kReadv,
                             HookOp::kSleep, HookOp::kAccept};
    for (const auto op : closed) {
        Check(!OpEnabled(op), Name(op) + " 全关后应为关闭");
    }
    SetAllOpsEnabled(true);
    std::printf("[hook-ops] 全关: 保留 5 个不可关闭位，其余关闭\n");
}

// ---------------------------------------------------------------------------
// 场景 3：6 个新接线的操作可正常切换
// ---------------------------------------------------------------------------
void ScenarioNewlyWired() {
    SetAllOpsEnabled(true);
    const HookOp wired[] = {HookOp::kReadv,  HookOp::kWritev, HookOp::kSendto,
                            HookOp::kAccept, HookOp::kRecvfrom, HookOp::kSleep};
    for (const auto op : wired) {
        const std::string name = Name(op);
        Check(OpEnabled(op), name + " 默认应启用");
        Check(SetOpEnabled(op, false), name + " 应允许关闭");
        Check(!OpEnabled(op), name + " 关闭后应为关闭");
        Check(SetOpEnabled(op, true), name + " 应允许恢复");
        Check(OpEnabled(op), name + " 恢复后应为启用");
    }
    std::printf("[hook-ops] 新接线: %zu 个操作开关正常\n", sizeof(wired) / sizeof(wired[0]));
}

// ---------------------------------------------------------------------------
// 场景 4：19 项全部有稳定名字，且能从名字解析回来
// ---------------------------------------------------------------------------
void ScenarioNames() {
    constexpr int kCount = static_cast<int>(HookOp::kCount);
    int named = 0;
    for (int i = 0; i < kCount; ++i) {
        const auto op = static_cast<HookOp>(i);
        const char* name = ToString(op);
        if (name == nullptr || name[0] == '\0') {
            Check(false, "操作 " + std::to_string(i) + " 没有名字");
            continue;
        }
        ++named;
        HookOp parsed{};
        if (!OpFromString(name, &parsed) || parsed != op) {
            Check(false, std::string("名字无法解析回同一操作: ") + name);
        }
    }
    Check(named == kCount, "19 项应全部有名字，实际 " + std::to_string(named));
    std::printf("[hook-ops] 名字: %d/%d 项可往返解析\n", named, kCount);
}

}  // namespace

int main() {
    std::printf("=== hook 每函数开关测试 ===\n");
    std::printf("[hook-ops][场景1] 不可关闭操作的硬拒绝\n");
    ScenarioNonDisableable();
    std::printf("[hook-ops][场景2] SetAllOpsEnabled 不能绕过硬拒绝\n");
    ScenarioSetAll();
    std::printf("[hook-ops][场景3] 6 个新接线操作\n");
    ScenarioNewlyWired();
    std::printf("[hook-ops][场景4] 名字往返\n");
    ScenarioNames();

    SetAllOpsEnabled(true);
    if (g_failures == 0) {
        std::printf("\n[hook-ops] failures=0\n");
    } else {
        std::printf("\n[hook-ops] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
