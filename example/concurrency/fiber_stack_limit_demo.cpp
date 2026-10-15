/**
 * @file fiber_stack_limit_demo.cpp
 * @brief fiber_stack_limit 简单版：Fiber 栈是**固定**的，先用三种手法绕开它。
 *
 * @details Fiber 默认栈 128 KiB 且不会增长；全项目没有 SIGSEGV handler、
 *          也没有 sigaltstack，所以爆栈 = 内核默认动作 = 进程被立即终止。
 *          本示例用三个最小场景说明工程上怎么处理这件事：
 *
 *            ① 量安全深度 —— 默认 128 KiB 栈上，保守的递归深度能跑通；
 *            ② 递归改迭代 —— 同样的计算改成循环，栈占用变成常数；
 *            ③ 显式限深   —— 解析深层 JSON 前先量嵌套深度，越界就返回错误。
 *
 *          精确的临界深度必须在 **fork 出来的子进程**里二分搜索才能测出
 *          （爆栈不可捕获，同进程内无法"试探到崩为止"），
 *          完整做法见 `fiber_stack_limit_full.cpp` 场景 1 与配套基准
 *          `benchmarks/fiber_stack_probe.cpp`。
 *
 * @return 全部校验通过返回 0，否则返回 1。
 */
#include "go2cpp/fiber.hpp"
#include "go2cpp/json.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

/// 防止编译器把无副作用的递归整段消除。
volatile std::uint64_t g_sink = 0;

/**
 * @brief 每层只吃一个很小的栈帧的递归函数。
 * @details `noinline` 是必须的：否则编译器可能把递归折叠成循环，
 *          那样就不消耗栈了，示例也就失去意义。
 */
__attribute__((noinline)) std::uint64_t walk(std::size_t depth) {
    volatile char pad[32];
    pad[0] = static_cast<char>(depth & 0x7F);
    if (depth == 0) {
        return static_cast<std::uint64_t>(pad[0]);
    }
    return walk(depth - 1) + static_cast<std::uint64_t>(pad[0]);
}

/**
 * @brief 场景 1：默认 128 KiB 栈上的安全递归深度。
 * @details 这里刻意只取 **300 层**这一安全侧采样，不去踩临界值：
 *          实测临界深度随每帧字节数变化（Release 下帧≈0 时约 1022 层、
 *          帧 256 字节时约 340 层），而无优化的 Debug 构建帧更大、临界更浅。
 *          示例必须在任何构建类型下都稳定通过，所以取一个留足余量的深度。
 */
bool scenario_measure_safe_depth() {
    std::clog << "[fiber_stack_limit][场景1] 默认栈上的安全递归深度\n";
    constexpr std::size_t kDepth = 300;
    const std::size_t stack = go2cpp::Fiber::DefaultStackSize();

    go2cpp::Fiber fiber([&] { g_sink += walk(kDepth); }, stack);
    const bool accepted = fiber.resume();
    const bool completed =
        accepted && fiber.state() == go2cpp::FiberState::Completed;

    std::cout << "  栈容量=" << stack / 1024U << " KiB，递归 " << kDepth
              << " 层完成=" << completed
              << "（实测临界：帧 0 约 1022 层、帧 256B 约 340 层，预期 1）\n";
    return completed && fiber.stack_size() == stack;
}

/// 场景 2：同样的求和，递归版受栈限制，迭代版栈占用恒定。
bool scenario_recursion_to_iteration() {
    std::clog << "[fiber_stack_limit][场景2] 递归改迭代：栈占用变成常数\n";
    constexpr std::size_t kDeep = 5000000;

    std::uint64_t sum = 0;
    for (std::size_t i = 1; i <= kDeep; ++i) {
        sum += i;
    }
    const std::uint64_t expected =
        static_cast<std::uint64_t>(kDeep) * (kDeep + 1) / 2;
    const bool ok = sum == expected;

    std::cout << "  迭代 " << kDeep << " 项求和=" << sum << " 公式校验=" << ok
              << "（同一栈上递归版最多约 10^3 层，预期 1）\n";
    return ok;
}

/**
 * @brief 场景 3：显式限深 —— 深层 JSON 被拒绝，而不是让进程消失。
 * @details `go2cpp::ParseJson` 先做**迭代**深度扫描（不递归、不分配），
 *          超限直接在入口返回 `Wrap(JsonDepthExceededError(), ...)`。
 *          错误种类用**身份比较**判定（文本里带具体上限，不能拿文本比）。
 *
 *          ⚠️ 被拦住的真正风险不在解析：上游 3.11.3 的解析器本身用显式栈
 *          （迭代），但**解析之后的 `dump()` 与拷贝仍是递归的** ——
 *          深文档进入程序后，任意一次 dump/拷贝都会让进程消失。
 *          实测临界深度见 `fiber_stack_limit_full.cpp` 场景 5。
 */
bool scenario_json_depth_guard() {
    std::clog << "[fiber_stack_limit][场景3] 显式限深：深层 JSON 返回错误而不是崩栈\n";

    std::string deep(1000, '[');
    deep.append(1000, ']');
    const go2cpp::JsonParseResult rejected = go2cpp::ParseJson(deep);
    const bool is_depth_error =
        go2cpp::Is(rejected.error, go2cpp::JsonDepthExceededError());

    const go2cpp::JsonParseResult shallow = go2cpp::ParseJson("{\"a\":[1,2,3]}");
    const bool shallow_ok = shallow.ok() && shallow.value["a"][2] == 3;

    const bool ok = !rejected.ok() && is_depth_error && shallow_ok;
    std::cout << "  1000 层嵌套被拒=" << !rejected.ok()
              << " 错误身份匹配=" << is_depth_error
              << " 浅层文档解析=" << shallow_ok << "（预期 1 1 1）\n";
    if (!rejected.ok()) {
        std::cout << "  拒绝原因：" << go2cpp::ErrorMessage(rejected.error) << "\n";
    }
    return ok;
}

}  // namespace

/**
 * @brief fiber_stack_limit 简单版：三个规避手法的最小演示。
 * @return 全部通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_measure_safe_depth() && ok;
    ok = scenario_recursion_to_iteration() && ok;
    ok = scenario_json_depth_guard() && ok;

    std::clog << (ok ? "[fiber_stack_limit] 全部通过\n"
                     : "[fiber_stack_limit] 有失败\n");
    return ok ? 0 : 1;
}
