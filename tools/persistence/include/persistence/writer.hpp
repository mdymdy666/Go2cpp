#pragma once

/**
 * @file writer.hpp
 * @brief 持久化原型：攒批 / 每线程缓冲 + 后台落盘 / 段文件 + fsync 策略。
 *
 * 设计目标（详见 docs/persistence-benchmark.md）：
 *   1. **减少 syscall**：事件先进 256 KiB–1 MiB 的批量缓冲，满批才 `write`；
 *   2. **解耦 IO 与生产者**：每个生产者线程持有独立 batch，封批后挂到
 *      MPSC 环形队列，由唯一后台线程顺序 `write` + `fdatasync`；
 *   3. **批量级排序**：slot 与 seq 区间在同一把极短自旋锁内分配，
 *      因此「文件顺序 == seq 顺序」；事件级无锁、无全局互斥量；
 *   4. **持久化语义**：
 *      - `kNone`：只写页缓存，不保证掉电持久；
 *      - `kBatch`：累计 `sync_bytes` 后 fdatasync；
 *      - `kInterval`：后台线程每 `interval_ms` fdatasync（空闲时也会触发）；
 *   5. **段文件（可选）**：每 `segment_bytes` 滚动一个不可变段文件，
 *      结束时写 manifest（段号、字节数、seq/时间范围），读取端按
 *      seq/时间范围二分定位段，再顺序解码。
 *
 * 边界：
 * - `Finish()` 必须在所有生产者线程停止调用 `Append()` 之后调用；
 * - 段文件是**裸记录流**（不含 G2CRPLY 文件头/kEnd），manifest 描述会话；
 *   单文件引擎仍写标准 G2CRPLY 头 + kMeta + kEnd；
 * - 每批按 `batch_bytes / 112` 预留 seq 区间，批未填满会留下 seq 空洞
 *   （文件顺序仍严格递增；回放端只依赖文件顺序与单调 seq）。
 */

#include "persistence/encoder.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace go2cpp::persistence {

/// 写入引擎。
enum class Engine {
    kBuffered,   ///< 单缓冲 + 全局互斥量（同步写，作为对照）
    kMpsc,       ///< 每线程 batch + 后台线程（单文件）
    kSegmented,  ///< 每线程 batch + 后台线程 + 段文件 + manifest
};

/// fsync 策略。
enum class SyncPolicy {
    kNone,
    kBatch,
    kInterval,
};

struct WriterOptions {
    std::string path{"/tmp/go2cpp_persistence"};
    Engine engine{Engine::kMpsc};
    SyncPolicy sync{SyncPolicy::kInterval};
    std::size_t batch_bytes{256U * 1024U};
    std::size_t sync_bytes{4U * 1024U * 1024U};
    std::uint32_t interval_ms{100};
    std::size_t segment_bytes{64U * 1024U * 1024U};
    std::uint32_t queue_batches{64};
    /// 每写满 writeback_bytes 调一次 sync_file_range(W RITE)，提前启动回写，
    /// 让后续 fdatasync 只需等待少量脏页（Linux 专有；其它平台自动跳过）。
    bool writeback_ahead{true};
    std::size_t writeback_bytes{8U * 1024U * 1024U};
};

/// 一个不可变段的元信息（同时写入 manifest，供读取端二分定位）。
struct SegmentInfo {
    std::uint32_t id{0};
    std::uint64_t bytes{0};
    std::uint64_t first_seq{0};
    std::uint64_t last_seq{0};
    std::uint64_t count{0};
    std::uint64_t first_ts{0};
    std::uint64_t last_ts{0};
};

struct WriterStats {
    std::uint64_t events{0};
    std::uint64_t bytes{0};
    std::uint64_t batches{0};
    std::uint64_t fsyncs{0};
    std::uint64_t fsync_p50_ns{0};
    std::uint64_t fsync_p99_ns{0};
    std::uint64_t max_sync_gap_ns{0};
    std::uint64_t logical_checksum{0};
};

class EventWriter;

/// 生产者句柄：每个调用 Append 的线程持有自己的 Producer。
class Producer {
public:
    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;
    ~Producer();

    std::uint32_t id() const noexcept { return m_id; }

private:
    friend class EventWriter;
    friend class BufferedWriter;
    friend class AsyncWriter;
    Producer(EventWriter* owner, std::uint32_t id) : m_owner(owner), m_id(id) {}
    EventWriter* m_owner{nullptr};
    std::uint32_t m_id{0};
    void* m_batch{nullptr};
    std::vector<std::uint8_t> m_scratch =
        std::vector<std::uint8_t>(64U * 1024U);
    std::uint64_t m_checksum{0};
};

class EventWriter {
public:
    virtual ~EventWriter() = default;

    /// 按 options 创建引擎；失败返回 nullptr 并写 `error`。
    static std::unique_ptr<EventWriter> Create(const WriterOptions& options,
                                               std::string* error);

    /// 创建一个生产者句柄（线程安全；通常每线程一次）。
    virtual Producer* NewProducer() = 0;

    /// 追加一条事件；返回 false 表示已发生不可恢复错误。
    virtual bool Append(Producer* producer,
                        go2cpp::replay::Event event) = 0;

    /// 封批、排空队列、写 kEnd/manifest、执行最终 fsync。
    virtual bool Finish(std::string* error) = 0;

    /// 统计（Finish 之后调用；未 Finish 时数值为当前快照）。
    virtual WriterStats Stats() const = 0;

    /// 段列表（单文件引擎返回 0 或 1 个伪段）。
    virtual const std::vector<SegmentInfo>& Segments() const = 0;

    virtual bool ok() const noexcept = 0;

    const WriterOptions& options() const noexcept { return m_options; }

protected:
    explicit EventWriter(WriterOptions options)
        : m_options(std::move(options)) {}
    WriterOptions m_options;
};

const char* EngineName(Engine engine) noexcept;
const char* SyncPolicyName(SyncPolicy policy) noexcept;

}  // namespace go2cpp::persistence
