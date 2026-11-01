/**
 * @file relay_test_support.hpp
 * @brief RelayPipe 测试用的内存 Stream 与确定性数据生成器。
 *
 * @details 这个文件只被 `tests/relay/test_relay_pipe.cpp` 使用，不参与安装。
 *          它提供三类能力：
 *
 *          1. `PatternByte`/`FillPattern`/`PatternDigest`：确定性伪随机字节流，
 *             发送端与期望摘要用同一函数生成，避免在测试里缓存 1e8 字节；
 *          2. `TestStream`：可配置的内存双向流，支持 EOF、阻塞写、慢写、短写、
 *             空闲等待（用于取消测试），且 `Close()` 能从其它线程打断阻塞；
 *          3. `AsyncRun`：在后台线程跑 `RelayPipe::Run`，供主线程观察统计/超时。
 *
 *          `TestStream` 的线程安全约定与生产 Stream 契约一致：Read/Write/Close
 *          可被不同线程并发调用。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#pragma once

#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/error.hpp"
#include "go2cpp/hash/sha256.hpp"
#include "go2cpp/relay/relay_pipe.hpp"
#include "go2cpp/streams.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace go2cpp_relay_test {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::milliseconds;

/// 确定性字节生成：同一个 (seed, offset) 永远得到同一个字节。
inline std::uint8_t PatternByte(std::uint64_t seed, std::uint64_t offset) {
    std::uint32_t x = static_cast<std::uint32_t>(offset) * 0x9E3779B1U +
                      static_cast<std::uint32_t>(seed * 0x85EBCA6BU);
    x ^= x >> 16;
    x *= 0x7FEB352DU;
    x ^= x >> 15;
    return static_cast<std::uint8_t>(x >> 24);
}

/// 从 `offset` 开始填充 `size` 字节的确定性模式。
inline void FillPattern(char* out, std::size_t size, std::uint64_t seed,
                        std::uint64_t offset) {
    for (std::size_t index = 0; index < size; ++index) {
        out[index] = static_cast<char>(PatternByte(seed, offset + index));
    }
}

/// 计算前 `bytes` 字节模式的 SHA-256（流式计算，不缓存整段数据）。
inline std::array<std::uint8_t, 32> PatternDigest(std::uint64_t seed,
                                                 std::uint64_t bytes) {
    go2cpp::hash::Sha256 hasher;
    std::array<char, 32U * 1024U> buffer{};
    std::uint64_t offset = 0;
    while (offset < bytes) {
        const std::uint64_t remaining = bytes - offset;
        const auto take = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        FillPattern(buffer.data(), take, seed, offset);
        hasher.Write(std::string_view(buffer.data(), take));
        offset += take;
    }
    const std::vector<std::uint8_t> sum = hasher.Sum();
    std::array<std::uint8_t, 32> digest{};
    std::copy(sum.begin(), sum.end(), digest.begin());
    return digest;
}

/// `TestStream` 的行为参数。
struct StreamOptions {
    /// 入站数据种子（A→B 的源数据由发送端的 seed 决定）。
    std::uint64_t seed{0};
    /// 入站总字节数；读完后按 `inbound_eof` 决定 EOF 或等待。
    std::uint64_t inbound_bytes{0};
    /// true = 读满后返回 `io.EOF`；false = 继续等待（配合 wait_for_input）。
    bool inbound_eof{true};
    /// true = 没有数据时阻塞在 Read，直到 Close；模拟空闲长连接。
    bool wait_for_input{false};
    /// true = Write 阻塞直到 Close；模拟完全不读的对端。
    bool block_writes{false};
    /// 每次 Write 的人为延迟；模拟读得很慢但仍在推进的对端。
    std::chrono::microseconds write_delay{0};
    /// 单次 Write 最多接受多少字节；0 = 不限制。用于覆盖短写循环。
    std::size_t max_write_chunk{0};
};

/**
 * @brief 测试用内存双向流。
 *
 * @details 入站数据按 `(seed, offset)` 现场生成，不占内存；出站数据边写边算
 *          SHA-256，只保留计数与摘要，因此 1e8 字节测试的常驻内存仍然很小。
 */
class TestStream final : public go2cpp::relay::Stream {
public:
    explicit TestStream(StreamOptions options) : m_options(std::move(options)) {}

    go2cpp::streams::ReadResult Read(char* buffer, std::size_t size) override {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (true) {
            if (m_closed) {
                return {0, go2cpp::NewError("test stream: closed")};
            }
            if (m_read_offset < m_options.inbound_bytes) {
                break;
            }
            if (!m_options.inbound_eof) {
                if (!m_options.wait_for_input) {
                    return {0, go2cpp::streams::IoErrNoProgress()};
                }
                m_cv.wait(lock, [this] { return m_closed; });
                continue;
            }
            return {0, go2cpp::streams::IoEof()};
        }
        const std::uint64_t available = m_options.inbound_bytes - m_read_offset;
        const auto take = static_cast<std::size_t>(
            std::min<std::uint64_t>(available, size));
        FillPattern(buffer, take, m_options.seed, m_read_offset);
        m_read_offset += take;
        return {take, {}};
    }

    go2cpp::streams::WriteResult Write(const char* data, std::size_t size) override {
        if (m_options.block_writes) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_closed; });
            return {0, go2cpp::NewError("test stream: write blocked")};
        }
        if (m_options.write_delay > std::chrono::microseconds::zero()) {
            const TimePoint deadline = Clock::now() + m_options.write_delay;
            std::unique_lock<std::mutex> lock(m_mutex);
            while (!m_closed) {
                const TimePoint now = Clock::now();
                if (now >= deadline) {
                    break;
                }
                const auto slice = std::min<Clock::duration>(
                    deadline - now, std::chrono::milliseconds(1));
                m_cv.wait_until(lock, now + slice);
            }
            if (m_closed) {
                return {0, go2cpp::NewError("test stream: closed")};
            }
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return {0, go2cpp::NewError("test stream: closed")};
        }
        std::size_t take = size;
        if (m_options.max_write_chunk != 0) {
            take = std::min(take, m_options.max_write_chunk);
        }
        m_hasher.Write(std::string_view(data, take));
        m_written += take;
        return {take, {}};
    }

    go2cpp::ErrorPtr Close() override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
        }
        m_cv.notify_all();
        return {};
    }

    /// 是否被关闭（RelayPipe 在超时/取消路径会关闭目的端）。
    bool closed() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }

    /// 已成功写出的字节数（即对端从本流读到的字节数）。
    std::uint64_t written() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_written;
    }

    /// 已从本流读出的字节数（RelayPipe 的 Read 消费量）。
    std::uint64_t read_bytes() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_read_offset;
    }

    /// 出站数据的 SHA-256。
    std::array<std::uint8_t, 32> Digest() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::vector<std::uint8_t> sum = m_hasher.Sum();
        std::array<std::uint8_t, 32> digest{};
        std::copy(sum.begin(), sum.end(), digest.begin());
        return digest;
    }

private:
    StreamOptions m_options;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_closed{false};
    std::uint64_t m_read_offset{0};
    std::uint64_t m_written{0};
    go2cpp::hash::Sha256 m_hasher;
};

/// 在后台线程运行 `RelayPipe::Run`，供主线程轮询统计。
class AsyncRun final {
public:
    AsyncRun(go2cpp::relay::RelayPipe* pipe, go2cpp::ContextPtr context)
        : m_pipe(pipe), m_context(std::move(context)) {}

    ~AsyncRun() {
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    AsyncRun(const AsyncRun&) = delete;
    AsyncRun& operator=(const AsyncRun&) = delete;

    void Start() {
        m_thread = std::thread([this] {
            m_ok = m_pipe->Run(m_context, &m_error);
            m_done.store(true, std::memory_order_release);
        });
    }

    /// 最多等 `timeout`；超时返回 false（线程仍存活，调用方应 abort）。
    bool Join(Duration timeout) {
        const TimePoint deadline = Clock::now() + timeout;
        while (!m_done.load(std::memory_order_acquire)) {
            if (Clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        m_thread.join();
        return true;
    }

    /// 测试用：超时即打印并 abort，避免留下 joinable 线程导致 terminate。
    void JoinOrAbort(Duration timeout, const char* what, const char* file, int line) {
        if (Join(timeout)) {
            return;
        }
        std::fprintf(stderr, "[relay-test] timeout waiting for %s (%s:%d)\n", what,
                     file, line);
        std::fflush(stderr);
        std::abort();
    }

    bool ok() const { return m_ok; }
    go2cpp::ErrorPtr error() const { return m_error; }

private:
    go2cpp::relay::RelayPipe* m_pipe;
    go2cpp::ContextPtr m_context;
    std::thread m_thread;
    std::atomic<bool> m_done{false};
    bool m_ok{false};
    go2cpp::ErrorPtr m_error;
};

/// 轮询等待条件成立；超时返回 false。
template <typename Predicate>
bool WaitUntil(Predicate predicate, Duration timeout) {
    const TimePoint deadline = Clock::now() + timeout;
    while (!predicate()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/// 汇总检查计数；`main` 用它决定退出码。
inline std::atomic<int>& Failures() {
    static std::atomic<int> failures{0};
    return failures;
}

/// 记录一次失败（线程安全；测试断言只在主线程调用）。
inline void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        return;
    }
    Failures().fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[relay-test] FAIL %s:%d: %s\n", file, line, expression);
}

/// 带实际值的 64 位相等检查，失败时打印两边数值。
inline void CheckU64(std::uint64_t actual, std::uint64_t expected,
                     const char* expression, const char* file, int line) {
    if (actual == expected) {
        return;
    }
    Failures().fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[relay-test] FAIL %s:%d: %s (actual=%llu expected=%llu)\n", file,
                 line, expression, static_cast<unsigned long long>(actual),
                 static_cast<unsigned long long>(expected));
}

}  // namespace go2cpp_relay_test

#define RELAY_CHECK(expression) \
    ::go2cpp_relay_test::Check(static_cast<bool>(expression), #expression, __FILE__, \
                               __LINE__)

#define RELAY_CHECK_U64(actual, expected) \
    ::go2cpp_relay_test::CheckU64(static_cast<std::uint64_t>(actual), \
                                  static_cast<std::uint64_t>(expected), \
                                  #actual " == " #expected, __FILE__, __LINE__)
