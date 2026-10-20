/**
 * @file fiber_stack_limit_full.cpp
 * @brief Fiber 栈上限的实测与六条工程对策（全面版）。
 *
 * @details 背景（定性结论来自本项目实测，数字均可在本文件运行输出中复现）：
 *
 *            - Fiber 默认栈 **128 KiB，且固定不增长**；栈最低地址处有 1 页
 *              `PROT_NONE` guard page。
 *            - 全项目**没有 SIGSEGV handler、没有 sigaltstack**，所以爆栈的
 *              唯一结果是内核默认动作 —— 进程被立即终止（退出码 139），
 *              **不可捕获、不可恢复**。
 *            - 实测规律：`可承受调用深度 ≈ 栈容量 / (每帧局部变量字节数 + 128)`。
 *              那个 128 字节是与帧大小无关的固定开销（返回地址、保存寄存器、对齐）。
 *
 *          ★ 一个贯穿全篇的关键区分：
 *            **Fiber 嵌套深度（nesting_depth）与 C++ 调用深度是两个正交维度。**
 *            前者每层自带一块独立栈，后者才消耗当前栈。所以"到达一定深度就
 *            resume 一个新 Fiber"是有效的规避手段 —— 它就是 Go 自动增长栈
 *            （morestack/copystack）的**手动等价物**。
 *
 *          ★ 场景 5 会推翻一个常见的想当然结论：**"深层 JSON 会在解析时爆栈"
 *            并不成立**。nlohmann/json v3.11.3 的 `parse()` 走
 *            `sax_parse_internal`，用堆上的 `std::vector<bool> states` 作显式栈，
 *            是**迭代**实现；真正会爆栈的是之后的 `dump()` 与拷贝（它们仍是递归）。
 *
 *          六个场景（每个场景一个函数，可独立阅读）：
 *            1. 探深       —— 在 fork 出来的子进程里二分搜索真实临界深度；
 *            2. 递归改迭代 —— 同样的计算改成循环，栈占用变成常数；
 *            3. 换 Fiber   —— ★ 每跨过一个阈值就换一块全新栈，突破单栈上限；
 *            4. 显式限深   —— 把"不可恢复的崩溃"换成"可处理的错误"；
 *            5. 解析与递归 —— 解析是迭代的，dump/拷贝才是递归的（实测临界深度）；
 *            6. JSON 深层嵌套 —— 真实场景：嵌套结构、边界、随机输入、攻击载荷。
 *
 * @note 场景 3 是规避手段的重点。它的三个代价必须记住：
 *       ① 父 Fiber **阻塞**在 resume 上（同步调用，不是并行）；
 *       ② 内存叠加（嵌套 N 层 = N × 128 KiB 栈）；
 *       ③ 每层有真实的 Fiber 创建开销（Release 下约 25 µs 量级）。
 * @note 所有"会不会崩"的判定都在 **fork 出来的子进程**里做，并用 waitpid 的
 *       终止状态判断结局 —— 爆栈不可捕获，同进程内无法安全地试探到崩为止。
 * @note 涉及计时的数字只在 Release 基准里才有性能意义（本示例可能在无优化
 *       构建下运行）。
 *
 * @return 全部场景通过返回 0，否则返回 1。
 */
#include "go2cpp/fiber.hpp"
#include "go2cpp/json.hpp"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <alloca.h>
#include <pthread.h>

#include <cstdlib>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <utility>

namespace {

using Clock = std::chrono::steady_clock;

/// 防止编译器把无副作用的递归整段消除。
volatile std::uint64_t g_sink = 0;

constexpr std::size_t kDefaultStack = 128U * 1024U;          // Fiber::DefaultStackSize()
constexpr std::size_t kThreadStack = 8U * 1024U * 1024U;     // 贴近 Linux 默认
constexpr std::size_t kProbeCeiling = 200000U;               // 深度搜索上界

// ============================================================ 公共工具

/** @brief 一次隔离探深的结局。 */
enum class Outcome { kOk, kCrash, kOther };

/// 结局的中文描述（输出用）。
const char* describe(Outcome outcome) {
    switch (outcome) {
        case Outcome::kOk:
            return "正常返回";
        case Outcome::kCrash:
            return "被信号杀死";
        case Outcome::kOther:
            return "其它结局";
    }
    return "未知";
}

/**
 * @brief 关闭 core dump。
 * @details 本文件会**故意**让子进程爆栈，不应该在构建目录里留下 core 文件。
 */
void disable_core_dumps() noexcept {
    struct rlimit limit;
    limit.rlim_cur = 0;
    limit.rlim_max = 0;
    (void)::setrlimit(RLIMIT_CORE, &limit);
}

/**
 * @brief 在独立子进程里执行 body，用 waitpid 的终止状态判断结局。
 * @details 两个必须的细节：
 *          ① fork **之前** `fflush(nullptr)`：否则子进程会继承父进程尚未写出的
 *             stdio 缓冲，在重定向或退出时把它再写一遍，表现为父进程的输出
 *             "重复了一次"。这一条是实测踩出来的。
 *          ② 子进程用 `dup2` 指向 /dev/null 而不是 `freopen`：不触碰继承来的
 *             stdio 缓冲状态；结束用 `_exit()` 而不是 `exit()`。
 */
template <typename Body>
Outcome run_isolated(Body&& body) {
    std::fflush(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        return Outcome::kOther;
    }
    if (pid == 0) {
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            (void)::dup2(devnull, STDOUT_FILENO);
            (void)::dup2(devnull, STDERR_FILENO);
            (void)::close(devnull);
        }
        body();
        ::_exit(0);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return Outcome::kOther;
    }
    if (WIFSIGNALED(status)) {
        return Outcome::kCrash;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return Outcome::kOk;
    }
    return Outcome::kOther;
}

/// 在 128 KiB 的 Fiber 栈上执行 body，返回子进程结局。
template <typename Body>
Outcome run_in_fiber(Body&& body) {
    return run_isolated([&] {
        go2cpp::Fiber fiber(std::forward<Body>(body), kDefaultStack);
        (void)fiber.resume();
    });
}

/// 生成深度为 depth 的嵌套 JSON（数组风格或对象风格）。
std::string nested_json(std::size_t depth, bool object_style) {
    std::string text;
    if (object_style) {
        text.reserve(depth * 6U + 1U);
        for (std::size_t i = 0; i < depth; ++i) {
            text += "{\"a\":";
        }
        text += '1';
        for (std::size_t i = 0; i < depth; ++i) {
            text += '}';
        }
    } else {
        text.reserve(depth * 2U);
        text.assign(depth, '[');
        text.append(depth, ']');
    }
    return text;
}

/**
 * @brief 二分搜索"刚好不崩"的最大深度。
 * @details 先用指数扩张找到第一个崩溃点，再在区间内二分。
 *          若连 depth=1 都崩则返回 0；若到搜索上界仍未崩则返回已确认的安全值。
 */
template <typename Probe>
std::size_t find_critical_depth(Probe&& probe) {
    if (probe(1) != Outcome::kOk) {
        return 0;
    }
    std::size_t lo = 1;  // 已知安全
    std::size_t hi = 2;  // 未知
    while (hi <= kProbeCeiling && probe(hi) == Outcome::kOk) {
        lo = hi;
        hi *= 2;
    }
    if (hi > kProbeCeiling) {
        return lo;
    }
    while (lo + 1 < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (probe(mid) == Outcome::kOk) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// ============================================================ 场景 1：探深

/**
 * @brief 吃栈递归：每层用 alloca 申请 frame_bytes 字节并写入。
 * @details noinline 是必须的 —— 否则编译器可能把递归折叠成循环，
 *          那样就不消耗栈，测出的深度会虚高一个数量级。
 */
__attribute__((noinline)) std::uint64_t eat_stack(std::size_t depth,
                                                  std::size_t frame_bytes) {
    volatile char* frame = static_cast<volatile char*>(::alloca(frame_bytes + 64U));
    frame[0] = static_cast<char>(depth & 0x7F);
    frame[frame_bytes] = static_cast<char>((depth >> 7) & 0x7F);
    if (depth == 0) {
        return static_cast<std::uint64_t>(frame[0]);
    }
    return eat_stack(depth - 1, frame_bytes) + static_cast<std::uint64_t>(frame[0]);
}

/// 在指定容量的 Fiber 栈上递归 depth 层。
Outcome probe_fiber(std::size_t stack_size, std::size_t frame_bytes,
                    std::size_t depth) {
    return run_isolated([&] {
        go2cpp::Fiber fiber([&] { g_sink += eat_stack(depth, frame_bytes); },
                            stack_size);
        (void)fiber.resume();
    });
}

/// 在 pthread（显式指定栈大小）上递归 depth 层。
Outcome probe_pthread(std::size_t stack_size, std::size_t frame_bytes,
                      std::size_t depth) {
    return run_isolated([&] {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        const std::size_t minimum_stack =
            static_cast<std::size_t>(PTHREAD_STACK_MIN);
        pthread_attr_setstacksize(&attr, stack_size < minimum_stack
                                             ? minimum_stack
                                             : stack_size);
        auto trampoline = [](void* arg) -> void* {
            auto* payload = static_cast<std::pair<std::size_t, std::size_t>*>(arg);
            g_sink += eat_stack(payload->first, payload->second);
            return nullptr;
        };
        std::pair<std::size_t, std::size_t> payload{depth, frame_bytes};
        pthread_t tid;
        if (pthread_create(&tid, &attr, trampoline, &payload) != 0) {
            pthread_attr_destroy(&attr);
            return;
        }
        pthread_join(tid, nullptr);
        pthread_attr_destroy(&attr);
    });
}

bool scenario_probe_depth() {
    std::clog << "[full][场景1] 探深：fork 隔离下二分搜索临界调用深度\n";

    const std::size_t fiber_none =
        find_critical_depth([&](std::size_t d) { return probe_fiber(kDefaultStack, 0, d); });
    const std::size_t fiber_256 =
        find_critical_depth([&](std::size_t d) { return probe_fiber(kDefaultStack, 256, d); });
    const std::size_t thread_none =
        find_critical_depth([&](std::size_t d) { return probe_pthread(kThreadStack, 0, d); });

    const double per_depth_fiber =
        fiber_none == 0 ? 0.0 : static_cast<double>(kDefaultStack) / static_cast<double>(fiber_none);
    const double per_depth_thread =
        thread_none == 0 ? 0.0 : static_cast<double>(kThreadStack) / static_cast<double>(thread_none);
    const double utilization =
        fiber_none == 0 ? 0.0 : per_depth_fiber * static_cast<double>(fiber_none) / static_cast<double>(kDefaultStack);

    // 断言刻意取得宽松：无优化构建的每帧字节数明显大于 Release，
    // 因此这里验证的是**关系与量级**，不是某个具体数字。
    const bool frame_monotone = fiber_256 < fiber_none;             // 帧更大 → 更浅
    const bool fiber_reasonable = fiber_none >= 200 && fiber_none <= 1400;
    const bool thread_much_deeper = thread_none > fiber_none * 20;  // 栈容量比 64 倍
    const bool ok = frame_monotone && fiber_reasonable && thread_much_deeper &&
                    fiber_none > 0 && thread_none > 0;

    std::cout << "  128 KiB Fiber，帧 0     → " << fiber_none << " 层（每层 ≈ "
              << per_depth_fiber << " B，栈利用率 " << utilization << "）\n"
              << "  128 KiB Fiber，帧 256 B → " << fiber_256 << " 层\n"
              << "    8 MiB pthread，帧 0   → " << thread_none << " 层（每层 ≈ "
              << per_depth_thread << " B）\n"
              << "  关系校验：帧越大越浅=" << frame_monotone
              << " 线程栈远深于 Fiber=" << thread_much_deeper
              << "（Release 基准实测：1022 / 340 / 65497）\n";
    return ok;
}

// ================================================== 场景 2：递归改迭代

/// 尾递归求和：深度直接受栈限制。
__attribute__((noinline)) std::uint64_t sum_recursive(std::size_t n) {
    if (n == 0) {
        return 0;
    }
    return n + sum_recursive(n - 1);
}

/// 迭代求和：栈占用恒定为常数，深度只受时间与累加器宽度限制。
std::uint64_t sum_iterative(std::size_t n) {
    std::uint64_t acc = 0;
    for (std::size_t i = 1; i <= n; ++i) {
        acc += i;
    }
    return acc;
}

bool scenario_recursion_to_iteration() {
    std::clog << "[full][场景2] 递归改迭代：栈占用变成常数\n";

    // 递归版只能在安全深度内跑；取 400 层，远低于 128 KiB 栈的临界值。
    constexpr std::size_t kShallow = 400;
    constexpr std::size_t kDeep = 20000000;

    const std::uint64_t reference = sum_iterative(kShallow);
    const bool equivalent = sum_recursive(kShallow) == reference;
    const std::uint64_t deep_value = sum_iterative(kDeep);
    const bool formula_ok =
        deep_value == static_cast<std::uint64_t>(kDeep) * (kDeep + 1) / 2;

    const bool ok = equivalent && formula_ok;
    std::cout << "  两者在 " << kShallow << " 项上结果一致=" << equivalent << "\n"
              << "  迭代 " << kDeep << " 项 = " << deep_value
              << "，公式校验=" << formula_ok
              << "（递归版在同一栈上只能到约 10^3 层，预期 1 1）\n";
    return ok;
}

// ================================================== 场景 3：换 Fiber 续跑

/**
 * @brief ★ 换 Fiber 续跑：把「函数调用深度」转成「Fiber 嵌套深度」。
 * @details 每递归到 300 层就创建一个新 Fiber，在其中继续递归。
 *          新 Fiber 拿到**一块全新的栈**，所以总深度不再受单片栈限制。
 *
 *          阈值取 300 而不是压在临界值附近：本函数每帧含 64 字节局部数组，
 *          帧更大、临界更浅，必须留足余量（示例要在任何构建类型下都稳定）。
 *
 * @param depth 剩余层数。
 * @param switches 出参：换栈次数。
 * @param max_nesting 出参：观测到的最深 Fiber 嵌套层数。
 */
std::uint64_t recursive_with_new_fiber(std::size_t depth, std::size_t* switches,
                                       std::size_t* max_nesting) {
    if (depth == 0) {
        return 0;
    }
    if (depth % 300U == 0) {
        ++(*switches);
        std::uint64_t inner_result = 0;
        go2cpp::Fiber inner(
            [&] {
                if (const go2cpp::Fiber* current = go2cpp::Fiber::Current()) {
                    const std::size_t nesting = current->nesting_depth();
                    if (nesting > *max_nesting) {
                        *max_nesting = nesting;
                    }
                }
                inner_result = recursive_with_new_fiber(depth - 1, switches, max_nesting);
            },
            kDefaultStack);
        const bool accepted = inner.resume();  // 父 Fiber 在此阻塞
        if (!accepted || inner.state() != go2cpp::FiberState::Completed) {
            return 0;
        }
        volatile char pad[64];
        pad[0] = static_cast<char>(depth & 0x7F);
        return inner_result + static_cast<std::uint64_t>(pad[0]);
    }
    volatile char pad[64];
    pad[0] = static_cast<char>(depth & 0x7F);
    const std::uint64_t below = recursive_with_new_fiber(depth - 1, switches, max_nesting);
    return below + static_cast<std::uint64_t>(pad[0]);
}

bool scenario_switch_fiber() {
    std::clog << "[full][场景3] ★ 换 Fiber 续跑：突破单片栈的上限\n";

    constexpr std::size_t kTarget = 4000;  // 远超单栈上限（约 10^3）
    std::size_t switches = 0;
    std::size_t max_nesting = 0;
    std::uint64_t result = 0;

    const auto started = Clock::now();
    go2cpp::Fiber outer(
        [&] { result = recursive_with_new_fiber(kTarget, &switches, &max_nesting); },
        kDefaultStack);
    const bool accepted = outer.resume();
    const bool completed = accepted && outer.state() == go2cpp::FiberState::Completed;
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();

    const std::size_t expected_switches = kTarget / 300U;
    const bool switches_ok = switches == expected_switches;
    const bool nesting_ok = max_nesting == switches + 1;  // 首层 1 + 每次换栈 +1
    const bool ok = completed && switches_ok && nesting_ok && result != 0;

    std::cout << "  目标深度=" << kTarget << "（单片 128 KiB 栈上限约 10^3，必须换栈）\n"
              << "  换栈次数=" << switches << "（预期 " << expected_switches << "）"
              << " 最深嵌套=" << max_nesting << "（预期 " << expected_switches + 1 << "）\n"
              << "  完成=" << completed << " 累加结果=" << result
              << " 总耗时=" << elapsed_us << " µs（Debug 计时仅供参考）\n"
              << "  ★ 代价：父 Fiber 阻塞在 resume（非并行）；嵌套 " << max_nesting
              << " 层 ≈ " << (max_nesting * kDefaultStack) / 1024U
              << " KiB 栈内存；每层都有真实创建开销。\n";
    return ok;
}

// ==================================================== 场景 4：显式限深

bool scenario_bounded_depth() {
    std::clog << "[full][场景4] 显式限深：把不可恢复的崩溃换成可处理的错误\n";

    std::string deep_1000(1000, '[');
    deep_1000.append(1000, ']');

    // ① 超限：必须是"深度错误"，且能用身份比较把它与其它错误区分开
    const go2cpp::JsonParseResult over = go2cpp::ParseJson(deep_1000, 64);
    const bool over_is_depth =
        go2cpp::Is(over.error, go2cpp::JsonDepthExceededError());

    // ② 语法错误：普通输入错误，不能与深度错误混淆
    const go2cpp::JsonParseResult syntax = go2cpp::ParseJson("{\"a\":}");
    const bool syntax_is_syntax =
        !syntax.ok() &&
        !go2cpp::Is(syntax.error, go2cpp::JsonDepthExceededError());

    // ③ 同一个文本，把上限提高后就能解析（说明拒绝来自栅栏而不是解析器）
    const go2cpp::JsonParseResult relaxed = go2cpp::ParseJson("[[[[[1]]]]]", 8);
    const bool relaxed_ok = relaxed.ok();

    const bool ok = !over.ok() && over_is_depth && syntax_is_syntax && relaxed_ok;
    std::cout << "  1000 层 + 上限 64 → 被拒=" << !over.ok()
              << "，且身份判定为深度错误=" << over_is_depth << "\n"
              << "  非法语法 → 被拒=" << !syntax.ok()
              << "，且不是深度错误=" << syntax_is_syntax << "\n"
              << "  5 层 + 上限 8 → 放行=" << relaxed_ok << "（预期 1 1 1）\n"
              << "  错误文本：" << go2cpp::ErrorMessage(over.error) << "\n"
              << "  说明：越界后的崩溃点「不在解析」，而在 dump()/拷贝等递归操作上"
                 "（见场景 5 的实测）。\n";
    return ok;
}

// ============================================ 场景 5：解析与递归的代价

/// 解析 depth 层后立刻 dump()：上游序列化是递归的。
Outcome probe_dump(std::size_t depth) {
    const std::string text = nested_json(depth, false);
    return run_in_fiber([&] {
        const go2cpp::JsonParseResult result = go2cpp::ParseJson(text, depth);
        if (!result.ok() || !result.value.is_array()) {
            ::_exit(3);
        }
        const std::string dumped = result.value.dump();
        if (dumped.size() != text.size()) {
            ::_exit(4);
        }
    });
}

/// 解析 depth 层后拷贝一份。**注意**：nlohmann::json 的拷贝是深拷贝（递归），
/// 但实测其临界深度高于 dump（1631 vs 1162）—— dump() 需要序列化 + 字符串拼接，
/// 栈帧更大。所以「拷贝更吃栈」是个错误假设，不能作为断言依据。
Outcome probe_copy(std::size_t depth) {
    const std::string text = nested_json(depth, false);
    return run_in_fiber([&] {
        const go2cpp::JsonParseResult result = go2cpp::ParseJson(text, depth);
        if (!result.ok() || !result.value.is_array()) {
            ::_exit(3);
        }
        const go2cpp::Json copied = result.value;
        if (!copied.is_array() || copied.size() != result.value.size()) {
            ::_exit(4);
        }
    });
}

/**
 * @brief 场景 5：解析是迭代的，但"解析成功"不等于安全。
 * @details 三层事实，全部在本场景里实测：
 *          ① nlohmann 3.11.3 的 `parse()` 用显式栈（`sax_parse_internal`），
 *             在 128 KiB 栈上解析几十万层也不会爆栈；
 *          ② 但 `dump()` 是递归的 → 深文档上调一次就触碰 guard page；
 *          ③ 拷贝同样是递归的，且临界深度比 dump 更低。
 *          因此限深栅栏必须放在**入口**：崩溃点离入口很远，现场极难定位。
 */
bool scenario_parse_vs_recursion() {
    std::clog << "[full][场景5] 解析是迭代的，dump/拷贝才是递归的\n";

    constexpr std::size_t kDeep = 200000;  // 远超任何递归操作能承受的深度

    // ① 只解析（结果在子进程里析构，不 dump、不拷贝）
    const Outcome parse_only = run_isolated([&] {
        go2cpp::Fiber fiber(
            [&] {
                const std::string text = nested_json(kDeep, false);
                const go2cpp::JsonParseResult result = go2cpp::ParseJson(text, kDeep);
                if (!result.ok()) {
                    ::_exit(3);
                }
            },
            kDefaultStack);
        (void)fiber.resume();
    });

    // ② 同一份深文档上 dump()；③ 拷贝一份
    const Outcome dump_crash = probe_dump(kDeep);
    const Outcome copy_crash = probe_copy(kDeep);

    // ④ 量化两者的临界深度（fork 二分；子进程爆栈不影响父进程）
    const std::size_t dump_limit =
        find_critical_depth([&](std::size_t d) { return probe_dump(d); });
    const std::size_t copy_limit =
        find_critical_depth([&](std::size_t d) { return probe_copy(d); });

    const bool ok = parse_only == Outcome::kOk && dump_crash == Outcome::kCrash &&
                    copy_crash == Outcome::kCrash &&
                    dump_limit >= 100 && dump_limit <= 6000;

    std::cout << "  解析 " << kDeep << " 层 = " << describe(parse_only)
              << "（上游解析器用堆上的 states 显式栈，非递归）\n"
              << "  同一文档 dump() = " << describe(dump_crash) << "，拷贝 = "
              << describe(copy_crash) << "（预期都是被信号杀死）\n"
              << "  128 KiB 栈上的临界深度：dump() = " << dump_limit
              << " 层，拷贝 = " << copy_limit << " 层\n"
              << "  ★ 结论：「解析成功」不等于「安全」—— 深文档进入程序后，"
                 "任意一次 dump/拷贝都可能让进程消失，\n"
              << "    而且崩点远离入口。入口限深（默认 64）正是为此，"
                 "相对最坏实测仍留有余量。\n";
    return ok;
}

// ============================================ 场景 6：JSON 深层嵌套实战

bool scenario_json_nesting() {
    std::clog << "[full][场景6] JSON 深层嵌套：边界、随机输入与攻击载荷\n";
    bool ok = true;

    // ① 正向：真实嵌套结构解析并取值
    const go2cpp::JsonParseResult real =
        go2cpp::ParseJson("{\"a\":[1,{\"b\":[2,{\"c\":3}]}]}");
    const bool real_ok = real.ok() && real.value["a"][1]["b"][1]["c"] == 3;
    std::cout << "  真实嵌套文档解析=" << real_ok << "，取到 c="
              << (real.ok() ? real.value["a"][1]["b"][1]["c"].dump() : "n/a")
              << "（预期 1 / 3）\n";
    ok = ok && real_ok;

    // ② 边界：上限 64 时，64 层放行、65 层拒绝（边界包含性）
    const go2cpp::JsonParseResult at_limit =
        go2cpp::ParseJson(nested_json(64, false), 64);
    const go2cpp::JsonParseResult over_limit =
        go2cpp::ParseJson(nested_json(65, false), 64);
    const bool over_is_depth =
        go2cpp::Is(over_limit.error, go2cpp::JsonDepthExceededError());
    std::cout << "  边界：64 层放行=" << at_limit.ok() << " 65 层被拒="
              << !over_limit.ok() << " 且为深度错误=" << over_is_depth
              << "（预期 1 1 1）\n";
    ok = ok && at_limit.ok() && !over_limit.ok() && over_is_depth;

    // ③ 随机测试：深度 1..120 × 上限 1..100，数组/对象两种风格交替。
    //    深度上界刻意压在"拷贝临界深度"之下：这些解析发生在**主线程**
    //    （8 MiB 栈）上，但仍然不去触碰需要递归操作的深值。
    std::mt19937 rng(20261015U);
    std::uniform_int_distribution<std::size_t> depth_dist(1, 120);
    std::uniform_int_distribution<std::size_t> limit_dist(1, 100);
    int mismatches = 0;
    for (int i = 0; i < 500; ++i) {
        const std::size_t depth = depth_dist(rng);
        const std::size_t limit = limit_dist(rng);
        const go2cpp::JsonParseResult result =
            go2cpp::ParseJson(nested_json(depth, (i % 2) == 0), limit);
        const bool expect_ok = depth <= limit;
        if (result.ok() != expect_ok) {
            ++mismatches;
        }
        if (!result.ok() &&
            !go2cpp::Is(result.error, go2cpp::JsonDepthExceededError())) {
            ++mismatches;  // 被拒的原因必须是深度，而不是别的错误
        }
    }
    std::cout << "  随机 500 组（深度 1..120 × 上限 1..100，数组/对象交替）不一致="
              << mismatches << "（预期 0）\n";
    ok = ok && mismatches == 0;

    // ④ 真实攻击载荷：50000 层嵌套（约 100 KB）——栅栏在超限处立即早退，
    //    所以拒绝成本与载荷深度无关。
    const std::string attack = nested_json(50000, false);
    const auto started = Clock::now();
    const go2cpp::JsonParseResult blocked = go2cpp::ParseJson(attack);
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
    const bool blocked_ok =
        !blocked.ok() &&
        go2cpp::Is(blocked.error, go2cpp::JsonDepthExceededError());
    std::cout << "  攻击载荷 " << attack.size() << " 字节 / 50000 层 → 被拒="
              << !blocked.ok() << "，且为深度错误=" << blocked_ok
              << "，拒绝耗时=" << elapsed_us << " µs（早退，与深度无关）\n";
    ok = ok && blocked_ok;

    // ⑤ 稳定性重复：3000 次不同深度的解析，结果必须始终一致
    int stress_mismatch = 0;
    for (int i = 0; i < 3000; ++i) {
        const std::size_t depth = 1 + static_cast<std::size_t>(i % 80);
        const go2cpp::JsonParseResult result =
            go2cpp::ParseJson(nested_json(depth, (i % 3) == 0), 64);
        if (result.ok() != (depth <= 64)) {
            ++stress_mismatch;
        }
    }
    std::cout << "  稳定性重复 3000 次（深度 1..80，上限 64）不一致=" << stress_mismatch
              << "（预期 0）\n";
    ok = ok && stress_mismatch == 0;

    return ok;
}

}  // namespace

/**
 * @brief Fiber 栈上限的实测与六条工程对策。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    // 本示例测量的是「固定栈」的临界深度与爆栈语义。默认的可变长栈会把
    // 深递归 mprotect 成更大的栈，从而改变场景 1/5 的预期（临界深度从
    // 128KiB 级提升到 8MiB 级）。显式关闭增长，让本示例继续作为固定栈
    // 基线的可复现实验；子进程继承该环境变量。
    ::setenv("GO2CPP_GROWABLE_STACK", "0", 1);
    disable_core_dumps();  // 探深会故意让子进程爆栈

    bool ok = true;
    ok = scenario_probe_depth() && ok;
    ok = scenario_recursion_to_iteration() && ok;
    ok = scenario_switch_fiber() && ok;
    ok = scenario_bounded_depth() && ok;
    ok = scenario_parse_vs_recursion() && ok;
    ok = scenario_json_nesting() && ok;

    std::clog << (ok ? "[full] 6 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
