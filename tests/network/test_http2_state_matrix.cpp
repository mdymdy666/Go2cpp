/**
 * @file test_http2_state_matrix.cpp
 * @brief 网络侧状态机穷举矩阵：HTTP/2 流状态机、HTTP/2 连接握手态、
 *        HTTP/1.1 ResponseWriter 流状态、SSE 流状态。
 *
 * ## 矩阵规模
 *
 * | 状态机 | 状态数 | 事件数 | 格数 | 说明 |
 * |---|---|---|---|---|
 * | HTTP/2 StreamState | 7 | 12 | 84 | RFC 9113 §5.1，逐格断言结果 + 下一状态 |
 * | HTTP/2 Connection 握手态 | 2 | 6 | 12 | 未握手必须拒绝发送类操作 |
 * | http::ResponseWriter::StreamState | 4 | 5 | 20 | Idle/HeadersCommitted/Closed/Failed |
 * | sse::StreamState | 4 | 6 | 24 | Idle/Open/Closed/Failed |
 *
 * ## 要点
 *
 * - 每格都断言**返回值**和**迁移后的状态**；非法事件必须返回明确的
 *   `kInvalidTransition` / false / Error::Closed，而不是崩溃或静默成功；
 * - 终态行（Closed/Failed）额外断言"任何事件都不改变终态"；
 * - 额外验证非法事件**不能破坏后续合法事件**（例如 idle 上被拒绝的
 *   RST_STREAM 之后仍必须能发 HEADERS）；
 * - 另加两个线程同时驱动**同一个** StreamStateMachine 的探针：该对象文档
 *   写的是"无锁、无 IO，可在任意线程使用"，但没有任何并发保护。功能层面
 *   只能断言"状态值始终在枚举范围内"，真正的数据竞争由 TSan 报告
 *   （见交付报告的 TSan 小节）。
 */
#include "go2cpp/network/http.hpp"
#include "go2cpp/network/http2/connection.hpp"
#include "go2cpp/network/http2/stream_state.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/network/sse.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <vector>

namespace {

using namespace go2cpp::net::http2;

int g_failures = 0;
int g_cells = 0;

void Check(bool condition, const std::string& what) {
    ++g_cells;
    if (!condition) {
        std::fprintf(stderr, "[net-matrix] FAILED: %s\n", what.c_str());
        ++g_failures;
    }
}

const char* StreamStateName(StreamState state) {
    switch (state) {
        case StreamState::kIdle: return "idle";
        case StreamState::kReservedLocal: return "reserved(local)";
        case StreamState::kReservedRemote: return "reserved(remote)";
        case StreamState::kOpen: return "open";
        case StreamState::kHalfClosedLocal: return "half-closed(local)";
        case StreamState::kHalfClosedRemote: return "half-closed(remote)";
        case StreamState::kClosed: return "closed";
    }
    return "?";
}

StreamStateMachine MakeStreamInState(StreamState target) {
    StreamStateMachine machine(1);
    switch (target) {
        case StreamState::kIdle:
            break;
        case StreamState::kReservedLocal:
            Check(machine.SendPushPromise() == StreamEventResult::kOk,
                  "fixture reserved(local)");
            break;
        case StreamState::kReservedRemote:
            Check(machine.ReceivePushPromise() == StreamEventResult::kOk,
                  "fixture reserved(remote)");
            break;
        case StreamState::kOpen:
            Check(machine.SendHeaders(false) == StreamEventResult::kOk,
                  "fixture open");
            break;
        case StreamState::kHalfClosedLocal:
            Check(machine.SendHeaders(true) == StreamEventResult::kOk,
                  "fixture half-closed(local)");
            break;
        case StreamState::kHalfClosedRemote:
            Check(machine.ReceiveHeaders(true) == StreamEventResult::kOk,
                  "fixture half-closed(remote)");
            break;
        case StreamState::kClosed:
            Check(machine.SendHeaders(false) == StreamEventResult::kOk,
                  "fixture closed: open");
            Check(machine.SendRstStream() == StreamEventResult::kOk,
                  "fixture closed: rst");
            break;
    }
    Check(machine.state() == target, std::string("fixture state mismatch: ") +
                                         StreamStateName(machine.state()));
    return machine;
}

enum class StreamEvent {
    kSendHeaders,
    kSendHeadersEnd,
    kSendData,
    kSendDataEnd,
    kSendRst,
    kSendPush,
    kRecvHeaders,
    kRecvHeadersEnd,
    kRecvData,
    kRecvDataEnd,
    kRecvRst,
    kRecvPush,
};

constexpr int kStreamEventCount = 12;
constexpr int kStreamRowCount = 7;

const char* StreamEventName(StreamEvent event) {
    switch (event) {
        case StreamEvent::kSendHeaders: return "send H";
        case StreamEvent::kSendHeadersEnd: return "send H+ES";
        case StreamEvent::kSendData: return "send D";
        case StreamEvent::kSendDataEnd: return "send D+ES";
        case StreamEvent::kSendRst: return "send RST";
        case StreamEvent::kSendPush: return "send PP";
        case StreamEvent::kRecvHeaders: return "recv H";
        case StreamEvent::kRecvHeadersEnd: return "recv H+ES";
        case StreamEvent::kRecvData: return "recv D";
        case StreamEvent::kRecvDataEnd: return "recv D+ES";
        case StreamEvent::kRecvRst: return "recv RST";
        case StreamEvent::kRecvPush: return "recv PP";
    }
    return "?";
}

StreamEventResult ApplyStreamEvent(StreamStateMachine* machine,
                                   StreamEvent event) {
    switch (event) {
        case StreamEvent::kSendHeaders: return machine->SendHeaders(false);
        case StreamEvent::kSendHeadersEnd: return machine->SendHeaders(true);
        case StreamEvent::kSendData: return machine->SendData(false);
        case StreamEvent::kSendDataEnd: return machine->SendData(true);
        case StreamEvent::kSendRst: return machine->SendRstStream();
        case StreamEvent::kSendPush: return machine->SendPushPromise();
        case StreamEvent::kRecvHeaders: return machine->ReceiveHeaders(false);
        case StreamEvent::kRecvHeadersEnd: return machine->ReceiveHeaders(true);
        case StreamEvent::kRecvData: return machine->ReceiveData(false);
        case StreamEvent::kRecvDataEnd: return machine->ReceiveData(true);
        case StreamEvent::kRecvRst: return machine->ReceiveRstStream();
        case StreamEvent::kRecvPush: return machine->ReceivePushPromise();
    }
    return StreamEventResult::kInvalidTransition;
}

/**
 * RFC 9113 §5.1 的期望矩阵。每一行 12 列，与 StreamEvent 一一对应。
 * `kInvalid` 表示必须明确拒绝；`kOk` 表示合法。下一状态由单独的
 * kStreamExpectedNext 表给出（每行 12 项）。
 */
const StreamEventResult kStreamExpectedResult[kStreamRowCount]
                                             [kStreamEventCount] = {
    // idle：只有 HEADERS / PUSH_PROMISE 合法，RST 也非法
    {StreamEventResult::kOk, StreamEventResult::kOk,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition, StreamEventResult::kOk,
     StreamEventResult::kOk, StreamEventResult::kOk,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition, StreamEventResult::kOk},
    // reserved(local)：只能发 HEADERS；RST 可关闭
    {StreamEventResult::kOk, StreamEventResult::kOk,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition, StreamEventResult::kOk,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition, StreamEventResult::kOk,
     StreamEventResult::kInvalidTransition},
    // reserved(remote)：只能收 HEADERS；RST 可关闭
    {StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition, StreamEventResult::kOk,
     StreamEventResult::kInvalidTransition, StreamEventResult::kOk,
     StreamEventResult::kOk, StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition, StreamEventResult::kOk,
     StreamEventResult::kInvalidTransition},
    // open：双向都合法；PP 非法
    {StreamEventResult::kOk, StreamEventResult::kOk, StreamEventResult::kOk,
     StreamEventResult::kOk, StreamEventResult::kOk,
     StreamEventResult::kInvalidTransition, StreamEventResult::kOk,
     StreamEventResult::kOk, StreamEventResult::kOk, StreamEventResult::kOk,
     StreamEventResult::kOk, StreamEventResult::kInvalidTransition},
    // half-closed(local)：本地已关闭发送方向
    {StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition, StreamEventResult::kOk,
     StreamEventResult::kInvalidTransition, StreamEventResult::kOk,
     StreamEventResult::kOk, StreamEventResult::kOk, StreamEventResult::kOk,
     StreamEventResult::kOk, StreamEventResult::kInvalidTransition},
    // half-closed(remote)：对端已关闭发送方向
    {StreamEventResult::kOk, StreamEventResult::kOk, StreamEventResult::kOk,
     StreamEventResult::kOk, StreamEventResult::kOk,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition,
     StreamEventResult::kInvalidTransition, StreamEventResult::kOk,
     StreamEventResult::kInvalidTransition},
    // closed：一切返回 kStreamClosed
    {StreamEventResult::kStreamClosed, StreamEventResult::kStreamClosed,
     StreamEventResult::kStreamClosed, StreamEventResult::kStreamClosed,
     StreamEventResult::kStreamClosed, StreamEventResult::kStreamClosed,
     StreamEventResult::kStreamClosed, StreamEventResult::kStreamClosed,
     StreamEventResult::kStreamClosed, StreamEventResult::kStreamClosed,
     StreamEventResult::kStreamClosed, StreamEventResult::kStreamClosed},
};

const StreamState kStreamExpectedNext[kStreamRowCount][kStreamEventCount] = {
    // idle
    {StreamState::kOpen, StreamState::kHalfClosedLocal, StreamState::kIdle,
     StreamState::kIdle, StreamState::kIdle, StreamState::kReservedLocal,
     StreamState::kOpen, StreamState::kHalfClosedRemote, StreamState::kIdle,
     StreamState::kIdle, StreamState::kIdle, StreamState::kReservedRemote},
    // reserved(local)
    {StreamState::kHalfClosedRemote, StreamState::kClosed,
     StreamState::kReservedLocal, StreamState::kReservedLocal,
     StreamState::kClosed, StreamState::kReservedLocal,
     StreamState::kReservedLocal, StreamState::kReservedLocal,
     StreamState::kReservedLocal, StreamState::kReservedLocal,
     StreamState::kClosed, StreamState::kReservedLocal},
    // reserved(remote)
    {StreamState::kReservedRemote, StreamState::kReservedRemote,
     StreamState::kReservedRemote, StreamState::kReservedRemote,
     StreamState::kClosed, StreamState::kReservedRemote,
     StreamState::kHalfClosedLocal, StreamState::kClosed,
     StreamState::kReservedRemote, StreamState::kReservedRemote,
     StreamState::kClosed, StreamState::kReservedRemote},
    // open
    {StreamState::kOpen, StreamState::kHalfClosedLocal, StreamState::kOpen,
     StreamState::kHalfClosedLocal, StreamState::kClosed, StreamState::kOpen,
     StreamState::kOpen, StreamState::kHalfClosedRemote, StreamState::kOpen,
     StreamState::kHalfClosedRemote, StreamState::kClosed,
     StreamState::kOpen},
    // half-closed(local)
    {StreamState::kHalfClosedLocal, StreamState::kHalfClosedLocal,
     StreamState::kHalfClosedLocal, StreamState::kHalfClosedLocal,
     StreamState::kClosed, StreamState::kHalfClosedLocal,
     StreamState::kHalfClosedLocal, StreamState::kClosed,
     StreamState::kHalfClosedLocal, StreamState::kClosed,
     StreamState::kClosed, StreamState::kHalfClosedLocal},
    // half-closed(remote)
    {StreamState::kHalfClosedRemote, StreamState::kClosed,
     StreamState::kHalfClosedRemote, StreamState::kClosed,
     StreamState::kClosed, StreamState::kHalfClosedRemote,
     StreamState::kHalfClosedRemote, StreamState::kHalfClosedRemote,
     StreamState::kHalfClosedRemote, StreamState::kHalfClosedRemote,
     StreamState::kClosed, StreamState::kHalfClosedRemote},
    // closed
    {StreamState::kClosed, StreamState::kClosed, StreamState::kClosed,
     StreamState::kClosed, StreamState::kClosed, StreamState::kClosed,
     StreamState::kClosed, StreamState::kClosed, StreamState::kClosed,
     StreamState::kClosed, StreamState::kClosed, StreamState::kClosed},
};

void ScenarioStreamMatrix() {
    std::printf("\n[HTTP/2 StreamState] 7 状态 x 12 事件 = %d 格\n",
                kStreamRowCount * kStreamEventCount);
    const StreamState rows[kStreamRowCount] = {
        StreamState::kIdle,          StreamState::kReservedLocal,
        StreamState::kReservedRemote, StreamState::kOpen,
        StreamState::kHalfClosedLocal, StreamState::kHalfClosedRemote,
        StreamState::kClosed};
    for (int r = 0; r < kStreamRowCount; ++r) {
        for (int e = 0; e < kStreamEventCount; ++e) {
            auto machine = MakeStreamInState(rows[r]);
            const auto event = static_cast<StreamEvent>(e);
            const auto result = ApplyStreamEvent(&machine, event);
            const std::string cell = std::string(StreamStateName(rows[r])) +
                                     " x " + StreamEventName(event);
            Check(result == kStreamExpectedResult[r][e],
                  cell + ": 结果=" + ToString(result) + " 期望=" +
                      ToString(kStreamExpectedResult[r][e]));
            Check(machine.state() == kStreamExpectedNext[r][e],
                  cell + ": 迁移后=" + StreamStateName(machine.state()) +
                      " 期望=" + StreamStateName(kStreamExpectedNext[r][e]));
            if (rows[r] == StreamState::kClosed) {
                Check(machine.closed(), cell + ": 终态被复活");
            }
        }
    }
}

void ScenarioInvalidDoesNotPoison() {
    // idle 上被拒绝的 RST_STREAM 不能破坏后续 HEADERS。
    {
        StreamStateMachine machine(1);
        Check(machine.SendRstStream() == StreamEventResult::kInvalidTransition,
              "idle RST 应被拒绝");
        Check(machine.ReceiveRstStream() ==
                  StreamEventResult::kInvalidTransition,
              "idle recv RST 应被拒绝");
        Check(machine.state() == StreamState::kIdle, "idle 应保持不变");
        Check(machine.SendHeaders(false) == StreamEventResult::kOk,
              "非法 RST 之后仍必须能发 HEADERS");
        Check(machine.state() == StreamState::kOpen, "应进入 open");
    }
    // half-closed(local) 上被拒绝的 DATA 不能破坏后续 RST。
    {
        auto machine = MakeStreamInState(StreamState::kHalfClosedLocal);
        Check(machine.SendData(false) == StreamEventResult::kInvalidTransition,
              "half-closed(local) 发 DATA 应被拒绝");
        Check(machine.SendRstStream() == StreamEventResult::kOk,
              "拒绝 DATA 之后仍必须能发 RST");
        Check(machine.state() == StreamState::kClosed, "RST 后应 closed");
    }
    // 随机事件序列：任何结果下状态都必须是合法枚举值。
    {
        StreamStateMachine machine(7);
        uint32_t seed = 20261018u;
        int ok = 0;
        int invalid = 0;
        int closed = 0;
        for (int i = 0; i < 20000; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const auto event = static_cast<StreamEvent>(seed % kStreamEventCount);
            switch (ApplyStreamEvent(&machine, event)) {
                case StreamEventResult::kOk: ++ok; break;
                case StreamEventResult::kInvalidTransition: ++invalid; break;
                case StreamEventResult::kStreamClosed: ++closed; break;
            }
            const int v = static_cast<int>(machine.state());
            Check(v >= static_cast<int>(StreamState::kIdle) &&
                      v <= static_cast<int>(StreamState::kClosed),
                  "随机序列产生了越界状态");
            if (g_failures > 0) break;
        }
        std::printf("[HTTP/2 StreamState] 随机 20000 步: ok=%d invalid=%d closed=%d\n",
                    ok, invalid, closed);
        Check(ok > 0 && invalid > 0, "随机序列未覆盖三类结果");
    }
}

void ScenarioStreamConcurrentProbe() {
    // 探针：两个线程同时驱动同一个状态机。
    // 该对象没有任何并发保护（文档只说"可在任意线程使用"），这里只做
    // 功能层面的健壮性断言；真正的问题是数据竞争，必须由 TSan 报出。
    std::printf("[HTTP/2 StreamState] 并发探针: 2 线程 x 50000 事件\n");
    for (int round = 0; round < 4; ++round) {
        StreamStateMachine machine(static_cast<std::uint32_t>(round + 1));
        std::atomic<bool> go{false};
        std::atomic<int> ok{0};
        std::atomic<int> invalid{0};
        std::atomic<int> closed{0};
        auto worker = [&](int tid) {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < 50000; ++i) {
                const auto event =
                    static_cast<StreamEvent>((i * 7 + tid * 3) % kStreamEventCount);
                switch (ApplyStreamEvent(&machine, event)) {
                    case StreamEventResult::kOk: ok.fetch_add(1); break;
                    case StreamEventResult::kInvalidTransition:
                        invalid.fetch_add(1);
                        break;
                    case StreamEventResult::kStreamClosed: closed.fetch_add(1); break;
                }
            }
        };
        std::thread first(worker, 0);
        std::thread second(worker, 1);
        go.store(true, std::memory_order_release);
        first.join();
        second.join();
        const int total = ok.load() + invalid.load() + closed.load();
        Check(total == 100000, "并发探针: 事件计数丢失");
        const int v = static_cast<int>(machine.state());
        Check(v >= static_cast<int>(StreamState::kIdle) &&
                  v <= static_cast<int>(StreamState::kClosed),
              "并发探针: 状态越界");
    }
}

// ===========================================================================
// HTTP/2 连接握手态
// ===========================================================================

std::vector<HeaderField> ConnectionRequestHeaders() {
    return {{":method", "POST"}, {":path", "/upload"},
            {":scheme", "https"}, {":authority", "example.com"}};
}

std::vector<std::uint8_t> EmptySettingsFrame() {
    // 9 字节帧头：length=0, type=SETTINGS(4), flags=0, stream=0
    return std::vector<std::uint8_t>{0, 0, 0, 4, 0, 0, 0, 0, 0};
}

void ScenarioConnectionHandshakeMatrix() {
    std::printf("\n[HTTP/2 Connection 握手态] 2 状态 x 6 事件 = 12 格\n");
    // 每个事件都用**全新的连接**：握手事件本身会把对象从"未握手"推进到
    // "已握手"，如果复用同一个对象，后面的格就不是在测原局面了。
    struct Fixture {
        std::unique_ptr<MemoryTransport> transport;
        std::unique_ptr<Connection> connection;
    };
    auto make = [](bool handshaked, bool with_inbound = true) {
        Fixture fixture;
        fixture.transport = std::make_unique<MemoryTransport>();
        if (with_inbound) {
            fixture.transport->PushInbound(EmptySettingsFrame());
        }
        fixture.connection = std::make_unique<Connection>(
            fixture.transport.get(), Role::kClient);
        if (handshaked) {
            Check(fixture.connection->Handshake() == ConnStatus::kOk,
                  "connection fixture: handshake");
            Check(fixture.connection->handshaked(),
                  "connection fixture: handshaked");
        }
        return fixture;
    };
    for (int handshaked = 0; handshaked < 2; ++handshaked) {
        const std::string prefix = handshaked ? "Handshaked" : "PreHandshake";

        // 事件 1：Handshake（重复调用必须幂等）
        {
            auto fixture = make(handshaked);
            Check(fixture.connection->Handshake() == ConnStatus::kOk,
                  prefix + " x Handshake: 必须返回 kOk");
            Check(fixture.connection->handshaked(),
                  prefix + " x Handshake: 状态未推进");
        }
        // 事件 2：StartRequest
        {
            auto fixture = make(handshaked);
            std::uint32_t stream_id = 0;
            const auto status = fixture.connection->StartRequest(
                ConnectionRequestHeaders(), true, &stream_id);
            if (handshaked) {
                Check(status == ConnStatus::kOk,
                      prefix + " x StartRequest: 应成功，实际 " +
                          ToString(status));
                Check(stream_id == 1, prefix + " x StartRequest: 流 id 应为 1");
                const auto& streams = fixture.connection->Streams();
                Check(streams.size() == 1 &&
                          streams.at(1).state() ==
                              go2cpp::net::http2::StreamState::kHalfClosedLocal,
                      prefix + " x StartRequest: END_STREAM 流应为 "
                               "half-closed(local)");
            } else {
                Check(status == ConnStatus::kProtocolError,
                      prefix + " x StartRequest: 未握手必须明确拒绝");
            }
        }
        // 事件 3/4：WriteData / SendResponseHeaders 落在未知流上
        {
            auto fixture = make(handshaked);
            const auto write_status = fixture.connection->WriteData(
                1, std::vector<std::uint8_t>{1, 2, 3}, false);
            const auto header_status =
                fixture.connection->SendResponseHeaders(1, {{":status", "200"}},
                                                        true);
            Check(write_status == ConnStatus::kProtocolError,
                  prefix + " x WriteData(未知流): 必须拒绝");
            Check(header_status == ConnStatus::kProtocolError,
                  prefix + " x SendResponseHeaders(未知流): 必须拒绝");
        }
        // 事件 5：ResetStream 未知流
        {
            auto fixture = make(handshaked);
            const auto status =
                fixture.connection->ResetStream(999, ErrorCode::kCancel);
            Check(status == ConnStatus::kProtocolError,
                  prefix + " x ResetStream(未知流): 必须拒绝");
        }
        // 事件 6：ReadEvent（对端无数据 / 空指针）
        {
            // 这一格测的是"没有数据可读"，所以握手用的 SETTINGS 不能留在
            // 输入缓冲里，否则 ReadEvent 会先把它当成 SETTINGS 事件读走。
            auto fixture = make(handshaked, /*with_inbound=*/handshaked);
            IncomingEvent event;
            const auto status = fixture.connection->ReadEvent(&event);
            Check(status == ConnStatus::kClosed,
                  prefix + " x ReadEvent(无数据): 应返回 kClosed，实际 " +
                      ToString(status));
            Check(fixture.connection->ReadEvent(nullptr) ==
                      ConnStatus::kProtocolError,
                  prefix + " x ReadEvent(nullptr): 必须明确拒绝");
        }
    }
}

// ===========================================================================
// HTTP/1.1 ResponseWriter 流状态
// ===========================================================================

struct SocketPair {
    go2cpp::net::Socket::Ptr writer;
    go2cpp::net::Socket::Ptr peer;
};

SocketPair MakeSocketPair() {
    int pair[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        Check(false, "socketpair 失败");
        return {};
    }
    SocketPair result;
    result.writer =
        go2cpp::net::Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
    result.peer = go2cpp::net::Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);
    return result;
}

enum class WriterEvent { kStart, kWrite, kFlush, kClose, kState };

constexpr int kWriterEventCount = 5;

const char* WriterEventName(WriterEvent event) {
    switch (event) {
        case WriterEvent::kStart: return "start";
        case WriterEvent::kWrite: return "write";
        case WriterEvent::kFlush: return "flush";
        case WriterEvent::kClose: return "close";
        case WriterEvent::kState: return "state()";
    }
    return "?";
}

void ScenarioResponseWriterMatrix() {
    using go2cpp::http::ResponseWriter;
    using go2cpp::http::StreamState;
    std::printf("\n[HTTP/1.1 ResponseWriter] 4 状态 x 5 事件 = 20 格\n");
    const StreamState rows[4] = {StreamState::Idle, StreamState::HeadersCommitted,
                                 StreamState::Closed, StreamState::Failed};
    const char* row_names[4] = {"Idle", "HeadersCommitted", "Closed", "Failed"};
    for (int r = 0; r < 4; ++r) {
        for (int e = 0; e < kWriterEventCount; ++e) {
            const auto event = static_cast<WriterEvent>(e);
            const std::string cell =
                std::string(row_names[r]) + " x " + WriterEventName(event);
            SocketPair pair = MakeSocketPair();
            auto writer = std::make_unique<ResponseWriter>(pair.writer);
            bool fixture_ok = true;
            switch (rows[r]) {
                case StreamState::Idle:
                    break;
                case StreamState::HeadersCommitted:
                    fixture_ok = writer->start();
                    break;
                case StreamState::Closed:
                    fixture_ok = writer->start() && writer->close();
                    break;
                case StreamState::Failed: {
                    // 用一个已关闭的 fd 构造 writer：start 的写必然失败，
                    // 从而进入 Failed 终态。这样不会触发 SIGPIPE。
                    int dead_pair[2] = {-1, -1};
                    Check(::socketpair(AF_UNIX, SOCK_STREAM, 0, dead_pair) == 0,
                          "dead socketpair 失败");
                    auto dead = go2cpp::net::Socket::FromFd(
                        dead_pair[0], AF_UNIX, SOCK_STREAM);
                    auto unused = go2cpp::net::Socket::FromFd(
                        dead_pair[1], AF_UNIX, SOCK_STREAM);
                    (void)unused;
                    Check(dead->close(), "关闭 socket 失败");
                    writer = std::make_unique<ResponseWriter>(dead);
                    fixture_ok = !writer->start();
                    break;
                }
            }
            Check(fixture_ok, cell + ": fixture 构造失败");
            Check(writer->state() == rows[r],
                  cell + ": fixture 状态=" +
                      std::string(writer->state() == StreamState::Idle ? "Idle"
                                  : writer->state() == StreamState::HeadersCommitted
                                      ? "HeadersCommitted"
                                  : writer->state() == StreamState::Closed
                                      ? "Closed"
                                      : "Failed") +
                      " 期望 " + row_names[r]);

            bool expected_ok = false;
            StreamState expected_after = rows[r];
            switch (event) {
                case WriterEvent::kStart:
                    expected_ok = rows[r] == StreamState::Idle;
                    expected_after = expected_ok ? StreamState::HeadersCommitted
                                                 : rows[r];
                    Check(writer->start() == expected_ok,
                          cell + ": start 返回值不符");
                    break;
                case WriterEvent::kWrite:
                    expected_ok = rows[r] == StreamState::HeadersCommitted;
                    Check(writer->write("x") == expected_ok,
                          cell + ": write 返回值不符");
                    break;
                case WriterEvent::kFlush:
                    expected_ok = rows[r] == StreamState::HeadersCommitted;
                    Check(writer->flush() == expected_ok,
                          cell + ": flush 返回值不符");
                    break;
                case WriterEvent::kClose:
                    expected_ok = rows[r] == StreamState::HeadersCommitted;
                    expected_after = expected_ok ? StreamState::Closed : rows[r];
                    Check(writer->close() == expected_ok,
                          cell + ": close 返回值不符");
                    break;
                case WriterEvent::kState:
                    Check(writer->state() == rows[r], cell + ": state 不符");
                    break;
            }
            Check(writer->state() == expected_after,
                  cell + ": 迁移后状态不符");
            if (rows[r] == StreamState::Closed ||
                rows[r] == StreamState::Failed) {
                Check(writer->state() == rows[r], cell + ": 终态被复活");
            }
        }
    }
}

// ===========================================================================
// SSE 流状态
// ===========================================================================

enum class SseEvent { kStart, kSend, kComment, kTick, kClose, kState };

constexpr int kSseEventCount = 6;

const char* SseEventName(SseEvent event) {
    switch (event) {
        case SseEvent::kStart: return "start";
        case SseEvent::kSend: return "send";
        case SseEvent::kComment: return "comment";
        case SseEvent::kTick: return "tick";
        case SseEvent::kClose: return "close";
        case SseEvent::kState: return "state()";
    }
    return "?";
}

void ScenarioSseMatrix() {
    using go2cpp::sse::Stream;
    using go2cpp::sse::StreamState;
    std::printf("\n[SSE StreamState] 4 状态 x 6 事件 = 24 格\n");
    const StreamState rows[4] = {StreamState::Idle, StreamState::Open,
                                 StreamState::Closed, StreamState::Failed};
    const char* row_names[4] = {"Idle", "Open", "Closed", "Failed"};
    for (int r = 0; r < 4; ++r) {
        for (int e = 0; e < kSseEventCount; ++e) {
            const auto event = static_cast<SseEvent>(e);
            const std::string cell =
                std::string(row_names[r]) + " x " + SseEventName(event);
            SocketPair pair = MakeSocketPair();
            go2cpp::sse::StreamOptions options;
            options.heartbeat_interval = std::chrono::milliseconds(0);
            auto stream = std::make_unique<Stream>(pair.writer, nullptr, options);
            bool fixture_ok = true;
            switch (rows[r]) {
                case StreamState::Idle:
                    break;
                case StreamState::Open:
                    fixture_ok = stream->start().ok();
                    break;
                case StreamState::Closed:
                    fixture_ok = stream->start().ok() && stream->close().ok();
                    break;
                case StreamState::Failed: {
                    // Failed 只能由 IO 写失败触发（start 时 socket 无效只会
                    // 返回 Error::Io，并不改状态）。这里先正常 start，再关闭
                    // 对端并用带写超时的写入把 sndbuf 打满，逼出 EPIPE/EAGAIN。
                    options.write_timeout = std::chrono::milliseconds(50);
                    stream = std::make_unique<Stream>(pair.writer, nullptr,
                                                      options);
                    fixture_ok = stream->start().ok();
                    if (fixture_ok) {
                        (void)pair.peer->close();
                        go2cpp::sse::Event big;
                        big.data.assign(256 * 1024, 'x');
                        bool failed = false;
                        for (int attempt = 0; attempt < 64 && !failed;
                             ++attempt) {
                            failed = !stream->send(big).ok();
                        }
                        fixture_ok = failed;
                    }
                    break;
                }
            }
            Check(fixture_ok, cell + ": fixture 构造失败");

            bool expected_ok = false;
            StreamState expected_after = rows[r];
            switch (event) {
                case SseEvent::kStart:
                    expected_ok = rows[r] == StreamState::Idle;
                    expected_after =
                        expected_ok ? StreamState::Open : rows[r];
                    Check(stream->start().ok() == expected_ok,
                          cell + ": start 返回值不符");
                    break;
                case SseEvent::kSend: {
                    go2cpp::sse::Event value;
                    value.data = "hello";
                    expected_ok = rows[r] == StreamState::Open;
                    Check(stream->send(value).ok() == expected_ok,
                          cell + ": send 返回值不符");
                    break;
                }
                case SseEvent::kComment:
                    expected_ok = rows[r] == StreamState::Open;
                    Check(stream->comment("hi").ok() == expected_ok,
                          cell + ": comment 返回值不符");
                    break;
                case SseEvent::kTick:
                    expected_ok = rows[r] == StreamState::Open;
                    Check(stream->tick().ok() == expected_ok,
                          cell + ": tick 返回值不符");
                    break;
                case SseEvent::kClose:
                    // close 只在 Open 上成功；Closed 上幂等成功；
                    // Idle/Failed 必须明确拒绝。
                    expected_ok = rows[r] == StreamState::Open ||
                                  rows[r] == StreamState::Closed;
                    expected_after = rows[r] == StreamState::Open
                                         ? StreamState::Closed
                                         : rows[r];
                    Check(stream->close().ok() == expected_ok,
                          cell + ": close 返回值不符");
                    break;
                case SseEvent::kState:
                    Check(stream->state() == rows[r], cell + ": state 不符");
                    break;
            }
            Check(stream->state() == expected_after, cell + ": 迁移后状态不符");
        }
    }
}

}  // namespace

int main() {
    // 写 socket 失败时不要因为 SIGPIPE 杀死测试进程。
    std::signal(SIGPIPE, SIG_IGN);
    std::printf("=== 网络侧状态机穷举矩阵 ===\n");
    ScenarioStreamMatrix();
    ScenarioInvalidDoesNotPoison();
    ScenarioStreamConcurrentProbe();
    ScenarioConnectionHandshakeMatrix();
    ScenarioResponseWriterMatrix();
    ScenarioSseMatrix();

    if (g_failures == 0) {
        std::printf("\n[net-matrix] cells=%d failures=0\n", g_cells);
    } else {
        std::printf("\n[net-matrix] cells=%d failures=%d\n", g_cells,
                    g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
