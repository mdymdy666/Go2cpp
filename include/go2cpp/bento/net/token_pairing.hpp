#pragma once

/**
 * @file token_pairing.hpp
 * @brief Bento relay 的 (key, token) 1:N 管道配对表。
 * @details 上层把一次「客户端 → 中转管道」的配对抽象成 key + token：
 *
 *          - key 是业务侧共享标识（房间号、玩家 id、会话 key），一个 key 可
 *            同时挂 N 个 token，因此是 1:N；
 *          - token 是 8 字节随机数（16 个 hex 字符）的 bearer secret，全局
 *            唯一，用于把匿名连接认领到某个 key 下的具体 peer；
 *          - peer_id 是 relay 侧连接句柄（连接 id / session 序号），配对表
 *            本身不解释它的含义，只原样保存。
 *
 *          典型流程：服务端 Issue(key, peer) 签发 token 并发给客户端；
 *          客户端带 (key, token) 重连，服务端 Find 校验后得到 peer；连接建立
 *          完成后 Release(token) 消费掉，避免 token 被重放。
 *
 *          设计取舍（为什么不是更「聪明」的实现）：
 *
 *          1. token 用字符串而不是二进制数组：上行协议（HTTP/JSON/查询串）
 *             普遍以文本传输，内部保持 16 hex 文本可以避免每次进出都做
 *             hex 编解码；8 字节熵（2^64）对短生命周期配对足够。
 *          2. 容量满时不淘汰活跃槽：relay 里被淘汰的 token 可能正好是正在
 *             握手的客户端，静默失效比显式拒绝更难排查。容量满时先回收过期
 *             槽，仍满则 Pair 返回 nullopt，由调用方决定重试或报错。
 *          3. 过期是「惰性判定 + 主动回收」：Find/List 只判断不删除（因此
 *             可以是 const，且高频路径不扫描全表）；PurgeExpired 与容量
 *             压力下的 Pair 负责真正删除。这样 size() 可能短暂包含已过期
 *             但尚未回收的槽，上限仍是 max_slots，不会无限增长。
 *          4. 本模块不写任何日志。token 是 bearer secret，调用方也不得把
 *             Slot::token 或生成结果写进日志/异常信息；需要区分槽位时请用
 *             key + peer_id。
 *
 *          线程安全：所有公有实例方法用同一把 mutex 保护，可跨线程并发调用；
 *          GenerateToken/IsValidToken/IsValidKey 是无状态静态函数，天然安全。
 *          返回的 Slot 是值拷贝，调用方持有它的期间不受表内变化影响。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace go2cpp::bento {

/// @brief token 的随机字节数：8 字节 = 64 bit 熵，hex 文本 16 字符。
inline constexpr std::size_t kTokenBytes = 8;

/// @brief token 的 hex 文本长度。
inline constexpr std::size_t kTokenHexChars = kTokenBytes * 2;

/// @brief key 的最大字节数。防止异常调用方用超长 key 撑爆内存。
inline constexpr std::size_t kMaxKeyBytes = 256;

/// @brief relay 侧连接句柄。上层可放连接 id、session 序号或指针值。
using PeerId = std::uint64_t;

/// @brief 配对表配置。
struct TokenPairingConfig {
    /// 槽位上限；0 表示拒绝一切 Pair。满时不淘汰活跃槽，避免互踢。
    std::size_t max_slots = 4096;
    /// 槽位存活时间（毫秒）；0 表示永不过期。从 Pair 成功时起算，不续期。
    std::uint64_t ttl_ms = 300000;
};

/// @brief 一个已配对的槽位。
/// @note token 是 bearer secret，调用方不得把它写进日志。
struct Slot {
    std::string key;               ///< 业务侧共享标识。
    std::string token;             ///< 16 个小写 hex 字符。
    PeerId peer_id = 0;            ///< relay 侧连接句柄，配对表不解释其语义。
    std::uint64_t created_ms = 0;  ///< 配对成功时刻（steady_clock 毫秒）。
    std::uint64_t expires_ms = 0;  ///< 过期时刻；0 表示永不过期。

    /// @brief 是否已过期。expires_ms == 0 表示永不过期。
    bool IsExpired(std::uint64_t now_ms) const noexcept {
        return expires_ms != 0 && now_ms >= expires_ms;
    }
};

/// @brief (key, token) 1:N 配对表，线程安全。
class TokenPairing final {
   public:
    /// @brief 用给定配置构造。默认 4096 槽、5 分钟 TTL。
    explicit TokenPairing(TokenPairingConfig config = {});

    ~TokenPairing() = default;

    TokenPairing(const TokenPairing&) = delete;
    TokenPairing& operator=(const TokenPairing&) = delete;
    TokenPairing(TokenPairing&&) = delete;
    TokenPairing& operator=(TokenPairing&&) = delete;

    /// @brief 生成 8 字节随机 token，输出 16 个小写 hex 字符。
    /// @note 每次调用重新取熵（Linux 下 std::random_device 读 /dev/urandom），
    ///       不保证跨调用唯一；需要唯一性时由 Pair 的查重保证。
    /// @throws 熵源不可用时 std::random_device 会抛异常，按系统级故障处理。
    static std::string GenerateToken();

    /// @brief 校验 token 文本：必须恰好是 16 个小写 hex 字符。
    /// @note 有意拒绝大写：生成侧只产生小写，严格单一写法可以避免同一
    ///       token 出现两种「合法」文本，也避免大小写规范化带来的歧义。
    static bool IsValidToken(std::string_view token) noexcept;

    /// @brief 校验 key：非空且不超过 kMaxKeyBytes 字节（不限制字符集）。
    static bool IsValidKey(std::string_view key) noexcept;

    /// @brief 用调用方给定的 token 建立配对（token 可来自外部协议）。
    /// @param key 业务侧共享标识，非空且不超过 kMaxKeyBytes 字节。
    /// @param token 16 个小写 hex 字符；必须全局唯一（含尚未回收的过期槽）。
    /// @param peer_id relay 侧连接句柄，原样保存。
    /// @return 成功返回 Slot 副本；参数非法、token 已存在或容量满返回 nullopt。
    std::optional<Slot> Pair(std::string_view key, std::string_view token, PeerId peer_id);

    /// @brief 生成新 token 并配对（服务端签发场景）。
    /// @return 成功返回含新 token 的 Slot；key 非法或容量满返回 nullopt。
    /// @note 内部对生成结果做最多 2 次重试；64 bit 熵下 token 碰撞概率可忽略，
    ///       实际失败几乎只可能是容量满。
    std::optional<Slot> Issue(std::string_view key, PeerId peer_id);

    /// @brief 按 (key, token) 查询未过期槽位。
    /// @return 命中返回 Slot 副本；token 不存在、已过期或 key 不匹配返回 nullopt。
    /// @note 不消费 token：Find 之后仍需 Release。查询路径不做全表清理。
    std::optional<Slot> Find(std::string_view key, std::string_view token) const;

    /// @brief 按 token 释放槽位（消费 token）。
    /// @return 槽位存在且未过期返回 true；不存在或已过期返回 false。
    /// @note 已过期的 token 即使返回 false 也会被回收，不会继续占用容量。
    bool Release(std::string_view token);

    /// @brief 列出 key 下所有未过期槽位。
    /// @return 按 created_ms、token 升序排列的快照；key 非法或无槽返回空。
    std::vector<Slot> List(std::string_view key) const;

    /// @brief 主动清理所有过期槽位。
    /// @return 本次回收的槽位数。
    std::size_t PurgeExpired();

    /// @brief 当前占用槽位数（含已过期但尚未回收的槽）。
    std::size_t size() const;

    /// @brief 槽位上限。
    std::size_t capacity() const noexcept {
        return m_max_slots;
    }

    /// @brief TTL 毫秒数（0 表示永不过期）。
    std::uint64_t ttl_ms() const noexcept {
        return m_ttl_ms;
    }

    /// @brief 累计回收的过期槽位数（PurgeExpired、容量压力清理、Release）。
    std::uint64_t purged() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_purged;
    }

   private:
    /// @brief 删除所有 expires_ms <= now_ms 的槽；调用方必须已持锁。
    std::size_t PurgeExpiredLocked(std::uint64_t now_ms);

    /// @brief 只从 key 索引移除 token 引用（主索引由调用方删除）；须持锁。
    void RemoveKeyRefLocked(const std::string& key, const std::string& token);

    /// 同时保护三张表与计数器。可重入路径（*Locked）不再单独加锁。
    mutable std::mutex m_mutex;
    /// 主索引：token 全局唯一，因此可直接用 token 定位槽位。
    std::unordered_map<std::string, Slot> m_by_token;
    /// 二级索引：key -> 该 key 下的 token 集合，供 List 使用。
    std::unordered_map<std::string, std::unordered_set<std::string>> m_by_key;
    const std::size_t m_max_slots;
    const std::uint64_t m_ttl_ms;
    std::uint64_t m_purged = 0;
};

}  // namespace go2cpp::bento
