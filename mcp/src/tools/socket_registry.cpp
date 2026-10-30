/**
 * @file socket_registry.cpp
 * @brief 本进程 socket 注册表/探测/事件/审计实现（Linux：/proc + TCP_INFO）。
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "mcp/tools/socket_registry.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/tcp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace go2cpp::mcp::tools {

namespace {

/// Unix 毫秒时间戳（system_clock，可跨机器比较）。
std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// ISO-8601 UTC（审计记录用；秒级精度足够）。
std::string NowIso8601() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    if (::gmtime_r(&now, &utc) == nullptr) {
        return "1970-01-01T00:00:00Z";
    }
    char buffer[32] = {};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return "1970-01-01T00:00:00Z";
    }
    return buffer;
}

/// errno 的可读文本（含数值，便于审计复核）。
std::string ErrnoText(int error) {
    if (error == 0) {
        return "无错误";
    }
    return std::string(std::strerror(error)) + " (errno=" +
           std::to_string(error) + ")";
}

// Linux 内核 TCP 状态值（include/net/tcp_states.h，ABI 稳定）。
// 不能直接用 <netinet/tcp.h> 的 `TCP_ESTABLISHED` 等枚举：该头与
// <linux/tcp.h> 的 `struct tcp_info` 定义冲突，而扩展字节字段只在后者中。
constexpr std::uint8_t kTcpStateEstablished = 1;
constexpr std::uint8_t kTcpStateSynSent = 2;
constexpr std::uint8_t kTcpStateSynRecv = 3;
constexpr std::uint8_t kTcpStateFinWait1 = 4;
constexpr std::uint8_t kTcpStateFinWait2 = 5;
constexpr std::uint8_t kTcpStateTimeWait = 6;
constexpr std::uint8_t kTcpStateClose = 7;
constexpr std::uint8_t kTcpStateCloseWait = 8;
constexpr std::uint8_t kTcpStateLastAck = 9;
constexpr std::uint8_t kTcpStateListen = 10;
constexpr std::uint8_t kTcpStateClosing = 11;
constexpr std::uint8_t kTcpStateNewSynRecv = 12;

/// TCP 内核状态名；未知值返回 "unknown"。
const char* TcpStateName(std::uint8_t state) noexcept {
    switch (state) {
        case kTcpStateEstablished:
            return "established";
        case kTcpStateSynSent:
            return "syn_sent";
        case kTcpStateSynRecv:
            return "syn_recv";
        case kTcpStateFinWait1:
            return "fin_wait1";
        case kTcpStateFinWait2:
            return "fin_wait2";
        case kTcpStateTimeWait:
            return "time_wait";
        case kTcpStateClose:
            return "close";
        case kTcpStateCloseWait:
            return "close_wait";
        case kTcpStateLastAck:
            return "last_ack";
        case kTcpStateListen:
            return "listen";
        case kTcpStateClosing:
            return "closing";
        case kTcpStateNewSynRecv:
            return "new_syn_recv";
        default:
            return "unknown";
    }
}

/// 把 sockaddr 转成数值地址；AF_UNIX 输出路径/抽象名。
std::string FormatAddress(const sockaddr* address, socklen_t length) {
    if (address == nullptr) {
        return {};
    }
    if (address->sa_family == AF_UNIX) {
        const auto* unix_address = reinterpret_cast<const sockaddr_un*>(address);
        const std::size_t base = offsetof(sockaddr_un, sun_path);
        const std::size_t capacity =
            length > base ? static_cast<std::size_t>(length) - base : 0;
        const std::size_t limit = std::min(capacity, sizeof(unix_address->sun_path));
        if (limit == 0) {
            return "unnamed";
        }
        if (unix_address->sun_path[0] == '\0') {
            std::string name;
            for (std::size_t index = 1; index < limit; ++index) {
                const char character = unix_address->sun_path[index];
                if (character == '\0') {
                    break;
                }
                name.push_back(character);
            }
            return name.empty() ? "unnamed" : "@" + name;
        }
        std::size_t used = 0;
        while (used < limit && unix_address->sun_path[used] != '\0') {
            ++used;
        }
        return std::string(unix_address->sun_path, used);
    }

    char host[NI_MAXHOST] = {};
    char service[NI_MAXSERV] = {};
    if (::getnameinfo(address, length, host, sizeof(host), service,
                      sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "unknown";
    }
    return std::string(host) + ":" + service;
}

/// `socket:[inode]` 的解析结果。
struct Probe {
    SocketProtocol protocol{SocketProtocol::kUnknown};
    SocketState state{SocketState::kUnknown};
    std::string inode;
    std::string local_address;
    std::string remote_address;
};

/**
 * @brief 探测一个 fd 是否为本进程 socket，并读取协议/状态/地址/inode。
 * @return 是 socket 返回 true；否则 false 并写 `*error`。
 */
bool ProbeFd(int fd, Probe* probe, std::string* error) {
    if (probe == nullptr) {
        if (error != nullptr) {
            *error = "内部错误：probe 为空";
        }
        return false;
    }
    if (fd < 0) {
        if (error != nullptr) {
            *error = "fd 必须是非负整数";
        }
        return false;
    }

    char link_path[64] = {};
    std::snprintf(link_path, sizeof(link_path), "/proc/self/fd/%d", fd);
    char target[256] = {};
    const ssize_t length = ::readlink(link_path, target, sizeof(target) - 1);
    if (length <= 0) {
        if (error != nullptr) {
            *error = "fd " + std::to_string(fd) + " 不属于当前进程或已关闭: " +
                     ErrnoText(errno);
        }
        return false;
    }
    target[length] = '\0';
    const std::string link(target);
    if (link.rfind("socket:[", 0) != 0 || link.size() < 10 ||
        link.back() != ']') {
        if (error != nullptr) {
            *error = "fd " + std::to_string(fd) + " 不是 socket（" + link + "）";
        }
        return false;
    }
    probe->inode = link.substr(8, link.size() - 9);

    int type = 0;
    socklen_t type_length = sizeof(type);
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_length) != 0) {
        if (error != nullptr) {
            *error = "fd " + std::to_string(fd) + " 读取 SO_TYPE 失败: " +
                     ErrnoText(errno);
        }
        return false;
    }

    sockaddr_storage local{};
    socklen_t local_length = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_length) !=
        0) {
        if (error != nullptr) {
            *error = "fd " + std::to_string(fd) + " 读取本地地址失败: " +
                     ErrnoText(errno);
        }
        return false;
    }

    const int family = reinterpret_cast<sockaddr*>(&local)->sa_family;
    if (family == AF_INET || family == AF_INET6) {
        if (type == SOCK_STREAM) {
            probe->protocol = SocketProtocol::kTcp;
        } else if (type == SOCK_DGRAM) {
            probe->protocol = SocketProtocol::kUdp;
        }
    } else if (family == AF_UNIX) {
        if (type == SOCK_STREAM) {
            probe->protocol = SocketProtocol::kUnixStream;
        } else if (type == SOCK_DGRAM) {
            probe->protocol = SocketProtocol::kUnixDatagram;
        }
    }
    probe->local_address =
        FormatAddress(reinterpret_cast<sockaddr*>(&local), local_length);

    int accepting = 0;
    socklen_t accepting_length = sizeof(accepting);
    if (::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting,
                     &accepting_length) == 0 &&
        accepting != 0) {
        probe->state = SocketState::kListening;
        return true;
    }

    sockaddr_storage remote{};
    socklen_t remote_length = sizeof(remote);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&remote), &remote_length) ==
        0) {
        probe->state = SocketState::kConnected;
        probe->remote_address =
            FormatAddress(reinterpret_cast<sockaddr*>(&remote), remote_length);
    } else {
        probe->state = SocketState::kUnconnected;
    }
    return true;
}

/// 读取内核 TCP_INFO；非 TCP/失败返回 false（`available` 保持 false）。
bool ReadTcpInfo(int fd, SocketTcpInfo* info) {
    if (info == nullptr) {
        return false;
    }
    tcp_info raw{};
    socklen_t length = sizeof(raw);
    if (::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &raw, &length) != 0) {
        return false;
    }
    info->available = true;
    info->sent_bytes = raw.tcpi_bytes_acked;
    info->received_bytes = raw.tcpi_bytes_received;
    info->retransmits = raw.tcpi_total_retrans;
    info->rtt_us = raw.tcpi_rtt;
    info->state = TcpStateName(raw.tcpi_state);
    return true;
}

/// `/proc/self/fd` 中扫描到的一个 socket。
struct ScannedSocket {
    int fd{-1};
    std::string inode;
};

/**
 * @brief 枚举本进程全部 socket fd。
 * @return 读目录成功返回 true；失败写 `*error`（仍可能是空列表）。
 */
bool ScanProcessSockets(std::vector<ScannedSocket>* sockets, std::string* error) {
    DIR* directory = ::opendir("/proc/self/fd");
    if (directory == nullptr) {
        if (error != nullptr) {
            *error = "无法读取 /proc/self/fd: " + ErrnoText(errno);
        }
        return false;
    }
    const int directory_fd = ::dirfd(directory);
    while (const dirent* item = ::readdir(directory)) {
        if (item->d_name[0] == '.') {
            continue;
        }
        char* end = nullptr;
        errno = 0;
        const long value = std::strtol(item->d_name, &end, 10);
        if (errno != 0 || end == item->d_name || *end != '\0' || value < 0) {
            continue;
        }
        const int fd = static_cast<int>(value);
        if (fd == directory_fd) {
            continue;
        }
        char link_path[64] = {};
        std::snprintf(link_path, sizeof(link_path), "/proc/self/fd/%d", fd);
        char target[256] = {};
        const ssize_t length = ::readlink(link_path, target, sizeof(target) - 1);
        if (length <= 0) {
            continue;  // fd 在扫描期间被关闭：竞态是正常的
        }
        target[length] = '\0';
        const std::string link(target);
        if (link.rfind("socket:[", 0) == 0 && link.size() >= 10 &&
            link.back() == ']') {
            sockets->push_back(ScannedSocket{fd, link.substr(8, link.size() - 9)});
        }
    }
    ::closedir(directory);
    std::sort(sockets->begin(), sockets->end(),
              [](const ScannedSocket& left, const ScannedSocket& right) {
                  return left.fd < right.fd;
              });
    return true;
}

}  // namespace

const char* SocketProtocolName(SocketProtocol protocol) noexcept {
    switch (protocol) {
        case SocketProtocol::kTcp:
            return "tcp";
        case SocketProtocol::kUdp:
            return "udp";
        case SocketProtocol::kUnixStream:
            return "unix_stream";
        case SocketProtocol::kUnixDatagram:
            return "unix_datagram";
        case SocketProtocol::kUnknown:
        default:
            return "unknown";
    }
}

const char* SocketStateName(SocketState state) noexcept {
    switch (state) {
        case SocketState::kListening:
            return "listening";
        case SocketState::kConnected:
            return "connected";
        case SocketState::kUnconnected:
            return "unconnected";
        case SocketState::kClosed:
            return "closed";
        case SocketState::kUnknown:
        default:
            return "unknown";
    }
}

const char* SocketEventKindName(SocketEventKind kind) noexcept {
    switch (kind) {
        case SocketEventKind::kRegistered:
            return "registered";
        case SocketEventKind::kConnected:
            return "connected";
        case SocketEventKind::kRead:
            return "read";
        case SocketEventKind::kWrite:
            return "write";
        case SocketEventKind::kError:
            return "error";
        case SocketEventKind::kTimeout:
            return "timeout";
        case SocketEventKind::kCloseRequested:
            return "close_requested";
        case SocketEventKind::kClosed:
            return "closed";
        default:
            return "unknown";
    }
}

json SocketEvent::ToJson() const {
    json value = json::object();
    value["kind"] = SocketEventKindName(kind);
    value["timestamp_ms"] = timestamp_ms;
    value["bytes"] = bytes;
    value["error"] = error;
    value["detail"] = detail;
    return value;
}

json SocketSnapshot::ToJson() const {
    json value = json::object();
    value["fd"] = fd;
    value["protocol"] = SocketProtocolName(protocol);
    value["state"] = SocketStateName(state);
    value["local"] = local_address;
    value["remote"] = remote_address;
    value["label"] = label;
    value["inode"] = inode;
    value["registered"] = registered;
    value["closable"] = closable;
    value["registered_at_ms"] = registered_at_ms;
    return value;
}

json SocketTcpInfo::ToJson() const {
    json value = json::object();
    value["available"] = available;
    value["sent_bytes"] = sent_bytes;
    value["received_bytes"] = received_bytes;
    value["retransmits"] = retransmits;
    value["rtt_us"] = rtt_us;
    value["state"] = state;
    return value;
}

json SocketStats::ToJson() const {
    json value = json::object();
    value["fd"] = fd;
    value["protocol"] = SocketProtocolName(protocol);
    value["state"] = SocketStateName(state);
    value["registered"] = registered;
    // 应用层埋点与内核计数分开展示；顶层取两者较大值，不相加（可能重复计数）。
    value["sent_bytes"] =
        tcp.available ? std::max(tcp.sent_bytes, app.sent_bytes) : app.sent_bytes;
    value["recv_bytes"] = tcp.available
                              ? std::max(tcp.received_bytes, app.recv_bytes)
                              : app.recv_bytes;
    value["app_sent_bytes"] = app.sent_bytes;
    value["app_recv_bytes"] = app.recv_bytes;
    value["errors"] = app.errors;
    value["timeouts"] = app.timeouts;
    value["tcp"] = tcp.ToJson();
    value["events"] = events;
    value["dropped_events"] = dropped_events;
    value["last_event_ms"] = last_event_ms;
    return value;
}

FileSocketAuditSink::FileSocketAuditSink(std::string path)
    : m_path(std::move(path)) {}

bool FileSocketAuditSink::Append(const json& record, std::string* error) {
    std::string line;
    try {
        line = record.dump(-1, ' ', false, json::error_handler_t::replace);
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("审计记录序列化失败: ") + exception.what();
        }
        return false;
    }
    line.push_back('\n');

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_path.empty()) {
        if (error != nullptr) {
            *error = "审计路径为空";
        }
        return false;
    }
    const std::filesystem::path path(m_path);
    if (path.has_parent_path()) {
        std::error_code directory_error;
        std::filesystem::create_directories(path.parent_path(), directory_error);
        if (directory_error) {
            if (error != nullptr) {
                *error = "创建审计目录失败: " + directory_error.message();
            }
            return false;
        }
    }
    const int fd = ::open(m_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        if (error != nullptr) {
            *error = "打开审计文件失败: " + ErrnoText(errno) + "（" + m_path +
                     "）";
        }
        return false;
    }

    std::size_t offset = 0;
    bool ok = true;
    while (offset < line.size()) {
        const ssize_t written =
            ::write(fd, line.data() + offset, line.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (error != nullptr) {
                *error = "写审计文件失败: " + ErrnoText(errno);
            }
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::close(fd) != 0 && ok) {
        if (error != nullptr) {
            *error = "关闭审计文件失败: " + ErrnoText(errno);
        }
        ok = false;
    }
    return ok;
}

std::string FileSocketAuditSink::Describe() const { return m_path; }

bool MemorySocketAuditSink::Append(const json& record, std::string* error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_failure.empty()) {
        if (error != nullptr) {
            *error = m_failure;
        }
        return false;
    }
    m_records.push_back(record);
    return true;
}

void MemorySocketAuditSink::FailWith(std::string message) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_failure = std::move(message);
}

void MemorySocketAuditSink::ClearFailure() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_failure.clear();
}

void MemorySocketAuditSink::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_records.clear();
    m_failure.clear();
}

std::vector<json> MemorySocketAuditSink::Records() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_records;
}

std::size_t MemorySocketAuditSink::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_records.size();
}

std::string DefaultSocketAuditPath() {
    const char* configured = std::getenv("GO2CPP_MCP_SOCKET_AUDIT");
    if (configured != nullptr && configured[0] != '\0') {
        return configured;
    }
    return "log/mcp-socket-audit.log";
}

SocketRegistry& SocketRegistry::Instance() {
    static SocketRegistry registry;
    return registry;
}

void SocketRegistry::AppendEventLocked(Entry& entry, SocketEventKind kind,
                                       std::uint64_t bytes, int error,
                                       std::string detail) {
    if (entry.events.size() >= kEventCapacity) {
        entry.events.pop_front();
        ++entry.dropped_events;
    }
    SocketEvent event;
    event.kind = kind;
    event.timestamp_ms = NowMs();
    event.bytes = bytes;
    event.error = error;
    event.detail = std::move(detail);
    entry.events.push_back(std::move(event));
}

void SocketRegistry::MarkClosedLocked(Entry& entry, const std::string& detail) {
    if (entry.closed) {
        return;
    }
    entry.closed = true;
    entry.registered = false;
    entry.state = SocketState::kClosed;
    AppendEventLocked(entry, SocketEventKind::kClosed, 0, 0, detail);
}

void SocketRegistry::SampleTcpLocked(Entry& entry, const SocketTcpInfo& tcp) {
    if (!tcp.available || entry.closed) {
        return;
    }
    const std::uint64_t sent =
        tcp.sent_bytes > entry.last_tcp_sent ? tcp.sent_bytes - entry.last_tcp_sent
                                             : 0;
    const std::uint64_t received = tcp.received_bytes > entry.last_tcp_recv
                                       ? tcp.received_bytes - entry.last_tcp_recv
                                       : 0;
    if (sent > 0) {
        AppendEventLocked(entry, SocketEventKind::kWrite, sent, 0, "tcp_info");
    }
    if (received > 0) {
        AppendEventLocked(entry, SocketEventKind::kRead, received, 0, "tcp_info");
    }
    entry.last_tcp_sent = std::max(entry.last_tcp_sent, tcp.sent_bytes);
    entry.last_tcp_recv = std::max(entry.last_tcp_recv, tcp.received_bytes);
    entry.last_tcp = tcp;
}

void SocketRegistry::PruneLocked() {
    while (m_entries.size() >= kMaxEntries) {
        auto oldest = m_entries.end();
        for (auto iterator = m_entries.begin(); iterator != m_entries.end();
             ++iterator) {
            if (!iterator->second.closed) {
                continue;
            }
            if (oldest == m_entries.end() ||
                iterator->second.registered_at_ms <
                    oldest->second.registered_at_ms) {
                oldest = iterator;
            }
        }
        if (oldest == m_entries.end()) {
            return;  // 没有可清理的墓碑
        }
        m_entries.erase(oldest);
    }
}

std::string SocketRegistry::NextAuditIdLocked() {
    ++m_audit_seq;
    return std::to_string(static_cast<long long>(::getpid())) + "-" +
           std::to_string(m_audit_seq);
}

bool SocketRegistry::Register(int fd, std::string label, std::string* error) {
    Probe probe;
    if (!ProbeFd(fd, &probe, error)) {
        return false;
    }
    SocketTcpInfo tcp;
    if (probe.protocol == SocketProtocol::kTcp) {
        (void)ReadTcpInfo(fd, &tcp);
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_entries.size() >= kMaxEntries) {
        PruneLocked();
        if (m_entries.size() >= kMaxEntries) {
            if (error != nullptr) {
                *error = "socket 注册表条目已达上限（" +
                         std::to_string(kMaxEntries) + "），请先关闭旧 socket";
            }
            return false;
        }
    }

    Entry entry;
    entry.fd = fd;
    entry.inode = probe.inode;
    entry.protocol = probe.protocol;
    entry.state = probe.state;
    entry.local_address = probe.local_address;
    entry.remote_address = probe.remote_address;
    entry.label = std::move(label);
    entry.registered_at_ms = NowMs();
    entry.registered = true;
    entry.last_tcp = tcp;
    entry.last_tcp_sent = tcp.sent_bytes;
    entry.last_tcp_recv = tcp.received_bytes;
    m_entries[fd] = std::move(entry);

    Entry& stored = m_entries[fd];
    AppendEventLocked(stored, SocketEventKind::kRegistered, 0, 0,
                      stored.label);
    if (stored.state == SocketState::kConnected) {
        AppendEventLocked(stored, SocketEventKind::kConnected, 0, 0,
                          stored.remote_address);
    }
    return true;
}

bool SocketRegistry::Unregister(int fd) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto iterator = m_entries.find(fd);
    if (iterator == m_entries.end()) {
        return false;
    }
    iterator->second.registered = false;
    return true;
}

bool SocketRegistry::UnregisterIfSameSocket(int fd, bool mark_closed) {
    if (fd < 0) {
        return false;
    }
    // 先探测再取锁：readlink/getsockopt 是系统调用，不放进注册表锁内。
    Probe probe;
    std::string probe_error;
    if (!ProbeFd(fd, &probe, &probe_error)) {
        // fd 已关闭或已不是 socket：保留条目，交给 List 扫描补墓碑，
        // 避免把「fd 已消失」误判成「新 socket 需要注销」。
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto iterator = m_entries.find(fd);
    if (iterator == m_entries.end()) {
        return false;
    }
    Entry& entry = iterator->second;
    if (entry.inode != probe.inode) {
        // fd 号码已被内核复用给别的 socket：本次注销属于旧 socket，
        // 不能动新条目（ABA 保护）。
        return false;
    }
    if (!entry.registered) {
        return false;
    }
    if (mark_closed) {
        MarkClosedLocked(entry, "socket 生命周期结束（自动注销）");
    } else {
        entry.registered = false;
        AppendEventLocked(entry, SocketEventKind::kCloseRequested, 0, 0,
                          "detached");
    }
    return true;
}

void SocketRegistry::RecordRead(int fd, std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto iterator = m_entries.find(fd);
    if (iterator == m_entries.end() || iterator->second.closed) {
        return;
    }
    iterator->second.counters.recv_bytes += bytes;
    AppendEventLocked(iterator->second, SocketEventKind::kRead, bytes, 0, "app");
}

void SocketRegistry::RecordWrite(int fd, std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto iterator = m_entries.find(fd);
    if (iterator == m_entries.end() || iterator->second.closed) {
        return;
    }
    iterator->second.counters.sent_bytes += bytes;
    AppendEventLocked(iterator->second, SocketEventKind::kWrite, bytes, 0, "app");
}

void SocketRegistry::RecordError(int fd, int error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto iterator = m_entries.find(fd);
    if (iterator == m_entries.end() || iterator->second.closed) {
        return;
    }
    ++iterator->second.counters.errors;
    AppendEventLocked(iterator->second, SocketEventKind::kError, 0, error,
                      ErrnoText(error));
}

void SocketRegistry::RecordTimeout(int fd) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto iterator = m_entries.find(fd);
    if (iterator == m_entries.end() || iterator->second.closed) {
        return;
    }
    ++iterator->second.counters.timeouts;
    AppendEventLocked(iterator->second, SocketEventKind::kTimeout, 0, 0, "app");
}

std::vector<SocketSnapshot> SocketRegistry::List(bool include_unobserved,
                                                 std::string* warning) {
    std::vector<ScannedSocket> scanned;
    std::string scan_error;
    if (!ScanProcessSockets(&scanned, &scan_error)) {
        if (warning != nullptr) {
            *warning = scan_error;
        }
    }

    struct Observed {
        ScannedSocket socket;
        Probe probe;
        SocketTcpInfo tcp;
        bool ok{false};
        std::string error;
    };
    std::vector<Observed> observed;
    observed.reserve(scanned.size());
    for (const auto& item : scanned) {
        Observed entry;
        entry.socket = item;
        entry.ok = ProbeFd(item.fd, &entry.probe, &entry.error);
        if (entry.ok && entry.probe.protocol == SocketProtocol::kTcp) {
            (void)ReadTcpInfo(item.fd, &entry.tcp);
        }
        observed.push_back(std::move(entry));
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    std::unordered_map<int, std::string> scanned_by_fd;
    scanned_by_fd.reserve(scanned.size());
    for (const auto& item : scanned) {
        scanned_by_fd[item.fd] = item.inode;
    }

    // 已登记但本次扫描不到、或 inode 变了：外部关闭/复用，留下 closed 事件。
    for (auto& pair : m_entries) {
        Entry& entry = pair.second;
        if (entry.closed) {
            continue;
        }
        const auto found = scanned_by_fd.find(entry.fd);
        if (found == scanned_by_fd.end()) {
            MarkClosedLocked(entry, "socket 已被外部关闭");
        } else if (found->second != entry.inode) {
            MarkClosedLocked(entry, "fd 已被复用（inode 变化）");
        }
    }

    std::vector<SocketSnapshot> snapshots;
    snapshots.reserve(observed.size());
    for (const auto& item : observed) {
        if (!item.ok) {
            continue;
        }
        const auto iterator = m_entries.find(item.socket.fd);
        if (iterator != m_entries.end() && !iterator->second.closed &&
            iterator->second.inode == item.socket.inode) {
            Entry& entry = iterator->second;
            entry.protocol = item.probe.protocol;
            entry.state = item.probe.state;
            entry.local_address = item.probe.local_address;
            entry.remote_address = item.probe.remote_address;
            SampleTcpLocked(entry, item.tcp);

            SocketSnapshot snapshot;
            snapshot.fd = entry.fd;
            snapshot.protocol = entry.protocol;
            snapshot.state = entry.state;
            snapshot.local_address = entry.local_address;
            snapshot.remote_address = entry.remote_address;
            snapshot.label = entry.label;
            snapshot.inode = entry.inode;
            snapshot.registered = entry.registered;
            snapshot.closable = entry.registered;
            snapshot.registered_at_ms = entry.registered_at_ms;
            snapshots.push_back(std::move(snapshot));
        } else if (include_unobserved) {
            SocketSnapshot snapshot;
            snapshot.fd = item.socket.fd;
            snapshot.protocol = item.probe.protocol;
            snapshot.state = item.probe.state;
            snapshot.local_address = item.probe.local_address;
            snapshot.remote_address = item.probe.remote_address;
            snapshot.inode = item.socket.inode;
            snapshots.push_back(std::move(snapshot));
        }
    }
    std::sort(snapshots.begin(), snapshots.end(),
              [](const SocketSnapshot& left, const SocketSnapshot& right) {
                  return left.fd < right.fd;
              });
    return snapshots;
}

std::optional<SocketStats> SocketRegistry::Stats(int fd, std::string* error) {
    Probe probe;
    if (!ProbeFd(fd, &probe, error)) {
        return std::nullopt;
    }
    SocketTcpInfo tcp;
    if (probe.protocol == SocketProtocol::kTcp) {
        (void)ReadTcpInfo(fd, &tcp);
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto iterator = m_entries.find(fd);
    const bool matches =
        iterator != m_entries.end() && iterator->second.inode == probe.inode;
    if (matches && iterator->second.closed) {
        if (error != nullptr) {
            *error = "fd " + std::to_string(fd) +
                     " 已关闭（注册表保留历史，可用 socket_events 查看）";
        }
        return std::nullopt;
    }

    SocketStats stats;
    stats.fd = fd;
    stats.protocol = probe.protocol;
    stats.state = probe.state;
    stats.tcp = tcp;
    if (matches) {
        Entry& entry = iterator->second;
        entry.protocol = probe.protocol;
        entry.state = probe.state;
        entry.local_address = probe.local_address;
        entry.remote_address = probe.remote_address;
        SampleTcpLocked(entry, tcp);
        stats.registered = entry.registered;
        stats.app = entry.counters;
        stats.tcp = tcp.available ? tcp : entry.last_tcp;
        stats.events = entry.events.size();
        stats.dropped_events = entry.dropped_events;
        stats.last_event_ms =
            entry.events.empty() ? 0 : entry.events.back().timestamp_ms;
    }
    return stats;
}

std::vector<SocketEvent> SocketRegistry::Events(int fd, std::size_t limit,
                                                bool* registered, bool* found,
                                                std::string* error) {
    if (registered != nullptr) {
        *registered = false;
    }
    if (found != nullptr) {
        *found = false;
    }
    if (error != nullptr) {
        error->clear();
    }
    if (limit == 0) {
        limit = 1;
    }
    if (limit > kEventCapacity) {
        limit = kEventCapacity;
    }

    Probe probe;
    std::string probe_error;
    const bool probe_ok = ProbeFd(fd, &probe, &probe_error);

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto iterator = m_entries.find(fd);
    const auto tail = [limit](const Entry& entry) {
        const std::size_t start =
            entry.events.size() > limit ? entry.events.size() - limit : 0;
        return std::vector<SocketEvent>(
            entry.events.begin() + static_cast<std::ptrdiff_t>(start),
            entry.events.end());
    };

    if (probe_ok) {
        if (iterator != m_entries.end() &&
            iterator->second.inode == probe.inode) {
            if (found != nullptr) {
                *found = true;
            }
            if (registered != nullptr) {
                *registered = iterator->second.registered &&
                              !iterator->second.closed;
            }
            return tail(iterator->second);
        }
        if (iterator != m_entries.end()) {
            m_entries.erase(iterator);  // fd 已复用，旧墓碑作废
        }
        return {};
    }

    if (iterator != m_entries.end() && iterator->second.closed) {
        if (found != nullptr) {
            *found = true;
        }
        return tail(iterator->second);
    }
    if (error != nullptr) {
        *error = probe_error.empty() ? "fd 不是本进程 socket" : probe_error;
    }
    return {};
}

SocketCloseOutcome SocketRegistry::Close(int fd, const std::string& reason,
                                         SocketAuditSink& audit) {
    std::lock_guard<std::mutex> lock(m_mutex);

    SocketCloseOutcome outcome;
    json record = json::object();
    record["ts"] = NowIso8601();
    record["pid"] = static_cast<long long>(::getpid());
    record["action"] = "socket_close";
    record["fd"] = fd;
    record["reason"] = reason.empty() ? std::string("(unspecified)") : reason;
    record["audit_id"] = NextAuditIdLocked();
    outcome.audit_id = record["audit_id"].get<std::string>();
    outcome.audit_sink = audit.Describe();

    const auto iterator = m_entries.find(fd);
    Probe probe;
    std::string probe_error;
    const bool probe_ok = ProbeFd(fd, &probe, &probe_error);
    if (iterator != m_entries.end()) {
        record["protocol"] = SocketProtocolName(iterator->second.protocol);
        record["local"] = iterator->second.local_address;
        record["remote"] = iterator->second.remote_address;
        record["label"] = iterator->second.label;
    } else if (probe_ok) {
        record["protocol"] = SocketProtocolName(probe.protocol);
        record["local"] = probe.local_address;
        record["remote"] = probe.remote_address;
    }

    const auto deny = [&](const std::string& message) -> SocketCloseOutcome {
        record["result"] = "denied";
        record["detail"] = message;
        std::string audit_error;
        if (!audit.Append(record, &audit_error)) {
            outcome.error = message + "；审计写入失败: " + audit_error;
        } else {
            outcome.error = message + "（审计已记录）";
        }
        outcome.ok = false;
        return outcome;
    };

    if (iterator == m_entries.end()) {
        return deny("fd 未登记，不在 socket_close 白名单");
    }
    if (iterator->second.closed) {
        return deny("fd 已关闭（注册表保留历史记录）");
    }
    if (!iterator->second.registered) {
        return deny("fd 已移出白名单，未登记");
    }
    if (!probe_ok) {
        MarkClosedLocked(iterator->second, probe_error);
        return deny("fd 已关闭或不是本进程 socket：" + probe_error);
    }
    if (probe.inode != iterator->second.inode) {
        MarkClosedLocked(iterator->second, "fd 已被复用（inode 变化）");
        return deny("fd 已被复用（inode 与登记时不一致），拒绝关闭");
    }

    record["protocol"] = SocketProtocolName(probe.protocol);
    record["local"] = probe.local_address;
    record["remote"] = probe.remote_address;
    record["result"] = "ok";
    record["detail"] = "closed by MCP socket_close";

    // fail-closed：审计先落盘，失败就不关闭。
    std::string audit_error;
    if (!audit.Append(record, &audit_error)) {
        outcome.ok = false;
        outcome.error = "审计写入失败，按 fail-closed 拒绝关闭：" + audit_error;
        return outcome;
    }

    AppendEventLocked(iterator->second, SocketEventKind::kCloseRequested, 0, 0,
                      reason);
    if (::close(fd) != 0) {
        const int close_error = errno;
        record["result"] = "error";
        record["detail"] = "close 失败：" + ErrnoText(close_error);
        std::string ignored_error;
        (void)audit.Append(record, &ignored_error);
        MarkClosedLocked(iterator->second, "close 失败");
        outcome.ok = false;
        outcome.error = "close(" + std::to_string(fd) +
                        ") 失败：" + ErrnoText(close_error);
        return outcome;
    }

    MarkClosedLocked(iterator->second,
                     reason.empty() ? "closed by MCP socket_close"
                                    : "closed: " + reason);
    outcome.ok = true;
    return outcome;
}

void SocketRegistry::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    m_audit_seq = 0;
}

std::size_t SocketRegistry::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t count = 0;
    for (const auto& pair : m_entries) {
        if (pair.second.registered && !pair.second.closed) {
            ++count;
        }
    }
    return count;
}

}  // namespace go2cpp::mcp::tools
