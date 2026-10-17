/**
 * @file test_connection.cpp
 * @brief HTTP/2 连接层测试（握手、请求/响应闭环、流管理、错误处理）。
 *
 * ## 测试策略
 *
 * 用 `MemoryTransport` 在**单线程**里模拟对端：
 * - 把"对端应该发的字节"喂给 `inbound`；
 * - 调用阻塞 API；
 * - 检查本端写出的字节（`outbound`）。
 *
 * 这样不需要真实 socket、不需要 Fiber，就能验证协议逻辑 —— 这正是把 IO
 * 抽象成 `Transport` 的价值。
 *
 * 闭环测试（场景 4/5）把客户端写出的字节直接喂给服务端，再反过来，验证
 * 「发出去的能被对方正确理解」。
 */
#include "go2cpp/network/http2/connection.hpp"
#include "go2cpp/network/http2/transport.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace go2cpp::net::http2;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[conn] FAILED: %s\n", what);
        ++g_failures;
    }
}

constexpr char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr std::size_t kPrefaceLen = sizeof(kPreface) - 1;

/// 序列化一帧，便于构造"对端发来的字节"。
std::vector<std::uint8_t> Bytes(const Frame& frame) {
    std::vector<std::uint8_t> out(kFrameHeaderSize + frame.payload.size());
    EncodeFrameHeader(frame.header, out.data());
    if (!frame.payload.empty()) {
        std::copy(frame.payload.begin(), frame.payload.end(),
                  out.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderSize));
    }
    return out;
}

/// 标准 SETTINGS（模拟对端一上来发的）。
std::vector<std::uint8_t> PeerSettings() {
    return Bytes(MakeSettings({
        {static_cast<std::uint16_t>(SettingsId::kHeaderTableSize), 4096},
        {static_cast<std::uint16_t>(SettingsId::kMaxFrameSize), 16384},
    }));
}

/// 从字节流里数出有多少个 SETTINGS 帧 / ACK 帧。
struct FrameCount {
    int settings = 0;
    int settings_ack = 0;
    int headers = 0;
    int data = 0;
    int ping = 0;
    int goaway = 0;
    int rst = 0;
    int total = 0;
};

FrameCount CountFrames(const std::vector<std::uint8_t>& bytes) {
    FrameCount count;
    std::size_t offset = 0;
    while (offset + kFrameHeaderSize <= bytes.size()) {
        FrameHeader header;
        if (DecodeFrameHeader(bytes.data() + offset, bytes.size() - offset, &header) !=
            FrameStatus::kOk) {
            break;
        }
        const std::size_t total = kFrameHeaderSize + header.length;
        if (offset + total > bytes.size()) {
            break;
        }
        ++count.total;
        switch (header.type) {
            case FrameType::kSettings:
                if ((header.flags & FrameFlags::kAck) != 0) {
                    ++count.settings_ack;
                } else {
                    ++count.settings;
                }
                break;
            case FrameType::kHeaders: ++count.headers; break;
            case FrameType::kData: ++count.data; break;
            case FrameType::kPing: ++count.ping; break;
            case FrameType::kGoAway: ++count.goaway; break;
            case FrameType::kRstStream: ++count.rst; break;
            default: break;
        }
        offset += total;
    }
    return count;
}

// ---------------------------------------------------------------------------
// 场景 1：客户端握手（发 preface + SETTINGS，收 SETTINGS，回 ACK）
// ---------------------------------------------------------------------------
void ScenarioClientHandshake() {
    MemoryTransport transport;
    transport.PushInbound(PeerSettings());
    Connection conn(&transport, Role::kClient);
    std::string error;
    const auto status = conn.Handshake(&error);
    Check(status == ConnStatus::kOk, "client handshake failed");
    if (status != ConnStatus::kOk) {
        std::fprintf(stderr, "[conn] 握手错误: %s\n", error.c_str());
        return;
    }
    Check(conn.handshaked(), "client should be handshaked");

    const auto out = transport.TakeOutbound();
    // 开头必须是 preface
    Check(out.size() >= kPrefaceLen, "client output too short");
    Check(std::memcmp(out.data(), kPreface, kPrefaceLen) == 0,
          "client must start with the HTTP/2 preface");

    const auto counts = CountFrames({out.begin() + static_cast<std::ptrdiff_t>(kPrefaceLen),
                                     out.end()});
    Check(counts.settings == 1, "client must send exactly 1 SETTINGS");
    Check(counts.settings_ack == 1, "client must ACK peer SETTINGS");
}

// ---------------------------------------------------------------------------
// 场景 2：服务端握手（读 preface + SETTINGS，发 SETTINGS + ACK）
// ---------------------------------------------------------------------------
void ScenarioServerHandshake() {
    MemoryTransport transport;
    // 服务端首先看到 preface + 客户端 SETTINGS
    std::vector<std::uint8_t> inbound(kPreface, kPreface + kPrefaceLen);
    const auto settings = PeerSettings();
    inbound.insert(inbound.end(), settings.begin(), settings.end());
    transport.PushInbound(inbound);

    Connection conn(&transport, Role::kServer);
    std::string error;
    const auto status = conn.Handshake(&error);
    Check(status == ConnStatus::kOk, "server handshake failed");
    if (status != ConnStatus::kOk) {
        std::fprintf(stderr, "[conn] 握手错误: %s\n", error.c_str());
        return;
    }
    const auto out = transport.TakeOutbound();
    const auto counts = CountFrames(out);
    Check(counts.settings == 1, "server must send exactly 1 SETTINGS");
    Check(counts.settings_ack == 1, "server must ACK client SETTINGS");
}

// ---------------------------------------------------------------------------
// 场景 3：preface 错误
// ---------------------------------------------------------------------------
void ScenarioBadPreface() {
    MemoryTransport transport;
    // 必须是 >= 24 字节的数据：少于 24 字节会被判为「对端关闭（EOF）」而不是
    // 「前言不匹配」。这里故意发一个完整的 HTTP/1.1 请求来触发不匹配。
    const char bad[] = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    transport.PushInbound(reinterpret_cast<const std::uint8_t*>(bad), sizeof(bad) - 1);
    Connection conn(&transport, Role::kServer);
    std::string error;
    Check(conn.Handshake(&error) == ConnStatus::kProtocolError,
          "server must reject a non-HTTP/2 preface");
    Check(!error.empty(), "bad preface should produce a diagnostic");

    // 数据不足 24 字节且对端关闭 -> 应当报 kClosed（区别于协议错误）
    {
        MemoryTransport short_transport;
        const char partial[] = "PRI * HT";
        short_transport.PushInbound(reinterpret_cast<const std::uint8_t*>(partial),
                                    sizeof(partial) - 1);
        short_transport.MarkEof();
        Connection short_conn(&short_transport, Role::kServer);
        std::string short_error;
        Check(short_conn.Handshake(&short_error) == ConnStatus::kClosed,
              "truncated preface with EOF must be kClosed, not protocol error");
    }
}

// ---------------------------------------------------------------------------
// 场景 4：完整请求闭环 —— 客户端发请求，服务端收到 HEADERS
// ---------------------------------------------------------------------------
void ScenarioRequestLoop() {
    MemoryTransport client_transport;
    MemoryTransport server_transport;

    // 1) 服务端先收到 preface + 客户端 SETTINGS（模拟真实时序）
    std::vector<std::uint8_t> server_inbound(kPreface, kPreface + kPrefaceLen);
    const auto client_settings = PeerSettings();
    server_inbound.insert(server_inbound.end(), client_settings.begin(),
                          client_settings.end());
    server_transport.PushInbound(server_inbound);

    Connection server(&server_transport, Role::kServer);
    std::string error;
    Check(server.Handshake(&error) == ConnStatus::kOk, "server handshake failed");

    // 2) 客户端握手：把服务端写出的 SETTINGS/ACK 喂给它
    const auto server_out = server_transport.TakeOutbound();
    client_transport.PushInbound(server_out);
    Connection client(&client_transport, Role::kClient);
    Check(client.Handshake(&error) == ConnStatus::kOk, "client handshake failed");

    // 3) 客户端发请求
    const std::vector<HeaderField> request = {
        {":method", "GET"},
        {":scheme", "https"},
        {":path", "/hello"},
        {":authority", "example.com"},
        {"user-agent", "go2cpp-test"},
    };
    std::uint32_t stream_id = 0;
    auto status = client.StartRequest(request, true, &stream_id, &error);
    Check(status == ConnStatus::kOk, "client StartRequest failed");
    if (status != ConnStatus::kOk) {
        std::fprintf(stderr, "[conn] StartRequest 错误: %s\n", error.c_str());
        return;
    }
    Check(stream_id == 1, "first client stream id must be 1 (odd)");

    // 4) 把客户端写出的字节喂给服务端（跳过它自己的 preface/SETTINGS，服务端不关心）
    const auto client_out = client_transport.TakeOutbound();
    // 客户端 outbound = preface + SETTINGS + ACK + HEADERS
    const std::size_t skip = kPrefaceLen;
    server_transport.PushInbound({client_out.begin() + static_cast<std::ptrdiff_t>(skip),
                                  client_out.end()});

    // 5) 服务端逐事件读，直到拿到 HEADERS
    bool got_headers = false;
    for (int guard = 0; guard < 10 && !got_headers; ++guard) {
        IncomingEvent event;
        status = server.ReadEvent(&event, &error);
        Check(status == ConnStatus::kOk, "server ReadEvent failed");
        if (status != ConnStatus::kOk) {
            break;
        }
        if (event.kind == IncomingEvent::Kind::kHeaders) {
            got_headers = true;
            Check(event.stream_id == 1, "server saw wrong stream id");
            Check(event.end_stream, "server should see end_stream on the request");
            Check(event.headers.size() == request.size(), "decoded header count wrong");
            if (event.headers.size() == request.size()) {
                for (std::size_t i = 0; i < request.size(); ++i) {
                    if (!(event.headers[i] == request[i])) {
                        std::fprintf(stderr, "[conn] header %zu: got %s=%s want %s=%s\n", i,
                                     event.headers[i].name.c_str(),
                                     event.headers[i].value.c_str(),
                                     request[i].name.c_str(), request[i].value.c_str());
                        Check(false, "decoded header mismatch");
                    }
                }
            }
        }
    }
    Check(got_headers, "server never received the request HEADERS");
}

// ---------------------------------------------------------------------------
// 场景 5：响应闭环 —— 服务端回 HEADERS + DATA，客户端读到
// ---------------------------------------------------------------------------
void ScenarioResponseLoop() {
    MemoryTransport client_transport;
    MemoryTransport server_transport;

    std::vector<std::uint8_t> server_inbound(kPreface, kPreface + kPrefaceLen);
    const auto client_settings = PeerSettings();
    server_inbound.insert(server_inbound.end(), client_settings.begin(),
                          client_settings.end());
    server_transport.PushInbound(server_inbound);

    Connection server(&server_transport, Role::kServer);
    std::string error;
    Check(server.Handshake(&error) == ConnStatus::kOk, "server handshake failed");
    client_transport.PushInbound(server_transport.TakeOutbound());
    Connection client(&client_transport, Role::kClient);
    Check(client.Handshake(&error) == ConnStatus::kOk, "client handshake failed");

    // 客户端发请求
    std::uint32_t stream_id = 0;
    Check(client.StartRequest({{":method", "POST"}, {":path", "/upload"},
                               {":authority", "example.com"}},
                              false, &stream_id, &error) == ConnStatus::kOk,
          "StartRequest failed");
    // 再发 DATA 结束发送方向
    Check(client.WriteData(stream_id, {'p', 'i', 'n', 'g'}, true, &error) == ConnStatus::kOk,
          "WriteData failed");

    // 服务端收
    const auto client_out = client_transport.TakeOutbound();
    server_transport.PushInbound({client_out.begin() + static_cast<std::ptrdiff_t>(kPrefaceLen),
                                  client_out.end()});
    bool got_headers = false;
    bool got_data = false;
    for (int guard = 0; guard < 12 && !(got_headers && got_data); ++guard) {
        IncomingEvent event;
        if (server.ReadEvent(&event, &error) != ConnStatus::kOk) {
            break;
        }
        if (event.kind == IncomingEvent::Kind::kHeaders) {
            got_headers = true;
            Check(!event.end_stream, "request should not end on HEADERS");
        } else if (event.kind == IncomingEvent::Kind::kData) {
            got_data = true;
            Check(event.data.size() == 4, "server should get 4 data bytes");
            Check(event.end_stream, "data should carry end_stream");
        }
    }
    Check(got_headers && got_data, "server should receive HEADERS and DATA");

    // 服务端回响应
    Check(server.SendResponseHeaders(stream_id, {{":status", "200"},
                                                 {"content-type", "text/plain"}},
                                     false, &error) == ConnStatus::kOk,
          "SendResponseHeaders failed");
    Check(server.WriteData(stream_id, {'p', 'o', 'n', 'g'}, true, &error) == ConnStatus::kOk,
          "server WriteData failed");

    // 客户端读
    auto server_out = server_transport.TakeOutbound();
    client_transport.PushInbound(server_out);
    bool client_got_headers = false;
    bool client_got_data = false;
    for (int guard = 0; guard < 12 && !(client_got_headers && client_got_data); ++guard) {
        IncomingEvent event;
        if (client.ReadEvent(&event, &error) != ConnStatus::kOk) {
            break;
        }
        if (event.kind == IncomingEvent::Kind::kHeaders) {
            client_got_headers = true;
            Check(event.headers.size() == 2, "response header count wrong");
            if (event.headers.size() == 2) {
                Check(event.headers[0].name == ":status" && event.headers[0].value == "200",
                      "response status wrong");
            }
        } else if (event.kind == IncomingEvent::Kind::kData) {
            client_got_data = true;
            Check(event.data.size() == 4 && event.end_stream, "response data wrong");
        }
    }
    Check(client_got_headers && client_got_data, "client should receive response");
}

// ---------------------------------------------------------------------------
// 场景 6：流状态推进与流表
// ---------------------------------------------------------------------------
void ScenarioStreamTracking() {
    MemoryTransport transport;
    transport.PushInbound(PeerSettings());
    Connection conn(&transport, Role::kClient);
    std::string error;
    Check(conn.Handshake(&error) == ConnStatus::kOk, "handshake failed");

    std::uint32_t s1 = 0;
    std::uint32_t s2 = 0;
    Check(conn.StartRequest({{":method", "GET"}, {":path", "/a"}, {":authority", "h"}},
                            true, &s1, &error) == ConnStatus::kOk, "request 1 failed");
    Check(conn.StartRequest({{":method", "GET"}, {":path", "/b"}, {":authority", "h"}},
                            true, &s2, &error) == ConnStatus::kOk, "request 2 failed");
    Check(s1 == 1 && s2 == 3, "client stream ids must be 1 then 3");
    Check(conn.Streams().size() == 2, "stream table should hold 2 streams");
    Check(conn.Streams().at(s1).state() == StreamState::kHalfClosedLocal,
          "stream with end_stream should be half-closed(local)");

    // 未知流上发 DATA 必须被拒绝
    Check(conn.WriteData(99, {1}, false, &error) == ConnStatus::kProtocolError,
          "DATA on unknown stream must be rejected");
}

// ---------------------------------------------------------------------------
// 场景 7：GOAWAY / RST_STREAM
// ---------------------------------------------------------------------------
void ScenarioGoAwayAndRst() {
    MemoryTransport transport;
    transport.PushInbound(PeerSettings());
    Connection conn(&transport, Role::kClient);
    std::string error;
    Check(conn.Handshake(&error) == ConnStatus::kOk, "handshake failed");
    (void)transport.TakeOutbound();

    std::uint32_t sid = 0;
    Check(conn.StartRequest({{":method", "GET"}, {":path", "/x"}, {":authority", "h"}},
                            false, &sid, &error) == ConnStatus::kOk, "request failed");

    // RST_STREAM 应把流置为 closed
    Check(conn.ResetStream(sid, ErrorCode::kCancel, &error) == ConnStatus::kOk,
          "ResetStream failed");
    Check(conn.Streams().at(sid).closed(), "stream must be closed after RST");

    const auto out = transport.TakeOutbound();
    Check(CountFrames(out).rst == 1, "should have emitted exactly 1 RST_STREAM");

    // GOAWAY
    Check(conn.SendGoAway(ErrorCode::kNoError, &error) == ConnStatus::kOk,
          "SendGoAway failed");
    const auto goaway_out = transport.TakeOutbound();
    Check(CountFrames(goaway_out).goaway == 1, "should have emitted 1 GOAWAY");
}

// ---------------------------------------------------------------------------
// 场景 8：错误处理
// ---------------------------------------------------------------------------
void ScenarioErrors() {
    // 未握手就发请求
    {
        MemoryTransport transport;
        Connection conn(&transport, Role::kClient);
        std::uint32_t sid = 0;
        std::string error;
        Check(conn.StartRequest({{":method", "GET"}}, true, &sid, &error) ==
                  ConnStatus::kProtocolError,
              "StartRequest before handshake must fail");
    }
    // 服务端不能主动发请求
    {
        MemoryTransport transport;
        std::vector<std::uint8_t> inbound(kPreface, kPreface + kPrefaceLen);
        const auto settings = PeerSettings();
        inbound.insert(inbound.end(), settings.begin(), settings.end());
        transport.PushInbound(inbound);
        Connection conn(&transport, Role::kServer);
        std::string error;
        Check(conn.Handshake(&error) == ConnStatus::kOk, "server handshake failed");
        std::uint32_t sid = 0;
        Check(conn.StartRequest({{":method", "GET"}}, true, &sid, &error) ==
                  ConnStatus::kUnsupported,
              "server must not StartRequest");
    }
    // 对端关闭后 ReadEvent 返回 kClosed
    {
        MemoryTransport transport;
        transport.PushInbound(PeerSettings());
        Connection conn(&transport, Role::kClient);
        std::string error;
        Check(conn.Handshake(&error) == ConnStatus::kOk, "handshake failed");
        transport.MarkEof();
        IncomingEvent event;
        Check(conn.ReadEvent(&event, &error) == ConnStatus::kClosed,
              "ReadEvent after EOF must return kClosed");
    }
}


// ---------------------------------------------------------------------------
// 场景 9：CONTINUATION —— 大 header 自动分片与拼接
// ---------------------------------------------------------------------------
void ScenarioContinuation() {
    MemoryTransport client_transport;
    MemoryTransport server_transport;

    std::vector<std::uint8_t> server_inbound(kPreface, kPreface + kPrefaceLen);
    const auto settings = PeerSettings();
    server_inbound.insert(server_inbound.end(), settings.begin(), settings.end());
    server_transport.PushInbound(server_inbound);

    Connection server(&server_transport, Role::kServer);
    std::string error;
    Check(server.Handshake(&error) == ConnStatus::kOk, "server handshake failed");
    client_transport.PushInbound(server_transport.TakeOutbound());
    Connection client(&client_transport, Role::kClient);
    Check(client.Handshake(&error) == ConnStatus::kOk, "client handshake failed");

    // 造一个 HPACK 编码后必定超过一帧（16384）的 header：
    // 50000 个 'x'，Huffman 对 'x' 是 5 位，编码后仍约 31250 字节 > 16384。
    const std::string big_value(50000, 'x');
    const std::vector<HeaderField> request = {
        {":method", "POST"},
        {":path", "/big"},
        {":authority", "example.com"},
        {"x-big-payload", big_value},
    };
    std::uint32_t stream_id = 0;
    Check(client.StartRequest(request, true, &stream_id, &error) == ConnStatus::kOk,
          "StartRequest with huge header failed");
    if (!error.empty()) {
        std::fprintf(stderr, "[conn] 大 header 发送错误: %s\n", error.c_str());
    }

    // 客户端写出的字节里，除了 preface/SETTINGS/ACK，应该有 HEADERS + 至少 1 个 CONTINUATION
    const auto client_out = client_transport.TakeOutbound();
    int headers_frames = 0;
    int continuation_frames = 0;
    {
        std::size_t offset = kPrefaceLen;
        while (offset + kFrameHeaderSize <= client_out.size()) {
            FrameHeader header;
            if (DecodeFrameHeader(client_out.data() + offset,
                                  client_out.size() - offset, &header) != FrameStatus::kOk) {
                break;
            }
            const std::size_t total = kFrameHeaderSize + header.length;
            if (offset + total > client_out.size()) {
                break;
            }
            if (header.type == FrameType::kHeaders) ++headers_frames;
            if (header.type == FrameType::kContinuation) ++continuation_frames;
            offset += total;
        }
    }
    std::printf("[conn] 大 header 分片: HEADERS=%d CONTINUATION=%d\n", headers_frames,
                continuation_frames);
    Check(headers_frames == 1, "should emit exactly one HEADERS");
    Check(continuation_frames >= 1, "big header must be split into CONTINUATION");

    // 服务端应能拼回完整 header
    server_transport.PushInbound({client_out.begin() + static_cast<std::ptrdiff_t>(kPrefaceLen),
                                  client_out.end()});
    bool got = false;
    for (int guard = 0; guard < 12 && !got; ++guard) {
        IncomingEvent event;
        if (server.ReadEvent(&event, &error) != ConnStatus::kOk) {
            std::fprintf(stderr, "[conn] 服务端读大 header 失败: %s\n", error.c_str());
            break;
        }
        if (event.kind == IncomingEvent::Kind::kHeaders) {
            got = true;
            std::string seen_big;
            for (const auto& field : event.headers) {
                if (field.name == "x-big-payload") seen_big = field.value;
            }
            Check(seen_big.size() == big_value.size(),
                  "assembled big header value has wrong length");
            Check(seen_big == big_value, "assembled big header value mismatch");
        }
    }
    Check(got, "server never received the split header block");
}


// ---------------------------------------------------------------------------
// 场景 10：流控 —— 窗口耗尽、WINDOW_UPDATE 恢复、接收侧阈值补窗口
// ---------------------------------------------------------------------------
void ScenarioFlowControl() {
    // 用很小的初始窗口，让几字节就把额度用完
    ConnectionConfig config;
    config.initial_window_size = 64;

    MemoryTransport client_transport;
    MemoryTransport server_transport;

    std::vector<std::uint8_t> server_inbound(kPreface, kPreface + kPrefaceLen);
    // 服务端 SETTINGS 里声明 initial_window_size = 64
    const auto peer_settings =
        Bytes(MakeSettings({{static_cast<std::uint16_t>(SettingsId::kInitialWindowSize), 64}}));
    server_inbound.insert(server_inbound.end(), peer_settings.begin(), peer_settings.end());
    server_transport.PushInbound(server_inbound);

    Connection server(&server_transport, Role::kServer, config);
    std::string error;
    Check(server.Handshake(&error) == ConnStatus::kOk, "server handshake failed");
    client_transport.PushInbound(server_transport.TakeOutbound());
    Connection client(&client_transport, Role::kClient, config);
    Check(client.Handshake(&error) == ConnStatus::kOk, "client handshake failed");

    std::uint32_t stream_id = 0;
    Check(client.StartRequest({{":method", "POST"}, {":path", "/fc"},
                               {":authority", "h"}},
                              false, &stream_id, &error) == ConnStatus::kOk,
          "StartRequest failed");

    // 1) 窗口 64，发 64 字节恰好用完
    Check(client.WriteData(stream_id, std::vector<std::uint8_t>(64, 'a'), false, &error) ==
              ConnStatus::kOk,
          "sending exactly the window should succeed");

    // 2) 窗口已耗尽，再发 1 字节必须 kFlowControl
    const auto over =
        client.WriteData(stream_id, std::vector<std::uint8_t>(1, 'b'), false, &error);
    Check(over == ConnStatus::kFlowControl,
          "sending past the window must return kFlowControl");
    Check(!error.empty(), "flow control rejection should explain itself");

    // 3) 恢复额度：RFC 9113 §6.9 要求**连接级与流级窗口都有额度**才能发，
    //    所以两个 WINDOW_UPDATE 都要补，只补流级是不够的。
    {
        const auto stream_update = Bytes(MakeWindowUpdate(stream_id, 16));
        client_transport.PushInbound(stream_update);
        const auto conn_update = Bytes(MakeWindowUpdate(0, 16));
        client_transport.PushInbound(conn_update);
    }
    IncomingEvent event;
    // 注意：握手后缓冲区里可能还有对端的 SETTINGS ACK 等事件帧，
    // 所以要循环读到 WINDOW_UPDATE 为止，不能假设第一次就是它。
    // 发了两个 WINDOW_UPDATE（流级 + 连接级），必须把两个都读掉，
    // 只读一个的话另一个还留在缓冲区里，连接级窗口不会恢复。
    int updates_seen = 0;
    for (int guard = 0; guard < 8 && updates_seen < 2; ++guard) {
        if (client.ReadEvent(&event, &error) != ConnStatus::kOk) {
            break;
        }
        if (event.kind == IncomingEvent::Kind::kWindowUpdate) {
            ++updates_seen;
        }
    }
    Check(updates_seen == 2, "should see two WINDOW_UPDATE (stream + connection)");

    // 4) 现在又能发 16 字节
    Check(client.WriteData(stream_id, std::vector<std::uint8_t>(16, 'c'), true, &error) ==
              ConnStatus::kOk,
          "sending after WINDOW_UPDATE should succeed");

    // 5) 接收侧：服务端收到 DATA 后应扣减本地窗口，并在低于一半时发 WINDOW_UPDATE
    const auto client_out = client_transport.TakeOutbound();
    server_transport.PushInbound({client_out.begin() + static_cast<std::ptrdiff_t>(kPrefaceLen),
                                  client_out.end()});
    std::size_t data_bytes = 0;
    bool got_data = false;
    for (int guard = 0; guard < 14; ++guard) {
        if (server.ReadEvent(&event, &error) != ConnStatus::kOk) {
            break;
        }
        if (event.kind == IncomingEvent::Kind::kData) {
            got_data = true;
            data_bytes += event.data.size();
            if (event.end_stream) break;
        }
    }
    Check(got_data, "server should receive DATA");
    Check(data_bytes == 80, "server should see 64 + 16 bytes");

    // 服务端写出的帧里应当有 WINDOW_UPDATE（因为 64 字节就把 64 的窗口耗尽到 0）
    const auto server_out = server_transport.TakeOutbound();
    int window_updates = 0;
    {
        std::size_t offset = 0;
        while (offset + kFrameHeaderSize <= server_out.size()) {
            FrameHeader header;
            if (DecodeFrameHeader(server_out.data() + offset, server_out.size() - offset,
                                  &header) != FrameStatus::kOk) {
                break;
            }
            const std::size_t total = kFrameHeaderSize + header.length;
            if (offset + total > server_out.size()) break;
            if (header.type == FrameType::kWindowUpdate) ++window_updates;
            offset += total;
        }
    }
    std::printf("[conn] 流控: 服务端补窗口 WINDOW_UPDATE=%d\n", window_updates);
    Check(window_updates >= 1, "receiver should replenish the window via WINDOW_UPDATE");
}


// ---------------------------------------------------------------------------
// 场景 11：header 大小上限（防 CONTINUATION 泛滥与 HPACK 解压炸弹）
// ---------------------------------------------------------------------------
void ScenarioHeaderLimits() {
    std::string error;

    // 起一对已握手的连接（用给定 config）
    const auto make_pair = [&error](const ConnectionConfig& config,
                                    MemoryTransport* ct, MemoryTransport* st,
                                    Connection** client_out, Connection** server_out) {
        std::vector<std::uint8_t> in(kPreface, kPreface + kPrefaceLen);
        const auto settings = PeerSettings();
        in.insert(in.end(), settings.begin(), settings.end());
        st->PushInbound(in);
        auto* server = new Connection(st, Role::kServer, config);
        if (server->Handshake(&error) != ConnStatus::kOk) return false;
        ct->PushInbound(st->TakeOutbound());
        auto* client = new Connection(ct, Role::kClient, config);
        if (client->Handshake(&error) != ConnStatus::kOk) return false;
        *client_out = client;
        *server_out = server;
        return true;
    };

    // ---------- 情况 1：压缩后的 header block 超限 ----------
    {
        ConnectionConfig config;
        config.max_header_block_size = 256;          // 很小
        config.max_header_list_size = 1u << 20;      // 这一项不设限
        MemoryTransport ct;
        MemoryTransport st;
        Connection* client = nullptr;
        Connection* server = nullptr;
        Check(make_pair(config, &ct, &st, &client, &server), "case1 handshake failed");
        if (client != nullptr) {
            const std::string big(4000, 'x');        // 压缩后仍远大于 256
            std::uint32_t sid = 0;
            Check(client->StartRequest({{":method", "GET"}, {":path", "/"},
                                        {"x-big", big}},
                                       true, &sid, &error) == ConnStatus::kOk,
                  "case1 send big header failed");
            const auto out = ct.TakeOutbound();
            st.PushInbound({out.begin() + static_cast<std::ptrdiff_t>(kPrefaceLen),
                            out.end()});
            ConnStatus status = ConnStatus::kOk;
            IncomingEvent ev;
            for (int i = 0; i < 12; ++i) {
                status = server->ReadEvent(&ev, &error);
                if (status != ConnStatus::kOk) break;
                if (ev.kind == IncomingEvent::Kind::kHeaders) break;
            }
            std::printf("[conn] header 上限-压缩块: status=%s\n",
                        ToString(status));
            Check(status == ConnStatus::kHeaderListTooLarge,
                  "oversized compressed header block must be kHeaderListTooLarge");
            Check(!error.empty(), "oversized header block should explain itself");
            delete client;
            delete server;
        }
    }

    // ---------- 情况 2：解压后 header list 超限 ----------
    {
        ConnectionConfig config;
        config.max_header_block_size = 1u << 20;     // 压缩块不设限
        config.max_header_list_size = 100;           // 解压后很小
        MemoryTransport ct;
        MemoryTransport st;
        Connection* client = nullptr;
        Connection* server = nullptr;
        Check(make_pair(config, &ct, &st, &client, &server), "case2 handshake failed");
        if (client != nullptr) {
            // 200 字节的 value：压缩后约 60 字节（远小于 1MB），
            // 但解压后 6 + 200 + 32 > 100。
            const std::string mid(200, 'y');
            std::uint32_t sid = 0;
            Check(client->StartRequest({{":method", "GET"}, {":path", "/"},
                                        {"x-mid", mid}},
                                       true, &sid, &error) == ConnStatus::kOk,
                  "case2 send mid header failed");
            const auto out = ct.TakeOutbound();
            st.PushInbound({out.begin() + static_cast<std::ptrdiff_t>(kPrefaceLen),
                            out.end()});
            ConnStatus status = ConnStatus::kOk;
            IncomingEvent ev;
            for (int i = 0; i < 12; ++i) {
                status = server->ReadEvent(&ev, &error);
                if (status != ConnStatus::kOk) break;
                if (ev.kind == IncomingEvent::Kind::kHeaders) break;
            }
            std::printf("[conn] header 上限-解压后: status=%s\n", ToString(status));
            Check(status == ConnStatus::kHeaderListTooLarge,
                  "oversized decoded header list must be kHeaderListTooLarge");
            delete client;
            delete server;
        }
    }

    // ---------- 情况 3：限内应当成功 ----------
    {
        ConnectionConfig config;                     // 默认 65536
        MemoryTransport ct;
        MemoryTransport st;
        Connection* client = nullptr;
        Connection* server = nullptr;
        Check(make_pair(config, &ct, &st, &client, &server), "case3 handshake failed");
        if (client != nullptr) {
            const std::string small(200, 'z');
            std::uint32_t sid = 0;
            Check(client->StartRequest({{":method", "GET"}, {":path", "/ok"},
                                        {"x-small", small}},
                                       true, &sid, &error) == ConnStatus::kOk,
                  "case3 send small header failed");
            const auto out = ct.TakeOutbound();
            st.PushInbound({out.begin() + static_cast<std::ptrdiff_t>(kPrefaceLen),
                            out.end()});
            bool got = false;
            IncomingEvent ev;
            for (int i = 0; i < 12 && !got; ++i) {
                if (server->ReadEvent(&ev, &error) != ConnStatus::kOk) break;
                if (ev.kind == IncomingEvent::Kind::kHeaders) {
                    got = true;
                    Check(ev.headers.size() == 3, "case3 header count wrong");
                }
            }
            Check(got, "header within limits must be accepted");
            delete client;
            delete server;
        }
    }
}

}  // namespace

int main() {
    std::printf("=== HTTP/2 连接层测试 ===\n");
    std::printf("[conn][场景1] 客户端握手\n");
    ScenarioClientHandshake();
    std::printf("[conn][场景2] 服务端握手\n");
    ScenarioServerHandshake();
    std::printf("[conn][场景3] preface 错误\n");
    ScenarioBadPreface();
    std::printf("[conn][场景4] 请求闭环（客户端->服务端 HEADERS）\n");
    ScenarioRequestLoop();
    std::printf("[conn][场景5] 响应闭环（HEADERS + DATA 双向）\n");
    ScenarioResponseLoop();
    std::printf("[conn][场景6] 流状态与流表\n");
    ScenarioStreamTracking();
    std::printf("[conn][场景7] GOAWAY 与 RST_STREAM\n");
    ScenarioGoAwayAndRst();
    std::printf("[conn][场景8] 错误处理\n");
    ScenarioErrors();
    std::printf("[conn][场景9] CONTINUATION 大 header\n");
    ScenarioContinuation();
    std::printf("[conn][场景10] 流控\n");
    ScenarioFlowControl();
    std::printf("[conn][场景11] header 大小上限\n");
    ScenarioHeaderLimits();

    if (g_failures == 0) {
        std::printf("\n[conn] failures=0\n");
    } else {
        std::printf("\n[conn] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
