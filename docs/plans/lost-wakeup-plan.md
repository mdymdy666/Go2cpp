# 丢唤醒（lost wakeup）专项排查计划

> 立项：2026-11-04 晚
> 背景：今天一天内在 **3 个不同模块**各发现一次同类 bug，全部导致「线程永久睡死」，且单测/常规压测都抓不到。
> 结论：这不是偶发，是**系统性的并发原语使用缺陷**，必须专项排查。
> 目标：**2026-11-05 完成全仓排查 + 修复 + 回归验证**。

---

## 一、今天已发现的 3 例（全部已修）

| # | 模块 | 位置 | 机制 | 修法 | 提交 |
|---|---|---|---|---|---|
| 1 | `RWMutex` | `include/go2cpp/concurrency/rwmutex.hpp` | 读者 `m_waiting_readers++` 后、park 前不重查谓词；`Unlock` 先改 `m_state` 再读等待计数 → 读者登记落在两者之间，唤醒被跳过 | 登记后重查 `m_state + m_pending`，成立则回滚计数 | `c5b888f` |
| 2 | `MuxStream` | `src/tunnel/mux.cpp` | `Close()`/`MarkPeerClosed()` **锁外**置位 + `notify_all` → 撞上「谓词判 false、尚未挂 futex」窗口 | 置位 + notify 都在 `m_mutex` 内；`SendPayload` 取锁后复查 `alive()` | `b176c7f` |
| 3 | `tunnel Direction` | `src/tunnel/{server,client}.cpp` | `Run(nullptr)`（`Context::Background()`）永不取消 → `RelayPipe::Run` 卡 `done_cv.wait` | `Context::WithCancel` + `Stop` 先 cancel + 在途连接 `Shutdown` + `TrackInflight` RAII | `8ebeb53` / `850e813` |

## 二、根因模式（三例的共同点）

```
线程 A（等待方）                      线程 B（唤醒方）
1. lock(m)
2. while (!pred) { ... }              1. 改状态（可能在锁外！）
3. cv.wait(m)  ← 挂 futex              2. cv.notify_all()（可能在锁外！）
                                       
窗口：A 判完 pred=false → B 改状态+notify → A 才挂 futex → 唤醒丢失 → A 永久睡死
```

**三条必守规则**（写进 `docs/engineering-standards.md` 的并发章节）：

1. **状态变更与 notify 必须在同一把锁内**——即使 notify 本身不需要锁；
2. **谓词必须是 `std::atomic` 或受同一把锁保护**，且等待方用 `while (!pred)` 而非 `if`；
3. **禁止在 `Context::Background()` 上阻塞**——所有可取消的等待必须带 cancel token 或超时。

## 三、全仓需要排查的 condvar 使用点（30 个文件）

按风险排序（**P0 = 并发核心 + 长生命周期；P1 = 请求路径；P2 = 工具/调试**）：

### P0（先查，6 个）

| 文件 | 重点 |
|---|---|
| `- [x]` `src/concurrency/parking_condition.cpp` | 所有 fiber 的 park/unpark 都走它（**已审 2026-11-05：无丢唤醒**，见执行记录） |
| `- [x]` `src/concurrency/sync.cpp` | Mutex/CondVar/WaitGroup（**已审 2026-11-05：发现并修复 P0**，见执行记录） |
| `- [x]` `src/concurrency/scheduler.cpp` | worker 唤醒、sysmon（**已审 2026-11-05：无丢唤醒**，见执行记录） |
| `- [x]` `src/concurrency/timer.cpp` | 定时器队列唤醒（**已审 2026-11-05：无严重丢唤醒**，见执行记录） |
| `- [x]` `src/concurrency/context.cpp` | cancel 传播（**已审 2026-11-05：无丢唤醒**，见执行记录） |
| `- [x]` `include/go2cpp/concurrency/channel.hpp` + `channel.cpp` | `Channel::Send/Recv` 阻塞（**已审 2026-11-05：无丢唤醒**，见执行记录） |

### P1（请求路径，8 个）

| 文件 | 重点 |
|---|---|
| `- [x]` `src/network/io.cpp` | IOManager 事件队列（**已审 2026-11-05：无严重丢唤醒**，见执行记录） |
| `- [x]` `src/tunnel/{mux,server,client}` | 已修 2 处（`b176c7f`/`8ebeb53`/`850e813`），**复查剩余：无丢唤醒**（见执行记录） |
| `src/relay/{relay_pipe,splice_pipe,lease}.cpp` | 数据面 |
| `src/rpc/server.cpp` | 新增的请求队列 |
| `src/app/{gateway,realtime_room,transport}.cpp` | 应用层 |
| `src/distributed/transport.cpp` | 连接池 |

### P2（工具/调试/存储，其余 16 个）

`src/storage/watch.cpp`、`src/os/pool.cpp`、`src/p2p/signaling.cpp`、`src/bento/net/memory_transport.cpp`、
`src/debug/*`、`include/go2cpp/logging/rotate.hpp`、`include/go2cpp/distributed/{metadata_store,coordinator}.hpp`、
`include/go2cpp/concurrency/core/futex_mutex.hpp` 等。

## 四、排查方法（每个文件 4 步）

### 步骤 1：静态模式扫描

对每个 `cv.wait*` 检查：

```bash
# 找出 wait 前未持锁 / 谓词非原子的模式
grep -rn "\.wait\|wait_for\|wait_until" src/ include/ | while read -r line; do ... done
```

**红旗模式**：
- `if (!pred)` 而非 `while (!pred)`（虚假唤醒未处理）；
- `pred` 是普通 `bool`/`int` 而非 `atomic`，且未受同一把锁保护；
- `notify_*` 出现在 `unlock()` 之后（锁外通知）；
- `wait` 没有超时且没有 cancel 检查（可能永久睡死）。

### 步骤 2：TSan + 压力

```bash
setarch x86_64 -R ./build-tsan/<target>   # 至少 10 次
```
TSan 报的 `ThreadSanitizer: data race` 若涉及谓词变量 → **必有丢唤醒风险**。

### 步骤 3：注入延迟（最有效的复现手段）

在 `notify_*` 之前插 `std::this_thread::sleep_for(1us)`（仅测试构建）——**放大窗口**。
今天 `MuxStream` 就是靠这个稳定复现的。

### 步骤 4：看门狗

给每个等待加「等待 >2s 就打印调用栈」的诊断（`debug_dump` 的 watchdog），跑长稳。

## 五、修复模板

```cpp
// ❌ 错误：锁外置位 + notify
void Close() {
    closed_.store(true);          // 锁外
    cv_.notify_all();             // 锁外 → 丢唤醒
}

// ✅ 正确：置位 + notify 同锁；谓词用 while
void Close() {
    {
        std::lock_guard lock(m_);
        closed_ = true;
        cv_.notify_all();         // 持锁通知
    }
    // 耗时操作（网络写）移到锁外
}

// ✅ 等待方
std::unique_lock lock(m_);
cv_.wait(lock, [&] { return closed_; });   // 谓词版 wait，内部 while
```

**如果 notify 必须锁外**（避免唤醒方抢锁），至少保证：
- 谓词是 `std::atomic`；
- 等待方用 `while (!pred) wait`；
- 状态变更用 release，等待方用 acquire。

## 六、2026-11-05 执行计划

| 时段 | 任务 | 产出 |
|---|---|---|
| 上午 | P0 六文件逐个人工审查 + 注入延迟复现 | 问题清单 |
| 下午 | P1 八文件 | 问题清单 |
| 傍晚 | 修复 + TSan 10 次 + 长稳 | 修复提交 |
| 晚上 | P2 扫尾 + 本文档更新（close out） | 最终报告 |

**验收标准**：
1. 全仓 condvar 使用点 **100% 审查过**（清单打勾）；
2. 每处都有「置位+notify 同锁」或「原子谓词 + while + 内存序」的证据；
3. TSan 在 tunnel/concurrency/rpc 各 10 次 **0 报告**；
4. 长稳 30 分钟无「等待超时」诊断日志。

## 七、防复发（机制）

1. **代码规范**：`docs/engineering-standards.md` 加「条件变量三规则」；
2. **CI 检查**：新增静态检查脚本（扫 `notify` 是否在锁外，可用 clang-tidy 自定义 check 或简单 grep）；
3. **测试模板**：所有 condvar 使用点必须有一个「注入延迟」的回归测试（今天 `MuxStream` 的教训）。

## 执行记录

| 时间 | 审查项 | 结论 |
|---|---|---|
| 2026-11-05 14:30 | `parking_condition.cpp`（P0 第 1 项） | ✅ **无丢唤醒**。两条路径分别核对：① **Linux futex 路径**（`:355-363`）`fetch_add(release)` + `FutexWake`，等待方 `FUTEX_WAIT(seq, expected)` 由内核保证「seq 已变则不阻塞」，无窗口；② **native 路径**（`:363-390` / `:512-521`）`Finish` 用 CAS(acq_rel) 改 `result`、`notify_one` 持 `m_wake_mutex`、等待方用 `unique_lock` + **谓词版** `wait(lock, pred)`——经典正确模式，谓词检查与挂起由 lock 串行化。无需修改。 |
| 2026-11-05 14:45 | `sync.cpp`（P0 第 2 项） | 🔴 **发现 P0 并已修**：non-Linux 路径 `m_native_condition.notify_one()` **不持 `m_native_mutex`**，而等待方是 `unique_lock(m_native_mutex) + wait(lock, pred)` → 「谓词已判 false、尚未挂 futex」窗口内 notify 必丢 → 普通线程永久睡死（Windows/macOS 受影响；Linux 靠 futex 的 (seq,expected) 免疫）。修法：notify 前加 `lock_guard<std::mutex>(m_native_mutex)`。 |
| 2026-11-05 15:00 | `scheduler.cpp`（P0 第 3 项） | ✅ **无丢唤醒**。三处 condvar 分别核对：① `Task` 终态等待（`:1096-1115`）`unique_lock` + 谓词 `wait`，`notify_terminal`（`:1154-1165`）用 Dekker（`terminal` release + `waiters` seq_cst）先判「有无等待者」，有则**先拿 `waiter->mutex` 做登记屏障再锁外 notify_all**——等待方在持锁状态下完成「登记+谓词复查」，锁保证通知必在登记之后，无窗口；② worker 睡眠（`:6629`）`condition.wait_for(lock, timeout, pred)` 标准谓词版，`notify_worker` 的锁外 `notify_one` 由 POSIX condvar「释放锁+挂起」的原子性保证不丢；③ sysmon（`:2616`）同样是谓词版 `wait_for`。无需修改。 |
| 2026-11-05 15:15 | `timer.cpp`（P0 第 4 项） | ✅ **无严重丢唤醒**。等待方两处都是**谓词版**（`:922` `m_cv.wait(lock, pred)`、`:954` `m_cv.wait_until(lock, deadline, pred)`）；notify 共 6 处（`:364/419/441/485/522/772`），前 4 处在 `lock_guard(m_mutex)` 内。**关键兜底**：`wait_until` 带 deadline——即便某处 notify 落在窗口内丢失，定时器也会在 deadline 到期时自然醒来，不会永久睡死。结论：不用改。 |
| 2026-11-05 15:30 | `context.cpp`（P0 第 5 项） | ✅ **无丢唤醒**（标准 Dekker）。等待方 `:494-499` 持 `st.mutex` 后判谓词、**同锁内** `++waiter_count`（`WaiterScope`），再谓词版 `wait`；唤醒方 `Signal` `:682-698` **同锁内** `done.store(release)` + 读 `waiter_count`，**锁外** `notify_all`。两条交错：① 唤醒先拿锁 → 读计数 0 跳过 notify，但等待方随后判谓词必见 `done=true` 直接返回；② 等待方先登记 → 唤醒方读计数 ≥1 必 notify。无窗口。三处 wait 均带谓词/超时（`:497/520/542`）。 |
| 2026-11-05 15:45 | `channel.hpp` + `channel.cpp`（P0 第 6 项，P0 收尾） | ✅ **无丢唤醒**。① `SelectWaitState`（`:800-855`）：`TrySelect`/`TrySelectPair` 持锁置 `m_selected`（`:766-775`/`:800-812`）、锁外 `notify_one`；等待方 `WaitFor`/`Wait`（`:821-837`）都是 `unique_lock(m_mutex)` + **谓词版** wait（谓词 `m_selected \|\| m_cancelled \|\| ctx->IsDone()` 在锁内检查）——置位与谓词同锁 = Dekker，无窗口；② `Cancel()`（`:844-853`）锁内置 `m_cancelled`、锁外 notify，同型；③ `m_change_cv`（`:2050/2084/2871`）`wait_for(lock, timeout, pred)` 带超时 + 谓词，即使 notify 丢失也会在超时后重检。P0 六文件全部审完。 |
| 2026-11-05 16:00 | `io.cpp`（P1 第 1 项） | ✅ **无严重丢唤醒**。两处 condvar：① `callback_cv`（`:455-470`）`end_callback` 在 `lock_guard(callback_mutex)` 内清 `callback_active`、锁外 `notify_all`，`stop_callbacks` 用 `unique_lock` + **谓词版** wait——Dekker（置位与谓词同锁），无窗口；② `m_queue_cv`（`:2013-2021`）`wait_for(lock, effective, pred)` **带超时 + 谓词**，即使 notify 落在窗口内丢失，超时后也会重检 `m_queue_depth`/`m_stop_poller`，不会永久睡死。生产端 `notify_one/all` 在锁外属正常写法（等待方是谓词版）。不用改。 |
| 2026-11-05 16:15 | `tunnel/{mux,server,client}`（P1 第 2 项） | ✅ **无丢唤醒**（含此前 3 处修复的复查）。① `MuxStream::m_cv`（`mux.cpp:267`）`wait(lock, pred)` 谓词版；`Close/MarkPeerClosed`（`:325-334`）**已修**为持 `m_mutex` 的 `lock_guard` 内 CAS + notify_all（注释写明窗口与 TSan 症状），网络动作移到锁外；② `m_ack_cv`（`:430/662/751`）、`m_closed_cv`（`:663/935`）、client 的 `m_state_cv`（`:171/474/484/491/539/563`）与 `workers->cv`（`:152`）全部 `wait_for(lock, timeout, pred)`——**带超时兜底**，即使 notify 丢失也会超时重检。无需修改。 |
