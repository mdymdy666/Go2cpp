/**
 * @file socket_registry.hpp
 * @brief 本进程 socket 的轻量可观测注册表、统计与审计（MCP `socket_*` 工具的数据源）。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 *
 * @details
 * 数据来源优先级：仓库已有 `go2cpp::os::monitor`/`debug` 面向的是全系统或调度
 * 事件，没有「本进程某个 fd 的收发字节与生命周期事件」，因此这里新增一个
 * **轻量全局注册表**（任务 1.3 允许的兜底路径）：
 *
 * - **白名单**：注册与关闭都先经 `/proc/self/fd/<fd>` 校验，只有指向本进程
 *   `socket:[inode]` 的 fd 才能登记；`socket_close` 只关闭**已登记**的 fd，
 *   因此 MCP 服务自身的监听 fd、stdio 管道、任意普通文件永远不在可关闭集合内。
 * - **身份校验**：登记时保存 `socket:[inode]`，每次查询/关闭都比对 inode。
 *   fd 被关闭后复用给新 socket/文件时，旧墓碑不会把新 fd 误认成旧 socket
 *   （避免 ABA）。
 * - **统计**：应用层可用 `RecordRead/RecordWrite/RecordError/RecordTimeout`
 *   埋点；TCP 还会读取内核 `TCP_INFO`（已确认发送字节、接收字节、重传、RTT），
 *   两者分开展示，**不做相加**，避免重复计数。
 * - **事件**：每个 fd 一个固定容量环形缓冲（256 条），记录登记/连接/读写/
 *   错误/超时/关闭；查询时对 TCP 采样内核字节增量并追加 `read/write`
 *   （`detail="tcp_info"`），因此未埋点的真实 TCP 流量也能在看到事件。
 * - **审计**：`Close` 在真正关闭前写审计记录（fail-closed：审计写入失败即
 *   拒绝关闭），拒绝/失败同样留痕，记录为单行 JSON（JSON Lines）。
 *
 * ## 边界（如实声明）
 *
 * - 自动登记：MCP 侧安装 `network` 的生命周期钩子（见
 *   `socket_autoregister.hpp`），启用 MCP 后 `go2cpp::net::Socket`（含
 *   `ListenTCP`/`DialTCP`/accept 连接）创建即进入可关闭白名单。安装前已存在
 *   的 socket、或直接用 `::socket()` 创建的裸 fd 仍需业务方显式
 *   `SocketRegistry::Register()`；未登记 socket 只能只读观察
 *   （`socket_list` 中 `registered=false`、`closable=false`）。
 * - 事件是**进程内**的：只覆盖本注册表登记后发生的事件，进程重启即丢失；
 *   环形缓冲满后丢弃最旧事件并累加 `dropped_events`。
 * - UDP/Unix socket 没有 `TCP_INFO`，字节数只来自应用层埋点；`tcp.available`
 *   为 false，不猜测数值。
 * - 审计文件默认 `log/mcp-socket-audit.log`（可用环境变量
 *   `GO2CPP_MCP_SOCKET_AUDIT` 覆盖），权限受 umask 影响；文件写入不是事务的，
 *   进程崩溃时最后一行可能不完整。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "mcp/protocol/types.hpp"

namespace go2cpp::mcp::tools {

/** @brief socket 协议类别（来自 `getsockopt(SO_TYPE)` + `getsockname` 的地址族）。 */
enum class SocketProtocol : std::uint8_t {
    kUnknown = 0,
    kTcp = 1,           ///< AF_INET/AF_INET6 + SOCK_STREAM
    kUdp = 2,           ///< AF_INET/AF_INET6 + SOCK_DGRAM
    kUnixStream = 3,    ///< AF_UNIX + SOCK_STREAM
    kUnixDatagram = 4,  ///< AF_UNIX + SOCK_DGRAM
};

/// 协议名；未知值返回 "unknown"。
const char* SocketProtocolName(SocketProtocol protocol) noexcept;

/** @brief socket 当前状态（每次查询重新探测，不缓存过期状态）。 */
enum class SocketState : std::uint8_t {
    kUnknown = 0,
    kListening = 1,    ///< `SO_ACCEPTCONN` 为真
    kConnected = 2,    ///< `getpeername` 成功
    kUnconnected = 3,  ///< 已创建但未连接（UDP 常见）
    kClosed = 4,       ///< 注册表墓碑（fd 已关闭）；`socket_list` 不返回
};

const char* SocketStateName(SocketState state) noexcept;

/** @brief 事件类别（稳定；新增只能追加，序列化使用名字）。 */
enum class SocketEventKind : std::uint8_t {
    kRegistered = 0,
    kConnected = 1,
    kRead = 2,
    kWrite = 3,
    kError = 4,
    kTimeout = 5,
    kCloseRequested = 6,
    kClosed = 7,
};

const char* SocketEventKindName(SocketEventKind kind) noexcept;

/** @brief 一条 socket 事件。 */
struct SocketEvent {
    SocketEventKind kind{SocketEventKind::kRegistered};
    std::int64_t timestamp_ms{0};  ///< Unix 毫秒（system_clock）
    std::uint64_t bytes{0};        ///< 本次读写字节数；其它事件为 0
    int error{0};                  ///< errno；非错误事件为 0
    std::string detail;            ///< 来源（app/tcp_info/关闭原因等）

    json ToJson() const;
};

/** @brief `socket_list` 的单项快照。 */
struct SocketSnapshot {
    int fd{-1};
    SocketProtocol protocol{SocketProtocol::kUnknown};
    SocketState state{SocketState::kUnknown};
    std::string local_address;
    std::string remote_address;
    std::string label;    ///< 登记时业务给的可读标签
    std::string inode;    ///< `socket:[inode]` 的 inode，用于识别 fd 复用
    bool registered{false};  ///< 是否在可关闭白名单内
    bool closable{false};    ///< registered 且 fd 仍在
    std::int64_t registered_at_ms{0};

    json ToJson() const;
};

/** @brief 应用层埋点计数（不是内核计数）。 */
struct SocketCounters {
    std::uint64_t sent_bytes{0};
    std::uint64_t recv_bytes{0};
    std::uint64_t errors{0};
    std::uint64_t timeouts{0};
};

/** @brief 内核 `TCP_INFO` 摘要；非 TCP 或内核不支持时 `available=false`。 */
struct SocketTcpInfo {
    bool available{false};
    std::uint64_t sent_bytes{0};      ///< `tcpi_bytes_acked`：本端已发送且被确认
    std::uint64_t received_bytes{0};  ///< `tcpi_bytes_received`：本端已接收
    std::uint32_t retransmits{0};     ///< `tcpi_total_retrans`
    std::uint32_t rtt_us{0};          ///< `tcpi_rtt`（微秒）
    std::string state;                ///< 内核 TCP 状态名

    json ToJson() const;
};

/** @brief `socket_stats` 的完整结果。 */
struct SocketStats {
    int fd{-1};
    SocketProtocol protocol{SocketProtocol::kUnknown};
    SocketState state{SocketState::kUnknown};
    bool registered{false};
    SocketCounters app;
    SocketTcpInfo tcp;
    std::size_t events{0};
    std::uint64_t dropped_events{0};
    std::int64_t last_event_ms{0};

    json ToJson() const;
};

/** @brief `socket_close` 的结果：成功/拒绝都会带审计定位信息。 */
struct SocketCloseOutcome {
    bool ok{false};
    std::string error;
    std::string audit_id;
    std::string audit_sink;
};

/**
 * @brief 审计汇：`Close` 在关闭前调用，返回 false 表示本次审计未落盘（fail-closed）。
 *
 * 实现必须线程安全（`MemorySocketAuditSink` 供测试注入，文件实现供生产使用）。
 */
class SocketAuditSink {
public:
    SocketAuditSink() = default;
    virtual ~SocketAuditSink() = default;
    SocketAuditSink(const SocketAuditSink&) = delete;
    SocketAuditSink& operator=(const SocketAuditSink&) = delete;
    SocketAuditSink(SocketAuditSink&&) = delete;
    SocketAuditSink& operator=(SocketAuditSink&&) = delete;

    /// 追加一条审计记录；失败时写 `*error` 并返回 false。
    virtual bool Append(const json& record, std::string* error) = 0;
    /// 汇的可读标识（文件路径 / "memory"），写入工具结果便于定位。
    virtual std::string Describe() const = 0;
};

/**
 * @brief 追加写 JSON Lines 的审计汇（`O_APPEND|O_CREAT|O_WRONLY`，权限 0600）。
 *
 * 每次 Append 独立打开/写入/关闭，进程崩溃最多丢最后一行，不会持有长期 fd。
 */
class FileSocketAuditSink final : public SocketAuditSink {
public:
    explicit FileSocketAuditSink(std::string path);
    bool Append(const json& record, std::string* error) override;
    std::string Describe() const override;

    const std::string& path() const noexcept { return m_path; }

private:
    std::string m_path;
    std::mutex m_mutex;
};

/** @brief 内存审计汇：测试断言用，可注入写入失败。 */
class MemorySocketAuditSink final : public SocketAuditSink {
public:
    bool Append(const json& record, std::string* error) override;
    std::string Describe() const override { return "memory"; }

    /// 设置后所有 Append 失败（模拟磁盘满/只读），用于验证 fail-closed。
    void FailWith(std::string message);
    void ClearFailure();
    void Clear();
    std::vector<json> Records() const;
    std::size_t size() const;

private:
    mutable std::mutex m_mutex;
    std::vector<json> m_records;
    std::string m_failure;
};

/// 默认审计路径：`GO2CPP_MCP_SOCKET_AUDIT` 非空时优先，否则 `log/mcp-socket-audit.log`。
std::string DefaultSocketAuditPath();

/**
 * @brief 本进程 socket 观测注册表（进程级单例）。
 *
 * 线程安全：所有公开方法内部加锁；`List/Stats` 的系统调用在锁外完成，
 * 只有合并结果与事件记录在锁内。
 */
class SocketRegistry final {
public:
    /// 进程级单例；首次使用时构造。
    static SocketRegistry& Instance();

    SocketRegistry() = default;
    ~SocketRegistry() = default;
    SocketRegistry(const SocketRegistry&) = delete;
    SocketRegistry& operator=(const SocketRegistry&) = delete;
    SocketRegistry(SocketRegistry&&) = delete;
    SocketRegistry& operator=(SocketRegistry&&) = delete;

    /**
     * @brief 登记一个本进程 socket（进入可关闭白名单）。
     * @param fd 目标 fd；必须指向 `/proc/self/fd` 中的 socket。
     * @param label 可读标签，写入快照/审计。
     * @return 成功 true；失败写 `*error`。
     */
    bool Register(int fd, std::string label, std::string* error);

    /// 移出白名单（不关闭 fd）；返回是否存在过。
    bool Unregister(int fd);

    /**
     * @brief 按 socket 身份注销（fd 复用保护）：inode 与登记时一致才注销。
     * @param mark_closed true 表示 Socket 已关闭：标记墓碑并追加 closed 事件；
     *        false 表示 fd 仍存活（`Socket::release()` 移交或上层主动排除），
     *        只移出白名单，不写 closed 事件。
     * @return 确实注销返回 true；fd 已失效/已复用/不在白名单返回 false。
     *
     * @details 自动登记的关闭钩子在 `::close(fd)` 之前触发，此时 fd 仍指向
     *          原 socket；先比对 `/proc/self/fd` 的 inode，若号码已被内核
     *          复用给新 socket，则保持新条目不动，避免 ABA 误注销。
     */
    bool UnregisterIfSameSocket(int fd, bool mark_closed = true);

    /// 应用层埋点：本端读/写字节。
    void RecordRead(int fd, std::uint64_t bytes);
    void RecordWrite(int fd, std::uint64_t bytes);
    /// 应用层埋点：一次错误/超时（`error` 为 errno，0 表示未知）。
    void RecordError(int fd, int error);
    void RecordTimeout(int fd);

    /**
     * @brief 列出本进程 socket。
     * @param include_unobserved 为 true 时也返回未登记的 socket（只读、不可关闭）。
     * @param warning 非空时写入扫描告警（例如 `/proc/self/fd` 不可读）。
     */
    std::vector<SocketSnapshot> List(bool include_unobserved, std::string* warning);

    /// 查询单个 fd 的统计；fd 不是本进程 socket 时返回 nullopt 并写 `*error`。
    std::optional<SocketStats> Stats(int fd, std::string* error);

    /**
     * @brief 查询最近 limit 条事件（时间升序）。
     * @param registered 输出：该 fd 当前是否在白名单内。
     * @param found      输出：注册表里是否有该 fd 的记录（含已关闭墓碑）。
     * @param error      非空表示查询失败（fd 不属于本进程）。
     */
    std::vector<SocketEvent> Events(int fd, std::size_t limit, bool* registered,
                                    bool* found, std::string* error);

    /**
     * @brief 关闭白名单内的 socket 并写审计。
     * @param reason 审计原因（工具参数 `reason`）；可为空。
     * @param audit  审计汇；写入失败时**不关闭**（fail-closed）。
     *
     * 未登记、非本进程 socket、inode 已变化（fd 复用）都会被拒绝，并写
     * `result=denied` 的审计记录。
     */
    SocketCloseOutcome Close(int fd, const std::string& reason, SocketAuditSink& audit);

    /// 清空注册表（测试/会话重置）；不关闭 fd。
    void Reset();

    /// 当前白名单内、仍存活的登记数。
    std::size_t size() const;

    /// 每个 fd 的事件环形容量（超出丢弃最旧并累加 dropped_events）。
    static constexpr std::size_t kEventCapacity = 256;
    /// 注册表条目上限（含墓碑）；超过时清理最旧的已关闭条目。
    static constexpr std::size_t kMaxEntries = 4096;

private:
    struct Entry {
        int fd{-1};
        std::string inode;
        SocketProtocol protocol{SocketProtocol::kUnknown};
        SocketState state{SocketState::kUnknown};
        std::string local_address;
        std::string remote_address;
        std::string label;
        std::int64_t registered_at_ms{0};
        bool registered{false};
        bool closed{false};
        SocketCounters counters;
        SocketTcpInfo last_tcp;
        std::uint64_t last_tcp_sent{0};
        std::uint64_t last_tcp_recv{0};
        std::deque<SocketEvent> events;
        std::uint64_t dropped_events{0};
    };

    /// 追加事件（锁内）：满容量时丢最旧并计数。
    void AppendEventLocked(Entry& entry, SocketEventKind kind, std::uint64_t bytes,
                           int error, std::string detail);
    /// 标记墓碑并追加 closed 事件（锁内，幂等）。
    void MarkClosedLocked(Entry& entry, const std::string& detail);
    /// 采样 TCP 字节增量并追加 read/write 事件（锁内）。
    void SampleTcpLocked(Entry& entry, const SocketTcpInfo& tcp);
    /// 超过上限时清理最旧的已关闭条目（锁内）。
    void PruneLocked();
    /// 生成审计 id（锁内，进程内单调）。
    std::string NextAuditIdLocked();

    mutable std::mutex m_mutex;
    std::unordered_map<int, Entry> m_entries;
    std::uint64_t m_audit_seq{0};
};

}  // namespace go2cpp::mcp::tools
