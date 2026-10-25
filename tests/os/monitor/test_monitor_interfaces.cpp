// go2cpp::os::monitor 接口化（interfaces.hpp）测试。
//
// 覆盖：
//   1. MemorySnapshotSink：容量淘汰、统计、按时间顺序取回；
//   2. FileSnapshotSink：JSON Lines 落盘、失败路径（目录不存在）；
//   3. MemorySnapshotStore：容量、Recent(n) 边界、Clear；
//   4. Manager 注入 SnapshotStore：容量由 store 决定，查询走注入实现；
//   5. Aggregator 依赖抽象 SnapshotSink：PublishNow 同步发布、后台定时发布、
//      SetSink 替换/关闭；
//   6. Pusher 只依赖 SnapshotSource 并实现 SnapshotSink：固定数据源 → HTTP →
//      Manager 全链路，以及 HttpSnapshotSink 失败统计；
//   7. 未注入 sink/store 时保持原行为（空 sink 不发布、默认内存存储）。

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/os/monitor/aggregator.hpp"
#include "go2cpp/os/monitor/interfaces.hpp"
#include "go2cpp/os/monitor/manager.hpp"
#include "go2cpp/os/monitor/pusher.hpp"
#include "test_support.hpp"

namespace {

using go2cpp::os::monitor::Aggregator;
using go2cpp::os::monitor::AggregatorConfig;
using go2cpp::os::monitor::FileSnapshotSink;
using go2cpp::os::monitor::HttpSinkConfig;
using go2cpp::os::monitor::HttpSnapshotSink;
using go2cpp::os::monitor::Manager;
using go2cpp::os::monitor::ManagerConfig;
using go2cpp::os::monitor::MemorySnapshotSink;
using go2cpp::os::monitor::MemorySnapshotStore;
using go2cpp::os::monitor::Pusher;
using go2cpp::os::monitor::PusherConfig;
using go2cpp::os::monitor::PusherStats;
using go2cpp::os::monitor::Snapshot;
using go2cpp::os::monitor::SnapshotFromJson;
using go2cpp::os::monitor::SnapshotSink;
using go2cpp::os::monitor::SnapshotSource;
using go2cpp::os::monitor::SnapshotStore;

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// 不读 /proc 的最小快照，字段足够走 JSON 与健康评分。
Snapshot MakeSnapshot(const std::string& host, double usage_percent) {
    Snapshot snapshot;
    snapshot.host = host;
    snapshot.timestamp_ms = NowMs();
    snapshot.host_info.hostname = host;
    snapshot.host_info.kernel = "test";
    snapshot.host_info.arch = "x86_64";
    go2cpp::os::monitor::CpuCore core;
    core.name = "cpu0";
    core.usage_percent = usage_percent;
    snapshot.cpus.push_back(core);
    snapshot.mem.total_kb = 1024 * 1024;
    snapshot.mem.available_kb = 768 * 1024;
    snapshot.mem.used_percent = 25.0;
    snapshot.load.load1 = 1.5;
    go2cpp::os::monitor::DiskStat disk;
    disk.name = "sda";
    disk.util_percent = 12.5;
    snapshot.disks.push_back(disk);
    go2cpp::os::monitor::NetDevStat net;
    net.name = "eth0";
    net.rcv_rate = 1024.0;
    net.snd_rate = 2048.0;
    snapshot.nets.push_back(net);
    snapshot.ebpf_active = false;
    snapshot.ebpf_reason = "test: no eBPF";
    return snapshot;
}

/// 固定数据源：证明 Pusher 只依赖 SnapshotSource，不需要真实采集器。
class FixedSource final : public SnapshotSource {
public:
    explicit FixedSource(Snapshot snapshot) : snapshot_(std::move(snapshot)) {}
    Snapshot Latest() const override { return snapshot_; }

private:
    Snapshot snapshot_;
};

void TestMemorySink() {
    go2cpp_tests::announce("memory_snapshot_sink");
    MemorySnapshotSink sink(2);
    GO2CPP_CHECK(std::string(sink.kind()) == "memory");
    GO2CPP_CHECK(sink.capacity() == 2);
    GO2CPP_CHECK(sink.Push(MakeSnapshot("sink-a", 10.0)));
    GO2CPP_CHECK(sink.Push(MakeSnapshot("sink-b", 20.0)));
    GO2CPP_CHECK(sink.Push(MakeSnapshot("sink-c", 30.0)));
    GO2CPP_CHECK(sink.size() == 2);
    GO2CPP_CHECK(sink.pushed() == 3);
    GO2CPP_CHECK(sink.Last().host == "sink-c");
    const auto snapshots = sink.Snapshots();
    GO2CPP_CHECK(snapshots.size() == 2);
    if (snapshots.size() == 2) {
        GO2CPP_CHECK(snapshots[0].host == "sink-b");
        GO2CPP_CHECK(snapshots[1].host == "sink-c");
    }
    sink.Clear();
    GO2CPP_CHECK(sink.size() == 0);
    GO2CPP_CHECK(sink.pushed() == 3);  // 统计累计，不随 Clear 重置
}

void TestFileSink() {
    go2cpp_tests::announce("file_snapshot_sink");
    char path[] = "/tmp/go2cpp_monitor_sink_XXXXXX";
    const int fd = ::mkstemp(path);
    GO2CPP_CHECK(fd >= 0);
    if (fd < 0) {
        return;
    }
    ::close(fd);

    {
        FileSnapshotSink sink(path, true);
        GO2CPP_CHECK(std::string(sink.kind()) == "file");
        GO2CPP_CHECK(sink.path() == path);
        GO2CPP_CHECK(sink.Push(MakeSnapshot("file-a", 11.0)));
        GO2CPP_CHECK(sink.Push(MakeSnapshot("file-b", 22.0)));
        GO2CPP_CHECK(sink.Stats().pushed == 2);
        GO2CPP_CHECK(sink.Stats().failed == 0);
    }

    std::ifstream input(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    GO2CPP_CHECK(lines.size() == 2);
    if (lines.size() == 2) {
        Snapshot first;
        std::string error;
        GO2CPP_CHECK(SnapshotFromJson(lines[0], first, &error));
        GO2CPP_CHECK(error.empty());
        GO2CPP_CHECK(first.host == "file-a");
        GO2CPP_CHECK(first.cpus.size() == 1);
        Snapshot second;
        GO2CPP_CHECK(SnapshotFromJson(lines[1], second, &error));
        GO2CPP_CHECK(second.host == "file-b");
    }
    ::unlink(path);

    // 打不开的路径：不抛异常，Push 返回 false 并记录 last_error。
    FileSnapshotSink broken(
        "/nonexistent-go2cpp-monitor-dir-9f3c/sink.jsonl", true);
    GO2CPP_CHECK(!broken.Push(MakeSnapshot("file-broken", 1.0)));
    GO2CPP_CHECK(broken.Stats().failed == 1);
    GO2CPP_CHECK(!broken.Stats().last_error.empty());
}

void TestMemoryStore() {
    go2cpp_tests::announce("memory_snapshot_store");
    MemorySnapshotStore store(2);
    GO2CPP_CHECK(store.capacity() == 2);
    GO2CPP_CHECK(store.Append(MakeSnapshot("store-a", 1.0)));
    GO2CPP_CHECK(store.Append(MakeSnapshot("store-b", 2.0)));
    GO2CPP_CHECK(store.Append(MakeSnapshot("store-c", 3.0)));
    GO2CPP_CHECK(store.size() == 2);
    GO2CPP_CHECK(store.Recent(0).empty());
    GO2CPP_CHECK(store.Recent(10).size() == 2);
    const auto recent = store.Recent(10);
    if (recent.size() == 2) {
        GO2CPP_CHECK(recent[0].host == "store-b");
        GO2CPP_CHECK(recent[1].host == "store-c");
    }
    const auto last = store.Recent(1);
    GO2CPP_CHECK(last.size() == 1);
    if (!last.empty()) {
        GO2CPP_CHECK(last.front().host == "store-c");
    }
    store.Clear();
    GO2CPP_CHECK(store.size() == 0);
    GO2CPP_CHECK(store.Recent(5).empty());
}

void TestManagerWithInjectedStore() {
    go2cpp_tests::announce("manager_injected_store");
    auto store = std::make_shared<MemorySnapshotStore>(2);
    ManagerConfig config;
    config.listen_address = "127.0.0.1";
    config.port = 0;
    config.capacity = 1;  // 注入 store 后容量应由 store 决定
    Manager manager(config, store);

    GO2CPP_CHECK(manager.Ingest(MakeSnapshot("manager-a", 10.0)));
    GO2CPP_CHECK(manager.Ingest(MakeSnapshot("manager-b", 20.0)));
    GO2CPP_CHECK(manager.Ingest(MakeSnapshot("manager-c", 30.0)));
    GO2CPP_CHECK(manager.received() == 3);
    GO2CPP_CHECK(manager.stored() == 2);
    GO2CPP_CHECK(store->size() == 2);
    const auto latest = manager.Latest(10);
    GO2CPP_CHECK(latest.size() == 2);
    if (latest.size() == 2) {
        GO2CPP_CHECK(latest[0].host == "manager-b");
        GO2CPP_CHECK(latest[1].host == "manager-c");
    }
    const auto summaries = manager.Summaries();
    GO2CPP_CHECK(summaries.size() == 2);
    const auto kernels = manager.KernelSummaries();
    GO2CPP_CHECK(kernels.size() == 2);
    GO2CPP_CHECK(manager.Latest(0).empty());
}

void TestAggregatorPublishNow() {
    go2cpp_tests::announce("aggregator_publish_now");
    AggregatorConfig config;
    config.interval = std::chrono::milliseconds(50);
    config.enable_ebpf = false;

    auto sink = std::make_shared<MemorySnapshotSink>(8);
    Aggregator aggregator(config, sink);
    GO2CPP_CHECK(aggregator.Sink() == sink);
    // 未 Start：同步采集并直接发布，确定性最高。
    GO2CPP_CHECK(aggregator.PublishNow());
    GO2CPP_CHECK(sink->size() == 1);
    const Snapshot published = sink->Last();
    GO2CPP_CHECK(!published.host.empty());
    GO2CPP_CHECK(published.timestamp_ms > 0);
    GO2CPP_CHECK(!published.cpus.empty());
    GO2CPP_CHECK(published.mem.total_kb > 0);
    GO2CPP_CHECK(!published.disks.empty());
    GO2CPP_CHECK(!published.nets.empty());

    Aggregator no_sink(config);
    GO2CPP_CHECK(!no_sink.PublishNow());
    GO2CPP_CHECK(!no_sink.PublishLatest());
}

void TestAggregatorBackgroundSink() {
    go2cpp_tests::announce("aggregator_background_sink");
    AggregatorConfig config;
    config.interval = std::chrono::milliseconds(30);
    config.enable_ebpf = false;
    config.workers = 2;

    auto sink = std::make_shared<MemorySnapshotSink>(64);
    Aggregator aggregator(config, sink);
    GO2CPP_CHECK(aggregator.Start());
    GO2CPP_REQUIRE_EVENTUALLY(sink->size() >= 2, std::chrono::seconds(10));
    GO2CPP_CHECK(sink->pushed() >= 2);

    const Snapshot latest = sink->Last();
    GO2CPP_CHECK(!latest.host.empty());
    GO2CPP_CHECK(!latest.cpus.empty());

    // 停止后不再发布：后台定时器已取消，计数稳定。
    aggregator.Stop();
    GO2CPP_CHECK(!aggregator.running());
    const std::size_t frozen = sink->size();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    GO2CPP_CHECK(sink->size() == frozen);

    // 运行中替换/关闭 sink。
    aggregator.SetSink(nullptr);
    GO2CPP_CHECK(aggregator.Sink() == nullptr);
    GO2CPP_CHECK(!aggregator.PublishLatest());
}

void TestPusherAsSink() {
    go2cpp_tests::announce("pusher_as_sink");
    ManagerConfig manager_config;
    manager_config.listen_address = "127.0.0.1";
    manager_config.port = 0;
    manager_config.capacity = 8;
    Manager manager(manager_config);
    GO2CPP_CHECK(manager.Start());
    const std::string base_url =
        "http://127.0.0.1:" + std::to_string(manager.port());

    FixedSource source(MakeSnapshot("pusher-host", 42.0));
    PusherConfig pusher_config;
    pusher_config.manager_url = base_url + "/api/push";
    pusher_config.interval = std::chrono::milliseconds(50);
    pusher_config.timeout = std::chrono::milliseconds(2000);
    Pusher pusher(pusher_config, source);
    GO2CPP_CHECK(std::string(pusher.kind()) == "pusher");

    // 通过 SnapshotSink 接口调用，不依赖 Pusher 的具体类型。
    SnapshotSink& sink = pusher;
    GO2CPP_CHECK(sink.Push(MakeSnapshot("pusher-via-sink", 11.0)));
    GO2CPP_CHECK(manager.received() == 1);
    GO2CPP_CHECK(pusher.PushOnce());
    GO2CPP_CHECK(manager.received() == 2);
    const PusherStats stats = pusher.Stats();
    GO2CPP_CHECK(stats.pushed == 2);
    GO2CPP_CHECK(stats.failed == 0);

    // HttpSnapshotSink 单独使用：同样推送到同一个 Manager。
    HttpSnapshotSink http_sink(
        HttpSinkConfig{base_url + "/api/push", std::chrono::milliseconds(2000)});
    GO2CPP_CHECK(http_sink.Push(MakeSnapshot("http-sink", 12.0)));
    GO2CPP_CHECK(http_sink.Stats().pushed == 1);
    GO2CPP_CHECK(manager.received() == 3);

    // 失败路径：Manager 停止后推送返回 false，统计 failed/last_error。
    manager.Stop();
    GO2CPP_CHECK(!http_sink.Push(MakeSnapshot("http-sink-fail", 13.0)));
    GO2CPP_CHECK(http_sink.Stats().failed == 1);
    GO2CPP_CHECK(!http_sink.Stats().last_error.empty());

    GO2CPP_CHECK(!pusher.Push(MakeSnapshot("pusher-fail", 14.0)));
    const PusherStats failed = pusher.Stats();
    GO2CPP_CHECK(failed.failed == 1);
    GO2CPP_CHECK(!failed.last_error.empty());
}

}  // namespace

int main() {
    TestMemorySink();
    TestFileSink();
    TestMemoryStore();
    TestManagerWithInjectedStore();
    TestAggregatorPublishNow();
    TestAggregatorBackgroundSink();
    TestPusherAsSink();

    const int failures =
        go2cpp_tests::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::fprintf(stderr, "[test] failures=%d\n", failures);
        return 1;
    }
    std::printf("[test] all monitor interface checks passed\n");
    return 0;
}
