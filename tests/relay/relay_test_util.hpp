#pragma once

/**
 * @file relay_test_util.hpp
 * @brief relay 测试共享的辅助类型：可计数的假连接、计时与统计工具。
 *
 * 只被 `tests/relay/` 下的用例使用；不进入库，也不改变现有测试目标。
 */

#include "go2cpp/relay/hub.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace go2cpp::relay::test {

/**
 * @brief 最小假连接：记录 Close 调用次数，并支持「只算第一次」的关闭计数。
 *
 * 同时统计两个量：
 * - `invocations()`：**调用**次数。用于验证 Hub 对每条连接只调用一次 Close；
 * - `close_total`（构造时可选）：**首次关闭**累计，用于验证一组连接被真正关闭。
 *
 * 实现的是 `Hub::Stream`（即 `streams::Closer`）：`Close()` 返回空错误。
 */
class FakeStream final : public Hub::Stream {
public:
    explicit FakeStream(std::atomic<std::uint64_t> *close_total = nullptr)
        : m_close_total(close_total) {}

    ::go2cpp::ErrorPtr Close() override {
        m_invocations.fetch_add(1, std::memory_order_relaxed);
        bool expected = false;
        if (!m_closed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return {};
        }
        if (m_close_total != nullptr) {
            m_close_total->fetch_add(1, std::memory_order_relaxed);
        }
        return {};
    }

    /** @brief 是否已经成功关闭（幂等语义下的「已关闭」）。 */
    bool closed() const noexcept { return m_closed.load(std::memory_order_acquire); }

    /** @brief Close() 被调用的次数（包含幂等重复调用）。 */
    std::uint64_t invocations() const noexcept {
        return m_invocations.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> m_closed{false};
    std::atomic<std::uint64_t> m_invocations{0};
    std::atomic<std::uint64_t> *m_close_total;
};

/** @brief 睡指定毫秒；测试里的等待统一走这里，便于替换。 */
inline void SleepMs(std::int64_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

/** @brief 单调时钟当前的毫秒数，用于测量触发延迟。 */
inline std::int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/** @brief 把毫秒样本排序后取分位数（p 取 0..100）。 */
inline double Percentile(std::vector<double> samples, double p) {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const double index = (p / 100.0) * static_cast<double>(samples.size() - 1);
    const auto lower = static_cast<std::size_t>(index);
    const auto upper = std::min(lower + 1, samples.size() - 1);
    const double fraction = index - static_cast<double>(lower);
    return samples[lower] * (1.0 - fraction) + samples[upper] * fraction;
}

} // namespace go2cpp::relay::test
