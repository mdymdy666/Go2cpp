#include "go2cpp/rd_net/protocol.hpp"

/// @file test_protocol.cpp
/// @brief REQ-008 验收：
///        1) 零拷贝（memset 反证：SendTo 后立刻把源缓冲覆盖成 0xCC，对端收到 0xCC）；
///        2) 20 行自定义 echo 协议挂载（Go2Cpp 零改动）；
///        3) 上层 ACK 位图 32→64 位（Go2Cpp 无感）；
///        4) SendTo 入队后立刻返回（1 万次 <10ms，心跳定时器不延迟）；
///        5) 单 tick 每 socket ≤64 包；
///        6) 定时器 id 回传与 detach 原因。

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "go2cpp/rd_net/event_loop.hpp"

namespace {

namespace p = rd::net;
using Clock = std::chrono::steady_clock;
using rd::net::Endpoint;
using rd::net::IoStatus;

int g_checks = 0;
int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

std::unique_ptr<rd::net::EventLoop> MakeLoop() {
    auto created = rd::net::EventLoop::Create();
    if (!created.ok()) {
        std::printf("[FAIL] EventLoop::Create status=%s\n",
                    rd::net::IoStatusName(created.status()));
        ++g_failures;
        return nullptr;
    }
    return std::move(created.value());
}

std::unique_ptr<rd::net::DatagramSocket> OpenUdp() {
    Endpoint local;
    local.host = "127.0.0.1";
    local.port = 0;
    auto opened = rd::net::DatagramSocket::Open(local);
    if (!opened.ok()) {
        std::printf("[FAIL] DatagramSocket::Open status=%s\n",
                    rd::net::IoStatusName(opened.status()));
        ++g_failures;
        return nullptr;
    }
    return std::make_unique<rd::net::DatagramSocket>(std::move(opened.value()));
}

void RunFor(rd::net::EventLoop& loop, std::chrono::milliseconds duration) {
    loop.ScheduleAfter(duration, [&loop]() { loop.Stop(); });
    loop.Run();
}

int64_t ElapsedNs(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count();
}

// ---------------------------------------------------------------------------
// 通用记录器：收到什么记什么，附带 tick 序号
// ---------------------------------------------------------------------------
class Recorder : public p::Protocol {
public:
    explicit Recorder(rd::net::EventLoop* loop = nullptr) : loop_(loop) {}

    void OnAttached(p::ProtocolContext&) override {}
    void OnDatagram(p::ProtocolContext&, p::ByteView data,
                    const Endpoint&) override {
        last_.assign(reinterpret_cast<const char*>(data.data),
                     reinterpret_cast<const char*>(data.data) + data.size);
        frames_.push_back(last_);
        ++count_;
        ticks_.push_back(loop_ != nullptr ? loop_->tick_count() : 0);
    }
    void OnTimer(p::ProtocolContext&, std::uint64_t) override {}
    void OnSendComplete(p::ProtocolContext&, std::uint64_t,
                        rd::net::SendResult) override {}
    void OnDetached(p::ProtocolContext&, p::DetachReason reason) override {
        ++detached_;
        reason_ = reason;
    }

    const std::vector<char>& last() const { return last_; }
    const std::vector<std::vector<char>>& frames() const { return frames_; }
    int count() const { return count_; }
    const std::vector<std::uint64_t>& ticks() const { return ticks_; }
    int detached() const { return detached_; }
    p::DetachReason reason() const { return reason_; }

private:
    rd::net::EventLoop* loop_{nullptr};
    std::vector<char> last_;
    std::vector<std::vector<char>> frames_;
    std::vector<std::uint64_t> ticks_;
    int count_{0};
    int detached_{0};
    p::DetachReason reason_{p::DetachReason::kError};
};

// ---------------------------------------------------------------------------
// 验收 1：零拷贝反证
// ---------------------------------------------------------------------------
class ScribbleSender : public p::Protocol {
public:
    explicit ScribbleSender(Endpoint back) : back_(std::move(back)) {}

    void OnAttached(p::ProtocolContext&) override {}
    void OnDatagram(p::ProtocolContext& ctx, p::ByteView,
                    const Endpoint&) override {
        std::memset(scratch_.data(), 0x5A, scratch_.size());  // 原始内容
        in_callback_ = true;
        const p::ByteView borrowed{
            reinterpret_cast<const std::uint8_t*>(scratch_.data()),
            scratch_.size()};
        queued_ = ctx.SendTo(borrowed, back_);
        in_callback_ = false;
        // SendTo 返回后立刻覆盖源缓冲：拷贝/同步发送都会让对端看到 0x5A。
        std::memset(scratch_.data(), 0xCC, scratch_.size());
    }
    void OnTimer(p::ProtocolContext&, std::uint64_t) override {}
    void OnSendComplete(p::ProtocolContext&, std::uint64_t token,
                        rd::net::SendResult result) override {
        ++complete_;
        complete_token_ = token;
        complete_status_ = result.status;
        complete_inside_callback_ = in_callback_;
    }
    void OnDetached(p::ProtocolContext&, p::DetachReason) override {}

    std::size_t scratch_size() const { return scratch_.size(); }
    rd::net::SendResult queued() const { return queued_; }
    int complete() const { return complete_; }
    std::uint64_t complete_token() const { return complete_token_; }
    IoStatus complete_status() const { return complete_status_; }
    bool complete_inside_callback() const { return complete_inside_callback_; }

private:
    Endpoint back_;
    std::array<char, 32> scratch_{};
    rd::net::SendResult queued_{};
    int complete_{0};
    std::uint64_t complete_token_{0};
    IoStatus complete_status_{IoStatus::kInvalid};
    bool complete_inside_callback_{true};
    bool in_callback_{false};
};

void TestZeroCopyOverwrite() {
    const auto loop = MakeLoop();
    const auto srv = OpenUdp();
    const auto cli = OpenUdp();
    if (!loop || !srv || !cli) {
        return;
    }
    ScribbleSender sender(cli->local_endpoint());
    Recorder rec;
    auto attach_srv = p::AttachProtocol(*loop, *srv, sender);
    auto attach_cli = p::AttachProtocol(*loop, *cli, rec);
    CHECK_TRUE(attach_srv != nullptr);
    CHECK_TRUE(attach_cli != nullptr);
    if (!attach_srv || !attach_cli) {
        return;
    }

    const char trigger[] = "trigger";
    rd::net::SendResult raw{};
    cli->SendTo(rd::net::ByteView{
                    reinterpret_cast<const std::uint8_t*>(trigger),
                    sizeof(trigger) - 1},
                srv->local_endpoint(), &raw);
    RunFor(*loop, std::chrono::milliseconds(50));

    bool all_cc = true;
    bool all_5a = true;
    for (char c : rec.last()) {
        all_cc = all_cc && static_cast<unsigned char>(c) == 0xCC;
        all_5a = all_5a && static_cast<unsigned char>(c) == 0x5A;
    }
    std::printf(
        "[ZEROCOPY] 源缓冲 %zu B：SendTo 后立即 memset(0xCC)；对端收到 %zu B，"
        "全0xCC=%d 全0x5A=%d\n",
        sender.scratch_size(), rec.last().size(), all_cc ? 1 : 0,
        all_5a ? 1 : 0);
    CHECK_TRUE(rec.count() == 1);
    CHECK_TRUE(rec.last().size() == sender.scratch_size());
    CHECK_TRUE(all_cc);    // 合规：入队不拷贝 + 回调返回后异步发送
    CHECK_TRUE(!all_5a);   // 命中即判 bug（偷偷拷贝或同步发送）
    CHECK_TRUE(sender.queued().status == IoStatus::kOk);
    CHECK_TRUE(sender.queued().token != 0);
    CHECK_TRUE(sender.complete() == 1);
    CHECK_TRUE(sender.complete_token() == sender.queued().token);
    CHECK_TRUE(sender.complete_status() == IoStatus::kOk);
    CHECK_TRUE(!sender.complete_inside_callback());  // 完成回调不在 SendTo 内
}

// ---------------------------------------------------------------------------
// 验收 2：20 行自定义 echo 协议（不碰 Go2Cpp 一行）
// ---------------------------------------------------------------------------
class EchoProtocol : public p::Protocol {
public:
    void OnAttached(p::ProtocolContext&) override {}
    void OnDatagram(p::ProtocolContext& ctx, p::ByteView data,
                    const Endpoint& from) override {
        ctx.SendTo(data, from);
    }
    void OnTimer(p::ProtocolContext&, std::uint64_t) override {}
    void OnSendComplete(p::ProtocolContext&, std::uint64_t,
                        rd::net::SendResult) override {}
    void OnDetached(p::ProtocolContext&, p::DetachReason) override {}
};

void TestEchoProtocol() {
    const auto loop = MakeLoop();
    const auto srv = OpenUdp();
    const auto cli = OpenUdp();
    if (!loop || !srv || !cli) {
        return;
    }
    EchoProtocol echo;
    Recorder rec;
    auto attach_srv = p::AttachProtocol(*loop, *srv, echo);
    auto attach_cli = p::AttachProtocol(*loop, *cli, rec);
    CHECK_TRUE(attach_srv != nullptr);
    CHECK_TRUE(attach_cli != nullptr);
    if (!attach_srv || !attach_cli) {
        return;
    }

    const std::string payload = "ping-echo-req008";
    rd::net::SendResult raw{};
    cli->SendTo(rd::net::ByteView{
                    reinterpret_cast<const std::uint8_t*>(payload.data()),
                    payload.size()},
                srv->local_endpoint(), &raw);
    RunFor(*loop, std::chrono::milliseconds(50));
    const std::string echoed(rec.last().begin(), rec.last().end());
    std::printf("[ECHO] 发送 \"%s\"，回显 \"%s\"（%d 次回调）\n",
                payload.c_str(), echoed.c_str(), rec.count());
    CHECK_TRUE(rec.count() == 1);
    CHECK_TRUE(echoed == payload);
}

// ---------------------------------------------------------------------------
// 验收 3：ACK 位图 32→64 位全部写在测试里，Go2Cpp 不感知
// ---------------------------------------------------------------------------
struct AckMap32 {
    std::uint32_t ack_bits[1]{0};
    void Set(int bit) { ack_bits[0] |= (1u << bit); }
    bool Test(int bit) const { return (ack_bits[0] & (1u << bit)) != 0; }
};

struct AckMap64 {
    std::uint64_t ack_bits[2]{0, 0};
    void Set(int bit) { ack_bits[bit / 64] |= (1ull << (bit % 64)); }
    bool Test(int bit) const {
        return (ack_bits[bit / 64] & (1ull << (bit % 64))) != 0;
    }
};

// 24 字节头 + 4 字节位图 / 24 字节头 + 8 字节位图，均为上层自定义格式。
std::vector<char> Encode32(std::uint32_t bits) {
    std::vector<char> frame(24 + 4, 0);
    std::memcpy(frame.data() + 24, &bits, 4);
    return frame;
}

std::vector<char> Encode64(const std::uint64_t (&words)[2]) {
    std::vector<char> frame(24 + 16, 0);
    std::memcpy(frame.data() + 24, words, 16);
    return frame;
}

void TestAckBitmap32To64() {
    AckMap32 a32;
    a32.Set(0);
    a32.Set(31);
    const std::vector<char> wire32 = Encode32(a32.ack_bits[0]);

    AckMap64 a64;
    a64.Set(0);
    a64.Set(64);
    const std::vector<char> wire64 = Encode64(a64.ack_bits);

    std::uint32_t back32 = 0;
    std::memcpy(&back32, wire32.data() + 24, 4);
    std::uint64_t back64[2] = {0, 0};
    std::memcpy(back64, wire64.data() + 24, 16);
    std::printf("[ACK] 32 位帧=%zu B（bit0=%d bit31=%d），64 位帧=%zu B（bit0=%d bit64=%d）\n",
                wire32.size(), (back32 & 1u) != 0, (back32 & (1u << 31)) != 0,
                wire64.size(), (back64[0] & 1ull) != 0, (back64[1] & 1ull) != 0);
    CHECK_TRUE(wire32.size() == 28);
    CHECK_TRUE(wire64.size() == 40);
    CHECK_TRUE((back32 & 1u) != 0);
    CHECK_TRUE((back32 & (1u << 31)) != 0);
    CHECK_TRUE((back64[0] & 1ull) != 0);
    CHECK_TRUE((back64[1] & 1ull) != 0);

    // 两种位图宽度都走同一个 plugin 入口：对端逐字节回显，格式由测试自己解释。
    const auto loop = MakeLoop();
    const auto srv = OpenUdp();
    const auto cli = OpenUdp();
    if (!loop || !srv || !cli) {
        return;
    }
    EchoProtocol echo;
    Recorder rec;
    auto attach_srv = p::AttachProtocol(*loop, *srv, echo);
    auto attach_cli = p::AttachProtocol(*loop, *cli, rec);
    CHECK_TRUE(attach_srv != nullptr);
    CHECK_TRUE(attach_cli != nullptr);
    if (!attach_srv || !attach_cli) {
        return;
    }
    // 两种宽度各发一帧，同一个 Run 内收齐（EventLoop::Stop 是终态，不能二次 Run）。
    for (const std::vector<char>* frame : {&wire32, &wire64}) {
        rd::net::SendResult raw{};
        cli->SendTo(rd::net::ByteView{
                        reinterpret_cast<const std::uint8_t*>(frame->data()),
                        frame->size()},
                    srv->local_endpoint(), &raw);
    }
    RunFor(*loop, std::chrono::milliseconds(60));
    std::printf("[ACK] 回显帧数=%zu（期望 2）\n", rec.frames().size());
    CHECK_TRUE(rec.frames().size() == 2);
    if (rec.frames().size() == 2) {
        CHECK_TRUE(std::equal(wire32.begin(), wire32.end(),
                              rec.frames()[0].begin(), rec.frames()[0].end()));
        CHECK_TRUE(std::equal(wire64.begin(), wire64.end(),
                              rec.frames()[1].begin(), rec.frames()[1].end()));
    }
    std::printf("[ACK] 28 B / 40 B 两种帧均经 plugin 原样往返，Go2Cpp 未参与解析\n");
}

// ---------------------------------------------------------------------------
// 验收 4：SendTo 入队立刻返回（1 万次），心跳定时器不延迟
// ---------------------------------------------------------------------------
class FloodProtocol : public p::Protocol {
public:
    FloodProtocol(Endpoint sink, int count) : sink_(std::move(sink)), count_(count) {}

    void OnAttached(p::ProtocolContext& ctx) override {
        const auto started = Clock::now();
        for (int i = 0; i < count_; ++i) {
            const rd::net::SendResult result = ctx.SendTo(
                p::ByteView{reinterpret_cast<const std::uint8_t*>(payload_.data()),
                            payload_.size()},
                sink_);
            if (result.status != IoStatus::kOk) {
                ++rejected_;
            }
        }
        enqueue_ns_ = ElapsedNs(started, Clock::now());
        ScheduleHeartbeat(ctx);
    }
    void OnDatagram(p::ProtocolContext&, p::ByteView, const Endpoint&) override {}
    void OnTimer(p::ProtocolContext&, std::uint64_t) override {}
    void OnSendComplete(p::ProtocolContext&, std::uint64_t,
                        rd::net::SendResult) override {
        ++completed_;
    }
    void OnDetached(p::ProtocolContext&, p::DetachReason) override {}

    int64_t enqueue_ns() const { return enqueue_ns_; }
    int count() const { return count_; }
    int rejected() const { return rejected_; }
    int completed() const { return completed_; }
    int64_t max_gap_ms() const { return max_gap_ms_; }

private:
    void ScheduleHeartbeat(p::ProtocolContext& ctx) {
        ctx_ = &ctx;
        ctx.ScheduleAfter(std::chrono::milliseconds(5), [this]() { Heartbeat(); });
    }
    // 回调期间 ctx_ 有效；桥接对象活到 OnDetached，故这里可以保存指针。
    void Heartbeat() {
        const auto now = Clock::now();
        if (last_fire_.time_since_epoch().count() != 0) {
            const int64_t gap_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                      last_fire_)
                    .count();
            if (gap_ms > max_gap_ms_) {
                max_gap_ms_ = gap_ms;
            }
        }
        last_fire_ = now;
        if (ctx_ != nullptr) {
            ctx_->ScheduleAfter(std::chrono::milliseconds(5),
                                [this]() { Heartbeat(); });
        }
    }

    Endpoint sink_;
    int count_{0};
    std::array<char, 32> payload_{};
    p::ProtocolContext* ctx_{nullptr};
    Clock::time_point last_fire_{};
    int64_t enqueue_ns_{0};
    int64_t max_gap_ms_{0};
    int rejected_{0};
    int completed_{0};
};

void TestSendToNonBlocking() {
    const auto loop = MakeLoop();
    const auto sink = OpenUdp();
    const auto me = OpenUdp();
    if (!loop || !sink || !me) {
        return;
    }
    FloodProtocol flood(sink->local_endpoint(), 10000);
    auto attach = p::AttachProtocol(*loop, *me, flood);
    CHECK_TRUE(attach != nullptr);
    if (!attach) {
        return;
    }
    const auto started = Clock::now();
    RunFor(*loop, std::chrono::milliseconds(400));
    const int64_t wall_ns = ElapsedNs(started, Clock::now());
    std::printf(
        "[NONBLOCK] SendTo 1 万次耗时 %.3f ms（阈值 10 ms），完成回调 %d 次，"
        "入队被拒 %d 次，心跳最大间隔 %lld ms（目标 5 ms），loop 总时长 %.1f ms\n",
        static_cast<double>(flood.enqueue_ns()) / 1e6, flood.completed(),
        flood.rejected(), static_cast<long long>(flood.max_gap_ms()),
        static_cast<double>(wall_ns) / 1e6);
    CHECK_TRUE(flood.count() == 10000);
    CHECK_TRUE(flood.rejected() == 0);
    CHECK_TRUE(flood.enqueue_ns() < 10'000'000);  // < 10 ms
    CHECK_TRUE(flood.max_gap_ms() <= 20);         // 心跳不延迟
}

// ---------------------------------------------------------------------------
// 验收 5：单 tick 每 socket ≤64 包
// ---------------------------------------------------------------------------
void TestPerTickCap() {
    const auto loop = MakeLoop();
    const auto srv = OpenUdp();
    const auto cli = OpenUdp();
    if (!loop || !srv || !cli) {
        return;
    }
    Recorder rec(loop.get());
    auto attach = p::AttachProtocol(*loop, *srv, rec);
    CHECK_TRUE(attach != nullptr);
    if (!attach) {
        return;
    }
    const std::array<std::uint8_t, 16> payload{};
    for (int i = 0; i < 100; ++i) {
        rd::net::SendResult raw{};
        cli->SendTo(rd::net::ByteView{payload.data(), payload.size()},
                    srv->local_endpoint(), &raw);
    }
    RunFor(*loop, std::chrono::milliseconds(120));

    std::vector<std::uint64_t> ticks = rec.ticks();
    std::sort(ticks.begin(), ticks.end());
    std::size_t max_per_tick = 0;
    for (std::size_t i = 0; i < ticks.size();) {
        std::size_t j = i;
        while (j < ticks.size() && ticks[j] == ticks[i]) {
            ++j;
        }
        max_per_tick = std::max(max_per_tick, j - i);
        std::printf("[CAP] tick=%llu 收到 %zu 包\n",
                    static_cast<unsigned long long>(ticks[i]), j - i);
        i = j;
    }
    std::size_t tick_groups = 0;
    for (std::size_t i = 0; i < ticks.size();) {
        std::size_t j = i;
        while (j < ticks.size() && ticks[j] == ticks[i]) {
            ++j;
        }
        ++tick_groups;
        i = j;
    }
    std::printf("[CAP] 共 %d 包，跨 %zu 个 tick，单 tick 峰值 %zu（上限 64）\n",
                rec.count(), tick_groups, max_per_tick);
    CHECK_TRUE(rec.count() == 100);
    CHECK_TRUE(max_per_tick <= 64);
    CHECK_TRUE(max_per_tick == 64);
}

// ---------------------------------------------------------------------------
// 定时器 id 回传 + detach 原因 + 解绑后不再回调
// ---------------------------------------------------------------------------
class TimerProbe : public p::Protocol {
public:
    void OnAttached(p::ProtocolContext& ctx) override {
        timer_id_ = ctx.ScheduleAfter(std::chrono::milliseconds(2),
                                      [this]() { fired_ = true; });
    }
    void OnDatagram(p::ProtocolContext&, p::ByteView, const Endpoint&) override {}
    void OnTimer(p::ProtocolContext&, std::uint64_t timer_id) override {
        if (timer_id == timer_id_) {
            timer_id_matched_ = true;
        }
    }
    void OnSendComplete(p::ProtocolContext&, std::uint64_t,
                        rd::net::SendResult) override {}
    void OnDetached(p::ProtocolContext&, p::DetachReason reason) override {
        ++detached_;
        reason_ = reason;
    }

    std::uint64_t timer_id() const { return timer_id_; }
    bool fired() const { return fired_; }
    bool timer_id_matched() const { return timer_id_matched_; }
    int detached() const { return detached_; }
    p::DetachReason reason() const { return reason_; }

private:
    std::uint64_t timer_id_{0};
    bool fired_{false};
    bool timer_id_matched_{false};
    int detached_{0};
    p::DetachReason reason_{p::DetachReason::kError};
};

void TestTimerAndDetach() {
    const auto loop = MakeLoop();
    const auto sock = OpenUdp();
    if (!loop || !sock) {
        return;
    }
    TimerProbe probe;
    auto attach = p::AttachProtocol(*loop, *sock, probe);
    CHECK_TRUE(attach != nullptr);
    if (!attach) {
        return;
    }
    RunFor(*loop, std::chrono::milliseconds(20));
    std::printf("[TIMER] id=%llu 触发=%d id匹配=%d\n",
                static_cast<unsigned long long>(probe.timer_id()),
                probe.fired() ? 1 : 0, probe.timer_id_matched() ? 1 : 0);
    CHECK_TRUE(probe.timer_id() != 0);
    CHECK_TRUE(probe.fired());
    CHECK_TRUE(probe.timer_id_matched());

    attach.reset();  // 触发 detach
    std::printf("[DETACH] OnDetached 次数=%d 原因=%s\n", probe.detached(),
                p::DetachReasonName(probe.reason()));
    CHECK_TRUE(probe.detached() == 1);
    CHECK_TRUE(probe.reason() == p::DetachReason::kClosed);

    // 重复解绑不应再回调。
    CHECK_TRUE(!loop->Detach(*sock));
    CHECK_TRUE(probe.detached() == 1);
}

}  // namespace

int main() {
    TestZeroCopyOverwrite();
    TestEchoProtocol();
    TestAckBitmap32To64();
    TestSendToNonBlocking();
    TestPerTickCap();
    TestTimerAndDetach();
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
