#pragma once

/**
 * @file random.hpp
 * @brief P2P 模块使用的随机数工具（事务 ID、nonce、候选 foundation 等）。
 * @details 统一走 `getrandom(2)`，失败时退回 `/dev/urandom`，最后才用
 *          `std::random_device`。不用 `rand()`：STUN 事务 ID 与 TURN 请求 ID
 *          需要跨进程不重复，低熵源会显著提高碰撞概率。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>

namespace go2cpp::p2p {

/// 填充 `length` 字节密码学安全随机数；任何错误都会退化为确定性填充（不抛异常）。
void SecureRandomBytes(void* data, std::size_t length);

/// 生成 32 位随机数（不保证非零；调用方按需处理 0）。
std::uint32_t RandomU32();

/// 生成 64 位随机数。
std::uint64_t RandomU64();

}  // namespace go2cpp::p2p
