/**
 * @file mq.cpp
 * @brief POSIX 消息队列与 `MqChannel` 的实现（R31：epoll 异步等待）。
 */
#include "go2cpp/os/mq.hpp"

#include "raw_io.hpp"

#include "go2cpp/network/io.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <limits.h>
#include <mqueue.h>
#include <unistd.h>

namespace go2cpp::os {

namespace {

constexpr std::chrono::milliseconds kForever{-1};

bool ValidQueueName(const std::string& name, std::string* reason) {
    if (name.size() < 2 || name[0] != '/') {
        if (reason) {
            *reason = "消息队列名字必须以 '/' 开头且不能只有 '/'";
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

struct timespec AbsoluteFromNow(std::chrono::milliseconds timeout) {
    struct timespec value {};
    clock_gettime(CLOCK_REALTIME, &value);
    if (timeout.count() <= 0) {
        return value;
    }
    value.tv_sec += static_cast<time_t>(timeout.count() / 1000);
    value.tv_nsec += static_cast<long>((timeout.count() % 1000) * 1000000);
    if (value.tv_nsec >= 1000000000L) {
        value.tv_sec += 1;
        value.tv_nsec -= 1000000000L;
    }
    return value;
}

/// 异步模式下「只尝试一次、不阻塞」的绝对截止时间（当前时刻）。
struct timespec ImmediateDeadline() {
    struct timespec value {};
    clock_gettime(CLOCK_REALTIME, &value);
    return value;
}

/// 剩余超时；`timeout < 0` 表示永久（返回 -1ms）。
std::chrono::milliseconds Remaining(std::chrono::steady_clock::time_point start,
                                    std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        return kForever;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (elapsed >= timeout) {
        return std::chrono::milliseconds(0);
    }
    return timeout - elapsed;
}

ProcErrorCode QueueErrnoCode(int error) {
    switch (error) {
        case ENOBUFS:
            return ProcErrorCode::kQueueFull;
        case EMSGSIZE:
            return ProcErrorCode::kInvalidArgument;
        case ETIMEDOUT:
        case EAGAIN:
            return ProcErrorCode::kTimeout;
        case EACCES:
        case EPERM:
            return ProcErrorCode::kPermissionDenied;
        case ENOENT:
        case EEXIST:
            return ProcErrorCode::kMqFailed;
        default:
            return ProcErrorCode::kMqFailed;
    }
}

}  // namespace

// ===========================================================================
// MessageQueue
// ===========================================================================

MessageQueue::~MessageQueue() { Close(); }

MessageQueue::MessageQueue(MessageQueue&& other) noexcept
    : m_name(std::move(other.m_name)),
      m_fd(other.m_fd),
      m_max_messages(other.m_max_messages),
      m_max_message_size(other.m_max_message_size),
      m_dropped(other.m_dropped) {
    {
        std::lock_guard<std::mutex> lock(other.m_async_mutex);
        m_async = std::move(other.m_async);
    }
    other.m_fd = -1;
    other.m_max_messages = 0;
    other.m_max_message_size = 0;
    other.m_name.clear();
    other.m_dropped = 0;
}

MessageQueue& MessageQueue::operator=(MessageQueue&& other) noexcept {
    if (this != &other) {
        Close();
        m_name = std::move(other.m_name);
        m_fd = other.m_fd;
        m_max_messages = other.m_max_messages;
        m_max_message_size = other.m_max_message_size;
        m_dropped = other.m_dropped;
        {
            std::lock_guard<std::mutex> lock(other.m_async_mutex);
            m_async = std::move(other.m_async);
        }
        other.m_fd = -1;
        other.m_max_messages = 0;
        other.m_max_message_size = 0;
        other.m_name.clear();
        other.m_dropped = 0;
    }
    return *this;
}

Result<void> MessageQueue::Create(const std::string& name, long max_messages,
                                  long max_message_size, MessageQueue* out,
                                  mode_t mode) {
    if (out == nullptr) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "输出指针为空",
                         "MessageQueue::Create");
    }
    std::string reason;
    if (!ValidQueueName(name, &reason)) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, reason,
                         "MessageQueue::Create");
    }
    if (max_messages <= 0 || max_message_size <= 0) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL,
                         "队列深度/消息大小必须 > 0", "MessageQueue::Create");
    }
    struct mq_attr attributes {};
    attributes.mq_maxmsg = max_messages;
    attributes.mq_msgsize = max_message_size;
    const int fd = ::mq_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, mode, &attributes);
    if (fd == static_cast<mqd_t>(-1)) {
        return MakeErrnoError(QueueErrnoCode(errno), errno,
                              "mq_open(" + name + ")");
    }
    struct mq_attr actual {};
    if (::mq_getattr(fd, &actual) != 0) {
        const int error = errno;
        (void)::mq_close(fd);
        (void)::mq_unlink(name.c_str());
        return MakeErrnoError(ProcErrorCode::kMqFailed, error,
                              "mq_getattr(" + name + ")");
    }
    MessageQueue queue;
    queue.m_name = name;
    queue.m_fd = fd;
    queue.m_max_messages = actual.mq_maxmsg;
    queue.m_max_message_size = actual.mq_msgsize;
    *out = std::move(queue);
    return OkResult();
}

Result<void> MessageQueue::Open(const std::string& name, MessageQueue* out,
                                MqAccess access) {
    if (out == nullptr) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "输出指针为空",
                         "MessageQueue::Open");
    }
    std::string reason;
    if (!ValidQueueName(name, &reason)) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, reason,
                         "MessageQueue::Open");
    }
    int flags = 0;
    switch (access) {
        case MqAccess::kReadOnly: flags = O_RDONLY; break;
        case MqAccess::kWriteOnly: flags = O_WRONLY; break;
        case MqAccess::kReadWrite: flags = O_RDWR; break;
    }
    const int fd = ::mq_open(name.c_str(), flags);
    if (fd == static_cast<mqd_t>(-1)) {
        return MakeErrnoError(QueueErrnoCode(errno), errno,
                              "mq_open(" + name + ")");
    }
    struct mq_attr actual {};
    if (::mq_getattr(fd, &actual) != 0) {
        const int error = errno;
        (void)::mq_close(fd);
        return MakeErrnoError(ProcErrorCode::kMqFailed, error,
                              "mq_getattr(" + name + ")");
    }
    MessageQueue queue;
    queue.m_name = name;
    queue.m_fd = fd;
    queue.m_max_messages = actual.mq_maxmsg;
    queue.m_max_message_size = actual.mq_msgsize;
    *out = std::move(queue);
    return OkResult();
}

Result<void> MessageQueue::Unlink(const std::string& name) {
    std::string reason;
    if (!ValidQueueName(name, &reason)) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, reason,
                         "MessageQueue::Unlink");
    }
    if (::mq_unlink(name.c_str()) != 0) {
        return MakeErrnoError(ProcErrorCode::kMqFailed, errno,
                              "mq_unlink(" + name + ")");
    }
    return OkResult();
}

Result<void> MessageQueue::Send(ByteSpan payload, unsigned priority,
                                std::chrono::milliseconds timeout,
                                QueueOverflowPolicy policy) {
    if (!valid()) {
        return MakeError(ProcErrorCode::kNotRunning, EBADF, "队列未打开",
                         "MessageQueue::Send");
    }
    if (payload.size > static_cast<std::size_t>(m_max_message_size)) {
        return MakeError(ProcErrorCode::kInvalidArgument, EMSGSIZE,
                         "消息超过 mq_msgsize", "MessageQueue::Send");
    }
    if (priority > static_cast<unsigned>(MQ_PRIO_MAX)) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL,
                         "优先级超过 MQ_PRIO_MAX", "MessageQueue::Send");
    }
    const std::shared_ptr<AsyncIoBinding> binding = async_binding();
    if (binding && binding->enabled() && policy == QueueOverflowPolicy::kBlock) {
        // 异步：先做一次「不阻塞」的发送，满队列才 park 等 EPOLLOUT。
        // 用 mq_timedsend(now) 而不是裸 mq_send，避免 fd 被外部改回阻塞后
        // 在 Fiber 里真的卡住 M。
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            const struct timespec deadline = ImmediateDeadline();
            const int result = ::mq_timedsend(
                m_fd, reinterpret_cast<const char*>(payload.data), payload.size,
                priority, &deadline);
            if (result == 0) {
                return OkResult();
            }
            const int error = errno;
            if (error == EINTR) {
                continue;
            }
            if (error != EAGAIN && error != ENOBUFS && error != ETIMEDOUT) {
                return MakeErrnoError(QueueErrnoCode(error), error, "mq_timedsend");
            }
            const auto remaining = Remaining(start, timeout);
            if (timeout.count() >= 0 && remaining.count() == 0) {
                return MakeError(ProcErrorCode::kTimeout, error, "等待队列可写超时",
                                 "MessageQueue::Send");
            }
            auto waited = binding->WaitWritable(remaining);
            if (!waited.ok()) {
                if (waited.error().code == ProcErrorCode::kTimeout) {
                    return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT,
                                     "等待队列可写超时", "MessageQueue::Send");
                }
                return waited.error();
            }
        }
    }
    const struct timespec deadline = AbsoluteFromNow(timeout);
    for (;;) {
        int result = 0;
        if (policy == QueueOverflowPolicy::kBlock && timeout.count() < 0) {
            result = ::mq_send(m_fd, reinterpret_cast<const char*>(payload.data),
                               payload.size, priority);
        } else {
            result = ::mq_timedsend(m_fd, reinterpret_cast<const char*>(payload.data),
                                    payload.size, priority, &deadline);
        }
        if (result == 0) {
            return OkResult();
        }
        if (errno == EINTR) {
            if (policy == QueueOverflowPolicy::kBlock) {
                continue;
            }
            return MakeError(ProcErrorCode::kTimeout, EINTR, "发送被信号中断",
                             "MessageQueue::Send");
        }
        const int error = errno;
        if (error == EAGAIN || error == ENOBUFS || error == ETIMEDOUT) {
            if (policy == QueueOverflowPolicy::kDropNewest) {
                ++m_dropped;
                return OkResult();
            }
            if (policy == QueueOverflowPolicy::kTimeout) {
                return MakeError(ProcErrorCode::kQueueFull, error, "队列已满",
                                 "MessageQueue::Send");
            }
            return MakeError(ProcErrorCode::kTimeout, error, "等待队列可写超时",
                             "MessageQueue::Send");
        }
        return MakeErrnoError(QueueErrnoCode(error), error, "mq_timedsend");
    }
}

Result<QueueMessage> MessageQueue::Receive(std::chrono::milliseconds timeout) {
    if (!valid()) {
        return MakeError(ProcErrorCode::kNotRunning, EBADF, "队列未打开",
                         "MessageQueue::Receive");
    }
    std::vector<std::byte> buffer(static_cast<std::size_t>(m_max_message_size));
    unsigned priority = 0;
    const std::shared_ptr<AsyncIoBinding> binding = async_binding();
    if (binding && binding->enabled()) {
        // 异步：先非阻塞尝试，空队列才 park 等 EPOLLIN。
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            const struct timespec deadline = ImmediateDeadline();
            const ssize_t count = ::mq_timedreceive(
                m_fd, reinterpret_cast<char*>(buffer.data()), buffer.size(), &priority,
                &deadline);
            if (count >= 0) {
                QueueMessage message;
                message.payload.assign(buffer.begin(), buffer.begin() + count);
                message.priority = priority;
                return message;
            }
            const int error = errno;
            if (error == EINTR) {
                continue;
            }
            if (error != EAGAIN && error != ETIMEDOUT) {
                return MakeErrnoError(QueueErrnoCode(error), error, "mq_timedreceive");
            }
            const auto remaining = Remaining(start, timeout);
            if (timeout.count() >= 0 && remaining.count() == 0) {
                return MakeError(ProcErrorCode::kTimeout, error, "等待消息超时",
                                 "MessageQueue::Receive");
            }
            auto waited = binding->WaitReadable(remaining);
            if (!waited.ok()) {
                if (waited.error().code == ProcErrorCode::kTimeout) {
                    return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT,
                                     "等待消息超时", "MessageQueue::Receive");
                }
                return waited.error();
            }
        }
    }
    for (;;) {
        ssize_t count = 0;
        if (timeout.count() < 0) {
            count = ::mq_receive(m_fd, reinterpret_cast<char*>(buffer.data()),
                                 buffer.size(), &priority);
        } else {
            const struct timespec deadline = AbsoluteFromNow(timeout);
            count = ::mq_timedreceive(m_fd, reinterpret_cast<char*>(buffer.data()),
                                      buffer.size(), &priority, &deadline);
        }
        if (count >= 0) {
            QueueMessage message;
            message.payload.assign(buffer.begin(), buffer.begin() + count);
            message.priority = priority;
            return message;
        }
        if (errno == EINTR) {
            continue;
        }
        const int error = errno;
        if (error == ETIMEDOUT || error == EAGAIN) {
            return MakeError(ProcErrorCode::kTimeout, error, "等待消息超时",
                             "MessageQueue::Receive");
        }
        return MakeErrnoError(QueueErrnoCode(error), error, "mq_timedreceive");
    }
}

Result<std::size_t> MessageQueue::SendBatch(
    const std::vector<QueueMessage>& messages, std::chrono::milliseconds timeout,
    QueueOverflowPolicy policy) {
    std::size_t sent = 0;
    for (const auto& message : messages) {
        auto status = Send(ByteSpan(message.payload.data(), message.payload.size()),
                           message.priority, timeout, policy);
        if (!status.ok()) {
            return status.error();
        }
        ++sent;
    }
    return sent;
}

Result<std::vector<QueueMessage>> MessageQueue::ReceiveBatch(
    std::size_t max_count, std::chrono::milliseconds timeout) {
    std::vector<QueueMessage> messages;
    if (max_count == 0) {
        return messages;
    }
    auto first = Receive(timeout);
    if (!first.ok()) {
        return first.error();
    }
    messages.push_back(std::move(first.value()));
    while (messages.size() < max_count) {
        auto next = Receive(std::chrono::milliseconds(0));
        if (!next.ok()) {
            break;
        }
        messages.push_back(std::move(next.value()));
    }
    return messages;
}

void MessageQueue::Close() noexcept {
    if (m_fd >= 0) {
        (void)DisableAsync();
        (void)::mq_close(m_fd);
        m_fd = -1;
    }
}

// ===========================================================================
// 能力与显式异步（R31）
// ===========================================================================

IoCapability MessageQueue::Capability() const {
    IoCapability capability;
    if (m_fd < 0) {
        capability.kind = IoKind::kUnknown;
        capability.reason = "消息队列未打开";
        return capability;
    }
    // mqueue fd 的 fstat 显示 S_IFREG，不能交给 DetectIoCapability 判断；
    // 但它是内核 mqueue 对象：epoll 可注册、O_NONBLOCK 下 EAGAIN 可观察。
    capability.kind = IoKind::kAnonInode;
    capability.pollable = true;
    capability.can_nonblocking = true;
    capability.native_blocking = true;
    capability.async_supported = true;
    return capability;
}

Result<void> MessageQueue::SetNonBlocking(bool enable) {
    if (!valid()) {
        return MakeError(ProcErrorCode::kNotRunning, EBADF, "队列未打开",
                         "MessageQueue::SetNonBlocking");
    }
    return SetFdNonBlocking(m_fd, enable);
}

std::shared_ptr<AsyncIoBinding> MessageQueue::async_binding() const {
    std::lock_guard<std::mutex> lock(m_async_mutex);
    return m_async;
}

Result<void> MessageQueue::EnableAsync(go2cpp::io::IOManager* manager) {
    if (!valid()) {
        return MakeError(ProcErrorCode::kNotRunning, EBADF, "队列未打开",
                         "MessageQueue::EnableAsync");
    }
    std::shared_ptr<AsyncIoBinding> binding;
    {
        std::lock_guard<std::mutex> lock(m_async_mutex);
        if (!m_async) {
            m_async = std::make_shared<AsyncIoBinding>();
        }
        binding = m_async;
    }
    return binding->Enable(m_fd, Capability(), manager);
}

Result<void> MessageQueue::DisableAsync() {
    const std::shared_ptr<AsyncIoBinding> binding = async_binding();
    if (!binding) {
        return OkResult();
    }
    return binding->Disable();
}

bool MessageQueue::async_enabled() const noexcept {
    std::lock_guard<std::mutex> lock(m_async_mutex);
    return m_async != nullptr && m_async->enabled();
}

// ===========================================================================
// MqChannel
// ===========================================================================

MqChannel::MqChannel(ChannelOptions options) : MessageChannel(options) {}

MqChannel::~MqChannel() { Close(); }

Result<void> MqChannel::OpenQueue(const std::string& name, bool owner) {
    m_owner = owner;
    if (owner) {
        return MessageQueue::Create(name, 10, 8192, &m_queue);
    }
    return MessageQueue::Open(name, &m_queue);
}

Result<std::unique_ptr<MqChannel>> MqChannel::Open(const std::string& name, bool owner,
                                                   ChannelOptions options) {
    auto channel = std::unique_ptr<MqChannel>(new MqChannel(options));
    auto opened = channel->OpenQueue(name, owner);
    if (!opened.ok()) {
        return opened.error();
    }
    return channel;
}

Result<void> MqChannel::Write(ByteSpan data, std::chrono::milliseconds timeout) {
    if (m_closed || !m_queue.valid()) {
        return MakeError(ProcErrorCode::kClosed, EBADF, "消息队列通道已关闭",
                         "MqChannel::Write");
    }
    auto sent = m_queue.Send(data, 0, timeout, QueueOverflowPolicy::kBlock);
    if (sent.ok()) {
        m_stats.bytes_written += data.size;
        ++m_stats.frames_written;
    }
    return sent;
}

Result<std::size_t> MqChannel::Pull(std::chrono::milliseconds timeout) {
    if (m_closed || !m_queue.valid()) {
        return MakeError(ProcErrorCode::kClosed, EBADF, "消息队列通道已关闭",
                         "MqChannel::Pull");
    }
    while (m_pending_offset >= m_pending.size()) {
        auto received = m_queue.Receive(timeout);
        if (!received.ok()) {
            return received.error();
        }
        m_pending = std::move(received.value().payload);
        m_pending_offset = 0;
    }
    const std::size_t remaining = m_pending.size() - m_pending_offset;
    const std::size_t count = std::min(remaining, m_options.read_chunk_bytes);
    AppendRx(m_pending.data() + m_pending_offset, count);
    m_pending_offset += count;
    m_stats.bytes_read += count;
    return count;
}

void MqChannel::Close() noexcept {
    if (m_closed) {
        return;
    }
    m_closed = true;
    m_queue.Close();
}

bool MqChannel::closed() const noexcept { return m_closed; }
const ChannelStats& MqChannel::stats() const noexcept { return m_stats; }
int MqChannel::native_fd() const noexcept { return m_closed ? -1 : m_queue.fd(); }
const std::string& MqChannel::name() const noexcept { return m_queue.name(); }

IoCapability MqChannel::Capability() const { return m_queue.Capability(); }

Result<void> MqChannel::SetNonBlocking(bool enable) {
    if (m_closed || !m_queue.valid()) {
        return MakeError(ProcErrorCode::kClosed, EBADF, "消息队列通道已关闭",
                         "MqChannel::SetNonBlocking");
    }
    return m_queue.SetNonBlocking(enable);
}

Result<void> MqChannel::EnableAsync(go2cpp::io::IOManager* manager) {
    if (m_closed || !m_queue.valid()) {
        return MakeError(ProcErrorCode::kClosed, EBADF, "消息队列通道已关闭",
                         "MqChannel::EnableAsync");
    }
    return m_queue.EnableAsync(manager);
}

Result<void> MqChannel::DisableAsync() { return m_queue.DisableAsync(); }

bool MqChannel::async_enabled() const noexcept { return m_queue.async_enabled(); }

}  // namespace go2cpp::os
