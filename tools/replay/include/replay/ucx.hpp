#pragma once

/**
 * @file ucx.hpp
 * @brief 单线程有栈协程（`ucontext`），只服务于 replay 引擎。
 *
 * 为什么不用 `go2cpp::Fiber`：`Fiber` 的 resume/yield 由 `Scheduler` 的 M
 * 拥有，手动 resume 会与调度器的 GMP 状态机冲突。replay 需要的是**完全
 * 由记录驱动**的单线程切换，因此这里用 `ucontext` 做一个 ~50 行的最小
 * 实现，与核心库零耦合，也不改动任何默认构建行为。
 *
 * 不变量：
 * - 只有引擎线程调用 `Resume`；fiber 只能通过 `Park`/`ExitCurrent` 回到引擎；
 * - `state == kDone` 的 fiber 不再被 resume；
 * - **任何跨 `swapcontext` 保存的 `Fiber&` 都必须在切回后重新获取**：
 *   `m_fibers` 是 `std::vector`，创建新 fiber 会扩容并把旧元素搬到新地址，
 *   悬空引用会让 `state = kDone` 写进已释放内存（实测：重放中途"完成"的
 *   fiber 永远不会被标记 done）。
 */

#include <cstdint>
#include <functional>
#include <string>
#include <ucontext.h>
#include <utility>
#include <vector>

namespace go2cpp::replay {

/**
 * @brief 最小单线程协程调度器。
 *
 * 线程模型：**非线程安全**，只允许创建它的线程使用（replay 是单线程的）。
 */
class UcontextScheduler {
public:
    enum class State { kNew, kRunning, kParked, kDone };

    static constexpr std::size_t kStackSize = 512U * 1024U;
    static constexpr std::uint32_t kNoFiber = 0xFFFFFFFFU;

    UcontextScheduler() = default;
    ~UcontextScheduler() = default;

    UcontextScheduler(const UcontextScheduler&) = delete;
    UcontextScheduler& operator=(const UcontextScheduler&) = delete;

    /// 创建一个 fiber，返回其内部 id（从 0 起严格递增）。
    std::uint32_t Create(std::function<void()> entry) {
        Fiber fiber;
        fiber.entry = std::move(entry);
        fiber.stack.resize(kStackSize);
        fiber.id = static_cast<std::uint32_t>(m_fibers.size());
        if (getcontext(&fiber.context) != 0) {
            m_error = "getcontext failed";
            return kNoFiber;
        }
        fiber.context.uc_stack.ss_sp = fiber.stack.data();
        fiber.context.uc_stack.ss_size = fiber.stack.size();
        fiber.context.uc_link = &m_engine;
        makecontext(&fiber.context, Trampoline, 0);
        m_fibers.push_back(std::move(fiber));
        return static_cast<std::uint32_t>(m_fibers.size() - 1U);
    }

    /**
     * @brief 切到 `fid`，直到它 park 或结束。
     *
     * 返回后 `state(fid)` 为 `kParked` 或 `kDone`；重复 resume 已结束的
     * fiber 是空操作。
     */
    void Resume(std::uint32_t fid) {
        if (fid >= m_fibers.size()) {
            m_error = "resume invalid fiber id";
            return;
        }
        if (m_fibers[fid].state == State::kDone) {
            return;
        }
        UcontextScheduler* const previous = s_current;
        s_current = this;
        m_current = fid;
        m_fibers[fid].state = State::kRunning;
        swapcontext(&m_engine, &m_fibers[fid].context);
        m_current = kNoFiber;
        s_current = previous;
    }

    /// fiber 侧：让出 CPU 并标记为 parked。
    void Park() {
        if (m_current >= m_fibers.size()) {
            return;
        }
        const std::uint32_t fid = m_current;
        m_fibers[fid].state = State::kParked;
        swapcontext(&m_fibers[fid].context, &m_engine);
        // 被唤醒后 Resume 已把 state 置回 kRunning；此处不再访问成员引用。
    }

    /// fiber 侧：提前结束（错误路径；不展开栈，fiber 内对象不会析构）。
    void ExitCurrent() {
        if (m_current >= m_fibers.size()) {
            return;
        }
        const std::uint32_t fid = m_current;
        m_fibers[fid].state = State::kDone;
        swapcontext(&m_fibers[fid].context, &m_engine);
    }

    State state(std::uint32_t fid) const {
        if (fid >= m_fibers.size()) {
            return State::kDone;
        }
        return m_fibers[fid].state;
    }

    bool done(std::uint32_t fid) const { return state(fid) == State::kDone; }

    std::uint32_t current() const noexcept { return m_current; }
    std::size_t size() const noexcept { return m_fibers.size(); }
    const std::string& error() const noexcept { return m_error; }

private:
    struct Fiber {
        ucontext_t context{};
        std::vector<char> stack;
        std::function<void()> entry;
        State state{State::kNew};
        std::uint32_t id{0};
    };

    /// `makecontext` 的入口：调用用户函数，结束后标记 done（`uc_link` 回引擎）。
    ///
    /// 注意：**不能在 entry 前后跨 swapcontext 持有 `Fiber&`** —— 等待期间
    /// 引擎可能创建新 fiber 导致 vector 扩容。
    static void Trampoline() {
        UcontextScheduler* const self = s_current;
        if (self == nullptr) {
            return;
        }
        const std::uint32_t fid = self->m_current;
        if (fid >= self->m_fibers.size()) {
            return;
        }
        try {
            if (self->m_fibers[fid].entry) {
                self->m_fibers[fid].entry();
            }
        } catch (...) {
            // replay 的 fiber 不应抛异常；吞掉后按结束处理，错误由引擎报告。
        }
        if (fid < self->m_fibers.size()) {
            self->m_fibers[fid].state = State::kDone;
        }
    }

    ucontext_t m_engine{};
    std::vector<Fiber> m_fibers;
    std::uint32_t m_current{kNoFiber};
    std::string m_error;
    static thread_local UcontextScheduler* s_current;
};

inline thread_local UcontextScheduler* UcontextScheduler::s_current = nullptr;

}  // namespace go2cpp::replay
