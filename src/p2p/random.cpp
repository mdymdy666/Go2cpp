/**
 * @file random.cpp
 * @brief 随机数工具实现：getrandom -> /dev/urandom -> random_device。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/p2p/random.hpp"

#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>

namespace go2cpp::p2p {
namespace {

std::size_t FillFromGetrandom(std::uint8_t* data, std::size_t length) noexcept {
    std::size_t filled = 0;
    while (filled < length) {
        const ssize_t rc = ::getrandom(data + filled, length - filled, 0);
        if (rc > 0) {
            filled += static_cast<std::size_t>(rc);
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    return filled;
}

std::size_t FillFromUrandom(std::uint8_t* data, std::size_t length) noexcept {
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    std::size_t filled = 0;
    while (filled < length) {
        const ssize_t rc = ::read(fd, data + filled, length - filled);
        if (rc > 0) {
            filled += static_cast<std::size_t>(rc);
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(fd);
    return filled;
}

void FillDeterministic(std::uint8_t* data, std::size_t length) noexcept {
    // 极端情况下（无 getrandom、无 /dev/urandom）的兜底：用时间戳 + 线程 id
    // 做 splitmix64 扩散。这不具备密码学强度，所以只作为最后手段。
    std::uint64_t state = static_cast<std::uint64_t>(
                              std::chrono::steady_clock::now().time_since_epoch().count()) ^
                          static_cast<std::uint64_t>(
                              std::hash<std::thread::id>{}(std::this_thread::get_id()));
    for (std::size_t index = 0; index < length; ++index) {
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t mixed = state;
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
        mixed ^= mixed >> 31U;
        data[index] = static_cast<std::uint8_t>(mixed & 0xFFU);
    }
}

/// 一次 getrandom 批量预取的字节数：STUN 事务 ID / 中继请求 ID 每次只取
/// 4~12 字节，逐次调用会变成「每包一次系统调用」（实测约 0.3~0.5us）。
/// 池化后仍是内核 CSPRNG 的输出（只是批量取回），语义不变。
constexpr std::size_t kRandomPoolSize = 1024;

struct RandomPool {
    std::uint8_t bytes[kRandomPoolSize] = {};
    /// used == kRandomPoolSize 表示需要重新填充（首次使用也走这条路径）。
    std::size_t used = kRandomPoolSize;
};

thread_local RandomPool g_random_pool;

void RefillPool(RandomPool* pool) noexcept {
    std::size_t filled = FillFromGetrandom(pool->bytes, kRandomPoolSize);
    if (filled < kRandomPoolSize) {
        filled += FillFromUrandom(pool->bytes + filled, kRandomPoolSize - filled);
    }
    if (filled < kRandomPoolSize) {
        FillDeterministic(pool->bytes + filled, kRandomPoolSize - filled);
    }
    pool->used = 0;
}

}  // namespace

void SecureRandomBytes(void* data, std::size_t length) {
    if (data == nullptr || length == 0) {
        return;
    }
    auto* bytes = static_cast<std::uint8_t*>(data);
    RandomPool& pool = g_random_pool;
    while (length > 0) {
        if (pool.used >= kRandomPoolSize) {
            RefillPool(&pool);
        }
        const std::size_t chunk = std::min(length, kRandomPoolSize - pool.used);
        std::memcpy(bytes, pool.bytes + pool.used, chunk);
        pool.used += chunk;
        bytes += chunk;
        length -= chunk;
    }
}

std::uint32_t RandomU32() {
    std::uint32_t value = 0;
    SecureRandomBytes(&value, sizeof(value));
    return value;
}

std::uint64_t RandomU64() {
    std::uint64_t value = 0;
    SecureRandomBytes(&value, sizeof(value));
    return value;
}

}  // namespace go2cpp::p2p
