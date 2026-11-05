/**
 * @file parking_condition.hpp
 * @brief Fiber 与普通线程共用的条件等待适配器。
 *
 * ## 模块定位
 *
 * `ParkingCondition` 是运行时里**唯一的"等待原语"**：Scheduler 的 idle 等待、
 * `sync::Mutex` 的阻塞、Channel 的收发、Future 的等待，最终都落到它上面。
 *
 * 它解决的问题是：同一份代码既可能在**受管 Fiber** 里跑（此时等待必须
 * park 协程、让出 M 线程），也可能在**普通线程**里跑（此时退化为原生条件
 * 等待）。调用方不需要知道自己所在的上下文。
 *
 * ## 两条路径
 *
 * - **Fiber 路径**：Scheduler park。WaitNode 绑定 Task，唤醒方通过
 *   wake_or_cancel 把 G 重新放回调度队列，不经过内核 futex；
 * - **线程路径**：每个 WaitNode 自己的 32 位 futex 代际字。notify_one 只推进
 *   **选中节点**的字并只唤醒它（审计 D2：共享代际字会唤醒错节点，被选中的
 *   节点留在内核睡眠队列里丢唤醒）；notify_all 走一次共享广播字 + 一次
 *   FUTEX_WAKE(INT_MAX)（futex_waitv 让等待者同时监听两个字），把 N 次唤醒
 *   系统调用降为 1 次。不支持 futex_waitv 的环境自动退回逐节点唤醒。
 *
 * ## 为什么必须在 park 前自旋一小段
 *
 * 如果直接 FUTEX_WAIT，在"刚检查完条件、还没进入内核"这个窗口里到达的
 * 唤醒会丢失，等待者被迫睡到超时。加一小段自旋（kSpinBudget）让这个窗口
 * 在热路径上极大概率被跨过去 —— 实测 2 个等待者的场景下，无自旋比
 * std::condition_variable 慢 3.3 倍，加了自旋后所有场景都更快。预算大小与
 * 是否真正划算随机器（尤其 WSL2 的唤醒延迟）变化，见同文件 .cpp 实测注释。
 *
 * ## 等待队列（P0 结构）
 *
 * 队列是**侵入式双向链表 + 每个等待者线程本地的节点池**：
 *
 * - 入队/摘链都是 O(1)，取消/超时清理不再需要 `std::find` + `erase` 中间
 *   元素（旧 `std::deque<std::shared_ptr<WaitNode>>` 在 N 个等待者逐个被
 *   唤醒时是 O(N²)）；
 * - 节点复用，稳态零堆分配；
 * - 队列锁只保护链接关系与计数，节点结果字段是独立原子量，等待者自旋
 *   不需要拿队列锁；managed 唤醒的 Scheduler 访问由每个节点自己的门保护。
 *
 * ## 能力清单
 *
 * - `wait / wait_for / wait_until`：带谓词等待，**调用方必须持有传入的外部
 *   互斥锁**；返回时锁仍被持有（与 `std::condition_variable` 契约一致）；
 * - `notify_one / notify_all`；
 * - `FiberWaitUnsupported()`：当前平台是否支持 Fiber 等待（移植性探测）。
 *
 * ## 明确不负责的事
 *
 * 它**不拥有业务状态**，也**不负责通知条件的产生** —— 谓词和状态由调用方
 * 管理。这样它才能被 Scheduler、Channel、Future 同时复用而互不耦合。
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "go2cpp/concurrency/core/futex_mutex.hpp"

namespace go2cpp::core {

// 带谓词等待保持 condition_variable 的外部锁契约，同时让受管 G 挂起
// Fiber，而不是阻塞承载它的 M 线程。
/**
 * 同时服务 Fiber 与普通线程的条件等待适配器。
 *
 * 依赖：Scheduler/Fiber 提供受管等待路径，per-node futex 提供普通线程路径。
 * 对上层提供带谓词的 wait/wait_for/wait_until，并要求调用方持有传入的外部
 * 互斥锁；它本身不拥有业务状态，也不负责通知条件的产生。
 */
/**
 * @brief 等待队列中单个节点的只读快照。
 * @note `state`：0=kWaiting 1=kNotified 2=kTimedOut 3=kCancelled。
 */
struct ParkingWaiterDebugSnapshot {
    std::size_t index{0};  ///< 在等待队列中的位置（0=FIFO 队首）
    bool native{false};    ///< true=普通线程等待；false=受管 Fiber
    int state{0};          ///< 节点结果状态（见上）
    // Doctor 等待图需要「哪个 G 在等」。受管节点在构造时绑定 Task，记录其
    // gid 是**已存在事实**的只读暴露；native 节点没有 Task，恒为 0。
    // 不要把它当所有权凭据：G 可能在本快照返回前就被回收。
    std::uint64_t gid{0};  ///< 受管等待者的 G 编号；native/未知为 0。
};

/**
 * @brief ParkingCondition 的只读调试快照。
 *
 * `waiters` 是队列长度；`waiting_nodes/notified_nodes` 按节点结果统计
 * （`kTimedOut/kCancelled` 的节点通常已被等待者自己摘除，队列里少见）。
 */
struct ParkingConditionDebugSnapshot {
    std::size_t waiters{0};
    std::size_t native_waiters{0};
    std::size_t fiber_waiters{0};
    std::size_t waiting_nodes{0};
    std::size_t notified_nodes{0};
    std::uint32_t broadcast_seq{0};      ///< notify_all 的广播代际
    std::uint32_t broadcast_waiters{0};  ///< futex_waitv 路径上的原生等待者数
    std::vector<ParkingWaiterDebugSnapshot> nodes;
};

class ParkingCondition final {
public:
    using Clock = std::chrono::steady_clock;

    /**
     * @brief 单次 `WaitOnce`（即一次 `wait/wait_for/wait_until` 的内部迭代）
     *        的结局。
     *
     * ## 为什么不是 bool
     *
     * 上层 `wait*` 只把「谓词最终是否为真」交回调用方，但“为什么结束”决定
     * 日志、超时统计与取消传播。旧版只有三个值，`kCancelled` 同时承担了
     * 「Task 取消」和「当前上下文无法 park」两种完全不同的处置方式；调用方
     * 无法从状态本身区分「该向上传播取消」还是「该换非阻塞接口」。
     *
     * ## 值稳定性
     *
     * 前三个值（0/1/2）是历史值，**不得改动**；新增值只追加在末尾。外部
     * `switch` 必须覆盖全部值（或带 `default`），否则新增值会触发
     * `-Wswitch` —— 这正是我们想要的编译期提醒。
     */
    enum class WaitStatus {
        /**
         * 正常唤醒：等待期间谓词变为真，或通知先于入队到达、入队前重检
         * 谓词即为真。调用方应按正常路径继续，**不要**重试本次等待。
         */
        kNotified = 0,
        /**
         * 超时：等待超过 deadline（或进入等待前 deadline 已过期），且谓词
         * 仍未满足。调用方应检查业务条件后决定重试还是放弃，并把超时作为
         * 结果返回上层；不要把它当成取消。
         */
        kTimedOut = 1,
        /**
         * 取消：所属 Task 的取消请求在等待前或等待期间生效（无限等待时这是
         * 唯一会打断阻塞的负向结局）。调用方应立即放弃本次操作并向上传播
         * 取消，**不要重试**。
         */
        kCancelled = 2,
        /**
         * 虚假唤醒：被与本次等待无关的唤醒/信号打断，且谓词仍未满足。
         * 当前实现内部重新 park（`WaitManaged` 的 spurious 重试循环），不会
         * 把它交给调用方；保留该值供未来需要显式暴露虚假唤醒的路径使用。
         * 若调用方收到，应重新检查谓词并重试等待。
         */
        kSpurious = 3,
        /**
         * 未武装：等待没有真正进入阻塞就返回 —— 典型是手动 Fiber（有 Fiber
         * 上下文但没有 Scheduler/Task 归属），既无法 park、也不允许阻塞
         * carrier 线程。对应旧实现里被并入 `kCancelled` 的「无调度器」场景。
         * 调用方应改用非阻塞接口，或先绑定 Scheduler；**不要原地重试**
         * （否则会变成忙等），也不要当作业务取消向上传播。
         */
        kNotArmed = 4,
        /**
         * 非法/实现违约：等待返回时节点结果仍是 `kWaiting`（状态机不变量
         * 被破坏），或参数组合非法。属于实现缺陷信号，调用方应停止重试、
         * 记录诊断并尽快失败，而不是把结果当作正常超时/取消处理。
         */
        kInvalid = 5,
    };

    ParkingCondition();
    ~ParkingCondition();
    ParkingCondition(const ParkingCondition&) = delete;
    ParkingCondition& operator=(const ParkingCondition&) = delete;

    /** @brief 唤醒一个等待者。 */
    void notify_one() noexcept;
    /** @brief 唤醒全部等待者。 */
    void notify_all() noexcept;

    /** @brief 持锁等待直到 predicate 成功或 Fiber 被取消。 */
    template <typename Predicate>
    bool wait(std::unique_lock<std::mutex>& lock, Predicate predicate) {
        while (!predicate()) {
            // 任何非 kNotified 结局都必须退出：kCancelled/kNotArmed/kInvalid
            // 都表示本次等待没有拿到通知，继续循环只会忙等（尤其是无法 park
            // 的手动 Fiber，每次都会立即返回 kNotArmed）。
            if (WaitOnce(lock, std::nullopt, predicate) !=
                WaitStatus::kNotified) {
                return predicate();
            }
        }
        return true;
    }

    /** @brief 带相对超时的条件等待。 */
    template <typename Rep, typename Period, typename Predicate>
    bool wait_for(std::unique_lock<std::mutex>& lock,
                  std::chrono::duration<Rep, Period> timeout,
                  Predicate predicate) {
        const auto duration =
            std::chrono::duration_cast<Clock::duration>(timeout);
        const auto now = Clock::now();
        const auto deadline = duration <= Clock::duration::zero()
                                  ? now
                                  : (duration >= Clock::time_point::max() - now
                                         ? Clock::time_point::max()
                                         : now + duration);
        return wait_until(lock, deadline, std::move(predicate));
    }

    /** @brief 等待到绝对截止时间或 predicate 成功。 */
    template <typename Predicate>
    bool wait_until(std::unique_lock<std::mutex>& lock,
                    Clock::time_point deadline, Predicate predicate) {
        while (!predicate()) {
            const auto result = WaitOnce(lock, deadline, predicate);
            if (result != WaitStatus::kNotified) {
                return predicate();
            }
        }
        return true;
    }

    /** @brief 返回当前 Fiber 是否收到取消请求。 */
    static bool CancellationRequested() noexcept;

    // 手动 Fiber（没有 Scheduler/Task 绑定）无法把等待交还给 carrier
    // 线程，因此禁止退回原生 condition_variable 造成整条线程阻塞。
    // 调用方应先绑定 Scheduler，或使用非阻塞接口处理该情况。
    /** @brief 返回当前 Fiber 是否缺少可用 Scheduler 等待后端。 */
    static bool FiberWaitUnsupported() noexcept;

    /**
     * @brief 采集等待队列的只读快照（等待者数量与各节点状态）。
     * @note 持队列锁遍历侵入式链表；不唤醒、不修改任何节点。
     */
    ParkingConditionDebugSnapshot DebugSnapshot() const;

private:
    class WaitNode;

    WaitStatus WaitOnce(std::unique_lock<std::mutex>& lock,
                        std::optional<Clock::time_point> deadline,
                        const std::function<bool()>& predicate);

    // mutable：DebugSnapshot() 需要作为 const 观测接口持锁遍历等待队列；
    // 只影响 const 限定，不改变任何同步语义。
    //
    // 队列锁只保护「链表的链接关系 + 等待者计数」，临界区是几条指针赋值。
    // 节点自己的结果/futex 代际是原子字段：等待者自旋、通知者的 Finish、
    // 以及 managed 唤醒都不需要这把锁（managed 走节点私有的 m_wake_mutex，
    // 即 Disarm 门）。锁序：队列锁与节点门从不嵌套获取。
    mutable FutexMutex m_queue_mutex;
    WaitNode* m_head{nullptr};
    WaitNode* m_tail{nullptr};
    std::size_t m_waiter_count{0};

    // ---- 广播代际字（futex_waitv 混合方案）----
    // notify_all 需要唤醒 N 个 native waiter。per-node futex 的地址彼此不同，
    // 逐个 FUTEX_WAKE 就是 N 次系统调用；因此让每个 native waiter 用
    // futex_waitv 同时监听「自己的节点字 + 这个共享广播字」：notify_one 只推进
    // 选中节点的字（保持 D2 的定向唤醒），notify_all 只推进一次广播字并做一次
    // FUTEX_WAKE(INT_MAX)。没有 futex_waitv 的内核/编译环境退回逐节点唤醒。
    struct BroadcastState {
        std::atomic<std::uint32_t> seq{0};
        // 处于 native 等待路径的 waiter 数量：为 0 时广播可跳过系统调用。
        std::atomic<std::uint32_t> waiters{0};
    };
    alignas(64) BroadcastState m_broadcast;
};


/**
 * @brief 把 `WaitStatus` 翻译成可写进日志/错误信息的具体含义。
 *
 * @param status [in] 任意 `WaitStatus` 值（含未来新增值）。
 * @return 描述「发生了什么、调用方该怎么做」的静态字符串；未知值返回
 *         `"unknown WaitStatus"`。返回值是静态存储，进程生命周期有效，
 *         调用方**不要**释放，也不要假设它等于枚举名。
 *
 * @note `noexcept` 且无堆分配：可在取消路径、信号处理后的收尾路径与
 *       每等待一次的日志里直接调用。它是诊断接口，不参与控制流——控制流
 *       一律比较枚举本身。
 */
inline const char* WaitStatusMessage(ParkingCondition::WaitStatus status) noexcept {
    switch (status) {
    case ParkingCondition::WaitStatus::kNotified:
        return "等待正常结束：谓词已满足（通知在入队前到达或等待期间被唤醒）；"
               "调用方按正常路径继续，不要重试本次等待";
    case ParkingCondition::WaitStatus::kTimedOut:
        return "等待超过 deadline 且谓词仍未满足；调用方应检查业务条件，"
               "决定重试或把超时返回上层，不要当作取消";
    case ParkingCondition::WaitStatus::kCancelled:
        return "所属 Task 的取消请求在等待前或等待期间生效；调用方应立即"
               "放弃本次操作并向上传播取消，不要重试";
    case ParkingCondition::WaitStatus::kSpurious:
        return "被与本次等待无关的唤醒/信号打断且谓词仍未满足；当前实现会在"
               "内部重新 park，正常情况下调用方看不到此值；"
               "若收到应重检谓词并重试等待";
    case ParkingCondition::WaitStatus::kNotArmed:
        return "当前上下文无法 park（典型是未绑定 Scheduler 的手动 Fiber），"
               "等待未进入阻塞就返回；调用方应改用非阻塞接口或先绑定"
               "Scheduler，不要原地重试，也不要当作业务取消";
    case ParkingCondition::WaitStatus::kInvalid:
        return "等待返回时节点仍处于 kWaiting，说明状态机不变量被破坏"
               "（实现缺陷或非法参数）；调用方应停止重试、记录诊断并尽快失败";
    }
    return "unknown WaitStatus";
}
}  // namespace go2cpp::core
