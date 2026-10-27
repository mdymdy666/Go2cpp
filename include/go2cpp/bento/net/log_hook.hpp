#pragma once

/**
 * @file log_hook.hpp
 * @brief Bento 的轻量日志/事件钩子：级别过滤、结构化事件、回调与计数。
 * @details 背景：Bento 刻意**不依赖** go2cpp 的 `log` 模块（传输层要能独立
 *          编译、独立测试，也避免把日志后端绑进库依赖）。但上层仍需要
 *          「连接建立/断开、握手重传、可靠层重传与丢包、认证失败」这些
 *          事件，用于运维告警与排障。本文件因此只提供一个极小的注入点：
 *
 *          - **回调**：`SetSink` 注入 `std::function<void(const LogRecord&)>`；
 *            未注入时所有 Emit 只做计数，零字符串格式化、零分配。
 *          - **级别**：Trace/Debug/Info/Warn/Error，低于 `min_level` 的记录
 *            被计数为 filtered，不回调。
 *          - **计数**：按级别、按事件分别累计，供 `Stats()` 暴露给
 *            Prometheus/状态页；`ResetStats` 便于测试隔离。
 *
 *          线程模型：`Emit` 可在任意线程调用（PeerHub 的 RunOnce 线程与
 *          并发 Send* 线程都会触发）。实现先在锁内完成级别过滤与计数，
 *          再**复制 sink 到锁外调用**——这样 sink 回调里可以安全地再次
 *          Emit 或调用 PeerHub/SessionControl 的接口，不会自锁。
 *          代价是每条通过过滤的记录复制一次 `std::function`；事件是低频
 *          路径（连接、重传、丢包、认证失败），可以接受。
 *
 *          生命周期：`LogRecord::message` 指向调用方传入的字符串，只在
 *          回调期间有效；sink 若需要留存必须自行复制。
 *
 * @author go2cpp contributors
 * @date 2026-10-27
 * @copyright MIT License
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

#include "go2cpp/bento/net/protocol.hpp"

namespace go2cpp::bento {

/// @brief 日志级别，数值越大越严重（与常见日志库一致）。
enum class LogLevel : std::uint8_t {
    kTrace = 0,  ///< 逐帧细节（默认不开启）。
    kDebug = 1,  ///< 重传、握手重试等周期性事件。
    kInfo = 2,   ///< 连接建立/关闭等生命周期事件。
    kWarn = 3,   ///< 丢包、认证失败、超时等需要关注的事件。
    kError = 4,  ///< 发送/接收失败等错误。
};

/// 级别枚举数量（数组下标上界）。
inline constexpr std::size_t kLogLevelCount = 5;

/// @brief 结构化事件类型。计数按事件维度累计，便于直接上指标。
enum class LogEvent : std::uint8_t {
    kPeerConnected = 0,     ///< 握手完成，peer 可用。
    kPeerDisconnected = 1,  ///< peer 被移除/主动关闭。
    kPeerTimeout = 2,       ///< 心跳或握手超时。
    kHandshakeRetry = 3,    ///< HELLO 重传（握手包丢失或对端未响应）。
    kHandshakeFailed = 4,   ///< 握手失败（超时上限/参数非法）。
    kRetransmit = 5,        ///< 可靠层重传（快速重传或 PTO）。
    kPacketLost = 6,        ///< 可靠层判定丢包（拥塞降窗事件）。
    kAuthFailure = 7,       ///< AEAD 认证失败（错误 PSK 或篡改）。
    kReplayDrop = 8,        ///< 重放窗口拒绝。
    kPlaintextRejected = 9, ///< 加密模式下收到明文帧（降级攻击）。
    kFrameRejected = 10,    ///< 帧格式/长度非法。
    kSendError = 11,        ///< sendto/加密封装失败。
    kRecvError = 12,        ///< recvfrom 失败。
    kControlChange = 13,    ///< 控制权变更（由 SessionControl 上层接入）。
    kCount = 14,            ///< 哨兵：事件数量。
};

/// 事件枚举数量（数组下标上界）。
inline constexpr std::size_t kLogEventCount = static_cast<std::size_t>(LogEvent::kCount);

/// @brief 级别名（"trace"/"debug"/"info"/"warn"/"error"）。
inline const char* LogLevelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::kTrace:
            return "trace";
        case LogLevel::kDebug:
            return "debug";
        case LogLevel::kInfo:
            return "info";
        case LogLevel::kWarn:
            return "warn";
        case LogLevel::kError:
            return "error";
    }
    return "unknown";
}

/// @brief 事件名（"peer_connected" 等，稳定字符串，可做指标标签）。
inline const char* LogEventName(LogEvent event) noexcept {
    switch (event) {
        case LogEvent::kPeerConnected:
            return "peer_connected";
        case LogEvent::kPeerDisconnected:
            return "peer_disconnected";
        case LogEvent::kPeerTimeout:
            return "peer_timeout";
        case LogEvent::kHandshakeRetry:
            return "handshake_retry";
        case LogEvent::kHandshakeFailed:
            return "handshake_failed";
        case LogEvent::kRetransmit:
            return "retransmit";
        case LogEvent::kPacketLost:
            return "packet_lost";
        case LogEvent::kAuthFailure:
            return "auth_failure";
        case LogEvent::kReplayDrop:
            return "replay_drop";
        case LogEvent::kPlaintextRejected:
            return "plaintext_rejected";
        case LogEvent::kFrameRejected:
            return "frame_rejected";
        case LogEvent::kSendError:
            return "send_error";
        case LogEvent::kRecvError:
            return "recv_error";
        case LogEvent::kControlChange:
            return "control_change";
        case LogEvent::kCount:
            break;
    }
    return "unknown";
}

/// @brief 一条日志记录的轻量视图（不拥有 message）。
struct LogRecord {
    LogLevel level = LogLevel::kInfo;           ///< 级别。
    LogEvent event = LogEvent::kPeerConnected;  ///< 事件类型。
    std::uint64_t timestamp_ms = 0;             ///< NowMs() 单调时钟毫秒。
    std::uint32_t peer_id = 0;                  ///< 相关 peer；0 = 与具体 peer 无关。
    std::uint64_t value = 0;                    ///< 事件数值（重传次数、丢弃字节等）。
    const char* message = nullptr;              ///< 短描述，仅回调期间有效。
};

/// @brief 钩子计数器快照。
struct LogHookStats {
    std::uint64_t emitted = 0;   ///< 通过过滤并调用了 sink 的记录数。
    std::uint64_t filtered = 0;  ///< 被级别过滤或没有 sink 而跳过的记录数。
    std::array<std::uint64_t, kLogLevelCount> per_level{};  ///< 按级别。
    std::array<std::uint64_t, kLogEventCount> per_event{};  ///< 按事件。
};

/// @brief 轻量日志钩子：注入回调、按级别过滤、按级别/事件计数。
class LogHook final {
   public:
    using Sink = std::function<void(const LogRecord&)>;

    LogHook() = default;
    explicit LogHook(Sink sink, LogLevel min_level = LogLevel::kInfo)
        : m_sink(std::move(sink)), m_min_level(min_level) {}

    LogHook(const LogHook&) = delete;
    LogHook& operator=(const LogHook&) = delete;

    /// @brief 设置回调（传空 function 等价于卸载）。
    void SetSink(Sink sink) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sink = std::move(sink);
    }

    /// @brief 是否已注入回调。
    bool has_sink() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<bool>(m_sink);
    }

    /// @brief 设置最低级别（低于它的记录只计入 filtered）。
    void SetMinLevel(LogLevel level) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_min_level = level;
    }

    LogLevel min_level() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_min_level;
    }

    /// @brief 记录一条事件。
    /// @param message 短描述；只在回调期间有效，可为 nullptr。
    void Emit(LogLevel level, LogEvent event, std::uint32_t peer_id, const char* message,
              std::uint64_t value = 0) {
        Sink sink;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_sink || static_cast<std::uint8_t>(level) <
                               static_cast<std::uint8_t>(m_min_level)) {
                ++m_stats.filtered;
                return;
            }
            ++m_stats.emitted;
            ++m_stats.per_level[LevelIndex(level)];
            ++m_stats.per_event[EventIndex(event)];
            // 复制后在锁外调用：sink 里可以再次 Emit / 查询统计 / 回调 PeerHub。
            sink = m_sink;
        }
        LogRecord record;
        record.level = level;
        record.event = event;
        record.timestamp_ms = NowMs();
        record.peer_id = peer_id;
        record.value = value;
        record.message = message;
        sink(record);
    }

    // ---- 便捷入口（语义等价于 Emit，减少调用点噪声） ----
    void Trace(LogEvent event, std::uint32_t peer_id, const char* message,
               std::uint64_t value = 0) {
        Emit(LogLevel::kTrace, event, peer_id, message, value);
    }
    void Debug(LogEvent event, std::uint32_t peer_id, const char* message,
               std::uint64_t value = 0) {
        Emit(LogLevel::kDebug, event, peer_id, message, value);
    }
    void Info(LogEvent event, std::uint32_t peer_id, const char* message,
              std::uint64_t value = 0) {
        Emit(LogLevel::kInfo, event, peer_id, message, value);
    }
    void Warn(LogEvent event, std::uint32_t peer_id, const char* message,
              std::uint64_t value = 0) {
        Emit(LogLevel::kWarn, event, peer_id, message, value);
    }
    void Error(LogEvent event, std::uint32_t peer_id, const char* message,
               std::uint64_t value = 0) {
        Emit(LogLevel::kError, event, peer_id, message, value);
    }

    /// @brief 计数器快照（线程安全）。
    LogHookStats Stats() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stats;
    }

    /// @brief 某一级别的累计条数。
    std::uint64_t Count(LogLevel level) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stats.per_level[LevelIndex(level)];
    }

    /// @brief 某一事件的累计条数。
    std::uint64_t Count(LogEvent event) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stats.per_event[EventIndex(event)];
    }

    /// @brief 清零计数器（不影响已注入的 sink 与级别）。
    void ResetStats() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats = LogHookStats{};
    }

   private:
    static std::size_t LevelIndex(LogLevel level) noexcept {
        const auto raw = static_cast<std::size_t>(level);
        return raw < kLogLevelCount ? raw : kLogLevelCount - 1;
    }
    static std::size_t EventIndex(LogEvent event) noexcept {
        const auto raw = static_cast<std::size_t>(event);
        return raw < kLogEventCount ? raw : kLogEventCount - 1;
    }

    mutable std::mutex m_mutex;
    Sink m_sink;
    LogLevel m_min_level = LogLevel::kInfo;
    LogHookStats m_stats;
};

}  // namespace go2cpp::bento
