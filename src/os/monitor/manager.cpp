#include "go2cpp/os/monitor/manager.hpp"

#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <type_traits>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "go2cpp/network/address.hpp"
#include "go2cpp/network/http.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/runtime.hpp"

namespace go2cpp::os::monitor {
namespace {

/// 把标量写进当前对象/数组（与 monitor.cpp 的 Put 同构；各 TU 独立）。
template <typename T>
void Put(go2cpp::json::JsonBuilder& builder, const char* key, const T& value) {
    builder.Key(key);
    if constexpr (std::is_same_v<T, bool>) {
        builder.Bool(value);
    } else if constexpr (std::is_same_v<T, std::string>) {
        builder.String(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        builder.Double(value);
    } else if constexpr (std::is_signed_v<T>) {
        builder.Int(value);
    } else {
        builder.Uint(value);
    }
}

/// 构造一个 JSON 响应。
http::Response JsonResponse(http::Status status, std::string body) {
    http::Response response;
    response.status = status;
    response.header.set("Content-Type", "application/json; charset=utf-8");
    response.body = std::move(body);
    return response;
}

/// 从 query string 中取整数参数，缺省/非法时返回 fallback。
std::size_t QuerySize(const std::string& query, const char* name,
                      std::size_t fallback) {
    http::QueryValues values;
    if (!http::decode_query(query, values)) {
        return fallback;
    }
    const auto it = values.find(name);
    if (it == values.end() || it->second.empty()) {
        return fallback;
    }
    try {
        const long long parsed = std::stoll(it->second.front());
        if (parsed <= 0) {
            return fallback;
        }
        return static_cast<std::size_t>(parsed);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

struct Manager::Impl {
    Impl(ManagerConfig cfg, std::shared_ptr<SnapshotStore> injected)
        : config(std::move(cfg)),
          store(injected
                    ? std::move(injected)
                    : std::make_shared<MemorySnapshotStore>(config.capacity)) {}

    ~Impl() { StopInternal(); }

    void StopInternal() {
        // 调用方必须持有 lifecycle_mutex。
        if (!running.load()) {
            return;
        }
        running = false;
        if (listener) {
            (void)listener->close();
        }
        if (scheduler) {
            scheduler->shutdown();
            scheduler.reset();
        }
        listener.reset();
        server.reset();
    }

    bool Ingest(const Snapshot& snapshot) {
        if (!store->Append(snapshot)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex);
        ++received;
        return true;
    }

    /// 「全部快照」视图：SIZE_MAX 由 MemorySnapshotStore 解释为不限条数。
    std::vector<Snapshot> AllSnapshots() const {
        return store->Recent(std::numeric_limits<std::size_t>::max());
    }

    std::vector<Snapshot> Latest(std::size_t n) const {
        return store->Recent(n);
    }

    std::vector<HostSummary> Summaries() const {
        // SnapshotStore 自己保证线程安全；先取一份视图再聚合。
        const std::vector<Snapshot> snapshots = AllSnapshots();
        std::map<std::string, const Snapshot*> newest;
        for (const auto& snapshot : snapshots) {
            auto& slot = newest[snapshot.host.empty() ? "unknown" : snapshot.host];
            if (slot == nullptr || snapshot.timestamp_ms >= slot->timestamp_ms) {
                slot = &snapshot;
            }
        }
        std::vector<HostSummary> result;
        result.reserve(newest.size());
        for (const auto& entry : newest) {
            const Snapshot& snapshot = *entry.second;
            HostSummary summary;
            summary.host = entry.first;
            summary.timestamp_ms = snapshot.timestamp_ms;
            summary.score = HealthScore(snapshot);
            if (!snapshot.cpus.empty()) {
                double sum = 0.0;
                for (const auto& core : snapshot.cpus) {
                    sum += core.usage_percent;
                }
                summary.cpu_percent = sum / static_cast<double>(snapshot.cpus.size());
            }
            summary.mem_used_percent = snapshot.mem.used_percent;
            for (const auto& disk : snapshot.disks) {
                summary.disk_util_percent =
                    std::max(summary.disk_util_percent, disk.util_percent);
            }
            summary.load1 = snapshot.load.load1;
            for (const auto& net : snapshot.nets) {
                summary.net_rcv_rate += net.rcv_rate;
                summary.net_snd_rate += net.snd_rate;
            }
            summary.ebpf_active = snapshot.ebpf_active;
            result.push_back(std::move(summary));
        }
        std::sort(result.begin(), result.end(),
                  [](const HostSummary& left, const HostSummary& right) {
                      return left.score > right.score;
                  });
        return result;
    }

    std::vector<KernelSummary> KernelSummaries() const {
        const std::vector<Snapshot> snapshots = AllSnapshots();
        std::map<std::string, const Snapshot*> newest;
        for (const auto& snapshot : snapshots) {
            auto& slot = newest[snapshot.host.empty() ? "unknown" : snapshot.host];
            if (slot == nullptr || snapshot.timestamp_ms >= slot->timestamp_ms) {
                slot = &snapshot;
            }
        }
        std::vector<KernelSummary> result;
        result.reserve(newest.size());
        for (const auto& entry : newest) {
            const Snapshot& snapshot = *entry.second;
            KernelSummary summary;
            summary.host = entry.first;
            summary.timestamp_ms = snapshot.timestamp_ms;
            summary.syscalls = snapshot.syscalls;
            summary.sched = snapshot.sched;
            summary.futex = snapshot.futex;
            summary.tcp = snapshot.tcp;
            summary.fileio = snapshot.fileio;
            result.push_back(std::move(summary));
        }
        return result;
    }

    void AcceptLoop() {
        while (running.load()) {
            auto connection = listener->accept();
            if (!connection) {
                // accept 失败分两类：Stop() 关闭监听 socket（应退出）与瞬时错误
                // （EAGAIN/ECONNABORTED/EINTR/EMFILE，并发建连时常见）。旧实现
                // 一律 break，只要出现一次瞬时失败整个 accept 循环就永久退出，
                // 后续连接全部被拒（4x100 并发 push 实测丢 21 条）。
                if (!running.load()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            auto connection_ptr = std::move(connection);
            scheduler->spawn([this, connection_ptr]() {
                server->serve_connection(connection_ptr);
            });
        }
    }

    ManagerConfig config;
    std::unique_ptr<Scheduler> scheduler;
    net::Socket::Ptr listener;
    std::unique_ptr<http::Server> server;
    std::shared_ptr<Task> accept_task;
    std::atomic<bool> running{false};
    std::uint16_t bound_port{0};
    /// 串行化 Start/Stop，避免并发进入时 close 正在 bind/listen 的 listener。
    std::mutex lifecycle_mutex;

    /// 只保护 received/rejected；快照存储由 SnapshotStore 实现自行同步。
    mutable std::mutex mutex;
    std::shared_ptr<SnapshotStore> store;
    std::uint64_t received{0};
    std::uint64_t rejected{0};
};

Manager::Manager(ManagerConfig config, std::shared_ptr<SnapshotStore> store)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(store))),
      config_(impl_->config) {}

Manager::~Manager() = default;

bool Manager::Start() {
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
    if (impl_->running.load()) {
        return false;
    }
    impl_->running = true;
    impl_->listener = net::Socket::CreateTCP();
    if (!impl_->listener || !impl_->listener->valid()) {
        impl_->running = false;
        impl_->listener.reset();
        return false;
    }
    auto address =
        net::IPv4Address::Create(impl_->config.listen_address.c_str(),
                                 impl_->config.port);
    if (!address) {
        impl_->running = false;
        impl_->listener.reset();
        return false;
    }
    if (!impl_->listener->bind(*address)) {
        impl_->running = false;
        impl_->listener.reset();
        return false;
    }
    if (!impl_->listener->listen(128)) {
        impl_->running = false;
        impl_->listener.reset();
        return false;
    }
    // Address 基类没有 port()（只有 IPAddress 有）；listener 已绑定，直接
    // 用 getsockname 读内核分配的端口，也顺带验证 bind 真的生效。
    struct sockaddr_in local_addr {};
    socklen_t addr_length = sizeof(local_addr);
    if (::getsockname(impl_->listener->fd(),
                      reinterpret_cast<struct sockaddr*>(&local_addr),
                      &addr_length) == 0) {
        impl_->bound_port = ntohs(local_addr.sin_port);
    }

    impl_->server = std::make_unique<http::Server>(impl_->listener);
    impl_->server->route(
        http::Method::Post, "/api/push",
        [this](const http::Request& request) {
            Snapshot snapshot;
            std::string error;
            if (!SnapshotFromJson(request.body, snapshot, &error)) {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                ++impl_->rejected;
                // 用 JSON 序列化而不是字符串拼接：解析错误信息里可能带
                // 双引号/反斜杠/控制字符，拼接会产出非法 JSON，客户端连
                // 错误原因都解析不了。
                go2cpp::json::JsonBuilder body;
                body.BeginObject();
                Put(body, "ok", false);
                Put(body, "error", error);
                body.EndObject();
                return JsonResponse(http::Status::BadRequest, body.Take());
            }
            if (snapshot.host.empty()) {
                snapshot.host = "unknown";
            }
            if (snapshot.timestamp_ms == 0) {
                snapshot.timestamp_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
            }
            impl_->Ingest(snapshot);
            return JsonResponse(http::Status::OK, "{\"ok\":true}");
        });
    impl_->server->route(
        http::Method::Get, "/api/query/latest",
        [this](const http::Request& request) {
            const std::size_t n = QuerySize(request.query(), "n", 10);
            const auto snapshots = impl_->Latest(n);
            go2cpp::json::JsonBuilder body;
            body.BeginArray();
            for (const auto& snapshot : snapshots) {
                ToJson(body, snapshot);
            }
            body.EndArray();
            return JsonResponse(http::Status::OK, body.Take());
        });
    impl_->server->route(
        http::Method::Get, "/api/query/stats",
        [this](const http::Request& request) {
            (void)request;
            const auto summaries = impl_->Summaries();
            go2cpp::json::JsonBuilder body;
            body.BeginArray();
            for (const auto& summary : summaries) {
                body.BeginObject();
                Put(body, "host", summary.host);
                Put(body, "timestamp_ms", summary.timestamp_ms);
                Put(body, "score", summary.score);
                Put(body, "cpu_percent", summary.cpu_percent);
                Put(body, "mem_used_percent", summary.mem_used_percent);
                Put(body, "disk_util_percent", summary.disk_util_percent);
                Put(body, "load1", summary.load1);
                Put(body, "net_rcv_rate", summary.net_rcv_rate);
                Put(body, "net_snd_rate", summary.net_snd_rate);
                Put(body, "ebpf_active", summary.ebpf_active);
                body.EndObject();
            }
            body.EndArray();
            return JsonResponse(http::Status::OK, body.Take());
        });
    impl_->server->route(
        http::Method::Get, "/api/query/kernel",
        [this](const http::Request& request) {
            (void)request;
            const auto summaries = impl_->KernelSummaries();
            go2cpp::json::JsonBuilder body;
            body.BeginArray();
            for (const auto& summary : summaries) {
                body.BeginObject();
                Put(body, "host", summary.host);
                Put(body, "timestamp_ms", summary.timestamp_ms);

                body.Key("syscall");
                body.BeginObject();
                Put(body, "active", summary.syscalls.active);
                Put(body, "total", summary.syscalls.total);
                Put(body, "p50_us", KernelHistPercentile(
                                        summary.syscalls.hist,
                                        summary.syscalls.total, 0.50));
                Put(body, "p99_us", KernelHistPercentile(
                                        summary.syscalls.hist,
                                        summary.syscalls.total, 0.99));
                body.Key("top");
                body.BeginArray();
                for (const auto& item : summary.syscalls.top) {
                    body.BeginObject();
                    Put(body, "name", item.name);
                    Put(body, "id", item.id);
                    Put(body, "count", item.count);
                    Put(body, "p50_us", item.p50_us);
                    Put(body, "p99_us", item.p99_us);
                    Put(body, "max_us", item.max_us);
                    body.EndObject();
                }
                body.EndArray();
                body.EndObject();

                body.Key("sched");
                body.BeginObject();
                Put(body, "active", summary.sched.active);
                Put(body, "switches", summary.sched.switches);
                Put(body, "wait_count", summary.sched.wait_count);
                Put(body, "wait_p50_us", summary.sched.wait_p50_us);
                Put(body, "wait_p99_us", summary.sched.wait_p99_us);
                Put(body, "wait_max_us", summary.sched.wait_max_us);
                body.Key("top");
                body.BeginArray();
                for (const auto& item : summary.sched.top) {
                    body.BeginObject();
                    Put(body, "pid", item.pid);
                    Put(body, "comm", item.comm);
                    Put(body, "wait_count", item.wait_count);
                    Put(body, "wait_max_us", item.wait_max_us);
                    body.EndObject();
                }
                body.EndArray();
                body.EndObject();

                body.Key("futex");
                body.BeginObject();
                Put(body, "active", summary.futex.active);
                Put(body, "wait_count", summary.futex.wait_count);
                Put(body, "wake_count", summary.futex.wake_count);
                Put(body, "wait_wake_ratio", summary.futex.wait_wake_ratio);
                Put(body, "wait_p50_us", summary.futex.wait_p50_us);
                Put(body, "wait_p99_us", summary.futex.wait_p99_us);
                Put(body, "wait_max_us", summary.futex.wait_max_us);
                body.Key("top");
                body.BeginArray();
                for (const auto& item : summary.futex.top) {
                    body.BeginObject();
                    Put(body, "pid", item.pid);
                    Put(body, "comm", item.comm);
                    Put(body, "wait_count", item.wait_count);
                    Put(body, "wake_count", item.wake_count);
                    Put(body, "wait_max_us", item.wait_max_us);
                    body.EndObject();
                }
                body.EndArray();
                body.EndObject();

                body.Key("tcp");
                body.BeginObject();
                Put(body, "active", summary.tcp.active);
                Put(body, "reason", summary.tcp.reason);
                Put(body, "rtt_source", summary.tcp.rtt_source);
                Put(body, "retransmits", summary.tcp.retransmits);
                Put(body, "retransmits_per_sec",
                    summary.tcp.retransmits_per_sec);
                Put(body, "rtt_samples", summary.tcp.rtt_samples);
                Put(body, "rtt_p50_us", summary.tcp.rtt_p50_us);
                Put(body, "rtt_p99_us", summary.tcp.rtt_p99_us);
                Put(body, "rtt_avg_us", summary.tcp.rtt_avg_us);
                Put(body, "rtt_max_us", summary.tcp.rtt_max_us);
                Put(body, "rtt_weighting", summary.tcp.rtt_weighting);
                Put(body, "conn_rtt_samples", summary.tcp.conn_rtt_samples);
                Put(body, "conn_rtt_p50_us", summary.tcp.conn_rtt_p50_us);
                Put(body, "conn_rtt_p99_us", summary.tcp.conn_rtt_p99_us);
                body.Key("top");
                body.BeginArray();
                for (const auto& item : summary.tcp.top) {
                    body.BeginObject();
                    Put(body, "local", item.local);
                    Put(body, "remote", item.remote);
                    Put(body, "retransmits", item.retransmits);
                    Put(body, "rtt_samples", item.rtt_samples);
                    Put(body, "rtt_p99_us", item.rtt_p99_us);
                    body.EndObject();
                }
                body.EndArray();
                body.EndObject();

                body.Key("fileio");
                body.BeginObject();
                Put(body, "active", summary.fileio.active);
                Put(body, "reason", summary.fileio.reason);
                Put(body, "read_count", summary.fileio.read_count);
                Put(body, "write_count", summary.fileio.write_count);
                Put(body, "read_p50_us", summary.fileio.read_p50_us);
                Put(body, "read_p99_us", summary.fileio.read_p99_us);
                Put(body, "write_p50_us", summary.fileio.write_p50_us);
                Put(body, "write_p99_us", summary.fileio.write_p99_us);
                Put(body, "read_iops", summary.fileio.read_iops);
                Put(body, "write_iops", summary.fileio.write_iops);
                Put(body, "readv_count", summary.fileio.readv_count);
                Put(body, "writev_count", summary.fileio.writev_count);
                Put(body, "uring_read_count",
                    summary.fileio.uring_read_count);
                Put(body, "uring_write_count",
                    summary.fileio.uring_write_count);
                Put(body, "uring_read_pending",
                    summary.fileio.uring_read_pending);
                Put(body, "uring_write_pending",
                    summary.fileio.uring_write_pending);
                Put(body, "coverage", summary.fileio.coverage);
                Put(body, "coverage_gap", summary.fileio.coverage_gap);
                body.Key("top");
                body.BeginArray();
                for (const auto& item : summary.fileio.top) {
                    body.BeginObject();
                    Put(body, "pid", item.pid);
                    Put(body, "comm", item.comm);
                    Put(body, "read_count", item.read_count);
                    Put(body, "write_count", item.write_count);
                    Put(body, "read_max_us", item.read_max_us);
                    Put(body, "write_max_us", item.write_max_us);
                    body.EndObject();
                }
                body.EndArray();
                body.EndObject();

                body.EndObject();
            }
            body.EndArray();
            return JsonResponse(http::Status::OK, body.Take());
        });
    impl_->server->route(
        http::Method::Get, "/api/health",
        [this](const http::Request& request) {
            (void)request;
            std::uint64_t received = 0;
            std::uint64_t rejected = 0;
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                received = impl_->received;
                rejected = impl_->rejected;
            }
            go2cpp::json::JsonBuilder body;
            body.BeginObject();
            Put(body, "status", std::string("ok"));
            Put(body, "received", received);
            Put(body, "rejected", rejected);
            Put(body, "stored", impl_->store->size());
            body.EndObject();
            return JsonResponse(http::Status::OK, body.Take());
        });

    impl_->scheduler = std::make_unique<Scheduler>(3);
    impl_->scheduler->start();
    impl_->accept_task =
        impl_->scheduler->spawn([this]() { impl_->AcceptLoop(); });
    return true;
}

void Manager::Stop() {
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
    impl_->StopInternal();
}

bool Manager::running() const noexcept {
    return impl_->running.load();
}

std::uint16_t Manager::port() const {
    return impl_->bound_port;
}

bool Manager::Ingest(const Snapshot& snapshot) {
    return impl_->Ingest(snapshot);
}

std::vector<Snapshot> Manager::Latest(std::size_t n) const {
    return impl_->Latest(n);
}

std::vector<HostSummary> Manager::Summaries() const {
    return impl_->Summaries();
}

std::vector<KernelSummary> Manager::KernelSummaries() const {
    return impl_->KernelSummaries();
}

std::uint64_t Manager::received() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->received;
}

std::uint64_t Manager::rejected() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->rejected;
}

std::size_t Manager::stored() const {
    return impl_->store->size();
}

}  // namespace go2cpp::os::monitor
