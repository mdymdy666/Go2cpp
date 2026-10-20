/**
 * @file test_diagnostic.cpp
 * @brief Doctor（在线诊断 + 受控解锁）的测试。
 *
 * 覆盖（按项目规范：压测 + 随机 + >=5 个真实场景）：
 *   场景 1  帧协议：往返、粘包/拆包、校验和、非法输入、超长拒绝
 *   场景 2  status：协程/M-P/runqueue/modules(build-id)，响应 stream_id 匹配
 *   场景 3  并发 10 条命令：串行执行（concurrent_peak==1），响应按 stream_id 归并
 *   场景 4  大响应分块：chunked / last_chunk / seq 递增
 *   场景 5  force-wake 白名单：mutex scope refused、未知 scope 报错
 *   场景 6  丢唤醒死锁：force-wake all 恢复（真实 Scheduler + ParkingCondition）
 *   场景 7  Channel spurious wake：唤醒后仍等待，Send 后正常拿到值
 *   场景 8  mutex-unlock：owner 已终止时成功解锁并唤醒等待者；owner 存活时 refused
 *   场景 9  全局无进展检测：deadlock-report 给出 deadlock 判定
 *   场景 10 诊断 Fiber：inject-fiber 在受管 G 上下文里执行 traverse
 *   场景 11 Unix socket 端到端：真实 socket 收发 G2CD 帧
 *   压测    200 次命令往返（status/force-wake/stats）
 *   随机    1000 次随机字节/随机截断的帧解析（不崩溃、不误判）
 */

#include "go2cpp/debug/diagnostic_agent.hpp"
#include "go2cpp/debug/diagnostic_protocol.hpp"
#include "go2cpp/json.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using go2cpp::Channel;
using go2cpp::Scheduler;
using go2cpp::SchedulerConfig;
using go2cpp::Task;
using go2cpp::core::ParkingCondition;
using go2cpp::debug::DiagnosticAgent;
using go2cpp::debug::DiagnosticAgentConfig;
using go2cpp::debug::DiagnosticEnvelope;
using go2cpp::debug::DiagnosticFrame;
using go2cpp::debug::DiagnosticFrameKind;
using go2cpp::debug::DiagnosticFrameParser;
using go2cpp::debug::MemoryDiagnosticTransport;

bool WaitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

bool TaskDone(const std::shared_ptr<Task>& task) {
    if (!task) {
        return true;
    }
    const auto state = task->state();
    return state == go2cpp::GState::kDead ||
           state == go2cpp::GState::kFailed ||
           state == go2cpp::GState::kCancelled;
}

std::string RunCommand(DiagnosticAgent& agent, const std::string& command,
                       bool* ok = nullptr) {
    const auto result = agent.Execute(command);
    if (ok != nullptr) {
        *ok = result.ok;
    }
    return result.payload;
}

/// 通过 Memory transport 走完整的「接收线程 -> 队列 -> 执行线程」链路。
class Harness {
public:
    bool Start(int recv_timeout_ms = 20,
               std::chrono::milliseconds detect_interval =
                   std::chrono::milliseconds(50),
               std::chrono::milliseconds deadlock_threshold =
                   std::chrono::milliseconds(200),
               std::size_t chunk_bytes = go2cpp::debug::kDiagnosticDefaultChunk) {
        config_.recv_timeout_ms = recv_timeout_ms;
        config_.detect_interval = detect_interval;
        config_.deadlock_threshold = deadlock_threshold;
        config_.chunk_bytes = chunk_bytes;
        config_.enable_deadlock_detection = true;
        transport_ = std::make_shared<MemoryDiagnosticTransport>();
        const bool started = DiagnosticAgent::Instance().Start(config_, transport_);
        return started;
    }

    void Stop() {
        DiagnosticAgent::Instance().Stop();
        transport_.reset();
    }

    bool RoundTrip(const std::string& command, std::uint32_t stream_id,
                   std::string* payload, DiagnosticFrameKind* kind,
                   std::chrono::milliseconds timeout =
                       std::chrono::milliseconds(4000)) {
        DiagnosticFrame request;
        request.kind = DiagnosticFrameKind::kRequest;
        request.stream_id = stream_id;
        request.payload = command;
        if (!transport_->PushRequest(std::move(request))) {
            return false;
        }
        std::string assembled;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            DiagnosticFrame frame;
            if (!transport_->PopResponse(frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (frame.stream_id != stream_id) {
                // 串行执行下通常不会出现；放进待处理列表以免丢失其它响应。
                other_responses_.push_back(std::move(frame));
                continue;
            }
            assembled += frame.payload;
            if (!frame.chunked() || frame.last_chunk()) {
                if (payload != nullptr) {
                    *payload = std::move(assembled);
                }
                if (kind != nullptr) {
                    *kind = frame.kind;
                }
                return true;
            }
        }
        return false;
    }

    MemoryDiagnosticTransport& transport() { return *transport_; }

private:
    DiagnosticAgentConfig config_;
    std::shared_ptr<MemoryDiagnosticTransport> transport_;
    std::vector<DiagnosticFrame> other_responses_;
};

void TestProtocol() {
    go2cpp_tests::announce("diagnostic protocol");
    using namespace go2cpp::debug;

    DiagnosticFrame frame;
    frame.kind = DiagnosticFrameKind::kRequest;
    frame.stream_id = 42;
    frame.seq = 3;
    frame.flags = kDiagnosticFlagJson;
    frame.payload = "status json";
    std::string encoded;
    GO2CPP_CHECK(EncodeDiagnosticFrame(frame, encoded));
    GO2CPP_CHECK(encoded.size() == kDiagnosticHeaderBytes + frame.payload.size());

    const auto decoded = DecodeDiagnosticFrame(encoded);
    GO2CPP_CHECK(decoded.status == DiagnosticDecodeResult::Status::kOk);
    GO2CPP_CHECK(decoded.frame.stream_id == 42);
    GO2CPP_CHECK(decoded.frame.seq == 3);
    GO2CPP_CHECK(decoded.frame.json());
    GO2CPP_CHECK(decoded.frame.payload == "status json");
    GO2CPP_CHECK(decoded.consumed == encoded.size());

    // 逐字节喂入：任意切分都必须能拼出同一帧。
    DiagnosticFrameParser parser;
    DiagnosticFrame parsed;
    bool got = false;
    for (unsigned char byte : encoded) {
        GO2CPP_CHECK(parser.Feed(&byte, 1));
        if (!got && parser.Next(parsed)) {
            got = true;
        }
    }
    GO2CPP_CHECK(got);
    GO2CPP_CHECK(parsed.payload == "status json");
    GO2CPP_CHECK(!parser.failed());

    // 损坏校验和。
    std::string corrupt = encoded;
    corrupt[corrupt.size() - 1] = static_cast<char>('X');
    const auto bad = DecodeDiagnosticFrame(corrupt);
    GO2CPP_CHECK(bad.status == DiagnosticDecodeResult::Status::kError);
    GO2CPP_CHECK(bad.error == "bad-checksum");

    // magic / version / kind / flags 非法。
    std::string bad_magic = encoded;
    bad_magic[0] = '\0';
    GO2CPP_CHECK(DecodeDiagnosticFrame(bad_magic).error == "bad-magic");
    std::string bad_version = encoded;
    bad_version[4] = 9;
    GO2CPP_CHECK(DecodeDiagnosticFrame(bad_version).error == "unsupported-version");
    std::string bad_kind = encoded;
    bad_kind[5] = 9;
    GO2CPP_CHECK(DecodeDiagnosticFrame(bad_kind).error == "bad-kind");

    // 超长 payload 编码拒绝，且不污染输出。
    DiagnosticFrame huge;
    huge.payload.assign(kDiagnosticMaxPayload + 1, 'x');
    std::string untouched = "keep";
    GO2CPP_CHECK(!EncodeDiagnosticFrame(huge, untouched));
    GO2CPP_CHECK(untouched == "keep");

    // 截断：头部不足 / payload 不足都返回 NeedMore。
    GO2CPP_CHECK(DecodeDiagnosticFrame(encoded.substr(0, 10)).status ==
                 DiagnosticDecodeResult::Status::kNeedMore);
    GO2CPP_CHECK(DecodeDiagnosticFrame(encoded.substr(0, encoded.size() - 1))
                     .status == DiagnosticDecodeResult::Status::kNeedMore);

    // 随机字节 / 随机截断：解析器要么 NeedMore，要么 Error，绝不崩溃。
    std::mt19937_64 random(20261020);
    for (int round = 0; round < 1000; ++round) {
        std::string bytes(static_cast<std::size_t>(random() % 96), '\0');
        for (char& byte : bytes) {
            byte = static_cast<char>(random() & 0xFF);
        }
        DiagnosticFrameParser random_parser;
        (void)random_parser.Feed(bytes);
        DiagnosticFrame ignored;
        while (random_parser.Next(ignored)) {
        }
        const std::size_t cut = encoded.size() == 0
                                    ? 0
                                    : static_cast<std::size_t>(
                                          random() % encoded.size());
        GO2CPP_CHECK(DecodeDiagnosticFrame(encoded.substr(0, cut)).status !=
                     DiagnosticDecodeResult::Status::kOk);
    }
}

void TestStatusAndQueue() {
    go2cpp_tests::announce("diagnostic status + single-thread queue");
    DiagnosticAgent& agent = DiagnosticAgent::Instance();
    agent.ResetRegistrations();

    SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 2;
    Scheduler scheduler(config);
    scheduler.start();
    GO2CPP_CHECK(agent.RegisterScheduler("scheduler", scheduler));

    Harness harness;
    GO2CPP_CHECK(harness.Start());
    GO2CPP_CHECK(agent.running());
    const auto before = agent.stats();

    std::string payload;
    DiagnosticFrameKind kind = DiagnosticFrameKind::kResponse;
    GO2CPP_CHECK(harness.RoundTrip("status", 7, &payload, &kind));
    GO2CPP_CHECK(kind == DiagnosticFrameKind::kResponse);
    GO2CPP_CHECK(payload.find("schema=go2cpp.diagnostic.status/1") !=
                 std::string::npos);
    GO2CPP_CHECK(payload.find("scheduler.0.processors=2") != std::string::npos);
    GO2CPP_CHECK(payload.find("module.0.build_id=") != std::string::npos);
    GO2CPP_CHECK(payload.find("doctor.concurrent_peak=") != std::string::npos);
    GO2CPP_CHECK(payload.find("[doctor duration_us=") != std::string::npos);

    // JSON 版本可被项目 JSON 解析。
    GO2CPP_CHECK(harness.RoundTrip("status json", 8, &payload, &kind));
    const auto parsed = go2cpp::ParseJson(payload);
    GO2CPP_CHECK(parsed.ok());
    GO2CPP_CHECK(parsed.value["schema"] == "go2cpp.diagnostic.status/1");
    GO2CPP_CHECK(parsed.value["schedulers"].is_array());
    GO2CPP_CHECK(parsed.value["modules"].is_array());

    // 未知命令 -> error 帧。
    GO2CPP_CHECK(harness.RoundTrip("no-such-command", 9, &payload, &kind));
    GO2CPP_CHECK(kind == DiagnosticFrameKind::kError);
    GO2CPP_CHECK(payload.find("unknown-command") != std::string::npos);

    // dump 复用 DebugServer 的 schema。
    GO2CPP_CHECK(harness.RoundTrip("dump sections=scheduler", 10, &payload,
                                   &kind));
    GO2CPP_CHECK(payload.find("schema=go2cpp.dump.scheduler/1") !=
                 std::string::npos);

    // 压测：200 次命令往返；统计里的 accepted/executed 与之一致。
    for (int round = 0; round < 200; ++round) {
        const std::string command =
            (round % 3 == 0) ? "status" : ((round % 3 == 1) ? "stats"
                                                            : "deadlock-report");
        GO2CPP_CHECK(harness.RoundTrip(command,
                                       static_cast<std::uint32_t>(100 + round),
                                       &payload, &kind));
    }
    const auto stats = agent.stats();
    GO2CPP_CHECK(stats.accepted - before.accepted >= 204);
    GO2CPP_CHECK(stats.executed - before.executed >= 204);
    GO2CPP_CHECK(stats.concurrent_peak == 1);
    GO2CPP_CHECK(stats.max_queue_depth >= 1);

    harness.Stop();
    agent.UnregisterScheduler("scheduler");
    agent.ResetRegistrations();
    scheduler.shutdown();
}

void TestConcurrentTen() {
    go2cpp_tests::announce("diagnostic concurrent 10 commands");
    DiagnosticAgent& agent = DiagnosticAgent::Instance();
    agent.ResetRegistrations();

    SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 2;
    Scheduler scheduler(config);
    scheduler.start();
    GO2CPP_CHECK(agent.RegisterScheduler("scheduler", scheduler));

    Harness harness;
    GO2CPP_CHECK(harness.Start(10));
    // 先把已有统计清零的观测基线取下来（单例可能已被前序场景使用）。
    const auto before = agent.stats();

    constexpr int kCommands = 10;
    for (int index = 0; index < kCommands; ++index) {
        DiagnosticFrame request;
        request.kind = DiagnosticFrameKind::kRequest;
        request.stream_id = static_cast<std::uint32_t>(1000 + index);
        request.payload = (index % 2 == 0) ? "status" : "deadlock-report";
        GO2CPP_CHECK(harness.transport().PushRequest(std::move(request)));
    }
    std::vector<bool> seen(kCommands, false);
    int last_chunks = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (last_chunks < kCommands &&
           std::chrono::steady_clock::now() < deadline) {
        DiagnosticFrame frame;
        if (!harness.transport().PopResponse(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (frame.stream_id < 1000U ||
            frame.stream_id >= 1000U + kCommands) {
            continue;
        }
        const std::size_t index = frame.stream_id - 1000U;
        GO2CPP_CHECK(!seen[index] || frame.chunked());
        if (frame.chunked() && !frame.last_chunk()) {
            continue;
        }
        if (!seen[index]) {
            seen[index] = true;
            ++last_chunks;
        }
    }
    GO2CPP_CHECK(last_chunks == kCommands);
    for (bool value : seen) {
        GO2CPP_CHECK(value);
    }
    const auto after = agent.stats();
    GO2CPP_CHECK(after.executed - before.executed >= kCommands);
    GO2CPP_CHECK(after.concurrent_peak == 1);

    harness.Stop();
    agent.ResetRegistrations();
    scheduler.shutdown();
}

void TestChunked() {
    go2cpp_tests::announce("diagnostic chunked response");
    DiagnosticAgent& agent = DiagnosticAgent::Instance();
    agent.ResetRegistrations();
    SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    Scheduler scheduler(config);
    scheduler.start();
    GO2CPP_CHECK(agent.RegisterScheduler("scheduler", scheduler));

    Harness harness;
    GO2CPP_CHECK(harness.Start(20, std::chrono::milliseconds(100),
                               std::chrono::milliseconds(200), 128));
    GO2CPP_CHECK(harness.transport().PushRequest([] {
        DiagnosticFrame request;
        request.kind = DiagnosticFrameKind::kRequest;
        request.stream_id = 77;
        request.payload = "status";
        return request;
    }()));
    std::string assembled;
    int frames = 0;
    std::uint32_t expected_seq = 0;
    bool complete = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
    while (!complete && std::chrono::steady_clock::now() < deadline) {
        DiagnosticFrame frame;
        if (!harness.transport().PopResponse(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        GO2CPP_CHECK(frame.stream_id == 77);
        GO2CPP_CHECK(frame.chunked());
        GO2CPP_CHECK(frame.seq == expected_seq++);
        assembled += frame.payload;
        ++frames;
        if (frame.last_chunk()) {
            complete = true;
        }
    }
    GO2CPP_CHECK(complete);
    GO2CPP_CHECK(frames > 1);
    GO2CPP_CHECK(assembled.find("schema=go2cpp.diagnostic.status/1") !=
                 std::string::npos);

    harness.Stop();
    agent.ResetRegistrations();
    scheduler.shutdown();
}

void TestForceWakeWhitelist() {
    go2cpp_tests::announce("diagnostic force-wake whitelist");
    DiagnosticAgent& agent = DiagnosticAgent::Instance();
    agent.ResetRegistrations();
    SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    Scheduler scheduler(config);
    scheduler.start();
    GO2CPP_CHECK(agent.RegisterScheduler("scheduler", scheduler));

    Harness harness;
    GO2CPP_CHECK(harness.Start());
    std::string payload;
    DiagnosticFrameKind kind = DiagnosticFrameKind::kResponse;

    // 未注册 mutex 等待者时显式请求 scope=mutex：必须 refused。
    GO2CPP_CHECK(harness.RoundTrip("force-wake scope=mutex", 1, &payload,
                                   &kind));
    GO2CPP_CHECK(kind == DiagnosticFrameKind::kResponse);
    GO2CPP_CHECK(payload.find("verdict=refused") != std::string::npos);
    GO2CPP_CHECK(payload.find("mutex-waiters-are-not-retryable") !=
                 std::string::npos);

    // 未知 scope：参数错误。
    GO2CPP_CHECK(harness.RoundTrip("force-wake scope=bogus", 2, &payload,
                                   &kind));
    GO2CPP_CHECK(kind == DiagnosticFrameKind::kError);
    GO2CPP_CHECK(payload.find("bad-scope") != std::string::npos);

    // 合法 scope 即使没有目标也应成功（verdict=ok，targets=0）。
    GO2CPP_CHECK(harness.RoundTrip("force-wake scope=parking-condition", 3,
                                   &payload, &kind));
    GO2CPP_CHECK(payload.find("verdict=ok") != std::string::npos);
    GO2CPP_CHECK(payload.find("skipped=no-registered-target") !=
                 std::string::npos);

    harness.Stop();
    agent.ResetRegistrations();
    scheduler.shutdown();
}

void TestLostWakeRecovery() {
    go2cpp_tests::announce("diagnostic lost-wake recovery");
    DiagnosticAgent& agent = DiagnosticAgent::Instance();
    agent.ResetRegistrations();

    SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 2;
    Scheduler scheduler(config);
    scheduler.start();
    GO2CPP_CHECK(agent.RegisterScheduler("scheduler", scheduler));

    Harness harness;
    GO2CPP_CHECK(harness.Start(20, std::chrono::milliseconds(50),
                               std::chrono::milliseconds(100)));

    // 用真正的条件变量构造「丢唤醒」：值已改变，但没有 notify。
    ParkingCondition condition;
    GO2CPP_CHECK(agent.RegisterParkingCondition("lost-wake", condition));
    std::mutex mutex;
    bool ready = false;
    auto waiter = scheduler.spawn([&] {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return ready; });
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return condition.DebugSnapshot().waiters == 1U; },
        std::chrono::milliseconds(2000)));
    {
        std::lock_guard<std::mutex> lock(mutex);
        ready = true;  // 数据已就绪，故意不 notify（模拟丢唤醒）。
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    GO2CPP_CHECK(condition.DebugSnapshot().waiters == 1U);
    GO2CPP_CHECK(!TaskDone(waiter));

    // 死锁检测应在这段时间内给出 deadlock/stalled 判定。
    std::string payload;
    DiagnosticFrameKind kind = DiagnosticFrameKind::kResponse;
    GO2CPP_CHECK(harness.RoundTrip("deadlock-report", 1, &payload, &kind));
    GO2CPP_CHECK(payload.find("verdict=deadlock") != std::string::npos ||
                 payload.find("verdict=stalled") != std::string::npos);

    // force-wake all：parking-condition 唤醒 G，scheduler-idle 唤醒 worker。
    GO2CPP_CHECK(harness.RoundTrip("force-wake scope=all", 2, &payload, &kind));
    GO2CPP_CHECK(payload.find("verdict=ok") != std::string::npos);
    GO2CPP_CHECK(WaitUntil([&] { return TaskDone(waiter); },
                           std::chrono::milliseconds(3000)));
    GO2CPP_CHECK(condition.DebugSnapshot().waiters == 0U);

    // 恢复后统计里应记录到至少一次 deadlock 事件（检测线程在无命令时空转）。
    const auto stats = agent.stats();
    GO2CPP_CHECK(agent.deadlock_verdict() ==
                     go2cpp::debug::DeadlockVerdict::kDeadlock ||
                 stats.deadlock_events >= 1);

    harness.Stop();
    agent.ResetRegistrations();
    scheduler.shutdown();
}

void TestChannelSpuriousWake() {
    go2cpp_tests::announce("diagnostic channel spurious wake");
    DiagnosticAgent& agent = DiagnosticAgent::Instance();
    agent.ResetRegistrations();
    SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 2;
    Scheduler scheduler(config);
    scheduler.start();

    auto channel = Channel<int>::Create(0);
    std::atomic<int> received{-1};
    auto receiver = scheduler.spawn([&] {
        const auto result = channel->Recv();
        if (result.value.has_value()) {
            received.store(*result.value);
        }
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return channel->DebugSnapshot().recv_waiters == 1U; },
        std::chrono::milliseconds(2000)));

    GO2CPP_CHECK(agent.RegisterScheduler("scheduler", scheduler));
    GO2CPP_CHECK(agent.RegisterChannel("data", channel));
    Harness harness;
    GO2CPP_CHECK(harness.Start());
    std::string payload;
    DiagnosticFrameKind kind = DiagnosticFrameKind::kResponse;
    GO2CPP_CHECK(harness.RoundTrip("force-wake scope=channel", 1, &payload,
                                   &kind));
    GO2CPP_CHECK(payload.find("woken_total=1") != std::string::npos);
    // spurious wake 不改变语义：接收者重新检查后继续等待。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    GO2CPP_CHECK(received.load() == -1);
    GO2CPP_CHECK(channel->DebugSnapshot().recv_waiters == 1U);
    GO2CPP_CHECK(channel->Send(9).status == go2cpp::ChannelStatus::kReady);
    GO2CPP_CHECK(WaitUntil([&] { return received.load() == 9; },
                           std::chrono::milliseconds(2000)));
    GO2CPP_CHECK(WaitUntil([&] { return TaskDone(receiver); },
                           std::chrono::milliseconds(2000)));

    harness.Stop();
    agent.ResetRegistrations();
    scheduler.shutdown();
}

void TestMutexUnlock() {
    go2cpp_tests::announce("diagnostic mutex-unlock");
    go2cpp::sync::SetHeldLockParkDetection(true);
    DiagnosticAgent& agent = DiagnosticAgent::Instance();
    agent.ResetRegistrations();
    SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 2;
    Scheduler scheduler(config);
    scheduler.start();
    GO2CPP_CHECK(agent.RegisterScheduler("scheduler", scheduler));

    // 场景 A：owner 仍存活 -> refused。
    go2cpp::sync::Mutex owned;
    GO2CPP_CHECK(owned.Lock());
    GO2CPP_CHECK(agent.RegisterMutex("owned", owned));
    auto live_waiter = scheduler.spawn([&] {
        if (owned.Lock()) {
            owned.Unlock();
        }
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return owned.DebugSnapshotInternal().waiters > 0U; },
        std::chrono::milliseconds(2000)));
    Harness harness;
    GO2CPP_CHECK(harness.Start());
    std::string payload;
    DiagnosticFrameKind kind = DiagnosticFrameKind::kResponse;
    GO2CPP_CHECK(harness.RoundTrip("mutex-unlock name=owned", 1, &payload,
                                   &kind));
    GO2CPP_CHECK(payload.find("refused") != std::string::npos);
    GO2CPP_CHECK(payload.find("owner-alive") != std::string::npos ||
                 payload.find("owner-unknown") != std::string::npos);
    owned.Unlock();
    GO2CPP_CHECK(WaitUntil([&] { return TaskDone(live_waiter); },
                           std::chrono::milliseconds(2000)));

    // 场景 B：owner G 已终止且未解锁 -> 受控解锁，等待者拿到锁。
    go2cpp::sync::Mutex abandoned;
    std::atomic<bool> acquired{false};
    auto owner = scheduler.spawn([&] {
        GO2CPP_CHECK(abandoned.Lock());
        // 故意不 Unlock：G 结束后锁永久持有（模拟临界区泄漏）。
    });
    GO2CPP_CHECK(owner->wait_for(std::chrono::seconds(2)));
    const auto abandoned_snapshot = abandoned.DebugSnapshotInternal();
    GO2CPP_CHECK(abandoned_snapshot.held);
    GO2CPP_CHECK(abandoned_snapshot.owner_known);
    GO2CPP_CHECK(abandoned_snapshot.owner_gid != 0U);
    auto waiter = scheduler.spawn([&] {
        if (abandoned.Lock()) {
            acquired.store(true);
            abandoned.Unlock();
        }
    });
    GO2CPP_CHECK(WaitUntil(
        [&] { return abandoned.DebugSnapshotInternal().waiters > 0U; },
        std::chrono::milliseconds(2000)));
    GO2CPP_CHECK(agent.RegisterMutex("abandoned", abandoned));
    GO2CPP_CHECK(harness.RoundTrip("mutex-unlock name=abandoned", 2, &payload,
                                   &kind));
    GO2CPP_CHECK(kind == DiagnosticFrameKind::kResponse);
    GO2CPP_CHECK(payload.find("verdict=ok") != std::string::npos);
    GO2CPP_CHECK(payload.find("reason=owner-gone") != std::string::npos);
    GO2CPP_CHECK(WaitUntil([&] { return acquired.load(); },
                           std::chrono::milliseconds(3000)));
    GO2CPP_CHECK(!abandoned.DebugSnapshotInternal().held);

    // 未注册对象：拒绝。
    GO2CPP_CHECK(harness.RoundTrip("mutex-unlock name=missing", 3, &payload,
                                   &kind));
    GO2CPP_CHECK(kind == DiagnosticFrameKind::kError);
    GO2CPP_CHECK(payload.find("not-registered") != std::string::npos);

    harness.Stop();
    agent.ResetRegistrations();
    scheduler.shutdown();
}

void TestDiagnosticFiber() {
    go2cpp_tests::announce("diagnostic fiber injection");
    DiagnosticAgent& agent = DiagnosticAgent::Instance();
    agent.ResetRegistrations();
    SchedulerConfig config;
    config.processor_count = 2;
    config.min_workers = 2;
    config.max_workers = 2;
    Scheduler scheduler(config);
    scheduler.start();
    GO2CPP_CHECK(agent.RegisterScheduler("scheduler", scheduler));
    GO2CPP_CHECK(agent.InstallDiagnosticFiber("doctor-fiber", scheduler));
    GO2CPP_CHECK(agent.diagnostic_fiber_installed());

    Harness harness;
    GO2CPP_CHECK(harness.Start());
    std::string payload;
    DiagnosticFrameKind kind = DiagnosticFrameKind::kResponse;
    GO2CPP_CHECK(harness.RoundTrip("inject-fiber fn=traverse json=1", 1,
                                   &payload, &kind));
    GO2CPP_CHECK(kind == DiagnosticFrameKind::kResponse);
    const auto parsed = go2cpp::ParseJson(payload);
    GO2CPP_CHECK(parsed.ok());
    if (!parsed.ok()) {
        harness.Stop();
        agent.StopDiagnosticFiber();
        agent.ResetRegistrations();
        scheduler.shutdown();
        return;
    }
    GO2CPP_CHECK(parsed.value["schema"] == "go2cpp.diagnostic.inject-fiber/1");
    GO2CPP_CHECK(parsed.value["result"]["context"]["managed"].get<bool>());
    GO2CPP_CHECK(parsed.value["result"]["context"]["gid"].get<std::int64_t>() >
                 0);
    GO2CPP_CHECK(parsed.value["result"]["tasks"].is_array());
    GO2CPP_CHECK(parsed.value["result"]["processors"].is_array());

    // 未注册函数 -> 错误。
    GO2CPP_CHECK(harness.RoundTrip("inject-fiber fn=nope", 2, &payload, &kind));
    GO2CPP_CHECK(kind == DiagnosticFrameKind::kError);
    GO2CPP_CHECK(payload.find("unknown-function") != std::string::npos);

    harness.Stop();
    agent.StopDiagnosticFiber();
    agent.ResetRegistrations();
    scheduler.shutdown();
}

void TestUnixSocket() {
    go2cpp_tests::announce("diagnostic unix socket end-to-end");
    DiagnosticAgent& agent = DiagnosticAgent::Instance();
    agent.ResetRegistrations();
    SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    Scheduler scheduler(config);
    scheduler.start();
    GO2CPP_CHECK(agent.RegisterScheduler("scheduler", scheduler));

    DiagnosticAgentConfig agent_config;
    agent_config.socket_path =
        "/tmp/go2cpp-doctor-test-" + std::to_string(::getpid()) + ".sock";
    GO2CPP_CHECK(agent.Start(agent_config));
    GO2CPP_CHECK(agent.running());

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    GO2CPP_CHECK(fd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, agent.socket_path().c_str(),
                 sizeof(address.sun_path) - 1);
    GO2CPP_CHECK(::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                           sizeof(address)) == 0);

    DiagnosticFrame request;
    request.kind = DiagnosticFrameKind::kRequest;
    request.stream_id = 5;
    request.payload = "status json";
    const std::string bytes = go2cpp::debug::EncodeDiagnosticFrame(request);
    GO2CPP_CHECK(!bytes.empty());
    GO2CPP_CHECK(::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL) ==
                 static_cast<ssize_t>(bytes.size()));

    DiagnosticFrameParser parser;
    DiagnosticFrame response;
    bool got = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!got && std::chrono::steady_clock::now() < deadline) {
        char buffer[4096];
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        GO2CPP_CHECK(parser.Feed(buffer, static_cast<std::size_t>(received)));
        while (parser.Next(response)) {
            if (response.stream_id == 5) {
                got = true;
            }
        }
    }
    ::close(fd);
    GO2CPP_CHECK(got);
    GO2CPP_CHECK(response.kind == DiagnosticFrameKind::kResponse);
    GO2CPP_CHECK(response.payload.find("go2cpp.diagnostic.status/1") !=
                 std::string::npos);
    GO2CPP_CHECK(::access(agent.socket_path().c_str(), F_OK) == 0);

    agent.Stop();
    GO2CPP_CHECK(::access(agent.socket_path().c_str(), F_OK) != 0);
    agent.ResetRegistrations();
    scheduler.shutdown();
}

}  // namespace

void run_diagnostic_tests() {
    TestProtocol();
    TestStatusAndQueue();
    TestConcurrentTen();
    TestChunked();
    TestForceWakeWhitelist();
    TestLostWakeRecovery();
    TestChannelSpuriousWake();
    TestMutexUnlock();
    TestDiagnosticFiber();
    TestUnixSocket();
}
