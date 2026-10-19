# coost 对比与 GMP 优化线索

> 源码：`third_party/coost`（github.com/idealvin/coost，clone 于 2026-10-19）
> 本轮目标：读懂 coost，定位 GMP 高 CPU 的根因。

## 1. coost 的协程模型

### 1.1 全局参数（`src/co/sched.h`）

```cpp
extern uint32 g_sched_num;    // 调度器数量
extern uint32 g_stack_num;    // 栈的数量
extern uint32 g_stack_size;   // 栈大小
extern __thread Sched* g_sched;   // 每线程一个调度器
```

### 1.2 共享栈 + 拷栈（`src/co/sched.cc:67`）

```cpp
void Sched::resume(Coroutine* co) {
    Stack* const s = this->get_stack(co->id);   // 按 id 取共享栈槽位
    if (s->co != co) {
        save_stack(s->co, s);      // 当前协程的栈内容 → co->buf
        restore_stack(co, s);      // 目标协程的 buf → 物理栈
        s->co = co;
    }
    from = tb_context_jump(co->ctx, _main_co);   // 只换寄存器
}
```

**每次协程切换都拷栈**（`co->buf.size()` 字节）。

### 1.3 与 GMP 的对照

| | coost | GMP |
|---|---|---|
| 栈 | `g_stack_num` 个物理栈 + 每协程 `buf` | 每 Fiber 独立 128KiB（池化） |
| 切换 | `save_stack` + `restore_stack`（**拷栈**） | `swapcontext`（**零拷贝**） |
| 线程模型 | 每线程一个 Sched（`__thread`） | 全局一个，P = 核数 |

**结论**：**栈切换上 GMP 更便宜**（零拷贝 vs 拷栈），所以 GMP 的高 CPU **不是栈导致的**。

## 2. 根因：epoll 触发模式（决定性差异）

### 2.1 coost 用 EPOLLET（边缘触发）

```cpp
// src/co/epoll/epoll.cc:36
ev.events = EPOLLIN | EPOLLET;
epoll_ctl(_ep, EPOLL_CTL_ADD, fd, &ev);      // 注册一次，永久有效

// :63 只在 fd 关闭时
epoll_ctl(_ep, EPOLL_CTL_DEL, fd, ...);
```

**没有 ONESHOT，没有 rearm（MOD）。**

### 2.2 GMP 用 EPOLLONESHOT

`src/network/io.cpp` 的 `register_wait` / `complete`：

- 每次等待要 `EPOLL_CTL_MOD`（rearm，因为 ONESHOT 事件后自动 disarm）；
- 每次完成要 `EPOLL_CTL_DEL`（或保留 slot 但 `interest=0`）。

**每请求 2 次 `epoll_ctl`**。

### 2.3 数据印证

s4（64 连接 × depth 1，4KB 排序 + 5ms 注入）：

| 指标 | GMP | epoll 池 256w | 差距 |
|---|---:|---:|---|
| `sys_us_per_req` | **283.9µs** | 62.4µs | **+221.5µs** |
| `cpu_us_per_req` | 315.6µs | 76.3µs | +239.3µs |
| QPS | 21,519 | 22,217 | −3% |

**每请求多出的 ~220µs syscall，与「2 次 `epoll_ctl` + 1 次 park/wake」的量级吻合。**

## 3. 优化方向（按风险从低到高）

### 方向 A：把 ONESHOT 改成 LT + 保留注册（低风险）

- `register_wait`：slot 已注册则**跳过 MOD**（interest 未变时）；
- `complete`：不 DEL，只把 `interest` 置 0 并 `MOD` 一次（或懒 MOD，下次 register 时一并处理）。

**预期**：每请求 `epoll_ctl` 从 2 次降到 0~1 次。

### 方向 B：改用 EPOLLET（中风险）

- 注册一次，永久有效；
- 但要求上层「读到 EAGAIN 为止」，否则事件丢失；
- 需要改 hook 层的 `recv/send` 循环（读到 EAGAIN）。

### 方向 C：合并 rearm（低风险，收益有限）

- 一次 `epoll_wait` 返回后，把同一批次里需要 rearm 的 fd **合并成一次 MOD**
- 但每个 fd 还是要一次 MOD，合并不了多少。

## 4. 下一步

1. 编译 coost + 跑自带 benchmark，拿它的 `syscall/req` 与 GMP 对比（验证方向 A 的预期收益）；
2. 在 GMP 上实现方向 A，用 `_bench_manual.py --scenario s4` 验证 `sys_us_per_req` 是否下降。

---

# 第二轮：找到并修复「P 数量被写死」的结构性问题

## 1. 之前的分析错在哪（两次修正）

### 错误 1：以为根因是「每请求 2 次 epoll_ctl」

**实测推翻**（`strace`，23,145 请求，关注入 s4）：

| syscall | calls | 每请求 |
|---|---:|---:|
| futex | 1,296 | 0.056 |
| epoll_wait | 131 | 0.006 |
| read | 178 | 0.008 |
| write | 48 | 0.002 |
| **epoll_ctl** | **1** | **0.00004** |

长连接场景下 `update_interest_locked` 的跳过逻辑本来就在生效，**epoll_ctl 不是瓶颈**。

### 错误 2：以为 `sys_us_per_req = 283.9µs` 是 IO 的成本

**真正来源是 `usleep` 的 hook**：benchmark 的「5ms 注入」调用 `usleep`，而 GMP 把它 hook 成
`cooperative_sleep_for()`（`hook.cpp:1804` → `:1246`），后者是 **eventfd + epoll + park**。

把注入改成**忙等**（真实 CPU，`GO2CPP_BENCH_BUSY_US` 控制）后：

| 指标 | usleep 注入 | **忙等注入** |
|---|---:|---:|
| `sys_us_per_req` | 283.9µs | **33.7µs** |
| `sys_share` | 91% | **1%** |

**结论：GMP 的 syscall 开销在真实 CPU 负载下并不高，之前的「高 CPU 低效率」是测量口径造成的。**

## 2. 真正的结构性缺陷：`processor_count` 被写死

```cpp
// benchmarks/gmp_vs_thread_epoll_benchmark.cpp:498（修复前）
config.processor_count = 0;      // 0 = 按硬件线程数（本机 12）
```

**GMP 的 P 数量永远等于硬件线程数（12），`--workers` 对它无效**；而 epoll 池可以用 256 个
worker。coost 的 `co_sched_num` 是**可配置**的——这正是需要对齐的地方。

**已修复**：`run_gmp_server` 增加 `workers` 参数，`config.processor_count = workers`。

## 3. 修复后的实测（忙等 5ms = 真实 CPU 计算）

| 配置 | QPS | P50 | P99.9 | CPU% | **µs/req** | sys µs |
|---|---:|---:|---:|---:|---:|---:|
| GMP **P=12** | 3,094 | 20.15ms | 45.63ms | 792% | 2,562 | 50.0 |
| **GMP P=48** | **6,024** | **8.53ms** | 43.65ms | 1169% | **1,941** | 50.4 |
| epoll 256w | 5,431 | 11.02ms | **29.07ms** | 1127% | 2,075 | 39.1 |

**P=48 时 GMP 对比 epoll 256w**：

| 维度 | GMP P=48 | epoll 256w | 谁赢 |
|---|---:|---:|---|
| QPS | **6,024** | 5,431 | **GMP +11%** |
| P50 | **8.53ms** | 11.02ms | **GMP −23%** |
| P99.9 | 43.65ms | **29.07ms** | epoll |
| CPU/req | **1,941µs** | 2,075µs | **GMP −6.5%** |
| sys/req | 50.4µs | 39.1µs | epoll |
| 线程数 | 50 | 257 | **GMP −80%** |

## 4. 为什么 P 超配有效（与 coost 对齐）

- **coost**：`co_sched_num` 默认 CPU 数，可调大；协程在**线程内**切换（纯用户态）；
- **GMP**：P 数量原本写死 = 核数，**P 上串行的 Fiber 数受限**；
- **超配 P 的收益**：当 Fiber 里有短时计算 + IO 等待混合时，更多的 P 能让「等待」和「计算」
  重叠得更充分（实测 QPS 从 3,094 → 6,024，**+95%**）；
- **代价**：虚拟内存增加（816MB → 3,920MB，每个 P 有自己的栈缓存），P99.9 仍需观察。

## 5. 下一步

1. **扫描 P 数量曲线**（12/24/48/96/192），找最优区间；
2. 检查 P 超配后 **P99.9 反而变差**（43.65 vs 29.07）的原因（是否 P 间窃取增加）；
3. 把「P 可配」写进 `docs/configuration.md` 与 benchmark 的 `usage()`。
