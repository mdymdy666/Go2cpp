/**
 * @file bufio_socket.hpp
 * @brief `bufio` 与 `net::Socket` 的适配层：缓冲读、批量写、零拷贝发送。
 *
 * @details `bufio::Reader` / `bufio::Writer` 只要求底层满足两个极小的契约：
 *
 *   @code
 *   streams::ReadResult  Read(char* buffer, std::size_t size);
 *   streams::WriteResult Write(const char* data, std::size_t size);
 *   @endcode
 *
 *   本文件把 `net::Socket` 包装成这两个契约，并补上 `bufio::Writer`
 *   批量 flush 所需的扩展接口：
 *
 *   | 适配器 | 方向 | 底层调用 | 解决的问题 |
 *   |---|---|---|---|
 *   | `SocketReader` | 读 | `Socket::recv` | 一次读取一大块，协议逐字节/逐帧解析只动内存 |
 *   | `SocketBatchWriter` | 写 | `Socket::send` / `sendv` / `send_batch` / `send_file` | N 条逻辑消息合并成 1 次系统调用；文件走 `sendfile(2)` |
 *
 * ## 批量写的三条路径
 *
 * `bufio::Writer::FlushBatch()` 检出 `WriteBatch(const bufio::BatchEntry*,
 * std::size_t)` 后会把当前所有待发段一次交出，`SocketBatchWriter` 按套接字
 * 类型选择最合适的系统调用：
 *
 * - **TCP（默认）**：`Socket::sendv` → `writev(2)`。`bufio::Writer` 的段都
 *   落在同一块连续缓冲里，因此这等价于一次 `write`，但保留了段边界，
 *   不必先把「消息头 + 消息体」拷贝成一个大字符串；
 * - **UDP（`datagram = true`）**：`Socket::send_batch` → `sendmmsg(2)`。
 *   每个段就是一条独立数据报，`transferred` 是**条数**，适配器会把它换算成
 *   「前 k 段的总字节数」，因为 `bufio::Writer` 的部分写语义按字节计。
 *   **要求 socket 已 `connect`**（`SendBatchEntry::to` 为空）；
 * - **文件**：`SendFile` → `Socket::send_file`（`sendfile(2)`），数据不经过
 *   用户态缓冲。
 *
 * ## 超时语义
 *
 * 适配器同时支持两种超时：
 * - `set_timeout(Duration)`：每次读写调用的预算，透传给 `Socket`；
 * - `SetDeadline(time_point)` / `ClearDeadline()`：整次协议的绝对截止时间。
 *   两者同时设置时取「剩余时间」与固定预算的较小值，与 `net::WebSocket` /
 *   `bufio` 上游对 deadline 的约定一致。
 *
 * ## 统计
 *
 * 三个计数器（`send_calls` / `writev_calls` / `sendmmsg_calls`）用于基准与
 * 回归断言：它们直接对应系统调用次数，是「批量是否真的生效」的硬证据。
 *
 * ## 边界与陷阱
 *
 * - 适配器只**引用** Socket（`shared_ptr` 保持存活），不拥有其生命周期；
 * - `SocketReader::Read` 返回 0 且 `error` 为空是合法状态（暂时无数据），
 *   `bufio::Reader` 连续 100 次无进展才判定违约；
 * - `SocketBatchWriter` 的 UDP 模式只做「一条段 = 一条数据报」，不负责重传、
 *   分片与地址管理——那是 `PeerHub`/`LinkServer` 的职责；
 * - `WriteBatch` 必须按调用方给的顺序提交；TCP 的部分写由 `bufio::Writer`
 *   重试剩余段，UDP 的部分成功由适配器换算成字节数后同样交回。
 */

#pragma once

#include "go2cpp/bufio/bufio.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/streams.hpp"
#include "go2cpp/tools/error.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::net {

namespace detail {

/// 把 errno 转成带文本的错误值；错误分类只在上层做，适配层不做隐式映射。
inline ErrorPtr SocketErrorFromErrno(int error, const char* what) {
    std::string text = what == nullptr ? "socket error" : what;
    if (error != 0) {
        text += ": ";
        text += std::strerror(error);
    }
    return NewError(text);
}

/// 计算「固定预算」与「绝对截止时间剩余」的较小值；无截止时间时原样返回。
inline Socket::Duration EffectiveTimeout(
    Socket::Duration budget, bool has_deadline,
    std::chrono::steady_clock::time_point deadline) {
    if (!has_deadline) {
        return budget;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return Socket::Duration::zero();
    }
    const auto remaining =
        std::chrono::duration_cast<Socket::Duration>(deadline - now);
    if (budget == Socket::Duration::max()) {
        return remaining;
    }
    return std::min(budget, remaining);
}

}  // namespace detail

/**
 * @brief 把 `Socket` 适配成 `streams::Reader`，供 `bufio::Reader` 包装。
 *
 * @details 除 socket 读之外还支持「预置字节」：`Prepend()` 的数据会在任何
 *   socket 读之前交付。WebSocket 握手时 `http::Parser` 可能已经多读了首帧，
 *   这段字节必须排在 socket 数据之前，否则首帧会被静默丢弃。
 */
class SocketReader final {
public:
    /// @param socket 已连接或已绑定的 socket；为空时所有读返回 EOF。
    /// @param timeout 单次 `recv` 的预算。
    explicit SocketReader(Socket::Ptr socket,
                          Socket::Duration timeout = Socket::Duration::max())
        : m_socket(std::move(socket)), m_timeout(timeout) {}

    SocketReader(const SocketReader&) = delete;
    SocketReader& operator=(const SocketReader&) = delete;

    /// 更新单次 `recv` 的预算（`Duration::max()` 表示不限时）。
    void set_timeout(Socket::Duration timeout) noexcept { m_timeout = timeout; }

    /// 设置整次协议的绝对截止时间；到期后 `recv` 以 0 预算（非阻塞）尝试一次。
    void SetDeadline(std::chrono::steady_clock::time_point deadline) noexcept {
        m_deadline = deadline;
        m_has_deadline = true;
    }

    /// 取消绝对截止时间，回到只用 `set_timeout()` 预算的模式。
    void ClearDeadline() noexcept { m_has_deadline = false; }

    /**
     * @brief 把 `bytes` 插到 socket 数据之前交付。
     * @note 只应在开始读之前调用；重复调用会**覆盖**尚未消费的预置数据。
     */
    void Prepend(std::string bytes) {
        m_pending = std::move(bytes);
        m_pending_offset = 0;
    }

    /// 最近一次失败对应的 errno（0 表示没有系统错误）。
    int last_system_error() const noexcept { return m_last_error; }

    /// 实际调用 `Socket::recv` 的次数（基准/回归断言用）。
    std::uint64_t recv_calls() const noexcept { return m_recv_calls; }

    /// 当前尚未交付的预置字节数。
    std::size_t pending_bytes() const noexcept {
        return m_pending.size() - m_pending_offset;
    }

    /**
     * @brief 满足 `streams::Reader` 契约的读。
     * @return 成功时 `count > 0`；对端关闭返回 `(0, IoEof())`；超时/取消/
     *         其它系统错误返回 `(0, 带文本错误)`，原始 errno 见
     *         `last_system_error()`。
     */
    streams::ReadResult Read(char* buffer, std::size_t size) {
        if (size == 0) {
            return streams::ReadResult{0, {}};
        }
        if (buffer == nullptr) {
            return streams::ReadResult{0, detail::SocketErrorFromErrno(EINVAL, "recv")};
        }
        // 预置字节优先：握手时 Parser 多读的首帧必须排在 socket 数据之前。
        if (m_pending_offset < m_pending.size()) {
            const std::size_t available = m_pending.size() - m_pending_offset;
            const std::size_t take = std::min(available, size);
            std::memcpy(buffer, m_pending.data() + m_pending_offset, take);
            m_pending_offset += take;
            if (m_pending_offset == m_pending.size()) {
                m_pending.clear();
                m_pending_offset = 0;
            }
            return streams::ReadResult{take, {}};
        }
        if (!m_socket) {
            return streams::ReadResult{0, streams::IoEof()};
        }
        ++m_recv_calls;
        m_last_error = 0;
        const auto timeout =
            detail::EffectiveTimeout(m_timeout, m_has_deadline, m_deadline);
        const SocketResult result = m_socket->recv(buffer, size, 0, timeout);
        if (result.ok()) {
            if (result.transferred == 0) {
                return streams::ReadResult{0, streams::IoEof()};
            }
            return streams::ReadResult{
                static_cast<std::size_t>(result.transferred), {}};
        }
        m_last_error = result.error;
        if (result.error == ETIMEDOUT) {
            return streams::ReadResult{
                0, detail::SocketErrorFromErrno(result.error, "recv timeout")};
        }
        if (result.error == ECANCELED) {
            return streams::ReadResult{
                0, detail::SocketErrorFromErrno(result.error, "recv canceled")};
        }
        if (result.error == EBADF || result.error == ENOTCONN ||
            result.error == ECONNRESET || result.error == EPIPE) {
            // 对端/本端已关闭：按 io.EOF 语义交付，上层据此结束连接。
            return streams::ReadResult{0, streams::IoEof()};
        }
        return streams::ReadResult{
            0, detail::SocketErrorFromErrno(result.error, "recv")};
    }

private:
    Socket::Ptr m_socket;      ///< 底层 socket（保持存活，不拥有语义上的连接）
    Socket::Duration m_timeout; ///< 单次 recv 预算
    std::string m_pending;      ///< 预置字节（握手多读）
    std::size_t m_pending_offset{0}; ///< 预置字节读游标
    bool m_has_deadline{false}; ///< 是否启用绝对截止时间
    std::chrono::steady_clock::time_point m_deadline{}; ///< 绝对截止时间
    std::uint64_t m_recv_calls{0}; ///< Socket::recv 调用次数
    int m_last_error{0};        ///< 最近一次失败的 errno
};

/**
 * @brief 把 `Socket` 适配成 `bufio::Writer` 的底层：单写 + 批量写 + 零拷贝。
 *
 * @details `Write` 用于「大块直写」快路径与缓冲未满时的单条消息；
 *   `WriteBatch` 供 `bufio::Writer::FlushBatch()` 一次提交多个段。
 */
class SocketBatchWriter final {
public:
    /// @param socket 已连接的 socket。
    /// @param timeout 单次写调用的预算。
    /// @param datagram true 时 `WriteBatch` 走 `sendmmsg`，每个段一条数据报。
    explicit SocketBatchWriter(Socket::Ptr socket,
                               Socket::Duration timeout = Socket::Duration::max(),
                               bool datagram = false)
        : m_socket(std::move(socket)), m_timeout(timeout), m_datagram(datagram) {}

    SocketBatchWriter(const SocketBatchWriter&) = delete;
    SocketBatchWriter& operator=(const SocketBatchWriter&) = delete;

    void set_timeout(Socket::Duration timeout) noexcept { m_timeout = timeout; }

    /// 设置整次协议的绝对截止时间。
    void SetDeadline(std::chrono::steady_clock::time_point deadline) noexcept {
        m_deadline = deadline;
        m_has_deadline = true;
    }

    void ClearDeadline() noexcept { m_has_deadline = false; }

    /// 是否为数据报模式（`WriteBatch` → `sendmmsg`）。
    bool datagram() const noexcept { return m_datagram; }

    /// 最近一次失败对应的 errno（0 表示没有系统错误）。
    int last_system_error() const noexcept { return m_last_error; }

    /// `Socket::send` 调用次数（单条/大块直写）。
    std::uint64_t send_calls() const noexcept { return m_send_calls; }
    /// `Socket::sendv`（`writev`）调用次数（TCP 批量）。
    std::uint64_t writev_calls() const noexcept { return m_writev_calls; }
    /// `Socket::send_batch`（`sendmmsg`）调用次数（UDP 批量）。
    std::uint64_t sendmmsg_calls() const noexcept { return m_sendmmsg_calls; }
    /// `Socket::send_file`（`sendfile`）调用次数。
    std::uint64_t sendfile_calls() const noexcept { return m_sendfile_calls; }
    /// 已提交的字节数（含部分写）。
    std::uint64_t bytes_written() const noexcept { return m_bytes_written; }

    /**
     * @brief 单次写（`streams::Writer` 契约）。
     * @return 已写入字节数；失败时返回已写部分与错误。
     */
    streams::WriteResult Write(const char* data, std::size_t size) {
        if (size == 0) {
            return streams::WriteResult{0, {}};
        }
        if (!m_socket) {
            return streams::WriteResult{
                0, detail::SocketErrorFromErrno(EBADF, "send")};
        }
        ++m_send_calls;
        m_last_error = 0;
        const auto timeout =
            detail::EffectiveTimeout(m_timeout, m_has_deadline, m_deadline);
        const SocketResult result = m_socket->send(data, size, 0, timeout);
        if (result.ok()) {
            const auto count = static_cast<std::size_t>(result.transferred);
            m_bytes_written += count;
            return streams::WriteResult{count, {}};
        }
        m_last_error = result.error;
        const std::size_t written = result.transferred > 0
                                        ? static_cast<std::size_t>(result.transferred)
                                        : 0;
        m_bytes_written += written;
        return streams::WriteResult{
            written, detail::SocketErrorFromErrno(result.error, "send")};
    }

    /**
     * @brief 批量写（`bufio::Writer` 批量 flush 契约）。
     * @details TCP 走 `sendv`（`writev`）；UDP 走 `send_batch`（`sendmmsg`）。
     *          UDP 的返回值换算成「前 k 段总字节数」，与 `bufio::Writer`
     *          按字节消费的部分写语义对齐。
     */
    streams::WriteResult WriteBatch(const bufio::BatchEntry* entries,
                                    std::size_t count) {
        if (entries == nullptr || count == 0) {
            return streams::WriteResult{0, {}};
        }
        if (!m_socket) {
            return streams::WriteResult{
                0, detail::SocketErrorFromErrno(EBADF, "send")};
        }
        if (m_datagram) {
            return WriteBatchDatagram(entries, count);
        }
        // TCP/Unix 流：writev 一次提交所有段（段来自同一块缓冲，逻辑上是
        // 一次 write，但不需要把「头 + 体」拷贝成一个字符串）。
        m_iovecs.clear();
        m_iovecs.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (entries[i].data == nullptr || entries[i].size == 0) {
                continue;
            }
            iovec entry{};
            entry.iov_base = const_cast<char*>(entries[i].data);
            entry.iov_len = entries[i].size;
            m_iovecs.push_back(entry);
        }
        if (m_iovecs.empty()) {
            return streams::WriteResult{0, {}};
        }
        ++m_writev_calls;
        m_last_error = 0;
        const auto timeout =
            detail::EffectiveTimeout(m_timeout, m_has_deadline, m_deadline);
        const SocketResult result =
            m_socket->sendv(m_iovecs.data(), m_iovecs.size(), 0, timeout);
        if (result.ok()) {
            const auto written = static_cast<std::size_t>(result.transferred);
            m_bytes_written += written;
            return streams::WriteResult{written, {}};
        }
        m_last_error = result.error;
        const std::size_t written = result.transferred > 0
                                        ? static_cast<std::size_t>(result.transferred)
                                        : 0;
        m_bytes_written += written;
        return streams::WriteResult{
            written, detail::SocketErrorFromErrno(result.error, "sendv")};
    }

    /**
     * @brief 零拷贝发送文件内容（`sendfile(2)`）。
     * @note 调用前必须先把 `bufio::Writer` 中尚未落地的字节 `Flush`/`FlushBatch`
     *       干净，否则文件内容会插到响应头之前。
     */
    SocketResult SendFile(int in_fd, off_t* offset, std::size_t count) {
        if (!m_socket) {
            return SocketResult{-1, EBADF};
        }
        ++m_sendfile_calls;
        m_last_error = 0;
        const auto timeout =
            detail::EffectiveTimeout(m_timeout, m_has_deadline, m_deadline);
        const SocketResult result =
            m_socket->send_file(in_fd, offset, count, timeout);
        if (result.ok()) {
            m_bytes_written += static_cast<std::size_t>(result.transferred);
        } else {
            m_last_error = result.error;
        }
        return result;
    }

    /**
     * @brief 显式目标地址的批量发送（UDP `sendmmsg`，支持未 connect 的 socket）。
     * @return 成功提交给内核的**条数**；部分成功时调用方应从
     *         `[transferred, count)` 继续。
     * @note 这是给「一个 socket 对 N 个 peer」的上层（如 UdpTransport /
     *       基准）预留的旁路，不经过 `bufio::Writer` 的字节流缓冲。
     */
    SocketResult SendToBatch(const SendBatchEntry* entries, std::size_t count,
                             int flags = 0) {
        if (!m_socket || entries == nullptr || count == 0) {
            return SocketResult{-1, EINVAL};
        }
        ++m_sendmmsg_calls;
        m_last_error = 0;
        const auto timeout =
            detail::EffectiveTimeout(m_timeout, m_has_deadline, m_deadline);
        const SocketResult result =
            m_socket->send_batch(entries, count, flags, timeout);
        if (result.ok()) {
            for (ssize_t i = 0; i < result.transferred; ++i) {
                m_bytes_written += entries[i].length;
            }
        } else {
            m_last_error = result.error;
        }
        return result;
    }

    /// 设置 UDP 批量写使用的固定目标地址（为空表示用 `connect` 的对端）。
    void set_datagram_target(const sockaddr* target, socklen_t length) noexcept {
        m_datagram_target = target;
        m_datagram_target_length = length;
    }

private:
    /// 数据报批量：一条 entry 一条数据报，返回「前 k 段字节数」。
    streams::WriteResult WriteBatchDatagram(const bufio::BatchEntry* entries,
                                            std::size_t count) {
        m_datagrams.clear();
        m_datagrams.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (entries[i].data == nullptr) {
                continue;
            }
            SendBatchEntry entry{};
            entry.data = entries[i].data;
            entry.length = entries[i].size;
            entry.to = m_datagram_target;  ///< 为空表示已 connect 的对端
            entry.to_length = m_datagram_target_length;
            m_datagrams.push_back(entry);
        }
        if (m_datagrams.empty()) {
            return streams::WriteResult{0, {}};
        }
        ++m_sendmmsg_calls;
        m_last_error = 0;
        const auto timeout =
            detail::EffectiveTimeout(m_timeout, m_has_deadline, m_deadline);
        const SocketResult result =
            m_socket->send_batch(m_datagrams.data(), m_datagrams.size(), 0, timeout);
        if (!result.error && result.transferred >= 0) {
            // 条数 → 字节数，供 bufio::Writer 的 ConsumeFront 消费前 k 段。
            std::size_t bytes = 0;
            for (ssize_t i = 0; i < result.transferred; ++i) {
                bytes += m_datagrams[static_cast<std::size_t>(i)].length;
            }
            m_bytes_written += bytes;
            return streams::WriteResult{bytes, {}};
        }
        m_last_error = result.error;
        return streams::WriteResult{
            0, detail::SocketErrorFromErrno(result.error, "sendmmsg")};
    }

    Socket::Ptr m_socket;        ///< 底层 socket
    Socket::Duration m_timeout;  ///< 单次写预算
    bool m_datagram{false};      ///< 数据报模式
    bool m_has_deadline{false};  ///< 是否启用绝对截止时间
    std::chrono::steady_clock::time_point m_deadline{}; ///< 绝对截止时间
    const sockaddr* m_datagram_target{nullptr}; ///< UDP 固定目标（可空）
    socklen_t m_datagram_target_length{0};      ///< 目标地址长度
    std::vector<iovec> m_iovecs;            ///< TCP 批量复用的 iovec 数组
    std::vector<SendBatchEntry> m_datagrams; ///< UDP 批量复用的条目数组
    std::uint64_t m_send_calls{0};     ///< Socket::send 次数
    std::uint64_t m_writev_calls{0};   ///< Socket::sendv 次数
    std::uint64_t m_sendmmsg_calls{0}; ///< Socket::send_batch 次数
    std::uint64_t m_sendfile_calls{0}; ///< Socket::send_file 次数
    std::uint64_t m_bytes_written{0};  ///< 已提交字节数
    int m_last_error{0};               ///< 最近一次失败 errno
};

/// 便捷别名：TCP 连接的缓冲读端。
using BufferedSocketReader = bufio::Reader<SocketReader>;
/// 便捷别名：TCP 连接的批量写端。
using BufferedSocketWriter = bufio::Writer<SocketBatchWriter>;

/**
 * @brief 为 `bufio::Writer` 套用项目统一的批量默认值（kHybrid + 16）。
 *
 * @details 任务要求「默认 kHybrid + max_batch 16」：协议层的小消息
 *   （HTTP 头+体、WebSocket 帧头+体、SSE chunk）通常 2~4 段，
 *   16 段足够把多个响应/事件合并，又不会让尾部延迟不可控。
 *   `threshold` 跟随缓冲容量，`interval` 保持 bufio 默认的 100 µs。
 */
inline void ApplyProtocolBatchDefaults(bufio::Writer<SocketBatchWriter>& writer) {
    bufio::BatchConfig config;
    config.policy = bufio::FlushPolicy::kHybrid;
    config.max_batch = 16;
    config.threshold = writer.Size();
    config.interval = std::chrono::microseconds{100};
    writer.SetBatchConfig(config);
}

}  // namespace go2cpp::net
