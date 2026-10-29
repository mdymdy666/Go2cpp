/**
 * @file backoff.cpp
 * @brief 指数退避 + 抖动实现（MOD-12）。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/net/backoff.hpp"

#include <algorithm>

namespace go2cpp::bento {
namespace {

/// 2^53：double 可精确表示的最大整数范围，用于把 53 位随机数映射到 [0, 1)。
constexpr double kUnitScale = 9007199254740992.0;

}  // namespace

Backoff::Backoff() : Backoff(Config{}) {}

Backoff::Backoff(Config config)
    : m_base_ms(config.base_ms == 0 ? 1 : config.base_ms),
      m_max_ms(std::max(config.max_ms, m_base_ms)),
      m_jitter_ratio(std::min(std::max(config.jitter_ratio, 0.0), 1.0)),
      m_initial_seed(config.seed),
      m_state(config.seed) {}

std::uint32_t Backoff::NominalDelayMs(std::uint32_t attempts) const noexcept {
    // attempts 最多取 32：base 是 uint32，左移 32 位后仍在 uint64 内
    // （0xFFFFFFFF << 32 < 2^64），再大的指数只会被上限截断。
    // 只取到 31 会漏掉 (2^31, UINT32_MAX] 这段上限：max_ms 配成 UINT32_MAX 时
    // 永远退不到配置的上限，属于可观测的参数失效。
    const std::uint32_t shift = std::min(attempts, 32u);
    const std::uint64_t scaled = static_cast<std::uint64_t>(m_base_ms) << shift;
    const std::uint64_t capped = std::min<std::uint64_t>(scaled, m_max_ms);
    return static_cast<std::uint32_t>(capped);
}

std::uint64_t Backoff::NextRandom() noexcept {
    m_state += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = m_state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

double Backoff::NextUnit() noexcept {
    return static_cast<double>(NextRandom() >> 11) * (1.0 / kUnitScale);
}

std::uint32_t Backoff::Next() noexcept {
    const std::uint32_t nominal = NominalDelayMs(m_attempts);
    ++m_attempts;
    if (m_jitter_ratio <= 0.0 || nominal <= 1) {
        return nominal;
    }
    // 先确定抖动区间 [lower, upper] 再在区间内均匀采样，upper 受 max 截断。
    // 不用「nominal ± span 后再 min(…, max)」：那样一旦 nominal*(1+jitter) 越过
    // max，越界部分会被硬压到 max 上——到 30s 上限时约一半样本堆在 30000，
    // 多客户端会在同一时刻一起重连，抖动完全失去打散作用。
    // 区间采样保证 [max*(1-jitter), max] 均匀，随机数消耗仍是每次 1 个。
    const double nominal_d = static_cast<double>(nominal);
    const double lower = nominal_d * (1.0 - m_jitter_ratio);
    const double upper =
        std::min(nominal_d * (1.0 + m_jitter_ratio), static_cast<double>(m_max_ms));
    const double shifted = lower + NextUnit() * (upper - lower);
    if (shifted <= 1.0) {
        return 1;
    }
    return static_cast<std::uint32_t>(shifted + 0.5);
}

void Backoff::Reset() noexcept {
    m_attempts = 0;
    m_state = m_initial_seed;
}

std::uint32_t Backoff::PeekNominalMs() const noexcept {
    return NominalDelayMs(m_attempts);
}

}  // namespace go2cpp::bento
