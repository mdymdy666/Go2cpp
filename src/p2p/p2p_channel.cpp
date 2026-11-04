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
    explicit Impl(P2PConfig config) : m_config(std::move(config)) {
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
            if (m_track_activity) {
                now_ns = NowNanos();
                MaybeSendKeepalive(now_ns);
                if (PeerTimedOut(now_ns)) {
                    SetFailed("对端静默超过 peer_timeout");
                    return 0;
                }
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
                HandleStun(bytes, received_bytes, source, relayed);
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
            if (!SourceMatchesPeer(source)) {
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
        return out;
    }

    std::string LastError() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_error;
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

    void HandleStun(const std::uint8_t* data, std::size_t length, const Endpoint& source,
                    bool relayed) {
        // 数据面里混入的 STUN 包：视图解析（0 分配）+ 栈上直接写 Success 回包，
        // 不再构造 StunMessage / vector 属性（旧实现每包 10+ 次分配）。
        const auto message = ParseStunMessageView(data, length);
        if (!message.has_value() || message->method() != kStunMethodBinding) {
            return;
        }
        if (message->klass() == StunClass::kRequest) {
            // 连接后只回应已确认 peer 的检查；任意源都能触发回包会变成
            // UDP 反射/探测点。
            if (!SourceMatchesPeer(source)) {
                return;
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
        if (message->klass() == StunClass::kSuccessResponse) {
            Bump(m_stats.connectivity_checks_received);
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

    /// @brief 连接成功：发布新快照并重置时间戳/序列基线。
    void Establish(const Endpoint& peer, bool relayed) {
        const std::int64_t now_ns = NowNanos();
        PublishStateIfOpen(relayed ? P2PState::kRelayed : P2PState::kConnected, peer);
        m_last_receive_ns.store(now_ns, std::memory_order_relaxed);
        m_last_keepalive_ns.store(now_ns, std::memory_order_relaxed);
        m_has_pending_keepalive.store(false, std::memory_order_relaxed);
        m_last_data_sequence.store(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error.clear();
    }

    P2PConfig m_config;
    /// keepalive / peer_timeout 都关闭时，Recv 循环不必每轮取 steady_clock。
    const bool m_track_activity = m_config.keepalive_interval.count() > 0 ||
                                  m_config.peer_timeout.count() > 0;
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

void P2PChannel::Close() {
    m_impl->Close();
}

}  // namespace go2cpp::p2p
