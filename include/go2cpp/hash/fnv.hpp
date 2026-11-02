#pragma once

/**
 * @file fnv.hpp
 * @brief FNV-1a 非加密哈希（32/64 位）的单一实现。
 *
 * @details FNV-1a 是项目里用于「候选排序 / 帧校验 / 迁移 build-id / 音频帧
 *          校验」的轻量哈希，原实现分散在 4 个模块。本文件把算法收敛到一处，
 *          只保留纯头文件 inline（零链接依赖、可被任意模块 include）。
 *
 * ## 算法（Fowler–Noll–Vo，1a 变体）
 *
 * ```text
 * hash = offset_basis
 * for each byte b:
 *     hash = (hash XOR b) * prime        // 32 位/64 位各自取模
 * ```
 *
 * 64 位参数：offset_basis = 0xcbf29ce484222325，prime = 0x100000001b3；
 * 32 位参数：offset_basis = 2166136261，prime = 16777619。
 *
 * ## 为什么提供 `*Update` 入口
 *
 * `p2p` 的 `Endpoint::Hash()` / `MakeFoundation()` 是「在已有状态上继续吸收」
 * 的增量用法（先 family 字节、再地址、再端口），所以算法入口必须能接收
 * 调用方给的初始状态，而不是只提供一次性入口。
 *
 * ## 历史偏移量
 *
 * `src/media/audio.cpp` 与 `src/p2p/{endpoint,candidate}.cpp` 用的是少一位的
 * 偏移量 `1469598103934665603`（标准值是 `14695981039346656037`）。该值已经
 * 进入线上校验/排序结果，**不能**顺手改成标准值；调用方通过 `Fnv1a64Update`
 * 显式传入原偏移量，保证输出逐字节不变。
 *
 * @author go2cpp contributors
 * @date 2026-11-02
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>

#include "go2cpp/hash/hash.hpp"

namespace go2cpp::hash {

/// FNV-1a/64 标准偏移基（`0xcbf29ce484222325`）。
inline constexpr std::uint64_t kFnv1a64OffsetBasis = 0xcbf29ce484222325ULL;
/// FNV-1a/64 质数（`0x00000100000001b3`）。
inline constexpr std::uint64_t kFnv1a64Prime = 0x00000100000001b3ULL;
/// FNV-1a/32 标准偏移基（`2166136261`）。
inline constexpr std::uint32_t kFnv1a32OffsetBasis = 2166136261U;
/// FNV-1a/32 质数（`16777619`）。
inline constexpr std::uint32_t kFnv1a32Prime = 16777619U;

/**
 * @brief 在已有状态上继续吸收字节（FNV-1a/64）。
 * @param state 初始状态（一次性计算传 `kFnv1a64OffsetBasis`；增量调用传上次结果）。
 * @param data 输入字节（可为 nullptr，此时 `size` 必须为 0）。
 * @param size 字节数。
 * @return 吸收后的状态。
 */
inline std::uint64_t Fnv1a64Update(std::uint64_t state, const std::uint8_t* data,
                                   std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        state ^= static_cast<std::uint64_t>(data[i]);
        state *= kFnv1a64Prime;
    }
    return state;
}

/// @brief 一次性 FNV-1a/64（标准偏移基）。
inline std::uint64_t Fnv1a64(const std::uint8_t* data, std::size_t size) noexcept {
    return Fnv1a64Update(kFnv1a64OffsetBasis, data, size);
}

/// @brief 一次性 FNV-1a/64（`ByteSpan` 重载）。
inline std::uint64_t Fnv1a64(ByteSpan data) noexcept {
    return Fnv1a64(data.data(), data.size());
}

/**
 * @brief 在已有状态上继续吸收字节（FNV-1a/32）。
 * @param state 初始状态（一次性计算传 `kFnv1a32OffsetBasis`）。
 * @param data 输入字节（可为 nullptr，此时 `size` 必须为 0）。
 * @param size 字节数。
 * @return 吸收后的状态。
 */
inline std::uint32_t Fnv1a32Update(std::uint32_t state, const std::uint8_t* data,
                                   std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        state ^= static_cast<std::uint32_t>(data[i]);
        state *= kFnv1a32Prime;
    }
    return state;
}

/// @brief 一次性 FNV-1a/32（标准偏移基）。
inline std::uint32_t Fnv1a32(const std::uint8_t* data, std::size_t size) noexcept {
    return Fnv1a32Update(kFnv1a32OffsetBasis, data, size);
}

/// @brief 一次性 FNV-1a/32（`ByteSpan` 重载）。
inline std::uint32_t Fnv1a32(ByteSpan data) noexcept {
    return Fnv1a32(data.data(), data.size());
}

}  // namespace go2cpp::hash
