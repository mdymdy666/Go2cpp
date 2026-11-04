#include "go2cpp/control_flow.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// control_flow 全面版：同一个组件的 6 个使用场景。
//   1  defer 的 LIFO 逆序      后注册的先执行
//   2  defer 的求值时机         值参数在"注册时"求值，引用捕获在"执行时"读取
//   3  dismiss / run_now        取消一个已注册的 defer，或提前手工执行
//   4  panic 是显式状态发布      ★ 不展开栈、不跳转，控制流继续往下走
//   5  recover 的边界           take() 只在 defer 回调内成功，边界外返回空
//   6  defer 回调抛异常         被记录而不中断，其余 defer 仍然执行
//
// ★★★ 场景 4/5 是本示例最重要的部分：本项目的 panic/recover 是「显式状态记录」，
//     与 Go 的 panic/recover 语义不同 —— 它不会展开 C++ 栈、不会自动执行外层 defer、
//     无人 recover 时也不会终止进程。业务代码必须自己检查状态并 return/分支。

namespace {

// 用固定名字的全局记录器，避免引入 Logger（其头文件可能变动）。
std::vector<std::string> g_log;

void note(const std::string& text) {
    g_log.push_back(text);
    std::cout << "  " << text << '\n';
}

bool has(const std::string& text) {
    for (const auto& item : g_log) {
        if (item == text) {
            return true;
        }
    }
    return false;
}

bool scenario_lifo() {
    using namespace go2cpp;
    std::clog << "[full][场景1] defer 按注册逆序执行（LIFO）\n";
    g_log.clear();
    {
        defer first([&] { note("first"); });
        defer second([&] { note("second"); });
        defer third([&] { note("third"); });
        note("body");
    }   // 离开作用域：third → second → first
    const bool ok = g_log.size() == 4 &&
                    g_log[0] == "body" &&
                    g_log[1] == "third" &&
                    g_log[2] == "second" &&
                    g_log[3] == "first";
    std::cout << "  顺序正确 = " << ok << "（预期 1）\n";
    return ok;
}

bool scenario_evaluation_timing() {
    using namespace go2cpp;
    std::clog << "[full][场景2] defer 的求值时机：值参数在注册时求值\n";
    g_log.clear();
    int evaluated = 1;
    {
        // 值参数：构造时就保存了 1，之后改 evaluated 不影响它
        defer by_value([&](int value) { note("by_value=" + std::to_string(value)); },
                       evaluated);
        // lambda 引用捕获：执行时才读，看到的是修改后的 9
        defer by_reference([&] { note("by_reference=" + std::to_string(evaluated)); });
        evaluated = 9;
    }
    // 逆序：by_reference 先执行，by_value 后执行
    const bool ok = has("by_reference=9") && has("by_value=1");
    std::cout << "  值参数=1、引用=9 → " << ok << "（预期 1，与 Go 的 defer 一致）\n";
    return ok;
}

bool scenario_dismiss_and_run_now() {
    using namespace go2cpp;
    std::clog << "[full][场景3] dismiss 取消注册 / run_now 提前执行\n";
    g_log.clear();
    {
        defer cancelled([&] { note("cancelled"); });
        cancelled.dismiss();                       // 取消，离开作用域时不再执行

        defer early([&] { note("early"); });
        early.run_now();                           // 立即执行一次
        // 注意：run_now 不会让析构时跳过，这里用 active() 观测状态
        std::cout << "  early.active()=" << early.active() << '\n';
    }
    const bool ok = !has("cancelled") && has("early");
    std::cout << "  已取消的不执行、提前执行的已执行 → " << ok << "（预期 1）\n";
    return ok;
}

bool scenario_panic_is_state_only() {
    using namespace go2cpp;
    std::clog << "[full][场景4] panic 是显式状态发布，控制流继续（与 Go 的差异）\n";
    panic failure;
    recover recovery(failure);

    bool reached_after_call = false;
    {
        defer cleanup([&] {
            const auto result = recovery.take();
            if (result.has_value()) {
                note("recovered: " + result->message);
            }
        });
        failure.call("业务失败", 503);
        // ★ 关键：这一行会执行。Go 的 panic 会立刻展开栈，这里不会。
        reached_after_call = true;
    }
    const bool ok = reached_after_call && failure.recovered() && !failure.active();
    std::cout << "  call 之后仍继续执行=" << reached_after_call
              << " 已被 recover=" << failure.recovered()
              << " 仍 active=" << failure.active() << "（预期 1 1 0）\n";
    std::cout << "  ★ 与 Go 的差异：panic 不展开栈、不自动跑外层 defer\n";
    return ok;
}

bool scenario_recover_boundary() {
    using namespace go2cpp;
    std::clog << "[full][场景5] recover 的边界：只在 defer 回调内可消费\n";
    panic failure;
    recover recovery(failure);
    failure.call("越界测试", 1);

    // 边界外直接 take：应拿不到（返回空），且状态不被消费
    const auto outside = recovery.take();
    const bool outside_empty = !outside.has_value();
    const bool still_active = failure.active();

    // 放进 defer 回调里再试：这次应当成功
    bool inside_ok = false;
    {
        defer cleanup([&] {
            const auto inside = recovery.take();
            inside_ok = inside.has_value() && inside->code == 1;
        });
    }
    std::cout << "  边界外取到=" << !outside_empty
              << " 边界外取后仍 active=" << still_active
              << " 边界内取到=" << inside_ok << "（预期 0 1 1）\n";
    return outside_empty && still_active && inside_ok;
}

bool scenario_defer_exception() {
    using namespace go2cpp;
    std::clog << "[full][场景6] defer 回调抛异常：被记录而不中断其余 defer\n";
    g_log.clear();
    {
        defer first([&] { note("first"); });
        defer throwing([&] {
            note("throwing");
            throw std::runtime_error("defer 内部异常");
        });
        defer last([&] { note("last"); });
    }
    // 逆序：last → throwing（抛）→ first 仍应执行
    const bool ok = has("last") && has("throwing") && has("first");
    std::cout << "  三个回调都执行了 = " << ok << "（预期 1，异常不中断清理）\n";
    return ok;
}

}  // namespace

/**
 * @brief control_flow 全面版：6 个场景，覆盖 defer 顺序/求值/取消与 panic 状态语义。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;

    bool ok = true;
    ok = scenario_lifo() && ok;
    ok = scenario_evaluation_timing() && ok;
    ok = scenario_dismiss_and_run_now() && ok;
    ok = scenario_panic_is_state_only() && ok;
    ok = scenario_recover_boundary() && ok;
    ok = scenario_defer_exception() && ok;

    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
