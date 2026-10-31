#pragma once

/**
 * @file session.h
 * @brief 会话与房间状态：在线表、昵称索引、断线 token 与超时回收。
 * @details 三个不变量：
 *          ① 在线昵称唯一（m_names 一 name 一 peer）；
 *          ② 断线后 token 在 grace 内仍可恢复昵称与房间；
 *          ③ 所有方法在内部持锁，回调线程与心跳 Fiber 可以并发调用。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace chat {

/// @brief LinkServer 的 peer 句柄（只在本机有意义）。
using PeerId = std::uint32_t;

/// @brief 一个会话的不可变快照。
struct SessionInfo {
    PeerId peer{0};
    std::string name;
    std::string room{"lobby"};
    std::string token;
    std::uint64_t last_seen_ms{0};
};

/// @brief Join/Resume 的结果；成功时带 token 与最终会话。
struct JoinResult {
    bool ok{false};
    bool resumed{false};
    std::string token;
    std::string error;
    SessionInfo session;
};

/// @brief 会话表；线程安全，所有时间参数由调用方传入（便于测试注入）。
class SessionTable final {
   public:
    explicit SessionTable(std::uint32_t reconnect_grace_ms);

    /// @brief 首次加入或重复 JOIN；token 非空时优先尝试恢复离线会话。
    JoinResult Join(PeerId peer, const std::string& name, const std::string& token,
                    std::uint64_t now_ms);

    /// @brief 断开：移出在线表，保留 token 到 grace 结束；返回断开时快照。
    std::optional<SessionInfo> Leave(PeerId peer, std::uint64_t now_ms);

    /// @brief 用 token 恢复会话到新 peer；token 无效/过期/昵称被占返回空。
    std::optional<SessionInfo> Resume(const std::string& token, PeerId peer,
                                      std::uint64_t now_ms);

    /// @brief 刷新最后活跃时间；peer 不在线返回 false。
    bool Touch(PeerId peer, std::uint64_t now_ms);

    /// @brief 切换房间；返回切换后的快照，peer 不在线返回空。
    std::optional<SessionInfo> SetRoom(PeerId peer, const std::string& room,
                                       std::uint64_t now_ms);

    std::optional<SessionInfo> Get(PeerId peer) const;
    std::optional<SessionInfo> FindByName(const std::string& name) const;
    std::vector<SessionInfo> Online() const;
    std::vector<SessionInfo> InRoom(const std::string& room) const;
    /// @brief 已知房间（至少包含 lobby），按字典序。
    std::vector<std::string> Rooms() const;

    /// @brief 取出超时会话（同时转入离线表），用于服务端广播后清理。
    std::vector<SessionInfo> ExpireIdle(std::uint64_t now_ms, std::uint32_t timeout_ms);

    /// @brief 删除过期离线 token，返回删除条数。
    std::size_t PurgeOffline(std::uint64_t now_ms);

    std::size_t online_count() const;
    std::size_t offline_count() const;

   private:
    struct OfflineEntry {
        std::string name;
        std::string room;
        std::uint64_t expires_ms{0};
    };

    /// @brief 生成新 token；调用方必须持锁，保证唯一。
    std::string NextTokenLocked();

    mutable std::mutex m_mutex;                   ///< 保护下面三张表。
    std::map<PeerId, SessionInfo> m_online;       ///< peer -> 在线会话。
    std::map<std::string, PeerId> m_names;        ///< 昵称 -> peer（唯一性索引）。
    std::map<std::string, OfflineEntry> m_offline;///< token -> 可恢复的离线会话。
    std::uint32_t m_grace_ms;                     ///< token 保留时长。
    std::uint64_t m_next_token{0};                ///< token 序号。
};

}  // namespace chat
