/**
 * @file shm.hpp
 * @brief 共享内存（`shm_open` + `mmap`）与**无锁 SPSC 环形缓冲**。
 *
 * ## 模块定位
 *
 * - `SharedMemory`：命名共享内存对象的创建/打开/映射/解除映射/删除；
 * - `RingBuffer`：在**已经映射好的内存**上构造的单生产者单消费者无锁环，
 *   读写指针是原子的；既可以放在共享内存里跨进程用，也可以放在普通内存里
 *   做线程间队列；
 * - `SharedRingBuffer`：把上面两者组合成「命名 + 容量 + 环」的跨进程对象；
 * - `ShmChannel`：把共享内存环适配成 `MessageChannel`，供上层切换后端。
 *
 * ## 为什么是 SPSC 而不是 MPSC
 *
 * 多生产者需要 CAS 抢写位置 + 处理写者之间的顺序，复杂度和缓存行争用都会
 * 上一个量级；本模块的明确目标是「一个进程写、一个进程读」的大块/高频传输
 * （例如 MCP 把工具输出流送给另一个进程）。需要多写者时，在上层加一个
 * 消息队列（`MessageQueue`）或多开几条环。
 *
 * ## 内存序（必须理解后再改代码）
 *
 * 环的同步只依赖两个原子变量：`write_pos`（生产者写）与 `read_pos`（消费者写）。
 *
 * - 生产者写数据 → `write_pos.store(release)`：保证**数据写入**先于新位置
 *   对消费者可见；
 * - 消费者 `write_pos.load(acquire)` → 读数据：保证看到位置时一定能看到数据；
 * - 消费者读数据 → `read_pos.store(release)`；生产者 `read_pos.load(acquire)`
 *   之后复用空间：保证不会在消费者读完前覆盖。
 *
 * 计数器是**单调递增的 64 位无符号数**，下标用 `pos % capacity`。无符号回绕
 * （2^64 之后）仍然成立，因为 `write_pos - read_pos` 的差值在模 2^64 下正确。
 *
 * ## 覆盖策略（「读方追赶写方」）
 *
 * - 默认（`overwrite == false`）：环满时写方只写能放下的部分，返回实际写入
 *   字节数；不丢已排队数据；
 * - 覆盖模式（`overwrite == true`）：写方在空间不足时**推进 `read_pos`**，
 *   丢弃最旧的数据，为本次写入腾空间；如果本次写入大于整个容量，只保留最后
 *   `capacity` 字节。读方每次读取前检查 `write_pos - read_pos > capacity`，
 *   发现自己落后就跳到 `write_pos - capacity`（并累加 `dropped`），因此读方
 *   不会读到已经被覆盖两轮以上的陈旧位置。
 *
 * 覆盖模式是 **best-effort**：写方可能在读方 memcpy 到一半时覆盖同一段内存，
 * 读到的数据可能撕裂。需要完整性保证的调用方应在 payload 里自带长度/CRC
 * （`ShmChannel` 的 G2CF 帧格式就带 CRC，检测到校验失败会丢弃该帧）。
 *
 * ## 权限与生命周期
 *
 * - 名称必须以 `/` 开头（POSIX 要求），内部不再自动补 `/`，避免「看起来成功
 *   实际写到别的名字」；
 * - 创建者的 `mode` 直接传给 `shm_open`（受 umask 影响）；
 * - **`shm_unlink` 不会释放已映射的内存**，只删除名字；`SharedMemory` 析构
 *   只 `munmap+close`，是否 unlink 由调用方显式决定（`owner` 语义在
 *   `SharedRingBuffer`/`ShmChannel` 里实现）。
 */
#pragma once

#include "go2cpp/os/channel.hpp"

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace go2cpp::os {

// ---------------------------------------------------------------------------
// SharedMemory
// ---------------------------------------------------------------------------

/** @brief 共享内存的访问模式。 */
enum class ShmAccess : std::uint8_t {
    kReadOnly,   ///< `O_RDONLY` 映射（`PROT_READ`）
    kReadWrite,  ///< `O_RDWR` 映射（`PROT_READ|PROT_WRITE`）
};

/**
 * @brief 命名共享内存的 RAII 句柄。
 *
 * @details 析构：`munmap` + `close`，**不** `shm_unlink`（名字的生命周期由
 *          创建者显式管理，避免「最后一个使用者退出时把别人正在打开的名字删了」）。
 *          需要删除名字请调用静态 `Unlink` 或 `SharedRingBuffer::Unlink`。
 *
 *          移动后源对象不再持有映射；不可复制。
 */
class SharedMemory final {
public:
    SharedMemory() noexcept = default;
    ~SharedMemory();

    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;
    SharedMemory(SharedMemory&& other) noexcept;
    SharedMemory& operator=(SharedMemory&& other) noexcept;

    /**
     * @brief 创建（`O_CREAT|O_EXCL`）并映射一块共享内存。
     * @param name 以 `/` 开头的名字。
     * @param size 字节数；必须 > 0。
     * @param out 成功时写入句柄。
     * @param mode 权限位（受 umask 影响），默认 0600。
     * @return 失败：`kInvalidArgument`（名字/大小非法）、`kShmFailed`（EEXIST 等）。
     */
    static Result<void> Create(const std::string& name, std::size_t size,
                               SharedMemory* out, mode_t mode = 0600);

    /**
     * @brief 打开并映射已存在的共享内存。
     * @param expected_size 非 0 时校验实际大小，不一致返回 `kShmFailed`。
     */
    static Result<void> Open(const std::string& name, SharedMemory* out,
                             ShmAccess access = ShmAccess::kReadWrite,
                             std::size_t expected_size = 0);

    /// 删除名字（已映射的句柄不受影响）；不存在返回 `kShmFailed` + ENOENT。
    static Result<void> Unlink(const std::string& name);

    /// 调整映射大小（仅创建者；`ftruncate` + 重新映射）。
    Result<void> Resize(std::size_t new_size);

    void* data() noexcept { return m_data; }
    const void* data() const noexcept { return m_data; }
    std::size_t size() const noexcept { return m_size; }
    const std::string& name() const noexcept { return m_name; }
    int fd() const noexcept { return m_fd; }
    bool valid() const noexcept { return m_data != nullptr && m_size > 0; }

    /// 解除映射并关闭 fd（幂等）。
    void Close() noexcept;

private:
    void* m_data{nullptr};
    std::size_t m_size{0};
    std::string m_name;
    int m_fd{-1};
    bool m_writable{true};
};

// ---------------------------------------------------------------------------
// RingBuffer（SPSC 无锁）
// ---------------------------------------------------------------------------

/**
 * @brief 单生产者单消费者无锁环形缓冲。
 *
 * @details 对象本身只持有指向外部内存的指针；header 与数据区都在那块内存里，
 *          因此可以放在 `mmap` 的共享内存上跨进程使用。`Init` 负责写 header，
 *          `Attach` 负责校验 header（magic/capacity/region 大小）。
 *
 *          布局（所有字段按 64 字节对齐，减少伪共享）：
 *          ```
 *          [ RingHeader: magic, capacity, flags, write_pos, read_pos, dropped ][ data ]
 *          ```
 *
 * @note 一个环只允许一个线程/进程写、一个线程/进程读；`Write`/`Read` 不内部
 *       加锁。`Readable`/`Writable` 是瞬时快照，只用于流控决策。
 */
class RingBuffer final {
public:
    RingBuffer() noexcept = default;

    /// 在 `region` 上初始化 header；`region_bytes` 至少为 `HeaderSize + capacity`。
    Result<void> Init(void* region, std::size_t region_bytes, bool overwrite);

    /// 连接到已初始化的环（校验 magic 与容量）。
    Result<void> Attach(void* region, std::size_t region_bytes);

    /// 写（可写多少写多少）；返回实际写入字节数。
    std::uint64_t Write(const void* data, std::uint64_t size) noexcept;
    /// 读（有多少读多少）；返回实际读出字节数。
    std::uint64_t Read(void* out, std::uint64_t size) noexcept;

    std::uint64_t Readable() const noexcept;
    std::uint64_t Writable() const noexcept;
    std::uint64_t capacity() const noexcept { return m_capacity; }
    bool overwrite() const noexcept { return m_overwrite; }
    std::uint64_t dropped_bytes() const noexcept;
    bool valid() const noexcept { return m_header != nullptr; }

    /// header + 数据区所需的最小字节数。
    static std::size_t RequiredBytes(std::size_t capacity) noexcept;

private:
    struct Header;  ///< 定义在 shm.cpp，避免公开原子布局

    Header* m_header{nullptr};
    std::byte* m_data{nullptr};
    std::uint64_t m_capacity{0};
    bool m_overwrite{false};
};

// ---------------------------------------------------------------------------
// SharedRingBuffer（跨进程组合）
// ---------------------------------------------------------------------------

/**
 * @brief 命名共享内存 + 已初始化的 `RingBuffer`。
 *
 * @details `Create` 创建名字、映射、初始化环；`Open` 打开名字并 `Attach`。
 *          `owner == true` 的对象析构时可选 unlink（默认不自动，调用方显式
 *          `Unlink`）；测试/示例通常由创建者负责删除名字。
 */
class SharedRingBuffer final {
public:
    SharedRingBuffer() = default;
    ~SharedRingBuffer() = default;

    SharedRingBuffer(const SharedRingBuffer&) = delete;
    SharedRingBuffer& operator=(const SharedRingBuffer&) = delete;
    SharedRingBuffer(SharedRingBuffer&&) noexcept = default;
    SharedRingBuffer& operator=(SharedRingBuffer&&) noexcept = default;

    /**
     * @brief 创建共享内存环。
     * @param capacity 数据区字节数（会向上对齐到 64）。
     */
    static Result<void> Create(const std::string& name, std::size_t capacity,
                               SharedRingBuffer* out, bool overwrite = false,
                               mode_t mode = 0600);
    /// 打开并校验已存在的环。
    static Result<void> Open(const std::string& name, SharedRingBuffer* out);

    RingBuffer& ring() noexcept { return m_ring; }
    const RingBuffer& ring() const noexcept { return m_ring; }
    SharedMemory& memory() noexcept { return m_memory; }
    const SharedMemory& memory() const noexcept { return m_memory; }
    const std::string& name() const noexcept { return m_name; }
    bool valid() const noexcept { return m_memory.valid() && m_ring.valid(); }

    /// 删除名字；不解除本对象的映射。
    Result<void> Unlink();

private:
    SharedMemory m_memory;
    RingBuffer m_ring;
    std::string m_name;
};

// ---------------------------------------------------------------------------
// ShmChannel（MessageChannel 后端）
// ---------------------------------------------------------------------------

/**
 * @brief 用共享内存环实现的 `MessageChannel`。
 *
 * @details 适合大块/高频单向数据；**没有内核事件通知**，读方采用「短轮询 +
 *          超时」模型（每次空读后 `nanosleep` 几十微秒）。因此不要把它放在
 *          低延迟控制路径上：控制信令请用 pipe/mq，数据面用 shm。
 *
 *          两个进程各自 `Create/Open` 同一个名字即可通信：一端写、一端读。
 *          双向通信开两条环（不同名字）。
 */
class ShmChannel final : public MessageChannel {
public:
    /**
     * @brief 创建或打开共享内存通道。
     * @param name 共享内存名字。
     * @param capacity 环容量（创建时有效）。
     * @param owner true 表示创建者；名字已存在时失败，不会静默复用。
     * @param overwrite 覆盖模式（见文件头）。
     */
    static Result<std::unique_ptr<ShmChannel>> Create(const std::string& name,
                                                      std::size_t capacity, bool owner,
                                                      bool overwrite = false,
                                                      ChannelOptions options = {});
    /// 打开已存在的共享内存通道。
    static Result<std::unique_ptr<ShmChannel>> Open(const std::string& name,
                                                    ChannelOptions options = {});

    ~ShmChannel() override;

    Result<void> Write(ByteSpan data, std::chrono::milliseconds timeout) override;
    void Close() noexcept override;
    bool closed() const noexcept override;
    const ChannelStats& stats() const noexcept override;

    const std::string& name() const noexcept;

protected:
    Result<std::size_t> Pull(std::chrono::milliseconds timeout) override;

private:
    explicit ShmChannel(ChannelOptions options);

    SharedRingBuffer m_buffer;
    bool m_closed{false};
    /// 空读后的退避（微秒）；避免空转把一个 CPU 核吃满。
    static constexpr long kIdleSleepNanoseconds = 50 * 1000;
};

}  // namespace go2cpp::os
