// rd::net 可靠传输通用骨架：回绕安全序号、RFC 6298 RTT 估算、deadline 重传队列、拥塞控制纯虚接口。
// 契约：本文件只提供容器与公式，不假设任何协议语义（序号含义、重传次数、RTO 策略、拥塞算法均由上层决定）。
#ifndef RD_NET_RELIABILITY_HPP
#define RD_NET_RELIABILITY_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rd {
namespace net {

// 无符号回绕安全序号。
// 判据：令 forward = other - this（模 2^N），当且仅当 0 < forward < 2^(N-1) 时 this 在 other 之前。
// 序号的具体含义（字节偏移、报文号、从几开始、是否复用）完全由上层定义，本类只做比较与递增。
template <typename Seq>
class SeqNum {
public:
    static_assert(std::is_unsigned<Seq>::value, "SeqNum 的 Seq 必须是无符号整数类型");

    using value_type = Seq;
    using signed_type = typename std::make_signed<Seq>::type;

    explicit SeqNum(Seq v = 0) : value_(v) {}

    Seq value() const { return value_; }

    bool LessThan(const SeqNum& other) const {
        const Seq forward = static_cast<Seq>(other.value_ - value_);
        return forward != 0 && forward < half_range();
    }

    SeqNum Next() const { return SeqNum(static_cast<Seq>(value_ + 1)); }

    // this 到 other 的有符号距离；跨回绕时返回小整数，而不是巨大的无符号值。
    signed_type Distance(const SeqNum& other) const {
        return static_cast<signed_type>(static_cast<Seq>(other.value_ - value_));
    }

private:
    static Seq half_range() {
        return static_cast<Seq>(Seq(1) << (std::numeric_limits<Seq>::digits - 1));
    }

    Seq value_;
};

// RFC 6298 第 2 节的默认参数，不是任何具体协议的常量；上层可整体覆盖。
struct RttParams {
    std::chrono::milliseconds initial_rto{1000};  // RFC 6298 2.1：初始 RTO 为 1 秒
    std::chrono::milliseconds max_rto{60000};     // RFC 6298 2.4：上限不低于 60 秒
};

// RFC 6298 第 2.2 节的公式实现：首个样本 srtt=R、rttvar=R/2；之后
// rttvar = 3/4*rttvar + 1/4*|srtt-R|，srtt = 7/8*srtt + 1/8*R，RTO = srtt + 4*rttvar。
// 结果夹在 [initial_rto, max_rto]。毫秒整数运算，除法向零截断。
class RttEstimator {
public:
    explicit RttEstimator(const RttParams& params = RttParams{})
        : params_(params), srtt_(0), rttvar_(0), rto_(params.initial_rto) {}

    void OnSample(std::chrono::milliseconds rtt) {
        if (!has_sample_) {
            srtt_ = rtt;
            rttvar_ = rtt / 2;
            has_sample_ = true;
        } else {
            const std::chrono::milliseconds delta = srtt_ > rtt ? srtt_ - rtt : rtt - srtt_;
            rttvar_ = (3 * rttvar_ + delta) / 4;
            srtt_ = (7 * srtt_ + rtt) / 8;
        }
        std::chrono::milliseconds rto = srtt_ + 4 * rttvar_;
        if (rto < params_.initial_rto) rto = params_.initial_rto;
        if (rto > params_.max_rto) rto = params_.max_rto;
        rto_ = rto;
    }

    std::chrono::milliseconds srtt() const { return srtt_; }
    std::chrono::milliseconds rttvar() const { return rttvar_; }
    std::chrono::milliseconds rto() const { return rto_; }
    bool has_sample() const { return has_sample_; }
    const RttParams& params() const { return params_; }

private:
    RttParams params_;
    std::chrono::milliseconds srtt_;
    std::chrono::milliseconds rttvar_;
    std::chrono::milliseconds rto_;
    bool has_sample_ = false;
};

// 按 deadline 有序的重传队列：只负责存，不决定重传次数、不计算 RTO。
// 同 token 再次 Push 视为替换；PopDue 返回即移除，保证同一项不会被取出两次。
// data 由队列以 std::vector<std::uint8_t> 持有（容器语义，不承诺零拷贝）。
class RetransmitQueue {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct Entry {
        std::uint64_t token = 0;
        TimePoint deadline{};
        std::vector<std::uint8_t> data;
    };

    void Push(std::uint64_t token, const std::vector<std::uint8_t>& data, TimePoint deadline) {
        Push(token, std::vector<std::uint8_t>(data), deadline);
    }

    void Push(std::uint64_t token, std::vector<std::uint8_t>&& data, TimePoint deadline) {
        auto it = index_.find(token);
        if (it != index_.end()) {
            by_deadline_.erase(it->second);
            index_.erase(it);
        }
        auto pos = by_deadline_.emplace(deadline, Entry{token, deadline, std::move(data)});
        index_.emplace(token, pos);
    }

    // 返回所有 deadline <= now 的项，并全部移除；下一次调用不会再返回它们。
    std::vector<Entry> PopDue(TimePoint now) {
        std::vector<Entry> due;
        auto it = by_deadline_.begin();
        while (it != by_deadline_.end() && it->first <= now) {
            index_.erase(it->second.token);
            due.push_back(std::move(it->second));
            it = by_deadline_.erase(it);
        }
        return due;
    }

    // 已确认：移除该项。返回 false 表示 token 不在队列中（幂等 no-op）。
    bool Ack(std::uint64_t token) { return Remove(token); }

    bool Remove(std::uint64_t token) {
        auto it = index_.find(token);
        if (it == index_.end()) {
            return false;
        }
        by_deadline_.erase(it->second);
        index_.erase(it);
        return true;
    }

    const Entry* Find(std::uint64_t token) const {
        auto it = index_.find(token);
        if (it == index_.end()) {
            return nullptr;
        }
        return &it->second->second;
    }

    std::size_t size() const { return index_.size(); }
    bool empty() const { return index_.empty(); }

private:
    std::multimap<TimePoint, Entry> by_deadline_;
    std::unordered_map<std::uint64_t, std::multimap<TimePoint, Entry>::iterator> index_;
};

// 拥塞控制纯虚接口：无默认实现，算法（AIMD、CUBIC 或其它）由上层提供。
class CongestionController {
public:
    virtual ~CongestionController() = default;
    virtual void OnAck(std::size_t bytes) = 0;
    virtual void OnLoss() = 0;
    virtual std::size_t Window() const = 0;
};

}  // namespace net
}  // namespace rd

#endif  // RD_NET_RELIABILITY_HPP
