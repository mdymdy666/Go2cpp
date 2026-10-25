/**
 * @file channel.cpp
 * @brief `MessageChannel`/`FdChannel`/`Pipe`/`SocketPair`/`FlowBuffer`/
 *        `ChannelRegistry` 的实现，以及 G2CF 帧编解码。
 */
#include "go2cpp/os/channel.hpp"

#include "go2cpp/os/mq.hpp"
#include "go2cpp/os/shm.hpp"
#include "raw_io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace go2cpp::os {

namespace {

// ---------------------------------------------------------------------------
// CRC32（IEEE，反射多项式 0xEDB88320）与大小端工具
// ---------------------------------------------------------------------------

const std::array<std::uint32_t, 256>& CrcTable() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> entries {};
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) != 0 ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            entries[index] = value;
        }
        return entries;
    }();
    return table;
}

std::uint32_t Crc32(const void* data, std::size_t size) {
    const auto& table = CrcTable();
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t index = 0; index < size; ++index) {
        crc = table[(crc ^ bytes[index]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void WriteU16(std::byte* output, std::uint16_t value) {
    output[0] = static_cast<std::byte>(value & 0xFFu);
    output[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
}

void WriteU32(std::byte* output, std::uint32_t value) {
    output[0] = static_cast<std::byte>(value & 0xFFu);
    output[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
    output[2] = static_cast<std::byte>((value >> 16) & 0xFFu);
    output[3] = static_cast<std::byte>((value >> 24) & 0xFFu);
}

std::uint16_t ReadU16(const std::byte* input) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(input[0]) |
                                      (static_cast<std::uint16_t>(input[1]) << 8));
}

std::uint32_t ReadU32(const std::byte* input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8) |
           (static_cast<std::uint32_t>(input[2]) << 16) |
           (static_cast<std::uint32_t>(input[3]) << 24);
}

std::chrono::milliseconds Remaining(std::chrono::steady_clock::time_point start,
                                    std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        return kWaitForever;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    const auto left = timeout - elapsed;
    return left.count() > 0 ? left : std::chrono::milliseconds(0);
}

int PollTimeout(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        return -1;
    }
    if (timeout.count() == 0) {
        return 0;
    }
    if (timeout.count() > static_cast<std::int64_t>(INT_MAX)) {
        return INT_MAX;
    }
    return static_cast<int>(timeout.count());
}

std::size_t ClampFrameLimit(std::size_t value) {
    if (value == 0 || value > kMaxFramePayloadLimit) {
        return kMaxFramePayloadLimit;
    }
    return value;
}

}  // namespace

// ===========================================================================
// MessageChannel
// ===========================================================================

MessageChannel::MessageChannel(ChannelOptions options) : m_options(options) {
    m_options.max_frame_bytes = ClampFrameLimit(m_options.max_frame_bytes);
    if (m_options.read_chunk_bytes == 0) {
        m_options.read_chunk_bytes = 64u * 1024u;
    }
}

Result<ReadResult> MessageChannel::Read(MutableByteSpan buffer,
                                        std::chrono::milliseconds timeout) {
    if (buffer.empty()) {
        return ReadResult{0, false};
    }
    if (m_options.framing == Framing::kRaw) {
        if (buffered() == 0) {
            auto pulled = Pull(timeout);
            if (!pulled.ok()) {
                if (pulled.error().code == ProcErrorCode::kAgain) {
                    NoteTimeout();
                    return MakeError(ProcErrorCode::kTimeout, EAGAIN, "暂无数据", "Read");
                }
                return pulled.error();
            }
            if (pulled.value() == 0 && eof()) {
                ++m_stats.eof_count;
                return MakeError(ProcErrorCode::kClosed, 0, "对端已关闭", "Read");
            }
        }
        if (buffered() == 0) {
            NoteTimeout();
            return MakeError(ProcErrorCode::kTimeout, EAGAIN, "暂无数据", "Read");
        }
        const std::size_t count = std::min(buffer.size, buffered());
        std::memcpy(buffer.data, rx_data(), count);
        Consume(count);
        m_stats.bytes_read += count;
        return ReadResult{count, false};
    }
    if (m_options.framing == Framing::kLine) {
        auto line = ReadDelimited(timeout);
        if (!line.ok()) {
            return line.error();
        }
        const std::size_t count = std::min(buffer.size, line.value().size());
        std::memcpy(buffer.data, line.value().data(), count);
        m_stats.bytes_read += count;
        return ReadResult{count, false};
    }
    auto frame = ReadFrame(timeout);
    if (!frame.ok()) {
        return frame.error();
    }
    const std::size_t count = std::min(buffer.size, frame.value().payload.size());
    if (count > 0) {
        std::memcpy(buffer.data, frame.value().payload.data(), count);
    }
    return ReadResult{count, false};
}

Result<std::string> MessageChannel::ReadLine(std::chrono::milliseconds timeout) {
    return ReadDelimited(timeout);
}

Result<void> MessageChannel::WriteLine(std::string_view line,
                                       std::chrono::milliseconds timeout) {
    if (line.find('\n') != std::string_view::npos) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL,
                         "行内容不能包含换行符", "WriteLine");
    }
    std::string payload(line);
    payload.push_back('\n');
    return Write(ByteSpan(payload.data(), payload.size()), timeout);
}

Result<void> MessageChannel::WriteFrame(std::uint8_t type, ByteSpan payload,
                                        std::chrono::milliseconds timeout,
                                        std::uint16_t flags) {
    if (payload.size > m_options.max_frame_bytes) {
        return MakeError(ProcErrorCode::kInvalidArgument, EMSGSIZE,
                         "payload 超过 max_frame_bytes", "WriteFrame");
    }
    const std::size_t total =
        kFrameHeaderSize + payload.size + kFrameTrailerSize;
    std::vector<std::byte> frame(total);
    std::byte* output = frame.data();
    WriteU32(output, kFrameMagic);
    output[4] = std::byte{kFrameVersion};
    output[5] = std::byte{type};
    WriteU16(output + 6, flags);
    WriteU32(output + 8, static_cast<std::uint32_t>(payload.size));
    if (payload.size > 0) {
        std::memcpy(output + kFrameHeaderSize, payload.data, payload.size);
    }
    const std::uint32_t checksum =
        Crc32(output + 4, 8 + payload.size);
    WriteU32(output + kFrameHeaderSize + payload.size, checksum);
    auto written = Write(ByteSpan(frame.data(), frame.size()), timeout);
    if (written.ok()) {
        ++m_stats.frames_written;
    }
    return written;
}

Result<Frame> MessageChannel::ReadFrame(std::chrono::milliseconds timeout) {
    const auto start = std::chrono::steady_clock::now();
    bool first_attempt = true;
    for (;;) {
        if (buffered() >= kFrameHeaderSize) {
            const std::byte* header = rx_data();
            if (ReadU32(header) != kFrameMagic) {
                ++m_stats.error_count;
                // 魔数错误意味着流已不可信（可能对端不是 G2CF 协议，或前一次
                // 半帧污染了缓冲）。清空接收缓冲做一次「重新同步」，否则之后
                // 每次解析都会从同一个垃圾字节开始，永远无法恢复。
                Consume(buffered());
                return MakeError(ProcErrorCode::kIoFailed, EPROTO,
                                 "帧魔数不匹配（非本协议数据）", "ReadFrame");
            }
            if (static_cast<std::uint8_t>(header[4]) != kFrameVersion) {
                ++m_stats.error_count;
                Consume(buffered());
                return MakeError(ProcErrorCode::kIoFailed, EPROTO,
                                 "帧版本不匹配", "ReadFrame");
            }
            const std::uint8_t type = static_cast<std::uint8_t>(header[5]);
            const std::uint16_t flags = ReadU16(header + 6);
            const std::uint32_t length = ReadU32(header + 8);
            if (length > m_options.max_frame_bytes) {
                ++m_stats.error_count;
                Consume(buffered());
                return MakeError(ProcErrorCode::kResourceLimit, EMSGSIZE,
                                 "帧长度超过上限", "ReadFrame");
            }
            const std::size_t total =
                kFrameHeaderSize + static_cast<std::size_t>(length) + kFrameTrailerSize;
            if (buffered() >= total) {
                const std::uint32_t expected =
                    ReadU32(header + kFrameHeaderSize + length);
                const std::uint32_t actual = Crc32(header + 4, 8 + length);
                Frame frame;
                frame.type = type;
                frame.flags = flags;
                frame.payload.resize(length);
                if (length > 0) {
                    std::memcpy(frame.payload.data(), header + kFrameHeaderSize, length);
                }
                Consume(total);
                if (expected != actual) {
                    ++m_stats.error_count;
                    return MakeError(ProcErrorCode::kIoFailed, EBADMSG,
                                     "帧 CRC 校验失败（已丢弃该帧）", "ReadFrame");
                }
                ++m_stats.frames_read;
                m_stats.bytes_read += length;
                return frame;
            }
        }
        const auto remaining = Remaining(start, timeout);
        if (timeout.count() >= 0 && remaining.count() == 0 && !first_attempt) {
            NoteTimeout();
            return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT,
                             "等待完整帧超时", "ReadFrame");
        }
        auto pulled = Pull(first_attempt ? timeout : remaining);
        first_attempt = false;
        if (!pulled.ok()) {
            if (pulled.error().code == ProcErrorCode::kAgain ||
                pulled.error().code == ProcErrorCode::kTimeout) {
                if (timeout.count() >= 0 &&
                    (timeout.count() == 0 ||
                     std::chrono::steady_clock::now() >= start + timeout)) {
                    NoteTimeout();
                    return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT,
                                     "等待完整帧超时", "ReadFrame");
                }
                continue;
            }
            if (pulled.error().code == ProcErrorCode::kClosed) {
                if (buffered() > 0) {
                    ++m_stats.error_count;
                    return MakeError(ProcErrorCode::kIoFailed, EPIPE,
                                     "帧被截断（对端在帧中间关闭）", "ReadFrame");
                }
                ++m_stats.eof_count;
                return MakeError(ProcErrorCode::kClosed, 0, "对端已关闭", "ReadFrame");
            }
            return pulled.error();
        }
        if (pulled.value() == 0 && eof()) {
            if (buffered() > 0) {
                ++m_stats.error_count;
                return MakeError(ProcErrorCode::kIoFailed, EPIPE,
                                 "帧被截断（对端在帧中间关闭）", "ReadFrame");
            }
            ++m_stats.eof_count;
            return MakeError(ProcErrorCode::kClosed, 0, "对端已关闭", "ReadFrame");
        }
    }
}

Result<std::string> MessageChannel::ReadToEnd(std::size_t max_bytes,
                                              std::chrono::milliseconds timeout) {
    std::string output;
    const auto start = std::chrono::steady_clock::now();
    std::byte buffer[65536];
    for (;;) {
        auto read = Read(MutableByteSpan(buffer, sizeof(buffer)), timeout);
        if (!read.ok()) {
            if (read.error().code == ProcErrorCode::kClosed) {
                break;
            }
            if (read.error().code == ProcErrorCode::kTimeout && output.empty()) {
                return read.error();
            }
            if (read.error().code == ProcErrorCode::kTimeout) {
                break;
            }
            return read.error();
        }
        const std::size_t count = std::min(read.value().bytes, sizeof(buffer));
        if (max_bytes > 0 && output.size() + count > max_bytes) {
            const std::size_t room = max_bytes - output.size();
            output.append(reinterpret_cast<const char*>(buffer), room);
            break;
        }
        output.append(reinterpret_cast<const char*>(buffer), count);
        if (timeout.count() >= 0 &&
            std::chrono::steady_clock::now() - start >= timeout) {
            break;
        }
    }
    return output;
}

IoCapability MessageChannel::Capability() const {
    return DetectIoCapability(native_fd());
}

Result<void> MessageChannel::SetNonBlocking(bool enable) {
    (void)enable;
    return MakeError(ProcErrorCode::kUnsupported, 0,
                     "该通道后端不支持设置非阻塞", "MessageChannel::SetNonBlocking");
}

Result<void> MessageChannel::EnableAsync(go2cpp::io::IOManager* manager) {
    (void)manager;
    return MakeError(ProcErrorCode::kUnsupported, 0,
                     "该通道后端不支持异步等待", "MessageChannel::EnableAsync");
}

Result<void> MessageChannel::DisableAsync() { return OkResult(); }

void MessageChannel::AppendRx(const void* data, std::size_t count) {
    if (count == 0) {
        return;
    }
    if (m_rx_offset >= m_rx.size()) {
        m_rx.clear();
        m_rx_offset = 0;
    } else if (m_rx_offset > 64u * 1024u) {
        m_rx.erase(m_rx.begin(), m_rx.begin() + static_cast<std::ptrdiff_t>(m_rx_offset));
        m_rx_offset = 0;
    }
    const auto* bytes = static_cast<const std::byte*>(data);
    m_rx.insert(m_rx.end(), bytes, bytes + count);
}

void MessageChannel::Consume(std::size_t count) noexcept {
    if (count >= buffered()) {
        m_rx.clear();
        m_rx_offset = 0;
        return;
    }
    m_rx_offset += count;
    if (m_rx_offset > 64u * 1024u) {
        m_rx.erase(m_rx.begin(), m_rx.begin() + static_cast<std::ptrdiff_t>(m_rx_offset));
        m_rx_offset = 0;
    }
}

Result<void> MessageChannel::EnsureBuffered(std::size_t needed,
                                            std::chrono::milliseconds timeout) {
    const auto start = std::chrono::steady_clock::now();
    while (buffered() < needed) {
        if (timeout.count() >= 0) {
            const auto remaining = Remaining(start, timeout);
            if (remaining.count() == 0) {
                NoteTimeout();
                return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT, "等待数据超时",
                                 "EnsureBuffered");
            }
        }
        auto pulled = Pull(Remaining(start, timeout));
        if (!pulled.ok()) {
            if (pulled.error().code == ProcErrorCode::kAgain) {
                NoteTimeout();
                return MakeError(ProcErrorCode::kTimeout, EAGAIN, "暂无数据",
                                 "EnsureBuffered");
            }
            return pulled.error();
        }
        if (pulled.value() == 0 && eof()) {
            return MakeError(ProcErrorCode::kClosed, 0, "对端已关闭", "EnsureBuffered");
        }
    }
    return OkResult();
}

Result<std::size_t> MessageChannel::FillFor(std::chrono::milliseconds timeout) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        auto pulled = Pull(Remaining(start, timeout));
        if (!pulled.ok()) {
            return pulled.error();
        }
        if (pulled.value() > 0 || eof()) {
            return pulled;
        }
        if (timeout.count() >= 0 && Remaining(start, timeout).count() == 0) {
            NoteTimeout();
            return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT, "等待数据超时", "FillFor");
        }
    }
}

Result<std::string> MessageChannel::ReadDelimited(std::chrono::milliseconds timeout) {
    const auto start = std::chrono::steady_clock::now();
    bool first_attempt = true;
    for (;;) {
        const std::byte* begin = rx_data();
        const std::byte* end = begin + buffered();
        const std::byte* newline = std::find(begin, end, std::byte{'\n'});
        if (newline != end) {
            const std::size_t length = static_cast<std::size_t>(newline - begin);
            std::string line(reinterpret_cast<const char*>(begin), length);
            Consume(length + 1);
            ++m_stats.frames_read;
            return line;
        }
        if (buffered() > m_options.max_frame_bytes) {
            ++m_stats.error_count;
            return MakeError(ProcErrorCode::kResourceLimit, EMSGSIZE,
                             "行长度超过上限", "ReadLine");
        }
        const auto remaining = Remaining(start, timeout);
        if (timeout.count() >= 0 && remaining.count() == 0 && !first_attempt) {
            NoteTimeout();
            return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT, "等待行超时", "ReadLine");
        }
        auto pulled = Pull(first_attempt ? timeout : remaining);
        first_attempt = false;
        if (!pulled.ok()) {
            if (pulled.error().code == ProcErrorCode::kAgain) {
                NoteTimeout();
                return MakeError(ProcErrorCode::kTimeout, EAGAIN, "暂无数据", "ReadLine");
            }
            if (pulled.error().code == ProcErrorCode::kClosed) {
                if (buffered() > 0) {
                    std::string line(reinterpret_cast<const char*>(rx_data()), buffered());
                    Consume(buffered());
                    return line;
                }
                ++m_stats.eof_count;
                return MakeError(ProcErrorCode::kClosed, 0, "对端已关闭", "ReadLine");
            }
            return pulled.error();
        }
        if (pulled.value() == 0 && eof()) {
            if (buffered() > 0) {
                std::string line(reinterpret_cast<const char*>(rx_data()), buffered());
                Consume(buffered());
                return line;
            }
            ++m_stats.eof_count;
            return MakeError(ProcErrorCode::kClosed, 0, "对端已关闭", "ReadLine");
        }
    }
}

// ===========================================================================
// FdChannel
// ===========================================================================

FdChannel::FdChannel(int fd, bool take_ownership, ChannelOptions options)
    : MessageChannel(options), m_fd(fd), m_owned(take_ownership) {
    m_capability = DetectIoCapability(fd);
    if (fd >= 0) {
        // 内部统一非阻塞：同步模式用 poll 等就绪，异步模式交给 IOManager。
        (void)detail::SetNonBlocking(fd);
        m_closed = false;
    } else {
        m_closed = true;
    }
}

FdChannel::~FdChannel() { Close(); }

Result<std::size_t> FdChannel::Pull(std::chrono::milliseconds timeout) {
    if (m_closed || m_fd < 0) {
        return MakeError(ProcErrorCode::kClosed, EBADF, "通道已关闭", "FdChannel::Pull");
    }
    char buffer[65536];
    if (m_async.enabled()) {
        auto waited = m_async.WaitReadable(timeout);
        if (!waited.ok()) {
            if (waited.error().code == ProcErrorCode::kTimeout) {
                NoteTimeout();
            }
            return waited.error();
        }
        const ssize_t count = detail::RawRead(m_fd, buffer, sizeof(buffer));
        if (count > 0) {
            AppendRx(buffer, static_cast<std::size_t>(count));
            return static_cast<std::size_t>(count);
        }
        if (count == 0) {
            MarkEof();
            ++m_stats.eof_count;
            return std::size_t{0};
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MakeError(ProcErrorCode::kAgain, EAGAIN, "暂无数据", "FdChannel::Pull");
        }
        if (errno == EIO) {
            // PTY：从端全关后 master 返回 EIO，语义等价于 EOF。
            MarkEof();
            ++m_stats.eof_count;
            return std::size_t{0};
        }
        ++m_stats.error_count;
        return MakeErrnoError(ProcErrorCode::kIoFailed, errno, "read");
    }
    for (;;) {
        struct pollfd descriptor {};
        descriptor.fd = m_fd;
        descriptor.events = POLLIN;
        const int ready = detail::RawPoll(&descriptor, 1, PollTimeout(timeout));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            ++m_stats.error_count;
            return MakeErrnoError(ProcErrorCode::kIoFailed, errno, "poll");
        }
        if (ready == 0) {
            NoteTimeout();
            return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT, "等待可读超时",
                             "FdChannel::Pull");
        }
        const ssize_t count = detail::RawRead(m_fd, buffer, sizeof(buffer));
        if (count > 0) {
            AppendRx(buffer, static_cast<std::size_t>(count));
            return static_cast<std::size_t>(count);
        }
        if (count == 0) {
            MarkEof();
            ++m_stats.eof_count;
            return std::size_t{0};
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }
        if (errno == EIO) {
            MarkEof();
            ++m_stats.eof_count;
            return std::size_t{0};
        }
        ++m_stats.error_count;
        return MakeErrnoError(ProcErrorCode::kIoFailed, errno, "read");
    }
}

Result<void> FdChannel::Write(ByteSpan data, std::chrono::milliseconds timeout) {
    if (m_closed || m_fd < 0) {
        return MakeError(ProcErrorCode::kClosed, EBADF, "通道已关闭", "FdChannel::Write");
    }
    if (data.size == 0) {
        return OkResult();
    }
    const auto start = std::chrono::steady_clock::now();
    std::size_t offset = 0;
    while (offset < data.size) {
        const auto remaining = Remaining(start, timeout);
        if (timeout.count() >= 0 && remaining.count() == 0) {
            NoteTimeout();
            return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT, "写超时",
                             "FdChannel::Write");
        }
        if (m_async.enabled()) {
            auto waited = m_async.WaitWritable(remaining);
            if (!waited.ok()) {
                if (waited.error().code == ProcErrorCode::kTimeout) {
                    NoteTimeout();
                }
                return waited.error();
            }
        } else {
            struct pollfd descriptor {};
            descriptor.fd = m_fd;
            descriptor.events = POLLOUT;
            const int ready = detail::RawPoll(&descriptor, 1, PollTimeout(remaining));
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ++m_stats.error_count;
                return MakeErrnoError(ProcErrorCode::kIoFailed, errno, "poll(POLLOUT)");
            }
            if (ready == 0) {
                NoteTimeout();
                return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT, "等待可写超时",
                                 "FdChannel::Write");
            }
        }
        const ssize_t count = detail::RawWrite(
            m_fd, reinterpret_cast<const char*>(data.data) + offset, data.size - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        ++m_stats.error_count;
        if (count < 0 && errno == EPIPE) {
            return MakeError(ProcErrorCode::kChannelClosed, EPIPE, "对端已关闭",
                             "FdChannel::Write");
        }
        return MakeErrnoError(ProcErrorCode::kIoFailed, errno, "write");
    }
    m_stats.bytes_written += data.size;
    return OkResult();
}

void FdChannel::Close() noexcept {
    if (m_closed) {
        return;
    }
    (void)m_async.Disable();
    if (m_owned && m_fd >= 0) {
        (void)detail::RawClose(m_fd);
    }
    m_fd = -1;
    m_closed = true;
}

bool FdChannel::closed() const noexcept { return m_closed; }
int FdChannel::native_fd() const noexcept { return m_closed ? -1 : m_fd; }
const ChannelStats& FdChannel::stats() const noexcept { return m_stats; }

IoCapability FdChannel::Capability() const { return m_capability; }

Result<void> FdChannel::SetNonBlocking(bool enable) {
    if (m_closed || m_fd < 0) {
        return MakeError(ProcErrorCode::kClosed, EBADF, "通道已关闭",
                         "FdChannel::SetNonBlocking");
    }
    if (!m_capability.can_nonblocking) {
        return MakeError(ProcErrorCode::kUnsupported, 0,
                         "fd 不支持非阻塞: " + m_capability.reason,
                         "FdChannel::SetNonBlocking");
    }
    return SetFdNonBlocking(m_fd, enable);
}

Result<void> FdChannel::EnableAsync(go2cpp::io::IOManager* manager) {
    if (m_closed || m_fd < 0) {
        return MakeError(ProcErrorCode::kClosed, EBADF, "通道已关闭",
                         "FdChannel::EnableAsync");
    }
    return m_async.Enable(m_fd, m_capability, manager);
}

Result<void> FdChannel::DisableAsync() { return m_async.Disable(); }

bool FdChannel::async_enabled() const noexcept { return m_async.enabled(); }

std::unique_ptr<FdChannel> MakeFdChannel(int fd, bool take_ownership,
                                         ChannelOptions options) {
    return std::make_unique<FdChannel>(fd, take_ownership, options);
}

// ===========================================================================
// Pipe / SocketPair
// ===========================================================================

Pipe::~Pipe() {
    CloseRead();
    CloseWrite();
}

Pipe::Pipe(Pipe&& other) noexcept : m_read(other.m_read), m_write(other.m_write) {
    other.m_read = -1;
    other.m_write = -1;
}

Pipe& Pipe::operator=(Pipe&& other) noexcept {
    if (this != &other) {
        CloseRead();
        CloseWrite();
        m_read = other.m_read;
        m_write = other.m_write;
        other.m_read = -1;
        other.m_write = -1;
    }
    return *this;
}

Result<Pipe> Pipe::Create(bool close_on_exec) {
    int fds[2] = {-1, -1};
    if (detail::RawPipe2(fds, close_on_exec ? O_CLOEXEC : 0) != 0) {
        return MakeErrnoError(ProcErrorCode::kIoFailed, errno, "pipe2");
    }
    Pipe pipe;
    pipe.m_read = fds[0];
    pipe.m_write = fds[1];
    return pipe;
}

int Pipe::ReleaseRead() noexcept {
    const int fd = m_read;
    m_read = -1;
    return fd;
}

int Pipe::ReleaseWrite() noexcept {
    const int fd = m_write;
    m_write = -1;
    return fd;
}

void Pipe::CloseRead() noexcept {
    if (m_read >= 0) {
        (void)detail::RawClose(m_read);
        m_read = -1;
    }
}

void Pipe::CloseWrite() noexcept {
    if (m_write >= 0) {
        (void)detail::RawClose(m_write);
        m_write = -1;
    }
}

IoCapability Pipe::Capability() const { return DetectIoCapability(m_read); }

Result<void> Pipe::SetNonBlocking(bool enable) {
    if (m_read >= 0) {
        auto status = SetFdNonBlocking(m_read, enable);
        if (!status.ok()) {
            return status.error();
        }
    }
    if (m_write >= 0) {
        auto status = SetFdNonBlocking(m_write, enable);
        if (!status.ok()) {
            return status.error();
        }
    }
    return OkResult();
}

SocketPair::~SocketPair() {
    CloseFirst();
    CloseSecond();
}

SocketPair::SocketPair(SocketPair&& other) noexcept
    : m_first(other.m_first), m_second(other.m_second) {
    other.m_first = -1;
    other.m_second = -1;
}

SocketPair& SocketPair::operator=(SocketPair&& other) noexcept {
    if (this != &other) {
        CloseFirst();
        CloseSecond();
        m_first = other.m_first;
        m_second = other.m_second;
        other.m_first = -1;
        other.m_second = -1;
    }
    return *this;
}

Result<SocketPair> SocketPair::Create(int type, bool close_on_exec) {
    int fds[2] = {-1, -1};
    const int socket_type = type | (close_on_exec ? SOCK_CLOEXEC : 0);
    if (detail::RawSocketPair(AF_UNIX, socket_type, 0, fds) != 0) {
        return MakeErrnoError(ProcErrorCode::kIoFailed, errno, "socketpair");
    }
    SocketPair pair;
    pair.m_first = fds[0];
    pair.m_second = fds[1];
    return pair;
}

int SocketPair::ReleaseFirst() noexcept {
    const int fd = m_first;
    m_first = -1;
    return fd;
}

int SocketPair::ReleaseSecond() noexcept {
    const int fd = m_second;
    m_second = -1;
    return fd;
}

void SocketPair::CloseFirst() noexcept {
    if (m_first >= 0) {
        (void)detail::RawClose(m_first);
        m_first = -1;
    }
}

void SocketPair::CloseSecond() noexcept {
    if (m_second >= 0) {
        (void)detail::RawClose(m_second);
        m_second = -1;
    }
}

IoCapability SocketPair::Capability() const { return DetectIoCapability(m_first); }

Result<void> SocketPair::SetNonBlocking(bool enable) {
    if (m_first >= 0) {
        auto status = SetFdNonBlocking(m_first, enable);
        if (!status.ok()) {
            return status.error();
        }
    }
    if (m_second >= 0) {
        auto status = SetFdNonBlocking(m_second, enable);
        if (!status.ok()) {
            return status.error();
        }
    }
    return OkResult();
}

// ===========================================================================
// FlowBuffer（R12）
// ===========================================================================

FlowBuffer::FlowBuffer(FlowControlPolicy policy) : m_policy(policy) {
    if (m_policy.high_watermark == 0) {
        m_policy.high_watermark = 1;
    }
    if (m_policy.low_watermark >= m_policy.high_watermark) {
        m_policy.low_watermark = m_policy.high_watermark / 2;
    }
}

bool FlowBuffer::CanAccept(std::size_t count) const noexcept {
    return m_data.size() + count <= m_policy.high_watermark;
}

std::size_t FlowBuffer::AvailableCapacity() const noexcept {
    return m_data.size() >= m_policy.high_watermark
               ? 0
               : m_policy.high_watermark - m_data.size();
}

Result<void> FlowBuffer::Push(ByteSpan data, std::chrono::milliseconds timeout) {
    if (data.size == 0) {
        return OkResult();
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    const bool oversized = data.size > m_policy.high_watermark;
    if (!oversized) {
        switch (m_policy.action) {
            case BackpressureAction::kDrop:
                if (!CanAccept(data.size)) {
                    m_stats.dropped_bytes += data.size;
                    return OkResult();
                }
                break;
            case BackpressureAction::kTimeout:
                if (!CanAccept(data.size)) {
                    return MakeError(ProcErrorCode::kQueueFull, ENOBUFS,
                                     "流控缓冲已满（kTimeout 策略）", "FlowBuffer::Push");
                }
                break;
            case BackpressureAction::kBlock: {
                const auto deadline =
                    timeout.count() < 0
                        ? std::chrono::steady_clock::time_point::max()
                        : std::chrono::steady_clock::now() + timeout;
                while (!CanAccept(data.size)) {
                    if (timeout.count() < 0) {
                        m_cv.wait(lock);
                        continue;
                    }
                    if (m_cv.wait_until(lock, deadline) == std::cv_status::timeout &&
                        !CanAccept(data.size)) {
                        return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT,
                                         "等待流控低水位超时", "FlowBuffer::Push");
                    }
                }
                break;
            }
        }
    }
    const auto* bytes = data.data;
    m_data.insert(m_data.end(), bytes, bytes + data.size);
    return OkResult();
}

std::size_t FlowBuffer::Pop(MutableByteSpan out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t count = std::min(out.size, m_data.size());
    if (count == 0) {
        return 0;
    }
    std::memcpy(out.data, m_data.data(), count);
    m_data.erase(m_data.begin(), m_data.begin() + static_cast<std::ptrdiff_t>(count));
    if (m_data.size() <= m_policy.low_watermark) {
        m_cv.notify_all();
    }
    return count;
}

std::size_t FlowBuffer::Peek(MutableByteSpan out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t count = std::min(out.size, m_data.size());
    if (count > 0) {
        std::memcpy(out.data, m_data.data(), count);
    }
    return count;
}

std::size_t FlowBuffer::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_data.size();
}

bool FlowBuffer::empty() const { return size() == 0; }

bool FlowBuffer::above_high() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_data.size() >= m_policy.high_watermark;
}

bool FlowBuffer::below_low() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_data.size() <= m_policy.low_watermark;
}

void FlowBuffer::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_data.clear();
    m_cv.notify_all();
}

// ===========================================================================
// ChannelRegistry（R10/R23）
// ===========================================================================

const char* ToString(ChannelKind kind) noexcept {
    switch (kind) {
        case ChannelKind::kPipe: return "pipe";
        case ChannelKind::kSocketPair: return "socketpair";
        case ChannelKind::kSharedMemory: return "shm";
        case ChannelKind::kMessageQueue: return "mq";
    }
    return "unknown";
}

ChannelRegistry& ChannelRegistry::Instance() {
    static ChannelRegistry registry;
    return registry;
}

ChannelRegistry::ChannelRegistry() {
    Register("pipe", [](const ChannelSpec& spec) -> Result<std::unique_ptr<MessageChannel>> {
        if (spec.fd < 0) {
            return MakeError(ProcErrorCode::kInvalidArgument, EBADF,
                             "pipe 后端需要 spec.fd", "ChannelRegistry::Create");
        }
        return std::unique_ptr<MessageChannel>(
            new FdChannel(spec.fd, spec.take_ownership, spec.options));
    });
    Register("socketpair",
             [](const ChannelSpec& spec) -> Result<std::unique_ptr<MessageChannel>> {
                 if (spec.fd < 0) {
                     return MakeError(ProcErrorCode::kInvalidArgument, EBADF,
                                      "socketpair 后端需要 spec.fd",
                                      "ChannelRegistry::Create");
                 }
                 return std::unique_ptr<MessageChannel>(
                     new FdChannel(spec.fd, spec.take_ownership, spec.options));
             });
    Register("shm", [](const ChannelSpec& spec) -> Result<std::unique_ptr<MessageChannel>> {
        Result<std::unique_ptr<ShmChannel>> channel =
            spec.owner
                ? ShmChannel::Create(spec.name, spec.capacity, true, spec.overwrite,
                                     spec.options)
                : ShmChannel::Open(spec.name, spec.options);
        if (!channel.ok()) {
            return channel.error();
        }
        return std::unique_ptr<MessageChannel>(std::move(channel.value()));
    });
    Register("mq", [](const ChannelSpec& spec) -> Result<std::unique_ptr<MessageChannel>> {
        Result<std::unique_ptr<MqChannel>> channel =
            MqChannel::Open(spec.name, spec.owner, spec.options);
        if (!channel.ok()) {
            return channel.error();
        }
        return std::unique_ptr<MessageChannel>(std::move(channel.value()));
    });
}

bool ChannelRegistry::Register(const std::string& backend, ChannelFactory factory) {
    if (backend.empty() || !factory) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& entry : m_factories) {
        if (entry.first == backend) {
            entry.second = std::move(factory);
            return true;
        }
    }
    m_factories.emplace_back(backend, std::move(factory));
    return true;
}

bool ChannelRegistry::Unregister(const std::string& backend) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_factories.begin(); it != m_factories.end(); ++it) {
        if (it->first == backend) {
            m_factories.erase(it);
            return true;
        }
    }
    return false;
}

bool ChannelRegistry::Has(const std::string& backend) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& entry : m_factories) {
        if (entry.first == backend) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> ChannelRegistry::Backends() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> names;
    names.reserve(m_factories.size());
    for (const auto& entry : m_factories) {
        names.push_back(entry.first);
    }
    return names;
}

Result<std::unique_ptr<MessageChannel>> ChannelRegistry::Create(
    const ChannelSpec& spec) const {
    return Create(ToString(spec.kind), spec);
}

Result<std::unique_ptr<MessageChannel>> ChannelRegistry::Create(
    const std::string& backend, const ChannelSpec& spec) const {
    ChannelFactory factory;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& entry : m_factories) {
            if (entry.first == backend) {
                factory = entry.second;
                break;
            }
        }
    }
    if (!factory) {
        return MakeError(ProcErrorCode::kUnsupported, 0,
                         "未注册的通道后端: " + backend, "ChannelRegistry::Create");
    }
    return factory(spec);
}

}  // namespace go2cpp::os
