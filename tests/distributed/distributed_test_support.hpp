/**
 * @file distributed_test_support.hpp
 * @brief 分布式原语测试共用的小工具：等待、信号、随机种子与默认配置。
 *
 * 设计与 `tests/relay/relay_test_util.hpp` 相同：只放头文件工具，不定义
 * main；每个测试可执行文件自己组织用例并复用 `tests/test_support.hpp`
 * 的 `GO2CPP_CHECK` 失败计数。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#pragma once

#include "go2cpp/distributed/coordinator.hpp"
#include "test_support.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>

namespace go2cpp::distributed::test {

using namespace std::chrono_literals;

inline void SleepMs(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

/**
 * @brief 在超时内轮询条件。
 * @return 条件成立返回 true；超时返回最后一次求值结果。
 */
template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout,
               std::chrono::milliseconds interval = 2ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(interval);
    }
    return predicate();
}

/// 测试用协调者配置：5ms 清扫，允许 10ms~10s 的租约。
inline CoordinatorOptions MakeOptions(std::chrono::milliseconds sweep = 5ms) {
    CoordinatorOptions options;
    options.sweep_interval = sweep;
    options.min_ttl = 10ms;
    options.max_ttl = 10s;
    options.start_sweeper = true;
    return options;
}

/// 一次性信号：后台线程 `Set()`，主线程 `WaitFor()`。
class Signal final {
public:
    void Set() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_set = true;
        }
        m_cv.notify_all();
    }

    bool WaitFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [this] { return m_set; });
    }

    bool set() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_set;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_set{false};
};

/// 每个测试用例重建：固定种子，失败可复现。
class Rng final {
public:
    explicit Rng(std::uint64_t seed) : m_engine(seed) {}

    std::uint64_t Next() { return m_distribution(m_engine); }
    std::uint64_t Below(std::uint64_t bound) { return bound == 0 ? 0 : Next() % bound; }
    bool Chance(std::uint32_t percent) { return Below(100) < percent; }

private:
    std::mt19937_64 m_engine;
    std::uniform_int_distribution<std::uint64_t> m_distribution;
};

/// 生成 `prefix-<number>` 形式的实例/节点名。
inline std::string MakeName(const std::string &prefix, std::uint64_t index) {
    return prefix + "-" + std::to_string(index);
}

} // namespace go2cpp::distributed::test
