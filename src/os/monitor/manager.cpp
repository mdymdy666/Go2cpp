#include "go2cpp/os/monitor/manager.hpp"

#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <limits>
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
                break;  // 监听 socket 已关闭
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
                const Json body{{"ok", false}, {"error", error}};
                return JsonResponse(http::Status::BadRequest, body.dump());
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
            Json body = Json::array();
            for (const auto& snapshot : snapshots) {
                body.push_back(snapshot);
            }
            return JsonResponse(http::Status::OK, body.dump());
        });
    impl_->server->route(
        http::Method::Get, "/api/query/stats",
        [this](const http::Request& request) {
            (void)request;
            const auto summaries = impl_->Summaries();
            Json body = Json::array();
            for (const auto& summary : summaries) {
                body.push_back(Json{{"host", summary.host},
                                    {"timestamp_ms", summary.timestamp_ms},
                                    {"score", summary.score},
                                    {"cpu_percent", summary.cpu_percent},
                                    {"mem_used_percent", summary.mem_used_percent},
                                    {"disk_util_percent",
                                     summary.disk_util_percent},
                                    {"load1", summary.load1},
                                    {"net_rcv_rate", summary.net_rcv_rate},
                                    {"net_snd_rate", summary.net_snd_rate},
                                    {"ebpf_active", summary.ebpf_active}});
            }
            return JsonResponse(http::Status::OK, body.dump());
        });
    impl_->server->route(
        http::Method::Get, "/api/query/kernel",
        [this](const http::Request& request) {
            (void)request;
            const auto summaries = impl_->KernelSummaries();
            Json body = Json::array();
            for (const auto& summary : summaries) {
                Json syscall_top = Json::array();
                for (const auto& item : summary.syscalls.top) {
                    syscall_top.push_back(Json{{"name", item.name},
                                               {"id", item.id},
                                               {"count", item.count},
                                               {"p50_us", item.p50_us},
                                               {"p99_us", item.p99_us},
                                               {"max_us", item.max_us}});
                }
                Json sched_top = Json::array();
                for (const auto& item : summary.sched.top) {
                    sched_top.push_back(Json{{"pid", item.pid},
                                             {"comm", item.comm},
                                             {"wait_count", item.wait_count},
                                             {"wait_max_us", item.wait_max_us}});
                }
                Json futex_top = Json::array();
                for (const auto& item : summary.futex.top) {
                    futex_top.push_back(Json{{"pid", item.pid},
                                             {"comm", item.comm},
                                             {"wait_count", item.wait_count},
                                             {"wake_count", item.wake_count},
                                             {"wait_max_us",
                                              item.wait_max_us}});
                }
                Json tcp_top = Json::array();
                for (const auto& item : summary.tcp.top) {
                    tcp_top.push_back(Json{{"local", item.local},
                                           {"remote", item.remote},
                                           {"retransmits", item.retransmits},
                                           {"rtt_samples", item.rtt_samples},
                                           {"rtt_p99_us", item.rtt_p99_us}});
                }
                Json fileio_top = Json::array();
                for (const auto& item : summary.fileio.top) {
                    fileio_top.push_back(Json{{"pid", item.pid},
                                              {"comm", item.comm},
                                              {"read_count", item.read_count},
                                              {"write_count", item.write_count},
                                              {"read_max_us", item.read_max_us},
                                              {"write_max_us",
                                               item.write_max_us}});
                }
                const double syscall_p50 = KernelHistPercentile(

                    summary.syscalls.hist, summary.syscalls.total, 0.50);
                const double syscall_p99 = KernelHistPercentile(
                    summary.syscalls.hist, summary.syscalls.total, 0.99);
                body.push_back(Json{
                    {"host", summary.host},
                    {"timestamp_ms", summary.timestamp_ms},
                    {"syscall",
                     Json{{"active", summary.syscalls.active},
                          {"total", summary.syscalls.total},
                          {"p50_us", syscall_p50},
                          {"p99_us", syscall_p99},
                          {"top", syscall_top}}},
                    {"sched",
                     Json{{"active", summary.sched.active},
                          {"switches", summary.sched.switches},
                          {"wait_count", summary.sched.wait_count},
                          {"wait_p50_us", summary.sched.wait_p50_us},
                          {"wait_p99_us", summary.sched.wait_p99_us},
                          {"wait_max_us", summary.sched.wait_max_us},
                          {"top", sched_top}}},
                    {"futex",
                     Json{{"active", summary.futex.active},
                          {"wait_count", summary.futex.wait_count},
                          {"wake_count", summary.futex.wake_count},
                          {"wait_wake_ratio",
                           summary.futex.wait_wake_ratio},
                          {"wait_p50_us", summary.futex.wait_p50_us},
                          {"wait_p99_us", summary.futex.wait_p99_us},
                          {"wait_max_us", summary.futex.wait_max_us},
                          {"top", futex_top}}},
                    {"tcp",
                     Json{{"active", summary.tcp.active},
                          {"reason", summary.tcp.reason},
                          {"rtt_source", summary.tcp.rtt_source},
                          {"retransmits", summary.tcp.retransmits},
                          {"retransmits_per_sec",
                           summary.tcp.retransmits_per_sec},
                          {"rtt_samples", summary.tcp.rtt_samples},
                          {"rtt_p50_us", summary.tcp.rtt_p50_us},
                          {"rtt_p99_us", summary.tcp.rtt_p99_us},
                          {"rtt_avg_us", summary.tcp.rtt_avg_us},
                          {"rtt_max_us", summary.tcp.rtt_max_us},
                          {"rtt_weighting", summary.tcp.rtt_weighting},
                          {"conn_rtt_samples",
                           summary.tcp.conn_rtt_samples},
                          {"conn_rtt_p50_us",
                           summary.tcp.conn_rtt_p50_us},
                          {"conn_rtt_p99_us",
                           summary.tcp.conn_rtt_p99_us},
                          {"top", tcp_top}}},
                    {"fileio",
                     Json{{"active", summary.fileio.active},
                          {"reason", summary.fileio.reason},
                          {"read_count", summary.fileio.read_count},
                          {"write_count", summary.fileio.write_count},
                          {"read_p50_us", summary.fileio.read_p50_us},
                          {"read_p99_us", summary.fileio.read_p99_us},
                          {"write_p50_us",
                           summary.fileio.write_p50_us},
                          {"write_p99_us",
                           summary.fileio.write_p99_us},
                          {"read_iops", summary.fileio.read_iops},
                          {"write_iops", summary.fileio.write_iops},
                          {"readv_count", summary.fileio.readv_count},
                          {"writev_count", summary.fileio.writev_count},
                          {"uring_read_count",
                           summary.fileio.uring_read_count},
                          {"uring_write_count",
                           summary.fileio.uring_write_count},
                          {"uring_read_pending",
                           summary.fileio.uring_read_pending},
                          {"uring_write_pending",
                           summary.fileio.uring_write_pending},
                          {"coverage", summary.fileio.coverage},
                          {"coverage_gap", summary.fileio.coverage_gap},
                          {"top", fileio_top}}}});
            }
            return JsonResponse(http::Status::OK, body.dump());
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
            Json body{{"status", "ok"},
                      {"received", received},
                      {"rejected", rejected},
                      {"stored", impl_->store->size()}};
            return JsonResponse(http::Status::OK, body.dump());
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
