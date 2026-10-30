/**
 * @file socket_debug.cpp
 * @brief SocketDebug 的实现：per-fd 计数、256 条事件环、关闭归档与文本 dump。
 * @details 实现只依赖标准库（atomic/mutex/shared_mutex/unordered_map/deque），
 *          不 include 任何 debug 模块头，避免 `network -> debug` 的反向依赖。
 *          编译期 `GO2CPP_DEBUG_SOCKET=OFF` 时整个文件退化为空 TU。
 */

#include "go2cpp/network/socket_debug.hpp"

#if GO2CPP_DEBUG_SOCKET

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <cerrno>

namespace go2cpp::net {

namespace detail {

/// 运行时开关的定义；头文件里的 `Enabled()` 直接 relaxed 读这个原子量。
std::atomic<bool> g_socket_debug_enabled{false};

}  // namespace detail

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kRingCapacity = SocketDebug::kEventRingCapacity;

/// @brief 取当前 steady_clock 的纳秒值；只在记录事件时调用（Disable 不取）。
std::uint64_t NowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

/**
 * @brief 一个 fd 的原子计数集合。
 * @details 每个字段独立原子：记录路径不做 CAS 循环，也不整块加锁；
 *          `relaxed` 足够，因为这里只统计、不用于同步。
 */
struct AtomicStats {
    std::atomic<std::uint64_t> bytes_sent{0};
    std::atomic<std::uint64_t> bytes_recv{0};
    std::atomic<std::uint64_t> send_calls{0};
    std::atomic<std::uint64_t> recv_calls{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint64_t> timeouts{0};
    std::atomic<std::uint64_t> connects{0};
    std::atomic<std::uint64_t> connect_ok{0};
    std::atomic<std::uint64_t> accepts{0};
    std::atomic<std::uint64_t> closes{0};
    std::atomic<std::uint64_t> connect_time_ns{0};
    std::atomic<std::uint64_t> last_activity_ns{0};
    std::atomic<int> last_errno{0};
};

/// @brief 单个 fd 的账本：原子计数 + 受 event_mutex 保护的 256 条环形事件。
struct Entry {
    explicit Entry(int descriptor) noexcept : fd(descriptor) {}

    const int fd;
    AtomicStats stats;
    mutable std::mutex event_mutex;  ///< 保护 events/write_index/next_seq/recorded。
    std::array<SocketEvent, kRingCapacity> events{};
    std::size_t write_index{0};   ///< 下一条事件写入位置。
    std::uint64_t next_seq{1};    ///< 本 fd 的下一个事件序号。
    std::uint64_t recorded{0};    ///< 累计写入事件数（含被覆盖的）。
    std::atomic<bool> closed{false};
};

// 全局注册表：活条目与最近关闭的归档条目共用一把读写锁。
std::shared_mutex g_registry_mutex;
std::unordered_map<int, std::shared_ptr<Entry>> g_live;
std::deque<std::pair<int, std::shared_ptr<Entry>>> g_finished;  // 最近关闭优先。
std::atomic<std::uint64_t> g_dropped{0};

inline void Add(std::atomic<std::uint64_t>& counter,
                std::uint64_t delta = 1) noexcept {
    counter.fetch_add(delta, std::memory_order_relaxed);
}

/// @brief 在活表中查找（不创建）。
std::shared_ptr<Entry> FindLive(int fd) noexcept {
    std::shared_lock<std::shared_mutex> lock(g_registry_mutex);
    const auto found = g_live.find(fd);
    return found == g_live.end() ? std::shared_ptr<Entry>{} : found->second;
}

/// @brief 活表优先、归档兜底；`Snapshot/Events` 都走这里。
std::shared_ptr<Entry> FindAny(int fd) noexcept {
    std::shared_lock<std::shared_mutex> lock(g_registry_mutex);
    const auto found = g_live.find(fd);
    if (found != g_live.end()) {
        return found->second;
    }
    // 归档按关闭先后倒序查找：同一 fd 号被复用过多次时取最近一次。
    for (auto item = g_finished.rbegin(); item != g_finished.rend(); ++item) {
        if (item->first == fd) {
            return item->second;
        }
    }
    return {};
}

/**
 * @brief 取 fd 对应的活条目，不存在则创建。
 * @details 双重检查：热路径先用 shared_lock 命中已有条目；只有首次出现某个
 *          fd 时才升级到 unique_lock。注册表满时不再新建，计入 dropped。
 */
std::shared_ptr<Entry> EnsureEntry(int fd) noexcept {
    if (fd < 0) {
        return {};
    }
    if (auto existing = FindLive(fd)) {
        return existing;
    }
    std::unique_lock<std::shared_mutex> lock(g_registry_mutex);
    const auto found = g_live.find(fd);
    if (found != g_live.end()) {
        return found->second;
    }
    if (g_live.size() >= SocketDebug::kMaxTrackedSockets) {
        Add(g_dropped);
        return {};
    }
    // fd 号复用：删掉归档里的旧条目，避免同一 fd 同时出现在活表与归档中。
    for (auto item = g_finished.begin(); item != g_finished.end(); ++item) {
        if (item->first == fd) {
            g_finished.erase(item);
            break;
        }
    }
    auto entry = std::make_shared<Entry>(fd);
    g_live.emplace(fd, entry);
    return entry;
}

/// @brief 写入一条事件并更新 last_activity；调用方已确认 entry 非空。
void PushEvent(const std::shared_ptr<Entry>& entry, SocketEventKind kind,
               int error, std::uint64_t bytes,
               std::uint64_t elapsed_ns) noexcept {
    SocketEvent event;
    event.ts_ns = NowNs();
    event.kind = kind;
    event.fd = entry->fd;
    event.error = error;
    event.bytes = bytes;
    event.elapsed_ns = elapsed_ns;
    {
        std::lock_guard<std::mutex> lock(entry->event_mutex);
        event.seq = entry->next_seq++;
        entry->events[entry->write_index] = event;
        entry->write_index = (entry->write_index + 1) % kRingCapacity;
        ++entry->recorded;
    }
    entry->stats.last_activity_ns.store(event.ts_ns, std::memory_order_relaxed);
}

/// @brief 失败调用的公共记账：errors 计数 + timeouts 细分 + last_errno。
void RecordFailure(const std::shared_ptr<Entry>& entry, int error) noexcept {
    Add(entry->stats.errors);
    if (error == ETIMEDOUT) {
        Add(entry->stats.timeouts);
    }
    entry->stats.last_errno.store(error, std::memory_order_relaxed);
}

/// @brief 由 Entry 构造快照副本（event_mutex 保护累计事件数）。
SocketStats MakeStats(const Entry& entry) noexcept {
    SocketStats stats;
    stats.fd = entry.fd;
    stats.tracked = true;
    stats.closed = entry.closed.load(std::memory_order_relaxed);
    stats.bytes_sent = entry.stats.bytes_sent.load(std::memory_order_relaxed);
    stats.bytes_recv = entry.stats.bytes_recv.load(std::memory_order_relaxed);
    stats.send_calls = entry.stats.send_calls.load(std::memory_order_relaxed);
    stats.recv_calls = entry.stats.recv_calls.load(std::memory_order_relaxed);
    stats.errors = entry.stats.errors.load(std::memory_order_relaxed);
    stats.timeouts = entry.stats.timeouts.load(std::memory_order_relaxed);
    stats.connects = entry.stats.connects.load(std::memory_order_relaxed);
    stats.connect_ok = entry.stats.connect_ok.load(std::memory_order_relaxed);
    stats.accepts = entry.stats.accepts.load(std::memory_order_relaxed);
    stats.closes = entry.stats.closes.load(std::memory_order_relaxed);
    stats.connect_time_ns =
        entry.stats.connect_time_ns.load(std::memory_order_relaxed);
    stats.last_activity_ns =
        entry.stats.last_activity_ns.load(std::memory_order_relaxed);
    stats.last_errno = entry.stats.last_errno.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(entry.event_mutex);
        stats.events = entry.recorded;
    }
    return stats;
}

/// @brief 单行 dump；字段顺序稳定，便于脚本 grep。
std::string FormatStats(const SocketStats& stats) {
    std::ostringstream line;
    line << "socket fd=" << stats.fd
         << " state=" << (stats.closed ? "closed" : "open")
         << " bytes_sent=" << stats.bytes_sent
         << " bytes_recv=" << stats.bytes_recv
         << " send_calls=" << stats.send_calls
         << " recv_calls=" << stats.recv_calls
         << " errors=" << stats.errors
         << " timeouts=" << stats.timeouts
         << " connects=" << stats.connects
         << " connect_ok=" << stats.connect_ok
         << " accepts=" << stats.accepts
         << " closes=" << stats.closes
         << " connect_time_ns=" << stats.connect_time_ns
         << " events=" << stats.events
         << " last_errno=" << stats.last_errno << '\n';
    return line.str();
}

}  // namespace

bool SocketDebug::Enable() noexcept {
    detail::g_socket_debug_enabled.store(true, std::memory_order_relaxed);
    return true;
}

void SocketDebug::Disable() noexcept {
    detail::g_socket_debug_enabled.store(false, std::memory_order_relaxed);
}

SocketStats SocketDebug::Snapshot(int fd) noexcept {
    const auto entry = FindAny(fd);
    if (!entry) {
        return {};
    }
    return MakeStats(*entry);
}

std::vector<SocketEvent> SocketDebug::Events(int fd, std::size_t max_events) {
    std::vector<SocketEvent> result;
    const auto entry = FindAny(fd);
    if (!entry) {
        return result;
    }
    std::lock_guard<std::mutex> lock(entry->event_mutex);
    const std::size_t available = static_cast<std::size_t>(
        std::min<std::uint64_t>(entry->recorded, kRingCapacity));
    std::size_t take = available;
    if (max_events != 0) {
        take = std::min(take, max_events);
    }
    result.reserve(take);
    // 环形中最旧的 available 条：从 write_index 回退 available 格。
    const std::size_t oldest =
        (entry->write_index + kRingCapacity - available) % kRingCapacity;
    const std::size_t skip = available - take;
    for (std::size_t index = 0; index < take; ++index) {
        result.push_back(
            entry->events[(oldest + skip + index) % kRingCapacity]);
    }
    return result;
}

std::string SocketDebug::DumpToString() {
    std::string out;
    DumpTo(out);
    return out;
}

void SocketDebug::DumpTo(std::string& out) {
    std::vector<SocketStats> rows;
    std::size_t live_count = 0;
    std::size_t finished_count = 0;
    std::uint64_t dropped = 0;
    {
        std::shared_lock<std::shared_mutex> lock(g_registry_mutex);
        live_count = g_live.size();
        finished_count = g_finished.size();
        dropped = g_dropped.load(std::memory_order_relaxed);
        rows.reserve(live_count + finished_count);
        for (const auto& item : g_live) {
            rows.push_back(MakeStats(*item.second));
        }
        for (const auto& item : g_finished) {
            rows.push_back(MakeStats(*item.second));
        }
    }
    // 活条目与归档条目混在一起时按 fd 排序，输出更稳定。
    std::sort(rows.begin(), rows.end(),
              [](const SocketStats& left, const SocketStats& right) {
                  return left.fd < right.fd;
              });
    std::ostringstream header;
    header << "socket_debug: available=1 enabled=" << (Enabled() ? 1 : 0)
           << " tracked=" << live_count << " finished=" << finished_count
           << " dropped=" << dropped << '\n';
    out = header.str();
    for (const auto& row : rows) {
        out += FormatStats(row);
    }
}

std::size_t SocketDebug::TrackedCount() noexcept {
    std::shared_lock<std::shared_mutex> lock(g_registry_mutex);
    return g_live.size();
}

void SocketDebug::Reset() noexcept {
    std::unique_lock<std::shared_mutex> lock(g_registry_mutex);
    g_live.clear();
    g_finished.clear();
    g_dropped.store(0, std::memory_order_relaxed);
}

void SocketDebug::RecordAccept(int fd) noexcept {
    if (!Enabled()) {
        return;
    }
    const auto entry = EnsureEntry(fd);
    if (!entry) {
        return;
    }
    Add(entry->stats.accepts);
    PushEvent(entry, SocketEventKind::kAccept, 0, 0, 0);
}

void SocketDebug::RecordConnect(int fd, int error,
                                std::uint64_t elapsed_ns) noexcept {
    if (!Enabled()) {
        return;
    }
    const auto entry = EnsureEntry(fd);
    if (!entry) {
        return;
    }
    Add(entry->stats.connects);
    Add(entry->stats.connect_time_ns, elapsed_ns);
    if (error == 0) {
        Add(entry->stats.connect_ok);
    } else {
        RecordFailure(entry, error);
    }
    SocketEventKind kind = SocketEventKind::kConnect;
    if (error == ETIMEDOUT) {
        kind = SocketEventKind::kTimeout;
    } else if (error != 0) {
        kind = SocketEventKind::kError;
    }
    PushEvent(entry, kind, error, 0, elapsed_ns);
}

void SocketDebug::RecordRead(int fd, std::uint64_t bytes,
                             int error) noexcept {
    if (!Enabled()) {
        return;
    }
    const auto entry = EnsureEntry(fd);
    if (!entry) {
        return;
    }
    Add(entry->stats.recv_calls);
    if (bytes > 0) {
        Add(entry->stats.bytes_recv, bytes);
    }
    SocketEventKind kind = SocketEventKind::kRead;
    if (error != 0) {
        RecordFailure(entry, error);
        kind = error == ETIMEDOUT ? SocketEventKind::kTimeout
                                  : SocketEventKind::kError;
    }
    PushEvent(entry, kind, error, bytes, 0);
}

void SocketDebug::RecordWrite(int fd, std::uint64_t bytes,
                              int error) noexcept {
    if (!Enabled()) {
        return;
    }
    const auto entry = EnsureEntry(fd);
    if (!entry) {
        return;
    }
    Add(entry->stats.send_calls);
    if (bytes > 0) {
        Add(entry->stats.bytes_sent, bytes);
    }
    SocketEventKind kind = SocketEventKind::kWrite;
    if (error != 0) {
        RecordFailure(entry, error);
        kind = error == ETIMEDOUT ? SocketEventKind::kTimeout
                                  : SocketEventKind::kError;
    }
    PushEvent(entry, kind, error, bytes, 0);
}

void SocketDebug::RecordClose(int fd) noexcept {
    if (!Enabled()) {
        return;
    }
    std::shared_ptr<Entry> entry;
    {
        std::unique_lock<std::shared_mutex> lock(g_registry_mutex);
        const auto found = g_live.find(fd);
        if (found == g_live.end()) {
            return;
        }
        entry = found->second;
        g_live.erase(found);
        entry->closed.store(true, std::memory_order_relaxed);
        g_finished.emplace_front(fd, entry);
        while (g_finished.size() > kMaxFinishedSockets) {
            g_finished.pop_back();
        }
    }
    Add(entry->stats.closes);
    PushEvent(entry, SocketEventKind::kClose, 0, 0, 0);
}

}  // namespace go2cpp::net

#endif  // GO2CPP_DEBUG_SOCKET
