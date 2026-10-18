/**
 * @file stream_state.cpp
 * @brief HTTP/2 流状态机实现（RFC 9113 §5.1）。
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

StreamEventResult StreamStateMachine::SendHeaders(bool end_stream) noexcept {
    if (m_state == StreamState::kClosed) {
        return StreamEventResult::kStreamClosed;
    }
    // idle -> open（首次发送 HEADERS）
    if (m_state == StreamState::kIdle) {
        m_local_end = end_stream;
        m_remote_end = false;
        RefreshFromEndFlags();
        return StreamEventResult::kOk;
    }
    // reserved(local) 只能由 PUSH_PROMISE 产生；此时发送 HEADERS 是允许的，
    // 用来把 promised stream 打开（RFC 9113 §5.1 的 reserved(local) -> half-closed(remote)）
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

void StreamStateMachine::ForceClose() noexcept {
    m_local_end = true;
    m_remote_end = true;
    m_state = StreamState::kClosed;
}

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
