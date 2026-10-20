#pragma once

/**
 * @file scenario.hpp
 * @brief record 与 replay 两个 demo 共用的场景逻辑（3 producer → 1 consumer）。
 *
 * 场景刻意选**无缓冲 channel**：每次交接都是一次真实 rendezvous，3 个
 * producer 在同一个 channel 上竞争，交错由调度器决定；consumer 收到的
 * 消息顺序就是「交错指纹」。replay 端复用同一份 `Produce`/`Consume`，
 * 因此消息序列一致即说明记录的交错被正确重演。
 */

#include "replay/format.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

namespace demo {

using go2cpp::replay::Decision;

inline constexpr std::uint32_t kProducers = 3;
inline constexpr std::uint32_t kMessagesPerProducer = 32;
inline constexpr std::uint32_t kTotalMessages = kProducers * kMessagesPerProducer;
inline constexpr std::uint32_t kConsumerGid = kProducers;
inline constexpr std::uint32_t kFiberCount = kProducers + 1;
inline constexpr std::uint32_t kChannelId = 0;

/// 消费结果：消息序列 + 顺序敏感的校验和。
struct Result {
    void Push(std::uint64_t value) {
        std::lock_guard<std::mutex> lock(mutex);
        received.push_back(value);
    }

    std::vector<std::uint64_t> Snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return received;
    }

    /// FNV-1a 64 位：对消息**顺序**敏感，用于一行对照。
    std::uint64_t Checksum() {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const std::uint64_t value : Snapshot()) {
            hash ^= value;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    std::mutex mutex;
    std::vector<std::uint64_t> received;
};

/// 进程级共享结果：自动注册的入口没有用户上下文，用这个单例回传。
inline Result& SharedResult() {
    static Result result;
    return result;
}

/// 发送 `kMessagesPerProducer` 条；返回实际成功条数。
template <typename Channel>
std::uint64_t Produce(std::uint32_t gid, Channel& channel) {
    std::uint64_t sent = 0;
    for (std::uint32_t index = 0; index < kMessagesPerProducer; ++index) {
        const std::uint64_t value =
            static_cast<std::uint64_t>(gid) * 10000ULL + index;
        if (channel.Send(value) != Decision::kOk) {
            break;
        }
        ++sent;
    }
    return sent;
}

/// 接收 `kTotalMessages` 条；返回实际收到条数。
template <typename Channel>
std::uint64_t Consume(Channel& channel, Result* result) {
    std::uint64_t received = 0;
    for (std::uint32_t index = 0; index < kTotalMessages; ++index) {
        auto outcome = channel.Recv();
        if (!outcome.ok()) {
            break;
        }
        result->Push(static_cast<std::uint64_t>(outcome.value));
        ++received;
    }
    return received;
}

}  // namespace demo
