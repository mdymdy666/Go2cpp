/**
 * @file hybrid_mutex.hpp
 * @brief 短临界区的"原子自旋 + 互斥锁"混合门。
 *
 * ## 模块定位
 *
 * 这是运行时里**性能最敏感、但适用范围极窄**的一把锁：它只用来保护
 * Fiber 唤醒/撤销这类**几十纳秒级**的临界区（例如把 G 从等待队列摘下来
 * 再标记为可运行）。
 *
 * ## 为什么需要它
 *
 * 这类临界区如果直接用 `std::mutex`，即使无竞争也要付一次 futex 快速路径
 * 的代价；而在调度器热路径上这个代价会被放大很多倍。先用原子标志自旋
 * `fast_attempts` 次（默认 64），绝大多数情况下根本进不到内核。
 *
 * ## 使用限制（重要）
 *
 * **不得**用于保护可能阻塞、执行用户回调、或执行系统调用的代码。
 * 原因：自旋阶段会占着 CPU 空转，临界区一旦变长就会把"省下的一次 futex"
 * 变成"烧掉一整段 CPU"，反而更慢 —— 这个模块历史上就有过因为盲目加自旋
 * 导致整体慢 36% 的案例。
 *
 * ## 能力清单
 *
 * - `lock / try_lock / unlock`：满足 BasicLockable，可与 `std::lock_guard` 等配合；
 * - `HybridGateOptions::fast_attempts`：热路径自旋次数，只影响竞争策略，
 *   不改变所有权语义。
 *
 * ## 与 `sync::Mutex` 的区别
 *
 * `sync::Mutex` 是给**用户代码**用的、语义完整的互斥量（支持超时、可重入
 * 等）；`HybridGate` 是**内部**工具，只为极短临界区服务，不要拿它替换业务
 * 代码里的锁。
 */
#pragma once

// 轻量混合锁：先在原子标志上做有限次自旋，竞争持续时再由普通
// std::mutex 把慢路径串行化。它只用于非常短的 Fiber 唤醒/撤销临界区，
// 不得保护可能阻塞、执行用户回调或执行系统调用的代码。

#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace go2cpp::core::detail {

/**
 * @brief HybridGate 热路径策略。
 * @details fast_attempts 控制原子自旋次数；超过该次数后进入 std::mutex
 *          慢路径。参数只影响锁竞争策略，不改变锁的所有权语义。
 */
struct HybridGateOptions {
    std::size_t fast_attempts{64};
};

/**
 * @brief 面向短临界区的原子自旋与互斥锁混合门。
 * @details 先在原子标志上短暂自旋，竞争持续时进入 std::mutex 慢路径，
 *          避免 Fiber 热路径频繁进入内核等待；持锁期间不得执行阻塞 I/O。
 * @note 依赖 C++ 原子操作和 std::mutex；向上层提供 BasicLockable 接口。
 */
class HybridGate final {
public:
    /** @brief 创建未加锁且使用当前默认策略的混合门。 */
    HybridGate() noexcept : m_options(DefaultOptions()) {}
    /** @brief 使用指定热路径策略创建混合门。 */
    explicit HybridGate(HybridGateOptions options) noexcept
        : m_options(Normalize(options)) {}
    HybridGate(const HybridGate&) = delete;
    HybridGate& operator=(const HybridGate&) = delete;

    /**
     * @brief 设置后续新建混合门使用的默认策略。
     * @param options 新策略；fast_attempts 为 0 时自动修正为 1。
     * @note 该配置不会修改已经构造的门，也不需要停止调度器。
     */
    static void SetDefaultOptions(HybridGateOptions options) noexcept {
        s_default_fast_attempts.store(
            Normalize(options).fast_attempts, std::memory_order_release);
    }

    /** @brief 获取新建混合门所使用的当前默认策略。 */
    static HybridGateOptions DefaultOptions() noexcept {
        return HybridGateOptions{
            s_default_fast_attempts.load(std::memory_order_acquire)};
    }

    /**
     * @brief 获取门锁，竞争时先自旋再阻塞等待。
     * @note 调用方必须保证成对调用 unlock()，不能递归加锁。
     */
    void lock() noexcept {
        for (std::size_t attempt = 0; attempt < m_options.fast_attempts;
             ++attempt) {
            if (!m_fast.test_and_set(std::memory_order_acquire)) {
                return;
            }
            RelaxCpu();
        }

        // 只有竞争者进入这里。慢路径锁不代表业务所有权；它只保证同一
        // 时间只有一个线程等待原子标志，避免多个失败者同时忙等。
        m_slow.lock();
        for (;;) {
            if (!m_fast.test_and_set(std::memory_order_acquire)) {
                m_slow.unlock();
                return;
            }
            RelaxCpu();
        }
    }

    /** @brief 释放门锁并唤醒慢路径竞争者。 */
    void unlock() noexcept { m_fast.clear(std::memory_order_release); }

private:
    static HybridGateOptions Normalize(HybridGateOptions options) noexcept {
        if (options.fast_attempts == 0) {
            options.fast_attempts = 1;
        }
        return options;
    }

    static void RelaxCpu() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        _mm_pause();
#else
        std::this_thread::yield();
#endif
    }

    std::atomic_flag m_fast = ATOMIC_FLAG_INIT;
    std::mutex m_slow;
    HybridGateOptions m_options;
    inline static std::atomic<std::size_t> s_default_fast_attempts{64};
};

}  // namespace go2cpp::core::detail

namespace go2cpp::core {
using HybridMutex = detail::HybridGate;
using HybridMutexOptions = detail::HybridGateOptions;
}  // namespace go2cpp::core
