#pragma once

/**
 * @file random.hpp
 * @brief P2P 模块使用的随机数工具（事务 ID、nonce、候选 foundation 等）。
 * @details 唯一熵源是 `go::crypto::rand`（`getrandom(2)` 优先、`/dev/urandom`
 *          兜底）；本文件只额外做「每线程 1 KiB 池化」，把 STUN 事务 ID /
 *          TURN 请求 ID 的逐包系统调用摊薄。不用 `rand()`：这些 ID 需要跨进程
 *          不重复，低熵源会显著提高碰撞概率。
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
