/**
 * @file examples/file_transfer/file_transfer_demo.cpp
 * @brief 文件传输示例：`ResourceTransfer`（512 KiB 分块 + SHA-256 + 断点续传）。
 *
 * ## 演示什么
 *
 * 直接使用 `include/go2cpp/app/resource.hpp` 的三个部件，不经过连接层：
 *
 * ```
 * 源文件 ──> FileChunkSource ──> ResourceProducer ──> 帧编码 ──> 通道
 *                                                              │
 *      目标文件 <── FileResourceSink <── ResourceReceiver <── 帧解码 <──┘
 * ```
 *
 * - **发送端**：`BuildResourceMeta` 扫描全文件（SHA-256/CRC-32/块数），
 *   `ResourceProducer` 逐块产出 DATA 帧，`AppendEncodedFrame` 编码成 9B 头 +
 *   载荷，写入通道；
 * - **接收端**：`FrameDecoder` 处理粘包/半包，`ResourceReceiver` 按
 *   `chunk_index + offset` 重组、校验块 CRC、增量算全资源 SHA-256，最后
 *   `Finish()` 校验；`FileResourceSink` 用 `pwrite` 落盘；
 * - **断点续传**：第一段只发到第 N 块就关闭通道（模拟断网），接收端
 *   `completed_chunks()` 给出连续完成块数；第二段用
 *   `ResourceProducer(meta, source, N)` + `ResourceReceiver(meta, sink, N)`
 *   从第 N 块继续，目标文件不截断（`FileResourceSink(path, false)`）。
 *
 * ## 两种传输层
 *
 * - `--transport memory`：进程内**有界**字节管道（64 MiB，模拟真背压）；
 * - `--transport tcp`：127.0.0.1 回环 TCP，真实 socket + 半包/粘包。
 *
 * 两种传输都走同一套帧编码/解码，因此线格式一致。
 *
 * ## 验证（每次运行都做）
 *
 * 1. 目标文件大小 == 源文件大小；
 * 2. 目标文件 SHA-256 == `meta.sha256_hex`（源文件哈希）；
 * 3. **逐字节比较**（4 MiB 缓冲，`pread` + `memcmp`）；
 * 4. 打印吞吐（MiB/s，接收端与发送端各一份）。
 *
 * ## 已知行为（必读）
 *
 * 续传时 `ResourceReceiver` 的 `m_partial=true`：它只对**本次段**的数据做
 * 增量哈希，`Finish()` 会**跳过**整资源 SHA-256 比较（否则会必然失败）。
 * 所以示例在续传后自己重新计算目标文件 SHA-256 并逐字节比较——这也是真实
 * 客户端应做的：框架校验块级完整性，应用层负责整文件一致性。
 *
 * ## 用法
 *
 * ```
 * build_and_run.sh                                     # 1 GiB（默认）
 * build_and_run.sh --transport tcp                     # 1 GiB over loopback TCP
 * build_and_run.sh --size 64M --interrupt-after 40     # 64 MiB，第 40 块后中断续传
 * build_and_run.sh --suite                             # 6 个边界尺寸全跑一遍
 * ```
 */
#include "go2cpp/app/frame.hpp"
#include "go2cpp/app/resource.hpp"

#include "go2cpp/hash/sha256.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using go2cpp::app::AppendEncodedFrame;
using go2cpp::app::ChunkSource;
using go2cpp::app::FileChunkSource;
using go2cpp::app::FileResourceSink;
using go2cpp::app::Frame;
using go2cpp::app::FrameDecoder;
using go2cpp::app::FrameStatus;
using go2cpp::app::FrameType;
using go2cpp::app::ResourceMeta;
using go2cpp::app::ResourceProducer;
using go2cpp::app::ResourceReceiver;
using go2cpp::app::ResourceStats;

/// 单帧载荷上限（与 `kDefaultMaxFramePayload` 一致）。
constexpr std::uint32_t kMaxPayload = 1024u * 1024u;
/// 示例使用的流 id（连接层会分配真实 id；这里固定一条流）。
constexpr std::uint32_t kStreamId = 1;
/// 有界内存管道的容量（模拟背压）。
constexpr std::size_t kPipeCapacity = 64u * 1024u * 1024u;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string FormatBytes(std::uint64_t bytes) {
    static const char* const kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    if (unit == 0) {
        std::snprintf(buffer, sizeof(buffer), "%.0f %s", value, kUnits[unit]);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, kUnits[unit]);
    }
    return buffer;
}

std::string FormatRate(double mib_per_second) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.1f MiB/s", mib_per_second);
    return buffer;
}

bool ParseSize(const std::string& text, std::uint64_t* out) {
    if (text.empty()) {
        return false;
    }
    std::size_t consumed = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(text, &consumed, 10);
    } catch (...) {
        return false;
    }
    std::string suffix = text.substr(consumed);
    for (char& ch : suffix) {
        ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
    }
    std::uint64_t multiplier = 1;
    if (suffix.empty() || suffix == "B") {
        multiplier = 1;
    } else if (suffix == "K" || suffix == "KB" || suffix == "KIB") {
        multiplier = 1024;
    } else if (suffix == "M" || suffix == "MB" || suffix == "MIB") {
        multiplier = 1024ull * 1024ull;
    } else if (suffix == "G" || suffix == "GB" || suffix == "GIB") {
        multiplier = 1024ull * 1024ull * 1024ull;
    } else {
        return false;
    }
    *out = static_cast<std::uint64_t>(value) * multiplier;
    return true;
}

bool ParseU32(const std::string& text, std::uint32_t* out) {
    try {
        const unsigned long value = std::stoul(text);
        *out = static_cast<std::uint32_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// 字节通道：内存管道 / TCP socket
// ---------------------------------------------------------------------------

/// 单向字节通道（发送端只写，接收端只读）。
class ByteChannel {
   public:
    virtual ~ByteChannel() = default;
    virtual bool WriteAll(const std::uint8_t* data, std::size_t size) = 0;
    /// @return 实际读到的字节数；0 表示 EOF。
    virtual std::size_t ReadSome(std::uint8_t* data, std::size_t size) = 0;
    /// 通知对端「不会再有数据」（发送 FIN / 关闭写端）。
    virtual void CloseWrite() {}
};

/**
 * @brief 有界字节管道：写满即阻塞，读空即等待，写端关闭后读端见 EOF。
 */
class MemoryPipe {
   public:
    explicit MemoryPipe(std::size_t capacity) : m_capacity(capacity) {}

    bool Push(std::vector<std::uint8_t> chunk) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_space_cv.wait(lock, [this, &chunk] {
            return m_write_closed || m_queued_bytes + chunk.size() <= m_capacity;
        });
        if (m_write_closed) {
            return false;
        }
        m_queued_bytes += chunk.size();
        m_queue.push_back(std::move(chunk));
        m_data_cv.notify_one();
        return true;
    }

    std::size_t Pop(std::uint8_t* out, std::size_t size) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_data_cv.wait(lock, [this] { return m_write_closed || !m_queue.empty(); });
        if (m_queue.empty()) {
            return 0;  // 写端已关闭且数据读完。
        }
        std::size_t total = 0;
        while (total < size && !m_queue.empty()) {
            const std::vector<std::uint8_t>& front = m_queue.front();
            const std::size_t available = front.size() - m_front_offset;
            const std::size_t take = std::min(available, size - total);
            std::memcpy(out + total, front.data() + m_front_offset, take);
            total += take;
            m_front_offset += take;
            m_queued_bytes -= take;
            if (m_front_offset == front.size()) {
                m_queue.pop_front();
                m_front_offset = 0;
            }
        }
        m_space_cv.notify_all();
        return total;
    }

    void CloseWrite() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_write_closed = true;
        m_data_cv.notify_all();
        m_space_cv.notify_all();
    }

   private:
    std::mutex m_mutex;                 ///< 保护队列。
    std::condition_variable m_data_cv;  ///< 有数据/写端关闭。
    std::condition_variable m_space_cv; ///< 有空间/写端关闭。
    std::deque<std::vector<std::uint8_t>> m_queue;  ///< 已写入的字节块。
    std::size_t m_capacity;      ///< 队列容量（字节）。
    std::size_t m_queued_bytes = 0;  ///< 当前排队字节数。
    std::size_t m_front_offset = 0;  ///< 队首块已消费偏移。
    bool m_write_closed = false;     ///< 写端是否关闭。
};

class MemoryWriter final : public ByteChannel {
   public:
    explicit MemoryWriter(MemoryPipe* pipe) : m_pipe(pipe) {}
    bool WriteAll(const std::uint8_t* data, std::size_t size) override {
        return m_pipe->Push(std::vector<std::uint8_t>(data, data + size));
    }
    std::size_t ReadSome(std::uint8_t*, std::size_t) override { return 0; }
    void CloseWrite() override { m_pipe->CloseWrite(); }

   private:
    MemoryPipe* m_pipe;  ///< 不拥有。
};

class MemoryReader final : public ByteChannel {
   public:
    explicit MemoryReader(MemoryPipe* pipe) : m_pipe(pipe) {}
    bool WriteAll(const std::uint8_t*, std::size_t) override { return false; }
    std::size_t ReadSome(std::uint8_t* data, std::size_t size) override {
        return m_pipe->Pop(data, size);
    }
    void CloseWrite() override {}

   private:
    MemoryPipe* m_pipe;  ///< 不拥有。
};

/// TCP 通道（示例里只用于 127.0.0.1 回环）。
class SocketChannel final : public ByteChannel {
   public:
    explicit SocketChannel(int fd) : m_fd(fd) {}
    ~SocketChannel() override { Close(); }

    SocketChannel(const SocketChannel&) = delete;
    SocketChannel& operator=(const SocketChannel&) = delete;

    bool WriteAll(const std::uint8_t* data, std::size_t size) override {
        std::size_t offset = 0;
        while (offset < size) {
            const ssize_t n = ::send(m_fd, data + offset, size - offset, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (n == 0) {
                return false;
            }
            offset += static_cast<std::size_t>(n);
        }
        return true;
    }

    std::size_t ReadSome(std::uint8_t* data, std::size_t size) override {
        while (true) {
            const ssize_t n = ::read(m_fd, data, size);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return 0;
            }
            return static_cast<std::size_t>(n);
        }
    }

    void CloseWrite() override {
        if (m_fd >= 0) {
            (void)::shutdown(m_fd, SHUT_WR);
        }
    }

    void Close() {
        if (m_fd >= 0) {
            (void)::close(m_fd);
            m_fd = -1;
        }
    }

   private:
    int m_fd = -1;  ///< socket fd。
};

int CreateLoopbackListener(std::uint16_t* port, std::string* error) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        *error = "socket() 失败";
        return -1;
    }
    const int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        *error = "bind(127.0.0.1:0) 失败";
        (void)::close(fd);
        return -1;
    }
    if (::listen(fd, 4) != 0) {
        *error = "listen() 失败";
        (void)::close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        *error = "getsockname() 失败";
        (void)::close(fd);
        return -1;
    }
    *port = ntohs(addr.sin_port);
    return fd;
}

int AcceptWithTimeout(int listener, int timeout_ms, std::string* error) {
    pollfd pfd{};
    pfd.fd = listener;
    pfd.events = POLLIN;
    const int ready = ::poll(&pfd, 1, timeout_ms);
    if (ready <= 0) {
        *error = ready == 0 ? "accept 超时" : "poll() 失败";
        return -1;
    }
    const int fd = ::accept(listener, nullptr, nullptr);
    if (fd < 0) {
        *error = "accept() 失败";
        return -1;
    }
    return fd;
}

int ConnectLoopback(std::uint16_t port, std::string* error) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        *error = "socket() 失败";
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        *error = "connect(127.0.0.1) 失败";
        (void)::close(fd);
        return -1;
    }
    return fd;
}

// ---------------------------------------------------------------------------
// 进度
// ---------------------------------------------------------------------------

/// 简单的进度打印：每 10% 或每 1 秒一行，带实时吞吐。
class Progress {
   public:
    Progress(std::string label, std::uint64_t total)
        : m_label(std::move(label)),
          m_total(total),
          m_started(NowSeconds()),
          m_last_print(m_started) {}

    void Add(std::uint64_t bytes) {
        m_done += bytes;
        const double now = NowSeconds();
        const double percent =
            m_total == 0 ? 100.0
                         : 100.0 * static_cast<double>(m_done) / static_cast<double>(m_total);
        if (percent >= m_next_percent || now - m_last_print >= 1.0 ||
            (m_done >= m_total && m_done != m_printed_done)) {
            Print(now, percent);
        }
    }

    void Finish() {
        if (m_done != m_printed_done) {
            const double now = NowSeconds();
            const double percent =
                m_total == 0
                    ? 100.0
                    : 100.0 * static_cast<double>(m_done) / static_cast<double>(m_total);
            Print(now, percent);
        }
    }

   private:
    void Print(double now, double percent) {
        const double elapsed = std::max(0.000001, now - m_started);
        const double rate = (static_cast<double>(m_done) / (1024.0 * 1024.0)) / elapsed;
        std::printf("  [%s] %s / %s (%5.1f%%) %s\n", m_label.c_str(),
                    FormatBytes(m_done).c_str(), FormatBytes(m_total).c_str(), percent,
                    FormatRate(rate).c_str());
        m_last_print = now;
        m_printed_done = m_done;
        while (m_next_percent <= percent && m_next_percent <= 100.0) {
            m_next_percent += 10.0;
        }
    }

    std::string m_label;      ///< 标签（发送/接收）。
    std::uint64_t m_total;    ///< 总字节数。
    std::uint64_t m_done = 0; ///< 已完成字节。
    std::uint64_t m_printed_done = 0;  ///< 上次打印时的字节数。
    double m_started = 0.0;   ///< 起始时刻。
    double m_last_print = 0.0;///< 上次打印时刻。
    double m_next_percent = 10.0;  ///< 下一个打印百分比。
};

// ---------------------------------------------------------------------------
// 收发
// ---------------------------------------------------------------------------

struct SendOutcome {
    bool ok = false;
    bool interrupted = false;
    std::string error;
    std::uint64_t bytes = 0;
    double seconds = 0.0;
    std::uint32_t data_frames = 0;
};

struct RecvOutcome {
    bool ok = false;
    std::string error;
    std::uint32_t completed_chunks = 0;
    std::uint32_t expected_chunks = 0;
    std::uint64_t bytes = 0;
    double seconds = 0.0;
    bool finished = false;
    bool verified = false;
    ResourceStats stats;
};

/// 发送：`ResourceProducer` → 编码 → 通道。
SendOutcome SendOnChannel(ByteChannel* channel, const ResourceMeta& meta,
                          std::shared_ptr<ChunkSource> source, std::uint32_t start_chunk,
                          std::uint32_t stop_after_chunks) {
    SendOutcome out;
    const double started = NowSeconds();
    ResourceProducer producer(meta, std::move(source), start_chunk);
    const std::uint64_t skip =
        static_cast<std::uint64_t>(start_chunk) * static_cast<std::uint64_t>(meta.chunk_size);
    Progress progress("发送", meta.size > skip ? meta.size - skip : 0);
    Frame frame;
    std::vector<std::uint8_t> wire;
    std::uint64_t last_produced = 0;
    std::uint32_t last_chunk_index = 0;
    std::uint32_t chunks_started = 0;
    bool have_chunk = false;
    while (!producer.done()) {
        if (!producer.Produce(kMaxPayload, &frame)) {
            out.error = "资源生产者提前结束（Produce 返回 false）";
            break;
        }
        frame.stream_id = kStreamId;
        wire.clear();
        if (!AppendEncodedFrame(frame, &wire)) {
            out.error = "帧编码失败";
            break;
        }
        if (!channel->WriteAll(wire.data(), wire.size())) {
            out.error = "写通道失败（对端可能已断开）";
            break;
        }
        if (frame.type == FrameType::kData) {
            ++out.data_frames;
            // 块可以跨多帧（大块）；按块头里的 chunk_index 统计「已开始的块」。
            if (frame.Size() >= go2cpp::app::kResourceChunkHeaderBytes) {
                const std::uint32_t index = go2cpp::app::GetU32(frame.Bytes());
                if (!have_chunk || index != last_chunk_index) {
                    have_chunk = true;
                    last_chunk_index = index;
                    ++chunks_started;
                }
            }
        }
        const std::uint64_t produced = producer.produced_bytes();
        progress.Add(produced - last_produced);
        last_produced = produced;
        if (stop_after_chunks != 0 && chunks_started >= stop_after_chunks) {
            out.interrupted = true;  // 模拟中断：不再发送剩余块。
            break;
        }
    }
    progress.Finish();
    out.bytes = producer.produced_bytes();
    out.seconds = NowSeconds() - started;
    out.ok = out.error.empty();
    return out;
}

/// 接收：通道 → 解码 → `ResourceReceiver` → 文件落点。
RecvOutcome ReceiveOnChannel(ByteChannel* channel, const ResourceMeta& meta,
                             const std::string& dst, std::uint32_t start_chunk,
                             bool truncate, bool require_complete) {
    RecvOutcome out;
    out.expected_chunks = meta.chunk_count;
    const double started = NowSeconds();
    ResourceReceiver receiver(meta, std::make_unique<FileResourceSink>(dst, truncate),
                              start_chunk);
    const std::uint64_t skip =
        static_cast<std::uint64_t>(start_chunk) * static_cast<std::uint64_t>(meta.chunk_size);
    Progress progress("接收", meta.size > skip ? meta.size - skip : 0);
    FrameDecoder decoder(kMaxPayload);
    std::vector<std::uint8_t> buffer(256u * 1024u);
    Frame frame;
    std::string error;
    while (true) {
        const std::size_t read = channel->ReadSome(buffer.data(), buffer.size());
        if (read == 0) {
            break;  // EOF：对端关闭写端。
        }
        decoder.Append(buffer.data(), read);
        bool failed = false;
        while (decoder.NextView(&frame) == FrameStatus::kOk) {
            if (frame.type == FrameType::kData) {
                if (!receiver.OnData(frame.Bytes(), frame.Size(), &error)) {
                    out.error = "接收块失败：" + error;
                    failed = true;
                    break;
                }
                progress.Add(frame.Size());
            }
        }
        if (failed) {
            break;
        }
    }
    if (out.error.empty() && require_complete) {
        if (!receiver.Finish(&error)) {
            out.error = "收尾校验失败：" + error;
        } else {
            out.finished = true;
            out.verified = receiver.stats().verified;
        }
    }
    progress.Finish();
    out.completed_chunks = receiver.completed_chunks();
    out.bytes = receiver.stats().bytes;
    out.stats = receiver.stats();
    out.seconds = NowSeconds() - started;
    if (out.error.empty() && require_complete) {
        out.ok = out.finished && out.completed_chunks == meta.chunk_count;
        if (!out.ok && out.error.empty()) {
            out.error = "块数不完整";
        }
    } else if (out.error.empty()) {
        // 中断段：不要求收满，只要没有协议/校验错误即可。
        out.ok = true;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 单段传输（内存 / TCP）
// ---------------------------------------------------------------------------

struct SegmentOutcome {
    bool ok = false;
    std::string error;
    SendOutcome send;
    RecvOutcome recv;
};

struct Options {
    std::string transport = "memory";
    std::uint64_t size = 1ull << 30;  ///< 默认 1 GiB。
    std::uint32_t chunk_size = 0;     ///< 0 = 默认 512 KiB。
    std::string src;
    std::string dst;
    std::uint32_t interrupt_after = 0;  ///< >0 表示在发完第 N 块后中断。
    bool suite = false;
    bool reuse = false;
};

SegmentOutcome RunSegment(const Options& opts, const ResourceMeta& meta,
                          const std::string& source_path, const std::string& dst,
                          std::uint32_t start_chunk, std::uint32_t stop_after_chunks,
                          bool truncate) {
    SegmentOutcome seg;
    auto source = std::make_shared<FileChunkSource>(source_path);
    std::string error;
    if (!source->Open(&error)) {
        seg.error = "打开源文件失败：" + error;
        return seg;
    }
    const bool require_complete = stop_after_chunks == 0;
    const std::uint32_t expected_final =
        require_complete ? meta.chunk_count : start_chunk + stop_after_chunks;

    if (opts.transport == "memory") {
        MemoryPipe pipe(kPipeCapacity);
        MemoryWriter writer(&pipe);
        MemoryReader reader(&pipe);
        std::thread receiver_thread([&] {
            seg.recv = ReceiveOnChannel(&reader, meta, dst, start_chunk, truncate,
                                        require_complete);
        });
        std::thread sender_thread([&] {
            seg.send = SendOnChannel(&writer, meta, source, start_chunk, stop_after_chunks);
            writer.CloseWrite();
        });
        sender_thread.join();
        receiver_thread.join();
    } else {
        std::uint16_t port = 0;
        const int listener = CreateLoopbackListener(&port, &error);
        if (listener < 0) {
            seg.error = "创建回环监听失败：" + error;
            return seg;
        }
        std::thread receiver_thread([&] {
            std::string accept_error;
            const int fd = AcceptWithTimeout(listener, 5000, &accept_error);
            if (fd < 0) {
                seg.recv.error = "接收端 accept 失败：" + accept_error;
                return;
            }
            SocketChannel channel(fd);
            seg.recv = ReceiveOnChannel(&channel, meta, dst, start_chunk, truncate,
                                        require_complete);
        });
        std::thread sender_thread([&] {
            std::string connect_error;
            const int fd = ConnectLoopback(port, &connect_error);
            if (fd < 0) {
                seg.send.error = "发送端连接失败：" + connect_error;
                return;
            }
            SocketChannel channel(fd);
            seg.send = SendOnChannel(&channel, meta, source, start_chunk, stop_after_chunks);
            channel.CloseWrite();
            // 示例简化：给接收端一点时间读完内核缓冲再 close，避免 RST 丢尾包。
            // 真实实现应由连接层用 FIN + 应用层关闭确认来保证。
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        });
        sender_thread.join();
        receiver_thread.join();
        (void)::close(listener);
    }

    seg.ok = seg.send.ok && seg.recv.ok &&
             seg.recv.completed_chunks == expected_final;
    if (!seg.ok && seg.error.empty()) {
        if (!seg.send.error.empty()) {
            seg.error = "发送端：" + seg.send.error;
        } else if (!seg.recv.error.empty()) {
            seg.error = "接收端：" + seg.recv.error;
        } else {
            seg.error = "块数不符：收到 " + std::to_string(seg.recv.completed_chunks) + "/" +
                        std::to_string(expected_final);
        }
    }
    return seg;
}

// ---------------------------------------------------------------------------
// 文件工具
// ---------------------------------------------------------------------------

bool EnsureDirectory(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

bool GeneratePatternFile(const std::string& path, std::uint64_t size, std::string* error) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        *error = "无法创建源文件：" + path;
        return false;
    }
    std::vector<std::uint8_t> buffer(1u * 1024u * 1024u);
    std::uint64_t state = 0x9e3779b97f4a7c15ull;
    std::uint64_t remaining = size;
    while (remaining > 0) {
        const std::size_t take =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        for (std::size_t i = 0; i < take; i += 8) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            const std::size_t n = std::min<std::size_t>(8, take - i);
            for (std::size_t j = 0; j < n; ++j) {
                buffer[i + j] = static_cast<std::uint8_t>(state >> (8 * j));
            }
        }
        std::size_t written = 0;
        while (written < take) {
            const ssize_t n = ::write(fd, buffer.data() + written, take - written);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                (void)::close(fd);
                *error = "写源文件失败";
                return false;
            }
            written += static_cast<std::size_t>(n);
        }
        remaining -= take;
    }
    (void)::close(fd);
    return true;
}

std::uint64_t FileSize(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return static_cast<std::uint64_t>(-1);
    }
    return static_cast<std::uint64_t>(st.st_size);
}

std::string Sha256File(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return {};
    }
    go2cpp::hash::Sha256 hasher;
    std::vector<std::uint8_t> buffer(1u * 1024u * 1024u);
    while (true) {
        const ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        hasher.Write(buffer.data(), static_cast<std::size_t>(n));
    }
    (void)::close(fd);
    const std::vector<std::uint8_t> digest = hasher.Sum();
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        hex.push_back(kHex[byte >> 4]);
        hex.push_back(kHex[byte & 0x0f]);
    }
    return hex;
}

/// 逐字节比较（4 MiB 缓冲，`pread`）。
bool CompareFiles(const std::string& left, const std::string& right, std::string* error) {
    const int fd_left = ::open(left.c_str(), O_RDONLY);
    const int fd_right = ::open(right.c_str(), O_RDONLY);
    if (fd_left < 0 || fd_right < 0) {
        if (fd_left >= 0) {
            (void)::close(fd_left);
        }
        if (fd_right >= 0) {
            (void)::close(fd_right);
        }
        *error = "无法打开比较文件";
        return false;
    }
    const std::uint64_t size_left = FileSize(left);
    const std::uint64_t size_right = FileSize(right);
    if (size_left != size_right) {
        *error = "大小不同：" + std::to_string(size_left) + " != " +
                 std::to_string(size_right);
        (void)::close(fd_left);
        (void)::close(fd_right);
        return false;
    }
    std::vector<std::uint8_t> buffer_left(4u * 1024u * 1024u);
    std::vector<std::uint8_t> buffer_right(4u * 1024u * 1024u);
    std::uint64_t offset = 0;
    while (offset < size_left) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer_left.size(), size_left - offset));
        std::size_t got_left = 0;
        std::size_t got_right = 0;
        while (got_left < want) {
            const ssize_t n = ::pread(fd_left, buffer_left.data() + got_left, want - got_left,
                                      static_cast<off_t>(offset + got_left));
            if (n <= 0) {
                break;
            }
            got_left += static_cast<std::size_t>(n);
        }
        while (got_right < want) {
            const ssize_t n = ::pread(fd_right, buffer_right.data() + got_right,
                                      want - got_right,
                                      static_cast<off_t>(offset + got_right));
            if (n <= 0) {
                break;
            }
            got_right += static_cast<std::size_t>(n);
        }
        if (got_left != want || got_right != want) {
            *error = "读取不完整（偏移 " + std::to_string(offset) + "）";
            (void)::close(fd_left);
            (void)::close(fd_right);
            return false;
        }
        if (std::memcmp(buffer_left.data(), buffer_right.data(), want) != 0) {
            *error = "偏移 " + std::to_string(offset) + " 处字节不同";
            (void)::close(fd_left);
            (void)::close(fd_right);
            return false;
        }
        offset += want;
    }
    (void)::close(fd_left);
    (void)::close(fd_right);
    return true;
}

// ---------------------------------------------------------------------------
// 单个场景（生成 → 传输 → 校验）
// ---------------------------------------------------------------------------

bool RunOne(const Options& opts, const std::string& tag, const std::string& src,
            const std::string& dst, std::uint32_t interrupt_after) {
    std::printf("\n=== %s：size=%s transport=%s chunk=%s\n", tag.c_str(),
                FormatBytes(opts.size).c_str(), opts.transport.c_str(),
                FormatBytes(opts.chunk_size == 0 ? go2cpp::app::kDefaultResourceChunkSize
                                                 : opts.chunk_size)
                    .c_str());

    // 1. 源文件（默认复用已存在且大小一致的文件）。
    if (!opts.reuse || FileSize(src) != opts.size) {
        std::string error;
        if (!GeneratePatternFile(src, opts.size, &error)) {
            std::fprintf(stderr, "[FAIL] %s\n", error.c_str());
            return false;
        }
    }

    // 2. 元数据（全文件扫描：SHA-256 / CRC-32 / 块数）。
    FileChunkSource source(src);
    std::string error;
    if (!source.Open(&error)) {
        std::fprintf(stderr, "[FAIL] 打开源文件：%s\n", error.c_str());
        return false;
    }
    const double meta_started = NowSeconds();
    const ResourceMeta meta =
        go2cpp::app::BuildResourceMeta(src, source, opts.chunk_size, {}, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "[FAIL] 元数据：%s\n", error.c_str());
        return false;
    }
    const double meta_seconds = NowSeconds() - meta_started;
    std::printf("  元数据：chunks=%u chunk_size=%s sha256=%s（扫描 %.2f s）\n", meta.chunk_count,
                FormatBytes(meta.chunk_size).c_str(), meta.sha256_hex.substr(0, 16).c_str(),
                meta_seconds);

    // 3. 传输（可选中断 + 续传）。
    std::uint32_t total_chunks_completed = 0;
    double total_seconds = 0.0;
    std::uint64_t total_bytes = 0;
    if (interrupt_after == 0 || interrupt_after >= meta.chunk_count) {
        const SegmentOutcome seg = RunSegment(opts, meta, src, dst, 0, 0, true);
        if (!seg.ok) {
            std::fprintf(stderr, "[FAIL] 传输失败：%s\n", seg.error.c_str());
            return false;
        }
        total_chunks_completed = seg.recv.completed_chunks;
        total_seconds = seg.recv.seconds;
        total_bytes = seg.recv.bytes;
        std::printf("  发送：%s / %.2f s（%s），接收：%s / %.2f s（%s）\n",
                    FormatBytes(seg.send.bytes).c_str(), seg.send.seconds,
                    FormatRate(seg.send.seconds > 0
                                   ? (static_cast<double>(seg.send.bytes) / 1048576.0) /
                                         seg.send.seconds
                                   : 0.0)
                        .c_str(),
                    FormatBytes(seg.recv.bytes).c_str(), seg.recv.seconds,
                    FormatRate(seg.recv.seconds > 0
                                   ? (static_cast<double>(seg.recv.bytes) / 1048576.0) /
                                         seg.recv.seconds
                                   : 0.0)
                        .c_str());
        std::printf("  接收端 finish=%s verified=%s\n", seg.recv.finished ? "true" : "false",
                    seg.recv.verified ? "true" : "false");
    } else {
        // 第一段：发到第 N 块就中断。
        std::printf("  [中断] 第一段发送到第 %u 块后关闭通道\n", interrupt_after);
        const SegmentOutcome first =
            RunSegment(opts, meta, src, dst, 0, interrupt_after, true);
        if (!first.ok) {
            std::fprintf(stderr, "[FAIL] 第一段失败：%s\n", first.error.c_str());
            return false;
        }
        const std::uint32_t resume_from = first.recv.completed_chunks;
        std::printf("  [中断] 接收端已连续完成 %u/%u 块（%s）\n", resume_from,
                    meta.chunk_count, FormatBytes(first.recv.bytes).c_str());
        if (resume_from == 0 || resume_from >= meta.chunk_count) {
            std::fprintf(stderr, "[FAIL] 中断点不合法：%u\n", resume_from);
            return false;
        }
        // 第二段：从 resume_from 续传，目标文件不截断。
        std::printf("  [续传] 从第 %u 块继续，目标文件不截断\n", resume_from);
        const SegmentOutcome second =
            RunSegment(opts, meta, src, dst, resume_from, 0, false);
        if (!second.ok) {
            std::fprintf(stderr, "[FAIL] 续传失败：%s\n", second.error.c_str());
            return false;
        }
        total_chunks_completed = second.recv.completed_chunks;
        total_seconds = first.recv.seconds + second.recv.seconds;
        total_bytes = first.recv.bytes + second.recv.bytes;
        std::printf("  第一段：%s / %.2f s；第二段：%s / %.2f s（%s）\n",
                    FormatBytes(first.recv.bytes).c_str(), first.recv.seconds,
                    FormatBytes(second.recv.bytes).c_str(), second.recv.seconds,
                    FormatRate(second.recv.seconds > 0
                                   ? (static_cast<double>(second.recv.bytes) / 1048576.0) /
                                         second.recv.seconds
                                   : 0.0)
                        .c_str());
    }

    // 4. 校验：大小、SHA-256、逐字节。
    const std::uint64_t src_size = FileSize(src);
    const std::uint64_t dst_size = FileSize(dst);
    if (src_size != dst_size) {
        std::fprintf(stderr, "[FAIL] 大小不一致：源 %llu，目标 %llu\n",
                     static_cast<unsigned long long>(src_size),
                     static_cast<unsigned long long>(dst_size));
        return false;
    }
    const double hash_started = NowSeconds();
    const std::string dst_sha = Sha256File(dst);
    const double hash_seconds = NowSeconds() - hash_started;
    if (dst_sha != meta.sha256_hex) {
        std::fprintf(stderr, "[FAIL] SHA-256 不一致：目标 %s，源 %s\n", dst_sha.c_str(),
                     meta.sha256_hex.c_str());
        return false;
    }
    std::string compare_error;
    const double compare_started = NowSeconds();
    if (!CompareFiles(src, dst, &compare_error)) {
        std::fprintf(stderr, "[FAIL] 逐字节比较：%s\n", compare_error.c_str());
        return false;
    }
    const double compare_seconds = NowSeconds() - compare_started;

    const double throughput =
        total_seconds > 0 ? (static_cast<double>(total_bytes) / (1024.0 * 1024.0)) / total_seconds
                          : 0.0;
    std::printf("  校验：SHA-256 一致（%s…，%.2f s），逐字节一致（%s，%.2f s）\n",
                dst_sha.substr(0, 16).c_str(), hash_seconds, FormatBytes(src_size).c_str(),
                compare_seconds);
    std::printf("  结果：%u/%u 块，%s，吞吐 %s，总用时 %.2f s\n", total_chunks_completed,
                meta.chunk_count, FormatBytes(total_bytes).c_str(),
                FormatRate(throughput).c_str(), total_seconds);
    std::printf("[PASS] %s\n", tag.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------

constexpr const char* kWorkDir = "/tmp/go2cpp_file_transfer";

void PrintUsage(const char* argv0) {
    std::printf(
        "用法：%s [选项]\n"
        "  --transport memory|tcp    传输层（默认 memory）\n"
        "  --size 1G                 载荷大小（默认 1 GiB；支持 K/M/G 后缀）\n"
        "  --chunk-size 512K         分块大小（默认 512 KiB）\n"
        "  --interrupt-after N       发完第 N 块后中断，再从 N 续传\n"
        "  --suite                   跑 6 个边界尺寸（0/1/511K/512K/1M+7/8M+3）\n"
        "  --reuse                   复用已存在且大小相同的源文件\n"
        "  --src PATH / --dst PATH   自定义源/目标路径\n"
        "  --keep                    保留工作目录（默认也保留，便于复查）\n",
        argv0);
}

int RunSuite(const Options& opts) {
    const std::vector<std::uint64_t> sizes = {
        0,
        1,
        511u * 1024u,
        512u * 1024u,
        1024u * 1024u + 7u,
        8u * 1024u * 1024u + 3u,
    };
    int passed = 0;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        Options one = opts;
        one.size = sizes[i];
        one.reuse = false;
        const std::string src =
            std::string(kWorkDir) + "/suite_src_" + std::to_string(i) + ".bin";
        const std::string dst =
            std::string(kWorkDir) + "/suite_dst_" + std::to_string(i) + ".bin";
        const std::string tag = "suite[" + std::to_string(i + 1) + "/" +
                                std::to_string(sizes.size()) + "] " +
                                FormatBytes(sizes[i]);
        if (RunOne(one, tag, src, dst, 0)) {
            ++passed;
        }
    }
    std::printf("\n[suite] %d/%zu 通过\n", passed, sizes.size());
    return passed == static_cast<int>(sizes.size()) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--transport" && i + 1 < argc) {
            opts.transport = argv[++i];
        } else if (arg == "--size" && i + 1 < argc) {
            if (!ParseSize(argv[++i], &opts.size)) {
                std::fprintf(stderr, "非法 --size\n");
                return 2;
            }
        } else if (arg == "--chunk-size" && i + 1 < argc) {
            std::uint64_t chunk = 0;
            if (!ParseSize(argv[++i], &chunk)) {
                std::fprintf(stderr, "非法 --chunk-size\n");
                return 2;
            }
            opts.chunk_size = static_cast<std::uint32_t>(chunk);
        } else if (arg == "--interrupt-after" && i + 1 < argc) {
            if (!ParseU32(argv[++i], &opts.interrupt_after)) {
                std::fprintf(stderr, "非法 --interrupt-after\n");
                return 2;
            }
        } else if (arg == "--suite") {
            opts.suite = true;
        } else if (arg == "--reuse") {
            opts.reuse = true;
        } else if (arg == "--src" && i + 1 < argc) {
            opts.src = argv[++i];
        } else if (arg == "--dst" && i + 1 < argc) {
            opts.dst = argv[++i];
        } else if (arg == "--keep") {
            // 默认就保留（示例便于复查产物）；保留参数便于脚本兼容。
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "未知参数：%s\n", arg.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }
    if (opts.transport != "memory" && opts.transport != "tcp") {
        std::fprintf(stderr, "未知传输层：%s\n", opts.transport.c_str());
        return 2;
    }
    if (!EnsureDirectory(kWorkDir)) {
        std::fprintf(stderr, "无法创建目录 %s\n", kWorkDir);
        return 2;
    }

    std::printf("[file_transfer] 资源传输示例（ResourceTransfer 512 KiB 分块 + SHA-256 + 续传）\n");
    std::printf("  工作目录：%s\n", kWorkDir);
    if (opts.suite) {
        return RunSuite(opts);
    }

    const std::string src =
        opts.src.empty()
            ? std::string(kWorkDir) + "/source_" + std::to_string(opts.size) + ".bin"
            : opts.src;
    const std::string dst =
        opts.dst.empty()
            ? std::string(kWorkDir) + "/target_" + std::to_string(opts.size) + ".bin"
            : opts.dst;
    const bool ok = RunOne(opts, "single", src, dst, opts.interrupt_after);
    return ok ? 0 : 1;
}
