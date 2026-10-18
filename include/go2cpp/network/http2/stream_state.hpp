/**
 * @file stream_state.hpp
 * @brief HTTP/2 流状态机（RFC 9113 §5.1）。
 *
 * ## 为什么单独抽出来
 *
 * 流状态机是 HTTP/2 里**最容易写错、也最难在集成测试里定位**的部分：它的非法
 * 转换不会崩，只会让连接在某个后续请求上莫名其妙地报协议错误。所以把它做成
 * **纯逻辑、无 IO、无锁**的独立组件，用穷举测试覆盖全部转换。
 *
 * ## 状态与转换（RFC 9113 §5.1）
 *
 * ```
 *                +--------+
 *          send PP|        |recv PP
 *         +--------+  idle  +--------+
 *         |        |        |        |
 *         v        +--------+        v
 *  +----------+          |     +----------+
 *  | reserved |          |     | reserved |
 *  | (local)  |          |     | (remote) |
 *  +----------+          |     +----------+
 *       |                |          |
 *  send H /             |          | recv H
 *       v                |          v
 *  +----------+          |     +----------+
 *  |half-closed|<---------+---->|half-closed|
 *  | (remote) |          |     |  (local)  |
 *  +----------+          |     +----------+
 *       |                |          |
 *  recv ES               |      send ES
 *       v                |          v
 *    +------+            |       +------+
 *    |closed|<-----------+------>|closed|
 *    +------+   send/recv RST     +------+
 * ```
 *
 * ## 本实现覆盖的规则
 *
 * - `idle` 只能被 `HEADERS` 或 `PUSH_PROMISE` 打开；
 * - `reserved(*)` 只能由对应的 `HEADERS` 转入 `half-closed(*)`；
 * - 在 `half-closed(local)` 上**不能**再发数据，在 `half-closed(remote)` 上
 *   **不能**再接数据；
 * - 任一侧发 `END_STREAM` 后，该方向永久关闭；
 * - 任何状态收到或发出 `RST_STREAM` 都直接进入 `closed`；
 * - `closed` 之后所有事件都返回 `kStreamClosed`（上层据此决定是报错还是忽略）。
 */
#pragma once

#include <cstdint>

namespace go2cpp::net::http2 {

/** @brief 流状态（RFC 9113 §5.1）。 */
enum class StreamState : std::uint8_t {
    kIdle,
    kReservedLocal,
    kReservedRemote,
    kOpen,
    kHalfClosedLocal,
    kHalfClosedRemote,
    kClosed,
};

const char* ToString(StreamState state) noexcept;

/** @brief 状态机事件的处理结果。 */
enum class StreamEventResult : std::uint8_t {
    kOk,
    /// 该事件在当前状态下不合法（属于协议错误）。
    kInvalidTransition,
    /// 流已关闭；上层可据此选择忽略或报 STREAM_CLOSED。
    kStreamClosed,
};

const char* ToString(StreamEventResult result) noexcept;

/**
 * @brief 单条流的 HTTP/2 状态机。
 * @details 无锁、无 IO；**非线程安全**：同一个实例同一时刻只允许一个
 *          线程/Fiber 驱动（上层 `Connection` 也按单驱动者使用）。可以在
 *          任意线程上使用，但不能被多个线程并发调用；需要并发访问时由
 *          调用方加锁。每次转换都返回明确结果，便于上层决定是发
 *          RST_STREAM 还是断开连接。
 */
class StreamStateMachine final {
public:
    explicit StreamStateMachine(std::uint32_t stream_id) noexcept
        : m_stream_id(stream_id) {}

    std::uint32_t id() const noexcept { return m_stream_id; }
    StreamState state() const noexcept { return m_state; }
    bool closed() const noexcept { return m_state == StreamState::kClosed; }

    // ---- 本地发起（发送方向）----

    /** @brief 发送 HEADERS（可带 END_STREAM）。idle -> open / half-closed(local)。 */
    StreamEventResult SendHeaders(bool end_stream) noexcept;

    /**
     * @brief 只读预检：当前状态是否允许发送 DATA。
     * @details 供上层在**推进状态之前**完成帧尺寸/流控校验，避免
     *          "校验失败但 END_STREAM 已被消耗"的状态脱节。
     */
    bool CanSendData() const noexcept;

    /** @brief 发送 DATA（可带 END_STREAM）。 */
    StreamEventResult SendData(bool end_stream) noexcept;

    /** @brief 发送 PUSH_PROMISE（仅 idle 合法）。idle -> reserved(local)。 */
    StreamEventResult SendPushPromise() noexcept;

    /** @brief 发送 RST_STREAM：任何状态 -> closed。 */
    StreamEventResult SendRstStream() noexcept;

    // ---- 对端发起（接收方向）----

    /** @brief 接收 HEADERS。idle -> open 或 reserved(remote) -> half-closed(local)。 */
    StreamEventResult ReceiveHeaders(bool end_stream) noexcept;

    /** @brief 接收 DATA。 */
    StreamEventResult ReceiveData(bool end_stream) noexcept;

    /** @brief 接收 PUSH_PROMISE。idle -> reserved(remote)。 */
    StreamEventResult ReceivePushPromise() noexcept;

    /** @brief 接收 RST_STREAM：任何状态 -> closed。 */
    StreamEventResult ReceiveRstStream() noexcept;

    /**
     * @brief 本地原因（如写帧失败）强制关闭该流。
     * @details 不发送 RST_STREAM，只把本地状态置为 closed；用于传输已
     *          不可用、无法再与对端同步的场合。
     */
    void ForceClose() noexcept;

private:
    // 注意：以下三个字段都是普通值，没有任何同步；本类按文档约定
    // 由单一驱动者访问（非线程安全），不要把它们改成"看起来原子"的写法
    // 而保留读改写序列。
    std::uint32_t m_stream_id;
    StreamState m_state{StreamState::kIdle};
    /// 本地是否已发 END_STREAM（用于区分 half-closed 方向）。
    bool m_local_end{false};
    /// 对端是否已发 END_STREAM。
    bool m_remote_end{false};

    /// 依据两侧 END_STREAM 标记重算 open/half-closed/closed。
    void RefreshFromEndFlags() noexcept;
};

}  // namespace go2cpp::net::http2
