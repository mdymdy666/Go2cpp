# IOManager ET 快路径只读调查报告

- 调查对象：`/UserData/CodexWorkSpace/Go2cpp`（WSL 真树，`git rev-parse --short HEAD` = `0f6fd2e`）
- 调查方式：纯静态阅读 + 历史提交比对；**未改任何代码、未 commit、未压测**
- 时间盒：单轮 ≤35 分钟，三击规则
- 结论前置：**题设中的「EPOLLONESHOT + EPOLLET 混合」与当前代码不符**。ONESHOT 已于提交 `214fbde` 移除，当前 linux 后端是纯 ET；`io.cpp:273/1871` 的 EPOLLONESHOT 只是**未同步的注释**。因此「去 ONESHOT」这一优化已经落地，本报告把调查重点改为：ET 落地后还剩哪些 `epoll_ctl`，以及快路径是否真的成立。

---

## ① 现状流程（代码证据）

### 1.1 后端：纯 ET，`supports_one_shot()` 返回 false

`src/network/io_backend.cpp:66-72`：

```cpp
    /**
     * @brief 返回 epoll 后端支持 EPOLLONESHOT。
     * @details IOManager 事件消费后显式重新 arm，减少重复就绪通知。
     */
    // Linux epoll 使用 ONESHOT，IOManager 在每次消费后显式重新布防。
    // 这样无等待者时不会反复收到同一就绪边沿，下一次等待只需一次 MOD。
    bool supports_one_shot() const noexcept override { return false; }
```

注意：**注释说 ONESHOT，返回值是 false**。这是 `214fbde`（2026-10-19，"perf: 修正 benchmark 不公平 + ET 优化"）的改动，该提交 diff 明确：

```diff
-    bool supports_one_shot() const noexcept override { return true; }
+    bool supports_one_shot() const noexcept override { return false; }
...
-            event.events = EPOLLERR | EPOLLHUP;
-            if (supports_one_shot()) {
-                event.events |= EPOLLONESHOT;
-            }
+            event.events = EPOLLERR | EPOLLHUP | EPOLLET;
```

`src/network/io_backend.cpp:79-99`（当前）：

```cpp
        if (interest != 0) {
            event.events = EPOLLERR | EPOLLHUP | EPOLLET;
            if (interest & kRead) event.events |= EPOLLIN | EPOLLRDHUP;
            if (interest & kWrite) event.events |= EPOLLOUT;
        }
        event.data.u64 = token;
        int operation = modify ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        if (::epoll_ctl(m_epoll_fd, operation, fd, &event) == 0) return 0;
```

`interest == 0` 时 `event.events` 保持 0（`epoll_event event{};`），即 `EPOLL_CTL_MOD` 成「保留注册、不监听普通读写」——这是 ET 方案的关键技巧，用来避免 DEL/ADD。

### 1.2 FdSlot：没有独立等待者计数，队列 `size()` 就是计数

`src/network/io.cpp:612-638`：

```cpp
    struct FdSlot {
        int fd{-1};                                  // 612
        std::uint64_t generation{0};                 // 616
        DescriptorTokenPtr descriptor;               // 620
        Queue readers;                               // 621
        Queue writers;                               // 624
        bool registered{false};                      // 626
        bool armed{false};                           // 629
        std::uint32_t interest{0};                   // 631
        std::uint64_t registration_id{0};            // 633
        bool force_rearm{false};                     // 635
        bool pending_read{false};                    // 637
        bool pending_write{false};                   // 638
    };
```

- 等待者计数：`readers.size() + writers.size()`（`std::deque`，O(1)）。**没有单独字段**，只读诊断接口 `waiter_depth()`（`io.cpp:2188-2197`）用同一表达式，且仅在 `GO2CPP_DEBUG_RESOURCES` 下编译。
- `armed` 在 ET 后端恒为 `true`（见 1.5 的赋值），对 ET 已是死字段。
- `descriptor` 是跨轮复用的强引用令牌，避免每轮 `F_GETFD`。

### 1.3 注册链：`IOManager::wait` → `register_wait` → `update_interest_locked`

入口 `IOManager::wait`（`io.cpp:2320` 起）在 step3 调注册：

`io.cpp:2377-2383`：

```cpp
    node = m_state->register_wait(fd, event, deadline,
                                  &reg_error, std::move(descriptor));
    if (!node && reg_error == 0) { ... }
```

`register_wait`（`io.cpp:817-1070`）六步（函数头见 `io.cpp:805-813`）：① 校验 token/fd/mask（817-844）；② 先读 FdSlot 缓存 descriptor，未命中才 `Capture`（845-866）；③ 取 fd 条带锁，二次校验代际 + `m_accepting`（869-900）；④ 从 TLS `s_node_cache` 复用/新建 `WaitNode`，维护 generation/descriptor（902-963）；⑤ 消费 `pending_*` 并用**零超时 `syscall(SYS_poll)` 二次确认**（965-1010）：

```cpp
   const long probe_result = ::syscall(SYS_poll, &probe, 1, 0);
   if (probe_result < 0 && errno != EINTR) { ... claim(kError) }
   else if (probe_result <= 0 || (probe.revents & POLLNVAL) != 0) pending_ready = false;
```

⑥ 调 `update_interest_locked` arm，失败回滚节点/定时器（1040-1060）。

### 1.4 事件链：`poll_once` → `process_events_batch` → `process_event_locked` → 唤醒

`io.cpp:1942-1992`（poll_once）step3-6：

```cpp
        do {
            count = m_backend->wait(events, kEventBatch, effective);
        } while (count < 0 && -count == EINTR);
        ...
        if (saw_wake) { drain_wake_fd(); m_tickle_pending.exchange(0, ...); }
        if (count > 0) process_events_batch(events, static_cast<std::size_t>(count));
        expire_timers();
```

`process_events_batch`（`io.cpp:1636-1655`）逐事件取**自己 fd 所属条带锁**，锁内调 `process_event_locked`，全部事件收集进 `WakeList` 后**锁外**统一 `wake_nodes`：

```cpp
        for (std::size_t index = 0; index < count; ++index) {
            if (events[index].token == kWakeRegistration) continue;
            SlotStripe& stripe = stripe_for(token_fd(events[index].token));
            std::lock_guard<std::mutex> lock(stripe.mutex);
            process_event_locked(events[index].token, events[index].events, wake);
        }
        wake_nodes(wake);
```

`process_event_locked`（`io.cpp:1509-1585`）核心：

```cpp
            const auto interest = slot.interest;
            const bool error_or_hup = (events & 0x0cU) != 0;
            const bool read_ready = error_or_hup || (events & kReadMask) != 0;
            const bool write_ready = error_or_hup || (events & kWriteMask) != 0;
            if (read_ready && (interest & kReadMask) != 0) {
                if (auto node = take_ready_locked(slot.readers)) {
                    slot.pending_read = false;
                    wake.push_back(std::move(node));
                } else {
                    slot.pending_read = true;
                }
            }
            // EPOLLET：本轮边沿已被消费。同一 fd 还有等待者时，必须让内核
            // 重新检查就绪状态并再产生一次边沿，否则后续等待者永远等不到。
            if (!one_shot && (!slot.readers.empty() || !slot.writers.empty())) {
                slot.force_rearm = true;
            }
            const int arm_error = update_interest_locked(slot);
```

唤醒：`wake_nodes`（`io.cpp:1994-2054`）先用 `wake_task_claimed()` 取 task 快照，>1 个时走 `m_scheduler->wake_io_batch(tasks)`，否则逐节点 `node->wake()` → `Scheduler::wake_io`（`io.cpp:123-128` 的 `WaitNode::wake`）。

醒来后做什么：`wait` 里 `park_io` 返回，读取 `node->outcome`，`recycle_node` 把节点放回 TLS 缓存（`io.cpp:1096-1135`）。**调用方负责重试真实 syscall**（`hook.cpp` 的 `cooperative_io` step6 回到 step4），IOManager 不替它读。

### 1.5 重新 arm：三条分支，稳态走「不 ctl」

`update_interest_locked`（`io.cpp:1351-1425`）先按队列算 interest：

```cpp
        std::uint32_t interest = 0;
        if (!slot.readers.empty()) interest |= kReadMask;
        if (!slot.writers.empty())  interest |= kWriteMask;
        const bool one_shot = m_backend && m_backend->supports_one_shot();
        if (interest == 0) {
            // 边沿触发后端：等待队列为空也保留注册与上次的兴趣位。
            if (!one_shot && m_backend && slot.registered) return 0;   // ← ET 稳态：不 DEL、不清兴趣
            ...
        }
        if (slot.registered && slot.interest == interest &&
            (!one_shot || slot.armed) && !slot.force_rearm) {
            return 0;                                                   // ← 同方向稳态：不 MOD
        }
        ...
        result = m_backend->add_or_modify(slot.fd, interest, registration_id, slot.registered);
        ...
        slot.registered = true;
        slot.armed = !one_shot || interest != 0;                        // ← ET 下恒 true
        slot.interest = interest;
```

三个关键点：

1. **队列空时不改 `slot.interest`**（第 1366-1370 行直接 return），内核仍监听上一次的方向位。设计意图是配合 `pending_*`：无等待者时到达的事件被记成 pending，下一次 `register_wait` 直接消费，省一次 MOD。
2. 队列非空且方向未变 ⇒ 早退，**不发 ctl**。
3. 方向变了（读↔写）⇒ `slot.interest != interest` ⇒ **一次 MOD**。

### 1.6 全仓 `epoll_ctl` 调用点

| 位置 | 操作 | 触发条件 | 频率（单 fd 单等待者稳态） |
|---|---|---|---|
| `io_backend.cpp:53` | ADD（eventfd） | 后端初始化 | 1 次/进程 |
| `io_backend.cpp:92-98` | ADD/MOD（含 EEXIST/ENOENT 纠错重试） | `update_interest_locked` 判定需要 | 首次 ADD；方向切换 MOD；否则 0 |
| `io_backend.cpp:105` | DEL | `invalidate_registration_locked`（close/代际变化/shutdown） | 关闭时 1 次 |
| `io_backend.cpp:120` 的 `wait` | `epoll_wait` | poll_once / worker 空闲 | 每轮 1 次（不是 ctl） |

`m_epoll_mutex`（`io.cpp:2171`）用 `try_lock` 包住 ctl（1340、1410），失败则直连——高并发下内核 `ep->mtx` 自旋是已知代价。

### 1.7 一张图

```
register_wait(fd, kRead)                         [io.cpp:817]
  ├─ 条带锁 → 复用/新建 WaitNode（TLS 缓存）      [902-928]
  ├─ readers.push_back(node)                      [1002]
  ├─ pending 命中? → 零超时 poll 确认 → claim     [965-1010]
  └─ update_interest_locked(slot)                 [1040]
        ├─ interest 未变且 registered → return 0  ← 稳态：0 次 syscall
        └─ 变化 → epoll_ctl(MOD/ADD) + m_epoll_mutex [1410]

        内核 ET 边沿
          ↓
   epoll_wait 返回（poll_once / worker）           [1959]
          ↓
   process_events_batch → 条带锁 → process_event_locked [1636/1509]
        ├─ take_ready_locked：队头 begin_operation + claim(kReady)
        ├─ 队列仍非空 → force_rearm = true         [1566]
        └─ update_interest_locked
              ├─ 队列空 → return 0（保留注册/兴趣位）[1366]
              ├─ 队列非空且方向未变 → return 0
              └─ 方向变/force_rearm → 一次 MOD        [1410]
          ↓
   wake_nodes（锁外）→ wake_io / wake_io_batch      [1994]
          ↓
   Fiber 恢复 → 重试真实 syscall（hook step4）
          ↓
   complete(node) → update_interest_locked           [1074/1090]
        └─ 队列空 → return 0                        （不 DEL、不清兴趣位）
```

---

## ② 快路径判定与可行性

### 2.1 判定条件在哪

- 计数：`slot.readers.size() + slot.writers.size()`（`io.cpp:612-624`）。`register_wait`/`process_event_locked` 全程持条带锁，读 size 是安全的 O(1)。
- 单 fd 单等待者的典型就是 WebSocket 每连接一 fiber：`ws_server.cpp:507` 的 `while (...) websocket->read_message(message)`，同一 fd 同时最多一个读等待者。
- 但读/写是**两个队列**：同一连接 ping 时写、pong 时读，两个方向交替出现，计数始终 ≤1，**却会触发方向切换**。

### 2.2 稳态到底有没有 ctl：静态结论

同方向反复 recv（只看读）：
1. 首次 `register_wait` → ADD；
2. `process_event` 唤醒 → 队列空 → interest=0 → `registered==true` ⇒ return 0；
3. `complete` → 同样 return 0；
4. 下一次 `register_wait`：`slot.interest` 仍是 kRead，`interest==kRead` ⇒ return 0。
→ **0 次 ctl/轮**。

读写交替（心跳 ping/pong）：
1. `register_wait(kRead)` → 注册 kRead；
2. `register_wait(kWrite)`（send EAGAIN）⇒ `slot.interest=kRead != kWrite` ⇒ **MOD**；
3. `complete` 保留 kWrite；
4. 下一次 `register_wait(kRead)` ⇒ **MOD**。
→ **每周期 2 次 MOD**。

20k 连接 × 1 次心跳/秒 = 40k 次 MOD/s，与题设「20k 心跳 ≈ 40k ctl/s」**数值吻合**。这是本报告对根因的判断（**静态推导，未实测**）：不是 ONESHOT rearm，而是**读写兴趣位振荡**。

### 2.3 读循环是否保证读到 EAGAIN

`hook.cpp:23`（设计说明）：

```
 * | **park** | socket / FIFO 上的 read/write/connect/accept | fd 切 `O_NONBLOCK`，先试一次，`EAGAIN` 才注册 epoll 并 park |
```

`hook.cpp:1145-1158`（step5/step6）：成功或非 EAGAIN 直接返回；EAGAIN 才 `register_wait` + park；醒来**回到 step4 重试**，因此「唤醒 ≠ 有数据」也在契约内。

WebSocket 侧每次 `read_message` 都走 `socket->recv`（`websocket.cpp:598`），而 `Socket::recv` 最终进 `cooperative_io`。结论：**每次 park 之前一定已经真调过一次 syscall 并拿到 EAGAIN**，满足 ET 的「读干净」前提。这是 ET 能安全落地的核心依据。

### 2.4 写侧 EPOLLOUT 的注册/清除

- 注册：只有 `slot.writers` 非空时 interest 才含 `kWriteMask`（`io.cpp:1358-1361`）；
- 清除：`complete` 后队列空，`update_interest_locked` 走 `interest==0` 分支 **return 0**——`slot.interest` 保留 kWrite，**内核仍监听 EPOLLOUT**。真正清除要等下一次方向切换或 close/`invalidate`。
- 后果：socket 长期可写时，每次 MOD 重新加 EPOLLOUT 都会**立即产生一次写边沿**（ET 注册时条件已成立），`process_event_locked` 会走 `write_ready` 分支；若此刻无写等待者，只置 `pending_write=true`（1581-1586），不唤醒。

### 2.5 跨线程唤醒

`tickle()`（`io.cpp:1842-1868`）三步：本线程正在 poll 直接返回；`m_waiters==0` 直接返回；否则 `m_tickle_pending` 0→1 时写 eventfd（`io_backend.cpp:133-137`）。`register_wait` 只在**新增 deadline** 时 tickle（`io.cpp:1055-1057`）；无超时注册不 tickle，理由是 `epoll_ctl(ADD/MOD)` 本身会唤醒阻塞中的 `epoll_wait`。稳态无 ctl 时新等待者入队，若 fd 已就绪，边沿要么已在 `pending_*`，要么由下一次内核事件唤醒。潜在缺口见 R4。

### 2.6 半关闭/RST/错误

`process_event_locked` 把 `EPOLLERR|EPOLLHUP` 同时当作 read/write ready（1546-1549），因此 `take_ready` 能唤醒等待者，`wait` 返回 kReady，随后 `recv` 返回 0/ECONNRESET，走正常关闭。

特殊分支（`io.cpp:1538-1546`）：

```cpp
            if (interest == 0 && (events & (0x04U | 0x08U)) != 0) {
                invalidate_registration_locked(slot);       // 显式 DEL
                if (slot.readers.empty() && slot.writers.empty()) stripe.slots.erase(fd);
                return;
            }
```

它只在 `slot.interest == 0` 时生效；而 ET 分支在队列空时**不会把 `slot.interest` 置 0**，所以这个防护在「曾经注册过、后来无等待者」的 fd 上**不生效**（见 R3）。

---

## ③ 风险清单

### R1 多等待者共享同一 fd 的丢唤醒（中）

- 触发：两个以上 Fiber 对同一 fd/方向 `wait`（`wait_many` 或业务自己并发）。
- 后果：ET 一次边沿只服务队头，第二个等待者依赖 `force_rearm` 的 MOD 重检；MOD 失败会 `fail_slot_locked` 把整条队列按错误唤醒（`io.cpp:1572-1575`），不会静默挂起。但 `force_rearm` 的 MOD 同时在 `m_epoll_mutex` 上，高并发时是最贵路径。
- 位置：`io.cpp:1566-1570`（置位）、`io.cpp:1410`（MOD）。

### R2 读不干净导致 ET 不再报（低，当前路径不触发）

- 触发：调用方绕过 `hook`（裸 fd）或自己 `epoll` 混用 IOManager；或者 `wait_many` 返回后只处理部分下标。
- 后果：内核读缓冲仍有数据但边沿已消费，fd 再无新边沿 → 永久挂起。
- 位置：`hook.cpp:1145-1158`（hook 保证先真调）；`io.cpp:2519` 注释明确「调用方必须对每个下标做真实 syscall，且仍可能 EAGAIN」。

### R3 无等待者时的 HUP/ERR 忙循环（中，ET 迁移引入）

- 触发：fd 曾注册过 kRead（`slot.interest=kRead`），等待者已 complete 离开，对端 RST/关闭，内核持续报告 EPOLLHUP。
- 后果：`interest != 0` 使 1538 的 `interest==0` 防护不生效；`error_or_hup` 让 read/write 都 ready，`take_ready` 空 → 置 `pending_*`；`update_interest_locked` 走 `interest==0 && registered` **return 0**，注册与旧兴趣位都保留 → `epoll_wait` 每次立即返回 HUP，形成 poller 忙循环，直到 `notify_close` 或新 waiter 消费。连接数多、同时断开时最明显。
- 位置：`io_backend.cpp:87`（EPOLLERR|EPOLLHUP 总是报告）、`io.cpp:1538-1546`、`io.cpp:1366-1370`。
- 备注：`test_io_audit.cpp:98 TestHangupWakesWaiter` 覆盖的是「有等待者」的情况，未覆盖「无等待者 + 旧兴趣位」。（推导，需专项测试确认。）

### R4 新等待者入队但既无 ctl 又无边沿（低，理论缺口）

- 触发：fd 从未注册过（`registered=false`）→ 第一次 `register_wait` 一定 ADD，不成立；已注册且方向相同、`pending` 被零超时 poll 判否、节点入队——此时若数据在 poll 之后、入队之前到达，内核会产生新边沿（ET），`process_event` 能看到队列非空并唤醒。**静态推演不丢**。
- 真正需要小心的是改动后「完全不调用 update_interest」的快路径：一旦去掉 ctl，必须保留 `pending_*` + 零超时 poll 这套补偿，否则这个窗口会变成永久挂起。
- 位置：`io.cpp:965-1010`（补偿机制）。

### R5 监听 socket 与普通连接的区别（低）

- 触发：`accept` 只取一个连接就回到 `wait`。
- 后果：ET 下 backlog 里剩余的连接不再产生新边沿 → 新连接饿死。
- 现状：`ws_server.cpp:351-397 AcceptLoop` 是**独立线程 + 循环 accept**，`accept` 返回空也继续，等价于「accept 到 EAGAIN 才停」，不依赖 ET 边沿。所以当前安全；若未来把 accept 改成 fiber 单次，就会踩坑。
- 位置：`src/bento/net/ws_server.cpp:351-357`。

### R6 方向振荡本身就是当前最大开销（高，见 2.2）

- 触发：同一连接读写交替（WebSocket ping/pong、请求-响应）。
- 后果：每周期 2 次 `epoll_ctl(MOD)` + 2 次 `m_epoll_mutex` 争用；20k 连接 ≈ 40k syscall/s，与 p99 恶化方向一致。
- 位置：`io.cpp:1362-1372`、`io.cpp:1410`、`io_backend.cpp:87`。

---

## ④ 改动设计（只设计，不实现）

### 4.1 目标与前提

- 目标：消除「方向切换」引起的 MOD，把单 fd 单等待者稳态的 ctl 降到 **0 次/轮**（首次 ADD 除外）。
- 前提：ONESHOT 已不存在，**不需要**再写「去 ONESHOT」的改动；`supports_one_shot()` 分支可以保留（第三方后端兼容），但 linux 路径不应再进入。
- 开关：`GO2CPP_IO_FAST_ET`（环境变量，默认 `0` 关闭；A/B 压测后再默认开）。命名沿用仓库已有的 `GO2CPP_*` 风格。

### 4.2 方案 A（推荐）：sticky interest（历史方向并集）

- 位置：`FdSlot` 增字段（`io.cpp:631` 附近）`std::uint32_t sticky_interest{0};`
- 位置：`update_interest_locked`（`io.cpp:1355-1372`）把 `interest` 改为 `interest | slot.sticky_interest` 后再比较/注册；`slot.sticky_interest = 合并值`。
- 清零点：`invalidate_registration_locked`（`io.cpp:1338-1349`）与代际切换（`io.cpp:944-951`）。
- 效果：读写交替不再改注册位；内核始终监听「该 fd 曾用过的方向」。ET 下多监听的写边沿只在缓冲区由满变空时产生，稳态代价≈0。
- 预期：20k 心跳场景 MOD 从 40k/s 降到接近 0（仅首次 ADD + 关闭 DEL）。
- 风险：`pending_write` 可能更频繁置位（无写等待者时的可写边沿），但只写一个 bool，不唤醒。

### 4.3 方案 B（更保守）：单等待者时注册 READ|WRITE

- 判定：`slot.readers.size() + slot.writers.size() <= 1`（`io.cpp:612-624`）。
- 位置：`io.cpp:1355-1364`，快路径强制 `interest |= kReadMask | kWriteMask`。
- 效果同 A，但把「历史」换成「当前只有一个等待者」；多等待者回落到精确兴趣 + `force_rearm` 慢路径。
- 适用：不希望引入新字段、可接受「单等待者时总能收到两个方向边沿」的语义。

### 4.4 慢路径边界（保留 ONESHOT 语义的分支）

- `wait_many`（`io.cpp:2541`）多 fd 时每个 fd 仍只有一个节点，走快路径即可；
- 同一 fd 出现 ≥2 等待者（`readers.size()>1` 或 `writers.size()>1`）时，保留现有 `force_rearm` + 精确兴趣逻辑（`io.cpp:1566`），不改；
- `notify_close`/`shutdown`/`cancel_all` 不动。

### 4.5 顺带清理（低风险，可同 PR）

1. `io.cpp:273` 注释 `epoll 实例（EPOLLONESHOT）` → 改为 EPOLLET；
2. `io.cpp:1871` 注释「EPOLLONESHOT 保证…」→ 改为「条带锁 + 单队列保证…」；
3. `io.cpp:629` `armed` 字段在 ET 下恒 true，可标注废弃或删除（需确认没有测试直接读它）；
4. `io_backend.cpp:66-72` 注释与返回值矛盾，必须一起改，否则下一个调查者还会误判。

### 4.6 改动点清单

| # | 文件:行 | 改动 | 预期影响 |
|---|---|---|---|
| 1 | `src/network/io.cpp:612-638` | FdSlot 增 `sticky_interest` | 结构体 +4B，无锁语义变化 |
| 2 | `src/network/io.cpp:1355-1372` | interest 合并 sticky | 消除方向 MOD |
| 3 | `src/network/io.cpp:1338-1349` | invalidate 清 sticky | 关闭/复用隔离 |
| 4 | `src/network/io.cpp:944-951` | 代际切换清 sticky | 同上 |
| 5 | `src/network/io.cpp:1538-1546` | HUP 防护改为「无等待者即 invalidate」 | 修 R3 忙循环 |
| 6 | `src/network/io_backend.cpp:66-72` | 注释与返回值对齐 | 消除误判 |
| 7 | `src/network/io.cpp:273/1871` | 注释同步 | 文档债 |

共 7 处（其中 5 处为行为改动，2 处为注释）。若只做方案 B，则 #1/#3/#4 可省，剩 5 处。

---

## ⑤ 验证方案

### 5.1 正确性

现有覆盖（`tests/network/test_io_audit.cpp`）：`TestHangupWakesWaiter`(98)、`TestShutdownWakesAllWaiters`(126)、`TestCancelAllDrainsEveryWaiter`(173)、`TestNotifyCloseDrainsEveryWaiter`(213)、`TestGenerationReuseAfterNotifyClose`(252)、`TestWaitAnyClose`(307)、`TestCloseWhileHeadCancelRace`(348)，分别覆盖 HUP 唤醒、shutdown 排空、cancel_all 排空、close 排空、fd 复用代际、wait_any+close、队头取消竞争。另有 `tests/network/test_io.cpp`、`test_hook_*`、`test_network.cpp`、`test_socket_debug.cpp`。

必须新增的回归（按优先级）：

1. **读写交替 MOD 计数**：LD_PRELOAD 或 `-Wl,--wrap=epoll_ctl` 计数；单连接做 `recv→send→recv→send` 10 轮，断言 ctl 次数 ≤ 2（首次 ADD 除外）。这是方案 A/B 的直接验收。
2. **无等待者 HUP 不忙循环**：注册读 → 不等待 → 对端 RST → 在 100ms 内统计 `poll_once` 返回次数，断言不持续 >0（R3）。
3. **多等待者共享 fd**：两个 Fiber 同 fd 读，先后到达两段数据，断言两个都被唤醒且 FIFO 顺序正确（R1）。
4. **wait_many 部分消费**：一个 fd 就绪但调用方不读，另一个 fd 继续就绪，断言不挂起（R2）。
5. **accept backlog**：listen fd + 10 个并发连接，fiber 单次 accept，断言不会只接受 1 个（R5 的回归护栏；当前线程模型应通过）。

### 5.2 性能

- 命令（只读观测，不修改）：
  - `perf stat -e syscalls:sys_enter_epoll_ctl -p <server_pid>`（需 perf 权限）；或
  - `strace -c -f -e trace=epoll_ctl -p <server_pid>`（`-c` 汇总 ADD/MOD/DEL 次数）。
- 场景：`app/ws_bench/run_bench.sh go2cpp 20000 60 <port> 4`（README 用法见 `run_bench.sh:4-6`），服务端 pin CPU 2-9、客户端 pin CPU 0。
- 指标：p50/p99、RSS、`epoll_ctl` 总次数与 ADD/MOD/DEL 比例、`sys` 时间。
- 判定：
  - 若 MOD 占比 >90% 且与心跳频率同步 ⇒ 方案 A/B 成立；
  - 若 DEL/ADD 占比高 ⇒ 是 slot 生命周期问题（close 频繁），应另案调查 `notify_close` 与 fd 复用，而不是 ET 快路径。
- 对比基线：改动前跑一次同参数，记录 `epoll_ctl` 计数与 p99；改动后同机同参数复测，至少 3 轮取中位数。

### 5.3 回归门槛

- `ctest` 全绿（改动前后各跑一次）；
- 上述 5 个新增用例全绿；
- 20k ws_bench：p99 不劣化、RSS 增幅 <2%、`epoll_ctl` 次数下降 ≥80%（方案 A/B 达成时）。

---

## ⑥ 结论

1. **能不能做**：能，但**原定的「去 ONESHOT」目标已经完成**（`214fbde`，`io_backend.cpp:72` 返回 false，`87` 只加 EPOLLET）。当前不存在「每帧 ONESHOT rearm」；`io.cpp:273/1871` 是过时注释。若按原题设直接动手，会改错对象。
2. **真正的待优化点**：读写方向交替导致的 MOD 振荡（`io.cpp:1362-1372`），静态推导与「20k 心跳 ≈40k ctl/s」数值吻合，但**尚未实测确认**。必须先做 5.2 的 `strace -c` 采样，再决定是否实施。
3. **值不值**：若采样确认 MOD 占绝对多数，方案 A（sticky interest）可用 ~70 行改动把每连接每周期 2 次 syscall 降到 0，收益是系统调用与 `m_epoll_mutex` 争用的数量级下降；若 MOD 不是主因，则收益有限，应先查 slot 生命周期与调度器唤醒。
4. **先决条件**：
   - 先纠正文档与注释（`io.cpp:273/1871`、`io_backend.cpp:66-72`），否则后续 agent 会继续被误导；
   - 先补 R3（无等待者 HUP 忙循环）的专项测试，它是 ET 迁移遗留的正确性风险，优先级高于性能；
   - 所有改动放在 `GO2CPP_IO_FAST_ET` 开关后，默认关，A/B 通过后再默认开。
5. **不建议**：在没有 ctl 计数证据前直接改 `update_interest_locked` 或删 `pending_*`/`force_rearm` 补偿——这两套机制是 ET 正确性的支柱，删掉会从「慢」变成「挂起」。

---

### 附：本次调查的只读证明

- `git status --porcelain` 在调查前后一致（见交付消息）；`src/network/` 下无任何修改；
- 未执行任何压测/benchmark；未启动 `app/ws_bench`；
- 报告中所有「推导/未实测」均已显式标注。
