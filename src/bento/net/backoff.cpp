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
    // attempts 最多取 31：base 是 uint32，左移 31 位后仍能放进 uint64，
    // 更大的指数没有意义（结果早已被上限截断）。
    const std::uint32_t shift = std::min(attempts, 31u);
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
    // 均匀抖动 symmetric：[-span, +span)。用 double 计算后再取整，
    // 误差不超过 1ms，远小于调用方对退避精度的需求。
    const double span = static_cast<double>(nominal) * m_jitter_ratio;
    const double offset = (NextUnit() * 2.0 - 1.0) * span;
    const double shifted = static_cast<double>(nominal) + offset;
    if (shifted <= 1.0) {
        return 1;
    }
    const double capped = std::min(shifted, static_cast<double>(m_max_ms));
    return static_cast<std::uint32_t>(capped + 0.5);
}

void Backoff::Reset() noexcept {
    m_attempts = 0;
    m_state = m_initial_seed;
}

std::uint32_t Backoff::PeekNominalMs() const noexcept {
    return NominalDelayMs(m_attempts);
}

}  // namespace go2cpp::bento
