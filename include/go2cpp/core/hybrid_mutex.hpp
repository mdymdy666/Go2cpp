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
 * @brief 面向短临界区的原子自旋与互斥锁混合门。
 * @details 先在原子标志上短暂自旋，竞争持续时进入 std::mutex 慢路径，
 *          避免 Fiber 热路径频繁进入内核等待；持锁期间不得执行阻塞 I/O。
 * @note 依赖 C++ 原子操作和 std::mutex；向上层提供 BasicLockable 接口。
 */
class HybridGate final {
public:
    /** @brief 创建未加锁的混合门。 */
    HybridGate() noexcept = default;
    HybridGate(const HybridGate&) = delete;
    HybridGate& operator=(const HybridGate&) = delete;

    /**
     * @brief 获取门锁，竞争时先自旋再阻塞等待。
     * @note 调用方必须保证成对调用 unlock()，不能递归加锁。
     */
    void lock() noexcept {
        for (std::size_t attempt = 0; attempt < kFastAttempts; ++attempt) {
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
    static constexpr std::size_t kFastAttempts = 64;

    static void RelaxCpu() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        _mm_pause();
#else
        std::this_thread::yield();
#endif
    }

    std::atomic_flag m_fast = ATOMIC_FLAG_INIT;
    std::mutex m_slow;
};

}  // namespace go2cpp::core::detail

namespace go2cpp::core {
using HybridMutex = detail::HybridGate;
}  // namespace go2cpp::core
