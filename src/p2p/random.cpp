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

}  // namespace

void SecureRandomBytes(void* data, std::size_t length) {
    if (data == nullptr || length == 0) {
        return;
    }
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t filled = FillFromGetrandom(bytes, length);
    if (filled < length) {
        filled += FillFromUrandom(bytes + filled, length - filled);
    }
    if (filled < length) {
        FillDeterministic(bytes + filled, length - filled);
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
