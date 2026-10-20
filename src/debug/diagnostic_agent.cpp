/**
 * @file diagnostic_agent.cpp
 * @brief Doctor 实现：单线程命令平面 + 可插拔 transport + 白名单唤醒。
 *
 * ## 三个实现要点（为什么这样写）
 *
 * 1. **执行线程唯一**。接收线程只做 `Recv`/入队，绝不读运行时状态；所有
 *    快照与唤醒都在执行线程串行发生。这样「一份 status 里的多个字段」
 *    不会被另一条诊断命令穿插，也不需要为每个字段设计全局原子快照。
 *
 * 2. **唤醒只走既有 API**。`force-wake scheduler-idle` -> `ForceWakeIdle()`
 *    -> 既有 `notify_all_workers()`；`parking-condition` -> 既有
 *    `ParkingCondition::notify_all()`；`channel` -> 既有
 *    `Channel::ForceWakeWaiters()`（内部只发 spurious wake）。Doctor
 *    **不写任何状态字**，因此不存在「诊断工具把状态机改坏」的路径。
 *
 * 3. **解锁前先证明 owner 已不可能再访问临界区**。`mutex-unlock` 只在
 *    `owner_known && owner 不在任何已注册 Scheduler 的活动 G 表里` 时调用
 *    既有 `Mutex::Unlock()`；其余情况一律 `refused` 并给出稳定原因。
 *
 * ## 平台
 *
 * Unix domain socket / `/proc` / ELF 解析都是 POSIX/Linux 实现；非 Linux
 * 平台编译时给出 `#error`（与 `debug_server.cpp` 的取舍一致：本模块不提供
 * 假装可移植的空实现）。
 */

#include "go2cpp/debug/diagnostic_agent.hpp"

#include "go2cpp/debug/debug_event_ring.hpp"
#include "go2cpp/debug/debug_server.hpp"
#include "go2cpp/json.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#if !defined(__linux__)
#error "go2cpp::debug::DiagnosticAgent 的 Unix socket / build-id 实现只支持 Linux"
#endif

namespace go2cpp::debug {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t NowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

std::uint64_t ElapsedNs(std::uint64_t start) noexcept {
    const std::uint64_t now = NowNs();
    return now >= start ? now - start : 0U;
}

std::uint64_t ToMs(std::uint64_t ns) noexcept { return ns / 1000000U; }
std::uint64_t ToUs(std::uint64_t ns) noexcept { return ns / 1000U; }

std::string Trim(const std::string& input) {
    std::size_t begin = 0;
    while (begin < input.size() &&
           (input[begin] == ' ' || input[begin] == '\t' ||
            input[begin] == '\r' || input[begin] == '\n')) {
        ++begin;
    }
    std::size_t end = input.size();
    while (end > begin &&
           (input[end - 1] == ' ' || input[end - 1] == '\t' ||
            input[end - 1] == '\r' || input[end - 1] == '\n')) {
        --end;
    }
    return input.substr(begin, end - begin);
}

std::string Lower(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

std::vector<std::string> Split(const std::string& input, char separator) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= input.size()) {
        const std::size_t end = input.find(separator, begin);
        if (end == std::string::npos) {
            parts.push_back(input.substr(begin));
            break;
        }
        parts.push_back(input.substr(begin, end - begin));
        begin = end + 1;
    }
    return parts;
}

/// 把一条命令拆成 `name` + 剩余参数（去掉首尾空白）。
struct CommandLine {
    std::string name;
    std::string argument;
};

CommandLine ParseCommandLine(const std::string& line) {
    const std::string trimmed = Trim(line);
    const std::size_t space = trimmed.find_first_of(" \t");
    if (space == std::string::npos) {
        return {Lower(trimmed), {}};
    }
    return {Lower(trimmed.substr(0, space)),
            Trim(trimmed.substr(space))};
}

/// key=value / 裸值参数表；裸值按出现顺序保存。
struct Arguments {
    std::map<std::string, std::string> values;
    std::vector<std::string> positional;

    const std::string& Get(const std::string& key) const {
        static const std::string empty;
        const auto found = values.find(key);
        return found == values.end() ? empty : found->second;
    }
    const std::string& Positional(std::size_t index) const {
        static const std::string empty;
        return index < positional.size() ? positional[index] : empty;
    }
};

Arguments ParseArguments(const std::string& text) {
    Arguments arguments;
    std::istringstream stream(text);
    std::string token;
    while (stream >> token) {
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos) {
            arguments.positional.push_back(token);
        } else {
            arguments.values[Lower(token.substr(0, equals))] =
                token.substr(equals + 1);
        }
    }
    return arguments;
}

std::string JsonText(const Json& value) {
    try {
        return value.dump();
    } catch (...) {
        return "{}";
    }
}

const char* VerdictName(DeadlockVerdict verdict) noexcept {
    switch (verdict) {
        case DeadlockVerdict::kUnknown: return "unknown";
        case DeadlockVerdict::kIdle: return "idle";
        case DeadlockVerdict::kProgressing: return "progressing";
        case DeadlockVerdict::kStalled: return "stalled";
        case DeadlockVerdict::kDeadlock: return "deadlock";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// ELF / /proc 工具：模块基址 + build-id（供外部符号化）
// ---------------------------------------------------------------------------

std::string ReadSymlink(const char* path) {
    char buffer[4096];
    const ssize_t length = ::readlink(path, buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return {};
    }
    buffer[length] = '\0';
    return std::string(buffer, static_cast<std::size_t>(length));
}

std::uint16_t ReadLe16(const unsigned char* data) noexcept {
    return static_cast<std::uint16_t>(data[0]) |
           (static_cast<std::uint16_t>(data[1]) << 8);
}

std::uint32_t ReadLe32(const unsigned char* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t ReadLe64(const unsigned char* data) noexcept {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | static_cast<std::uint64_t>(data[i]);
    }
    return value;
}

std::string HexBytes(const unsigned char* data, std::size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2U);
    for (std::size_t i = 0; i < size; ++i) {
        text.push_back(digits[(data[i] >> 4) & 0x0FU]);
        text.push_back(digits[data[i] & 0x0FU]);
    }
    return text;
}

/// 读 ELF64 的 `.note.gnu.build-id`；失败返回空串（不猜、不伪造）。
std::string ReadBuildId(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    unsigned char header[64];
    stream.read(reinterpret_cast<char*>(header), sizeof(header));
    if (stream.gcount() != static_cast<std::streamsize>(sizeof(header))) {
        return {};
    }
    if (header[0] != 0x7FU || header[1] != 'E' || header[2] != 'L' ||
        header[3] != 'F') {
        return {};
    }
    if (header[4] != 2U || header[5] != 1U) {  // ELFCLASS64, ELFDATA2LSB
        return {};
    }
    const std::uint64_t program_offset = ReadLe64(header + 32);
    const std::uint16_t program_size = ReadLe16(header + 54);
    const std::uint16_t program_count = ReadLe16(header + 56);
    if (program_size < 56U || program_count == 0U || program_count > 512U) {
        return {};
    }
    constexpr std::uint32_t kProgramNote = 4U;  // PT_NOTE
    for (std::uint16_t index = 0; index < program_count; ++index) {
        unsigned char program[56];
        stream.seekg(static_cast<std::streamoff>(
                         program_offset +
                         static_cast<std::uint64_t>(index) * program_size),
                     std::ios::beg);
        stream.read(reinterpret_cast<char*>(program), sizeof(program));
        if (stream.gcount() != static_cast<std::streamsize>(sizeof(program))) {
            return {};
        }
        if (ReadLe32(program) != kProgramNote) {
            continue;
        }
        const std::uint64_t note_offset = ReadLe64(program + 8);
        const std::uint64_t note_size = ReadLe64(program + 32);
        if (note_size == 0U || note_size > 1024U * 1024U) {
            continue;
        }
        std::string notes(static_cast<std::size_t>(note_size), '\0');
        stream.seekg(static_cast<std::streamoff>(note_offset), std::ios::beg);
        stream.read(notes.data(),
                    static_cast<std::streamsize>(notes.size()));
        const std::size_t read_size =
            static_cast<std::size_t>(stream.gcount());
        if (read_size < 12U) {
            continue;
        }
        std::size_t offset = 0;
        while (offset + 12U <= read_size) {
            const auto* note =
                reinterpret_cast<const unsigned char*>(notes.data() + offset);
            const std::uint32_t name_size = ReadLe32(note);
            const std::uint32_t desc_size = ReadLe32(note + 4);
            const std::uint32_t type = ReadLe32(note + 8);
            const std::size_t name_padded = (name_size + 3U) & ~3UL;
            const std::size_t desc_padded = (desc_size + 3U) & ~3UL;
            if (offset + 12U + name_padded + desc_padded > read_size) {
                break;
            }
            const char* name = notes.data() + offset + 12U;
            const bool is_gnu =
                name_size >= 4U && name[0] == 'G' && name[1] == 'N' &&
                name[2] == 'U' && name[3] == '\0';
            if (is_gnu && type == 3U && desc_size != 0U) {  // NT_GNU_BUILD_ID
                return HexBytes(reinterpret_cast<const unsigned char*>(
                                    name + name_padded),
                                desc_size);
            }
            offset += 12U + name_padded + desc_padded;
        }
    }
    return {};
}

struct ModuleInfo {
    std::string role;      ///< "exe" / "lib"
    std::string path;
    std::uint64_t base{0};
    std::string build_id;  ///< 空串 = 读不到（输出 unavailable）
    bool build_id_known{false};
};

/// 从 /proc/self/maps 采集主程序与 libgo2cpp_*.so 的基址，并读 build-id。
std::vector<ModuleInfo> CollectModules() {
    std::vector<ModuleInfo> modules;
    const std::string executable = ReadSymlink("/proc/self/exe");
    std::map<std::string, std::uint64_t> bases;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        // 行格式：start-end perms offset dev inode path
        std::istringstream stream(line);
        std::string range;
        std::string perms;
        std::string offset;
        std::string device;
        std::string inode;
        if (!(stream >> range >> perms >> offset >> device >> inode)) {
            continue;
        }
        std::string path;
        std::getline(stream, path);
        path = Trim(path);
        if (path.empty() || path[0] != '/') {
            continue;
        }
        if (!executable.empty() && path == executable) {
            // 主程序：所有映射都算，取最小地址。
        } else if (path.find("libgo2cpp_") == std::string::npos) {
            continue;
        }
        const std::size_t dash = range.find('-');
        if (dash == std::string::npos) {
            continue;
        }
        std::uint64_t start = 0;
        try {
            start = std::stoull(range.substr(0, dash), nullptr, 16);
        } catch (...) {
            continue;
        }
        const auto found = bases.find(path);
        if (found == bases.end() || start < found->second) {
            bases[path] = start;
        }
    }
    constexpr std::size_t kMaxModules = 32;
    for (const auto& entry : bases) {
        if (modules.size() >= kMaxModules) {
            break;
        }
        ModuleInfo info;
        info.path = entry.first;
        info.base = entry.second;
        info.role = (!executable.empty() && entry.first == executable)
                        ? "exe"
                        : "lib";
        info.build_id = ReadBuildId(entry.first);
        info.build_id_known = !info.build_id.empty();
        modules.push_back(std::move(info));
    }
    std::sort(modules.begin(), modules.end(),
              [](const ModuleInfo& left, const ModuleInfo& right) {
                  if (left.role != right.role) {
                      return left.role == "exe";
                  }
                  return left.path < right.path;
              });
    return modules;
}

// ---------------------------------------------------------------------------
// 文本/JSON 小工具
// ---------------------------------------------------------------------------

std::string HexAddress(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llx",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

std::string BoolText(bool value) { return value ? "true" : "false"; }

/// describe 字符串里是否报告了非零等待者（用于死锁检测的「有人等」判定）。
bool DescribeHasWaiters(const std::string& describe) {
    const std::string needle = "waiters=";
    std::size_t position = 0;
    while ((position = describe.find(needle, position)) != std::string::npos) {
        position += needle.size();
        std::size_t end = position;
        while (end < describe.size() && describe[end] >= '0' &&
               describe[end] <= '9') {
            ++end;
        }
        if (end > position) {
            try {
                if (std::stoull(describe.substr(position, end - position)) != 0U) {
                    return true;
                }
            } catch (...) {
            }
        }
        // `send_waiters=` / `recv_waiters=` / `native_waiters=` 同样命中。
        position = end;
    }
    return false;
}

std::string DescribeMutex(const sync::Mutex& mutex) {
    const auto snapshot = mutex.DebugSnapshotInternal();
    return "held=" + BoolText(snapshot.held) +
           " waiters=" + std::to_string(snapshot.waiters) +
           " native_waiters=" + std::to_string(snapshot.native_waiters) +
           " fiber_waiters=" + std::to_string(snapshot.fiber_waiters) +
           " owner_gid=" + std::to_string(snapshot.owner_gid) +
           " owner_known=" + BoolText(snapshot.owner_known);
}

std::string DescribeParkingCondition(const core::ParkingCondition& condition) {
    const auto snapshot = condition.DebugSnapshot();
    std::string gids;
    for (const auto& node : snapshot.nodes) {
        if (node.native || node.gid == 0U) {
            continue;
        }
        if (!gids.empty()) {
            gids.push_back(',');
        }
        gids += std::to_string(node.gid);
    }
    return "waiters=" + std::to_string(snapshot.waiters) +
           " native_waiters=" + std::to_string(snapshot.native_waiters) +
           " fiber_waiters=" + std::to_string(snapshot.fiber_waiters) +
           " waiting_nodes=" + std::to_string(snapshot.waiting_nodes) +
           " gids=" + (gids.empty() ? std::string("-") : gids);
}

}  // namespace

// ---------------------------------------------------------------------------
// MemoryDiagnosticTransport
// ---------------------------------------------------------------------------

bool MemoryDiagnosticTransport::Start(std::string* error) {
    (void)error;
    m_running.store(true, std::memory_order_release);
    return true;
}

void MemoryDiagnosticTransport::Stop() {
    m_running.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cv.notify_all();
}

bool MemoryDiagnosticTransport::PushRequest(DiagnosticFrame frame) {
    if (!m_running.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_requests.push_back(std::move(frame));
    }
    m_cv.notify_all();
    return true;
}

bool MemoryDiagnosticTransport::PopResponse(DiagnosticFrame& frame) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_responses.empty()) {
        return false;
    }
    frame = std::move(m_responses.front());
    m_responses.pop_front();
    return true;
}

std::size_t MemoryDiagnosticTransport::pending_requests() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_requests.size();
}

bool MemoryDiagnosticTransport::Recv(DiagnosticEnvelope& envelope,
                                     int timeout_ms, std::string* error) {
    (void)error;
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_cv.wait_for(lock, std::chrono::milliseconds(std::max(0, timeout_ms)),
                       [this] {
                           return !m_requests.empty() ||
                                  !m_running.load(std::memory_order_acquire);
                       })) {
        return false;
    }
    if (m_requests.empty()) {
        return false;
    }
    envelope.frame = std::move(m_requests.front());
    m_requests.pop_front();
    envelope.peer = m_next_peer++;
    return true;
}

bool MemoryDiagnosticTransport::Send(const DiagnosticEnvelope& envelope,
                                     std::string* error) {
    (void)error;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_responses.push_back(envelope.frame);
    }
    m_cv.notify_all();
    return m_running.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// UnixSocketDiagnosticTransport
// ---------------------------------------------------------------------------

struct UnixSocketDiagnosticTransport::Connection {
    // fd / 状态位用原子：Recv 在接收线程、Send 在执行线程，二者会并发查找
    // 同一个连接；普通 bool/int 在这里是数据竞争。parser 只由接收线程访问，
    // 不需要原子。
    std::atomic<int> fd{-1};
    std::uint64_t peer{0};
    DiagnosticFrameParser parser;
    std::atomic<bool> awaiting_reply{false};
    std::atomic<bool> closed{false};
};

UnixSocketDiagnosticTransport::UnixSocketDiagnosticTransport(std::string path,
                                                             unsigned int mode)
    : m_path(std::move(path)), m_mode(mode) {}

UnixSocketDiagnosticTransport::~UnixSocketDiagnosticTransport() { Stop(); }

bool UnixSocketDiagnosticTransport::Start(std::string* error) {
    if (running()) {
        return true;
    }
    if (m_path.empty()) {
        if (error != nullptr) {
            *error = "empty-socket-path";
        }
        return false;
    }
    if (m_path.size() >= sizeof(sockaddr_un::sun_path)) {
        if (error != nullptr) {
            *error = "socket-path-too-long";
        }
        return false;
    }
    const int listen_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        if (error != nullptr) {
            *error = std::string("socket: ") + std::strerror(errno);
        }
        return false;
    }
    // 允许覆盖遗留 socket；不是 socket 的路径也移除（路径由调用方显式指定）。
    (void)::unlink(m_path.c_str());
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, m_path.c_str(), m_path.size() + 1U);
    if (::bind(listen_fd, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0) {
        const int saved = errno;
        ::close(listen_fd);
        if (error != nullptr) {
            *error = std::string("bind: ") + std::strerror(saved);
        }
        return false;
    }
    (void)::chmod(m_path.c_str(), static_cast<mode_t>(m_mode));
    if (::listen(listen_fd, 32) != 0) {
        const int saved = errno;
        ::close(listen_fd);
        (void)::unlink(m_path.c_str());
        if (error != nullptr) {
            *error = std::string("listen: ") + std::strerror(saved);
        }
        return false;
    }
    const int flags = ::fcntl(listen_fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_listen_fd = listen_fd;
        m_last_error.clear();
    }
    m_stop.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    return true;
}

void UnixSocketDiagnosticTransport::Stop() {
    if (!m_running.exchange(false)) {
        // 仍然清理残留 fd/路径（幂等）。
    }
    m_stop.store(true, std::memory_order_release);
    std::vector<std::shared_ptr<Connection>> connections;
    int listen_fd = -1;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        connections.swap(m_connections);
        listen_fd = m_listen_fd;
        m_listen_fd = -1;
    }
    for (auto& connection : connections) {
        if (connection) {
            const int fd =
                connection->fd.exchange(-1, std::memory_order_acq_rel);
            if (fd >= 0) {
                ::close(fd);
            }
            connection->closed.store(true, std::memory_order_release);
        }
    }
    if (listen_fd >= 0) {
        ::close(listen_fd);
    }
    if (!m_path.empty()) {
        (void)::unlink(m_path.c_str());
    }
}

std::string UnixSocketDiagnosticTransport::last_error() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_error;
}

void UnixSocketDiagnosticTransport::DropConnection(std::uint64_t peer) {
    std::shared_ptr<Connection> removed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto iterator = m_connections.begin();
             iterator != m_connections.end(); ++iterator) {
            if (*iterator && (*iterator)->peer == peer) {
                removed = *iterator;
                m_connections.erase(iterator);
                break;
            }
        }
    }
    if (removed) {
        const int fd = removed->fd.exchange(-1, std::memory_order_acq_rel);
        if (fd >= 0) {
            ::close(fd);
        }
        removed->closed.store(true, std::memory_order_release);
    }
}

bool UnixSocketDiagnosticTransport::TryReadFrame(Connection& connection) {
    const int fd = connection.fd.load(std::memory_order_acquire);
    if (fd < 0 ||
        connection.awaiting_reply.load(std::memory_order_acquire)) {
        return false;
    }
    char buffer[8192];
    for (;;) {
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received > 0) {
            if (!connection.parser.Feed(buffer,
                                        static_cast<std::size_t>(received))) {
                return false;
            }
            continue;
        }
        if (received == 0) {
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool UnixSocketDiagnosticTransport::Recv(DiagnosticEnvelope& envelope,
                                         int timeout_ms, std::string* error) {
    if (!running()) {
        if (error != nullptr) {
            *error = "transport-not-running";
        }
        return false;
    }

    // 先在没有新数据的情况下检查「已完成读帧、等待回写」的连接。
    std::vector<std::shared_ptr<Connection>> snapshot;
    int listen_fd = -1;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& connection : m_connections) {
            if (!connection) {
                continue;
            }
            if (connection->fd.load(std::memory_order_acquire) < 0) {
                continue;
            }
            snapshot.push_back(connection);
        }
        listen_fd = m_listen_fd;
    }

    // 先尝试从已有缓冲里取帧（连接可能在上次 Recv 里已读进 parser）。
    for (auto& connection : snapshot) {
        if (connection->awaiting_reply.load(std::memory_order_acquire) ||
            connection->fd.load(std::memory_order_acquire) < 0) {
            continue;
        }
        DiagnosticFrame frame;
        if (!connection->parser.Next(frame)) {
            if (connection->parser.failed()) {
                DropConnection(connection->peer);
            }
            continue;
        }
        connection->awaiting_reply.store(true, std::memory_order_release);
        envelope.frame = std::move(frame);
        envelope.peer = connection->peer;
        return true;
    }

    std::vector<pollfd> descriptors;
    descriptors.reserve(snapshot.size() + 1U);
    if (listen_fd >= 0) {
        pollfd listener{};
        listener.fd = listen_fd;
        listener.events = POLLIN;
        descriptors.push_back(listener);
    }
    for (auto& connection : snapshot) {
        pollfd entry{};
        entry.fd = connection->fd.load(std::memory_order_acquire);
        entry.events = POLLIN;
        descriptors.push_back(entry);
    }
    if (descriptors.empty()) {
        return false;
    }
    const int ready = ::poll(descriptors.data(),
                             static_cast<nfds_t>(descriptors.size()),
                             timeout_ms);
    if (ready < 0) {
        if (errno == EINTR) {
            return false;
        }
        if (error != nullptr) {
            *error = std::string("poll: ") + std::strerror(errno);
        }
        return false;
    }
    if (ready == 0) {
        return false;
    }

    std::size_t descriptor_index = 0;
    bool listener_ready = false;
    if (listen_fd >= 0) {
        listener_ready = (descriptors[descriptor_index].revents & POLLIN) != 0;
        ++descriptor_index;
    }
    std::vector<std::shared_ptr<Connection>> readable;
    for (auto& connection : snapshot) {
        const short events = descriptors[descriptor_index].revents;
        ++descriptor_index;
        if ((events & (POLLIN | POLLHUP | POLLERR)) != 0) {
            readable.push_back(connection);
        }
    }

    // 1) 读已就绪的连接。
    for (auto& connection : readable) {
        if (connection->awaiting_reply.load(std::memory_order_acquire) ||
            connection->fd.load(std::memory_order_acquire) < 0) {
            continue;
        }
        if (!TryReadFrame(*connection)) {
            DropConnection(connection->peer);
            continue;
        }
        DiagnosticFrame frame;
        if (connection->parser.Next(frame)) {
            connection->awaiting_reply.store(true,
                                             std::memory_order_release);
            envelope.frame = std::move(frame);
            envelope.peer = connection->peer;
            return true;
        }
        if (connection->parser.failed()) {
            DropConnection(connection->peer);
        }
    }

    // 2) 接受新连接（可能在同一次 poll 里就绪）。
    if (listener_ready && listen_fd >= 0) {
        for (;;) {
            const int client = ::accept4(listen_fd, nullptr, nullptr,
                                         SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (client < 0) {
                break;
            }
            auto connection = std::make_shared<Connection>();
            connection->fd.store(client, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                connection->peer = m_next_peer++;
                m_connections.push_back(connection);
            }
            if (!TryReadFrame(*connection)) {
                DropConnection(connection->peer);
                continue;
            }
            DiagnosticFrame frame;
            if (connection->parser.Next(frame)) {
                connection->awaiting_reply.store(true,
                                                 std::memory_order_release);
                envelope.frame = std::move(frame);
                envelope.peer = connection->peer;
                return true;
            }
        }
    }
    return false;
}

bool UnixSocketDiagnosticTransport::Send(const DiagnosticEnvelope& envelope,
                                         std::string* error) {
    std::shared_ptr<Connection> connection;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& item : m_connections) {
            if (item && item->peer == envelope.peer) {
                connection = item;
                break;
            }
        }
        if (connection) {
            m_connections.erase(
                std::remove(m_connections.begin(), m_connections.end(),
                            connection),
                m_connections.end());
        }
    }
    if (!connection) {
        if (error != nullptr) {
            *error = "unknown-peer";
        }
        return false;
    }
    // 抢占 fd：与 Recv 的 poll 快照并发时，保证只有一个线程 close 它。
    const int fd = connection->fd.exchange(-1, std::memory_order_acq_rel);
    if (fd < 0) {
        if (error != nullptr) {
            *error = "unknown-peer";
        }
        return false;
    }
    const std::string bytes = EncodeDiagnosticFrame(envelope.frame);
    std::size_t offset = 0;
    bool ok = !bytes.empty();
    while (ok && offset < bytes.size()) {
        const ssize_t sent = ::send(fd, bytes.data() + offset,
                                    bytes.size() - offset, MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        ok = false;
    }
    ::close(fd);
    connection->closed.store(true, std::memory_order_release);
    if (!ok && error != nullptr) {
        *error = "send-failed";
    }
    return ok;
}

// ---------------------------------------------------------------------------
// DiagnosticAgent 生命周期
// ---------------------------------------------------------------------------

DiagnosticAgent& DiagnosticAgent::Instance() {
    static DiagnosticAgent* instance = new DiagnosticAgent();
    return *instance;
}

DiagnosticAgent::DiagnosticAgent() = default;
DiagnosticAgent::~DiagnosticAgent() { Stop(); }

std::string DiagnosticAgent::DefaultSocketPath() {
    return "/tmp/go2cpp-" + std::to_string(static_cast<int>(::getpid())) +
           "-doctor.sock";
}

std::string DiagnosticAgent::ResolveSocketPath(const std::string& configured) {
    if (!configured.empty()) {
        return configured;
    }
    const char* environment = std::getenv("GO2CPP_DOCTOR_SOCKET");
    if (environment != nullptr && *environment != '\0') {
        return environment;
    }
    return DefaultSocketPath();
}

bool DiagnosticAgent::Start(const DiagnosticAgentConfig& config) {
    return Start(config, nullptr);
}

bool DiagnosticAgent::Start(const DiagnosticAgentConfig& config,
                            std::shared_ptr<DiagnosticTransport> transport) {
    std::lock_guard<std::mutex> lifecycle(m_lifecycle_mutex);
    if (m_running.load(std::memory_order_acquire)) {
        return true;
    }
    m_config = config;
    if (m_config.max_queue == 0U) {
        m_config.max_queue = 1U;
    }
    if (m_config.recv_timeout_ms <= 0) {
        m_config.recv_timeout_ms = 100;
    }
    if (m_config.detect_interval <= std::chrono::milliseconds::zero()) {
        m_config.detect_interval = std::chrono::milliseconds(1);
    }
    m_stop.store(false, std::memory_order_release);
    m_start_ns = NowNs();
    m_last_progress_ns.store(m_start_ns, std::memory_order_release);
    m_no_progress_ms.store(0, std::memory_order_release);
    m_verdict.store(DeadlockVerdict::kUnknown, std::memory_order_release);
    m_last_detect_ns = 0;
    m_last_deadlock_event_ns = 0;

    if (!transport) {
        m_socket_path = ResolveSocketPath(m_config.socket_path);
        transport = std::make_shared<UnixSocketDiagnosticTransport>(
            m_socket_path, m_config.socket_mode);
    } else {
        m_socket_path = m_config.socket_path;
    }
    std::string error;
    if (!transport->Start(&error)) {
        {
            std::lock_guard<std::mutex> lock(m_error_mutex);
            m_last_error = error.empty() ? "transport-start-failed" : error;
        }
        m_transport.reset();
        return false;
    }
    m_transport = std::move(transport);
    {
        std::lock_guard<std::mutex> lock(m_error_mutex);
        m_last_error.clear();
    }
    m_running.store(true, std::memory_order_release);
    try {
        m_receiver = std::thread([this] { ReceiverMain(); });
        m_executor = std::thread([this] { ExecutorMain(); });
    } catch (...) {
        m_running.store(false, std::memory_order_release);
        m_stop.store(true, std::memory_order_release);
        m_queue_cv.notify_all();
        if (m_transport) {
            m_transport->Stop();
        }
        m_transport.reset();
        {
            std::lock_guard<std::mutex> lock(m_error_mutex);
            m_last_error = "thread-start-failed";
        }
        return false;
    }
    m_last_fingerprint.store(ProgressFingerprint(),
                             std::memory_order_release);
    return true;
}

void DiagnosticAgent::Stop() {
    std::thread receiver;
    std::thread executor;
    std::shared_ptr<DiagnosticTransport> transport;
    {
        std::lock_guard<std::mutex> lifecycle(m_lifecycle_mutex);
        if (!m_running.exchange(false, std::memory_order_acq_rel) &&
            !m_receiver.joinable() && !m_executor.joinable()) {
            return;
        }
        m_stop.store(true, std::memory_order_release);
        m_queue_cv.notify_all();
        receiver = std::move(m_receiver);
        executor = std::move(m_executor);
        transport = m_transport;
    }
    if (receiver.joinable()) {
        receiver.join();
    }
    if (executor.joinable()) {
        executor.join();
    }
    if (transport) {
        transport->Stop();
    }
    {
        std::lock_guard<std::mutex> lifecycle(m_lifecycle_mutex);
        m_transport.reset();
        m_queue.clear();
        m_queue_depth.store(0, std::memory_order_release);
    }
}

std::string DiagnosticAgent::socket_path() const {
    std::lock_guard<std::mutex> lock(m_lifecycle_mutex);
    return m_socket_path;
}

std::string DiagnosticAgent::last_error() const {
    std::lock_guard<std::mutex> lock(m_error_mutex);
    return m_last_error;
}

std::string DiagnosticAgent::transport_name() const {
    std::lock_guard<std::mutex> lock(m_lifecycle_mutex);
    return m_transport ? m_transport->name() : std::string();
}

// ---------------------------------------------------------------------------
// 接收线程 / 执行线程
// ---------------------------------------------------------------------------

void DiagnosticAgent::ReceiverMain() {
    std::string error;
    while (!m_stop.load(std::memory_order_acquire)) {
        if (!m_transport) {
            break;
        }
        DiagnosticEnvelope envelope;
        if (!m_transport->Recv(envelope, m_config.recv_timeout_ms, &error)) {
            if (!error.empty()) {
                std::lock_guard<std::mutex> lock(m_error_mutex);
                m_last_error = error;
                error.clear();
            }
            continue;
        }
        if (envelope.frame.kind != DiagnosticFrameKind::kRequest) {
            continue;
        }
        m_accepted.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            if (m_queue.size() >= m_config.max_queue) {
                DiagnosticCommandResult rejected;
                rejected.ok = false;
                rejected.error_code = "queue-full";
                rejected.error_message = "doctor command queue is full";
                rejected.payload = "queue-full: doctor command queue is full";
                (void)SendResult(envelope, rejected);
                m_rejected.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            m_queue.push_back(QueuedCommand{envelope, Clock::now()});
            const auto depth = m_queue.size();
            std::uint64_t observed =
                m_max_queue_depth.load(std::memory_order_relaxed);
            while (depth > observed &&
                   !m_max_queue_depth.compare_exchange_weak(
                       observed, depth, std::memory_order_relaxed)) {
            }
            m_queue_depth.store(depth, std::memory_order_release);
        }
        m_queue_cv.notify_one();
    }
}

void DiagnosticAgent::ExecutorMain() {
    for (;;) {
        QueuedCommand command;
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_queue_cv.wait_for(
                lock, std::chrono::milliseconds(50), [this] {
                    return m_stop.load(std::memory_order_acquire) ||
                           !m_queue.empty();
                });
            if (m_queue.empty()) {
                const bool stopping =
                    m_stop.load(std::memory_order_acquire);
                lock.unlock();
                MaybeDetectDeadlock();
                if (stopping) {
                    break;
                }
                continue;
            }
            command = std::move(m_queue.front());
            m_queue.pop_front();
            m_queue_depth.store(m_queue.size(), std::memory_order_release);
        }
        const std::uint64_t queue_wait_us =
            ToUs(ElapsedNs(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    command.enqueued.time_since_epoch())
                    .count())));
        DiagnosticCommandResult result = RunCommand(
            command.envelope.frame.payload, queue_wait_us);
        m_executed.fetch_add(1, std::memory_order_relaxed);
        if (!result.ok) {
            m_failed.fetch_add(1, std::memory_order_relaxed);
        }
        (void)SendResult(command.envelope, result);
        MaybeDetectDeadlock();
    }
}

void DiagnosticAgent::HandleFrame(const DiagnosticEnvelope& envelope) {
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    m_queue.push_back(QueuedCommand{envelope, Clock::now()});
    m_queue_cv.notify_one();
}

// ---------------------------------------------------------------------------
// 注册
// ---------------------------------------------------------------------------

bool DiagnosticAgent::AddWakeTarget(const std::string& name,
                                    WakeSupport support,
                                    std::function<std::size_t()> wake,
                                    std::function<std::string()> describe) {
    if (name.empty() || !wake) {
        return false;
    }
    auto target = std::make_shared<WakeTarget>();
    target->name = name;
    target->support = support;
    target->wake = std::move(wake);
    target->describe = std::move(describe);
    std::lock_guard<std::mutex> lock(m_registry_mutex);
    for (auto& existing : m_targets) {
        if (existing && existing->name == name) {
            existing = std::move(target);
            return true;
        }
    }
    m_targets.push_back(std::move(target));
    return true;
}

bool DiagnosticAgent::RegisterScheduler(const std::string& name,
                                        Scheduler& scheduler) {
    if (name.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        bool found = false;
        for (auto& entry : m_schedulers) {
            if (entry.first == name) {
                entry.second = &scheduler;
                found = true;
                break;
            }
        }
        if (!found) {
            m_schedulers.emplace_back(name, &scheduler);
        }
    }
    // 桥接 DebugServer：让 `dump scheduler` 复用既有 schema；若调用方已注册
    // 同名组件，这里覆盖为同一个 provider（幂等）。
    // 必须写全限定名：本类的成员函数也叫 RegisterScheduler，非限定调用会
    // 命中成员重载而参数不匹配。
    (void)::go2cpp::debug::RegisterScheduler(name, scheduler);
    return AddWakeTarget(
        name, WakeSupport::kSchedulerIdle,
        [&scheduler]() -> std::size_t {
            return scheduler.ForceWakeIdle();
        },
        [&scheduler]() -> std::string {
            return "workers=" + std::to_string(scheduler.worker_count()) +
                   " runnable=" +
                   std::to_string(scheduler.runnable_count()) +
                   " runnable_global=" +
                   std::to_string(scheduler.global_runnable_count());
        });
}

bool DiagnosticAgent::RegisterParkingCondition(
    const std::string& name, core::ParkingCondition& condition) {
    if (name.empty()) {
        return false;
    }
    (void)DebugServer::Instance().Register(
        name, "ParkingCondition waiters and node states",
        [&condition]() { return DumpParkingCondition(condition); });
    return AddWakeTarget(
        name, WakeSupport::kParkingCondition,
        [&condition]() -> std::size_t {
            const auto snapshot = condition.DebugSnapshot();
            condition.notify_all();
            return snapshot.waiters;
        },
        [&condition]() { return DescribeParkingCondition(condition); });
}

bool DiagnosticAgent::RegisterMutex(const std::string& name,
                                    sync::Mutex& mutex) {
    if (name.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        bool found = false;
        for (auto& entry : m_mutexes) {
            if (entry.name == name) {
                entry.mutex = &mutex;
                found = true;
                break;
            }
        }
        if (!found) {
            m_mutexes.push_back(MutexRegistration{name, &mutex});
        }
    }
    (void)DebugServer::Instance().Register(
        name, "sync::Mutex owner/waiters/state bits",
        [&mutex]() { return DumpMutex(mutex); });
    return true;
}

bool DiagnosticAgent::RegisterFiberFunction(
    const std::string& name, std::function<std::string()> function) {
    if (name.empty() || !function) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_registry_mutex);
    for (auto& entry : m_fiber_functions) {
        if (entry.first == name) {
            entry.second = std::move(function);
            return true;
        }
    }
    m_fiber_functions.emplace_back(name, std::move(function));
    return true;
}

std::vector<std::string> DiagnosticAgent::FiberFunctions() const {
    std::lock_guard<std::mutex> lock(m_registry_mutex);
    std::vector<std::string> names;
    names.reserve(m_fiber_functions.size());
    for (const auto& entry : m_fiber_functions) {
        names.push_back(entry.first);
    }
    return names;
}

bool DiagnosticAgent::UnregisterTarget(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_registry_mutex);
    const auto before = m_targets.size();
    m_targets.erase(
        std::remove_if(m_targets.begin(), m_targets.end(),
                       [&name](const std::shared_ptr<WakeTarget>& target) {
                           return target && target->name == name;
                       }),
        m_targets.end());
    return m_targets.size() != before;
}

bool DiagnosticAgent::UnregisterScheduler(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        const auto before = m_schedulers.size();
        m_schedulers.erase(
            std::remove_if(m_schedulers.begin(), m_schedulers.end(),
                           [&name](const auto& entry) {
                               return entry.first == name;
                           }),
            m_schedulers.end());
        if (m_schedulers.size() == before) {
            return false;
        }
    }
    return UnregisterTarget(name);
}

bool DiagnosticAgent::UnregisterMutex(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_registry_mutex);
    const auto before = m_mutexes.size();
    m_mutexes.erase(
        std::remove_if(m_mutexes.begin(), m_mutexes.end(),
                       [&name](const MutexRegistration& registration) {
                           return registration.name == name;
                       }),
        m_mutexes.end());
    return m_mutexes.size() != before;
}

void DiagnosticAgent::ResetRegistrations() {
    std::vector<std::string> component_names;
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        for (const auto& entry : m_schedulers) {
            component_names.push_back(entry.first);
        }
        for (const auto& target : m_targets) {
            if (target) {
                component_names.push_back(target->name);
            }
        }
        for (const auto& registration : m_mutexes) {
            component_names.push_back(registration.name);
        }
        m_schedulers.clear();
        m_targets.clear();
        m_mutexes.clear();
        m_fiber_functions.clear();
    }
    // 注册时桥接过 DebugServer（组件的 dump provider 捕获对象地址）。对象
    // 销毁前必须同时移除这些组件，否则后续 `DUMP ALL` 会访问悬空指针。
    // 只移除本 Agent 自己注册过的名字，不碰其它组件。
    for (const auto& name : component_names) {
        (void)DebugServer::Instance().Unregister(name);
    }
}

// ---------------------------------------------------------------------------
// 诊断 Fiber：在 Scheduler 的受管 G 上下文里执行注册函数
// ---------------------------------------------------------------------------

bool DiagnosticAgent::InstallDiagnosticFiber(const std::string& name,
                                             Scheduler& scheduler) {
    std::lock_guard<std::mutex> lock(m_fiber_mutex);
    if (m_fiber_installed.load(std::memory_order_acquire)) {
        return false;
    }
    m_fiber_name = name.empty() ? "diagnostic" : name;
    m_fiber_scheduler = &scheduler;
    m_fiber_job = FiberJob{};
    auto task = scheduler.spawn([this] { DiagnosticFiberLoop(); });
    if (!task) {
        m_fiber_scheduler = nullptr;
        return false;
    }
    m_fiber_task = task;
    m_fiber_installed.store(true, std::memory_order_release);

    // 内置 `traverse`：只读遍历、输出当前 GMP 坐标。它必须在 Fiber 内执行，
    // 因此不能放进普通命令表。
    auto traverse = [&scheduler]() -> std::string {
        Json out = Json::object();
        out["schema"] = "go2cpp.diagnostic.traverse/1";
        if (const auto current = Scheduler::current_task()) {
            out["context"]["gid"] = static_cast<std::int64_t>(current->id());
        } else {
            out["context"]["gid"] = 0;
        }
        out["context"]["managed"] =
            Scheduler::current_task() != nullptr;
        out["context"]["processor_id"] = static_cast<std::int64_t>(
            Scheduler::current_processor_id());
        out["context"]["machine_id"] = static_cast<std::int64_t>(
            Scheduler::current_machine_id());
        Json processors = Json::array();
        for (const auto& processor : scheduler.processors()) {
            Json item = Json::object();
            item["id"] = static_cast<std::int64_t>(processor.id);
            item["state"] = PStateNameStable(processor.state);
            item["queued"] = static_cast<std::int64_t>(processor.queued);
            processors.push_back(std::move(item));
        }
        out["processors"] = std::move(processors);
        Json machines = Json::array();
        for (const auto& machine : scheduler.machines()) {
            Json item = Json::object();
            item["id"] = static_cast<std::int64_t>(machine.id);
            item["state"] = MStateNameStable(machine.state);
            item["processor"] = static_cast<std::int64_t>(machine.processor);
            machines.push_back(std::move(item));
        }
        out["machines"] = std::move(machines);
        Json tasks = Json::array();
        std::size_t waiting = 0;
        for (const auto& task : scheduler.DebugSnapshotTasks()) {
            Json item = Json::object();
            item["gid"] = static_cast<std::int64_t>(task.id);
            item["state"] = GStateNameStable(task.state);
            item["queued"] = task.queued;
            item["last_processor_id"] =
                static_cast<std::int64_t>(task.last_processor_id);
            if (task.state == GState::kWaiting) {
                ++waiting;
            }
            tasks.push_back(std::move(item));
        }
        out["tasks"] = std::move(tasks);
        out["tasks_waiting"] = static_cast<std::int64_t>(waiting);
        return out.dump();
    };
    (void)RegisterFiberFunction("traverse", std::move(traverse));
    return true;
}

bool DiagnosticAgent::diagnostic_fiber_installed() const {
    return m_fiber_installed.load(std::memory_order_acquire);
}

void DiagnosticAgent::StopDiagnosticFiber() {
    {
        std::lock_guard<std::mutex> lock(m_fiber_mutex);
        if (!m_fiber_installed.load(std::memory_order_acquire)) {
            return;
        }
        m_fiber_job.stop = true;
        m_fiber_job.has_job = false;
    }
    m_fiber_wake.notify_all();
    if (m_fiber_scheduler != nullptr) {
        try {
            (void)m_fiber_scheduler->ForceWakeIdle();
        } catch (...) {
        }
    }
    m_fiber_installed.store(false, std::memory_order_release);
}

void DiagnosticAgent::DiagnosticFiberLoop() {
    for (;;) {
        std::unique_lock<std::mutex> lock(m_fiber_mutex);
        m_fiber_wake.wait(lock, [this] {
            return m_fiber_job.stop || m_fiber_job.has_job;
        });
        if (m_fiber_job.stop) {
            return;
        }
        const std::string function_name = m_fiber_job.function_name;
        std::function<std::string()> function = m_fiber_job.function;
        m_fiber_job.has_job = false;
        lock.unlock();

        std::string output;
        std::string error;
        bool ok = true;
        try {
            output = function ? function() : std::string();
        } catch (const std::exception& exception) {
            ok = false;
            error = exception.what();
        } catch (...) {
            ok = false;
            error = "unknown-exception";
        }
        (void)function_name;

        lock.lock();
        m_fiber_job.done = true;
        m_fiber_job.ok = ok;
        m_fiber_job.result = std::move(output);
        m_fiber_job.error = std::move(error);
        lock.unlock();
        m_fiber_done.notify_all();
    }
}

bool DiagnosticAgent::RunOnDiagnosticFiber(const std::string& function_name,
                                           std::string* output,
                                           std::string* error,
                                           std::uint64_t timeout_ms) {
    std::function<std::string()> function;
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        for (const auto& entry : m_fiber_functions) {
            if (entry.first == function_name) {
                function = entry.second;
                break;
            }
        }
    }
    if (!function) {
        if (error != nullptr) {
            *error = "unknown-function";
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_fiber_mutex);
        if (!m_fiber_installed.load(std::memory_order_acquire)) {
            if (error != nullptr) {
                *error = "fiber-not-installed";
            }
            return false;
        }
        if (m_fiber_job.has_job) {
            if (error != nullptr) {
                *error = "fiber-busy";
            }
            return false;
        }
        m_fiber_job.has_job = true;
        m_fiber_job.done = false;
        m_fiber_job.ok = true;
        m_fiber_job.function = function;
        m_fiber_job.function_name = function_name;
        m_fiber_job.result.clear();
        m_fiber_job.error.clear();
    }
    m_fiber_jobs.fetch_add(1, std::memory_order_relaxed);
    m_fiber_wake.notify_one();
    // 死锁场景下 worker 可能全 park：Fiber 已被唤醒入队，但没有 worker 取。
    // 这条调用是「诊断 Fiber 的独立注入路径」：wake（G 入队）+ ForceWakeIdle
    // （M 醒来取队列），两步都走既有 API。
    Scheduler* scheduler = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_fiber_mutex);
        scheduler = m_fiber_scheduler;
    }
    if (scheduler != nullptr) {
        (void)scheduler->ForceWakeIdle();
    }
    std::unique_lock<std::mutex> lock(m_fiber_mutex);
    if (!m_fiber_done.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [this] { return m_fiber_job.done; })) {
        m_fiber_job.has_job = false;
        m_fiber_failures.fetch_add(1, std::memory_order_relaxed);
        if (error != nullptr) {
            *error = "fiber-timeout";
        }
        return false;
    }
    if (!m_fiber_job.ok) {
        m_fiber_failures.fetch_add(1, std::memory_order_relaxed);
        if (error != nullptr) {
            *error = m_fiber_job.error.empty() ? "fiber-function-failed"
                                               : m_fiber_job.error;
        }
        return false;
    }
    if (output != nullptr) {
        *output = m_fiber_job.result;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 命令入口与分发
// ---------------------------------------------------------------------------

DiagnosticCommandResult DiagnosticAgent::Execute(const std::string& command) {
    return RunCommand(command, 0);
}

DiagnosticCommandResult DiagnosticAgent::RunCommand(
    const std::string& command, std::uint64_t queue_wait_us) {
    std::lock_guard<std::mutex> gate(m_exec_mutex);
    const std::uint32_t concurrent =
        m_concurrent.fetch_add(1, std::memory_order_acq_rel) + 1U;
    std::uint64_t observed = m_concurrent_peak.load(std::memory_order_relaxed);
    while (concurrent > observed &&
           !m_concurrent_peak.compare_exchange_weak(
               observed, concurrent, std::memory_order_relaxed)) {
    }
    const std::uint64_t start_ns = NowNs();
    DiagnosticCommandResult result;
    try {
        if (command.size() > 4096U) {
            result = Fail("bad-command", "command-too-long");
        } else {
            const CommandLine line = ParseCommandLine(command);
            if (line.name.empty()) {
                result = Fail("bad-command", "empty-command");
            } else if (line.name == kDoctorCmdStatus) {
                result = CmdStatus(line.argument);
            } else if (line.name == kDoctorCmdDump) {
                result = CmdDump(line.argument);
            } else if (line.name == kDoctorCmdForceWake) {
                result = CmdForceWake(line.argument);
            } else if (line.name == kDoctorCmdDeadlockReport) {
                result = CmdDeadlockReport(line.argument);
            } else if (line.name == kDoctorCmdMutexUnlock) {
                result = CmdMutexUnlock(line.argument);
            } else if (line.name == kDoctorCmdInjectFiber) {
                result = CmdInjectFiber(line.argument);
            } else if (line.name == kDoctorCmdStats) {
                result = CmdStats(line.argument);
            } else if (line.name == kDoctorCmdPing) {
                result.ok = true;
                result.payload = "pong";
            } else if (line.name == kDoctorCmdHelp) {
                result.ok = true;
                result.payload = HelpText();
            } else {
                result = Fail("unknown-command", line.name);
            }
        }
    } catch (const std::exception& exception) {
        result = Fail("internal", exception.what());
    } catch (...) {
        result = Fail("internal", "command-exception");
    }
    m_concurrent.fetch_sub(1, std::memory_order_acq_rel);
    result.queue_wait_us = queue_wait_us;
    result.duration_us = ToUs(ElapsedNs(start_ns));
    if (!result.ok) {
        m_failed.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

DiagnosticCommandResult DiagnosticAgent::Fail(std::string code,
                                              std::string message) const {
    DiagnosticCommandResult result;
    result.ok = false;
    result.error_code = code;
    result.error_message = message;
    result.payload = code + ": " + message;
    return result;
}

bool DiagnosticAgent::SendResult(const DiagnosticEnvelope& request,
                                 const DiagnosticCommandResult& result) {
    std::string payload = result.payload;
    if (result.json) {
        const auto parsed = ParseJson(payload);
        if (parsed.ok() && parsed.value.is_object()) {
            Json data = parsed.value;
            data["doctor_duration_us"] =
                static_cast<std::int64_t>(result.duration_us);
            data["doctor_queue_wait_us"] =
                static_cast<std::int64_t>(result.queue_wait_us);
            payload = JsonText(data);
        }
    } else {
        payload += "\n[doctor duration_us=" +
                   std::to_string(result.duration_us) +
                   " queue_wait_us=" +
                   std::to_string(result.queue_wait_us) +
                   " ok=" + BoolText(result.ok) + "]\n";
    }

    const std::uint16_t base_flags =
        result.json ? static_cast<std::uint16_t>(kDiagnosticFlagJson) : 0U;
    const DiagnosticFrameKind kind =
        result.ok ? DiagnosticFrameKind::kResponse : DiagnosticFrameKind::kError;
    const std::size_t chunk =
        m_config.chunk_bytes == 0U ? payload.size() : m_config.chunk_bytes;
    bool all_sent = true;
    if (chunk == 0U || payload.size() <= chunk) {
        DiagnosticFrame frame;
        frame.kind = kind;
        frame.flags = base_flags;
        frame.stream_id = request.frame.stream_id;
        frame.seq = 0;
        frame.payload = std::move(payload);
        DiagnosticEnvelope response;
        response.frame = std::move(frame);
        response.peer = request.peer;
        std::string error;
        all_sent = m_transport && m_transport->Send(response, &error);
    } else {
        std::uint32_t seq = 0;
        for (std::size_t offset = 0; offset < payload.size(); offset += chunk) {
            const std::size_t length = std::min(chunk, payload.size() - offset);
            const bool last = offset + length >= payload.size();
            DiagnosticFrame frame;
            frame.kind = kind;
            frame.flags = static_cast<std::uint16_t>(
                base_flags | kDiagnosticFlagChunked |
                (last ? static_cast<std::uint16_t>(kDiagnosticFlagLastChunk)
                      : static_cast<std::uint16_t>(0U)));
            frame.stream_id = request.frame.stream_id;
            frame.seq = seq++;
            frame.payload = payload.substr(offset, length);
            DiagnosticEnvelope response;
            response.frame = std::move(frame);
            response.peer = request.peer;
            std::string error;
            if (!m_transport || !m_transport->Send(response, &error)) {
                all_sent = false;
                break;
            }
        }
    }
    if (!all_sent) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    return all_sent;
}

// ---------------------------------------------------------------------------
// status：协程数 / M-P / runqueue / 锁等待 / 模块基址与 build-id
// ---------------------------------------------------------------------------

DiagnosticCommandResult DiagnosticAgent::CmdStatus(const std::string& argument) {
    const Arguments arguments = ParseArguments(argument);
    const bool want_json = arguments.Get("format") == "json" ||
                           arguments.Get("json") == "1" ||
                           arguments.Positional(0) == "json";
    const DiagnosticAgentStats agent_stats = stats();

    std::vector<std::pair<std::string, Scheduler*>> schedulers;
    std::vector<std::shared_ptr<WakeTarget>> targets;
    std::vector<MutexRegistration> mutexes;
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        schedulers = m_schedulers;
        targets = m_targets;
        mutexes = m_mutexes;
    }

    Json root = Json::object();
    root["schema"] = "go2cpp.diagnostic.status/1";
    root["version"] = 1;
    root["protocol"] = kDiagnosticProtocol;
    root["pid"] = static_cast<std::int64_t>(::getpid());
    const std::uint64_t uptime_ms =
        m_start_ns == 0U ? 0U : ToMs(ElapsedNs(m_start_ns));
    root["uptime_ms"] = static_cast<std::int64_t>(uptime_ms);
    root["doctor"] = Json::object();
    root["doctor"]["running"] = running();
    root["doctor"]["transport"] = transport_name();
    root["doctor"]["queue_depth"] =
        static_cast<std::int64_t>(agent_stats.queue_depth);
    root["doctor"]["accepted"] =
        static_cast<std::int64_t>(agent_stats.accepted);
    root["doctor"]["executed"] =
        static_cast<std::int64_t>(agent_stats.executed);
    root["doctor"]["failed"] = static_cast<std::int64_t>(agent_stats.failed);
    root["doctor"]["rejected"] =
        static_cast<std::int64_t>(agent_stats.rejected);
    root["doctor"]["dropped"] = static_cast<std::int64_t>(agent_stats.dropped);
    root["doctor"]["max_queue_depth"] =
        static_cast<std::int64_t>(agent_stats.max_queue_depth);
    root["doctor"]["concurrent_peak"] =
        static_cast<std::int64_t>(agent_stats.concurrent_peak);
    root["doctor"]["deadlock_events"] =
        static_cast<std::int64_t>(agent_stats.deadlock_events);
    root["doctor"]["deadlock_verdict"] = VerdictName(deadlock_verdict());
    root["doctor"]["no_progress_ms"] =
        static_cast<std::int64_t>(no_progress_ms());
    root["doctor"]["diagnostic_fiber"] = diagnostic_fiber_installed();
    root["doctor"]["fiber_functions"] = FiberFunctions();

    Json schedulers_json = Json::array();
    Json processors_json = Json::array();
    Json machines_json = Json::array();
    Json per_processor = Json::array();
    std::size_t goroutine_total = 0;
    std::size_t goroutine_running = 0;
    std::size_t goroutine_runnable = 0;
    std::size_t goroutine_waiting = 0;
    std::size_t goroutine_dead = 0;
    std::size_t runqueue_local = 0;
    std::size_t runqueue_global = 0;
    std::size_t machine_sequence = 0;
    for (const auto& entry : schedulers) {
        Scheduler* scheduler = entry.second;
        if (scheduler == nullptr) {
            continue;
        }
        Json item = Json::object();
        item["name"] = entry.first;
        item["processors"] =
            static_cast<std::int64_t>(scheduler->processor_count());
        item["workers"] =
            static_cast<std::int64_t>(scheduler->worker_count());
        item["runnable"] =
            static_cast<std::int64_t>(scheduler->runnable_count());
        item["runnable_global"] =
            static_cast<std::int64_t>(scheduler->global_runnable_count());
        item["running"] = scheduler->is_running();
        item["sysmon_running"] = scheduler->sysmon_running();
        const auto metrics = scheduler->metrics();
        item["metrics"] = Json::object();
        item["metrics"]["task_runs"] =
            static_cast<std::int64_t>(metrics.task_runs);
        item["metrics"]["task_completions"] =
            static_cast<std::int64_t>(metrics.task_completions);
        item["metrics"]["fiber_resume_ns"] =
            static_cast<std::int64_t>(metrics.fiber_resume_ns);
        Json scheduler_processors = Json::array();
        for (const auto& processor : scheduler->processors()) {
            Json entry_processor = Json::object();
            entry_processor["id"] = static_cast<std::int64_t>(processor.id);
            entry_processor["state"] = PStateNameStable(processor.state);
            entry_processor["queued"] =
                static_cast<std::int64_t>(processor.queued);
            scheduler_processors.push_back(entry_processor);
            processors_json.push_back(std::move(entry_processor));
            per_processor.push_back(
                static_cast<std::int64_t>(processor.queued));
            runqueue_local += processor.queued;
        }
        item["processors_detail"] = std::move(scheduler_processors);
        Json scheduler_machines = Json::array();
        for (const auto& machine : scheduler->machines()) {
            Json entry_machine = Json::object();
            entry_machine["id"] = static_cast<std::int64_t>(machine.id);
            entry_machine["state"] = MStateNameStable(machine.state);
            entry_machine["processor"] =
                static_cast<std::int64_t>(machine.processor);
            entry_machine["detached"] = machine.processor_detached;
            entry_machine["replacement"] = machine.replacement;
            entry_machine["blocking_task"] =
                static_cast<std::int64_t>(machine.blocking_task);
            entry_machine["index"] =
                static_cast<std::int64_t>(machine_sequence++);
            scheduler_machines.push_back(entry_machine);
            machines_json.push_back(std::move(entry_machine));
        }
        item["machines_detail"] = std::move(scheduler_machines);
        std::size_t scheduler_waiting = 0;
        for (const auto& task : scheduler->DebugSnapshotTasks()) {
            ++goroutine_total;
            switch (task.state) {
                case GState::kRunning: ++goroutine_running; break;
                case GState::kRunnable:
                case GState::kNew: ++goroutine_runnable; break;
                case GState::kWaiting:
                    ++goroutine_waiting;
                    ++scheduler_waiting;
                    break;
                case GState::kDead:
                case GState::kCancelled:
                case GState::kFailed: ++goroutine_dead; break;
            }
        }
        item["tasks_waiting"] =
            static_cast<std::int64_t>(scheduler_waiting);
        runqueue_global += scheduler->global_runnable_count();
        schedulers_json.push_back(std::move(item));
    }
    root["schedulers"] = std::move(schedulers_json);
    root["processors"] = std::move(processors_json);
    root["machines"] = std::move(machines_json);
    root["goroutines"] = Json::object();
    root["goroutines"]["total"] = static_cast<std::int64_t>(goroutine_total);
    root["goroutines"]["running"] =
        static_cast<std::int64_t>(goroutine_running);
    root["goroutines"]["runnable"] =
        static_cast<std::int64_t>(goroutine_runnable);
    root["goroutines"]["waiting"] =
        static_cast<std::int64_t>(goroutine_waiting);
    root["goroutines"]["dead"] = static_cast<std::int64_t>(goroutine_dead);
    root["runqueue"] = Json::object();
    root["runqueue"]["local_total"] =
        static_cast<std::int64_t>(runqueue_local);
    root["runqueue"]["global"] = static_cast<std::int64_t>(runqueue_global);
    root["runqueue"]["per_p"] = std::move(per_processor);

    Json waits = Json::object();
    for (const auto& target : targets) {
        if (!target) {
            continue;
        }
        Json item = Json::object();
        switch (target->support) {
            case WakeSupport::kSchedulerIdle:
                item["kind"] = kWakeScopeSchedulerIdle;
                item["retryable"] = true;
                break;
            case WakeSupport::kParkingCondition:
                item["kind"] = kWakeScopeParkingCondition;
                item["retryable"] = true;
                break;
            case WakeSupport::kChannel:
                item["kind"] = kWakeScopeChannel;
                item["retryable"] = true;
                break;
            default:
                item["kind"] = "unsupported";
                item["retryable"] = false;
                break;
        }
        try {
            item["describe"] = target->describe ? target->describe() : "";
        } catch (...) {
            item["describe"] = "unavailable";
        }
        waits[target->name] = std::move(item);
    }
    for (const auto& registration : mutexes) {
        if (registration.mutex == nullptr) {
            continue;
        }
        Json item = Json::object();
        item["kind"] = "mutex";
        item["retryable"] = false;
        try {
            item["describe"] = DescribeMutex(*registration.mutex);
        } catch (...) {
            item["describe"] = "unavailable";
        }
        waits[registration.name] = std::move(item);
    }
    root["waits"] = std::move(waits);

    Json modules = Json::array();
    try {
        for (const auto& module : CollectModules()) {
            Json item = Json::object();
            item["role"] = module.role;
            item["path"] = module.path;
            item["base"] = HexAddress(module.base);
            item["build_id"] =
                module.build_id_known ? module.build_id : "unavailable";
            modules.push_back(std::move(item));
        }
    } catch (...) {
    }
    root["modules"] = std::move(modules);

    DiagnosticCommandResult result;
    result.ok = true;
    result.json = want_json;
    if (want_json) {
        result.payload = JsonText(root);
        return result;
    }

    TextDump text;
    text.Head("go2cpp.diagnostic.status/1");
    text.Key("protocol", kDiagnosticProtocol);
    text.Key("pid", static_cast<std::int64_t>(::getpid()));
    text.Key("uptime_ms", uptime_ms);
    text.Key("doctor.running", running());
    text.Key("doctor.transport", transport_name());
    text.Key("doctor.queue_depth", agent_stats.queue_depth);
    text.Key("doctor.accepted", agent_stats.accepted);
    text.Key("doctor.executed", agent_stats.executed);
    text.Key("doctor.failed", agent_stats.failed);
    text.Key("doctor.rejected", agent_stats.rejected);
    text.Key("doctor.dropped", agent_stats.dropped);
    text.Key("doctor.max_queue_depth", agent_stats.max_queue_depth);
    text.Key("doctor.concurrent_peak", agent_stats.concurrent_peak);
    text.Key("doctor.deadlock_verdict", VerdictName(deadlock_verdict()));
    text.Key("doctor.no_progress_ms", no_progress_ms());
    text.Key("doctor.diagnostic_fiber", diagnostic_fiber_installed());
    text.Key("scheduler.count",
             static_cast<std::uint64_t>(schedulers.size()));
    for (std::size_t index = 0; index < schedulers.size(); ++index) {
        Scheduler* scheduler = schedulers[index].second;
        if (scheduler == nullptr) {
            continue;
        }
        const std::string prefix = "scheduler." + std::to_string(index) + ".";
        text.Key(prefix + "name", schedulers[index].first);
        text.Key(prefix + "processors", scheduler->processor_count());
        text.Key(prefix + "workers", scheduler->worker_count());
        text.Key(prefix + "runnable", scheduler->runnable_count());
        text.Key(prefix + "runnable_global",
                 scheduler->global_runnable_count());
    }
    text.Key("goroutines.total", goroutine_total);
    text.Key("goroutines.running", goroutine_running);
    text.Key("goroutines.runnable", goroutine_runnable);
    text.Key("goroutines.waiting", goroutine_waiting);
    text.Key("goroutines.dead", goroutine_dead);
    text.Key("runqueue.local_total", runqueue_local);
    text.Key("runqueue.global", runqueue_global);
    std::string per_processor_text;
    for (const auto& value : root["runqueue"]["per_p"]) {
        if (!per_processor_text.empty()) {
            per_processor_text.push_back(',');
        }
        per_processor_text += std::to_string(value.get<std::int64_t>());
    }
    text.Key("runqueue.per_p",
             per_processor_text.empty() ? "-" : per_processor_text);
    std::size_t machine_index = 0;
    for (const auto& entry : schedulers) {
        Scheduler* scheduler = entry.second;
        if (scheduler == nullptr) {
            continue;
        }
        for (const auto& machine : scheduler->machines()) {
            const std::string prefix =
                "machine." + std::to_string(machine_index++) + ".";
            text.Key(prefix + "id", machine.id);
            text.Key(prefix + "state", MStateNameStable(machine.state));
            text.Key(prefix + "processor", machine.processor);
            text.Key(prefix + "detached", machine.processor_detached);
        }
        for (const auto& processor : scheduler->processors()) {
            const std::string prefix =
                "processor." + std::to_string(processor.id) + ".";
            text.Key(prefix + "state", PStateNameStable(processor.state));
            text.Key(prefix + "queued", processor.queued);
        }
    }
    for (const auto& target : targets) {
        if (!target) {
            continue;
        }
        std::string describe;
        try {
            describe = target->describe ? target->describe() : "";
        } catch (...) {
            describe = "unavailable";
        }
        text.Key("waits." + target->name, describe);
    }
    for (const auto& registration : mutexes) {
        if (registration.mutex == nullptr) {
            continue;
        }
        text.Key("waits." + registration.name,
                 DescribeMutex(*registration.mutex));
    }
    try {
        std::size_t module_index = 0;
        for (const auto& module : CollectModules()) {
            const std::string prefix =
                "module." + std::to_string(module_index++) + ".";
            text.Key(prefix + "role", module.role);
            text.Key(prefix + "path", module.path);
            text.KeyHex(prefix + "base", module.base);
            text.Key(prefix + "build_id",
                     module.build_id_known ? module.build_id
                                           : std::string("unavailable"));
        }
    } catch (...) {
    }
    result.payload = text.Take();
    return result;
}

// ---------------------------------------------------------------------------
// dump：复用 DebugServer 的组件 dump
// ---------------------------------------------------------------------------

DiagnosticCommandResult DiagnosticAgent::CmdDump(const std::string& argument) {
    const Arguments arguments = ParseArguments(argument);
    std::string sections = arguments.Get("sections");
    if (sections.empty()) {
        sections = arguments.Positional(0);
    }
    if (sections.empty()) {
        sections = "all";
    }
    const bool want_json = arguments.Get("format") == "json" ||
                           arguments.Get("json") == "1";
    DiagnosticCommandResult result;
    result.ok = true;
    std::string output;
    bool available = false;
    for (const std::string& raw : Split(sections, ',')) {
        const std::string name = Trim(raw);
        if (name.empty()) {
            continue;
        }
        const std::string lookup =
            (name == "all" || name == "ALL" || name == "*") ? "all" : name;
        DumpPayload payload;
        try {
            payload = DebugServer::Instance().Dump(lookup);
        } catch (const std::exception& exception) {
            output += "# component " + name + " failed: " + exception.what() +
                      "\n";
            continue;
        }
        if (!payload.available) {
            output += "# component " + name + " unavailable: " +
                      payload.reason + "\n";
            continue;
        }
        available = true;
        output += "===== component " + name + " =====\n";
        output += want_json ? payload.json : payload.text;
        if (output.empty() || output.back() != '\n') {
            output.push_back('\n');
        }
    }
    if (!available && output.empty()) {
        return Fail("no-available-component",
                    "no component produced a dump");
    }
    result.json = false;
    result.payload = std::move(output);
    return result;
}

// ---------------------------------------------------------------------------
// force-wake：白名单 + refused 判定
// ---------------------------------------------------------------------------

std::size_t DiagnosticAgent::ForceWakeScope(const std::string& scope,
                                            bool* refused,
                                            std::string* reason,
                                            std::string* detail) {
    if (refused != nullptr) {
        *refused = false;
    }
    if (reason != nullptr) {
        reason->clear();
    }
    if (detail != nullptr) {
        detail->clear();
    }
    if (scope == kWakeScopeMutex) {
        if (refused != nullptr) {
            *refused = true;
        }
        if (reason != nullptr) {
            *reason = "mutex-waiters-are-not-retryable";
        }
        if (detail != nullptr) {
            *detail =
                "sync::Mutex 的交接是 FIFO 所有权转移：被唤醒者不会重检"
                "“锁是否仍可用”，盲目唤醒会破坏 kLocked/kWoken 状态。"
                "请使用 mutex-unlock（它会先证明 owner 已不可能再访问临界区）";
        }
        return 0;
    }

    std::vector<std::shared_ptr<WakeTarget>> targets;
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        targets = m_targets;
    }
    std::size_t total = 0;
    std::size_t matched = 0;
    const auto run = [&](WakeSupport support, const char* label) {
        std::size_t woken = 0;
        std::size_t count = 0;
        for (const auto& target : targets) {
            if (!target || target->support != support) {
                continue;
            }
            ++count;
            try {
                woken += target->wake ? target->wake() : 0U;
            } catch (...) {
                if (detail != nullptr) {
                    *detail += std::string(label) + "." + target->name +
                               "=exception ";
                }
            }
        }
        matched += count;
        total += woken;
        if (detail != nullptr) {
            *detail += std::string("woken.") + label + "=" +
                       std::to_string(woken) + " targets=" +
                       std::to_string(count) + " ";
        }
    };

    // 顺序很重要：先把 G 重新入队（parking/channel），最后再唤醒空闲 worker。
    // 反过来的话，worker 可能在被唤醒后立刻检查到空 runqueue 又睡回去，而随后
    // 入队的 G 没有新的 notify 去踢它（虽然 wake 路径通常也会 notify，但诊断
    // 层不应依赖这个隐含行为）。
    if (scope == kWakeScopeParkingCondition || scope == kWakeScopeAll) {
        run(WakeSupport::kParkingCondition, kWakeScopeParkingCondition);
    }
    if (scope == kWakeScopeChannel || scope == kWakeScopeAll) {
        run(WakeSupport::kChannel, kWakeScopeChannel);
    }
    if (scope == kWakeScopeSchedulerIdle || scope == kWakeScopeAll) {
        run(WakeSupport::kSchedulerIdle, kWakeScopeSchedulerIdle);
    }
    if (matched == 0U && detail != nullptr) {
        *detail += "skipped=no-registered-target ";
    }
    return total;
}

DiagnosticCommandResult DiagnosticAgent::CmdForceWake(
    const std::string& argument) {
    const Arguments arguments = ParseArguments(argument);
    std::string scope = arguments.Get("scope");
    if (scope.empty()) {
        scope = arguments.Positional(0);
    }
    if (scope.empty()) {
        scope = kWakeScopeAll;
    }
    scope = Lower(scope);
    const bool known =
        scope == kWakeScopeSchedulerIdle ||
        scope == kWakeScopeParkingCondition || scope == kWakeScopeChannel ||
        scope == kWakeScopeAll || scope == kWakeScopeMutex;
    if (!known) {
        return Fail("bad-scope",
                    "scope must be scheduler-idle|parking-condition|channel|all");
    }

    bool refused = false;
    std::string reason;
    std::string detail;
    const std::size_t woken =
        ForceWakeScope(scope, &refused, &reason, &detail);
    const char* verdict = refused ? kVerdictRefused : kVerdictOk;
    if (!refused && woken == 0U && detail.find("skipped=") != std::string::npos) {
        verdict = kVerdictOk;
    }
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    GO2CPP_DEBUG_EVENT(kDiagnosticForceWake, 0,
                       static_cast<std::uint64_t>(woken));
#endif

    DiagnosticCommandResult result;
    result.ok = true;
    const bool want_json = arguments.Get("format") == "json" ||
                           arguments.Get("json") == "1";
    result.json = want_json;
    if (want_json) {
        Json root = Json::object();
        root["schema"] = "go2cpp.diagnostic.force-wake/1";
        root["version"] = 1;
        root["scope"] = scope;
        root["verdict"] = verdict;
        root["woken_total"] = static_cast<std::int64_t>(woken);
        root["refused_reason"] = reason;
        root["detail"] = detail;
        result.payload = JsonText(root);
    } else {
        result.payload = "schema=go2cpp.diagnostic.force-wake/1\n";
        result.payload += "version=1\n";
        result.payload += "scope=" + scope + "\n";
        result.payload += std::string("verdict=") + verdict + "\n";
        result.payload += "woken_total=" + std::to_string(woken) + "\n";
        if (!reason.empty()) {
            result.payload += "refused_reason=" + reason + "\n";
        }
        if (!detail.empty()) {
            result.payload += "detail=" + detail + "\n";
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// deadlock-report：等待图
// ---------------------------------------------------------------------------

std::string DiagnosticAgent::BuildDeadlockText(bool json) {
    std::vector<std::pair<std::string, Scheduler*>> schedulers;
    std::vector<std::shared_ptr<WakeTarget>> targets;
    std::vector<MutexRegistration> mutexes;
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        schedulers = m_schedulers;
        targets = m_targets;
        mutexes = m_mutexes;
    }

    // 先收集全部活动 G，供 Mutex 的 owner 存活判定使用。
    std::map<std::uint64_t, GState> live_tasks;
    for (const auto& entry : schedulers) {
        if (entry.second == nullptr) {
            continue;
        }
        for (const auto& task : entry.second->DebugSnapshotTasks()) {
            live_tasks[task.id] = task.state;
        }
    }

    Json root = Json::object();
    root["schema"] = "go2cpp.diagnostic.deadlock-report/1";
    root["version"] = 1;
    root["verdict"] = VerdictName(deadlock_verdict());
    root["no_progress_ms"] = static_cast<std::int64_t>(no_progress_ms());
    root["threshold_ms"] =
        static_cast<std::int64_t>(m_config.deadlock_threshold.count());
    root["auto_force_wake"] = m_config.auto_force_wake;
    root["detection_enabled"] = m_config.enable_deadlock_detection;
    Json edges = Json::array();
    Json schedulers_json = Json::array();

    for (const auto& entry : schedulers) {
        Scheduler* scheduler = entry.second;
        if (scheduler == nullptr) {
            continue;
        }
        std::size_t parked_machines = 0;
        std::size_t running_machines = 0;
        std::size_t blocking_machines = 0;
        for (const auto& machine : scheduler->machines()) {
            switch (machine.state) {
                case MState::kParked:
                case MState::kIdle: ++parked_machines; break;
                case MState::kRunning: ++running_machines; break;
                case MState::kBlocking: ++blocking_machines; break;
                default: break;
            }
        }
        Json item = Json::object();
        item["name"] = entry.first;
        item["runnable"] =
            static_cast<std::int64_t>(scheduler->runnable_count());
        item["runnable_global"] =
            static_cast<std::int64_t>(scheduler->global_runnable_count());
        item["workers"] =
            static_cast<std::int64_t>(scheduler->worker_count());
        item["parked_machines"] =
            static_cast<std::int64_t>(parked_machines);
        item["running_machines"] =
            static_cast<std::int64_t>(running_machines);
        item["blocking_machines"] =
            static_cast<std::int64_t>(blocking_machines);
        schedulers_json.push_back(item);

        Json edge = Json::object();
        edge["from"] = "scheduler:" + entry.first;
        edge["kind"] = kWakeScopeSchedulerIdle;
        edge["parked_machines"] =
            static_cast<std::int64_t>(parked_machines);
        edge["running_machines"] =
            static_cast<std::int64_t>(running_machines);
        edge["runnable"] =
            static_cast<std::int64_t>(scheduler->runnable_count());
        edge["retryable"] = true;
        edges.push_back(std::move(edge));
    }

    for (const auto& target : targets) {
        if (!target) {
            continue;
        }
        Json edge = Json::object();
        edge["from"] = target->name;
        switch (target->support) {
            case WakeSupport::kSchedulerIdle:
                edge["kind"] = kWakeScopeSchedulerIdle;
                edge["retryable"] = true;
                break;
            case WakeSupport::kParkingCondition:
                edge["kind"] = kWakeScopeParkingCondition;
                edge["retryable"] = true;
                break;
            case WakeSupport::kChannel:
                edge["kind"] = kWakeScopeChannel;
                edge["retryable"] = true;
                break;
            default:
                edge["kind"] = "unsupported";
                edge["retryable"] = false;
                break;
        }
        try {
            edge["describe"] = target->describe ? target->describe() : "";
        } catch (...) {
            edge["describe"] = "unavailable";
        }
        edges.push_back(std::move(edge));
    }

    for (const auto& registration : mutexes) {
        if (registration.mutex == nullptr) {
            continue;
        }
        const auto snapshot = registration.mutex->DebugSnapshotInternal();
        Json edge = Json::object();
        edge["from"] = registration.name;
        edge["kind"] = "mutex";
        edge["held"] = snapshot.held;
        edge["waiters"] = static_cast<std::int64_t>(snapshot.waiters);
        edge["native_waiters"] =
            static_cast<std::int64_t>(snapshot.native_waiters);
        edge["fiber_waiters"] =
            static_cast<std::int64_t>(snapshot.fiber_waiters);
        edge["owner_gid"] = static_cast<std::int64_t>(snapshot.owner_gid);
        edge["owner_known"] = snapshot.owner_known;
        bool owner_alive = false;
        if (snapshot.owner_known && snapshot.owner_gid != 0U) {
            const auto found = live_tasks.find(snapshot.owner_gid);
            if (found != live_tasks.end()) {
                switch (found->second) {
                    case GState::kNew:
                    case GState::kRunnable:
                    case GState::kRunning:
                    case GState::kWaiting:
                        owner_alive = true;
                        break;
                    default:
                        break;
                }
            }
        }
        edge["owner_alive"] = owner_alive;
        edge["retryable"] = false;
        edge["waiter_ids"] = "unavailable";
        edges.push_back(std::move(edge));
    }
    root["schedulers"] = std::move(schedulers_json);
    root["edges"] = std::move(edges);
    root["live_tasks"] = static_cast<std::int64_t>(live_tasks.size());
    Json events = Json::array();
    if (EventRing::Enabled()) {
        for (const auto& record : EventRing::Snapshot(20)) {
            Json item = Json::object();
            item["seq"] = static_cast<std::int64_t>(record.seq);
            item["ts_ns"] = static_cast<std::int64_t>(record.ts_ns);
            item["gid"] = static_cast<std::int64_t>(record.gid);
            item["code"] = static_cast<std::int64_t>(record.code);
            item["code_name"] =
                SchedEventCodeName(static_cast<SchedEventCode>(record.code));
            item["arg2"] = static_cast<std::int64_t>(record.arg2);
            item["pid"] = static_cast<std::int64_t>(record.pid);
            item["critical"] = SchedEventCodeIsCritical(
                static_cast<SchedEventCode>(record.code));
            events.push_back(std::move(item));
        }
    }
    root["events"] = std::move(events);

    if (json) {
        return JsonText(root);
    }

    TextDump text;
    text.Head("go2cpp.diagnostic.deadlock-report/1");
    text.Key("verdict", VerdictName(deadlock_verdict()));
    text.Key("no_progress_ms", no_progress_ms());
    text.Key("threshold_ms",
             static_cast<std::uint64_t>(m_config.deadlock_threshold.count()));
    text.Key("auto_force_wake", m_config.auto_force_wake);
    text.Key("detection_enabled", m_config.enable_deadlock_detection);
    text.Key("live_tasks", static_cast<std::uint64_t>(live_tasks.size()));
    std::size_t edge_index = 0;
    for (const auto& edge : root["edges"]) {
        const std::string prefix = "edge." + std::to_string(edge_index++) + ".";
        text.Key(prefix + "from", edge.value("from", std::string{}));
        text.Key(prefix + "kind", edge.value("kind", std::string{}));
        text.Key(prefix + "retryable",
                 edge.value("retryable", false));
        if (edge.contains("describe")) {
            text.Key(prefix + "describe",
                     edge.value("describe", std::string{}));
        }
        if (edge.contains("owner_gid")) {
            text.Key(prefix + "owner_gid",
                     edge.value("owner_gid", static_cast<std::int64_t>(0)));
            text.Key(prefix + "owner_known",
                     edge.value("owner_known", false));
            text.Key(prefix + "owner_alive",
                     edge.value("owner_alive", false));
            text.Key(prefix + "waiters",
                     edge.value("waiters", static_cast<std::int64_t>(0)));
        }
        if (edge.contains("parked_machines")) {
            text.Key(prefix + "parked_machines",
                     edge.value("parked_machines", static_cast<std::int64_t>(0)));
            text.Key(prefix + "running_machines",
                     edge.value("running_machines", static_cast<std::int64_t>(0)));
        }
    }
    for (const auto& record : EventRing::Snapshot(20)) {
        text.Raw("event.seq=" + std::to_string(record.seq) +
                 " ts_ns=" + std::to_string(record.ts_ns) +
                 " gid=" + std::to_string(record.gid) +
                 " code=" +
                 SchedEventCodeName(static_cast<SchedEventCode>(record.code)) +
                 " arg2=" + std::to_string(record.arg2) +
                 " pid=" + std::to_string(record.pid) + "\n");
    }
    return text.Take();
}

DiagnosticCommandResult DiagnosticAgent::CmdDeadlockReport(
    const std::string& argument) {
    const Arguments arguments = ParseArguments(argument);
    const bool want_json = arguments.Get("format") == "json" ||
                           arguments.Get("json") == "1";
    DiagnosticCommandResult result;
    result.ok = true;
    result.json = want_json;
    result.payload = BuildDeadlockText(want_json);
    return result;
}

// ---------------------------------------------------------------------------
// mutex-unlock：只有「owner 已不可能再访问临界区」才允许
// ---------------------------------------------------------------------------

DiagnosticCommandResult DiagnosticAgent::CmdMutexUnlock(
    const std::string& argument) {
    const Arguments arguments = ParseArguments(argument);
    std::string name = arguments.Get("name");
    if (name.empty()) {
        name = arguments.Positional(0);
    }
    if (name.empty()) {
        return Fail("bad-argument", "mutex-unlock requires name=<registered-mutex>");
    }
    sync::Mutex* mutex = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        for (const auto& registration : m_mutexes) {
            if (registration.name == name) {
                mutex = registration.mutex;
                break;
            }
        }
    }
    if (mutex == nullptr) {
        return Fail("not-registered", name);
    }

    const auto snapshot = mutex->DebugSnapshotInternal();
    if (!snapshot.held) {
        return Fail("refused", "not-locked: mutex is not currently held");
    }
    if (snapshot.waiters == 0U) {
        return Fail("refused", "no-waiters: nothing would be woken by unlocking");
    }
    if (!snapshot.owner_known || snapshot.owner_gid == 0U) {
        return Fail("refused",
                    "owner-unknown: Mutex keeps no authoritative owner; "
                    "cannot prove the critical section is abandoned");
    }

    // 在所有已注册 Scheduler 的活动 G 里查 owner。
    bool owner_alive = false;
    GState owner_state = GState::kDead;
    std::vector<std::pair<std::string, Scheduler*>> schedulers;
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        schedulers = m_schedulers;
    }
    for (const auto& entry : schedulers) {
        if (entry.second == nullptr) {
            continue;
        }
        for (const auto& task : entry.second->DebugSnapshotTasks()) {
            if (task.id != snapshot.owner_gid) {
                continue;
            }
            owner_state = task.state;
            switch (task.state) {
                case GState::kNew:
                case GState::kRunnable:
                case GState::kRunning:
                case GState::kWaiting:
                    owner_alive = true;
                    break;
                default:
                    break;
            }
        }
    }
    if (owner_alive) {
        return Fail("refused",
                    "owner-alive: owner gid " +
                        std::to_string(snapshot.owner_gid) +
                        " is still a live G (state=" +
                        GStateNameStable(owner_state) +
                        "); force-unlock would corrupt the mutex");
    }

    // owner 已不在任何活动 G 表中：临界区不可能再被访问，走既有 Unlock()。
    bool unlocked = false;
    std::string unlock_error;
    try {
        mutex->Unlock();
        unlocked = true;
    } catch (const std::exception& exception) {
        unlock_error = exception.what();
    } catch (...) {
        unlock_error = "unknown-exception";
    }
    if (!unlocked) {
        return Fail("refused", "unlock-failed: " + unlock_error);
    }
    const auto after = mutex->DebugSnapshotInternal();
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    GO2CPP_DEBUG_EVENT(kDiagnosticMutexUnlock, snapshot.owner_gid,
                       static_cast<std::uint64_t>(snapshot.waiters));
#endif

    DiagnosticCommandResult result;
    result.ok = true;
    const bool want_json = arguments.Get("format") == "json" ||
                           arguments.Get("json") == "1";
    result.json = want_json;
    if (want_json) {
        Json root = Json::object();
        root["schema"] = "go2cpp.diagnostic.mutex-unlock/1";
        root["version"] = 1;
        root["name"] = name;
        root["verdict"] = kVerdictOk;
        root["reason"] = "owner-gone";
        root["owner_gid"] = static_cast<std::int64_t>(snapshot.owner_gid);
        root["owner_state"] = GStateNameStable(owner_state);
        root["waiters_before"] =
            static_cast<std::int64_t>(snapshot.waiters);
        root["waiters_after"] =
            static_cast<std::int64_t>(after.waiters);
        root["held_after"] = after.held;
        result.payload = JsonText(root);
    } else {
        result.payload = "schema=go2cpp.diagnostic.mutex-unlock/1\n";
        result.payload += "name=" + name + "\n";
        result.payload += std::string("verdict=") + kVerdictOk + "\n";
        result.payload += "reason=owner-gone\n";
        result.payload += "owner_gid=" +
                          std::to_string(snapshot.owner_gid) + "\n";
        result.payload += "waiters_before=" +
                          std::to_string(snapshot.waiters) + "\n";
        result.payload += "waiters_after=" +
                          std::to_string(after.waiters) + "\n";
        result.payload += "held_after=" + BoolText(after.held) + "\n";
    }
    return result;
}

// ---------------------------------------------------------------------------
// inject-fiber：在受管 G 的 GMP 上下文里执行
// ---------------------------------------------------------------------------

DiagnosticCommandResult DiagnosticAgent::CmdInjectFiber(
    const std::string& argument) {
    const Arguments arguments = ParseArguments(argument);
    std::string function_name = arguments.Get("fn");
    if (function_name.empty()) {
        function_name = arguments.Positional(0);
    }
    if (function_name.empty()) {
        return Fail("bad-argument", "inject-fiber requires fn=<registered-function>");
    }
    std::uint64_t timeout_ms = 2000;
    if (!arguments.Get("timeout_ms").empty()) {
        try {
            timeout_ms = std::stoull(arguments.Get("timeout_ms"));
        } catch (...) {
            return Fail("bad-argument", "timeout_ms must be an integer");
        }
    }
    if (!diagnostic_fiber_installed()) {
        return Fail("fiber-not-installed",
                    "call InstallDiagnosticFiber() first");
    }
    const std::uint64_t start_ns = NowNs();
    std::string output;
    std::string error;
    const bool ok =
        RunOnDiagnosticFiber(function_name, &output, &error, timeout_ms);
    if (!ok) {
        return Fail(error == "fiber-timeout" ? "fiber-timeout"
                                             : "fiber-failed",
                    error);
    }
    DiagnosticCommandResult result;
    result.ok = true;
    const bool want_json = arguments.Get("format") == "json" ||
                           arguments.Get("json") == "1";
    result.json = want_json;
    if (want_json) {
        Json root = Json::object();
        root["schema"] = "go2cpp.diagnostic.inject-fiber/1";
        root["version"] = 1;
        root["function"] = function_name;
        root["duration_us"] =
            static_cast<std::int64_t>(ToUs(ElapsedNs(start_ns)));
        // 函数自己返回 JSON（traverse）时嵌入；否则作为字符串。
        const auto parsed = ParseJson(output);
        if (parsed.ok() && parsed.value.is_object()) {
            root["result"] = parsed.value;
        } else {
            root["result"] = output;
        }
        result.payload = JsonText(root);
    } else {
        result.payload = "schema=go2cpp.diagnostic.inject-fiber/1\n";
        result.payload += "function=" + function_name + "\n";
        result.payload += "duration_us=" +
                          std::to_string(ToUs(ElapsedNs(start_ns))) + "\n";
        result.payload += output;
        if (result.payload.empty() || result.payload.back() != '\n') {
            result.payload.push_back('\n');
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// stats / help
// ---------------------------------------------------------------------------

DiagnosticCommandResult DiagnosticAgent::CmdStats(const std::string& argument) {
    const Arguments arguments = ParseArguments(argument);
    const DiagnosticAgentStats snapshot = stats();
    DiagnosticCommandResult result;
    result.ok = true;
    const bool want_json = arguments.Get("format") == "json" ||
                           arguments.Get("json") == "1";
    result.json = want_json;
    if (want_json) {
        Json root = Json::object();
        root["schema"] = "go2cpp.diagnostic.stats/1";
        root["version"] = 1;
        root["accepted"] = static_cast<std::int64_t>(snapshot.accepted);
        root["executed"] = static_cast<std::int64_t>(snapshot.executed);
        root["failed"] = static_cast<std::int64_t>(snapshot.failed);
        root["rejected"] = static_cast<std::int64_t>(snapshot.rejected);
        root["dropped"] = static_cast<std::int64_t>(snapshot.dropped);
        root["queue_depth"] =
            static_cast<std::int64_t>(snapshot.queue_depth);
        root["max_queue_depth"] =
            static_cast<std::int64_t>(snapshot.max_queue_depth);
        root["concurrent_peak"] =
            static_cast<std::int64_t>(snapshot.concurrent_peak);
        root["deadlock_events"] =
            static_cast<std::int64_t>(snapshot.deadlock_events);
        root["auto_force_wakes"] =
            static_cast<std::int64_t>(snapshot.auto_force_wakes);
        root["fiber_jobs"] = static_cast<std::int64_t>(snapshot.fiber_jobs);
        root["fiber_failures"] =
            static_cast<std::int64_t>(snapshot.fiber_failures);
        root["deadlock_verdict"] = VerdictName(deadlock_verdict());
        root["no_progress_ms"] =
            static_cast<std::int64_t>(no_progress_ms());
        result.payload = JsonText(root);
    } else {
        result.payload = "schema=go2cpp.diagnostic.stats/1\n";
        result.payload += "accepted=" + std::to_string(snapshot.accepted) + "\n";
        result.payload += "executed=" + std::to_string(snapshot.executed) + "\n";
        result.payload += "failed=" + std::to_string(snapshot.failed) + "\n";
        result.payload += "rejected=" + std::to_string(snapshot.rejected) + "\n";
        result.payload += "dropped=" + std::to_string(snapshot.dropped) + "\n";
        result.payload += "queue_depth=" +
                          std::to_string(snapshot.queue_depth) + "\n";
        result.payload += "max_queue_depth=" +
                          std::to_string(snapshot.max_queue_depth) + "\n";
        result.payload += "concurrent_peak=" +
                          std::to_string(snapshot.concurrent_peak) + "\n";
        result.payload += "deadlock_events=" +
                          std::to_string(snapshot.deadlock_events) + "\n";
        result.payload += "auto_force_wakes=" +
                          std::to_string(snapshot.auto_force_wakes) + "\n";
        result.payload += "fiber_jobs=" +
                          std::to_string(snapshot.fiber_jobs) + "\n";
        result.payload += "fiber_failures=" +
                          std::to_string(snapshot.fiber_failures) + "\n";
        result.payload += "deadlock_verdict=";
        result.payload += VerdictName(deadlock_verdict());
        result.payload += "\nno_progress_ms=" +
                          std::to_string(no_progress_ms()) + "\n";
    }
    return result;
}

// ---------------------------------------------------------------------------
// 全局无进展检测
// ---------------------------------------------------------------------------

std::uint64_t DiagnosticAgent::ProgressFingerprint() {
    std::vector<std::pair<std::string, Scheduler*>> schedulers;
    std::vector<std::shared_ptr<WakeTarget>> targets;
    std::vector<MutexRegistration> mutexes;
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        schedulers = m_schedulers;
        targets = m_targets;
        mutexes = m_mutexes;
    }
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    const auto mix_text = [&mix](const std::string& text) {
        mix(static_cast<std::uint64_t>(text.size()));
        for (unsigned char character : text) {
            mix(static_cast<std::uint64_t>(character));
        }
    };
    for (const auto& entry : schedulers) {
        Scheduler* scheduler = entry.second;
        if (scheduler == nullptr) {
            continue;
        }
        mix_text(entry.first);
        mix(scheduler->processor_count());
        mix(scheduler->worker_count());
        mix(scheduler->runnable_count());
        mix(scheduler->global_runnable_count());
        const auto metrics = scheduler->metrics();
        mix(metrics.task_runs);
        mix(metrics.task_completions);
        // 只采样前 512 个 G：无进展检测是「周期性的低开销采样」，不是审计；
        // 超过 512 的规模仍能通过 runnable/completions 的变化发现进展。
        std::size_t sampled = 0;
        for (const auto& task : scheduler->DebugSnapshotTasks()) {
            if (sampled++ >= 512U) {
                break;
            }
            mix(task.id);
            mix(static_cast<std::uint64_t>(task.state));
            mix(task.queued ? 1U : 0U);
        }
        for (const auto& machine : scheduler->machines()) {
            mix(static_cast<std::uint64_t>(machine.state));
            mix(machine.processor);
            mix(machine.blocking_task);
        }
    }
    for (const auto& target : targets) {
        if (!target) {
            continue;
        }
        mix(static_cast<std::uint64_t>(target->support));
        try {
            mix_text(target->describe ? target->describe() : std::string());
        } catch (...) {
            mix_text("exception");
        }
    }
    for (const auto& registration : mutexes) {
        if (registration.mutex == nullptr) {
            continue;
        }
        try {
            const auto snapshot = registration.mutex->DebugSnapshotInternal();
            mix(snapshot.held ? 1U : 0U);
            mix(static_cast<std::uint64_t>(snapshot.waiters));
            mix(snapshot.owner_gid);
        } catch (...) {
            mix_text("mutex-exception");
        }
    }
    return hash == 0U ? 1U : hash;
}

namespace {

bool TaskIsPending(GState state) noexcept {
    return state == GState::kNew || state == GState::kRunnable ||
           state == GState::kWaiting;
}

}  // namespace

void DiagnosticAgent::MaybeDetectDeadlock() {
    if (!m_config.enable_deadlock_detection) {
        m_verdict.store(DeadlockVerdict::kUnknown, std::memory_order_release);
        return;
    }
    const std::uint64_t now = NowNs();
    const std::uint64_t interval_ns = static_cast<std::uint64_t>(
        std::max<std::int64_t>(1, m_config.detect_interval.count()) * 1000000LL);
    if (m_last_detect_ns != 0U && now - m_last_detect_ns < interval_ns) {
        return;
    }
    m_last_detect_ns = now;

    const std::uint64_t fingerprint = ProgressFingerprint();
    if (fingerprint != m_last_fingerprint.load(std::memory_order_acquire)) {
        m_last_fingerprint.store(fingerprint, std::memory_order_release);
        m_last_progress_ns.store(now, std::memory_order_release);
        m_no_progress_ms.store(0, std::memory_order_release);
        m_verdict.store(DeadlockVerdict::kProgressing,
                        std::memory_order_release);
        return;
    }

    const std::uint64_t no_progress_ns =
        ElapsedNs(m_last_progress_ns.load(std::memory_order_acquire));
    m_no_progress_ms.store(ToMs(no_progress_ns), std::memory_order_release);

    std::vector<std::pair<std::string, Scheduler*>> schedulers;
    std::vector<std::shared_ptr<WakeTarget>> targets;
    std::vector<MutexRegistration> mutexes;
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        schedulers = m_schedulers;
        targets = m_targets;
        mutexes = m_mutexes;
    }
    if (schedulers.empty()) {
        m_verdict.store(DeadlockVerdict::kUnknown, std::memory_order_release);
        return;
    }

    bool pending_waiters = false;
    bool any_running_machine = false;
    bool any_blocking_machine = false;
    std::size_t machine_count = 0;
    for (const auto& entry : schedulers) {
        Scheduler* scheduler = entry.second;
        if (scheduler == nullptr) {
            continue;
        }
        for (const auto& task : scheduler->DebugSnapshotTasks()) {
            if (TaskIsPending(task.state)) {
                pending_waiters = true;
                break;
            }
        }
        for (const auto& machine : scheduler->machines()) {
            ++machine_count;
            if (machine.state == MState::kRunning) {
                any_running_machine = true;
            } else if (machine.state == MState::kBlocking) {
                // 原生阻塞中的 M 可能正在推进 IO；不判定为全局死锁。
                any_blocking_machine = true;
            }
        }
    }
    for (const auto& target : targets) {
        if (!target) {
            continue;
        }
        try {
            if (DescribeHasWaiters(target->describe ? target->describe()
                                                    : std::string())) {
                pending_waiters = true;
                break;
            }
        } catch (...) {
        }
    }
    for (const auto& registration : mutexes) {
        if (registration.mutex == nullptr) {
            continue;
        }
        try {
            if (registration.mutex->DebugSnapshotInternal().waiters != 0U) {
                pending_waiters = true;
                break;
            }
        } catch (...) {
        }
    }

    if (!pending_waiters) {
        m_verdict.store(DeadlockVerdict::kIdle, std::memory_order_release);
        return;
    }
    if (any_running_machine || any_blocking_machine) {
        m_verdict.store(DeadlockVerdict::kProgressing,
                        std::memory_order_release);
        return;
    }
    (void)machine_count;

    const std::uint64_t threshold_ns = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, m_config.deadlock_threshold.count()) *
        1000000LL);
    if (no_progress_ns < threshold_ns) {
        m_verdict.store(DeadlockVerdict::kStalled, std::memory_order_release);
        return;
    }

    const DeadlockVerdict previous =
        m_verdict.exchange(DeadlockVerdict::kDeadlock,
                           std::memory_order_acq_rel);
    if (previous != DeadlockVerdict::kDeadlock) {
        m_deadlock_events.fetch_add(1, std::memory_order_relaxed);
    }
    // 事件环记录节流到 1 秒：detect_interval 可能远小于 1 秒。
    if (m_last_deadlock_event_ns == 0U ||
        now - m_last_deadlock_event_ns >= 1000000000ULL) {
        m_last_deadlock_event_ns = now;
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        GO2CPP_DEBUG_EVENT(kDiagnosticSuspected, 0,
                           ToMs(no_progress_ns));
#endif
    }
    if (m_config.auto_force_wake) {
        bool refused = false;
        std::string reason;
        std::string detail;
        (void)ForceWakeScope(kWakeScopeAll, &refused, &reason, &detail);
        m_auto_force_wakes.fetch_add(1, std::memory_order_relaxed);
    }
}

DiagnosticAgentStats DiagnosticAgent::stats() const {
    DiagnosticAgentStats snapshot;
    snapshot.accepted = m_accepted.load(std::memory_order_relaxed);
    snapshot.executed = m_executed.load(std::memory_order_relaxed);
    snapshot.failed = m_failed.load(std::memory_order_relaxed);
    snapshot.rejected = m_rejected.load(std::memory_order_relaxed);
    snapshot.dropped = m_dropped.load(std::memory_order_relaxed);
    snapshot.queue_depth = m_queue_depth.load(std::memory_order_relaxed);
    snapshot.max_queue_depth =
        m_max_queue_depth.load(std::memory_order_relaxed);
    snapshot.concurrent_peak =
        m_concurrent_peak.load(std::memory_order_relaxed);
    snapshot.deadlock_events =
        m_deadlock_events.load(std::memory_order_relaxed);
    snapshot.auto_force_wakes =
        m_auto_force_wakes.load(std::memory_order_relaxed);
    snapshot.fiber_jobs = m_fiber_jobs.load(std::memory_order_relaxed);
    snapshot.fiber_failures =
        m_fiber_failures.load(std::memory_order_relaxed);
    return snapshot;
}

DeadlockVerdict DiagnosticAgent::deadlock_verdict() const {
    return m_verdict.load(std::memory_order_acquire);
}

std::uint64_t DiagnosticAgent::no_progress_ms() const {
    return m_no_progress_ms.load(std::memory_order_acquire);
}

std::string DiagnosticAgent::HelpText() {
    return
        "Doctor commands (G2CD/1 frame; one command per request):\n"
        "  status [json]                     goroutines/M-P/runqueue/waits/"
        "modules(build-id)\n"
        "  dump [sections=a,b] [format=json]  reuse DebugServer component dumps\n"
        "  force-wake scope=scheduler-idle|parking-condition|channel|all\n"
        "  deadlock-report [json]             wait-for graph + verdict\n"
        "  mutex-unlock name=<registered>     controlled unlock (owner-gone only)\n"
        "  inject-fiber fn=<registered> [timeout_ms=N]\n"
        "  ping | stats [json] | help\n"
        "frame: magic=G2CD version=1 header=24 bytes, checksum=FNV-1a/32\n"
        "transport: unix:/tmp/go2cpp-<pid>-doctor.sock | memory | quic(planned)\n";
}

}  // namespace go2cpp::debug
