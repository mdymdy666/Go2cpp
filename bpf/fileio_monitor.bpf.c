// SPDX-License-Identifier: GPL-2.0
//
// fileio_monitor.bpf.c —— 文件 IO 延迟统计（go2cpp::os::monitor::FileIoMonitor）。
//
// 用 kprobe/kretprobe 包住 vfs_read / vfs_write：
//   entry  记录 (pid_tgid -> 起始 ns)
//   exit   计算 delta（含排队、缺页、等待块设备的时间），按全局与进程累计
//          count / total_ns / max_ns / 32 桶对数直方图
//
// 为什么挂 vfs_read / vfs_write 而不是 block:block_rq_complete：vfs_* 是进程
// 上下文里的文件 IO 入口，能直接拿到 pid/comm（按进程 top 的前提），并且覆盖
// 页缓存命中（亚微秒）、缺页回读与真正落盘（毫秒）三种情况；块设备层只有
// 「设备延迟」，没有进程归属，且会被文件系统预读/合并放大或掩盖。
// 设备/文件系统维度的延迟仍可通过 DiskMonitor 的 /proc/diskstats 平均延迟观察。
//
// 只统计成功且有实际数据量的调用（返回值 > 0）：失败/返回 0 的调用没有数据
// 面延迟，混进直方图会把 p50 拉低。
//
// 覆盖范围（io_uring/read_iter P2 修复）：
//   - 普通 read/pread 走 vfs_read -> new_sync_read -> call_read_iter，
//     vfs_read 的入口延迟已经包含 read_iter，不重复计数；
//   - readv/writev 走 vfs_readv/vfs_writev（由 do_readv/do_preadv 调用，
//     内部 import_iovec + call_read_iter），**不经过 vfs_read**；6.6 实测
//     vfs_readv 也不经过 vfs_iter_read（ftrace 计数 0），必须单独挂
//     vfs_readv/vfs_writev；
//   - io_uring 走 io_read/io_write -> io_iter_do_read -> call_read_iter，
//     同样**绕过 vfs_read**，必须挂 io_read/io_write。
// 三组路径在同一线程内严格互不嵌套（内核不会在 vfs_read 里再调
// vfs_readv/io_read），因此各自使用独立的 start map，互不干扰。
//
// 为什么进程级只存 count/total/max、不存直方图：BPF 栈上限 512 字节，
// 584 字节的 per-task value（含两份 32 桶直方图）无法在栈上构造，必须用
// per-cpu 临时 map 或把 value 拆小。进程级 p50/p99 的价值低于实现复杂度与
// 内存（1024 进程 × 584B ≈ 600KB），因此分位数只做全局，进程维度给
// count/avg/max；全局直方图直接操作 map 指针，不占栈。
//
// 开销：每次 IO 一次 entry map update + exit 一次 lookup/delete + 4 次原子加
// （全局 3 次 + 直方图 1 次）+ 进程 3 次原子加。按 4KB 顺序读折算，每次 IO
// 跟踪成本约 0.3–0.5µs，远小于一次真实 IO 的系统调用开销。

#include <linux/bpf.h>
#include <linux/types.h>
#include <asm/ptrace.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "monitor_hist.h"

/* 单进程统计的 map 容量；超出后新进程不进入 top（全局统计不受影响）。 */
#define GO2CPP_FILEIO_TASK_MAX 1024

struct fileio_start {
    __u64 ts;
};

/* 每进程累计值；布局必须与用户态 FileIoTaskRaw 逐字节一致。 */
struct fileio_task {
    char comm[16];
    __u64 read_count;
    __u64 read_ns;
    __u64 read_max_ns;
    __u64 write_count;
    __u64 write_ns;
    __u64 write_max_ns;
    /* 采集端当前 pid namespace 的 tgid；0 = 未解析。两个 u32 把结构体填到
     * 8 字节对齐的 72 字节，与用户态 FileIoTaskRaw 的 static_assert 对应。 */
    __u32 ns_pid;
    __u32 pad;
};

/* pid namespace 映射配置：用户态 Init 时写入 /proc/self/ns/pid 的
 * st_dev/st_ino。bpf_get_ns_current_pid_tgid 要求 dev/ino 与当前任务的
 * nsfs 匹配，用它把 init-ns tgid 换算成采集端当前 ns 的 tgid（ps 可见）。 */
struct fileio_ns_info {
    __u64 dev;
    __u64 ino;
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct fileio_ns_info);
} fileio_ns_map SEC(".maps");

/* 全局累计值；布局必须与用户态 FileIoGlobalRaw 逐字节一致。
 *
 * 除 vfs_read/vfs_write 的延迟直方图外，另记三组「覆盖计数」：
 *   - iter_*：vfs_iter_read/vfs_iter_write（readv/writev 等，绕过 vfs_read）；
 *   - uring_*：io_uring 的 io_read/io_write，只有同步完成（返回值 0）才有
 *     可信延迟；异步（-EIOCBQUEUED）只累加 pending 计数——异步 IO 的完成
 *     发生在 io-wq/软中断里，kretprobe 返回时延迟尚未结束。
 * 覆盖计数只给 count/total/max，不建直方图：保持 ARRAY value 小、用户态读取
 * 便宜；分位数仍只由 vfs_read/vfs_write 的直方图给出（文档已写明）。
 */
struct fileio_global {
    __u64 read_count;
    __u64 read_ns;
    __u64 read_max_ns;
    __u64 write_count;
    __u64 write_ns;
    __u64 write_max_ns;
    __u64 read_hist[GO2CPP_HIST_BUCKETS];
    __u64 write_hist[GO2CPP_HIST_BUCKETS];
    __u64 iter_read_count;
    __u64 iter_read_ns;
    __u64 iter_read_max_ns;
    __u64 iter_write_count;
    __u64 iter_write_ns;
    __u64 iter_write_max_ns;
    __u64 uring_read_count;
    __u64 uring_read_ns;
    __u64 uring_read_max_ns;
    __u64 uring_write_count;
    __u64 uring_write_ns;
    __u64 uring_write_max_ns;
    __u64 uring_read_pending;
    __u64 uring_write_pending;
};

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 16384);
    __type(key, __u64);
    __type(value, struct fileio_start);
} fileio_read_start_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 16384);
    __type(key, __u64);
    __type(value, struct fileio_start);
} fileio_write_start_map SEC(".maps");

/* readv/writev（vfs_readv/vfs_writev）与 io_uring（io_read/io_write）的起始时间。
 * 容量 8192：这些路径的同一时刻并发数远低于普通 read/write；LRU 在异常
 * 丢 exit 事件时也不会耗尽内存。 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 8192);
    __type(key, __u64);
    __type(value, struct fileio_start);
} fileio_iter_read_start_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 8192);
    __type(key, __u64);
    __type(value, struct fileio_start);
} fileio_iter_write_start_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 8192);
    __type(key, __u64);
    __type(value, struct fileio_start);
} fileio_uring_read_start_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 8192);
    __type(key, __u64);
    __type(value, struct fileio_start);
} fileio_uring_write_start_map SEC(".maps");

/* 每进程累计值；LRU 避免长期运行后 map 被已退出进程占满，新进程
 * 无法进入 top（全局统计仍由 ARRAY 承担，不受影响）。 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, GO2CPP_FILEIO_TASK_MAX);
    __type(key, __u32);
    __type(value, struct fileio_task);
} fileio_task_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct fileio_global);
} fileio_global_map SEC(".maps");

/*
 * 把 bpf_get_current_pid_tgid 的 init-ns tgid 映射成采集端当前 ns 的 tgid。
 *
 * bpf_get_current_pid_tgid 返回的是 init PID namespace 的编号；在嵌套 pid
 * namespace（WSL2 容器/容器/unshare -p）里与 getpid()/ps 看到的不同，直接用
 * 会让用户态无法把 top 条目与 ps 对上。bpf_get_ns_current_pid_tgid 接受
 * nsfs 的 dev/ino（用户态从 /proc/self/ns/pid 的 stat 取得，写入
 * fileio_ns_map），返回当前 namespace 的 pid/tgid。未配置或 helper 失败时
 * 保持 0，用户态再用 /proc NSpid 做 best-effort 回退。
 */
static __always_inline void fill_ns_pid(struct fileio_task *task)
{
    __u32 zero = 0;
    struct fileio_ns_info *ns = bpf_map_lookup_elem(&fileio_ns_map, &zero);
    struct bpf_pidns_info info = {};

    if (!ns || ns->ino == 0)
        return;
    if (bpf_get_ns_current_pid_tgid(ns->dev, ns->ino, &info, sizeof(info)) == 0)
        task->ns_pid = info.tgid;
}

/*
 * 取（或创建）进程统计项。
 *
 * 首事件可能在同一进程的多个线程上并发发生（key 是 pid 而不是 tid），因此
 * 插入必须用 BPF_NOEXIST：BPF_ANY 会让后插入的 CPU 用全零 init 覆盖先插入者
 * 已累计的计数。插入失败只说明另一个 CPU 已经建好条目 —— 无论成功与否都
 * 重新 lookup，由调用方 __sync_fetch_and_add 累加，并发首事件一条都不丢。
 * 不能返回插入前那个可能为 NULL 的指针：helper 调用后旧 map value 指针失效。
 */
static __always_inline struct fileio_task *ensure_task(__u32 pid)
{
    struct fileio_task *task = bpf_map_lookup_elem(&fileio_task_map, &pid);
    struct fileio_task init = {};

    if (task)
        return task;
    bpf_get_current_comm(&init.comm, sizeof(init.comm));
    (void)bpf_map_update_elem(&fileio_task_map, &pid, &init, BPF_NOEXIST);
    /* 并发首事件下 BPF_NOEXIST 的插入与随后 lookup 之间，另一个 CPU 可能
     * 正在完成同一 key 的插入；lookup 偶发返回 NULL 会丢掉该样本。重试
     * 两次（无 helper 开销可忽略）以消除这个窗口。 */
#pragma unroll
    for (int i = 0; i < 3; ++i) {
        struct fileio_task *retry =
            bpf_map_lookup_elem(&fileio_task_map, &pid);
        if (retry)
            return retry;
    }
    return 0;
}

SEC("kprobe/vfs_read")
int BPF_KPROBE(fileio_read_enter)
{
    __u64 key = bpf_get_current_pid_tgid();
    struct fileio_start start = {};

    start.ts = bpf_ktime_get_ns();
    bpf_map_update_elem(&fileio_read_start_map, &key, &start, BPF_ANY);
    return 0;
}

SEC("kretprobe/vfs_read")
int fileio_read_exit(struct pt_regs *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 pid = (__u32)(pid_tgid >> 32);
    __u32 zero = 0;
    struct fileio_start *start;
    struct fileio_global *global;
    struct fileio_task *task;
    __u64 delta;
    __u32 bucket;

    start = bpf_map_lookup_elem(&fileio_read_start_map, &pid_tgid);
    if (!start)
        return 0;
    delta = bpf_ktime_get_ns() - start->ts;
    bpf_map_delete_elem(&fileio_read_start_map, &pid_tgid);
    if (PT_REGS_RC(ctx) <= 0)
        return 0;

    bucket = go2cpp_hist_bucket(delta) & (GO2CPP_HIST_BUCKETS - 1);
    global = bpf_map_lookup_elem(&fileio_global_map, &zero);
    if (global) {
        __sync_fetch_and_add(&global->read_count, 1);
        __sync_fetch_and_add(&global->read_ns, delta);
        if (delta > global->read_max_ns)
            global->read_max_ns = delta;
        __sync_fetch_and_add(&global->read_hist[bucket], 1);
    }
    task = ensure_task(pid);
    if (task) {
        __sync_fetch_and_add(&task->read_count, 1);
        __sync_fetch_and_add(&task->read_ns, delta);
        if (delta > task->read_max_ns)
            task->read_max_ns = delta;
        /* ns 映射放到首事件之后：并发首事件的插入窗口里不做 helper 调用，
         * 从本进程的第二次 IO 起再补算（u32 对齐 store 是原子的）。 */
        if (task->ns_pid == 0 && task->read_count > 1)
            fill_ns_pid(task);
    }
    return 0;
}

SEC("kprobe/vfs_write")
int BPF_KPROBE(fileio_write_enter)
{
    __u64 key = bpf_get_current_pid_tgid();
    struct fileio_start start = {};

    start.ts = bpf_ktime_get_ns();
    bpf_map_update_elem(&fileio_write_start_map, &key, &start, BPF_ANY);
    return 0;
}

SEC("kretprobe/vfs_write")
int fileio_write_exit(struct pt_regs *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 pid = (__u32)(pid_tgid >> 32);
    __u32 zero = 0;
    struct fileio_start *start;
    struct fileio_global *global;
    struct fileio_task *task;
    __u64 delta;
    __u32 bucket;

    start = bpf_map_lookup_elem(&fileio_write_start_map, &pid_tgid);
    if (!start)
        return 0;
    delta = bpf_ktime_get_ns() - start->ts;
    bpf_map_delete_elem(&fileio_write_start_map, &pid_tgid);
    if (PT_REGS_RC(ctx) <= 0)
        return 0;

    bucket = go2cpp_hist_bucket(delta) & (GO2CPP_HIST_BUCKETS - 1);
    global = bpf_map_lookup_elem(&fileio_global_map, &zero);
    if (global) {
        __sync_fetch_and_add(&global->write_count, 1);
        __sync_fetch_and_add(&global->write_ns, delta);
        if (delta > global->write_max_ns)
            global->write_max_ns = delta;
        __sync_fetch_and_add(&global->write_hist[bucket], 1);
    }
    task = ensure_task(pid);
    if (task) {
        __sync_fetch_and_add(&task->write_count, 1);
        __sync_fetch_and_add(&task->write_ns, delta);
        if (delta > task->write_max_ns)
            task->write_max_ns = delta;
        /* 同 read 路径：首事件窗口不调用 helper。 */
        if (task->ns_pid == 0 && task->write_count > 1)
            fill_ns_pid(task);
    }
    return 0;
}


/*
 * readv/writev 路径：vfs_readv/vfs_writev。
 *
 * 返回值语义与 vfs_read/vfs_write 相同（ssize_t，>0 表示成功传输字节数）。
 * readv/preadv 的 syscall 实现是 do_readv/do_preadv -> vfs_readv，不经过
 * vfs_read；6.6 实测 vfs_readv 也不调用 vfs_iter_read（ftrace 计数 0），
 * 所以这一组计数与 read_count 独立、不重复。
 */
SEC("kprobe/vfs_readv")
int BPF_KPROBE(fileio_iter_read_enter)
{
    __u64 key = bpf_get_current_pid_tgid();
    struct fileio_start start = {};

    start.ts = bpf_ktime_get_ns();
    bpf_map_update_elem(&fileio_iter_read_start_map, &key, &start, BPF_ANY);
    return 0;
}

SEC("kretprobe/vfs_readv")
int fileio_iter_read_exit(struct pt_regs *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 zero = 0;
    struct fileio_start *start;
    struct fileio_global *global;
    __u64 delta;

    start = bpf_map_lookup_elem(&fileio_iter_read_start_map, &pid_tgid);
    if (!start)
        return 0;
    delta = bpf_ktime_get_ns() - start->ts;
    bpf_map_delete_elem(&fileio_iter_read_start_map, &pid_tgid);
    if (PT_REGS_RC(ctx) <= 0)
        return 0;

    global = bpf_map_lookup_elem(&fileio_global_map, &zero);
    if (global) {
        __sync_fetch_and_add(&global->iter_read_count, 1);
        __sync_fetch_and_add(&global->iter_read_ns, delta);
        if (delta > global->iter_read_max_ns)
            global->iter_read_max_ns = delta;
    }
    return 0;
}

SEC("kprobe/vfs_writev")
int BPF_KPROBE(fileio_iter_write_enter)
{
    __u64 key = bpf_get_current_pid_tgid();
    struct fileio_start start = {};

    start.ts = bpf_ktime_get_ns();
    bpf_map_update_elem(&fileio_iter_write_start_map, &key, &start, BPF_ANY);
    return 0;
}

SEC("kretprobe/vfs_writev")
int fileio_iter_write_exit(struct pt_regs *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 zero = 0;
    struct fileio_start *start;
    struct fileio_global *global;
    __u64 delta;

    start = bpf_map_lookup_elem(&fileio_iter_write_start_map, &pid_tgid);
    if (!start)
        return 0;
    delta = bpf_ktime_get_ns() - start->ts;
    bpf_map_delete_elem(&fileio_iter_write_start_map, &pid_tgid);
    if (PT_REGS_RC(ctx) <= 0)
        return 0;

    global = bpf_map_lookup_elem(&fileio_global_map, &zero);
    if (global) {
        __sync_fetch_and_add(&global->iter_write_count, 1);
        __sync_fetch_and_add(&global->iter_write_ns, delta);
        if (delta > global->iter_write_max_ns)
            global->iter_write_max_ns = delta;
    }
    return 0;
}

/*
 * io_uring 路径：io_read/io_write。
 *
 * 这两个函数的返回值是 `int`（不是字节数）：
 *   0            IOU_OK：本次 issue 同步完成，enter -> exit 的墙钟时间就是
 *                这次文件 IO 的可信延迟（含页缓存/块设备等待）；
 *   -EIOCBQUEUED 异步排队（buffered 阻塞读、io-wq 执行）：kretprobe 返回时
 *                IO 尚未完成，延迟不可测，只累加 pending 计数，绝不进延迟统计；
 *   其它负值     错误（同样只计 pending）。
 * 因此 io_uring 的延迟样本只覆盖「同步完成」的 IO；覆盖率与缺口在用户态
 * coverage/coverage_gap 与 docs/os-monitor.md 中明确写出。
 */
SEC("kprobe/io_read")
int BPF_KPROBE(fileio_uring_read_enter)
{
    __u64 key = bpf_get_current_pid_tgid();
    struct fileio_start start = {};

    start.ts = bpf_ktime_get_ns();
    bpf_map_update_elem(&fileio_uring_read_start_map, &key, &start, BPF_ANY);
    return 0;
}

SEC("kretprobe/io_read")
int fileio_uring_read_exit(struct pt_regs *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 zero = 0;
    int result = (int)PT_REGS_RC(ctx);
    struct fileio_start *start;
    struct fileio_global *global;
    __u64 delta;

    start = bpf_map_lookup_elem(&fileio_uring_read_start_map, &pid_tgid);
    if (!start)
        return 0;
    delta = bpf_ktime_get_ns() - start->ts;
    bpf_map_delete_elem(&fileio_uring_read_start_map, &pid_tgid);

    global = bpf_map_lookup_elem(&fileio_global_map, &zero);
    if (!global)
        return 0;
    if (result != 0) {
        __sync_fetch_and_add(&global->uring_read_pending, 1);
        return 0;
    }
    __sync_fetch_and_add(&global->uring_read_count, 1);
    __sync_fetch_and_add(&global->uring_read_ns, delta);
    if (delta > global->uring_read_max_ns)
        global->uring_read_max_ns = delta;
    return 0;
}

SEC("kprobe/io_write")
int BPF_KPROBE(fileio_uring_write_enter)
{
    __u64 key = bpf_get_current_pid_tgid();
    struct fileio_start start = {};

    start.ts = bpf_ktime_get_ns();
    bpf_map_update_elem(&fileio_uring_write_start_map, &key, &start, BPF_ANY);
    return 0;
}

SEC("kretprobe/io_write")
int fileio_uring_write_exit(struct pt_regs *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 zero = 0;
    int result = (int)PT_REGS_RC(ctx);
    struct fileio_start *start;
    struct fileio_global *global;
    __u64 delta;

    start = bpf_map_lookup_elem(&fileio_uring_write_start_map, &pid_tgid);
    if (!start)
        return 0;
    delta = bpf_ktime_get_ns() - start->ts;
    bpf_map_delete_elem(&fileio_uring_write_start_map, &pid_tgid);

    global = bpf_map_lookup_elem(&fileio_global_map, &zero);
    if (!global)
        return 0;
    if (result != 0) {
        __sync_fetch_and_add(&global->uring_write_pending, 1);
        return 0;
    }
    __sync_fetch_and_add(&global->uring_write_count, 1);
    __sync_fetch_and_add(&global->uring_write_ns, delta);
    if (delta > global->uring_write_max_ns)
        global->uring_write_max_ns = delta;
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
