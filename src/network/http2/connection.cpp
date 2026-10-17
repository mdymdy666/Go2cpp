/**
 * @file connection.cpp
 * @brief HTTP/2 连接层实现。
 */
#include "go2cpp/network/http2/connection.hpp"

#include <algorithm>
#include <cstring>

namespace go2cpp::net::http2 {

namespace {

/// 客户端连接前言（RFC 9113 §3.4）。
constexpr char kClientPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr std::size_t kClientPrefaceLength = sizeof(kClientPreface) - 1;

}  // namespace

const char* ToString(ConnStatus status) noexcept {
    switch (status) {
        case ConnStatus::kOk: return "ok";
        case ConnStatus::kClosed: return "closed";
        case ConnStatus::kProtocolError: return "protocol-error";
        case ConnStatus::kUnsupported: return "unsupported";
    }
    return "unknown";
}

Connection::Connection(Transport* transport, Role role, ConnectionConfig config)
    : m_transport(transport),
      m_role(role),
      m_config(config),
      m_peer_max_frame_size(config.max_frame_size),
      m_decoder(config.header_table_size),
      m_encoder(config.header_table_size) {}

bool Connection::FillBuffer(std::size_t need, std::string* error) {
    if (m_transport == nullptr) {
        if (error != nullptr) *error = "Transport 为空";
        return false;
    }
    while (m_read_buffer.size() < need) {
        std::uint8_t chunk[4096];
        const std::size_t got = m_transport->Read(chunk, sizeof(chunk));
        if (got == 0) {
            if (error != nullptr) *error = "对端关闭或读取失败";
            return false;
        }
        m_read_buffer.insert(m_read_buffer.end(), chunk, chunk + got);
    }
    return true;
}

ConnStatus Connection::TakeFrame(Frame* out, std::string* error) {
    // 先保证帧头
    if (!FillBuffer(kFrameHeaderSize, error)) {
        return ConnStatus::kClosed;
    }
    FrameHeader header;
    if (DecodeFrameHeader(m_read_buffer.data(), kFrameHeaderSize, &header) !=
        FrameStatus::kOk) {
        if (error != nullptr) *error = "帧头解析失败";
        return ConnStatus::kProtocolError;
    }
    const std::size_t total = kFrameHeaderSize + header.length;
    if (!FillBuffer(total, error)) {
        return ConnStatus::kClosed;
    }
    std::size_t consumed = 0;
    const auto status =
        DecodeFrame(m_read_buffer.data(), total, out, &consumed, m_config.max_frame_size);
    if (status != FrameStatus::kOk) {
        if (error != nullptr) {
            *error = "帧解析失败: ";
            *error += ToString(status);
        }
        return status == FrameStatus::kFrameSizeError ? ConnStatus::kProtocolError
                                                      : ConnStatus::kProtocolError;
    }
    m_read_buffer.erase(m_read_buffer.begin(),
                        m_read_buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
    return ConnStatus::kOk;
}

ConnStatus Connection::WriteFrame(const Frame& frame, std::string* error) {
    if (m_transport == nullptr) {
        if (error != nullptr) *error = "Transport 为空";
        return ConnStatus::kClosed;
    }
    std::vector<std::uint8_t> bytes(kFrameHeaderSize + frame.payload.size());
    EncodeFrameHeader(frame.header, bytes.data());
    if (!frame.payload.empty()) {
        std::memcpy(bytes.data() + kFrameHeaderSize, frame.payload.data(), frame.payload.size());
    }
    const std::size_t written = m_transport->Write(bytes.data(), bytes.size());
    if (written != bytes.size()) {
        if (error != nullptr) *error = "写帧失败";
        return ConnStatus::kClosed;
    }
    return ConnStatus::kOk;
}

StreamStateMachine* Connection::StreamFor(std::uint32_t stream_id) {
    const auto it = m_streams.find(stream_id);
    if (it == m_streams.end()) {
        return nullptr;
    }
    return &it->second;
}

ConnStatus Connection::Handshake(std::string* error) {
    if (m_transport == nullptr) {
        if (error != nullptr) *error = "Transport 为空";
        return ConnStatus::kClosed;
    }
    if (m_handshaked) {
        return ConnStatus::kOk;
    }
    if (m_role == Role::kClient) {
        // 1) 发送 preface（直接写字节，不是帧）
        const std::size_t written = m_transport->Write(
            reinterpret_cast<const std::uint8_t*>(kClientPreface), kClientPrefaceLength);
        if (written != kClientPrefaceLength) {
            if (error != nullptr) *error = "写 preface 失败";
            return ConnStatus::kClosed;
        }
    } else {
        // 服务端：先读 preface
        if (!FillBuffer(kClientPrefaceLength, error)) {
            return ConnStatus::kClosed;
        }
        if (std::memcmp(m_read_buffer.data(), kClientPreface, kClientPrefaceLength) != 0) {
            if (error != nullptr) *error = "客户端前言不匹配";
            return ConnStatus::kProtocolError;
        }
        m_read_buffer.erase(m_read_buffer.begin(),
                            m_read_buffer.begin() +
                                static_cast<std::ptrdiff_t>(kClientPrefaceLength));
    }

    // 2) 发送本地 SETTINGS
    const std::vector<SettingsEntry> local_settings = {
        {static_cast<std::uint16_t>(SettingsId::kHeaderTableSize), m_config.header_table_size},
        {static_cast<std::uint16_t>(SettingsId::kMaxFrameSize), m_config.max_frame_size},
        {static_cast<std::uint16_t>(SettingsId::kInitialWindowSize),
         m_config.initial_window_size},
        {static_cast<std::uint16_t>(SettingsId::kMaxConcurrentStreams),
         m_config.max_concurrent_streams},
    };
    auto status = WriteFrame(MakeSettings(local_settings), error);
    if (status != ConnStatus::kOk) {
        return status;
    }
    m_local_settings_sent = true;

    // 3) 等对端 SETTINGS 并发 ACK
    return FinishSettingsHandshake(error);
}

ConnStatus Connection::FinishSettingsHandshake(std::string* error) {
    if (m_peer_settings_received) {
        m_handshaked = true;
        return ConnStatus::kOk;
    }
    for (;;) {
        Frame frame;
        auto status = TakeFrame(&frame, error);
        if (status != ConnStatus::kOk) {
            return status;
        }
        if (frame.header.type == FrameType::kSettings) {
            SettingsFrame settings;
            if (ParseSettings(frame, &settings, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            if (settings.ack) {
                // 对端确认了我们的 SETTINGS
                continue;
            }
            // 应用对端 SETTINGS 中的关键值
            for (const auto& entry : settings.entries) {
                switch (static_cast<SettingsId>(entry.id)) {
                    case SettingsId::kHeaderTableSize:
                        m_encoder.SetTableSize(entry.value);
                        break;
                    case SettingsId::kMaxFrameSize:
                        if (entry.value >= kMinMaxFrameSize && entry.value <= kMaxMaxFrameSize) {
                            m_peer_max_frame_size = entry.value;
                        }
                        break;
                    default:
                        break;
                }
            }
            m_peer_settings_received = true;
            // 回 ACK
            status = WriteFrame(MakeSettingsAck(), error);
            if (status != ConnStatus::kOk) {
                return status;
            }
            m_handshaked = true;
            return ConnStatus::kOk;
        }
        if (frame.header.type == FrameType::kPing) {
            PingFrame ping;
            if (ParsePing(frame, &ping, error) == FrameStatus::kOk && !ping.ack) {
                status = WriteFrame(MakePing(ping.opaque_data, true), error);
                if (status != ConnStatus::kOk) {
                    return status;
                }
            }
            continue;
        }
        // 握手期收到其它帧：忽略（真实实现应报 PROTOCOL_ERROR，
        // 但这里选择宽容，避免对端在握手后立刻发 HEADERS 的竞态导致误判）
    }
}

ConnStatus Connection::StartRequest(const std::vector<HeaderField>& headers, bool end_stream,
                                    std::uint32_t* out_stream_id, std::string* error) {
    if (m_role != Role::kClient) {
        if (error != nullptr) *error = "只有客户端可以发起请求";
        return ConnStatus::kUnsupported;
    }
    if (!m_handshaked) {
        if (error != nullptr) *error = "握手尚未完成";
        return ConnStatus::kProtocolError;
    }
    const std::uint32_t stream_id = m_next_stream_id;
    m_next_stream_id += 2;   // 客户端流 id 为奇数且递增

    std::vector<std::uint8_t> block;
    if (m_encoder.EncodeBlock(headers, &block, error) != HpackStatus::kOk) {
        if (error != nullptr) *error = "HPACK 编码失败";
        return ConnStatus::kProtocolError;
    }
    if (block.size() + kFrameHeaderSize > m_peer_max_frame_size) {
        if (error != nullptr) *error = "header block 超过帧上限（未实现 CONTINUATION）";
        return ConnStatus::kUnsupported;
    }
    // 用 try_emplace 而不是 operator[]：StreamStateMachine 没有默认构造，
    // 而且流 id 总是确定的，无需"先默认构造再赋值"。
    auto& machine = m_streams.try_emplace(stream_id, stream_id).first->second;
    if (machine.SendHeaders(end_stream) != StreamEventResult::kOk) {
        if (error != nullptr) *error = "流状态机拒绝发送 HEADERS";
        return ConnStatus::kProtocolError;
    }
    const auto status = WriteFrame(MakeHeaders(stream_id, end_stream, true, block), error);
    if (status != ConnStatus::kOk) {
        return status;
    }
    if (out_stream_id != nullptr) {
        *out_stream_id = stream_id;
    }
    return ConnStatus::kOk;
}

ConnStatus Connection::WriteData(std::uint32_t stream_id,
                                 const std::vector<std::uint8_t>& data, bool end_stream,
                                 std::string* error) {
    auto* machine = StreamFor(stream_id);
    if (machine == nullptr) {
        if (error != nullptr) *error = "未知流";
        return ConnStatus::kProtocolError;
    }
    if (machine->SendData(end_stream) != StreamEventResult::kOk) {
        if (error != nullptr) *error = "流状态机拒绝发送 DATA";
        return ConnStatus::kProtocolError;
    }
    if (data.size() + kFrameHeaderSize > m_peer_max_frame_size) {
        if (error != nullptr) *error = "DATA 超过帧上限（未实现分片）";
        return ConnStatus::kUnsupported;
    }
    return WriteFrame(MakeData(stream_id, end_stream, data), error);
}

ConnStatus Connection::SendResponseHeaders(std::uint32_t stream_id,
                                           const std::vector<HeaderField>& headers,
                                           bool end_stream, std::string* error) {
    auto* machine = StreamFor(stream_id);
    if (machine == nullptr) {
        if (error != nullptr) *error = "未知流";
        return ConnStatus::kProtocolError;
    }
    if (machine->SendHeaders(end_stream) != StreamEventResult::kOk) {
        if (error != nullptr) *error = "流状态机拒绝发送响应 HEADERS";
        return ConnStatus::kProtocolError;
    }
    std::vector<std::uint8_t> block;
    if (m_encoder.EncodeBlock(headers, &block, error) != HpackStatus::kOk) {
        if (error != nullptr) *error = "HPACK 编码失败";
        return ConnStatus::kProtocolError;
    }
    return WriteFrame(MakeHeaders(stream_id, end_stream, true, block), error);
}

ConnStatus Connection::ResetStream(std::uint32_t stream_id, ErrorCode code,
                                   std::string* error) {
    auto* machine = StreamFor(stream_id);
    if (machine == nullptr) {
        if (error != nullptr) *error = "未知流";
        return ConnStatus::kProtocolError;
    }
    machine->SendRstStream();
    return WriteFrame(MakeRstStream(stream_id, code), error);
}

ConnStatus Connection::SendGoAway(ErrorCode code, std::string* error) {
    std::uint32_t last = 0;
    // 最后一个已处理的流 id
    for (const auto& [id, machine] : m_streams) {
        (void)machine;
        if (id > last) {
            last = id;
        }
    }
    return WriteFrame(MakeGoAway(last, code), error);
}

ConnStatus Connection::ReadEvent(IncomingEvent* out, std::string* error) {
    if (out == nullptr) {
        return ConnStatus::kProtocolError;
    }
    Frame frame;
    auto status = TakeFrame(&frame, error);
    if (status != ConnStatus::kOk) {
        return status;
    }
    *out = IncomingEvent{};

    switch (frame.header.type) {
        case FrameType::kHeaders: {
            HeadersFrame headers;
            if (ParseHeaders(frame, &headers, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            if (!headers.end_headers) {
                if (error != nullptr) *error = "不支持 CONTINUATION 分片的 header block";
                return ConnStatus::kUnsupported;
            }
            std::vector<HeaderField> decoded;
            if (m_decoder.DecodeBlock(headers.header_block.data(),
                                      headers.header_block.size(), &decoded,
                                      error) != HpackStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            // 流状态推进
            auto& machine =
                m_streams.try_emplace(headers.stream_id, headers.stream_id).first->second;
            if (machine.ReceiveHeaders(headers.end_stream) != StreamEventResult::kOk) {
                if (error != nullptr) *error = "流状态机拒绝接收 HEADERS";
                return ConnStatus::kProtocolError;
            }
            out->kind = IncomingEvent::Kind::kHeaders;
            out->stream_id = headers.stream_id;
            out->end_stream = headers.end_stream;
            out->headers = std::move(decoded);
            return ConnStatus::kOk;
        }
        case FrameType::kData: {
            DataFrame data;
            if (ParseData(frame, &data, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            auto* machine = StreamFor(data.stream_id);
            if (machine == nullptr) {
                if (error != nullptr) *error = "DATA 落在未知流上";
                return ConnStatus::kProtocolError;
            }
            if (machine->ReceiveData(data.end_stream) != StreamEventResult::kOk) {
                if (error != nullptr) *error = "流状态机拒绝接收 DATA";
                return ConnStatus::kProtocolError;
            }
            out->kind = IncomingEvent::Kind::kData;
            out->stream_id = data.stream_id;
            out->end_stream = data.end_stream;
            out->data = std::move(data.data);
            // 简化流控：立刻把收到的字节数补回窗口
            if (!out->data.empty()) {
                status = WriteFrame(
                    MakeWindowUpdate(data.stream_id,
                                     static_cast<std::uint32_t>(out->data.size())),
                    error);
                if (status != ConnStatus::kOk) {
                    return status;
                }
            }
            return ConnStatus::kOk;
        }
        case FrameType::kSettings: {
            SettingsFrame settings;
            if (ParseSettings(frame, &settings, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            if (!settings.ack) {
                status = WriteFrame(MakeSettingsAck(), error);
                if (status != ConnStatus::kOk) {
                    return status;
                }
            }
            out->kind = IncomingEvent::Kind::kSettings;
            return ConnStatus::kOk;
        }
        case FrameType::kPing: {
            PingFrame ping;
            if (ParsePing(frame, &ping, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            if (!ping.ack) {
                status = WriteFrame(MakePing(ping.opaque_data, true), error);
                if (status != ConnStatus::kOk) {
                    return status;
                }
            }
            out->kind = IncomingEvent::Kind::kPing;
            out->ping_opaque = ping.opaque_data;
            return ConnStatus::kOk;
        }
        case FrameType::kRstStream: {
            RstStreamFrame rst;
            if (ParseRstStream(frame, &rst, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            if (auto* machine = StreamFor(rst.stream_id)) {
                machine->ReceiveRstStream();
            }
            out->kind = IncomingEvent::Kind::kRstStream;
            out->stream_id = rst.stream_id;
            out->error_code = rst.error_code;
            return ConnStatus::kOk;
        }
        case FrameType::kGoAway: {
            GoAwayFrame goaway;
            if (ParseGoAway(frame, &goaway, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            out->kind = IncomingEvent::Kind::kGoAway;
            out->error_code = goaway.error_code;
            out->last_stream_id = goaway.last_stream_id;
            return ConnStatus::kOk;
        }
        case FrameType::kWindowUpdate: {
            WindowUpdateFrame update;
            if (ParseWindowUpdate(frame, &update, error) != FrameStatus::kOk) {
                return ConnStatus::kProtocolError;
            }
            out->kind = IncomingEvent::Kind::kWindowUpdate;
            out->stream_id = update.stream_id;
            return ConnStatus::kOk;
        }
        default:
            out->kind = IncomingEvent::Kind::kUnknown;
            return ConnStatus::kOk;
    }
}

}  // namespace go2cpp::net::http2
