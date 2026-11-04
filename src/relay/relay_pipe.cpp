/**
 * @file relay_pipe.cpp
 * @brief `RelayPipe` 的实现：有界队列、双方向泵、写 deadline 看门狗与统计。
 *
 * @details 线程与数据流：
 *
 * ```
 *   from --Read--> reader --Push--> [bounded queue] --Pop--> writer --Write--> to
 * ```
 *
 * 每个方向两条线程。读线程只负责「读 + 入队」，队列满时阻塞；写线程只负责
 * 「出队 + 写满一个 chunk」。直通模式（`PipeConfig::direct`）下每方向只有
 * 一条线程，读到就写，不建队列——见 `Direction::DirectLoop`。队列容量按字节计，任何时刻都不会超过
 * `queue_cap`（外加当前正在写的那一个 chunk）。看门狗线程每 5 ms 检查一次
 * 写 deadline；超时只关闭该方向的**目的端**，不取消另一方向。
 *
 * ## 关键不变量
 *
 * - `ChunkQueue` 的 `m_bytes` 与实际 chunk 总字节严格一致，受 `m_capacity` 约束；
 * - 每个方向最多报告一次 timeout（`Direction::NoteTimeout` 的 CAS）；
 * - `Abort()` 只在第一次生效；重复调用不会覆盖首个错误，也不会重复关闭；
 * - 读线程正在 `Read` 时才允许关闭源端（`m_phase == kReading`），否则只关队列，
 *   避免无谓地拆掉健康的一端；
 * - 所有统计用 relaxed 原子累加，`Stats()` 在运行期读取不会阻塞。
 *
 * ## 为什么不用 `streams::Copy`
 *
 * `streams::Copy` 是同步单方向复制，无法给写操作设 deadline，也无法在队列满时
 * 保留另一方向的独立性。本模块需要「读写解耦 + 有界缓冲 + 可打断的写」，
 * 因此自己实现两条泵，但仍复用 `streams::ReadResult/WriteResult` 的 Go 语义
 * （允许 `count > 0` 同时带错误、短写不丢数据）。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include "go2cpp/relay/relay_pipe.hpp"

#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/error.hpp"
#include "go2cpp/streams.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace go2cpp {
namespace relay {
namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

/// 默认参数（与 `PipeConfig` 的成员初值保持一致，集中一处便于归一化）。
constexpr std::size_t kDefaultCopyBuf = 32U * 1024U;
constexpr std::size_t kDefaultQueueCap = 256U * 1024U;
constexpr std::chrono::milliseconds kDefaultWriteTimeout{30000};

/// 连续空读/零进展写达到该次数即判定对端违约，避免忙等。
constexpr int kMaxNoProgress = 100;

/// 看门狗轮询间隔；只影响超时判定的上限误差，不参与数据路径。
constexpr std::chrono::milliseconds kWatchdogTick{5};

/// 读 `GO2CPP_RELAY_DIRECT`：1/true/on/yes（大小写不敏感）打开直通。
bool EnvDirectMode() {
    const char* raw = std::getenv("GO2CPP_RELAY_DIRECT");
    if (raw == nullptr) {
        return false;
    }
    std::string value(raw);
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value == "1" || value == "true" || value == "on" ||
           value == "yes";
}

/// 归一化配置：零值/非正值回落默认，队列至少能容纳一个 chunk。
PipeConfig NormalizeConfig(PipeConfig cfg) {
    if (cfg.copy_buf == 0) {
        cfg.copy_buf = kDefaultCopyBuf;
    }
    if (cfg.queue_cap == 0) {
        cfg.queue_cap = kDefaultQueueCap;
    }
    if (cfg.queue_cap < cfg.copy_buf) {
        cfg.queue_cap = cfg.copy_buf;
    }
    if (cfg.write_timeout <= std::chrono::milliseconds::zero()) {
        cfg.write_timeout = kDefaultWriteTimeout;
    }
    // 直通开关默认关；环境变量只在字段未显式置位时生效（便于 A/B 对比）。
    if (!cfg.direct) {
        cfg.direct = EnvDirectMode();
    }
    return cfg;
}

/// 方向标签，用于错误文本与日志定位（不参与逻辑）。
std::string DirectionLabel(int index) {
    return index == 0 ? "relay: a->b" : "relay: b->a";
}

/// 超时错误的统一包装：保留 `Is(err, ErrTimeout())` 的可识别性。
ErrorPtr TimeoutError(const std::string& where) {
    return Wrap(ErrTimeout(), where);
}

}  // namespace

class Direction;

namespace detail {

/**
 * @brief `RelayPipe` 的共享状态：配置、统计、看门狗与两个方向。
 *
 * @details 由 `shared_ptr` 持有，取消回调也只捕获它（不捕获 `RelayPipe`），
 *          因此 `Run` 返回后即使回调还在执行也不会访问已释放对象；回调只碰
 *          原子标志与 `done_cv`，不碰 `Stream`。
 */
struct RelayPipeState final {
    RelayPipeState(Stream* a_in, Stream* b_in, PipeConfig cfg_in)
        : a(a_in), b(b_in), cfg(NormalizeConfig(cfg_in)) {}

    /// 端点 A（不拥有）。
    Stream* a;
    /// 端点 B（不拥有）。
    Stream* b;
    /// 归一化后的配置；`Run` 期间只读。
    PipeConfig cfg;

    /// A → B 已写出的字节数。
    std::atomic<std::uint64_t> a_to_b{0};
    /// B → A 已写出的字节数。
    std::atomic<std::uint64_t> b_to_a{0};
    /// 已读出但未写出的字节数。
    std::atomic<std::uint64_t> dropped{0};
    /// deadline 事件数（每个方向最多一次）。
    std::atomic<std::uint64_t> timeouts{0};

    /// `Run` 是否已经进入过；只允许一次。
    std::atomic<bool> run_started{false};
    /// 上下文取消标志；由取消回调置位。
    std::atomic<bool> cancel_requested{false};

    /// 保护 `directions_done` 与等待通知。
    std::mutex done_mutex;
    /// 两个方向都结束时（或取消时）唤醒 `Run`。
    std::condition_variable done_cv;
    /// 已完成（写线程退出）的方向数。
    int directions_done{0};

    /// 保护写 deadline 槽位与看门狗停止标志。
    std::mutex write_mutex;
    /// 唤醒看门狗（槽位变化或停止）。
    std::condition_variable write_cv;

    /// 每个方向一个写槽位：只有 `Write` 调用进行中才 active。
    struct WriteSlot {
        bool active{false};
        bool timed_out{false};
        TimePoint deadline{};
    };
    WriteSlot slots[2];
    bool watchdog_stop{false};
    /// 低频 deadline 检查线程。
    std::thread watchdog;

    /// 两个方向：0 = A→B，1 = B→A。
    std::unique_ptr<Direction> directions[2];

    /// 析构定义在 `Direction` 完整之后，避免 `unique_ptr` 删除不完整类型。
    ~RelayPipeState();

    /// 写线程退出时调用；第二个方向退出会唤醒 `Run`。
    void OnDirectionDone() {
        std::lock_guard<std::mutex> lock(done_mutex);
        ++directions_done;
        done_cv.notify_all();
    }

    /// 累加已交付字节。
    void AddDelivered(int index, std::size_t bytes) {
        if (index == 0) {
            a_to_b.fetch_add(bytes, std::memory_order_relaxed);
        } else {
            b_to_a.fetch_add(bytes, std::memory_order_relaxed);
        }
    }

    /// 累加丢弃字节。
    void AddDropped(std::size_t bytes) {
        dropped.fetch_add(bytes, std::memory_order_relaxed);
    }

    /// 标记一次写开始；deadline 是绝对时间，chunk 内多次短写共用同一 deadline。
    void BeginWrite(int index, TimePoint deadline) {
        std::lock_guard<std::mutex> lock(write_mutex);
        slots[index].active = true;
        slots[index].timed_out = false;
        slots[index].deadline = deadline;
        write_cv.notify_all();
    }

    /// 标记写结束；返回本次写期间看门狗是否判定超时。
    bool EndWrite(int index) {
        std::lock_guard<std::mutex> lock(write_mutex);
        slots[index].active = false;
        const bool timed_out = slots[index].timed_out;
        slots[index].timed_out = false;
        write_cv.notify_all();
        return timed_out;
    }

    /// 启动看门狗线程；`Run` 在方向线程启动前调用。
    void StartWatchdog() { watchdog = std::thread([this] { WatchdogLoop(); }); }

    /// 停止并回收看门狗；方向线程都已 join 后调用。
    void StopWatchdog() {
        {
            std::lock_guard<std::mutex> lock(write_mutex);
            watchdog_stop = true;
        }
        write_cv.notify_all();
        if (watchdog.joinable()) {
            watchdog.join();
        }
    }

    /**
     * @brief 看门狗主循环。
     *
     * @details 5 ms 轮询而不是精确 `wait_until`：实现简单、对停止请求响应快，
     *          代价是每次超时最多多等 5 ms，对 30 s 默认值可忽略。发现超时后
     *          先把槽位标成 `timed_out`，再由**看门狗**关闭目的端；写线程在下一次
     *          `EndWrite` 看到标志后走失败路径，保证 `timeouts` 只加一次。
     */
    void WatchdogLoop();

    /// 取消路径：强制关闭两端，让阻塞中的读/写尽快返回。
    void ForceCloseAll();
};

}  // namespace detail

/**
 * @brief 单个方向的有界队列（按字节计容量）。
 *
 * @details 生产者阻塞等待腾空间；队列持续满、且一个队列容量的数据在
 *          `stall_timeout` 内没有排空时返回 `kStalled`，由读线程按「慢 peer」
 *          处理。消费者在 `Close()` 后仍可把剩余 chunk 排干（用于 EOF 后的
 *          冲刷），队列空后才返回 false。
 */
class ChunkQueue final {
public:
    /// `Push` 的三种结果。
    enum class PushResult {
        kOk,       ///< 入队成功。
        kClosed,   ///< 队列已关闭（取消/方向失败），chunk 未入队。
        kStalled,  ///< 持续满且一个容量未在预算内排空，chunk 未入队。
    };

    ChunkQueue(std::size_t capacity, std::chrono::milliseconds stall_timeout)
        : m_capacity(capacity), m_stall_timeout(stall_timeout) {}

    /// 入队；队列满时按字节等待，绝不无界增长。
    PushResult Push(std::vector<char> chunk) {
        const std::size_t bytes = chunk.size();
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_closed) {
            return PushResult::kClosed;
        }
        bool waited = false;
        while (m_bytes + bytes > m_capacity) {
            if (m_closed) {
                return PushResult::kClosed;
            }
            waited = true;
            const TimePoint now = Clock::now();
            if (!m_stall_active) {
                m_stall_active = true;
                m_stall_start = now;
                m_drained_since_stall = 0;
            }
            // 健康但较慢的链路会持续把队列排空：只要一个「队列容量」在
            // write_timeout 内被消费掉，就认为 peer 仍在推进，重置预算。
            // 这样 100 Mbps 的长传不会因为队列一直满而被误杀。
            if (m_drained_since_stall >= m_capacity) {
                m_drained_since_stall = 0;
                m_stall_start = now;
            }
            // 判据是「持续满且一个队列容量在预算内没有排空」，而不是单次等待
            // 时长：慢 peer 每 30 ms 只腾出一小块也会让单次等待结束，但吞吐
            // 不足以在一个预算内清空队列，仍应被判定为慢 peer。
            if (now - m_stall_start >= m_stall_timeout) {
                return PushResult::kStalled;
            }
            m_not_full.wait_until(lock, m_stall_start + m_stall_timeout);
        }
        if (!waited) {
            // 本次入队没有等待，说明消费者跟得上，清除持续满计时。
            m_stall_active = false;
            m_drained_since_stall = 0;
        }
        m_bytes += bytes;
        m_chunks.push_back(std::move(chunk));
        m_not_empty.notify_one();
        return PushResult::kOk;
    }

    /// 出队；关闭且队列为空时返回 false。
    bool Pop(std::vector<char>* chunk) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_not_empty.wait(lock, [this] { return m_closed || !m_chunks.empty(); });
        if (m_chunks.empty()) {
            return false;
        }
        *chunk = std::move(m_chunks.front());
        m_chunks.pop_front();
        m_bytes -= chunk->size();
        if (m_stall_active) {
            m_drained_since_stall += chunk->size();
        }
        if (m_chunks.empty()) {
            // 队列排空说明消费者跟得上，持续满计时清零。
            m_stall_active = false;
            m_drained_since_stall = 0;
        }
        m_not_full.notify_all();
        return true;
    }

    /// 关闭队列（幂等）；唤醒所有等待者。
    void Close() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_not_empty.notify_all();
        m_not_full.notify_all();
    }

    /// 丢弃全部剩余 chunk 并返回字节数；用于失败路径统计 `dropped`。
    std::size_t DiscardAll() {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::size_t bytes = m_bytes;
        m_chunks.clear();
        m_bytes = 0;
        m_stall_active = false;
        m_drained_since_stall = 0;
        m_not_full.notify_all();
        return bytes;
    }

private:
    /// 最大字节容量；构造后不变。
    const std::size_t m_capacity;
    /// 队列停滞预算；构造后不变。
    const std::chrono::milliseconds m_stall_timeout;

    /// 保护以下全部字段。
    std::mutex m_mutex;
    /// 有 chunk 可出队。
    std::condition_variable m_not_empty;
    /// 有空间可入队。
    std::condition_variable m_not_full;
    /// FIFO chunk 队列。
    std::deque<std::vector<char>> m_chunks;
    /// 当前已入队字节数；与 `m_chunks` 总和严格一致。
    std::size_t m_bytes{0};
    /// 是否处于「持续满」状态；由 `Push`/`Pop`/`DiscardAll` 维护。
    bool m_stall_active{false};
    /// 持续满的起始时刻；`m_stall_active` 为真时有效。
    TimePoint m_stall_start{};
    /// 本次持续满期间消费者已排空的字节数；达到 `m_capacity` 即重置预算。
    std::size_t m_drained_since_stall{0};
    /// 是否已关闭；关闭后不再接受新 chunk，但仍允许排干。
    bool m_closed{false};
};

/**
 * @brief 一个方向的读/写泵。
 *
 * @details 读线程：`Read` → `Push`；写线程：`Pop` → 循环 `Write` 直到 chunk
 *          写完。两者共享队列与取消标志，但错误只由首个失败者记录。
 */
class Direction final {
public:
    Direction(detail::RelayPipeState* state, Stream* from, Stream* to, int index)
        : m_state(state),
          m_from(from),
          m_to(to),
          m_index(index),
          m_label(DirectionLabel(index)),
          m_queue(state->cfg.queue_cap, state->cfg.write_timeout) {}

    ~Direction() { Join(); }

    Direction(const Direction&) = delete;
    Direction& operator=(const Direction&) = delete;

    /// 启动泵线程：直通模式每方向一条，否则读/写各一条。
    void Start() {
        if (m_state->cfg.direct) {
            m_reader = std::thread([this] { DirectLoop(); });
            return;
        }
        m_reader = std::thread([this] { ReaderLoop(); });
        m_writer = std::thread([this] { WriterLoop(); });
    }

    /// 回收线程；可重复调用。
    void Join() {
        if (m_reader.joinable()) {
            m_reader.join();
        }
        if (m_writer.joinable()) {
            m_writer.join();
        }
    }

    /// 目的端（写超时时由看门狗关闭）。
    Stream* Destination() const noexcept { return m_to; }

    /// 本方向是否干净 EOF（两个线程都已退出后读取）。
    bool Clean() const noexcept { return m_clean; }

    /// 本方向首个错误（两个线程都已退出后读取）。
    ErrorPtr Error() const {
        std::lock_guard<std::mutex> lock(m_error_mutex);
        return m_error;
    }

    /// 报告一次 deadline 事件；CAS 保证每个方向最多计入一次 `timeouts`。
    void NoteTimeout() {
        bool expected = false;
        if (m_timeout_noted.compare_exchange_strong(expected, true,
                                                    std::memory_order_relaxed)) {
            m_state->timeouts.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /**
     * @brief 取消路径：关闭两端并唤醒所有阻塞点。
     * @details 与 `Abort` 不同，即使已经 abort 也会再次 Close（幂等），保证
     *          `Run` 的取消路径一定能打断可能仍在阻塞的 `Read`/`Write`。
     */
    void ForceStop() {
        {
            std::lock_guard<std::mutex> lock(m_control_mutex);
            m_aborting.store(true, std::memory_order_release);
        }
        SetError(NewError(m_label + ": canceled"));
        m_queue.Close();
        if (m_from != nullptr) {
            m_from->Close();
        }
        if (m_to != nullptr) {
            m_to->Close();
        }
    }

private:
    /// 读线程当前是否正阻塞在 `Read` 中；决定 abort 时是否需要关闭源端。
    enum class ReaderPhase {
        kIdle,
        kReading,
    };

    /**
     * @brief 首个失败者生效的 abort。
     * @param error 失败原因；已被更早的 abort 记录时忽略。
     * @param close_destination 是否关闭目的端（写失败/超时要关；读失败不关）。
     */
    void Abort(ErrorPtr error, bool close_destination) {
        bool close_source = false;
        {
            std::lock_guard<std::mutex> lock(m_control_mutex);
            if (m_aborting.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            close_source = (m_phase == ReaderPhase::kReading);
        }
        SetError(std::move(error));
        m_queue.Close();
        if (close_destination && m_to != nullptr) {
            m_to->Close();
        }
        // 读线程若正阻塞在 Read，只有关闭源端才能把它收回来；此时源端本身
        // 已属于被放弃的方向，不会影响另一条泵。
        if (close_source && m_from != nullptr) {
            m_from->Close();
        }
    }

    /// 只记录第一个错误；后续错误不覆盖，便于定位根因。
    void SetError(ErrorPtr error) {
        if (!error) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_error_mutex);
        if (!m_error) {
            m_error = std::move(error);
        }
    }

    /**
     * @brief 直通模式的单线程泵：读到就写，不经过队列。
     *
     * @details 与 `ReaderLoop + WriterLoop` 的产物完全等价（统计、错误、EOF、
     *          超时、取消语义一致），区别只在于没有中间队列与写线程：
     * - 读缓冲直接作为 `WriteChunk` 的输入，不构造 chunk、不入队、不唤醒；
     * - 背压 = 目的端写阻塞，读线程自然停在 `Read` 之前，不会无界积压；
     * - 写 deadline 仍由 `BeginWrite/EndWrite` + 看门狗维护，慢 peer 只关
     *   本方向的目的端，另一方向继续。
     *
     * ## 并发/失败语义
     * - 只在 `Start()` 创建的唯一线程里运行；
     * - `m_phase` 在读前后切换，使 `Abort` 能判断「是否正阻塞在 Read」；
     * - 任一失败路径都经 `Abort` 记录首个错误并结束本方向，另一方向独立。
     */
    void DirectLoop() {
        std::vector<char> buffer(m_state->cfg.copy_buf);
        int empty_reads = 0;
        while (true) {
            {
                std::lock_guard<std::mutex> lock(m_control_mutex);
                if (m_aborting.load(std::memory_order_acquire)) {
                    break;
                }
                m_phase = ReaderPhase::kReading;
            }
            const streams::ReadResult result =
                m_from->Read(buffer.data(), buffer.size());
            bool aborted = false;
            {
                std::lock_guard<std::mutex> lock(m_control_mutex);
                m_phase = ReaderPhase::kIdle;
                aborted = m_aborting.load(std::memory_order_acquire);
            }
            if (result.count > buffer.size()) {
                SetError(NewError(m_label + " read returned too many bytes"));
                break;
            }
            if (result.count > 0) {
                empty_reads = 0;
                if (aborted) {
                    m_state->AddDropped(result.count);
                    break;
                }
                // 直通：读缓冲直接写目的端，无 chunk 分配、无队列、无接力。
                if (!WriteChunk(buffer.data(), result.count)) {
                    break;
                }
            }
            if (result.error) {
                if (Is(result.error, streams::IoEof())) {
                    m_reader_clean = true;
                } else {
                    SetError(Wrap(result.error, m_label + " read"));
                }
                break;
            }
            if (result.count == 0) {
                if (++empty_reads >= kMaxNoProgress) {
                    SetError(NewError(m_label + " read made no progress"));
                    break;
                }
                std::this_thread::yield();
            }
            if (aborted) {
                break;
            }
        }
        if (!m_aborting.load(std::memory_order_acquire)) {
            m_clean = m_reader_clean;
        }
        m_state->OnDirectionDone();
    }

    /// 读线程主循环。
    void ReaderLoop() {
        std::vector<char> buffer(m_state->cfg.copy_buf);
        int empty_reads = 0;
        while (true) {
            {
                std::lock_guard<std::mutex> lock(m_control_mutex);
                if (m_aborting.load(std::memory_order_acquire)) {
                    break;
                }
                m_phase = ReaderPhase::kReading;
            }
            const streams::ReadResult result = m_from->Read(buffer.data(), buffer.size());
            bool aborted = false;
            {
                std::lock_guard<std::mutex> lock(m_control_mutex);
                m_phase = ReaderPhase::kIdle;
                aborted = m_aborting.load(std::memory_order_acquire);
            }
            if (result.count > buffer.size()) {
                // Stream 违约：不能拿超出缓冲的 count 去构造 chunk。
                SetError(NewError(m_label + " read returned too many bytes"));
                break;
            }
            if (result.count > 0) {
                empty_reads = 0;
                if (aborted) {
                    m_state->AddDropped(result.count);
                    break;
                }
                const auto take = static_cast<std::ptrdiff_t>(result.count);
                std::vector<char> chunk(buffer.begin(), buffer.begin() + take);
                const ChunkQueue::PushResult pushed = m_queue.Push(std::move(chunk));
                if (pushed == ChunkQueue::PushResult::kStalled) {
                    NoteTimeout();
                    m_state->AddDropped(result.count);
                    Abort(TimeoutError(m_label + " queue stalled"), true);
                    break;
                }
                if (pushed == ChunkQueue::PushResult::kClosed) {
                    m_state->AddDropped(result.count);
                    break;
                }
            }
            if (result.error) {
                if (Is(result.error, streams::IoEof())) {
                    m_reader_clean = true;
                } else {
                    SetError(Wrap(result.error, m_label + " read"));
                }
                break;
            }
            if (result.count == 0) {
                if (++empty_reads >= kMaxNoProgress) {
                    SetError(NewError(m_label + " read made no progress"));
                    break;
                }
                std::this_thread::yield();
            }
            if (aborted) {
                break;
            }
        }
        m_queue.Close();
    }

    /// 写线程主循环：排空队列，失败时丢弃剩余数据并统计 dropped。
    void WriterLoop() {
        bool failed = false;
        while (!failed) {
            if (m_aborting.load(std::memory_order_acquire)) {
                m_state->AddDropped(m_queue.DiscardAll());
                break;
            }
            std::vector<char> chunk;
            if (!m_queue.Pop(&chunk)) {
                break;
            }
            if (m_aborting.load(std::memory_order_acquire)) {
                m_state->AddDropped(chunk.size());
                m_state->AddDropped(m_queue.DiscardAll());
                failed = true;
                break;
            }
            if (!WriteChunk(chunk.data(), chunk.size())) {
                failed = true;
                break;
            }
        }
        if (!m_aborting.load(std::memory_order_acquire)) {
            m_clean = m_reader_clean;
        }
        m_state->OnDirectionDone();
    }

    /**
     * @brief 把一段字节写满目的端；失败时未写部分与队列剩余计入 dropped。
     *
     * @param data [in] 待写字节；调用期间有效，本函数不保留指针。
     * @param size [in] 字节数；0 直接返回 true。
     * @return 全部写出返回 true；超时/写错误/取消返回 false（已记录首个错误）。
     *
     * ## 并发/失败语义
     * - 只在所属方向的泵线程调用；`m_to` 由看门狗在超时时并发 `Close()`；
     * - 每次底层 `Write` 前刷新 deadline，chunk 内多次短写共用同一 deadline；
     * - 失败后本方向 abort，队列剩余由 `DiscardAll` 计为 dropped。
     */
    bool WriteChunk(const char* data, std::size_t size) {
        const TimePoint deadline = Clock::now() + m_state->cfg.write_timeout;
        std::size_t offset = 0;
        int no_progress = 0;
        while (offset < size) {
            if (m_aborting.load(std::memory_order_acquire)) {
                m_state->AddDropped(size - offset);
                m_state->AddDropped(m_queue.DiscardAll());
                return false;
            }
            if (Clock::now() >= deadline) {
                NoteTimeout();
                Abort(TimeoutError(m_label + " write"), true);
                m_state->AddDropped(size - offset);
                m_state->AddDropped(m_queue.DiscardAll());
                return false;
            }
            m_state->BeginWrite(m_index, deadline);
            const streams::WriteResult result =
                m_to->Write(data + offset, size - offset);
            const bool timed_out = m_state->EndWrite(m_index);
            if (result.count > 0) {
                if (result.count > size - offset) {
                    Abort(NewError(m_label + " write returned too many bytes"), true);
                    m_state->AddDropped(size - offset);
                    m_state->AddDropped(m_queue.DiscardAll());
                    return false;
                }
                offset += result.count;
                m_state->AddDelivered(m_index, result.count);
                no_progress = 0;
            }
            if (timed_out) {
                // 看门狗已关闭目的端并计入 timeouts；这里补上方向级 abort，
                // 让 Run 返回失败（否则读端已 EOF，可能被误判成干净结束）。
                Abort(TimeoutError(m_label + " write"), true);
                m_state->AddDropped(size - offset);
                m_state->AddDropped(m_queue.DiscardAll());
                return false;
            }
            if (result.error) {
                Abort(Wrap(result.error, m_label + " write"), true);
                m_state->AddDropped(size - offset);
                m_state->AddDropped(m_queue.DiscardAll());
                return false;
            }
            if (result.count == 0) {
                if (++no_progress >= kMaxNoProgress) {
                    Abort(NewError(m_label + " write made no progress"), true);
                    m_state->AddDropped(size - offset);
                    m_state->AddDropped(m_queue.DiscardAll());
                    return false;
                }
                std::this_thread::yield();
            }
        }
        return true;
    }

    /// 共享状态（不拥有）。
    detail::RelayPipeState* m_state;
    /// 源端（不拥有）。
    Stream* m_from;
    /// 目的端（不拥有）；超时时被看门狗关闭。
    Stream* m_to;
    /// 0 = A→B，1 = B→A；决定统计字段。
    int m_index;
    /// 错误标签。
    std::string m_label;

    /// 有界队列。
    ChunkQueue m_queue;

    /// 保护 `m_phase` 与 `m_aborting` 的写入顺序。
    std::mutex m_control_mutex;
    /// 读线程当前阶段；Abort 据此决定是否关闭源端。
    ReaderPhase m_phase{ReaderPhase::kIdle};
    /// 是否已中止；快速路径读。
    std::atomic<bool> m_aborting{false};
    /// 是否已经计入过 timeout。
    std::atomic<bool> m_timeout_noted{false};

    /// 保护 `m_error`；`Error()` 是 const 查询，故 mutable。
    mutable std::mutex m_error_mutex;
    /// 首个错误。
    ErrorPtr m_error;

    /// 读线程是否看到干净 EOF；写线程在队列排干后读取。
    bool m_reader_clean{false};
    /// 本方向是否干净结束；写线程退出前写入。
    bool m_clean{false};

    std::thread m_reader;
    std::thread m_writer;
};

// ============================================================================
// RelayPipeState（依赖完整的 Direction）
// ============================================================================

namespace detail {

RelayPipeState::~RelayPipeState() = default;

void RelayPipeState::WatchdogLoop() {
    std::unique_lock<std::mutex> lock(write_mutex);
    while (!watchdog_stop) {
        std::vector<Stream*> expired;
        const TimePoint now = Clock::now();
        for (int index = 0; index < 2; ++index) {
            WriteSlot& slot = slots[index];
            if (!slot.active || slot.timed_out || slot.deadline > now) {
                continue;
            }
            slot.timed_out = true;
            if (directions[index]) {
                directions[index]->NoteTimeout();
                expired.push_back(directions[index]->Destination());
            }
        }
        lock.unlock();
        for (Stream* stream : expired) {
            if (stream != nullptr) {
                stream->Close();
            }
        }
        lock.lock();
        write_cv.wait_for(lock, kWatchdogTick, [this] { return watchdog_stop; });
    }
}

void RelayPipeState::ForceCloseAll() {
    for (auto& direction : directions) {
        if (direction) {
            direction->ForceStop();
        }
    }
}

}  // namespace detail

// ============================================================================
// RelayPipe
// ============================================================================

RelayPipe::RelayPipe(Stream* a, Stream* b, PipeConfig cfg)
    : m_state(std::make_shared<detail::RelayPipeState>(a, b, cfg)) {
    m_state->directions[0] =
        std::make_unique<Direction>(m_state.get(), a, b, 0);
    m_state->directions[1] =
        std::make_unique<Direction>(m_state.get(), b, a, 1);
}

RelayPipe::~RelayPipe() = default;

bool RelayPipe::Run(const ContextPtr& context, ErrorPtr* err) {
    if (err != nullptr) {
        *err = {};
    }
    detail::RelayPipeState& state = *m_state;

    if (state.a == nullptr || state.b == nullptr) {
        if (err != nullptr) {
            *err = NewError("relay: null stream");
        }
        return false;
    }

    bool expected = false;
    if (!state.run_started.compare_exchange_strong(expected, true)) {
        if (err != nullptr) {
            *err = NewError("relay: Run already called");
        }
        return false;
    }

    const ContextPtr ctx = context ? context : Context::Background();
    if (ctx->IsDone()) {
        state.ForceCloseAll();
        const ErrorPtr cause = ctx->Err();
        if (err != nullptr) {
            *err = cause ? cause : NewError("relay: canceled");
        }
        return false;
    }

    // 取消回调只捕获 state（shared_ptr），不捕获 RelayPipe，也不访问 Stream：
    // 即使回调在 Run 返回后才执行，也只会碰原子标志与条件变量。
    const std::shared_ptr<detail::RelayPipeState> keep = m_state;
    const DoneSignal::CallbackId callback_id = ctx->Done().AddCallback([keep] {
        {
            std::lock_guard<std::mutex> lock(keep->done_mutex);
            keep->cancel_requested.store(true, std::memory_order_release);
        }
        keep->done_cv.notify_all();
    });

    state.StartWatchdog();
    state.directions[0]->Start();
    state.directions[1]->Start();

    bool incomplete = false;
    {
        std::unique_lock<std::mutex> lock(state.done_mutex);
        state.done_cv.wait(lock, [&state] {
            return state.directions_done == 2 ||
                   state.cancel_requested.load(std::memory_order_acquire);
        });
        incomplete = state.directions_done < 2;
    }
    if (incomplete && state.cancel_requested.load(std::memory_order_acquire)) {
        state.ForceCloseAll();
    }

    state.directions[0]->Join();
    state.directions[1]->Join();
    state.StopWatchdog();
    ctx->Done().RemoveCallback(callback_id);

    bool all_clean = true;
    ErrorPtr first_error;
    for (auto& direction : state.directions) {
        if (!direction->Clean()) {
            all_clean = false;
        }
        if (!first_error) {
            first_error = direction->Error();
        }
    }

    const bool canceled = state.cancel_requested.load(std::memory_order_acquire);
    if (canceled && (incomplete || !all_clean)) {
        const ErrorPtr cause = ctx->Err();
        if (err != nullptr) {
            *err = cause ? cause : NewError("relay: canceled");
        }
        return false;
    }
    if (all_clean) {
        return true;
    }
    if (err != nullptr) {
        *err = first_error ? first_error : NewError("relay: aborted");
    }
    return false;
}

PipeStats RelayPipe::Stats() const {
    const detail::RelayPipeState& state = *m_state;
    PipeStats stats;
    stats.a_to_b = state.a_to_b.load(std::memory_order_relaxed);
    stats.b_to_a = state.b_to_a.load(std::memory_order_relaxed);
    stats.dropped = state.dropped.load(std::memory_order_relaxed);
    stats.timeouts = state.timeouts.load(std::memory_order_relaxed);
    return stats;
}

const PipeConfig& RelayPipe::Config() const noexcept { return m_state->cfg; }

}  // namespace relay
}  // namespace go2cpp
