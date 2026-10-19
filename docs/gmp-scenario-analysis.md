# GMP 各场景优劣分析（实测中）

> 对应 `docs/gmp-use-cases.md`「待补实验」的逐项验证。每轮追加一节。
> 所有数据同窗口 A/B 连续跑，记录 `/proc/loadavg` 第 4 字段。

## 实验 0：先确认 benchmark 的两种驱动方式（重要方法论）

`gmp_vs_thread_epoll_benchmark` 的 `run` 子命令**只起一个客户端**，而 s1（短连接）
的单线程 epoll 客户端在 ~8k QPS 就是瓶颈 —— 用它得到的数据**反映的是客户端上限**，
不是服务端差异：

```
[gmp/s1]    qps=7858   threads=14  errors=11
[thread/s1] qps=10991  threads=55  errors=0
```

这组数字**不能**用来对比服务端（thread 反而「更快」，因为它的客户端压力被 55 个线程
接住了）。**正确做法**是用多客户端驱动（`.dsh_tmp/_bench_manual.py`），4 个客户端 ×
16 连接。

## 实验 1 预演：s1 多客户端基线（GMP）

```
命令: _bench_manual.py --scenario s1 --conns 16 --clients 4 --duration-ms 6000 --model gmp
loadavg: 1.12 0.28 0.09 1/426
结果: qps=20629  completed=123777  errors=1179(0.95%)  established=165686
      p50=2.97ms  p99=5.60ms
服务端: cpu=704.1% (user=112.0% sys=592.0%, sys_share=84%)
       peak_threads=17  peak_rss=235.9MB  cpu_us_per_req=341.3
       ctxt_sw/req(vol=0.321 nonvol=0.106)
```

与 `docs/gmp-vs-thread-epoll-2026-10-18.md` 的 20,284 QPS 一致（±1.7%）。

**关键观察**：`sys_share=84%` —— 84% 的 CPU 花在内核态。这与 callgrind 的结论一致
（用户态只占 3~4%），也解释了为什么优化空间都在「减少 syscall 条数」而非「优化 C++ 代码」。

## 待续

- 实验 1：thread 对照（同窗口）+ 4/8/12 核扩展曲线
- 实验 2：10 万连接
- 实验 3：accept 上限
- 实验 4：真实业务形态（4KB 请求-响应 + 5ms 处理）
