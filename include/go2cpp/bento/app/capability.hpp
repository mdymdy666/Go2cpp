#pragma once

/**
 * @file capability.hpp
 * @brief Linux 运行环境能力探测（MOD-37）：/proc、权限、epoll、cgroup、
 *        网络接口、文件系统与 rlimit 的可运行性矩阵。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 *
 * @details 背景：Bento 的通用模块会被嵌进各种宿主环境——开发机、精简容器、
 *          只读根文件系统、受限 rlimit 的 sidecar。很多「连不上/起不来」的问题
 *          不是协议 bug，而是环境缺少前置条件（`/proc` 被隐藏、`epoll` 被
 *          seccomp 禁用、`RLIMIT_NOFILE` 太小、根文件系统只读）。如果这些
 *          条件到了业务路径才以 `ECONNREFUSED`/`ENOSPC` 的形式暴露，排障成本
 *          很高。本组件在启动阶段做一次**只读**探测，把「能用 / 不可用 / 受限」
 *          与原因一次性摊开，供上层决定降级策略（例如禁用 UDP 回退到 TCP、
 *          减少并发、拒绝启动）。
 *
 * ## 探测项（名称稳定，可作为指标标签）
 *
 * | 名称 | 依赖 | 受限/不可用的典型原因 |
 * |---|---|---|
 * | `proc.meminfo` | `/proc/meminfo` | /proc 未挂载、容器隐藏 |
 * | `proc.cpuinfo` | `/proc/cpuinfo` | /proc 未挂载 |
 * | `proc.self.status` | `/proc/self/status` | 隐藏进程视图 |
 * | `proc.self.fd` | `/proc/self/fd` | 隐藏 fd 目录 |
 * | `unix.privilege` | `getuid`/`geteuid` + CapEff | 取不到身份（极罕见） |
 * | `ipc.epoll` | `epoll_create1` | seccomp 拒绝、fd 耗尽 |
 * | `cgroup.version` | cgroup v1/v2 根目录 | /sys/fs/cgroup 未挂载 |
 * | `cgroup.memory` | v2 `memory.max` / v1 `memory.limit_in_bytes` | 无法读取限制 |
 * | `cgroup.cpu` | v2 `cpu.max` / v1 `cpu.cfs_quota_us` | 无法读取配额 |
 * | `net.interfaces` | `getifaddrs` | 仅回环接口（无外部连通） |
 * | `fs.tmpfs` | `statfs(tmp_path)` | 临时目录不在 tmpfs 上 |
 * | `fs.root_readonly` | `statvfs(root_path)` | 根文件系统只读 |
 * | `rlimit.nofile` | `getrlimit(RLIMIT_NOFILE)` | 上限低于阈值 |
 * | `rlimit.nproc` | `getrlimit(RLIMIT_NPROC)` | 上限低于阈值 |
 * | `kernel.info` | `uname` | 系统调用失败（极罕见） |
 *
 * 正常 Linux 环境通常 15 项全部可用。探测**只读**：不写文件、不改 sysctl、
 * 不尝试网络连接；`epoll_create1` 创建的 fd 会立即关闭。为防止探测路径被
 * 指向超大文件，单个文件读取上限为 8 MiB，超限按「不可用」记录。
 *
 * ## 状态语义
 *
 * - `kAvailable`：能力存在且满足最低要求；
 * - `kDegraded`：能力存在但受限（读得到但低于阈值、只能识别版本却读不到配额、
 *   文件可读但内容缺关键字段）；
 * - `kUnavailable`：连探测都无法完成（路径不可读、系统调用失败）。
 *
 * `IsDegraded()` 对「受限」与「不可用」都返回 true——从调用方视角，两者都
 * 意味着「不能按默认假设运行」，都需要读 `Reasons()` 决定降级动作。
 *
 * ## 线程模型
 *
 * `Probe()` 是纯函数式的一次性调用，可在任意线程执行；不访问全局可变状态。
 * 唯一副作用是探测期间短暂打开 `/proc` 与 cgroup 文件、创建一个 epoll fd
 * 并立即关闭。并发调用安全，但同一进程内重复探测会重复这些系统调用；
 * 建议启动时探测一次并把 `CapabilityMatrix` 传给需要的组件。
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::bento::app {

/// @brief 单个探测项的状态。
enum class CapabilityStatus : std::uint8_t {
    kAvailable = 0,   ///< 能力存在且满足最低要求。
    kDegraded = 1,    ///< 能力存在但受限（阈值不满足/信息不完整）。
    kUnavailable = 2, ///< 能力不可用（路径不可读/系统调用失败）。
};

/// @brief 状态名（稳定小写字符串，可做日志/指标标签）："available"/"degraded"/"unavailable"。
const char* CapabilityStatusName(CapabilityStatus status) noexcept;

/**
 * @brief 一项能力探测结果。
 *
 * @details `reason` 只在非 `kAvailable` 时填写，必须是**可操作**的原因
 *          （例如 "RLIMIT_NOFILE=64 < min_nofile=256"），不能只写 "failed"。
 *          `detail` 记录探测到的具体数值（内存、CPU 数、cgroup 限制），
 *          便于日志与排障，不参与判定。
 */
struct CapabilityItem {
    std::string name;                                       ///< 稳定名称，见文件头表格。
    CapabilityStatus status = CapabilityStatus::kAvailable; ///< 探测结论。
    std::string reason;                                     ///< 受限/不可用原因；可用时为空。
    std::string detail;                                     ///< 可读的探测明细（数值/版本）。

    /// @brief 是否完全可用。
    bool available() const noexcept { return status == CapabilityStatus::kAvailable; }
    /// @brief 是否受限（仍然存在但低于要求）。
    bool degraded() const noexcept { return status == CapabilityStatus::kDegraded; }
    /// @brief 是否不可用。
    bool unavailable() const noexcept { return status == CapabilityStatus::kUnavailable; }
};

/**
 * @brief 一次完整探测的结果矩阵。
 *
 * @details 项顺序固定（与文件头表格一致），便于测试与快照对比；`Find` 按名称
 *          查找，找不到返回 `nullptr`（调用方应处理新增/缺失项，而不是假设
 *          永远存在）。
 */
struct CapabilityMatrix {
    std::vector<CapabilityItem> items; ///< 探测项，顺序稳定。

    /// @brief 项数（正常 Linux 环境为 15）。
    std::size_t Size() const noexcept { return items.size(); }
    /// @brief 是否没有探测项（例如空配置导致全部跳过）。
    bool Empty() const noexcept { return items.empty(); }

    /// @brief 按名称查找；不存在返回 nullptr。
    const CapabilityItem* Find(std::string_view name) const noexcept;

    /// @brief 统计某状态的项数。
    std::size_t Count(CapabilityStatus status) const noexcept;

    /// @brief 是否存在任何非 `kAvailable` 项（受限或不可用）。
    bool IsDegraded() const noexcept;

    /**
     * @brief 全部非可用项的原因，格式为 `"<name>: <reason>"`。
     * @details 顺序与 `items` 一致；`reason` 为空时退化为状态名，保证每条都能
     *          直接写进日志。全可用时返回空 vector。
     */
    std::vector<std::string> Reasons() const;

    /// @brief 单行摘要（项数/可用数/受限数/不可用数），适合启动日志。
    std::string Summary() const;
};

/**
 * @brief 探测配置（测试可注入假路径与阈值）。
 *
 * @details 所有路径都允许覆盖，因此测试可以在不改变真实系统的前提下模拟
 *          「/proc 不可读」「cgroup 未挂载」「rlimit 过低」等场景。
 */
struct ProbeOptions {
    std::string proc_root = "/proc";            ///< /proc 挂载点。
    std::string cgroup_root = "/sys/fs/cgroup"; ///< cgroup v1/v2 根目录。
    std::string tmp_path = "/tmp";              ///< 用于判断 tmpfs 的目录。
    std::string root_path = "/";                ///< 用于判断只读根文件系统。
    std::uint64_t min_nofile = 256;             ///< RLIMIT_NOFILE 软上限最低值。
    std::uint64_t min_nproc = 64;               ///< RLIMIT_NPROC 软上限最低值；0 = 不判定。
    std::uint64_t min_mem_available = 64ull * 1024ull * 1024ull; ///< 可用内存下限（字节）。
    bool check_epoll = true;                    ///< 是否实际调用 epoll_create1。
    bool check_interfaces = true;               ///< 是否枚举网络接口。
    bool check_cgroup = true;                   ///< 是否读取 cgroup 限制。
    bool check_rlimit = true;                   ///< 是否读取 rlimit。
};

/**
 * @brief 执行一次完整环境探测。
 * @param options 路径与阈值覆盖；默认探测真实系统。
 * @return 固定顺序的能力矩阵；任何单项失败都不会抛异常，而是记录为
 *         `kUnavailable`/`kDegraded` 并写明原因。
 *
 * @note 本函数不缓存结果。需要多次读取时由调用方保存返回值。
 */
CapabilityMatrix Probe(const ProbeOptions& options = ProbeOptions{});

}  // namespace go2cpp::bento::app
