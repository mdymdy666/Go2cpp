# startup_cost：池化资源启动开销基准

量 Go2Cpp 各池化资源在**启动期与首次使用时**的时间与内存开销。
方法论、实测数据与结论见 [`docs/modules/startup-cost.md`](../../docs/modules/startup-cost.md)。

## 快速开始

```bash
# 1. 构建（链接 .build-bento-wsl 里已构建好的 .so，不改顶层 CMake）
tools/startup_cost/build.sh

# 2. 全量测量，报告写到 /tmp/startup_cost.md（约 3 秒）
.build-startup-cost/startup_cost

# 3. 调试：少样本、不写报告
.build-startup-cost/startup_cost --quick --no-report
```

## 目录

```text
tools/startup_cost/
  build.sh         # 构建脚本（g++ 直编，不改仓库 CMake）
  measure.hpp      # rdtsc 校准、VmRSS/Δheap 采样、中位/百分位（header-only）
  pools.hpp/.cpp   # 池清单 + 11 个探针（每个池一个）
  report.hpp/.cpp  # 控制台表格 + markdown 报告
  main.cpp         # 父模式（fork/exec 编排）与子模式（单探针）
  min_main.cpp     # 场景 A 的裸基线程序（只 main + return）
```

## 四个场景

| 场景 | 做什么 | 回答什么 |
|---|---|---|
| A | 只 `main`+`return` / 完整二进制空跑 | 进程启动底噪是多少 |
| B | 每个池一个独立进程 | 每个池初始化多久、多少内存、多少槽 |
| C | 单进程按真实顺序初始化全部池 | 总共多久、多少内存；分段归因 |
| D | 首次 vs 同进程第二次 | 惰性池的一次性成本落在谁头上 |

## 常用命令

```bash
startup_cost --list                         # 列出全部池 id
startup_cost --child jitter_buffer --mode mem
startup_cost --child cache_node_pool --mode time --repeat 10000
startup_cost --out /tmp/startup_cost.md     # 指定报告路径
STARTUP_COST_TRACE=1 startup_cost --child __all__   # 场景 C 逐段计时到 stderr
```

## 读报告时注意

- **`ΔVmRSS` 是 4 KiB 粒度**：小于一页的池（WaitNode 等）看 `Δheap`；
- **`ΔVmSize` 主要是栈 arena 的 VA 预留**（64 MiB），不是物理内存；
- **`Δheap` 会含首次路径上的其它分配**（Fiber 对象、状态记录），
  判断池体积请看「槽数 × 单槽」；
- **场景 C 的 11 ms 里有约 10 ms 是 park→wake 等待**，报告已单列并扣除。

## 加一个新池

见 `docs/modules/startup-cost.md` 第 8 节：加一条 `PoolInfo`、写一个
`ProbeXxx(ProbeMode, std::size_t)`、在 `RunProbe()` 里分发。
