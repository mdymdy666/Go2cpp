#include "go2cpp/io.hpp"

#include "go2cpp/concurrency/core/futex_mutex.hpp"

#include <sys/syscall.h>
#include <unistd.h>

#include <fcntl.h>
#include <poll.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <deque>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace go2cpp::io {
namespace {


constexpr std::uint64_t kWakeRegistration = 0;
constexpr std::uint32_t kReadMask = 1U;
constexpr std::uint32_t kWriteMask = 2U;

std::mutex& manager_registry_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<Scheduler*, IOManager*>& manager_registry() {
    static std::unordered_map<Scheduler*, IOManager*> registry;
    return registry;
}

// 后端拥有 poller 和 token 状态，与 IOManager 一对一绑定，禁止跨管理器复用。
std::mutex& backend_owner_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<IOBackend*, const void*>& backend_owners() {
    static std::unordered_map<IOBackend*, const void*> owners;
    return owners;
}

bool descriptor_is_open(int fd) noexcept {
    if (fd < 0) {
        errno = EBADF;
        return false;
    }
    // 只有 F_GETFD 成功才能证明描述符仍然有效。将 EIO/EINTR 等其余
    // 错误误判为“仍打开”会把无效 fd 注册到 epoll，最终造成永久等待。
    for (;;) {
#ifdef SYS_fcntl
        const long result = ::syscall(SYS_fcntl, fd, F_GETFD);
#else
        const int result = ::fcntl(fd, F_GETFD);
#endif
        if (result >= 0) return true;
        if (errno == EINTR) continue;
        return false;
    }
}

std::uint32_t event_mask(IOEvent event) noexcept {
    switch (event) {
        case IOEvent::kRead:
            return kReadMask;
        case IOEvent::kWrite:
            return kWriteMask;
    }
    return 0;
}

/// 函数功能：对单个 fd 执行零超时就绪探测。
/// 执行流程：使用原生 poll 系统调用，不进入 Hook，避免零超时路径递归
/// 注册 Fiber 等待；调用方已完成 Context 状态检查。
/// @param[in] fd 待探测的文件描述符。
/// @param[in] event 读或写事件。
/// @param[in] expected_descriptor 可选的 fd 代际令牌。
/// @return 返回 ready、timeout 或错误状态。
WaitResult probe_immediate(int fd, IOEvent event,
                           const DescriptorTokenPtr& expected_descriptor) {
    if (event_mask(event) == 0) return {WaitStatus::kError, EINVAL};
    if ((expected_descriptor &&
         (!expected_descriptor->valid() || expected_descriptor->fd() != fd)) ||
        (!expected_descriptor && !descriptor_is_open(fd))) {
        return {WaitStatus::kError, EBADF};
    }
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = event == IOEvent::kRead
                            ? static_cast<short>(POLLIN | POLLPRI)
                            : POLLOUT;
    const long result = ::syscall(SYS_poll, &descriptor, 1, 0);
    if (result < 0) {
        return {WaitStatus::kError, errno == 0 ? EIO : errno};
    }
    if ((descriptor.revents & POLLNVAL) != 0) {
        return {WaitStatus::kError, EBADF};
    }
    const short ready_mask = static_cast<short>(POLLIN | POLLOUT | POLLPRI |
                                                 POLLERR | POLLHUP
#ifdef POLLRDHUP
                                                 | POLLRDHUP
#endif
    );
    if ((descriptor.revents & ready_mask) != 0) {
        return {WaitStatus::kReady, 0};
    }
    return {WaitStatus::kTimeout, ETIMEDOUT};
}

std::uint64_t encode_outcome(WaitStatus status, int system_error) noexcept {
    return static_cast<std::uint64_t>(status) |
           (static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(system_error))
            << 8U);
}

WaitResult decode_outcome(std::uint64_t outcome) noexcept {
    if (outcome == 0) {
        return {WaitStatus::kError, ECANCELED};
    }
    return {static_cast<WaitStatus>(outcome & 0xffU),
            static_cast<int>(static_cast<std::uint32_t>(outcome >> 8U))};
}

}  // namespace

struct DescriptorRegistryState {
    std::recursive_mutex m_mutex;
    std::unordered_map<int, std::weak_ptr<DescriptorToken>> m_tokens;
    std::uint64_t m_next_generation{0};
};

namespace {
std::shared_ptr<DescriptorRegistryState> descriptor_registry() {
    static auto s_registry = std::make_shared<DescriptorRegistryState>();
    return s_registry;
}

using CloseCallback = std::function<void(int)>;
std::unordered_map<Scheduler*, CloseCallback>& close_registry() {
    static std::unordered_map<Scheduler*, CloseCallback> s_registry;
    return s_registry;
}
}  // namespace

DescriptorToken::DescriptorToken(
    int fd, std::uint64_t generation,
    std::weak_ptr<DescriptorRegistryState> registry)
    : m_fd(fd), m_generation(generation), m_registry(std::move(registry)) {}

DescriptorToken::~DescriptorToken() {
    if (const auto registry = m_registry.lock()) {
        std::lock_guard<std::recursive_mutex> lock(registry->m_mutex);
        const auto found = registry->m_tokens.find(m_fd);
        if (found != registry->m_tokens.end() && found->second.expired()) {
            registry->m_tokens.erase(found);
        }
    }
}

DescriptorGuard::DescriptorGuard()
    : m_state(descriptor_registry()), m_lock(m_state->m_mutex) {}
DescriptorGuard::~DescriptorGuard() = default;

DescriptorTokenPtr DescriptorGuard::Capture(int fd) {
    DescriptorGuard guard;
    if (!descriptor_is_open(fd)) {
        return {};
    }
    const auto found = guard.m_state->m_tokens.find(fd);
    if (found != guard.m_state->m_tokens.end()) {
        if (auto token = found->second.lock()) {
            if (token->valid()) {
                return token;
            }
        }
    }
    if (++guard.m_state->m_next_generation == 0) {
        ++guard.m_state->m_next_generation;
    }
    auto token = std::shared_ptr<DescriptorToken>(new DescriptorToken(
        fd, guard.m_state->m_next_generation, guard.m_state));
    guard.m_state->m_tokens[fd] = token;
    return token;
}

void DescriptorGuard::Invalidate(int fd) noexcept {
    DescriptorGuard guard;
    const auto found = guard.m_state->m_tokens.find(fd);
    if (found != guard.m_state->m_tokens.end()) {
        if (const auto token = found->second.lock()) {
            token->m_valid.store(false, std::memory_order_release);
        }
        guard.m_state->m_tokens.erase(found);
    }
}

struct IOManager::State : public std::enable_shared_from_this<State> {
    // epoll token 直接编码 fd 与注册序号，避免每次 ONESHOT rearm 都在
    // m_registrations 哈希表中分配/释放节点。序号用于拒绝已经从内核
    // 队列迟到的旧事件；fd 仍在 State::m_mutex 下重新查找，关闭后复用
    // 时不会解引用悬空指针。token=0 保留给内部 eventfd。
    static constexpr std::uint64_t kRegistrationSequenceMask = 0xffffffffULL;
    static std::uint64_t make_registration_token(int fd,
                                                  std::uint64_t sequence) noexcept {
        const auto fd_bits = static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(fd) + 1U);
        const auto sequence_bits = sequence & kRegistrationSequenceMask;
        return (fd_bits << 32U) | (sequence_bits == 0 ? 1U : sequence_bits);
    }
    static int token_fd(std::uint64_t token) noexcept {
        const auto fd_bits = static_cast<std::uint32_t>(token >> 32U);
        return fd_bits == 0 ? -1 : static_cast<int>(fd_bits - 1U);
    }
    struct WaitNode {
        std::uint64_t id{0};
        int fd{-1};
        std::uint64_t generation{0};
        IOEvent event{IOEvent::kRead};
        Scheduler* scheduler{nullptr};
        std::shared_ptr<Task> task;
        DescriptorTokenPtr descriptor;
        core::FutexMutex wake_mutex;
        bool wake_active{true};
        std::atomic<std::uint64_t> outcome{0};
        std::shared_ptr<WaitNode> wake_next;
        std::mutex callback_mutex;
        std::condition_variable callback_cv;
        // 无 Context 的 IO 等待占绝大多数热路径；此标志允许回收时
        // 跳过两次 callback_mutex 加锁。置位发生在 AddCallback 之前，
        // 因而即使取消回调在注册期间同步执行，回收仍会等待它结束。
        std::atomic<bool> callbacks_registered{false};
        bool callback_active{false};
        bool callbacks_stopped{false};
        // 回收保护使用一个小型无锁状态机，避免每次 epoll 唤醒都争用
        // std::mutex。状态含义：0=空闲，1=有一个唤醒操作，2=停止中，
        // 3=已停止。WaitNode 同一代只允许一个活动操作；回收先把状态
        // 置为停止中，因而不会再有新的操作进入，随后等待当前操作结束。
        std::atomic<std::uint8_t> operation_state{0};

        void wake() noexcept {
            std::lock_guard<core::FutexMutex> lock(wake_mutex);
            if (wake_active && scheduler && task) {
                (void)scheduler->wake_io(task);
            }
        }

        /// 返回当前仍可唤醒的任务快照；不执行调度操作，供批量唤醒路径
        /// 在释放每个节点锁后统一按 P 分桶入队。
        std::shared_ptr<Task> wake_task() noexcept {
            std::lock_guard<core::FutexMutex> lock(wake_mutex);
            return wake_active ? task : std::shared_ptr<Task>{};
        }

        // WakeList 中的节点已经完成 claim 并持有 operation token；回收
        // 线程在该 token 结束前不会执行 disarm/reset。因此批量唤醒可以
        // 直接读取快照，避免每个 readiness 事件再次获取 wake_mutex。
        std::shared_ptr<Task> wake_task_claimed() const noexcept {
            return wake_active ? task : std::shared_ptr<Task>{};
        }

        void disarm() noexcept {
            std::lock_guard<core::FutexMutex> lock(wake_mutex);
            wake_active = false;
            scheduler = nullptr;
        }

        void reset() noexcept {
            id = 0;
            fd = -1;
            generation = 0;
            event = IOEvent::kRead;
            scheduler = nullptr;
            task.reset();
            descriptor.reset();
            wake_active = true;
            outcome.store(0, std::memory_order_relaxed);
            wake_next.reset();
            // reset 只在 stop_callbacks() 返回后执行，因而没有回调仍在
            // 观察业务字段；标志本身仍用同一把锁复位，避免新的
            // begin_callback() 与复用操作发生数据竞争。
            if (callbacks_registered.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                callbacks_stopped = false;
                callback_active = false;
            } else {
                // 本代从未注册 Context 回调，节点尚未对外发布，
                // 不需要为回收路径重新获取 callback_mutex。
                callbacks_stopped = false;
                callback_active = false;
            }
            callbacks_registered.store(false, std::memory_order_release);
            // 节点从 TLS 缓存复用时必须开启新一代操作生命周期；否则
            // 上一代 stop_operations 设置的停止位会让后续就绪事件被丢弃。
            operation_state.store(0, std::memory_order_release);
        }

        bool begin_callback() noexcept {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (callbacks_stopped) {
                return false;
            }
            callback_active = true;
            return true;
        }

        void end_callback() noexcept {
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                callback_active = false;
            }
            callback_cv.notify_all();
        }

        void stop_callbacks() noexcept {
            if (!callbacks_registered.load(std::memory_order_acquire)) {
                return;
            }
            std::unique_lock<std::mutex> lock(callback_mutex);
            callbacks_stopped = true;
            callback_cv.wait(lock, [this] { return !callback_active; });
        }

        bool begin_operation() noexcept {
            std::uint8_t expected = 0;
            return operation_state.compare_exchange_strong(
                expected, static_cast<std::uint8_t>(1),
                std::memory_order_acq_rel, std::memory_order_acquire);
        }

        void end_operation() noexcept {
            // 1->0 是正常完成；若回收线程已经把状态改为 2，
            // 则直接发布 3。这里不能无条件 store(0)，否则回收线程
            // 在 2->3 之间可能被新的 begin_operation 插入。
            std::uint8_t expected = 1;
            if (operation_state.compare_exchange_strong(
                    expected, static_cast<std::uint8_t>(0),
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                return;
            }
            if (expected == 2) {
                operation_state.store(3, std::memory_order_release);
            }
        }

        void stop_operations() noexcept {
            for (;;) {
                std::uint8_t expected = 0;
                if (operation_state.compare_exchange_weak(
                        expected, static_cast<std::uint8_t>(3),
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return;
                }
                if (expected == 3) {
                    return;
                }
                if (expected == 1) {
                    std::uint8_t active = 1;
                    (void)operation_state.compare_exchange_strong(
                        active, static_cast<std::uint8_t>(2),
                        std::memory_order_acq_rel,
                        std::memory_order_acquire);
                }
                // 活动操作只包含一次 Scheduler::wake_io，通常很短；
                // 用 yield 避免为每个节点创建 condition_variable。
                while (operation_state.load(std::memory_order_acquire) == 2) {
                    std::this_thread::yield();
                }
            }
        }
    };

    using NodePtr = std::shared_ptr<WaitNode>;
    using Queue = std::deque<NodePtr>;

    // WaitNode 的所有权会随 Fiber 迁移到不同 M；用当前 M 的 TLS 小缓存
    // 避免每轮 wait 都争用 State 总锁。缓存项不持有 State，只带原始键，
    // 因而 State 销毁不会形成反向生命周期。
    inline static thread_local std::vector<std::pair<State*, NodePtr>>
        s_node_cache;

    class WakeList {
    public:
        void push_back(NodePtr node) noexcept {
            node->wake_next.reset();
            if (m_tail) {
                m_tail->wake_next = node;
            } else {
                m_head = node;
            }
            m_tail = std::move(node);
            ++m_size;
        }
        NodePtr pop_front() noexcept {
            auto node = std::move(m_head);
            if (node) {
                m_head = std::move(node->wake_next);
                if (!m_head) {
                    m_tail.reset();
                }
                --m_size;
            }
            return node;
        }
        std::size_t size() const noexcept { return m_size; }
    private:
        NodePtr m_head;
        NodePtr m_tail;
        std::size_t m_size{0};
    };

    struct FdSlot {
        int fd{-1};
        std::uint64_t generation{0};
        // 强引用跨越同一 fd 的多轮等待，避免每次注册重新分配
        // DescriptorToken；slot 删除时自然释放，fd 代际变化时替换。
        DescriptorTokenPtr descriptor;
        Queue readers;
        Queue writers;
        bool registered{false};
        // 一次性后端事件返回后，armed 表示内核是否仍在监听。
        bool armed{false};
        std::uint32_t interest{0};
        std::uint64_t registration_id{0};
        bool pending_read{false};
        bool pending_write{false};
    };

    using DeadlineOrder = std::multimap<TimePoint, std::uint64_t>;
    struct TimerRecord {
        DeadlineOrder::iterator order;
        std::weak_ptr<WaitNode> node;
    };

    explicit State(Scheduler* scheduler, IOBackendPtr backend)
        : m_scheduler(scheduler), m_backend(std::move(backend)) {
        if (!m_backend) {
            m_backend = CreateBackend();
        }
        if (m_backend) {
            std::lock_guard<std::mutex> lock(backend_owner_mutex());
            const auto [it, inserted] =
                backend_owners().emplace(m_backend.get(), this);
            if (!inserted && it->second != this) {
                m_init_error = EBUSY;
                m_backend.reset();
                return;
            }
            m_backend_owner_registered = true;
        }
        if (!m_backend || !m_backend->initialize(&m_init_error)) {
            if (m_init_error == 0) {
                m_init_error = ENODEV;
            }
        }
    }

    ~State() {
        shutdown();
        if (m_backend) {
            m_backend->shutdown();
        }
        if (m_backend_owner_registered && m_backend) {
            std::lock_guard<std::mutex> lock(backend_owner_mutex());
            const auto found = backend_owners().find(m_backend.get());
            if (found != backend_owners().end() && found->second == this) {
                backend_owners().erase(found);
            }
        }
    }

    bool start() {
        std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mutex);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_accepting) {
                return true;
            }
            if (m_permanently_stopped || m_init_error != 0) {
                return false;
            }
            m_stop_poller = false;
            m_accepting = true;
        }

        try {
            const auto self = shared_from_this();
            m_poller = std::thread([self] { self->poll_loop(); });
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_accepting = false;
            m_stop_poller = true;
            m_init_error = EAGAIN;
            return false;
        }
        m_running.store(true, std::memory_order_release);
        return true;
    }

    void shutdown() noexcept {
        std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mutex);
        WakeList wake;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_permanently_stopped && !m_poller.joinable()) {
                return;
            }
            m_accepting = false;
            m_permanently_stopped = true;
            m_stop_poller = true;

            for (auto& entry : m_slots) {
                FdSlot& slot = *entry.second;
                invalidate_registration_locked(slot);
                drain_queue_locked(slot.readers, WaitStatus::kCancelled,
                                   ECANCELED, wake);
                drain_queue_locked(slot.writers, WaitStatus::kCancelled,
                                   ECANCELED, wake);
            }
            m_slots.clear();
            m_deadline_index.clear();
            m_deadline_order.clear();
        }

        wake_nodes(wake);
        tickle();
        if (m_poller.joinable() &&
            m_poller.get_id() != std::this_thread::get_id()) {
            m_poller.join();
        }
        m_running.store(false, std::memory_order_release);
    }

    bool is_running() const noexcept {
        return m_running.load(std::memory_order_acquire);
    }

    NodePtr register_wait(int fd, IOEvent event,
                          std::optional<TimePoint> deadline, int* error,
                          DescriptorTokenPtr expected_descriptor) {
        // Hook 调用通常已经捕获了代际 token。此路径不再重复取得全局
        // 描述符注册锁，也不重复执行 F_GETFD；token 的有效位就是 close/
        // dup2 交接的线性化结果。未提供 token 的裸 API 仍保留完整校验。
        if (error != nullptr) {
            *error = 0;
        }
        if ((expected_descriptor &&
             (!expected_descriptor->valid() || expected_descriptor->fd() != fd)) ||
            (fd < 0 && !expected_descriptor)) {
            if (error != nullptr) {
                *error = EBADF;
            }
            return {};
        }

        const std::uint32_t mask = event_mask(event);
        if (mask == 0) {
            if (error != nullptr) {
                *error = EINVAL;
            }
            return {};
        }

        auto task = Scheduler::current_task();
        DescriptorTokenPtr descriptor = std::move(expected_descriptor);
        if (!descriptor) {
            // 同一 fd 的连续等待通常跨越多个 Fiber 调度轮次，但仍由同一
            // FdSlot 持有有效代际 token。先读本地缓存，避免每轮 F_GETFD
            // 和全局 descriptor registry 锁；首次注册或 close/dup2 后才
            // 回退到 Capture。这里只复制 shared_ptr，不跨锁持有 State。
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto cached = m_slots.find(fd);
            if (cached != m_slots.end() && cached->second->descriptor &&
                cached->second->descriptor->valid() &&
                cached->second->descriptor->fd() == fd) {
                descriptor = cached->second->descriptor;
            }
        }
        if (!descriptor) {
            descriptor = DescriptorGuard::Capture(fd);
        }
        // Capture 已经在描述符注册表锁内完成 F_GETFD 校验；下面 m_mutex
        // 下的 valid/fd 二次检查仍是 close/dup2 与发布等待节点之间的
        // 线性化门。若缓存 token 在两次加锁之间失效，直接返回 EBADF，
        // 不把旧代际重新放回等待队列。
        if (!descriptor) {
            if (error != nullptr) {
                *error = EBADF;
            }
            return {};
        }
        NodePtr node;

        int arm_error = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_accepting) {
                if (error != nullptr) {
                    *error = ESHUTDOWN;
                }
                return {};
            }
            // 首次校验与取得 m_mutex 之间可能正好发生 close/dup2。
            // 关闭路径先使 DescriptorToken 失效，再唤醒旧 waiter；如果
            // 这里不在发布节点前再次检查，关闭完成后本次 wait 仍会把
            // 旧 fd 放回队列，既没有新的 epoll 注册者，也没有后续关闭
            // 事件可以唤醒它，最终形成永久挂起。该检查是 fd 代际门的
            // 第二个线性化点；检查后到入队期间的失效仍由同一 m_mutex
            // 下的 notify_close 排空处理。
            if (descriptor &&
                (!descriptor->valid() || descriptor->fd() != fd)) {
                if (error != nullptr) {
                    *error = EBADF;
                }
                return {};
            }

            for (auto it = s_node_cache.begin(); it != s_node_cache.end(); ++it) {
                if (it->first == this) {
                    node = std::move(it->second);
                    s_node_cache.erase(it);
                    node->reset();
                    break;
                }
            }
            if (!node) {
                try {
                    node = std::make_shared<WaitNode>();
                } catch (...) {
                    if (error != nullptr) {
                        *error = ENOMEM;
                    }
                    return {};
                }
            }
            node->fd = fd;
            node->event = event;
            node->scheduler = m_scheduler;
            node->task = std::move(task);
            node->descriptor = std::move(descriptor);

            auto found = m_slots.find(fd);
            if (found == m_slots.end()) {
                auto slot = std::make_unique<FdSlot>();
                slot->fd = fd;
                slot->generation = next_generation_locked();
                found = m_slots.emplace(fd, std::move(slot)).first;
            }
            FdSlot& slot = *found->second;
            const auto node_descriptor = node->descriptor;
            if (node_descriptor &&
                slot.generation != node_descriptor->generation()) {
                if (!slot.readers.empty() || !slot.writers.empty()) {
                    if (error != nullptr) {
                        *error = ESTALE;
                    }
                    return {};
                }
                invalidate_registration_locked(slot);
                slot.generation = node_descriptor->generation();
                slot.pending_read = false;
                slot.pending_write = false;
                slot.descriptor = node_descriptor;
            } else if (!slot.descriptor) {
                slot.descriptor = node_descriptor;
            } else if (slot.descriptor->valid() && node_descriptor &&
                       slot.descriptor->generation() ==
                           node_descriptor->generation()) {
                // FdSlot 持有强令牌，后续同 fd 等待复用同一个控制块，
                // 避免每轮 Wait 都重新分配 DescriptorToken。
                node->descriptor = slot.descriptor;
            }
            node->id = next_node_id_locked();
            node->generation = slot.generation;

            try {
                bool consume_pending = event == IOEvent::kRead
                                           ? slot.pending_read
                                           : slot.pending_write;
                bool pending_ready = consume_pending;
                if (consume_pending) {
                    // pending 只是 epoll 的候选提示；FD 可能已被业务线程
                    // 读空。用零超时 poll 做最终确认，避免 stale ready。
                    pollfd probe{};
                    probe.fd = fd;
                    probe.events = event == IOEvent::kRead
                                       ? static_cast<short>(POLLIN | POLLPRI)
                                       : POLLOUT;
                    const long probe_result =
                        ::syscall(SYS_poll, &probe, 1, 0);
                    if (probe_result < 0 && errno != EINTR) {
                        pending_ready = false;
                        (void)claim(*node, WaitStatus::kError,
                                    errno == 0 ? EIO : errno);
                    } else if (probe_result <= 0 ||
                               (probe.revents & POLLNVAL) != 0) {
                        pending_ready = false;
                    } else if ((probe.revents &
                                (POLLIN | POLLOUT | POLLPRI | POLLERR |
                                 POLLHUP
#ifdef POLLRDHUP
                                 | POLLRDHUP
#endif
                                 )) == 0) {
                        pending_ready = false;
                    }
                }
                if (consume_pending && pending_ready) {
                    if (event == IOEvent::kRead) {
                        slot.pending_read = false;
                    } else {
                        slot.pending_write = false;
                    }
                    (void)claim(*node, WaitStatus::kReady, 0);
                } else if (node->outcome.load(std::memory_order_relaxed) == 0) {
                    if (event == IOEvent::kRead) {
                        slot.pending_read = false;
                    } else {
                        slot.pending_write = false;
                    }
                    queue_for(slot, event).push_back(node);
                }
                if (deadline.has_value() &&
                    node->outcome.load(std::memory_order_relaxed) == 0) {
                    const auto order = m_deadline_order.emplace(*deadline, node->id);
                    try {
                        m_deadline_index.emplace(
                            node->id, TimerRecord{order, std::weak_ptr<WaitNode>(node)});
                    } catch (...) {
                        m_deadline_order.erase(order);
                        throw;
                    }
                }
            } catch (...) {
                erase_node_locked(slot, node);
                erase_timer_locked(*node);
                if (slot.readers.empty() && slot.writers.empty() &&
                    !slot.registered) {
                    m_slots.erase(found);
                }
                if (error) {
                    *error = ENOMEM;
                }
                return {};
            }

            arm_error = update_interest_locked(slot);
            if (arm_error != 0) {
                erase_node_locked(slot, node);
                erase_timer_locked(*node);
                (void)claim(*node, WaitStatus::kError, arm_error);
                if (!slot.registered && slot.readers.empty() &&
                    slot.writers.empty()) {
                    m_slots.erase(found);
                }
            }
        }

        if (arm_error != 0) {
            if (error != nullptr) {
                *error = arm_error;
            }
            return node;
        }

        // 新增定时器可能早于 epoll_wait 上一次计算出的超时时间。
        tickle();
        return node;
    }

    bool complete(const NodePtr& node, WaitStatus status,
                  int system_error) noexcept {
        if (!node || !node->begin_operation()) {
            return false;
        }
        if (!claim(*node, status, system_error)) {
            node->end_operation();
            return false;
        }

        WakeList wake;
        wake.push_back(node);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            erase_timer_locked(*node);
            const auto found = m_slots.find(node->fd);
            if (found != m_slots.end() &&
                found->second->generation == node->generation) {
                FdSlot& slot = *found->second;
                erase_node_locked(slot, node);
                const int arm_error = update_interest_locked(slot);
                if (arm_error != 0) {
                    fail_slot_locked(slot, arm_error, wake);
                }
                if (slot.readers.empty() && slot.writers.empty() &&
                    !slot.registered) {
                    m_slots.erase(found);
                }
            }
        }
        wake_nodes(wake);
        tickle();
        return true;
    }

    void recycle_node(NodePtr node) noexcept {
        if (!node) {
            return;
        }
        const bool had_callbacks =
            node->callbacks_registered.load(std::memory_order_acquire);
        node->stop_callbacks();
        node->stop_operations();
        // stop_operations 已经确认没有迟到的完成/唤醒操作。无 Context
        // 的热路径没有外部回调可以再次访问节点，因此不必再取得
        // wake_mutex；带 Context 的节点仍保留 disarm，隔离迟到取消回调。
        if (had_callbacks) {
            node->disarm();
        }
        node->reset();
        try {
            // 常见高并发 IO 会在同一批 Fiber 上反复等待；保留较大的
            // M-local 节点缓存，减少 shared_ptr/WaitNode 的重复分配。
            if (s_node_cache.size() < 4096) {
                s_node_cache.emplace_back(this, std::move(node));
            }
        } catch (...) {
            // 缓存只是性能优化，分配失败不影响等待语义。
        }
    }

    // 仅允许队列头节点由补采样路径完成，保持同一 fd/方向的 FIFO。
    bool complete_if_head(const NodePtr& node, WaitStatus status,
                          int system_error) noexcept {
        if (!node) {
            return false;
        }
        WakeList wake;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_slots.find(node->fd);
            if (found == m_slots.end() ||
                found->second->generation != node->generation) {
                return false;
            }
            FdSlot& slot = *found->second;
            Queue& queue = queue_for(slot, node->event);
            purge_terminal_locked(queue);
            if (queue.empty() || queue.front().get() != node.get()) {
                return false;
            }
            // 在确认仍是队头后再取得操作令牌，和 poller 的
            // take_ready_locked 共用 m_mutex 线性化。这样 shutdown 的
            // drain 不会遇到“外部已登记但节点尚未从队列取出”的窗口。
            if (!node->begin_operation()) {
                return false;
            }
            if (!claim(*node, status, system_error)) {
                node->end_operation();
                if (node->outcome.load(std::memory_order_acquire) != 0) {
                    queue.pop_front();
                }
                return false;
            }
            queue.pop_front();
            if (node->event == IOEvent::kRead) {
                slot.pending_read = false;
            } else {
                slot.pending_write = false;
            }
            erase_timer_locked(*node);
            // 补采样完成节点时，poller 可能已经把旧 registration 的
            // 事件放进内核队列。删除 registration 使迟到事件被忽略，
            // 同时只在这个需要消费旧事件的路径付出 DEL/ADD 成本。
            if (slot.readers.empty() && slot.writers.empty() &&
                slot.registered) {
                invalidate_registration_locked(slot);
            }
            const int arm_error = update_interest_locked(slot);
            if (arm_error != 0) {
                fail_slot_locked(slot, arm_error, wake);
            }
            if (slot.readers.empty() && slot.writers.empty() &&
                !slot.registered) {
                m_slots.erase(found);
            }
            wake.push_back(node);
        }
        wake_nodes(wake);
        tickle();
        return true;
    }

    bool cancel(int fd, IOEvent event) noexcept {
        return finish_fd(fd, event, false, WaitStatus::kCancelled,
                         ECANCELED);
    }

    bool cancel_all(int fd) noexcept {
        return finish_fd(fd, IOEvent::kRead, true, WaitStatus::kCancelled,
                         ECANCELED);
    }

    bool notify_close(int fd) noexcept {
        WakeList wake;
        bool found_slot = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_slots.find(fd);
            if (found == m_slots.end()) {
                return false;
            }
            found_slot = true;
            FdSlot& slot = *found->second;
            invalidate_registration_locked(slot);
            drain_queue_locked(slot.readers, WaitStatus::kClosed, EBADF, wake);
            drain_queue_locked(slot.writers, WaitStatus::kClosed, EBADF, wake);
            m_slots.erase(found);
        }
        wake_nodes(wake);
        tickle();
        return found_slot;
    }

private:
    static bool claim(WaitNode& node, WaitStatus status,
                      int system_error) noexcept {
        std::uint64_t expected = 0;
        return node.outcome.compare_exchange_strong(
            expected, encode_outcome(status, system_error),
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    static Queue& queue_for(FdSlot& slot, IOEvent event) noexcept {
        return event == IOEvent::kRead ? slot.readers : slot.writers;
    }

    static void erase_node_from_queue(Queue& queue,
                                      const NodePtr& node) noexcept {
        const auto found = std::find(queue.begin(), queue.end(), node);
        if (found != queue.end()) {
            queue.erase(found);
        }
    }

    static void erase_node_locked(FdSlot& slot,
                                  const NodePtr& node) noexcept {
        erase_node_from_queue(queue_for(slot, node->event), node);
    }

    static void purge_terminal_locked(Queue& queue) noexcept {
        queue.erase(std::remove_if(queue.begin(), queue.end(),
                                   [](const NodePtr& node) {
                                       return !node ||
                                              node->outcome.load(
                                                  std::memory_order_acquire) !=
                                                  0;
                                   }),
                    queue.end());
    }

    std::uint64_t next_node_id_locked() noexcept {
        if (++m_next_node_id == 0) {
            ++m_next_node_id;
        }
        return m_next_node_id;
    }

    std::uint64_t next_generation_locked() noexcept {
        if (++m_next_generation == 0) {
            ++m_next_generation;
        }
        return m_next_generation;
    }

    std::uint64_t next_registration_id_locked() noexcept {
        ++m_next_registration_id;
        const auto sequence = m_next_registration_id &
                               kRegistrationSequenceMask;
        return sequence == 0 ? 1U : sequence;
    }

    void erase_timer_locked(const WaitNode& node) noexcept {
        const auto timer = m_deadline_index.find(node.id);
        if (timer == m_deadline_index.end()) {
            return;
        }
        m_deadline_order.erase(timer->second.order);
        m_deadline_index.erase(timer);
    }

    void invalidate_registration_locked(FdSlot& slot) noexcept {
        if (slot.registered && m_backend) {
            (void)m_backend->remove(slot.fd);
        }
        slot.registered = false;
        slot.armed = false;
        slot.interest = 0;
        slot.registration_id = 0;
    }

    int update_interest_locked(FdSlot& slot) noexcept {
        purge_terminal_locked(slot.readers);
        purge_terminal_locked(slot.writers);

        std::uint32_t interest = 0;
        if (!slot.readers.empty()) {
            interest |= kReadMask;
        }
        if (!slot.writers.empty()) {
            interest |= kWriteMask;
        }

        const bool one_shot =
            m_backend && m_backend->supports_one_shot();
        if (interest == 0) {
            if (one_shot) {
                // ONESHOT 事件返回后已自动停用；保留内核注册和代际，
                // 下一次 waiter 通过 MOD 重新 arm。取消路径若仍 armed
                // 则必须 DEL，避免无等待者时收到 HUP/ERR。
                if (!slot.registered || !slot.armed) {
                    slot.interest = 0;
                    return 0;
                }
                invalidate_registration_locked(slot);
                return 0;
            }
            // 普通后端在支持时保留 events=0 注册；不支持时沿用删除路径。
            if (!slot.registered || !m_backend ||
                !m_backend->supports_zero_interest()) {
                if (slot.registered && m_backend &&
                    !m_backend->supports_zero_interest()) {
                    invalidate_registration_locked(slot);
                }
                slot.interest = 0;
                return 0;
            }
        }

        // interest 必须是当前等待队列的精确投影。旧实现只在“新集合是
        // 旧集合子集”时直接返回，导致 READ|WRITE 收缩为 WRITE 后仍保留
        // READ 注册；水平触发下会持续收到无消费者的 READ 事件并忙循环。
        if (slot.registered && slot.interest == interest &&
            (!one_shot || slot.armed)) {
            return 0;
        }

        const std::uint64_t registration_id =
            make_registration_token(slot.fd, next_registration_id_locked());

        const int result = m_backend
                               ? m_backend->add_or_modify(
                                     slot.fd, interest, registration_id,
                                     slot.registered)
                               : ENODEV;
        if (result != 0) {
            const int saved_error = result;
            return saved_error;
        }

        slot.registered = true;
        slot.armed = !one_shot || interest != 0;
        slot.interest = interest;
        slot.registration_id = registration_id;
        return 0;
    }

    void drain_queue_locked(Queue& queue, WaitStatus status, int system_error,
                            WakeList& wake) noexcept {
        while (!queue.empty()) {
            // 先登记操作，再从队列移除。complete_if_head 可能已经在
            // 队列外取得该节点的操作令牌并等待 m_mutex；先 pop 会让它
            // 获取锁后找不到节点，最终把未完成节点永久遗留。
            NodePtr node = queue.front();
            const bool started = node && node->begin_operation();
            if (started && claim(*node, status, system_error)) {
                queue.pop_front();
                erase_timer_locked(*node);
                wake.push_back(std::move(node));
            } else if (started) {
                // 只有成功登记活动操作的路径才允许配对结束。
                if (node->outcome.load(std::memory_order_acquire) != 0) {
                    queue.pop_front();
                }
                node->end_operation();
            } else {
                // 另一个完成路径正在处理队头，保持 FIFO，交给它在
                // 释放操作令牌后完成移除和唤醒。
                break;
            }
        }
    }

    void fail_slot_locked(FdSlot& slot, int system_error,
                          WakeList& wake) noexcept {
        invalidate_registration_locked(slot);
        drain_queue_locked(slot.readers, WaitStatus::kError, system_error,
                           wake);
        drain_queue_locked(slot.writers, WaitStatus::kError, system_error,
                           wake);
    }

    NodePtr take_ready_locked(Queue& queue) noexcept {
        while (!queue.empty()) {
            NodePtr node = queue.front();
            const bool started = node && node->begin_operation();
            if (started && claim(*node, WaitStatus::kReady, 0)) {
                queue.pop_front();
                erase_timer_locked(*node);
                return node;
            } else if (started) {
                if (node->outcome.load(std::memory_order_acquire) != 0) {
                    queue.pop_front();
                }
                node->end_operation();
            } else {
                // 队头正在由取消、超时或零超时补采样路径处理；不能
                // 越过它，否则会破坏同一 fd/方向上的 FIFO。
                return {};
            }
        }
        return {};
    }

    void process_event_locked(std::uint64_t registration_id,
                              std::uint32_t events,
                              WakeList& wake) noexcept {
            const int fd = token_fd(registration_id);
            const auto registration = m_slots.find(fd);
            if (registration == m_slots.end()) {
                return;
            }
            FdSlot& slot = *registration->second;
            if (!slot.registered || slot.registration_id != registration_id) {
                return;
            }
            const bool one_shot =
                m_backend && m_backend->supports_one_shot();
            if (one_shot) {
                // epoll 已消费一次性事件；后续只接受新的代际 token。
                if (!slot.armed) {
                    return;
                }
                slot.armed = false;
            }
            const auto interest = slot.interest;

            // events=0 用于高频等待复用注册，但 Linux 仍会为已关闭 fd
            // 报告 EPOLLERR/EPOLLHUP。此时不能把错误写入 pending 位，
            // 否则 poller 会反复收到同一个 HUP 并忙循环；直接失效该代际
            // 注册，后续若有新 waiter 会重新完成 token/generation 校验。
            if (interest == 0 &&
                (events & (0x04U | 0x08U)) != 0) {
                invalidate_registration_locked(slot);
                if (slot.readers.empty() && slot.writers.empty()) {
                    m_slots.erase(fd);
                }
                return;
            }

            const bool error_or_hup = (events & 0x0cU) != 0;
            const bool read_ready = error_or_hup || (events & kReadMask) != 0;
            const bool write_ready = error_or_hup || (events & kWriteMask) != 0;
            if (read_ready && (interest & kReadMask) != 0) {
                if (auto node = take_ready_locked(slot.readers)) {
                    // readiness 已交给具体 waiter；不要把同一个事件
                    // 留在 pending 位，避免下一次注册得到伪 ready。
                    slot.pending_read = false;
                    wake.push_back(std::move(node));
                } else {
                    slot.pending_read = true;
                }
            }
            if (write_ready && (interest & kWriteMask) != 0) {
                if (auto node = take_ready_locked(slot.writers)) {
                    slot.pending_write = false;
                    wake.push_back(std::move(node));
                } else {
                    slot.pending_write = true;
                }
            }

            const int arm_error = update_interest_locked(slot);
            if (arm_error != 0) {
                fail_slot_locked(slot, arm_error, wake);
            }
            if (slot.readers.empty() && slot.writers.empty() &&
                !slot.registered) {
                m_slots.erase(fd);
            }
    }

    void process_event(std::uint64_t registration_id,
                       std::uint32_t events) noexcept {
        WakeList wake;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            process_event_locked(registration_id, events, wake);
        }
        wake_nodes(wake);
    }

    // 批量处理一次 epoll_wait 返回的事件。迟到的 registration token
    // 仍由 locked helper 忽略；所有 Fiber 在释放 m_mutex 后统一唤醒。
    void process_events_batch(const BackendEvent* events,
                              std::size_t count) noexcept {
        if (events == nullptr || count == 0) {
            return;
        }
        WakeList wake;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (std::size_t index = 0; index < count; ++index) {
                if (events[index].token != kWakeRegistration) {
                    process_event_locked(events[index].token,
                                         events[index].events, wake);
                }
            }
        }
        wake_nodes(wake);
    }

    void expire_timers() noexcept {
        WakeList wake;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto now = Clock::now();
            while (!m_deadline_order.empty() &&
                   m_deadline_order.begin()->first <= now) {
                const auto order = m_deadline_order.begin();
                const std::uint64_t node_id = order->second;
                const auto timer = m_deadline_index.find(node_id);
                NodePtr node;
                if (timer != m_deadline_index.end()) {
                    node = timer->second.node.lock();
                    m_deadline_index.erase(timer);
                }
                m_deadline_order.erase(order);
                // 超时路径也必须先登记活动操作，才能与 WakeList::wake_nodes
                // 中的 end_operation 一一配对。否则使用计数型生命周期时会
                // 出现下溢；当前互斥量实现也需要这一配对来保持复用顺序。
                const bool operation_started =
                    node && node->begin_operation();
                if (!operation_started ||
                    !claim(*node, WaitStatus::kTimeout, ETIMEDOUT)) {
                    if (operation_started) {
                        node->end_operation();
                    }
                    continue;
                }
                const auto found = m_slots.find(node->fd);
                if (found != m_slots.end() &&
                    found->second->generation == node->generation) {
                    FdSlot& slot = *found->second;
                    erase_node_locked(slot, node);
                    const int arm_error = update_interest_locked(slot);
                    if (arm_error != 0) {
                        fail_slot_locked(slot, arm_error, wake);
                    }
                    if (slot.readers.empty() && slot.writers.empty() &&
                        !slot.registered) {
                        m_slots.erase(found);
                    }
                }
                wake.push_back(std::move(node));
            }
        }
        wake_nodes(wake);
    }

    int poll_timeout_ms() const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop_poller) {
            return 0;
        }
        if (m_deadline_order.empty()) {
            return -1;
        }
        const auto now = Clock::now();
        if (m_deadline_order.begin()->first <= now) {
            return 0;
        }
        const auto remaining = m_deadline_order.begin()->first - now;
        // 在从时钟周期转换前先限制范围。max() 截止时间可能比毫秒类型更宽，
        // 直接转换会回绕成负数，导致 epoll_wait 失败而不是进行轮询。
        constexpr auto kMaxPollSeconds =
            std::chrono::seconds(std::numeric_limits<int>::max());
        const auto bounded = std::min(
            remaining,
            std::chrono::duration_cast<Clock::duration>(kMaxPollSeconds));
        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(bounded);
        std::uint64_t rounded = static_cast<std::uint64_t>(milliseconds.count());
        if (milliseconds < bounded) {
            ++rounded;
        }
        return static_cast<int>(std::min<std::uint64_t>(
            rounded, static_cast<std::uint64_t>(
                         std::numeric_limits<int>::max())));
    }

    void drain_wake_fd() noexcept {
        if (m_backend) {
            m_backend->drain_wake();
        }
    }

    void poll_loop() noexcept {
        constexpr int kEventBatch = 256;
        BackendEvent events[kEventBatch]{};
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stop_poller) {
                    break;
                }
                // register_wait 在同一把锁下发布 epoll 节点；只有它在
                // 这里置位后并发发布才需要 eventfd 打断 epoll_wait。
                m_poller_waiting.store(true, std::memory_order_release);
            }
            int count;
            do {
                count = m_backend
                            ? m_backend->wait(events, kEventBatch,
                                              poll_timeout_ms())
                            : -ENODEV;
            } while (count < 0 && -count == EINTR);
            m_poller_waiting.store(false, std::memory_order_release);

            if (count < 0) {
                WakeList wake;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (!m_stop_poller) {
                        m_init_error = count < 0 ? -count : EIO;
                        m_accepting = false;
                        m_stop_poller = true;
                        for (auto& entry : m_slots) {
                            FdSlot& slot = *entry.second;
                            invalidate_registration_locked(slot);
                            drain_queue_locked(slot.readers,
                                               WaitStatus::kError,
                                               m_init_error, wake);
                            drain_queue_locked(slot.writers,
                                               WaitStatus::kError,
                                               m_init_error, wake);
                        }
                    }
                }
                wake_nodes(wake);
                break;
            }

            bool saw_wake = false;
            for (int index = 0; index < count; ++index) {
                saw_wake = saw_wake ||
                           events[index].token == kWakeRegistration;
            }
            if (saw_wake) {
                drain_wake_fd();
                m_tickle_pending.store(false, std::memory_order_release);
            }
            process_events_batch(events, static_cast<std::size_t>(count));
            expire_timers();

            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stop_poller) {
                break;
            }
        }
        m_running.store(false, std::memory_order_release);
    }

    void tickle() noexcept {
        if (!m_backend) {
            return;
        }
        if (!m_poller_waiting.load(std::memory_order_acquire)) {
            return;
        }
        // poller 休眠期间可能有多个注册或完成事件到达。一个 eventfd 计数就
        // 足以触发一次 epoll 返回，其余事件合并到 poller 排空计数时处理。
        // 这样每个 Fiber 等待都不必执行一次 write 系统调用，同时不改变就绪协议。
        if (m_tickle_pending.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        m_backend->wake();
    }

    void wake_nodes(WakeList& nodes) noexcept {
        std::vector<NodePtr> pending;
        try {
            pending.reserve(nodes.size());
            while (const auto node = nodes.pop_front()) {
                pending.push_back(node);
            }
        } catch (...) {
            // 仅性能辅助容器分配失败时回退原始单节点唤醒，不能丢失
            // 已经从 WakeList 取出的节点。
            for (const auto& node : pending) {
                if (node) {
                    node->wake();
                    node->end_operation();
                }
            }
            while (const auto node = nodes.pop_front()) {
                if (node) {
                    node->wake();
                    node->end_operation();
                }
            }
            return;
        }

        std::vector<std::shared_ptr<Task>> tasks;
        try {
            tasks.reserve(pending.size());
            for (const auto& node : pending) {
                if (node) {
                    if (auto task = node->wake_task_claimed()) {
                        tasks.push_back(std::move(task));
                    }
                }
            }
        } catch (...) {
            for (const auto& node : pending) {
                if (node) {
                    node->wake();
                    node->end_operation();
                }
            }
            return;
        }

        if (tasks.size() > 1) {
            (void)m_scheduler->wake_io_batch(tasks);
        } else {
            for (const auto& node : pending) {
                if (node) {
                    node->wake();
                }
            }
        }
        for (const auto& node : pending) {
            if (node) {
                node->end_operation();
            }
        }
    }

    bool finish_fd(int fd, IOEvent event, bool both, WaitStatus status,
                   int system_error) noexcept {
        WakeList wake;
        bool had_waiter = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_slots.find(fd);
            if (found == m_slots.end()) {
                return false;
            }
            FdSlot& slot = *found->second;
            const std::size_t old_size = wake.size();
            drain_queue_locked(queue_for(slot, event), status, system_error,
                               wake);
            if (both) {
                drain_queue_locked(slot.writers, status, system_error, wake);
                if (event != IOEvent::kRead) {
                    drain_queue_locked(slot.readers, status, system_error,
                                       wake);
                }
            }
            had_waiter = wake.size() != old_size;
            const int arm_error = update_interest_locked(slot);
            if (arm_error != 0) {
                fail_slot_locked(slot, arm_error, wake);
            }
            if (slot.readers.empty() && slot.writers.empty() &&
                !slot.registered) {
                m_slots.erase(found);
            }
        }
        wake_nodes(wake);
        tickle();
        return had_waiter;
    }

    Scheduler* m_scheduler{nullptr};
    IOBackendPtr m_backend;
    bool m_backend_owner_registered{false};
    std::atomic<bool> m_poller_waiting{false};
    std::atomic<bool> m_tickle_pending{false};
    int m_init_error{0};

    mutable std::mutex m_mutex;
    std::mutex m_lifecycle_mutex;
    bool m_accepting{false};
    bool m_stop_poller{false};
    bool m_permanently_stopped{false};
    std::atomic<bool> m_running{false};
    std::thread m_poller;

    std::unordered_map<int, std::unique_ptr<FdSlot>> m_slots;
    DeadlineOrder m_deadline_order;
    std::unordered_map<std::uint64_t, TimerRecord> m_deadline_index;
    std::uint64_t m_next_node_id{0};
    std::uint64_t m_next_generation{0};
    std::uint64_t m_next_registration_id{0};
};

IOManager::IOManager(SchedulerConfig config, IOBackendPtr backend)
    : m_scheduler(std::move(config)),
      m_state(std::make_shared<State>(&m_scheduler, std::move(backend))) {
    std::lock_guard<std::mutex> lock(manager_registry_mutex());
    manager_registry()[&m_scheduler] = this;
    const std::weak_ptr<State> weak_state(m_state);
    close_registry()[&m_scheduler] = [weak_state](int fd) {
        if (const auto state = weak_state.lock()) {
            (void)state->notify_close(fd);
        }
    };
}

IOManager::~IOManager() {
    shutdown();
    std::lock_guard<std::mutex> lock(manager_registry_mutex());
    const auto found = manager_registry().find(&m_scheduler);
    if (found != manager_registry().end() && found->second == this) {
        manager_registry().erase(found);
    }
    close_registry().erase(&m_scheduler);
}

bool IOManager::start() {
    if (!m_state || !m_state->start()) {
        return false;
    }
    try {
        m_scheduler.start();
    } catch (...) {
        // poller 不能超过失败的 worker 启动过程继续存活，否则重试或销毁管理器
        // 的调用者会看到仍接受注册的 epoll 状态，而 Scheduler 已经回滚。
        m_state->shutdown();
        return false;
    }
    if (!m_scheduler.is_running()) {
        m_state->shutdown();
        return false;
    }
    return true;
}

void IOManager::shutdown() {
    if (m_state) {
        m_state->shutdown();
    }
    m_scheduler.shutdown();
}

bool IOManager::is_running() const noexcept {
    return m_state && m_state->is_running() && m_scheduler.is_running();
}

std::shared_ptr<Task> IOManager::go(Task::Function function) {
    return m_scheduler.go(std::move(function));
}

WaitResult IOManager::wait(int fd, IOEvent event,
                           std::optional<TimePoint> deadline,
                           ContextPtr context,
                           DescriptorTokenPtr expected_descriptor) {
    if (Scheduler::current_scheduler() != &m_scheduler ||
        !Scheduler::current_task()) {
        return {WaitStatus::kError, EPERM};
    }
    if (event_mask(event) == 0) {
        return {WaitStatus::kError, EINVAL};
    }
    if (context && context->IsDone()) {
        const bool timed_out = Is(context->Err(), DeadlineExceededError());
        return {timed_out ? WaitStatus::kTimeout : WaitStatus::kCancelled,
                timed_out ? ETIMEDOUT : ECANCELED};
    }
    if (deadline.has_value() && *deadline <= Clock::now()) {
        return probe_immediate(fd, event, expected_descriptor);
    }

    int registration_error = 0;
    State::NodePtr node;
    try {
        node = m_state->register_wait(fd, event, deadline,
                                     &registration_error,
                                     std::move(expected_descriptor));
    } catch (...) {
        return {WaitStatus::kError, ENOMEM};
    }
    if (!node) {
        return {registration_error == ESHUTDOWN ? WaitStatus::kClosed
                                                : WaitStatus::kError,
                registration_error == 0 ? EIO : registration_error};
    }
    if (node->outcome.load(std::memory_order_acquire) != 0) {
        const auto result = decode_outcome(node->outcome.load(std::memory_order_acquire));
        m_state->recycle_node(std::move(node));
        return result;
    }

    DoneSignal::CallbackId callback_id = 0;
    if (context) {
        const std::weak_ptr<State> weak_state(m_state);
        const std::weak_ptr<State::WaitNode> weak_node(node);
        const std::weak_ptr<Context> weak_context(context);
        node->callbacks_registered.store(true, std::memory_order_release);
        try {
            callback_id = context->Done().AddCallback(
            [weak_state, weak_node, weak_context] {
                const auto state = weak_state.lock();
                const auto current_node = weak_node.lock();
                if (!state || !current_node) {
                    return;
                }
                if (!current_node->begin_callback()) {
                    return;
                }
                struct CallbackGuard final {
                    State::WaitNode* node;
                    ~CallbackGuard() { node->end_callback(); }
                } callback_guard{current_node.get()};
                const auto current_context = weak_context.lock();
                const bool timed_out =
                    current_context &&
                    Is(current_context->Err(), DeadlineExceededError());
                (void)state->complete(
                    current_node,
                    timed_out ? WaitStatus::kTimeout
                              : WaitStatus::kCancelled,
                    timed_out ? ETIMEDOUT : ECANCELED);
            });
        } catch (...) {
            (void)m_state->complete(node, WaitStatus::kError, ENOMEM);
            m_state->recycle_node(std::move(node));
            return {WaitStatus::kError, ENOMEM};
        }
    }

    if (deadline.has_value() && *deadline <= Clock::now()) {
        (void)m_state->complete(node, WaitStatus::kTimeout, ETIMEDOUT);
    }

    // 即使完成事件抢在 park 前到达，也要消耗 pending 交接令牌。无关 permit
    // 和外部唤醒都是伪唤醒，不表示取消。
    for (;;) {
        const bool parked = m_scheduler.park_io(node->task);
        if (node->outcome.load(std::memory_order_acquire) != 0) {
            break;
        }
        // park 失败通常表示消耗了 pending 唤醒但没有挂起。如果 Scheduler 已
        // 停止，或当前 G 身份意外变化，不要让仍 armed 且无法恢复的 IO 节点
        // 无限自旋。
        if (node->task->cancellation_requested() ||
            Scheduler::current_scheduler() != &m_scheduler ||
            Scheduler::current_task().get() != node->task.get() ||
            (!parked && !m_scheduler.is_running())) {
            (void)m_state->complete(node, WaitStatus::kCancelled,
                                    ECANCELED);
            break;
        }
    }

    if (context && callback_id != 0) {
        context->Done().RemoveCallback(callback_id);
    }
    const auto result = decode_outcome(node->outcome.load(std::memory_order_acquire));
    m_state->recycle_node(std::move(node));
    return result;
}

WaitResult IOManager::wait_for(int fd, IOEvent event, Duration timeout,
                               ContextPtr context) {
    const auto now = Clock::now();
    const auto remaining = TimePoint::max() - now;
    const auto deadline = timeout <= Duration::zero()
                              ? now
                              : (timeout >= remaining ? TimePoint::max()
                                                      : now + timeout);
    return wait(fd, event, deadline, std::move(context));
}

WaitManyResult IOManager::wait_many(
    const std::vector<WaitRequest>& requests,
    std::optional<TimePoint> deadline,
    ContextPtr context) {
    WaitManyResult result;
    const auto fail = [&result](WaitStatus status, int error) {
        result.status = status;
        result.system_error = error;
        result.ready_indices.clear();
    };

    if (requests.empty()) {
        fail(WaitStatus::kError, EINVAL);
        return result;
    }
    if (Scheduler::current_scheduler() != &m_scheduler ||
        !Scheduler::current_task()) {
        // IOManager 的多 fd 等待必须让出 managed Fiber。普通线程请使用
        // 原生 poll/select；这里不偷偷阻塞调用方线程。
        fail(WaitStatus::kError, EPERM);
        return result;
    }
    if (context && context->IsDone()) {
        const bool timed_out = Is(context->Err(), DeadlineExceededError());
        fail(timed_out ? WaitStatus::kTimeout : WaitStatus::kCancelled,
             timed_out ? ETIMEDOUT : ECANCELED);
        return result;
    }
    // 一个 WaitMany 集合中同一 fd/方向只能出现一次；否则一次 readiness
    // 可能同时消费多个请求，无法给外部 waiter 提供可验证的 FIFO。
    for (std::size_t i = 0; i < requests.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (requests[i].fd == requests[j].fd &&
                requests[i].event == requests[j].event) {
                fail(WaitStatus::kError, EINVAL);
                return result;
            }
        }
    }

    // 先验证完整请求集合，再发布任何 epoll 节点。否则前面的可读 FD
    // 可能先触发并让 WaitMany 返回，后面的无效 FD 就会被静默跳过。
    for (const auto& request : requests) {
        if (event_mask(request.event) == 0) {
            fail(WaitStatus::kError, EINVAL);
            return result;
        }
        if ((request.expected_descriptor &&
             (!request.expected_descriptor->valid() ||
               request.expected_descriptor->fd() != request.fd)) ||
            (!request.expected_descriptor && !descriptor_is_open(request.fd))) {
            fail(WaitStatus::kError, EBADF);
            return result;
        }
    }

    if (deadline.has_value() && *deadline <= Clock::now()) {
        std::vector<pollfd> probes;
        try {
            probes.reserve(requests.size());
            for (const auto& request : requests) {
                pollfd descriptor{};
                descriptor.fd = request.fd;
                descriptor.events = request.event == IOEvent::kRead
                                        ? static_cast<short>(POLLIN | POLLPRI)
                                        : POLLOUT;
                probes.push_back(descriptor);
            }
        } catch (...) {
            fail(WaitStatus::kError, ENOMEM);
            return result;
        }
        const long poll_result = ::syscall(
            SYS_poll, probes.data(), static_cast<nfds_t>(probes.size()), 0);
        if (poll_result < 0) {
            fail(WaitStatus::kError, errno == 0 ? EIO : errno);
            return result;
        }
        const short ready_mask = static_cast<short>(POLLIN | POLLOUT | POLLPRI |
                                                     POLLERR | POLLHUP
#ifdef POLLRDHUP
                                                     | POLLRDHUP
#endif
        );
        for (std::size_t index = 0; index < probes.size(); ++index) {
            const short revents = probes[index].revents;
            if ((revents & POLLNVAL) != 0) {
                fail(WaitStatus::kError, EBADF);
                return result;
            }
            if ((revents & ready_mask) != 0) {
                result.ready_indices.push_back(index);
            }
        }
        if (!result.ready_indices.empty()) {
            result.status = WaitStatus::kReady;
            result.system_error = 0;
        } else {
            fail(WaitStatus::kTimeout, ETIMEDOUT);
        }
        return result;
    }

    std::vector<State::NodePtr> nodes;
    try {
        nodes.reserve(requests.size());
    } catch (...) {
        fail(WaitStatus::kError, ENOMEM);
        return result;
    }

    const auto cleanup = [&nodes, this] {
        for (const auto& node : nodes) {
            if (!node) {
                continue;
            }
            if (node->outcome.load(std::memory_order_acquire) == 0) {
                (void)m_state->complete(node, WaitStatus::kCancelled,
                                        ECANCELED);
            }
            node->disarm();
        }
    };

    for (const auto& request : requests) {
        if (event_mask(request.event) == 0) {
            cleanup();
            fail(WaitStatus::kError, EINVAL);
            return result;
        }
        int registration_error = 0;
        State::NodePtr node;
        try {
            node = m_state->register_wait(
                request.fd, request.event, deadline, &registration_error,
                request.expected_descriptor);
        } catch (...) {
            cleanup();
            fail(WaitStatus::kError, ENOMEM);
            return result;
        }
        if (!node) {
            cleanup();
            fail(registration_error == ESHUTDOWN ? WaitStatus::kClosed
                                                 : WaitStatus::kError,
                 registration_error == 0 ? EIO : registration_error);
            return result;
        }
        nodes.push_back(std::move(node));
        const auto& registered = nodes.back();
        if (registered->outcome.load(std::memory_order_acquire) != 0) {
            // 注册期间可能同步消耗 pending readiness。它仍是集合中的成功成员，
            // 不能因此丢弃 ready_indices；只有错误或关闭结果才终止完整请求集合。
            const WaitResult terminal = decode_outcome(
                registered->outcome.load(std::memory_order_acquire));
            if (terminal.status != WaitStatus::kReady) {
                cleanup();
                fail(terminal.status, terminal.system_error);
                return result;
            }
        }
    }

    const auto task = nodes.front()->task;
    DoneSignal::CallbackId callback_id = 0;
    if (context) {
        const std::weak_ptr<State> weak_state(m_state);
        const std::weak_ptr<Context> weak_context(context);
        for (const auto& node : nodes) {
            if (node) {
                node->callbacks_registered.store(true,
                                                   std::memory_order_release);
            }
        }
        // 在该回调中持有节点，防止迟到的取消回调观察到已经销毁的节点。下面
        // 的 RemoveCallback 会先移除回调，再释放这个所有者。
        std::shared_ptr<std::vector<State::NodePtr>> callback_nodes;
        try {
            callback_nodes =
                std::make_shared<std::vector<State::NodePtr>>(nodes);
            callback_id = context->Done().AddCallback(
                [weak_state, weak_context, callback_nodes] {
                    const auto state = weak_state.lock();
                    if (!state) {
                        return;
                    }
                    const auto current_context = weak_context.lock();
                    const bool timed_out =
                        current_context &&
                        Is(current_context->Err(), DeadlineExceededError());
                    for (const auto& node : *callback_nodes) {
                        if (!node || !node->begin_callback()) {
                            continue;
                        }
                        struct CallbackGuard final {
                            State::WaitNode* node;
                            ~CallbackGuard() { node->end_callback(); }
                        } callback_guard{node.get()};
                        (void)state->complete(
                            node,
                            timed_out ? WaitStatus::kTimeout
                                      : WaitStatus::kCancelled,
                            timed_out ? ETIMEDOUT : ECANCELED);
                    }
                });
        } catch (...) {
            cleanup();
            fail(WaitStatus::kError, ENOMEM);
            return result;
        }
    }

    const auto has_terminal = [&nodes] {
        for (const auto& node : nodes) {
            if (node && node->outcome.load(std::memory_order_acquire) != 0) {
                return true;
            }
        }
        return false;
    };

    // 注册与 park 之间截止时间可能到期。挂起前先完成请求集合，避免零长度
    // 竞争让 Fiber 永久睡眠。
    if (deadline.has_value() && *deadline <= Clock::now()) {
        for (const auto& node : nodes) {
            (void)m_state->complete(node, WaitStatus::kTimeout, ETIMEDOUT);
        }
    }

    // 所有请求发布完成后先做一次零超时探测。数据可能在第一个节点
    // 注册后、第二个节点注册前就已经到达；如果直接 park，poller 可能
    // 只先唤醒一个 Fiber，导致另一个已就绪节点依赖后续 eventfd 时序。
    // 这里使用原生 syscall，避免进入 poll Hook 递归等待。
    try {
        std::vector<pollfd> probes;
        std::vector<std::size_t> probe_indices;
        probes.reserve(nodes.size());
        probe_indices.reserve(nodes.size());
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const auto& node = nodes[index];
            if (!node || node->outcome.load(std::memory_order_acquire) != 0) {
                continue;
            }
            pollfd descriptor{};
            descriptor.fd = node->fd;
            descriptor.events = node->event == IOEvent::kRead
                                    ? static_cast<short>(POLLIN | POLLPRI)
                                    : POLLOUT;
            probes.push_back(descriptor);
            probe_indices.push_back(index);
        }
        if (!probes.empty()) {
            const long poll_result = ::syscall(
                SYS_poll, probes.data(), static_cast<nfds_t>(probes.size()), 0);
            if (poll_result > 0) {
                for (std::size_t probe_index = 0;
                     probe_index < probes.size(); ++probe_index) {
                    const short revents = probes[probe_index].revents;
                    if (revents == 0) {
                        continue;
                    }
                    const auto& node = nodes[probe_indices[probe_index]];
                    if (!node) {
                        continue;
                    }
                    if ((revents & POLLNVAL) != 0) {
                        (void)m_state->complete_if_head(
                            node, WaitStatus::kClosed, EBADF);
                    } else if ((revents & (POLLIN | POLLOUT | POLLPRI |
                                           POLLERR | POLLHUP
#ifdef POLLRDHUP
                                           | POLLRDHUP
#endif
                                           )) != 0) {
                        (void)m_state->complete_if_head(
                            node, WaitStatus::kReady, 0);
                    }
                }
            }
        }
    } catch (...) {
        // 预探测只是减少已就绪请求的唤醒延迟，内存不足时保留
        // epoll 主路径，不改变等待结果。
    }

    while (!has_terminal()) {
        const bool parked = m_scheduler.park_io(task);
        if (has_terminal()) {
            break;
        }
        if (task->cancellation_requested() ||
            Scheduler::current_scheduler() != &m_scheduler ||
            Scheduler::current_task().get() != task.get() ||
            (!parked && !m_scheduler.is_running())) {
            for (const auto& node : nodes) {
                (void)m_state->complete(node, WaitStatus::kCancelled,
                                         ECANCELED);
            }
            break;
        }
    }

    if (context && callback_id != 0) {
        try {
            context->Done().RemoveCallback(callback_id);
        } catch (...) {
    // 当前实现的 RemoveCallback 不抛出异常；如果可替换的 Context 后端抛出，
    // 仍保留已经得到的唤醒结果。
        }
    }

    // epoll 的唤醒回调可能在同一个 Fiber 恢复前后分批到达。对仍未
    // claim 的节点做一次零超时 poll，把同一时刻已经就绪的 FD 一并收集，
    // 避免第一个回调先恢复 Fiber 后 cleanup 取消第二个已就绪节点。
    try {
        std::vector<pollfd> probes;
        std::vector<std::size_t> probe_indices;
        probes.reserve(nodes.size());
        probe_indices.reserve(nodes.size());
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const auto& node = nodes[index];
            if (!node || node->outcome.load(std::memory_order_acquire) != 0) {
                continue;
            }
            pollfd descriptor{};
            descriptor.fd = node->fd;
            descriptor.events = node->event == IOEvent::kRead
                                    ? static_cast<short>(POLLIN | POLLPRI)
                                    : POLLOUT;
            probes.push_back(descriptor);
            probe_indices.push_back(index);
        }
        if (!probes.empty()) {
            // 这里只做零超时的内部补探测，不能经过本项目导出的 poll Hook，
            // 否则 Hook-on 构建会再次进入阻塞封装并可能递归 park 当前 Fiber。
            const long poll_result = ::syscall(
                SYS_poll, probes.data(), static_cast<nfds_t>(probes.size()), 0);
            if (poll_result > 0) {
                for (std::size_t probe_index = 0;
                     probe_index < probes.size(); ++probe_index) {
                    const short revents = probes[probe_index].revents;
                    if (revents == 0) {
                        continue;
                    }
                    const auto node_index = probe_indices[probe_index];
                    const auto& node = nodes[node_index];
                    if (!node) {
                        continue;
                    }
                    if ((revents & POLLNVAL) != 0) {
                        (void)m_state->complete_if_head(
                            node, WaitStatus::kClosed, EBADF);
                    } else if ((revents & (POLLIN | POLLOUT | POLLPRI |
                                          POLLERR | POLLHUP
#ifdef POLLRDHUP
                                          | POLLRDHUP
#endif
                                          )) != 0) {
                        (void)m_state->complete_if_head(node,
                                                          WaitStatus::kReady,
                                                          0);
                    }
                }
            }
        }
    } catch (...) {
        // 结果收集是增强路径；epoll 的已 claim 结果仍然有效，分配失败
        // 时保留原有单节点语义。
    }

    // 多个 epoll/timer 回调竞争时，就绪优先于超时或取消。这与 poll/select
    // 的规则一致：已经观察到的就绪事件可以执行。
    WaitStatus terminal_status = WaitStatus::kCancelled;
    int terminal_error = ECANCELED;
    bool have_terminal = false;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        if (!node) {
            continue;
        }
        const WaitResult current = decode_outcome(
            node->outcome.load(std::memory_order_acquire));
        if (current.status == WaitStatus::kReady) {
            try {
                result.ready_indices.push_back(index);
            } catch (...) {
                result.ready_indices.clear();
                fail(WaitStatus::kError, ENOMEM);
                cleanup();
                return result;
            }
            continue;
        }
        // 只记录第一个终态；就绪（kReady）已在上面 continue 掉，
        // 因此这里遇到的都是终态，不需要再区分 kError 的优先级。
        if (!have_terminal) {
            terminal_status = current.status;
            terminal_error = current.system_error;
            have_terminal = true;
        }
    }
    if (!result.ready_indices.empty()) {
        result.status = WaitStatus::kReady;
        result.system_error = 0;
    } else if (have_terminal) {
        fail(terminal_status, terminal_error);
    } else {
        fail(WaitStatus::kCancelled, ECANCELED);
    }
    cleanup();
    return result;
}

WaitAnyResult IOManager::wait_any(const std::vector<WaitRequest>& requests,
                                   std::optional<TimePoint> deadline,
                                   ContextPtr context) {
    WaitAnyResult result;
    const WaitManyResult many =
        wait_many(requests, deadline, std::move(context));
    result.status = many.status;
    result.system_error = many.system_error;
    if (many.ready_indices.empty()) {
        return result;
    }
    result.index = many.ready_indices.front();
    if (result.index < requests.size()) {
        result.fd = requests[result.index].fd;
        result.event = requests[result.index].event;
    }
    return result;
}

WaitAnyResult IOManager::wait_any_for(
    const std::vector<WaitRequest>& requests, Duration timeout,
    ContextPtr context) {
    const auto now = Clock::now();
    const auto remaining = TimePoint::max() - now;
    const auto deadline = timeout <= Duration::zero()
                              ? now
                              : (timeout >= remaining ? TimePoint::max()
                                                      : now + timeout);
    return wait_any(requests, deadline, std::move(context));
}

WaitManyResult IOManager::wait_many_for(
    const std::vector<WaitRequest>& requests, Duration timeout,
    ContextPtr context) {
    const auto now = Clock::now();
    const auto remaining = TimePoint::max() - now;
    const auto deadline = timeout <= Duration::zero()
                              ? now
                              : (timeout >= remaining ? TimePoint::max()
                                                      : now + timeout);
    return wait_many(requests, deadline, std::move(context));
}

bool IOManager::cancel(int fd, IOEvent event) {
    return event_mask(event) != 0 && m_state && m_state->cancel(fd, event);
}

bool IOManager::cancel_all(int fd) {
    return m_state && m_state->cancel_all(fd);
}

bool IOManager::notify_close(int fd) {
    DescriptorGuard guard;
    DescriptorGuard::Invalidate(fd);
    return m_state && m_state->notify_close(fd);
}

void IOManager::NotifyCloseAll(int fd) noexcept {
    DescriptorGuard guard;
    DescriptorGuard::Invalidate(fd);
    std::vector<CloseCallback> callbacks;
    try {
        std::lock_guard<std::mutex> lock(manager_registry_mutex());
        callbacks.reserve(close_registry().size());
        for (const auto& entry : close_registry()) {
            callbacks.emplace_back(entry.second);
        }
    } catch (...) {
        // 即使内存分配有压力也要保持唤醒进度。这只是最后的兜底路径；正常
        // 交付不会在持有注册表互斥量时调用用户可见回调。
        std::lock_guard<std::mutex> lock(manager_registry_mutex());
        for (const auto& entry : close_registry()) {
            try {
                entry.second(fd);
            } catch (...) {
            }
        }
        return;
    }
    for (const auto& callback : callbacks) {
        try {
            callback(fd);
        } catch (...) {
        }
    }
}

IOManager* IOManager::current() noexcept {
    Scheduler* scheduler = Scheduler::current_scheduler();
    if (!scheduler) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(manager_registry_mutex());
    const auto found = manager_registry().find(scheduler);
    return found == manager_registry().end() ? nullptr : found->second;
}

}  // namespace go2cpp::io