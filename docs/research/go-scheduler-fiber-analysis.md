# Go 1.3 调度器与 Fiber 的 C 源码深度分析：Bento 还能优化多少

> 日期：2026-10-30 · Go 源码：`/UserData/CodexWorkSpace/GoSrc/go13/src/pkg/runtime/`（go1.3，纯 C + 汇编）
> 对照源码：`/usr/local/go/src/runtime/`（go1.27.1，用于「后来优化了什么」）
> 工作树：`/mnt/e/CodexWorkspace/Go2Cpp` · Bento 侧：`src/concurrency/{scheduler,fiber,context_x64}.*`
> 范围：**只读 Go/Bento 源码 + 只写本文档**；未改代码、未 commit。
> 基线（既有实测，本文引用不重测）：Fiber 切换 34.4ns、JumpContext 裸往返 6.7ns、cap0 阻塞收发中 Scheduler+Fiber ≈90%（切换 16.4% + 调度链 74.4%）、worker 调度链 123.8ns / 往返 6 次加锁。

---

## 0. 一句话结论

**Go 1.3 最值得抄的不是 `schedule()`，而是「把状态迁移的所有权收敛到单一线程」这条纪律。** 它的 runq（`proc.c:2912-3027`）在 Go 1.27 里逐行同构、13 年没改，是已被时间验证的最优设计；而 `gopark`/`ready` 能在同 P 内做到 **零锁**，靠的是「G 的状态只由当前执行者或 STW 改写」这个不变量，不是靠更快的锁。Bento 的 123.8ns 调度链里，**6 次互斥量至少贡献 60~90ns**，Fiber 切换 34.4ns 反而已经接近合理区间；把锁账清掉，Bento 的完整 park→wake→resume 往返可以从 ~158ns 压到 45~65ns，**低于 Go 1.3 同场景的估算值**，因为 Bento 的挂起只需一次 `JumpContext`，而 Go 1.3 要 `mcall`（切 g0 栈）+ `schedule` + `gogo`（切回用户栈）两次栈切换。

三条结构性判断：

| # | 判断 | 依据 | 对 Bento 的含义 |
|---|---|---|---|
| J1 | **runq 设计是最优的**，可直接移植 | `proc.c:2912-3027` 与 `proc.go:7529-7670` 结构一致，仅多 `runnext` | 换掉 `std::deque + mutex`，改 256 槽环形队列，热路径 0 锁 |
| J2 | **Go 1.3 的状态机是历史妥协**，不是标杆 | go1.3 全树无 `casgstatus`（`grep` 0 命中），裸写 `gp->status` 靠 STW 兜底；Go 1.5 才补 CAS | Bento 的原子状态 + 令牌方向是对的，错在用 mutex 保护它 |
| J3 | **Bento 的 Fiber 层已经优于 Go 1.3**，不要回退 | 一次 `JumpContext` vs Go 的两次栈切换；6.7ns vs Go 单次 `gogo` 量级 | 优化重点在 Scheduler 的锁，不在切换汇编 |

---

## 1. 口径与四条事实校正

任务描述里有四个与技术事实不符的假设，先校正，否则后面的结论会建立在错误前提上。

### 1.1 runq 不是「无锁 SPSC」，是「单生产者 + CAS 多消费者」

任务写「Go 1.3 是无锁 SPSC（runqhead/runqtail 原子）」。源码不是这样：

- **生产者只有一个**：`runqput` 注释明写 `Executed only by the owner P`（`proc.c:2910`），`p->runqtail` 是**普通写**，不是原子写（`proc.c:2918,2921`）。
- **消费者有多个**：owner 自己 `runqget`，窃取者 `runqgrab`；两者都用 `runtime·cas(&p->runqhead, h, h+1)` 抢头（`proc.c:2972,2996`）。
- 所以准确模型是 **owner-only 单生产者 + CAS 多消费者（MPSC）**。这比「无锁 SPSC」更强：生产端没有原子 RMW，只有一次 release store；消费端才付 CAS。

这个区别决定了移植价值：Bento 的每个 P 也可以做到「只有自己的 worker 写 tail」，于是入队路径**一条原子指令都不需要**（除 release store）。

### 1.2 `casgstatus` 在 Go 1.3 不存在

任务要求分析「状态迁移（`casgstatus`）——Go 的状态机是否无锁」。实测：

```
grep -rn 'casgstatus' /UserData/CodexWorkSpace/GoSrc/go13/src/pkg/runtime/*.c *.h
→ 0 命中
```

go1.3 里所有状态迁移都是裸赋值：`gp->status = Grunnable`（`proc.c:382`）、`gp->status = Gwaiting`（`proc.c:1394`）、`gp->status = Grunning`（`proc.c:1136`）。它能这么写，靠的是 **stop-the-world**：GC 要读所有 G 状态前先停世界（`gcstopm`，`proc.c:1105-1123`），于是「谁在写 status」不需要原子化。

`casgstatus` 是 Go 1.5 引入 runtime2.go/proc.go 后才有的（为并发 GC 的混合写屏障服务）。**结论：Go 1.3 的状态机不是无锁的，是「不加锁 + 全局停顿」——这是它最大的历史妥协之一。**

### 1.3 Go 1.3 是分段栈，不是「栈翻倍 + 拷贝」

任务写「栈增长（`morestack`——Go 的栈翻倍+拷贝，不是 mprotect 动态增长）」。go1.3 的 `newstack` 有两条路（`stack.c:754-850`）：

1. 若 `runtime·copystack` 为真且顶部段所有帧可拷贝：`newsize = oldsize * 2`，`copystack()` 整段搬走（`stack.c:760-774`）。**这条才是「翻倍+拷贝」。**
2. 否则：**分配一个新段**（`framesize` 按需 round2 到 2 的幂，`stack.c:780-787`），把参数拷到新段，`gogo` 到 `lessstack`，旧段留在链上（`stack.c:798-850`）。返回时 `oldstack` 弹段（`stack.c:176-227`）。

`copystack` 默认开启（`proc.c:147,178`：`precisestack=true` → `copystack=precisestack`），但**只对「顶部段全可拷贝」生效**；一旦栈里有 C 帧/defer 等不可拷贝帧，就退回分段。所以 go1.3 是**混合模型**：能拷就拷，不能拷就分段。真正的「永远整栈翻倍拷贝」是 Go 1.4 的连续栈（contiguous stack）。

### 1.4 `gogo` 是 14 条指令，不是 22 条

`asm_amd64.s:140-153` 逐条数：`MOVQ 8(SP),BX` / `MOVQ gobuf_g(BX),DX` / `MOVQ 0(DX),CX` / `get_tls(CX)`（amd64 上展开为 `MOVQ TLS, CX`，1 条）/ `MOVQ DX,g(CX)` / `MOVQ gobuf_sp(BX),SP` / `MOVQ gobuf_ret(BX),AX` / `MOVQ gobuf_ctxt(BX),DX` / 3×`MOVQ $0`（清 sp/ret/ctxt 帮 GC）/ `MOVQ gobuf_pc(BX),BX` / `JMP BX` = **14 条**（含 3 条清零）。`RuntimeGogoBytes = 64`（`arch_amd64.h:15`）是给 traceback 做 PC 区间判断用的上界，不是指令数。

`gosave` 更短：`asm_amd64.s:125-136`，7 条 + `RET`。

---

## 2. Go 调度器 C 实现逐段分析

### 2.1 数据结构与关键常量

P 结构（`runtime.h` 的 `struct P`）：

```c
uint32  runqhead;      // 消费端 CAS
uint32  runqtail;      // 生产端（owner）普通写
G*      runq[256];     // 环形队列，2 的幂 → % 编译成 and
G*      gfree;         // 本 P 的 dead G 缓存
int32   gfreecnt;      // ≥64 时批量转全局
byte    pad[64];       // 防伪共享
```

栈相关常量（`stack.h`）：`StackMin = 8192`（79 行）、`FixedStack = StackMin + StackSystemRounded`（81 行；Linux/amd64 上 `StackSystem=0`，故 = 8KiB）、`StackGuard = 256`（92 行）、`StackBig = 4096`（88 行）。缓存：`StackCacheSize = 32`、`StackCacheBatch = 16`（`runtime.h:153,155`）。

**要点：8KiB 是最小栈，且是 2 的幂，所以「按大小分档」在 go1.3 里其实只有两档——8KiB 走 free-list，其余走 malloc。**

### 2.2 `schedule()`（`proc.c:1311-1357`）

```c
top:
    if(runtime·sched.gcwaiting) { gcstopm(); goto top; }      // 1320-1323
    gp = nil;
    tick = m->p->schedtick;                                   // 1329
    if(tick - (((uint64)tick*0x4325c53fu)>>36)*61 == 0 &&    // 1332
       runtime·sched.runqsize > 0) {
        lock(&runtime·sched); gp = globrunqget(m->p, 1); unlock(&runtime·sched);
        if(gp) resetspinning();
    }
    if(gp == nil) { gp = runqget(m->p); ... }                 // 1339-1343
    if(gp == nil) { gp = findrunnable(); resetspinning(); }   // 1344-1347
    if(gp->lockedm) { startlockedm(gp); goto top; }           // 1349-1354
    execute(gp);                                              // 1356
```

值得注意的三点：

1. **`tick%61` 用两次乘法代替除法**（1330-1332 注释明说）。这是 2014 年的微优化，在今天 `div` 也就 20~30 cycle 的情况下收益已经很小，但它体现了 Go 的一条原则：**调度循环里不出现除法、不出现分支难以预测的代码**。
2. **全局队列最多每 61 次调度碰一次**，且只在 `runqsize > 0` 时才加锁。这是「本地优先 + 周期性公平」的雏形。
3. **热路径（本地队列非空）只有 `runqget` 一次 CAS**，没有任何锁。

### 2.3 `execute()`（`proc.c:1128-1150`）

```c
if(gp->status != Grunnable) throw(...);      // 1132
gp->status = Grunning;                       // 1136
gp->waitsince = 0; gp->preempt = false;
gp->stackguard0 = gp->stackguard;            // 1139 恢复栈保护
m->p->schedtick++;                           // 1140
m->curg = gp; gp->m = m;                     // 1141-1142
hz = runtime·sched.profilehz;                // 1144-1147 CPU profiler 开关
runtime·gogo(&gp->sched);                    // 1149 永不返回
```

`execute` 是「把 G 挂到 M 上然后跳进去」的唯一入口，**12 行有效代码，0 锁 0 原子**。Bento 的对应物是 `worker_loop` 里的 `try_mark_running()` + `resume_from_scheduler()`（`scheduler.cpp:3849`、`scheduler.cpp:925`），前者一次 `m_transition_mutex`（`scheduler.cpp:611`），后者两次 `m_resume_claim` CAS（`fiber.cpp:1366-1372`）。

### 2.4 `findrunnable()`（`proc.c:1155-1283`）——工作窃取的全部逻辑

查找顺序（这个顺序本身就是设计）：

| 顺序 | 动作 | 行号 | 锁 |
|---|---|---|---|
| 1 | `runqget(m->p)` 本地队列 | 1169 | 0（1 CAS） |
| 2 | 全局队列 `globrunqget(m->p, 0)` | 1173-1179 | `sched` 锁 |
| 3 | `netpoll(false)` 非阻塞轮询 | 1181-1186 | 0（内部） |
| 4 | 自旋配额检查：`2*nmspinning >= gomaxprocs-npidle` 就放弃 | 1190 | 原子读 |
| 5 | 随机窃取 `2*gomaxprocs` 次 | 1197-1207 | 0（CAS） |
| 6 | 放弃 P 前再扫一遍所有 P 的 `runqhead!=runqtail` | 1228-1240 | 0（裸读） |
| 7 | `netpoll(true)` 阻塞等 IO | 1242-1248 | 0 |

两个细节值得 Bento 直接借鉴：

- **自旋配额**（1190）：`2*nmspinning >= gomaxprocs - npidle` 时不再自旋。防止 `GOMAXPROCS` 很大但并行度很低时所有 M 空转烧 CPU。Bento 目前没有等价机制（`worker_loop` 靠 `condition.wait` 阻塞）。
- **释放 P 前的二次扫描**（1228-1240）：窃取失败、准备 `pidleput` 之前，再裸读一遍所有 P 的队列头尾。这是为了避免「刚判定没活、其实别人刚入队」的丢唤醒窗口。Bento 的 `recover_unclaimed`（`scheduler.cpp:3801-3823`）解决的是同类问题，但用的是 mutex + 补发布，重得多。

### 2.5 park 家族（`proc.c:1361-1411`）与 `ready`（`proc.c:373-389`）

```c
void runtime·park(bool(*unlockf)(G*, void*), void *lock, int8 *reason) {
    if(g->status != Grunning) throw("bad g status");   // 1364
    m->waitlock = lock; m->waitunlockf = unlockf;      // 1366-1367
    g->waitreason = reason;
    runtime·mcall(park0);                              // 1369 切到 g0 栈，不返回
}

static void park0(G *gp) {
    gp->status = Gwaiting; gp->m = nil; m->curg = nil;  // 1394-1396
    if(m->waitunlockf) {
        ok = m->waitunlockf(gp, m->waitlock);          // 1398 解锁（chan 的锁在这里放）
        if(!ok) { gp->status = Grunnable; execute(gp); }// 1401-1404 复核失败直接跑回去
    }
    if(m->lockedg) { stoplockedm(); execute(gp); }
    schedule();                                        // 1410
}
```

**`park` 分两段是核心设计**：第一段在用户栈上只做「存参数」，第二段在 g0 栈上做「改状态 + 解锁 + 进调度」。这样 G 的状态永远只在 g0（或持有 sched 锁的线程）上改，天然单写者，不需要任何原子。

`ready` 更直接：

```c
void runtime·ready(G *gp) {
    m->locks++;                                        // 377 禁抢占
    if(gp->status != Gwaiting) throw(...);             // 378-381
    gp->status = Grunnable;                            // 382
    runqput(m->p, gp);                                 // 383 入「当前 M」的 P，不是 gp 原来的 P
    if(npidle != 0 && nmspinning == 0) wakep();        // 384-385
    m->locks--;
}
```

**关键洞察：`ready` 把 G 放进「唤醒者自己的 P」，而不是被唤醒 G 原来的 P。** 这避免了「精确投递给目标 P」所需的跨核队列锁；代价是被唤醒的 G 可能被别的 P 偷走（缓存亲和性损失）。这是一个明确的 trade-off，而 Bento 选了另一条路（`requeue_to_processor` 按 `last_processor_id` 精确投递，`scheduler.cpp:3037-3043`），于是每次唤醒都要拿目标 P 的 `processor.mutex`。

### 2.6 runq：13 年没变的那 100 行（`proc.c:2912-3027`）

```c
static void runqput(P *p, G *gp) {                    // 2912
retry:
    h = atomicload(&p->runqhead);                     // 2917 load-acquire
    t = p->runqtail;                                  // 2918 普通读（owner-only）
    if(t - h < nelem(p->runq)) {
        p->runq[t%nelem(p->runq)] = gp;               // 2920
        atomicstore(&p->runqtail, t+1);               // 2921 store-release
        return;
    }
    if(runqputslow(p, gp, h, t)) return;              // 2924 满了才批量转全局
    goto retry;
}

static G* runqget(P *p) {                             // 2961
    for(;;) {
        h = atomicload(&p->runqhead);                 // 2967
        t = p->runqtail;
        if(t == h) return nil;
        gp = p->runq[h%nelem(p->runq)];
        if(cas(&p->runqhead, h, h+1)) return gp;      // 2972
    }
}

static uint32 runqgrab(P *p, G **batch) {             // 2981 窃取一半
    ...
    n = t - h; n = n - n/2;                           // 2988-2989 取一半（向上取整）
    for(i=0; i<n; i++) batch[i] = p->runq[(h+i)%nelem(p->runq)];
    if(cas(&p->runqhead, h, h+n)) break;              // 2996 一次 CAS 抢走 n 个
}

static G* runqsteal(P *p, P *p2) {                    // 3006
    n = runqgrab(p2, batch);
    ...
    gp = batch[n-1];                                  // 3016 自己留一个先跑
    ... 把剩下 n-1 个写进自己的 runq，一次 release store 发布（3025）
}
```

对照 Go 1.27（`proc.go:7529-7670`）：`runqput` 多了一个 `runnext` 快槽，`runqget`/`runqsteal` 与 1.3 **逐行同构**。**这是本文最强的一条证据：这套队列在 13 年、几代 CPU、两次 GC 重写之后没有被替换，说明它已接近该问题的下界。**

`runqputslow`（`proc.c:2933-2956`）是唯一的慢路径：队列满时取一半 + 新 G，CAS 抢头，然后**拿一次全局 `sched` 锁**批量挂到全局队列。也就是说**只有队列满（≥256）时才有锁**，正常负载下永远不触发。

### 2.7 全局队列与窃取的配合（`proc.c:2827-2881`）

`globrunqget(p, max)`（2855-2881）：一次取 `runqsize/gomaxprocs + 1` 个（不超过 `max`、不超过本地队列一半），第一个直接返回，其余 `runqput` 进本地队列。**这保证了全局队列不会一次被一个 P 抽干，同时避免每次只取一个导致反复抢全局锁。**

`globrunqput`/`globrunqputbatch`（2827-2850）是纯链表尾插，调用方必须持 `sched` 锁。

### 2.8 状态迁移：裸赋值 + STW（反面教材）

go1.3 的状态集合（`runtime.h`）：`Gidle/Grunnable/Grunning/Gsyscall/Gwaiting/Gdead`。迁移点全部是裸写：

| 迁移 | 位置 | 保护机制 |
|---|---|---|
| Grunnable → Grunning | `proc.c:1136`（execute） | 执行者独占 |
| Grunning → Gwaiting | `proc.c:1394`（park0） | g0 上执行 |
| Gwaiting → Grunnable | `proc.c:382`（ready） | 唤醒者持有「该 G 只在等待」的知识 |
| Grunning → Grunnable | `proc.c:1426`（gosched0） | g0 上执行 |
| → Gdead | `proc.c:1458`（goexit0） | g0 上执行 |
| Gsyscall 相关 | `proc.c:1590-1700` | 系统调用返回时复核 |

它能成立的前提是：**除 ready 外，所有迁移都在该 G 自己的 g0 上发生；而 ready 的调用者必须先证明 G 处于 Gwaiting（如 channel 锁内）**。这是「用协议换原子指令」的典型。代价是 GC 必须 STW 才能安全读状态——这就是 Go 1.3 停顿大的根因之一。

### 2.9 sysmon / retake / preemptone（`proc.c:2535-2709`）

- `sysmon`（2535-2596）：独立 M，20µs 起步、空闲翻倍、上限 10ms 的退避循环；每 10ms 强制 `netpoll(false)`；调 `retake`。
- `retake`（2610-2690）：抢回 `Psyscall` 超过 20µs 的 P；对运行超过 10ms 的 G 调 `preemptone`。
- `preemptone`（2695-2709）：`gp->preempt = true; gp->stackguard0 = StackPreempt;`——**把抢占请求写进栈保护字**，等 G 下次函数序言检查栈时触发 `morestack`，在 `newstack` 里发现 `stackguard0 == StackPreempt` 就转去 `gosched0`（`stack.c:733-752`）。

这是**协作式抢占**：紧循环（无函数调用）里永远不触发。Go 1.14 用信号实现异步抢占，正是补这个洞。

---

## 3. Go 的 Fiber：栈与上下文切换

### 3.1 栈分配：`stackalloc` / `stackfree` / stackcache（`stack.c:38-171`）

三层结构：

```
per-M cache:  m->stackcache[32]（环形），stackcachecnt
     ↕ 批量 16 个
全局 stackcache 链表（stackcachemu 保护）
     ↕ 批量 16 个
SysAlloc（一次 16*8KiB = 128KiB）
```

- `stackalloc`（88-139）：**必须在 g0 上调用**（98-99 行直接 throw）；`n == FixedStack` 时从 per-M 环形缓存取（123-131），否则 `mallocgc`（133）。取到后写 `Stktop`（135-137）。
- `stackfree`（141-171）：`top->malloced` 为真就 `free`，否则塞回 per-M 缓存；缓存满 32 个就 `stackcacherelease` 批量还 16 个到全局（164-165）。
- **热路径零系统调用**：8KiB 栈的分配/释放就是「环形数组取一个指针 / 放一个指针」。

这就是任务里说的「stackpool 按大小分档，可复用」的真相：**只有一档（8KiB）有池，大栈直接走通用分配器**。Bento 的 `FiberStack` 已经是「thread_local 池 + 全局池」（`fiber.cpp:207-228`），思路一致，但池里存的是**整段 mmap 映射**（含 guard page、可能已 mprotect 增长过），复用时要重置 committed 区间（`fiber.cpp:672` 附近）。

### 3.2 栈增长：`morestack` → `newstack` → `copystack`（`asm_amd64.s:198-230` + `stack.c:660-853`）

```asm
morestack:                                  ; 198
    CMPQ g(CX), m_g0(BX); JNE 2(PC); INT $3 ; 200-203  g0 上不能增长
    MOVQ 8(SP), AX; MOVQ AX, (m_morebuf+gobuf_pc)(BX)   ; 207-208 记 f 的调用者
    LEAQ 16(SP), AX; MOVQ AX, (m_morebuf+gobuf_sp)(BX)  ; 209-210
    MOVQ 0(SP), AX; MOVQ AX, (g_sched+gobuf_pc)(SI)     ; 217-218 记 f 自己的 PC
    LEAQ 8(SP), AX; MOVQ AX, (g_sched+gobuf_sp)(SI)     ; 220-221
    MOVQ m_g0(BX), BP; MOVQ BP, g(CX)                   ; 225-226 切 g0
    MOVQ (g_sched+gobuf_sp)(BP), SP                     ; 227
    CALL runtime·newstack(SB)                           ; 228
```

`newstack` 的判断顺序（`stack.c:666-853`）：

1. 若 `stackguard0 == StackPreempt` → 当作 `Gosched` 处理（733-752）。
2. 若 `copystack` 且顶部段全可拷贝 → `newsize = oldsize*2`，`copystack`（756-774）。
3. 否则分配新段（780-792）、拷贝参数（822-830）、`gogo` 到 `lessstack`（839-850）。
4. 返回时 `lessstack`（`asm_amd64.s:392-404`）→ `oldstack`（`stack.c:176-227`）弹段、拷返回值、`stackfree`。

`copystack`（596-646）的关键成本：`gentraceback(..., adjustframe, ...)` 逐帧调指针（626）、`adjustctxt`/`adjustdefers`（629-630）、`memmove`（633）。**这是 O(栈深度) 的工作，不是 O(1)。**

### 3.3 上下文切换：`gosave` / `gogo` / `mcall`

| 函数 | 行号 | 指令数 | 动作 |
|---|---|---|---|
| `gosave` | `asm_amd64.s:125-136` | 7 + RET | 存 sp/pc/ret/ctxt/g 到 Gobuf |
| `gogo` | `asm_amd64.s:140-153` | 14 | 恢复 g/TLS/sp/ret/ctxt/pc，清 Gobuf |
| `mcall` | `asm_amd64.s:159-185` | ~12 | 存当前 G 状态到 `g->sched`，切 g0 栈，`CALL fn(g)` |
| `lessstack` | `asm_amd64.s:392-404` | 6 | 存返回值到 `m->cret`，切 g0，调 `oldstack` |

**一次 Go 的 park→resume 往返要几次跳转？**

```
用户 G ──mcall──► g0 栈 ──schedule──►（不跳，直接调 execute）──gogo──► 用户 G
```

即 **2 次栈切换 + 2 次 Gobuf 读写**。Bento 是 **1 次 `JumpContext`**（`context_x64.S:52-75`：6 push + 6 pop + 2 mov + jmp，17 条）。这是 Bento 的结构性优势，**不要为了「像 Go」而加一层调度栈**。

---

## 4. 「Go 是否最优」逐项判定

| 维度 | Go 1.3 做法 | 判定 | 理由 | Bento 该怎么办 |
|---|---|---|---|---|
| **local runq** | owner-only 写 tail + CAS 抢 head（`proc.c:2912-3027`） | ✅ **最优** | 1.27 逐行同构，13 年未改；生产端 0 原子 RMW | 照抄：256 槽环形 + release store + CAS |
| **ready 投递目标** | 投到唤醒者自己的 P（`proc.c:383`） | ✅ **最优（有代价）** | 0 锁；代价是亲和性靠窃取恢复 | 评估：当前精确投递换 1 次锁，不划算 |
| **park 两段式** | 用户栈存参 → mcall → g0 上提交（`proc.c:1361-1411`） | ✅ **最优结构** | 状态单写者，无需原子 | 不必抄「两段」，但要抄「单写者」不变量 |
| **全局队列** | 单 `sched` 锁 + 每 61 次调度取一次 | ⚠️ **妥协但合理** | 热路径不碰；锁只在队列空/满时出现 | 保留，但只在本地空/满时进入 |
| **状态机** | 裸写 + STW（无 `casgstatus`） | ❌ **历史妥协** | 1.5 补 CAS 就是承认这点 | 不要抄；Bento 的原子状态方向正确，去掉 mutex 即可 |
| **栈模型** | 分段 + 条件拷贝（`stack.c:754-850`） | ❌ **历史妥协** | 段分裂/弹段有 O(depth) 成本；1.4 连续栈取代 | 保留 Bento 的 growable（深栈更优），补小栈固定池 |
| **切换汇编** | `gogo` 14 条（`asm_amd64.s:140`） | ✅ **接近下界** | 但一次 park 要 2 次切换 | Bento 1 次 `JumpContext` 已更优 |
| **抢占** | 协作式（`stack.c:733`）+ sysmon 10ms | ❌ **妥协** | 紧循环不可抢占；1.14 异步抢占补课 | P2 再做，先保证不丢唤醒 |
| **netpoll** | findrunnable 内非阻塞 + sysmon 10ms 兜底 | ✅ **正确设计** | IO 完成直接进调度器，不经额外线程 | Bento 已有 io_poller，可对齐这个调用位置 |
| **工作窃取** | 随机偷一半（`proc.c:1197-1207,2981-3027`） | ✅ **最优** | 随机化避免锁步；偷一半摊薄 CAS | P2 移植，收益在多核吞吐不在单次延迟 |
| **自旋配额** | `2*nmspinning >= gomaxprocs-npidle`（`proc.c:1190`） | ✅ **正确** | 防 GOMAXPROCS 大而并行度低时烧 CPU | P1 移植 |

**总结论：Go 1.3 的最优部分集中在「队列 + 所有权协议」，妥协部分集中在「状态机 + 栈 + 抢占」。Bento 已经在 Fiber 层超过它，差距全在把「所有权协议」换成了「互斥量协议」。**

---

## 5. 核心：一次 park→wake→resume 的加锁账

### 5.1 Bento 的 6 次锁分解（cap0 阻塞收发路径）

调用链：`channel` 阻塞 → `ParkingCondition::wait`（`parking_condition.cpp:411`）→ `Scheduler::park_wait`（`scheduler.cpp:2849`）。

| # | 位置 | 锁 | 保护对象 | 是否热路径必需 |
|---|---|---|---|---|
| 1 | `Task::park_for_scheduler`（`scheduler.cpp:593`） | `m_transition_mutex` | `m_state` + `m_wake_pending` | **可用 CAS + 令牌位替代** |
| 2 | `Task::wake_for_wait`（`scheduler.cpp:524`） | `m_transition_mutex` | 同上 | **同上** |
| 3 | `Task::try_mark_queued`（`scheduler.cpp:366`，经 `requeue_to_processor:2712`） | `m_transition_mutex` | `m_queued` 与队列节点发布 | **可合进 #2** |
| 4 | `requeue_to_processor`（`scheduler.cpp:2720`） | `processor.mutex` | `std::deque<shared_ptr<Task>>` | **可用无锁环形队列替代** |
| 5 | `worker_loop` 快路径（`scheduler.cpp:3838`） | `own_processor.mutex` | 同上（消费端） | **同上** |
| 6 | `Task::try_mark_running`（`scheduler.cpp:611`） | `m_transition_mutex` | `m_state` + `m_queued` + `m_execution_claim` | **可合进 #5（dequeue 即 claim）** |

再加 `resume_from_scheduler` 的两次 `m_resume_claim` CAS（`fiber.cpp:1366-1372,1378-1382`）——不是锁，但每次是 `lock cmpxchg`（跨核缓存行 RFO）。

> 口径校正：既有观测说「transition_mutex 每往返 6 次加锁」。按上面逐点核对，**准确分解是 4 次 `m_transition_mutex` + 2 次 `processor.mutex` + 2 次 `m_resume_claim` CAS**。这个区别很重要：只把 `transition_mutex` 换成原子操作，还剩 2 次队列锁（≈20~40ns），必须同时做 P0-2 才能吃掉大头。

**Go 同场景的对应账：0 次锁、1 次 CAS（`runqget`）、1 次 release store（`runqput`）。**

### 5.2 123.8ns 的构成估算

| 项 | 估算 | 依据 |
|---|---|---|
| 6 次 mutex（未争用 lock+unlock） | 60~90ns | x86 上 `std::mutex` 快路径 = 1 次 `lock cmpxchg` + 1 次 store；同核 ~10ns，跨核 ~20~40ns |
| 2 次 `m_resume_claim` CAS | 8~16ns | 跨核缓存行 RFO |
| `shared_ptr` 引用计数（push_back 拷贝 + 析构） | 10~20ns | 每次 1 次 `lock xadd`/`lock dec` |
| 队列/deque 操作 + 多次状态检查 + 事件宏 | 15~25ns | `worker_loop` 每轮有 4~6 次 atomic load + 分支 |
| **合计** | **93~151ns** | 与实测 123.8ns 吻合 |

结论：**123.8ns 里 65%~75% 是同步原语，不是「调度逻辑」本身。** 这与「调度链占 74.4%」的观测完全一致。

### 5.3 为什么 Go 能 0 锁，Bento 现在不能

不是 Bento 的实现者不懂，而是 Bento 多背了三个 Go 1.3 没有的需求：

1. **可取消**（`cancellation_requested`）——Go 1.3 的 G 没有取消语义，`ready` 只要看 `Gwaiting` 就够。
2. **shutdown/draining**（`stopping`/`draining`）——Go 1.3 靠 `throw` 直接崩，不需要优雅停机。
3. **跨线程精确唤醒**（IO poller、timer、外部 mutex unlock）——Go 1.3 把这类唤醒统统投到「唤醒者自己的 P」，靠窃取兜底。

所以正确做法不是「删掉 mutex」，而是**把 mutex 从热路径赶到冷路径**：正常 park/wake 用 CAS + 令牌位；只有 detect 到 cancel/shutdown 竞争时才进慢路径（加锁复检）。这与 Go 1.27 的 `gopark` 里 `checkTimeouts()` + `acquirem()` 的思路一致（`proc.go:457-475`）。

---

## 6. 可优化点清单

> 收益为「每次 park→wake→resume 往返」的估算，基于 §5.2 的模型；风险等级：低 = 语义不变可灰度，中 = 需证明不变量，高 = 可能丢唤醒/悬垂。

| # | Go 位置 | Bento 现状 | 改法 | 预期收益 | 风险 |
|---|---|---|---|---|---|
| **P0-1** | `proc.c:1394,382`（裸写 + 单写者协议） | `park_for_scheduler`/`wake_for_wait` 各一次 `m_transition_mutex`（`scheduler.cpp:593,524`） | 状态改 `std::atomic<GState>`；park 用 `compare_exchange_strong(kRunning→kWaiting)`；wake 用 `exchange(wake_pending=true)` + 若状态已 kWaiting 再 CAS 到 kRunnable；仅 CAS 失败时进 mutex 慢路径复检 | **-25~40ns** | 中：需证明「wake 先于 park」时令牌不丢；用 `m_wake_pending` 单比特 + 状态双重检查 |
| **P0-2** | `proc.c:2912-3027`（runq） | `std::deque<shared_ptr<Task>>` + `processor.mutex`（`scheduler.cpp:2720,3838`） | 每 P 一条 256 槽环形队列：`runqhead` 原子 CAS、`runqtail` 仅 owner 写 + release store；满时批量转全局（照抄 `runqputslow:2933`） | **-15~25ns**（含入队+出队两次锁） | 中：内存序写错会丢 G；需 TSAN + 随机压测 |
| **P0-3** | `proc.c:1136`（execute 无锁） | `try_mark_running` 持 `m_transition_mutex`（`scheduler.cpp:611`） | dequeue 成功即视为 claim（队列节点归 worker 独占），状态用 `store(release)`；`m_execution_claim` 合并进队列节点所有权 | **-8~15ns** | 中：与 recover_unclaimed 的补发布逻辑耦合，需一起改 |
| **P0-4** | `asm_amd64.s:140`（gogo 无 claim） | `resume_from_scheduler` 两次 CAS（`fiber.cpp:1366,1378`） | 由「队列节点所有权」保证同一 Fiber 只被一个 worker resume，删 `m_resume_claim`；保留一个 debug 断言（release 下编译掉） | **-6~10ns/切换**（34.4 → 25~28ns） | 低：所有权已由 P0-2/P0-3 提供；需保留 `Fiber::resume()` 的公开路径检查 |
| **P1-1** | `proc.c:2919`（G* 裸指针） | 队列元素 `shared_ptr<Task>`，每次 push/pop 一次原子增/减 | 改侵入式 `Task*` 链表节点 + registry 保活（任务在 registry 中时不会被析构）；`shared_ptr` 只留在 API 边界 | **-10~15ns** | 高：生命周期错误 = UAF；需 ASAN + 长稳压测 |
| **P1-2** | `stack.c:88-171`（8KiB 池） | `FiberStack` 每次 `go()` 取整段 mmap（池化后 ~14.5µs，`fiber.cpp:186-188`） | 增加 8/16/32/64KiB 固定档 free-list（纯指针数组，0 系统调用）；仅 >64KiB 走 mmap+growable | go() **-80%~90%**（14.5µs → 1~2µs） | 低：纯分配策略；需保证 guard page 语义不变 |
| **P1-3** | `proc.go:7529`（runnext，Go 1.27） | 无 | 每 P 一个 `runnext` 槽：`ready` 若目标是「刚被 park 的 G」直接放 runnext，下次 `runqget` 优先取 | **-5~10ns**（ping-pong 场景） | 低：可独立开关 |
| **P1-4** | `proc.c:1190`（自旋配额） | 无（worker 直接 `condition.wait`） | 加 `2*spinning >= nproc - nidle` 判定，超额就阻塞而不是忙等 | 省 CPU（非延迟） | 低 |
| **P2-1** | `proc.c:1197-1207,3006`（窃取） | 全局队列 + 随机投递（`scheduler.cpp:2637-2648`） | 实现 `runqsteal`：随机选 P，偷一半，自己留一个先跑 | 多核吞吐 +10~20%（非单次延迟） | 中 |
| **P2-2** | `proc.c:2535-2709`（sysmon/抢占） | 无等价物 | 独立 monitor 线程 + `preempt` 标志；Bento 的 `SuspendReason` 已可承载 | 防单 G 独占 P | 中：需 safe point |
| **P2-3** | `proc.c:1181`（netpoll 在 findrunnable 内） | `io_poller` 独立线程唤醒（`scheduler.cpp:1191-1196`） | 把非阻塞 poll 挪进 worker 的查找循环，减少一次跨线程唤醒 | **-5~15ns**（IO 密集场景） | 低 |

**P0 合计预期：123.8ns → 55~75ns（-40%~-55%）；切换 34.4ns → 25~28ns。**
**P0+P1 合计：往返 ~158ns → 45~65ns，低于 Go 1.3 同场景估算。**

---

## 7. 分阶段路线

### P0（1~2 周，目标：去掉热路径全部 mutex）

1. **先立不变量，再改代码**：写一份 `Task 状态迁移表`，明确每个迁移的「唯一合法写者」。这是 P0-1/P0-3 的前提，没有它不要动锁。
2. **P0-2 先做**（收益最大、耦合最小）：每 P 一条环形队列，替换 `processor.queue`；保留 `processor.mutex` 仅用于「队列满/空」的慢路径。
3. **P0-1 再做**：`m_transition_mutex` 只保留在 cancel/shutdown/enqueue 冷路径；park/wake 热路径改 CAS。
4. **P0-3/P0-4 收尾**：dequeue 即 claim，删 `try_mark_running` 的锁与 `resume_claim` 的 CAS。
5. **验收**：cap0 阻塞收发 μs 级压测 + 1e9 次 park/wake 随机压测（对齐既有「11.14 亿条未复现」的口径）+ TSAN/ASAN 全绿 + 60 分钟长稳。

### P1（2~4 周，目标：去掉堆分配与引用计数）

6. **P1-1**：侵入式队列节点 + registry 保活；这一步风险最高，必须单独 PR、单独长稳。
7. **P1-2**：栈分档池（8/16/32/64KiB），`go()` 路径不再 mmap。
8. **P1-3/P1-4**：runnext + 自旋配额。

### P2（按需，目标：多核扩展性）

9. **P2-1** 工作窃取；**P2-2** sysmon/抢占；**P2-3** netpoll 前移。
10. 每步都用同一套基准（cap0/cap64/cap1024 + 1P/2P/4P）对比，防止「单核变快、多核退化」。

---

## 8. 预期收益汇总与验证方法

| 阶段 | 调度链 | 切换 | cap0 往返（估） | 相对 Go 1.3 |
|---|---|---|---|---|
| 现状 | 123.8ns | 34.4ns | ~158ns | 慢 ~3~4× |
| P0 后 | 55~75ns | 25~28ns | 80~100ns | 接近 |
| P0+P1 后 | 35~50ns | 20~25ns | 55~70ns | **持平或略优** |
| P0+P1+P2 后 | 25~40ns | 18~22ns | 45~60ns | 优于 1.3，接近 1.27 |

**验证方法（必须，否则收益不可信）：**

1. **微基准**：`JumpContext` 裸往返（应保持 6.7ns 不退化）、`resume` 单次、`park→wake→resume` 往返，各 1e7 次取 P50/P99。
2. **真实场景 ≥5 个**：cap0 生产者-消费者、cap64 流水线、mutex 竞争、IO 密集（netpoll 路径）、深递归（栈增长路径）。
3. **正确性**：TSAN + ASAN + 1e9 次随机 park/wake/cancel 交错 + 60 分钟长稳。
4. **防退化门槛**：任一场景 P99 退化 >5% 即回滚该步。

---

## 9. 附：Go 1.3 → 1.27 的调度器演进与移植价值

| 版本 | 变更 | 解决什么 | 对 Bento 的价值 |
|---|---|---|---|
| 1.4 | 连续栈（整栈翻倍拷贝） | 分段栈的段分裂/弹段开销 | 低（Bento 用 growable，方向不同） |
| 1.5 | 并发 GC + `casgstatus` | STW 停顿 | **高**：证明「状态必须原子化」是必经之路 |
| 1.11 | `runnext` 快槽（`proc.go:7529`） | channel ping-pong 场景的排队延迟 | **中高**：P1-3 直接抄 |
| 1.14 | 基于信号的异步抢占 | 紧循环不可抢占 | 中：Bento 的 safe point 可复用取消机制 |
| 1.11~1.14 | 全局队列批量转移（`runqputbatch`/`runqdrain`）、`sched.runq` 锁细化 | 批量操作摊薄全局锁 | 中：P2 时参考 |
| 1.21+ | P 的 `runq` 仍 256 槽 | —— | **这是最强的「不用改」证据** |
| 1.25 | 容器感知 GOMAXPROCS | cgroup CPU 限额 | 低（Bento 已有 `pin_workers_to_cpu`） |

**移植优先级：`runnext`（P1-3）> `casgstatus` 思路（P0-1）> 工作窃取（P2-1）> 异步抢占（P2-2）> 连续栈（不移植）。**

---

## 10. 三条给决策者的结论

1. **Go 1.3 不是「最优」，但它的队列是最优的。** 把 `runq` 那 100 行照抄过来，比在 Bento 现有 `deque+mutex` 上调优有效一个数量级。
2. **Bento 的 Fiber 层已经赢了，别再「向 Go 看齐」加调度栈。** 34.4ns 的切换里，6.7ns 是裸跳，剩余 27.7ns 主要是 `resume_claim` 的两次 CAS 与状态检查——P0-4 能把它压到 25~28ns，而 Go 的 park 要两次栈切换。
3. **最大的风险不是性能，是生命周期。** P1-1（去 `shared_ptr`）的收益只有 10~15ns，但 UAF 风险最高；建议放在 P0 全部稳定、且有 60 分钟长稳基线之后再做。
