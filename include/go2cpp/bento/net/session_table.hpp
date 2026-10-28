#pragma once

/**
 * @file session_table.hpp
 * @brief 线程安全会话表（MOD-14）：`key → {server, clients[]}` 的 1:N 映射、
 *        客户端上限与 90s 租约过期清理。
 * @details 背景：JinShan 要求「一个被控 Host 对应 N 个 Viewer」且所有连接
 *          共享同一邀请密钥（FR-06 / RISK-032）。配对协议（MOD-13）只解决
 *          「这一对是否可信」，本组件负责回答「某个 key 现在有哪些会话」：
 *
 *          - **1:N**：每个 key 恰好一个 server（Host），最多
 *            `max_clients_per_key`（默认 4）个 client（Viewer）；
 *          - **拒绝重复**：同一 key 重复 AddServer 返回 `kAlreadyExists`，
 *            不会出现两个 server 抢同一 key（AX-009 的 double-bind 防护）；
 *          - **上限拒绝**：第 5 个 client 返回 `kFull`，调用方应转成明确的
 *            `ERR_ATTENDEE_LIMIT` 提示，而不是静默黑屏（RISK-032）；
 *          - **租约**：条目带 90s 租约（`lease_deadline_ms`），由真实的
 *            活跃操作（AddClient / RemoveClient / Touch 心跳）续期；`Find` /
 *            `List` 只**过滤**过期条目而不改写表，物理删除交给 `Tick` 或
 *            下一次写操作。这样读路径也是纯只读语义，不会被一次查询“续命”；
 *          - **线程安全**：整表一把 `std::mutex`。会话表规模是「一个 Host
 *            的 key 数 × 每 key 4 个 client」，操作 O(log n)，单锁足以覆盖
 *            16 线程并发注册且不会出现锁顺序问题；将来若做多 Host 聚合，
 *            可在外层按 key 分片而不改本类接口。
 *
 *          租约语义（`lease_ms == 0` 表示永不过期，仅测试/嵌入式建议）：
 *          - `AddServer` / `AddClient` / `RemoveClient` / `Touch` 把条目的
 *            `lease_deadline_ms` 刷新为 `now + lease_ms`；
 *          - `Find` / `List` 对 `now >= lease_deadline_ms` 的条目返回空/跳过，
 *            但不删除；
 *          - `Tick(now)` 物理删除所有过期条目并返回删除数量；`AddServer` /
 *            `AddClient` 等写操作也会顺带清理，避免过期条目占用 max_keys。
 *
 *          典型用法（Host 每收到配对成功调用 AddClient，心跳调用 Touch）：
 *          ```cpp
 *          SessionTable table;
 *          table.AddServer(invite_key, host_id);         // 启动监听时
 *          table.AddClient(invite_key, viewer_id);       // 每个 Viewer 配对成功
 *          table.Touch(invite_key);                      // 心跳续租
 *          if (!table.Find(invite_key)) { ... }          // 已过期
 *          ```
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "go2cpp/bento/net/protocol.hpp"

namespace go2cpp::bento {

/// 每个 key 默认最多 4 个 client（Viewer）。与 RISK-032 的 AttendeeLimit 对齐。
inline constexpr std::size_t kSessionMaxClientsPerKey = 4;

/// 默认租约（毫秒）。90s 内没有任何活跃操作即视为 Host 已失联。
inline constexpr std::uint64_t kSessionLeaseMs = 90000;

/// 默认 key 数量上限，防止无限注册把表撑爆。
inline constexpr std::size_t kSessionMaxKeys = 1024;

/// @brief 会话表操作结果。
enum class SessionStatus : std::uint8_t {
    kOk = 0,              ///< 成功。
    kNotFound = 1,        ///< key/server/client 不存在或已过期。
    kAlreadyExists = 2,   ///< 重复 key（AddServer）或重复 client。
    kFull = 3,            ///< 该 key 的 client 已达上限。
    kExpired = 4,         ///< 条目已过期（保留给调用方显式判断）。
    kInvalidArgument = 5, ///< 空 key 或 id == 0。
    kKeyLimit = 6,        ///< 全表 key 数已达上限。
};

/// @brief 结果名（稳定小写字符串，可做日志/指标标签）。
const char* SessionStatusName(SessionStatus status) noexcept;

/// @brief 一个 client 会话（Viewer）。
struct SessionClient {
    std::uint64_t client_id = 0;  ///< Viewer 的稳定标识（非 0）。
    std::uint64_t joined_ms = 0;  ///< 加入时间（调用方时钟毫秒）。
};

/// @brief 一个 key 对应的会话条目（一个 server + 0..N 个 client）。
struct SessionEntry {
    std::string key;                             ///< 邀请密钥（原样保存，不上线）。
    std::uint64_t server_id = 0;                 ///< Host 的稳定标识（非 0）。
    std::vector<SessionClient> clients;          ///< 按加入顺序排列的 Viewer。
    std::uint64_t created_ms = 0;                ///< 创建时间（毫秒）。
    std::uint64_t lease_deadline_ms = 0;         ///< 租约到期绝对时间；0 = 永不过期。

    /// @brief client 数量。
    std::size_t ClientCount() const noexcept { return clients.size(); }

    /// @brief 是否包含指定 client。
    bool HasClient(std::uint64_t client_id) const noexcept;

    /// @brief 是否已过期（lease_deadline_ms == 0 视为永不过期）。
    bool IsExpired(std::uint64_t now_ms) const noexcept {
        return lease_deadline_ms != 0 && now_ms >= lease_deadline_ms;
    }
};

/// @brief 会话表配置。
struct SessionTableConfig {
    /// 每个 key 最多 client 数；0 钳到 1。
    std::size_t max_clients_per_key = kSessionMaxClientsPerKey;
    /// 租约毫秒；0 = 永不过期（仅测试/嵌入式建议）。
    std::uint64_t lease_ms = kSessionLeaseMs;
    /// 全表 key 上限；0 钳到 1。
    std::size_t max_keys = kSessionMaxKeys;
};

/// @brief 会话表计数器快照。
struct SessionTableStats {
    std::uint64_t servers_added = 0;      ///< 成功 AddServer 次数。
    std::uint64_t clients_added = 0;      ///< 成功 AddClient 次数。
    std::uint64_t removals = 0;           ///< 显式删除次数（整条/client/server）。
    std::uint64_t expirations = 0;        ///< 过期清理删除的条目数。
    std::uint64_t duplicate_rejects = 0;  ///< 重复 key/client 拒绝次数。
    std::uint64_t full_rejects = 0;       ///< client 超限拒绝次数。
    std::uint64_t key_limit_rejects = 0;  ///< key 数超限拒绝次数。
    std::uint64_t not_found = 0;          ///< 找不到 key/client 的次数。
    std::uint64_t touches = 0;            ///< 成功续租次数。
};

/// @brief 线程安全会话表：一个 key → 一个 server + 至多 N 个 client。
class SessionTable final {
   public:
    explicit SessionTable(SessionTableConfig config = {});

    SessionTable(const SessionTable&) = delete;
    SessionTable& operator=(const SessionTable&) = delete;

    // ---- 写入 -------------------------------------------------------------

    /// @brief 注册 server（Host）。重复 key 返回 kAlreadyExists。
    SessionStatus AddServer(const std::string& key, std::uint64_t server_id,
                            std::uint64_t now_ms = NowMs());

    /// @brief 为已存在的 key 添加 client（Viewer）。
    /// @return kOk；kNotFound（key 不存在/过期）；kAlreadyExists（重复
    ///         client_id）；kFull（达到 max_clients_per_key）；kInvalidArgument。
    SessionStatus AddClient(const std::string& key, std::uint64_t client_id,
                            std::uint64_t now_ms = NowMs());

    /// @brief 删除整个 key 条目（server 下线/邀请撤销）。
    /// @return 存在且未过期返回 true。
    bool Remove(const std::string& key, std::uint64_t now_ms = NowMs());

    /// @brief 移除一个 client；key 或 client 不存在返回 kNotFound。
    SessionStatus RemoveClient(const std::string& key, std::uint64_t client_id,
                               std::uint64_t now_ms = NowMs());

    /// @brief 校验 server_id 后删除整个条目；不匹配返回 kNotFound。
    SessionStatus RemoveServer(const std::string& key, std::uint64_t server_id,
                               std::uint64_t now_ms = NowMs());

    /// @brief 续租（心跳）；条目不存在或已过期返回 false。
    bool Touch(const std::string& key, std::uint64_t now_ms = NowMs());

    // ---- 查询 -------------------------------------------------------------

    /// @brief 查找 key 的会话条目副本；不存在/已过期返回 nullopt。
    /// @note 纯只读，不续租、不清理（过期条目由 Tick/写操作物理删除）。
    std::optional<SessionEntry> Find(const std::string& key,
                                     std::uint64_t now_ms = NowMs()) const;

    /// @brief 列出所有未过期条目（按 key 字典序）。
    std::vector<SessionEntry> List(std::uint64_t now_ms = NowMs()) const;

    /// @brief key 当前是否存在且未过期。
    bool Contains(const std::string& key, std::uint64_t now_ms = NowMs()) const;

    /// @brief 当前表项数量（包含尚未被 Tick 清理的过期项）。
    std::size_t size() const;

    /// @brief 表是否为空（语义同 size() == 0）。
    bool empty() const;

    // ---- 维护 -------------------------------------------------------------

    /// @brief 物理清理所有过期条目，返回删除数量。
    std::size_t Tick(std::uint64_t now_ms = NowMs());

    /// @brief 计数器快照。
    SessionTableStats stats() const;

    /// @brief 实际生效的配置（构造时已钳过非法值）。
    const SessionTableConfig& config() const noexcept { return m_config; }

   private:
    /// 刷新条目的租约（lease_ms == 0 时保持 0）。
    void RefreshLeaseLocked(SessionEntry& entry, std::uint64_t now_ms) const;
    /// 调用方必须已持锁；删除所有过期条目并返回数量。
    std::size_t RemoveExpiredLocked(std::uint64_t now_ms);

    SessionTableConfig m_config;
    mutable std::mutex m_mutex;
    std::map<std::string, SessionEntry> m_entries;
    SessionTableStats m_stats;
};

}  // namespace go2cpp::bento
