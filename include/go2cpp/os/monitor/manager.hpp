#pragma once

// Manager：简化的监控数据接收端。
//
// 参考实现用 gRPC + MySQL；这里改成 HTTP + 可替换存储：
//   POST /api/push               接收 Snapshot JSON，按主机名归档
//   GET  /api/query/latest?n=10  最近 n 条
//   GET  /api/query/stats        每主机最新一条 + 健康评分
//   GET  /api/query/kernel       每主机最新一条的 syscall/sched/futex 摘要
//   GET  /api/health             存活探针
//
// 解耦：Manager 只依赖 SnapshotStore（默认 MemorySnapshotStore，保留最近 N
// 条，进程退出即丢失）；持久化后端实现同一个接口即可替换。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/os/monitor/interfaces.hpp"
#include "go2cpp/os/monitor/monitor.hpp"

namespace go2cpp::os::monitor {

/** @brief Manager 配置。 */
struct ManagerConfig {
    std::string listen_address{"127.0.0.1"};
    std::uint16_t port{9101};
    /// 内存中保留的最大快照条数；注入自定义 store 时不生效。
    std::size_t capacity{256};
};

/** @brief 单主机最新状态（查询 /api/query/stats 的返回项）。 */
struct HostSummary {
    std::string host;
    std::int64_t timestamp_ms{0};
    double score{0.0};
    double cpu_percent{0.0};
    double mem_used_percent{0.0};
    double disk_util_percent{0.0};
    double load1{0.0};
    double net_rcv_rate{0.0};
    double net_snd_rate{0.0};
    bool ebpf_active{false};
};

/**
 * @brief 单个主机的内核 tracepoint 摘要（`/api/query/kernel` 的返回项）。
 *
 * 只带 top-N 热点与分位数，完整直方图仍可从 `/api/query/latest` 的
 * `syscalls/sched/futex` 字段取到。
 */
struct KernelSummary {
    std::string host;
    std::int64_t timestamp_ms{0};
    SyscallStats syscalls;
    SchedStats sched;
    FutexStats futex;
    TcpStats tcp;
    FileIoStats fileio;
};

/**
 * @brief 监控数据接收端。
 *
 * Start() 后由后台 Fiber 接受连接并处理请求；Stop() 关闭监听 socket。
 * 所有查询接口线程安全。可以注入自定义 SnapshotStore（默认内存实现），
 * 用于持久化后端或测试替身。
 */
class Manager {
public:
    /**
     * @param config 监听地址、端口与默认内存容量
     * @param store  可选的存储实现；为空时创建容量为 config.capacity 的
     *               MemorySnapshotStore
     */
    explicit Manager(ManagerConfig config = {},
                     std::shared_ptr<SnapshotStore> store = nullptr);
    ~Manager();

    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    /** @brief 绑定端口并启动 accept 循环。端口被占用时返回 false。 */
    bool Start();

    /** @brief 停止服务并关闭监听。 */
    void Stop();

    bool running() const noexcept;

    /** @brief 实际监听端口（port 配置为 0 时由内核分配）。 */
    std::uint16_t port() const;

    /** @brief 接收一条快照（HTTP handler 与测试共用）。 */
    bool Ingest(const Snapshot& snapshot);

    /** @brief 最近 n 条快照（按时间正序）。 */
    std::vector<Snapshot> Latest(std::size_t n) const;

    /** @brief 每主机最新快照的摘要，按健康评分降序。 */
    std::vector<HostSummary> Summaries() const;

    /** @brief 每主机最新快照的内核监控摘要，按主机名升序。 */
    std::vector<KernelSummary> KernelSummaries() const;

    /** @brief 已接收快照总数。 */
    std::uint64_t received() const;

    /** @brief 已拒绝（JSON 非法）次数。 */
    std::uint64_t rejected() const;

    /** @brief 存储中的快照条数。 */
    std::size_t stored() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ManagerConfig config_;
};

}  // namespace go2cpp::os::monitor
