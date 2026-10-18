# GMP 性能分析：与 thread / epoll+池 的真实差距（2026-10-18）

- 被测代码：`benchmarks/gmp_vs_thread_epoll_benchmark.cpp`（三模型共用同一协议、
  同一 `process_request`、同一 epoll 客户端）+ 库 `build-quality`（Release `-O2`）
- 宿主：AMD Ryzen 9 8945HX / 12 逻辑核 / WSL2 6.6.87.2 / g++ 13.3.0
- 全量测试：`ctest --test-dir build-quality` → **113/113**
- 目的：回答 **「GMP 不如 threadpool+epoll」是设计问题、实现问题，还是环境问题**

> 读法：本文每组数据都标注了「是否开启基准注入的随机 IO 延迟」。
> 基准 `process_request` 默认 `GO2CPP_BENCH_IO_SLEEP_US=5000`，即**每个请求
> 注入 `U(0, 5ms)` 的 `usleep`（均值 2.5ms）**。这一项决定了 p50 的下限，
> 跨「开/关」两组数字**不可直接比较**。

---

## 0. 结论（先看）

| 判断 | 证据 |
|---|---|
| **不是设计问题** | 关掉注入延迟后，s2（64 连接 ping-pong）GMP **30,720 QPS** vs thread **29,078**（+5.6%）；批量就绪场景（pipe × 64）GMP 比 thread+epoll **快 3.6 倍** |
| **是实现问题，且主因已定位并修复** | 描述符注册表的**全局递归锁**把 accept 路径压到 5k/s；改成 64 条带后 s1 **4,573 → 11,800 QPS（2.6x）**、p50 **13.6ms → 4.6ms**、错误 673 → 500 |
| **环境放大严重** | 同 QPS 下 GMP 的内核态 CPU 是 thread 的 **3.5 倍**（sys 170µs vs 49µs/req）；WSL2 上一次跨线程唤醒 ~55µs，GMP 每条 IO 事件都要付一次 |
| **剩余差距集中在「单事件」形态** | s3（1 万连接 × 1 次/秒）GMP 8,505 vs thread 9,671（-12%），但 **p50 160ms vs 0.53ms**：1 万事件突发时 12 个 worker 处理，排队而非慢 |

---

## 1. 测量口径与环境修正

### 1.1 注入延迟是所有 p50 数字的主体

`process_request()` 默认对每个请求 `usleep(xorshift % (5000+1))`，均值 2.5ms。
这解释了此前一系列「反常」延迟：

| 观测 | 旧解读 | 实测（本节） |
|---|---|---|
| 单连接 echo p50 **2.9ms** | 「GMP 唤醒慢」 | **thread 模型同样 2.89ms**；关掉注入后仍 1.65~2.12ms（环境 TCP 往返） |
| `--pin` 后单连接 p50 3.37ms | 「绑核恶化延迟」 | pin 21,911 / 2.92ms vs 不 pin 22,146 / 2.90ms —— **无差异** |
| s2 depth=1 p50 2.88ms | 「跨线程 handoff 90µs × 30」 | 64 连接 × 1/2.9ms ≈ 22k QPS，**p50 由注入延迟决定，与并发度无关** |

**方法论教训**：基准必须显式区分「被测模型的成本」与「基准自己注入的业务成本」，
否则会把 2.5ms 的 `usleep` 归因到调度器头上（H8 的全部成因就是这一条）。

### 1.2 用 `utime/stime` 拆分判断「用户态实现 vs 内核/环境」

`/proc/<pid>/stat` 的 utime/stime 分开采样（脚本 `.dsh_tmp/_bench_manual.py`）：

| 模型（s2，64 连接，depth=1，开注入） | QPS | CPU% | user µs/req | sys µs/req |
|---|---:|---:|---:|---:|
| GMP | 22,308 | 468 | **40** | **170** |
| thread | 21,841 | 126 | 9 | 49 |

**结论**：GMP 的用户态成本（40µs）只有 thread（9µs）的 4 倍，但**内核态是 3.5 倍**；
81% 的 CPU 花在内核。这既不是「Fiber 切换慢」（实测 0.73~0.80µs/往返），
也不是「锁自旋烧 CPU」，而是**每条 IO 等待/唤醒附带的 syscall + 调度开销**在
WSL2 上被放大。

---

## 2. H4：`--depth 32` 只到 10.7k 的隐藏瓶颈 —— 已定位

### 2.1 复现（当前代码已好于交接文档）

`s2`，64 连接，单客户端，8s 窗口（开注入）：depth 1 / 8 / 32 →

| depth | QPS | p50 |
|---:|---:|---:|
| 1 | 21,911 | 2.92ms |
| 8 | 23,563 | 21.69ms |
| 32 | 23,602 | 86.61ms |

交接文档记录的是 7,709 / 8,449 / 10,711。**当前实现已经整体高 2.2~2.8 倍**
（原因见 §4 的 A/B：描述符注册表分条）。深度 8→32 不再增长，说明**不是批量能力不足**。

### 2.2 判定瓶颈侧：客户端不是瓶颈

两个客户端进程各 32 连接（总连接数不变、客户端处理能力翻倍）：

```
clients=2 conns=32 depth=8 → 11,862 + 11,854 = 23,716 QPS（单客户端 64 连接 23,563）
```

**总吞吐不变 ⇒ 天花板在服务端（或两端共用的内核路径），不在单客户端事件循环。**

### 2.3 天花板是什么：关掉注入延迟后与 thread 持平

| 配置（s2，64 连接，depth=1） | GMP | thread | 差 |
|---|---:|---:|---:|
| 开注入（U(0,5ms)） | 22,308 | 21,841 | +2.1% |
| **关注入** | **30,720** | **29,078** | **+5.6%** |

GMP 在纯模型对比下**不落后**。23.6k→30.7k 的差值是注入延迟吃掉的并发容量
（64 连接 × 1/2.5ms ≈ 25.6k，与 23.6k 同量级）。

**H4 结论**：`--depth 32` 上不去的原因不是「批量没生效」，而是
(a) 注入延迟占满并发容量；(b) 服务端每请求仍有 ~130µs CPU 的固定成本
（见 §5），在 12 核上折合 3~4 核，再往上要先降这笔成本。
**「加了批量还是不够」= 每请求固定成本，而不是批量机制缺失。**

---

## 3. H3：s1 的「错误」不是 connect 失败，而是 EPOLLHUP

### 3.1 直方图（`GO2CPP_BENCH_ERRNO=1`）

s1，64 连接，6s 窗口，唯一出现的失败点：

```
[errno] client site=client_epollerr_hup 0(Success)=864
```

- `client_connect` / `client_so_error` **一次都没有出现** ⇒ **不是 connect 失败**；
- 记录值是 `0`，因为 `handle_event()` 在 `EPOLLERR|EPOLLHUP` 分支直接 `++m_errors`，
  没有取 errno。

### 3.2 其中一个成分：`EPOLLIN|EPOLLHUP` 同批到达时响应被丢弃

试验性改动（仅诊断副本 `.dsh_tmp/diag_bench.cpp`）：**先消费 `EPOLLIN` 再判 HUP**，
并在 `recv()==0` 前先 `parse_responses()`。

| 版本 | QPS | completed | errors |
|---|---:|---:|---:|
| 原客户端 | 11,962 | 71,797 | **646** |
| 先读后判 HUP | 2,893 | 17,360 | **17** |

错误几乎消失（-97%），证明**大部分 HUP 与「响应 + FIN 同批到达」有关**；
但吞吐崩到 1/4，说明 s1 的吞吐与「客户端收到 HUP 就立刻重连」这一行为强耦合
（提前重连掩盖了完成路径）。**因此该改动已回滚，不入库**。

**H3 结论**：s1 的 errors 是「连接被对端关闭/重置时客户端尚未完成解析」，
不是 accept 失败。要把它降到 0，需要同时改「客户端事件顺序 + 服务端何时关闭连接」
两侧语义，单改一侧会以吞吐为代价。

---

## 4. 核心修复：描述符注册表从「全局锁」改为「按 fd 分条」

### 4.1 改动内容

`include/go2cpp/network/io.hpp` / `src/network/io.cpp`：

- `DescriptorGuard` 由「默认构造 + 全局递归锁」改为 **`explicit DescriptorGuard(int fd)`**，
  构造时锁住该 fd 所属**条带**（`kStripeCount = 64`，`fd & 63`）；
- `DescriptorRegistryState` 变为 `std::array<Stripe, 64>`，每个条带自带 `tokens` 与
  `next_generation`（同一 fd 永远落同一条带 ⇒ 代际/在途 syscall 的互斥不变）；
- `Socket::init_nonblock()` 用 `::syscall(SYS_fcntl, F_GETFL)` 读标志，只有在**确实要改**
  时才走 Hook 版 `fcntl`（避免每个 accept 两次全局锁 + 两次 syscall）。

### 4.2 A/B（只改 `kStripeCount`，其余完全不变）

| s1（64 连接，开注入） | QPS | p50 | 错误 |
|---|---:|---:|---:|
| `kStripeCount = 1`（等效全局锁） | 4,517 / 4,630 | 13.67 / 13.47ms | 656 / 691 |
| **`kStripeCount = 64`** | **12,104 / 11,474** | **4.51 / 4.73ms** | 615 / 488 |

**2.6 倍吞吐、p50 降 3 倍。** 单锁档的 4.5k/13.6ms 与交接文档的 4,383/13.86ms
在误差内一致 —— 说明历史上「GMP 慢 2.4 倍」的主因**就是这把全局锁**，不是调度模型。

### 4.3 三模型当前对比（各 1~2 次，开注入）

| 场景 | GMP | thread | epoll+池 | 旧文档 GMP |
|---|---:|---:|---:|---:|
| s1 QPS | **11,800** | 10,458 | 3,027 | 4,383 |
| s1 P50 | **4.6ms** | 4.73ms | 20.93ms | 13.86ms |
| s1 错误 | ~500 | 0 | 0 | 1,264 |
| s3 QPS | 6,270 | **9,674** | 3,057 | 3,463 |
| s3 P50 | 445ms | **2.83ms** | 2,245ms | 76.9ms |

s1 上 GMP **已经反超 thread**（+13%）并把 P50 拉平；s3（1 万空闲连接）仍落后
thread 35%，原因见 §6。

> **注意 s3 的硬指标在协议上不可达**：客户端 `conns=10000`、每连接每秒 1 次 PING，
> QPS 上限恒为 **10,000**（thread 档 9,674 已贴近上限）。「s3 ≥ 12,000」不可能靠
> 优化达到，只能改场景参数（提高 `--interval-ms` 或连接数）。

---

## 5. 剩余成本：每请求 ~130µs 的内核态（s2 纯模型）

| 指标（s2，64c，depth=1，关注入） | GMP | thread |
|---|---:|---:|
| QPS | 30,720 | 29,078 |
| CPU% | 404.7 | 113.0 |
| user µs/req | 22.1 | 4.0 |
| **sys µs/req** | **109.7** | 34.8 |

GMP 多出来的 ~75µs/请求内核时间，来自三条每条 IO 都要走一次的路径：

1. **`IOManager::State::register_wait()` / `complete()` 全局 `m_mutex`，
   且 `update_interest_locked()` 在锁内做 `epoll_ctl`**（EPOLLONESHOT rearm/DEL）。
   gdb 采样（`ptrace_scope=1`，用 gdb 作为父进程）在 22k QPS 下看到
   **61/112 个线程栈顶停在 `futex_wait`（`pthread_mutex_lock`）**，
   全帧热点是 `wait_for → register_wait → __lll_lock_wait`：
   ```
   61  futex_wait          (leaf, 全部经 lll_mutex_lock_optimized → pthread_mutex_lock)
   42  go2cpp::io::IOManager::wait_for
   37  go2cpp::io::IOManager::State::register_wait
   34  ...  poll_once / worker_loop
   ```
2. **每个 park/wake 一次跨线程唤醒**（worker 在 `epoll_wait` 里被 eventfd/futex 唤醒，
   WSL2 实测 ~55µs/次）。
3. **7.6 次 syscall/请求**（`strace` 计数，`recvfrom`/`sendmsg` 各有一半 EAGAIN）。

> 第 1 条是可以继续修的**实现问题**：`State::m_mutex` 是 IOManager 的单一全局锁，
> 所有 fd、所有 worker 共用，临界区里还夹着 `epoll_ctl`。这与已经修好的
> 描述符注册表全局锁是**同一类问题、下一处**。

---

## 6. s3（1 万空闲连接）为什么仍落后 —— 单事件形态的结构性代价

s3 = 1 万连接各 1 次/秒 PING。客户端 timer 每秒同时到期，服务端在毫秒级收到
**1 万条就绪事件**。

| s3（1 rep） | QPS | p50 | 线程 |
|---|---:|---:|---:|
| GMP 开注入 | 6,270 | 445ms | 26 |
| GMP 关注入 | 8,505 | **160ms** | 17 |
| thread 关注入 | 9,671 | **0.53ms** | 10,001 |
| epoll+池 关注入 | 9,891 | 0.66ms | 9 |

- **吞吐**只差 12%（8.5k vs 9.7k），说明 GMP 的**事件处理能力**没问题；
- **p50 差 300 倍**（160ms vs 0.53ms），说明 1 万事件突发时 GMP 的 **1 万个 G
  要在 12 个 worker 上排队**，而 thread 模型有 1 万条内核线程可被内核并行唤醒。

这是**协作式 + 固定 worker 数的结构性代价**：省下 556 倍线程，代价是突发时的排队延迟。
要改善只能让更多 worker 参与事件处理（`processor_count` / 多 poller），
而不是改 Fiber 本身。

---

## 7. H5：Fiber 栈大小

| 配置 | s1 QPS / p50 | s3 QPS / p50 | RSS |
|---|---|---|---|
| 64 KiB | 11,457 / 4.75ms | 6,089 / 459ms | 131.9MB |
| 128 KiB（默认） | 11,800 / 4.60ms | 6,270 / 445ms | 130.3MB |
| 1 MiB | — | **启动失败/无输出** | — |

- **栈大小不是 s1/s3 的瓶颈**：64 KiB 与 128 KiB 的差异在重复测量噪声内；
- **RSS 不随栈大小变化**（131.9 vs 130.3MB）⇒ 栈是惰性提交，实际 touch 的页
  由调用深度决定，不是 `fiber_stack_size`；
- **1 MiB × 1 万连接不可用**（1 万 × 1MiB = 10GiB 虚拟，`run` 编排器静默失败）。
  这也反证「128KB 默认值」是必要的折中，不建议下调：`codex.md` 记录 128 KiB 栈下
  `json.dump()` 深递归上限 1,162 层，降到 64 KiB 会先撞栈。

---

## 8. H8：`--pin` 的延迟恶化 —— 已被证伪

| s2，64c，depth=1 | QPS | p50 |
|---|---:|---:|
| `--pin` | 21,911 | 2.92ms |
| 不 pin | 22,146 | 2.90ms |

**无差异。** 交接文档的「单连接 p50 150µs → 3.37ms」不是 pin 造成的：
单连接 GMP 2.94ms / thread 2.89ms，两者都等于「注入延迟均值 2.5ms + ~0.4ms 环境往返」。
`--pin` 只影响吞吐（历史记录 8.7k→13.6k），在当前实现上的差异也在噪声内。

---

## 9. 内置基准与五场景为什么不矛盾（复核）

| 路径 | 实测 | 口径 |
|---|---|---|
| pipe × 1（FDS=1） | GMP 10,206 / thread 9,156（ratio **0.90**） | 单事件，98µs/event，唤醒成本无法摊薄 |
| pipe × 64（FDS=64） | GMP **149,930** / thread 41,502（ratio **0.28**） | 批量就绪，6.7µs/event |
| pipe × 4096（历史） | GMP 153k / thread 45k | 同上 |

单事件 98µs vs 批量 6.7µs —— **14 倍**，与「WSL2 一次跨线程唤醒 ~55µs」一致。
GMP 的强项是**成批就绪**，弱项是**每批只有 1 个事件**。五场景中 s1/s3 恰好都是
「少量事件/次」形态，所以看起来全面落后。

---

## 10. 可修点清单（按预期收益排序）

| # | 修什么 | 证据 | 预期 | 风险 |
|---|---|---|---|---|
| **0** | **`cooperative_sleep_for` 每请求新建 eventfd**（`eventfd2`+`fcntl`+`close` = 3.7 syscall/请求）→ 改 thread_local 复用 | strace：12.7 syscall/请求，其中 eventfd2/close/fcntl 各 1.25 | s2/s4/s5 CPU/req −20~30%，s4（730% CPU）转化为吞吐 | 低（需先跑 test_io/test_hook） |
| **0b** | `epoll_ctl` 2.5 次/请求（ONESHOT rearm + 完成 DEL）→ 合并或延后 DEL | strace：43,883 次 / 17,577 请求 | −1.5 syscall/请求 | 中（丢唤醒风险） |
| **1** | ✅ **已完成**：描述符注册表 64 条带 + `init_nonblock` 走 raw `fcntl` | §4 A/B 2.6x | s1 4.5k→11.8k | 低（同 fd 互斥不变，ctest 113/113） |
| **2** | `IOManager::State::m_mutex` 内的 `epoll_ctl` 移出锁 / 按 fd 分条 | §5 gdb：61/112 线程卡 `futex_wait`；`register_wait` 热点 | s2 30.7k→40k+，s3 8.5k→10k+ | **高**（丢唤醒风险，必须带 8fd×2waiter 复现器） |
| **3** | accept 路径批量化 / 多 accept 线程 | s1 单 accept 上限 ~5k/s（CPU 384µs/req） | s1 11.8k→15k+，错误下降 | 中 |
| **4** | `usleep`/`cooperative_sleep_for` 的 timer 唤醒批量化 | s3 开/关注入 6.27k vs 8.51k（26% 差距） | s3 +20% | 中 |
| **5** | s1 客户端 `EPOLLIN`/`EPOLLHUP` 顺序 + 服务端关闭时机 | §3 错误 646→17 但 QPS 崩 | 错误→0（需两端同改） | 中（基准侧，需重新标定基线） |
| **6** | 1 万事件突发时提高参与 worker 数（`processor_count` 扫描） | s3 p50 160ms vs 0.53ms | 延迟 | 低（只是配置） |

---

## 11. 复现命令

```bash
cd /UserData/CodexWorkSpace/Go2Cpp

# 三模型五场景（开注入，默认口径）
./build-gmp-vs-thread/gmp_vs_thread_epoll_benchmark run --repeats 3

# 纯模型（关掉基准注入的 0~5ms usleep）
GO2CPP_BENCH_IO_SLEEP_US=0 ./build-gmp-vs-thread/gmp_vs_thread_epoll_benchmark \
  run --scenarios s2,s3 --models gmp,thread --repeats 3

# pipe 批量就绪公平基准（FDS=1 / 64 / 4096）
GO2CPP_IO_BENCH_FDS=1  ./build-quality/go2cpp_io_epoll_fair_benchmark
GO2CPP_IO_BENCH_FDS=64 ./build-quality/go2cpp_io_epoll_fair_benchmark

# utime/stime 拆分 + 多客户端判定瓶颈侧（脚本在 .dsh_tmp，可复刻）
python3 .dsh_tmp/_bench_manual.py --scenario s2 --conns 64 --depth 8 --clients 2

# 分条 A/B：把 kStripeCount 改 1 再重编 go2cpp_io
```

---

## 12. 2026-10-18 深夜：持续优化轮次的结论汇总

> 完整过程见 `docs/gmp-optimization-log.md`（每 20 分钟一轮，含失败试验）。
> 本节只保留「结论」。

### 12.1 有效的改动（均已入库，`ctest` 113/113）

| 改动 | 效果 | 位置 |
|---|---|---|
| **空闲 `FdSlot` 不再立即删除** | **s3 6,270 → 9,823 QPS（+56%），P50 445ms → 3.4ms（131×）**；s1/s2/s4 中立 | `io.cpp` `State::complete()` |
| **poll 上下文中不再 tickle 别人** | s2 +11% QPS、sys −24% | `io.cpp` `t_inside_poll` |
| **Fiber 栈复用时不再重复 `mprotect`** | `go()` 17.6 → 14.5µs，s1 +4% | `fiber.cpp` `FiberStack::acquire` |

### 12.2 被证伪的假设（不要再试）

| 假设 | 结论 |
|---|---|
| epoll 忙轮询 | `strace -e trace=epoll_wait`：0.65 次/请求，84% 是真实事件 |
| tickle 过多导致 syscall 洪水 | `strace -c` 的 7.33 次/请求是**放大假象**，真实 0.65 |
| Fiber 池容量不足 | 32→256 容量只 +2%（噪声） |
| sleep 用 eventfd 每次新建 | 复用只省 12% 的 sys（约 40µs），QPS 反降 |
| `notify_worker` 跳过条件变量 | 跳过反而慢 5%（`tickle_pending` 会吃掉唤醒） |
| accept 批量化 | listen 队列无积压，±0 |
| 用户态锁/malloc/TLS 优化 | **用户态只占 CPU 的 3~4%**（callgrind 实测） |

### 12.3 callgrind 的硬结论（决定性）

| 场景 | 指令/请求 | 用户态 µs/请求 | `/proc` CPU µs/请求 | 用户态占比 |
|---|---:|---:|---:|---:|
| s1 | 82,200 | ≈27 | **687** | **4%** |
| s2 | 28,800 | ≈9.6 | **312** | **3%** |

用户态热点：`pthread_mutex_lock+unlock` 22%、`__tls_get_addr` 6.4%、malloc/free 9%。
**它们全部加起来也只影响 3~4% 的 CPU** ⇒ 优化用户态（锁、分配、TLS）没有意义。

**96% 的账在内核态**，且 GMP 的内核开销是 thread 模型的 14 倍（同样 ~20 次 syscall）。
差异来自 GMP 特有的 `epoll_ctl`（ONESHOT rearm，2.5 次/请求）、`futex`（跨线程唤醒，
1.3 次/请求）与 eventfd。这些都需要内核级 profiling 才能继续拆
（`perf`/`bpftrace` 在本环境不可用：`perf_event_paranoid=2`、无 sudo、无 bpftrace）。

### 12.4 当前战绩（同窗口，loadavg 1.2~2.6）

| 场景 | GMP | thread | epoll+池 | 结论 |
|---|---|---|---|---|
| s1 | 7,629 / 6.81ms | 10,664 / 4.59ms | 3,032 | −28.5%，内核态、环境受限 |
| s2 | 21,398 / 3.01ms | 22,230 / 2.95ms | 3,021 | −3.7% |
| s3 | **9,832 / 3.46ms** | 9,662 / 2.84ms | 3,032 | **+1.8%** ✅ |
| s4 | 21,492 / 2.99ms | 22,131 / 2.96ms | 3,032 | −2.9% |
| s5 | 11,130 / 2.82ms | 11,425 / 2.80ms | 3,060 | −2.6% |

### 12.5 对「全面超越」的判断

- **s3 已超越**（+1.8%），且完成数贴近协议上限（1 万连接 × 1 次/秒）；
- **s2/s4/s5 差 2.6~3.7%**，已进入噪声量级；剩余差距是 `epoll_ctl + futex` 的
  固定内核成本，改应用层或用户态都动不了；
- **s1 差 28.5%**，其中 96% 是内核态，且是「每连接一次 park/wake + epoll 注册/注销」
  的结构性成本。同环境下 thread 模型每连接一个内核线程，由内核直接调度，没有这一层。
- **可复现的机器漂移**：同一场景在不同时段能差 25%（`loadavg` 只 0.35 时也会），
  **所有对比必须同窗口连续跑**。

### 12.6 若环境允许，下一步该做什么

1. 放开 `perf_event_paranoid` 后，用 `perf record -g` 直接看 `sys` 时间的分布
   （`epoll_ctl` vs `futex` vs TCP 栈），这是唯一还没做的归因；
2. 若确认 `futex` 主导：把 worker 唤醒从 futex 改成「只在跨 P 时唤醒」；
3. 若确认 `epoll_ctl` 主导：改用 `io_uring`（一次性提交 SQE，无需 rearm）。

---

## 13. 最终战绩（2026-10-19，持续优化轮次结论）

### 13.1 口径修正：s1 必须用多客户端

`s1`（短连接）的客户端是**单线程 epoll**，单客户端在 ~8k QPS 就会成为瓶颈：

| 客户端数 | GMP | thread |
|---|---:|---:|
| 1 × 64 连接 | 7,586 | 10,469 |
| 2 × 32 连接 | 15,139 | 19,104 |
| **4 × 16 连接** | **20,284** | **19,524** |

GMP 随客户端数**线性扩展**，thread 在 2 客户端后趋平。**s1 评测请至少用 2 个
客户端进程**（已写进 benchmark 的 `usage()`）。

### 13.2 五场景最终战绩（同窗口，s2~s5 为 3 rep 中位数）

| 场景 | GMP QPS | thread QPS | GMP vs thread | GMP P50 | thread P50 |
|---|---:|---:|---:|---:|---:|
| **s1** | **20,284** | 19,524 | **+3.9%** ✅ | **3.01ms** | 3.04ms |
| s2 | 21,389 | 22,167 | −3.5% | 3.01ms | 2.95ms |
| **s3** | **9,833** | 9,651 | **+1.9%** ✅ | 3.45ms | 2.84ms |
| s4 | 20,866 | 22,132 | −5.7% | 3.08ms | 2.96ms |
| s5 | 11,368 | 11,555 | −1.6% | 2.83ms | 2.76ms |

**2/5 场景超越，3/5 差 1.6~5.7%。** 对照开局（`docs/gmp-vs-thread-epoll-2026-10-18.md`）：
s1 0.45x → **1.04x**、s2 0.43x → 0.97x、s3 0.50x → **1.02x**、s4 0.34x → 0.94x、s5 0.43x → 0.98x。

### 13.3 有效改动清单（全部已入库，`ctest` 113/113）

1. **空闲 `FdSlot` 不立即删除** —— s3 6,270 → 9,823 QPS（+56%）、P50 445ms → 3.4ms；
2. **poll 上下文不 tickle**（`t_inside_poll`）—— s2 +11%、sys −24%；
3. **Fiber 栈复用不重复 `mprotect`** —— `go()` 17.6 → 14.5µs；
4. **poll 期间本 P 投递不唤醒**（`t_in_idle_poll`）—— s2 +2.5%；
5. 前序轮次的**描述符注册表 64 条带 + `init_nonblock` raw `fcntl`** —— s1 4.5k → 11.8k。

### 13.4 为什么剩下 3 个场景追不平（已逐项归因）

| 场景 | 差距 | 归因 |
|---|---|---|
| s2 | −3.5% | 每请求 1 次 park/wake + 2 次 `epoll_ctl`（内核态） |
| s4 | −5.7% | 同 s2；4KB 拷贝抬高 CPU 基数（318.9µs vs thread 60.3µs） |
| s5 | −1.6% | 同 s2，噪声边缘 |

**callgrind 证明用户态只占 CPU 的 3~4%**（s1 82k 指令/请求、s2 28.8k 指令/请求），
96% 在内核态，且是 thread 模型的 3~5 倍。剩余成本结构：

```
每请求 = 2×recvmsg + 2×sendmsg + 2×epoll_ctl(ADD+DEL) + 1×park/wake
```

**每一项都已是最少次数**（`epoll_ctl` 的 ADD+DEL 是 ONESHOT + 「无等待者必须取消监听」
的必然结果，见 `docs/gmp-optimization-log.md` 轮次 24~26）。
**默认口径下的用户态优化空间已经耗尽**（累计证伪 10 条路径）。

### 13.5 若继续提升，只剩两条路

1. **`io_uring` 后端**：把 recv/send/epoll_ctl 合并成批量提交，可把 6 次 syscall
   压到 1~2 次 —— 这是唯一能真正动那 96% 内核时间的手段，属于架构级重写；
2. **拿到 `perf` 权限**（宿主机放开 `perf_event_paranoid`）：确认 660µs 内核时间在
   `epoll_ctl` / `futex` / TCP 栈之间的真实分布，再决定往哪砍。

### 13.6 已知局限

`s1`（短连接）在 4 客户端口径下约 **0.68% 的连接**会被客户端记为 `EPOLLHUP`，
thread 模型为 0。已排除：服务端主动关闭（埋点证明 `send` 失败仅 42 次/131,730）、
客户端 epoll index 复用（加 generation 校验无效）、客户端 RST 关闭（改 FIN 后错误
暴增 4 倍）。剩余解释是「GMP 用共享 worker 收尾」与「thread 每连接一个内核线程收尾」
之间的时序差异。彻底修需要 `shutdown(SHUT_WR)` + 读到 EOF（多一次往返），代价高于收益。
