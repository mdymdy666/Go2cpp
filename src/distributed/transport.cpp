/**
 * @file transport.cpp
 * @brief TCP 协调者服务端/客户端与二进制帧协议实现。
 *
 * 协议（同一构建内使用，无版本协商）：
 * ```
 *   frame   = u32_be(payload_len) payload
 *   request = u8(op) fields...
 *   reply   = u8(status) str(message) fields...
 *   str     = u32_be(len) bytes
 *   u32/u64 = 大端
 * ```
 *
 * 设计取舍：
 * - 不做请求多路复用：一个客户端连接上一次只有一个在途请求，客户端用互斥
 *   锁串行化。心跳与业务调用共用一把锁是可接受的（每次都是几十字节），
 *   而实现多路复用需要请求 id/超时/乱序响应，是另一个模块的量级。
 * - 响应不压缩、不加密：默认只监听回环地址（`127.0.0.1`）；跨机部署需要
 *   外层 TLS/VPN，文档明确标注。
 * - 服务端每连接一个线程：连接数在数十级（每个进程一个客户端），用线程
 *   模型最简单；若需要上万连接应换事件循环（接口不变）。
 */

#include "go2cpp/distributed/transport.hpp"

#include "go2cpp/network/socket.hpp"
#include "go2cpp/relay/hub.hpp"
#include "go2cpp/streams.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/tcp.h>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace go2cpp::distributed {

namespace {

// ---------------------------------------------------------------------------
// 编解码
// ---------------------------------------------------------------------------

class Writer {
public:
    void U8(std::uint8_t value) { m_data.push_back(value); }

    void U32(std::uint32_t value) {
        m_data.push_back(static_cast<std::uint8_t>(value >> 24));
        m_data.push_back(static_cast<std::uint8_t>(value >> 16));
        m_data.push_back(static_cast<std::uint8_t>(value >> 8));
        m_data.push_back(static_cast<std::uint8_t>(value));
    }

    void U64(std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            m_data.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void I64(std::int64_t value) { U64(static_cast<std::uint64_t>(value)); }

    void Str(std::string_view value) {
        U32(static_cast<std::uint32_t>(value.size()));
        m_data.insert(m_data.end(), value.begin(), value.end());
    }

    void Bool(bool value) { U8(value ? 1 : 0); }

    void Append(const std::vector<std::uint8_t> &other) {
        m_data.insert(m_data.end(), other.begin(), other.end());
    }

    const std::vector<std::uint8_t> &data() const noexcept { return m_data; }
    std::vector<std::uint8_t> Take() { return std::move(m_data); }

private:
    std::vector<std::uint8_t> m_data;
};

class Reader {
public:
    Reader(const std::uint8_t *data, std::size_t size)
        : m_data(data != nullptr ? data : &s_empty), m_size(size) {}

    bool U8(std::uint8_t &value) {
        if (!Ensure(1)) {
            return false;
        }
        value = m_data[m_pos++];
        return true;
    }

    bool U32(std::uint32_t &value) {
        if (!Ensure(4)) {
            return false;
        }
        value = (static_cast<std::uint32_t>(m_data[m_pos]) << 24) |
                (static_cast<std::uint32_t>(m_data[m_pos + 1]) << 16) |
                (static_cast<std::uint32_t>(m_data[m_pos + 2]) << 8) |
                static_cast<std::uint32_t>(m_data[m_pos + 3]);
        m_pos += 4;
        return true;
    }

    bool U64(std::uint64_t &value) {
        if (!Ensure(8)) {
            return false;
        }
        value = 0;
        for (int index = 0; index < 8; ++index) {
            value = (value << 8) | m_data[m_pos + static_cast<std::size_t>(index)];
        }
        m_pos += 8;
        return true;
    }

    bool I64(std::int64_t &value) {
        std::uint64_t raw = 0;
        if (!U64(raw)) {
            return false;
        }
        value = static_cast<std::int64_t>(raw);
        return true;
    }

    bool Str(std::string &value) {
        std::uint32_t length = 0;
        if (!U32(length) || !Ensure(length)) {
            return false;
        }
        value.assign(reinterpret_cast<const char *>(m_data + m_pos), length);
        m_pos += length;
        return true;
    }

    bool Bool(bool &value) {
        std::uint8_t raw = 0;
        if (!U8(raw)) {
            return false;
        }
        value = raw != 0;
        return true;
    }

    const std::uint8_t *Rest() const noexcept { return m_data + m_pos; }
    std::size_t RestSize() const noexcept { return m_size - m_pos; }

private:
    bool Ensure(std::size_t count) {
        if (count > m_size - m_pos) {
            return false;
        }
        return true;
    }

    static const std::uint8_t s_empty;
    const std::uint8_t *m_data{nullptr};
    std::size_t m_size{0};
    std::size_t m_pos{0};
};

const std::uint8_t Reader::s_empty = 0;

constexpr std::size_t kFrameHeaderBytes = 4;

/// 操作码：服务端与客户端必须来自同一构建（不做事先版本协商）。
enum class Op : std::uint8_t {
    kPing = 1,
    kStats = 2,
    kRegister = 3,
    kRenew = 4,
    kRelease = 5,
    kLookup = 6,
    kListServices = 7,
    kAcquireLock = 8,
    kQueryLock = 9,
    kListLocks = 10,
    kJoinElection = 11,
    kQueryElection = 12,
    kListElections = 13,
    kSweep = 14,
};

struct Response {
    Status status{Status::kProtocolError};
    std::string message;
    std::vector<std::uint8_t> body;
};

bool ReadExact(net::Socket &socket, void *buffer, std::size_t size, Duration timeout) {
    auto *cursor = static_cast<std::uint8_t *>(buffer);
    const TimePoint deadline = Clock::now() + timeout;
    while (size > 0) {
        const TimePoint::duration left = deadline - Clock::now();
        if (left <= TimePoint::duration::zero()) {
            return false;
        }
        Duration wait = std::chrono::duration_cast<Duration>(left);
        if (wait <= Duration::zero()) {
            wait = Duration(1);
        }
        const net::SocketResult result = socket.recv(cursor, size, 0, wait);
        if (!result.ok() || result.transferred == 0) {
            return false;
        }
        cursor += result.transferred;
        size -= static_cast<std::size_t>(result.transferred);
    }
    return true;
}

bool WriteExact(net::Socket &socket, const void *buffer, std::size_t size, Duration timeout) {
    const auto *cursor = static_cast<const std::uint8_t *>(buffer);
    const TimePoint deadline = Clock::now() + timeout;
    while (size > 0) {
        const TimePoint::duration left = deadline - Clock::now();
        if (left <= TimePoint::duration::zero()) {
            return false;
        }
        Duration wait = std::chrono::duration_cast<Duration>(left);
        if (wait <= Duration::zero()) {
            wait = Duration(1);
        }
        const net::SocketResult result = socket.send(cursor, size, 0, wait);
        if (!result.ok() || result.transferred <= 0) {
            return false;
        }
        cursor += result.transferred;
        size -= static_cast<std::size_t>(result.transferred);
    }
    return true;
}

bool ReadFrame(net::Socket &socket, std::size_t max_bytes, std::vector<std::uint8_t> &payload,
               Duration timeout) {
    std::array<std::uint8_t, kFrameHeaderBytes> header{};
    if (!ReadExact(socket, header.data(), header.size(), timeout)) {
        return false;
    }
    const std::uint32_t length = (static_cast<std::uint32_t>(header[0]) << 24) |
                                 (static_cast<std::uint32_t>(header[1]) << 16) |
                                 (static_cast<std::uint32_t>(header[2]) << 8) |
                                 static_cast<std::uint32_t>(header[3]);
    if (length == 0 || length > max_bytes) {
        return false;
    }
    payload.resize(length);
    return ReadExact(socket, payload.data(), payload.size(), timeout);
}

bool WriteFrame(net::Socket &socket, const std::vector<std::uint8_t> &payload,
                Duration timeout) {
    if (payload.empty() || payload.size() > 0xFFFFFFFFU) {
        return false;
    }
    // 头和负载拼成一次 send：分两次发小包会撞上 Nagle + 延迟 ACK，在
    // loopback 上实测每次往返多 40~80ms，租约 TTL 会被这段人为延迟吃掉。
    std::vector<std::uint8_t> frame;
    frame.resize(kFrameHeaderBytes + payload.size());
    const auto length = static_cast<std::uint32_t>(payload.size());
    frame[0] = static_cast<std::uint8_t>(length >> 24);
    frame[1] = static_cast<std::uint8_t>(length >> 16);
    frame[2] = static_cast<std::uint8_t>(length >> 8);
    frame[3] = static_cast<std::uint8_t>(length);
    std::memcpy(frame.data() + kFrameHeaderBytes, payload.data(), payload.size());
    return WriteExact(socket, frame.data(), frame.size(), timeout);
}

void WriteLeaseView(Writer &writer, const LeaseView &view) {
    writer.U64(view.id);
    writer.U8(static_cast<std::uint8_t>(view.kind));
    writer.Str(view.name);
    writer.Str(view.instance);
    writer.Str(view.owner);
    writer.Str(view.endpoint);
    writer.I64(view.ttl.count());
    writer.I64(view.remaining.count());
    writer.U64(view.fencing);
    writer.Bool(view.leader);
    writer.Str(view.leader_id);
    writer.U64(view.epoch);
}

bool ReadLeaseView(Reader &reader, LeaseView &view) {
    std::uint8_t kind = 0;
    std::int64_t ttl = 0;
    std::int64_t remaining = 0;
    if (!reader.U64(view.id) || !reader.U8(kind) || !reader.Str(view.name) ||
        !reader.Str(view.instance) || !reader.Str(view.owner) || !reader.Str(view.endpoint) ||
        !reader.I64(ttl) || !reader.I64(remaining) || !reader.U64(view.fencing) ||
        !reader.Bool(view.leader) || !reader.Str(view.leader_id) || !reader.U64(view.epoch)) {
        return false;
    }
    if (kind > static_cast<std::uint8_t>(LeaseKind::kElection)) {
        return false;
    }
    view.kind = static_cast<LeaseKind>(kind);
    view.ttl = Duration(ttl);
    view.remaining = Duration(remaining);
    return true;
}

void WriteLockInfo(Writer &writer, const LockInfo &info) {
    writer.Str(info.key);
    writer.Str(info.owner);
    writer.U64(info.lease);
    writer.U64(info.fencing);
    writer.I64(info.ttl.count());
    writer.I64(info.remaining.count());
    writer.Bool(info.held);
}

bool ReadLockInfo(Reader &reader, LockInfo &info) {
    std::int64_t ttl = 0;
    std::int64_t remaining = 0;
    if (!reader.Str(info.key) || !reader.Str(info.owner) || !reader.U64(info.lease) ||
        !reader.U64(info.fencing) || !reader.I64(ttl) || !reader.I64(remaining) ||
        !reader.Bool(info.held)) {
        return false;
    }
    info.ttl = Duration(ttl);
    info.remaining = Duration(remaining);
    return true;
}

void WriteElectionState(Writer &writer, const ElectionState &state) {
    writer.Str(state.election);
    writer.Str(state.leader);
    writer.U64(state.epoch);
    writer.U32(static_cast<std::uint32_t>(state.candidates.size()));
    for (const CandidateInfo &candidate : state.candidates) {
        writer.Str(candidate.node);
        writer.U64(candidate.lease);
        writer.I64(candidate.remaining.count());
        writer.Bool(candidate.leader);
    }
}

bool ReadElectionState(Reader &reader, ElectionState &state) {
    std::uint32_t count = 0;
    if (!reader.Str(state.election) || !reader.Str(state.leader) || !reader.U64(state.epoch) ||
        !reader.U32(count)) {
        return false;
    }
    // 防御性上限：正常候选者是几十个，超过 1e6 视为损坏帧。
    if (count > 1'000'000U) {
        return false;
    }
    state.candidates.resize(count);
    for (CandidateInfo &candidate : state.candidates) {
        std::int64_t remaining = 0;
        if (!reader.Str(candidate.node) || !reader.U64(candidate.lease) ||
            !reader.I64(remaining) || !reader.Bool(candidate.leader)) {
            return false;
        }
        candidate.remaining = Duration(remaining);
    }
    return true;
}

std::vector<std::uint8_t> EncodeResponse(Status status, const std::string &message,
                                         const std::vector<std::uint8_t> &fields) {
    Writer writer;
    writer.U8(static_cast<std::uint8_t>(status));
    writer.Str(message);
    writer.Append(fields);
    return writer.Take();
}

/// 从响应 body 中剥出状态与消息，剩余部分交给调用方解析。
bool DecodeResponseHeader(const std::vector<std::uint8_t> &frame, Response &response) {
    Reader reader(frame.data(), frame.size());
    std::uint8_t status = 0;
    if (!reader.U8(status) || !reader.Str(response.message)) {
        return false;
    }
    if (status > static_cast<std::uint8_t>(Status::kClosed)) {
        return false;
    }
    response.status = static_cast<Status>(status);
    response.body.assign(reader.Rest(), reader.Rest() + reader.RestSize());
    return true;
}

bool ValidTtlForCoordinator(Duration ttl) { return ttl > Duration::zero(); }

// ---------------------------------------------------------------------------
// 服务端内部：连接对象与请求分发
// ---------------------------------------------------------------------------

std::atomic<std::uint64_t> g_connection_sequence{1};

class ServerConnection final : public ::go2cpp::streams::Closer {
public:
    ServerConnection(net::Socket::Ptr socket, std::string key)
        : m_socket(std::move(socket)), m_key(std::move(key)) {}

    ::go2cpp::ErrorPtr Close() override {
        if (m_closed.exchange(true)) {
            return {};
        }
        m_socket->close();
        return {};
    }

    const net::Socket::Ptr &socket() const noexcept { return m_socket; }
    const std::string &key() const noexcept { return m_key; }

private:
    net::Socket::Ptr m_socket;
    std::string m_key;
    std::atomic<bool> m_closed{false};
};

} // namespace

// ---------------------------------------------------------------------------
// CoordinatorServer
// ---------------------------------------------------------------------------

struct CoordinatorServer::Impl {
    ICoordinator *coordinator{nullptr};
    CoordinatorServerOptions options;

    mutable std::mutex mutex;
    net::Socket::Ptr listener;
    std::thread accept_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::atomic<std::uint16_t> bound_port{0};
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::size_t> connections{0};
    relay::Hub hub;

    std::mutex workers_mutex;
    std::condition_variable workers_cv;
    std::size_t workers{0};

    void AcceptLoop();
    void Serve(const std::shared_ptr<ServerConnection> &connection);
    std::vector<std::uint8_t> Dispatch(const std::vector<std::uint8_t> &payload);
};

void CoordinatorServer::Impl::AcceptLoop() {
    while (!stopping.load(std::memory_order_acquire)) {
        net::Socket::Ptr socket = listener->accept(options.accept_poll);
        if (!socket) {
            // 超时/临时错误：回到循环顶部检查 stopping；监听 socket 被
            // Stop() 关闭后 accept 也会失败，循环随即退出。
            continue;
        }
        socket->set_recv_timeout(options.io_timeout);
        socket->set_send_timeout(options.io_timeout);
        const std::string key = "conn:" + std::to_string(g_connection_sequence.fetch_add(1));
        auto connection = std::make_shared<ServerConnection>(socket, key);
        hub.Register(key, connection);
        connections.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(workers_mutex);
            ++workers;
        }
        // detach + 计数：Stop() 关闭连接后等待 workers 归零，因此无需保存
        // 每个 std::thread 的句柄（保存反而要处理线程自移除的竞态）。
        std::thread([this, connection] { Serve(connection); }).detach();
    }
}

void CoordinatorServer::Impl::Serve(const std::shared_ptr<ServerConnection> &connection) {
    while (!stopping.load(std::memory_order_acquire)) {
        std::vector<std::uint8_t> payload;
        if (!ReadFrame(*connection->socket(), options.max_frame_bytes, payload,
                       options.io_timeout)) {
            break;
        }
        requests.fetch_add(1, std::memory_order_relaxed);
        std::vector<std::uint8_t> reply = Dispatch(payload);
        if (reply.empty() ||
            !WriteFrame(*connection->socket(), reply, options.io_timeout)) {
            break;
        }
    }
    hub.Unregister(connection->key(), connection);
    connection->Close();
    connections.fetch_sub(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(workers_mutex);
        --workers;
    }
    workers_cv.notify_all();
}

std::vector<std::uint8_t> CoordinatorServer::Impl::Dispatch(
    const std::vector<std::uint8_t> &payload) {
    Reader reader(payload.data(), payload.size());
    std::uint8_t op = 0;
    if (!reader.U8(op)) {
        return EncodeResponse(Status::kProtocolError, "empty request", {});
    }

    Status status = Status::kOk;
    std::string message;
    Writer fields;
    try {
        switch (static_cast<Op>(op)) {
        case Op::kPing:
            break;
        case Op::kStats: {
            const CoordinatorStats stats = coordinator->Stats();
            fields.U64(stats.active_leases);
            fields.U64(stats.services);
            fields.U64(stats.instances);
            fields.U64(stats.locks);
            fields.U64(stats.held_locks);
            fields.U64(stats.elections);
            fields.U64(stats.candidates);
            fields.U64(stats.expired_total);
            fields.U64(stats.sweeps);
            break;
        }
        case Op::kRegister: {
            ServiceRegistration registration;
            std::int64_t ttl = 0;
            if (!reader.Str(registration.service) || !reader.Str(registration.instance) ||
                !reader.Str(registration.endpoint) || !reader.Str(registration.owner) ||
                !reader.I64(ttl)) {
                status = Status::kProtocolError;
                message = "malformed register request";
                break;
            }
            if (!ValidTtlForCoordinator(Duration(ttl))) {
                status = Status::kInvalidArgument;
                message = "invalid ttl";
                break;
            }
            registration.ttl = Duration(ttl);
            const Result<LeaseView> result = coordinator->RegisterService(registration);
            status = result.status;
            message = result.message;
            if (result.ok()) {
                WriteLeaseView(fields, result.value);
            }
            break;
        }
        case Op::kRenew: {
            LeaseId lease = kInvalidLease;
            std::string owner;
            if (!reader.U64(lease) || !reader.Str(owner)) {
                status = Status::kProtocolError;
                message = "malformed renew request";
                break;
            }
            const Result<LeaseView> result = coordinator->RenewLease(lease, owner);
            status = result.status;
            message = result.message;
            if (result.ok()) {
                WriteLeaseView(fields, result.value);
            }
            break;
        }
        case Op::kRelease: {
            LeaseId lease = kInvalidLease;
            std::string owner;
            if (!reader.U64(lease) || !reader.Str(owner)) {
                status = Status::kProtocolError;
                message = "malformed release request";
                break;
            }
            const Outcome outcome = coordinator->ReleaseLease(lease, owner);
            status = outcome.status;
            message = outcome.message;
            break;
        }
        case Op::kLookup: {
            std::string service;
            if (!reader.Str(service)) {
                status = Status::kProtocolError;
                message = "malformed lookup request";
                break;
            }
            const std::vector<ServiceInstance> instances = coordinator->LookupService(service);
            fields.U32(static_cast<std::uint32_t>(instances.size()));
            for (const ServiceInstance &instance : instances) {
                fields.Str(instance.service);
                fields.Str(instance.instance);
                fields.Str(instance.endpoint);
                fields.Str(instance.owner);
                fields.U64(instance.lease);
                fields.I64(instance.ttl.count());
                fields.I64(instance.remaining.count());
            }
            break;
        }
        case Op::kListServices: {
            const std::vector<std::string> services = coordinator->ListServices();
            fields.U32(static_cast<std::uint32_t>(services.size()));
            for (const std::string &service : services) {
                fields.Str(service);
            }
            break;
        }
        case Op::kAcquireLock: {
            std::string key;
            std::string owner;
            std::int64_t ttl = 0;
            if (!reader.Str(key) || !reader.Str(owner) || !reader.I64(ttl)) {
                status = Status::kProtocolError;
                message = "malformed acquire request";
                break;
            }
            if (!ValidTtlForCoordinator(Duration(ttl))) {
                status = Status::kInvalidArgument;
                message = "invalid ttl";
                break;
            }
            const Result<LeaseView> result = coordinator->AcquireLock(key, owner, Duration(ttl));
            status = result.status;
            message = result.message;
            if (result.ok()) {
                WriteLeaseView(fields, result.value);
            }
            break;
        }
        case Op::kQueryLock: {
            std::string key;
            if (!reader.Str(key)) {
                status = Status::kProtocolError;
                message = "malformed query request";
                break;
            }
            const std::optional<LockInfo> info = coordinator->QueryLock(key);
            fields.Bool(info.has_value());
            if (info.has_value()) {
                WriteLockInfo(fields, *info);
            }
            break;
        }
        case Op::kListLocks: {
            const std::vector<LockInfo> locks = coordinator->ListLocks();
            fields.U32(static_cast<std::uint32_t>(locks.size()));
            for (const LockInfo &info : locks) {
                WriteLockInfo(fields, info);
            }
            break;
        }
        case Op::kJoinElection: {
            std::string election;
            std::string candidate;
            std::int64_t ttl = 0;
            if (!reader.Str(election) || !reader.Str(candidate) || !reader.I64(ttl)) {
                status = Status::kProtocolError;
                message = "malformed join request";
                break;
            }
            if (!ValidTtlForCoordinator(Duration(ttl))) {
                status = Status::kInvalidArgument;
                message = "invalid ttl";
                break;
            }
            const Result<LeaseView> result =
                coordinator->JoinElection(election, candidate, Duration(ttl));
            status = result.status;
            message = result.message;
            if (result.ok()) {
                WriteLeaseView(fields, result.value);
            }
            break;
        }
        case Op::kQueryElection: {
            std::string election;
            if (!reader.Str(election)) {
                status = Status::kProtocolError;
                message = "malformed query request";
                break;
            }
            WriteElectionState(fields, coordinator->QueryElection(election));
            break;
        }
        case Op::kListElections: {
            const std::vector<std::string> elections = coordinator->ListElections();
            fields.U32(static_cast<std::uint32_t>(elections.size()));
            for (const std::string &election : elections) {
                fields.Str(election);
            }
            break;
        }
        case Op::kSweep: {
            fields.U64(coordinator->SweepExpired());
            break;
        }
        default:
            status = Status::kProtocolError;
            message = "unknown op";
            break;
        }
    } catch (const std::exception &exception) {
        status = Status::kUnavailable;
        message = exception.what();
    } catch (...) {
        status = Status::kUnavailable;
        message = "coordinator threw an unknown exception";
    }
    return EncodeResponse(status, message, fields.data());
}

CoordinatorServer::CoordinatorServer(ICoordinator &coordinator, CoordinatorServerOptions options)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->coordinator = &coordinator;
    m_impl->options = std::move(options);
}

CoordinatorServer::~CoordinatorServer() { Stop(); }

Outcome CoordinatorServer::Start() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->running.load()) {
        return Outcome::Failure(Status::kClosed, "server already running");
    }
    m_impl->listener = net::ListenTCP(m_impl->options.host, m_impl->options.port,
                                      m_impl->options.backlog);
    if (!m_impl->listener) {
        return Outcome::Failure(Status::kUnavailable,
                                "failed to listen on " + m_impl->options.host + ":" +
                                    std::to_string(m_impl->options.port));
    }
    std::uint16_t port = m_impl->options.port;
    if (const auto ipv4 =
            std::dynamic_pointer_cast<net::IPv4Address>(m_impl->listener->local_address())) {
        port = ipv4->port();
    } else if (const auto ipv6 = std::dynamic_pointer_cast<net::IPv6Address>(
                   m_impl->listener->local_address())) {
        port = ipv6->port();
    }
    m_impl->bound_port.store(port);
    m_impl->stopping.store(false);
    m_impl->running.store(true);
    m_impl->accept_thread = std::thread([impl = m_impl.get()] { impl->AcceptLoop(); });
    return Outcome::Success();
}

void CoordinatorServer::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->running.load() && !m_impl->accept_thread.joinable()) {
            return;
        }
        m_impl->running.store(false);
        m_impl->stopping.store(true);
    }
    if (m_impl->listener) {
        // close 会取消 accept 的等待（Socket 内部有跨线程取消代际）。
        m_impl->listener->close();
    }
    for (const std::string &key : m_impl->hub.Keys()) {
        m_impl->hub.CloseKey(key);
    }
    if (m_impl->accept_thread.joinable()) {
        m_impl->accept_thread.join();
    }
    {
        // 不设超时：连接已被 Hub 关闭，每个处理线程最多再阻塞一次 I/O 超时
        // （默认 2s）就会退出。这里必须等它们全部归零——否则 Impl 析构后
        // 仍可能有 detached 线程访问 coordinator/hub（use-after-free）。
        std::unique_lock<std::mutex> lock(m_impl->workers_mutex);
        m_impl->workers_cv.wait(lock, [this] { return m_impl->workers == 0; });
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->listener.reset();
    m_impl->bound_port.store(0);
}

bool CoordinatorServer::running() const { return m_impl->running.load(); }

std::uint16_t CoordinatorServer::port() const { return m_impl->bound_port.load(); }

std::size_t CoordinatorServer::active_connections() const {
    return m_impl->connections.load();
}

std::uint64_t CoordinatorServer::requests_served() const { return m_impl->requests.load(); }

const CoordinatorServerOptions &CoordinatorServer::options() const { return m_impl->options; }

// ---------------------------------------------------------------------------
// CoordinatorClient
// ---------------------------------------------------------------------------

struct CoordinatorClient::Impl {
    std::string host;
    std::uint16_t port{0};
    CoordinatorClientOptions options;

    mutable std::mutex mutex;
    net::Socket::Ptr socket;
    bool closed{false};

    bool ConnectLocked() {
        if (socket) {
            return true;
        }
        socket = net::DialTCP(host, port, options.connect_timeout);
        if (!socket) {
            return false;
        }
        // 客户端侧也必须关 Nagle：协调者请求都是几十字节的小包，分两次
        // 写的小包在 loopback 上会被延迟 ACK 压成 40ms 级往返。
        const int no_delay = 1;
        socket->set_option(IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
        socket->set_recv_timeout(options.io_timeout);
        socket->set_send_timeout(options.io_timeout);
        return true;
    }

    void CloseLocked() {
        if (socket) {
            socket->close();
            socket.reset();
        }
    }

    Outcome Connect() {
        std::lock_guard<std::mutex> lock(mutex);
        closed = false;
        if (!ConnectLocked()) {
            return Outcome::Failure(Status::kUnavailable,
                                    "connect " + host + ":" + std::to_string(port) + " failed");
        }
        return Outcome::Success();
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mutex);
        closed = true;
        CloseLocked();
    }

    bool Connected() const {
        std::lock_guard<std::mutex> lock(mutex);
        return socket != nullptr;
    }

    Response Call(Op op, const Writer &request) {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed) {
            return Response{Status::kClosed, "client closed", {}};
        }
        if (!ConnectLocked()) {
            return Response{Status::kUnavailable, "connect failed", {}};
        }
        std::vector<std::uint8_t> payload;
        payload.reserve(request.data().size() + 1);
        payload.push_back(static_cast<std::uint8_t>(op));
        payload.insert(payload.end(), request.data().begin(), request.data().end());
        if (!WriteFrame(*socket, payload, options.io_timeout)) {
            CloseLocked();
            return Response{Status::kUnavailable, "write failed", {}};
        }
        std::vector<std::uint8_t> frame;
        if (!ReadFrame(*socket, options.max_frame_bytes, frame, options.io_timeout)) {
            CloseLocked();
            return Response{Status::kUnavailable, "read failed or frame too large", {}};
        }
        Response response;
        if (!DecodeResponseHeader(frame, response)) {
            CloseLocked();
            return Response{Status::kProtocolError, "malformed response", {}};
        }
        return response;
    }
};

CoordinatorClient::CoordinatorClient(std::string host, std::uint16_t port,
                                     CoordinatorClientOptions options)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->host = std::move(host);
    m_impl->port = port;
    m_impl->options = std::move(options);
}

CoordinatorClient::~CoordinatorClient() { Close(); }

Outcome CoordinatorClient::Connect() { return m_impl->Connect(); }

void CoordinatorClient::Close() { m_impl->Close(); }

bool CoordinatorClient::connected() const { return m_impl->Connected(); }

const std::string &CoordinatorClient::host() const { return m_impl->host; }

std::uint16_t CoordinatorClient::port() const { return m_impl->port; }

Result<LeaseView> CoordinatorClient::RegisterService(const ServiceRegistration &registration) {
    Writer request;
    request.Str(registration.service);
    request.Str(registration.instance);
    request.Str(registration.endpoint);
    request.Str(registration.owner);
    request.I64(registration.ttl.count());
    const Response response = m_impl->Call(Op::kRegister, request);
    if (response.status != Status::kOk) {
        return Result<LeaseView>::Failure(response.status, response.message);
    }
    Reader reader(response.body.data(), response.body.size());
    LeaseView view;
    if (!ReadLeaseView(reader, view)) {
        return Result<LeaseView>::Failure(Status::kProtocolError, "malformed lease view");
    }
    return Result<LeaseView>::Success(std::move(view));
}

Result<LeaseView> CoordinatorClient::RenewLease(LeaseId lease, std::string_view owner) {
    Writer request;
    request.U64(lease);
    request.Str(owner);
    const Response response = m_impl->Call(Op::kRenew, request);
    if (response.status != Status::kOk) {
        return Result<LeaseView>::Failure(response.status, response.message);
    }
    Reader reader(response.body.data(), response.body.size());
    LeaseView view;
    if (!ReadLeaseView(reader, view)) {
        return Result<LeaseView>::Failure(Status::kProtocolError, "malformed lease view");
    }
    return Result<LeaseView>::Success(std::move(view));
}

Outcome CoordinatorClient::ReleaseLease(LeaseId lease, std::string_view owner) {
    Writer request;
    request.U64(lease);
    request.Str(owner);
    const Response response = m_impl->Call(Op::kRelease, request);
    return Outcome{response.status, response.message};
}

std::vector<ServiceInstance> CoordinatorClient::LookupService(std::string_view service) const {
    Writer request;
    request.Str(service);
    const Response response = m_impl->Call(Op::kLookup, request);
    std::vector<ServiceInstance> instances;
    if (response.status != Status::kOk) {
        return instances;
    }
    Reader reader(response.body.data(), response.body.size());
    std::uint32_t count = 0;
    if (!reader.U32(count) || count > 1'000'000U) {
        return instances;
    }
    instances.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        ServiceInstance instance;
        std::int64_t ttl = 0;
        std::int64_t remaining = 0;
        if (!reader.Str(instance.service) || !reader.Str(instance.instance) ||
            !reader.Str(instance.endpoint) || !reader.Str(instance.owner) ||
            !reader.U64(instance.lease) || !reader.I64(ttl) || !reader.I64(remaining)) {
            instances.clear();
            return instances;
        }
        instance.ttl = Duration(ttl);
        instance.remaining = Duration(remaining);
        instances.push_back(std::move(instance));
    }
    return instances;
}

std::vector<std::string> CoordinatorClient::ListServices() const {
    Writer request;
    const Response response = m_impl->Call(Op::kListServices, request);
    std::vector<std::string> services;
    if (response.status != Status::kOk) {
        return services;
    }
    Reader reader(response.body.data(), response.body.size());
    std::uint32_t count = 0;
    if (!reader.U32(count) || count > 1'000'000U) {
        return services;
    }
    services.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::string service;
        if (!reader.Str(service)) {
            services.clear();
            return services;
        }
        services.push_back(std::move(service));
    }
    return services;
}

Result<LeaseView> CoordinatorClient::AcquireLock(std::string_view key, std::string_view owner,
                                                 Duration ttl) {
    Writer request;
    request.Str(key);
    request.Str(owner);
    request.I64(ttl.count());
    const Response response = m_impl->Call(Op::kAcquireLock, request);
    if (response.status != Status::kOk) {
        return Result<LeaseView>::Failure(response.status, response.message);
    }
    Reader reader(response.body.data(), response.body.size());
    LeaseView view;
    if (!ReadLeaseView(reader, view)) {
        return Result<LeaseView>::Failure(Status::kProtocolError, "malformed lease view");
    }
    return Result<LeaseView>::Success(std::move(view));
}

std::optional<LockInfo> CoordinatorClient::QueryLock(std::string_view key) const {
    Writer request;
    request.Str(key);
    const Response response = m_impl->Call(Op::kQueryLock, request);
    if (response.status != Status::kOk) {
        return std::nullopt;
    }
    Reader reader(response.body.data(), response.body.size());
    bool present = false;
    if (!reader.Bool(present) || !present) {
        return std::nullopt;
    }
    LockInfo info;
    if (!ReadLockInfo(reader, info)) {
        return std::nullopt;
    }
    return info;
}

std::vector<LockInfo> CoordinatorClient::ListLocks() const {
    Writer request;
    const Response response = m_impl->Call(Op::kListLocks, request);
    std::vector<LockInfo> locks;
    if (response.status != Status::kOk) {
        return locks;
    }
    Reader reader(response.body.data(), response.body.size());
    std::uint32_t count = 0;
    if (!reader.U32(count) || count > 1'000'000U) {
        return locks;
    }
    locks.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        LockInfo info;
        if (!ReadLockInfo(reader, info)) {
            locks.clear();
            return locks;
        }
        locks.push_back(std::move(info));
    }
    return locks;
}

Result<LeaseView> CoordinatorClient::JoinElection(std::string_view election,
                                                  std::string_view candidate, Duration ttl) {
    Writer request;
    request.Str(election);
    request.Str(candidate);
    request.I64(ttl.count());
    const Response response = m_impl->Call(Op::kJoinElection, request);
    if (response.status != Status::kOk) {
        return Result<LeaseView>::Failure(response.status, response.message);
    }
    Reader reader(response.body.data(), response.body.size());
    LeaseView view;
    if (!ReadLeaseView(reader, view)) {
        return Result<LeaseView>::Failure(Status::kProtocolError, "malformed lease view");
    }
    return Result<LeaseView>::Success(std::move(view));
}

ElectionState CoordinatorClient::QueryElection(std::string_view election) const {
    Writer request;
    request.Str(election);
    const Response response = m_impl->Call(Op::kQueryElection, request);
    ElectionState state;
    if (response.status != Status::kOk) {
        return state;
    }
    Reader reader(response.body.data(), response.body.size());
    if (!ReadElectionState(reader, state)) {
        return {};
    }
    return state;
}

std::vector<std::string> CoordinatorClient::ListElections() const {
    Writer request;
    const Response response = m_impl->Call(Op::kListElections, request);
    std::vector<std::string> elections;
    if (response.status != Status::kOk) {
        return elections;
    }
    Reader reader(response.body.data(), response.body.size());
    std::uint32_t count = 0;
    if (!reader.U32(count) || count > 1'000'000U) {
        return elections;
    }
    elections.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::string election;
        if (!reader.Str(election)) {
            elections.clear();
            return elections;
        }
        elections.push_back(std::move(election));
    }
    return elections;
}

std::size_t CoordinatorClient::SweepExpired() {
    Writer request;
    const Response response = m_impl->Call(Op::kSweep, request);
    if (response.status != Status::kOk) {
        return 0;
    }
    Reader reader(response.body.data(), response.body.size());
    std::uint64_t swept = 0;
    if (!reader.U64(swept)) {
        return 0;
    }
    return static_cast<std::size_t>(swept);
}

CoordinatorStats CoordinatorClient::Stats() const {
    Writer request;
    const Response response = m_impl->Call(Op::kStats, request);
    CoordinatorStats stats;
    if (response.status != Status::kOk) {
        return stats;
    }
    Reader reader(response.body.data(), response.body.size());
    std::uint64_t values[9]{};
    for (std::uint64_t &value : values) {
        if (!reader.U64(value)) {
            return {};
        }
    }
    stats.active_leases = static_cast<std::size_t>(values[0]);
    stats.services = static_cast<std::size_t>(values[1]);
    stats.instances = static_cast<std::size_t>(values[2]);
    stats.locks = static_cast<std::size_t>(values[3]);
    stats.held_locks = static_cast<std::size_t>(values[4]);
    stats.elections = static_cast<std::size_t>(values[5]);
    stats.candidates = static_cast<std::size_t>(values[6]);
    stats.expired_total = values[7];
    stats.sweeps = values[8];
    return stats;
}

} // namespace go2cpp::distributed
