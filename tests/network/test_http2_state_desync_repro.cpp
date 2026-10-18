/**
 * @file test_http2_state_desync_repro.cpp
 * @brief HTTP/2 连接层"状态与线上状态脱节"回归测试（R1/R2/R13）。
 *
 * ## 由来
 *
 * 本文件最初是**缺陷复现测试**：在修复前它稳定失败，用来证明三个问题真实
 * 存在，而不是"看代码觉得可疑"：
 *
 * 1. `WriteData` 先推进流状态机、再做尺寸/流控校验 —— 被拒绝的写入仍把
 *    END_STREAM 记在流上（状态与线上脱节，且流控的"重试"契约失效）；
 * 2. 连接没有 GOAWAY/draining 状态 —— 发/收 GOAWAY 后仍接受新流；
 * 3. 接收侧流控不校验 —— 对端 DATA 超过接收窗口仍被当成合法事件
 *    （RFC 9113 §6.9.1 要求 FLOW_CONTROL_ERROR）。
 *
 * 修复后它转为**常规回归测试**（CMake 里不再 DISABLED）：任何一条断言
 * 回退都会让 ctest 直接失败。
 *
 * ## 关于接收侧流控的构造
 *
 * 不能用"连续发很多满帧"来证明窗口越界：接收方在窗口降到一半以下时会回
 * WINDOW_UPDATE 并恢复自己的窗口，随后到达的帧是**合法**的（credit 已补）。
 * 所以这里把 `initial_window_size` 配成 1 字节，再发一个 16 字节 DATA 帧 ——
 * 第一帧就明确超过可用窗口，与补窗策略无关。
 */
#include "go2cpp/network/http2/connection.hpp"
#include "go2cpp/network/http2/frame.hpp"
#include "go2cpp/network/http2/transport.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace go2cpp::net::http2;

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        std::fprintf(stderr, "[regress] FAILED: %s\n", what);
        ++g_failures;
    }
}

std::vector<std::uint8_t> EmptySettingsFrame() {
    return std::vector<std::uint8_t>{0, 0, 0, 4, 0, 0, 0, 0, 0};
}

std::vector<std::uint8_t> DataFrame(std::uint32_t stream_id,
                                    std::size_t payload_size) {
    std::vector<std::uint8_t> frame(9 + payload_size, 0);
    frame[0] = static_cast<std::uint8_t>((payload_size >> 16) & 0xff);
    frame[1] = static_cast<std::uint8_t>((payload_size >> 8) & 0xff);
    frame[2] = static_cast<std::uint8_t>(payload_size & 0xff);
    frame[3] = 0;  // type = DATA
    frame[4] = 0;  // flags
    frame[5] = static_cast<std::uint8_t>((stream_id >> 24) & 0x7f);
    frame[6] = static_cast<std::uint8_t>((stream_id >> 16) & 0xff);
    frame[7] = static_cast<std::uint8_t>((stream_id >> 8) & 0xff);
    frame[8] = static_cast<std::uint8_t>(stream_id & 0xff);
    return frame;
}

std::vector<std::uint8_t> WindowUpdateFrame(std::uint32_t stream_id,
                                            std::uint32_t increment) {
    std::vector<std::uint8_t> frame(13, 0);
    frame[2] = 4;  // length = 4
    frame[3] = 8;  // type = WINDOW_UPDATE
    frame[5] = static_cast<std::uint8_t>((stream_id >> 24) & 0x7f);
    frame[6] = static_cast<std::uint8_t>((stream_id >> 16) & 0xff);
    frame[7] = static_cast<std::uint8_t>((stream_id >> 8) & 0xff);
    frame[8] = static_cast<std::uint8_t>(stream_id & 0xff);
    frame[9] = static_cast<std::uint8_t>((increment >> 24) & 0x7f);
    frame[10] = static_cast<std::uint8_t>((increment >> 16) & 0xff);
    frame[11] = static_cast<std::uint8_t>((increment >> 8) & 0xff);
    frame[12] = static_cast<std::uint8_t>(increment & 0xff);
    return frame;
}

std::vector<std::uint8_t> GoAwayFrame(std::uint32_t last_stream_id,
                                      std::uint32_t error_code) {
    std::vector<std::uint8_t> frame(17, 0);
    frame[2] = 8;  // length = 8
    frame[3] = 7;  // type = GOAWAY
    frame[5] = static_cast<std::uint8_t>((last_stream_id >> 24) & 0x7f);
    frame[6] = static_cast<std::uint8_t>((last_stream_id >> 16) & 0xff);
    frame[7] = static_cast<std::uint8_t>((last_stream_id >> 8) & 0xff);
    frame[8] = static_cast<std::uint8_t>(last_stream_id & 0xff);
    frame[12] = static_cast<std::uint8_t>((error_code >> 24) & 0xff);
    frame[13] = static_cast<std::uint8_t>((error_code >> 16) & 0xff);
    frame[14] = static_cast<std::uint8_t>((error_code >> 8) & 0xff);
    frame[15] = static_cast<std::uint8_t>(error_code & 0xff);
    return frame;
}

std::vector<HeaderField> RequestHeaders() {
    return {{":method", "POST"}, {":path", "/upload"}, {":scheme", "https"},
            {":authority", "example.com"}};
}

struct Handshaked {
    MemoryTransport transport;
    // 注意：Connection 持有 Transport*，必须保证 transport 生命周期更长。
    std::unique_ptr<Connection> connection;
};

std::unique_ptr<Handshaked> MakeClient(ConnectionConfig config = {}) {
    auto fixture = std::make_unique<Handshaked>();
    fixture->transport.PushInbound(EmptySettingsFrame());
    fixture->connection =
        std::make_unique<Connection>(&fixture->transport, Role::kClient, config);
    Check(fixture->connection->Handshake() == ConnStatus::kOk,
          "fixture: handshake 失败");
    return fixture;
}

// ---------------------------------------------------------------------------
// R1a：WriteData 的尺寸校验必须发生在状态推进之前
// ---------------------------------------------------------------------------
void RegressionWriteDataOversize() {
    auto fixture = MakeClient();
    auto& connection = *fixture->connection;
    std::uint32_t stream_id = 0;
    Check(connection.StartRequest(RequestHeaders(), false, &stream_id) ==
              ConnStatus::kOk,
          "R1a: StartRequest 失败");
    Check(connection.Streams().at(stream_id).state() == StreamState::kOpen,
          "R1a: 初始流应 open");

    // 超过 SETTINGS_MAX_FRAME_SIZE（默认 16384）的 DATA：必须被拒绝。
    const std::vector<std::uint8_t> oversized(20000, 'x');
    const auto rejected =
        connection.WriteData(stream_id, oversized, true /*end_stream*/);
    Check(rejected == ConnStatus::kUnsupported,
          "R1a: 超尺寸 DATA 应返回 kUnsupported");

    // 关键断言：被拒绝的写入不得推进流状态机。
    Check(connection.Streams().at(stream_id).state() == StreamState::kOpen,
          "R1a: 被拒绝的写入把流推进到了 half-closed(local)/closed");

    // 合法的小写入（带 END_STREAM）必须仍能成功。
    const std::vector<std::uint8_t> small{'o', 'k'};
    Check(connection.WriteData(stream_id, small, true) == ConnStatus::kOk,
          "R1a: 被拒绝的写入之后，合法的 END_STREAM 写入也被拒绝");
    Check(connection.Streams().at(stream_id).state() ==
              StreamState::kHalfClosedLocal,
          "R1a: 合法 END_STREAM 写入后应为 half-closed(local)");
}

// ---------------------------------------------------------------------------
// R1b：WriteData 的流控校验必须发生在状态推进/扣额度之前
// ---------------------------------------------------------------------------
void RegressionWriteDataFlowControl() {
    auto fixture = MakeClient();
    auto& connection = *fixture->connection;
    std::uint32_t stream_id = 0;
    Check(connection.StartRequest(RequestHeaders(), false, &stream_id) ==
              ConnStatus::kOk,
          "R1b: StartRequest 失败");

    // 把连接级/流级发送窗口消耗到只剩 1535 字节。
    const std::vector<std::uint8_t> chunk(16000, 'a');
    for (int i = 0; i < 4; ++i) {
        if (connection.WriteData(stream_id, chunk, false) != ConnStatus::kOk) {
            Check(false, "R1b: 窗口填充写入失败");
            return;
        }
    }
    // 超过剩余窗口且带 END_STREAM：应返回 kFlowControl，状态保持 open。
    const std::vector<std::uint8_t> over(2000, 'b');
    Check(connection.WriteData(stream_id, over, true) ==
              ConnStatus::kFlowControl,
          "R1b: 窗口不足应返回 kFlowControl");
    Check(connection.Streams().at(stream_id).state() == StreamState::kOpen,
          "R1b: 流控拒绝的写入把流推进到了 half-closed(local)");

    // 被拒绝的写入也不能扣额度：剩余 1535 字节内的小写入必须成功。
    const std::vector<std::uint8_t> fits(1024, 's');
    Check(connection.WriteData(stream_id, fits, false) == ConnStatus::kOk,
          "R1b: 被拒绝的写入扣减了发送窗口");

    // 文档承诺"等 WINDOW_UPDATE 后重试"：补窗口后重试必须成功。
    fixture->transport.PushInbound(WindowUpdateFrame(0, 65535));
    fixture->transport.PushInbound(WindowUpdateFrame(stream_id, 65535));
    IncomingEvent event;
    Check(connection.ReadEvent(&event) == ConnStatus::kOk,
          "R1b: 读取连接级 WINDOW_UPDATE 失败");
    Check(connection.ReadEvent(&event) == ConnStatus::kOk,
          "R1b: 读取流级 WINDOW_UPDATE 失败");
    Check(connection.WriteData(stream_id, over, true) == ConnStatus::kOk,
          "R1b: 补窗口后重试仍失败");
    Check(connection.Streams().at(stream_id).state() ==
              StreamState::kHalfClosedLocal,
          "R1b: 重试成功后应为 half-closed(local)");
}

// ---------------------------------------------------------------------------
// R2a：SendGoAway 之后连接进入 kDraining，不再接受新流
// ---------------------------------------------------------------------------
void RegressionSendGoAwayDraining() {
    auto fixture = MakeClient();
    auto& connection = *fixture->connection;
    Check(connection.state() == ConnState::kOpen,
          "R2a: 握手后应为 kOpen");
    Check(connection.SendGoAway(ErrorCode::kNoError) == ConnStatus::kOk,
          "R2a: SendGoAway 失败");
    Check(connection.state() == ConnState::kDraining,
          "R2a: 发 GOAWAY 后应为 kDraining");
    std::uint32_t stream_id = 0;
    const auto status =
        connection.StartRequest(RequestHeaders(), true, &stream_id);
    Check(status == ConnStatus::kProtocolError,
          "R2a: 已发 GOAWAY 的连接仍然接受新流");
    Check(connection.state() == ConnState::kDraining,
          "R2a: 被拒绝的 StartRequest 不得改变连接状态");
}

// ---------------------------------------------------------------------------
// R2b：收到 GOAWAY 之后连接进入 kDraining，不再接受新流
// ---------------------------------------------------------------------------
void RegressionReceivedGoAwayDraining() {
    auto fixture = MakeClient();
    auto& connection = *fixture->connection;
    fixture->transport.PushInbound(GoAwayFrame(0, 0));
    IncomingEvent event;
    Check(connection.ReadEvent(&event) == ConnStatus::kOk,
          "R2b: 读取 GOAWAY 失败");
    Check(event.kind == IncomingEvent::Kind::kGoAway,
          "R2b: 事件类型应为 kGoAway");
    Check(connection.state() == ConnState::kDraining,
          "R2b: 收到 GOAWAY 后应为 kDraining");
    Check(connection.peer_last_stream_id() == 0,
          "R2b: peer_last_stream_id 应记录为 0");
    std::uint32_t stream_id = 0;
    const auto status =
        connection.StartRequest(RequestHeaders(), true, &stream_id);
    Check(status == ConnStatus::kProtocolError,
          "R2b: 收到 GOAWAY 后仍接受新流");
}

// ---------------------------------------------------------------------------
// R13：接收侧流控窗口越界必须报 FLOW_CONTROL_ERROR
// ---------------------------------------------------------------------------
void RegressionReceiveWindowOverflow() {
    ConnectionConfig config;
    config.initial_window_size = 1;  // 小于单帧，第一帧就会越界
    auto fixture = MakeClient(config);
    auto& connection = *fixture->connection;
    std::uint32_t stream_id = 0;
    Check(connection.StartRequest(RequestHeaders(), false, &stream_id) ==
              ConnStatus::kOk,
          "R13: StartRequest 失败");

    fixture->transport.PushInbound(DataFrame(stream_id, 16));
    IncomingEvent event;
    const auto status = connection.ReadEvent(&event);
    Check(status == ConnStatus::kProtocolError,
          "R13: 超过接收窗口的 DATA 未被拒绝（应为 FLOW_CONTROL_ERROR）");
}

// ---------------------------------------------------------------------------
// R2c：传输写失败后连接进入 kClosed，后续操作快速失败
// ---------------------------------------------------------------------------
class FailingTransport final : public Transport {
public:
    std::size_t Read(std::uint8_t*, std::size_t) override { return 0; }
    std::size_t Write(const std::uint8_t*, std::size_t) override { return 0; }
};

void RegressionTransportFailureCloses() {
    FailingTransport transport;
    Connection connection(&transport, Role::kClient);
    std::uint32_t stream_id = 0;
    // 握手写 preface 就会失败 -> kClosed 且状态置 kClosed。
    Check(connection.Handshake() == ConnStatus::kClosed,
          "R2c: 写失败时握手应返回 kClosed");
    Check(connection.state() == ConnState::kClosed,
          "R2c: 写失败后连接应为 kClosed");
    Check(connection.StartRequest(RequestHeaders(), true, &stream_id) ==
              ConnStatus::kProtocolError,
          "R2c: 关闭后的 StartRequest 应被拒绝");
    IncomingEvent event;
    Check(connection.ReadEvent(&event) == ConnStatus::kClosed,
          "R2c: 关闭后的 ReadEvent 应返回 kClosed");
}

}  // namespace

int main() {
    std::printf("=== HTTP/2 状态脱节回归测试（R1/R2/R13）===\n");
    std::printf("[R1a] WriteData 尺寸校验晚于状态推进\n");
    RegressionWriteDataOversize();
    std::printf("[R1b] WriteData 流控校验晚于状态推进\n");
    RegressionWriteDataFlowControl();
    std::printf("[R2a] SendGoAway 后进入 draining\n");
    RegressionSendGoAwayDraining();
    std::printf("[R2b] 收到 GOAWAY 后进入 draining\n");
    RegressionReceivedGoAwayDraining();
    std::printf("[R13] 接收流控窗口越界\n");
    RegressionReceiveWindowOverflow();
    std::printf("[R2c] 传输写失败后连接关闭\n");
    RegressionTransportFailureCloses();

    if (g_failures == 0) {
        std::printf("\n[regress] checks=%d failures=0\n", g_checks);
    } else {
        std::printf("\n[regress] checks=%d failures=%d\n", g_checks,
                    g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
