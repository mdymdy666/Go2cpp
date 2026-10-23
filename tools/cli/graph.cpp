/**
 * @file graph.cpp
 * @brief `go2cpp-cli graph` 的实现：采集数据 → 建图模型 → 渲染 Mermaid / DOT / JSON。
 *
 * ## 数据流
 *
 * ```text
 *   --pid ──► DebugServer VIEW resources / history / sched / park / gmp
 *          └► Doctor deadlock-report json=1（owner_gid / waiters 的权威来源）
 *   --replay ──► JSON（dump / view / trace 事件数组）或 # go2cpp-trace 文本
 *                     │
 *                     ▼
 *               资源模型（owner / waiters / 统计）
 *                     │
 *                     ▼
 *             Graph（节点 + 边 + 环 + notes）
 *                     │
 *        ┌────────────┼─────────────┐
 *        ▼            ▼             ▼
 *     Mermaid        DOT           JSON
 * ```
 *
 * ## 为什么先建统一模型再渲染
 *
 * 五种图的语义不同，但**数据来源高度重叠**：wait-for 与 resource 共用资源
 * 事件；fiber-state 与 sequence 共用事件流；gmp 的 G 节点也来自同一份
 * `VIEW gmp`。若每种图各自解析 JSON，字段一改就要改五处。这里把「采集」
 * （`Collected`）与「建模」（`Graph`）分开，渲染层只认节点/边。
 *
 * ## 降级策略（不是静默省略）
 *
 * - Doctor 不可用：owner 改由 `VIEW history` 的事件推导，notes 记录；
 * - DebugServer 不可用：只画 Doctor 给出的「资源 → owner」与 waiters 计数，
 *   不伪造等待者 gid；
 * - 两者都不可用：输出只含 note 节点的图并返回退出码 3，提示需要
 *   `-DGO2CPP_DEBUG_DUMP=ON` / `-DGO2CPP_DIAGNOSTIC=ON`。
 *
 * ## 已知窗口限制
 *
 * 事件环按线程有界。真实死锁场景中，若在死锁建立很久之后才采集，park/mutex
 * 事件可能已被高频 timer/PC 事件冲掉；此时 `VIEW history` 返回空，图只能展示
 * Doctor 的 owner/waiters 计数。`window.truncated` 会写入 notes，绝不把「窗口内
 * 没看到」当成「不存在」。
 */

#include "graph.hpp"

#include "args.hpp"
#include "cli.hpp"
#include "formats.hpp"
#include "help.hpp"
#include "transport.hpp"

#include "go2cpp/debug/diagnostic_protocol.hpp"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace go2cpp::cli {
namespace {

using nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace protocol = go2cpp::debug;

constexpr int kDefaultTimeoutMs = 5000;
constexpr int kMaxTimeoutMs = 600000;
constexpr std::size_t kDefaultMaxG = 16;
constexpr std::size_t kDefaultMaxResources = 48;
constexpr std::size_t kDefaultMaxEvents = 400;
constexpr std::size_t kMaxHistoryRequests = 24;
constexpr std::size_t kMaxCycles = 16;
constexpr std::size_t kMaxCycleEdges = 12;
constexpr std::uint64_t kMaxFileBytes = 64ULL * 1024ULL * 1024ULL;

// ---------------------------------------------------------------------------
// JSON 取值（类型不符时回退，不抛异常：dump 字段可能来自旧版本）
// ---------------------------------------------------------------------------

bool Has(const json& object, const char* key) {
    return object.is_object() && object.find(key) != object.end();
}

const json* Get(const json& object, const char* key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &(*it);
}

std::uint64_t U64(const json& object, const char* key, std::uint64_t fallback = 0) {
    const json* value = Get(object, key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->is_number_unsigned()) {
        return value->get<std::uint64_t>();
    }
    if (value->is_number_integer()) {
        const auto signed_value = value->get<std::int64_t>();
        return signed_value < 0 ? fallback : static_cast<std::uint64_t>(signed_value);
    }
    if (value->is_string()) {
        try {
            return static_cast<std::uint64_t>(std::stoull(value->get<std::string>()));
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string Str(const json& object, const char* key, const std::string& fallback = {}) {
    const json* value = Get(object, key);
    if (value == nullptr || !value->is_string()) {
        return fallback;
    }
    return value->get<std::string>();
}

bool Bool(const json& object, const char* key, bool fallback = false) {
    const json* value = Get(object, key);
    if (value == nullptr || !value->is_boolean()) {
        return fallback;
    }
    return value->get<bool>();
}

const json* GetPath(const json& root, std::initializer_list<const char*> keys) {
    const json* node = &root;
    for (const char* key : keys) {
        node = Get(*node, key);
        if (node == nullptr) {
            return nullptr;
        }
    }
    return node;
}

// ---------------------------------------------------------------------------
// 图模型
// ---------------------------------------------------------------------------

struct Node {
    std::string id;     ///< 语义词 id（JSON 中可见；Mermaid/DOT 渲染时映射为 n0..nN）。
    std::string label;  ///< 多行标签（渲染时转成 <br/> 或 \n）。
    std::string kind;   ///< g / resource / machine / processor / queue / state / note。
    std::string style;  ///< 颜色样式名（g / resource / state-running / ...）。
    bool deadlock{false};
};

struct Edge {
    std::string from;
    std::string to;
    std::string label;
    std::string kind;  ///< wait / held / schedule / runqueue / message / ...
    bool deadlock{false};
};

struct Participant {
    std::string id;     ///< Mermaid participant id（R1 / G4761 / S）。
    std::string label;  ///< 显示名。
};

struct Message {
    std::string from;   ///< Participant id。
    std::string to;
    std::string label;
    std::uint64_t ts_ns{0};
    std::size_t count{1}; ///< 连续重复消息折叠后的次数（渲染为 ×N）。
    bool dashed{false}; ///< true → -->>（响应/广播）。
};

struct Graph {
    std::string type;
    std::string title;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::vector<std::string> notes;
    std::vector<std::vector<std::string>> cycles;
    std::vector<Participant> participants;
    std::vector<Message> messages;
    json meta = json::object();
    bool ok{true};

    Node* Find(const std::string& id) {
        for (auto& node : nodes) {
            if (node.id == id) {
                return &node;
            }
        }
        return nullptr;
    }

    void AddNode(const std::string& id, const std::string& label, const std::string& kind,
                 const std::string& style = {}) {
        if (Find(id) != nullptr) {
            return;
        }
        nodes.push_back(Node{id, label, kind, style.empty() ? kind : style, false});
    }

    void AddEdge(const std::string& from, const std::string& to, const std::string& label,
                 const std::string& kind) {
        for (const auto& edge : edges) {
            if (edge.from == from && edge.to == to && edge.kind == kind) {
                return;
            }
        }
        edges.push_back(Edge{from, to, label, kind, false});
    }

    void Note(const std::string& text) { notes.push_back(text); }
};

// ---------------------------------------------------------------------------
// 文本转义
// ---------------------------------------------------------------------------

std::string TrimCopy(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
                           text[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> SplitTokens(const std::string& text) {
    std::vector<std::string> tokens;
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) {
            ++index;
        }
        const std::size_t begin = index;
        while (index < text.size() && text[index] != ' ' && text[index] != '\t') {
            ++index;
        }
        if (index > begin) {
            tokens.push_back(text.substr(begin, index - begin));
        }
    }
    return tokens;
}

/// Mermaid 标签：引号会破坏 `id["..."]`，换行转 `<br/>`；`#` 在部分版本会被
/// 当作 HTML 实体前缀，统一换成 `-`（JSON/DOT 中保留原始名字）。
std::string MermaidLabel(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char ch : text) {
        switch (ch) {
            case '"': out += "&quot;"; break;
            case '\n': out += "<br/>"; break;
            case '\r': break;
            case '|': out += "／"; break;
            case '#': out += '-'; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

/// Mermaid 的 `|label|` 中不能出现裸 `|`/换行；引号同样替换。
std::string MermaidEdgeLabel(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text) {
        if (ch == '"') {
            out += "'";
        } else if (ch == '|') {
            out += "／";
        } else if (ch == '\n' || ch == '\r') {
            out += ' ';
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::string DotLabel(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char ch : text) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

/// Mermaid sequence 的消息/参与者文本（`#` 在部分版本会被当作实体前缀）。
std::string MermaidMessage(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text) {
        if (ch == '#') {
            out += '-';
        } else if (ch == '"') {
            out += "'";
        } else if (ch == '\n' || ch == '\r') {
            out += ' ';
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::string Join(const std::vector<std::string>& parts, const char* separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out += separator;
        }
        out += parts[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// 参数
// ---------------------------------------------------------------------------

struct Options {
    std::string type;
    std::string format{"mermaid"};
    std::string out{"-"};
    int pid{0};
    bool has_pid{false};
    std::string replay_path;
    std::string debug_socket;
    std::string doctor_socket;
    int timeout_ms{kDefaultTimeoutMs};
    std::size_t max_g{kDefaultMaxG};
    std::size_t max_resources{kDefaultMaxResources};
    std::size_t max_events{kDefaultMaxEvents};
};

// ---------------------------------------------------------------------------
// Doctor（G2CD/1）客户端：只发一条命令，收完整响应
// ---------------------------------------------------------------------------

int RemainingMs(Clock::time_point deadline) {
    const auto now = Clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return remaining <= 0 ? 1 : static_cast<int>(remaining);
}

int PollFd(int fd, short events, int timeout_ms) {
    pollfd entry{};
    entry.fd = fd;
    entry.events = events;
    for (;;) {
        const int result = ::poll(&entry, 1, timeout_ms);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return result;
    }
}

bool SendAll(int fd, const std::string& bytes, Clock::time_point deadline) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t sent =
            ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const int remaining = RemainingMs(deadline);
            if (remaining == 0 || PollFd(fd, POLLOUT, remaining) <= 0) {
                return false;
            }
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

/// 连接 Doctor；失败时区分「未启用（可降级）」与「协议/IO 错误」。
int ConnectDoctor(const std::string& path, int timeout_ms, std::string* error,
                  bool* unavailable) {
    *unavailable = false;
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        *error = "Doctor socket 路径过长（" + std::to_string(path.size()) + " 字节，上限 " +
                 std::to_string(sizeof(sockaddr_un::sun_path) - 1) + "）";
        return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = std::string("socket: ") + std::strerror(errno);
        return -1;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        if (errno != EINPROGRESS) {
            const int saved = errno;
            ::close(fd);
            *unavailable = saved == ENOENT || saved == ECONNREFUSED || saved == EACCES ||
                           saved == EPERM;
            *error = saved == ENOENT
                         ? "Doctor 未启用：socket '" + path +
                               "' 不存在（目标进程需要 -DGO2CPP_DIAGNOSTIC=ON 并调用 "
                               "DiagnosticAgent::Start()）"
                         : std::string("连接 Doctor 失败：") + std::strerror(saved);
            return -1;
        }
        if (PollFd(fd, POLLOUT, timeout_ms) <= 0) {
            ::close(fd);
            *unavailable = true;
            *error = "连接 Doctor 超时（" + std::to_string(timeout_ms) + "ms）：'" + path + "'";
            return -1;
        }
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0 ||
            socket_error != 0) {
            const int saved = socket_error == 0 ? errno : socket_error;
            ::close(fd);
            *unavailable = saved == ENOENT || saved == ECONNREFUSED || saved == ENOTCONN;
            *error = std::string("连接 Doctor 失败：") + std::strerror(saved);
            return -1;
        }
    }
    return fd;
}

std::optional<json> DoctorCall(const std::string& path, int timeout_ms,
                               const std::string& command, std::string* error,
                               bool* unavailable) {
    *unavailable = false;
    std::string connect_error;
    const int fd = ConnectDoctor(path, timeout_ms, &connect_error, unavailable);
    if (fd < 0) {
        *error = connect_error;
        return std::nullopt;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    protocol::DiagnosticFrame request;
    request.kind = protocol::DiagnosticFrameKind::kRequest;
    request.stream_id = 1;
    request.payload = command;
    const std::string bytes = protocol::EncodeDiagnosticFrame(request);
    if (bytes.empty() || !SendAll(fd, bytes, deadline)) {
        ::close(fd);
        *error = "发送 Doctor 命令失败";
        return std::nullopt;
    }

    protocol::DiagnosticFrameParser parser;
    std::string assembled;
    bool complete = false;
    bool was_error = false;
    while (!complete && RemainingMs(deadline) > 0) {
        const int ready = PollFd(fd, POLLIN, RemainingMs(deadline));
        if (ready <= 0) {
            break;
        }
        char buffer[65536];
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        if (!parser.Feed(buffer, static_cast<std::size_t>(received))) {
            ::close(fd);
            *error = "Doctor 帧解析失败：" + parser.error();
            return std::nullopt;
        }
        protocol::DiagnosticFrame frame;
        while (parser.Next(frame)) {
            if (frame.stream_id != 1) {
                continue;
            }
            assembled += frame.payload;
            was_error = was_error || frame.kind == protocol::DiagnosticFrameKind::kError;
            if (!frame.chunked() || frame.last_chunk()) {
                complete = true;
                break;
            }
        }
        if (parser.failed()) {
            ::close(fd);
            *error = "Doctor 帧解析失败：" + parser.error();
            return std::nullopt;
        }
    }
    ::close(fd);
    if (!complete) {
        *error = "等待 Doctor 响应超时（" + std::to_string(timeout_ms) + "ms）";
        return std::nullopt;
    }
    if (was_error) {
        *error = "Doctor 返回 error 帧：" + assembled;
        return std::nullopt;
    }
    json parsed = json::parse(assembled, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        *error = "Doctor 返回的不是 JSON（需要用 json=1）：" +
                 assembled.substr(0, 120);
        return std::nullopt;
    }
    return parsed;
}

std::string ResolveDoctorPath(const Options& options, int pid) {
    if (!options.doctor_socket.empty()) {
        return options.doctor_socket;
    }
    const char* environment = std::getenv("GO2CPP_DOCTOR_SOCKET");
    if (environment != nullptr && *environment != '\0') {
        return environment;
    }
    return "/tmp/go2cpp-" + std::to_string(pid) + "-doctor.sock";
}

// ---------------------------------------------------------------------------
// 事件模型
// ---------------------------------------------------------------------------

struct Event {
    std::uint64_t seq{0};
    std::uint64_t ts_ns{0};
    std::uint64_t gid{0};
    std::uint64_t resource_id{0};
    std::uint64_t target_gid{0};
    std::uint64_t waiters{0};
    std::string code;
    std::string result;
    std::string detail;
    std::string primitive_reason;

    /// 事件唯一性键：不同视图的同一事件应被折叠。
    std::string Key() const {
        return std::to_string(ts_ns) + "|" + std::to_string(seq) + "|" + code + "|" +
               std::to_string(gid) + "|" + std::to_string(resource_id);
    }
};

std::optional<Event> ParseEvent(const json& item) {
    if (!item.is_object()) {
        return std::nullopt;
    }
    Event event;
    event.seq = U64(item, "seq");
    event.ts_ns = U64(item, "ts_ns");
    event.gid = U64(item, "gid");
    event.resource_id = U64(item, "resource_id");
    event.target_gid = U64(item, "target_gid");
    event.waiters = U64(item, "waiters", U64(item, "waiter_depth"));
    event.code = Str(item, "code");
    if (event.code.empty()) {
        event.code = Str(item, "type");
    }
    event.result = Str(item, "result_name");
    event.detail = Str(item, "detail");
    event.primitive_reason = Str(item, "primitive_reason");
    if (event.code.empty()) {
        return std::nullopt;
    }
    return event;
}

std::vector<Event> EventsFromArray(const json& array) {
    std::vector<Event> events;
    if (!array.is_array()) {
        return events;
    }
    for (const auto& item : array) {
        const auto event = ParseEvent(item);
        if (event.has_value()) {
            events.push_back(*event);
        }
    }
    return events;
}

std::vector<Event> SortedUnique(std::vector<Event> events, std::size_t limit) {
    std::sort(events.begin(), events.end(), [](const Event& lhs, const Event& rhs) {
        if (lhs.ts_ns != rhs.ts_ns) {
            return lhs.ts_ns < rhs.ts_ns;
        }
        if (lhs.seq != rhs.seq) {
            return lhs.seq < rhs.seq;
        }
        return lhs.code < rhs.code;
    });
    std::set<std::string> seen;
    std::vector<Event> unique;
    unique.reserve(std::min(events.size(), limit));
    for (auto& event : events) {
        if (seen.insert(event.Key()).second) {
            unique.push_back(std::move(event));
            if (unique.size() >= limit) {
                break;
            }
        }
    }
    return unique;
}

// ---------------------------------------------------------------------------
// 采集
// ---------------------------------------------------------------------------

struct ViewFetch {
    bool ok{false};
    bool unavailable{false};
    json value;
    std::string error;
};

ViewFetch FetchView(DebugClient& client, const std::string& view,
                    std::optional<std::uint64_t> argument) {
    ViewFetch result;
    std::string command = "VIEW " + view;
    if (argument.has_value()) {
        command += " " + std::to_string(*argument);
    }
    try {
        const DebugReply reply = client.Call(command);
        json parsed = json::parse(reply.payload, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            result.error = "视图 " + view + " 返回的不是 JSON";
            return result;
        }
        if (parsed.contains("ok") && parsed["ok"].is_boolean() && !parsed["ok"].get<bool>()) {
            result.error = "视图 " + view + " 被目标进程拒绝：" +
                           parsed.value("error", std::string("unknown"));
            return result;
        }
        result.ok = true;
        result.value = std::move(parsed);
    } catch (const DebugError& error) {
        result.error = std::string("视图 ") + view + "：" + error.what();
    } catch (const CliError& error) {
        result.unavailable = error.code() == ExitCode::kUnavailable;
        result.error = std::string("视图 ") + view + "：" + error.what();
    }
    return result;
}

struct Collected {
    bool debug_ok{false};
    bool doctor_ok{false};
    bool debug_down{false};
    bool doctor_down{false};
    std::string debug_error;
    std::string doctor_error;
    std::optional<json> resources;
    std::optional<json> primitives;
    std::optional<json> sched;
    std::optional<json> gmp;
    std::optional<json> park;
    std::optional<json> deadlock;
    std::optional<json> doctor_status;
    std::map<std::uint64_t, json> histories;
    std::vector<Event> events;  ///< 文件模式 / trace 模式的事件
    std::string source_desc;
    std::string file_format;
    std::size_t meta_trace_lines{0};
    bool file_mode{false};
    bool window_truncated{false};
};

bool ResourceKindVisible(const json& item) {
    const std::string kind = Str(item, "kind");
    if (kind == "kMutex" || kind == "kRwMutex" || kind == "kRecursiveMutex" ||
        kind == "kTimedMutex" || kind == "kSpinMutex" || kind == "kConditionVariable" ||
        kind == "kWaitGroup" || kind == "kChannel") {
        return true;
    }
    if (kind == "kParkingCondition") {
        return U64(item, "event_count") > 0 || U64(item, "waits") > 0 ||
               U64(item, "wakes") > 0;
    }
    return false;
}

std::uint64_t ResourceActivity(const json& item) {
    return U64(item, "waits") + U64(item, "holds") + U64(item, "wakes") +
           U64(item, "event_count");
}

std::vector<std::uint64_t> SelectResourceIds(const json& resources_view, std::size_t max) {
    const json* list = GetPath(resources_view, {"resources", "resources"});
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranked;
    if (list == nullptr || !list->is_array()) {
        return {};
    }
    for (const auto& item : *list) {
        if (!item.is_object() || !ResourceKindVisible(item)) {
            continue;
        }
        const std::uint64_t id = U64(item, "id");
        if (id == 0) {
            continue;
        }
        ranked.emplace_back(ResourceActivity(item), id);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first) {
            return lhs.first > rhs.first;
        }
        return lhs.second < rhs.second;
    });
    std::vector<std::uint64_t> ids;
    for (const auto& entry : ranked) {
        if (ids.size() >= max) {
            break;
        }
        ids.push_back(entry.second);
    }
    return ids;
}

bool ViewWindowTruncated(const json& view) {
    const json* window = Get(view, "window");
    return window != nullptr && Bool(*window, "truncated");
}

bool HistoryHasEvents(const json& history_view) {
    const json* events = GetPath(history_view, {"history", "events"});
    return events != nullptr && events->is_array() && !events->empty();
}

Collected CollectLive(const Options& options) {
    Collected collected;
    collected.source_desc = "pid " + std::to_string(options.pid);

    const bool need_deadlock =
        options.type == "wait-for" || options.type == "resource";
    const bool need_debug = true;

    DebugTarget target;
    target.pid = options.pid;
    target.timeout_ms = options.timeout_ms;
    if (!options.debug_socket.empty()) {
        target.socket_path = options.debug_socket;
    } else {
        try {
            target = ResolveDebugTarget(options.pid, options.timeout_ms);
        } catch (const CliError& error) {
            collected.debug_down = true;
            collected.debug_error = error.what();
        }
    }

    std::unique_ptr<DebugClient> client;
    if (!collected.debug_down && need_debug) {
        client = std::make_unique<DebugClient>(target);
    }

    const auto fetch = [&](const std::string& view,
                           std::optional<std::uint64_t> argument = std::nullopt)
        -> std::optional<json> {
        if (!client || collected.debug_down) {
            return std::nullopt;
        }
        ViewFetch result = FetchView(*client, view, argument);
        if (!result.ok) {
            if (result.unavailable) {
                collected.debug_down = true;
            }
            if (collected.debug_error.empty()) {
                collected.debug_error = result.error;
            }
            return std::nullopt;
        }
        collected.debug_ok = true;
        return result.value;
    };

    if (options.type == "gmp") {
        if (const auto gmp = fetch("gmp")) {
            collected.gmp = *gmp;
        }
    } else if (options.type == "fiber-state") {
        if (const auto sched = fetch("sched")) {
            collected.sched = *sched;
        }
        if (const auto park = fetch("park")) {
            collected.park = *park;
        }
        if (const auto gmp = fetch("gmp")) {
            collected.gmp = *gmp;
        }
    } else if (options.type == "wait-for" || options.type == "resource") {
        if (const auto resources = fetch("resources")) {
            collected.resources = *resources;
            collected.window_truncated =
                collected.window_truncated || ViewWindowTruncated(*resources);
        }
        if (collected.resources.has_value()) {
            const std::vector<std::uint64_t> ids = SelectResourceIds(
                *collected.resources, std::min(options.max_resources, kMaxHistoryRequests));
            for (std::uint64_t id : ids) {
                if (collected.debug_down) {
                    break;
                }
                if (const auto history = fetch("history", id)) {
                    collected.histories[id] = *history;
                }
            }
        }
        // 资源历史为空（窗口被冲刷）时，primitives 是退路：事件里有 resource_id。
        bool any_history_events = false;
        for (const auto& entry : collected.histories) {
            if (HistoryHasEvents(entry.second)) {
                any_history_events = true;
                break;
            }
        }
        if (!any_history_events) {
            if (const auto primitives = fetch("primitives")) {
                collected.primitives = *primitives;
            }
        }
    } else if (options.type == "sequence") {
        if (const auto resources = fetch("resources")) {
            collected.resources = *resources;
        }
        if (collected.resources.has_value()) {
            const std::vector<std::uint64_t> ids = SelectResourceIds(
                *collected.resources, std::min(options.max_resources, kMaxHistoryRequests));
            for (std::uint64_t id : ids) {
                if (collected.debug_down) {
                    break;
                }
                if (const auto history = fetch("history", id)) {
                    collected.histories[id] = *history;
                }
            }
        }
        bool any_history_events = false;
        for (const auto& entry : collected.histories) {
            if (HistoryHasEvents(entry.second)) {
                any_history_events = true;
                break;
            }
        }
        if (!any_history_events) {
            if (const auto primitives = fetch("primitives")) {
                collected.primitives = *primitives;
            }
        }
    }

    if (need_deadlock) {
        const std::string doctor_path = ResolveDoctorPath(options, options.pid);
        std::string error;
        bool unavailable = false;
        if (const auto report =
                DoctorCall(doctor_path, options.timeout_ms, "deadlock-report json=1",
                           &error, &unavailable)) {
            collected.deadlock = *report;
            collected.doctor_ok = true;
        } else {
            collected.doctor_error = error;
            collected.doctor_down = unavailable;
        }
    }
    // gmp 的兜底：Doctor status 也带 machines/processors/runqueue。
    if (options.type == "gmp" && !collected.gmp.has_value()) {
        const std::string doctor_path = ResolveDoctorPath(options, options.pid);
        std::string error;
        bool unavailable = false;
        if (const auto status = DoctorCall(doctor_path, options.timeout_ms, "status json=1",
                                           &error, &unavailable)) {
            collected.doctor_status = *status;
            collected.doctor_ok = true;
        } else {
            collected.doctor_error = error;
            collected.doctor_down = unavailable;
        }
    }

    if (!collected.debug_ok && !collected.doctor_ok) {
        collected.debug_error = collected.debug_error.empty() ? "DebugServer 不可用"
                                                              : collected.debug_error;
    }
    return collected;
}

// ---------------------------------------------------------------------------
// --replay 文件解析（JSON / trace 文本）
// ---------------------------------------------------------------------------

void ExtractJsonEvents(const json& root, std::vector<Event>* events, int depth) {
    if (depth > 6) {
        return;
    }
    if (root.is_array()) {
        for (const auto& item : root) {
            if (const auto event = ParseEvent(item)) {
                events->push_back(*event);
            }
        }
        return;
    }
    if (!root.is_object()) {
        return;
    }
    if (const json* direct = Get(root, "events"); direct != nullptr && direct->is_array()) {
        for (const auto& item : *direct) {
            if (const auto event = ParseEvent(item)) {
                events->push_back(*event);
            }
        }
    }
    static const char* kPaths[][2] = {
        {"sched", "flow"},       {"timeline", "events"}, {"primitives", "flow"},
        {"history", "events"},   {"park", "items"},
    };
    for (const auto& path : kPaths) {
        const json* node = GetPath(root, {path[0], path[1]});
        if (node == nullptr || !node->is_array()) {
            continue;
        }
        if (std::string(path[0]) == "park") {
            continue;  // park items 不是事件（没有 code 字段），单独处理。
        }
        for (const auto& item : *node) {
            if (const auto event = ParseEvent(item)) {
                events->push_back(*event);
            }
        }
    }
    if (const json* components = Get(root, "components"); components != nullptr &&
                                                          components->is_object()) {
        for (const auto& entry : components->items()) {
            ExtractJsonEvents(entry.value(), events, depth + 1);
        }
    }
}

void ExtractJsonStructures(const json& root, Collected* collected) {
    // gmp 视图
    if (const json* gmp = Get(root, "gmp"); gmp != nullptr && gmp->is_object()) {
        collected->gmp = *gmp;
    } else if (Get(root, "summary") != nullptr && Get(root, "processors") != nullptr) {
        json wrapper = json::object();
        wrapper["gmp"] = root;
        collected->gmp = std::move(wrapper);
    }
    // resources 视图
    if (const json* resources = GetPath(root, {"resources", "resources"});
        resources != nullptr && resources->is_array()) {
        collected->resources = root;
    } else if (const json* resources = Get(root, "resources");
               resources != nullptr && resources->is_array()) {
        json wrapper = json::object();
        wrapper["resources"] = *resources;
        collected->resources = std::move(wrapper);
    }
    // 单资源历史：BuildSequenceGraph 统一按「view 根」读取 history.events。
    if (const json* history = Get(root, "history"); history != nullptr &&
                                                     history->is_object()) {
        const std::uint64_t id = U64(*history, "resource_id", U64(*history, "id"));
        collected->histories[id] = root;
    } else if (Get(root, "events") != nullptr && Get(root, "resource_id") != nullptr) {
        // 也接受只含 {resource_id, events} 的简化历史文件。
        const std::uint64_t id = U64(root, "resource_id");
        json wrapper = json::object();
        wrapper["history"] = root;
        collected->histories[id] = std::move(wrapper);
    }
    // Doctor deadlock-report JSON
    if (const json* edges = Get(root, "edges"); edges != nullptr && edges->is_array() &&
                                                  Has(root, "verdict")) {
        collected->deadlock = root;
    }
}

std::vector<Event> ParseTrace(const std::string& content, std::size_t* lines_out,
                              std::size_t* records_out) {
    std::vector<Event> events;
    std::size_t lines = 0;
    std::size_t records = 0;
    std::size_t begin = 0;
    while (begin <= content.size()) {
        const auto newline = content.find('\n', begin);
        const std::string line = TrimCopy(content.substr(
            begin, newline == std::string::npos ? std::string::npos : newline - begin));
        ++lines;
        if (newline == std::string::npos) {
            break;
        }
        begin = newline + 1;
        if (line.empty() || line[0] != '#') {
            continue;
        }
        const std::vector<std::string> tokens = SplitTokens(line);
        if (tokens.size() < 3 || (tokens[1] != "INPUT" && tokens[1] != "OUTPUT")) {
            continue;
        }
        const std::string& kind = tokens[2];
        std::string code;
        if (kind == "SPAWN") {
            code = "kGCreated";
        } else if (kind == "PARK") {
            code = "kParkEnter";
        } else if (kind == "WAKE") {
            code = "kWakeEnqueued";
        } else if (kind == "ENQ") {
            code = "kEnqueue";
        } else if (kind == "EXIT") {
            code = "kGCompleted";
        } else {
            continue;
        }
        Event event;
        event.ts_ns = records + 1;
        event.seq = records + 1;
        ++records;
        std::string detail = kind;
        for (std::size_t i = 3; i < tokens.size(); ++i) {
            const auto equal = tokens[i].find('=');
            if (equal == std::string::npos) {
                detail += " " + tokens[i];
                continue;
            }
            const std::string key = tokens[i].substr(0, equal);
            const std::string value = tokens[i].substr(equal + 1);
            if (key == "gid" && value != "-") {
                try {
                    event.gid = static_cast<std::uint64_t>(std::stoull(value));
                } catch (...) {
                    event.gid = 0;
                }
            } else if (key == "reason") {
                event.primitive_reason = value;
                detail += " reason=" + value;
            } else if (key == "state") {
                detail += " state=" + value;
            }
        }
        event.code = code;
        event.detail = detail;
        event.result = "ok";
        events.push_back(std::move(event));
    }
    if (lines_out != nullptr) {
        *lines_out = lines;
    }
    if (records_out != nullptr) {
        *records_out = records;
    }
    return events;
}

Collected CollectFile(const Options& options) {
    Collected collected;
    collected.file_mode = true;
    collected.source_desc = "replay " + options.replay_path;
    const FileProbe probe = ProbeFile(options.replay_path, kMaxFileBytes);
    collected.file_format = FileFormatName(probe.format);
    if (probe.format == FileFormat::kTraceText) {
        const std::string content =
            ReadTextFile(options.replay_path, kMaxFileBytes, nullptr);
        std::size_t lines = 0;
        std::size_t records = 0;
        collected.events = ParseTrace(content, &lines, &records);
        if (records == 0) {
            throw CliError(ExitCode::kFormat,
                           "trace 文件不含可识别的事件（需要 OUTPUT SPAWN/PARK/WAKE/ENQ/EXIT）：'" +
                               options.replay_path + "'");
        }
        collected.meta_trace_lines = lines;
        return collected;
    }
    if (probe.format != FileFormat::kJson) {
        throw CliError(ExitCode::kFormat,
                       "graph --replay 只支持 JSON dump 与 # go2cpp-trace 文本；'" +
                           options.replay_path + "' 是 " + FileFormatName(probe.format) +
                           "。G2CRPLY 二进制记录请先用 replay 工具导出 JSON 事件。");
    }
    const std::string content = ReadTextFile(options.replay_path, kMaxFileBytes, nullptr);
    json root = json::parse(content, nullptr, false);
    if (root.is_discarded()) {
        throw CliError(ExitCode::kFormat,
                       "JSON 解析失败：'" + options.replay_path + "'（语法错误或被截断）");
    }
    ExtractJsonStructures(root, &collected);
    ExtractJsonEvents(root, &collected.events, 0);
    return collected;
}

// part2 继续（同一匿名命名空间）：资源模型、建图、渲染、RunGraph。
// ---------------------------------------------------------------------------
// 资源模型：从 VIEW resources + VIEW history 还原 owner / waiters
// ---------------------------------------------------------------------------

struct ResourceModel {
    std::uint64_t id{0};
    std::string kind;
    std::string debug_name;
    std::string doctor_name;
    std::string display;
    bool alive{false};
    bool has_stats{false};
    bool doctor_only{false};
    bool in_deadlock{false};
    std::uint64_t events{0};
    std::uint64_t holds{0};
    std::uint64_t waits{0};
    std::uint64_t wakes{0};
    std::uint64_t max_waiters{0};
    std::uint64_t owner_gid{0};
    bool owner_known{false};
    std::vector<std::uint64_t> waiters;
};

void RemoveWaiter(std::vector<std::uint64_t>* waiters, std::uint64_t gid) {
    if (gid == 0) {
        return;
    }
    for (auto it = waiters->begin(); it != waiters->end(); ++it) {
        if (*it == gid) {
            waiters->erase(it);
            return;
        }
    }
}

void AddWaiter(std::vector<std::uint64_t>* waiters, std::uint64_t gid) {
    if (gid == 0) {
        return;
    }
    for (std::uint64_t existing : *waiters) {
        if (existing == gid) {
            return;
        }
    }
    waiters->push_back(gid);
}

bool IsLockAcquireCode(const std::string& code) {
    return code == "kMutexLock" || code == "kRwLockRead" || code == "kRwLockWrite" ||
           code == "kRecursiveLock" || code == "kMutexFastLock";
}

bool IsLockReleaseCode(const std::string& code) {
    return code == "kMutexUnlock" || code == "kRwUnlockRead" ||
           code == "kRwUnlockWrite" || code == "kRecursiveUnlock";
}

bool IsChannelBlockCode(const std::string& code) {
    return code == "kChannelSend" || code == "kChannelRecv";
}

bool IsChannelOkCode(const std::string& code) {
    return code == "kChannelSendOk" || code == "kChannelRecvOk";
}

bool IsPcWaitCode(const std::string& code) { return code == "kPCWait"; }

bool IsPcNotifyCode(const std::string& code) {
    return code == "kPCNotify" || code == "kPCNotifyAll" || code == "kCvNotify" ||
           code == "kCvNotifyAll";
}

bool IsWakeEventCode(const std::string& code) {
    return code == "kWakeEnqueued" || code == "kWakePending" || code == "kWakeBatch" ||
           code == "kChannelWake";
}

void ApplyHistoryEvents(ResourceModel* model, const std::vector<Event>& events) {
    for (const Event& event : events) {
        if (event.waiters > model->max_waiters) {
            model->max_waiters = event.waiters;
        }
        if (IsLockAcquireCode(event.code) || IsChannelBlockCode(event.code)) {
            if (event.result == "ok") {
                if (IsLockAcquireCode(event.code)) {
                    model->owner_gid = event.gid;
                    model->owner_known = event.gid != 0;
                }
                RemoveWaiter(&model->waiters, event.gid);
            } else if (event.result == "blocked") {
                AddWaiter(&model->waiters, event.gid);
            } else {
                RemoveWaiter(&model->waiters, event.gid);
            }
            continue;
        }
        if (IsLockReleaseCode(event.code)) {
            model->owner_gid = 0;
            model->owner_known = false;
            continue;
        }
        if (IsChannelOkCode(event.code)) {
            RemoveWaiter(&model->waiters, event.gid);
            continue;
        }
        if (IsPcWaitCode(event.code)) {
            AddWaiter(&model->waiters, event.gid);
            continue;
        }
        if (IsPcNotifyCode(event.code)) {
            if (event.code == "kPCNotifyAll" || event.code == "kCvNotifyAll") {
                model->waiters.clear();
            } else {
                if (!model->waiters.empty()) {
                    model->waiters.erase(model->waiters.begin());
                }
            }
            continue;
        }
        if (IsWakeEventCode(event.code)) {
            if (event.target_gid != 0) {
                RemoveWaiter(&model->waiters, event.target_gid);
            } else if (!model->waiters.empty() &&
                       (event.code == "kChannelWake" || event.code == "kWakeEnqueued")) {
                // 事件只说明「唤醒了一个等待者」但没有带 target_gid：
                // 无法确定是哪一个，保留队列，交由后续 acquire 事件收敛。
            }
        }
    }
}

std::vector<Event> HistoryEvents(const json& history_view) {
    const json* events = GetPath(history_view, {"history", "events"});
    if (events == nullptr) {
        return {};
    }
    return EventsFromArray(*events);
}

void ApplyHistoryView(ResourceModel* model, const json& history_view) {
    const json* history = Get(history_view, "history");
    if (history == nullptr) {
        return;
    }
    if (const json* resource = Get(*history, "resource"); resource != nullptr) {
        if (model->debug_name.empty()) {
            model->debug_name = Str(*resource, "name");
        }
        if (model->kind.empty()) {
            model->kind = Str(*resource, "kind");
        }
        model->alive = Bool(*resource, "alive", model->alive);
    }
    if (const json* summary = Get(*history, "summary"); summary != nullptr) {
        model->has_stats = true;
        model->holds = U64(*summary, "holds", model->holds);
        model->waits = U64(*summary, "waits", model->waits);
        model->wakes = U64(*summary, "wakes", model->wakes);
        model->events = U64(*summary, "events", model->events);
        model->max_waiters =
            std::max(model->max_waiters, U64(*summary, "max_waiters"));
    }
    ApplyHistoryEvents(model, HistoryEvents(history_view));
}

void ApplyResourcesView(ResourceModel* model, const json& item) {
    model->id = U64(item, "id", model->id);
    model->kind = Str(item, "kind", model->kind);
    model->debug_name = Str(item, "name", model->debug_name);
    model->alive = Bool(item, "alive", model->alive);
    model->events = U64(item, "event_count", model->events);
    model->holds = U64(item, "holds", model->holds);
    model->waits = U64(item, "waits", model->waits);
    model->wakes = U64(item, "wakes", model->wakes);
    model->max_waiters = std::max(model->max_waiters, U64(item, "max_waiters"));
    if (model->holds != 0 || model->waits != 0 || model->wakes != 0) {
        model->has_stats = true;
    }
}

std::string ResourceLabel(const ResourceModel& model) {
    std::string label = model.display.empty() ? model.debug_name : model.display;
    if (label.empty()) {
        label = "resource";
    }
    if (!model.doctor_name.empty() && model.debug_name != model.doctor_name &&
        !model.debug_name.empty()) {
        label += "\n" + model.debug_name;
    }
    if (!model.kind.empty()) {
        label += "\n" + model.kind;
    }
    if (model.owner_known && model.owner_gid != 0) {
        label += "\nowner=G" + std::to_string(model.owner_gid);
    } else if (!model.owner_known && model.kind.find("Mutex") != std::string::npos) {
        label += "\nowner=unknown";
    }
    if (!model.waiters.empty()) {
        label += "\nwaiters=" + std::to_string(model.waiters.size());
    } else if (model.waits != 0 && model.doctor_only) {
        label += "\nwaiters=" + std::to_string(model.waits) + " (gid 未提供)";
    }
    if (model.doctor_only && model.waits != 0 && model.owner_known) {
        label += "\nwaits=" + std::to_string(model.waits);
    }
    if (model.has_stats) {
        label += "\nholds=" + std::to_string(model.holds) +
                 " waits=" + std::to_string(model.waits) +
                 " wakes=" + std::to_string(model.wakes);
    }
    return label;
}

std::vector<ResourceModel> BuildResourceModels(const Collected& collected,
                                               const Options& options,
                                               std::vector<std::string>* notes) {
    std::vector<ResourceModel> models;
    const json* list = collected.resources.has_value()
                           ? GetPath(*collected.resources, {"resources", "resources"})
                           : nullptr;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranked;  // activity, id
    if (list != nullptr && list->is_array()) {
        std::size_t visible = 0;
        for (const auto& item : *list) {
            if (!item.is_object() || !ResourceKindVisible(item)) {
                continue;
            }
            ++visible;
            const std::uint64_t id = U64(item, "id");
            if (id == 0) {
                continue;
            }
            ranked.emplace_back(ResourceActivity(item), id);
        }
        std::sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.first != rhs.first) {
                return lhs.first > rhs.first;
            }
            return lhs.second < rhs.second;
        });
        if (visible > options.max_resources) {
            notes->push_back("资源共 " + std::to_string(visible) + " 个，按活动量只展示前 " +
                             std::to_string(options.max_resources) + " 个");
        }
    }

    const std::size_t limit = options.max_resources;
    for (const auto& entry : ranked) {
        if (models.size() >= limit) {
            break;
        }
        ResourceModel model;
        model.id = entry.second;
        // 从 resources 视图补齐名称/统计；history 只用于事件级别推导。
        if (list != nullptr) {
            for (const auto& item : *list) {
                if (U64(item, "id") == model.id) {
                    ApplyResourcesView(&model, item);
                    break;
                }
            }
        }
        const auto history = collected.histories.find(model.id);
        if (history != collected.histories.end()) {
            ApplyHistoryView(&model, history->second);
        }
        model.display = model.doctor_name.empty() ? model.debug_name : model.doctor_name;
        if (model.display.empty()) {
            model.display = "resource#" + std::to_string(model.id);
        }
        models.push_back(std::move(model));
    }

    // primitives 退路：资源历史为空时，primitive 事件里仍有 resource_id/gid。
    if (collected.primitives.has_value()) {
        const json* flow = GetPath(*collected.primitives, {"primitives", "flow"});
        if (flow != nullptr && flow->is_array()) {
            std::map<std::uint64_t, ResourceModel> primitive_models;
            for (const Event& event : EventsFromArray(*flow)) {
                if (event.resource_id == 0) {
                    continue;
                }
                ResourceModel& model = primitive_models[event.resource_id];
                if (model.id == 0) {
                    model.id = event.resource_id;
                    model.kind = "primitive";
                    model.debug_name = "resource#" + std::to_string(event.resource_id);
                    model.display = model.debug_name;
                }
                ApplyHistoryEvents(&model, {event});
            }
            for (auto& entry : primitive_models) {
                if (list != nullptr) {
                    for (const auto& item : *list) {
                        if (U64(item, "id") == entry.first) {
                            ApplyResourcesView(&entry.second, item);
                            entry.second.display = entry.second.debug_name;
                            break;
                        }
                    }
                }
                bool merged = false;
                for (auto& model : models) {
                    if (model.id != entry.first) {
                        continue;
                    }
                    if (!model.owner_known && entry.second.owner_known) {
                        model.owner_gid = entry.second.owner_gid;
                        model.owner_known = true;
                    }
                    for (std::uint64_t waiter : entry.second.waiters) {
                        AddWaiter(&model.waiters, waiter);
                    }
                    model.max_waiters =
                        std::max(model.max_waiters, entry.second.max_waiters);
                    merged = true;
                    break;
                }
                if (!merged && models.size() < limit) {
                    models.push_back(entry.second);
                }
            }
            if (!primitive_models.empty()) {
                notes->push_back(
                    "VIEW history 为空（事件窗口已冲刷），owner/waiters 由 VIEW primitives 推导");
            }
        }
    }

    // 合并 Doctor deadlock-report 的 mutex edges（权威 owner / waiters 计数）。
    if (collected.deadlock.has_value()) {
        const json* edges = Get(*collected.deadlock, "edges");
        if (edges != nullptr && edges->is_array()) {
            for (const auto& edge : *edges) {
                if (Str(edge, "kind") != "mutex") {
                    continue;
                }
                const std::string name = Str(edge, "from");
                if (name.empty()) {
                    continue;
                }
                const std::uint64_t owner = U64(edge, "owner_gid");
                const bool owner_known = Bool(edge, "owner_known");
                const std::uint64_t waiters = U64(edge, "waiters");
                ResourceModel* target = nullptr;
                for (auto& model : models) {
                    if (!model.doctor_name.empty()) {
                        continue;
                    }
                    if (model.kind.find("Mutex") == std::string::npos) {
                        continue;
                    }
                    if (owner_known && owner != 0 && model.owner_known &&
                        model.owner_gid == owner) {
                        target = &model;
                        break;
                    }
                }
                if (target == nullptr) {
                    // 没有可匹配的 DebugServer 资源（或 owner 未知）：建立 Doctor-only 节点。
                    for (auto& model : models) {
                        if (model.doctor_name == name) {
                            target = &model;
                            break;
                        }
                    }
                    if (target == nullptr) {
                        // DebugServer 不可用时，Doctor 的资源清单就是全部可用数据；
                        // DebugServer 可用时，owner=0/waiters=0 的注册名无法与
                        // resource_id 对齐（协议里没有地址字段），不重复造节点。
                        if (owner_known || waiters > 0 || !collected.debug_ok) {
                            ResourceModel model;
                            model.kind = "Mutex";
                            model.doctor_name = name;
                            model.display = name;
                            model.debug_name = name;
                            model.doctor_only = true;
                            model.owner_gid = owner;
                            model.owner_known = owner_known;
                            model.waits = waiters;
                            models.push_back(std::move(model));
                        }
                        continue;
                    }
                }
                target->doctor_name = name;
                target->display = name;
                target->doctor_only = false;
                if (owner_known && owner != 0) {
                    target->owner_gid = owner;
                    target->owner_known = true;
                }
                if (waiters > target->waiters.size()) {
                    notes->push_back(name + ": Doctor waiters=" + std::to_string(waiters) +
                                     "，事件窗口推导到 " +
                                     std::to_string(target->waiters.size()) +
                                     " 个（窗口截断，不补造 gid）");
                }
            }
        }
    }

    // 去掉完全无信息、且非 Doctor 命名的内部资源。
    const auto is_primary_kind = [](const std::string& kind) {
        return kind.find("Mutex") != std::string::npos ||
               kind.find("Channel") != std::string::npos ||
               kind.find("ConditionVariable") != std::string::npos ||
               kind == "primitive";
    };
    std::vector<ResourceModel> visible;
    visible.reserve(models.size());
    for (auto& model : models) {
        if (model.doctor_only) {
            visible.push_back(std::move(model));
            continue;
        }
        const bool interesting = !model.waiters.empty() || model.owner_known ||
                                 model.waits != 0 || model.holds != 0 || model.wakes != 0 ||
                                 model.events != 0;
        if (interesting || !model.doctor_name.empty() || is_primary_kind(model.kind)) {
            visible.push_back(std::move(model));
        }
    }
    return visible;
}

// ---------------------------------------------------------------------------
// 环检测（wait-for / resource）
// ---------------------------------------------------------------------------

struct CycleResult {
    std::vector<std::vector<std::string>> cycles;
};

std::string CycleSignature(const std::vector<std::string>& cycle) {
    if (cycle.empty()) {
        return {};
    }
    std::size_t best = 0;
    for (std::size_t index = 1; index < cycle.size(); ++index) {
        if (cycle[index] < cycle[best]) {
            best = index;
        }
    }
    std::vector<std::string> rotated;
    rotated.reserve(cycle.size());
    for (std::size_t step = 0; step < cycle.size(); ++step) {
        rotated.push_back(cycle[(best + step) % cycle.size()]);
    }
    return Join(rotated, "->");
}

std::vector<std::vector<std::string>> FindWaitCycles(Graph* graph) {
    std::map<std::string, std::vector<std::pair<std::string, std::size_t>>> adjacency;
    for (std::size_t index = 0; index < graph->edges.size(); ++index) {
        const Edge& edge = graph->edges[index];
        if (edge.kind != "wait" && edge.kind != "held") {
            continue;
        }
        adjacency[edge.from].push_back({edge.to, index});
    }
    std::vector<std::string> path;
    std::set<std::string> in_path;
    std::set<std::string> signatures;
    std::vector<std::vector<std::string>> cycles;
    std::vector<std::size_t> path_edges;

    const std::function<void(const std::string&, const std::string&)> dfs =
        [&](const std::string& start, const std::string& current) {
            if (cycles.size() >= kMaxCycles || path.size() >= kMaxCycleEdges) {
                return;
            }
            const auto found = adjacency.find(current);
            if (found == adjacency.end()) {
                return;
            }
            for (const auto& step : found->second) {
                if (step.first == start && path.size() >= 2) {
                    const std::string signature = CycleSignature(path);
                    if (signatures.insert(signature).second) {
                        cycles.push_back(path);
                    }
                    continue;
                }
                if (in_path.count(step.first) != 0) {
                    continue;
                }
                in_path.insert(step.first);
                path.push_back(step.first);
                path_edges.push_back(step.second);
                dfs(start, step.first);
                path_edges.pop_back();
                path.pop_back();
                in_path.erase(step.first);
                if (cycles.size() >= kMaxCycles) {
                    return;
                }
            }
        };

    for (const auto& node : graph->nodes) {
        if (node.kind != "g" && node.kind != "resource") {
            continue;
        }
        path.clear();
        in_path.clear();
        path.push_back(node.id);
        in_path.insert(node.id);
        dfs(node.id, node.id);
        if (cycles.size() >= kMaxCycles) {
            break;
        }
    }

    // 只保留同时含 G 与 resource 的环，并标记节点/边。
    std::vector<std::vector<std::string>> filtered;
    for (const auto& cycle : cycles) {
        bool has_g = false;
        bool has_resource = false;
        for (const std::string& id : cycle) {
            const Node* node = graph->Find(id);
            if (node == nullptr) {
                continue;
            }
            has_g = has_g || node->kind == "g";
            has_resource = has_resource || node->kind == "resource";
        }
        if (!has_g || !has_resource) {
            continue;
        }
        filtered.push_back(cycle);
        for (std::size_t index = 0; index < cycle.size(); ++index) {
            const std::string& from = cycle[index];
            const std::string& to = cycle[(index + 1) % cycle.size()];
            if (Node* node = graph->Find(from)) {
                node->deadlock = true;
            }
            for (Edge& edge : graph->edges) {
                if (edge.from == from && edge.to == to &&
                    (edge.kind == "wait" || edge.kind == "held")) {
                    edge.deadlock = true;
                }
            }
        }
    }
    std::vector<std::vector<std::string>> closed;
    for (auto& cycle : filtered) {
        cycle.push_back(cycle.front());
        closed.push_back(std::move(cycle));
    }
    return closed;
}

// ---------------------------------------------------------------------------
// 五种图
// ---------------------------------------------------------------------------

std::string Condense(const std::string& text, std::size_t limit = 140) {
    std::string out;
    out.reserve(std::min(text.size(), limit) + 3);
    for (char ch : text) {
        if (out.size() >= limit) {
            out += "...";
            return out;
        }
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            if (!out.empty() && out.back() != ' ') {
                out.push_back(' ');
            }
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

std::string SourceNotes(const Collected& collected, bool include_doctor) {
    std::string note;
    note += collected.debug_ok ? "DebugServer: ok" : "DebugServer: 不可用";
    if (!collected.debug_ok && !collected.debug_error.empty()) {
        note += "（" + Condense(collected.debug_error) + "）";
    }
    if (include_doctor) {
        note += collected.doctor_ok ? "；Doctor: ok" : "；Doctor: 不可用";
        if (!collected.doctor_ok && !collected.doctor_error.empty()) {
            note += "（" + Condense(collected.doctor_error) + "）";
        }
    }
    return note;
}

Graph BuildWaitGraph(const Collected& collected, const Options& options, bool focus_wait) {
    Graph graph;
    graph.type = focus_wait ? "wait-for" : "resource";
    graph.title = std::string(focus_wait ? "wait-for" : "resource") + " @ " +
                  collected.source_desc;
    std::vector<std::string> notes;

    std::vector<ResourceModel> resources = BuildResourceModels(collected, options, &notes);
    if (collected.window_truncated) {
        notes.push_back("事件窗口已截断（window.truncated=true）：等待者可能不完整");
    }
    notes.push_back(SourceNotes(collected, true));

    std::size_t shown = 0;
    for (const ResourceModel& model : resources) {
        const bool waitable = model.kind.find("Mutex") != std::string::npos ||
                              model.kind.find("Channel") != std::string::npos ||
                              model.kind == "primitive";
        if (focus_wait) {
            // wait-for 聚焦「G → 资源 → 持有者」：ParkingCondition 等内部会合点
            // 没有 owner 语义，混进来只会让环更难读；它们仍会出现在 resource 图。
            if (!waitable) {
                continue;
            }
            const bool relevant =
                model.owner_known || !model.waiters.empty() || model.doctor_only;
            if (!relevant) {
                continue;
            }
        }
        ++shown;
        graph.AddNode(model.display, ResourceLabel(model), "resource", "resource");
        if (model.owner_known && model.owner_gid != 0) {
            const std::string gid = "G" + std::to_string(model.owner_gid);
            graph.AddNode(gid, gid + "\nowner", "g", "g");
            graph.AddEdge(model.display, gid, "held", "held");
        }
        for (std::uint64_t waiter : model.waiters) {
            const std::string gid = "G" + std::to_string(waiter);
            graph.AddNode(gid, gid + "\nwaiting", "g", "g");
            graph.AddEdge(gid, model.display, "wait", "wait");
        }
        if (!model.waiters.empty()) {
            // 让边标签带上等待深度（事件里有 waiters 时用实际上限）。
            const std::string label =
                model.max_waiters > 0
                    ? "wait (depth=" + std::to_string(model.max_waiters) + ")"
                    : "wait";
            for (Edge& edge : graph.edges) {
                if (edge.kind == "wait" && edge.to == model.display) {
                    edge.label = label;
                }
            }
        }
        if (model.doctor_only && model.waits != 0) {
            notes.push_back(model.display + ": 只有 waiters=" + std::to_string(model.waits) +
                            " 计数（Doctor 的 waiter_ids=unavailable），不画虚假等待者边");
        }
    }

    if (shown == 0) {
        graph.ok = false;
        notes.push_back(
            "需要 Doctor 或 DebugServer：没有可用的等待/持有关系。目标进程需以 "
            "-DGO2CPP_DEBUG_DUMP=ON（DebugServer）与 -DGO2CPP_DIAGNOSTIC=ON（Doctor）"
            "构建并启动对应服务。");
    }
    graph.cycles = FindWaitCycles(&graph);
    if (!graph.cycles.empty()) {
        std::vector<std::string> cycle_text;
        for (const auto& cycle : graph.cycles) {
            cycle_text.push_back(Join(cycle, " -> "));
        }
        notes.push_back("检测到 " + std::to_string(graph.cycles.size()) +
                        " 个等待环：" + Join(cycle_text, "；"));
    } else if (shown != 0 && collected.debug_ok) {
        notes.push_back("未检测到闭合等待环（可能需要更大的事件窗口，或确实没有循环等待）");
    }

    graph.notes = std::move(notes);
    graph.meta["pid"] = options.has_pid ? json(options.pid) : json(nullptr);
    graph.meta["sources"] = {{"debugserver", collected.debug_ok},
                             {"doctor", collected.doctor_ok}};
    graph.meta["resource_count"] = shown;
    graph.meta["window_truncated"] = collected.window_truncated;
    return graph;
}

std::optional<std::string> StateOfEvent(const std::string& code) {
    if (code == "kGCreated") {
        return "created";
    }
    if (code == "kGStarted" || code == "kGResumed") {
        return "running";
    }
    if (code == "kParkEnter" || code == "kParkCaptured") {
        return "parked";
    }
    if (code == "kParkReturn") {
        return "running";
    }
    if (code == "kWakeEnqueued" || code == "kWakePending" || code == "kWakeBatch" ||
        code == "kEnqueue" || code == "kRunQueuePush") {
        return "ready";
    }
    if (code == "kRunQueuePop" || code == "kRunQueueClaimOk") {
        return "running";
    }
    if (code == "kGCompleted") {
        return "completed";
    }
    if (code == "kGFailed") {
        return "failed";
    }
    if (code == "kGCancelled") {
        return "cancelled";
    }
    return std::nullopt;
}

std::string ShortEventName(const std::string& code) {
    if (code == "kGCreated") {
        return "created";
    }
    if (code == "kGStarted") {
        return "started";
    }
    if (code == "kGResumed") {
        return "resumed";
    }
    if (code == "kParkEnter" || code == "kParkCaptured") {
        return "park";
    }
    if (code == "kParkReturn") {
        return "park_return";
    }
    if (code == "kWakeEnqueued" || code == "kWakePending") {
        return "wake";
    }
    if (code == "kEnqueue") {
        return "enqueue";
    }
    if (code == "kRunQueuePush") {
        return "runqueue_push";
    }
    if (code == "kRunQueuePop") {
        return "runqueue_pop";
    }
    if (code == "kRunQueueClaimOk") {
        return "claim";
    }
    if (code == "kGCompleted") {
        return "completed";
    }
    if (code == "kGFailed") {
        return "failed";
    }
    if (code == "kGCancelled") {
        return "cancelled";
    }
    return code;
}

std::vector<Event> FiberStateEvents(const Collected& collected, std::vector<std::string>* notes) {
    std::vector<Event> events;
    if (collected.sched.has_value()) {
        const json* flow = GetPath(*collected.sched, {"sched", "flow"});
        if (flow != nullptr) {
            events = EventsFromArray(*flow);
        }
    }
    if (events.empty() && collected.file_mode) {
        events = collected.events;
    }
    if (events.empty() && collected.gmp.has_value()) {
        notes->push_back("事件环窗口为空，fiber-state 只显示 VIEW gmp 的快照状态");
    }
    return SortedUnique(std::move(events), 4000);
}

std::string CurrentStateName(const std::string& state) {
    if (state == "kWaiting") {
        return "parked";
    }
    if (state == "kRunning") {
        return "running";
    }
    if (state == "kRunnable" || state == "kNew") {
        return "ready";
    }
    if (state == "kDead") {
        return "completed";
    }
    if (state == "kFailed") {
        return "failed";
    }
    if (state == "kCancelled") {
        return "cancelled";
    }
    if (state.empty()) {
        return "unknown";
    }
    return state;
}

Graph BuildFiberStateGraph(const Collected& collected, const Options& options) {
    Graph graph;
    graph.type = "fiber-state";
    graph.title = "fiber-state @ " + collected.source_desc;
    std::vector<std::string> notes;
    std::vector<Event> events = FiberStateEvents(collected, &notes);

    struct Transition {
        std::string from;
        std::string to;
        std::string label;
        std::uint64_t count{0};
    };
    std::map<std::uint64_t, std::vector<Transition>> per_gid;
    std::map<std::uint64_t, std::string> last_state;
    std::map<std::uint64_t, std::uint64_t> event_count;

    for (const Event& event : events) {
        if (event.gid == 0) {
            continue;
        }
        const auto state = StateOfEvent(event.code);
        if (!state.has_value()) {
            continue;
        }
        ++event_count[event.gid];
        const auto previous = last_state.find(event.gid);
        if (previous == last_state.end()) {
            last_state[event.gid] = *state;
            per_gid[event.gid].push_back(
                Transition{"<start>", *state, ShortEventName(event.code), 1});
            continue;
        }
        if (previous->second == *state) {
            continue;
        }
        auto& transitions = per_gid[event.gid];
        bool merged = false;
        for (auto& transition : transitions) {
            if (transition.from == previous->second && transition.to == *state) {
                ++transition.count;
                merged = true;
                break;
            }
        }
        if (!merged) {
            transitions.push_back(
                Transition{previous->second, *state, ShortEventName(event.code), 1});
        }
        previous->second = *state;
    }

    // 优先展示阻塞/等待中的 G（现场最关心的就是它们），其余按事件数取前 max_g。
    std::map<std::uint64_t, int> priority;
    std::map<std::uint64_t, std::string> snapshot;
    if (collected.park.has_value()) {
        const json* items = GetPath(*collected.park, {"park", "items"});
        if (items != nullptr && items->is_array()) {
            for (const auto& item : *items) {
                const std::uint64_t gid = U64(item, "gid");
                const std::string status = Str(item, "status");
                if (gid != 0 && (status == "open" || status == "woken_pending_resume" ||
                                 status == "waiting_no_park_in_window")) {
                    priority[gid] = 0;
                }
            }
        }
    }
    if (collected.gmp.has_value()) {
        if (const json* blocked = GetPath(*collected.gmp, {"gmp", "blocked", "items"});
            blocked != nullptr && blocked->is_array()) {
            for (const auto& item : *blocked) {
                const std::uint64_t gid = U64(item, "gid");
                if (gid != 0) {
                    priority[gid] = 0;
                }
            }
        }
        const json* g = GetPath(*collected.gmp, {"gmp", "g"});
        if (g != nullptr && g->is_array()) {
            for (const auto& item : *g) {
                const std::uint64_t gid = U64(item, "gid");
                if (gid == 0) {
                    continue;
                }
                const std::string state = Str(item, "state");
                snapshot[gid] = state;
                if (priority.count(gid) != 0) {
                    continue;
                }
                if (state == "kWaiting") {
                    priority[gid] = 1;
                } else if (state == "kRunning" || state == "kRunnable") {
                    priority[gid] = 2;
                } else {
                    priority[gid] = 3;
                }
            }
        }
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranked;  // count, gid
    for (const auto& entry : event_count) {
        ranked.emplace_back(entry.second, entry.first);
    }
    for (const auto& entry : snapshot) {
        if (event_count.count(entry.first) == 0) {
            ranked.emplace_back(0, entry.first);
        }
    }
    std::sort(ranked.begin(), ranked.end(), [&](const auto& lhs, const auto& rhs) {
        const auto lhs_priority = priority.count(lhs.second) != 0 ? priority[lhs.second] : 4;
        const auto rhs_priority = priority.count(rhs.second) != 0 ? priority[rhs.second] : 4;
        if (lhs_priority != rhs_priority) {
            return lhs_priority < rhs_priority;
        }
        if (lhs.first != rhs.first) {
            return lhs.first > rhs.first;
        }
        return lhs.second < rhs.second;
    });
    std::vector<std::uint64_t> selected;
    for (const auto& entry : ranked) {
        if (selected.size() >= options.max_g) {
            notes.push_back("只展示前 " + std::to_string(options.max_g) + " 个 G（共 " +
                            std::to_string(ranked.size()) + " 个）");
            break;
        }
        selected.push_back(entry.second);
    }

    std::size_t snapshot_mismatch = 0;
    for (std::uint64_t gid : selected) {
        const auto transitions = per_gid.find(gid);
        if (transitions != per_gid.end() && !transitions->second.empty()) {
            for (const Transition& transition : transitions->second) {
                const std::string from_id =
                    transition.from == "<start>" ? "G" + std::to_string(gid) + ":start"
                                                 : "G" + std::to_string(gid) + ":" + transition.from;
                const std::string to_id = "G" + std::to_string(gid) + ":" + transition.to;
                graph.AddNode(from_id,
                              transition.from == "<start>"
                                  ? "G" + std::to_string(gid) + "\n(窗口起点)"
                                  : "G" + std::to_string(gid) + "\n" + transition.from,
                              "state", "state_" + transition.from);
                graph.AddNode(to_id, "G" + std::to_string(gid) + "\n" + transition.to,
                              "state", "state_" + transition.to);
                graph.AddEdge(from_id, to_id,
                              transition.label +
                                  (transition.count > 1
                                       ? " x" + std::to_string(transition.count)
                                       : ""),
                              "transition");
            }
            const auto current = snapshot.find(gid);
            if (current != snapshot.end()) {
                const std::string mapped = CurrentStateName(current->second);
                const auto last = last_state.find(gid);
                if (last != last_state.end() && last->second != mapped) {
                    ++snapshot_mismatch;
                }
            }
        } else {
            const std::string state = snapshot.count(gid) != 0
                                          ? CurrentStateName(snapshot[gid])
                                          : "unknown";
            const std::string id = "G" + std::to_string(gid) + ":" + state;
            graph.AddNode(id, "G" + std::to_string(gid) + "\n" + state, "state",
                          "state_" + state);
        }
    }
    if (snapshot_mismatch != 0) {
        notes.push_back(std::to_string(snapshot_mismatch) +
                        " 个 G 的快照状态与窗口最后状态不同（事件窗口截断，快照可能更新）");
    }

    if (selected.empty()) {
        graph.ok = false;
        notes.push_back(
            "需要 DebugServer 的事件环（VIEW sched / VIEW gmp）：未找到任何 G 事件。"
            "目标进程需以 -DGO2CPP_DEBUG_DUMP=ON 构建，且在事件窗口内发生调度事件。");
    } else if (events.empty()) {
        notes.push_back("没有状态迁移边：事件窗口为空，仅显示当前快照状态");
    }
    notes.push_back(SourceNotes(collected, false));
    graph.notes = std::move(notes);
    graph.meta["pid"] = options.has_pid ? json(options.pid) : json(nullptr);
    graph.meta["goroutines"] = selected.size();
    graph.meta["events"] = events.size();
    graph.meta["sources"] = {{"debugserver", collected.debug_ok}};
    return graph;
}

Graph BuildGmpGraph(const Collected& collected, const Options& options) {
    Graph graph;
    graph.type = "gmp";
    graph.title = "gmp @ " + collected.source_desc;
    std::vector<std::string> notes;

    json gmp = json::object();
    bool has_gmp = false;
    if (collected.gmp.has_value()) {
        if (const json* node = Get(*collected.gmp, "gmp"); node != nullptr && node->is_object()) {
            gmp = *node;
            has_gmp = true;
        } else if (Get(*collected.gmp, "processors") != nullptr) {
            gmp = *collected.gmp;
            has_gmp = true;
        }
    }
    json machines = json::array();
    json processors = json::array();
    json tasks = json::array();
    json blocked = json::array();
    std::uint64_t runnable_global = 0;
    std::uint64_t local_total = 0;
    std::uint64_t goroutine_total = 0;
    bool from_doctor = false;

    if (has_gmp) {
        if (const json* value = Get(gmp, "machines"); value != nullptr && value->is_array()) {
            machines = *value;
        }
        if (const json* value = Get(gmp, "processors"); value != nullptr && value->is_array()) {
            processors = *value;
        }
        if (const json* value = Get(gmp, "g"); value != nullptr && value->is_array()) {
            tasks = *value;
        }
        if (const json* value = GetPath(gmp, {"blocked", "items"});
            value != nullptr && value->is_array()) {
            blocked = *value;
        }
        if (const json* summary = Get(gmp, "summary"); summary != nullptr) {
            runnable_global = U64(*summary, "runnable_global");
            if (const json* runqueue = Get(*summary, "runqueue"); runqueue != nullptr) {
                local_total = U64(*runqueue, "local_total");
                runnable_global = U64(*runqueue, "global", runnable_global);
            }
        }
    } else if (collected.doctor_status.has_value()) {
        const json& status = *collected.doctor_status;
        if (const json* value = Get(status, "machines"); value != nullptr && value->is_array()) {
            machines = *value;
        }
        if (const json* value = Get(status, "processors"); value != nullptr && value->is_array()) {
            processors = *value;
        }
        if (const json* value = Get(status, "runqueue"); value != nullptr) {
            local_total = U64(*value, "local_total");
            runnable_global = U64(*value, "global");
        }
        if (const json* goroutines = Get(status, "goroutines"); goroutines != nullptr) {
            goroutine_total = U64(*goroutines, "total");
        }
        from_doctor = true;
        notes.push_back("DebugServer 的 VIEW gmp 不可用，G/M/P 拓扑来自 Doctor status（G 级快照缺失）");
    }

    if (!has_gmp && !from_doctor) {
        graph.ok = false;
        notes.push_back(
            "需要 DebugServer 的 VIEW gmp（或 Doctor status）：两者都不可用。"
            "目标进程需以 -DGO2CPP_DEBUG_DUMP=ON/-DGO2CPP_DIAGNOSTIC=ON 构建并启动服务。");
    }

    // 全局 runqueue 节点
    graph.AddNode("runqueue",
                  "global runqueue\nrunnable_global=" + std::to_string(runnable_global) +
                      "\nlocal_total=" + std::to_string(local_total),
                  "queue", "queue");

    // P 节点 + runqueue 边
    for (const auto& processor : processors) {
        const std::uint64_t id = U64(processor, "id");
        const std::string state = Str(processor, "state");
        const std::uint64_t queued = U64(processor, "queued");
        const std::string node_id = "P" + std::to_string(id);
        graph.AddNode(node_id,
                      "P" + std::to_string(id) + "\n" + state +
                          "\nqueued=" + std::to_string(queued),
                      "processor", "processor");
        graph.AddEdge("runqueue", node_id, "queued=" + std::to_string(queued), "runqueue");
    }

    // M 节点 + M→P 边
    for (const auto& machine : machines) {
        const std::uint64_t id = U64(machine, "id");
        const std::string state = Str(machine, "state");
        const std::uint64_t processor = U64(machine, "processor");
        const std::string node_id = "M" + std::to_string(id);
        graph.AddNode(node_id,
                      "M" + std::to_string(id) + "\n" + state +
                          "\nP" + std::to_string(processor),
                      "machine", state == "kParked" ? "machine_parked" : "machine");
        graph.AddNode("P" + std::to_string(processor),
                      "P" + std::to_string(processor), "processor", "processor");
        graph.AddEdge(node_id, "P" + std::to_string(processor), state, "affinity");
        const std::uint64_t blocking = U64(machine, "blocking_task");
        if (blocking != 0) {
            const std::string gid = "G" + std::to_string(blocking);
            graph.AddNode(gid, gid + "\nblocking M" + std::to_string(id), "g", "g");
            graph.AddEdge(gid, node_id, "blocking", "blocking");
        }
    }

    // G 节点：优先 blocked，其次 running/waiting，再按事件顺序。
    std::set<std::uint64_t> blocked_gids;
    for (const auto& item : blocked) {
        const std::uint64_t gid = U64(item, "gid");
        if (gid != 0) {
            blocked_gids.insert(gid);
        }
    }
    std::vector<std::pair<int, std::uint64_t>> ranked;
    for (const auto& task : tasks) {
        const std::uint64_t gid = U64(task, "gid");
        if (gid == 0) {
            continue;
        }
        int priority = 3;
        if (blocked_gids.count(gid) != 0) {
            priority = 0;
        } else if (Str(task, "state") == "kRunning") {
            priority = 1;
        } else if (Str(task, "state") == "kWaiting") {
            priority = 2;
        }
        ranked.emplace_back(priority, gid);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first) {
            return lhs.first < rhs.first;
        }
        return lhs.second < rhs.second;
    });
    std::size_t shown_g = 0;
    for (const auto& entry : ranked) {
        if (shown_g >= options.max_g) {
            notes.push_back("只展示前 " + std::to_string(options.max_g) + " 个 G（共 " +
                            std::to_string(ranked.size()) + " 个）");
            break;
        }
        ++shown_g;
        const std::uint64_t gid = entry.second;
        const json* task = nullptr;
        for (const auto& item : tasks) {
            if (U64(item, "gid") == gid) {
                task = &item;
                break;
            }
        }
        const std::string state = task != nullptr ? Str(*task, "state") : "kUnknown";
        const bool is_blocked = blocked_gids.count(gid) != 0;
        graph.AddNode("G" + std::to_string(gid),
                      "G" + std::to_string(gid) + "\n" + state +
                          (is_blocked ? "\nblocked" : ""),
                      "g", is_blocked ? "g_blocked" : "g");
        if (task != nullptr) {
            const std::uint64_t processor = U64(*task, "last_processor_id");
            if (processor < processors.size()) {
                graph.AddEdge("G" + std::to_string(gid), "P" + std::to_string(processor),
                              "last P", "affinity");
            }
        }
    }
    if (goroutine_total != 0) {
        graph.AddNode("all-goroutines",
                      "goroutines\ntotal=" + std::to_string(goroutine_total),
                      "g", "g");
    }

    if (has_gmp) {
        notes.push_back("P/M/G 来自 VIEW gmp：runqueue、blocked、last_processor_id");
    }
    notes.push_back(SourceNotes(collected, collected.doctor_status.has_value()));
    graph.notes = std::move(notes);
    graph.meta["pid"] = options.has_pid ? json(options.pid) : json(nullptr);
    graph.meta["processors"] = processors.size();
    graph.meta["machines"] = machines.size();
    graph.meta["goroutines"] = ranked.size();
    graph.meta["sources"] = {{"debugserver", has_gmp}, {"doctor", from_doctor}};
    return graph;
}

// ---------------------------------------------------------------------------
// sequence：资源时间线（live）或事件文件（--replay）
// ---------------------------------------------------------------------------

std::string ResourceEventLabel(const Event& event) {
    if (IsLockAcquireCode(event.code)) {
        std::string label = event.code == "kRwLockRead" ? "rlock" : "lock";
        if (!event.result.empty()) {
            label += " " + event.result;
        }
        if (event.waiters != 0) {
            label += " (waiters=" + std::to_string(event.waiters) + ")";
        }
        return label;
    }
    if (IsLockReleaseCode(event.code)) {
        return "unlock";
    }
    if (IsPcWaitCode(event.code)) {
        return "wait";
    }
    if (IsPcNotifyCode(event.code)) {
        return event.code == "kPCNotifyAll" || event.code == "kCvNotifyAll" ? "notify all"
                                                                           : "notify";
    }
    if (IsChannelBlockCode(event.code)) {
        return event.code == "kChannelSend" ? "send (blocked)" : "recv (blocked)";
    }
    if (IsChannelOkCode(event.code)) {
        return event.code == "kChannelSendOk" ? "send ok" : "recv ok";
    }
    return event.code;
}

std::string ScheduledEventLabel(const Event& event) {
    if (event.code == "kGCreated") {
        return "spawn";
    }
    if (event.code == "kGStarted") {
        return "start";
    }
    if (event.code == "kGResumed") {
        return "resume";
    }
    if (event.code == "kParkEnter") {
        return "park" + (event.primitive_reason.empty() ? std::string()
                                                        : " (" + event.primitive_reason + ")");
    }
    if (event.code == "kParkReturn") {
        return "park return";
    }
    if (event.code == "kWakeEnqueued" || event.code == "kWakePending") {
        return "wake";
    }
    if (event.code == "kEnqueue") {
        return "enqueue";
    }
    if (event.code == "kGCompleted") {
        return "completed";
    }
    if (event.code == "kGFailed") {
        return "failed";
    }
    if (event.code == "kGCancelled") {
        return "cancelled";
    }
    return event.code;
}

Graph BuildSequenceGraph(const Collected& collected, const Options& options) {
    Graph graph;
    graph.type = "sequence";
    graph.title = "sequence @ " + collected.source_desc;
    std::vector<std::string> notes;

    // 资源名映射：resource_id → 显示名 / kind。
    std::map<std::uint64_t, std::string> resource_names;
    std::map<std::uint64_t, std::string> resource_kinds;
    if (collected.resources.has_value()) {
        const json* list = GetPath(*collected.resources, {"resources", "resources"});
        if (list != nullptr && list->is_array()) {
            for (const auto& item : *list) {
                const std::uint64_t id = U64(item, "id");
                if (id != 0) {
                    resource_names[id] = Str(item, "name", "resource#" + std::to_string(id));
                    resource_kinds[id] = Str(item, "kind");
                }
            }
        }
    }
    if (collected.deadlock.has_value()) {
        const json* edges = Get(*collected.deadlock, "edges");
        if (edges != nullptr && edges->is_array()) {
            for (const auto& edge : *edges) {
                if (Str(edge, "kind") == "mutex" && !Str(edge, "from").empty()) {
                    // Doctor 的注册名与 DebugServer 的 resource_id 没有直接字段可对应；
                    // 这里只用它做名称表的补充：按资源 id 顺序无法可靠映射，故不映射。
                }
            }
        }
    }

    std::vector<Event> events;
    bool resource_mode = false;
    for (const auto& entry : collected.histories) {
        const json* history_events = GetPath(entry.second, {"history", "events"});
        if (history_events == nullptr || !history_events->is_array() || history_events->empty()) {
            continue;
        }
        resource_mode = true;
        for (const auto& item : *history_events) {
            const auto event = ParseEvent(item);
            if (event.has_value()) {
                events.push_back(*event);
            }
        }
    }
    if (!resource_mode && collected.primitives.has_value()) {
        // 优先只取 Mutex/Channel 事件：primitives.flow 里 ParkingCondition 的
        // notify 会淹没真正的资源时序（真实场景里 945 条中 884 条是 PC）。
        std::vector<Event> focused;
        for (const char* key : {"mutex", "channel"}) {
            const json* list = GetPath(*collected.primitives, {"primitives", key});
            if (list == nullptr || !list->is_array()) {
                continue;
            }
            for (const auto& item : *list) {
                const auto event = ParseEvent(item);
                if (event.has_value() && event->resource_id != 0) {
                    focused.push_back(*event);
                }
            }
        }
        if (!focused.empty()) {
            events = std::move(focused);
            resource_mode = true;
            notes.push_back("VIEW history 为空（事件窗口已冲刷），时序改用 VIEW primitives 的 Mutex/Channel 事件");
        } else {
            const json* flow = GetPath(*collected.primitives, {"primitives", "flow"});
            if (flow != nullptr && flow->is_array()) {
                for (const auto& item : *flow) {
                    const auto event = ParseEvent(item);
                    if (event.has_value() && event->resource_id != 0) {
                        events.push_back(*event);
                    }
                }
                resource_mode = !events.empty();
            }
        }
    }
    if (events.empty() && collected.file_mode) {
        events = collected.events;
        resource_mode = false;
    }
    events = SortedUnique(std::move(events), options.max_events);

    // 有 Mutex/Channel 事件时聚焦它们：ParkingCondition 的 notify 数量极大，
    // 混在时序里会淹没真正的锁等待顺序；只有在完全没有锁/通道事件时才退回全部资源。
    std::set<std::uint64_t> focused_ids;
    for (const Event& event : events) {
        if (event.resource_id == 0) {
            continue;
        }
        const auto kind = resource_kinds.find(event.resource_id);
        if (kind == resource_kinds.end()) {
            continue;
        }
        if (kind->second.find("Mutex") != std::string::npos ||
            kind->second.find("Channel") != std::string::npos) {
            focused_ids.insert(event.resource_id);
        }
    }
    if (!focused_ids.empty()) {
        std::vector<Event> filtered;
        filtered.reserve(events.size());
        for (const Event& event : events) {
            if (event.resource_id == 0 || focused_ids.count(event.resource_id) != 0) {
                filtered.push_back(event);
            }
        }
        if (filtered.size() != events.size()) {
            notes.push_back("sequence 聚焦 Mutex/Channel（过滤掉 " +
                            std::to_string(events.size() - filtered.size()) +
                            " 条 ParkingCondition 事件）");
        }
        events = std::move(filtered);
    }

    std::vector<Message> raw_messages;
    if (resource_mode) {
        for (const Event& event : events) {
            if (event.gid == 0) {
                // 缺口事件（例如 PC notify-all 的 gid=0 聚合）没有发起者；
                // 强行画到 G0 会制造一个不存在的参与者。
                continue;
            }
            Message message;
            message.from = "G" + std::to_string(event.gid);
            message.to = event.resource_id != 0 ? "R" + std::to_string(event.resource_id) : "S";
            message.label = ResourceEventLabel(event);
            message.ts_ns = event.ts_ns;
            message.dashed = IsLockReleaseCode(event.code) || IsPcNotifyCode(event.code);
            raw_messages.push_back(std::move(message));
        }
    } else {
        for (const Event& event : events) {
            if (event.gid == 0) {
                continue;
            }
            const std::string gid = "G" + std::to_string(event.gid);
            Message message;
            message.label = ScheduledEventLabel(event);
            message.ts_ns = event.ts_ns;
            // park/enqueue 等由 G 发起；spawn/start/wake/resume 由调度器发起。
            if (event.code == "kParkEnter" || event.code == "kEnqueue" ||
                event.code == "kGCompleted" || event.code == "kGFailed" ||
                event.code == "kGCancelled" || event.code == "kParkReturn") {
                message.from = gid;
                message.to = "S";
            } else {
                message.from = "S";
                message.to = gid;
                message.dashed = true;
            }
            raw_messages.push_back(std::move(message));
        }
    }

    // 连续重复的相同操作（PC notify-all 尤其常见）折叠成一条并标次数。
    std::vector<Message> collapsed;
    for (auto& message : raw_messages) {
        if (!collapsed.empty() && collapsed.back().from == message.from &&
            collapsed.back().to == message.to && collapsed.back().label == message.label &&
            collapsed.back().dashed == message.dashed) {
            ++collapsed.back().count;
            continue;
        }
        collapsed.push_back(std::move(message));
    }
    raw_messages = std::move(collapsed);

    // 参与者按消息出现顺序收集；超过上限后不再引入新参与者，避免
    // 「消息引用了未声明 participant」的 Mermaid 自动建人。
    std::set<std::string> seen_participants;
    const auto ensure_participant = [&](const std::string& id) {
        if (seen_participants.count(id) != 0) {
            return true;
        }
        if (graph.participants.size() >= 24) {
            return false;
        }
        seen_participants.insert(id);
        std::string label = id;
        if (!id.empty() && id[0] == 'R') {
            try {
                const std::uint64_t resource_id =
                    static_cast<std::uint64_t>(std::stoull(id.substr(1)));
                const auto found = resource_names.find(resource_id);
                label = found == resource_names.end() ? "resource-" + id.substr(1)
                                                      : found->second;
            } catch (...) {
                label = id;
            }
        }
        graph.participants.push_back(Participant{id, label});
        return true;
    };
    for (const Message& message : raw_messages) {
        if (graph.messages.size() >= options.max_events) {
            notes.push_back("时序消息超过上限 " + std::to_string(options.max_events) + " 条，已截断");
            break;
        }
        if (!ensure_participant(message.from) || !ensure_participant(message.to)) {
            continue;
        }
        graph.messages.push_back(message);
    }

    if (graph.messages.empty()) {
        graph.ok = false;
        notes.push_back(
            "sequence 需要 DebugServer 的 VIEW history（资源时间线）或 --replay 的事件文件；"
            "当前没有可排序的事件。目标进程需以 -DGO2CPP_DEBUG_DUMP=ON 构建。");
    } else {
        notes.push_back("按 ts_ns 排序的资源/调度操作（" +
                        std::to_string(graph.messages.size()) + " 条，上限 " +
                        std::to_string(options.max_events) + "）");
    }
    if (collected.window_truncated) {
        notes.push_back("事件窗口已截断（window.truncated=true）：时序可能不完整");
    }
    notes.push_back(SourceNotes(collected, false));
    graph.notes = std::move(notes);
    graph.meta["pid"] = options.has_pid ? json(options.pid) : json(nullptr);
    graph.meta["messages"] = graph.messages.size();
    graph.meta["events"] = events.size();
    graph.meta["sources"] = {{"debugserver", collected.debug_ok}};
    return graph;
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

std::string NodeShapeMermaid(const Node& node) {
    if (node.kind == "g") {
        return "([\"";
    }
    if (node.kind == "state") {
        return "(\"";
    }
    return "[\"";
}

std::string NodeShapeMermaidClose(const Node& node) {
    if (node.kind == "g") {
        return "\"])";
    }
    if (node.kind == "state") {
        return "\")";
    }
    return "\"]";
}

void AddNoteNodeMermaid(const Graph& graph, std::string* out) {
    if (graph.notes.empty()) {
        return;
    }
    std::vector<std::string> notes;
    for (const std::string& note : graph.notes) {
        notes.push_back("• " + note);
    }
    *out += "  legend[\"" + MermaidLabel(Join(notes, "\n")) + "\"]\n";
}

std::string RenderMermaidSequence(const Graph& graph) {
    std::string out = "%% go2cpp-cli graph: " + graph.type + "\n";
    out += "sequenceDiagram\n";
    if (graph.participants.empty()) {
        out += "  participant S as Scheduler\n";
    } else {
        for (const Participant& participant : graph.participants) {
            out += "  participant " + participant.id + " as " +
                   MermaidMessage(participant.label) + "\n";
        }
    }
    for (const Message& message : graph.messages) {
        const std::string suffix =
            message.count > 1 ? " x" + std::to_string(message.count) : std::string();
        out += "  " + message.from + (message.dashed ? "-->>" : "->>") + " " +
               message.to + ": " + MermaidMessage(message.label + suffix) + "\n";
    }
    if (graph.messages.empty()) {
        const std::string target =
            graph.participants.empty() ? "S" : graph.participants.front().id;
        std::string note = "无事件";
        if (!graph.notes.empty()) {
            note = graph.notes.front();
        }
        out += "  Note over " + target + ": " + MermaidMessage(note) + "\n";
    }
    if (!graph.notes.empty()) {
        for (const std::string& note : graph.notes) {
            out += "  %% note: " + note + "\n";
        }
    }
    return out;
}

std::string RenderMermaid(const Graph& graph) {
    if (graph.type == "sequence") {
        return RenderMermaidSequence(graph);
    }
    std::string out = "%% go2cpp-cli graph: " + graph.type + " — " + graph.title + "\n";
    out += "graph LR\n";

    std::map<std::string, std::string> ids;
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        ids[graph.nodes[index].id] = "n" + std::to_string(index);
    }

    std::set<std::string> styles;
    for (const Node& node : graph.nodes) {
        const auto found = ids.find(node.id);
        if (found == ids.end()) {
            continue;
        }
        out += "  " + found->second + NodeShapeMermaid(node) +
               MermaidLabel(node.label) + NodeShapeMermaidClose(node) + "\n";
        styles.insert(node.style.empty() ? node.kind : node.style);
    }

    std::size_t link_index = 0;
    std::vector<std::pair<std::size_t, std::string>> deadlock_links;
    for (const Edge& edge : graph.edges) {
        const auto from = ids.find(edge.from);
        const auto to = ids.find(edge.to);
        if (from == ids.end() || to == ids.end()) {
            continue;
        }
        out += "  " + from->second + " -->";
        if (!edge.label.empty()) {
            out += "|\"" + MermaidEdgeLabel(edge.label) + "\"|";
        }
        out += " " + to->second + "\n";
        if (edge.deadlock) {
            deadlock_links.emplace_back(link_index, edge.label);
        }
        ++link_index;
    }

    const char* kClassDefs[] = {
        "  classDef g fill:#e8f0fe,stroke:#1a73e8,color:#174ea6",
        "  classDef g_blocked fill:#ffe3e3,stroke:#c00000,color:#8b0000",
        "  classDef resource fill:#f1f3f4,stroke:#5f6368,color:#202124",
        "  classDef machine fill:#e6f4ea,stroke:#137333,color:#0d652d",
        "  classDef machine_parked fill:#fef7e0,stroke:#b06000,color:#7a4f01",
        "  classDef processor fill:#e0f7fa,stroke:#00838f,color:#006064",
        "  classDef queue fill:#f3e8fd,stroke:#7b1fa2,color:#4a148c",
        "  classDef note fill:#fff8e1,stroke:#b06000,color:#7a4f01",
        "  classDef state_start fill:#ffffff,stroke:#9aa0a6,color:#3c4043,stroke-dasharray: 4 2",
        "  classDef state_created fill:#e8f0fe,stroke:#1a73e8,color:#174ea6",
        "  classDef state_running fill:#e6f4ea,stroke:#137333,color:#0d652d",
        "  classDef state_parked fill:#fef7e0,stroke:#b06000,color:#7a4f01",
        "  classDef state_ready fill:#e0f7fa,stroke:#00838f,color:#006064",
        "  classDef state_completed fill:#f1f3f4,stroke:#5f6368,color:#202124",
        "  classDef state_failed fill:#ffe3e3,stroke:#c00000,color:#8b0000",
        "  classDef state_cancelled fill:#f3e8fd,stroke:#7b1fa2,color:#4a148c",
        "  classDef state_unknown fill:#f1f3f4,stroke:#9aa0a6,color:#3c4043",
        "  classDef deadlock fill:#ffe3e3,stroke:#c00000,stroke-width:2px,color:#8b0000",
    };
    for (const char* definition : kClassDefs) {
        out += std::string(definition) + ";\n";
    }

    // 按 style 分组 class 语句。
    std::map<std::string, std::vector<std::string>> class_members;
    for (const Node& node : graph.nodes) {
        const auto found = ids.find(node.id);
        if (found == ids.end()) {
            continue;
        }
        class_members[node.deadlock ? std::string("deadlock")
                                    : (node.style.empty() ? node.kind : node.style)]
            .push_back(found->second);
    }
    for (const auto& entry : class_members) {
        out += "  class " + Join(entry.second, ",") + " " + entry.first + ";\n";
    }

    for (const auto& link : deadlock_links) {
        out += "  linkStyle " + std::to_string(link.first) +
               " stroke:#c00000,stroke-width:2px;\n";
    }

    AddNoteNodeMermaid(graph, &out);
    if (!graph.notes.empty()) {
        out += "  class legend note;\n";
    }
    return out;
}

std::string RenderDot(const Graph& graph) {
    std::string out;
    if (graph.type == "sequence") {
        out += "digraph go2cpp_graph {\n";
        out += "  graph [rankdir=TB, fontname=\"sans-serif\", fontsize=11, labelloc=t];\n";
        out += "  node [shape=box, style=\"rounded,filled\", fontname=\"sans-serif\", "
               "fontsize=9, fillcolor=\"#ffffff\", color=\"#666666\"];\n";
        out += "  edge [fontname=\"sans-serif\", fontsize=8, color=\"#888888\"];\n";
        out += "  label=\"" + DotLabel(graph.title) + "\";\n";
        std::size_t index = 0;
        for (const Message& message : graph.messages) {
            const std::string id = "m" + std::to_string(index);
            const std::string suffix =
                message.count > 1 ? " x" + std::to_string(message.count) : std::string();
            out += "  " + id + " [label=\"#" + std::to_string(index + 1) + "\\n" +
                   DotLabel(message.from + " -> " + message.to) + "\\n" +
                   DotLabel(message.label + suffix) + "\"";
            if (message.dashed) {
                out += ", style=\"rounded,filled,dashed\"";
            }
            out += "];\n";
            if (index != 0) {
                out += "  m" + std::to_string(index - 1) + " -> " + id +
                       " [color=\"#bbbbbb\", arrowsize=0.6];\n";
            }
            ++index;
        }
        if (!graph.notes.empty()) {
            std::vector<std::string> notes;
            for (const std::string& note : graph.notes) {
                notes.push_back("• " + note);
            }
            out += "  note [shape=note, style=\"filled\", fillcolor=\"#fff8e1\", "
                   "color=\"#b06000\", label=\"" +
                   DotLabel(Join(notes, "\n")) + "\"];\n";
        }
        out += "}\n";
        return out;
    }

    out += "digraph go2cpp_graph {\n";
    out += "  graph [rankdir=LR, fontname=\"sans-serif\", fontsize=11, labelloc=t];\n";
    out += "  node [shape=box, style=\"rounded,filled\", fontname=\"sans-serif\", "
           "fontsize=10, fillcolor=\"#ffffff\", color=\"#666666\"];\n";
    out += "  edge [fontname=\"sans-serif\", fontsize=9, color=\"#666666\"];\n";
    std::string title = graph.title;
    if (!graph.notes.empty()) {
        std::vector<std::string> notes;
        for (const std::string& note : graph.notes) {
            notes.push_back("• " + note);
        }
        title += "\n" + Join(notes, "\n");
    }
    out += "  label=\"" + DotLabel(title) + "\";\n";

    std::map<std::string, std::string> ids;
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        ids[graph.nodes[index].id] = "n" + std::to_string(index);
    }
    for (const Node& node : graph.nodes) {
        const auto found = ids.find(node.id);
        if (found == ids.end()) {
            continue;
        }
        out += "  " + found->second + " [label=\"" + DotLabel(node.label) + "\"";
        if (node.kind == "g") {
            out += ", shape=ellipse";
        } else if (node.kind == "processor") {
            out += ", shape=hexagon";
        } else if (node.kind == "queue") {
            out += ", shape=cylinder";
        } else if (node.kind == "machine") {
            out += ", shape=box3d";
        } else if (node.kind == "state") {
            out += ", shape=box";
        }
        if (node.deadlock) {
            out += ", fillcolor=\"#ffe3e3\", color=\"#c00000\", penwidth=2";
        } else if (node.style == "state_failed") {
            out += ", fillcolor=\"#ffe3e3\", color=\"#c00000\"";
        } else if (node.style == "state_running" || node.style == "machine") {
            out += ", fillcolor=\"#e6f4ea\", color=\"#137333\"";
        } else if (node.style == "state_parked" || node.style == "machine_parked") {
            out += ", fillcolor=\"#fef7e0\", color=\"#b06000\"";
        } else if (node.style == "state_ready" || node.style == "processor") {
            out += ", fillcolor=\"#e0f7fa\", color=\"#00838f\"";
        } else if (node.style == "state_completed") {
            out += ", fillcolor=\"#f1f3f4\", color=\"#5f6368\"";
        } else if (node.style == "queue") {
            out += ", fillcolor=\"#f3e8fd\", color=\"#7b1fa2\"";
        } else if (node.style == "g_blocked") {
            out += ", fillcolor=\"#ffe3e3\", color=\"#c00000\"";
        } else if (node.style == "g") {
            out += ", fillcolor=\"#e8f0fe\", color=\"#1a73e8\"";
        }
        out += "];\n";
    }
    for (const Edge& edge : graph.edges) {
        const auto from = ids.find(edge.from);
        const auto to = ids.find(edge.to);
        if (from == ids.end() || to == ids.end()) {
            continue;
        }
        out += "  " + from->second + " -> " + to->second + " [label=\"" +
               DotLabel(edge.label) + "\"";
        if (edge.deadlock) {
            out += ", color=\"#c00000\", fontcolor=\"#c00000\", penwidth=2";
        }
        out += "];\n";
    }
    out += "}\n";
    return out;
}

std::string RenderJson(const Graph& graph) {
    json root = json::object();
    root["schema"] = "go2cpp.graph." + graph.type + "/1";
    root["version"] = 1;
    root["type"] = graph.type;
    root["title"] = graph.title;
    root["ok"] = graph.ok;
    root["notes"] = graph.notes;
    root["source"] = graph.meta;
    root["deadlock_cycles"] = graph.cycles;
    json nodes = json::array();
    for (const Node& node : graph.nodes) {
        nodes.push_back({{"id", node.id},
                         {"label", node.label},
                         {"kind", node.kind},
                         {"style", node.style},
                         {"deadlock", node.deadlock}});
    }
    json edges = json::array();
    for (const Edge& edge : graph.edges) {
        edges.push_back({{"from", edge.from},
                         {"to", edge.to},
                         {"label", edge.label},
                         {"kind", edge.kind},
                         {"deadlock", edge.deadlock}});
    }
    root["nodes"] = std::move(nodes);
    root["edges"] = std::move(edges);
    if (graph.type == "sequence") {
        json participants = json::array();
        for (const Participant& participant : graph.participants) {
            participants.push_back({{"id", participant.id}, {"label", participant.label}});
        }
        json messages = json::array();
        for (const Message& message : graph.messages) {
            messages.push_back({{"from", message.from},
                                {"to", message.to},
                                {"label", message.label},
                                {"ts_ns", message.ts_ns},
                                {"count", message.count},
                                {"dashed", message.dashed}});
        }
        root["sequence"] = {{"participants", std::move(participants)},
                            {"messages", std::move(messages)}};
    }
    return root.dump(2) + "\n";
}

std::string RenderGraph(const Graph& graph, const std::string& format) {
    if (format == "dot") {
        return RenderDot(graph);
    }
    if (format == "json") {
        return RenderJson(graph);
    }
    return RenderMermaid(graph);
}

}  // namespace

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------

int RunGraph(const std::vector<std::string>& args) {
    const ParsedArgs parsed = ParseArgs(
        args,
        {
            {"--pid", "PID", "目标进程 pid（与 --replay 二选一）"},
            {"--replay", "FILE", "JSON dump / view JSON / # go2cpp-trace 文件"},
            {"--type", "TYPE", "wait-for|resource|fiber-state|gmp|sequence", true},
            {"--format", "FMT", "mermaid（默认）| dot | json"},
            {"--out", "FILE", "输出文件；默认 -（stdout）"},
            {"--debug-socket", "PATH", "覆盖 DebugServer socket 路径"},
            {"--doctor-socket", "PATH", "覆盖 Doctor socket 路径"},
            {"--timeout-ms", "N", "连接 + 收发的总超时，默认 5000"},
            {"--max-g", "N", "最多显示多少个 G，默认 16"},
            {"--max-resources", "N", "最多显示多少个资源，默认 48"},
            {"--max-events", "N", "sequence 最多多少条消息，默认 400"},
        },
        0, 0);
    if (parsed.help_requested) {
        PrintGraphHelp();
        return 0;
    }

    Options options;
    options.type = parsed.Get("--type");
    options.format = parsed.GetOr("--format", "mermaid");
    options.out = parsed.GetOr("--out", "-");
    options.debug_socket = parsed.Get("--debug-socket");
    options.doctor_socket = parsed.Get("--doctor-socket");
    options.timeout_ms =
        ParseIntOption(parsed.GetOr("--timeout-ms", "5000"), "--timeout-ms", 1, kMaxTimeoutMs);
    options.max_g = static_cast<std::size_t>(
        ParseIntOption(parsed.GetOr("--max-g", "16"), "--max-g", 1, 4096));
    options.max_resources = static_cast<std::size_t>(
        ParseIntOption(parsed.GetOr("--max-resources", "48"), "--max-resources", 1, 8192));
    options.max_events = static_cast<std::size_t>(
        ParseIntOption(parsed.GetOr("--max-events", "400"), "--max-events", 1, 100000));

    static const std::vector<std::string> kTypes = {"wait-for", "resource", "fiber-state",
                                                    "gmp", "sequence"};
    if (std::find(kTypes.begin(), kTypes.end(), options.type) == kTypes.end()) {
        throw CliError(ExitCode::kUsage,
                       "选项 '--type' 只接受 wait-for|resource|fiber-state|gmp|sequence，实际 '" +
                           options.type + "'");
    }
    if (options.format != "mermaid" && options.format != "dot" && options.format != "json") {
        throw CliError(ExitCode::kUsage,
                       "选项 '--format' 只接受 mermaid|dot|json，实际 '" + options.format + "'");
    }

    const bool has_pid = parsed.Has("--pid");
    const bool has_replay = parsed.Has("--replay");
    if (has_pid == has_replay) {
        throw CliError(ExitCode::kUsage,
                       "graph 需要 --pid <pid> 或 --replay <file>（只能二选一）");
    }
    if (has_pid) {
        options.pid = ParseIntOption(parsed.Get("--pid"), "--pid", 1, 2147483647);
        options.has_pid = true;
    } else {
        options.replay_path = parsed.Get("--replay");
        if (options.replay_path.empty()) {
            throw CliError(ExitCode::kUsage, "选项 '--replay' 需要一个文件路径");
        }
    }

    Collected collected;
    if (has_pid) {
        collected = CollectLive(options);
    } else {
        collected = CollectFile(options);
    }

    Graph graph;
    if (options.type == "wait-for") {
        graph = BuildWaitGraph(collected, options, true);
    } else if (options.type == "resource") {
        graph = BuildWaitGraph(collected, options, false);
    } else if (options.type == "fiber-state") {
        graph = BuildFiberStateGraph(collected, options);
    } else if (options.type == "gmp") {
        graph = BuildGmpGraph(collected, options);
    } else {
        graph = BuildSequenceGraph(collected, options);
    }

    if (!graph.ok) {
        for (const std::string& note : graph.notes) {
            std::fprintf(stderr, "go2cpp-cli: graph: %s\n", note.c_str());
        }
    }

    const std::string content = RenderGraph(graph, options.format);
    if (options.out.empty() || options.out == "-") {
        std::fwrite(content.data(), 1, content.size(), stdout);
        if (!content.empty() && content.back() != '\n') {
            std::fputc('\n', stdout);
        }
        std::fflush(stdout);
    } else {
        WriteTextFileAtomic(options.out, content);
        std::printf("已写入 %s（%s）\n", options.out.c_str(),
                    FormatBytes(content.size()).c_str());
        std::fflush(stdout);
    }
    return graph.ok ? 0 : static_cast<int>(ExitCode::kUnavailable);
}

}  // namespace go2cpp::cli
