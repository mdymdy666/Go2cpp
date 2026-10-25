#pragma once

// go2cpp::os::monitor —— 主机监控模块的公共类型与采集器接口。
//
// 设计对齐 Go 的「接口 + 实现」：`Monitor` 只声明一次采集（UpdateOnce）与
// 停止（Stop），每个采集器只负责填充 Snapshot 中属于自己的字段。聚合器
// （Aggregator）把多个采集器的结果合并成完整快照，Pusher 负责 HTTP 推送。
//
// 与参考实现（monitor_system/worker）的差异：不依赖 protobuf / gRPC / 内核
// 模块，全部改由 /proc + eBPF（TC hook）读取，数据传输用 JSON over HTTP。

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "go2cpp/tools/json.hpp"

namespace go2cpp::os::monitor {

/** @brief 采集器状态。Unsupported 表示当前环境缺权限或内核能力，不视为错误。 */
enum class Status : std::uint8_t {
    kOk = 0,
    kUnsupported = 1,
    kError = 2,
    kStopped = 3,
};

/** @brief 状态转字符串，便于日志与 JSON 输出。 */
const char* StatusName(Status status) noexcept;

/** @brief 单核 CPU 原始累计时间（jiffies）与由相邻两次采样算出的占比。 */
struct CpuCore {
    std::string name;  ///< "cpu0"、"cpu1"…；汇总行是 "cpu"。
    std::uint64_t user{0};
    std::uint64_t nice{0};
    std::uint64_t system{0};
    std::uint64_t idle{0};
    std::uint64_t iowait{0};
    std::uint64_t irq{0};
    std::uint64_t softirq{0};
    std::uint64_t steal{0};
    std::uint64_t guest{0};
    std::uint64_t guest_nice{0};

    // 相邻两次采样的差分占比（0-100）。首次采样时全部为 0。
    double usage_percent{0.0};
    double user_percent{0.0};
    double system_percent{0.0};
    double nice_percent{0.0};
    double idle_percent{0.0};
    double iowait_percent{0.0};
    double irq_percent{0.0};
    double softirq_percent{0.0};
};

/** @brief /proc/loadavg 的负载与可运行任务数。 */
struct CpuLoad {
    double load1{0.0};
    double load5{0.0};
    double load15{0.0};
    std::uint64_t runnable{0};
    std::uint64_t total_threads{0};
};

/** @brief /proc/meminfo 的关键字段（单位 kB）。 */
struct MemInfo {
    std::uint64_t total_kb{0};
    std::uint64_t free_kb{0};
    std::uint64_t available_kb{0};
    std::uint64_t buffers_kb{0};
    std::uint64_t cached_kb{0};
    std::uint64_t swap_total_kb{0};
    std::uint64_t swap_free_kb{0};
    std::uint64_t active_kb{0};
    std::uint64_t inactive_kb{0};
    std::uint64_t dirty_kb{0};
    std::uint64_t writeback_kb{0};
    std::uint64_t anon_pages_kb{0};
    std::uint64_t mapped_kb{0};
    std::uint64_t sreclaimable_kb{0};
    std::uint64_t sunreclaim_kb{0};
    /// (total - available) / total * 100；total 为 0 时为 0。
    double used_percent{0.0};
};

/** @brief 单块磁盘的 /proc/diskstats 累计值 + 差分速率。 */
struct DiskStat {
    std::string name;
    std::uint64_t reads{0};
    std::uint64_t writes{0};
    std::uint64_t sectors_read{0};
    std::uint64_t sectors_written{0};
    std::uint64_t read_time_ms{0};
    std::uint64_t write_time_ms{0};
    std::uint64_t io_in_progress{0};
    std::uint64_t io_time_ms{0};
    std::uint64_t weighted_io_time_ms{0};

    double read_bytes_per_sec{0.0};
    double write_bytes_per_sec{0.0};
    double read_iops{0.0};
    double write_iops{0.0};
    double avg_read_latency_ms{0.0};
    double avg_write_latency_ms{0.0};
    /// 设备繁忙时间占比（0-100）。
    double util_percent{0.0};
};

/** @brief 单网卡的 /proc/net/dev 累计值 + 差分速率。 */
struct NetDevStat {
    std::string name;
    std::uint64_t rcv_bytes{0};
    std::uint64_t rcv_packets{0};
    std::uint64_t rcv_errs{0};
    std::uint64_t rcv_drops{0};
    std::uint64_t snd_bytes{0};
    std::uint64_t snd_packets{0};
    std::uint64_t snd_errs{0};
    std::uint64_t snd_drops{0};

    double rcv_rate{0.0};          ///< 字节/秒
    double snd_rate{0.0};          ///< 字节/秒
    double rcv_packets_rate{0.0};  ///< 包/秒
    double snd_packets_rate{0.0};  ///< 包/秒
};

/** @brief eBPF TC hook 统计的单网卡累计值（内核 map 读出）。 */
struct NetEbpfStat {
    std::string name;
    std::uint64_t rcv_bytes{0};
    std::uint64_t rcv_packets{0};
    std::uint64_t snd_bytes{0};
    std::uint64_t snd_packets{0};
};

// ---------------------------------------------------------------------------
// 内核 tracepoint 监控（syscall / sched / futex）
// ---------------------------------------------------------------------------

/** @brief 内核延迟直方图的桶数（对数桶，定义见 KernelHistUpperUs）。 */
inline constexpr std::size_t kKernelHistBuckets = 32;

/** @brief 32 个累计计数桶；三个 tracepoint 监控器共用同一套桶边界。 */
using KernelHistogram = std::array<std::uint64_t, kKernelHistBuckets>;

/**
 * @brief 第 bucket 个直方图桶的上界（微秒）。
 *
 * 桶 0 是 `[0, 1µs)`；桶 i（1..30）是 `[2^(i-1), 2^i) µs`；桶 31 是
 * `[2^30 µs, +∞)` 的溢出桶。桶 0..30 返回桶上界，分位数是保守估计（只
 * 会略高、不会偏低）；溢出桶没有上界，返回下界 2^30 µs，落在该桶的分位
 * 数会偏低（只影响超过 17.9 分钟的极端长尾）。
 */
double KernelHistUpperUs(std::size_t bucket) noexcept;

/** @brief 第 bucket 个桶的可读区间，如 `1-2us`、`<1us`、`>=1073.7s`。 */
std::string KernelHistLabel(std::size_t bucket);

/**
 * @brief 从累计直方图估算分位数（单位微秒）。
 * @param hist  累计计数
 * @param total 样本总数；为 0 时返回 0
 * @param percentile 0..1，例如 0.99
 */
double KernelHistPercentile(const KernelHistogram& hist, std::uint64_t total,
                            double percentile);

/** @brief 单个 syscall 的延迟聚合（用户态计算，内核只存原始计数）。 */
struct SyscallLatency {
    std::uint32_t id{0};
    std::string name;  ///< 来自 x86_64 syscall 名表；未知编号为 syscall_<id>
    std::uint64_t count{0};
    double avg_us{0.0};
    double p50_us{0.0};
    double p99_us{0.0};
    double max_us{0.0};
};

/** @brief SyscallMonitor 的一次采样结果。 */
struct SyscallStats {
    bool active{false};
    std::string reason;
    std::uint64_t total{0};
    KernelHistogram hist{};  ///< 全部 syscall 的合并直方图
    std::vector<SyscallLatency> top;  ///< 按 count 降序，默认前 10
};

/** @brief 单任务的调度统计。 */
struct SchedTaskStat {
    std::uint32_t pid{0};
    std::string comm;
    std::uint64_t switches{0};
    double on_cpu_ms{0.0};
    std::uint64_t wait_count{0};
    double wait_avg_us{0.0};
    double wait_max_us{0.0};
};

/** @brief SchedMonitor 的一次采样结果（runqueue wait 为主）。 */
struct SchedStats {
    bool active{false};
    std::string reason;
    std::uint64_t switches{0};    ///< 全局上下文切换次数
    std::uint64_t wait_count{0};  ///< 有唤醒起点的 runqueue wait 样本数
    double wait_avg_us{0.0};
    double wait_p50_us{0.0};
    double wait_p99_us{0.0};
    double wait_max_us{0.0};
    KernelHistogram hist{};
    std::vector<SchedTaskStat> top;
};

/** @brief 单进程的 futex 统计。 */
struct FutexTaskStat {
    std::uint32_t pid{0};
    std::string comm;
    /// 真正睡着后被成功唤醒的 FUTEX_WAIT 次数（syscall 返回 0）。只有这类
    /// 样本计入等待时长与全局等待直方图。
    std::uint64_t wait_count{0};
    std::uint64_t wake_count{0};
    double wait_avg_us{0.0};
    double wait_max_us{0.0};
    /// FUTEX_WAIT 返回 -EAGAIN（uaddr 值不匹配，内核未睡眠）的次数。
    std::uint64_t eagain_count{0};
    /// FUTEX_WAIT 返回 -ETIMEDOUT（带超时的等待到点）的次数。
    std::uint64_t timedout_count{0};
    /// 其余负返回值（-EINTR/-EFAULT/-ENOSYS/...）的次数；-EINTR 可能睡眠过，
    /// 但语义不纯，不混入等待分布。
    std::uint64_t error_count{0};
};

/** @brief FutexMonitor 的一次采样结果。 */
struct FutexStats {
    bool active{false};
    std::string reason;
    /// 真正睡着后被成功唤醒的 FUTEX_WAIT 次数（syscall 返回 0）。等待直方图、
    /// wait_avg_us、wait_p50/p99 都只基于这一类样本。
    std::uint64_t wait_count{0};
    std::uint64_t wake_count{0};
    /// FUTEX_WAIT 值不匹配（-EAGAIN，未睡眠）的次数；不计入直方图。
    std::uint64_t eagain_count{0};
    /// FUTEX_WAIT 超时（-ETIMEDOUT）的次数；不计入直方图。
    std::uint64_t timedout_count{0};
    /// 其余负返回值的次数；不计入直方图。
    std::uint64_t error_count{0};
    /// 成功等待样本数 / wake 类调用数；wake 为 0 时为 0。注意这是「等待
    /// 调用 / 唤醒调用」的比值，不是等待线程数（一次 WAKE 可唤醒多个）。
    double wait_wake_ratio{0.0};
    double wait_avg_us{0.0};
    double wait_p50_us{0.0};
    double wait_p99_us{0.0};
    double wait_max_us{0.0};
    KernelHistogram hist{};
    std::vector<FutexTaskStat> top;
};

// ---------------------------------------------------------------------------
// TCP 重传 / RTT 与文件 IO 延迟（eBPF tracepoint / kprobe）
// ---------------------------------------------------------------------------

/** @brief 单条 TCP 连接的重传统计与 RTT 分位数。 */
struct TcpConnStat {
    std::string local;   ///< 本地地址，如 "127.0.0.1:8080"
    std::string remote;  ///< 对端地址
    std::string family;  ///< "ipv4"、"ipv6" 或 "family_<n>"
    std::uint64_t retransmits{0};
    /// 1/8 采样后的样本数。每个样本是内核平滑后的 srtt，不是单次 RTT。
    std::uint64_t rtt_samples{0};
    double rtt_avg_us{0.0};  ///< srtt 平均值（µs）
    double rtt_p50_us{0.0};  ///< srtt p50（µs）
    double rtt_p99_us{0.0};  ///< srtt p99（µs）
    double rtt_max_us{0.0};  ///< srtt 最大值（µs）
};

/** @brief TcpMonitor 的一次采样结果。 */
struct TcpStats {
    bool active{false};
    std::string reason;
    /// "tcp_probe" 或降级原因（例如 "tcp_probe unavailable: ..."）。
    std::string rtt_source;
    std::uint64_t retransmits{0};     ///< 自 attach 起的累计重传次数
    double retransmits_per_sec{0.0};  ///< 相邻两次采样的差分速率
    /// 采样后的 srtt 样本数（tcp_probe 只有平滑值，没有单次 RTT）。
    std::uint64_t rtt_samples{0};
    KernelHistogram rtt_hist{};  ///< srtt 直方图（µs）
    double rtt_p50_us{0.0};      ///< srtt p50（µs）
    double rtt_p99_us{0.0};      ///< srtt p99（µs）
    double rtt_avg_us{0.0};      ///< srtt 平均值（µs）
    double rtt_max_us{0.0};      ///< srtt 最大值（µs）

    /// 直方图加权口径的机器可读说明。当前固定为：
    /// "rtt_hist=1/8_ack_samples; conn_rtt_hist=one_sample_per_connection"。
    /// rtt_hist 按 1/8 ACK 采样样本加权（每样本 1），**不是严格报文/字节
    /// 加权**；tcp_probe 的 srtt 是内核平滑值，没有单报文 RTT。
    std::string rtt_weighting{
        "rtt_hist=1/8_ack_samples; conn_rtt_hist=one_sample_per_connection"};

    /// 连接加权直方图的样本数 = 至少被采样过一次的不同连接数。
    std::uint64_t conn_rtt_samples{0};
    /// 每连接一个 srtt 样本的直方图（计数单位是连接数，不是 ACK 数）。
    KernelHistogram conn_rtt_hist{};
    double conn_rtt_p50_us{0.0};  ///< 连接加权 srtt p50（µs）
    double conn_rtt_p99_us{0.0};  ///< 连接加权 srtt p99（µs）

    std::vector<TcpConnStat> top;  ///< 按重传次数降序
};

/**
 * @brief 单进程的文件 IO 统计（进程级分位数不支持，见内核程序注释）。
 *
 * pid 语义：内核侧用 bpf_get_current_pid_tgid() 作 map key，它返回的是
 * **init PID namespace** 的 tgid。在嵌套 pid namespace（WSL2 容器、Docker、
 * unshare -p）里它与 getpid()/ps 看到的 pid 不同（本机实测偏移 391）。
 * namespace_pid 是采集端当前 namespace 的 pid（可直接对照 ps），由 eBPF
 * helper bpf_get_ns_current_pid_tgid() 按 /proc/self/ns/pid 的 dev/ino
 * 解析；内核不支持该 helper 或解析失败时为 0，并用 /proc 的 NSpid 做
 * best-effort 回退。
 */
struct FileIoTaskStat {
    /// init PID namespace 的 tgid；与 ps/getpid() 对照请用 namespace_pid。
    std::uint32_t pid{0};
    std::string comm;
    std::uint64_t read_count{0};
    std::uint64_t write_count{0};
    double read_avg_us{0.0};
    double read_max_us{0.0};
    double write_avg_us{0.0};
    double write_max_us{0.0};
    /// 采集端当前 PID namespace 的 pid（ps 可见）；0 表示无法解析。
    std::uint32_t namespace_pid{0};
};

/** @brief FileIoMonitor 的一次采样结果。 */
struct FileIoStats {
    bool active{false};
    std::string reason;
    std::uint64_t read_count{0};
    std::uint64_t write_count{0};
    KernelHistogram read_hist{};
    KernelHistogram write_hist{};
    double read_p50_us{0.0};
    double read_p99_us{0.0};
    double read_avg_us{0.0};
    double read_max_us{0.0};
    double write_p50_us{0.0};
    double write_p99_us{0.0};
    double write_avg_us{0.0};
    double write_max_us{0.0};
    double read_iops{0.0};  ///< 相邻两次采样的差分 IOPS
    double write_iops{0.0};

    // ---- readv/writev（vfs_readv/vfs_writev）----
    // readv/preadv 走 do_readv/do_preadv -> vfs_readv，绕过 vfs_read；这组
    // 计数与 read_count 独立、不重复。只给 count/avg/max，分位数仍来自
    // vfs_read/vfs_write 的直方图（原因见 fileio_monitor.bpf.c）。
    std::uint64_t readv_count{0};
    std::uint64_t writev_count{0};
    double readv_avg_us{0.0};
    double readv_max_us{0.0};
    double writev_avg_us{0.0};
    double writev_max_us{0.0};

    // ---- io_uring（io_read/io_write）----
    // uring_*_count 只统计「同步完成」（io_read 返回 IOU_OK=0）的调用，
    // 其延迟可信；uring_*_pending 统计异步排队/错误（返回 -EIOCBQUEUED
    // 等负数），这些调用在 kretprobe 返回时 IO 尚未完成，**不进延迟统计**。
    std::uint64_t uring_read_count{0};
    std::uint64_t uring_write_count{0};
    std::uint64_t uring_read_pending{0};
    std::uint64_t uring_write_pending{0};
    double uring_read_avg_us{0.0};
    double uring_read_max_us{0.0};
    double uring_write_avg_us{0.0};
    double uring_write_max_us{0.0};

    /// 本次运行已覆盖的路径（逗号分隔），如
    /// "vfs_read,vfs_write,vfs_readv,vfs_writev,io_read,io_write"。
    std::string coverage;
    /// 未覆盖的路径与原因；空表示全部覆盖成功。
    std::string coverage_gap;

    std::vector<FileIoTaskStat> top;  ///< 按 IO 次数降序
};

/** @brief 主机静态信息。 */
struct HostInfo {
    std::string hostname;
    std::string os;
    std::string kernel;
    std::string arch;
};

/** @brief 一次完整采集的结果；各采集器只填自己的字段。 */
struct Snapshot {
    std::string host;
    std::int64_t timestamp_ms{0};

    HostInfo host_info;
    CpuLoad load;
    std::vector<CpuCore> cpus;
    MemInfo mem;
    std::vector<DiskStat> disks;
    std::vector<NetDevStat> nets;
    std::vector<NetEbpfStat> ebpf_nets;

    /// 三个 tracepoint 监控器的结果；未启用/不可用时 active=false。
    SyscallStats syscalls;
    SchedStats sched;
    FutexStats futex;

    /// TCP 重传/RTT 与文件 IO 延迟；未启用/不可用时 active=false。
    TcpStats tcp;
    FileIoStats fileio;

    /// eBPF 网络统计是否生效；false 时 ebpf_reason 给出原因。
    bool ebpf_active{false};
    std::string ebpf_reason;
};

/** @brief Go 风格接口：一次采集 + 停止。 */
class Monitor {
public:
    Monitor() = default;
    virtual ~Monitor() = default;
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;
    Monitor(Monitor&&) = delete;
    Monitor& operator=(Monitor&&) = delete;

    /**
     * @brief 采集一次并写入 snapshot 中本采集器负责的字段。
     * @return kOk 表示写入成功；kUnsupported 表示环境不支持（不可重试）。
     */
    virtual Status UpdateOnce(Snapshot& snapshot) = 0;

    /** @brief 停止采集并释放资源；可重复调用。 */
    virtual void Stop() = 0;

    /** @brief 当前采集器是否可用。 */
    virtual bool available() const { return true; }

    /** @brief 不可用时的人类可读原因。 */
    virtual std::string reason() const { return {}; }
};

// ---- JSON 序列化（nlohmann::json ADL）----
void to_json(Json& j, const CpuCore& value);
void from_json(const Json& j, CpuCore& value);
void to_json(Json& j, const CpuLoad& value);
void from_json(const Json& j, CpuLoad& value);
void to_json(Json& j, const MemInfo& value);
void from_json(const Json& j, MemInfo& value);
void to_json(Json& j, const DiskStat& value);
void from_json(const Json& j, DiskStat& value);
void to_json(Json& j, const NetDevStat& value);
void from_json(const Json& j, NetDevStat& value);
void to_json(Json& j, const NetEbpfStat& value);
void from_json(const Json& j, NetEbpfStat& value);
void to_json(Json& j, const SyscallLatency& value);
void from_json(const Json& j, SyscallLatency& value);
void to_json(Json& j, const SyscallStats& value);
void from_json(const Json& j, SyscallStats& value);
void to_json(Json& j, const SchedTaskStat& value);
void from_json(const Json& j, SchedTaskStat& value);
void to_json(Json& j, const SchedStats& value);
void from_json(const Json& j, SchedStats& value);
void to_json(Json& j, const FutexTaskStat& value);
void from_json(const Json& j, FutexTaskStat& value);
void to_json(Json& j, const FutexStats& value);
void from_json(const Json& j, FutexStats& value);
void to_json(Json& j, const TcpConnStat& value);
void from_json(const Json& j, TcpConnStat& value);
void to_json(Json& j, const TcpStats& value);
void from_json(const Json& j, TcpStats& value);
void to_json(Json& j, const FileIoTaskStat& value);
void from_json(const Json& j, FileIoTaskStat& value);
void to_json(Json& j, const FileIoStats& value);
void from_json(const Json& j, FileIoStats& value);
void to_json(Json& j, const HostInfo& value);
void from_json(const Json& j, HostInfo& value);
void to_json(Json& j, const Snapshot& value);
void from_json(const Json& j, Snapshot& value);

/** @brief Snapshot 序列化为紧凑 JSON 字符串。 */
std::string SnapshotToJson(const Snapshot& snapshot);

/** @brief 从 JSON 字符串还原 Snapshot；格式错误返回 false 并写 error。 */
bool SnapshotFromJson(const std::string& text, Snapshot& snapshot,
                      std::string* error = nullptr);

/**
 * @brief 健康评分（0-100，越高越好）。
 *
 * 与参考实现的 HostManager::CalcScore 对齐：CPU、内存、磁盘 util、负载四项
 * 加权扣分，网络错误率作为附加惩罚。所有输入都来自 Snapshot，缺项按 0 计。
 */
double HealthScore(const Snapshot& snapshot);

}  // namespace go2cpp::os::monitor
