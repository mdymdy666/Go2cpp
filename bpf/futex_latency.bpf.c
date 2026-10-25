// SPDX-License-Identifier: GPL-2.0
//
// futex_latency.bpf.c —— futex 竞争/等待统计（go2cpp::os::monitor::FutexMonitor）。
//
// 挂两个 tracepoint：
//   syscalls:sys_enter_futex  记录 (pid_tgid -> 起始时间, op)
//   syscalls:sys_exit_futex   计算 syscall 时长，按 op 分类：
//                               wait 类（FUTEX_WAIT/WAIT_BITSET/LOCK_PI/
//                               WAIT_REQUEUE_PI）计入等待直方图；
//                               wake 类（WAKE/WAKE_BITSET/UNLOCK_PI/...）
//                               只计数，用于 wait/wake 争用比。
//
// 为什么用 syscalls:sys_enter_futex 而不是 raw_syscalls + id 过滤：这个
// tracepoint 的 format 是固定布局（uaddr @16, op @24），不依赖
// __NR_futex 编号，跨架构更稳；同目录的 syscall_latency.bpf.c 已经覆盖了
// 全部 syscall 的总延迟，这里只关心 futex 语义。
//
// 注意：FUTEX_WAIT 的 syscall 时长 = 实际睡眠时间 + 少量进出开销；值不匹配
// 时立即返回 -EAGAIN，内核根本没睡。旧实现把它也计进等待直方图，大量亚微秒
// 样本会把 p50/p99 拉低（系统繁忙时 EAGAIN 可占 wait 调用的大头）。现在按
// sys_exit 的返回值分类：返回 0（真正睡着后被唤醒）才进入等待直方图与
// wait_ns/max；-EAGAIN 计 eagain_count；-ETIMEDOUT 计 timedout_count；其余
// 负值（-EINTR/-EFAULT/...）计 error_count。三者都不进直方图。

#include <linux/bpf.h>
#include <linux/types.h>
#include <bpf/bpf_helpers.h>

#include "monitor_hist.h"

#define FUTEX_CMD_MASK 0x7f /* 去掉 FUTEX_PRIVATE_FLAG / CLOCK_REALTIME */

struct futex_start {
    __u64 ts;
    __u32 op;
    __u32 pad;
};

/* futex syscall 返回值的分类常量（Linux 通用 errno，各架构一致）。 */
#define GO2CPP_EAGAIN 11
#define GO2CPP_ETIMEDOUT 110

/* 每进程累计值；value 布局必须与用户态 FutexTaskRaw 一致。 */
struct futex_task {
    char comm[16];
    __u64 wait_count;   /* 返回 0：真正睡着后被唤醒，直方图样本 */
    __u64 wait_ns;
    __u64 max_wait_ns;
    __u64 wake_count;
    __u64 eagain_count;   /* -EAGAIN：值不匹配，未睡眠 */
    __u64 timedout_count; /* -ETIMEDOUT：带超时的等待到点 */
    __u64 error_count;    /* 其余负值（-EINTR/-EFAULT/...） */
};

/* 全局累计值 + 等待时长直方图（只含返回 0 的睡眠样本）。 */
struct futex_global {
    __u64 wait_count;
    __u64 wait_ns;
    __u64 max_wait_ns;
    __u64 wake_count;
    __u64 eagain_count;
    __u64 timedout_count;
    __u64 error_count;
    __u64 hist[GO2CPP_HIST_BUCKETS];
};

/* 进入 futex 的起点。LRU：线程在 sys_exit 前被杀会留下孤儿条目，
 * 固定 HASH 满后所有新 futex 调用都 update 失败、等待样本归零。 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 16384);
    __type(key, __u64);
    __type(value, struct futex_start);
} futex_start_map SEC(".maps");

/* 每进程累计值；LRU 避免 pid 复用/短期进程占满固定 HASH。 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, struct futex_task);
} futex_task_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct futex_global);
} futex_global_map SEC(".maps");

/* syscalls/sys_enter_futex 的 format：syscall_nr @8，uaddr @16，op @24。 */
struct trace_futex_enter {
    __u64 unused;
    __s32 syscall_nr;
    __u32 pad;
    __u64 uaddr;
    __u64 op;
};

/* syscalls/sys_exit_futex 的 format：syscall_nr @8，ret @16。 */
struct trace_futex_exit {
    __u64 unused;
    __s32 syscall_nr;
    __u32 pad;
    __s64 ret;
};

static __always_inline int futex_is_wait(__u32 cmd)
{
    /* FUTEX_WAIT/FUTEX_LOCK_PI/FUTEX_WAIT_BITSET/FUTEX_WAIT_REQUEUE_PI */
    return cmd == 0 || cmd == 6 || cmd == 9 || cmd == 11;
}

static __always_inline int futex_is_wake(__u32 cmd)
{
    /* FUTEX_WAKE/REQUEUE/CMP_REQUEUE/WAKE_OP/UNLOCK_PI/WAKE_BITSET/
     * CMP_REQUEUE_PI：都会推进/唤醒等待队列，作为 wake 侧计数。 */
    return cmd == 1 || cmd == 3 || cmd == 4 || cmd == 5 || cmd == 7 ||
           cmd == 10 || cmd == 12;
}

static __always_inline struct futex_task *ensure_task(__u32 pid)
{
    struct futex_task *task = bpf_map_lookup_elem(&futex_task_map, &pid);

    if (task)
        return task;
    struct futex_task init = {};

    bpf_get_current_comm(&init.comm, sizeof(init.comm));
    bpf_map_update_elem(&futex_task_map, &pid, &init, BPF_NOEXIST);
    return bpf_map_lookup_elem(&futex_task_map, &pid);
}

SEC("tracepoint/syscalls/sys_enter_futex")
int futex_enter(struct trace_futex_enter *ctx)
{
    __u64 key = bpf_get_current_pid_tgid();
    struct futex_start start = {};

    start.ts = bpf_ktime_get_ns();
    start.op = (__u32)ctx->op & FUTEX_CMD_MASK;
    bpf_map_update_elem(&futex_start_map, &key, &start, BPF_ANY);
    return 0;
}

SEC("tracepoint/syscalls/sys_exit_futex")
int futex_exit(struct trace_futex_exit *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 pid = (__u32)(pid_tgid >> 32);
    __u32 zero = 0;
    struct futex_start *start;
    struct futex_global *global;
    struct futex_task *task;
    __u64 delta;
    __u32 op;
    __s64 ret;

    start = bpf_map_lookup_elem(&futex_start_map, &pid_tgid);
    if (!start)
        return 0;

    delta = bpf_ktime_get_ns() - start->ts;
    op = start->op;
    ret = ctx->ret;
    bpf_map_delete_elem(&futex_start_map, &pid_tgid);

    if (futex_is_wait(op)) {
        if (ret == 0) {
            /* 真正睡着后被成功唤醒：唯一的直方图样本来源。 */
            __u32 bucket = go2cpp_hist_bucket(delta) &
                           (GO2CPP_HIST_BUCKETS - 1);

            global = bpf_map_lookup_elem(&futex_global_map, &zero);
            if (global) {
                __sync_fetch_and_add(&global->wait_count, 1);
                __sync_fetch_and_add(&global->wait_ns, delta);
                if (delta > global->max_wait_ns)
                    global->max_wait_ns = delta;
                __sync_fetch_and_add(&global->hist[bucket], 1);
            }
            task = ensure_task(pid);
            if (task) {
                __sync_fetch_and_add(&task->wait_count, 1);
                __sync_fetch_and_add(&task->wait_ns, delta);
                if (delta > task->max_wait_ns)
                    task->max_wait_ns = delta;
            }
        } else if (ret == -GO2CPP_EAGAIN) {
            /* 值不匹配，内核没有入等待队列：不计时长。 */
            global = bpf_map_lookup_elem(&futex_global_map, &zero);
            if (global)
                __sync_fetch_and_add(&global->eagain_count, 1);
            task = ensure_task(pid);
            if (task)
                __sync_fetch_and_add(&task->eagain_count, 1);
        } else if (ret == -GO2CPP_ETIMEDOUT) {
            global = bpf_map_lookup_elem(&futex_global_map, &zero);
            if (global)
                __sync_fetch_and_add(&global->timedout_count, 1);
            task = ensure_task(pid);
            if (task)
                __sync_fetch_and_add(&task->timedout_count, 1);
        } else {
            /* -EINTR 等：语义不纯，单独计错，不计时长。 */
            global = bpf_map_lookup_elem(&futex_global_map, &zero);
            if (global)
                __sync_fetch_and_add(&global->error_count, 1);
            task = ensure_task(pid);
            if (task)
                __sync_fetch_and_add(&task->error_count, 1);
        }
    } else if (futex_is_wake(op)) {
        global = bpf_map_lookup_elem(&futex_global_map, &zero);
        if (global)
            __sync_fetch_and_add(&global->wake_count, 1);
        task = ensure_task(pid);
        if (task)
            __sync_fetch_and_add(&task->wake_count, 1);
    }
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
