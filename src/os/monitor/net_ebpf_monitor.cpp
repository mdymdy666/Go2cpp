#include "go2cpp/os/monitor/net_ebpf_monitor.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(GO2CPP_HAVE_EBPF)
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <dirent.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <unistd.h>
#endif

#if defined(GO2CPP_HAVE_EBPF)
// 由 CMake 的 embed_bpf.cmake 在构建期生成：把 net_stats.bpf.o 的 ELF 字节
// 内嵌进用户态库，运行时不再需要磁盘上的 .bpf.o 文件。
extern "C" {
extern const unsigned char go2cpp_net_stats_elf[];
extern const unsigned long go2cpp_net_stats_elf_len;
}
#endif

namespace go2cpp::os::monitor {
namespace {

/// 与 net_stats.bpf.c 完全一致的 map value 布局（4 个 u64）。
struct NetStatsValue {
    std::uint64_t rcv_bytes;
    std::uint64_t rcv_packets;
    std::uint64_t snd_bytes;
    std::uint64_t snd_packets;
};

#if defined(GO2CPP_HAVE_EBPF)

/// 列出 /sys/class/net 下所有网卡（含 lo；挂载时统一跳过 loopback）。
std::vector<std::string> ListInterfaces() {
    std::vector<std::string> names;
    DIR* dir = ::opendir("/sys/class/net");
    if (dir == nullptr) {
        return names;
    }
    while (dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        names.emplace_back(entry->d_name);
    }
    ::closedir(dir);
    return names;
}

/// libbpf 的 TC API 统一返回负 errno；转成可读字符串。
std::string TcErrorString(long error) {
    const int code = static_cast<int>(error < 0 ? -error : error);
    return std::strerror(code == 0 ? EINVAL : code);
}

#endif  // GO2CPP_HAVE_EBPF

}  // namespace

struct NetEbpfMonitor::Impl {
    /// 串行化 UpdateOnce / 热插拔扫描 / Stop / 查询：TC link、map fd、
    /// attachments 与 available 都会在生命周期切换和扫描中被改写，并发访问
    /// 是数据竞争（其它九个采集器都有同样的锁）。
    mutable std::mutex mutex;
    bool available{false};
    /// 不可用原因，或最近一次热插拔失败（available==true 时仍可能有值）。
    std::string reason;
    /// reason 是否由热插拔失败写入：只有这种 reason 能在扫描恢复正常后清空，
    /// 不能误清 "requires root privileges" / "stopped" 等生命周期原因。
    bool reason_is_hotplug{false};
    std::atomic<std::uint64_t> sample_count{0};

#if defined(GO2CPP_HAVE_EBPF)
    /// 一张网卡上两个方向各自已 attach 的 TC 程序。
    ///
    /// 为什么必须保存 handle/priority：bpf_tc_detach() 靠 attach 时由
    /// libbpf 回填的这对标识定位 clsact 上的过滤器。只传 `sz`（handle 与
    /// priority 都是 0）会失败（-EINVAL），过滤器留在 qdisc 上继续运行，
    /// 把 BPF 程序和 map 钉在内核里 —— 每次 Start/Stop 泄漏一对程序，且
    /// 残留程序仍然逐包执行。原实现忽略了 detach 的返回值，因此这个泄漏
    /// 在测试里完全不可见（实测跑 5 次后 eth0 上残留 5 对过滤器）。
    ///
    /// ifindex 是热插拔的稳定标识：网卡改名时 ifindex 不变，只更新 name；
    /// 网卡删除/重建时 ifindex 会变（也可能被复用），因此扫描以 ifindex 为准
    /// 做 diff，避免按名字比较把 rename 误判成「删除 + 新增」。
    struct TcAttachment {
        std::string name;
        unsigned int ifindex{0};
        std::uint32_t ingress_handle{0};
        std::uint32_t ingress_priority{0};
        std::uint32_t egress_handle{0};
        std::uint32_t egress_priority{0};
        bool ingress{false};
        bool egress{false};
    };

    /// 热插拔扫描的最小间隔。UpdateOnce 通常 1Hz，因此每个采集周期最多
    /// 扫描一次 /sys/class/net；显式 RefreshInterfaces() 跳过节流。
    static constexpr std::chrono::milliseconds kRescanInterval{1000};

    bpf_object* object{nullptr};
    bpf_program* ingress_prog{nullptr};
    bpf_program* egress_prog{nullptr};
    int map_fd{-1};
    std::vector<TcAttachment> attachments;
    std::chrono::steady_clock::time_point last_scan{};
    bool scan_done{false};

    ~Impl() { Shutdown(); }

    /// 按保存的 handle/priority 分离一张网卡某个方向的过滤器。
    static bool DetachOne(int ifindex, bpf_tc_attach_point point, __u32 handle,
                          __u32 priority) {
        if (handle == 0) {
            // libbpf 没有回填标识（异常路径）：无从定位，按失败上报。
            return false;
        }
        // 不用 LIBBPF_OPTS：它的 designated initializer 在 C++17 下会触发
        // -Wc++20-extensions；手工填 sz/字段与宏展开完全等价。
        bpf_tc_hook hook{};
        hook.sz = sizeof(hook);
        hook.ifindex = ifindex;
        hook.attach_point = point;
        bpf_tc_opts opts{};
        opts.sz = sizeof(opts);
        opts.handle = handle;
        opts.priority = priority;
        return bpf_tc_detach(&hook, &opts) == 0;
    }

    void SetHotplugReason(std::string text) {
        reason = std::move(text);
        reason_is_hotplug = true;
    }

    void ClearHotplugReason() {
        if (reason_is_hotplug) {
            reason.clear();
            reason_is_hotplug = false;
        }
    }

    /// 把两个 TC 程序挂到一张网卡上；成功时追加到 attachments。
    bool AttachOne(const std::string& name, unsigned int ifindex,
                   std::string* error) {
        if (object == nullptr || ingress_prog == nullptr ||
            egress_prog == nullptr) {
            *error = "eBPF object is not loaded";
            return false;
        }
        bpf_tc_hook hook{};
        hook.sz = sizeof(hook);
        hook.ifindex = static_cast<int>(ifindex);
        hook.attach_point = BPF_TC_INGRESS;
        const long hook_error = bpf_tc_hook_create(&hook);
        // -EEXIST 表示 clsact 已存在，可以直接复用。
        if (hook_error != 0 && hook_error != -EEXIST) {
            *error = "clsact qdisc on " + name +
                     " could not be created: " + TcErrorString(hook_error);
            return false;
        }

        TcAttachment attachment;
        attachment.name = name;
        attachment.ifindex = ifindex;
        bpf_tc_opts ingress_opts{};
        ingress_opts.sz = sizeof(ingress_opts);
        ingress_opts.prog_fd = bpf_program__fd(ingress_prog);
        const long ingress_error = bpf_tc_attach(&hook, &ingress_opts);
        if (ingress_error == 0) {
            // 成功时 libbpf 会把内核分配的 handle/priority 回填进 opts；
            // detach 必须原样带回，否则定位不到过滤器。
            attachment.ingress = true;
            attachment.ingress_handle = ingress_opts.handle;
            attachment.ingress_priority = ingress_opts.priority;
        }
        hook.attach_point = BPF_TC_EGRESS;
        bpf_tc_opts egress_opts{};
        egress_opts.sz = sizeof(egress_opts);
        egress_opts.prog_fd = bpf_program__fd(egress_prog);
        const long egress_error = bpf_tc_attach(&hook, &egress_opts);
        if (egress_error == 0) {
            attachment.egress = true;
            attachment.egress_handle = egress_opts.handle;
            attachment.egress_priority = egress_opts.priority;
        }
        if (!attachment.ingress && !attachment.egress) {
            *error = "TC attach on " + name + " failed: ingress " +
                     TcErrorString(ingress_error) + ", egress " +
                     TcErrorString(egress_error);
            return false;
        }
        attachments.push_back(std::move(attachment));
        return true;
    }

    /// 扫描 /sys/class/net 并与 attachments 做 diff：
    ///   - 新出现的非 loopback 网卡 → attach ingress/egress；
    ///   - 消失的网卡 → 从记录中移除（设备已删时内核会连 qdisc 一起销毁，
    ///     无从也无需 detach；ifindex 仍有效时仍按 handle 精确分离）；
    ///   - rename（ifindex 不变）→ 只更新记录里的名字。
    ///
    /// 调用方必须持有 mutex。失败写 reason（不把 available 置 false：
    /// 一个接口挂不上不应让整个采集器停摆，下一轮扫描会重试）。
    void Rescan(bool force) {
        if (!available || object == nullptr) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && scan_done && now - last_scan < kRescanInterval) {
            return;
        }
        last_scan = now;
        scan_done = true;

        const std::vector<std::string> names = ListInterfaces();
        if (names.empty()) {
            SetHotplugReason(
                "hotplug: cannot read /sys/class/net; interface changes are "
                "not tracked");
            return;
        }
        std::vector<std::pair<std::string, unsigned int>> present;
        present.reserve(names.size());
        for (const auto& name : names) {
            if (name == "lo") {
                continue;
            }
            const unsigned int index = ::if_nametoindex(name.c_str());
            if (index == 0) {
                // 扫描与删除并发：接口在 readdir 之后消失，下个周期再收敛。
                continue;
            }
            present.emplace_back(name, index);
        }

        std::string errors;
        // 1) 消失（或改名后不再出现在当前名字下）的接口。
        for (auto it = attachments.begin(); it != attachments.end();) {
            const bool still_present =
                std::any_of(present.begin(), present.end(),
                            [&it](const std::pair<std::string, unsigned int>&
                                      item) {
                                return item.second == it->ifindex;
                            });
            if (still_present) {
                ++it;
                continue;
            }
            const unsigned int index = ::if_nametoindex(it->name.c_str());
            if (index != 0) {
                // ifindex 仍可解析：接口还在（例如被移出 /sys/class/net 的
                // 竞态窗口），按保存的 handle/priority 精确分离。
                if (it->ingress &&
                    !DetachOne(static_cast<int>(index), BPF_TC_INGRESS,
                               it->ingress_handle, it->ingress_priority)) {
                    if (!errors.empty()) {
                        errors += "; ";
                    }
                    errors += "detach " + it->name + " ingress failed";
                }
                if (it->egress &&
                    !DetachOne(static_cast<int>(index), BPF_TC_EGRESS,
                               it->egress_handle, it->egress_priority)) {
                    if (!errors.empty()) {
                        errors += "; ";
                    }
                    errors += "detach " + it->name + " egress failed";
                }
            }
            it = attachments.erase(it);
        }

        // 2) 新出现的接口（按 ifindex 匹配；rename 只更新名字）。
        for (const auto& item : present) {
            auto found = std::find_if(
                attachments.begin(), attachments.end(),
                [&item](const TcAttachment& attachment) {
                    return attachment.ifindex == item.second;
                });
            if (found != attachments.end()) {
                if (found->name != item.first) {
                    found->name = item.first;
                }
                continue;
            }
            std::string error;
            if (!AttachOne(item.first, item.second, &error)) {
                if (!errors.empty()) {
                    errors += "; ";
                }
                errors += error;
                continue;
            }
        }

        if (errors.empty()) {
            ClearHotplugReason();
        } else {
            SetHotplugReason("hotplug: " + errors);
        }
    }

    void Shutdown() {
        if (object == nullptr) {
            map_fd = -1;
            attachments.clear();
            scan_done = false;
            return;
        }
        // 先分离所有已挂载的 TC hook。clsact qdisc 本身保留：它可能由其它
        // 工具或本模块的另一次运行创建，删除会破坏别人的配置。
        std::size_t failed = 0;
        for (const auto& attachment : attachments) {
            const unsigned int ifindex =
                ::if_nametoindex(attachment.name.c_str());
            if (ifindex == 0) {
                // 网卡已删除，过滤器随设备一起消失，不算失败。
                continue;
            }
            if (attachment.ingress &&
                !DetachOne(static_cast<int>(ifindex), BPF_TC_INGRESS,
                           attachment.ingress_handle,
                           attachment.ingress_priority)) {
                ++failed;
            }
            if (attachment.egress &&
                !DetachOne(static_cast<int>(ifindex), BPF_TC_EGRESS,
                           attachment.egress_handle,
                           attachment.egress_priority)) {
                ++failed;
            }
        }
        attachments.clear();
        bpf_object__close(object);
        object = nullptr;
        ingress_prog = nullptr;
        egress_prog = nullptr;
        map_fd = -1;
        available = false;
        scan_done = false;
        reason_is_hotplug = false;
        // 分离失败写进 reason 而不是静默吞掉：Aggregator 的 Stop 与诊断
        // 接口能看出内核里还留着过滤器。
        reason = failed == 0
                     ? "stopped"
                     : "stopped; " + std::to_string(failed) +
                           " TC hook(s) could not be detached";
    }

    /// 加载内嵌 ELF 并把两个 TC 程序挂到所有非 loopback 网卡。
    void Init() {
        if (::geteuid() != 0) {
            reason =
                "requires root privileges (euid != 0); eBPF load is disabled";
            return;
        }
        // unprivileged_bpf_disabled=2 时非 root 连 open 都会被拒；上面的
        // euid 检查已提前拦截，这里只处理真正的加载/挂载失败。
        object = bpf_object__open_mem(go2cpp_net_stats_elf,
                                      go2cpp_net_stats_elf_len, nullptr);
        long err = libbpf_get_error(object);
        if (err != 0) {
            reason = std::string("bpf_object__open_mem failed: ") +
                     std::strerror(static_cast<int>(-err));
            object = nullptr;
            return;
        }
        if (bpf_object__load(object) != 0) {
            reason = std::string("bpf_object__load failed: ") +
                     std::strerror(errno);
            bpf_object__close(object);
            object = nullptr;
            return;
        }
        ingress_prog = bpf_object__find_program_by_name(object, "tc_ingress");
        egress_prog = bpf_object__find_program_by_name(object, "tc_egress");
        if (ingress_prog == nullptr || egress_prog == nullptr) {
            reason = "embedded BPF object lacks tc_ingress/tc_egress";
            bpf_object__close(object);
            object = nullptr;
            ingress_prog = nullptr;
            egress_prog = nullptr;
            return;
        }
        map_fd = bpf_object__find_map_fd_by_name(object, "net_stats_map");
        if (map_fd < 0) {
            reason = "net_stats_map not found in embedded BPF object";
            bpf_object__close(object);
            object = nullptr;
            ingress_prog = nullptr;
            egress_prog = nullptr;
            return;
        }

        // 首轮扫描复用热插拔的 diff 逻辑（attachments 为空，等价于把所有
        // 现存非 loopback 网卡挂一遍）。
        available = true;
        reason.clear();
        reason_is_hotplug = false;
        scan_done = false;
        Rescan(true);
        if (!attachments.empty()) {
            return;
        }
        // 一个接口都没挂上：与旧实现一致，降级为不可用并释放对象。
        const std::string scan_reason = reason;
        bpf_object__close(object);
        object = nullptr;
        ingress_prog = nullptr;
        egress_prog = nullptr;
        map_fd = -1;
        available = false;
        scan_done = false;
        reason_is_hotplug = false;
        reason = scan_reason.empty()
                     ? "TC hook attach failed on every interface (need "
                       "CAP_NET_ADMIN)"
                     : scan_reason;
    }
#endif  // GO2CPP_HAVE_EBPF
};

NetEbpfMonitor::NetEbpfMonitor() : impl_(std::make_unique<Impl>()) {
#if defined(GO2CPP_HAVE_EBPF)
    impl_->Init();
#else
    impl_->reason =
        "go2cpp built without libbpf/clang BPF support (GO2CPP_HAVE_EBPF=0)";
#endif
}

NetEbpfMonitor::~NetEbpfMonitor() = default;

Status NetEbpfMonitor::UpdateOnce(Snapshot& snapshot) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->available) {
        // 不能把调用方 Snapshot 里的旧 ebpf_nets 留在原地，否则 Stop 之后
        // 直接复用 Snapshot 的调用方会看到过期计数。
        snapshot.ebpf_nets.clear();
        snapshot.ebpf_active = false;
        snapshot.ebpf_reason = impl_->reason;
        return Status::kUnsupported;
    }
#if defined(GO2CPP_HAVE_EBPF)
    // 1Hz 节流的热插拔扫描：容器/隧道/team 网卡在 Start 之后才出现时，
    // 下一个采集周期就会接管；接口消失则从记录中移除。
    impl_->Rescan(false);

    std::vector<NetEbpfStat> stats;
    std::uint32_t key = 0;
    std::uint32_t next = 0;
    // bpf_map_get_next_key 的语义：key 为 NULL 取第一个；否则取 key 的后继。
    // 用 do/while 先取第一个再迭代，避免漏掉 key==0 的项。
    bool first = true;
    while (true) {
        const int rc = bpf_map_get_next_key(impl_->map_fd, first ? nullptr : &key,
                                            &next);
        if (rc != 0) {
            break;
        }
        first = false;
        NetStatsValue value{};
        if (bpf_map_lookup_elem(impl_->map_fd, &next, &value) != 0) {
            key = next;
            continue;
        }
        char name[IF_NAMESIZE] = {};
        if (::if_indextoname(next, name) == nullptr) {
            key = next;
            continue;
        }
        NetEbpfStat stat;
        stat.name = name;
        stat.rcv_bytes = value.rcv_bytes;
        stat.rcv_packets = value.rcv_packets;
        stat.snd_bytes = value.snd_bytes;
        stat.snd_packets = value.snd_packets;
        stats.push_back(std::move(stat));
        key = next;
    }
    snapshot.ebpf_nets = std::move(stats);
    snapshot.ebpf_active = true;
    // 正常情况下为空；热插拔失败时把原因透出给采集链路（available 仍为
    // true，其它网卡继续采集）。
    snapshot.ebpf_reason = impl_->reason;
    ++impl_->sample_count;
    return Status::kOk;
#else
    (void)snapshot;
    return Status::kUnsupported;
#endif
}

void NetEbpfMonitor::Stop() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
#if defined(GO2CPP_HAVE_EBPF)
    impl_->Shutdown();
#else
    impl_->available = false;
#endif
}

bool NetEbpfMonitor::available() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->available;
}

std::string NetEbpfMonitor::reason() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->reason;
}

std::size_t NetEbpfMonitor::RefreshInterfaces() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
#if defined(GO2CPP_HAVE_EBPF)
    impl_->Rescan(true);
    return impl_->attachments.size();
#else
    return 0;
#endif
}

std::uint64_t NetEbpfMonitor::sample_count() const noexcept {
    return impl_->sample_count.load(std::memory_order_relaxed);
}

std::vector<std::string> NetEbpfMonitor::attached_interfaces() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
#if defined(GO2CPP_HAVE_EBPF)
    std::vector<std::string> names;
    names.reserve(impl_->attachments.size());
    for (const auto& attachment : impl_->attachments) {
        names.push_back(attachment.name);
    }
    return names;
#else
    return {};
#endif
}

}  // namespace go2cpp::os::monitor
