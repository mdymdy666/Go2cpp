#pragma once

// TcpMonitor —— 基于 eBPF tracepoint 的 TCP 重传与 RTT 统计。
//
// 内核侧（go2cpp/bpf/tcp_monitor.bpf.c）挂两个 tracepoint：
//   tcp:tcp_retransmit_skb  按四元组 + 全局累计重传次数
//   tcp:tcp_probe           每次收到 ACK 采样 srtt（内核已平滑的 RTT）
// 用户态每次 UpdateOnce 读取两张 map，计算全局与每连接的重传数、RTT
// p50/p99/avg/max，并按「重传次数优先、RTT 样本数次之」取 top-N 连接。
//
// RTT 采样率：内核按 1/8 采样（GO2CPP_TCP_RTT_SAMPLE_MASK），用户态看到的
// rtt_samples 是采样后的计数，不是 ACK 总数。分位数在采样样本上计算，偏差
// 在一秒以上的窗口内可忽略。
//
// RTT 口径（P2 修复）：tcp_probe 的 srtt 是内核平滑值，每个 ACK 触发一次、
// 再按 1/8 采样。因此 rtt_hist 是「1/8 ACK 样本分布」，不是严格的报文/字节
// 加权；长连接 ACK 多、在直方图里权重自然更大。为了让「典型连接」的 RTT
// 不被高 ACK 连接淹没，内核额外维护 conn_rtt_hist：**每个连接只在第一次被
// 采样时写入一个样本**，计数单位是连接数（conn_rtt_samples）。两套直方图
// 的字段分别对应 rtt_* 与 conn_rtt_*，不能相加或混用。
//
// 降级分两级：
//   - 非 root / 构建期无 libbpf / 内核缺 tcp_retransmit_skb：available()==false，
//     UpdateOnce 返回 kUnsupported 并写 snapshot.tcp.reason；
//   - 内核有 tcp_retransmit_skb 但没有 tcp_probe（部分裁剪内核没有编入
//     该 tracepoint；上游 5.15 移除的是同名的 tcp_probe 模块，tracepoint
//     本身在 6.6 仍存在）：重传统计继续工作，rtt_source 写成
//     "tcp_probe unavailable: ..." 作为显式降级标记，RTT 字段保持 0。
//
// 开销：默认 1Hz 采样下，每次 UpdateOnce 读 1 条全局记录 + 最多 4096 条连接
// 记录（每条 288 字节），用户态耗时约 0.2–0.5ms；内核侧每条重传 1 次原子加，
// 每条采样 ACK 3–4 次原子加。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "go2cpp/os/monitor/monitor.hpp"

namespace go2cpp::os::monitor {

class TcpMonitor final : public Monitor {
public:
    /** @brief @param top_n 输出到 snapshot.tcp.top 的连接数。 */
    explicit TcpMonitor(std::size_t top_n = 10);
    ~TcpMonitor() override;

    /** @brief 读取内核 map 并填充 snapshot.tcp。 */
    Status UpdateOnce(Snapshot& snapshot) override;

    /** @brief 分离 tracepoint 并关闭 BPF 对象；可重复调用。 */
    void Stop() override;

    bool available() const override;
    std::string reason() const override;

    /** @brief 成功读数次数（诊断用）。 */
    std::uint64_t sample_count() const noexcept;

    /** @brief 已 attach 程序的累计内核执行时间/次数（开销验证用）。 */
    void ProgramCost(std::uint64_t* run_time_ns, std::uint64_t* run_cnt) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace go2cpp::os::monitor
