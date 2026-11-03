#include "go2cpp/rd_net/event_loop.hpp"

/// @file event_loop.cpp
/// @brief REQ-007 实现：单线程 poll(2) 后端 + 最小堆定时器 + eventfd 唤醒。
///
/// @details 单 tick 顺序见 event_loop.hpp 顶部注释；本文件只在 Linux/POSIX 编译，
///          _WIN32 分支给出显式失败占位（不静默崩溃）。

#if !defined(_WIN32)

#include <algorithm>
#include <cerrno>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rd::net {
namespace {

/// @brief 单个 socket 单 tick 最多派发的包数（REQ-007 约束 3）。
constexpr std::size_t kMaxPacketsPerTick = 64;
/// @brief 单次 recvfrom 的临时缓冲（数据报超过它时返回 kTruncated）。
constexpr std::size_t kScratchBytes = 65536;
/// @brief cancelled_ 集合超过该阈值时重建堆，避免长期累积已触发的 id。
constexpr std::size_t kCancelPruneThreshold = 4096;
/// @brief poll 单次最长阻塞，避免极端情况下长时间不检查 stop_。
constexpr int kMaxPollTimeoutMs = 60000;

struct TimerEntry {
    EventTimePoint deadline{};
    std::uint64_t id{0};
    std::function<void()> callback;
};

/// @brief 最小堆比较器：deadline 早的优先，同一 deadline 按 id 升序。
struct TimerEarlier {
    bool operator()(const TimerEntry& a, const TimerEntry& b) const noexcept {
        if (a.deadline != b.deadline) {
            return a.deadline > b.deadline;
        }
        return a.id > b.id;
    }
};

/// @brief 一个已绑定 socket 的槽位。Detach 只把 active 置 false，
///        保证同一 tick 内 poll 索引到 slot 的映射不会因 erase 而错位。
struct Slot {
    int fd{-1};
    bool is_stream{false};
    bool active{false};
    DatagramSocket* datagram{nullptr};
    StreamSocket* stream{nullptr};
    Protocol* datagram_handler{nullptr};
    StreamProtocol* stream_handler{nullptr};
};

}  // namespace

struct EventLoop::Impl {
    /// @brief 定时器最小堆（std::vector + push_heap/pop_heap，便于取消时重建）。
    std::vector<TimerEntry> timers;
    std::unordered_set<std::uint64_t> cancelled;
    std::uint64_t next_timer_id{1};

    /// @brief Post 队列：posted 由投递线程写、loop 线程整体 swap 到 draining。
    std::mutex post_mutex;
    std::vector<std::function<void()>> posted;
    std::vector<std::function<void()>> draining;

    int wake_fd{-1};
    std::vector<Slot> slots;
    std::vector<pollfd> pollfds;
    std::vector<std::size_t> poll_to_slot;
    std::vector<std::uint8_t> scratch;

    ~Impl() {
        if (wake_fd >= 0) {
            ::close(wake_fd);
            wake_fd = -1;
        }
    }
};

EventLoop::EventLoop() : impl_(std::make_unique<Impl>()) {}
EventLoop::~EventLoop() = default;

Result<std::unique_ptr<EventLoop>> EventLoop::Create() {
    std::unique_ptr<EventLoop> loop(new EventLoop());
    Impl& impl = *loop->impl_;
    impl.wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (impl.wake_fd < 0) {
        return Result<std::unique_ptr<EventLoop>>::Err(IoStatus::kIoError, errno);
    }
    impl.scratch.resize(kScratchBytes);
    impl.pollfds.reserve(2);
    impl.poll_to_slot.reserve(2);
    return Result<std::unique_ptr<EventLoop>>::Ok(std::move(loop));
}

void EventLoop::Wake() {
    if (!impl_ || impl_->wake_fd < 0) {
        return;
    }
    const std::uint64_t one = 1;
    const ssize_t written = ::write(impl_->wake_fd, &one, sizeof(one));
    (void)written;  // EAGAIN 表示计数已达上限，唤醒效果已经存在。
}

void EventLoop::DrainWakeFd() {
    std::uint64_t value = 0;
    while (::read(impl_->wake_fd, &value, sizeof(value)) > 0) {
    }
}

bool EventLoop::AttachDatagram(DatagramSocket& socket, Protocol* handler) {
    if (!impl_ || !socket.valid()) {
        return false;
    }
    for (Slot& slot : impl_->slots) {
        if (slot.active && slot.datagram == &socket) {
            slot.datagram_handler = handler;
            return true;
        }
    }
    Slot fresh;
    fresh.fd = socket.fd();
    fresh.is_stream = false;
    fresh.active = true;
    fresh.datagram = &socket;
    fresh.datagram_handler = handler;
    for (Slot& slot : impl_->slots) {
        if (!slot.active) {
            slot = fresh;
            if (handler != nullptr) {
                handler->OnAttached(*this);
            }
            return true;
        }
    }
    impl_->slots.push_back(fresh);
    impl_->pollfds.reserve(impl_->slots.size() + 1);
    impl_->poll_to_slot.reserve(impl_->slots.size() + 1);
    if (handler != nullptr) {
        handler->OnAttached(*this);
    }
    return true;
}

bool EventLoop::AttachStream(StreamSocket& socket, StreamProtocol* handler) {
    if (!impl_ || !socket.valid()) {
        return false;
    }
    for (Slot& slot : impl_->slots) {
        if (slot.active && slot.stream == &socket) {
            slot.stream_handler = handler;
            return true;
        }
    }
    Slot fresh;
    fresh.fd = socket.fd();
    fresh.is_stream = true;
    fresh.active = true;
    fresh.stream = &socket;
    fresh.stream_handler = handler;
    for (Slot& slot : impl_->slots) {
        if (!slot.active) {
            slot = fresh;
            if (handler != nullptr) {
                handler->OnAttached(*this);
            }
            return true;
        }
    }
    impl_->slots.push_back(fresh);
    impl_->pollfds.reserve(impl_->slots.size() + 1);
    impl_->poll_to_slot.reserve(impl_->slots.size() + 1);
    if (handler != nullptr) {
        handler->OnAttached(*this);
    }
    return true;
}

bool EventLoop::Detach(DatagramSocket& socket) {
    if (!impl_) {
        return false;
    }
    for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
        if (impl_->slots[i].active && impl_->slots[i].datagram == &socket) {
            DetachSlot(i);
            return true;
        }
    }
    return false;
}

bool EventLoop::Detach(StreamSocket& socket) {
    if (!impl_) {
        return false;
    }
    for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
        if (impl_->slots[i].active && impl_->slots[i].stream == &socket) {
            DetachSlot(i);
            return true;
        }
    }
    return false;
}

void EventLoop::DetachSlot(std::size_t slot_index) {
    if (slot_index >= impl_->slots.size()) {
        return;
    }
    Slot& slot = impl_->slots[slot_index];
    if (!slot.active) {
        return;
    }
    slot.active = false;
    slot.fd = -1;
    if (slot.is_stream) {
        StreamProtocol* handler = slot.stream_handler;
        slot.stream_handler = nullptr;
        slot.stream = nullptr;
        if (handler != nullptr) {
            handler->OnDetached();
        }
    } else {
        Protocol* handler = slot.datagram_handler;
        slot.datagram_handler = nullptr;
        slot.datagram = nullptr;
        if (handler != nullptr) {
            handler->OnDetached();
        }
    }
}

std::uint64_t EventLoop::ScheduleAfter(std::chrono::milliseconds delay,
                                       std::function<void()> callback) {
    if (!impl_) {
        return 0;
    }
    if (delay < std::chrono::milliseconds::zero()) {
        delay = std::chrono::milliseconds::zero();
    }
    const std::uint64_t id = impl_->next_timer_id++;
    TimerEntry entry;
    entry.deadline = EventClock::now() + delay;
    entry.id = id;
    entry.callback = std::move(callback);
    impl_->timers.push_back(std::move(entry));
    std::push_heap(impl_->timers.begin(), impl_->timers.end(), TimerEarlier{});
    if (impl_->timers.front().id == id) {
        // 新定时器成为堆顶：若 poll 正在阻塞，唤醒它以重新计算超时。
        Wake();
    }
    return id;
}

bool EventLoop::CancelTimer(std::uint64_t id) {
    if (!impl_ || id == 0) {
        return false;
    }
    impl_->cancelled.insert(id);
    if (impl_->cancelled.size() > kCancelPruneThreshold) {
        std::vector<TimerEntry> kept;
        kept.reserve(impl_->timers.size());
        for (TimerEntry& entry : impl_->timers) {
            if (impl_->cancelled.count(entry.id) == 0) {
                kept.push_back(std::move(entry));
            }
        }
        impl_->timers = std::move(kept);
        std::make_heap(impl_->timers.begin(), impl_->timers.end(), TimerEarlier{});
        impl_->cancelled.clear();
    }
    Wake();
    return true;
}

void EventLoop::Post(std::function<void()> task) {
    if (!impl_) {
        return;
    }
    bool need_wake = false;
    {
        std::lock_guard<std::mutex> lock(impl_->post_mutex);
        need_wake = impl_->posted.empty();
        impl_->posted.push_back(std::move(task));
    }
    if (need_wake) {
        Wake();
    }
}

void EventLoop::Run() {
    if (!impl_) {
        return;
    }
    while (!stop_.load(std::memory_order_acquire)) {
        const EventTimePoint cutoff = EventClock::now();
        tick_.fetch_add(1, std::memory_order_acq_rel);

        // 阶段 1：Post 任务（FIFO 整批）。
        DrainPostedTasks();
        if (stop_.load(std::memory_order_acquire)) {
            break;
        }

        // 阶段 2：截止 tick 起点已经到期的定时器。
        FireDueTimers(cutoff);
        if (stop_.load(std::memory_order_acquire)) {
            break;
        }

        // 阶段 3：poll 等待 + IO 派发。
        RefreshPollSet();
        const int timeout_ms = ComputePollTimeoutMs();
        const int ready = ::poll(impl_->pollfds.data(),
                                 static_cast<nfds_t>(impl_->pollfds.size()),
                                 timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            Stop();
            break;
        }
        DispatchReadyEvents(ready);
    }
}

void EventLoop::DrainPostedTasks() {
    {
        std::lock_guard<std::mutex> lock(impl_->post_mutex);
        impl_->draining.swap(impl_->posted);
    }
    for (std::function<void()>& task : impl_->draining) {
        if (stop_.load(std::memory_order_acquire)) {
            break;
        }
        if (task) {
            task();
        }
    }
    impl_->draining.clear();
}

void EventLoop::FireDueTimers(EventTimePoint cutoff) {
    std::vector<TimerEntry>& timers = impl_->timers;
    while (!timers.empty()) {
        if (impl_->cancelled.erase(timers.front().id) > 0) {
            std::pop_heap(timers.begin(), timers.end(), TimerEarlier{});
            timers.pop_back();
            continue;
        }
        if (timers.front().deadline > cutoff) {
            break;
        }
        std::pop_heap(timers.begin(), timers.end(), TimerEarlier{});
        TimerEntry entry = std::move(timers.back());
        timers.pop_back();
        if (entry.callback) {
            entry.callback();
        }
        if (stop_.load(std::memory_order_acquire)) {
            return;
        }
    }
}

void EventLoop::RefreshPollSet() {
    impl_->pollfds.clear();
    impl_->poll_to_slot.clear();
    impl_->pollfds.push_back(pollfd{impl_->wake_fd, POLLIN, 0});
    impl_->poll_to_slot.push_back(0);
    for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
        const Slot& slot = impl_->slots[i];
        if (!slot.active || slot.fd < 0) {
            continue;
        }
        impl_->pollfds.push_back(pollfd{slot.fd, POLLIN, 0});
        impl_->poll_to_slot.push_back(i);
    }
}

int EventLoop::ComputePollTimeoutMs() {
    std::vector<TimerEntry>& timers = impl_->timers;
    while (!timers.empty() && impl_->cancelled.count(timers.front().id) > 0) {
        impl_->cancelled.erase(timers.front().id);
        std::pop_heap(timers.begin(), timers.end(), TimerEarlier{});
        timers.pop_back();
    }
    if (timers.empty()) {
        return -1;
    }
    const EventTimePoint now = EventClock::now();
    const EventTimePoint deadline = timers.front().deadline;
    if (deadline <= now) {
        return 0;
    }
    const auto remaining_us =
        std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
    const long long ceiling_ms = (remaining_us + 999) / 1000;
    if (ceiling_ms > kMaxPollTimeoutMs) {
        return kMaxPollTimeoutMs;
    }
    return static_cast<int>(ceiling_ms);
}

void EventLoop::DispatchReadyEvents(int ready) {
    if (ready <= 0) {
        return;
    }
    if (!impl_->pollfds.empty() && (impl_->pollfds[0].revents & POLLIN) != 0) {
        DrainWakeFd();
    }
    for (std::size_t i = 1; i < impl_->pollfds.size(); ++i) {
        const short revents = impl_->pollfds[i].revents;
        if (revents == 0) {
            continue;
        }
        const std::size_t slot_index = impl_->poll_to_slot[i];
        if ((revents & POLLNVAL) != 0) {
            DetachSlot(slot_index);
            continue;
        }
        if (impl_->slots[slot_index].is_stream) {
            DrainStreamSlot(slot_index);
        } else {
            DrainDatagramSlot(slot_index);
        }
    }
}

void EventLoop::DrainDatagramSlot(std::size_t slot_index) {
    Slot& slot = impl_->slots[slot_index];
    if (!slot.active || slot.datagram == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < kMaxPacketsPerTick; ++i) {
        if (!slot.active || slot.datagram == nullptr) {
            return;
        }
        Endpoint from;
        std::size_t received = 0;
        MutableByteView view{impl_->scratch.data(), impl_->scratch.size()};
        const IoStatus status = slot.datagram->RecvFrom(view, &from, &received);
        if (status == IoStatus::kWouldBlock) {
            return;
        }
        if (status == IoStatus::kOk || status == IoStatus::kTruncated) {
            const std::size_t length =
                received < impl_->scratch.size() ? received : impl_->scratch.size();
            if (slot.datagram_handler != nullptr) {
                ByteView data{impl_->scratch.data(), length};
                slot.datagram_handler->OnDatagram(data, from);
            }
            continue;
        }
        // kClosed / kReset / kIoError / kInvalid：摘除该 socket，避免空转。
        DetachSlot(slot_index);
        return;
    }
}

void EventLoop::DrainStreamSlot(std::size_t slot_index) {
    Slot& slot = impl_->slots[slot_index];
    if (!slot.active || slot.stream == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < kMaxPacketsPerTick; ++i) {
        if (!slot.active || slot.stream == nullptr) {
            return;
        }
        std::size_t received = 0;
        MutableByteView view{impl_->scratch.data(), impl_->scratch.size()};
        const IoStatus status = slot.stream->Recv(view, &received);
        if (status == IoStatus::kWouldBlock) {
            return;
        }
        if (status == IoStatus::kOk && received > 0) {
            if (slot.stream_handler != nullptr) {
                ByteView data{impl_->scratch.data(), received};
                slot.stream_handler->OnData(data);
            }
            continue;
        }
        // kClosed / kReset / 读 0 字节 / 错误：通知后摘除。
        if (slot.stream_handler != nullptr) {
            slot.stream_handler->OnClosed();
        }
        DetachSlot(slot_index);
        return;
    }
}

std::size_t EventLoop::attached_count() const noexcept {
    if (!impl_) {
        return 0;
    }
    std::size_t count = 0;
    for (const Slot& slot : impl_->slots) {
        if (slot.active) {
            ++count;
        }
    }
    return count;
}

std::size_t EventLoop::timer_count() const noexcept {
    if (!impl_) {
        return 0;
    }
    // 堆里可能残留少量已取消项；数量只作诊断用，不做精确保证。
    if (impl_->timers.size() <= impl_->cancelled.size()) {
        return 0;
    }
    return impl_->timers.size() - impl_->cancelled.size();
}

void EventLoop::Stop() {
    stop_.store(true, std::memory_order_release);
    Wake();
}

}  // namespace rd::net

#else  // defined(_WIN32)

namespace rd::net {

struct EventLoop::Impl {};

EventLoop::EventLoop() = default;
EventLoop::~EventLoop() = default;

Result<std::unique_ptr<EventLoop>> EventLoop::Create() {
    // Windows 后端未实现。REQ-007 的验收实现是 Linux/POSIX；这里显式失败而不是
    // 静默崩溃。接入 WSA 时可用 WSAPoll（poll 语义），若退化为 select 则必须
    // 文档化 FD_SETSIZE=64 的上限。
    return Result<std::unique_ptr<EventLoop>>::Err(IoStatus::kInvalid, 0);
}

bool EventLoop::AttachDatagram(DatagramSocket& socket, Protocol* handler) {
    (void)socket;
    (void)handler;
    return false;
}

bool EventLoop::AttachStream(StreamSocket& socket, StreamProtocol* handler) {
    (void)socket;
    (void)handler;
    return false;
}

bool EventLoop::Detach(DatagramSocket& socket) {
    (void)socket;
    return false;
}

bool EventLoop::Detach(StreamSocket& socket) {
    (void)socket;
    return false;
}

std::uint64_t EventLoop::ScheduleAfter(std::chrono::milliseconds delay,
                                       std::function<void()> callback) {
    (void)delay;
    (void)callback;
    return 0;
}

bool EventLoop::CancelTimer(std::uint64_t id) {
    (void)id;
    return false;
}

void EventLoop::Post(std::function<void()> task) { (void)task; }
void EventLoop::Run() {}
void EventLoop::Stop() {}

void EventLoop::DrainPostedTasks() {}
void EventLoop::FireDueTimers(EventTimePoint cutoff) { (void)cutoff; }
void EventLoop::RefreshPollSet() {}
int EventLoop::ComputePollTimeoutMs() { return -1; }
void EventLoop::DispatchReadyEvents(int ready) { (void)ready; }
void EventLoop::DrainDatagramSlot(std::size_t slot) { (void)slot; }
void EventLoop::DrainStreamSlot(std::size_t slot) { (void)slot; }
void EventLoop::DetachSlot(std::size_t slot) { (void)slot; }
void EventLoop::DrainWakeFd() {}
void EventLoop::Wake() {}

std::size_t EventLoop::attached_count() const noexcept { return 0; }
std::size_t EventLoop::timer_count() const noexcept { return 0; }

}  // namespace rd::net

#endif  // _WIN32
