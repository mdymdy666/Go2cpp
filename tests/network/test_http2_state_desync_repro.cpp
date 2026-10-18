/**
 * @file test_http2_state_desync_repro.cpp
 * @brief **已知缺陷的复现测试**（当前应当失败；修复后转正）。
 *
 * ## 为什么单独放一个文件而不是直接塞进 ctest 主套件
 *
 * 本文件断言的是**期望语义**（RFC 9113 + 类注释），而不是当前实现行为。
 * 在缺陷修复之前它会稳定失败，所以 CMake 里用 `DISABLED TRUE` 注册 ——
 * 直接跑 `ctest` 不会把它算成回归；需要验证时显式执行：
 *
 * ```
 * cmake --build build-quality --target go2cpp_http2_state_desync_repro
 * ./build-quality/go2cpp_http2_state_desync_repro
 * ```
 *
 * 修复对应缺陷后，删掉 CMake 里的 `DISABLED TRUE` 即可转成常规回归测试。
 *
 * ## 覆盖的三个缺陷
 *
 * 1. `Connection::WriteData` 先推进流状态机、再做尺寸/流控校验：被拒绝的
 *    写入仍把 END_STREAM 记在流上，导致"没有发出 END_STREAM 却已
 *    half-closed"的状态与线上状态脱节，且按文档提示"等 WINDOW_UPDATE 后
 *    重试"会永久失败；
 * 2. `SendGoAway` 不记录任何连接级状态，发完仍然接受新流；
 * 3. 收到对端 GOAWAY 后不进入 draining 状态，仍然接受新流；
 * 4. 接收侧流控窗口越界不报 FLOW_CONTROL_ERROR（RFC 9113 §6.9.1）。
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

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[repro] FAILED: %s\n", what);
        ++g_failures;
    }
}

std::vector<std::uint8_t> EmptySettingsFrame() {
    return std::vector<std::uint8_t>{0, 0, 0, 4, 0, 0, 0, 0, 0};
}

std::vector<std::uint8_t> WindowUpdateFrame(std::uint32_t stream_id,
                                            std::uint32_t increment) {
    std::vector<std::uint8_t> frame(13, 0);
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 4;  // length = 4
    frame[3] = 8;  // type = WINDOW_UPDATE
    frame[4] = 0;  // flags
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

std::unique_ptr<Handshaked> MakeClient() {
    auto fixture = std::make_unique<Handshaked>();
    fixture->transport.PushInbound(EmptySettingsFrame());
    fixture->connection = std::make_unique<Connection>(
        &fixture->transport, Role::kClient);
    Check(fixture->connection->Handshake() == ConnStatus::kOk,
          "fixture: handshake 失败");
    return fixture;
}

// ---------------------------------------------------------------------------
// 缺陷 1：WriteData 的尺寸校验发生在 SendData 之后
// ---------------------------------------------------------------------------
void ReproWriteDataOversizeDesync() {
    auto fixture = MakeClient();
    auto& connection = *fixture->connection;
    std::uint32_t stream_id = 0;
    Check(connection.StartRequest(RequestHeaders(), false, &stream_id) ==
              ConnStatus::kOk,
          "repro1: StartRequest 失败");
    Check(connection.Streams().at(stream_id).state() ==
              StreamState::kOpen,
          "repro1: 初始流应 open");

    // 超过 SETTINGS_MAX_FRAME_SIZE（默认 16384）的 DATA：必须被拒绝。
    const std::vector<std::uint8_t> oversized(20000, 'x');
    const auto rejected =
        connection.WriteData(stream_id, oversized, true /*end_stream*/);
    Check(rejected == ConnStatus::kUnsupported,
          "repro1: 超尺寸 DATA 应返回 kUnsupported");

    // 关键断言：被拒绝的写入不得推进流状态机。
    Check(connection.Streams().at(stream_id).state() == StreamState::kOpen,
          "repro1: 被拒绝的写入把流推进到了 half-closed(local)/closed —— "
          "状态机与线上状态脱节");

    // 合法的小写入（带 END_STREAM）必须仍能成功。
    const std::vector<std::uint8_t> small{'o', 'k'};
    Check(connection.WriteData(stream_id, small, true) == ConnStatus::kOk,
          "repro1: 被拒绝的写入之后，合法的 END_STREAM 写入也被拒绝");
}

// ---------------------------------------------------------------------------
// 缺陷 1b：WriteData 的流控校验发生在 SendData 之后
// ---------------------------------------------------------------------------
void ReproWriteDataFlowControlDesync() {
    auto fixture = MakeClient();
    auto& connection = *fixture->connection;
    std::uint32_t stream_id = 0;
    Check(connection.StartRequest(RequestHeaders(), false, &stream_id) ==
              ConnStatus::kOk,
          "repro1b: StartRequest 失败");

    // 把连接级/流级发送窗口消耗到只剩 1535 字节。
    const std::vector<std::uint8_t> chunk(16000, 'a');
    for (int i = 0; i < 4; ++i) {
        if (connection.WriteData(stream_id, chunk, false) != ConnStatus::kOk) {
            Check(false, "repro1b: 窗口填充写入失败");
            return;
        }
    }
    // 超过剩余窗口且带 END_STREAM：应返回 kFlowControl，状态保持 open。
    const std::vector<std::uint8_t> over(2000, 'b');
    Check(connection.WriteData(stream_id, over, true) ==
              ConnStatus::kFlowControl,
          "repro1b: 窗口不足应返回 kFlowControl");
    Check(connection.Streams().at(stream_id).state() == StreamState::kOpen,
          "repro1b: 流控拒绝的写入把流推进到了 half-closed(local)");

    // 文档明确写着"调用方应等待后重试"：补窗口后重试必须成功。
    fixture->transport.PushInbound(WindowUpdateFrame(0, 65535));
    fixture->transport.PushInbound(WindowUpdateFrame(stream_id, 65535));
    IncomingEvent event;
    Check(connection.ReadEvent(&event) == ConnStatus::kOk,
          "repro1b: 读取连接级 WINDOW_UPDATE 失败");
    Check(connection.ReadEvent(&event) == ConnStatus::kOk,
          "repro1b: 读取流级 WINDOW_UPDATE 失败");
    Check(connection.WriteData(stream_id, over, true) == ConnStatus::kOk,
          "repro1b: 补窗口后重试仍失败（状态机已提前 half-closed）");
}

// ---------------------------------------------------------------------------
// 缺陷 2：SendGoAway 后仍接受新流
// ---------------------------------------------------------------------------
void ReproSendGoAwayStillAcceptsNewStreams() {
    auto fixture = MakeClient();
    auto& connection = *fixture->connection;
    Check(connection.SendGoAway(ErrorCode::kNoError) == ConnStatus::kOk,
          "repro2: SendGoAway 失败");
    std::uint32_t stream_id = 0;
    const auto status =
        connection.StartRequest(RequestHeaders(), true, &stream_id);
    Check(status == ConnStatus::kProtocolError,
          "repro2: 已发 GOAWAY 的连接仍然接受新流（类注释声称"
          "\"标记连接不再接受新流\"）");
}

// ---------------------------------------------------------------------------
// 缺陷 3：收到 GOAWAY 后仍接受新流
// ---------------------------------------------------------------------------
void ReproReceivedGoAwayStillAcceptsNewStreams() {
    auto fixture = MakeClient();
    auto& connection = *fixture->connection;
    fixture->transport.PushInbound(GoAwayFrame(0, 0));
    IncomingEvent event;
    Check(connection.ReadEvent(&event) == ConnStatus::kOk,
          "repro3: 读取 GOAWAY 失败");
    Check(event.kind == IncomingEvent::Kind::kGoAway,
          "repro3: 事件类型应为 kGoAway");
    std::uint32_t stream_id = 0;
    const auto status =
        connection.StartRequest(RequestHeaders(), true, &stream_id);
    Check(status == ConnStatus::kProtocolError,
          "repro3: 收到 GOAWAY 后仍接受新流（连接缺少 draining/closed 状态）");
}


// ---------------------------------------------------------------------------
// 缺陷 4：接收侧流控窗口越界不报 FLOW_CONTROL_ERROR
// ---------------------------------------------------------------------------
void ReproReceiveWindowOverflowAccepted() {
    auto fixture = MakeClient();
    auto& connection = *fixture->connection;
    std::uint32_t stream_id = 0;
    Check(connection.StartRequest(RequestHeaders(), false, &stream_id) ==
              ConnStatus::kOk,
          "repro4: StartRequest 失败");

    // 默认连接级接收窗口 65535。对端连续发 5 个满帧 DATA（5 x 16384），
    // 第 5 帧之后窗口已为负；RFC 9113 §6.9.1 要求接收方回
    // FLOW_CONTROL_ERROR，当前实现照单全收并继续回 WINDOW_UPDATE。
    std::vector<std::uint8_t> frame(9 + 16384, 0);
    frame[1] = 0x40;  // length = 0x004000 = 16384
    frame[2] = 0x00;
    frame[3] = 0x00;  // type = DATA
    frame[4] = 0x00;  // flags
    frame[5] = 0x00;
    frame[6] = 0x00;
    frame[7] = 0x00;
    frame[8] = static_cast<std::uint8_t>(stream_id);
    bool saw_error = false;
    for (int i = 0; i < 5; ++i) {
        fixture->transport.PushInbound(frame);
        IncomingEvent event;
        const auto status = connection.ReadEvent(&event);
        if (status != ConnStatus::kOk) {
            saw_error = true;
            break;
        }
    }
    Check(saw_error,
          "repro4: 超过接收窗口的对端 DATA 被当成合法事件接收"
          "（应为 FLOW_CONTROL_ERROR）");
}

}  // namespace

int main() {
    std::printf("=== HTTP/2 状态脱节缺陷复现（修复前应失败）===\n");
    std::printf("[repro1]  WriteData 尺寸校验晚于状态推进\n");
    ReproWriteDataOversizeDesync();
    std::printf("[repro1b] WriteData 流控校验晚于状态推进\n");
    ReproWriteDataFlowControlDesync();
    std::printf("[repro2]  SendGoAway 后仍接受新流\n");
    ReproSendGoAwayStillAcceptsNewStreams();
    std::printf("[repro3]  收到 GOAWAY 后仍接受新流\n");
    ReproReceivedGoAwayStillAcceptsNewStreams();
    std::printf("[repro4]  接收流控窗口越界未报 FLOW_CONTROL_ERROR\n");
    ReproReceiveWindowOverflowAccepted();

    if (g_failures == 0) {
        std::printf("\n[repro] failures=0（缺陷已修复，可去掉 DISABLED）\n");
    } else {
        std::printf("\n[repro] failures=%d（预期非零：缺陷仍在）\n",
                    g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
