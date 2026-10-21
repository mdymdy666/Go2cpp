# Fiber vs 线程：内存基准（2026-10-21）

> 目的：回答「Fiber 省下来的内存体现在哪」。结论：**省在虚拟地址空间与固定开销，物理 RSS 只略省**。

## 方法

- 基准：`tests/bench/fiber_vs_thread_memory.cpp` + `run_memory_bench.sh`；
- 每个 `(mode, stack_kb, n)` **独立进程**（避免栈缓存/arena 互相污染）；
- 采样：`base`（main 开头）→ `created`（创建 N 个 Fiber/线程后）→ `touched`（栈触页后）；
- 指标：`VmRSS`（物理）与 `VmSize`（虚拟），取自 `/proc/self/status`；每阶段采样 3 次取中位；
- 环境：WSL2（6.6.87.2-microsoft-standard）/ x86_64 / 12 CPU / 页 4096B；
- 构建目录：`build-work`（默认 OFF）。

## 数据（QUICK 矩阵：栈 64KB，n=100/1000）

### 每单位 VmRSS（KB/单位，touched−base）

| mode | stack_kb | n=100 | n=1000 |
|---|---:|---:|---:|
| fiber-direct | 64 | 60.6 | **60.6** |
| thread | 64 | 69.6 | **65.8** |

### 每单位 VmSize（KB/单位，含虚拟预留）

| mode | stack_kb | n=100 | n=1000 |
|---|---:|---:|---:|
| fiber-direct | 64 | 69.3 | **68.5** |
| thread | 64 | **62328.4** | **6294.4** |

### 固定记录开销（created−base，栈未触页）

| 指标 | mode | n=100 | n=1000 |
|---|---|---:|---:|
| VmRSS（KB） | fiber-direct | 60 | 500 |
| VmRSS（KB） | thread | 1356 | 9772 |
| VmSize（KB） | fiber-direct | 0 | 528 |
| VmSize（KB） | thread | **6232844** | **6294448** |

### 栈触页增量（touched−created）

| mode | n=100 | n=1000 |
|---|---:|---:|
| fiber-direct | 6004 KB | 60064 KB |
| thread | 5600 KB | 56000 KB |

## 结论

1. **物理内存（VmRSS）**：fiber **60.6 KB/单位** vs 线程 **65.8 KB/单位**——fiber 略省约 8%（栈 64KB 场景）；
2. **虚拟地址（VmSize）**：fiber **68.5 KB/单位** vs 线程 **6294 KB/单位**（≈6.1MB）——**线程是 fiber 的约 92 倍**；
3. **固定记录开销**：fiber 60–500 KB（RSS）/ 0–528 KB（虚拟）；线程 **1.3–9.8 MB RSS / 6.2 GB 虚拟**（glibc per-thread arena 64MiB × 线程）；
4. **栈触页**：两者相近（fiber 60KB vs 线程 56KB/单位，64KB 栈）——栈本身没有本质差别。

**所以「Fiber 省内存」的正确表述**：
- 省在 **虚拟地址空间**（线程 8MB 栈 + 64MiB arena/线程 vs fiber 8MB 预留 `PROT_NONE` + 4KB 起按需）；
- 省在 **固定开销**（fiber 记录 ~0.5KB/个 vs 线程 arena 6MB+/个）；
- 物理 RSS 只略省（8%），因为栈触页成本相同、fiber 的记录开销抵消了一部分。

## 边界（必须一起读）

- **线程内核栈（16KB/线程）与 `task_struct` 不计入 VmRSS**——线程的真实系统内存被低估；
- glibc 的 per-thread arena（64MiB 虚拟）是线程 VmSize 膨胀的主因，可用 `MALLOC_ARENA_MAX=1` 关闭（基准的 `ARENA_MAX=1` 参数）；
- 栈 64KB 场景；更大栈（1MB）需全量矩阵（`bash tests/bench/run_memory_bench.sh`，约数分钟）；
- 上一轮粗测（8KB 栈帧）曾得出「fiber 贵 5x」，那是**编译器栈帧优化 + 栈缓存**导致的失真，以本基准为准。

## 复现

```bash
# QUICK（小矩阵，约 1 分钟）
QUICK=1 bash tests/bench/run_memory_bench.sh

# 全量（5 模式 × 3 栈 × 3 数量，约数分钟）
bash tests/bench/run_memory_bench.sh

# 自定义
MODES="thread fiber-direct" STACKS="64" NS="1000" bash tests/bench/run_memory_bench.sh
```
