/**
 * @file metadata_store.cpp
 * @brief `MetadataStore` 的实现：内存索引 + 可选 WAL + revision 有序派发。
 *
 * ## WAL 记录格式（小端无关，全部大端）
 *
 * ```
 * 偏移  长度  字段
 * 0     4     魔数 'G','2','M','D'
 * 4     1     类型（1 = put，2 = delete）
 * 5     3     保留（写 0）
 * 8     8     revision
 * 16    4     key 长度
 * 20    4     value 长度
 * 24    N     key 字节
 * 24+N  M     value 字节
 * 末尾  4     CRC32（覆盖本记录除 CRC 外的全部字节）
 * ```
 *
 * 恢复策略（与 storage 模块的 WAL 一致）：
 * - 尾部不足一条记录 / 尾部 CRC 错 → 视为「最后一次写被断电打断」，截断到
 *   最后一条完整记录后继续启动；
 * - 中间记录 CRC 错、魔法错、revision 不严格递增 → 视为损坏，`Open` 失败。
 *   静默跳过中间坏记录会让 revision 出现空洞，Watch 的「按 revision 顺序」
 *   与协调者的单调性都无法再保证。
 *
 * ## 并发模型
 *
 * - `m_mutex` 保护 map / revision / 订阅表 / 历史；写路径在锁内追加 WAL，
 *   因此「revision 顺序 == 落盘顺序」，无需额外排序；
 * - `m_queue_mutex` 保护异步派发队列与计数器；锁序为 `m_mutex` →
 *   `m_queue_mutex`（写路径），派发线程只短暂持有 `m_queue_mutex`；
 * - `m_dispatch_mutex` 串行化**回调调用**（异步派发线程与 `Replay` 共用），
 *   保证同一订阅看到的事件严格按 revision 递增；
 * - 回调总是在**不持有** `m_mutex`/`m_queue_mutex` 时执行，因此回调内可以
 *   安全地再次调用 `Put`（异步模式）。
 */

#include "go2cpp/distributed/metadata_store.hpp"

#include "snapshot_codec.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace go2cpp::distributed {
namespace {

using detail::Crc32;
using detail::SnapshotReader;
using detail::SnapshotWriter;

/// WAL 魔数（'G','2','M','D'）。
constexpr std::uint32_t kWalMagic = 0x47324D44U;
constexpr std::uint8_t kWalPut = 1;
constexpr std::uint8_t kWalDelete = 2;
constexpr std::size_t kWalHeaderBytes = 24;
constexpr std::size_t kWalCrcBytes = 4;
constexpr std::size_t kMaxKeyBytes = 1U << 20;
constexpr std::size_t kMaxValueBytes = 1U << 20;
constexpr std::size_t kMaxWalBytes = 256U << 20;

/// @brief 前缀匹配（前缀为空匹配全部）。
bool MatchesPrefix(const std::string &prefix, const std::string &key) {
    return key.size() >= prefix.size() &&
           key.compare(0, prefix.size(), prefix) == 0;
}

/// @brief 把一条事件编码成 WAL 记录（不含 CRC 的 body + CRC）。
std::string EncodeWalRecord(const MetadataEvent &event) {
    SnapshotWriter writer;
    writer.U32(kWalMagic);
    writer.U8(event.deleted ? kWalDelete : kWalPut);
    writer.U8(0);
    writer.U16(0);
    writer.U64(event.revision);
    writer.U32(static_cast<std::uint32_t>(event.key.size()));
    writer.U32(static_cast<std::uint32_t>(event.value.size()));
    writer.buffer.append(event.key);
    writer.buffer.append(event.value);
    writer.AppendCrc();
    return writer.buffer;
}

/// @brief 全量写文件；返回 false 时 `errno` 有效。
bool WriteAll(int fd, const char *data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t result = ::write(fd, data + written, size - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

std::string DescribeErrno(const char *what) {
    return std::string(what) + ": " + std::strerror(errno);
}

/// @brief 尽力回滚 WAL 到 `offset`；失败不覆盖原始错误（只记录在日志语义里）。
void BestEffortTruncate(int fd, off_t offset) {
    if (::ftruncate(fd, offset) != 0) {
        // 回滚失败时 WAL 可能留下半条记录：恢复路径会把它当作「尾部不完整」
        // 截掉，因此这里不改变调用方看到的错误原因。
    }
}

} // namespace

MetadataStore::MetadataStore(MetadataStoreOptions options) : m_options(std::move(options)) {}

Result<std::unique_ptr<MetadataStore>> MetadataStore::Open(
    const MetadataStoreOptions &options) {
    auto store = std::unique_ptr<MetadataStore>(new MetadataStore(options));
    if (store->m_options.sync_callbacks) {
        store->m_options.queue_capacity = 0;
    } else if (store->m_options.queue_capacity == 0) {
        store->m_options.queue_capacity = 1;
    }
    // step1: WAL 打开 + 恢复（内存模式跳过）。
    if (!store->OpenWal()) {
        // 权限/IO 失败是「暂时不可用」，不能报成协议错误（调用方会误以为
        // 数据损坏而放弃重试）；只有 WAL 内容异常才是 kProtocolError。
        const Status status = store->m_last_status == Status::kOk
                                  ? Status::kProtocolError
                                  : store->m_last_status;
        return Result<std::unique_ptr<MetadataStore>>::Failure(status,
                                                              store->m_last_error);
    }
    // step2: 异步派发线程（同步模式在写线程内直接回调，不需要线程）。
    if (!store->m_options.sync_callbacks) {
        store->m_dispatcher = std::thread([raw = store.get()] { raw->DispatchLoop(); });
    }
    return Result<std::unique_ptr<MetadataStore>>::Success(std::move(store));
}

Result<std::unique_ptr<MetadataStore>> MetadataStore::OpenMemory() {
    return Open(MetadataStoreOptions{});
}

MetadataStore::~MetadataStore() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
    }
    if (!m_options.sync_callbacks) {
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_stop_dispatcher = true;
        }
        m_queue_cv.notify_all();
        // 背压写者也要放行，否则析构会等一个永远不返回的写者。
        m_space_cv.notify_all();
        if (m_dispatcher.joinable()) {
            m_dispatcher.join();
        }
    }
    CloseWal();
}

bool MetadataStore::persistent() const noexcept { return m_wal_fd >= 0; }

bool MetadataStore::OpenWal() {
    m_last_status = Status::kOk;
    if (m_options.wal_path.empty()) {
        return true;
    }
    // step1: 打开/创建（追加写；不存在时创建）。
    m_wal_fd = ::open(m_options.wal_path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (m_wal_fd < 0) {
        m_last_error = DescribeErrno("open wal");
        m_last_status = Status::kUnavailable;
        return false;
    }
    // step2: 全量读入（WAL 只装元数据，256MiB 上限防御异常文件）。
    const off_t size = ::lseek(m_wal_fd, 0, SEEK_END);
    if (size < 0) {
        m_last_error = DescribeErrno("lseek wal");
        m_last_status = Status::kUnavailable;
        CloseWal();
        return false;
    }
    if (static_cast<std::uint64_t>(size) > kMaxWalBytes) {
        m_last_error = "wal size out of range";
        m_last_status = Status::kProtocolError;
        CloseWal();
        return false;
    }
    std::string content(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        off_t offset = 0;
        while (offset < size) {
            const ssize_t got = ::pread(m_wal_fd, content.data() + offset,
                                        static_cast<std::size_t>(size - offset), offset);
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                m_last_error = DescribeErrno("pread wal");
                m_last_status = Status::kUnavailable;
                CloseWal();
                return false;
            }
            if (got == 0) {
                break;
            }
            offset += got;
        }
    }
    // step3: 逐条解析。任何一步失败都记录「坏在哪」，便于运维定位。
    std::size_t pos = 0;
    bool damaged = false;
    std::size_t valid_end = 0;
    while (pos + kWalHeaderBytes + kWalCrcBytes <= content.size()) {
        SnapshotReader reader(content);
        reader.Skip(pos);
        std::uint32_t magic = 0;
        std::uint8_t type = 0;
        std::uint8_t reserved8 = 0;
        std::uint16_t reserved16 = 0;
        std::uint64_t revision = 0;
        std::uint32_t key_size = 0;
        std::uint32_t value_size = 0;
        if (!reader.U32(&magic) || magic != kWalMagic || !reader.U8(&type) ||
            !reader.U8(&reserved8) || !reader.U16(&reserved16) ||
            !reader.U64(&revision) || !reader.U32(&key_size) ||
            !reader.U32(&value_size) || key_size > kMaxKeyBytes ||
            value_size > kMaxValueBytes) {
            // 尾部 header 损坏视为「最后一条写被撕裂」，截断即可；中间损坏
            // 无法安全跳过，直接拒绝打开。
            if (pos + kWalHeaderBytes + kWalCrcBytes == content.size()) {
                break;
            }
            m_last_error = "wal header corrupt at offset " + std::to_string(pos);
            damaged = true;
            break;
        }
        const std::size_t record_size =
            kWalHeaderBytes + key_size + value_size + kWalCrcBytes;
        if (pos + record_size > content.size()) {
            break;  // 尾部不完整：正常断电，下面按截断处理。
        }
        const std::uint32_t stored_crc = Crc32(content.data() + pos,
                                               record_size - kWalCrcBytes);
        std::uint32_t actual_crc = 0;
        SnapshotReader tail(content);
        tail.Skip(pos + record_size - kWalCrcBytes);
        if (!tail.U32(&actual_crc) || actual_crc != stored_crc) {
            if (pos + record_size == content.size()) {
                break;  // 尾记录撕裂：截断。
            }
            m_last_error = "wal crc mismatch at offset " + std::to_string(pos);
            damaged = true;
            break;
        }
        if (revision <= m_revision || (type != kWalPut && type != kWalDelete)) {
            m_last_error = "wal revision/type invalid at offset " + std::to_string(pos);
            damaged = true;
            break;
        }
        // step4: 应用到内存索引（revision 直接采用记录里的值）。
        std::string key(content.data() + pos + kWalHeaderBytes, key_size);
        std::string value(content.data() + pos + kWalHeaderBytes + key_size, value_size);
        m_revision = revision;
        if (type == kWalPut) {
            m_entries[key] = MetadataEntry{key, std::move(value), revision};
        } else {
            m_entries.erase(key);
        }
        ++m_recover_records;
        pos += record_size;
        valid_end = pos;
    }
    m_recovered_bytes = valid_end;
    if (damaged) {
        m_last_status = Status::kProtocolError;
        CloseWal();
        return false;
    }
    // step5: 截断不完整尾部，保证后续追加从完整记录之后开始。
    if (valid_end < content.size()) {
        m_truncated_bytes = content.size() - valid_end;
        if (::ftruncate(m_wal_fd, static_cast<off_t>(valid_end)) != 0) {
            m_last_error = DescribeErrno("ftruncate wal");
            m_last_status = Status::kUnavailable;
            CloseWal();
            return false;
        }
    }
    return true;
}

void MetadataStore::CloseWal() {
    if (m_wal_fd >= 0) {
        ::close(m_wal_fd);
        m_wal_fd = -1;
    }
}

bool MetadataStore::AppendWalLocked(const MetadataEvent &event) {
    if (m_wal_fd < 0) {
        return true;  // 纯内存模式。
    }
    const std::string record = EncodeWalRecord(event);
    const off_t start = ::lseek(m_wal_fd, 0, SEEK_END);
    if (start < 0) {
        m_last_error = DescribeErrno("lseek wal");
        return false;
    }
    if (!WriteAll(m_wal_fd, record.data(), record.size())) {
        m_last_error = DescribeErrno("write wal");
        BestEffortTruncate(m_wal_fd, start);
        return false;
    }
    if (m_options.fsync_on_write && ::fdatasync(m_wal_fd) != 0) {
        m_last_error = DescribeErrno("fdatasync wal");
        BestEffortTruncate(m_wal_fd, start);
        return false;
    }
    return true;
}

void MetadataStore::EnqueueLocked(const MetadataEvent &event) {
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_queue.push_back(event);
        ++m_enqueued;
    }
    m_queue_cv.notify_one();
}

/**
 * @brief 背压入口：队列满时在 `m_space_cv` 上等待派发线程腾出空间。
 *
 * ## 定位
 * 唯一允许阻塞写线程的地方。**绝不能在持有 `m_mutex` 时调用**：派发线程
 * 必须先 `pop` 才能腾出空间，而它在 `DeliverLocked` 里也要拿 `m_mutex`——
 * 写者持锁等待、派发线程持空间等锁，就是死锁（原实现在持锁状态自旋，
 * 甚至派发线程连 `pop` 都拿不到 `m_queue_mutex`）。
 *
 * ## 并发/失败语义
 * - 调用约束：任意写线程，返回后才去抢 `m_mutex` 分配 revision；
 * - 原子性：与派发线程通过 `m_queue_mutex` + `m_space_cv` 同步；
 * - 不变量：返回时要么队列有空位、要么 store 正在关闭；
 *   因为等待在 `m_mutex` 之外，并发写者可能短暂把队列顶到容量之上（软上限，
 *   最多超出并发写者数），revision 顺序仍由 `m_mutex` 临界区保证。
 */
void MetadataStore::AwaitQueueSpace() {
    if (m_options.sync_callbacks) {
        return;
    }
    std::unique_lock<std::mutex> lock(m_queue_mutex);
    m_space_cv.wait(lock, [this] {
        return m_stop_dispatcher || m_queue.size() < m_options.queue_capacity;
    });
}

void MetadataStore::DispatchLoop() {
    for (;;) {
        MetadataEvent event;
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_queue_cv.wait(lock, [this] {
                return m_stop_dispatcher || !m_queue.empty();
            });
            if (m_queue.empty()) {
                if (m_stop_dispatcher) {
                    return;
                }
                continue;
            }
            event = std::move(m_queue.front());
            m_queue.pop_front();
            // 空间已经真正释放再唤醒：写者拿到 m_queue_mutex 时谓词一定成立。
            m_space_cv.notify_one();
        }
        std::lock_guard<std::mutex> dispatch_lock(m_dispatch_mutex);
        std::uint64_t matched = 0;
        DeliverLocked(event, &matched);
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            ++m_delivered;
        }
        m_flush_cv.notify_all();
        if (m_stop_dispatcher) {
            // 关闭时把剩余队列排空后再退出（析构保证已入队事件一定被派发）。
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            if (m_queue.empty()) {
                return;
            }
        }
    }
}

void MetadataStore::DeliverLocked(const MetadataEvent &event, std::uint64_t *matched) {
    struct Target {
        Callback callback;
    };
    std::vector<Target> targets;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto &[id, subscription] : m_subscriptions) {
            (void)id;
            if (subscription.cursor >= event.revision ||
                !MatchesPrefix(subscription.prefix, event.key)) {
                continue;
            }
            subscription.cursor = event.revision;
            targets.push_back(Target{subscription.callback});
            ++(*matched);
        }
    }
    for (const Target &target : targets) {
        std::uint64_t errors = 0;
        try {
            target.callback(event);
        } catch (...) {
            ++errors;
        }
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        ++m_matched;
        m_callback_errors += errors;
    }
}

Result<MetadataEntry> MetadataStore::Get(std::string_view key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_entries.find(std::string(key));
    if (it == m_entries.end()) {
        return Result<MetadataEntry>::Failure(Status::kNotFound, "key not found");
    }
    return Result<MetadataEntry>::Success(it->second);
}

std::vector<MetadataEntry> MetadataStore::List(std::string_view prefix) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<MetadataEntry> result;
    for (auto it = m_entries.lower_bound(std::string(prefix));
         it != m_entries.end() && MatchesPrefix(std::string(prefix), it->first); ++it) {
        result.push_back(it->second);
    }
    return result;
}

Outcome MetadataStore::Put(std::string_view key, std::string_view value,
                           std::uint64_t *revision) {
    if (key.empty()) {
        return Outcome::Failure(Status::kInvalidArgument, "key must be non-empty");
    }
    if (key.size() > kMaxKeyBytes || value.size() > kMaxValueBytes) {
        return Outcome::Failure(Status::kInvalidArgument, "key/value too large");
    }
    MetadataEvent event;
    event.key.assign(key.data(), key.size());
    event.value.assign(value.data(), value.size());
    event.deleted = false;

    std::uint64_t assigned = 0;
    // step1.5: 背压在 `m_mutex` **之外**等待：持锁等待会与派发线程互等。
    AwaitQueueSpace();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return Outcome::Failure(Status::kClosed, "store closed");
        }
        // step2: 先落 WAL 再改内存：返回成功 ⇔ 崩溃后可读回。
        event.revision = m_revision + 1;
        if (!AppendWalLocked(event)) {
            return Outcome::Failure(Status::kUnavailable, m_last_error);
        }
        m_revision = event.revision;
        m_entries[event.key] = MetadataEntry{event.key, event.value, event.revision};
        if (m_options.max_events > 0) {
            m_history.push_back(event);
            while (m_history.size() > m_options.max_events) {
                m_history.pop_front();
            }
        }
        ++m_puts;
        assigned = event.revision;
        if (!m_options.sync_callbacks) {
            EnqueueLocked(event);
        } else {
            std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
            ++m_enqueued;
        }
    }
    if (m_options.sync_callbacks) {
        std::lock_guard<std::mutex> dispatch_lock(m_dispatch_mutex);
        std::uint64_t matched = 0;
        DeliverLocked(event, &matched);
        std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
        ++m_delivered;
    }
    if (revision != nullptr) {
        *revision = assigned;
    }
    return Outcome::Success();
}

Outcome MetadataStore::Delete(std::string_view key, std::uint64_t *revision) {
    if (key.empty()) {
        return Outcome::Failure(Status::kInvalidArgument, "key must be non-empty");
    }
    if (key.size() > kMaxKeyBytes) {
        return Outcome::Failure(Status::kInvalidArgument, "key too large");
    }
    MetadataEvent event;
    event.key.assign(key.data(), key.size());
    event.deleted = true;

    std::uint64_t assigned = 0;
    // step1.5: 与 Put 相同，背压在 `m_mutex` 之外等待。
    AwaitQueueSpace();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed) {
            return Outcome::Failure(Status::kClosed, "store closed");
        }
        event.revision = m_revision + 1;
        if (!AppendWalLocked(event)) {
            return Outcome::Failure(Status::kUnavailable, m_last_error);
        }
        m_revision = event.revision;
        m_entries.erase(event.key);
        if (m_options.max_events > 0) {
            m_history.push_back(event);
            while (m_history.size() > m_options.max_events) {
                m_history.pop_front();
            }
        }
        ++m_deletes;
        assigned = event.revision;
        if (!m_options.sync_callbacks) {
            EnqueueLocked(event);
        } else {
            std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
            ++m_enqueued;
        }
    }
    if (m_options.sync_callbacks) {
        std::lock_guard<std::mutex> dispatch_lock(m_dispatch_mutex);
        std::uint64_t matched = 0;
        DeliverLocked(event, &matched);
        std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
        ++m_delivered;
    }
    if (revision != nullptr) {
        *revision = assigned;
    }
    return Outcome::Success();
}

std::uint64_t MetadataStore::Revision() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_revision;
}

std::uint64_t MetadataStore::Watch(std::string_view prefix, Callback callback,
                                   std::uint64_t stable_id) {
    if (!callback) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::uint64_t id =
        stable_id != 0 ? stable_id : m_next_subscription++;
    Subscription subscription;
    subscription.prefix.assign(prefix.data(), prefix.size());
    subscription.callback = std::move(callback);
    // 只接收注册之后的事件；历史补发走 Replay()，避免重复投递。
    subscription.cursor = m_revision;
    const auto existing = m_subscriptions.find(id);
    if (existing != m_subscriptions.end()) {
        subscription.cursor = existing->second.cursor;
    }
    m_subscriptions[id] = std::move(subscription);
    return id;
}

bool MetadataStore::Unwatch(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_subscriptions.erase(id) != 0;
}

Outcome MetadataStore::Replay(std::uint64_t id, std::uint64_t from_revision) {
    std::vector<MetadataEvent> events;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_subscriptions.find(id);
        if (it == m_subscriptions.end()) {
            return Outcome::Failure(Status::kNotFound, "unknown subscription");
        }
        if (m_closed) {
            return Outcome::Failure(Status::kClosed, "store closed");
        }
        // 历史窗口之外：无法保证顺序完整，要求调用方重建订阅。
        if (from_revision < m_revision && !m_history.empty() &&
            from_revision < m_history.front().revision) {
            return Outcome::Failure(Status::kNotFound,
                                    "revision is older than retained history");
        }
        for (const MetadataEvent &event : m_history) {
            if (event.revision >= from_revision &&
                MatchesPrefix(it->second.prefix, event.key)) {
                events.push_back(event);
            }
        }
    }
    std::lock_guard<std::mutex> dispatch_lock(m_dispatch_mutex);
    for (const MetadataEvent &event : events) {
        Callback callback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_subscriptions.find(id);
            if (it == m_subscriptions.end()) {
                return Outcome::Failure(Status::kNotFound, "subscription removed");
            }
            it->second.cursor = event.revision;
            callback = it->second.callback;
        }
        std::uint64_t errors = 0;
        try {
            callback(event);
        } catch (...) {
            ++errors;
        }
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        ++m_replayed;
        m_callback_errors += errors;
    }
    return Outcome::Success();
}

bool MetadataStore::Flush() {
    if (m_options.sync_callbacks) {
        return true;
    }
    std::unique_lock<std::mutex> lock(m_queue_mutex);
    return m_flush_cv.wait_for(lock, std::chrono::seconds(10), [this] {
        return m_delivered >= m_enqueued;
    });
}

MetadataStoreStats MetadataStore::GetStats() const {
    MetadataStoreStats stats;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        stats.last_revision = m_revision;
        stats.subscriptions = m_subscriptions.size();
        stats.entries = m_entries.size();
        stats.history = m_history.size();
        stats.recover_records = m_recover_records;
        stats.recovered_bytes = m_recovered_bytes;
        stats.truncated_bytes = m_truncated_bytes;
    }
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        stats.delivered = m_delivered;
        stats.matched = m_matched;
        stats.replayed = m_replayed;
        stats.callback_errors = m_callback_errors;
        stats.puts = m_puts;
        stats.deletes = m_deletes;
    }
    return stats;
}

std::string MetadataStore::LastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_error;
}

} // namespace go2cpp::distributed
