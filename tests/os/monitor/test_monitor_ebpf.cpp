// eBPF 网络统计测试。
//
// 两种运行模式：
//   - 默认（ctest 普通用例）：以当前用户身份运行。非 root 时必须看到
//     available()==false、reason 非空、UpdateOnce 返回 kUnsupported，
//     即「优雅降级」路径；
//   - `--require-ebpf`（ctest 用 sudo 重跑）：要求 eBPF 真正可用，否则返回
//     77 让 ctest 记为 SKIP（环境无法提权时不应判失败）。
//
// root 模式下会挂 TC hook、产生真实 HTTP 流量，并验证内核 map 计数增长。
// Stop 之后还会用 `tc filter show` 核对 clsact 上没有残留的 bpf 过滤器：
// 这是 detach 必须带 attach 返回的 handle/priority 的回归测试。

#include <arpa/inet.h>
#include <dirent.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/network/http.hpp"
#include "go2cpp/os/monitor/net_ebpf_monitor.hpp"
#include "test_support.hpp"

namespace {

using go2cpp::os::monitor::NetEbpfMonitor;
using go2cpp::os::monitor::NetEbpfStat;
using go2cpp::os::monitor::Snapshot;
using go2cpp::os::monitor::Status;

/// 用一次真实 HTTP 请求制造 eth0 上的收发流量。
bool GenerateTraffic() {
    go2cpp::http::Client client(std::chrono::milliseconds(4000));
    const auto result = client.get("http://example.com/");
    return result.ok();
}

std::uint64_t TotalBytes(const Snapshot& snapshot) {
    std::uint64_t total = 0;
    for (const auto& net : snapshot.ebpf_nets) {
        total += net.rcv_bytes + net.snd_bytes;
    }
    return total;
}

/// 一张网卡两个方向上的 bpf TC filter 数量。
struct TcFilterCounts {
    int ingress{0};
    int egress{0};
};

/// 统计某个方向上 clsact 里的 bpf 过滤器数量；失败返回 -1。
///
/// 只看带 `handle 0x` 的详情行：`tc filter show` 对每个过滤器会先打一行
/// 摘要、再打一行详情，两行都含 "bpf"，只数详情行才不会被重复计数。
int CountBpfFilters(const std::string& interface, const char* direction) {
    char command[512] = {};
    std::snprintf(command, sizeof(command),
                  "/usr/sbin/tc filter show dev %s %s 2>/dev/null",
                  interface.c_str(), direction);
    FILE* pipe = ::popen(command, "r");
    if (pipe == nullptr) {
        return -1;
    }
    char line[512] = {};
    int count = 0;
    while (std::fgets(line, sizeof(line), pipe) != nullptr) {
        if (std::strstr(line, "handle 0x") != nullptr) {
            ++count;
        }
    }
    const int close_result = ::pclose(pipe);
    return close_result == 0 ? count : -1;
}

/// 记录 /sys/class/net 下所有非 loopback 网卡当前的 bpf TC filter 数量。
std::map<std::string, TcFilterCounts> SnapshotTcFilters() {
    std::map<std::string, TcFilterCounts> result;
    DIR* dir = ::opendir("/sys/class/net");
    if (dir == nullptr) {
        return result;
    }
    while (dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || name == "lo") {
            continue;
        }
        TcFilterCounts counts;
        counts.ingress = CountBpfFilters(name, "ingress");
        counts.egress = CountBpfFilters(name, "egress");
        result.emplace(name, counts);
    }
    ::closedir(dir);
    return result;
}

/// Stop 之后核对网卡上的 bpf filter 是否回到 attach 前的基线。
///
/// 修复前的实现用零值 opts 调 bpf_tc_detach()：handle/priority 为 0 时
/// libbpf 返回 -EINVAL，过滤器留在 clsact 上，BPF 程序与 map 被钉在内核里
/// 逐包执行。这个检查在修复前必然失败（每次 Stop 残留一对）。
void CheckTcFiltersRestored(
    const std::map<std::string, TcFilterCounts>& baseline,
    const std::vector<std::string>& interfaces, const char* phase) {
    for (const auto& name : interfaces) {
        const int ingress = CountBpfFilters(name, "ingress");
        const int egress = CountBpfFilters(name, "egress");
        const auto it = baseline.find(name);
        const int want_ingress = it == baseline.end() ? 0 : it->second.ingress;
        const int want_egress = it == baseline.end() ? 0 : it->second.egress;
        std::printf(
            "[ebpf] tc filters (%s) %s: ingress %d->%d egress %d->%d\n", phase,
            name.c_str(), want_ingress, ingress, want_egress, egress);
        GO2CPP_CHECK(ingress == want_ingress);
        GO2CPP_CHECK(egress == want_egress);
    }
}

// ---------------------------------------------------------------------------
// 接口热插拔（创建/删除 veth）
// ---------------------------------------------------------------------------

/// 找到 iproute2 的 ip 命令；不存在返回 nullptr。
const char* FindIpCommand() {
    static const char* const kCandidates[] = {"/usr/sbin/ip", "/sbin/ip",
                                              "/usr/bin/ip", "/bin/ip"};
    for (const char* candidate : kCandidates) {
        if (::access(candidate, X_OK) == 0) {
            return candidate;
        }
    }
    return nullptr;
}

bool ContainsInterface(const std::vector<std::string>& names,
                       const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool RunShell(const std::string& command) {
    const int status = std::system(command.c_str());
    return status == 0;
}

/// 用例名称带 pid 后缀，避免并行 ctest 或残留 veth 的名字冲突（IFNAMSIZ=16）。
std::string UniqueName(const char* prefix) {
    return std::string(prefix) + std::to_string(::getpid() % 100000);
}

/// 一对 veth 的 RAII 包装：析构时确保从内核里删除，断言失败/异常也不留垃圾。
class VethPair {
public:
    explicit VethPair(const char* ip)
        : ip_(ip), name_(UniqueName("vethh")), peer_(UniqueName("vethp")) {}

    ~VethPair() { Remove(); }

    VethPair(const VethPair&) = delete;
    VethPair& operator=(const VethPair&) = delete;

    bool Create() {
        if (!RunShell(ip_ + " link add " + name_ + " type veth peer name " +
                      peer_ + " >/dev/null 2>&1")) {
            return false;
        }
        created_ = true;
        // 两个方向都要 up：AF_PACKET 往 DOWN 的接口发送会得到 ENETDOWN。
        if (!RunShell(ip_ + " link set " + name_ + " up >/dev/null 2>&1") ||
            !RunShell(ip_ + " link set " + peer_ + " up >/dev/null 2>&1")) {
            return false;
        }
        return true;
    }

    /// 改名（ifindex 不变）：用来验证热插拔 diff 以 ifindex 而不是名字为准。
    bool Rename(const std::string& new_name) {
        // rename 要求接口先 down。
        if (!RunShell(ip_ + " link set " + name_ + " down >/dev/null 2>&1") ||
            !RunShell(ip_ + " link set " + name_ + " name " + new_name +
                      " >/dev/null 2>&1")) {
            return false;
        }
        name_ = new_name;
        return RunShell(ip_ + " link set " + name_ + " up >/dev/null 2>&1");
    }

    void Remove() {
        if (!created_) {
            return;
        }
        const int status = std::system(
            (ip_ + " link delete " + name_ + " >/dev/null 2>&1").c_str());
        (void)status;
        created_ = false;
    }

    const std::string& name() const noexcept { return name_; }
    const std::string& peer() const noexcept { return peer_; }

private:
    std::string ip_;
    std::string name_;
    std::string peer_;
    bool created_{false};
};

/// 从指定接口发出 count 个以太网帧（不需要配置 IP）。
///
/// AF_PACKET 的发送路径经过 dev_queue_xmit，正是 TC egress hook 的位置，
/// 因此能在新 attach 的 veth 上产生真实计数，证明「挂上了」不是只记了名字。
int SendRawFrames(const std::string& interface, int count) {
    const unsigned int ifindex = ::if_nametoindex(interface.c_str());
    if (ifindex == 0) {
        return -1;
    }
    const int fd = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_ll address {};
    address.sll_family = AF_PACKET;
    address.sll_protocol = htons(ETH_P_ALL);
    address.sll_ifindex = static_cast<int>(ifindex);
    address.sll_halen = 6;
    std::memset(address.sll_addr, 0xff, 6);

    unsigned char frame[64] = {};
    std::memset(frame, 0xff, 6);  // 目的 MAC：广播
    frame[6] = 0x02;              // 源 MAC：本地管理地址
    frame[12] = 0x08;
    frame[13] = 0x00;  // EtherType：IPv4

    int sent = 0;
    for (int i = 0; i < count; ++i) {
        const ssize_t result =
            ::sendto(fd, frame, sizeof(frame), 0,
                     reinterpret_cast<struct sockaddr*>(&address),
                     sizeof(address));
        if (result > 0) {
            ++sent;
        }
    }
    (void)::close(fd);
    return sent;
}

const NetEbpfStat* FindNetStat(const Snapshot& snapshot,
                               const std::string& name) {
    for (const auto& net : snapshot.ebpf_nets) {
        if (net.name == name) {
            return &net;
        }
    }
    return nullptr;
}

/// 接口热插拔：新接口必须被 attach 且能真实计数，删除后必须从记录中移除。
///
/// 需要 CAP_NET_ADMIN（root）。环境不允许创建 veth（无 ip 命令、内核没编
/// veth、容器限制能力）时打印原因并跳过：这属于宿主环境配置，不应让整个
/// eBPF 用例失败。
void CheckHotplug(NetEbpfMonitor& monitor) {
    go2cpp_tests::announce("net_ebpf_hotplug");
    const char* ip = FindIpCommand();
    if (ip == nullptr) {
        std::printf("[ebpf] hotplug: `ip` not found; skipped\n");
        return;
    }
    VethPair veth(ip);
    if (!veth.Create()) {
        std::printf(
            "[ebpf] hotplug: cannot create veth (need CAP_NET_ADMIN); "
            "skipped\n");
        return;
    }
    std::printf("[ebpf] hotplug: created %s <-> %s\n", veth.name().c_str(),
                veth.peer().c_str());

    // 1) UpdateOnce 自己的 1Hz 扫描必须能发现新接口（不能只靠显式刷新）。
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    Snapshot first;
    GO2CPP_CHECK(monitor.UpdateOnce(first) == Status::kOk);
    std::vector<std::string> attached = monitor.attached_interfaces();
    GO2CPP_CHECK(ContainsInterface(attached, veth.name()));
    GO2CPP_CHECK(ContainsInterface(attached, veth.peer()));
    // 热插拔成功时不应留下 reason。
    GO2CPP_CHECK(monitor.reason().empty());
    std::printf("[ebpf] hotplug: UpdateOnce attached the new veth pair\n");

    // 2) 新接口上的 TC hook 必须真的在跑：发原始帧后 egress 计数增长。
    const int frames = 32;
    const int sent = SendRawFrames(veth.name(), frames);
    std::printf("[ebpf] hotplug: sent %d/%d raw frames on %s\n", sent, frames,
                veth.name().c_str());
    GO2CPP_CHECK(sent == frames);
    if (sent > 0) {
        Snapshot with_traffic;
        GO2CPP_CHECK(monitor.UpdateOnce(with_traffic) == Status::kOk);
        const NetEbpfStat* stat = FindNetStat(with_traffic, veth.name());
        GO2CPP_CHECK(stat != nullptr);
        if (stat != nullptr) {
            std::printf(
                "[ebpf] hotplug: %s snd_packets=%llu rcv_packets=%llu\n",
                veth.name().c_str(),
                static_cast<unsigned long long>(stat->snd_packets),
                static_cast<unsigned long long>(stat->rcv_packets));
            GO2CPP_CHECK(stat->snd_packets > 0);
        }
    }

    // 3) rename 不改变 ifindex：记录应平滑改名，而不是「删除 + 新增」。
    const std::string renamed = UniqueName("vethr");
    if (veth.Rename(renamed)) {
        (void)monitor.RefreshInterfaces();
        attached = monitor.attached_interfaces();
        GO2CPP_CHECK(ContainsInterface(attached, renamed));
        GO2CPP_CHECK(ContainsInterface(attached, veth.peer()));
        std::printf("[ebpf] hotplug: rename tracked as %s\n", renamed.c_str());
    } else {
        std::printf("[ebpf] hotplug: rename not supported here; skipped\n");
    }

    // 4) 删除接口后必须从 attach 记录里消失（设备已删时内核会连 clsact qdisc
    //    一起销毁，用户态只需移除记录并停止展示）。
    const std::string removed_name = veth.name();
    veth.Remove();
    const std::size_t remaining = monitor.RefreshInterfaces();
    attached = monitor.attached_interfaces();
    GO2CPP_CHECK(!ContainsInterface(attached, removed_name));
    GO2CPP_CHECK(!ContainsInterface(attached, veth.peer()));
    GO2CPP_CHECK(remaining == attached.size());
    GO2CPP_CHECK(remaining >= 1);  // eth0/docker0 仍在
    std::printf("[ebpf] hotplug: removed; %zu interface(s) still attached\n",
                remaining);

    // 5) 幂等：重复扫描不改变状态、不崩溃。
    GO2CPP_CHECK(monitor.RefreshInterfaces() == remaining);
}

/// 扫描与采集并发：两个线程同时调用 RefreshInterfaces / UpdateOnce，
/// 内部由同一把 mutex 串行化（TC link、map fd、attachments 都是共享状态）。
void CheckConcurrentScanAndCollect(NetEbpfMonitor& monitor) {
    go2cpp_tests::announce("net_ebpf_concurrent_scan");
    std::atomic<bool> stop{false};
    std::atomic<int> samples{0};
    std::thread collector([&monitor, &stop, &samples]() {
        Snapshot snapshot;
        while (!stop.load(std::memory_order_acquire)) {
            if (monitor.UpdateOnce(snapshot) == Status::kOk) {
                samples.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < 300; ++i) {
        (void)monitor.RefreshInterfaces();
    }
    stop.store(true, std::memory_order_release);
    GO2CPP_JOIN_WITH_WATCHDOG(collector, std::chrono::seconds(30));
    GO2CPP_CHECK(samples.load() > 0);
    GO2CPP_CHECK(!monitor.attached_interfaces().empty());
    std::printf("[ebpf] concurrent scan/collect: %d samples\n", samples.load());
}

/// 非 root / 内核不支持时的降级契约。
int CheckUnsupported(NetEbpfMonitor& monitor, bool require_ebpf) {
    const std::string reason = monitor.reason();
    std::printf("[ebpf] unavailable: %s\n", reason.c_str());
    if (require_ebpf) {
        std::fprintf(stderr,
                     "[ebpf] --require-ebpf set but eBPF unavailable: %s\n",
                     reason.c_str());
        return 77;  // ctest SKIP_RETURN_CODE
    }
    GO2CPP_CHECK(!monitor.available());
    GO2CPP_CHECK(!reason.empty());

    Snapshot snapshot;
    GO2CPP_CHECK(monitor.UpdateOnce(snapshot) == Status::kUnsupported);
    GO2CPP_CHECK(!snapshot.ebpf_active);
    GO2CPP_CHECK(!snapshot.ebpf_reason.empty());
    GO2CPP_CHECK(snapshot.ebpf_nets.empty());

    // Stop 必须幂等且不崩溃。
    monitor.Stop();
    monitor.Stop();
    GO2CPP_CHECK(monitor.UpdateOnce(snapshot) == Status::kUnsupported);
    std::printf("[ebpf] graceful degradation verified\n");
    return 0;
}

int CheckAvailable(NetEbpfMonitor& monitor,
                   const std::map<std::string, TcFilterCounts>& tc_baseline,
                   bool tc_available) {
    GO2CPP_CHECK(monitor.available());
    const auto interfaces = monitor.attached_interfaces();
    GO2CPP_CHECK(!interfaces.empty());
    std::printf("[ebpf] attached interfaces:");
    for (const auto& name : interfaces) {
        std::printf(" %s", name.c_str());
    }
    std::printf("\n");

    Snapshot first;
    GO2CPP_CHECK(monitor.UpdateOnce(first) == Status::kOk);
    GO2CPP_CHECK(first.ebpf_active);
    GO2CPP_CHECK(first.ebpf_reason.empty());

    // 刚 attach 时 map 可能还没有任何条目（挂载后尚未产生流量），先发一次
    // 请求再判断，避免把「暂时为空」误判成「采集器没工作」。
    if (TotalBytes(first) == 0) {
        (void)GenerateTraffic();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        GO2CPP_CHECK(monitor.UpdateOnce(first) == Status::kOk);
    }
    GO2CPP_CHECK(!first.ebpf_nets.empty());
    const std::uint64_t before = TotalBytes(first);
    std::printf("[ebpf] bytes before traffic: %llu\n",
                static_cast<unsigned long long>(before));

    // 制造多轮流量，确保计数增长可观测（单次请求可能被内核合并）。
    int ok_requests = 0;
    for (int i = 0; i < 3; ++i) {
        if (GenerateTraffic()) {
            ++ok_requests;
        }
    }
    GO2CPP_CHECK(ok_requests > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    Snapshot second;
    GO2CPP_CHECK(monitor.UpdateOnce(second) == Status::kOk);
    const std::uint64_t after = TotalBytes(second);
    std::printf("[ebpf] bytes after traffic: %llu\n",
                static_cast<unsigned long long>(after));
    GO2CPP_CHECK(after > before);

    // 每个条目都要能映射回真实网卡名。
    for (const auto& net : second.ebpf_nets) {
        GO2CPP_CHECK(!net.name.empty());
        GO2CPP_CHECK(net.rcv_bytes + net.snd_bytes > 0);
    }
    GO2CPP_CHECK(monitor.sample_count() >= 2);

    // 接口热插拔与「扫描/采集并发」用例要在 Stop 之前跑：需要 monitor 处于
    // 可用状态，且 Stop 之后的 tc filter 基线检查只针对启动时就存在的网卡。
    CheckHotplug(monitor);
    CheckConcurrentScanAndCollect(monitor);

    monitor.Stop();
    GO2CPP_CHECK(!monitor.available());
    // 分离失败会写进 reason；正常 Stop 的 reason 是 "stopped"。
    GO2CPP_CHECK(monitor.reason().find("could not be detached") ==
                 std::string::npos);
    if (!tc_available) {
        std::printf("[ebpf] tc not found; detach regression check skipped\n");
    } else {
        CheckTcFiltersRestored(tc_baseline, interfaces, "after first stop");
        // 第二个 attach/detach 周期：此时 clsact 已存在（bpf_tc_hook_create
        // 走 -EEXIST 分支），Aggregator 的 Stop→Start 正是这条路径，必须
        // 同样能清理干净、不能累积过滤器。
        NetEbpfMonitor again;
        GO2CPP_CHECK(again.available());
        const auto again_interfaces = again.attached_interfaces();
        again.Stop();
        CheckTcFiltersRestored(tc_baseline, again_interfaces,
                               "after second stop");
    }
    std::printf("[ebpf] live counters verified\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool require_ebpf = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--require-ebpf") == 0) {
            require_ebpf = true;
        }
    }

    go2cpp_tests::announce("net_ebpf_monitor");
    // root 下先记录 attach 前的 bpf TC filter 基线：Stop 之后必须回到基线。
    const bool tc_available =
        ::geteuid() == 0 && ::access("/usr/sbin/tc", X_OK) == 0;
    std::map<std::string, TcFilterCounts> tc_baseline;
    if (tc_available) {
        tc_baseline = SnapshotTcFilters();
    }
    NetEbpfMonitor monitor;
    int result = 0;
    if (!monitor.available()) {
        result = CheckUnsupported(monitor, require_ebpf);
    } else {
        result = CheckAvailable(monitor, tc_baseline, tc_available);
    }

    const int failures = go2cpp_tests::g_failures.load(std::memory_order_relaxed);
    if (failures != 0) {
        std::fprintf(stderr, "[test] failures=%d\n", failures);
        return 1;
    }
    return result;
}
