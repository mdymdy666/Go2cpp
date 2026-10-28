/**
 * @file relay.cpp
 * @brief `Relay` 实现：单线程切片轮转的双向拷贝、有界缓冲背压、慢客户端与半关闭。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 *
 * @details 实现刻意与 `TcpProxy::PumpDirection`（见 `src/bento/net/tcp_proxy.cpp`）
 *          保持同一套顺序：**先续写 pending，再读源**。区别在于 `TcpProxy`
 *          由 `poll` 告知就绪，而这里只能靠 `ITransport` 的 deadline 读写做
 *          时间片轮转，因此多了一个「双向都无进展就休眠」的节拍，避免空转。
 *
 *          背压不是额外的流控协议：只要 pending 达到 `buffer_bytes` 就不再
 *          调用 `Read`，源端的 TCP 窗口自然会把压力传回去。慢客户端判定也不是
 *          「某次 Write 超时」，而是「连续 `write_timeout_ms` 没写出任何字节」，
 *          这样慢但仍在推进的客户端不会被误杀。
 */

#include "go2cpp/bento/net/relay.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace go2cpp::bento::net {
namespace {

using Clock = std::chrono::steady_clock;

/// 单调毫秒时钟；只用于算时间差，不要求与墙上时钟对齐。
std::uint64_t NowMs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
            .count());
}

std::chrono::milliseconds Milliseconds(std::uint32_t value) noexcept {
    return std::chrono::milliseconds(static_cast<std::int64_t>(value));
}

/// 把用户配置收敛到可实现的范围：容量至少 1 字节，读块不超过容量。
RelayOptions Normalize(const RelayOptions& options) {
    RelayOptions normalized = options;
    if (normalized.buffer_bytes == 0) {
        normalized.buffer_bytes = 64 * 1024;
    }
    if (normalized.read_chunk_bytes == 0 || normalized.read_chunk_bytes > normalized.buffer_bytes) {
        normalized.read_chunk_bytes = normalized.buffer_bytes;
    }
    return normalized;
}

/**
 * @brief 单个方向的 pending 缓冲与状态。
 *
 * @details 用 `begin`/`end` 下标而不是每轮 `erase(0, n)`：写出一部分就只推进
 *          `begin`，避免搬移剩余数据。只有尾部写满（`end == capacity`）且前半
 *          段已经空出时，才把剩余数据前移一次（`Defragment`）。
 */
struct DirectionState {
    std::vector<std::uint8_t> buffer;  ///< 固定容量缓冲。
    std::size_t begin = 0;             ///< 待写出数据的起始下标。
    std::size_t end = 0;               ///< 已读入数据的末尾下标。
    bool eof = false;                  ///< 源方向已经 EOF。
    bool fin_sent = false;             ///< 已经向下游传播过 FIN。
    bool stalling = false;             ///< 当前是否处于「写无进展」状态。
    std::uint64_t stall_start_ms = 0;  ///< 本次无进展的起始时刻。

    std::size_t pending() const noexcept {
        return end - begin;
    }
    std::size_t room() const noexcept {
        return buffer.size() - pending();
    }
    std::size_t contiguous_room() const noexcept {
        return buffer.size() - end;
    }
    std::uint8_t* write_pos() noexcept {
        return buffer.data() + end;
    }
    const std::uint8_t* read_pos() const noexcept {
        return buffer.data() + begin;
    }

    void Commit(std::size_t count) noexcept {
        end += count;
    }

    void Consume(std::size_t count) noexcept {
        begin += count;
        if (begin == end) {
            begin = 0;
            end = 0;
        }
    }

    /// 尾部没有连续空间、但前半段已空出时把 pending 前移，保证还能继续读。
    void Defragment() noexcept {
        if (end != buffer.size() || begin == 0) {
            return;
        }
        const std::size_t count = pending();
        if (count != 0) {
            std::memmove(buffer.data(), buffer.data() + begin, count);
        }
        begin = 0;
        end = count;
    }
};

/**
 * @brief 一次 `Run` 的执行体：持有两条传输的引用、两个方向的缓冲与本次统计。
 *
 * @details 单线程、无锁；生命周期只覆盖一次 `Run`，所以运行结束后 pending
 *          缓冲随对象析构释放，不需要额外的清空逻辑。
 */
class Runner final {
public:
    Runner(ITransport& src, ITransport& dst, const RelayOptions& options)
        : m_src(src), m_dst(dst), m_options(Normalize(options)) {
        m_a.buffer.resize(m_options.buffer_bytes);
        m_b.buffer.resize(m_options.buffer_bytes);
    }

    RelayResult Execute() {
        RelayResult result;
        m_last_activity_ms = NowMs();
        RelayCloseReason reason = RelayCloseReason::kCompleted;

        for (;;) {
            bool progressed = false;

            const PumpResult forward = Pump(m_src, m_dst, m_a, true, &progressed);
            if (forward == PumpResult::kSourceError) {
                ++m_stats.read_errors;
                reason = RelayCloseReason::kReadError;
                break;
            }
            if (forward == PumpResult::kDestClosed || forward == PumpResult::kDestError) {
                ++m_stats.write_errors;
                reason = RelayCloseReason::kWriteError;
                break;
            }

            const PumpResult backward = Pump(m_dst, m_src, m_b, false, &progressed);
            if (backward == PumpResult::kSourceError) {
                ++m_stats.read_errors;
                reason = RelayCloseReason::kReadError;
                break;
            }
            if (backward == PumpResult::kDestClosed || backward == PumpResult::kDestError) {
                ++m_stats.write_errors;
                reason = RelayCloseReason::kWriteError;
                break;
            }

            UpdatePeaks();
            SendPendingFin(m_dst, m_a);
            SendPendingFin(m_src, m_b);

            if (Finished()) {
                reason = RelayCloseReason::kCompleted;
                break;
            }

            const std::uint64_t now = NowMs();
            if (Stalled(m_a, now) || Stalled(m_b, now)) {
                ++m_stats.slow_consumers;
                reason = RelayCloseReason::kSlowConsumer;
                break;
            }
            if (m_options.idle_timeout_ms != 0 &&
                now - m_last_activity_ms >= static_cast<std::uint64_t>(m_options.idle_timeout_ms)) {
                ++m_stats.idle_timeouts;
                reason = RelayCloseReason::kIdleTimeout;
                break;
            }

            // 两个方向都没有字节流动时不要忙转：Read 的 slice 为 0 时一次调用
            // 会立刻返回，没有这个休眠就会占满一个核。
            if (!progressed && m_options.poll_interval_ms != 0) {
                std::this_thread::sleep_for(Milliseconds(m_options.poll_interval_ms));
            }
        }

        // 半关闭只关写方向；完整释放必须走 Close，否则 fd/队列会残留。
        (void)m_src.Close();
        (void)m_dst.Close();
        result.reason = reason;
        result.stats = m_stats;
        result.system_error = m_system_error;
        return result;
    }

private:
    enum class PumpResult : std::uint8_t {
        kIdle = 0,        ///< 没有任何进展。
        kProgress = 1,    ///< 读到或写出了字节。
        kSourceEof = 2,   ///< 源方向 EOF（不是错误）。
        kSourceError = 3, ///< 源方向读错误。
        kDestClosed = 4,  ///< 目标方向已关闭（EPIPE/RST）。
        kDestError = 5,   ///< 目标方向写错误。
    };

    /// 推进一个方向：先续写 pending，再在缓冲未满时读源。返回本方向的结论。
    PumpResult Pump(ITransport& src, ITransport& dst, DirectionState& dir, bool src_to_dst,
                    bool* progressed) {
        bool local_progress = false;

        // 1) 先写：上一轮读进来的数据优先排空，避免 pending 无谓堆积。
        if (dir.pending() != 0) {
            const Result written =
                dst.Write(dir.read_pos(), dir.pending(), Milliseconds(m_options.write_slice_ms));
            const std::size_t moved = std::min(written.bytes, dir.pending());
            if (moved != 0) {
                dir.Consume(moved);
                if (src_to_dst) {
                    m_stats.bytes_src_to_dst += moved;
                } else {
                    m_stats.bytes_dst_to_src += moved;
                }
                dir.stalling = false;
                m_last_activity_ms = NowMs();
                local_progress = true;
            }
            if (written.code == Code::kClosed) {
                m_system_error = written.system_error;
                return PumpResult::kDestClosed;
            }
            if (written.code == Code::kError) {
                m_system_error = written.system_error;
                return PumpResult::kDestError;
            }
            // 契约上 kOk 且 size>0 至少应写出 1 字节；0 字节按无进展处理，
            // 既不会死循环，也能让慢客户端判定在畸形实现上仍然生效。
            if (moved == 0 && !dir.stalling) {
                dir.stalling = true;
                dir.stall_start_ms = NowMs();
            }
        }

        // 2) 再读：缓冲满就停读，这是端到端背压的唯一来源。
        if (!dir.eof && dir.room() != 0) {
            dir.Defragment();
            const std::size_t available = std::min(dir.room(), dir.contiguous_room());
            const std::size_t want = std::min(m_options.read_chunk_bytes, available);
            if (want != 0) {
                const Result read =
                    src.Read(dir.write_pos(), want, Milliseconds(m_options.read_slice_ms));
                const std::size_t got = std::min(read.bytes, want);
                if (got != 0) {
                    dir.Commit(got);
                    if (src_to_dst) {
                        m_stats.read_src_to_dst += got;
                    } else {
                        m_stats.read_dst_to_src += got;
                    }
                    m_last_activity_ms = NowMs();
                    local_progress = true;
                }
                if (read.code == Code::kError) {
                    m_system_error = read.system_error;
                    return PumpResult::kSourceError;
                }
                if (read.code == Code::kClosed) {
                    // 数据先于 EOF：上面已把读到的字节提交，pending 排空后才发 FIN。
                    dir.eof = true;
                    *progressed = *progressed || local_progress;
                    return PumpResult::kSourceEof;
                }
            }
        }

        *progressed = *progressed || local_progress;
        return local_progress ? PumpResult::kProgress : PumpResult::kIdle;
    }

    /// EOF 且 pending 已排空时向下游传播 FIN（只发一次）。
    void SendPendingFin(ITransport& dst, DirectionState& dir) {
        if (!dir.eof || dir.fin_sent || dir.pending() != 0) {
            return;
        }
        (void)dst.ShutdownWrite();
        dir.fin_sent = true;
        ++m_stats.half_closes;
    }

    bool Finished() const noexcept {
        return m_a.eof && m_a.fin_sent && m_b.eof && m_b.fin_sent;
    }

    bool Stalled(const DirectionState& dir, std::uint64_t now) const noexcept {
        if (m_options.write_timeout_ms == 0 || !dir.stalling || dir.pending() == 0) {
            return false;
        }
        return now - dir.stall_start_ms >= static_cast<std::uint64_t>(m_options.write_timeout_ms);
    }

    void UpdatePeaks() {
        const std::uint64_t pending_a = static_cast<std::uint64_t>(m_a.pending());
        const std::uint64_t pending_b = static_cast<std::uint64_t>(m_b.pending());
        m_stats.peak_pending_src_to_dst = std::max(m_stats.peak_pending_src_to_dst, pending_a);
        m_stats.peak_pending_dst_to_src = std::max(m_stats.peak_pending_dst_to_src, pending_b);
        m_stats.peak_buffered_bytes = std::max(m_stats.peak_buffered_bytes, pending_a + pending_b);
    }

    ITransport& m_src;
    ITransport& m_dst;
    RelayOptions m_options;
    DirectionState m_a;  ///< src → dst。
    DirectionState m_b;  ///< dst → src。
    RelayStats m_stats;
    std::uint64_t m_last_activity_ms = 0;
    int m_system_error = 0;
};

}  // namespace

RelayStats& RelayStats::operator+=(const RelayStats& other) noexcept {
    bytes_src_to_dst += other.bytes_src_to_dst;
    bytes_dst_to_src += other.bytes_dst_to_src;
    read_src_to_dst += other.read_src_to_dst;
    read_dst_to_src += other.read_dst_to_src;
    // 峰值不能相加：跨多次运行取历史最大值。
    peak_pending_src_to_dst = std::max(peak_pending_src_to_dst, other.peak_pending_src_to_dst);
    peak_pending_dst_to_src = std::max(peak_pending_dst_to_src, other.peak_pending_dst_to_src);
    peak_buffered_bytes = std::max(peak_buffered_bytes, other.peak_buffered_bytes);
    half_closes += other.half_closes;
    slow_consumers += other.slow_consumers;
    idle_timeouts += other.idle_timeouts;
    connect_timeouts += other.connect_timeouts;
    connect_failures += other.connect_failures;
    read_errors += other.read_errors;
    write_errors += other.write_errors;
    return *this;
}

const char* RelayCloseReasonName(RelayCloseReason reason) noexcept {
    switch (reason) {
        case RelayCloseReason::kCompleted:
            return "Completed";
        case RelayCloseReason::kSlowConsumer:
            return "SlowConsumer";
        case RelayCloseReason::kIdleTimeout:
            return "IdleTimeout";
        case RelayCloseReason::kConnectTimeout:
            return "ConnectTimeout";
        case RelayCloseReason::kConnectFailed:
            return "ConnectFailed";
        case RelayCloseReason::kReadError:
            return "ReadError";
        case RelayCloseReason::kWriteError:
            return "WriteError";
    }
    return "Unknown";
}

RelayResult Relay::Run(ITransport& src, ITransport& dst, const RelayOptions& options) {
    const std::uint64_t start = NowMs();
    Runner runner(src, dst, options);
    RelayResult result = runner.Execute();
    result.elapsed_ms = NowMs() - start;
    m_stats += result.stats;
    return result;
}

RelayResult Relay::ConnectAndRun(ITransport& src, ITransport& dst, const Endpoint& destination,
                                 const RelayOptions& options) {
    const std::uint64_t start = NowMs();
    const Result connected = dst.Connect(destination, Milliseconds(options.connect_timeout_ms));
    if (connected.code != Code::kOk) {
        RelayResult result;
        result.reason = connected.code == Code::kTimeout ? RelayCloseReason::kConnectTimeout
                                                         : RelayCloseReason::kConnectFailed;
        result.system_error = connected.system_error;
        if (connected.code == Code::kTimeout) {
            ++result.stats.connect_timeouts;
        } else {
            ++result.stats.connect_failures;
        }
        (void)src.Close();
        (void)dst.Close();
        result.elapsed_ms = NowMs() - start;
        m_stats += result.stats;
        return result;
    }
    // 拨号成功后 Run 自己会累计统计，这里只把 elapsed 换成含拨号的总时长。
    RelayResult result = Run(src, dst, options);
    result.elapsed_ms = NowMs() - start;
    return result;
}

}  // namespace go2cpp::bento::net
