// SPDX-License-Identifier: GPL-2.0
//
// syscall_latency.bpf.c —— 系统调用延迟统计（go2cpp::os::monitor::SyscallMonitor）。
//
// 挂两个 tracepoint：
//   raw_syscalls:sys_enter  记录 (pid_tgid -> 起始时间)
//   raw_syscalls:sys_exit   取起始时间算 delta，写入该 syscall id 的
//                           计数/总时长/max/对数直方图
//
// 为什么不用 syscalls:sys_enter_* 系列：每个 syscall 的 tracepoint 参数布局
// 各不相同（u32/u64 混合，偏移由内核逐个定义），用 raw_syscalls 的
// `args[6]` 统一为 unsigned long 才能一套代码处理全部 syscall。futex 的
// 等待时长另有专门的 futex_latency.bpf.c，避免这里为它做特殊分支。
//
// 开销设计（实测 A/B 差分调优过）：
//   - 起始项**常驻**（exit 不删除），enter 命中后只写 8 字节；只有线程第一次
//     进入才 update，省掉每次 syscall 的 delete + 分配；
//   - syscall 统计用 ARRAY[512]（id 直接索引）而不是 HASH：ARRAY lookup 是
//     一次数组访问，比 HASH 查找便宜，且预分配后没有插入/扩容；
//   - 起始 map 用 LRU_HASH：线程退出后的残留项会被自动淘汰，不会把 map 塞满
//     导致新线程永远 update 失败。

#include <linux/bpf.h>
#include <linux/types.h>
#include <bpf/bpf_helpers.h>

#include "monitor_hist.h"

/* syscall 编号上限；x86_64 6.6 内核目前是 0..461。 */
#define GO2CPP_MAX_SYSCALL_ID 512

/* 每个线程一次 syscall 的起点。key = bpf_get_current_pid_tgid()。 */
struct syscall_start {
    __u64 ts;
};

/* 每个 syscall id 的累计统计；value 布局必须与用户态一致。 */
struct syscall_stat {
    __u64 count;
    __u64 total_ns;
    __u64 max_ns;
    __u64 hist[GO2CPP_HIST_BUCKETS];
};

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 16384);
    __type(key, __u64);
    __type(value, struct syscall_start);
} syscall_start_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, GO2CPP_MAX_SYSCALL_ID);
    __type(key, __u32);
    __type(value, struct syscall_stat);
} syscall_stat_map SEC(".maps");

/* tracepoint/raw_syscalls/sys_enter 的稳定前缀布局。 */
struct trace_sys_enter {
    __u64 unused;
    __s64 id;
    __u64 args[6];
};

struct trace_sys_exit {
    __u64 unused;
    __s64 id;
    __s64 ret;
};

SEC("tracepoint/raw_syscalls/sys_enter")
int syscall_enter(struct trace_sys_enter *ctx)
{
    __u64 key = bpf_get_current_pid_tgid();
    __u64 now = bpf_ktime_get_ns();
    struct syscall_start *start;

    (void)ctx;
    start = bpf_map_lookup_elem(&syscall_start_map, &key);
    if (start) {
        start->ts = now;
        return 0;
    }
    struct syscall_start init = {};

    init.ts = now;
    bpf_map_update_elem(&syscall_start_map, &key, &init, BPF_ANY);
    return 0;
}

SEC("tracepoint/raw_syscalls/sys_exit")
int syscall_exit(struct trace_sys_exit *ctx)
{
    __u64 key = bpf_get_current_pid_tgid();
    struct syscall_start *start;
    struct syscall_stat *stat;
    __u64 delta;
    __u32 id;
    __u32 bucket;

    start = bpf_map_lookup_elem(&syscall_start_map, &key);
    if (!start)
        return 0;

    delta = bpf_ktime_get_ns() - start->ts;
    id = (__u32)ctx->id;
    if (id >= GO2CPP_MAX_SYSCALL_ID)
        return 0;
    bucket = go2cpp_hist_bucket(delta) & (GO2CPP_HIST_BUCKETS - 1);

    stat = bpf_map_lookup_elem(&syscall_stat_map, &id);
    if (!stat)
        return 0;

    __sync_fetch_and_add(&stat->count, 1);
    __sync_fetch_and_add(&stat->total_ns, delta);
    /* max 是近似值：并发竞争时可能丢失一次更新（只会偏小）。 */
    if (delta > stat->max_ns)
        stat->max_ns = delta;
    __sync_fetch_and_add(&stat->hist[bucket], 1);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
