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
