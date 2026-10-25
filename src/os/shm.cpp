/**
 * @file shm.cpp
 * @brief 共享内存、SPSC 环形缓冲、跨进程环与 `ShmChannel` 的实现。
 */
#include "go2cpp/os/shm.hpp"

#include "raw_io.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace go2cpp::os {

namespace {

constexpr std::uint64_t kRingMagic = 0x31474E49525F4732ULL;  // "2G_RING1"
constexpr std::uint64_t kRingOverwriteFlag = 1ULL;
constexpr std::uint64_t kAlignment = 64;

bool ValidSharedName(const std::string& name, std::string* reason) {
    if (name.size() < 2 || name[0] != '/') {
        if (reason) {
            *reason = "共享内存/队列名字必须以 '/' 开头且不能只有 '/'";
        }
        return false;
    }
    if (name.find('/', 1) != std::string::npos) {
        if (reason) {
            *reason = "名字中只能有一个 '/'（POSIX 要求）";
        }
        return false;
    }
    return true;
}

}  // namespace

// ===========================================================================
// SharedMemory
// ===========================================================================

SharedMemory::~SharedMemory() { Close(); }

SharedMemory::SharedMemory(SharedMemory&& other) noexcept
    : m_data(other.m_data),
      m_size(other.m_size),
      m_name(std::move(other.m_name)),
      m_fd(other.m_fd),
      m_writable(other.m_writable) {
    other.m_data = nullptr;
    other.m_size = 0;
    other.m_fd = -1;
    other.m_name.clear();
    other.m_writable = true;
}

SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept {
    if (this != &other) {
        Close();
        m_data = other.m_data;
        m_size = other.m_size;
        m_name = std::move(other.m_name);
        m_fd = other.m_fd;
        m_writable = other.m_writable;
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_fd = -1;
        other.m_name.clear();
        other.m_writable = true;
    }
    return *this;
}

Result<void> SharedMemory::Create(const std::string& name, std::size_t size,
                                  SharedMemory* out, mode_t mode) {
    if (out == nullptr) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "输出指针为空",
                         "SharedMemory::Create");
    }
    std::string reason;
    if (!ValidSharedName(name, &reason)) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, reason,
                         "SharedMemory::Create");
    }
    if (size == 0) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "大小必须 > 0",
                         "SharedMemory::Create");
    }
    const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, mode);
    if (fd < 0) {
        return MakeErrnoError(ProcErrorCode::kShmFailed, errno,
                              "shm_open(" + name + ")");
    }
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        const int error = errno;
        (void)detail::RawClose(fd);
        (void)::shm_unlink(name.c_str());
        return MakeErrnoError(ProcErrorCode::kShmFailed, error,
                              "ftruncate(" + name + ")");
    }
    void* mapped = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        const int error = errno;
        (void)detail::RawClose(fd);
        (void)::shm_unlink(name.c_str());
        return MakeErrnoError(ProcErrorCode::kShmFailed, error, "mmap(" + name + ")");
    }
    SharedMemory memory;
    memory.m_data = mapped;
    memory.m_size = size;
    memory.m_name = name;
    memory.m_fd = fd;
    memory.m_writable = true;
    *out = std::move(memory);
    return OkResult();
}

Result<void> SharedMemory::Open(const std::string& name, SharedMemory* out,
                                ShmAccess access, std::size_t expected_size) {
    if (out == nullptr) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "输出指针为空",
                         "SharedMemory::Open");
    }
    std::string reason;
    if (!ValidSharedName(name, &reason)) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, reason,
                         "SharedMemory::Open");
    }
    const bool writable = access == ShmAccess::kReadWrite;
    const int fd = ::shm_open(name.c_str(), writable ? O_RDWR : O_RDONLY, 0);
    if (fd < 0) {
        return MakeErrnoError(ProcErrorCode::kShmFailed, errno,
                              "shm_open(" + name + ")");
    }
    struct ::stat info {};
    if (::fstat(fd, &info) != 0) {
        const int error = errno;
        (void)detail::RawClose(fd);
        return MakeErrnoError(ProcErrorCode::kShmFailed, error, "fstat(" + name + ")");
    }
    const std::size_t size = static_cast<std::size_t>(info.st_size);
    if (size == 0) {
        (void)detail::RawClose(fd);
        return MakeError(ProcErrorCode::kShmFailed, EINVAL, "共享内存大小为 0",
                         "SharedMemory::Open");
    }
    if (expected_size != 0 && expected_size > size) {
        (void)detail::RawClose(fd);
        return MakeError(ProcErrorCode::kShmFailed, EINVAL,
                         "共享内存小于期望大小", "SharedMemory::Open");
    }
    void* mapped = ::mmap(nullptr, size, writable ? (PROT_READ | PROT_WRITE) : PROT_READ,
                          MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        const int error = errno;
        (void)detail::RawClose(fd);
        return MakeErrnoError(ProcErrorCode::kShmFailed, error, "mmap(" + name + ")");
    }
    SharedMemory memory;
    memory.m_data = mapped;
    memory.m_size = size;
    memory.m_name = name;
    memory.m_fd = fd;
    memory.m_writable = writable;
    *out = std::move(memory);
    return OkResult();
}

Result<void> SharedMemory::Unlink(const std::string& name) {
    std::string reason;
    if (!ValidSharedName(name, &reason)) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, reason,
                         "SharedMemory::Unlink");
    }
    if (::shm_unlink(name.c_str()) != 0) {
        return MakeErrnoError(ProcErrorCode::kShmFailed, errno,
                              "shm_unlink(" + name + ")");
    }
    return OkResult();
}

Result<void> SharedMemory::Resize(std::size_t new_size) {
    if (!valid() || !m_writable) {
        return MakeError(ProcErrorCode::kNotRunning, EBADF, "共享内存不可写",
                         "SharedMemory::Resize");
    }
    if (new_size == 0) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "大小必须 > 0",
                         "SharedMemory::Resize");
    }
    if (::ftruncate(m_fd, static_cast<off_t>(new_size)) != 0) {
        return MakeErrnoError(ProcErrorCode::kShmFailed, errno, "ftruncate");
    }
    void* remapped = ::mremap(m_data, m_size, new_size, MREMAP_MAYMOVE);
    if (remapped == MAP_FAILED) {
        return MakeErrnoError(ProcErrorCode::kShmFailed, errno, "mremap");
    }
    m_data = remapped;
    m_size = new_size;
    return OkResult();
}

void SharedMemory::Close() noexcept {
    if (m_data != nullptr) {
        (void)::munmap(m_data, m_size);
        m_data = nullptr;
    }
    if (m_fd >= 0) {
        (void)detail::RawClose(m_fd);
        m_fd = -1;
    }
    m_size = 0;
}

// ===========================================================================
// RingBuffer（SPSC 无锁）
// ===========================================================================

struct alignas(64) RingBuffer::Header final {
    std::uint64_t magic{0};
    std::uint64_t capacity{0};
    std::uint64_t flags{0};
    std::atomic<std::uint64_t> write_pos{0};
    std::atomic<std::uint64_t> read_pos{0};
    std::atomic<std::uint64_t> dropped{0};
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "跨进程环要求 64 位原子无锁");

std::size_t RingBuffer::RequiredBytes(std::size_t capacity) noexcept {
    if (capacity == 0) {
        capacity = kAlignment;
    }
    const std::size_t aligned = (capacity + kAlignment - 1) & ~(kAlignment - 1);
    return sizeof(Header) + aligned;
}

Result<void> RingBuffer::Init(void* region, std::size_t region_bytes, bool overwrite) {
    static_assert(sizeof(Header) == 64, "环 header 必须占一个缓存行");
    if (region == nullptr) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "内存区域为空",
                         "RingBuffer::Init");
    }
    if (region_bytes <= sizeof(Header) + kAlignment) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "内存区域太小",
                         "RingBuffer::Init");
    }
    const std::size_t capacity = (region_bytes - sizeof(Header)) & ~(kAlignment - 1);
    auto* header = new (region) Header();
    header->magic = kRingMagic;
    header->capacity = capacity;
    header->flags = overwrite ? kRingOverwriteFlag : 0;
    header->write_pos.store(0, std::memory_order_relaxed);
    header->read_pos.store(0, std::memory_order_relaxed);
    header->dropped.store(0, std::memory_order_relaxed);
    m_header = header;
    m_data = static_cast<std::byte*>(region) + sizeof(Header);
    m_capacity = capacity;
    m_overwrite = overwrite;
    return OkResult();
}

Result<void> RingBuffer::Attach(void* region, std::size_t region_bytes) {
    if (region == nullptr || region_bytes < sizeof(Header)) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "内存区域无效",
                         "RingBuffer::Attach");
    }
    auto* header = static_cast<Header*>(region);
    if (header->magic != kRingMagic) {
        return MakeError(ProcErrorCode::kShmFailed, EPROTO, "环 header 魔数不匹配",
                         "RingBuffer::Attach");
    }
    if (header->capacity == 0 ||
        header->capacity > region_bytes - sizeof(Header)) {
        return MakeError(ProcErrorCode::kShmFailed, EPROTO, "环容量与映射大小不一致",
                         "RingBuffer::Attach");
    }
    m_header = header;
    m_data = static_cast<std::byte*>(region) + sizeof(Header);
    m_capacity = header->capacity;
    m_overwrite = (header->flags & kRingOverwriteFlag) != 0;
    return OkResult();
}

std::uint64_t RingBuffer::Write(const void* data, std::uint64_t size) noexcept {
    if (m_header == nullptr || data == nullptr || size == 0) {
        return 0;
    }
    const auto* source = static_cast<const std::byte*>(data);
    // 生产者独占 write_pos：写数据前用 relaxed 读自己的位置即可。
    std::uint64_t write = m_header->write_pos.load(std::memory_order_relaxed);
    // 消费者用 release 发布 read_pos；这里 acquire 读，确保看到「空间已释放」。
    std::uint64_t read = m_header->read_pos.load(std::memory_order_acquire);
    std::uint64_t used = write - read;
    if (used > m_capacity) {
        // 覆盖模式下消费者落后超过一圈：把读指针推进到有效窗口起点。
        read = write - m_capacity;
        m_header->read_pos.store(read, std::memory_order_release);
        used = m_capacity;
    }
    if (size > m_capacity) {
        if (!m_overwrite) {
            const std::uint64_t free = m_capacity - used;
            size = std::min<std::uint64_t>(size, free);
        } else {
            // 单次写入大于整个容量：只保留最后 capacity 字节。
            source += size - m_capacity;
            size = m_capacity;
            read = write;
            m_header->read_pos.store(read, std::memory_order_release);
            used = 0;
        }
    }
    std::uint64_t free = m_capacity - used;
    if (m_overwrite && size > free) {
        const std::uint64_t drop = size - free;
        // 与消费者竞争推进 read_pos：用 CAS 保证指针单调、不回退。
        std::uint64_t expected = m_header->read_pos.load(std::memory_order_acquire);
        while (!m_header->read_pos.compare_exchange_weak(
            expected, expected + drop, std::memory_order_release,
            std::memory_order_acquire)) {
        }
        m_header->dropped.fetch_add(drop, std::memory_order_relaxed);
        free = size;
    }
    const std::uint64_t count = std::min<std::uint64_t>(size, free);
    if (count == 0) {
        return 0;
    }
    const std::uint64_t start = write % m_capacity;
    const std::uint64_t first = std::min<std::uint64_t>(count, m_capacity - start);
    std::memcpy(m_data + start, source, static_cast<std::size_t>(first));
    if (count > first) {
        std::memcpy(m_data, source + first, static_cast<std::size_t>(count - first));
    }
    // release：保证数据写入先于新位置对消费者可见。
    m_header->write_pos.store(write + count, std::memory_order_release);
    return count;
}

std::uint64_t RingBuffer::Read(void* out, std::uint64_t size) noexcept {
    if (m_header == nullptr || out == nullptr || size == 0) {
        return 0;
    }
    auto* target = static_cast<std::byte*>(out);
    // 消费者独占 read_pos：relaxed 读自己的位置。
    std::uint64_t read = m_header->read_pos.load(std::memory_order_relaxed);
    // acquire：看到 write_pos 时一定能看到生产者的数据。
    const std::uint64_t write = m_header->write_pos.load(std::memory_order_acquire);
    if (write - read > m_capacity) {
        // 读方追赶写方：跳过已经被覆盖的旧数据。
        const std::uint64_t skipped = (write - read) - m_capacity;
        read = write - m_capacity;
        m_header->read_pos.store(read, std::memory_order_release);
        m_header->dropped.fetch_add(skipped, std::memory_order_relaxed);
    }
    const std::uint64_t available = write - read;
    const std::uint64_t count = std::min<std::uint64_t>(size, available);
    if (count == 0) {
        return 0;
    }
    const std::uint64_t start = read % m_capacity;
    const std::uint64_t first = std::min<std::uint64_t>(count, m_capacity - start);
    std::memcpy(target, m_data + start, static_cast<std::size_t>(first));
    if (count > first) {
        std::memcpy(target + first, m_data, static_cast<std::size_t>(count - first));
    }
    // CAS 推进读指针：若写方在读取期间为覆盖而推进了 read_pos，CAS 失败，
    // 本次数据可能已撕裂 —— 覆盖模式是 best-effort，上层用 CRC 丢弃坏帧。
    std::uint64_t expected = read;
    while (!m_header->read_pos.compare_exchange_weak(
        expected, read + count, std::memory_order_release,
        std::memory_order_acquire)) {
        if (expected >= read + count) {
            return 0;
        }
        break;
    }
    return count;
}

std::uint64_t RingBuffer::Readable() const noexcept {
    if (m_header == nullptr) {
        return 0;
    }
    const std::uint64_t write = m_header->write_pos.load(std::memory_order_acquire);
    const std::uint64_t read = m_header->read_pos.load(std::memory_order_acquire);
    const std::uint64_t used = write - read;
    return used > m_capacity ? m_capacity : used;
}

std::uint64_t RingBuffer::Writable() const noexcept {
    if (m_header == nullptr) {
        return 0;
    }
    if (m_overwrite) {
        return m_capacity;
    }
    const std::uint64_t write = m_header->write_pos.load(std::memory_order_acquire);
    const std::uint64_t read = m_header->read_pos.load(std::memory_order_acquire);
    const std::uint64_t used = write - read;
    return used >= m_capacity ? 0 : m_capacity - used;
}

std::uint64_t RingBuffer::dropped_bytes() const noexcept {
    return m_header == nullptr ? 0 : m_header->dropped.load(std::memory_order_relaxed);
}

// ===========================================================================
// SharedRingBuffer
// ===========================================================================

Result<void> SharedRingBuffer::Create(const std::string& name, std::size_t capacity,
                                      SharedRingBuffer* out, bool overwrite,
                                      mode_t mode) {
    if (out == nullptr) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "输出指针为空",
                         "SharedRingBuffer::Create");
    }
    const std::size_t bytes = RingBuffer::RequiredBytes(capacity);
    SharedMemory memory;
    auto created = SharedMemory::Create(name, bytes, &memory, mode);
    if (!created.ok()) {
        return created.error();
    }
    SharedRingBuffer buffer;
    auto initialized = buffer.m_ring.Init(memory.data(), memory.size(), overwrite);
    if (!initialized.ok()) {
        (void)SharedMemory::Unlink(name);
        return initialized.error();
    }
    buffer.m_memory = std::move(memory);
    buffer.m_name = name;
    *out = std::move(buffer);
    return OkResult();
}

Result<void> SharedRingBuffer::Open(const std::string& name,
                                    SharedRingBuffer* out) {
    if (out == nullptr) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "输出指针为空",
                         "SharedRingBuffer::Open");
    }
    SharedMemory memory;
    auto opened = SharedMemory::Open(name, &memory);
    if (!opened.ok()) {
        return opened.error();
    }
    SharedRingBuffer buffer;
    auto attached = buffer.m_ring.Attach(memory.data(), memory.size());
    if (!attached.ok()) {
        return attached.error();
    }
    buffer.m_memory = std::move(memory);
    buffer.m_name = name;
    *out = std::move(buffer);
    return OkResult();
}

Result<void> SharedRingBuffer::Unlink() {
    if (m_name.empty()) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "名字为空",
                         "SharedRingBuffer::Unlink");
    }
    return SharedMemory::Unlink(m_name);
}

// ===========================================================================
// ShmChannel
// ===========================================================================

ShmChannel::ShmChannel(ChannelOptions options) : MessageChannel(options) {}

ShmChannel::~ShmChannel() { Close(); }

Result<std::unique_ptr<ShmChannel>> ShmChannel::Create(const std::string& name,
                                                       std::size_t capacity, bool owner,
                                                       bool overwrite,
                                                       ChannelOptions options) {
    auto channel = std::unique_ptr<ShmChannel>(new ShmChannel(options));
    Result<void> opened = owner
                              ? SharedRingBuffer::Create(name, capacity,
                                                         &channel->m_buffer, overwrite)
                              : SharedRingBuffer::Open(name, &channel->m_buffer);
    if (!opened.ok()) {
        return opened.error();
    }
    return channel;
}

Result<std::unique_ptr<ShmChannel>> ShmChannel::Open(const std::string& name,
                                                     ChannelOptions options) {
    auto channel = std::unique_ptr<ShmChannel>(new ShmChannel(options));
    auto opened = SharedRingBuffer::Open(name, &channel->m_buffer);
    if (!opened.ok()) {
        return opened.error();
    }
    return channel;
}

Result<void> ShmChannel::Write(ByteSpan data, std::chrono::milliseconds timeout) {
    if (m_closed || !m_buffer.valid()) {
        return MakeError(ProcErrorCode::kClosed, EBADF, "共享内存通道已关闭",
                         "ShmChannel::Write");
    }
    const auto start = std::chrono::steady_clock::now();
    std::size_t offset = 0;
    while (offset < data.size) {
        const std::uint64_t written = m_buffer.ring().Write(
            data.data + offset, static_cast<std::uint64_t>(data.size - offset));
        offset += static_cast<std::size_t>(written);
        if (offset >= data.size) {
            break;
        }
        if (timeout.count() >= 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (elapsed >= timeout) {
                NoteTimeout();
                return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT,
                                 "共享内存环写满，等待超时", "ShmChannel::Write");
            }
        }
        (void)detail::RawNanosleep(0, kIdleSleepNanoseconds);
    }
    m_stats.bytes_written += data.size;
    return OkResult();
}

Result<std::size_t> ShmChannel::Pull(std::chrono::milliseconds timeout) {
    if (m_closed || !m_buffer.valid()) {
        return MakeError(ProcErrorCode::kClosed, EBADF, "共享内存通道已关闭",
                         "ShmChannel::Pull");
    }
    const auto start = std::chrono::steady_clock::now();
    std::byte chunk[65536];
    for (;;) {
        const std::uint64_t count = m_buffer.ring().Read(
            chunk, std::min<std::uint64_t>(sizeof(chunk), m_options.read_chunk_bytes));
        if (count > 0) {
            AppendRx(chunk, static_cast<std::size_t>(count));
            m_stats.bytes_read += count;
            return static_cast<std::size_t>(count);
        }
        if (timeout.count() >= 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (elapsed >= timeout) {
                NoteTimeout();
                return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT,
                                 "共享内存环暂无数据", "ShmChannel::Pull");
            }
        }
        (void)detail::RawNanosleep(0, kIdleSleepNanoseconds);
    }
}

void ShmChannel::Close() noexcept {
    if (m_closed) {
        return;
    }
    m_closed = true;
    m_buffer.memory().Close();
}

bool ShmChannel::closed() const noexcept { return m_closed; }
const ChannelStats& ShmChannel::stats() const noexcept { return m_stats; }
const std::string& ShmChannel::name() const noexcept { return m_buffer.name(); }

}  // namespace go2cpp::os
