# Go2Cpp GMP 协程 vs 传统 thread + epoll 压测报告

- 日期：2026-10-18
- 被测代码：`benchmarks/gmp_vs_thread_epoll_benchmark.cpp`（新增，未改任何既有源文件）
- 构建脚本：`benchmarks/build_gmp_vs_thread_epoll.sh`（未改 `cmake/Go2CppTests.cmake`）
- 原始数据：`benchmarks/results/gmp_vs_thread_epoll_table.md`
- 全量测试：`ctest --test-dir build-quality` → **113/113 通过**（串行 67.9s）

---

## 1. 两种（实际三种）模型的实现方式

三种服务端共用**同一套协议、同一个 `process_request` 业务函数、同一个客户端**，
差异只来自并发模型：

| | 模型 A `gmp` | 模型 B1 `thread` | 模型 B2 `epoll` |
|---|---|---|---|
| 并发单元 | 每连接一个 Fiber（G） | 每连接一个 pthread | 每连接一个状态机对象 |
| IO 方式 | Fiber 内阻塞 `Socket::recv/send`，内部分片等待并 park G | 阻塞 `recv/send` | 非阻塞 fd + epoll（LT） |
| 线程 | `IOManager` 内部 12 个 M（`processor_count=0` → 12） | 1 主线程 + N 连接线程（默认 8MB 栈） | 1 个 epoll IO 线程 + 8 个 worker |
| 写路径 | `send` 阻塞即 park | `send` 阻塞即占住线程 | 用户态发送队列 + EPOLLOUT，慢客户端只涨队列 |
| 栈开销 | Fiber 默认 128KB（惰性提交） | 8MB 虚拟/线程（惰性提交） | 无协程/线程栈 |

关键实现点：

- **GMP**：主线程 `accept` → `manager.go([socket]{ serve(...) })`；连接内是
  直白的 `recv 4B 头 → recv body → 处理 → send`，看不到状态机。配置
  `collect_metrics=false`，并开启 `pin_workers_to_cpu`（实测不绑核 8.7k QPS，
  绑核 13.6k QPS，取较优配置）。
- **B1**：`accept` 后 `pthread_create`（可配置栈大小），线程内阻塞 `recv/send`；
  线程创建失败会计数并在 stderr 汇报（本次 1 万连接场景未失败）。
- **B2**：IO 线程独占全部连接状态；worker 只做 `process_request`，
  完成后把响应塞进完成队列并用 eventfd 唤醒 IO 线程；IO 线程负责非阻塞写、
  EPOLLOUT 续写、输出超过 8MB 时暂停 EPOLLIN 形成背压。这是该模型的标准做法，
  慢客户端不会占住 worker。

协议（避免 HTTP 解析干扰模型对比）：

```
请求 [u32 len][u8 op][body]   响应 [u32 len][body]
op: 1 ECHO(原样回显) 2 READ(4KB上行/1B下行) 3 WRITE(1B上行/4KB下行)
    4 BIG(下行4MB，慢客户端) 5 PING(1B)
```

五个场景：s1 短连接（64 并发，连接→1 请求→关闭→重连）、s2 长连接 echo
（64 连接持续 ping-pong）、s3 1 万空闲连接（每连接每秒 1 次 PING）、
s4 混合读写（READ/WRITE 各半）、s5 慢客户端（1 个慢读 4MB 连接 + 32 个正常
echo 连接，服务端 SNDBUF 统一 256KB）。

---

## 2. 五场景 × 三模型完整数据（每格 5 次取中位数）

| 场景 | 模型 | QPS | P50(ms) | P95(ms) | P99(ms) | MB/s | 线程峰值 | RSS峰值(MB) | CPU% | CPU(µs/req) | 切换/请求 | 完成 | 错误 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| s1 | GMP | 4729 | 11.54 | 13.11 | 14.25 | 0.29 | 16 | 50.1 | 247.6 | 517.4 | 1.74 | 23650 | 3782 |
| s1 | thread | 10421 | 4.00 | 4.69 | 5.17 | 0.64 | 52 | 5.9 | 151.0 | 143.9 | 0.11 | 52093 | 0 |
| s1 | epoll | **19320** | **2.86** | **3.88** | **4.36** | 1.18 | 9 | 4.7 | 122.6 | 63.5 | 0.10 | 96609 | 0 |
| s2 | GMP | 13125 | 1.45 | 20.50 | 33.56 | 0.80 | 15 | 5.2 | 148.0 | 112.4 | 0.00 | 65634 | 0 |
| s2 | thread | **30493** | 2.03 | 2.34 | 4.06 | **1.86** | 65 | 5.1 | 122.2 | 40.5 | 0.00 | 152530 | 0 |
| s2 | epoll | 28075 | 2.03 | 3.92 | 4.48 | 1.71 | 9 | 4.8 | 133.2 | 47.5 | 0.11 | 140393 | 0 |
| s3 | GMP | 4798 | 40.34 | 131.07 | 168.93 | 0.01 | **18** | 64.5 | 284.5 | 587.7 | 0.00 | 48578 | 0 |
| s3 | thread | 9659 | 0.53 | 1.55 | 2.11 | 0.01 | 10001 | 93.5 | 56.7 | 56.8 | 0.00 | 99800 | 0 |
| s3 | epoll | **9876** | **0.80** | 4.61 | 9.14 | 0.01 | **9** | **6.0** | 64.8 | 64.9 | 0.19 | 99884 | 0 |
| s4 | GMP | 7473 | 7.80 | 16.84 | 22.93 | 14.60 | 15 | 5.9 | 176.4 | 237.4 | 0.00 | 37369 | 0 |
| s4 | thread | 22152 | 2.95 | 5.09 | 5.39 | 43.28 | 65 | 5.3 | 134.0 | 60.5 | 0.00 | 110779 | 0 |
| s4 | epoll | 3050 | 20.99 | 27.18 | 29.97 | 5.95 | 9 | 5.5 | 31.4 | 102.9 | 1.40 | 15252 | 0 |
| s5 | GMP | 12964 | **0.77** | 9.74 | 17.20 | 2.39 | 15 | 5.1 | 152.3 | 116.8 | 0.00 | 129645 | 0 |
| s5 | thread | **30211** | 1.00 | **1.23** | **2.04** | 3.04 | 34 | 4.7 | 122.8 | 40.2 | 0.00 | 302126 | 0 |
| s5 | epoll | 27351 | 1.07 | 2.06 | 2.47 | **3.27** | 9 | 8.8 | 133.7 | 48.8 | 0.11 | 273516 | 0 |

说明：
- `s1` GMP 的 3782 个错误是**连接建立失败**（客户端 connect 失败/被重置），
  错误率 3782/(23650+3782)=13.8%，QPS 只统计成功请求，对比时要注意这一点。
- `切换/请求` 读 `/proc/<pid>/status` 的 voluntary+nonvoluntary。GMP 为 0.00
  并不代表“零切换”——它的切换是用户态 `swapcontext`，内核计数看不到；
  epoll 模型的 0.10–0.19 是 worker 被 eventfd 唤醒的内核切换。
- CPU% 是进程 utime+stime 在测量窗口内的占比（12 核可超过 100%）。

### 2.1 「进步多少」总表（GMP 相对对照模型）

倍数口径：QPS/吞吐 = GMP ÷ 对照（>1 GMP 更好）；延迟/CPU/RSS/线程数 =
对照 ÷ GMP（>1 GMP 更好）。

| 场景 | 指标 | GMP | thread | epoll+池 | vs thread | vs epoll |
|---|---|---|---|---|---|---|
| s1 | QPS | 4729 | 10421 | 19320 | **0.45x** | **0.24x** |
| s1 | P99(ms) | 14.25 | 5.17 | 4.36 | **0.31x** | **0.31x** |
| s2 | QPS | 13125 | 30493 | 28075 | **0.43x** | **0.47x** |
| s2 | P99(ms) | 33.56 | 4.06 | 4.48 | **0.12x** | **0.13x** |
| s3 | QPS | 4798 | 9659 | 9876 | **0.50x** | **0.49x** |
| s3 | 线程峰值 | 18 | 10001 | 9 | **556x** | 0.50x |
| s3 | RSS(MB) | 64.5 | 93.5 | 6.0 | **1.45x** | 0.09x |
| s4 | QPS | 7473 | 22152 | 3050* | **0.34x** | 2.45x* |
| s4 | 吞吐(MB/s) | 14.60 | 43.28 | 5.95* | **0.34x** | 2.45x* |
| s5 | QPS | 12964 | 30211 | 27351 | **0.43x** | **0.47x** |
| s5 | 服务端 RSS(MB) | 5.1 | 4.7 | 8.8 | 0.92x | **1.73x** |

---

## 3. 结论：哪些好、哪些差不多、哪些更差

### 3.1 GMP 明显更好的地方

1. **线程数**：1 万连接下 GMP 18 个线程，每连接一线程模型 10001 个线程，
   GMP 少 **556 倍**。这是 GMP 的核心结构优势，数据完全成立。
2. **相对线程模型的 RSS**：10k 连接 GMP 64.5MB vs 线程模型 93.5MB（低 31%）。
   幅度没有“KB 栈 vs MB 栈”听上去那么夸张，因为线程栈也是惰性提交的：
   1 万线程实际只用了 93.5MB，约 9.3KB/线程。
3. **s5 的 P50**：慢客户端场景 GMP P50 0.77ms，比 thread(1.00ms)/epoll(1.07ms) 略好，
   说明「阻塞写只会 park 自己」的语义有效；但 P95/P99 仍被唤醒延迟拖垮。

### 3.2 差不多的场景

没有找到“基本持平”的场景。5 个场景的 QPS 全部是 GMP 落后。

### 3.3 GMP 更差的场景（本机实测）

**相对每连接一线程，5 个场景的吞吐、P95/P99、CPU/请求全部更差**；相对 epoll+池，s1/s2/s3/s5 更差，s4 的 epoll 数字异常（见 8.3），不能据此说 GMP 赢。s1 还多出 13.8% 连接错误。
最严重的是：

- s1 短连接：QPS 只有 epoll 的 0.24x，P99 14.25ms vs 4.36ms，且 13.8% 失败；
- s3 1 万空闲连接：理想应完成 10 万次/10s（1 万 QPS），GMP 只做到 4798 QPS，
  P50 40ms、P99 169ms；
- s2：P99 33.6ms，是 thread(4.06)/epoll(4.48) 的 7–8 倍；s4 在修正为真 50/50 负载后 GMP P99 22.9ms，thread 5.39ms；
- 内存：10k 空闲连接 GMP 64.5MB，是 epoll 模型 6.0MB 的 **10.7 倍**。

---

## 4. 为什么 GMP 更差（基于测量，不是猜）

### 4.1 主要机制：单事件唤醒 + 跨线程 handoff

GMP 的 IO 等待路径是：worker 上的 G 调 `Socket::recv` → 无数据 → 在
`IOManager` 注册 fd 并 park → **IOManager 的单个 epoll poller 线程**收到就绪 →
唤醒 G 到某个 worker（跨线程）→ worker 继续跑。

本机实测的三个关键数字（微基准，代码见文末复现命令）：

| 路径 | 实测 |
|---|---|
| 纯 GMP park/wake（两个 Fiber 经 channel 往返，同线程） | **0.73–0.80 µs/往返** |
| IOManager + Socket 单连接 ping-pong（跨线程） | **185–225 µs/往返** |
| 两个 pthread 经 condition_variable 往返（WSL2 跨线程基线） | **54–56 µs/往返** |

也就是说：**GMP 的用户态切换本身极快（0.8µs），但每次 fd 就绪都要一次
跨线程唤醒；在 WSL2 上这次唤醒本身就要 50µs 量级**。poller 是单线程、串行
唤醒，因此单事件到达（短连接、1 万空闲连接每秒各 1 次）时，上限就是
「1 / 单次唤醒成本」≈ 5k–9k 次/秒——正好对上 s1 的 4.7k、s3 的 4.8k、
s2 的 13k。

**反证**：项目自带的 `go2cpp_io_epoll_fair_benchmark`（4096 个 pipe、按波次
批量就绪）在同一台机器上给出 IOManager+Fiber **153k events/s** vs
thread-pool epoll **45k events/s**，GMP 快 3.4 倍。说明 GMP 并非“实现得慢”，
而是**批处理时很强、单事件跨线程唤醒时被环境成本卡住**。本次五个场景恰好
都是“单事件”形态。

### 4.2 s1 的 13.8% 错误：accept 是串行的

GMP 的 accept 在主线程（非调度线程）执行，每接受一个连接还要 `manager.go`
入队。实测 accept 路径约 200µs/次 → 上限约 5k 连接/秒，与 s1 QPS 4729 完全吻合。
64 并发短连接把 SYN/accept 队列打满后，客户端 connect 失败或被重置。
B1 的 accept 只做 `pthread_create`（~30µs），B2 只做 `epoll_ctl`，都不受此限。

### 4.3 内存：Fiber 栈 vs 纯状态机

1 万空闲连接：

- GMP：18 线程 + 1 万份 128KB 栈（惰性提交）→ 64.5MB；
- thread：10001 线程 × 8MB 虚拟栈（惰性提交）→ 93.5MB；
- epoll：9 线程 + 每连接约 200B 状态 → 6.0MB。

结论：**Fiber 栈比线程栈省，但比“无栈事件驱动状态机”贵一个数量级**。
1 万连接这种量级，epoll+线程池的内存优势比 GMP 更大；GMP 真正不可替代的是
“同步写法 + 线程数固定”，不是内存绝对值。

### 4.4 环境放大效应（必须说明）

WSL2 的跨线程唤醒（futex/condvar）实测 54–56µs，裸机 Linux 通常在个位数 µs。
GMP 对这条路径最敏感（每个就绪事件一次跨线程唤醒），因此**本报告的 GMP
绝对性能被 WSL2 显著放大惩罚**。但本次没有裸机对照，不能给出“裸机上会好多
少”的数字；能确认的是：同一环境下线程/epoll 模型受影响更小，因为它们
批处理或直接在工作线程里完成 IO。

---

## 5. 环境与可复现性

- CPU：AMD Ryzen 9 8945HX，12 逻辑核（WSL2 分配）
- 内存：19.5 GiB，swap 24 GiB；`vm.max_map_count=1048576`，`overcommit_memory=0`
- 内核：WSL2（`uname -r` 见运行记录）
- fd 上限：驱动内抬到 65535（1 万连接 ×2 进程）
- 压测期间 `procs_running` 峰值 **14**（vmstat 采样，12 核）；`loadavg` 在 WSL2
  失真，未使用
- 客户端与服务端同机，互相争抢 CPU；每格 5 次取中位数
- 全量 ctest：**113/113 通过**（串行）。补充：`ctest -j4` 时 `go2cpp_tests`
  偶发一次失败（hook FIFO 时序用例），单测重跑与串行全量均通过，属并行下的
  时序敏感，不是本次改动引入

复现命令：

```bash
cd /UserData/CodexWorkSpace/Go2Cpp
flock /tmp/go2cpp_build.lock bash benchmarks/build_gmp_vs_thread_epoll.sh
build-gmp-vs-thread/gmp_vs_thread_epoll_benchmark run --repeats 5 \
  | tee benchmarks/results/gmp_vs_thread_epoll_table.md
```

只跑部分：`run --scenarios s2 --models gmp,epoll --repeats 3`。
单模型服务端/客户端也可独立运行（`server --model ...`、`client --scenario ...`）。

---

## 6. 需要并入 `cmake/Go2CppTests.cmake` 的片段（未改该文件）

放在 `if (GO2CPP_BUILD_BENCHMARKS)` 块内：

```cmake
    add_executable(go2cpp_gmp_vs_thread_epoll_benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/gmp_vs_thread_epoll_benchmark.cpp)
    target_link_libraries(go2cpp_gmp_vs_thread_epoll_benchmark PRIVATE go2cpp_runtime)
    target_compile_options(go2cpp_gmp_vs_thread_epoll_benchmark PRIVATE
        -O2 -Wall -Wextra -Wpedantic)
```

在并入之前，`benchmarks/build_gmp_vs_thread_epoll.sh` 用 build-quality 的
`.so` 直接编译同名可执行文件，保证可复现。

---

## 7. 未覆盖 / 已知限制

- 未在裸机 Linux 复测，无法量化 WSL2 对 GMP 的惩罚幅度；
- 客户端与服务端同机，12 核共享，绝对 QPS 偏低；
- GMP 只测了 `processor_count=0`(12) + `pin_workers_to_cpu=true` 一种配置，
  未扫 P 数、`idle_wait`、`fiber_stack_size` 等参数；
- s1 的 GMP 错误率高，说明该模型不适合“高并发短连接 + 单线程 accept”的用法，
  但本次未尝试多 accept 线程改造；
- 慢客户端只测了 1 条慢连接 + 32 条正常连接，未做多慢连接扩展性测试。

---

## 8. 补充实验与一处修正（二次测量）

### 8.1 修正：原 s4 实际是 100% READ

`on_response` 先把 `pending_op` 清零、再判断方向，导致“混合读写”一直发 READ。
修正后是真正的 50/50（4KB 上行 / 4KB 下行交替），表中 s4 已替换为修正后
5 次中位数；原 s4 数据作废。

### 8.2 `--pin` 与流水线深度

- `--pin`（worker 绑核）：64 连接 echo 从 8.7k 提到 13.6k QPS；但**单连接**
  从 6.1k 掉到 295 QPS（p50 150µs → 3.37ms）。说明 GMP 的唤醒/放置策略对
  延迟极其敏感。最终表用的是 `--pin`（对吞吐更有利），单连接延迟不代表
  GMP 的普遍水平。
- `--depth`（每连接在途请求数，本次新增）：s2、64 连接，
  depth=1/8/32 → 7709 / 8449 / 10711 QPS，p50 = 7.7 / 59.3 / 184.0 ms。
  加大批量只带来 1.39x，且延迟爆炸。因此**“批量就绪”不能单独解释内置基准
  与本次结果的差距**；GMP 唤醒路径上还有别的串行/开销点，本次未定位。

### 8.3 s4 的 epoll+池异常（未解决）

s4 的 epoll+池只有 3050 QPS、CPU 31%、1 连接时 p50 已 2.8ms；
`--workers 1` 与 `--workers 8` 结果相同，客户端关 TCP_NODELAY 也不改变。
瓶颈在 B2 的 IO 线程路径，本次未定位到具体代码。**该数字不用于评价
epoll+线程池模型**，s4 只做 GMP vs thread 的比较。

### 8.4 为什么项目内置基准给出相反结论

- 内置 `go2cpp_io_epoll_fair_benchmark`：4096 个 pipe、25 波；每一波 4096 个 fd
  **同时**就绪，一次 `epoll_wait` 返回一大批事件，`wake_io_batch` 再按 P 分组
  批量唤醒。102400 事件 / 0.667s = **153k events/s ≈ 6.5µs/event**。它测的是
  **“事件成批到达时的 readiness 吞吐”**。
- 本次五个场景都是“少量事件/次”：ping-pong 每连接只有 1 个在途请求；
  1 万空闲连接每秒各 1 次。单次跨线程唤醒成本（WSL2 实测 54–56µs）无法摊薄，
  而 IOManager 只有 1 个 poller 线程串行处理事件 → 5–13k/s 上限。
- 两者并不矛盾：一个测“批量事件的处理上限”，一个测“一问一答的往返延迟/吞吐”。
  GMP 的设计在批处理路径上很强（同机比 thread-pool epoll 快 3.4 倍），
  但在“单事件 + 跨线程唤醒”路径上被环境成本主导。
