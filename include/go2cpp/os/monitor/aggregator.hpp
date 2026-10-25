#pragma once

// 聚合器：每个采集器一个 Fiber，采集结果经 Channel 汇入聚合 Fiber，
// 由聚合 Fiber 合并成 Snapshot；定时由定时器（every）或 Fiber 内 Sleep 驱动。
//
// 线程模型：Start() 惰性创建独立的 Scheduler（默认 GOMAXPROCS 个 M），
// 不干扰调用方已有的默认调度器；Stop() 关闭 Channel 并等待 Fiber 退出。
//
// 解耦：聚合器实现 SnapshotSource，并可按需持有一个 SnapshotSink。设置 sink
// 后，每个采集周期把「新」快照推给它（Http/Memory/File 或测试替身均可），
// 聚合器本身不再关心推送协议与存储。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "go2cpp/os/monitor/interfaces.hpp"
#include "go2cpp/os/monitor/monitor.hpp"

namespace go2cpp::os::monitor {

/** @brief 聚合器配置。 */
struct AggregatorConfig {
    /// 相邻两次采样的间隔。
    std::chrono::milliseconds interval{1000};
    /// 各采集器开关；eBPF 需要 root，默认开启但会自行降级。
    bool enable_cpu{true};
    bool enable_mem{true};
    bool enable_disk{true};
    bool enable_net{true};
    bool enable_ebpf{true};
    /// 三个内核 tracepoint 监控器；需要 root，非 root 自动降级为 kUnsupported。
    bool enable_syscall{true};
    bool enable_sched{true};
    bool enable_futex{true};
    /// TCP 重传/RTT 与文件 IO 延迟；需要 root，非 root 自动降级。
    bool enable_tcp{true};
    bool enable_fileio{true};
    /// Scheduler 的工作线程数；0 表示使用硬件并发度。
    int workers{0};
};

/** @brief 采集器种类，用于 Channel 消息标注来源。 */
enum class CollectorKind : std::uint8_t {
    kCpu = 0,
    kMem = 1,
    kDisk = 2,
    kNet = 3,
    kEbpf = 4,
    kSyscall = 5,
    kSched = 6,
    kFutex = 7,
    kTcp = 8,
    kFileIo = 9,
};

/** @brief 聚合器运行统计。 */
struct AggregatorStats {
    /// 已合并的采集结果数（每个采集器每次成功 UpdateOnce +1，不是
    /// 「完整快照轮数」；十个采集器都成功时一轮约 +10）。
    std::uint64_t snapshots{0};
    std::uint64_t errors{0};      ///< 采集错误次数
    std::uint64_t ebpf_samples{0};
    bool ebpf_active{false};
    std::string ebpf_reason;
};

/**
 * @brief 多采集器聚合器。
 *
 * 用法：
 * @code
 * Aggregator agg(AggregatorConfig{.interval = std::chrono::milliseconds(500)});
 * agg.Start();
 * Snapshot snap = agg.Latest();
 * agg.Stop();
 * @endcode
 *
 * CollectNow() 提供同步单次采集，供测试与需要确定性结果的调用方使用。
 * 传入 sink（或运行中 SetSink）后，每个周期把新快照发布给 sink。
 *
 * 生命周期：Stop() 会关闭五个依赖 eBPF 的内核采集器（syscall/sched/
 * futex/tcp/fileio）；再次 Start() 会重建它们并重新 attach，不会停在
 * reason="stopped" 的降级状态。四个 /proc 采集器的差分基线从零开始
 * （重启后的第一帧速率/占比为 0，符合「无前值」语义）。
 */
class Aggregator : public SnapshotSource {
public:
    explicit Aggregator(AggregatorConfig config = {},
                        std::shared_ptr<SnapshotSink> sink = nullptr);
    ~Aggregator() override;

    Aggregator(const Aggregator&) = delete;
    Aggregator& operator=(const Aggregator&) = delete;

    /** @brief 启动采集 Fiber。重复调用返回 false。 */
    bool Start();

    /** @brief 停止并等待所有 Fiber 退出；可重复调用。 */
    void Stop();

    /** @brief 是否正在运行。 */
    bool running() const noexcept;

    /**
     * @brief 同步采集一次（不经过 Channel/Fiber），返回完整快照。
     *
     * 线程安全：与后台定时器按采集器分别串行化，同一采集器不会被并发
     * 调用；与 Start/Stop 也由生命周期锁串行化。
     */
    Snapshot CollectNow();

    /** @brief 最近一次由后台 Fiber 聚合出的快照（无则返回空快照）。 */
    Snapshot Latest() const override;

    /** @brief 最近一次聚合快照的时间戳（毫秒）；0 表示尚无。 */
    std::int64_t LatestTimestampMs() const;

    /** @brief 运行统计。 */
    AggregatorStats Stats() const;

    /** @brief 当前配置。 */
    const AggregatorConfig& config() const noexcept { return config_; }

    /** @brief eBPF 采集器是否可用。 */
    bool EbpfAvailable() const;

    /** @brief eBPF 不可用原因。 */
    std::string EbpfReason() const;

    /**
     * @brief 注入/替换快照接收端；传 nullptr 关闭周期发布。
     *
     * 线程安全：已启动的聚合器可在运行中替换，下一个采集周期生效。
     */
    void SetSink(std::shared_ptr<SnapshotSink> sink);

    /** @brief 当前 sink（可能为空）。 */
    std::shared_ptr<SnapshotSink> Sink() const;

    /**
     * @brief 把「最新且尚未发布过」的快照推给 sink。
     *
     * 只有 Push 成功才把该 timestamp 记为已发布；失败时下一个发布周期
     * 会重试同一快照（不会因为一次瞬断永久丢掉这一帧）。
     *
     * @return 发生了推送返回 sink->Push 的结果；无 sink 或无新快照返回 false。
     */
    bool PublishLatest();

    /**
     * @brief 同步采集一次并直接推给 sink（不更新 latest）。
     *
     * 适合未 Start 的确定性测试。已启动时也可安全调用（内部与后台
     * 采集串行化），但它与后台发布器各自 Push，生产路径优先用
     * PublishLatest() 做去重发布。
     */
    bool PublishNow();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AggregatorConfig config_;
};

}  // namespace go2cpp::os::monitor
