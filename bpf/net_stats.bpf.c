// SPDX-License-Identifier: GPL-2.0
//
// net_stats.bpf.c —— TC hook 网络统计（go2cpp::os::monitor 的 eBPF 部分）。
//
// 与参考实现 monitor_system/worker/src/ebpf/net_stats.bpf.c 的设计一致：
//   - 在 clsact qdisc 的 ingress / egress 各挂一个 TC 程序；
//   - 以 ifindex 为 key 写入 HASH map，用户态用 if_indextoname 还原网卡名；
//   - 每条路径只做一次 map lookup + 两个原子加，不分配、不拷贝 payload。
//
// TC hook 位于 L2/L3 边界，`struct __sk_buff` 是内核保证的稳定 ABI，因此本程序
// 不需要 vmlinux.h / CO-RE，字节码可以跨内核版本复用（WSL2 6.6 实测可加载）。
//
// 编译（由 go2cpp/cmake/Go2CppMonitor.cmake 自动执行）：
//   clang -O2 -g -target bpf -D__TARGET_ARCH_x86 -I/usr/include/x86_64-linux-gnu \
//         -c net_stats.bpf.c -o net_stats.bpf.o

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

/* TC 返回值：继续正常处理，不改变数据包路径。 */
#define TC_ACT_OK 0

/* 与用户态共享的统计结构；字段顺序/大小必须与 net_ebpf_monitor.cpp 一致。 */
struct net_stats {
    __u64 rcv_bytes;   /* 接收字节数 */
    __u64 rcv_packets; /* 接收包数 */
    __u64 snd_bytes;   /* 发送字节数 */
    __u64 snd_packets; /* 发送包数 */
};

/* key = ifindex，value = 该网卡的累计计数；最多 64 个网卡。 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, struct net_stats);
} net_stats_map SEC(".maps");

/*
 * 更新单个网卡的统计。
 *
 * 首次看到某个 ifindex 时插入初始值，之后用 __sync_fetch_and_add 原子累加：
 * 同一网卡的 ingress/egress 可能在多个 CPU 上并发执行，非原子读改写会丢计数。
 * 不用 PERCPU map 是为了让用户态读取保持单次 lookup 的简单语义。
 */
static __always_inline void update_stats(__u32 ifindex, __u32 len, int is_rx)
{
    struct net_stats *stats = bpf_map_lookup_elem(&net_stats_map, &ifindex);
    if (!stats) {
        struct net_stats init = {};
        if (is_rx) {
            init.rcv_bytes = len;
            init.rcv_packets = 1;
        } else {
            init.snd_bytes = len;
            init.snd_packets = 1;
        }
        /* BPF_NOEXIST 而不是 BPF_ANY：同一网卡的首包可能同时在多个 CPU 上
         * 到达，BPF_ANY 会让后插入者把先插入者的计数（含它自己的首包）
         * 整个覆盖掉，直接丢包计数。插入成功说明本 CPU 已经把这一个包写进
         * 初值，直接返回；插入失败说明别的 CPU 已建好条目，重新 lookup 后
         * 走原子累加，两个首包都不会丢。 */
        if (bpf_map_update_elem(&net_stats_map, &ifindex, &init,
                                BPF_NOEXIST) == 0)
            return;
        stats = bpf_map_lookup_elem(&net_stats_map, &ifindex);
        if (!stats)
            return;
    }
    if (is_rx) {
        __sync_fetch_and_add(&stats->rcv_bytes, len);
        __sync_fetch_and_add(&stats->rcv_packets, 1);
    } else {
        __sync_fetch_and_add(&stats->snd_bytes, len);
        __sync_fetch_and_add(&stats->snd_packets, 1);
    }
}

/* 入方向：数据包从网卡进入协议栈。skb->ifindex 是入口网卡。 */
SEC("tc")
int tc_ingress(struct __sk_buff *skb)
{
    __u32 ifindex = skb->ifindex;
    __u32 len = skb->len;
    if (ifindex == 0 || len == 0)
        return TC_ACT_OK;
    update_stats(ifindex, len, 1);
    return TC_ACT_OK;
}

/* 出方向：数据包从协议栈发往网卡。 */
SEC("tc")
int tc_egress(struct __sk_buff *skb)
{
    __u32 ifindex = skb->ifindex;
    __u32 len = skb->len;
    if (ifindex == 0 || len == 0)
        return TC_ACT_OK;
    update_stats(ifindex, len, 0);
    return TC_ACT_OK;
}

char LICENSE[] SEC("license") = "GPL";
