// SPDX-License-Identifier: GPL-2.0
//
// monitor_hist.h —— 三个 tracepoint 监控程序共用的对数延迟直方图工具。
//
// 桶定义（与用户态 go2cpp::os::monitor::KernelHistUpperUs 完全一致）：
//   bucket 0        : [0, 1µs)
//   bucket i (1..30): [2^(i-1) µs, 2^i µs)
//   bucket 31       : [2^30 µs, +∞)（约 17.9 分钟以上，做溢出桶）
//
// 为什么不用 __builtin_clz：BPF 指令集没有 clz，clang 会把它降级成
// __clzsi2 libcall，而 BPF 后端不支持 libcall，编译直接失败。这里用 5 次
// 比较+移位手算 floor(log2)，对 verifier 也是完全可展开的直线代码。

#ifndef GO2CPP_BPF_MONITOR_HIST_H
#define GO2CPP_BPF_MONITOR_HIST_H

#include <linux/types.h>

/* 与用户态 kKernelHistBuckets 保持一致，改动时必须同步两边。 */
#define GO2CPP_HIST_BUCKETS 32

static __always_inline __u32 go2cpp_hist_bucket(__u64 delta_ns)
{
    __u32 value;
    __u32 log2 = 0;
    __u32 bucket;
    __u64 us;

    if (delta_ns < 1000ULL)
        return 0; /* 亚微秒：从 raw_syscalls 采样时非常常见 */

    us = delta_ns / 1000ULL;
    /* 桶号用 u32 的微秒值算 log2。futex 等长等待可能超过 71.6 分钟
     * （u32 微秒上限），直接截断会得到一个偏小甚至接近 0 的 value，
     * 把长等待错误地记进小桶。先判溢出，统一落进溢出桶 31。 */
    if (us > 0xFFFFFFFFULL)
        return 31;
    value = (__u32)us;
    if (value == 0)
        return 1;
    if (value >= (1U << 16)) {
        value >>= 16;
        log2 += 16;
    }
    if (value >= (1U << 8)) {
        value >>= 8;
        log2 += 8;
    }
    if (value >= (1U << 4)) {
        value >>= 4;
        log2 += 4;
    }
    if (value >= (1U << 2)) {
        value >>= 2;
        log2 += 2;
    }
    if (value >= (1U << 1))
        log2 += 1;

    bucket = log2 + 1;
    if (bucket > 31)
        bucket = 31;
    return bucket;
}

#endif /* GO2CPP_BPF_MONITOR_HIST_H */
