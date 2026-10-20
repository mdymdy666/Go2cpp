#pragma once

/**
 * @file diagnostic_protocol.hpp
 * @brief Doctor 诊断通道的**帧协议**（header-only，零运行时依赖）。
 *
 * ## 为什么单独一个头文件
 *
 * `go2cpp-cli` 的构建约束是「不链接任何 go2cpp 运行时库」（见
 * `tools/cli/CMakeLists.txt`）。诊断协议的编解码必须同时被：
 *
 * - 目标进程内的 `DiagnosticAgent`（链接运行时）；
 * - 独立 CLI / 第三方客户端（不链接运行时）；
 * - 用户的「网络 topic 传输字节流」项目（只拿协议，不拿运行时）；
 *
 * 使用，因此本文件**只依赖 C++ 标准库**，不 include 任何 go2cpp 运行时头。
 *
 * ## 帧格式（G2CD/1，稳定）
 *
 * ```text
 * offset size field          说明
 * 0      4    magic          0x47 0x32 0x43 0x44（ASCII "G2CD"）
 * 4      1    version        1
 * 5      1    kind           1=request 2=response 3=error
 * 6      2    flags          bit0=chunked bit1=last_chunk bit2=json_payload
 * 8      4    stream_id      应用层命令序号（客户端分配，响应原样带回）
 * 12     4    seq            同一 stream 内的分块序号（请求恒为 0）
 * 16     4    payload_len    payload 字节数（0..16 MiB）
 * 20     4    checksum       FNV-1a/32（header 中此字段置 0 后 + payload）
 * 24     N    payload        UTF-8 命令文本 / 结果文本 / JSON
 * ```
 *
 * 所有多字节字段是**小端序**；编解码不依赖宿主字节序。
 *
 * ## 为什么是「文本命令 + 二进制帧」而不是全 JSON
 *
 * - 文本命令可读、可手写（`force-wake scope=all`），与既有 `GO2CPP-DEBUG/1`
 *   风格一致；
 * - 帧头负责**边界、多路复用（stream_id）、校验、分块**，JSON 只作为可选
 *   payload 格式（flags.json_payload），大 dump 走纯文本更省字节；
 * - 用户的 topic 字节流只需要按「先 24 字节头、再 payload_len 字节」切帧，
 *   不涉及 JSON 解析。
 *
 * ## 分块
 *
 * 一条响应可以拆成多个帧：同一 `stream_id`、`seq` 从 0 递增，
 * `flags.chunked=1`，最后一帧 `flags.last_chunk=1`。接收方按 `stream_id`
 * 归并。单帧 payload 上限 16 MiB，分块建议 64 KiB（Doctor 默认值）。
 *
 * ## 校验与安全
 *
 * - **校验和只用于发现截断/串线，不是密码学认证**（FNV-1a 非加密）。
 * - `Decode` 严格校验 magic/version/kind/flags/payload_len/checksum，任何
 *   不一致返回错误，不「猜」；
 * - 超过 `kMaxPayloadBytes` 直接拒绝，避免恶意长度导致内存放大。
 *
 * ## 与 transport 的关系
 *
 * 本文件只负责「字节流 <-> 帧」。可靠字节流（Unix socket / TCP / QUIC
 * stream / 用户的 topic 字节流）由 `DiagnosticTransport` 提供；帧里的
 * `stream_id` 由客户端分配，**不依赖**底层流的 id（QUIC 的一条 bidi stream
 * 里照样可以放多个帧，但 Doctor 的约定是「一条 QUIC stream 只承载一条命令的
 * 请求 + 响应」）。
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace go2cpp::debug {

/// 帧魔数：ASCII "G2CD"（按小端读作 0x44433247）。
inline constexpr std::uint32_t kDiagnosticMagic = 0x44433247U;
/// 协议版本；不兼容改动必须递增（Doctor 与 CLI 都要校验）。
inline constexpr std::uint8_t kDiagnosticVersion = 1;
/// 帧头固定长度（字节）。
inline constexpr std::size_t kDiagnosticHeaderBytes = 24;
/// 单帧 payload 上限（16 MiB）。
inline constexpr std::size_t kDiagnosticMaxPayload = 16U * 1024U * 1024U;
/// Doctor 默认分块大小（64 KiB）。
inline constexpr std::size_t kDiagnosticDefaultChunk = 64U * 1024U;

/// 帧类型。
enum class DiagnosticFrameKind : std::uint8_t {
    kRequest = 1,  ///< 客户端 -> Doctor
    kResponse = 2, ///< Doctor -> 客户端（成功）
    kError = 3,    ///< Doctor -> 客户端（失败；payload 为 `code: message`）
};

/// 帧标志位。
enum DiagnosticFrameFlags : std::uint16_t {
    kDiagnosticFlagChunked = 1U << 0,   ///< 本响应由多个帧组成
    kDiagnosticFlagLastChunk = 1U << 1, ///< 本帧是同一 stream 的最后一帧
    kDiagnosticFlagJson = 1U << 2,      ///< payload 是 JSON（否则是 UTF-8 文本）
};

/// 一个已解码的帧（不含 transport 的 peer 元数据）。
struct DiagnosticFrame {
    DiagnosticFrameKind kind{DiagnosticFrameKind::kRequest};
    std::uint16_t flags{0};
    std::uint32_t stream_id{0};
    std::uint32_t seq{0};
    std::string payload;

    bool chunked() const noexcept {
        return (flags & kDiagnosticFlagChunked) != 0U;
    }
    bool last_chunk() const noexcept {
        return (flags & kDiagnosticFlagLastChunk) != 0U;
    }
    bool json() const noexcept {
        return (flags & kDiagnosticFlagJson) != 0U;
    }
};

/// FNV-1a/32：非加密校验，只用于发现截断/串线。
inline std::uint32_t DiagnosticChecksum(const std::uint8_t* data,
                                        std::size_t size) noexcept {
    std::uint32_t hash = 2166136261U;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint32_t>(data[i]);
        hash *= 16777619U;
    }
    return hash;
}

namespace detail {

inline void PutU16(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
}

inline void PutU32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
    out[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFU);
}

inline std::uint16_t GetU16(const std::uint8_t* in) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(in[0]) |
        (static_cast<std::uint16_t>(in[1]) << 8));
}

inline std::uint32_t GetU32(const std::uint8_t* in) noexcept {
    return static_cast<std::uint32_t>(in[0]) |
           (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) |
           (static_cast<std::uint32_t>(in[3]) << 24);
}

inline bool ValidKind(std::uint8_t value) noexcept {
    return value == static_cast<std::uint8_t>(DiagnosticFrameKind::kRequest) ||
           value == static_cast<std::uint8_t>(DiagnosticFrameKind::kResponse) ||
           value == static_cast<std::uint8_t>(DiagnosticFrameKind::kError);
}

inline bool ValidFlags(std::uint16_t flags) noexcept {
    constexpr std::uint16_t kKnown =
        kDiagnosticFlagChunked | kDiagnosticFlagLastChunk | kDiagnosticFlagJson;
    return (flags & static_cast<std::uint16_t>(~kKnown)) == 0U;
}

}  // namespace detail

/**
 * @brief 把一帧编码为字节串（追加到 out）。
 * @return payload 超过上限或字段非法时返回 false，且不修改 out 的既有内容
 *         （先写临时串再 append，避免半帧污染）。
 */
inline bool EncodeDiagnosticFrame(const DiagnosticFrame& frame,
                                  std::string& out) {
    if (frame.payload.size() > kDiagnosticMaxPayload) {
        return false;
    }
    if (!detail::ValidKind(static_cast<std::uint8_t>(frame.kind)) ||
        !detail::ValidFlags(frame.flags)) {
        return false;
    }
    std::string encoded;
    encoded.resize(kDiagnosticHeaderBytes + frame.payload.size());
    auto* bytes = reinterpret_cast<std::uint8_t*>(&encoded[0]);
    detail::PutU32(bytes + 0, kDiagnosticMagic);
    bytes[4] = kDiagnosticVersion;
    bytes[5] = static_cast<std::uint8_t>(frame.kind);
    detail::PutU16(bytes + 6, frame.flags);
    detail::PutU32(bytes + 8, frame.stream_id);
    detail::PutU32(bytes + 12, frame.seq);
    detail::PutU32(bytes + 16, static_cast<std::uint32_t>(frame.payload.size()));
    detail::PutU32(bytes + 20, 0U);  // checksum 先置 0，再计算
    if (!frame.payload.empty()) {
        std::memcpy(bytes + kDiagnosticHeaderBytes, frame.payload.data(),
                    frame.payload.size());
    }
    const std::uint32_t checksum = DiagnosticChecksum(bytes, encoded.size());
    detail::PutU32(bytes + 20, checksum);
    out += encoded;
    return true;
}

/// 便捷重载：返回编码后的完整帧字节串（失败返回空串）。
inline std::string EncodeDiagnosticFrame(const DiagnosticFrame& frame) {
    std::string out;
    if (!EncodeDiagnosticFrame(frame, out)) {
        return {};
    }
    return out;
}

/// 解码结果。
struct DiagnosticDecodeResult {
    enum class Status { kOk, kNeedMore, kError };
    Status status{Status::kNeedMore};
    DiagnosticFrame frame;
    std::size_t consumed{0};  ///< kOk 时本帧占用的字节数
    std::string error;        ///< kError 时的稳定原因
};

/// 从字节串尝试解出一帧（不修改入参；consumed 由调用方移除）。
inline DiagnosticDecodeResult DecodeDiagnosticFrame(const std::uint8_t* data,
                                                    std::size_t size) {
    DiagnosticDecodeResult result;
    if (size < kDiagnosticHeaderBytes) {
        result.status = DiagnosticDecodeResult::Status::kNeedMore;
        return result;
    }
    if (detail::GetU32(data) != kDiagnosticMagic) {
        result.status = DiagnosticDecodeResult::Status::kError;
        result.error = "bad-magic";
        return result;
    }
    if (data[4] != kDiagnosticVersion) {
        result.status = DiagnosticDecodeResult::Status::kError;
        result.error = "unsupported-version";
        return result;
    }
    if (!detail::ValidKind(data[5])) {
        result.status = DiagnosticDecodeResult::Status::kError;
        result.error = "bad-kind";
        return result;
    }
    const std::uint16_t flags = detail::GetU16(data + 6);
    if (!detail::ValidFlags(flags)) {
        result.status = DiagnosticDecodeResult::Status::kError;
        result.error = "bad-flags";
        return result;
    }
    const std::uint32_t payload_size = detail::GetU32(data + 16);
    if (payload_size > kDiagnosticMaxPayload) {
        result.status = DiagnosticDecodeResult::Status::kError;
        result.error = "payload-too-large";
        return result;
    }
    const std::size_t total = kDiagnosticHeaderBytes + payload_size;
    if (size < total) {
        result.status = DiagnosticDecodeResult::Status::kNeedMore;
        return result;
    }
    // 校验和把 checksum 字段当 0 计算：临时清零一份 24 字节头。
    std::uint8_t header[kDiagnosticHeaderBytes];
    std::memcpy(header, data, kDiagnosticHeaderBytes);
    const std::uint32_t expected = detail::GetU32(header + 20);
    detail::PutU32(header + 20, 0U);
    std::uint32_t actual = DiagnosticChecksum(header, kDiagnosticHeaderBytes);
    if (payload_size != 0U) {
        const std::uint8_t* payload = data + kDiagnosticHeaderBytes;
        for (std::uint32_t i = 0; i < payload_size; ++i) {
            actual ^= static_cast<std::uint32_t>(payload[i]);
            actual *= 16777619U;
        }
    }
    if (actual != expected) {
        result.status = DiagnosticDecodeResult::Status::kError;
        result.error = "bad-checksum";
        return result;
    }

    result.frame.kind = static_cast<DiagnosticFrameKind>(data[5]);
    result.frame.flags = flags;
    result.frame.stream_id = detail::GetU32(data + 8);
    result.frame.seq = detail::GetU32(data + 12);
    result.frame.payload.assign(
        reinterpret_cast<const char*>(data + kDiagnosticHeaderBytes),
        payload_size);
    result.consumed = total;
    result.status = DiagnosticDecodeResult::Status::kOk;
    return result;
}

inline DiagnosticDecodeResult DecodeDiagnosticFrame(const std::string& data) {
    return DecodeDiagnosticFrame(
        reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

/**
 * @brief 增量帧解析器：喂入任意切分的字节流，按帧取出。
 *
 * 传输层（Unix socket / TCP / QUIC stream）每次 recv 的边界与帧边界无关，
 * 必须由解析器缓冲。用法：
 *
 * ```cpp
 * DiagnosticFrameParser parser;
 * parser.Feed(network_bytes);
 * DiagnosticFrame frame;
 * while (parser.Next(frame)) { handle(frame); }
 * if (parser.failed()) { ... }
 * ```
 *
 * @note 非线程安全：每个连接/stream 一个实例。
 * @note 缓冲上限 = 单帧上限 + 帧头；恶意客户端持续发垃圾会被 `failed()`
 *       拒绝，调用方应关闭连接。
 */
class DiagnosticFrameParser final {
public:
    /// 追加字节；超过缓冲上限时置为 failed。
    bool Feed(const void* data, std::size_t size) {
        if (m_failed) {
            return false;
        }
        if (size != 0U) {
            m_buffer.append(static_cast<const char*>(data), size);
        }
        if (m_buffer.size() > kDiagnosticMaxPayload + kDiagnosticHeaderBytes) {
            Fail("buffer-limit");
            return false;
        }
        return true;
    }
    bool Feed(const std::string& data) {
        return Feed(data.data(), data.size());
    }

    /// 取下一帧；返回 false 表示需要更多数据（或已 failed）。
    bool Next(DiagnosticFrame& frame) {
        if (m_failed || m_buffer.empty()) {
            return false;
        }
        const auto result = DecodeDiagnosticFrame(
            reinterpret_cast<const std::uint8_t*>(m_buffer.data()),
            m_buffer.size());
        if (result.status == DiagnosticDecodeResult::Status::kNeedMore) {
            return false;
        }
        if (result.status == DiagnosticDecodeResult::Status::kError) {
            Fail(result.error);
            return false;
        }
        m_buffer.erase(0, result.consumed);
        frame = std::move(result.frame);
        return true;
    }

    bool failed() const noexcept { return m_failed; }
    const std::string& error() const noexcept { return m_error; }
    void Reset() {
        m_buffer.clear();
        m_failed = false;
        m_error.clear();
    }

private:
    void Fail(std::string error) {
        m_failed = true;
        m_error = std::move(error);
        m_buffer.clear();
    }

    std::string m_buffer;
    bool m_failed{false};
    std::string m_error;
};

// ---------------------------------------------------------------------------
// 命令与响应的稳定文本约定（Doctor 与 CLI 共用，改名视为破坏协议）
// ---------------------------------------------------------------------------

/// 协议标识字符串（响应 JSON 的 `protocol` 字段）。
inline constexpr const char* kDiagnosticProtocol = "G2CD/1";

/// `status`：协程数、M/P、runqueue、锁等待、模块基址 + build-id。
inline constexpr const char* kDoctorCmdStatus = "status";
/// `dump [sections=a,b]`：复用 DebugServer 的组件 dump。
inline constexpr const char* kDoctorCmdDump = "dump";
/// `force-wake scope=scheduler-idle|parking-condition|channel|all`。
inline constexpr const char* kDoctorCmdForceWake = "force-wake";
/// `deadlock-report`：等待图。
inline constexpr const char* kDoctorCmdDeadlockReport = "deadlock-report";
/// `mutex-unlock name=<registered-name>`：受控解锁（白名单判定）。
inline constexpr const char* kDoctorCmdMutexUnlock = "mutex-unlock";
/// `inject-fiber fn=<registered-function>`：在诊断 Fiber 内执行。
inline constexpr const char* kDoctorCmdInjectFiber = "inject-fiber";
/// `ping`：存活探测（不触碰运行时状态）。
inline constexpr const char* kDoctorCmdPing = "ping";
/// `help`：命令列表。
inline constexpr const char* kDoctorCmdHelp = "help";
/// `stats`：Doctor 自身的队列/执行统计。
inline constexpr const char* kDoctorCmdStats = "stats";

/// `force-wake` 的合法 scope（稳定字符串）。
inline constexpr const char* kWakeScopeSchedulerIdle = "scheduler-idle";
inline constexpr const char* kWakeScopeParkingCondition = "parking-condition";
inline constexpr const char* kWakeScopeChannel = "channel";
inline constexpr const char* kWakeScopeAll = "all";
/// 显式请求但**不在白名单**的 scope：用于演示 `refused` 语义。
inline constexpr const char* kWakeScopeMutex = "mutex";

/// 结果判定词（稳定；CLI 按此分支）。
inline constexpr const char* kVerdictOk = "ok";
inline constexpr const char* kVerdictPartial = "partial";
inline constexpr const char* kVerdictRefused = "refused";

}  // namespace go2cpp::debug
