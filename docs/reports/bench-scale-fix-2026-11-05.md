# 基准规模整改报告（2026-11-05）

## 1. 判据

全局规范「基准设计（强制）」：**单样本窗口 100 ms – 1 s**，同环境 **5 轮 CV < 5%**。
改造前三个基准的窗口都远小于 100 ms，计时器分辨率与调度抖动无法摊薄。

## 2. 改造前后对照

| 基准 | 改造前（数据量 / 窗口） | 改造后（数据量 / 窗口） | 5 轮中位 | CV |
|---|---|---|---|---|
| `go2cpp_bufio_test` 场景 5 | 1 MiB/轮，**0.06 ms** | 2 GiB/轮（1 MiB × 2048），**123–128 ms** | 16509.9 MiB/s | **1.46%** ✅ |
| `bench_tunnel` 单连接 | 8 MiB/轮，直连 3.59 ms / 隧道 7.09 ms | 512 MiB/轮（8 MiB × 64），**115–125 / 323–338 ms** | 直连 4215.9、隧道 1543.2 MiB/s | **2.81% / 1.82%** ✅ |
| `bench_tunnel` 4 并发 | 32 MiB/轮，直连 14.5 ms / 隧道 41.2 ms | 512 MiB/轮（4 × 8 MiB × 16），**147–163 / 373–391 ms** | 直连 3268.8、隧道 1342.1 MiB/s | **3.28% / 1.57%** ✅ |
| Parser chunked（`go2cpp_tests`） | 6 MiB，约 1.5–13 ms | 900 MiB/轮（6 MiB × 150），**107–183 ms** | 118 ms（7627 MiB/s） | **23.4%** ❌ |

`bufio` 场景 5 保留原 1 MiB 单次样本作「冷/热」对照：窗口中位 0.060 ms、16737.8 MiB/s，
只用于说明小样本为什么没有判别力，不参与验收。

## 3. 5 轮原始值

### 3.1 bufio 场景 5（2 GiB/轮；同进程 6 轮、首轮丢弃）

| 轮次 | 1 | 2 | 3 | 4 | 5 | 6（丢弃） |
|---|---|---|---|---|---|---|
| MiB/s | 16269.6 | 16509.9 | 15985.6 | 16481.6 | 16573.0 | 16677.5 |
| 窗口 ms | 125.9 | 124.0 | 128.1 | 124.3 | 123.6 | 122.8 |

median=16509.9，mean=16445.5，std=239.6，**CV=1.46%**。

### 3.2 bench_tunnel（rounds=5，直连/隧道每轮交替）

单连接 512 MiB/轮：
- direct MiB/s：4111.1 / 4215.9 / 4176.4 / 4275.8 / 4461.1 → median 4215.9，**CV 2.81%**
- tunnel MiB/s：1515.5 / 1517.1 / 1568.2 / 1587.3 / 1543.2 → median 1543.2，**CV 1.82%**

4 并发 512 MiB/轮：
- direct MiB/s：3138.1 / 3261.5 / 3268.8 / 3270.0 / 3473.5 → median 3268.8，**CV 3.28%**
- tunnel MiB/s：1310.7 / 1358.7 / 1372.8 / 1342.1 / 1335.7 → median 1342.1，**CV 1.57%**

### 3.3 Parser chunked（900 MiB/轮，进程级 5 轮，每轮前过门禁）

- chunked ms：113 / 183 / 174 / 118 / 107 → median 118，mean 139.0，std 32.6，**CV 23.4%**
- 同轮 close-delimited ms：64 / 119 / 84 / 73 / 60（与 chunked 同向波动）

## 4. 结论与遗留问题

1. **bufio 与 tunnel 达标**：窗口 115–391 ms，CV 1.46–3.28%，全部 < 5%。
2. **chunked 窗口已达标（107–183 ms）但 CV 23.4% 未达标**。已排除「窗口太短」与
   「每轮重建对象」两类原因，实测三种改法均复现同样的双峰（快组 ≈110 ms / 慢组 ≈178 ms）：
   - 同进程 6 轮（首轮丢弃）→ 154–528 ms，CV 47%；
   - 单条 120 MiB wire 一次解析（消除重复分配）→ 67–283 ms，CV 66%；
   - 6 MiB × 150 次（当前）→ 107–183 ms，CV 23.4%。
3. **噪声归属判定为共享机器竞争**：同轮 close-delimited 同步变慢（60→119 ms），而
   bufio（L2/L3 内 memcpy、不吃内存带宽）同期 CV 仅 1.46%；`nproc=12`，目标核 6 的
   SMT 兄弟是 cpu7（`thread_siblings_list=6-7`），其它 agent 进程可落到 cpu7 与共享内存带宽。
   chunked 基准要跑在 6 MiB 级流式内存上，对带宽竞争最敏感。
4. **下一步（未做，时间盒到）**：chunked 基准改用 `perf stat -e cycles` 或 CPU 时间
   （`clock_gettime(CLOCK_THREAD_CPUTIME_ID)`）替代 wall time，天然抵消调度抢占；
   或把验收固定在独占机器上执行。

## 5. 复现

```bash
cmake -S . -B .build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DGO2CPP_BUILD_TESTS=ON \
      -DGO2CPP_BUILD_EXAMPLES=OFF -DGO2CPP_WITH_BENTO=OFF -DGO2CPP_WITH_MCP=OFF \
      -DGO2CPP_WITH_APP=OFF -DGO2CPP_WITH_P2P=OFF -DGO2CPP_WITH_SQLITE=OFF
ninja -C .build-bench go2cpp_bufio_test bench_tunnel go2cpp_tests
scripts/run_bench_scale_5rounds.sh      # 门禁 + 绑核 + 三基准
```

改动文件：`tests/bufio/test_bufio.cpp`、`tests/tunnel/bench_tunnel.cpp`、
`tests/network/test_network.cpp`、`scripts/run_bench_scale_5rounds.sh`。未改 `src/`，未 commit。
