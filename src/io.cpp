#include "go2cpp/io.hpp"

#ifndef __linux__
#error "go2cpp::io currently requires Linux epoll"
#endif

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <deque>
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

void raw_close(int fd) noexcept {
    if (fd >= 0) {
        (void)::syscall(SYS_close, fd);
    }
}

bool descriptor_is_open(int fd) noexcept {
    if (fd < 0) {
        errno = EBADF;
        return false;
    }
#ifdef SYS_fcntl
    return ::syscall(SYS_fcntl, fd, F_GETFD) >= 0 || errno != EBADF;
#else
    return ::fcntl(fd, F_GETFD) >= 0 || errno != EBADF;
#endif
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
    struct WaitNode {
        std::uint64_t id{0};
        int fd{-1};
        std::uint64_t generation{0};
        IOEvent event{IOEvent::kRead};
        Scheduler* scheduler{nullptr};
        std::shared_ptr<Task> task;
        DescriptorTokenPtr descriptor;
        std::mutex wake_mutex;
        bool wake_active{true};
        std::atomic<std::uint64_t> outcome{0};
        std::shared_ptr<WaitNode> wake_next;

        void wake() noexcept {
            std::lock_guard<std::mutex> lock(wake_mutex);
            if (wake_active && scheduler && task) {
                (void)scheduler->wake(task);
            }
        }

        void disarm() noexcept {
            std::lock_guard<std::mutex> lock(wake_mutex);
            wake_active = false;
            scheduler = nullptr;
        }
    };

    using NodePtr = std::shared_ptr<WaitNode>;
    using Queue = std::deque<NodePtr>;

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
        Queue readers;
        Queue writers;
        bool registered{false};
        std::uint32_t interest{0};
        std::uint64_t registration_id{0};
    };

    struct Registration {
        int fd{-1};
        std::uint64_t generation{0};
        std::uint32_t interest{0};
    };

    using DeadlineOrder = std::multimap<TimePoint, std::uint64_t>;
    struct TimerRecord {
        DeadlineOrder::iterator order;
        std::weak_ptr<WaitNode> node;
    };

    explicit State(Scheduler* scheduler) : m_scheduler(scheduler) {
        m_epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (m_epoll_fd < 0) {
            m_init_error = errno;
            return;
        }
        m_wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (m_wake_fd < 0) {
            m_init_error = errno;
            raw_close(m_epoll_fd);
            m_epoll_fd = -1;
            return;
        }

        epoll_event event{};
        event.events = EPOLLIN;
        event.data.u64 = kWakeRegistration;
        if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_wake_fd, &event) != 0) {
            m_init_error = errno;
            raw_close(m_wake_fd);
            raw_close(m_epoll_fd);
            m_wake_fd = -1;
            m_epoll_fd = -1;
        }
    }

    ~State() {
        shutdown();
        raw_close(m_wake_fd);
        raw_close(m_epoll_fd);
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
            m_registrations.clear();
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
        DescriptorGuard descriptor_guard;
        if (error != nullptr) {
            *error = 0;
        }
        if ((expected_descriptor &&
             (!expected_descriptor->valid() || expected_descriptor->fd() != fd)) ||
            !descriptor_is_open(fd)) {
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

        auto node = std::make_shared<WaitNode>();
        node->fd = fd;
        node->event = event;
        node->scheduler = m_scheduler;
        node->task = Scheduler::current_task();
        node->descriptor = expected_descriptor
                               ? std::move(expected_descriptor)
                               : DescriptorGuard::Capture(fd);

        int arm_error = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_accepting) {
                if (error != nullptr) {
                    *error = ESHUTDOWN;
                }
                return {};
            }

            auto found = m_slots.find(fd);
            if (found == m_slots.end()) {
                auto slot = std::make_unique<FdSlot>();
                slot->fd = fd;
                slot->generation = next_generation_locked();
                found = m_slots.emplace(fd, std::move(slot)).first;
            }
            FdSlot& slot = *found->second;
            node->id = next_node_id_locked();
            node->generation = slot.generation;

            try {
                queue_for(slot, event).push_back(node);
                if (deadline.has_value()) {
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
                if (slot.readers.empty() && slot.writers.empty()) {
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

        // New timers may be earlier than epoll_wait's previous timeout.
        tickle();
        return node;
    }

    bool complete(const NodePtr& node, WaitStatus status,
                  int system_error) noexcept {
        if (!node || !claim(*node, status, system_error)) {
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
                if (slot.readers.empty() && slot.writers.empty()) {
                    m_slots.erase(found);
                }
            }
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
        do {
            if (++m_next_registration_id == kWakeRegistration) {
                ++m_next_registration_id;
            }
        } while (m_registrations.find(m_next_registration_id) !=
                 m_registrations.end());
        return m_next_registration_id;
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
        if (slot.registered) {
            (void)::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, slot.fd, nullptr);
        }
        if (slot.registration_id != 0) {
            m_registrations.erase(slot.registration_id);
        }
        slot.registered = false;
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

        if (interest == 0) {
            invalidate_registration_locked(slot);
            return 0;
        }

        epoll_event event{};
        event.events = EPOLLONESHOT | EPOLLERR | EPOLLHUP;
        if ((interest & kReadMask) != 0) {
            event.events |= EPOLLIN | EPOLLRDHUP;
        }
        if ((interest & kWriteMask) != 0) {
            event.events |= EPOLLOUT;
        }

        const std::uint64_t registration_id = next_registration_id_locked();
        event.data.u64 = registration_id;
        try {
            m_registrations.emplace(
                registration_id,
                Registration{slot.fd, slot.generation, interest});
        } catch (...) {
            return ENOMEM;
        }

        int operation = slot.registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        int result = ::epoll_ctl(m_epoll_fd, operation, slot.fd, &event);
        if (result != 0 && operation == EPOLL_CTL_ADD && errno == EEXIST) {
            operation = EPOLL_CTL_MOD;
            result = ::epoll_ctl(m_epoll_fd, operation, slot.fd, &event);
        }
        if (result != 0) {
            const int saved_error = errno == 0 ? EIO : errno;
            m_registrations.erase(registration_id);
            return saved_error;
        }

        if (slot.registration_id != 0) {
            m_registrations.erase(slot.registration_id);
        }
        slot.registered = true;
        slot.interest = interest;
        slot.registration_id = registration_id;
        return 0;
    }

    void drain_queue_locked(Queue& queue, WaitStatus status, int system_error,
                            WakeList& wake) noexcept {
        while (!queue.empty()) {
            NodePtr node = std::move(queue.front());
            queue.pop_front();
            if (node && claim(*node, status, system_error)) {
                erase_timer_locked(*node);
                wake.push_back(std::move(node));
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
            NodePtr node = std::move(queue.front());
            queue.pop_front();
            if (node && claim(*node, WaitStatus::kReady, 0)) {
                erase_timer_locked(*node);
                return node;
            }
        }
        return {};
    }

    void process_event(std::uint64_t registration_id,
                       std::uint32_t events) noexcept {
        WakeList wake;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto registration = m_registrations.find(registration_id);
            if (registration == m_registrations.end()) {
                return;
            }
            const Registration snapshot = registration->second;
            const auto found = m_slots.find(snapshot.fd);
            if (found == m_slots.end() ||
                found->second->generation != snapshot.generation ||
                found->second->registration_id != registration_id) {
                m_registrations.erase(registration);
                return;
            }

            FdSlot& slot = *found->second;
            m_registrations.erase(registration);
            slot.registration_id = 0;
            // EPOLLONESHOT leaves the open-file registration present but
            // disabled. update_interest_locked() therefore uses MOD.
            slot.registered = true;

            const bool error_or_hup =
                (events & (EPOLLERR | EPOLLHUP)) != 0;
            const bool read_ready =
                error_or_hup || (events & (EPOLLIN | EPOLLRDHUP)) != 0;
            const bool write_ready = error_or_hup || (events & EPOLLOUT) != 0;
            if (read_ready && (snapshot.interest & kReadMask) != 0) {
                if (auto node = take_ready_locked(slot.readers)) {
                    wake.push_back(std::move(node));
                }
            }
            if (write_ready && (snapshot.interest & kWriteMask) != 0) {
                if (auto node = take_ready_locked(slot.writers)) {
                    wake.push_back(std::move(node));
                }
            }

            const int arm_error = update_interest_locked(slot);
            if (arm_error != 0) {
                fail_slot_locked(slot, arm_error, wake);
            }
            if (slot.readers.empty() && slot.writers.empty()) {
                m_slots.erase(found);
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
                if (!node ||
                    !claim(*node, WaitStatus::kTimeout, ETIMEDOUT)) {
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
                    if (slot.readers.empty() && slot.writers.empty()) {
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
        // Bound before converting from the clock's tick period. A max()
        // deadline may be wider than milliseconds and a direct cast can wrap
        // negative, which would make epoll_wait fail instead of polling.
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
        std::uint64_t value = 0;
        while (::syscall(SYS_read, m_wake_fd, &value, sizeof(value)) ==
               static_cast<long>(sizeof(value))) {
        }
    }

    void poll_loop() noexcept {
        constexpr int kEventBatch = 64;
        epoll_event events[kEventBatch]{};
        for (;;) {
            int count;
            do {
                count = ::epoll_wait(m_epoll_fd, events, kEventBatch,
                                     poll_timeout_ms());
            } while (count < 0 && errno == EINTR);

            if (count < 0) {
                WakeList wake;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (!m_stop_poller) {
                        m_init_error = errno == 0 ? EIO : errno;
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

            for (int index = 0; index < count; ++index) {
                if (events[index].data.u64 == kWakeRegistration) {
                    drain_wake_fd();
                } else {
                    process_event(events[index].data.u64,
                                  events[index].events);
                }
            }
            expire_timers();

            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stop_poller) {
                break;
            }
        }
        m_running.store(false, std::memory_order_release);
    }

    void tickle() noexcept {
        if (m_wake_fd < 0) {
            return;
        }
        const std::uint64_t value = 1;
        const long result =
            ::syscall(SYS_write, m_wake_fd, &value, sizeof(value));
        (void)result;
    }

    void wake_nodes(WakeList& nodes) noexcept {
        while (const auto node = nodes.pop_front()) {
            if (node) {
                // false may mean the pending-wake token was accepted while
                // the G was still running. It is not a failure in that race.
                node->wake();
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
            if (slot.readers.empty() && slot.writers.empty()) {
                m_slots.erase(found);
            }
        }
        wake_nodes(wake);
        tickle();
        return had_waiter;
    }

    Scheduler* m_scheduler{nullptr};
    int m_epoll_fd{-1};
    int m_wake_fd{-1};
    int m_init_error{0};

    mutable std::mutex m_mutex;
    std::mutex m_lifecycle_mutex;
    bool m_accepting{false};
    bool m_stop_poller{false};
    bool m_permanently_stopped{false};
    std::atomic<bool> m_running{false};
    std::thread m_poller;

    std::unordered_map<int, std::unique_ptr<FdSlot>> m_slots;
    std::unordered_map<std::uint64_t, Registration> m_registrations;
    DeadlineOrder m_deadline_order;
    std::unordered_map<std::uint64_t, TimerRecord> m_deadline_index;
    std::uint64_t m_next_node_id{0};
    std::uint64_t m_next_generation{0};
    std::uint64_t m_next_registration_id{0};
};

IOManager::IOManager(SchedulerConfig config)
    : m_scheduler(std::move(config)),
      m_state(std::make_shared<State>(&m_scheduler)) {
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
        // The poller must not outlive a failed worker start.  Otherwise a
        // caller retrying or destroying the manager observes an accepting
        // epoll state whose Scheduler has already rolled itself back.
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
        return {WaitStatus::kTimeout, ETIMEDOUT};
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
        node->disarm();
        return decode_outcome(node->outcome.load(std::memory_order_acquire));
    }

    DoneSignal::CallbackId callback_id = 0;
    if (context) {
        const std::weak_ptr<State> weak_state(m_state);
        const std::weak_ptr<State::WaitNode> weak_node(node);
        const std::weak_ptr<Context> weak_context(context);
        try {
            callback_id = context->Done().AddCallback(
            [weak_state, weak_node, weak_context] {
                const auto state = weak_state.lock();
                const auto current_node = weak_node.lock();
                if (!state || !current_node) {
                    return;
                }
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
            node->disarm();
            return {WaitStatus::kError, ENOMEM};
        }
    }

    if (deadline.has_value() && *deadline <= Clock::now()) {
        (void)m_state->complete(node, WaitStatus::kTimeout, ETIMEDOUT);
    }

    // Consume the pending handoff even for a completion that raced ahead of
    // park. Unrelated permits and external wakeups are spurious, not cancel.
    for (;;) {
        const bool parked = m_scheduler.park_io(node->task);
        if (node->outcome.load(std::memory_order_acquire) != 0) {
            break;
        }
        // A failed park normally means a pending wake was consumed without
        // suspending. If the scheduler is no longer running, or the current
        // G identity changed unexpectedly, do not spin forever with an armed
        // IO node that can no longer be resumed.
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
    node->disarm();
    return decode_outcome(node->outcome.load(std::memory_order_acquire));
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
    if (deadline.has_value() && *deadline <= Clock::now()) {
        fail(WaitStatus::kTimeout, ETIMEDOUT);
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
            // An invalid fd or epoll arm failure is terminal for the set. The
            // already registered nodes are cancelled below.
            const WaitResult terminal = decode_outcome(
                registered->outcome.load(std::memory_order_acquire));
            cleanup();
            fail(terminal.status, terminal.system_error);
            return result;
        }
    }

    const auto task = nodes.front()->task;
    DoneSignal::CallbackId callback_id = 0;
    if (context) {
        const std::weak_ptr<State> weak_state(m_state);
        const std::weak_ptr<Context> weak_context(context);
        // Holding the nodes in this callback prevents a late cancellation
        // callback from observing a destroyed node. RemoveCallback below
        // removes the callback before this owner is released.
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

    // A deadline can become due between registration and park. Complete the
    // set before parking so a zero-length race cannot leave the Fiber asleep.
    if (deadline.has_value() && *deadline <= Clock::now()) {
        for (const auto& node : nodes) {
            (void)m_state->complete(node, WaitStatus::kTimeout, ETIMEDOUT);
        }
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
            // RemoveCallback is non-throwing in the current implementation;
            // preserve the wake result if a replaceable Context backend throws.
        }
    }

    // Ready wins over timeout/cancel when several epoll/timer callbacks race.
    // This mirrors poll/select's rule that observed readiness is actionable.
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
        if (!have_terminal && current.status != WaitStatus::kError) {
            terminal_status = current.status;
            terminal_error = current.system_error;
            have_terminal = true;
        } else if (!have_terminal) {
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
        // Preserve wakeup progress even under allocation pressure. This is a
        // last-resort path only; normal delivery never invokes user-visible
        // callbacks while holding the registry mutex.
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
