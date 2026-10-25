// SPDX-License-Identifier: GPL-2.0
//
// sched_latency.bpf.c —— 调度延迟统计（go2cpp::os::monitor::SchedMonitor）。
//
// 挂三个 tracepoint：
//   sched:sched_wakeup  pid 被唤醒 -> 记录唤醒时间戳
//   sched:sched_switch  prev 换出（累计 on-cpu 时长）与 next 换入
//                       （next 若有唤醒时间戳，算出 runqueue 等待）
//
// runqueue wait 的口径：从 sched_wakeup 到该任务真正获得 CPU 的时间。
// 这是一个近似值：
//   - 唤醒后被迁移到别的 CPU 仍能配对（key 是全局 pid，不是 per-cpu）；
//   - 启动监控前已在 runqueue 上、或由 sched_wakeup_new 唤醒的任务没有起点，
//     不计入等待样本；
//   - sched_wakeup 可能对同一任务重复触发，后一次覆盖前一次，等待偏短。
//
// on-cpu 时间用「上次 switch-in 时间戳 -> 本次 switch-out」计算，跨 CPU 迁移
// 也成立；监控 attach 之前就在运行的任务第一次换出时没有起点，只计 switches。
//
// 开销设计：switch-in 时间戳常驻（prev 换出时不删除，next 换入时覆盖），
// 每次 switch 少一次 map delete。

#include <linux/bpf.h>
#include <linux/types.h>
#include <bpf/bpf_helpers.h>

#include "monitor_hist.h"

/* 每任务累计值；value 布局必须与用户态一致。 */
struct sched_task {
    char comm[16];
    __u64 on_cpu_ns;
    __u64 switches;
    __u64 wait_ns;
    __u64 wait_count;
    __u64 max_wait_ns;
};

/* 全局累计值 + runqueue wait 直方图。 */
struct sched_global {
    __u64 switches;
    __u64 wait_count;
    __u64 wait_ns;
    __u64 max_wait_ns;
    __u64 hist[GO2CPP_HIST_BUCKETS];
};

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 16384);
    __type(key, __u32);
    __type(value, __u64);
} sched_on_cpu_map SEC(".maps");

/* 唤醒时间戳。用 LRU 而不是 HASH：被唤醒后又直接退出、或 pid 在
 * switch-in 前被回收的任务会留下永远不会消费的条目；HASH 满 16384 后
 * 新的唤醒 update 失败，runqueue wait 样本会静默变成 0。LRU 让最旧的
 * 残留项自动淘汰。 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 16384);
    __type(key, __u32);
    __type(value, __u64);
} sched_wakeup_map SEC(".maps");

/* 每任务累计值。LRU：长跑后 pid 复用/大量短期任务会把固定 HASH 占满，
 * 导致后来真正活跃的任务永远插不进去（top 列表停在旧数据）。淘汰代价
 * 只是极不活跃任务的累计值归零。 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, struct sched_task);
} sched_task_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct sched_global);
} sched_global_map SEC(".maps");

struct trace_sched_switch {
    __u64 unused;
    char prev_comm[16];
    __s32 prev_pid;
    __s32 prev_prio;
    __s64 prev_state;
    char next_comm[16];
    __s32 next_pid;
    __s32 next_prio;
};

struct trace_sched_wakeup {
    __u64 unused;
    char comm[16];
    __s32 pid;
    __s32 prio;
    __s32 target_cpu;
};

/* 取（或创建）任务统计项；comm 由调用方从 tracepoint 上下文提供。 */
static __always_inline struct sched_task *ensure_task(__u32 pid,
                                                      const char *comm)
{
    struct sched_task *task = bpf_map_lookup_elem(&sched_task_map, &pid);

    if (task)
        return task;
    struct sched_task init = {};

    __builtin_memcpy(init.comm, comm, 16);
    /* 并发首次插入只有一个成功，失败方下次 switch 会重试。 */
    bpf_map_update_elem(&sched_task_map, &pid, &init, BPF_NOEXIST);
    return bpf_map_lookup_elem(&sched_task_map, &pid);
}

SEC("tracepoint/sched/sched_wakeup")
int sched_wakeup(struct trace_sched_wakeup *ctx)
{
    __u32 pid = (__u32)ctx->pid;
    __u64 ts = bpf_ktime_get_ns();

    if (pid == 0)
        return 0;
    bpf_map_update_elem(&sched_wakeup_map, &pid, &ts, BPF_ANY);
    return 0;
}

SEC("tracepoint/sched/sched_switch")
int sched_switch(struct trace_sched_switch *ctx)
{
    __u64 now = bpf_ktime_get_ns();
    __u32 prev_pid = (__u32)ctx->prev_pid;
    __u32 next_pid = (__u32)ctx->next_pid;
    __u32 zero = 0;
    struct sched_global *global;
    struct sched_task *task;
    __u64 *started;
    __u64 *wake;

    global = bpf_map_lookup_elem(&sched_global_map, &zero);
    if (global)
        __sync_fetch_and_add(&global->switches, 1);

    /* prev：换出 -> 结算 on-cpu 时间。pid 0 是 idle，不统计。 */
    if (prev_pid != 0) {
        started = bpf_map_lookup_elem(&sched_on_cpu_map, &prev_pid);
        if (started) {
            __u64 on_cpu = now - *started;

            task = ensure_task(prev_pid, ctx->prev_comm);
            if (task) {
                __sync_fetch_and_add(&task->on_cpu_ns, on_cpu);
                __sync_fetch_and_add(&task->switches, 1);
            }
        }
    }

    if (next_pid == 0)
        return 0;

    /* next：记录 switch-in 时间，供下一次换出时结算 on-cpu。 */
    started = bpf_map_lookup_elem(&sched_on_cpu_map, &next_pid);
    if (started)
        *started = now;
    else
        bpf_map_update_elem(&sched_on_cpu_map, &next_pid, &now, BPF_ANY);

    /* next 有唤醒时间戳 -> 产生一条 runqueue wait 样本。 */
    wake = bpf_map_lookup_elem(&sched_wakeup_map, &next_pid);
    if (wake) {
        __u64 wait = now - *wake;
        __u32 bucket = go2cpp_hist_bucket(wait) & (GO2CPP_HIST_BUCKETS - 1);

        bpf_map_delete_elem(&sched_wakeup_map, &next_pid);
        global = bpf_map_lookup_elem(&sched_global_map, &zero);
        if (global) {
            __sync_fetch_and_add(&global->wait_count, 1);
            __sync_fetch_and_add(&global->wait_ns, wait);
            if (wait > global->max_wait_ns)
                global->max_wait_ns = wait;
            __sync_fetch_and_add(&global->hist[bucket], 1);
        }
        task = ensure_task(next_pid, ctx->next_comm);
        if (task) {
            __sync_fetch_and_add(&task->wait_ns, wait);
            __sync_fetch_and_add(&task->wait_count, 1);
            if (wait > task->max_wait_ns)
                task->max_wait_ns = wait;
        }
    }
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
