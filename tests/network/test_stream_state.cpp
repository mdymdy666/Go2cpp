/**
 * @file test_stream_state.cpp
 * @brief HTTP/2 流状态机测试（RFC 9113 §5.1）。
 *
 * 核心是**穷举矩阵**：7 个状态 × 8 个事件，逐个验证合法性。这能抓到"half-closed
 * 方向搞反"这类最难发现的错误 —— 它们不会崩，只会让连接在后续请求上莫名报错。
 */
#include "go2cpp/network/http2/stream_state.hpp"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace go2cpp::net::http2;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[stream] FAILED: %s\n", what);
        ++g_failures;
    }
}

/// 把状态机驱动到指定状态（用一条最短事件路径）。
StreamStateMachine MakeInState(StreamState target) {
    StreamStateMachine sm(1);
    switch (target) {
        case StreamState::kIdle:
            break;
        case StreamState::kReservedLocal:
            sm.SendPushPromise();
            break;
        case StreamState::kReservedRemote:
            sm.ReceivePushPromise();
            break;
        case StreamState::kOpen:
            sm.SendHeaders(false);
            break;
        case StreamState::kHalfClosedLocal:
            sm.SendHeaders(true);
            break;
        case StreamState::kHalfClosedRemote:
            sm.ReceiveHeaders(true);
            break;
        case StreamState::kClosed:
            sm.SendHeaders(false);
            sm.SendRstStream();
            break;
    }
    return sm;
}

// ---------------------------------------------------------------------------
// 场景 1：完整生命周期
// ---------------------------------------------------------------------------
void ScenarioLifecycle() {
    StreamStateMachine sm(1);
    Check(sm.state() == StreamState::kIdle, "fresh stream must be idle");
    Check(sm.SendHeaders(false) == StreamEventResult::kOk, "send headers failed");
    Check(sm.state() == StreamState::kOpen, "after send headers must be open");
    Check(sm.SendData(false) == StreamEventResult::kOk, "send data on open failed");
    Check(sm.SendData(true) == StreamEventResult::kOk, "send end_stream failed");
    Check(sm.state() == StreamState::kHalfClosedLocal,
          "after local end_stream must be half-closed(local)");
    Check(sm.ReceiveData(true) == StreamEventResult::kOk, "receive data failed");
    Check(sm.state() == StreamState::kClosed, "after both end_stream must be closed");
    Check(sm.closed(), "closed() should be true");
    // 关闭后任何事件都是 kStreamClosed
    Check(sm.SendData(false) == StreamEventResult::kStreamClosed,
          "send after closed must be kStreamClosed");
    Check(sm.ReceiveHeaders(false) == StreamEventResult::kStreamClosed,
          "recv after closed must be kStreamClosed");
}

// ---------------------------------------------------------------------------
// 场景 2：穷举合法性矩阵
//
// 期望值直接来自 RFC 9113 §5.1：
//   - idle 上除 HEADERS/PUSH_PROMISE(PP) 外都是协议错误（含 RST）；
//   - half-closed(local) 不能再发 DATA/HEADERS；half-closed(remote) 不能再收；
//   - reserved(local) 只能发 HEADERS；reserved(remote) 只能收 HEADERS；
//   - closed 上一切返回 kStreamClosed。
// ---------------------------------------------------------------------------
struct EventCase {
    const char* name;
    bool local;      // true = 本地发送
    int kind;        // 0=HEADERS(no ES) 1=HEADERS(ES) 2=DATA 3=DATA(ES) 4=RST 5=PP
};

StreamEventResult Apply(StreamStateMachine* sm, const EventCase& e, bool end_stream) {
    if (e.local) {
        switch (e.kind) {
            case 0: case 1: return sm->SendHeaders(end_stream);
            case 2: case 3: return sm->SendData(end_stream);
            case 4: return sm->SendRstStream();
            default: return sm->SendPushPromise();
        }
    }
    switch (e.kind) {
        case 0: case 1: return sm->ReceiveHeaders(end_stream);
        case 2: case 3: return sm->ReceiveData(end_stream);
        case 4: return sm->ReceiveRstStream();
        default: return sm->ReceivePushPromise();
    }
}

void ScenarioMatrix() {
    const std::vector<StreamState> states = {
        StreamState::kIdle, StreamState::kOpen, StreamState::kHalfClosedLocal,
        StreamState::kHalfClosedRemote, StreamState::kReservedLocal,
        StreamState::kReservedRemote, StreamState::kClosed,
    };
    struct Kind { const char* name; bool local; int kind; bool es; };
    const std::vector<Kind> kinds = {
        {"send H", true, 0, false},        {"send H+ES", true, 1, true},
        {"send D", true, 2, false},        {"send D+ES", true, 3, true},
        {"send RST", true, 4, false},      {"send PP", true, 5, false},
        {"recv H", false, 0, false},       {"recv H+ES", false, 1, true},
        {"recv D", false, 2, false},       {"recv D+ES", false, 3, true},
        {"recv RST", false, 4, false},     {"recv PP", false, 5, false},
    };

    int checked = 0;
    for (const auto state : states) {
        for (const auto& k : kinds) {
            StreamStateMachine sm = MakeInState(state);
            const auto before = sm.state();
            const auto result = Apply(&sm, EventCase{k.name, k.local, k.kind}, k.es);
            ++checked;

            // 1) closed 上一切返回 kStreamClosed
            if (state == StreamState::kClosed) {
                if (result != StreamEventResult::kStreamClosed) {
                    std::fprintf(stderr,
                                 "[stream] 矩阵: closed 上 %s 返回 %s（应为 stream-closed）\n",
                                 k.name, ToString(result));
                    Check(false, "closed must return kStreamClosed");
                }
                continue;
            }
            // 2) idle 上只有 HEADERS / PP 合法（RST 也非法）
            if (state == StreamState::kIdle) {
                const bool allowed = (k.kind == 0 || k.kind == 1 || k.kind == 5);
                if (allowed && result != StreamEventResult::kOk) {
                    std::fprintf(stderr, "[stream] 矩阵: idle 上 %s 应合法，实际 %s\n",
                                 k.name, ToString(result));
                    Check(false, "idle allows HEADERS/PP");
                }
                if (!allowed && result == StreamEventResult::kOk) {
                    std::fprintf(stderr, "[stream] 矩阵: idle 上 %s 应非法，实际成功\n", k.name);
                    Check(false, "idle rejects others");
                }
            }
            // 3) half-closed(local) 不能再发 D/H
            if (state == StreamState::kHalfClosedLocal &&
                k.local && (k.kind == 0 || k.kind == 1 || k.kind == 2 || k.kind == 3)) {
                if (result == StreamEventResult::kOk) {
                    std::fprintf(stderr,
                                 "[stream] 矩阵: half-closed(local) 上 %s 应非法\n", k.name);
                    Check(false, "half-closed(local) rejects local send");
                }
            }
            // 4) half-closed(remote) 不能再收 D/H
            if (state == StreamState::kHalfClosedRemote &&
                !k.local && (k.kind == 0 || k.kind == 1 || k.kind == 2 || k.kind == 3)) {
                if (result == StreamEventResult::kOk) {
                    std::fprintf(stderr,
                                 "[stream] 矩阵: half-closed(remote) 上 %s 应非法\n", k.name);
                    Check(false, "half-closed(remote) rejects remote send");
                }
            }
            // 5) reserved(local) 只能发 H；reserved(remote) 只能收 H
            if (state == StreamState::kReservedLocal && k.local &&
                (k.kind == 2 || k.kind == 3)) {
                if (result == StreamEventResult::kOk) {
                    Check(false, "reserved(local) rejects send DATA");
                }
            }
            if (state == StreamState::kReservedRemote && !k.local &&
                (k.kind == 2 || k.kind == 3)) {
                if (result == StreamEventResult::kOk) {
                    Check(false, "reserved(remote) rejects recv DATA");
                }
            }
            // 6) 除 RST 外，事件不应把状态意外改回 idle
            if (result == StreamEventResult::kOk && k.kind != 4) {
                if (sm.state() == StreamState::kIdle) {
                    Check(false, "successful event must not return to idle");
                }
            }
            (void)before;
        }
    }
    std::printf("[stream] 矩阵覆盖 %d 个组合（状态 x 事件）\n", checked);
}

// ---------------------------------------------------------------------------
// 场景 3：PUSH_PROMISE 路径
// ---------------------------------------------------------------------------
void ScenarioPushPromise() {
    // 本端 push：reserved(local) -> send HEADERS -> half-closed(remote)
    {
        StreamStateMachine sm(2);
        Check(sm.SendPushPromise() == StreamEventResult::kOk, "send PP failed");
        Check(sm.state() == StreamState::kReservedLocal, "after send PP must be reserved(local)");
        Check(sm.SendHeaders(false) == StreamEventResult::kOk, "send headers after PP failed");
        Check(sm.state() == StreamState::kHalfClosedRemote,
              "promised stream must be half-closed(remote)");
        Check(sm.SendData(true) == StreamEventResult::kOk, "promised stream can send data");
        Check(sm.state() == StreamState::kClosed, "promised stream should close after ES");
    }
    // 对端 push：reserved(remote) -> recv HEADERS -> half-closed(local)
    {
        StreamStateMachine sm(4);
        Check(sm.ReceivePushPromise() == StreamEventResult::kOk, "recv PP failed");
        Check(sm.state() == StreamState::kReservedRemote,
              "after recv PP must be reserved(remote)");
        Check(sm.ReceiveHeaders(false) == StreamEventResult::kOk, "recv headers after PP failed");
        Check(sm.state() == StreamState::kHalfClosedLocal,
              "received promised stream must be half-closed(local)");
    }
}

// ---------------------------------------------------------------------------
// 场景 4：RST_STREAM 从任何非 idle 状态都关闭
// ---------------------------------------------------------------------------
void ScenarioRstCloses() {
    const std::vector<StreamState> states = {
        StreamState::kOpen, StreamState::kHalfClosedLocal,
        StreamState::kHalfClosedRemote, StreamState::kReservedLocal,
        StreamState::kReservedRemote,
    };
    for (const auto state : states) {
        {
            StreamStateMachine sm = MakeInState(state);
            Check(sm.SendRstStream() == StreamEventResult::kOk, "send RST should succeed");
            Check(sm.state() == StreamState::kClosed, "send RST must close the stream");
        }
        {
            StreamStateMachine sm = MakeInState(state);
            Check(sm.ReceiveRstStream() == StreamEventResult::kOk, "recv RST should succeed");
            Check(sm.state() == StreamState::kClosed, "recv RST must close the stream");
        }
    }
    // idle 上 RST 非法（RFC §5.1）
    {
        StreamStateMachine sm(1);
        Check(sm.SendRstStream() == StreamEventResult::kInvalidTransition,
              "send RST on idle must be invalid");
        Check(sm.ReceiveRstStream() == StreamEventResult::kInvalidTransition,
              "recv RST on idle must be invalid");
        Check(sm.state() == StreamState::kIdle, "idle must stay idle after invalid RST");
    }
}

// ---------------------------------------------------------------------------
// 场景 5：随机事件序列（不崩、状态始终合法）
// ---------------------------------------------------------------------------
void ScenarioRandomSequence() {
    std::mt19937 rng(20261017);
    std::uniform_int_distribution<int> event_dist(0, 7);
    constexpr int kRounds = 5000;
    int ok_count = 0;
    int invalid_count = 0;
    int closed_count = 0;
    for (int round = 0; round < kRounds; ++round) {
        StreamStateMachine sm(static_cast<std::uint32_t>(round + 1));
        for (int step = 0; step < 20; ++step) {
            const auto result = [&] {
                switch (event_dist(rng)) {
                    case 0: return sm.SendHeaders(false);
                    case 1: return sm.SendHeaders(true);
                    case 2: return sm.SendData(false);
                    case 3: return sm.SendData(true);
                    case 4: return sm.SendRstStream();
                    case 5: return sm.ReceiveHeaders(false);
                    case 6: return sm.ReceiveData(false);
                    default: return sm.ReceiveRstStream();
                }
            }();
            switch (result) {
                case StreamEventResult::kOk: ++ok_count; break;
                case StreamEventResult::kInvalidTransition: ++invalid_count; break;
                case StreamEventResult::kStreamClosed: ++closed_count; break;
            }
            // 状态必须始终是 7 个合法值之一
            const auto v = static_cast<int>(sm.state());
            if (v < 0 || v > static_cast<int>(StreamState::kClosed)) {
                Check(false, "state out of enum range");
            }
        }
    }
    std::printf("[stream] 随机序列: ok=%d invalid=%d closed=%d\n", ok_count, invalid_count,
                closed_count);
    Check(ok_count > 0 && invalid_count > 0 && closed_count > 0,
          "random sequence should exercise all three outcomes");
}

}  // namespace

int main() {
    std::printf("=== HTTP/2 流状态机测试（RFC 9113 §5.1）===\n");
    std::printf("[stream][场景1] 完整生命周期\n");
    ScenarioLifecycle();
    std::printf("[stream][场景2] 穷举合法性矩阵\n");
    ScenarioMatrix();
    std::printf("[stream][场景3] PUSH_PROMISE 路径\n");
    ScenarioPushPromise();
    std::printf("[stream][场景4] RST_STREAM 关闭语义\n");
    ScenarioRstCloses();
    std::printf("[stream][场景5] 随机事件序列 5000 轮\n");
    ScenarioRandomSequence();

    if (g_failures == 0) {
        std::printf("\n[stream] failures=0\n");
    } else {
        std::printf("\n[stream] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
