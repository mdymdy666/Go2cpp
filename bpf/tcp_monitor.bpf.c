// SPDX-License-Identifier: GPL-2.0
//
// tcp_monitor.bpf.c —— TCP 重传与 RTT 统计（go2cpp::os::monitor::TcpMonitor）。
//
// 挂两个 tracepoint：
//   tcp:tcp_retransmit_skb  每重传一个 skb 计一次（按连接 + 全局）
//   tcp:tcp_probe           每次收到 ACK 采样 srtt（内核已平滑的 RTT，单位 µs）
//
// 为什么用这两个 tracepoint 而不是 kprobe tcp_rcv_established / CO-RE 读
// tcp_sock：两个 tracepoint 的 format 已经直接给出四元组（地址 + 端口 +
// family），tcp_probe 还给出 srtt。这样不需要 vmlinux.h/CO-RE，字节码不依赖
// 内核内部结构偏移，跨内核小版本可复用；WSL2 6.6.87.2 实测两个 tracepoint
// 都存在（`/sys/kernel/tracing/events/tcp/`）。
//
// 关键实现约束：tracepoint 的 ctx 只能按「ctx + 编译期常量偏移」做标量读取。
// 若用 `memcpy(key.saddr, ctx->saddr, 16)`，clang 会先生成 ctx+8 指针再取
// +12 偏移，verifier 直接报 "dereference of modified ctx ptr"（实测 -EACCES）。
// 因此下面的 ctx 结构体把地址拆成 u32/u16 标量字段，组合写进栈上的 key；
// 地址字节序按内存原样保存，用户态用 in_addr/in6_addr 直接解释。
//
// 采样：tcp_probe 每收到一个 ACK 触发一次，全系统高速连接下每秒可达数万次。
// 按 1/8 采样既保留足够的 p50/p99 样本，又把 map 更新开销压到可忽略；采样
// 计数用全局 ARRAY 的一条 u64，__sync_fetch_and_add 保证多核一致。
//
// RTT 直方图口径（P2 修复）：
//   - rtt_hist：1/8 ACK 采样样本，每个样本权重 1。tcp_probe 的 srtt 本身是
//     内核平滑值，不是单个报文的 RTT；用「收到 ACK 的次数」近似报文加权，
//     大连接（ACK 多）自然占更大权重，但不是严格的按字节/报文加权。
//   - conn_rtt_hist：**每连接一个样本**（该连接第一次被采样时的 srtt），
//     计数单位是连接数。它回答「典型连接的 RTT 分布」，不受长连接的高
//     ACK 数淹没；两条直方图分别对应用户态 rtt_* 与 conn_rtt_* 字段。
//   - 不用 data_len 加权：srtt 是平滑值，按字节加权会把「RTT 分布」变成
//     「按字节数加权的 RTT 分布」，含义不直观且大包连接会压倒一切；这里
//     保留两种明确口径，而不是造一个解释不清的第三种。

#include <linux/bpf.h>
#include <linux/types.h>
#include <bpf/bpf_helpers.h>

#include "monitor_hist.h"

/* 同时存在的连接数上限；超出后新连接不进入统计（已有连接继续累加）。 */
#define GO2CPP_TCP_CONN_MAX 4096

/* RTT 采样掩码：1/8。改动时必须与用户态文档中的说明同步。 */
#define GO2CPP_TCP_RTT_SAMPLE_MASK 7

/* 四元组。saddr/daddr 是原始网络序字节按 u32 存放（IPv4 只用 [0]），
 * 用户态把 saddr[0] 的内存字节直接当作 in_addr，不需要再做端序转换。 */
struct tcp_conn_key {
    __u32 saddr[4];
    __u32 daddr[4];
    __u16 sport;
    __u16 dport;
    __u16 family;
    __u16 pad;
};

/* 每连接累计值；布局必须与用户态 TcpConnRaw 逐字节一致。 */
struct tcp_conn {
    __u64 retrans;
    __u64 rtt_count;
    __u64 rtt_us_sum;
    __u32 rtt_max_us;
    __u32 pad;
    __u64 rtt_hist[GO2CPP_HIST_BUCKETS];
};

/* 全局累计值 + 两份 RTT 直方图 + 采样序号。
 *
 * rtt_hist 是 1/8 ACK 采样加权（每样本 1）；conn_rtt_hist 是连接加权
 * （每连接只在首次被采样时写一个样本）。两者计数单位不同，用户态分别
 * 用 rtt_samples 与 conn_rtt_samples 解释，不能相加。 */
struct tcp_global {
    __u64 retrans;
    __u64 rtt_count;
    __u64 rtt_us_sum;
    __u32 rtt_max_us;
    __u32 pad;
    __u64 rtt_hist[GO2CPP_HIST_BUCKETS];
    __u64 probe_seq;
    __u64 conn_rtt_count;
    __u64 conn_rtt_hist[GO2CPP_HIST_BUCKETS];
};

/* 每连接累计值。LRU：连接关闭后不会再有事件来删除条目，固定 HASH 在
 * 累计建立过 4096 条连接后会被历史连接占满，新连接 ensure_conn 永远
 * 失败、top 列表定格在旧连接上。LRU 按最近使用淘汰已关闭的连接。 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, GO2CPP_TCP_CONN_MAX);
    __type(key, struct tcp_conn_key);
    __type(value, struct tcp_conn);
} tcp_conn_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct tcp_global);
} tcp_global_map SEC(".maps");

/* tracepoint/tcp/tcp_retransmit_skb 的 format（6.6）：
 *   skbaddr @8, skaddr @16, state @24, sport @28, dport @30, family @32,
 *   saddr[4] @34, daddr[4] @38, saddr_v6[16] @42, daddr_v6[16] @58。
 * v4 地址在 34/38 处未按 4 字节对齐，拆成 u16 对读取后用移位拼成 u32。 */
struct trace_tcp_retransmit {
    __u64 unused;
    __u64 skbaddr;
    __u64 skaddr;
    __s32 state;
    __u16 sport;
    __u16 dport;
    __u16 family;
    __u16 saddr_lo;
    __u16 saddr_hi;
    __u16 daddr_lo;
    __u16 daddr_hi;
    __u16 saddr_v6[8];
    __u16 daddr_v6[8];
};

/* tracepoint/tcp/tcp_probe 的 format（6.6）：
 *   saddr[28] @8, daddr[28] @36, sport @64, dport @66, family @68,
 *   mark @72, data_len @76, ..., srtt @100, rcv_wnd @104, sock_cookie @112。
 *
 * 注意 saddr/daddr 不是裸地址，而是 struct sockaddr_storage：
 *   +0  family（u16，AF_INET=2）
 *   +2  port（u16，网络序）
 *   +4  IPv4 地址（4 字节）；IPv6 时地址在 +8（16 字节）
 * 证据：tracefs 的 print fmt 用 %pISpc 直接解析这两个数组；实测 eBPF 从
 * @8 读 u32 得到的是 family|port 的组合值，真正的 127.0.0.1 在 @12。
 * 因此下面把数组按 u32 下标取：IPv4 用 [1]（+4），IPv6 用 [2..5]（+8..23，
 * 即 saddr[2]、saddr[3]、saddr_pad[0]、saddr_pad[1]）。 */
struct trace_tcp_probe {
    __u64 unused;
    __u32 saddr[4]; /* @8：IPv4 只用 [0] */
    __u32 saddr_pad[3];
    __u32 daddr[4]; /* @36 */
    __u32 daddr_pad[3];
    __u16 sport; /* @64 */
    __u16 dport; /* @66 */
    __u16 family;
    __u16 pad0;
    __u32 mark;
    __u16 data_len;
    __u16 pad1;
    __u32 snd_nxt;
    __u32 snd_una;
    __u32 snd_cwnd;
    __u32 ssthresh;
    __u32 snd_wnd;
    __u32 srtt; /* @100，单位 µs（内核已把 srtt_us >> 3） */
    __u32 rcv_wnd;
    __u32 pad2;
    __u64 sock_cookie;
};

/*
 * 取（或创建）连接统计项。
 *
 * 同一四元组的首个事件可能在不同 CPU 上并发到达（例如重传定时器与 ACK
 * 软中断同时落在一张新连接上）。插入用 BPF_NOEXIST，失败只表示另一个 CPU
 * 已经建好条目；无论插入成功与否都重新 lookup，把指针交给调用方原子累加，
 * 并发首事件不会丢样本（与 net_stats.bpf.c 的 update_stats 同一修法）。
 */
static __always_inline struct tcp_conn *ensure_conn(
    const struct tcp_conn_key *key)
{
    struct tcp_conn *conn = bpf_map_lookup_elem(&tcp_conn_map, key);
    struct tcp_conn init = {};

    if (conn)
        return conn;
    (void)bpf_map_update_elem(&tcp_conn_map, key, &init, BPF_NOEXIST);
    return bpf_map_lookup_elem(&tcp_conn_map, key);
}

SEC("tracepoint/tcp/tcp_retransmit_skb")
int tcp_retransmit(struct trace_tcp_retransmit *ctx)
{
    struct tcp_conn_key key = {};
    struct tcp_conn *conn;
    struct tcp_global *global;
    __u32 zero = 0;

    key.sport = ctx->sport;
    key.dport = ctx->dport;
    key.family = ctx->family;
    if (ctx->family == 2) { /* AF_INET：v4 地址在未对齐的 34/38 */
        key.saddr[0] = (__u32)ctx->saddr_lo | ((__u32)ctx->saddr_hi << 16);
        key.daddr[0] = (__u32)ctx->daddr_lo | ((__u32)ctx->daddr_hi << 16);
    } else {
        key.saddr[0] = (__u32)ctx->saddr_v6[0] |
                       ((__u32)ctx->saddr_v6[1] << 16);
        key.saddr[1] = (__u32)ctx->saddr_v6[2] |
                       ((__u32)ctx->saddr_v6[3] << 16);
        key.saddr[2] = (__u32)ctx->saddr_v6[4] |
                       ((__u32)ctx->saddr_v6[5] << 16);
        key.saddr[3] = (__u32)ctx->saddr_v6[6] |
                       ((__u32)ctx->saddr_v6[7] << 16);
        key.daddr[0] = (__u32)ctx->daddr_v6[0] |
                       ((__u32)ctx->daddr_v6[1] << 16);
        key.daddr[1] = (__u32)ctx->daddr_v6[2] |
                       ((__u32)ctx->daddr_v6[3] << 16);
        key.daddr[2] = (__u32)ctx->daddr_v6[4] |
                       ((__u32)ctx->daddr_v6[5] << 16);
        key.daddr[3] = (__u32)ctx->daddr_v6[6] |
                       ((__u32)ctx->daddr_v6[7] << 16);
    }

    conn = ensure_conn(&key);
    if (conn)
        __sync_fetch_and_add(&conn->retrans, 1);

    global = bpf_map_lookup_elem(&tcp_global_map, &zero);
    if (global)
        __sync_fetch_and_add(&global->retrans, 1);
    return 0;
}

SEC("tracepoint/tcp/tcp_probe")
int tcp_rtt_probe(struct trace_tcp_probe *ctx)
{
    struct tcp_conn_key key = {};
    struct tcp_conn *conn;
    struct tcp_global *global;
    __u32 zero = 0;
    __u32 srtt_us;
    __u32 bucket;
    __u64 seq;

    global = bpf_map_lookup_elem(&tcp_global_map, &zero);
    if (!global)
        return 0;
    seq = __sync_fetch_and_add(&global->probe_seq, 1);
    if ((seq & GO2CPP_TCP_RTT_SAMPLE_MASK) != 0)
        return 0;

    srtt_us = ctx->srtt;
    if (srtt_us == 0)
        return 0;
    bucket = go2cpp_hist_bucket((__u64)srtt_us * 1000ULL) &
             (GO2CPP_HIST_BUCKETS - 1);

    key.sport = ctx->sport;
    key.dport = ctx->dport;
    key.family = ctx->family;
    if (ctx->family == 2) {
        key.saddr[0] = ctx->saddr[1]; /* sockaddr_storage +4 */
        key.daddr[0] = ctx->daddr[1];
    } else {
        key.saddr[0] = ctx->saddr[2]; /* sockaddr_in6 +8 */
        key.saddr[1] = ctx->saddr[3];
        key.saddr[2] = ctx->saddr_pad[0];
        key.saddr[3] = ctx->saddr_pad[1];
        key.daddr[0] = ctx->daddr[2];
        key.daddr[1] = ctx->daddr[3];
        key.daddr[2] = ctx->daddr_pad[0];
        key.daddr[3] = ctx->daddr_pad[1];
    }

    conn = ensure_conn(&key);
    if (conn) {
        /* 返回旧值：0 表示这是该连接的第一个采样样本，同时写入连接加权
         * 直方图（每连接一个 srtt 样本，计数单位 = 连接数）。 */
        __u64 previous = __sync_fetch_and_add(&conn->rtt_count, 1);
        __sync_fetch_and_add(&conn->rtt_us_sum, (__u64)srtt_us);
        if (srtt_us > conn->rtt_max_us)
            conn->rtt_max_us = srtt_us;
        __sync_fetch_and_add(&conn->rtt_hist[bucket], 1);
        if (previous == 0) {
            __sync_fetch_and_add(&global->conn_rtt_count, 1);
            __sync_fetch_and_add(&global->conn_rtt_hist[bucket], 1);
        }
    }

    __sync_fetch_and_add(&global->rtt_count, 1);
    __sync_fetch_and_add(&global->rtt_us_sum, (__u64)srtt_us);
    if (srtt_us > global->rtt_max_us)
        global->rtt_max_us = srtt_us;
    __sync_fetch_and_add(&global->rtt_hist[bucket], 1);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
