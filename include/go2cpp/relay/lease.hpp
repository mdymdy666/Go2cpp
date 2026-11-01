#pragma once

/**
 * @file lease.hpp
 * @brief 租约（Lease）与心跳（Heartbeat）：单调时钟驱动的「存活窗口 + 周期探测」（JinShan REQ-005）。
 *
 * ## 模块定位
 *
 * 两者都只负责**时间**，不碰连接、不发报文：
 *
 * - `Lease`：一个「最后一次续约后 timeout 内有效」的窗口。到期时回调一次
 *   `on_expire`，由调用方决定怎么处置（关连接、清路由、上报）。典型用法是
 *   RDCH 的「90s 没收到心跳判半开」；
 * - `Heartbeat`：按 `interval` 周期调用调用方给的 `send()`。连续返回失败时
 *   回调一次 `on_dead`。典型用法是每 30s 发一次心跳帧。
 *
 * 为什么放在同一份头文件/实现里：它们共享同一条硬约束——**只用单调时钟**
 * （`std::chrono::steady_clock`），且终态回调只允许触发一次。JinShan 的
 * 「30s 心跳 / 90s 判半开」也是同一处配置；拆成两个编译单元只会把这条
 * 时钟约定各写一遍，更容易漂移。
 *
 * ## 单调时钟
 *
 * 所有等待都基于 `steady_clock`（Linux 上是 `CLOCK_MONOTONIC`），**不读墙钟**。
 * 因此 NTP 校时、手工改系统时间、跨时区不会让租约提前/推迟触发。
 * 对应的验收测试用 `LD_PRELOAD` 注入 `clock_gettime(CLOCK_REALTIME)` 跳变
 * （+1 小时）来证明这一点，见 `tests/relay/test_lease_clockjump.cpp`。
 *
 * ## 与业务超时分离
 *
 * `Lease` 不取消任何业务操作，也不生成 `Context`；它只回答「这个窗口还在
 * 不在」。业务侧的读写超时/重试策略由调用方自己的 `Context`/deadline 决定，
 * 两者不要互相代替：租约判的是**对端是否还活着**，业务超时判的是**这一次
 * 请求是否值得继续等**。
 *
 * ## 线程模型
 *
 * 每个 `Lease` / `Heartbeat` 各占**一个后台线程**（用条件变量等待，不轮询）。
 * 这对「每连接一两个定时器」的量级足够；不要拿它做十万级定时器——
 * 那种场景应换成中心定时器。回调都在后台线程上执行：
 *
 * - 回调里**不要长时间阻塞**，否则会推迟同对象后续的状态收尾；
 * - 回调里可以安全调用本对象的 `Stop()` / `Renew()`（实现里处理了自连接）；
 * - 回调里可以删除本对象（析构会检测「当前就在计时线程上」并放弃 join），
 *   但这是最后手段，正常写法应在回调外管理生命周期。
 *
 * ## 已知边界
 *
 * - 回调只保证**最多一次**：`Stop()` 与到期竞争时，可能已经有一次回调在执行，
 *   `Stop()` 不会中断正在执行的回调，也不会再追加一次；
 * - 两个对象都不做线程池复用，析构会等待后台线程退出（若回调正在执行，
 *   析构会等回调返回）；
 * - `Heartbeat::Start` 需要先 `Stop` 才能再次启动（重复 `Start` 是空操作）。
 */

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace go2cpp {
class Context;
} // namespace go2cpp

namespace go2cpp::relay {

/**
 * @brief 可续约的存活窗口：构造即开始计时，`Renew()` 重置，到期回调一次。
 *
 * ## 状态机
 * ```
 *   [Armed] --Renew()--> [Armed]        （只看最近一次 Renew）
 *      |                    |
 *      | timeout 到期        | Stop()
 *      v                    v
 *   [Expired] --on_expire--> （回调一次，此后 Renew 无效）   [Stopped]（不再触发）
 * ```
 *
 * ## 语义
 * - 构造时立即布防：从构造时刻起 `timeout` 内没有 `Renew()` 就触发；
 * - `Renew()` 把截止时间重置为「现在 + timeout」；到期或 `Stop()` 之后再
 *   `Renew()` 是空操作（不会复活一个已经结束的租约）；
 * - `Stop()` 幂等：取消后永不再触发；`Stop()` 之后对象仍可安全析构；
 * - `timeout <= 0` 合法：等价于「下一次检查立即到期」，用于测试或
 *   「必须马上超时」的降级路径；
 * - `on_expire` 可以为空，此时租约只静默到期。
 *
 * @warning 回调在计时线程上执行；`~Lease()` 会等待该线程退出，所以不要在
 *          `on_expire` 里无限阻塞。
 */
class Lease final {
public:
    using Clock = std::chrono::steady_clock;

    /**
     * @brief 创建并立即布防。
     * @param timeout 未续约的存活时长（`<= 0` 表示立即到期）。
     * @param on_expire 到期回调，最多执行一次；可为空。
     */
    Lease(std::chrono::milliseconds timeout, std::function<void()> on_expire);

    /** @brief 停止并等待计时线程退出（等价于 `Stop()` 后再析构）。 */
    ~Lease();

    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&) = delete;
    Lease &operator=(Lease &&) = delete;

    /** @brief 重置计时（现在 + timeout）。已到期或已停止时为空操作。 */
    void Renew();

    /** @brief 取消租约，幂等；取消后不会再触发 `on_expire`。 */
    void Stop();

private:
    /** @brief 计时线程主体：等到截止时间或被续约/停止唤醒。 */
    void Run();

    std::chrono::milliseconds m_timeout;
    std::function<void()> m_on_expire;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    Clock::time_point m_deadline;
    /// 每次 Renew 自增；计时线程用它判断「醒来是因为续约」而不是超时，
    /// 避免把一次续约误判为到期（条件变量没有「代际」概念，必须自己带）。
    std::uint64_t m_generation{0};
    bool m_stopped{false};
    bool m_expired{false};
    std::thread m_thread;
};

/**
 * @brief 周期心跳：按 `interval` 调用 `send()`，失败时回调一次 `on_dead`。
 *
 * ## 语义
 * - 启动后**先等一个 interval** 再发第一次（与 Go `time.Ticker` 一致），
 *   避免「刚建立连接就立刻多打一包」；
 * - `send()` 返回 `true` 视为存活；返回 `false`（或抛异常）视为死亡：
 *   触发一次 `on_dead` 并自行停止；
 * - 传入的 `Context` 取消时**静默停止**，不触发 `on_dead`——这是调用方
 *   主动关停，不是对端死亡；
 * - `Stop()` 幂等，返回后不再调用 `send()`/`on_dead`（正在执行的那一次
 *   `send()` 会执行完，但结果不再触发回调）；
 * - `interval <= 0` 会被夹到 1ms，防止忙等；实际发送间隔不会小于该值，
 *   系统调度抖动会让间隔略长。
 *
 * ## 与 Lease 的关系
 * 两者互补但不耦合：常见写法是 `send()` 成功时 `lease.Renew()`，
 * `lease` 到期才判半开；`Heartbeat` 自己只负责「发包 + 单次失败判定」。
 * 心跳包发出成功不代表对端活着——所以判定存活应以**收到对端数据后
 * Renew 租约**为准，不要直接把「send 成功」当成存活。
 *
 * ## 生命周期
 * - `ctx` 必须比 `Heartbeat` 活得久（`Start` 只保存裸指针）；
 * - 若 `ctx` 在 `Start` 之前就已经取消，则不启动、不触发 `on_dead`；
 * - 心跳死亡（`on_dead`）后如需重启，先 `Stop()` 再 `Start()`。
 */
class Heartbeat final {
public:
    /** @brief 创建心跳；`interval <= 0` 会被夹到 1ms。 */
    explicit Heartbeat(std::chrono::milliseconds interval);

    /** @brief 停止并等待后台线程退出。 */
    ~Heartbeat();

    Heartbeat(const Heartbeat &) = delete;
    Heartbeat &operator=(const Heartbeat &) = delete;
    Heartbeat(Heartbeat &&) = delete;
    Heartbeat &operator=(Heartbeat &&) = delete;

    /**
     * @brief 启动心跳线程。
     * @param ctx 取消源；可为空（表示只由 `Stop()` 结束）。必须比本对象活得久。
     * @param send 每 interval 调用一次；返回 `false` 表示对端已死。
     * @param on_dead 判定死亡时调用一次；可为空。
     * @note 已在运行时调用是空操作；`ctx` 已取消时也不会启动。
     */
    void Start(::go2cpp::Context *ctx, std::function<bool()> send,
               std::function<void()> on_dead);

    /** @brief 停止心跳，幂等。 */
    void Stop();

private:
    /// 后台线程与 Start/Stop 共享的状态；用 shared_ptr 让取消回调只持弱引用，
    /// 这样即使 Context 的回调晚于对象销毁执行也不会碰已释放内存。
    struct State;

    /// 心跳线程主体：按 interval 周期调用 send()，失败/停止/取消时退出。
    static void Run(const std::shared_ptr<State> &state, ::go2cpp::Context *ctx);

    /// 等后台线程退出；若调用发生在后台线程上则 detach（不能自 join）。
    static void JoinWorker(std::thread &worker);

    std::chrono::milliseconds m_interval;

    /// 保护下面四个成员；与 State::mutex 分开，避免 Stop 持锁时和发送路径互等。
    std::mutex m_mutex;
    std::shared_ptr<State> m_state;
    std::thread m_worker;
    ::go2cpp::Context *m_ctx{nullptr};
    std::uint64_t m_cancel_id{0};
};

} // namespace go2cpp::relay
