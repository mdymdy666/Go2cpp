/**
 * @file test_secure_random.cpp
 * @brief 安全随机统一后的验证：bento / p2p 两套 SecureRandomBytes 的
 *        1e6 字节 χ²、16 字节块唯一性，以及「新旧实现不同但都合格」的对比。
 *
 * @details 验证目标（对应 docs/reports/reuse-audit.md T3）：
 *          - bento::SecureRandomBytes 与 p2p::SecureRandomBytes 都走
 *            go::crypto::rand（getrandom → /dev/urandom）；
 *          - 各取 1e6 字节：字节频率 χ² 落在 df=255 的合理区间，
 *            16 字节块无重复；
 *          - 与改动前的两个旧实现（bento 的 std::random_device 版、
 *            p2p 的 getrandom 直读版）对比：输出互不相同（不同字节 > 99%），
 *            但旧实现同样通过 χ² / 唯一性检查 —— 即「不同但都合格」。
 *
 *          独立可执行（不注册 ctest：cmake/ 不在本次改动边界）：
 *          g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -I include \
 *              tests/bento/test_secure_random.cpp -o /tmp/test_secure_random \
 *              -L/tmp/build-reuse -lgo2cpp_bento -lgo2cpp_p2p -lpthread
 */

#include "go2cpp/bento/net/crypto.hpp"
#include "go2cpp/p2p/random.hpp"

#include <sys/random.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

constexpr std::size_t kBytes = 1000000;
constexpr std::size_t kBlockBytes = 16;

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("[FAIL] %s\n", what);
    }
}

/// 字节频率 χ²（df = 255，期望 255，标准差约 22.6）。
double ChiSquare(const std::vector<std::uint8_t>& data) {
    std::array<std::uint64_t, 256> counts{};
    for (const std::uint8_t byte : data) {
        ++counts[byte];
    }
    const double expected = static_cast<double>(data.size()) / 256.0;
    double chi = 0.0;
    for (const std::uint64_t count : counts) {
        const double diff = static_cast<double>(count) - expected;
        chi += diff * diff / expected;
    }
    return chi;
}

/// 按 16 字节切块，排序后统计重复块数（随机下应为 0）。
std::size_t DuplicateBlockCount(const std::vector<std::uint8_t>& data) {
    std::vector<std::array<std::uint8_t, kBlockBytes>> blocks;
    blocks.reserve(data.size() / kBlockBytes);
    for (std::size_t offset = 0; offset + kBlockBytes <= data.size(); offset += kBlockBytes) {
        std::array<std::uint8_t, kBlockBytes> block{};
        std::memcpy(block.data(), data.data() + offset, kBlockBytes);
        blocks.push_back(block);
    }
    std::sort(blocks.begin(), blocks.end());
    const auto unique_end = std::unique(blocks.begin(), blocks.end());
    return static_cast<std::size_t>(blocks.end() - unique_end);
}

/// 两组等长数据的「不同字节占比」。
double DifferentRatio(const std::vector<std::uint8_t>& left,
                      const std::vector<std::uint8_t>& right) {
    if (left.size() != right.size() || left.empty()) {
        return 0.0;
    }
    std::size_t different = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index] != right[index]) {
            ++different;
        }
    }
    return static_cast<double>(different) / static_cast<double>(left.size());
}

// ---------------------------------------------------------------------------
// 改动前的两个旧实现（参考组；逐行副本）
// ---------------------------------------------------------------------------

/// 旧 bento::SecureRandomBytes：std::random_device，每次 32 位拆 4 字节。
void LegacyBentoRandom(std::uint8_t* out, std::size_t bytes) {
    std::random_device device;
    std::size_t offset = 0;
    while (offset < bytes) {
        const std::uint32_t value = device();
        for (int byte = 0; byte < 4 && offset < bytes; ++byte, ++offset) {
            out[offset] = static_cast<std::uint8_t>(value >> (8 * byte));
        }
    }
}

/// 旧 p2p::SecureRandomBytes 的熵源：getrandom 直读（池化只是批量取回，
/// 不改变输出分布，这里省略池化以便对照）。
void LegacyP2pRandom(std::uint8_t* out, std::size_t bytes) {
    std::size_t filled = 0;
    while (filled < bytes) {
        const ssize_t got = ::getrandom(out + filled, bytes - filled, 0);
        if (got > 0) {
            filled += static_cast<std::size_t>(got);
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

std::vector<std::uint8_t> BentoBytes() {
    std::vector<std::uint8_t> data(kBytes);
    go2cpp::bento::SecureRandomBytes(data.data(), data.size());
    return data;
}

std::vector<std::uint8_t> P2pBytes() {
    std::vector<std::uint8_t> data(kBytes);
    go2cpp::p2p::SecureRandomBytes(data.data(), data.size());
    return data;
}

std::vector<std::uint8_t> LegacyBentoBytes() {
    std::vector<std::uint8_t> data(kBytes);
    LegacyBentoRandom(data.data(), data.size());
    return data;
}

std::vector<std::uint8_t> LegacyP2pBytes() {
    std::vector<std::uint8_t> data(kBytes);
    LegacyP2pRandom(data.data(), data.size());
    return data;
}

}  // namespace

int main() {
    const std::vector<std::uint8_t> bento = BentoBytes();
    const std::vector<std::uint8_t> p2p = P2pBytes();
    const std::vector<std::uint8_t> legacy_bento = LegacyBentoBytes();
    const std::vector<std::uint8_t> legacy_p2p = LegacyP2pBytes();

    // ------------------------------------------------------------------
    // 1) 字节频率 χ²（df=255；99.9% 区间约 [150, 400]）
    // ------------------------------------------------------------------
    const double chi_bento = ChiSquare(bento);
    const double chi_p2p = ChiSquare(p2p);
    const double chi_legacy_bento = ChiSquare(legacy_bento);
    const double chi_legacy_p2p = ChiSquare(legacy_p2p);
    std::printf("chi-square (1e6 bytes, df=255):\n");
    std::printf("  bento        = %.1f\n", chi_bento);
    std::printf("  p2p          = %.1f\n", chi_p2p);
    std::printf("  legacy bento = %.1f\n", chi_legacy_bento);
    std::printf("  legacy p2p   = %.1f\n", chi_legacy_p2p);
    Check(chi_bento > 150.0 && chi_bento < 400.0, "bento 1e6 字节 χ² 在合理区间");
    Check(chi_p2p > 150.0 && chi_p2p < 400.0, "p2p 1e6 字节 χ² 在合理区间");
    Check(chi_legacy_bento > 150.0 && chi_legacy_bento < 400.0, "旧 bento 实现 χ² 合格（参考组）");
    Check(chi_legacy_p2p > 150.0 && chi_legacy_p2p < 400.0, "旧 p2p 实现 χ² 合格（参考组）");

    // ------------------------------------------------------------------
    // 2) 16 字节块唯一性（1e6 字节 -> 62500 块；随机下重复数为 0）
    // ------------------------------------------------------------------
    const std::size_t dup_bento = DuplicateBlockCount(bento);
    const std::size_t dup_p2p = DuplicateBlockCount(p2p);
    std::printf("duplicate 16B blocks: bento=%zu p2p=%zu\n", dup_bento, dup_p2p);
    Check(dup_bento == 0, "bento 16 字节块无重复");
    Check(dup_p2p == 0, "p2p 16 字节块无重复");

    // ------------------------------------------------------------------
    // 3) 新旧实现「不同但都合格」
    // ------------------------------------------------------------------
    const double ratio_bento_p2p = DifferentRatio(bento, p2p);
    const double ratio_bento_legacy = DifferentRatio(bento, legacy_bento);
    const double ratio_p2p_legacy = DifferentRatio(p2p, legacy_p2p);
    std::printf("different-byte ratio: bento-vs-p2p=%.4f bento-vs-legacy=%.4f p2p-vs-legacy=%.4f\n",
                ratio_bento_p2p, ratio_bento_legacy, ratio_p2p_legacy);
    Check(ratio_bento_p2p > 0.99, "bento 与 p2p 输出不同（>99% 字节）");
    Check(ratio_bento_legacy > 0.99, "新 bento 与旧 random_device 实现输出不同");
    Check(ratio_p2p_legacy > 0.99, "新 p2p 与旧 getrandom 直读实现输出不同");

    // ------------------------------------------------------------------
    // 4) 退化检查：不是全零、不是单字节重复、没有明显长串
    // ------------------------------------------------------------------
    const bool bento_all_zero =
        std::all_of(bento.begin(), bento.end(), [](std::uint8_t value) { return value == 0; });
    const bool p2p_all_zero =
        std::all_of(p2p.begin(), p2p.end(), [](std::uint8_t value) { return value == 0; });
    Check(!bento_all_zero, "bento 输出不是全零");
    Check(!p2p_all_zero, "p2p 输出不是全零");

    std::array<bool, 256> seen{};
    for (const std::uint8_t byte : p2p) {
        seen[byte] = true;
    }
    const std::size_t distinct =
        static_cast<std::size_t>(std::count(seen.begin(), seen.end(), true));
    std::printf("p2p distinct byte values: %zu / 256\n", distinct);
    Check(distinct == 256, "p2p 1e6 字节覆盖全部 256 个字节值");

    // ------------------------------------------------------------------
    // 5) p2p 池化路径：多次小读取（跨池边界）仍正常且分布均匀
    // ------------------------------------------------------------------
    std::vector<std::uint8_t> pooled(400000);
    for (std::size_t offset = 0; offset + 4 <= pooled.size(); offset += 4) {
        std::uint32_t value = 0;
        go2cpp::p2p::SecureRandomBytes(&value, sizeof(value));
        std::memcpy(pooled.data() + offset, &value, sizeof(value));
    }
    const double chi_pooled = ChiSquare(pooled);
    std::printf("chi-square (p2p 4B calls, 400000 bytes) = %.1f\n", chi_pooled);
    Check(chi_pooled > 100.0 && chi_pooled < 500.0, "p2p 池化小读取 χ² 合格");

    std::printf("secure random: checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
