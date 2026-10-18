/**
 * @file stream_state.cpp
 * @brief HTTP/2 流状态机实现（RFC 9113 §5.1）。
 *
 * ## 模块定位
 * `stream_state.hpp` 的唯一实现：把 RFC 9113 §5.1 的转化规则写成显式分支。
 * 无 IO、无锁、不分配内存，因此可被状态机测试穷举全部输入组合。
 *
 * ## 本文件定义什么
 * - `ToString(StreamState)` / `ToString(StreamEventResult)`：日志与断言的文案；
 * - `RefreshFromEndFlags()`：由两侧 END_STREAM 标记重算 open/half-closed/closed；
 * - 全部 `Send*` / `Receive*` / `ForceClose()` 的转化实现。
 *
 * ## 阅读建议
 * 先看 `RefreshFromEndFlags()`（所有落点的结算函数），再逐个看 `Send*`/`Receive*`
 * 的分支；对照 `docs/state-machines/Http2StreamState.md` 的转化表阅读。
 *
 * ## 边界与陷阱
 * - `reserved(*)` 不参与 END_STREAM 结算，是唯一「直接赋值」的状态；
 * - 失败分支必须**不改状态**（预检语义），否则上层重试会看到被污染的流；
 * - `idle` 上收发 RST_STREAM 都是协议错误，不是「直接关闭」。
 */
#include "go2cpp/network/http2/stream_state.hpp"

namespace go2cpp::net::http2 {

const char* ToString(StreamState state) noexcept {
    switch (state) {
        case StreamState::kIdle: return "idle";
        case StreamState::kReservedLocal: return "reserved(local)";
        case StreamState::kReservedRemote: return "reserved(remote)";
        case StreamState::kOpen: return "open";
        case StreamState::kHalfClosedLocal: return "half-closed(local)";
        case StreamState::kHalfClosedRemote: return "half-closed(remote)";
        case StreamState::kClosed: return "closed";
    }
    return "unknown";
}

const char* ToString(StreamEventResult result) noexcept {
    switch (result) {
        case StreamEventResult::kOk: return "ok";
        case StreamEventResult::kInvalidTransition: return "invalid-transition";
        case StreamEventResult::kStreamClosed: return "stream-closed";
    }
    return "unknown";
}

void StreamStateMachine::RefreshFromEndFlags() noexcept {
    if (m_local_end && m_remote_end) {
        m_state = StreamState::kClosed;
    } else if (m_local_end) {
        m_state = StreamState::kHalfClosedLocal;
    } else if (m_remote_end) {
        m_state = StreamState::kHalfClosedRemote;
    } else {
        m_state = StreamState::kOpen;
    }
}

/**
 * @brief 发送 HEADERS（可带 END_STREAM）。
 *
 * ## 步骤
 * step1. `kClosed` -> `kStreamClosed`；
 * step2. `kIdle` -> 置两侧标记后重算（open 或 half-closed(local)）；
 * step3. `kReservedLocal` -> 打开 promised stream（对端方向视为已结束）；
 * step4. `kOpen` / `kHalfClosedRemote` -> trailers 语义，带 END_STREAM 时更新标记；
 * step5. 其余 -> `kInvalidTransition`。
 */
StreamEventResult StreamStateMachine::SendHeaders(bool end_stream) noexcept {
    if (m_state == StreamState::kClosed) {
        return StreamEventResult::kStreamClosed;
    }
    // idle -> open（首次发送 HEADERS）
    // step2: idle 只能由 HEADERS 打开。
    if (m_state == StreamState::kIdle) {
        m_local_end = end_stream;
        m_remote_end = false;
        RefreshFromEndFlags();
        return StreamEventResult::kOk;
    }
    // reserved(local) 只能由 PUSH_PROMISE 产生；此时发送 HEADERS 是允许的，
    // 用来把 promised stream 打开（RFC 9113 §5.1 的 reserved(local) -> half-closed(remote)）
    // step3: reserved(local) 用 HEADERS 打开 promised stream。
    if (m_state == StreamState::kReservedLocal) {
        m_local_end = end_stream;
        m_remote_end = true;   // promised stream 对端只能接收
        RefreshFromEndFlags();
        return StreamEventResult::kOk;
    }
    // 已开流上再发 HEADERS 属于 trailers（合法，但不改变状态，除非带 END_STREAM）
    if (m_state == StreamState::kOpen) {
        if (end_stream && !m_local_end) {
            m_local_end = true;
            RefreshFromEndFlags();
        }
        return StreamEventResult::kOk;
    }
    // half-closed(local)：本地已关闭，不能再发
    // half-closed(remote)/reserved(remote)：本地仍可发 trailers
    if (m_state == StreamState::kHalfClosedRemote) {
        if (end_stream && !m_local_end) {
            m_local_end = true;
            RefreshFromEndFlags();
        }
        return StreamEventResult::kOk;
    }
    if (m_state == StreamState::kHalfClosedLocal) {
        return StreamEventResult::kInvalidTransition;
    }
    // reserved(remote)：本端不能发 HEADERS
    return StreamEventResult::kInvalidTransition;
}

bool StreamStateMachine::CanSendData() const noexcept {
    return m_state == StreamState::kOpen ||
           m_state == StreamState::kHalfClosedRemote;
}

/**
 * @brief 本地强制关闭（传输已不可用，无法与对端同步）。
 *
 * ## 说明
 * 不发送 RST_STREAM，只把两侧标记置真并直接落到 `kClosed`；
 * 用于写帧失败或流控额度提交失败等「线上状态已不可信」的场合。
 */
void StreamStateMachine::ForceClose() noexcept {
    m_local_end = true;
    m_remote_end = true;
    m_state = StreamState::kClosed;
}

/**
 * @brief 发送 DATA（可带 END_STREAM）。
 *
 * ## 步骤
 * step1. `kClosed` -> `kStreamClosed`；
 * step2. `CanSendData()` 为假 -> `kInvalidTransition`；
 * step3. 带 END_STREAM 时置 `m_local_end` 并重算状态。
 */
StreamEventResult StreamStateMachine::SendData(bool end_stream) noexcept {
    if (m_state == StreamState::kClosed) {
        return StreamEventResult::kStreamClosed;
    }
    // 只有 open 与 half-closed(remote) 允许本地发送数据
    if (!CanSendData()) {
        return StreamEventResult::kInvalidTransition;
    }
    if (end_stream && !m_local_end) {
        m_local_end = true;
        RefreshFromEndFlags();
    }
    return StreamEventResult::kOk;
}

StreamEventResult StreamStateMachine::SendPushPromise() noexcept {
    if (m_state == StreamState::kClosed) {
        return StreamEventResult::kStreamClosed;
    }
    if (m_state != StreamState::kIdle) {
        return StreamEventResult::kInvalidTransition;
    }
    m_state = StreamState::kReservedLocal;
    return StreamEventResult::kOk;
}

StreamEventResult StreamStateMachine::SendRstStream() noexcept {
    // RFC 9113 §5.1：idle 上除 HEADERS/PRIORITY 外的帧都是协议错误，
    // 所以 idle 不能直接发 RST_STREAM。
    if (m_state == StreamState::kIdle) {
        return StreamEventResult::kInvalidTransition;
    }
    if (m_state == StreamState::kClosed) {
        return StreamEventResult::kStreamClosed;
    }
    m_state = StreamState::kClosed;
    return StreamEventResult::kOk;
}

/**
 * @brief 接收 HEADERS（可能是首帧或 trailers）。
 *
 * ## 步骤
 * step1. `kClosed` -> `kStreamClosed`；
 * step2. `kIdle` -> 由对端 HEADERS 打开；
 * step3. `kReservedRemote` -> 打开 promised stream（本地方向视为已结束）；
 * step4. `kOpen` / `kHalfClosedLocal` -> trailers 语义；
 * step5. `kHalfClosedRemote` / `kReservedLocal` -> `kInvalidTransition`。
 */
StreamEventResult StreamStateMachine::ReceiveHeaders(bool end_stream) noexcept {
    if (m_state == StreamState::kClosed) {
        return StreamEventResult::kStreamClosed;
    }
    if (m_state == StreamState::kIdle) {
        m_local_end = false;
        m_remote_end = end_stream;
        RefreshFromEndFlags();
        return StreamEventResult::kOk;
    }
    // step3: 对端发起的 promised stream 由 HEADERS 打开。
    if (m_state == StreamState::kReservedRemote) {
        // reserved(remote) -> half-closed(local)
        m_local_end = true;
        m_remote_end = end_stream;
        RefreshFromEndFlags();
        return StreamEventResult::kOk;
    }
    if (m_state == StreamState::kOpen || m_state == StreamState::kHalfClosedLocal) {
        if (end_stream && !m_remote_end) {
            m_remote_end = true;
            RefreshFromEndFlags();
        }
        return StreamEventResult::kOk;
    }
    // half-closed(remote)：对端已关闭，不能再收
    // reserved(local)：本端发起的 promised stream，不能收 HEADERS
    return StreamEventResult::kInvalidTransition;
}

/**
 * @brief 接收 DATA（可带 END_STREAM）。
 *
 * ## 步骤
 * step1. `kClosed` -> `kStreamClosed`；
 * step2. 只有 `kOpen` / `kHalfClosedLocal` 允许收数据；
 * step3. 带 END_STREAM 时置 `m_remote_end` 并重算状态。
 */
StreamEventResult StreamStateMachine::ReceiveData(bool end_stream) noexcept {
    if (m_state == StreamState::kClosed) {
        return StreamEventResult::kStreamClosed;
    }
    // 只有 open 与 half-closed(local) 允许接收数据
    if (m_state != StreamState::kOpen && m_state != StreamState::kHalfClosedLocal) {
        return StreamEventResult::kInvalidTransition;
    }
    if (end_stream && !m_remote_end) {
        m_remote_end = true;
        RefreshFromEndFlags();
    }
    return StreamEventResult::kOk;
}

StreamEventResult StreamStateMachine::ReceivePushPromise() noexcept {
    if (m_state == StreamState::kClosed) {
        return StreamEventResult::kStreamClosed;
    }
    if (m_state != StreamState::kIdle) {
        return StreamEventResult::kInvalidTransition;
    }
    m_state = StreamState::kReservedRemote;
    return StreamEventResult::kOk;
}

StreamEventResult StreamStateMachine::ReceiveRstStream() noexcept {
    // 同上：idle 上收到 RST_STREAM 是协议错误。
    if (m_state == StreamState::kIdle) {
        return StreamEventResult::kInvalidTransition;
    }
    if (m_state == StreamState::kClosed) {
        return StreamEventResult::kStreamClosed;
    }
    m_state = StreamState::kClosed;
    return StreamEventResult::kOk;
}

}  // namespace go2cpp::net::http2
