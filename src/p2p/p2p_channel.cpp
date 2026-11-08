/**
 * @file p2p_channel.cpp
 * @brief P2P 通道实现：候选收集、ICE 连通性检查、中继降级与数据面。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "go2cpp/p2p/p2p_channel.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "go2cpp/p2p/random.hpp"
#include "go2cpp/p2p/stun_client.hpp"
#include "go2cpp/p2p/stun_message.hpp"
#include "go2cpp/p2p/turn.hpp"

namespace go2cpp::p2p {
namespace {

using Clock = std::chrono::steady_clock;

/// 数据帧 magic：ASCII "G2PD"。
constexpr std::uint32_t kDataMagic = 0x47325044U;
constexpr std::uint8_t kDataVersion = 1;
constexpr std::size_t kDataHeaderSize = 12;
/// 连接阶段接收轮询的最长单次等待（毫秒）。
constexpr int kConnectPollMs = 20;
/// 已连接后接收轮询的最长单次等待（毫秒）。
constexpr int kRecvPollMs = 50;
/// 记录待匹配 STUN 检查事务的上限。
constexpr std::size_t kMaxPendingChecks = 32;

enum class FrameType : std::uint8_t {
    kData = 1,
    kKeepalive = 2,
    kKeepaliveAck = 3,
};

// 线上字节序固定大端。x86-64 是 little-endian：一次 unaligned load/store
// 加 bswap（2~3 条指令）比逐字节移位拼接（8~12 条）省一半以上。
std::uint16_t ReadU16(const std::uint8_t* data) noexcept {
    std::uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return __builtin_bswap16(value);
}

std::uint32_t ReadU32(const std::uint8_t* data) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return __builtin_bswap32(value);
}

void StoreU16(std::uint8_t* out, std::uint16_t value) noexcept {
    value = __builtin_bswap16(value);
    std::memcpy(out, &value, sizeof(value));
}

void StoreU32(std::uint8_t* out, std::uint32_t value) noexcept {
    value = __builtin_bswap32(value);
    std::memcpy(out, &value, sizeof(value));
}

/// @brief 帧写缓冲容量（12B 头 + 单包上限 1200B）。
inline constexpr std::size_t kFrameCapacity = kDataHeaderSize + kP2PMaxPayload;

/// @brief 把完整帧写进调用方提供的定长缓冲，返回帧字节数。
/// @details 旧实现用 `vector::resize`：容量足够时不分配，但对新增的
///          `length` 个元素做值初始化（每包一次 512B memset，约 10~20ns）。
///          改成写定长数组后每包零分配、零清零，只写真实载荷。
std::size_t BuildFrameInto(std::uint8_t* out, FrameType type, std::uint32_t sequence,
                           const void* payload, std::size_t length) noexcept {
    StoreU32(out, kDataMagic);
    out[4] = kDataVersion;
    out[5] = static_cast<std::uint8_t>(type);
    StoreU16(out + 6, static_cast<std::uint16_t>(length));
    StoreU32(out + 8, sequence);
    if (length > 0) {
        std::memcpy(out + kDataHeaderSize, payload, length);
    }
    return kDataHeaderSize + length;
}

/// @brief 连接状态快照：发布后不再修改，读路径只需一次 acquire load。
struct ConnectionSnapshot {
    P2PState state = P2PState::kIdle;
    Endpoint peer;
};

/// @brief steady_clock 纳秒时间戳（原子字段存 int64，避免 time_point 宽度歧义）。
std::int64_t NowNanos() noexcept {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

std::int64_t ToNanos(std::chrono::milliseconds value) noexcept {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(value).count());
}

/// @brief 统计计数器的无锁自增（relaxed：只要求最终一致）。
void Bump(std::atomic<std::uint64_t>& counter, std::uint64_t amount = 1) noexcept {
    counter.fetch_add(amount, std::memory_order_relaxed);
}

/// @brief 数据面热路径的**单写者**自增：relaxed load+store，无 `lock` 前缀。
///
/// @details 线程模型规定同一时刻只有一个 Send 线程和一个 Recv 线程，因此
///          `packets_sent`/`bytes_sent`（Send 写）与 `packets_received`/
///          `bytes_received`（Recv 写）都不存在并发写者。`fetch_add` 在这
///          种场景下仍然发 `lock xadd`（独占 cacheline，单核约 10 周期），
///          而 load+add+store 只是普通访存。`Stats()` 的并发读仍由 relaxed
///          原子保证单字段自洽。
///          **前提**：调用方遵守「Send/Recv 不在多线程同时调用」；违反时
///          会丢计数（旧实现不会）——控制面计数继续用 `Bump`。
template <typename T>
void BumpLocal(std::atomic<T>& counter, T amount = 1) noexcept {
    counter.store(static_cast<T>(counter.load(std::memory_order_relaxed) + amount),
                  std::memory_order_relaxed);
}

bool ParseFrame(const std::uint8_t* data, std::size_t length, FrameType* type,
                std::uint32_t* sequence, const std::uint8_t** payload,
                std::size_t* payload_length) {
    if (data == nullptr || length < kDataHeaderSize) {
        return false;
    }
    if (ReadU32(data) != kDataMagic || data[4] != kDataVersion) {
        return false;
    }
    const std::uint8_t raw_type = data[5];
    if (raw_type < static_cast<std::uint8_t>(FrameType::kData) ||
        raw_type > static_cast<std::uint8_t>(FrameType::kKeepaliveAck)) {
        return false;
    }
    const std::uint16_t declared = ReadU16(data + 6);
    if (kDataHeaderSize + declared != length) {
        return false;
    }
    *type = static_cast<FrameType>(raw_type);
    *sequence = ReadU32(data + 8);
    *payload = data + kDataHeaderSize;
    *payload_length = declared;
    return true;
}

bool LooksLikeStun(const std::uint8_t* data, std::size_t length) {
    return data != nullptr && length >= kStunHeaderSize && ReadU32(data + 4) == kStunMagicCookie;
}

/// @brief 源地址是否属于已交换的候选集合（ICE 候选对匹配）。
/// @details 连通性检查的 Request 不带认证信息；若不加来源过滤，任意第三方
///          只要向本端 UDP 端口发一个 Binding Request 就能把 peer 锁定到
///          自己的地址，之后数据面全部发给攻击者（连接劫持）。只接受来自
///          targets 的检查，等价于 RFC 8445 7.2.5.2.1 的候选对约束。
bool ContainsEndpoint(const std::vector<Endpoint>& targets, const Endpoint& endpoint) {
    return std::find(targets.begin(), targets.end(), endpoint) != targets.end();
}

/**
 * @brief 待匹配的 STUN 检查事务 ID（固定容量环形表）。
 * @details 旧实现是 `std::vector<std::array<...>>`：打洞期间每轮每个 target 都
 *          push_back，超过上限再 erase(begin())（O(n) 搬移），每次连接至少
 *          1 次堆分配 + 多次 memmove。环形表容量固定，Push/Contains 都是
 *          纯栈操作，且保持「保留最近 32 个」的语义。
 */
struct PendingChecks {
    void Push(const std::array<std::uint8_t, kStunTransactionIdSize>& id) noexcept {
        m_items[(m_head + m_count) % kMaxPendingChecks] = id;
        if (m_count < kMaxPendingChecks) {
            ++m_count;
        } else {
            m_head = (m_head + 1U) % kMaxPendingChecks;
        }
    }

    bool Contains(const std::uint8_t* id) const noexcept {
        for (std::size_t index = 0; index < m_count; ++index) {
            if (std::memcmp(m_items[(m_head + index) % kMaxPendingChecks].data(), id,
                            kStunTransactionIdSize) == 0) {
                return true;
            }
        }
        return false;
    }

private:
    std::array<std::array<std::uint8_t, kStunTransactionIdSize>, kMaxPendingChecks> m_items{};
    std::size_t m_head = 0;
    std::size_t m_count = 0;
};

std::chrono::milliseconds Remaining(Clock::time_point deadline) {
    const Clock::time_point now = Clock::now();
    if (now >= deadline) {
        return std::chrono::milliseconds(0);
    }
    return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
                    std::chrono::milliseconds(1));
}

/// 生成连接会话 ID：热切换时保持不变，只有新建 P2PChannel 才会变。
/// @details 低 16 位是进程内自增计数（保证同一纳秒内创建的两个通道不撞），
///          高位来自 steady_clock 纳秒。不用于安全用途，只作「同一条流」的标识。
std::uint64_t NextSessionId() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t tick = static_cast<std::uint64_t>(NowNanos());
    const std::uint64_t index = counter.fetch_add(1, std::memory_order_relaxed);
    return (tick << 16U) ^ index ^ 0x9E3779B97F4A7C15ULL;
}

std::chrono::milliseconds ClampPoll(std::chrono::milliseconds remaining, int limit_ms) {
    if (remaining.count() > limit_ms) {
        return std::chrono::milliseconds(limit_ms);
    }
    return std::max(remaining, std::chrono::milliseconds(1));
}

}  // namespace

const char* P2PStateName(P2PState state) noexcept {
    switch (state) {
        case P2PState::kIdle:
            return "idle";
        case P2PState::kGathering:
            return "gathering";
        case P2PState::kConnecting:
            return "connecting";
        case P2PState::kConnected:
            return "connected";
        case P2PState::kRelayed:
            return "relayed";
        case P2PState::kFailed:
            return "failed";
        case P2PState::kClosed:
            return "closed";
    }
    return "unknown";
}

class P2PChannel::Impl {
public:
    explicit Impl(P2PConfig config)
        : m_config(std::move(config)), m_ice(m_config.ice_switchover) {
        // 先发布初始快照：Send/Recv/state() 可在任何时刻无锁读取。
        PublishState(P2PState::kIdle, Endpoint());
        if (m_config.transport) {
            m_transport = m_config.transport;
            return;
        }
        auto socket = std::make_unique<UdpTransport>(BuildUdpConfig());
        std::string error;
        if (!socket->Start(&error)) {
            SetError("创建本地 UDP socket 失败: " + error);
            return;
        }
        m_transport = std::shared_ptr<DatagramTransport>(std::move(socket));
    }

    ~Impl() { Close(); }

    std::vector<Candidate> GatherCandidates() {
        if (!m_transport || m_closed.load(std::memory_order_acquire)) {
            return {};
        }
        PublishStateIfOpen(P2PState::kGathering, Endpoint());
        std::vector<Candidate> result;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed.load(std::memory_order_relaxed)) {
                return {};
            }
            m_local_candidates.clear();
            m_error.clear();
            const Endpoint local = m_transport->LocalEndpoint();

            if (m_config.gather_host_candidates) {
                std::vector<Endpoint> addresses;
                if (!local.valid() || !local.IsUnspecified()) {
                    if (local.valid()) {
                        addresses.push_back(local);
                    }
                } else {
                    addresses = EnumerateLocalAddresses(local.port(),
                                                        m_config.gather_loopback_candidates);
                }
                for (const Endpoint& address : addresses) {
                    if (!address.valid()) {
                        continue;
                    }
                    if (!m_config.gather_loopback_candidates && address.IsLoopback()) {
                        continue;
                    }
                    Candidate candidate;
                    candidate.type = CandidateType::kHost;
                    candidate.endpoint = address;
                    candidate.base = address;
                    m_local_candidates.push_back(candidate);
                    if (m_local_candidates.size() >= m_config.max_local_candidates) {
                        break;
                    }
                }
            }

            for (const Endpoint& server : m_config.stun_servers) {
                if (!server.valid()) {
                    continue;
                }
                StunClient client(m_transport.get(), server);
                std::string error;
                const auto mapped = client.Binding(m_config.gather_timeout, &error);
                if (!mapped.has_value()) {
                    m_error = "STUN(" + server.ToString() + ") 失败: " + error;
                    continue;
                }
                Candidate candidate;
                candidate.type = CandidateType::kSrflx;
                candidate.endpoint = *mapped;
                candidate.base = local.valid() && !local.IsUnspecified() ? local : *mapped;
                m_local_candidates.push_back(candidate);
                if (m_local_candidates.size() >= m_config.max_local_candidates) {
                    break;
                }
            }

            if (m_config.enable_relay && m_config.turn_server.valid()) {
                RelayClient* relay = EnsureRelay();
                std::string error;
                if (relay != nullptr && relay->Allocate(&error)) {
                    Candidate candidate;
                    candidate.type = CandidateType::kRelay;
                    candidate.endpoint = relay->relayed_endpoint();
                    candidate.base = m_config.turn_server;
                    m_local_candidates.push_back(candidate);
                } else {
                    m_error = "TURN 分配失败: " + error;
                }
            }

            for (std::size_t index = 0; index < m_local_candidates.size(); ++index) {
                FinalizeCandidate(
                    &m_local_candidates[index],
                    static_cast<std::uint32_t>(65535U - std::min<std::size_t>(index, 255U)));
            }
            SortCandidatesByPriority(&m_local_candidates);
            result = m_local_candidates;
        }
        PublishStateIfOpen(P2PState::kIdle, Endpoint());
        return result;
    }

    void AddRemoteCandidate(const Candidate& candidate) {
        if (!candidate.endpoint.valid()) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const Candidate& existing : m_remote_candidates) {
            if (existing.endpoint == candidate.endpoint && existing.type == candidate.type) {
                return;
            }
        }
        Candidate normalized = candidate;
        FinalizeCandidate(&normalized);
        m_remote_candidates.push_back(normalized);
        SortCandidatesByPriority(&m_remote_candidates);
    }

    /**
     * @brief ICE 式选路：交换候选 → 直连优先 → 预算内中继降级，直到连上或超时。
     *
     * @param timeout [in] 总预算毫秒；<=0 时改用配置的 connect_timeout。
     *
     * @return
     * - true：已进入 kConnected/kRelayed——调用方**必须**用 state() 区分直连还是中继；
     * - false：已置失败态（kFailed）或已关闭——调用方**必须**读 LastError，不要立即重试同一批候选。
     *
     * ## 定位
     * P2P 通道唯一的连接决策点。直连与中继的预算分配（70%/30%）写错会让弱网永远退化到中继，
     * 或在有中继可用时白白超时；候选未排序会优先尝试低成功率地址。
     *
     * ## 步骤
     * step1. 校验传输，归一化 timeout；
     * step2. 持锁检查当前状态：已连直接成功，已关直接失败，清空错误；
     * step3. 发布 kConnecting 快照；
     * step4. 本地候选为空时现场采集；
     * step5. 配置了信令时交换候选（用总预算的一半作为上限）；
     * step6. 按类型拆分直连/中继目标并排序；
     * step7. 直连优先，有中继时只给直连 70% 预算；
     * step8. 剩余预算尝试中继，全失败则置失败并返回 false。
     *
     * ## 并发/失败语义
     * - 调用约束：同一通道不可并发 Connect；状态发布用原子快照，读路径无锁。
     * - 原子性：状态迁移是「先快照后工作」，中途读到的 state 可能仍是 kConnecting。
     * - 失败语义：不抛异常；失败写入 m_error 并发布 kFailed。
     * - 不变量：返回 true 后 state 一定是 kConnected 或 kRelayed。
     */
    bool Connect(std::chrono::milliseconds timeout) {
        if (!m_transport) {
            SetError("传输未初始化");
            return false;
        }
        if (timeout.count() <= 0) {
            timeout = m_config.connect_timeout;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const P2PState current = CurrentSnapshot().state;
            if (current == P2PState::kConnected || current == P2PState::kRelayed) {
                return true;
            }
            if (current == P2PState::kClosed) {
                return false;
            }
            m_error.clear();
        }
        PublishStateIfOpen(P2PState::kConnecting, Endpoint());
        const Clock::time_point started = Clock::now();
        const Clock::time_point deadline = started + timeout;

        if (m_local_candidates.empty()) {
            GatherCandidates();
        }
        // 配置了信令且启用自动交换时，总是交换候选：调用方可能已经手动塞入
        // 了额外候选（例如测试里的黑洞候选），不能因为「远端列表非空」就
        // 跳过交换，否则永远拿不到对端的真实候选。
        if (m_config.signaling != nullptr && m_config.auto_exchange_candidates) {
            const auto exchange_deadline =
                std::min(deadline, started + std::chrono::milliseconds(timeout.count() / 2));
            if (!ExchangeCandidates(exchange_deadline)) {
                SetFailed("信令交换候选失败: " + LastError());
                return false;
            }
        }
        const std::vector<Candidate> remote = RemoteCandidateSnapshot();
        if (remote.empty()) {
            SetFailed("没有远端候选，无法连接");
            return false;
        }

        std::vector<Endpoint> direct_targets;
        std::vector<Endpoint> relay_targets;
        for (const Candidate& candidate : remote) {
            if (candidate.type == CandidateType::kRelay) {
                if (m_config.enable_relay) {
                    relay_targets.push_back(candidate.endpoint);
                }
            } else {
                direct_targets.push_back(candidate.endpoint);
            }
        }
        SortEndpointsByRemotePriority(&direct_targets, remote);
        // 注册「本地候选 × 远端直连候选」候选对，供连接建立后的热切换使用。
        RegisterCandidatePairs(direct_targets, remote);

        // 没有可用的中继候选时，把整个预算都给直连；否则直连先用 70%，
        // 剩下 30% 留给中继降级。
        const bool can_use_relay = m_config.enable_relay && !relay_targets.empty();
        const auto direct_deadline =
            can_use_relay
                ? std::min(deadline,
                           started + std::chrono::milliseconds(timeout.count() * 7 / 10))
                : deadline;
        if (!direct_targets.empty() &&
            TryDirect(direct_targets, direct_deadline, started)) {
            return true;
        }
        if (!relay_targets.empty() && m_config.enable_relay) {
            if (TryRelay(relay_targets, deadline, started)) {
                return true;
            }
        }
        SetFailed("连接超时：直连与中继均未成功（远端候选 " +
                  std::to_string(remote.size()) + " 个）");
        return false;
    }

    bool Send(const void* data, std::size_t length) {
        if (data == nullptr || length == 0) {
            SetError("Send: 数据为空");
            return false;
        }
        if (length > kP2PMaxPayload) {
            SetError("Send: 载荷超过 " + std::to_string(kP2PMaxPayload) + " 字节");
            return false;
        }
        const ConnectionSnapshot& snapshot = CurrentSnapshot();
        const bool relayed = snapshot.state == P2PState::kRelayed;
        if (!relayed && snapshot.state != P2PState::kConnected) {
            return false;
        }
        // 快照对象不可变且存活到 Impl 析构：直接引用，省掉每包 136 字节
        // （sockaddr_storage + socklen）的结构体拷贝。
        const Endpoint& peer = snapshot.peer;
        if (!peer.valid()) {
            return false;
        }
        // 单发送线程：序号用 load+store 即可（fetch_add 的原子性在此多余）。
        const std::uint32_t sequence =
            m_send_sequence.load(std::memory_order_relaxed) + 1U;
        m_send_sequence.store(sequence, std::memory_order_relaxed);
        const std::size_t wire_bytes =
            BuildFrameInto(m_tx_frame.data(), FrameType::kData, sequence, data, length);
        const bool sent = relayed ? SendViaRelay(peer, m_tx_frame.data(), wire_bytes)
                                  : SendDirect(peer, m_tx_frame.data(), wire_bytes);
        if (sent) {
            BumpLocal(m_stats.packets_sent);
            BumpLocal(m_stats.bytes_sent, length);
            if (relayed) {
                BumpLocal(m_stats.relayed_packets_sent);
            }
        }
        return sent;
    }

    std::size_t Recv(void* buffer, std::size_t capacity, std::chrono::milliseconds timeout) {
        if (buffer == nullptr && capacity > 0) {
            return 0;
        }
        if (!IsConnected()) {
            return 0;
        }
        // deadline 惰性化：数据包快路径（收到合法数据帧）完全不取时钟。
        // steady_clock::now() 走 vDSO 也要 ~15-20ns，是 Recv 每包最大的
        // 纯用户态开销；只有空轮询/控制帧/无效包才需要真正的截止时间。
        // 首次 poll 最多消耗 min(timeout, kRecvPollMs)，因此实际超时上界
        // 是 timeout + 一个 poll 窗口（旧实现严格为 timeout）。
        std::chrono::milliseconds left = timeout;
        Clock::time_point deadline{};
        bool have_deadline = false;
        const auto ensure_deadline = [&]() {
            if (!have_deadline) {
                deadline = Clock::now() + left;
                have_deadline = true;
            }
        };
        for (;;) {
            // 每轮只取一次时钟；当 keepalive 与 peer_timeout 都关闭时
            // （纯数据面压测）完全跳过 steady_clock::now() 的 vDSO 调用。
            std::int64_t now_ns = 0;
            if (m_need_clock) {
                now_ns = NowNanos();
            }
            if (m_track_activity) {
                MaybeSendKeepalive(now_ns);
                if (PeerTimedOut(now_ns)) {
                    SetFailed("对端静默超过 peer_timeout");
                    return 0;
                }
            }
            // 候选对探测/切换只在 >=2 个直连对时存在（单对零开销）。
            if (m_ice_active.load(std::memory_order_relaxed)) {
                MaybeSendIceProbes(now_ns);
                EvaluateSwitchover(now_ns);
            }
            if (left.count() <= 0) {
                return 0;
            }
            Endpoint source;
            const bool relayed = CurrentSnapshot().state == P2PState::kRelayed;
            const int received = relayed ? RecvViaRelay(&source, ClampPoll(left, kRecvPollMs))
                                         : RecvDirect(&source, ClampPoll(left, kRecvPollMs));
            if (received == 0) {
                ensure_deadline();
                left = Remaining(deadline);
                continue;
            }
            if (received < 0) {
                SetError("Recv: 接收失败");
                return 0;
            }
            if (m_track_activity) {
                TouchPeer(now_ns);
            }
            const auto* bytes = m_recv_buffer.data();
            const std::size_t received_bytes = static_cast<std::size_t>(received);
            if (LooksLikeStun(bytes, received_bytes)) {
                HandleStun(bytes, received_bytes, source, relayed, now_ns);
                ensure_deadline();
                left = Remaining(deadline);
                continue;
            }
            FrameType type = FrameType::kData;
            std::uint32_t sequence = 0;
            const std::uint8_t* payload = nullptr;
            std::size_t payload_length = 0;
            if (!ParseFrame(bytes, received_bytes, &type, &sequence, &payload, &payload_length)) {
                BumpLocal(m_stats.dropped_packets);
                ensure_deadline();
                left = Remaining(deadline);
                continue;
            }
            if (type == FrameType::kKeepalive) {
                // keepalive ACK 用接收侧专用 scratch：与另一线程的 Send() 互不干扰。
                const std::size_t ack_bytes = BuildFrameInto(
                    m_rx_frame.data(), FrameType::kKeepaliveAck, sequence, nullptr, 0);
                (void)(relayed ? SendViaRelay(source, m_rx_frame.data(), ack_bytes)
                               : SendDirect(source, m_rx_frame.data(), ack_bytes));
                Bump(m_stats.keepalives_received);
                ensure_deadline();
                left = Remaining(deadline);
                continue;
            }
            if (type == FrameType::kKeepaliveAck) {
                UpdateRtt();
                ensure_deadline();
                left = Remaining(deadline);
                continue;
            }
            if (!AcceptSource(source, now_ns)) {
                BumpLocal(m_stats.dropped_packets);
                ensure_deadline();
                left = Remaining(deadline);
                continue;
            }
            const std::size_t copied = std::min(payload_length, capacity);
            if (copied > 0) {
                std::memcpy(buffer, payload, copied);
            }
            BumpLocal(m_stats.packets_received);
            BumpLocal(m_stats.bytes_received, copied);
            if (relayed) {
                BumpLocal(m_stats.relayed_packets_received);
            }
            UpdateSequenceGap(sequence);
            if (m_track_activity) {
                m_last_receive_ns.store(now_ns, std::memory_order_relaxed);
            }
            return copied;
        }
    }

    bool IsRelayed() const {
        return CurrentSnapshot().state == P2PState::kRelayed;
    }

    bool IsConnected() const {
        const P2PState current = CurrentSnapshot().state;
        return current == P2PState::kConnected || current == P2PState::kRelayed;
    }

    P2PState state() const {
        return CurrentSnapshot().state;
    }

    Endpoint LocalEndpoint() const {
        return m_transport ? m_transport->LocalEndpoint() : Endpoint();
    }

    Endpoint PeerEndpoint() const {
        return CurrentSnapshot().peer;
    }

    std::vector<Candidate> LocalCandidates() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_local_candidates;
    }

    std::vector<Candidate> RemoteCandidates() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_remote_candidates;
    }

    P2PStats Stats() const {
        // 逐字段 relaxed 读：统计快照不要求事务性，只要最终一致。
        P2PStats out;
        out.packets_sent = m_stats.packets_sent.load(std::memory_order_relaxed);
        out.packets_received = m_stats.packets_received.load(std::memory_order_relaxed);
        out.bytes_sent = m_stats.bytes_sent.load(std::memory_order_relaxed);
        out.bytes_received = m_stats.bytes_received.load(std::memory_order_relaxed);
        out.keepalives_sent = m_stats.keepalives_sent.load(std::memory_order_relaxed);
        out.keepalives_received = m_stats.keepalives_received.load(std::memory_order_relaxed);
        out.connectivity_checks_sent =
            m_stats.connectivity_checks_sent.load(std::memory_order_relaxed);
        out.connectivity_checks_received =
            m_stats.connectivity_checks_received.load(std::memory_order_relaxed);
        out.dropped_packets = m_stats.dropped_packets.load(std::memory_order_relaxed);
        out.relayed_packets_sent =
            m_stats.relayed_packets_sent.load(std::memory_order_relaxed);
        out.relayed_packets_received =
            m_stats.relayed_packets_received.load(std::memory_order_relaxed);
        out.sequence_gaps = m_stats.sequence_gaps.load(std::memory_order_relaxed);
        out.rtt_ms = m_stats.rtt_ms.load(std::memory_order_relaxed);
        out.rtt_avg_ms = m_stats.rtt_avg_ms.load(std::memory_order_relaxed);
        out.ice_probes_sent = m_stats.ice_probes_sent.load(std::memory_order_relaxed);
        out.ice_pair_switches =
            m_stats.ice_pair_switches.load(std::memory_order_relaxed);
        return out;
    }

    std::string LastError() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_error;
    }

    std::uint64_t SessionId() const {
        return m_session_id.load(std::memory_order_relaxed);
    }

    Endpoint SelectedPairRemote() const {
        const ConnectionSnapshot& snapshot = CurrentSnapshot();
        if (snapshot.state != P2PState::kConnected) {
            return Endpoint();
        }
        return snapshot.peer;
    }

    std::vector<IcePairSnapshot> CandidatePairs() const {
        return m_ice.Snapshot(NowNanos());
    }

    bool EvaluateIceSwitchover() {
        if (!m_ice_active.load(std::memory_order_relaxed)) {
            return false;
        }
        return EvaluateSwitchover(NowNanos());
    }

    std::uint64_t SwitchGeneration() const {
        return m_ice_generation.load(std::memory_order_relaxed);
    }

    void Close() {
        if (m_closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        PublishState(P2PState::kClosed, Endpoint());
        RelayClient* relay = m_relay.load(std::memory_order_acquire);
        if (relay != nullptr) {
            // 不销毁 relay 对象：Close 可能与 Send/Recv 并发，销毁会让在途
            // 调用访问已释放内存。对象保留到 Impl 析构，Release 只放弃 allocation。
            relay->Release();
        }
    }

private:
    /// @brief 懒创建 RelayClient，并把裸指针原子发布给收发热路径。
    /// @details 只允许 Connect/GatherCandidates 线程调用；对象由
    ///          m_relay_owner 持有到 Impl 析构，因此读路径无需加锁/计数。
    RelayClient* EnsureRelay() {
        RelayClient* existing = m_relay.load(std::memory_order_acquire);
        if (existing != nullptr) {
            return existing;
        }
        RelayClientConfig relay_config;
        relay_config.server = m_config.turn_server;
        relay_config.request_timeout = m_config.gather_timeout;
        m_relay_owner = std::make_unique<RelayClient>(m_transport.get(), relay_config);
        m_relay.store(m_relay_owner.get(), std::memory_order_release);
        return m_relay_owner.get();
    }

    UdpTransportConfig BuildUdpConfig() const {
        UdpTransportConfig udp;
        udp.bind_endpoint = m_config.bind_endpoint;
        udp.receive_buffer_bytes = m_config.receive_buffer_bytes;
        udp.send_buffer_bytes = m_config.receive_buffer_bytes;
        return udp;
    }

    std::vector<Candidate> RemoteCandidateSnapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_remote_candidates;
    }

    /// @brief 持锁发布一份不可变连接快照（旧快照保留，读者可安全持有引用）。
    void PublishLocked(P2PState state, const Endpoint& peer) {
        auto snapshot = std::make_unique<ConnectionSnapshot>();
        snapshot->state = state;
        snapshot->peer = peer;
        const ConnectionSnapshot* raw = snapshot.get();
        m_snapshot_history.push_back(std::move(snapshot));
        m_snapshot.store(raw, std::memory_order_release);
    }

    void PublishState(P2PState state, const Endpoint& peer) {
        std::lock_guard<std::mutex> lock(m_mutex);
        PublishLocked(state, peer);
    }

    /// @brief 只在未 Close 时发布：避免 Gather/Connect 的中间态覆盖 kClosed。
    void PublishStateIfOpen(P2PState state, const Endpoint& peer) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed.load(std::memory_order_relaxed)) {
            return;
        }
        PublishLocked(state, peer);
    }

    const ConnectionSnapshot& CurrentSnapshot() const noexcept {
        return *m_snapshot.load(std::memory_order_acquire);
    }

    void SetError(const std::string& error) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = error;
    }

    void SetFailed(const std::string& error) {
        const P2PState current = CurrentSnapshot().state;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_error = error;
        }
        if (current != P2PState::kClosed) {
            PublishStateIfOpen(P2PState::kFailed, CurrentSnapshot().peer);
        }
    }

    /// @brief 组合候选对优先级（RFC 8445 §6.1.2.3）。
    /// @details `2^32×MIN + 2×MAX + (G>D)`：本地优先级为 G、远端为 D。两边对等
    ///          时用同一公式，保证「双方算出的相对顺序一致」，否则会互相把对方
    ///          往不同路径上拽。
    static std::uint64_t PairPriority(std::uint32_t local_priority,
                                      std::uint32_t remote_priority) noexcept {
        const std::uint64_t high = std::max<std::uint64_t>(local_priority, remote_priority);
        const std::uint64_t low = std::min<std::uint64_t>(local_priority, remote_priority);
        return (low << 32U) + (high << 1U) + (local_priority > remote_priority ? 1U : 0U);
    }

    /// @brief 查某个远端地址在远端候选列表里的优先级；未知返回 0。
    std::uint32_t RemotePriorityOf(const std::vector<Candidate>& remote,
                                   const Endpoint& endpoint) const {
        for (const Candidate& candidate : remote) {
            if (candidate.endpoint == endpoint) {
                return candidate.priority;
            }
        }
        return 0;
    }

    /**
     * @brief 注册「本地候选 × 远端直连候选」候选对，并决定热切换是否参与收发路径。
     *
     * @param direct_targets [in] 远端非 relay 候选地址（调用方已按优先级排序）。
     * @param remote         [in] 远端候选全集，用来查组合优先级。
     *
     * ## 定位
     * 候选对是热切换的唯一数据来源：漏注册会让最优路径永远没资格被选中，
     * 多注册只会多几个探测包。`m_ice_active` 只在这里置位，一旦为 true 就
     * 固定不变，收发热路径用一次原子读做门控。
     *
     * ## 步骤
     * step1. 清空上一轮候选对（重复 Connect 不残留）；
     * step2. 取本地候选快照；没有本地候选（纯中继/未 gather）直接返回；
     * step3. 每个远端直连候选注册一条（本地候选取优先级最高的那条，因为
     *    同一 socket 下「不同本地候选 + 同一远端」是同一物理路径）；
     *    relay 本地候选跳过：它的 local 是 TURN 映射地址，本端 socket 没绑定；
     * step4. 只有 >= 2 对才启用：单对没有可切换目标，保持旧路径零额外开销。
     *
     * ## 并发/失败语义
     * - 调用约束：只在 Connect 线程调用；与 GatherCandidates 不并发。
     * - 原子性：`m_ice_active` 是 release store，读侧 relaxed 即可（只需要
     *   「看到 true 时候选对已经注册完」这一个方向）。
     * - 失败语义：`AddPair` 失败（重复/超上限）被忽略；注册数为 1 时清空并
     *   关闭功能，不留半套状态。
     */
    void RegisterCandidatePairs(const std::vector<Endpoint>& direct_targets,
                                const std::vector<Candidate>& remote) {
        // step1: 幂等：每次 Connect 都从零开始注册。
        m_ice.Clear();
        m_ice_active.store(false, std::memory_order_relaxed);
        if (!m_config.ice_switchover.enabled) {
            return;
        }
        // step2: 本地候选快照（持锁拷贝，之后不再碰 m_local_candidates）。
        std::vector<Candidate> locals;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            locals = m_local_candidates;
        }
        if (locals.empty() || direct_targets.empty()) {
            return;
        }
        // step3: 组合注册。**按远端去重**：本模块所有本地候选共享同一个
        // socket（`SendTo` 的源地址由内核路由决定，候选里的本地端点只是标称），
        // 因此「同一远端 + 不同本地候选」在物理上是同一条路径。若逐对注册，
        // 切换会选中 remote 相同、仅 local 不同的那条——`peer` 根本没变，
        // 表现为「代数涨了但地址没变」的假切换。这里每个远端只保留本地
        // 优先级最高的那条，与「候选对 = 一条实际路径」的语义对齐。
        for (const Endpoint& target : direct_targets) {
            const Candidate* best_local = nullptr;
            for (const Candidate& local : locals) {
                if (local.type == CandidateType::kRelay) {
                    continue;
                }
                if (best_local == nullptr || local.priority > best_local->priority) {
                    best_local = &local;
                }
            }
            if (best_local == nullptr) {
                continue;
            }
            const std::uint32_t remote_priority = RemotePriorityOf(remote, target);
            (void)m_ice.AddPair(best_local->endpoint, target,
                                PairPriority(best_local->priority, remote_priority));
        }
        // step4: 单对场景关闭功能，行为与未引入热切换时逐字节一致。
        const bool active = m_ice.size() >= 2U;
        if (!active) {
            m_ice.Clear();
            return;
        }
        m_ice_active.store(true, std::memory_order_release);
    }

    /**
     * @brief 周期性向候选对发 STUN Binding Request，刷新 writable/receiving。
     *
     * @param now_ns [in] 当前 steady_clock 纳秒时间戳（调用方已取时钟）。
     *
     * ## 定位
     * 「当前对已死」「备用对可用」两个判断都依赖持续探测；不探测就只能等上层
     * 发现断流再重连，等于没有迁移能力。探测包同时兼任 NAT 映射保活。
     *
     * ## 步骤
     * step1. 门控在调用方（`m_ice_active`，候选对 < 2 时为 false）：单对场景
     *    连这个函数都不会被调用，由既有 keepalive 负责保活；
     * step2. 由 controller 决定这一轮探测谁（失败对按 backoff 降频）；
     * step3. 逐目标发检查；只有真正发出去了才记 `OnCheckSent`，发送失败下一轮重试。
     */
    void MaybeSendIceProbes(std::int64_t now_ns) {
        const std::vector<Endpoint> targets = m_ice.DueProbeTargets(now_ns);
        for (const Endpoint& target : targets) {
            std::array<std::uint8_t, 64> wire{};
            std::array<std::uint8_t, kStunTransactionIdSize> transaction_id{};
            const std::size_t wire_size =
                BuildBindingRequestWire(wire.data(), wire.size(), &transaction_id);
            if (wire_size == 0 || !SendDirect(target, wire.data(), wire_size)) {
                continue;
            }
            m_ice.OnCheckSent(target, now_ns);
            Bump(m_stats.ice_probes_sent);
        }
    }

    /**
     * @brief 评估并执行候选对热切换（只换 peer，不重建连接）。
     *
     * @param now_ns [in] 当前 steady_clock 纳秒时间戳。
     *
     * @return
     * - true：选定对已变且 `peer` 已更新——调用方**必须**重新读 `PeerEndpoint()`，
     *   不要缓存旧快照里的地址；
     * - false：没换（含冷却期阻塞）——调用方**不要**做任何补救动作。
     *
     * ## 定位
     * 「网络切换不重连」的唯一提交点。这里若顺手改了 `P2PState` 或重置序列号，
     * 上层会把迁移误判成断线重连（mux 重建、WS 重握手），功能等于白做。
     *
     * ## 步骤
     * step1. 只有直连态参与（中继/失败/关闭态一律不评估）；
     * step2. 交给 controller 判定并拿事件（防抖已在 controller 内完成）；
     * step3. 真正切换时只发布「同状态 + 新 peer」的快照；
     * step4. 收包/保活计时重置到 now，新路径从零开始计；
     * step5. 代数与统计更新，序列号**不**重置（否则对端看到假缺口）。
     *
     * ## 并发/失败语义
     * - 调用约束：Recv 线程或显式 `EvaluateIceSwitchover()` 调用；与 Send 并发
     *   安全（peer 通过不可变原子快照发布）。
     * - 原子性：controller 先改选定对，这里随后发布 peer；两步之间 Send 仍用旧
     *   peer，最多丢一两个包——这正是正常迁移语义。
     * - 不变量：返回 true 后 `PeerEndpoint() == event.to` 且 `state()` 仍是 kConnected。
     */
    bool EvaluateSwitchover(std::int64_t now_ns) {
        // step1: 只对直连态生效。
        if (CurrentSnapshot().state != P2PState::kConnected) {
            return false;
        }
        // step2: controller 的决策已含 receiving 宽限期与切换冷却期。
        const IceSwitchEvent event = m_ice.Evaluate(now_ns);
        if (event.decision != IceSwitchDecision::kSwitch &&
            event.decision != IceSwitchDecision::kSwitchBack) {
            return false;
        }
        if (!event.to.valid()) {
            return false;
        }
        // step3: 状态保持 kConnected —— 这就是「上层流不变」的实现点。
        PublishStateIfOpen(P2PState::kConnected, event.to);
        // step4: 新路径的计时从 now 起算。
        m_last_receive_ns.store(now_ns, std::memory_order_relaxed);
        m_last_keepalive_ns.store(now_ns, std::memory_order_relaxed);
        m_has_pending_keepalive.store(false, std::memory_order_relaxed);
        // step5: 代数与统计（不碰 m_last_data_sequence）。
        m_ice_generation.store(event.generation, std::memory_order_relaxed);
        Bump(m_stats.ice_pair_switches);
        return true;
    }

    /**
     * @brief 业务包来源校验：只接受「选定对」或「已确认可写的备用对」。
     *
     * @param source [in] 收到的数据报源地址。
     * @param now_ns [in] 当前时间戳；<=0 表示调用方没取时钟（冷路径自己取）。
     *
     * @return true 允许递交给上层；false 丢弃（由调用方计入 dropped_packets）。
     *
     * ## 定位
     * 防注入的第二道闸：候选对来自信令交换，第三方地址不在集合里。放宽成
     * 「任意已注册对」会让从未验证过的地址注入业务数据；收紧成「只有选定对」
     * 则对端切换后本端会持续丢包（双方收敛慢一个探测周期）。
     *
     * ## 步骤
     * step1. 命中当前选定 peer 直接通过（热路径：无锁、无时钟）；
     * step2. 热切换未启用时一律拒绝（与旧行为一致）；
     * step3. 备用对只认 writable，并顺手刷新它的 receiving 时刻。
     *
     * ## 并发/失败语义
     * - 调用约束：Recv 线程；与 Send 并发安全。
     * - 不变量：不会把任何未注册/不可写的源判为合法。
     */
    bool AcceptSource(const Endpoint& source, std::int64_t now_ns) {
        // step1: 快路径。
        if (source == CurrentSnapshot().peer) {
            return true;
        }
        // step2: 功能关闭时行为与旧版本一致（只有 peer）。
        if (!m_ice_active.load(std::memory_order_relaxed)) {
            return false;
        }
        // step3: 备用对必须已通过检查证明可写。
        if (!m_ice.IsWritable(source)) {
            return false;
        }
        const std::int64_t stamp = now_ns > 0 ? now_ns : NowNanos();
        m_ice.OnPacketReceived(source, stamp);
        return true;
    }

    void TouchPeer(std::int64_t now_ns) noexcept {
        m_last_receive_ns.store(now_ns, std::memory_order_relaxed);
    }

    bool PeerTimedOut(std::int64_t now_ns) const noexcept {
        const P2PState current = CurrentSnapshot().state;
        if (current != P2PState::kConnected && current != P2PState::kRelayed) {
            return false;
        }
        if (m_config.peer_timeout.count() <= 0) {
            return false;
        }
        return now_ns - m_last_receive_ns.load(std::memory_order_relaxed) >
               ToNanos(m_config.peer_timeout);
    }

    void UpdateSequenceGap(std::uint32_t sequence) noexcept {
        // 序号从 1 开始，0 即「尚未收到数据包」的哨兵：省掉一个独立的
        // has 标志（旧实现每包都多一次 atomic load）。首包的 gap 恒为 0
        // （sequence=1 → 1-0-1=0），与旧语义一致。
        const std::uint32_t last = m_last_data_sequence.load(std::memory_order_relaxed);
        if (sequence > last) {
            const std::uint32_t gap = sequence - last - 1U;
            if (gap > 0 && gap < 100000U) {
                BumpLocal(m_stats.sequence_gaps, gap);
            }
            m_last_data_sequence.store(sequence, std::memory_order_relaxed);
        }
    }

    void UpdateRtt() {
        if (!m_has_pending_keepalive.exchange(false, std::memory_order_relaxed)) {
            return;
        }
        const std::int64_t now_ns = NowNanos();
        const double rtt = static_cast<double>(
                               now_ns - m_last_keepalive_ns.load(std::memory_order_relaxed)) /
                           1.0e6;
        m_stats.rtt_ms.store(rtt, std::memory_order_relaxed);
        // m_rtt_samples / m_rtt_sum 只由唯一的 Recv 线程读写。
        m_rtt_samples += 1;
        m_rtt_sum += rtt;
        m_stats.rtt_avg_ms.store(m_rtt_sum / static_cast<double>(m_rtt_samples),
                                 std::memory_order_relaxed);
    }

    void MaybeSendKeepalive(std::int64_t now_ns) {
        if (m_config.keepalive_interval.count() <= 0) {
            return;
        }
        const ConnectionSnapshot& snapshot = CurrentSnapshot();
        if (snapshot.state != P2PState::kConnected &&
            snapshot.state != P2PState::kRelayed) {
            return;
        }
        if (now_ns - m_last_keepalive_ns.load(std::memory_order_relaxed) <
            ToNanos(m_config.keepalive_interval)) {
            return;
        }
        m_last_keepalive_ns.store(now_ns, std::memory_order_relaxed);
        m_has_pending_keepalive.store(true, std::memory_order_relaxed);
        const Endpoint peer = snapshot.peer;
        if (!peer.valid()) {
            return;
        }
        const std::uint32_t sequence = ++m_keepalive_sequence;
        const std::size_t wire_bytes =
            BuildFrameInto(m_rx_frame.data(), FrameType::kKeepalive, sequence, nullptr, 0);
        const bool relayed = snapshot.state == P2PState::kRelayed;
        const bool sent =
            relayed ? SendViaRelay(peer, m_rx_frame.data(), wire_bytes)
                    : SendDirect(peer, m_rx_frame.data(), wire_bytes);
        if (sent) {
            Bump(m_stats.keepalives_sent);
        }
    }

    bool SendDirect(const Endpoint& destination, const std::uint8_t* wire, std::size_t bytes) {
        return m_transport != nullptr && m_transport->SendTo(destination, wire, bytes);
    }

    bool SendViaRelay(const Endpoint& destination, const std::uint8_t* wire, std::size_t bytes) {
        RelayClient* relay = m_relay.load(std::memory_order_acquire);
        if (relay == nullptr) {
            return false;
        }
        return relay->SendTo(destination, wire, bytes);
    }

    int RecvDirect(Endpoint* source, std::chrono::milliseconds timeout) {
        if (m_transport == nullptr) {
            return -1;
        }
        return m_transport->RecvFrom(source, m_recv_buffer.data(), m_recv_buffer.size(), timeout);
    }

    int RecvViaRelay(Endpoint* source, std::chrono::milliseconds timeout) {
        RelayClient* relay = m_relay.load(std::memory_order_acquire);
        if (relay == nullptr) {
            return -1;
        }
        return relay->RecvFrom(source, m_recv_buffer.data(), m_recv_buffer.size(), timeout);
    }

    bool SourceMatchesPeer(const Endpoint& source) const {
        return source == CurrentSnapshot().peer;
    }

    /**
     * @brief 处理数据面里混入的 STUN Binding 包（连接建立后的检查/响应）。
     *
     * @param data   [in] 收到的数据报首地址；长度不足时内部直接返回。
     * @param length [in] 数据报字节数。
     * @param source [in] 数据报源端点（已由 `Endpoint::FromSockaddr` 归一化）。
     * @param relayed [in] 是否走中继接收；中继路径回包也必须走中继。
     * @param now_ns [in] 当前 steady_clock 纳秒时间戳；<=0 表示调用方没取时钟。
     *
     * ## 定位
     * 这是「对端探测 → 本端确认该对可写」的唯一入口。放宽成任意源会让第三方
     * 一个伪造源地址就注入/反射（历史上出过 peer 劫持回归，见
     * `tests/p2p/test_p2p_peer_validation.cpp`）；收紧成只有选定 peer 则对端切换
     * 后本端永远发现不了新路径。
     *
     * ## 步骤
     * step1. 视图解析（0 分配）；不是 Binding 直接丢弃；
     * step2. Request：只接受选定 peer 或已注册候选对；后者回 Success 即证明
     *        双向可达，把它标记为可写；
     * step3. Success Response：来自已注册对时刷新其 writable/receiving。
     *
     * ## 并发/失败语义
     * - 调用约束：只在 Recv 线程调用；`m_ice` 内部加锁，与
     *   `CandidatePairs()` 的并发读安全。
     * - 失败语义：解析失败/类别不认识一律静默丢弃（不抛、不写 error）——
     *   数据面被任意包刷错误串会让 LastError 失去诊断价值。
     * - 不变量：返回后不会有任何未经验证的源被登记进候选对。
     */
    void HandleStun(const std::uint8_t* data, std::size_t length, const Endpoint& source,
                    bool relayed, std::int64_t now_ns) {
        // step1: 视图解析（0 分配）+ 栈上直接写 Success 回包，不构造
        // StunMessage / vector 属性（旧实现每包 10+ 次分配）。
        const auto message = ParseStunMessageView(data, length);
        if (!message.has_value() || message->method() != kStunMethodBinding) {
            return;
        }
        const bool ice_active = m_ice_active.load(std::memory_order_relaxed);
        if (message->klass() == StunClass::kRequest) {
            // step2: 连接后只回应「已确认的 peer」或「已注册的候选对」；任意源
            // 都能触发回包会变成 UDP 反射/探测点。
            if (!SourceMatchesPeer(source)) {
                if (!ice_active || !m_ice.IsKnownPair(source)) {
                    return;
                }
                // 对端主动探测到这条备用路径：收到请求即证明它可达，回 Success
                // 之后该对就是可写的（ICE 的 consent 语义），双方各自收敛。
                const std::int64_t stamp = now_ns > 0 ? now_ns : NowNanos();
                m_ice.OnCheckSucceeded(source, stamp, 0.0);
            }
            // 连接后收到对端重传的检查包：回一个 Success，避免对端超时重试。
            std::array<std::uint8_t, 128> wire{};
            std::array<std::uint8_t, kStunTransactionIdSize> transaction_id{};
            std::memcpy(transaction_id.data(), message->transaction_id(), transaction_id.size());
            const std::size_t wire_size = BuildBindingSuccessWire(
                wire.data(), wire.size(), transaction_id, source, nullptr, nullptr, {});
            if (wire_size > 0) {
                (void)(relayed ? SendViaRelay(source, wire.data(), wire_size)
                               : SendDirect(source, wire.data(), wire_size));
            }
            return;
        }
        // step3: Success Response 是「该对可写」的证据。本端只向已注册对发过
        // 检查，凭空出现的 Success 不可能来自第三方（它得先看到请求），
        // 因此不必再校验 transaction id。
        if (message->klass() == StunClass::kSuccessResponse) {
            Bump(m_stats.connectivity_checks_received);
            if (ice_active) {
                const std::int64_t stamp = now_ns > 0 ? now_ns : NowNanos();
                m_ice.OnCheckSucceeded(source, stamp, 0.0);
            }
        }
    }

    /**
     * @brief 通过 signaling 发送本地候选并等待远端候选。
     * @details 双方都先 Send 再 Recv，因此即使一端先调用也不会死锁：
     *          先 Send 的一方会在自己的 Recv 里等到后一方的候选。
     */
    bool ExchangeCandidates(Clock::time_point deadline) {
        const std::string payload = SerializeCandidates(LocalCandidates());
        if (!m_config.signaling->Send(payload)) {
            SetError("信令发送失败");
            return false;
        }
        while (Clock::now() < deadline) {
            std::string incoming;
            const std::chrono::milliseconds left = Remaining(deadline);
            const int received = m_config.signaling->Recv(
                &incoming, std::min(left, std::chrono::milliseconds(100)));
            if (received < 0) {
                SetError("信令通道已关闭");
                return false;
            }
            if (received == 0) {
                continue;
            }
            std::string error;
            const std::vector<Candidate> parsed = ParseCandidates(incoming, &error);
            if (parsed.empty()) {
                SetError("远端候选解析失败: " + error);
                continue;
            }
            for (const Candidate& candidate : parsed) {
                AddRemoteCandidate(candidate);
            }
            if (!RemoteCandidateSnapshot().empty()) {
                return true;
            }
        }
        SetError("等待远端候选超时");
        return false;
    }

    void SortEndpointsByRemotePriority(std::vector<Endpoint>* endpoints,
                                       const std::vector<Candidate>& remote) const {
        std::stable_sort(endpoints->begin(), endpoints->end(),
                         [&remote](const Endpoint& left, const Endpoint& right) {
                             const auto priority_of = [&remote](const Endpoint& endpoint) {
                                 for (const Candidate& candidate : remote) {
                                     if (candidate.endpoint == endpoint) {
                                         return candidate.priority;
                                     }
                                 }
                                 return 0U;
                             };
                             return priority_of(left) > priority_of(right);
                         });
    }

    bool TryDirect(const std::vector<Endpoint>& targets, Clock::time_point deadline,
                   Clock::time_point started) {
        PendingChecks pending;
        Clock::time_point next_check = started;
        const auto interval = std::max(m_config.check_interval, std::chrono::milliseconds(1));
        while (Clock::now() < deadline) {
            const Clock::time_point now = Clock::now();
            if (now >= next_check) {
                for (const Endpoint& target : targets) {
                    if (!SendCheckDirect(target, &pending)) {
                        continue;
                    }
                }
                next_check = now + interval;
            }
            Endpoint source;
            const int received = RecvDirect(&source, ClampPoll(Remaining(deadline), kConnectPollMs));
            if (received == 0) {
                continue;
            }
            if (received < 0) {
                SetFailed("直连检查接收失败");
                return false;
            }
            const auto* bytes = m_recv_buffer.data();
            if (!LooksLikeStun(bytes, static_cast<std::size_t>(received))) {
                continue;
            }
            const auto message =
                ParseStunMessageView(bytes, static_cast<std::size_t>(received));
            if (!message.has_value() || message->method() != kStunMethodBinding) {
                continue;
            }
            if (message->klass() == StunClass::kRequest) {
                // 只接受已交换候选的检查：否则任意第三方一个 Request 就能把
                // peer 锁定到自己（连接劫持）。
                if (!ContainsEndpoint(targets, source)) {
                    continue;
                }
                std::array<std::uint8_t, 128> wire{};
                std::array<std::uint8_t, kStunTransactionIdSize> transaction_id{};
                std::memcpy(transaction_id.data(), message->transaction_id(),
                            transaction_id.size());
                const std::size_t wire_size = BuildBindingSuccessWire(
                    wire.data(), wire.size(), transaction_id, source, nullptr, nullptr, {});
                if (wire_size > 0) {
                    (void)SendDirect(source, wire.data(), wire_size);
                }
                Bump(m_stats.connectivity_checks_received);
                Establish(source, false);
                return true;
            }
            if (message->klass() == StunClass::kSuccessResponse &&
                pending.Contains(message->transaction_id())) {
                Bump(m_stats.connectivity_checks_received);
                Establish(source, false);
                return true;
            }
        }
        return false;
    }

    bool TryRelay(const std::vector<Endpoint>& targets, Clock::time_point deadline,
                  Clock::time_point started) {
        RelayClient* relay = EnsureRelay();
        if (relay == nullptr) {
            SetError("中继客户端未初始化");
            return false;
        }
        if (!relay->allocated()) {
            std::string error;
            if (!relay->Allocate(&error)) {
                SetError("中继分配失败: " + error);
                return false;
            }
        }
        PendingChecks pending;
        Clock::time_point next_check = started;
        const auto interval = std::max(m_config.check_interval, std::chrono::milliseconds(1));
        while (Clock::now() < deadline) {
            const Clock::time_point now = Clock::now();
            if (now >= next_check) {
                for (const Endpoint& target : targets) {
                    std::array<std::uint8_t, 64> wire{};
                    std::array<std::uint8_t, kStunTransactionIdSize> transaction_id{};
                    const std::size_t wire_size =
                        BuildBindingRequestWire(wire.data(), wire.size(), &transaction_id);
                    if (wire_size == 0) {
                        continue;
                    }
                    if (relay->SendTo(target, wire.data(), wire_size)) {
                        pending.Push(transaction_id);
                        Bump(m_stats.connectivity_checks_sent);
                    }
                }
                next_check = now + interval;
            }
            Endpoint source;
            const int received =
                relay->RecvFrom(&source, m_recv_buffer.data(), m_recv_buffer.size(),
                                ClampPoll(Remaining(deadline), kConnectPollMs));
            if (received == 0) {
                continue;
            }
            if (received < 0) {
                SetFailed("中继检查接收失败");
                return false;
            }
            const auto* bytes = m_recv_buffer.data();
            if (!LooksLikeStun(bytes, static_cast<std::size_t>(received))) {
                continue;
            }
            const auto message =
                ParseStunMessageView(bytes, static_cast<std::size_t>(received));
            if (!message.has_value() || message->method() != kStunMethodBinding) {
                continue;
            }
            if (message->klass() == StunClass::kRequest) {
                // 同 TryDirect：中继路径也必须校验源地址属于候选集合。
                if (!ContainsEndpoint(targets, source)) {
                    continue;
                }
                std::array<std::uint8_t, 128> wire{};
                std::array<std::uint8_t, kStunTransactionIdSize> transaction_id{};
                std::memcpy(transaction_id.data(), message->transaction_id(),
                            transaction_id.size());
                const std::size_t wire_size = BuildBindingSuccessWire(
                    wire.data(), wire.size(), transaction_id, source, nullptr, nullptr, {});
                if (wire_size > 0) {
                    (void)relay->SendTo(source, wire.data(), wire_size);
                }
                Bump(m_stats.connectivity_checks_received);
                Establish(source, true);
                return true;
            }
            if (message->klass() == StunClass::kSuccessResponse &&
                pending.Contains(message->transaction_id())) {
                Bump(m_stats.connectivity_checks_received);
                Establish(source, true);
                return true;
            }
        }
        return false;
    }

    bool SendCheckDirect(const Endpoint& target, PendingChecks* pending) {
        // 检查包直接写栈缓冲：不再构造 StunMessage（SOFTWARE 属性 vector 分配）
        // 再 Serialize（body + output 两次分配）。事务 ID 从 TLS 随机池取，
        // 也省掉了每个检查包一次 getrandom 系统调用。
        std::array<std::uint8_t, 64> wire{};
        std::array<std::uint8_t, kStunTransactionIdSize> transaction_id{};
        const std::size_t wire_size =
            BuildBindingRequestWire(wire.data(), wire.size(), &transaction_id);
        if (wire_size == 0 || !SendDirect(target, wire.data(), wire_size)) {
            return false;
        }
        pending->Push(transaction_id);
        Bump(m_stats.connectivity_checks_sent);
        return true;
    }

    /**
     * @brief 连接成功：发布新快照、重置时间戳/序列基线，并登记热切换的选定对。
     *
     * @param peer    [in] 选定的对端端点（直连为对端候选地址，中继为中继出口地址）。
     * @param relayed [in] 是否走中继；中继路径不参与候选对热切换。
     *
     * ## 定位
     * 唯一的「连接成功提交点」。会话 ID 在这里生成，热切换时**不会**重新生成——
     * 它是「上层流未重建」的可观测证据；若在换 peer 时也生成，mux/WS 会把迁移
     * 误判为新连接并重建流，功能就白做了。
     *
     * ## 步骤
     * step1. 首次连接生成会话 ID，发布状态快照；
     * step2. 重置收包/keepalive/序列基线，避免沿用旧路径的陈旧时间戳；
     * step3. 直连时把该对登记为候选对里的选定对（中继不登记）。
     *
     * ## 并发/失败语义
     * - 调用约束：Connect 线程调用；状态先发布再做登记，读路径无锁。
     * - 失败语义：`Select()` 失败（地址不在候选对里）不报错，controller 保持
     *   「无选定对」，`Evaluate()` 恒返回 kNone —— 自动退化为不做切换。
     * - 不变量：返回后 state 是 kConnected 或 kRelayed，且（直连时）
     *   `SessionId() != 0`。
     */
    void Establish(const Endpoint& peer, bool relayed) {
        const std::int64_t now_ns = NowNanos();
        // step1: 会话 ID 只在首次连接时生成；热切换绝不改写它。
        if (m_session_id.load(std::memory_order_relaxed) == 0) {
            m_session_id.store(NextSessionId(), std::memory_order_relaxed);
        }
        PublishStateIfOpen(relayed ? P2PState::kRelayed : P2PState::kConnected, peer);
        // step2: 新路径从「现在」起算，旧路径的收包时刻不能当本路径的证据。
        m_last_receive_ns.store(now_ns, std::memory_order_relaxed);
        m_last_keepalive_ns.store(now_ns, std::memory_order_relaxed);
        m_has_pending_keepalive.store(false, std::memory_order_relaxed);
        m_last_data_sequence.store(0, std::memory_order_relaxed);
        // step3: 只有直连有候选对；中继的 peer 是 TURN 出口，不参与切换。
        if (!relayed && m_ice_active.load(std::memory_order_relaxed)) {
            (void)m_ice.Select(peer, now_ns);
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error.clear();
    }

    P2PConfig m_config;
    /// keepalive / peer_timeout / 候选对探测都关闭时，Recv 循环不必每轮取
    /// steady_clock（数据快路径上一次 vDSO 也要 ~15ns，每包都取很可观）。
    const bool m_track_activity = m_config.keepalive_interval.count() > 0 ||
                                  m_config.peer_timeout.count() > 0;
    /// 是否需要在 Recv 循环里取时钟：候选对探测也依赖时间推进。
    const bool m_need_clock = m_track_activity || m_config.ice_switchover.enabled;
    /// 候选对热切换决策器；候选对注册后只由 Recv 线程驱动。
    IcePairController m_ice;
    /// 是否让候选对参与收发路径（>=2 个直连对才为 true，注册后不再改变）。
    std::atomic<bool> m_ice_active{false};
    /// 连接会话 ID：热切换保持不变，只有新建通道才会变。
    std::atomic<std::uint64_t> m_session_id{0};
    /// 最近一次热切换后的代数（供上层观测「切过但没重建」）。
    std::atomic<std::uint64_t> m_ice_generation{0};
    std::shared_ptr<DatagramTransport> m_transport;
    /// 保护 error / 候选集合 / 快照历史；收发热路径不再使用它。
    mutable std::mutex m_mutex;
    std::vector<Candidate> m_local_candidates;
    std::vector<Candidate> m_remote_candidates;
    std::string m_error;
    /// 连接状态快照：发布后不可变，旧快照保留到析构（读路径可安全持有）。
    std::atomic<const ConnectionSnapshot*> m_snapshot{nullptr};
    std::vector<std::unique_ptr<ConnectionSnapshot>> m_snapshot_history;
    /// 统计：每个字段独立原子，读快照允许轻微不一致（与旧实现一致）。
    /// 发送侧（Send 线程每包写）与接收侧（Recv 线程每包写）分处不同
    /// cacheline：否则两个线程每包都互相打掉对方刚写热的 cacheline
    /// （伪共享），真实双线程收发的固定税会翻倍。
    struct AtomicStats {
        alignas(64) std::atomic<std::uint64_t> packets_sent{0};
        std::atomic<std::uint64_t> bytes_sent{0};
        std::atomic<std::uint64_t> relayed_packets_sent{0};
        std::atomic<std::uint64_t> keepalives_sent{0};
        std::atomic<std::uint64_t> connectivity_checks_sent{0};
        alignas(64) std::atomic<std::uint64_t> packets_received{0};
        std::atomic<std::uint64_t> bytes_received{0};
        std::atomic<std::uint64_t> relayed_packets_received{0};
        std::atomic<std::uint64_t> keepalives_received{0};
        std::atomic<std::uint64_t> connectivity_checks_received{0};
        std::atomic<std::uint64_t> dropped_packets{0};
        std::atomic<std::uint32_t> sequence_gaps{0};
        std::atomic<double> rtt_ms{0.0};
        std::atomic<double> rtt_avg_ms{0.0};
        std::atomic<std::uint64_t> ice_probes_sent{0};
        std::atomic<std::uint64_t> ice_pair_switches{0};
    };
    AtomicStats m_stats;
    /// steady_clock 纳秒时间戳（原子：收包线程写、任意线程读）。
    std::atomic<std::int64_t> m_last_receive_ns{NowNanos()};
    std::atomic<std::int64_t> m_last_keepalive_ns{NowNanos()};
    std::atomic<bool> m_has_pending_keepalive{false};
    std::atomic<std::uint32_t> m_last_data_sequence{0};
    std::atomic<bool> m_closed{false};
    // Send() 允许与 Recv() 在不同线程并发，序号必须是原子的。
    std::atomic<std::uint32_t> m_send_sequence{0};
    std::uint32_t m_keepalive_sequence = 0;  // 仅 Recv 线程
    std::uint64_t m_rtt_samples = 0;         // 仅 Recv 线程
    double m_rtt_sum = 0.0;                  // 仅 Recv 线程
    /// 复用帧缓冲：Send 用 tx、Recv 内的 keepalive/ACK 用 rx，互不干扰。
    /// 定长数组（非 vector）：建帧不再对载荷做值初始化，每包省一次 memset。
    std::array<std::uint8_t, kFrameCapacity> m_tx_frame{};
    std::array<std::uint8_t, kFrameCapacity> m_rx_frame{};
    /// relay 对象由 owner 持有到析构，裸指针原子发布给热路径。
    std::unique_ptr<RelayClient> m_relay_owner;
    std::atomic<RelayClient*> m_relay{nullptr};
    std::vector<std::uint8_t> m_recv_buffer = std::vector<std::uint8_t>(65536);
};

P2PChannel::P2PChannel(P2PConfig config) : m_impl(std::make_unique<Impl>(std::move(config))) {}

P2PChannel::~P2PChannel() = default;

std::vector<Candidate> P2PChannel::GatherCandidates() {
    return m_impl->GatherCandidates();
}

void P2PChannel::AddRemoteCandidate(const Candidate& candidate) {
    m_impl->AddRemoteCandidate(candidate);
}

bool P2PChannel::Connect(std::chrono::milliseconds timeout) {
    return m_impl->Connect(timeout);
}

bool P2PChannel::Send(const void* data, std::size_t length) {
    return m_impl->Send(data, length);
}

std::size_t P2PChannel::Recv(void* buffer, std::size_t capacity,
                             std::chrono::milliseconds timeout) {
    return m_impl->Recv(buffer, capacity, timeout);
}

bool P2PChannel::IsRelayed() const {
    return m_impl->IsRelayed();
}

bool P2PChannel::IsConnected() const {
    return m_impl->IsConnected();
}

P2PState P2PChannel::state() const {
    return m_impl->state();
}

Endpoint P2PChannel::LocalEndpoint() const {
    return m_impl->LocalEndpoint();
}

Endpoint P2PChannel::PeerEndpoint() const {
    return m_impl->PeerEndpoint();
}

std::vector<Candidate> P2PChannel::LocalCandidates() const {
    return m_impl->LocalCandidates();
}

std::vector<Candidate> P2PChannel::RemoteCandidates() const {
    return m_impl->RemoteCandidates();
}

P2PStats P2PChannel::Stats() const {
    return m_impl->Stats();
}

std::string P2PChannel::LastError() const {
    return m_impl->LastError();
}

std::uint64_t P2PChannel::SessionId() const {
    return m_impl->SessionId();
}

Endpoint P2PChannel::SelectedPairRemote() const {
    return m_impl->SelectedPairRemote();
}

std::vector<IcePairSnapshot> P2PChannel::CandidatePairs() const {
    return m_impl->CandidatePairs();
}

bool P2PChannel::EvaluateIceSwitchover() {
    return m_impl->EvaluateIceSwitchover();
}

std::uint64_t P2PChannel::SwitchGeneration() const {
    return m_impl->SwitchGeneration();
}

void P2PChannel::Close() {
    m_impl->Close();
}

}  // namespace go2cpp::p2p
