/**
 * @file channel.hpp
 * @brief `go2cpp::os` 的**进程间通信**抽象：字节通道、帧格式、管道/socketpair、
 *        背压缓冲与可扩展的后端注册表。
 *
 * ## 设计边界（为什么不是一个万能 Channel）
 *
 * 上层（`Process` 的 stdio、`Supervisor`、`ProcessPool`）只需要「读写字节 +
 * 超时 + 关闭」这一个最小接口；帧格式是**协议层**的事，放在基类的非虚
 * `WriteFrame`/`ReadFrame` 里，后端不需要各自实现。
 *
 * 因此 `MessageChannel` 的子类只需要实现两件事：
 * - `Pull()`：把底层数据读进内部接收缓冲（返回本次读到的字节数，0 表示 EOF）；
 * - `Write()`：把一整段字节写出去（内部负责 EINTR/部分写/超时）。
 *
 * `Read()` 与全部 `*Frame`/`*Line` 都在基类里用同一份接收缓冲实现，保证
 * pipe/socketpair/共享内存环/消息队列四种后端的 framing 行为完全一致。
 *
 * ## 帧格式（R10，文档定死）
 *
 * 所有整数小端（little-endian）：
 *
 * | 偏移 | 长度 | 字段 | 说明 |
 * |---:|---:|---|---|
 * | 0 | 4 | magic | `0x46 0x43 0x32 0x47`，即 ASCII `G2CF` |
 * | 4 | 1 | version | 当前 `1`；不匹配直接报错，不做兼容猜测 |
 * | 5 | 1 | type | 应用自定义（0 保留给「未分类」） |
 * | 6 | 2 | flags | 应用自定义位标志 |
 * | 8 | 4 | payload_size | payload 字节数，上限 `max_frame_bytes` |
 * | 12 | N | payload | 原始字节 |
 * | 12+N | 4 | crc32 | IEEE CRC-32，覆盖 version..payload（不含 magic） |
 *
 * 校验和覆盖 `version` 到 `payload`：magic 只用于快速定位/拒绝非本协议数据，
 * 把它排除在 CRC 外可以让「扫描流中帧头」的实现更简单。
 *
 * ## 超时与 EOF
 *
 * - `Read`/`ReadFrame`/`ReadLine`：`timeout` 语义与 `proc.hpp` 一致；
 * - 对端关闭时返回 `kClosed`（而不是空数据），调用方不会把 EOF 误当超时；
 * - `Write` 在对端关闭时返回 `kChannelClosed`（底层 `EPIPE`），不会杀进程
 *   （`SIGPIPE` 由 `MSG_NOSIGNAL`/`SIG_IGN` 策略规避，本模块不修改全局信号处理）。
 *
 * ## 与 Fiber 的关系
 *
 * `FdChannel::Pull/Write` 用 `poll` + `read/write` 的**裸 syscall**，不经过
 * Hook。在 Fiber 中调用会阻塞 M。需要协程化请直接用 `native_fd()` 走 libc
 * （hook 路径），代价见 `docs/os-process.md`。
 */
#pragma once

#include "go2cpp/os/io_capability.hpp"
#include "go2cpp/os/proc.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::os {

// ---------------------------------------------------------------------------
// 字节视图（C++17 没有 std::span，这里用最薄的指针 + 长度）
// ---------------------------------------------------------------------------

/** @brief 只读字节视图；不拥有内存。 */
struct ByteSpan final {
    const std::byte* data{nullptr};
    std::size_t size{0};

    ByteSpan() = default;
    ByteSpan(const void* bytes, std::size_t length) noexcept
        : data(static_cast<const std::byte*>(bytes)), size(length) {}

    bool empty() const noexcept { return size == 0; }
};

/** @brief 可写字节视图；不拥有内存。 */
struct MutableByteSpan final {
    std::byte* data{nullptr};
    std::size_t size{0};

    MutableByteSpan() = default;
    MutableByteSpan(void* bytes, std::size_t length) noexcept
        : data(static_cast<std::byte*>(bytes)), size(length) {}

    bool empty() const noexcept { return size == 0; }
};

/** @brief `std::string_view` -> `ByteSpan`。 */
inline ByteSpan AsBytes(std::string_view text) noexcept {
    return ByteSpan(text.data(), text.size());
}

/** @brief `ByteSpan` -> `std::string_view`（只读，不拷贝）。 */
inline std::string_view AsStringView(ByteSpan bytes) noexcept {
    return std::string_view(reinterpret_cast<const char*>(bytes.data), bytes.size);
}

/** @brief `ByteSpan` -> `std::string`（拷贝）。 */
inline std::string AsString(ByteSpan bytes) {
    return std::string(AsStringView(bytes));
}

// ---------------------------------------------------------------------------
// 帧与 framing
// ---------------------------------------------------------------------------

/// 帧头长度（magic + version + type + flags + size）。
inline constexpr std::size_t kFrameHeaderSize = 12;
/// 帧尾长度（CRC32）。
inline constexpr std::size_t kFrameTrailerSize = 4;
/// 帧魔数（小端读作 `0x47433246`，即 ASCII "G2CF"）。
inline constexpr std::uint32_t kFrameMagic = 0x47433246u;
/// 当前协议版本。
inline constexpr std::uint8_t kFrameVersion = 1;
/// 单帧 payload 的绝对上限（防止恶意长度导致 OOM）。
inline constexpr std::size_t kMaxFramePayloadLimit = 64u * 1024u * 1024u;

/** @brief 一帧解出来的内容。 */
struct Frame final {
    std::uint8_t type{0};
    std::uint16_t flags{0};
    std::vector<std::byte> payload;
};

/** @brief 通道的 framing 模式。 */
enum class Framing : std::uint8_t {
    kRaw,             ///< 纯字节流；`Read` 直接返回可用字节
    kLine,            ///< 行帧：`\n` 分隔；`ReadLine` 更直接
    kLengthPrefixed,  ///< 上面的 G2CF 帧格式
};

/** @brief 通道配置。 */
struct ChannelOptions final {
    Framing framing{Framing::kRaw};
    /// 单帧 payload 上限；超过 `kMaxFramePayloadLimit` 时被钳制。
    std::size_t max_frame_bytes{8u * 1024u * 1024u};
    /// 每次 `Pull` 最多读多少字节（也是内部接收缓冲的增长粒度）。
    std::size_t read_chunk_bytes{64u * 1024u};
};

/** @brief 通道累计统计。 */
struct ChannelStats final {
    std::uint64_t bytes_read{0};
    std::uint64_t bytes_written{0};
    std::uint64_t frames_read{0};
    std::uint64_t frames_written{0};
    std::uint64_t eof_count{0};
    std::uint64_t timeout_count{0};
    std::uint64_t error_count{0};
    std::uint64_t dropped_bytes{0};  ///< 背压丢弃（`FlowBuffer` 使用）
};

/** @brief 一次原始读取的结果。 */
struct ReadResult final {
    std::size_t bytes{0};
    bool eof{false};
};

// ---------------------------------------------------------------------------
// MessageChannel
// ---------------------------------------------------------------------------

/**
 * @brief 统一的字节通道抽象。
 *
 * @details 子类实现 `Pull`/`Write`/`Close`/`closed`/`stats`；基类提供 framing
 *          与便利方法。所有方法都**不抛异常**，错误用 `Result` 表达。
 *
 * @note 不是线程安全的：同一个通道只能由一个读者 + 一个写者使用（SPSC）。
 *       需要多写者请在外层加锁或改用消息队列。
 */
class MessageChannel {
public:
    explicit MessageChannel(ChannelOptions options = {});
    virtual ~MessageChannel() = default;

    MessageChannel(const MessageChannel&) = delete;
    MessageChannel& operator=(const MessageChannel&) = delete;

    /**
     * @brief 读一段数据。
     * @details `kRaw` 返回字节流；`kLine` 返回一整行（不含 `\n`）；
     *          `kLengthPrefixed` 返回一帧的 payload（丢弃 type/flags，想拿
     *          type 用 `ReadFrame`）。
     * @return 超时返回 `kTimeout`；对端关闭返回 `kClosed`；成功可能返回 0 字节
     *         （非阻塞无数据时返回 `kAgain`，不会返回空成功）。
     */
    Result<ReadResult> Read(MutableByteSpan buffer, std::chrono::milliseconds timeout);

    /// 写一整段（内部处理 EINTR/部分写/超时）。
    virtual Result<void> Write(ByteSpan data, std::chrono::milliseconds timeout) = 0;

    /// 与 `Write` 同义（保留名字以便协议层代码可读）。
    Result<void> WriteAll(ByteSpan data, std::chrono::milliseconds timeout) {
        return Write(data, timeout);
    }

    /// 读一整行（`kLine` 模式；不含 `\n`）。EOF 且无残余数据返回 `kClosed`。
    Result<std::string> ReadLine(std::chrono::milliseconds timeout);

    /// 写一整行（拒绝包含 `\n` 的内容，避免协议歧义）。
    Result<void> WriteLine(std::string_view line, std::chrono::milliseconds timeout);

    /// 读一帧（`kLengthPrefixed` 模式）。
    Result<Frame> ReadFrame(std::chrono::milliseconds timeout);

    /// 写一帧（自动填 magic/version/crc32）。
    Result<void> WriteFrame(std::uint8_t type, ByteSpan payload,
                            std::chrono::milliseconds timeout,
                            std::uint16_t flags = 0);

    /// 读最多 `max_bytes` 的剩余内容直到 EOF（用于收集输出）。
    Result<std::string> ReadToEnd(std::size_t max_bytes,
                                  std::chrono::milliseconds timeout);

    virtual void Close() noexcept = 0;
    virtual bool closed() const noexcept = 0;
    /// 底层 fd；非 fd 后端返回 -1。
    virtual int native_fd() const noexcept { return -1; }
    virtual const ChannelStats& stats() const noexcept = 0;

    // ---- IO 能力与显式异步（R27-R30）----
    /// 底层 fd 的能力描述；无 fd 的后端返回 `kUnknown`。
    virtual IoCapability Capability() const;
    /// 设置/清除 `O_NONBLOCK`；能力不允许时返回 `kUnsupported` + reason。
    virtual Result<void> SetNonBlocking(bool enable);
    /// 显式把等待交给 `IOManager`；不支持时返回 `kUnsupported` + reason。
    virtual Result<void> EnableAsync(go2cpp::io::IOManager* manager);
    /// 退回原生阻塞并解绑；幂等。
    virtual Result<void> DisableAsync();
    virtual bool async_enabled() const noexcept { return false; }

    Framing framing() const noexcept { return m_options.framing; }
    void SetFraming(Framing framing) noexcept { m_options.framing = framing; }
    const ChannelOptions& options() const noexcept { return m_options; }

protected:
    /**
     * @brief 把底层数据读进内部接收缓冲。
     * @return 本次读到的字节数；0 表示 EOF；超时返回 `kTimeout`；无数据返回 `kAgain`。
     */
    virtual Result<std::size_t> Pull(std::chrono::milliseconds timeout) = 0;

    /// 确保接收缓冲至少有 `needed` 字节；失败返回错误（含 EOF 的 `kClosed`）。
    Result<void> EnsureBuffered(std::size_t needed, std::chrono::milliseconds timeout);

    /// 把新读到的字节追加到接收缓冲（子类在 `Pull` 里调用）。
    void AppendRx(const void* data, std::size_t count);

    /// 从接收缓冲消费 `count` 字节（调用方保证已缓冲）。
    void Consume(std::size_t count) noexcept;

    /// 接收缓冲中可读字节数。
    std::size_t buffered() const noexcept { return m_rx.size() - m_rx_offset; }

    /// 接收缓冲起始指针。
    const std::byte* rx_data() const noexcept { return m_rx.data() + m_rx_offset; }

    /// 接收缓冲的原始指针（解帧后整体搬移用）。
    std::vector<std::byte>& rx_buffer() noexcept { return m_rx; }

    /// 设置 EOF（子类在 `Pull` 返回 0 时调用）。
    void MarkEof() noexcept { m_eof = true; }
    bool eof() const noexcept { return m_eof; }

    /// 记录一次超时（子类可调用）。
    void NoteTimeout() noexcept { ++m_stats.timeout_count; }

    ChannelOptions m_options;  ///< framing/上限等配置
    ChannelStats m_stats;      ///< 累计统计

private:
    Result<std::size_t> FillFor(std::chrono::milliseconds timeout);
    Result<std::string> ReadDelimited(std::chrono::milliseconds timeout);

    std::vector<std::byte> m_rx;  ///< 接收缓冲；`m_rx_offset` 之前是已消费数据
    std::size_t m_rx_offset{0};
    bool m_eof{false};
};

// ---------------------------------------------------------------------------
// FdChannel：pipe / socketpair 的 fd 后端
// ---------------------------------------------------------------------------

/**
 * @brief 基于 fd 的通道（pipe 或 Unix socketpair）。
 *
 * @details 构造时把 fd 设为非阻塞（`fcntl` 裸调用）；`take_ownership == true`
 *          时析构关闭 fd。读写全部走裸 syscall，不经过 Hook。
 */
class FdChannel final : public MessageChannel {
public:
    FdChannel(int fd, bool take_ownership, ChannelOptions options = {});
    ~FdChannel() override;

    Result<void> Write(ByteSpan data, std::chrono::milliseconds timeout) override;
    void Close() noexcept override;
    bool closed() const noexcept override;
    int native_fd() const noexcept override;
    const ChannelStats& stats() const noexcept override;

    // ---- IO 能力与异步（R27-R30）----
    IoCapability Capability() const override;
    Result<void> SetNonBlocking(bool enable) override;
    Result<void> EnableAsync(go2cpp::io::IOManager* manager) override;
    Result<void> DisableAsync() override;
    bool async_enabled() const noexcept override;

protected:
    Result<std::size_t> Pull(std::chrono::milliseconds timeout) override;

private:
    int m_fd{-1};
    bool m_owned{true};
    bool m_closed{false};
    IoCapability m_capability;  ///< 构造时探测一次（fd 类型不会变）
    AsyncIoBinding m_async;     ///< 显式异步绑定；默认未启用
};

/** @brief 便捷构造。 */
std::unique_ptr<FdChannel> MakeFdChannel(int fd, bool take_ownership,
                                         ChannelOptions options = {});

// ---------------------------------------------------------------------------
// Pipe / SocketPair（R7）
// ---------------------------------------------------------------------------

/**
 * @brief 单向管道（`pipe2`）封装。
 *
 * @details 所有权规则：
 * - `Create()` 返回的对象持有两个 fd；
 * - `ReleaseRead()`/`ReleaseWrite()` 放弃所有权（返回 fd，对象不再关闭它）；
 * - 析构关闭仍持有的 fd；移动后源对象不再持有。
 * - 默认 `O_CLOEXEC`（`close_on_exec = false` 时显式清除）。
 */
class Pipe final {
public:
    Pipe() noexcept = default;
    ~Pipe();

    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    Pipe(Pipe&& other) noexcept;
    Pipe& operator=(Pipe&& other) noexcept;

    static Result<Pipe> Create(bool close_on_exec = true);

    int read_fd() const noexcept { return m_read; }
    int write_fd() const noexcept { return m_write; }
    bool valid() const noexcept { return m_read >= 0 && m_write >= 0; }

    int ReleaseRead() noexcept;
    int ReleaseWrite() noexcept;
    void CloseRead() noexcept;
    void CloseWrite() noexcept;

    /// 管道两端的能力描述（基于读端 fd 探测）。
    IoCapability Capability() const;
    /// 对两端同时设置/清除 `O_NONBLOCK`。
    Result<void> SetNonBlocking(bool enable);

private:
    int m_read{-1};
    int m_write{-1};
};

/**
 * @brief 双向 Unix domain socketpair。
 *
 * @details 两端对等（`SOCK_STREAM` 或 `SOCK_SEQPACKET`），常用于需要双向
 *          通信又不想开两个管道的场景。所有权/移动语义与 `Pipe` 相同。
 */
class SocketPair final {
public:
    SocketPair() noexcept = default;
    ~SocketPair();

    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;
    SocketPair(SocketPair&& other) noexcept;
    SocketPair& operator=(SocketPair&& other) noexcept;

    /// `type` 默认 `SOCK_STREAM`；`SOCK_SEQPACKET` 保留消息边界。
    static Result<SocketPair> Create(int type = 1 /* SOCK_STREAM */,
                                     bool close_on_exec = true);

    int first_fd() const noexcept { return m_first; }
    int second_fd() const noexcept { return m_second; }
    bool valid() const noexcept { return m_first >= 0 && m_second >= 0; }

    int ReleaseFirst() noexcept;
    int ReleaseSecond() noexcept;
    void CloseFirst() noexcept;
    void CloseSecond() noexcept;

    /// socketpair 两端的能力描述（基于第一端 fd 探测）。
    IoCapability Capability() const;
    /// 对两端同时设置/清除 `O_NONBLOCK`。
    Result<void> SetNonBlocking(bool enable);

private:
    int m_first{-1};
    int m_second{-1};
};

// ---------------------------------------------------------------------------
// 背压与水位线（R12）
// ---------------------------------------------------------------------------

/** @brief 缓冲超过高水位时的动作。 */
enum class BackpressureAction : std::uint8_t {
    kBlock,    ///< 阻塞到低于低水位（受 timeout 约束）
    kTimeout,  ///< 立即返回 `kQueueFull`
    kDrop,     ///< 丢弃新写入的数据并计数（返回成功）
};

/** @brief 背压策略。 */
struct FlowControlPolicy final {
    std::size_t high_watermark{256u * 1024u};
    std::size_t low_watermark{64u * 1024u};
    BackpressureAction action{BackpressureAction::kBlock};
    std::chrono::milliseconds timeout{1000};
};

/**
 * @brief 有界字节缓冲 + 高/低水位 + 背压策略。
 *
 * @details 给单线程事件循环使用：写端 `Push` 超过高水位时按策略阻塞/超时/丢弃，
 *          读端 `Pop` 降到低水位以下时唤醒写端。用 `mutex + condition_variable`
 *          而不是无锁结构 —— 这里的瓶颈是策略语义，不是微秒级延迟；共享内存
 *          的大块传输用 `RingBuffer`（无锁 SPSC）而不是本类。
 *
 *          单条数据大于「高水位 - 低水位」时，`kBlock` 会直接入队（允许暂时
 *          超过高水位），否则会永久等不到足够空间 —— 这是有意的例外，文档写明。
 */
class FlowBuffer final {
public:
    explicit FlowBuffer(FlowControlPolicy policy = {});

    /**
     * @brief 写入数据。
     * @return `kTimeout`（kBlock 超时/kTimeout 策略）或 `kQueueFull`；
     *         `kDrop` 策略下返回成功但 `stats().dropped_bytes` 增加。
     */
    Result<void> Push(ByteSpan data, std::chrono::milliseconds timeout);

    /// 读出最多 `out.size` 字节；返回实际字节数（可能为 0）。
    std::size_t Pop(MutableByteSpan out);

    /// 查看但不消费；返回可拷贝的字节数。
    std::size_t Peek(MutableByteSpan out) const;

    std::size_t size() const;
    bool empty() const;
    bool above_high() const;
    bool below_low() const;
    const FlowControlPolicy& policy() const noexcept { return m_policy; }
    const ChannelStats& stats() const noexcept { return m_stats; }
    void Clear();

private:
    bool CanAccept(std::size_t count) const noexcept;
    std::size_t AvailableCapacity() const noexcept;

    FlowControlPolicy m_policy;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<std::byte> m_data;
    std::size_t m_head{0};
    ChannelStats m_stats;
};

// ---------------------------------------------------------------------------
// 后端注册表（R10/R23）
// ---------------------------------------------------------------------------

/** @brief 内置后端种类。 */
enum class ChannelKind : std::uint8_t {
    kPipe,          ///< 已有 fd（管道读/写端）
    kSocketPair,    ///< 已有 fd（socketpair 一端）
    kSharedMemory,  ///< 命名共享内存环
    kMessageQueue,  ///< POSIX 消息队列
};

/** @brief 创建通道的规格。 */
struct ChannelSpec final {
    ChannelKind kind{ChannelKind::kPipe};
    int fd{-1};                 ///< kPipe/kSocketPair：要接管的 fd
    bool take_ownership{true};  ///< fd 是否由通道负责关闭
    std::string name;           ///< shm/mq 名称
    std::size_t capacity{1u << 20};  ///< shm 环容量 / mq 深度相关
    bool owner{false};          ///< shm/mq：是否创建者（负责 unlink）
    bool overwrite{false};      ///< shm 环：覆盖模式
    ChannelOptions options;
};

/** @brief 后端工厂。 */
using ChannelFactory =
    std::function<Result<std::unique_ptr<MessageChannel>>(const ChannelSpec&)>;

/**
 * @brief 后端注册表：内置 pipe/socketpair/shm/mq，允许注册自定义名字。
 *
 * @details R23 要求「不许把共享内存或 mq 写死进上层」：上层只持有
 *          `ChannelSpec` 与 `MessageChannel`，通过本注册表创建。新增后端只需
 *          `Register("my-backend", factory)`，不改任何调用方代码。
 *
 * @note 注册/查询是线程安全的（内部互斥量）；工厂在调用线程执行。
 */
class ChannelRegistry final {
public:
    static ChannelRegistry& Instance();

    /// 注册/覆盖一个后端；名字为空返回 false。
    bool Register(const std::string& backend, ChannelFactory factory);
    bool Unregister(const std::string& backend);
    bool Has(const std::string& backend) const;
    /// 已注册的名字（含内置四项）。
    std::vector<std::string> Backends() const;

    /// 按 `spec.kind` 对应的内置名字创建通道。
    Result<std::unique_ptr<MessageChannel>> Create(const ChannelSpec& spec) const;
    /// 按显式后端名字创建通道（自定义后端走这里）。
    Result<std::unique_ptr<MessageChannel>> Create(const std::string& backend,
                                                   const ChannelSpec& spec) const;

private:
    ChannelRegistry();
    mutable std::mutex m_mutex;
    std::vector<std::pair<std::string, ChannelFactory>> m_factories;
};

/** @brief 内置后端名字：`"pipe"` / `"socketpair"` / `"shm"` / `"mq"`。 */
const char* ToString(ChannelKind kind) noexcept;

}  // namespace go2cpp::os
