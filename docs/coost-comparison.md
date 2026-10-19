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
