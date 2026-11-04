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

/**
 * @brief 把一个已打开的 DatagramSocket 挂到事件循环的 slot 表，并绑定协议处理器。
 *
 * @param socket [in] 数据报 socket——按引用借用，生命周期必须长于挂载期；invalid 的 socket 直接拒绝。
 * @param handler [in] 协议处理器——可为 nullptr（只占位不回调）；非空时本函数会立即回调其 OnAttached。
 *
 * @return
 * - true：已挂载（或该 socket 之前已挂载，仅替换 handler）——调用方**必须**保证 socket 活到 Detach；
 * - false：impl_ 缺失或 socket 无效——调用方**不要**假定已挂载，需先修好 socket/loop。
 *
 * ## 定位
 * 事件循环与 socket 之间唯一的挂载点；slot 下标是 poll 结果到 socket 的唯一映射依据。
 * 写错会让事件被派发到错误的 socket（fd 错配），表现为「A 的数据出现在 B」。
 *
 * ## 步骤
 * step1. 校验 impl_ 与 socket 有效性，失败即返回 false；
 * step2. 先查重：同一 socket 已挂载时只替换 handler，不新增 slot（避免同一 fd 被 poll 两次、事件被消费两遍）；
 * step3. 构造 Slot 并优先复用空闲槽位；复用失败才 push_back 并预留 poll 表容量；
 * step4. 挂载成功后立即回调 handler->OnAttached，让协议层可以马上发起首包或注册定时器。
 *
 * ## 并发/失败语义
 * - 调用约束：只能在事件循环线程（Run 所在线程）或 loop 启动前调用；slot 表无锁。
 * - 原子性：slot 写入与 poll 表刷新不是原子的——挂载后到下一次 RefreshPollSet 之前不会有该 fd 的事件。
 * - 失败语义：不抛异常；返回 false 时 slot 表未被修改。
 * - 不变量：返回 true 时恰好有一个 active slot 指向该 socket；handler 至多被 OnAttached 一次。
 */
bool EventLoop::AttachDatagram(DatagramSocket& socket, Protocol* handler) {
    // step1: 校验 impl_ 与 socket 有效性；失败返回 false，slot 表不变。
    if (!impl_ || !socket.valid()) {
        return false;
    }
    for (Slot& slot : impl_->slots) {
        if (slot.active && slot.datagram == &socket) {
            slot.datagram_handler = handler;
            return true;
        }
    }
    // step2: 构造 Slot 前先查重——同 socket 已挂载时只替换 handler，避免同一 fd 被 poll 两次。
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
    // step3: 优先复用空闲槽位，无空位才追加并预留 poll 表容量。
    impl_->slots.push_back(fresh);
    impl_->pollfds.reserve(impl_->slots.size() + 1);
    impl_->poll_to_slot.reserve(impl_->slots.size() + 1);
    if (handler != nullptr) {
        // step4: 挂载成功后立即回调 OnAttached，让协议层可马上发起首包或注册定时器。
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

/**
 * @brief 释放一个 slot：清空 fd 与处理器引用，并回调对应协议层的 OnDetached。
 *
 * @param slot_index [in] slot 下标——越界或该槽位已 inactive 时静默返回（幂等）。
 *
 * ## 定位
 * slot 生命周期的唯一回收点；Detach 与「socket 出错被摘除」都走这里。
 * 写错会让 slot 残留 active 状态，poll 派发到已销毁的 socket（悬垂引用）或重复回调 OnDetached。
 *
 * @return 无返回值——调用方**不要**在返回后继续使用该 slot 的 handler。
 *
 * ## 步骤
 * step1. 越界与已 inactive 的幂等早退，保证重复 Detach 不会二次回调；
 * step2. 先置 active=false 并清 fd，使后续 poll 刷新不会再包含它（先摘除再回调）；
 * step3. 按 stream/datagram 分支取出 handler 并清空引用，再调用 OnDetached——保证回调内看不到已失效的槽位。
 *
 * ## 并发/失败语义
 * - 调用约束：只能在事件循环线程调用；slot 表无锁。
 * - 原子性：active 置位与 handler 清空在同一线程顺序执行，对 poll 刷新可见。
 * - 失败语义：不抛异常；越界/重复调用均为 no-op。
 * - 不变量：返回后该 slot 的 active 为 false 且不再持有任何 socket/handler 指针。
 */
void EventLoop::DetachSlot(std::size_t slot_index) {
    // step1: 越界与已 inactive 的幂等早退，保证重复 Detach 不会二次回调 OnDetached。
    if (slot_index >= impl_->slots.size()) {
        return;
    }
    Slot& slot = impl_->slots[slot_index];
    if (!slot.active) {
        return;
    }
    // step2: 先清 active 与 fd，使后续 poll 刷新不再包含它（先摘除、再回调）。
    slot.active = false;
    slot.fd = -1;
    // step3: 按 stream/datagram 分支取出 handler 并清空引用，再回调 OnDetached，避免回调内看到失效槽位。
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

/**
 * @brief 事件循环主循环：按「Post 任务 → 到期定时器 → poll → 派发」四阶段推进，直到 Stop。
 *
 * @param 无（依赖 impl_、stop_、tick_ 等成员状态）。
 *
 * @return
 * - 无返回值：仅在本循环因 stop_ 置位或 poll 致命错误而退出——调用方**必须**在返回后自行清理已挂载资源；
 * - 循环体内的派发不产生返回值——调用方**不要**期望这里上报单次 IO 结果，结果经 Protocol 回调送出。
 *
 * ## 定位
 * 整个 rd_net 的唯一驱动点；tick 顺序（任务→定时器→IO）决定了同一时刻的公平性与延迟上界。
 * 写错会让定时器饿死（IO 密集时永不触发）或 Post 任务插队，破坏调用方对时序的假设。
 *
 * ## 步骤
 * step1. 读取 tick 起点作为 cutoff 并自增 tick 计数，保证本轮到期判定有统一时间基准；
 * step2. 整批排空 Post 队列（FIFO），每轮检查 Stop 以便及时退出；
 * step3. 触发 cutoff 之前到期的定时器（回调内 Stop 也立即退出）；
 * step4. 刷新 poll 集合并计算本轮超时（最近定时器截止时间，被 kMaxPollTimeoutMs 截断）；
 * step5. 调 poll：EINTR 直接重试下一轮，其它错误 Stop 并退出；成功则派发就绪事件。
 *
 * ## 并发/失败语义
 * - 调用约束：只能由一条线程调用 Run；Post/ScheduleAfter 可跨线程（内部有锁）但 Run 本身不可重入。
 * - 原子性：tick 计数是原子自增，仅用于观测；stop_ 用 acquire 读取，保证外部 Stop 能被看到。
 * - 失败语义：不抛异常（回调异常由回调方自行处理）；poll 致命错误等价于 Stop。
 * - 不变量：退出时 stop_ 为真；每轮至多派发一轮 poll 结果，不会在同一 tick 内二次阻塞。
 */
void EventLoop::Run() {
    if (!impl_) {
        return;
    }
    // step1: 每轮取 tick 起点作为 cutoff 并自增计数，保证本轮到期判定共用同一时间基准。
    while (!stop_.load(std::memory_order_acquire)) {
        const EventTimePoint cutoff = EventClock::now();
        tick_.fetch_add(1, std::memory_order_acq_rel);

        // 阶段 1：Post 任务（FIFO 整批）。
        // step2: 整批排空 Post 队列（FIFO），并在批后检查 Stop 以便及时退出。
        DrainPostedTasks();
        if (stop_.load(std::memory_order_acquire)) {
            break;
        }

        // 阶段 2：截止 tick 起点已经到期的定时器。
        // step3: 触发 cutoff 之前到期的定时器；回调内 Stop 也会立即退出。
        FireDueTimers(cutoff);
        if (stop_.load(std::memory_order_acquire)) {
            break;
        }

        // 阶段 3：poll 等待 + IO 派发。
        RefreshPollSet();
        const int timeout_ms = ComputePollTimeoutMs();
        // step4: 刷新 poll 集合并按最近定时器截止时间计算超时；EINTR 重试，其它错误 Stop 退出。
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
        // step5: 派发本轮就绪事件，把数据推给对应协议处理器。
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

/**
 * @brief 弹出并执行所有截止时间不晚于 cutoff 的定时器回调，跳过已取消项。
 *
 * @param cutoff [in] 本轮到期判定上界——用 Run 的 tick 起点，保证同一 tick 内判定一致；早于等于它的定时器才触发。
 *
 * ## 定位
 * 定时器堆的唯一消费点；取消与到期的竞争在这里收敛。
 * 写错会重复触发同一回调或漏触发（堆序被破坏），前者导致重传风暴，后者导致超时逻辑失效。
 *
 * @return 无返回值——到期回调在本次调用内同步执行；调用方**不要**在其返回后再次假定定时器未触发。
 *
 * ## 步骤
 * step1. 循环看堆顶：先剔除已取消的 id（惰性删除），保持 pop_heap/pop_back 的堆结构；
 * step2. 堆顶截止时间晚于 cutoff 时停止（堆按最早到期排序，后续不可能到期）；
 * step3. 弹出堆顶并执行回调；每执行一个都检查 Stop，保证长回调链不会拖延关停。
 *
 * ## 并发/失败语义
 * - 调用约束：只能在 Run 所在线程调用；timers/cancelled 无锁。
 * - 原子性：单个定时器「弹出 + 执行」之间不原子，回调内 CancelTimer 只能影响尚未弹出的项。
 * - 失败语义：不抛异常到循环层；回调自身异常需在回调内消化。
 * - 不变量：返回后堆顶的截止时间一定晚于 cutoff；被取消的定时器不会再触发。
 */
void EventLoop::FireDueTimers(EventTimePoint cutoff) {
    // step1: 先惰性剔除堆顶已取消的 id，保持 pop_heap/pop_back 的堆结构完整。
    std::vector<TimerEntry>& timers = impl_->timers;
    while (!timers.empty()) {
        if (impl_->cancelled.erase(timers.front().id) > 0) {
            std::pop_heap(timers.begin(), timers.end(), TimerEarlier{});
            timers.pop_back();
            continue;
        }
        // step2: 堆按最早到期排序，堆顶晚于 cutoff 即说明本轮没有更多到期项，直接结束。
        if (timers.front().deadline > cutoff) {
            break;
        }
        std::pop_heap(timers.begin(), timers.end(), TimerEarlier{});
        TimerEntry entry = std::move(timers.back());
        timers.pop_back();
        // step3: 弹出堆顶并执行回调；每执行一个都检查 Stop，避免长回调链拖延关停。
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

#else  // defined(_WIN32)：Winsock 事件后端（WSAEventSelect + WaitForMultipleObjectsEx）。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

/// @file event_loop.cpp（Windows 分支）
/// @brief 单线程事件循环的 Windows 后端：每个 socket 关联一个 WSAEVENT
///        （WSAEventSelect），用 WaitForMultipleObjectsEx 统一等待 socket 事件
///        与 Post/Stop 的唤醒事件；定时器由 select 式等待的毫秒超时实现。
///
/// @details 为什么不用 WSAPoll：WSAPoll 在 Windows 上有已知缺陷（对非阻塞
///          connect 完成后不报 POLLOUT、UDP 的 POLLOUT/POLLHUP 语义与 poll(2)
///          不一致），且它**无法等待一个非 socket 的 HANDLE**，所以 Post 的
///          跨线程唤醒只能另找机制。本实现改用 WSAEventSelect +
///          WaitForMultipleObjectsEx：语义与 poll(2) 的「可读」等价，唤醒用
///          CreateEvent 的自动重置事件，代价是句柄数受
///          MAXIMUM_WAIT_OBJECTS=64 约束——每个 loop 最多 63 个 socket。
///
///          定时器：EventClock(steady_clock) 算 deadline，把「距最近 deadline
///          的剩余毫秒」作为 WaitForMultipleObjectsEx 的超时；不建线程，
///          与 Linux 的 poll timeout 路径同构。

namespace rd::net {
namespace {

/// @brief 单个 socket 单 tick 最多派发的包数（REQ-007 约束 3）。
constexpr std::size_t kMaxPacketsPerTick = 64;
/// @brief 单次收发的临时缓冲（数据报超过它时返回 kTruncated）。
constexpr std::size_t kScratchBytes = 65536;
/// @brief cancelled_ 集合超过该阈值时重建堆，避免长期累积已触发的 id。
constexpr std::size_t kCancelPruneThreshold = 4096;
/// @brief 单次最长等待，避免极端情况下长时间不检查 stop_。
constexpr int kMaxPollTimeoutMs = 60000;
/// @brief Windows 等待句柄硬上限（MAXIMUM_WAIT_OBJECTS = 64）。
///        索引 0 固定给唤醒事件，故单 loop 最多 63 个 socket；
///        超过时 Attach* 返回 false（见 docs/reports/platform-diff.md）。
constexpr std::size_t kMaxWaitHandles = 64;

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
///        保证同一 tick 内等待句柄下标到 slot 的映射不会因 erase 而错位。
struct Slot {
    SOCKET socket{INVALID_SOCKET};
    WSAEVENT event{WSA_INVALID_EVENT};
    bool is_stream{false};
    bool active{false};
    DatagramSocket* datagram{nullptr};
    StreamSocket* stream{nullptr};
    Protocol* datagram_handler{nullptr};
    StreamProtocol* stream_handler{nullptr};
};

/// @brief 进程级 Winsock 初始化（一次，线程安全）；返回 0 或 WSA 错误码。
int EnsureWinsock() noexcept {
    static const int result = []() noexcept -> int {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data);
    }();
    return result;
}

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

    /// @brief 跨线程唤醒事件（自动重置）：Wait 消费信号后无需手动 Reset，
    ///        避免「先 Set 再 Reset」丢唤醒的经典竞态。
    HANDLE wake_event{nullptr};
    std::vector<Slot> slots;
    /// @brief 等待句柄表：handles[0]=wake_event，handles[i+1]=第 i 个 active slot。
    std::vector<HANDLE> handles;
    /// @brief handles[1+i] 对应的 slots 下标（slots 内的 active 过滤顺序）。
    std::vector<std::size_t> handle_to_slot;
    std::vector<std::uint8_t> scratch;

    ~Impl() {
        // 未 Detach 的槽位：WSAEVENT 由 loop 创建，必须由 loop 关闭，
        // 否则 EventLoop 析构会泄漏句柄（Linux 的 fd 由 socket 自己持有，
        // 没有这个问题）。
        for (Slot& slot : slots) {
            if (slot.event != WSA_INVALID_EVENT) {
                ::WSACloseEvent(slot.event);
                slot.event = WSA_INVALID_EVENT;
            }
        }
        if (wake_event != nullptr) {
            ::CloseHandle(wake_event);
            wake_event = nullptr;
        }
    }
};

EventLoop::EventLoop() : impl_(std::make_unique<Impl>()) {}
EventLoop::~EventLoop() = default;

Result<std::unique_ptr<EventLoop>> EventLoop::Create() {
    const int startup = EnsureWinsock();
    if (startup != 0) {
        return Result<std::unique_ptr<EventLoop>>::Err(IoStatus::kIoError, startup);
    }
    std::unique_ptr<EventLoop> loop(new EventLoop());
    Impl& impl = *loop->impl_;
    impl.wake_event = ::CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (impl.wake_event == nullptr) {
        return Result<std::unique_ptr<EventLoop>>::Err(
            IoStatus::kIoError, static_cast<int>(::GetLastError()));
    }
    impl.scratch.resize(kScratchBytes);
    impl.handles.reserve(kMaxWaitHandles);
    impl.handle_to_slot.reserve(kMaxWaitHandles);
    return Result<std::unique_ptr<EventLoop>>::Ok(std::move(loop));
}

/// @brief 唤醒阻塞中的 WaitForMultipleObjectsEx（可从任意线程调用）。
void EventLoop::Wake() {
    if (!impl_ || impl_->wake_event == nullptr) {
        return;
    }
    ::SetEvent(impl_->wake_event);
}

/// @brief 消费唤醒信号。Windows 用自动重置事件，Wait 返回时信号已被消耗，
///        这里只需保证没有残留（重复 SetEvent 只保留一次信号，Post 队列
///        是整批 swap，因此不会丢任务）。
void EventLoop::DrainWakeFd() {}

/**
 * @brief 把 UDP socket 关联到 WSAEVENT 并挂到等待表。
 *
 * @param socket [in] 数据报 socket——按引用借用，生命周期必须长于挂载期。
 * @param handler [in] 协议处理器——可为 nullptr；非空时立即回调 OnAttached。
 *
 * @return
 * - true：已挂载（或已在挂载，仅替换 handler）——调用方**必须**保证 socket 活到 Detach；
 * - false：impl_ 缺失、socket 无效、句柄配额（63）已满或 WSAEventSelect 失败
 *   ——调用方**不要**假定已挂载。
 *
 * ## 定位
 * 事件循环与 socket 之间的唯一挂载点；等待句柄到 slot 的映射是事件派发依据。
 * 写错会把 A 的可读事件派发给 B（数据错配）。
 *
 * ## 步骤
 * step1. 校验 impl_ 与 socket，查重（同 socket 只替换 handler）；
 * step2. 配额检查：slots 已满 63 时拒绝（等待句柄上限 64 含唤醒事件）；
 * step3. WSACreateEvent + WSAEventSelect 注册 FD_READ（UDP 无 FD_CLOSE 语义）；
 * step4. 复用空槽位或追加，成功后回调 OnAttached。
 *
 * ## 并发/失败语义
 * - 调用约束：只能在事件循环线程或 Run 之前调用；slot 表无锁。
 * - 失败语义：不抛异常；失败时已创建的事件对象被关闭，slot 表不变。
 * - 不变量：返回 true 时恰好一个 active slot 指向该 socket。
 */
bool EventLoop::AttachDatagram(DatagramSocket& socket, Protocol* handler) {
    // step1: 校验与查重。
    if (!impl_ || !socket.valid()) {
        return false;
    }
    for (Slot& slot : impl_->slots) {
        if (slot.active && slot.datagram == &socket) {
            slot.datagram_handler = handler;
            return true;
        }
    }
    // step2: 句柄配额（64 - 1 个唤醒事件）。
    if (impl_->slots.size() >= kMaxWaitHandles - 1) {
        return false;
    }
    // step3: 事件对象 + 网络事件注册。
    Slot fresh;
    fresh.socket = static_cast<SOCKET>(socket.native_handle());
    fresh.event = ::WSACreateEvent();
    if (fresh.event == WSA_INVALID_EVENT) {
        return false;
    }
    if (::WSAEventSelect(fresh.socket, fresh.event, FD_READ) == SOCKET_ERROR) {
        ::WSACloseEvent(fresh.event);
        return false;
    }
    fresh.is_stream = false;
    fresh.active = true;
    fresh.datagram = &socket;
    fresh.datagram_handler = handler;
    // step4: 复用空槽位，无空位才追加。
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
    if (handler != nullptr) {
        handler->OnAttached(*this);
    }
    return true;
}

/// @brief 同 AttachDatagram；TCP 额外注册 FD_CLOSE（对端 FIN 时唤醒）。
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
    if (impl_->slots.size() >= kMaxWaitHandles - 1) {
        return false;
    }
    Slot fresh;
    fresh.socket = static_cast<SOCKET>(socket.native_handle());
    fresh.event = ::WSACreateEvent();
    if (fresh.event == WSA_INVALID_EVENT) {
        return false;
    }
    if (::WSAEventSelect(fresh.socket, fresh.event, FD_READ | FD_CLOSE) == SOCKET_ERROR) {
        ::WSACloseEvent(fresh.event);
        return false;
    }
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

/**
 * @brief 释放一个 slot：解绑网络事件、关闭事件对象、回调 OnDetached。
 *
 * @param slot_index [in] slot 下标——越界或已 inactive 时静默返回（幂等）。
 *
 * ## 定位
 * slot 生命周期的唯一回收点；Detach 与「socket 出错被摘除」都走这里。
 * 写错会残留 active slot → 等待表里留下已关闭的句柄（Wait 立即返回，
 * 空转烧 CPU）或悬垂 socket 指针。
 *
 * ## 步骤
 * step1. 越界与幂等早退；
 * step2. 先置 active=false，再 WSAEventSelect(socket, nullptr, 0) 解绑；
 * step3. WSACloseEvent 关闭事件对象并清空句柄字段；
 * step4. 取出 handler、清空引用后回调 OnDetached。
 *
 * ## 并发/失败语义
 * - 调用约束：只能在事件循环线程调用；slot 表无锁。
 * - 失败语义：不抛异常；重复调用为 no-op。
 * - 不变量：返回后该 slot active=false 且不持有 socket/event/handler。
 */
void EventLoop::DetachSlot(std::size_t slot_index) {
    // step1: 越界与幂等早退。
    if (!impl_ || slot_index >= impl_->slots.size()) {
        return;
    }
    Slot& slot = impl_->slots[slot_index];
    if (!slot.active) {
        return;
    }
    // step2: 先摘除 active，再解绑网络事件。
    slot.active = false;
    if (slot.socket != INVALID_SOCKET) {
        (void)::WSAEventSelect(slot.socket, nullptr, 0);
        slot.socket = INVALID_SOCKET;
    }
    // step3: 关闭事件对象，避免句柄泄漏。
    if (slot.event != WSA_INVALID_EVENT) {
        ::WSACloseEvent(slot.event);
        slot.event = WSA_INVALID_EVENT;
    }
    // step4: 取 handler、清引用后再回调，回调内看不到失效槽位。
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
        // 新定时器成为堆顶：若正在等待，唤醒它以重新计算超时。
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

/**
 * @brief 事件循环主循环：Post 任务 → 到期定时器 → 等待 → 派发。
 *
 * @param 无（依赖 impl_、stop_、tick_）。
 *
 * @return 无返回值——只在 stop_ 置位或等待失败时退出；调用方**必须**在返回后
 *         自行清理已挂载资源。
 *
 * ## 定位
 * 整个 rd_net 的唯一驱动点；tick 顺序决定公平性与延迟上界。
 * 写错会让定时器饿死（IO 密集时永不触发）或 Post 任务插队。
 *
 * ## 步骤
 * step1. 取 tick 起点作为 cutoff 并自增计数；
 * step2. 整批排空 Post 队列；
 * step3. 触发 cutoff 前到期的定时器；
 * step4. 重建等待句柄表，计算毫秒超时；
 * step5. WaitForMultipleObjectsEx：WAIT_FAILED 退出；超时下一轮；否则派发
 *        对应 slot（索引 0 是唤醒事件，只消费信号）。
 *
 * ## 并发/失败语义
 * - 调用约束：只能由一条线程调用 Run；Post/Stop 可跨线程。
 * - 失败语义：不抛异常；等待失败等价 Stop。
 * - 不变量：退出时 stop_ 为真；每轮至多派发一个句柄。
 */
void EventLoop::Run() {
    if (!impl_) {
        return;
    }
    // step1: 每轮取 tick 起点作为 cutoff 并自增计数。
    while (!stop_.load(std::memory_order_acquire)) {
        const EventTimePoint cutoff = EventClock::now();
        tick_.fetch_add(1, std::memory_order_acq_rel);

        // step2: 阶段 1：Post 任务（FIFO 整批）。
        DrainPostedTasks();
        if (stop_.load(std::memory_order_acquire)) {
            break;
        }

        // step3: 阶段 2：已到期定时器。
        FireDueTimers(cutoff);
        if (stop_.load(std::memory_order_acquire)) {
            break;
        }

        // step4: 阶段 3：重建等待集合并计算超时。
        RefreshPollSet();
        const int timeout_ms = ComputePollTimeoutMs();
        const DWORD handle_count = static_cast<DWORD>(impl_->handles.size());
        const DWORD wait_ms = timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
        // step5: 统一等待 socket 事件与唤醒事件。
        const DWORD ready = ::WaitForMultipleObjectsEx(
            handle_count, impl_->handles.data(), FALSE, wait_ms, FALSE);
        if (ready == WAIT_FAILED) {
            Stop();
            break;
        }
        if (ready == WAIT_TIMEOUT) {
            continue;
        }
        const std::size_t signaled = static_cast<std::size_t>(ready - WAIT_OBJECT_0);
        if (signaled == 0) {
            // 唤醒事件：Post/Stop/ScheduleAfter 已写入共享状态，下一轮处理。
            DrainWakeFd();
            continue;
        }
        DispatchReadyEvents(static_cast<int>(signaled));
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

/**
 * @brief 弹出并执行所有 deadline <= cutoff 的定时器，跳过已取消项。
 *
 * @param cutoff [in] 本轮到期判定上界（tick 起点）。
 *
 * ## 定位
 * 定时器堆的唯一消费点；取消与到期的竞争在这里收敛。
 * 写错会重复触发（重传风暴）或漏触发（超时逻辑失效）。
 *
 * ## 步骤
 * step1. 惰性剔除堆顶已取消的 id；
 * step2. 堆顶晚于 cutoff 即结束；
 * step3. 弹出并执行，每执行一个检查 Stop。
 *
 * ## 并发/失败语义
 * - 调用约束：只能由 Run 所在线程调用；timers/cancelled 无锁。
 * - 不变量：返回后堆顶 deadline 一定晚于 cutoff。
 */
void EventLoop::FireDueTimers(EventTimePoint cutoff) {
    // step1: 惰性剔除已取消的堆顶。
    std::vector<TimerEntry>& timers = impl_->timers;
    while (!timers.empty()) {
        if (impl_->cancelled.erase(timers.front().id) > 0) {
            std::pop_heap(timers.begin(), timers.end(), TimerEarlier{});
            timers.pop_back();
            continue;
        }
        // step2: 堆顶晚于 cutoff 说明本轮没有更多到期项。
        if (timers.front().deadline > cutoff) {
            break;
        }
        std::pop_heap(timers.begin(), timers.end(), TimerEarlier{});
        TimerEntry entry = std::move(timers.back());
        timers.pop_back();
        // step3: 执行回调并检查 Stop。
        if (entry.callback) {
            entry.callback();
        }
        if (stop_.load(std::memory_order_acquire)) {
            return;
        }
    }
}

/// @brief 重建等待句柄表：[唤醒事件, 各 active socket 的 WSAEVENT, ...]。
void EventLoop::RefreshPollSet() {
    impl_->handles.clear();
    impl_->handle_to_slot.clear();
    impl_->handles.push_back(impl_->wake_event);
    for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
        const Slot& slot = impl_->slots[i];
        if (!slot.active || slot.socket == INVALID_SOCKET ||
            slot.event == WSA_INVALID_EVENT) {
            continue;
        }
        impl_->handles.push_back(slot.event);
        impl_->handle_to_slot.push_back(i);
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

/**
 * @brief 派发一个被信号唤醒的 socket 事件。
 *
 * @param ready [in] WaitForMultipleObjectsEx 返回的句柄序号（1 起，0 是唤醒事件）。
 *
 * ## 定位
 * WSAEventSelect 的「可读」到 recv 的唯一桥接点。写错会把 A 的事件派给 B。
 *
 * ## 步骤
 * step1. 用 handle_to_slot 把句柄序号映射回 slot 下标；
 * step2. WSAEnumNetworkEvents 取事件并**重置事件对象**（等价 poll 的消费）；
 * step3. 按 stream/datagram 走对应的 drain。
 *
 * ## 并发/失败语义
 * - 调用约束：只在 Run 所在线程调用。
 * - 失败语义：枚举失败视为 socket 失效，DetachSlot 摘除。
 */
void EventLoop::DispatchReadyEvents(int ready) {
    if (ready <= 0) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(ready);
    if (index > impl_->handle_to_slot.size()) {
        return;
    }
    // step1: 句柄序号 → slot 下标。
    const std::size_t slot_index = impl_->handle_to_slot[index - 1];
    Slot& slot = impl_->slots[slot_index];
    if (!slot.active) {
        return;
    }
    // step2: 取事件并重置事件对象；FD_CLOSE/FD_READ 都走 drain（recv 决定语义）。
    WSANETWORKEVENTS events{};
    if (::WSAEnumNetworkEvents(slot.socket, slot.event, &events) == SOCKET_ERROR) {
        DetachSlot(slot_index);
        return;
    }
    // step3: 按槽位类型 drain。
    if (slot.is_stream) {
        DrainStreamSlot(slot_index);
    } else {
        DrainDatagramSlot(slot_index);
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

#endif  // _WIN32
