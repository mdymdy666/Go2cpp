/**
 * @file mq.hpp
 * @brief POSIX 消息队列（`mq_open`/`mq_send`/`mq_receive`）封装（R31 接入异步）。
 *
 * ## 为什么需要它
 *
 * 管道/共享内存都不提供「带优先级、带消息边界、内核负责唤醒」的语义。
 * MCP 的任务分发（topic/frame → 多个 worker 进程）正好需要：
 * 消息边界、优先级、队列满时的明确策略（`ENOBUFS`）。
 *
 * ## 名称与生命周期
 *
 * - 名字必须以 `/` 开头且**只含一个 `/`**（POSIX 要求），本模块不做自动补
 *   `/`，非法名字返回 `kInvalidArgument`；
 * - `Create` 用 `O_CREAT|O_EXCL`，已存在返回 `kMqFailed` + `EEXIST`，不会
 *   静默复用别人的队列；
 * - `mq_unlink` 只删除名字，已打开的描述符仍可收发（POSIX 语义）；所以
 *   「先 unlink 再关闭」是安全的清理顺序，`MessageQueue::Unlink` 只做 unlink。
 * - 进程退出后 `/dev/mqueue` 里的名字会残留（内核不自动清理），测试和示例
 *   必须显式 `Unlink`，文档里已写明。
 *
 * ## 超时与 `ENOBUFS` 策略
 *
 * `mq_timedsend` 的 `abs_timeout` 是**绝对时间**，本模块把相对毫秒换算成
 * `CLOCK_REALTIME` 绝对时间；`timeout == 0` 传「当前时间」实现立即返回，
 * `kWaitForever` 传 `nullptr`。
 *
 * 队列满时：
 * - `QueueOverflowPolicy::kBlock`：按 `timeout` 阻塞等待；
 * - `QueueOverflowPolicy::kTimeout`：立即返回 `kQueueFull`（不等待）；
 * - `QueueOverflowPolicy::kDropNewest`：**丢弃本条新消息**并返回成功，同时
 *   在 `stats.dropped` 计数（不删除队列里的旧消息 —— 那需要额外一次 receive，
 *   会在多消费者场景抢走别人的消息）。
 *
 * ## 大小限制
 *
 * `max_message_size` 不能超过 `/proc/sys/fs/mqueue/msgsize_max`，而
 * `max_messages` 不能超过 `msg_max`。创建前会用 `mq_open` 的实际属性校验；
 * 本机默认是 `10` 条 / `8192` 字节（WSL），因此压测按「小消息 + 高频」设计，
 * 而不是靠大队列堆吞吐。
 *
 * ## 异步等待（R31，优先 epoll）
 *
 * Linux 的 `mqd_t` 是文件描述符，可 `epoll_ctl` 注册：队列非空时 `EPOLLIN`、
 * 未满时 `EPOLLOUT`（满队列时 `EPOLLOUT` 被抑制，取走一条后恢复）。
 * `EnableAsync(manager)` 之后：
 * - `Receive`：空队列时 `WaitReadable` park Fiber，就绪后非阻塞重试；
 * - `Send(kBlock)`：满队列时 `WaitWritable` park Fiber，就绪后重试；
 * - **不使用 `mq_notify`**：SIGEV_THREAD 会创建内核线程，且与每队列单次通知
 *   的语义（`mq_notify` 每次都要重新注册）不适合多 Fiber 等待；epoll 是
 *   每 fd 的就绪位，语义更直接。
 *
 * @note `fstat(mqd_t)` 显示为 `S_IFREG`（mqueue 文件系统），`DetectIoCapability`
 *       会误判为普通文件。mq 的能力必须用 `MessageQueue::Capability()` 查询。
 * @note `mq_send`/`mq_receive` 不在 `HookOp` 列表内：同步模式下在 Fiber 中
 *       调用会阻塞 M；异步模式下等待由 `IOManager` park。
 */
#pragma once

#include "go2cpp/os/channel.hpp"

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace go2cpp::os {

/** @brief 消息队列的打开模式。 */
enum class MqAccess : std::uint8_t { kReadOnly, kWriteOnly, kReadWrite };

/** @brief 队列满时的策略。 */
enum class QueueOverflowPolicy : std::uint8_t {
    kBlock,       ///< 阻塞到 `timeout`（`kWaitForever` 为永久）
    kTimeout,     ///< 立即返回 `kQueueFull`
    kDropNewest,  ///< 丢弃新消息并返回成功（计数）
};

/** @brief 从队列收到的一条消息。 */
struct QueueMessage final {
    std::vector<std::byte> payload;
    unsigned priority{0};
};

/**
 * @brief POSIX 消息队列的 RAII 句柄（可移动，不可复制）。
 *
 * @details 析构只 `mq_close`，不 `mq_unlink` —— 名字的生命周期由创建者显式
 *          管理。`Create` 成功后应保证有一条清理路径（测试用 RAII guard）。
 */
class MessageQueue final {
public:
    MessageQueue() noexcept = default;
    ~MessageQueue();

    MessageQueue(const MessageQueue&) = delete;
    MessageQueue& operator=(const MessageQueue&) = delete;
    MessageQueue(MessageQueue&& other) noexcept;
    MessageQueue& operator=(MessageQueue&& other) noexcept;

    /**
     * @brief 创建队列（`O_CREAT|O_EXCL|O_RDWR`）。
     * @param name 以 `/` 开头的名字。
     * @param max_messages 队列深度（>0）。
     * @param max_message_size 单条消息上限（>0，且不超过内核 `msgsize_max`）。
     * @param out 成功时写入句柄。
     * @param mode 权限位，默认 0600。
     */
    static Result<void> Create(const std::string& name, long max_messages,
                               long max_message_size, MessageQueue* out,
                               mode_t mode = 0600);

    /// 打开已存在的队列。
    static Result<void> Open(const std::string& name, MessageQueue* out,
                             MqAccess access = MqAccess::kReadWrite);

    /// 删除名字（已打开的句柄不受影响）。
    static Result<void> Unlink(const std::string& name);

    /**
     * @brief 发送一条消息。
     * @param priority 0..`MQ_PRIO_MAX`（Linux 为 32）；越大越先被接收。
     * @param timeout 统一超时语义。
     * @param policy 队列满时的策略。
     * @return 超限 `kInvalidArgument`；队列满按策略返回 `kQueueFull`；
     *         内核错误返回 `kMqFailed`（`errno` 保留）。
     */
    Result<void> Send(ByteSpan payload, unsigned priority = 0,
                      std::chrono::milliseconds timeout = kWaitForever,
                      QueueOverflowPolicy policy = QueueOverflowPolicy::kBlock);

    /// 接收一条消息；超时返回 `kTimeout`。
    Result<QueueMessage> Receive(std::chrono::milliseconds timeout = kWaitForever);

    /// 批量发送；返回成功条数（遇到满队列按策略停止/继续）。
    Result<std::size_t> SendBatch(const std::vector<QueueMessage>& messages,
                                  std::chrono::milliseconds timeout = kWaitForever,
                                  QueueOverflowPolicy policy = QueueOverflowPolicy::kBlock);

    /// 批量接收最多 `max_count` 条；至少收到一条才算成功。
    Result<std::vector<QueueMessage>> ReceiveBatch(
        std::size_t max_count,
        std::chrono::milliseconds timeout = kWaitForever);

    const std::string& name() const noexcept { return m_name; }
    int fd() const noexcept { return m_fd; }
    long max_messages() const noexcept { return m_max_messages; }
    long max_message_size() const noexcept { return m_max_message_size; }
    /// `kDropNewest` 策略下被丢弃的消息数。
    std::uint64_t dropped() const noexcept { return m_dropped; }
    bool valid() const noexcept { return m_fd >= 0; }

    /// 关闭描述符（幂等）；会先解绑异步等待。
    void Close() noexcept;

    // ---- IO 能力与显式异步（R31）---------------------------------------

    /**
     * @brief mq fd 的能力描述。
     * @details 恒为可 poll / 可非阻塞 / 支持异步（Linux mqueue 实现保证了
     *          `EPOLLIN`/`EPOLLOUT` 且 `O_NONBLOCK` 下返回 `EAGAIN`）。
     *          未打开时返回 `kUnknown` + reason。
     */
    IoCapability Capability() const;

    /// 设置/清除 mq fd 的 `O_NONBLOCK`（异步模式内部使用）。
    Result<void> SetNonBlocking(bool enable);

    /**
     * @brief 把等待显式交给 `IOManager`（epoll，不用 `mq_notify`）。
     * @return 未打开 `kNotRunning`；重复启用 `kInvalidArgument`；
     *         manager 为空 `kInvalidArgument`。
     */
    Result<void> EnableAsync(go2cpp::io::IOManager* manager);

    /// 恢复 Enable 前的阻塞状态并解绑（幂等）。
    Result<void> DisableAsync();

    bool async_enabled() const noexcept;

private:
    std::string m_name;
    int m_fd{-1};
    long m_max_messages{0};
    long m_max_message_size{0};
    std::uint64_t m_dropped{0};

    /// 取异步绑定快照；调用方持有返回的 `shared_ptr` 期间绑定不会被销毁。
    std::shared_ptr<AsyncIoBinding> async_binding() const;

    /// 保护 `m_async` 指针本身：`EnableAsync` 与 `Send`/`Receive` 可能来自
    /// 不同线程/Fiber（TSan 在 R31 异步测试里捕捉到该竞争）。绑定内部状态
    /// 仍由 `AsyncIoBinding` 自己的原子量 + 互斥量保护，这里只保护指针读写。
    mutable std::mutex m_async_mutex;
    std::shared_ptr<AsyncIoBinding> m_async;
};

/**
 * @brief 用消息队列实现的 `MessageChannel` 后端。
 *
 * @details 每条 `Write` 是一整条 mq 消息；`Read` 从内部缓存返回一条消息。
 *          适合「离散命令/事件」而不是大块流；大块数据请用 shm。
 *
 *          `EnableAsync` 转发给内部的 `MessageQueue`，因此 `Pull`/`Write`
 *          在空队列/满队列时会 park Fiber（与 pipe/socketpair 后端一致）。
 */
class MqChannel final : public MessageChannel {
public:
    /// 创建（`owner == true`）或打开同名队列。
    static Result<std::unique_ptr<MqChannel>> Open(const std::string& name, bool owner,
                                                   ChannelOptions options = {});

    ~MqChannel() override;

    Result<void> Write(ByteSpan data, std::chrono::milliseconds timeout) override;
    void Close() noexcept override;
    bool closed() const noexcept override;
    const ChannelStats& stats() const noexcept override;
    int native_fd() const noexcept override;

    // ---- IO 能力与异步（R31）----
    IoCapability Capability() const override;
    Result<void> SetNonBlocking(bool enable) override;
    Result<void> EnableAsync(go2cpp::io::IOManager* manager) override;
    Result<void> DisableAsync() override;
    bool async_enabled() const noexcept override;

    const std::string& name() const noexcept;

protected:
    Result<std::size_t> Pull(std::chrono::milliseconds timeout) override;

private:
    explicit MqChannel(ChannelOptions options);
    Result<void> OpenQueue(const std::string& name, bool owner);

    MessageQueue m_queue;
    bool m_owner{false};
    bool m_closed{false};
    std::vector<std::byte> m_pending;  ///< 已收到但尚未被上层消费的当前消息
    std::size_t m_pending_offset{0};
};

}  // namespace go2cpp::os
