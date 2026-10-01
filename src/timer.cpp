#include "go2cpp/core/timer.hpp"

#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace go2cpp::core {

struct Timer::State final {
    std::weak_ptr<TimerService::Impl> m_owner;
    std::mutex m_mutex;
    std::function<void()> m_callback;
    bool m_active{true};
};

class TimerService::Impl final {
public:
    using Entries = std::multimap<Clock::time_point,
                                 std::shared_ptr<Timer::State>>;

    Impl() : m_thread([this] { Run(); }) {}

    ~Impl() {
        Entries abandoned;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
            abandoned.swap(m_entries);
        }
        m_cv.notify_all();
        if (m_thread.joinable() &&
            m_thread.get_id() == std::this_thread::get_id()) {
            // worker 仍会执行成员函数，不能让它的生命周期超过回调中正在
            // 销毁所有者的 Impl 对象。
            std::terminate();
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        for (const auto& entry : abandoned) {
            Clear(entry.second);
        }
    }

    void Add(Clock::time_point deadline,
             const std::shared_ptr<Timer::State>& state) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                throw std::runtime_error("timer service is stopped");
            }
            m_entries.emplace(deadline, state);
        }
        m_cv.notify_all();
    }

    void Remove(const std::shared_ptr<Timer::State>& state) noexcept {
        // 保留一个强引用的局部状态，保证回调捕获对象不会在服务队列锁内
        // 被销毁，包括发生重入取消的情况。
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
                if (it->second == state) {
                    m_entries.erase(it);
                    break;
                }
            }
        }
        m_cv.notify_all();
    }

    static void Clear(const std::shared_ptr<Timer::State>& state) noexcept {
        std::function<void()> retired;
        {
            std::lock_guard<std::mutex> lock(state->m_mutex);
            state->m_active = false;
            retired.swap(state->m_callback);
        }
    }

private:
    void Run() noexcept {
        for (;;) {
            std::shared_ptr<Timer::State> expired;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (m_stopping) {
                    return;
                }
                if (m_entries.empty()) {
                    m_cv.wait(lock, [this] {
                        return m_stopping || !m_entries.empty();
                    });
                    continue;
                }
                const auto deadline = m_entries.begin()->first;
                if (Clock::now() < deadline) {
                    m_cv.wait_until(lock, deadline);
                    continue;
                }
                expired = std::move(m_entries.begin()->second);
                m_entries.erase(m_entries.begin());
            }
            std::function<void()> callback;
            {
                std::lock_guard<std::mutex> lock(expired->m_mutex);
                expired->m_active = false;
                callback.swap(expired->m_callback);
            }
            if (callback) {
                try {
                    callback();
                } catch (...) {
                    // 定时器观察者不能终止共享的 worker 线程。
                }
            }
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    Entries m_entries;
    bool m_stopping{false};
    std::thread m_thread;
};

Timer::Timer() noexcept = default;
Timer::Timer(std::shared_ptr<State> state) : m_state(std::move(state)) {}
Timer::~Timer() { Cancel(); }
Timer::Timer(Timer&&) noexcept = default;

Timer& Timer::operator=(Timer&& other) noexcept {
    if (this != &other) {
        Cancel();
        m_state = std::move(other.m_state);
    }
    return *this;
}

void Timer::Cancel() noexcept {
    auto state = std::move(m_state);
    if (!state) {
        return;
    }
    TimerService::Impl::Clear(state);
    if (auto owner = state->m_owner.lock()) {
        owner->Remove(state);
    }
}

bool Timer::Active() const noexcept {
    const auto state = m_state;
    if (!state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state->m_mutex);
    return state->m_active;
}

TimerService::TimerService() : m_impl(std::make_shared<Impl>()) {}
TimerService::~TimerService() = default;

TimerService& TimerService::Default() {
    static TimerService service;
    return service;
}

Timer TimerService::Schedule(Clock::time_point deadline,
                             std::function<void()> callback) {
    if (!callback) {
        return {};
    }
    const auto state = std::make_shared<Timer::State>();
    state->m_owner = m_impl;
    state->m_callback = std::move(callback);
    m_impl->Add(deadline, state);
    return Timer(state);
}

}  // namespace go2cpp::core
