/**
 * @file test_ctx_cancel.cpp
 * @brief Doctor `ctx.cancel` 单测：拒绝路径 + 成功取消 + cancel_race。
 *
 * 只在 `GO2CPP_DIAGNOSTIC=ON` 构建编译（DiagnosticAgent 符号只在该构建存在）。
 *
 * 覆盖：
 *   1. 拒绝路径：缺定位参数 / 非法 gid / gid=0 / 未注册 gid / 未注册地址 /
 *      未注册名字，错误码分别是 bad-argument / not-registered；
 *   2. 成功取消：按 gid、按 name、按 address 三种定位各取消一个
 *      `Context::WithCancel` 子 Context，验证 IsDone/Err、审计文本、
 *      context_cancels 计数与 `format=json` 的 schema；
 *   3. 重复取消：was_done=true，不 panic（Doctor 只取消、不重复回滚）；
 *   4. cancel_race：64 个 Context × 8 线程并发取消，全部成功、统计精确、
 *      注册表可清空。
 *
 * 说明：`ctx.cancel` **不解锁、不解引用任意地址**——地址必须命中间注册表。
 */
#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/debug/diagnostic_agent.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using go2cpp::Context;
using go2cpp::ContextPtr;
using go2cpp::debug::DiagnosticAgent;
using go2cpp::debug::DiagnosticCommandResult;

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

DiagnosticCommandResult Run(const std::string& command) {
    return DiagnosticAgent::Instance().Execute(command);
}

void TestRejectedArguments() {
    go2cpp_tests::announce("ctx.cancel: rejected arguments");
    auto& agent = DiagnosticAgent::Instance();
    agent.ResetRegistrations();

    const auto missing = Run("ctx.cancel");
    GO2CPP_CHECK(!missing.ok);
    GO2CPP_CHECK(missing.error_code == "bad-argument");

    const auto bad_gid = Run("ctx.cancel gid=abc");
    GO2CPP_CHECK(!bad_gid.ok);
    GO2CPP_CHECK(bad_gid.error_code == "bad-argument");

    const auto zero_gid = Run("ctx.cancel gid=0");
    GO2CPP_CHECK(!zero_gid.ok);
    GO2CPP_CHECK(zero_gid.error_code == "bad-argument");

    const auto unknown_gid = Run("ctx.cancel gid=999999");
    GO2CPP_CHECK(!unknown_gid.ok);
    GO2CPP_CHECK(unknown_gid.error_code == "not-registered");

    const auto unknown_address = Run("ctx.cancel --address 0x1");
    GO2CPP_CHECK(!unknown_address.ok);
    GO2CPP_CHECK(unknown_address.error_code == "not-registered");

    const auto unknown_name = Run("ctx.cancel name=nope");
    GO2CPP_CHECK(!unknown_name.ok);
    GO2CPP_CHECK(unknown_name.error_code == "not-registered");

    // 未知选项必须被拒绝，而不是被当作名字/地址。
    const auto unknown_option = Run("ctx.cancel --force");
    GO2CPP_CHECK(!unknown_option.ok);
    GO2CPP_CHECK(unknown_option.error_code == "bad-argument");
}

void TestSuccessfulCancel() {
    go2cpp_tests::announce("ctx.cancel: successful cancel by gid/name/address");
    auto& agent = DiagnosticAgent::Instance();
    agent.ResetRegistrations();
    const std::uint64_t before = agent.stats().context_cancels;

    // 按 gid 定位。
    {
        auto pair = Context::WithCancel(Context::Background());
        (void)pair.second;
        GO2CPP_CHECK(agent.RegisterContext("cancel-by-gid", pair.first, 4242));
        GO2CPP_CHECK(!pair.first->IsDone());
        const auto result = Run("ctx.cancel gid=4242");
        GO2CPP_CHECK(result.ok);
        GO2CPP_CHECK(Contains(result.payload, "verdict=cancelled"));
        GO2CPP_CHECK(Contains(result.payload, "matched_by=gid"));
        GO2CPP_CHECK(Contains(result.payload, "now_done=true"));
        GO2CPP_CHECK(pair.first->IsDone());
        GO2CPP_CHECK(pair.first->Err() != nullptr);
        // 重复取消：was_done=true，但命令本身仍返回 cancelled。
        const auto again = Run("ctx.cancel gid=4242");
        GO2CPP_CHECK(again.ok);
        GO2CPP_CHECK(Contains(again.payload, "was_done=true"));
    }

    // 按 name 定位 + JSON 模式。
    {
        auto pair = Context::WithCancel(Context::Background());
        (void)pair.second;
        GO2CPP_CHECK(agent.RegisterContext("cancel-by-name", pair.first, 0));
        const auto result = Run("ctx.cancel name=cancel-by-name format=json");
        GO2CPP_CHECK(result.ok);
        GO2CPP_CHECK(Contains(result.payload,
                              "go2cpp.diagnostic.ctx-cancel/1"));
        GO2CPP_CHECK(Contains(result.payload, "\"matched_by\":\"name\""));
        GO2CPP_CHECK(Contains(result.payload, "\"verdict\":\"cancelled\""));
        GO2CPP_CHECK(pair.first->IsDone());
    }

    // 按 address 定位（地址来自注册表身份，不是任意指针解引用）。
    {
        auto pair = Context::WithCancel(Context::Background());
        (void)pair.second;
        GO2CPP_CHECK(agent.RegisterContext("cancel-by-address", pair.first, 0));
        const std::uintptr_t address =
            reinterpret_cast<std::uintptr_t>(pair.first.get());
        char command[128] = {};
        std::snprintf(command, sizeof(command), "ctx.cancel --address 0x%llx",
                      static_cast<unsigned long long>(address));
        const auto result = Run(command);
        GO2CPP_CHECK(result.ok);
        GO2CPP_CHECK(Contains(result.payload, "matched_by=address"));
        GO2CPP_CHECK(pair.first->IsDone());
    }

    // 成功取消共 4 次：gid 两次（第二次 was_done=true 仍计一次）+ name + address。
    const std::uint64_t after = agent.stats().context_cancels;
    GO2CPP_CHECK(after == before + 4U);
    agent.ResetRegistrations();
    GO2CPP_CHECK(agent.stats().context_cancels == after);
}

void TestCancelRace() {
    go2cpp_tests::announce("ctx.cancel: cancel_race 64 contexts x 8 threads");
    auto& agent = DiagnosticAgent::Instance();
    agent.ResetRegistrations();
    const std::uint64_t before = agent.stats().context_cancels;

    constexpr int kContexts = 64;
    constexpr int kThreads = 8;
    std::vector<ContextPtr> contexts;
    contexts.reserve(kContexts);
    for (int index = 0; index < kContexts; ++index) {
        auto pair = Context::WithCancel(Context::Background());
        (void)pair.second;
        GO2CPP_CHECK(agent.RegisterContext("race-" + std::to_string(index),
                                           pair.first, 100000 + index));
        contexts.push_back(pair.first);
    }

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([&, thread_index] {
            for (int index = thread_index; index < kContexts;
                 index += kThreads) {
                const auto result =
                    Run("ctx.cancel gid=" + std::to_string(100000 + index));
                if (!result.ok) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    GO2CPP_CHECK(failures.load() == 0);
    for (const auto& context : contexts) {
        GO2CPP_CHECK(context->IsDone());
        GO2CPP_CHECK(context->Err() != nullptr);
    }
    const std::uint64_t after = agent.stats().context_cancels;
    GO2CPP_CHECK(after == before + static_cast<std::uint64_t>(kContexts));
    agent.ResetRegistrations();
}

}  // namespace

int main() {
    std::printf("=== Doctor ctx.cancel 单测 ===\n");
    TestRejectedArguments();
    TestSuccessfulCancel();
    TestCancelRace();
    const int failures = go2cpp_tests::g_failures.load();
    std::printf("[ctx-cancel] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
