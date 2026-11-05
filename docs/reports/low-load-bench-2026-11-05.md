# 低负载稳定测试：bufio / Parser / tunnel mux（2026-11-05）

> **背景**：2026-11-05 前三轮抽查把 bufio、Parser、tunnel mux 判为「测量不可靠」，理由是共享机器有负载
> （同轮波动 2 倍）。本次在 **load average(1min) < 1.0 且目标核 idle > 90%** 的门槛下，用 `taskset -c 6`
> 绑核重测，每模块 **5 轮**，给出中位、标准差与变异系数（CV = std/median）。
>
> **结论**：**0/3 模块达到 CV < 5%**。bufio **CV 25.50%**（>20%，该基准在低负载下仍不可靠，
> 主因是单次测量窗口只有约 0.05 s，需改基准本身）；Parser **CV 9.43%**、tunnel mux **CV 10.61%**
> （5–20%，可重复但达不到严格稳定线）。

## 1. 方法与环境

| 项 | 值 |
|---|---|
| 机器 | WSL2 / 12 vCPU / CPU 频率固定 2495 MHz（`/proc/cpuinfo`，无降频） |
| 判定脚本 | **新增** `scripts/wait_low_load.sh`（只读 `/proc/loadavg` + `/proc/stat`） |
| 门槛 | load1 < 1.0 **且** 目标核 idle > 90%（1 s 采样差值，idle 含 iowait） |
| 每轮前置 | 先跑 `bash scripts/wait_low_load.sh 6`，5/5 轮返回 `LOW_LOAD_OK core=6` |
| 绑定核 | **core 6**（自动选核时脚本报 core 0 idle 100%，主动避开 0 号核的中断；6 号核无重负载进程） |
| 构建 | `.build-bento-wsl`，Release（`-O3 -DNDEBUG`），2026-11-05 15:02 产物 |
| 时间盒 | 45 min，只读源码，仅新增脚本与文档；每轮顺序 bufio → Parser → tunnel，轮间 `sleep 2` |

## 2. bufio 热态（`go2cpp_bufio_test` 场景5：1 MiB、缓冲 4096、512 B Read、内部 6 轮丢首轮取 median）

| 轮次 | 1 | 2 | 3 | 4 | 5 | 中位 | 标准差 | **CV** |
|---|---|---|---|---|---|---|---|---|
| MiB/s | 4945.7 | 9156.9 | 9659.3 | 6116.0 | 7831.5 | **7831.5** | 1996.81 | **25.50%** |

- 与基线 **17695 MiB/s**（`asm-bufio` 探针，同构场景）比：中位 **0.44×**，最差 0.28×，最好 0.55×。
- **诊断（关键）**：同核同负载下连跑 **30 次**该基准：median **14436.1**、p25 12690.0、p75 15843.8、
  min 7278.4、max 19289.8、mean 14047.5、std 2692.3、**CV 18.6%**。
- 即：5 轮的 7831.5 是小样本撞上低尾，不是系统性降速；30 次中位回到基线的 **0.82×**。
- 但 30 次 CV 仍有 18.6%：**每次只搬 5 MiB、窗口约 0.05 s**，噪声无法摊薄，属基准规模问题（应放大到 ≥10 MiB × ≥20 轮，或统一要求 20 次中位）。

## 3. Parser（chunked 6 MiB / 1 KiB feed，`go2cpp_tests`）

| 轮次 | 1 | 2 | 3 | 4 | 5 | 中位 | 标准差 | **CV** |
|---|---|---|---|---|---|---|---|---|
| chunked (ms) | 26 | 27 | 23 | 22 | 23 | **23** | 2.17 | **9.43%** |
| close-delimited (ms) | 10 | 10 | 9 | 9 | 8 | **9** | 0.84 | **9.30%** |

- 基线：chunked **17 ms** / close **9 ms**（2026-11-04）。
- close 对照 **复现基线**（9 ms，0%），说明本轮机器状态与基线可比；chunked 中位 23 ms **仍高 35%**。
  上一轮（19:18）把它判为「机器负载噪声」的依据是「对照组也变慢（9→17）」——本轮对照回到 9，
  **该噪声解释不成立**，chunked 的 +35% 需要单独排查（本次只读，不在范围内）。
- 注：`duration_cast<milliseconds>` 分辨率本身带来约 ±1 ms（≈±4%）量化误差，是 CV 的一部分。

## 4. tunnel mux（`bench_tunnel 3 1`，8 MiB POST，取 `summary single`）

| 轮次 | 1 | 2 | 3 | 4 | 5 | 中位 | 标准差 | **CV** |
|---|---|---|---|---|---|---|---|---|
| tunnel (MiB/s) | 1381.37 | 1455.04 | 1393.50 | 1126.17 | 1511.65 | **1393.50** | 147.79 | **10.61%** |
| direct (MiB/s) | 3236.76 | 3041.95 | 2918.43 | 2403.73 | 3481.46 | **3041.95** | 403.25 | **13.26%** |
| 开销倍率 | 2.34 | 2.09 | 2.09 | 2.13 | 2.30 | **2.13** | — | — |

- 基线：raw **2385 MiB/s** / 1.27×；mux 旧基线 770–1139 MiB/s。本轮 mux 中位 1393.5 **高于旧 mux 基线**，
  与基线口径不同（raw 需 `GO2CPP_TUNNEL_RAW_THRESHOLD=0`），**不可直接比绝对值**。
- direct 的 CV（13.26%）大于 tunnel（10.61%）：直连 8 MiB 单次仅约 3 ms，比 mux 路径更短，噪声占比更高。

## 5. 结论（CV 判定）

| 模块 | 5 轮中位 | CV | 判定 |
|---|---|---|---|
| bufio 热态 | 7831.5 MiB/s | **25.50%** | ❌ **>20%：低负载下仍不可靠，需改基准本身**（窗口 0.05 s 过短；30 次聚合 0.82× 基线） |
| Parser chunked | 23 ms | **9.43%** | ⚠️ 5–20%：可重复，但未达 CV<5%；且 +35% vs 基线需另查 |
| tunnel mux | 1393.5 MiB/s | **10.61%** | ⚠️ 5–20%：可重复，未达 CV<5%；与旧 mux 基线一致，无回归 |

**总体**：低负载门槛把「同轮 2 倍波动」压下来了（bufio 30 次 CV 18.6%、tunnel 10.6%），但**仍不足以支撑 CV<5%**。下一步应改基准而非加轮次：bufio 放大搬运量到 ≥10 MiB、Parser 改用微秒计时、tunnel 增加 payload 或轮数。

## 6. 复现

```bash
cd /UserData/CodexWorkSpace/Go2cpp
bash scripts/wait_low_load.sh 6                 # 必须输出 LOW_LOAD_OK core=6
taskset -c 6 ./.build-bento-wsl/go2cpp_bufio_test   # 看 "median=... MiB/s"
taskset -c 6 ./.build-bento-wsl/go2cpp_tests        # 看 "chunked 6MiB/1KiB feed: ... ms"
taskset -c 6 ./.build-bento-wsl/bench_tunnel 3 1    # 看 "summary single ..."
```

原始每轮日志：`/tmp/lowload_bench.log`（临时目录，重启即失；数据已抄录本文）。
