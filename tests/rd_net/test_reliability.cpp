// rd::net 可靠传输容器自测：5 个场景 + 1e6 次压测，不依赖任何测试框架。
#include "go2cpp/rd_net/reliability.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                  do {                                                                                 ++g_checks;                                                                      if (!(cond)) {                                                                       ++g_failures;                                                                    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                  }                                                                            } while (false)

using ms = std::chrono::milliseconds;

// 场景 1：SeqNum<uint32_t> 的 0xFFFFFFFF -> 0 回绕。
void scenario1_seq_wrap() {
    std::printf("[场景1] SeqNum<uint32_t> 回绕\n");
    using Seq32 = rd::net::SeqNum<std::uint32_t>;
    using Seq16 = rd::net::SeqNum<std::uint16_t>;

    CHECK(Seq32(0xFFFFFFFFu).value() == 0xFFFFFFFFu);
    CHECK(Seq32(0xFFFFFFFFu).Next().value() == 0u);
    CHECK(Seq32(0u).Next().value() == 1u);

    // 跨回绕：0xFFFFFFFE 在 1 之前（模差 3），反向不成立。
    CHECK(Seq32(0xFFFFFFFEu).LessThan(Seq32(1u)));
    CHECK(!Seq32(1u).LessThan(Seq32(0xFFFFFFFEu)));
    CHECK(Seq32(0xFFFFFFFFu).LessThan(Seq32(0u)));
    CHECK(!Seq32(0u).LessThan(Seq32(0xFFFFFFFFu)));

    // 自反与半程边界（模差恰为 2^(N-1) 时判否）。
    CHECK(!Seq32(7u).LessThan(Seq32(7u)));
    CHECK(!Seq32(0u).LessThan(Seq32(0x80000000u)));
    CHECK(!Seq32(0x80000000u).LessThan(Seq32(0u)));
    CHECK(Seq32(0u).LessThan(Seq32(0x7FFFFFFFu)));

    // 从 0xFFFFFFF0 连续递增 40 次，每次都必须满足 LessThan。
    Seq32 cur(0xFFFFFFF0u);
    bool mono = true;
    for (int i = 0; i < 40; ++i) {
        const Seq32 next = cur.Next();
        mono = mono && cur.LessThan(next);
        cur = next;
    }
    CHECK(mono);

    // Distance 走有符号模差。
    CHECK(Seq32(5u).Distance(Seq32(3u)) == -2);
    CHECK(Seq32(3u).Distance(Seq32(5u)) == 2);
    CHECK(Seq32(0xFFFFFFFFu).Distance(Seq32(0u)) == 1);
    CHECK(Seq32(0u).Distance(Seq32(0xFFFFFFFFu)) == -1);

    // 模板对 16 位序号同样成立。
    CHECK(Seq16(0xFFFFu).Next().value() == 0u);
    CHECK(Seq16(0xFFFEu).LessThan(Seq16(1u)));
    CHECK(!Seq16(0x8000u).LessThan(Seq16(0u)));
}

// 场景 2：1e5 项批量入队，PopDue 一次取走全部到期项且不重复。
void scenario2_queue_bulk() {
    std::printf("[场景2] RetransmitQueue 1e5 项\n");
    rd::net::RetransmitQueue q;
    const auto base = rd::net::RetransmitQueue::Clock::now();
    const std::size_t total = 100000;
    const std::vector<std::uint8_t> payload(32, 0xAB);
    std::size_t future = 0;

    for (std::size_t i = 0; i < total; ++i) {
        const bool is_future = (i % 3 == 0);
        const auto deadline = is_future ? base + std::chrono::hours(1)
                                        : base - std::chrono::milliseconds(1);
        if (is_future) {
            ++future;
        }
        q.Push(static_cast<std::uint64_t>(i) + 1, payload, deadline);
    }
    CHECK(q.size() == total);

    const auto due = q.PopDue(base);
    CHECK(due.size() == total - future);

    std::unordered_set<std::uint64_t> seen;
    bool unique = true;
    bool payload_ok = true;
    for (const auto& e : due) {
        unique = seen.insert(e.token).second && unique;
        payload_ok = payload_ok && (e.data.size() == 32) && (e.data[0] == 0xAB) && (e.deadline <= base);
    }
    CHECK(unique);
    CHECK(seen.size() == due.size());
    CHECK(payload_ok);
    CHECK(q.size() == future);
    CHECK(q.PopDue(base).empty());

    // Ack 幂等递减：第一次返回 true 且 size 减 1，第二次 no-op。
    const std::uint64_t alive = 1;
    CHECK(q.Find(alive) != nullptr);
    const std::size_t before = q.size();
    CHECK(q.Ack(alive));
    CHECK(q.size() == before - 1);
    CHECK(q.Find(alive) == nullptr);
    CHECK(!q.Ack(alive));
    CHECK(q.size() == before - 1);

    CHECK(!q.Remove(999999999u));
    CHECK(q.Find(999999999u) == nullptr);
}

// 场景 3：RFC 6298 公式与手算期望值一致，并测 max_rto / initial_rto 夹紧。
void scenario3_rtt() {
    std::printf("[场景3] RttEstimator RFC 6298\n");
    const rd::net::RttParams defaults;
    CHECK(defaults.initial_rto == ms(1000));
    CHECK(defaults.max_rto == ms(60000));

    rd::net::RttParams p;
    p.initial_rto = ms(100);
    p.max_rto = ms(60000);
    rd::net::RttEstimator est(p);
    CHECK(est.has_sample() == false);
    CHECK(est.rto() == ms(100));

    est.OnSample(ms(100));
    CHECK(est.srtt() == ms(100));
    CHECK(est.rttvar() == ms(50));
    CHECK(est.rto() == ms(300));

    // 第二个样本 200ms：rttvar=(3*50+100)/4=62，srtt=(7*100+200)/8=112，RTO=112+248=360。
    est.OnSample(ms(200));
    CHECK(est.rttvar() == ms(62));
    CHECK(est.srtt() == ms(112));
    CHECK(est.rto() == ms(360));

    // 第三个样本 300ms：rttvar=(3*62+188)/4=93，srtt=(7*112+300)/8=135，RTO=135+372=507。
    est.OnSample(ms(300));
    CHECK(est.rttvar() == ms(93));
    CHECK(est.srtt() == ms(135));
    CHECK(est.rto() == ms(507));

    // max_rto 夹紧：单样本 60s 展开为 180s，被夹到 2s。
    rd::net::RttParams p2;
    p2.initial_rto = ms(1000);
    p2.max_rto = ms(2000);
    rd::net::RttEstimator e2(p2);
    e2.OnSample(ms(60000));
    CHECK(e2.srtt() == ms(60000));
    CHECK(e2.rttvar() == ms(30000));
    CHECK(e2.rto() == ms(2000));

    // initial_rto 夹紧：10ms 样本算出 30ms，被抬到 1s。
    rd::net::RttEstimator e3;
    e3.OnSample(ms(10));
    CHECK(e3.srtt() == ms(10));
    CHECK(e3.rttvar() == ms(5));
    CHECK(e3.rto() == ms(1000));
}

// 场景 4：上层提供 AIMD 实现，容器代码零改动。
class AimdController : public rd::net::CongestionController {
public:
    void OnAck(std::size_t bytes) override {
        (void)bytes;
        if (window_ < kMaxWindow) {
            ++window_;
        }
    }
    void OnLoss() override { window_ = std::max<std::size_t>(1, window_ / 2); }
    std::size_t Window() const override { return window_; }

private:
    static constexpr std::size_t kMaxWindow = 1024;
    std::size_t window_ = 1;
};

std::size_t drive_acks(rd::net::CongestionController& cc, std::size_t times) {
    for (std::size_t i = 0; i < times; ++i) {
        cc.OnAck(1200);
    }
    return cc.Window();
}

void scenario4_congestion_plugin() {
    std::printf("[场景4] CongestionController 换成 AIMD\n");
    AimdController aimd;
    rd::net::CongestionController& base_ref = aimd;
    CHECK(base_ref.Window() == 1);
    CHECK(drive_acks(base_ref, 10) == 11);
    base_ref.OnLoss();
    CHECK(aimd.Window() == 5);
    base_ref.OnLoss();
    CHECK(aimd.Window() == 2);
    CHECK(drive_acks(base_ref, 3) == 5);

    // 换成另一个实现（固定窗口 + 记录调用）验证接口可插拔。
    class FixedController : public rd::net::CongestionController {
    public:
        void OnAck(std::size_t bytes) override { last_bytes_ = bytes; }
        void OnLoss() override { loss_calls_ += 1; }
        std::size_t Window() const override { return 42; }
        std::size_t last_bytes() const { return last_bytes_; }
        int loss_calls() const { return loss_calls_; }

    private:
        std::size_t last_bytes_ = 0;
        int loss_calls_ = 0;
    };
    FixedController fixed;
    rd::net::CongestionController& other = fixed;
    CHECK(other.Window() == 42);
    other.OnAck(777);
    other.OnLoss();
    CHECK(fixed.last_bytes() == 777);
    CHECK(fixed.loss_calls() == 1);
}

// 场景 5：10 万次随机操作与 std::map<token, deadline> 参考模型对拍。
void scenario5_random_model() {
    std::printf("[场景5] 随机对拍 1e5 次\n");
    using TimePoint = rd::net::RetransmitQueue::TimePoint;
    rd::net::RetransmitQueue q;
    std::map<std::uint64_t, TimePoint> model;
    const auto base = rd::net::RetransmitQueue::Clock::now();
    std::mt19937_64 rng(20261103u);
    constexpr int kOps = 100000;
    int mismatches = 0;
    int push_ops = 0;
    int ack_ops = 0;
    int pop_ops = 0;

    for (int op = 0; op < kOps; ++op) {
        const std::uint64_t kind = rng() % 100u;
        const std::uint64_t token = rng() % 512u;
        const TimePoint now = base + std::chrono::microseconds(static_cast<std::int64_t>(rng() % 2000u));

        if (kind < 55u) {
            ++push_ops;
            const TimePoint deadline =
                now + std::chrono::microseconds(static_cast<std::int64_t>(rng() % 5000u) - 2500);
            q.Push(token, std::vector<std::uint8_t>(8, static_cast<std::uint8_t>(token & 0xFFu)), deadline);
            model[token] = deadline;
        } else if (kind < 80u) {
            ++ack_ops;
            const bool got = q.Ack(token);
            const bool want = model.erase(token) != 0;
            if (got != want) {
                ++mismatches;
            }
        } else {
            ++pop_ops;
            const auto due = q.PopDue(now);
            std::map<std::uint64_t, TimePoint> got_model;
            for (const auto& e : due) {
                if (!got_model.emplace(e.token, e.deadline).second) {
                    ++mismatches;
                }
            }
            std::map<std::uint64_t, TimePoint> want_model;
            for (auto it = model.begin(); it != model.end();) {
                if (it->second <= now) {
                    want_model.emplace(it->first, it->second);
                    it = model.erase(it);
                } else {
                    ++it;
                }
            }
            if (got_model != want_model) {
                ++mismatches;
            }
        }
        if (q.size() != model.size()) {
            ++mismatches;
        }
    }
    std::printf("  操作分布: push=%d ack=%d pop=%d, 最终 size=%zu, 不匹配=%d\n",
                push_ops, ack_ops, pop_ops, q.size(), mismatches);
    CHECK(mismatches == 0);
    CHECK(static_cast<std::size_t>(push_ops + ack_ops + pop_ops) == kOps);
}

std::string proc_status_field(const char* key) {
    std::ifstream in("/proc/self/status");
    const std::string prefix(key);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(prefix, 0) == 0) {
            return line;
        }
    }
    return std::string("(未找到)");
}

// 压测：1e6 次 Push + PopDue，报 ns/op 与 RSS。
void benchmark_push_pop() {
    constexpr std::size_t kOps = 1000000;
    rd::net::RetransmitQueue q;
    const auto base = rd::net::RetransmitQueue::Clock::now();
    const std::vector<std::uint8_t> payload(64, 0x5A);
    std::size_t popped = 0;

    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kOps; ++i) {
        q.Push(static_cast<std::uint64_t>(i) + 1, payload, base);
        popped += q.PopDue(base).size();
    }
    const auto t1 = std::chrono::steady_clock::now();

    const double ns_total = std::chrono::duration<double, std::nano>(t1 - t0).count();
    std::printf("[压测] 1e6 次 Push+PopDue: 总 %.3f ms, %.1f ns/op, 弹出 %zu 项, 结束时 size=%zu\n",
                ns_total / 1e6, ns_total / static_cast<double>(kOps), popped, q.size());
    std::printf("[压测] %s\n", proc_status_field("VmRSS:").c_str());
    std::printf("[压测] %s\n", proc_status_field("VmHWM:").c_str());
    CHECK(popped == kOps);
}

}  // namespace

int main() {
    std::printf("== rd::net reliability 容器自测 ==\n");
    scenario1_seq_wrap();
    scenario2_queue_bulk();
    scenario3_rtt();
    scenario4_congestion_plugin();
    scenario5_random_model();
    benchmark_push_pop();
    std::printf("== 检查 %d 项，失败 %d 项 ==\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
