/**
 * @file node_id.cpp
 * @brief `NodeId` 的进程级唯一性来源：原子计数器 + 一次性随机种子。
 *
 * 为什么不是纯 header-only：`Generate()` 的「进程内唯一」依赖**全进程共享
 * 一个计数器**。若把 `inline` 计数器放头文件，每个翻译单元各有一份，两个
 * 模块生成同一个序号就会撞低 64 位；因此产生点必须落在唯一的 .cpp 里。
 * 其余接口（Parse/ToString/比较）仍在头文件内联。
 *
 * @author go2cpp contributors
 * @date 2026-11-04
 * @copyright MIT License
 */

#include "go2cpp/distributed/node_id.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>

namespace go2cpp::distributed {
namespace {

/// @brief 当前 epoch 毫秒（system_clock 才有跨进程可比的时间原点）。
std::uint64_t NowMillis() noexcept {
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count());
}

/// @brief SplitMix64：一轮混合就有良好雪崩，无需保存状态。
std::uint64_t SplitMix64(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

/**
 * @brief 进程级随机种子；只在首次调用时读一次系统熵源。
 * @return 非 0 种子；`/dev/urandom` 不可用时退化为「时间 + 地址熵」。
 */
std::uint64_t Seed() noexcept {
    static const std::uint64_t seed = []() noexcept {
        std::uint64_t value = 0;
        try {
            std::random_device device;
            value = (static_cast<std::uint64_t>(device()) << 32) ^ device();
        } catch (...) {
            value = 0;
        }
        // 即使 random_device 失败也必须非 0：混入时间与栈地址，保证不同
        // 进程/不同启动时刻至少有一个变化的熵源。
        value ^= NowMillis() * 0xD6E8FEB86659FD93ULL;
        value ^= reinterpret_cast<std::uintptr_t>(&value) * 0x9E3779B97F4A7C15ULL;
        return value == 0 ? 0xA5A5A5A5A5A5A5A5ULL : value;
    }();
    return seed;
}

/// @brief 全进程唯一的序号源；进程内所有 `Generate()` 调用共享。
std::atomic<std::uint64_t>& Counter() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

/// @brief 十六进制字符转 0..15；非法返回 16。
int HexValue(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return 16;
}

} // namespace

NodeId NodeId::Generate() noexcept {
    // step1: 序号在进程内严格递增；不同调用绝不会拿到同一个序号。
    const std::uint64_t sequence = Counter().fetch_add(1, std::memory_order_relaxed);
    // step2: 低 64 位 = 毫秒时间（高位）+ 序号（低 32 位，回绕周期 42 亿次）。
    //        即使同一毫秒内生成，序号也保证低半区唯一。
    const std::uint64_t low = (NowMillis() << 32) | (sequence & 0xffffffffULL);
    // step3: 高 64 位由进程级种子与序号混合；不同进程种子不同，跨进程也不同。
    const std::uint64_t high = SplitMix64(Seed() + sequence * 0x9E3779B97F4A7C15ULL);
    return NodeId::FromParts(high, low);
}

Result<NodeId> NodeId::ParseChecked(std::string_view text) noexcept {
    NodeId id;
    if (!TryParse(text, &id)) {
        // 消息用 SSO 短串：noexcept 路径上不引入分配失败点。
        return Result<NodeId>::Failure(Status::kInvalidArgument, "bad node id");
    }
    return Result<NodeId>::Success(id);
}

NodeId NodeId::Parse(std::string_view text) noexcept {
    NodeId id;
    if (TryParse(text, &id)) {
        return id;
    }
    return NodeId{};
}

bool NodeId::TryParse(std::string_view text, NodeId* out) noexcept {
    if (out == nullptr || text.empty() || text.size() != kTextLength) {
        return false;
    }
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const int value = HexValue(text[index]);
        if (value >= 16) {
            return false;
        }
        if (index < 16) {
            high = (high << 4) | static_cast<std::uint64_t>(value);
        } else {
            low = (low << 4) | static_cast<std::uint64_t>(value);
        }
    }
    *out = NodeId::FromParts(high, low);
    return true;
}

std::string NodeId::ToString() const {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text(kTextLength, '0');
    std::uint64_t high = m_high;
    std::uint64_t low = m_low;
    for (std::size_t index = 16; index > 0; --index) {
        text[index - 1] = kDigits[high & 0xFULL];
        high >>= 4;
    }
    for (std::size_t index = 32; index > 16; --index) {
        text[index - 1] = kDigits[low & 0xFULL];
        low >>= 4;
    }
    return text;
}

} // namespace go2cpp::distributed
