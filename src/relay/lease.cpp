/**
 * @file lease.cpp
 * @brief Lease / Heartbeat 的实现：每对象一个后台线程，全部基于 steady_clock。
 *
 * ## 为什么用「线程 + 条件变量」而不是中心定时器
 *
 * 这两个组件的典型用量是「每条连接一个 90s 租约 + 一个 30s 心跳」，数量在
 * 10^2~10^3 量级，创建线程的开销可以忽略；换来的是**没有共享定时轮**带来的
 * 额外锁竞争与「定时器线程被一个慢回调拖住全部租约」的风险。每个租约只
 * 关心自己的截止时间，等待走 `wait_until(predicate)`，续约/停止用代际号
 * 唤醒，因此不会出现「醒来后误判超时」的经典丢唤醒问题。
 *
 * 数量上到 10^4 以上时应改成中心定时器（时间轮/最小堆 + 单线程批量触发），
 * 本实现的接口不变，只换内部调度即可。
 *
 * ## 为什么停止要「先摘状态、再唤醒、最后 join」
 *
 * 回调是用户代码，可能在回调里调用 `Stop()` 甚至删除对象：
 * - 若持 `m_mutex` 去 join，回调里的 `Stop()` 会与 join 互相等待；
 * - 若在回调线程上 join 自己，会抛 `std::system_error` 或死锁。
 * 因此 `Stop()` 先在锁内把线程对象搬出来、置停止标志，再在锁外唤醒并 join；
 * 检测到「当前线程就是计时/心跳线程」时改为 detach，让回调返回后线程自然
 * 结束（此后该线程不再访问对象成员）。
 */
#include "go2cpp/relay/lease.hpp"

#include "go2cpp/concurrency/context.hpp"

#include <algorithm>
#include <atomic>
#include <utility>

namespace go2cpp::relay {

namespace {

/// 心跳最小间隔：`interval <= 0` 会被夹到这里，防止后台线程忙等把 CPU 打满。
constexpr std::chrono::milliseconds kMinHeartbeatInterval{1};

/** @brief 把非法（<=0）的心跳间隔夹到 1ms。 */
std::chrono::milliseconds NormalizeInterval(std::chrono::milliseconds interval) {
    return interval < kMinHeartbeatInterval ? kMinHeartbeatInterval : interval;
}

} // namespace

// ---------------------------------------------------------------------------
// Lease
// ---------------------------------------------------------------------------

Lease::Lease(std::chrono::milliseconds timeout, std::function<void()> on_expire)
    : m_timeout(std::max(timeout, std::chrono::milliseconds::zero())),
      m_on_expire(std::move(on_expire)), m_deadline(Clock::now() + m_timeout) {
    // 构造即布防：验收要求「90s 无 Renew 触发一次」，从构造时刻开始计时，
    // 而不是从第一次 Renew 开始（否则永远不续约的租约永远不到期）。
    m_thread = std::thread([this] { Run(); });
}

Lease::~Lease() { Stop(); }

void Lease::Renew() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 已到期/已停止的租约不复活：否则「到期后立刻 Renew」会把一次
        // 已经对外宣布的死亡又拉回存活，语义上自相矛盾。
        if (m_stopped || m_expired) {
            return;
        }
        m_deadline = Clock::now() + m_timeout;
        ++m_generation;
    }
    // 锁外通知：避免把计时线程唤醒到「等锁」状态；它拿到锁后会按新的
    // generation/deadline 重新计算，因此这里不会丢唤醒。
    m_cv.notify_all();
}

void Lease::Stop() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopped = true;
        if (m_thread.joinable()) {
            if (m_thread.get_id() == std::this_thread::get_id()) {
                // 在 on_expire 里调用 Stop()：自己不能 join 自己。
                // 回调返回后计时线程随即结束，不再访问成员，detach 是安全的。
                m_thread.detach();
            } else {
                worker = std::move(m_thread);
            }
        }
    }
    m_cv.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
}

void Lease::Run() {
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        if (m_stopped || m_expired) {
            return;
        }
        const std::uint64_t generation = m_generation;
        const Clock::time_point deadline = m_deadline;
        // 谓词同时覆盖「续约」与「停止」两种唤醒：续约后 generation 变了，
        // 回到循环顶部用新 deadline 重算；停止则直接返回。
        const bool woken = m_cv.wait_until(lock, deadline, [this, generation] {
            return m_stopped || m_generation != generation;
        });
        if (woken) {
            continue;
        }
        m_expired = true;
        // 把回调搬出成员后再解锁执行：回调里可能删除本对象，执行期间不再
        // 触碰 this（除了已经搬出的局部副本）。
        std::function<void()> callback = std::move(m_on_expire);
        lock.unlock();
        if (callback) {
            callback();
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

struct Heartbeat::State {
    /// 保护 stop 与等待队列；send/on_dead/interval 在启动前写入，之后只读。
    std::mutex mutex;
    std::condition_variable cv;
    /// 停止标志：Stop() 与 Context 取消都会置真。
    bool stop{false};
    /// on_dead 是否已经触发；用 CAS 保证「只触发一次」在多条失败路径上成立。
    std::atomic<bool> dead_fired{false};
    std::chrono::milliseconds interval{kMinHeartbeatInterval};
    std::function<bool()> send;
    std::function<void()> on_dead;
};

Heartbeat::Heartbeat(std::chrono::milliseconds interval)
    : m_interval(NormalizeInterval(interval)) {}

Heartbeat::~Heartbeat() { Stop(); }

void Heartbeat::Start(::go2cpp::Context *ctx, std::function<bool()> send,
                      std::function<void()> on_dead) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state) {
        // 已在运行：不重启、不叠加线程（重启必须先 Stop，语义更可预期）。
        return;
    }
    if (ctx != nullptr && ctx->Done().IsDone()) {
        // 启动前就取消了：直接不启动，也不触发 on_dead（这不是对端死亡）。
        return;
    }

    auto state = std::make_shared<State>();
    state->interval = m_interval;
    state->send = std::move(send);
    state->on_dead = std::move(on_dead);

    if (ctx != nullptr) {
        // 只捕获 weak_ptr：Context 的回调可能在 Heartbeat 销毁之后才执行，
        // 强引用会让回调把 State 拖到 Context 结束，弱引用则安全失效。
        const std::weak_ptr<State> weak = state;
        const std::uint64_t cancel_id = ctx->Done().AddCallback([weak] {
            const auto locked = weak.lock();
            if (!locked) {
                return;
            }
            {
                std::lock_guard<std::mutex> guard(locked->mutex);
                locked->stop = true;
            }
            locked->cv.notify_all();
        });
        // 注册与取消可能竞态：AddCallback 在信号已完成时会立刻同步执行回调
        // 并返回 0。这里复核一次，避免留下一个「立即退出」的空线程。
        if (ctx->Done().IsDone()) {
            if (cancel_id != 0) {
                ctx->Done().RemoveCallback(cancel_id);
            }
            return;
        }
        m_ctx = ctx;
        m_cancel_id = cancel_id;
    }

    m_state = state;
    m_worker = std::thread([state, ctx] { Run(state, ctx); });
}

void Heartbeat::Stop() {
    std::shared_ptr<State> state;
    std::thread worker;
    ::go2cpp::Context *ctx = nullptr;
    std::uint64_t cancel_id = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state = std::move(m_state);
        m_state.reset();
        ctx = m_ctx;
        m_ctx = nullptr;
        cancel_id = m_cancel_id;
        m_cancel_id = 0;
        worker = std::move(m_worker);
    }
    if (state) {
        // 先注销 Context 回调，避免对象销毁后还留着一个观测点；注销只保证
        // 「尚未触发的不会再触发」，正在执行的会自己拿 weak_ptr 判活。
        if (ctx != nullptr && cancel_id != 0) {
            ctx->Done().RemoveCallback(cancel_id);
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->stop = true;
        }
        state->cv.notify_all();
    }
    JoinWorker(worker);
}

void Heartbeat::JoinWorker(std::thread &worker) {
    if (!worker.joinable()) {
        return;
    }
    if (worker.get_id() == std::this_thread::get_id()) {
        // 在 on_dead 里调用 Stop()：当前就在心跳线程上，不能自 join。
        worker.detach();
        return;
    }
    worker.join();
}

void Heartbeat::Run(const std::shared_ptr<State> &state, ::go2cpp::Context *ctx) {
    const std::chrono::milliseconds interval = state->interval;
    auto next = std::chrono::steady_clock::now() + interval;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            // 先等一个 interval 再发第一次（Go ticker 语义）：刚建立连接时
            // 双方通常已经在握手报文里交换过存活信息，不需要立刻再打一包。
            if (state->cv.wait_until(lock, next, [&state] { return state->stop; })) {
                return;
            }
        }
        // send() 是用户代码：抛异常也按「死亡」处理，不能让后台线程把进程
        // 带走（std::thread 里未捕获异常会直接 std::terminate）。
        bool alive = false;
        try {
            alive = state->send && state->send();
        } catch (...) {
            alive = false;
        }
        if (!alive) {
            const bool canceled = ctx != nullptr && ctx->Done().IsDone();
            bool expected = false;
            if (!canceled && state->dead_fired.compare_exchange_strong(expected, true)) {
                if (state->on_dead) {
                    state->on_dead();
                }
            }
            return;
        }
        next += interval;
        const auto now = std::chrono::steady_clock::now();
        if (next <= now) {
            // 发送耗时超过一个周期（例如 send 内部阻塞）：重新对齐到「现在 +
            // interval」，避免连续补发一串心跳把对端打爆。
            next = now + interval;
        }
    }
}

} // namespace go2cpp::relay
