/**
 * @file writer.cpp
 * @brief 持久化原型写入引擎实现。
 */

#include "persistence/writer.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace go2cpp::persistence {
namespace {

constexpr std::uint64_t kSeqGap = 0;

std::uint64_t NowNs() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

std::string ErrnoText(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

bool EnsureParent(const std::string& path, std::string* error) {
    const std::filesystem::path parent =
        std::filesystem::path(path).parent_path();
    if (parent.empty()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        *error = "create_directories(" + parent.string() + "): " + ec.message();
        return false;
    }
    return true;
}

bool WriteAll(int fd, const void* data, std::size_t size,
              std::string* error) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(fd, bytes + offset, size - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            *error = ErrnoText("write");
            return false;
        }
        if (written == 0) {
            *error = "write 返回 0";
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool FsyncFile(int fd, std::string* error) {
    if (fd < 0) {
        return true;
    }
    if (::fdatasync(fd) != 0) {
        *error = ErrnoText("fdatasync");
        return false;
    }
    return true;
}

std::string SegmentPath(const std::string& base, std::uint32_t id) {
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), ".seg-%06u", id);
    return base + suffix;
}

std::string ManifestPath(const std::string& base) {
    return base + ".manifest";
}

/// 极短临界区自旋锁：只在「分配 batch slot + seq 区间」时持有。
class SpinLock {
public:
    void Lock() noexcept {
        while (m_flag.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#else
            std::this_thread::yield();
#endif
        }
    }
    void Unlock() noexcept { m_flag.clear(std::memory_order_release); }

private:
    std::atomic_flag m_flag = ATOMIC_FLAG_INIT;
};

class SpinGuard {
public:
    explicit SpinGuard(SpinLock& lock) : m_lock(lock) { m_lock.Lock(); }
    ~SpinGuard() { m_lock.Unlock(); }
    SpinGuard(const SpinGuard&) = delete;
    SpinGuard& operator=(const SpinGuard&) = delete;

private:
    SpinLock& m_lock;
};

struct Batch {
    explicit Batch(std::size_t capacity_bytes)
        : data(capacity_bytes), capacity(capacity_bytes) {
        data.resize(capacity_bytes);
    }
    std::vector<std::uint8_t> data;
    std::size_t used{0};
    std::size_t capacity{0};
    std::uint32_t count{0};
    std::uint32_t slot{0};
    std::uint64_t base{0};
    std::uint64_t first_ts{0};
    std::uint64_t last_ts{0};
    bool meta_batch{false};
    std::atomic<bool> ready{false};
};

class BatchPool {
public:
    BatchPool(std::size_t capacity, std::size_t preallocate)
        : m_capacity(capacity) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (std::size_t i = 0; i < preallocate; ++i) {
            m_free.push_back(std::make_unique<Batch>(capacity));
        }
    }

    Batch* Acquire(std::size_t capacity) {
        if (capacity <= m_capacity) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_free.empty()) {
                std::unique_ptr<Batch> batch = std::move(m_free.back());
                m_free.pop_back();
                batch->used = 0;
                batch->count = 0;
                batch->first_ts = 0;
                batch->last_ts = 0;
                batch->meta_batch = false;
                batch->ready.store(false, std::memory_order_relaxed);
                return batch.release();
            }
        }
        return new Batch(capacity);
    }

    void Release(Batch* batch) {
        if (batch == nullptr) {
            return;
        }
        if (batch->capacity != m_capacity) {
            delete batch;
            return;
        }
        batch->used = 0;
        batch->count = 0;
        batch->meta_batch = false;
        batch->ready.store(false, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_free.emplace_back(batch);
    }

private:
    std::size_t m_capacity;
    std::mutex m_mutex;
    std::vector<std::unique_ptr<Batch>> m_free;
};

std::pair<std::uint64_t, std::uint64_t> Percentiles(
    const std::vector<std::uint32_t>& samples) {
    if (samples.empty()) {
        return {0, 0};
    }
    std::vector<std::uint32_t> copy = samples;
    std::sort(copy.begin(), copy.end());
    const auto pick = [&copy](double q) {
        const std::size_t index = static_cast<std::size_t>(
            q * static_cast<double>(copy.size() - 1));
        return static_cast<std::uint64_t>(copy[index]);
    };
    return {pick(0.50), pick(0.99)};
}

void FillEventHeaderFields(go2cpp::replay::Event* event,
                           std::uint64_t start_ns) {
    if (event->ts_ns == 0) {
        event->ts_ns = NowNs();
    }
    if (event->vtime_ns == 0) {
        event->vtime_ns = event->ts_ns > start_ns ? event->ts_ns - start_ns : 0;
    }
}

}  // namespace

const char* EngineName(Engine engine) noexcept {
    switch (engine) {
        case Engine::kBuffered: return "buffered";
        case Engine::kMpsc: return "mpsc";
        case Engine::kSegmented: return "segmented";
    }
    return "unknown";
}

const char* SyncPolicyName(SyncPolicy policy) noexcept {
    switch (policy) {
        case SyncPolicy::kNone: return "none";
        case SyncPolicy::kBatch: return "batch";
        case SyncPolicy::kInterval: return "interval";
    }
    return "unknown";
}

Producer::~Producer() = default;

// ---------------------------------------------------------------------------
// BufferedWriter：单缓冲 + 全局互斥量（同步 write，作为对照）
// ---------------------------------------------------------------------------
class BufferedWriter final : public EventWriter {
public:
    explicit BufferedWriter(WriterOptions options)
        : EventWriter(std::move(options)) {
        std::string error;
        if (!EnsureParent(m_options.path, &error)) {
            SetError(error);
            return;
        }
        m_fd = ::open(m_options.path.c_str(),
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (m_fd < 0) {
            SetError(ErrnoText("open"));
            return;
        }
        go2cpp::replay::FileHeader header;
        header.created_unix_ns = static_cast<std::uint64_t>(
            std::chrono::system_clock::now().time_since_epoch().count());
        header.producer_id = 0x67326372706C79ULL;
        std::vector<std::uint8_t> encoded;
        go2cpp::replay::EncodeFileHeader(header, encoded);
        if (!WriteAll(m_fd, encoded.data(), encoded.size(), &error)) {
            SetError(error);
            return;
        }
        m_bytes += encoded.size();
        m_start_ns = NowNs();

        go2cpp::replay::Event meta = MakeMetaEvent();
        meta.seq = 0;
        m_seq = 1;
        encoded.clear();
        go2cpp::replay::EncodeEvent(meta, encoded);
        if (!WriteAll(m_fd, encoded.data(), encoded.size(), &error)) {
            SetError(error);
            return;
        }
        m_bytes += encoded.size();
        m_seg.id = 0;
        m_seg.first_seq = 0;
        m_seg.last_ts = meta.ts_ns;
    }

    ~BufferedWriter() override {
        std::string ignored;
        Finish(&ignored);
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

    Producer* NewProducer() override {
        std::lock_guard<std::mutex> lock(m_producers_mutex);
        m_producers.push_back(std::unique_ptr<Producer>(
            new Producer(this, m_next_producer_id++)));
        return m_producers.back().get();
    }

    bool Append(Producer* producer, go2cpp::replay::Event event) override {
        (void)producer;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_fd < 0 || !m_error.empty()) {
            return false;
        }
        FillEventHeaderFields(&event, m_start_ns);
        event.seq = m_seq++;
        const std::size_t fixed = FixedRecordSize(event.type);
        const std::size_t record_size =
            fixed != 0 ? fixed : EncodeGeneric(event, m_scratch.data(),
                                               m_scratch.size());
        if (fixed != 0) {
            if (m_scratch.size() < fixed) {
                m_scratch.resize(fixed);
            }
            EncodeFixed(event, m_scratch.data());
        } else if (record_size == 0) {
            SetError("单条事件超出通用编码缓冲");
            return false;
        }
        if (m_buffer.size() + record_size > m_options.batch_bytes) {
            if (!FlushLocked()) {
                return false;
            }
        }
        m_buffer.insert(m_buffer.end(), m_scratch.data(),
                        m_scratch.data() + record_size);
        m_records += 1;
        m_checksum += MixValue(event);
        if (m_seg.count == 0) {
            m_seg.first_seq = event.seq;
            m_seg.first_ts = event.ts_ns;
        }
        m_seg.last_seq = event.seq;
        m_seg.last_ts = event.ts_ns;
        m_seg.count += 1;
        MaybeSyncLocked();
        return true;
    }

    bool Finish(std::string* error) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_finished) {
                if (error != nullptr) {
                    *error = m_error;
                }
                return m_error.empty();
            }
            m_finished = true;
            if (m_fd < 0) {
                if (error != nullptr) {
                    *error = m_error;
                }
                return m_error.empty();
            }
            if (!FlushLocked()) {
                if (error != nullptr) {
                    *error = m_error;
                }
                return false;
            }
            go2cpp::replay::Event end;
            end.type = go2cpp::replay::EventType::kEnd;
            end.seq = m_seq++;
            end.ts_ns = NowNs();
            end.value = static_cast<std::int64_t>(m_records);
            end.sched_type = 0;
            m_scratch.clear();
            go2cpp::replay::EncodeEvent(end, m_scratch);
            if (!WriteAll(m_fd, m_scratch.data(), m_scratch.size(), &m_error)) {
                if (error != nullptr) {
                    *error = m_error;
                }
                return false;
            }
            m_bytes += m_scratch.size();
            if (m_options.sync != SyncPolicy::kNone) {
                SyncLocked();
            }
            const auto p = Percentiles(m_fsync_latencies);
            m_fsync_p50 = p.first;
            m_fsync_p99 = p.second;
            m_seg.bytes = m_bytes;
            ::close(m_fd);
            m_fd = -1;
        }
        if (error != nullptr) {
            *error = m_error;
        }
        return m_error.empty();
    }

    WriterStats Stats() const override {
        WriterStats stats;
        stats.events = m_records;
        stats.bytes = m_bytes;
        stats.batches = m_batches;
        stats.fsyncs = m_fsyncs;
        stats.fsync_p50_ns = m_fsync_p50;
        stats.fsync_p99_ns = m_fsync_p99;
        stats.max_sync_gap_ns = m_max_sync_gap_ns;
        stats.logical_checksum = m_checksum;
        return stats;
    }

    const std::vector<SegmentInfo>& Segments() const override {
        return m_segments;
    }

    bool ok() const noexcept override { return m_error.empty(); }

private:
    static go2cpp::replay::Event MakeMetaEvent() {
        go2cpp::replay::Event meta;
        meta.type = go2cpp::replay::EventType::kMeta;
        meta.gid = 64;
        meta.channel = 4;
        meta.note = "persistence-proto";
        meta.ext_flags = go2cpp::replay::kMetaHasValueCodec;
        meta.clock_mode = go2cpp::replay::kClockVirtual;
        meta.ts_ns = NowNs();
        return meta;
    }

    void SetError(const std::string& error) {
        if (m_error.empty()) {
            m_error = error;
        }
    }

    bool FlushLocked() {
        if (m_buffer.empty()) {
            return true;
        }
        if (!WriteAll(m_fd, m_buffer.data(), m_buffer.size(), &m_error)) {
            return false;
        }
        m_bytes += m_buffer.size();
        m_unsynced_bytes += m_buffer.size();
        m_dirty = true;
        m_buffer.clear();
        ++m_batches;
        if (m_options.sync == SyncPolicy::kBatch &&
            m_unsynced_bytes >= m_options.sync_bytes) {
            SyncLocked();
        }
        return m_error.empty();
    }

    void MaybeSyncLocked() {
        if (m_options.sync != SyncPolicy::kInterval || !m_dirty) {
            return;
        }
        const std::uint64_t now = NowNs();
        if (m_last_sync_ns != 0 &&
            now - m_last_sync_ns <
                static_cast<std::uint64_t>(m_options.interval_ms) * 1000000ULL) {
            return;
        }
        SyncLocked();
    }

    void SyncLocked() {
        if (!m_dirty && m_last_sync_ns != 0) {
            return;
        }
        const std::uint64_t begin = NowNs();
        if (!FsyncFile(m_fd, &m_error)) {
            return;
        }
        const std::uint64_t end = NowNs();
        m_fsync_latencies.push_back(static_cast<std::uint32_t>(
            std::min<std::uint64_t>(end - begin, 0xFFFFFFFFULL)));
        if (m_last_sync_ns != 0) {
            m_max_sync_gap_ns =
                std::max(m_max_sync_gap_ns, begin - m_last_sync_ns);
        }
        m_last_sync_ns = end;
        m_dirty = false;
        m_unsynced_bytes = 0;
        ++m_fsyncs;
    }

    int m_fd{-1};
    mutable std::mutex m_mutex;
    std::mutex m_producers_mutex;
    std::vector<std::unique_ptr<Producer>> m_producers;
    std::uint32_t m_next_producer_id{0};
    std::vector<std::uint8_t> m_buffer;
    std::vector<std::uint8_t> m_scratch = std::vector<std::uint8_t>(4096);
    std::vector<SegmentInfo> m_segments;
    SegmentInfo m_seg;
    std::vector<std::uint32_t> m_fsync_latencies;
    std::uint64_t m_seq{1};
    std::uint64_t m_records{0};
    std::uint64_t m_bytes{0};
    std::uint64_t m_batches{0};
    std::uint64_t m_checksum{0};
    std::uint64_t m_start_ns{0};
    std::uint64_t m_last_sync_ns{0};
    std::uint64_t m_max_sync_gap_ns{0};
    std::uint64_t m_unsynced_bytes{0};
    std::uint64_t m_writeback_mark{0};
    std::uint64_t m_fsyncs{0};
    std::uint64_t m_fsync_p50{0};
    std::uint64_t m_fsync_p99{0};
    std::string m_error;
    bool m_dirty{false};
    bool m_finished{false};
};

// ---------------------------------------------------------------------------
// AsyncWriter：每线程 batch + MPSC 环 + 单后台线程写/fsync（可选段文件）
// ---------------------------------------------------------------------------
class AsyncWriter final : public EventWriter {
public:
    explicit AsyncWriter(WriterOptions options)
        : EventWriter(std::move(options)) {}

    ~AsyncWriter() override {
        if (m_pool == nullptr) {
            return;
        }
        std::string ignored;
        Finish(&ignored);
    }

    bool Initialize() {
        if (m_options.batch_bytes < 4096) {
            SetError("batch_bytes 必须 >= 4096");
            return false;
        }
        if (m_options.queue_batches < 2) {
            SetError("queue_batches 必须 >= 2");
            return false;
        }
        m_segmented = m_options.engine == Engine::kSegmented;
        if (m_segmented && m_options.segment_bytes <= m_options.batch_bytes) {
            SetError("segment_bytes 必须大于 batch_bytes");
            return false;
        }
        m_max_events = std::max<std::size_t>(
            1, m_options.batch_bytes / kResV5RecordSize);
        m_queue_capacity = m_options.queue_batches;
        m_ring = std::vector<std::atomic<Batch*>>(m_queue_capacity);
        for (auto& slot : m_ring) {
            slot.store(nullptr, std::memory_order_relaxed);
        }
        m_pool = std::make_unique<BatchPool>(
            m_options.batch_bytes,
            static_cast<std::size_t>(m_options.queue_batches) + 4);
        std::string error;
        if (!EnsureParent(m_options.path, &error)) {
            SetError(error);
            return false;
        }
        m_start_ns = NowNs();
        if (m_segmented) {
            if (!OpenSegment(1)) {
                return false;
            }
        } else {
            m_fd = ::open(m_options.path.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (m_fd < 0) {
                SetError(ErrnoText("open"));
                return false;
            }
            go2cpp::replay::FileHeader header;
            header.created_unix_ns = static_cast<std::uint64_t>(
                std::chrono::system_clock::now().time_since_epoch().count());
            header.producer_id = 0x67326372706C79ULL;
            std::vector<std::uint8_t> encoded;
            go2cpp::replay::EncodeFileHeader(header, encoded);
            if (!WriteAll(m_fd, encoded.data(), encoded.size(), &error)) {
                SetError(error);
                return false;
            }
            m_bytes += encoded.size();
            m_current_seg_bytes += encoded.size();
        }
        // slot 0 固定给 kMeta：seq=0，先于所有生产者 batch。
        Batch* meta = m_pool->Acquire(m_options.batch_bytes);
        meta->slot = 0;
        meta->base = 0;
        go2cpp::replay::Event meta_event = MakeMetaEvent();
        const std::size_t size = EncodeGeneric(meta_event, meta->data.data(),
                                               meta->capacity);
        if (size == 0) {
            SetError("kMeta 编码失败");
            return false;
        }
        meta->used = size;
        meta->count = 1;
        meta->first_ts = meta_event.ts_ns;
        meta->last_ts = meta_event.ts_ns;
        meta->meta_batch = true;
        meta->ready.store(true, std::memory_order_release);
        m_ring[0].store(meta, std::memory_order_release);
        m_next_slot.store(1, std::memory_order_release);
        m_next_seq = 1;
        m_consumer = std::thread([this] { ConsumerLoop(); });
        return true;
    }

    Producer* NewProducer() override {
        std::lock_guard<std::mutex> lock(m_producers_mutex);
        m_producers.push_back(std::unique_ptr<Producer>(
            new Producer(this, m_next_producer_id++)));
        return m_producers.back().get();
    }

    bool Append(Producer* producer, go2cpp::replay::Event event) override {
        if (producer == nullptr || m_failed.load(std::memory_order_relaxed)) {
            return false;
        }
        FillEventHeaderFields(&event, m_start_ns);
        const std::size_t fixed = FixedRecordSize(event.type);
        std::size_t record_size = fixed;
        if (fixed == 0) {
            record_size = EncodeGeneric(event, producer->m_scratch.data(),
                                        producer->m_scratch.size());
            if (record_size == 0) {
                SetError("通用事件超过 64 KiB 生产者缓冲");
                return false;
            }
        }
        Batch* batch = static_cast<Batch*>(producer->m_batch);
        if (batch == nullptr ||
            batch->used + record_size > batch->capacity) {
            Seal(producer);
            batch = AcquireBatch(producer, record_size);
            if (batch == nullptr) {
                return false;
            }
        }
        event.seq = batch->base + batch->count;
        if (fixed != 0) {
            const std::size_t written =
                EncodeFixed(event, batch->data.data() + batch->used);
            if (written != fixed) {
                SetError("定长编码失败");
                return false;
            }
            record_size = written;
        } else {
            std::memcpy(batch->data.data() + batch->used,
                        producer->m_scratch.data(), record_size);
        }
        if (batch->count == 0) {
            batch->first_ts = event.ts_ns;
        }
        batch->last_ts = event.ts_ns;
        batch->used += record_size;
        batch->count += 1;
        producer->m_checksum += MixValue(event);
        if (batch->count >= m_max_events) {
            Seal(producer);
        }
        return true;
    }

    bool Finish(std::string* error) override {
        {
            std::lock_guard<std::mutex> lock(m_finish_mutex);
            if (m_finished) {
                if (error != nullptr) {
                    *error = m_error;
                }
                return m_error.empty();
            }
            m_finished = true;
        }
        for (auto& producer : m_producers) {
            Seal(producer.get());
        }
        m_stop.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(m_wait_mutex);
            m_wait_cv.notify_all();
        }
        if (m_consumer.joinable()) {
            m_consumer.join();
        }
        if (m_error.empty()) {
            FinalizeOutput();
        }
        m_checksum = m_meta_checksum;
        for (const auto& producer : m_producers) {
            m_checksum += producer->m_checksum;
        }
        const auto p = Percentiles(m_fsync_latencies);
        m_fsync_p50 = p.first;
        m_fsync_p99 = p.second;
        if (error != nullptr) {
            *error = m_error;
        }
        return m_error.empty();
    }

    WriterStats Stats() const override {
        WriterStats stats;
        stats.events = m_records;
        stats.bytes = m_bytes;
        stats.batches = m_batches;
        stats.fsyncs = m_fsyncs;
        stats.fsync_p50_ns = m_fsync_p50;
        stats.fsync_p99_ns = m_fsync_p99;
        stats.max_sync_gap_ns = m_max_sync_gap_ns;
        stats.logical_checksum = m_checksum;
        return stats;
    }

    const std::vector<SegmentInfo>& Segments() const override {
        return m_segments;
    }

    bool ok() const noexcept override { return m_error.empty(); }

private:
    static go2cpp::replay::Event MakeMetaEvent() {
        go2cpp::replay::Event meta;
        meta.type = go2cpp::replay::EventType::kMeta;
        meta.gid = 64;
        meta.channel = 4;
        meta.note = "persistence-proto";
        meta.ext_flags = go2cpp::replay::kMetaHasValueCodec;
        meta.clock_mode = go2cpp::replay::kClockVirtual;
        meta.ts_ns = NowNs();
        meta.vtime_ns = 0;
        return meta;
    }

    void SetError(const std::string& error) {
        m_failed.store(true, std::memory_order_release);
        if (m_error.empty()) {
            m_error = error;
        }
    }

    bool HasFailed() const noexcept {
        return m_failed.load(std::memory_order_relaxed);
    }

    Batch* AcquireBatch(Producer* producer, std::size_t capacity) {
        const std::size_t wanted = std::max(capacity, m_options.batch_bytes);
        for (;;) {
            if (HasFailed()) {
                return nullptr;
            }
            Batch* batch = m_pool->Acquire(wanted);
            bool acquired = false;
            {
                SpinGuard guard(m_slot_lock);
                const std::uint64_t consumed =
                    m_consumed_slot.load(std::memory_order_acquire);
                if (m_next_slot.load(std::memory_order_relaxed) - consumed <
                    m_queue_capacity) {
                    batch->slot = static_cast<std::uint32_t>(
                        m_next_slot.load(std::memory_order_relaxed));
                    m_next_slot.store(m_next_slot.load(std::memory_order_relaxed) + 1,
                                      std::memory_order_release);
                    batch->base = m_next_seq;
                    m_next_seq += m_max_events;
                    batch->used = 0;
                    batch->count = 0;
                    acquired = true;
                }
            }
            if (acquired) {
                producer->m_batch = batch;
                return batch;
            }
            m_pool->Release(batch);
            std::this_thread::yield();
        }
    }

    void Seal(Producer* producer) {
        Batch* batch = static_cast<Batch*>(producer->m_batch);
        if (batch == nullptr) {
            return;
        }
        producer->m_batch = nullptr;
        batch->ready.store(true, std::memory_order_release);
        m_ring[batch->slot % m_queue_capacity].store(
            batch, std::memory_order_release);
        m_wait_cv.notify_one();
    }

    void ConsumerLoop() {
        for (;;) {
            const std::uint64_t read =
                m_consumed_slot.load(std::memory_order_relaxed);
            const std::uint64_t produced =
                m_next_slot.load(std::memory_order_acquire);
            if (read >= produced) {
                if (m_stop.load(std::memory_order_acquire)) {
                    break;
                }
                MaybeIntervalSync();
                std::unique_lock<std::mutex> lock(m_wait_mutex);
                m_wait_cv.wait_for(lock, std::chrono::milliseconds(2), [this] {
                    return m_stop.load(std::memory_order_acquire) ||
                           m_consumed_slot.load(std::memory_order_relaxed) <
                               m_next_slot.load(std::memory_order_acquire);
                });
                continue;
            }
            Batch* batch =
                m_ring[read % m_queue_capacity].load(std::memory_order_acquire);
            if (batch == nullptr) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                continue;
            }
            if (m_error.empty()) {
                Consume(batch);
            }
            m_ring[read % m_queue_capacity].store(nullptr,
                                                  std::memory_order_release);
            m_consumed_slot.store(read + 1, std::memory_order_release);
            m_pool->Release(batch);
        }
    }

    void Consume(Batch* batch) {
        if (batch->count == 0) {
            return;
        }
        if (m_segmented && m_current_seg_bytes > 0 &&
            m_current_seg_bytes + batch->used > m_options.segment_bytes) {
            CloseSegment(true);
            if (!m_error.empty()) {
                return;
            }
        }
        if (m_fd < 0) {
            SetError("segment fd 未打开");
            return;
        }
        if (!WriteAll(m_fd, batch->data.data(), batch->used, &m_error)) {
            return;
        }
        m_bytes += batch->used;
        m_current_seg_bytes += batch->used;
        m_batches += 1;
        MaybeStartWriteback();
        const std::uint32_t event_count = batch->meta_batch ? 0U : batch->count;
        m_records += event_count;
        if (event_count > 0) {
            if (m_current_seg.count == 0) {
                m_current_seg.first_seq = batch->base;
                m_current_seg.first_ts = batch->first_ts;
            }
            m_current_seg.last_seq = batch->base + batch->count - 1;
            m_current_seg.last_ts = batch->last_ts;
            m_current_seg.count += batch->count;
        }
        m_current_seg.bytes = m_current_seg_bytes;
        m_unsynced_bytes += batch->used;
        m_dirty = true;
        if (m_options.sync == SyncPolicy::kBatch &&
            m_unsynced_bytes >= m_options.sync_bytes) {
            Sync();
        } else if (m_options.sync == SyncPolicy::kInterval) {
            MaybeIntervalSync();
        }
    }

    void MaybeStartWriteback() {
#if defined(__linux__) && defined(SYNC_FILE_RANGE_WRITE)
        if (!m_options.writeback_ahead || m_fd < 0) {
            return;
        }
        if (m_bytes - m_writeback_mark < m_options.writeback_bytes) {
            return;
        }
        // 只触发回写、不等待完成；失败不影响正确性（fdatasync 仍是兜底）。
        (void)::sync_file_range(m_fd, 0, 0, SYNC_FILE_RANGE_WRITE);
        m_writeback_mark = m_bytes;
#endif
    }

    void MaybeIntervalSync() {
        if (m_options.sync != SyncPolicy::kInterval || !m_dirty) {
            return;
        }
        const std::uint64_t now = NowNs();
        if (m_last_sync_ns != 0 &&
            now - m_last_sync_ns <
                static_cast<std::uint64_t>(m_options.interval_ms) * 1000000ULL) {
            return;
        }
        Sync();
    }

    void Sync() {
        if (!m_dirty || m_fd < 0) {
            return;
        }
        const std::uint64_t begin = NowNs();
        if (!FsyncFile(m_fd, &m_error)) {
            return;
        }
        const std::uint64_t end = NowNs();
        m_fsync_latencies.push_back(static_cast<std::uint32_t>(
            std::min<std::uint64_t>(end - begin, 0xFFFFFFFFULL)));
        if (m_last_sync_ns != 0) {
            m_max_sync_gap_ns =
                std::max(m_max_sync_gap_ns, begin - m_last_sync_ns);
        }
        m_last_sync_ns = end;
        m_dirty = false;
        m_unsynced_bytes = 0;
        ++m_fsyncs;
    }

    bool OpenSegment(std::uint32_t id) {
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
        const std::string path = SegmentPath(m_options.path, id);
        m_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (m_fd < 0) {
            SetError(ErrnoText("open segment"));
            return false;
        }
        m_current_seg = SegmentInfo{};
        m_current_seg.id = id;
        m_current_seg_bytes = 0;
        m_writeback_mark = m_bytes;
        return true;
    }

    void CloseSegment(bool open_next) {
        if (m_fd < 0) {
            return;
        }
        if (m_options.sync != SyncPolicy::kNone) {
            Sync();
        }
        if (m_current_seg_bytes > 0) {
            m_current_seg.bytes = m_current_seg_bytes;
            m_segments.push_back(m_current_seg);
        }
        ::close(m_fd);
        m_fd = -1;
        m_current_seg = SegmentInfo{};
        m_current_seg_bytes = 0;
        if (open_next) {
            OpenSegment(static_cast<std::uint32_t>(m_segments.size() + 1));
        }
    }

    void FinalizeOutput() {
        if (m_segmented) {
            CloseSegment(false);
            if (!m_error.empty()) {
                return;
            }
            WriteManifest();
            return;
        }
        go2cpp::replay::Event end;
        end.type = go2cpp::replay::EventType::kEnd;
        end.seq = m_next_seq + kSeqGap;
        end.ts_ns = NowNs();
        end.value = static_cast<std::int64_t>(m_records);
        end.sched_type = 0;
        std::vector<std::uint8_t> encoded;
        go2cpp::replay::EncodeEvent(end, encoded);
        if (!WriteAll(m_fd, encoded.data(), encoded.size(), &m_error)) {
            return;
        }
        m_bytes += encoded.size();
        if (m_options.sync != SyncPolicy::kNone) {
            Sync();
        }
        if (m_current_seg.count > 0) {
            m_current_seg.bytes = m_current_seg_bytes + encoded.size();
            m_segments.push_back(m_current_seg);
        }
        ::close(m_fd);
        m_fd = -1;
    }

    void WriteManifest() {
        std::ostringstream out;
        out << "G2CSESSION 1\n";
        out << "created_unix_ns "
            << static_cast<std::uint64_t>(
                   std::chrono::system_clock::now().time_since_epoch().count())
            << "\n";
        out << "base " << m_options.path << "\n";
        out << "engine segmented\n";
        out << "records " << m_records << "\n";
        out << "bytes " << m_bytes << "\n";
        out << "checksum " << m_checksum << "\n";
        out << "segments " << m_segments.size() << "\n";
        for (const auto& segment : m_segments) {
            out << "seg " << segment.id << " " << segment.bytes << " "
                << segment.first_seq << " " << segment.last_seq << " "
                << segment.count << " " << segment.first_ts << " "
                << segment.last_ts << "\n";
        }
        const std::string text = out.str();
        const std::string path = ManifestPath(m_options.path);
        const std::string temp = path + ".tmp";
        const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            SetError(ErrnoText("open manifest"));
            return;
        }
        std::string error;
        if (!WriteAll(fd, text.data(), text.size(), &error) ||
            !FsyncFile(fd, &error)) {
            ::close(fd);
            SetError(error);
            return;
        }
        ::close(fd);
        if (::rename(temp.c_str(), path.c_str()) != 0) {
            SetError(ErrnoText("rename manifest"));
            return;
        }
        const std::filesystem::path parent =
            std::filesystem::path(path).parent_path();
        if (!parent.empty()) {
            const int dir = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
            if (dir >= 0) {
                (void)::fsync(dir);
                ::close(dir);
            }
        }
    }

    std::unique_ptr<BatchPool> m_pool;
    bool m_segmented{false};
    std::size_t m_max_events{1};
    mutable std::mutex m_finish_mutex;
    std::mutex m_producers_mutex;
    std::vector<std::unique_ptr<Producer>> m_producers;
    std::uint32_t m_next_producer_id{0};
    std::size_t m_queue_capacity{64};
    std::vector<std::atomic<Batch*>> m_ring;
    std::atomic<std::uint64_t> m_next_slot{0};
    std::atomic<std::uint64_t> m_consumed_slot{0};
    std::uint64_t m_next_seq{1};
    SpinLock m_slot_lock;
    int m_fd{-1};
    std::thread m_consumer;
    std::atomic<bool> m_stop{false};
    std::mutex m_wait_mutex;
    std::condition_variable m_wait_cv;
    std::vector<SegmentInfo> m_segments;
    SegmentInfo m_current_seg;
    std::uint64_t m_current_seg_bytes{0};
    std::vector<std::uint32_t> m_fsync_latencies;
    std::uint64_t m_records{0};
    std::uint64_t m_bytes{0};
    std::uint64_t m_batches{0};
    std::uint64_t m_meta_checksum{0};
    std::uint64_t m_checksum{0};
    std::uint64_t m_start_ns{0};
    std::uint64_t m_last_sync_ns{0};
    std::uint64_t m_max_sync_gap_ns{0};
    std::uint64_t m_unsynced_bytes{0};
    std::uint64_t m_writeback_mark{0};
    std::uint64_t m_fsyncs{0};
    std::uint64_t m_fsync_p50{0};
    std::uint64_t m_fsync_p99{0};
    std::atomic<bool> m_failed{false};
    std::string m_error;
    bool m_dirty{false};
    bool m_finished{false};
};

std::unique_ptr<EventWriter> EventWriter::Create(const WriterOptions& options,
                                                 std::string* error) {
    switch (options.engine) {
        case Engine::kBuffered: {
            auto writer = std::make_unique<BufferedWriter>(options);
            if (!writer->ok()) {
                if (error != nullptr) {
                    *error = "BufferedWriter 初始化失败";
                }
                return nullptr;
            }
            return writer;
        }
        case Engine::kMpsc:
        case Engine::kSegmented: {
            auto writer = std::make_unique<AsyncWriter>(options);
            if (!writer->Initialize()) {
                if (error != nullptr) {
                    *error = "AsyncWriter 初始化失败";
                }
                return nullptr;
            }
            return writer;
        }
    }
    if (error != nullptr) {
        *error = "未知 engine";
    }
    return nullptr;
}

}  // namespace go2cpp::persistence
