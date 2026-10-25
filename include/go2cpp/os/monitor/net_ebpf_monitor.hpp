#pragma once

// eBPF 网络统计采集器：TC hook（ingress/egress）统计每网卡收发字节/包数。
//
// 与参考实现 monitor_system 的 net_stats.bpf.c 对齐：BPF 程序用 ifindex 作
// map key，用户态通过 if_indextoname 还原网卡名。区别是字节码在构建期嵌入
// 用户态库（bpf_object__open_mem），运行时不依赖 .bpf.o 文件。
//
// 接口热插拔：Start 之后新出现的网卡（容器 veth、隧道、bond/team、USB 网卡）
// 会在 UpdateOnce 内按 1Hz 节流重新扫描 /sys/class/net 并自动 attach；消失的
// 网卡从记录中移除（设备删除时内核会连同 clsact qdisc 一起销毁）。需要确定性
// 时序的调用方可用 RefreshInterfaces() 跳过 1Hz 节流立即扫描。
//
// 降级：非 root / 内核缺 BPF / 无 libbpf 编译支持时 available()==false，
// reason() 给出原因，UpdateOnce 返回 kUnsupported，不影响其它采集器。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/os/monitor/monitor.hpp"

namespace go2cpp::os::monitor {

class NetEbpfMonitor final : public Monitor {
public:
    NetEbpfMonitor();
    ~NetEbpfMonitor() override;

    /** @brief 读取内核 map 并填充 snapshot.ebpf_nets。 */
    Status UpdateOnce(Snapshot& snapshot) override;

    /** @brief 分离 TC hook 并关闭 BPF 对象；可重复调用。 */
    void Stop() override;

    bool available() const override;
    std::string reason() const override;

    /**
     * @brief 立即重新扫描 /sys/class/net：新网卡 attach、消失的网卡 detach。
     *
     * 正常采集不需要显式调用（UpdateOnce 内已按 1Hz 节流自动扫描）。本方法
     * 跳过节流，供诊断与需要确定性时序的测试使用。
     *
     * @return 扫描后处于 attach 状态的网卡数量；不可用时返回 0。
     */
    std::size_t RefreshInterfaces();

    /** @brief 已成功读数次数（诊断用）。 */
    std::uint64_t sample_count() const noexcept;

    /** @brief 已挂载的网卡名列表（诊断用）。 */
    std::vector<std::string> attached_interfaces() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace go2cpp::os::monitor
