# Go runtime 底层 C/汇编源码挖掘报告

> **源码位置（持久保留）**：`E:\CodexWorkspace\GoSrc\{go10,go11,go13,go14}`
> （go1.0.3 / 1.1 / 1.3 / 1.4 完整源码，含 `src/pkg/runtime/chan.c`、`chan.goc`、`chan.h`）
> 原 /tmp 副本已迁移到此目录。

> 目的：给 Go2cpp 的 `Channel` / `FastChannel` / Fiber 切换 / Scheduler 找**可以照抄的原件**，不自己造轮子。
> 所有代码摘自本机克隆的真实源码，标注 `文件:行号`，可直接对照。
>
> 挖掘时间：2026-10-30。本机 `/usr/local/go` 只有 Go 1.27.1；历史源码从 gitee 镜像
> `https://gitee.com/mirrors/go.git` 浅克隆到 `E:\CodexWorkspace\GoSrc\go10`、`E:\CodexWorkspace\GoSrc\go11`、`E:\CodexWorkspace\GoSrc\go13`、`E:\CodexWorkspace\GoSrc\go14`（克隆保留，可复查）。

---

## 0. 结论先行（TL;DR）

1. **"Go 的 C 版 channel" 真实存在，且在 1.4 之前**：
   - Go 1.0–1.1：`src/pkg/runtime/chan.c`，**纯 C**，`Hchan` 结构体直接内联在 `chan.c` 顶部；
   - Go 1.2–1.3：`src/pkg/runtime/chan.goc` + `chan.h`。`.goc` 是"C 的函数体 + Go 的声明"方言，
     **`Hchan`/`SudoG`/`WaitQ`/`Select` 的 C 结构体在 `chan.h` 里逐字段可见**；
   - Go 1.4：`chan.go` 用 Go 重写，但**仍然直接操作 C 的 `Hchan`**（`chan.h` 没变），
     所以 1.4 的 `chan.go` 是"理解 C 结构 → Go 代码"的最佳对照物；
   - Go 1.5+ 才把 `Hchan` 改成 Go 的 `hchan`（定义进 `chan.go`，`buf` 变 `unsafe.Pointer`）。
2. **任务书里的 `goc.c` 不存在**。实测 go1.0.3 / go1.1 / go1.3 / go1.4 的 `src(/pkg)/runtime/` 下都没有 `goc.c`；
   goroutine 调度一直在 **`proc.c`**（C，1.0→1.4 都是）。用户记忆中的 `goc.c` 应理解为
   **`*.goc` 后缀文件**（`chan.goc`、`sema.goc`、`malloc.goc` —— 那套"Go 风格 C"方言）。
3. **最值得抄的三份**：
   - `chan.h`（1.3/1.4）：`Hchan` 字段布局 + `chanbuf` 宏 —— FastChannel 字段表可直接照抄；
   - `chan.goc` 的 `chansend`/`chanrecv`/`closechan`/`dequeue`/`enqueue`（1.3，行 74–201、204–343、1035–1146）——
     逻辑与今天的 `chan.go` 一模一样，但只有 100 行 C，没有 GC 写屏障/race/泛型噪音；
   - `asm_amd64.s` 的 `runtime·gosave`/`runtime·gogo`/`runtime·mcall`（1.3，行 123–185）——
     最干净的协程切换汇编，比 1.27 少了 R14(g) 与 frame pointer 的干扰。
4. **FastChannel 现状方向正确**（`PushLocked` 先 recvq 直交再 buffer；`PopLocked` 先 buffer、`RefillLocked` 补位、
   再 sendq 会合，与 Go 的 `chansend`/`chanrecv` 分支顺序一致）。差距在三处细节：**锁外唤醒**、
   **`closed` 无锁快照**、**关闭时接收者写零值**。详见第 8 节。

---

## 1. 源码地图与版本考证

### 1.1 本地已拉取的四个版本

| 版本 | 克隆路径 | chan 实现文件 | `Hchan` 定义位置 | 语言 |
|---|---|---|---|---|
| go1.0.3 | `E:\CodexWorkspace\GoSrc\go10` | `src/pkg/runtime/chan.c` | `chan.c:32` | 纯 C |
| go1.1 | `E:\CodexWorkspace\GoSrc\go11` | `src/pkg/runtime/chan.c` | `chan.c:38` | 纯 C |
| go1.3 | `E:\CodexWorkspace\GoSrc\go13` | `src/pkg/runtime/chan.goc` | `chan.h:29` | `.goc`（C 方言） |
| go1.4 | `E:\CodexWorkspace\GoSrc\go14` | `src/runtime/chan.go`（Go 重写） | `chan.h:17`（C 结构） | Go + C 结构 |
| go1.27.1 | `/usr/local/go` | `src/runtime/chan.go` | `chan.go:34`（Go 结构） | Go |

### 1.2 版本演进的关键事实（逐条可验证）

- **go1.1 的 `chan.c` 是纯 C**：`void runtime·chansend(ChanType *t, Hchan *c, byte *ep, bool *pres, void *pc)`
  （`E:\CodexWorkspace\GoSrc\go11/src/pkg/runtime/chan.c:160`）。`pres != nil` 表示非阻塞，这是 1.3 改成 `bool block` 的前身。
- **go1.2 起引入 `.goc`**：`chan.goc` 里 `func chansend1(t *ChanType, c *Hchan, elem *byte) { ... }`
  （`E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/chan.goc:346`）——函数声明是 Go 语法，函数体是 C；
  `#pragma textflag NOSPLIT` 之类指令可直接写。
- **go1.4 的 `chan.go` 是"Go 写的 C"**：它 include 了 `chan.h` 的 C 结构（Go 侧以 `hchan`、`sudog` 小写引用），
  `makechan` 里 `c.buf = (*uint8)(add(unsafe.Pointer(c), hchanSize))`（`E:\CodexWorkspace\GoSrc\go14/src/runtime/chan.go:42`）
  与 C 版 `chanbuf` 宏完全对应。
- **`goc.c` 查无此文件**：
  ```
  E:\CodexWorkspace\GoSrc\go11/src/pkg/runtime/goc.c  → 不存在
  E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/goc.c  → 不存在
  E:\CodexWorkspace\GoSrc\go14/src/runtime/goc.c      → 不存在
  ```
  调度核心（`newproc`/`schedule`/`execute`/`park`/`ready`）全在 `proc.c`。

### 1.3 本报告引用的文件清单

| 文件 | 版本 | 行数 | 用途 |
|---|---|---|---|
| `src/pkg/runtime/chan.c` | go1.0.3 / go1.1 | 1361 | 纯 C 版 channel（含结构体） |
| `src/pkg/runtime/chan.goc` | go1.3 | 1155 | C 方言版 channel（含 selectgo） |
| `src/pkg/runtime/chan.h` | go1.3 / go1.4 | 75 / 68 | `Hchan`/`SudoG`/`WaitQ`/`Select` 结构 |
| `src/pkg/runtime/sema.goc` | go1.3 | 294 | `semacquire`/`semrelease`（sleep/wakeup 原语） |
| `src/pkg/runtime/lock_sema.c` | go1.3 | 266 | `Lock`/`Note` 的 C 实现（futex 语义） |
| `src/pkg/runtime/proc.c` | go1.3 | 3143 | 调度器：`ready`/`park`/`schedule`/`execute` |
| `src/pkg/runtime/asm_amd64.s` | go1.3 | 2199 | `gosave`/`gogo`/`mcall` 上下文切换 |
| `src/runtime/chan.go` | go1.4 | 655 | Go 重写版（对照用） |
| `src/runtime/chan.go` | go1.27.1 | 956 | 现役实现（`hchan`、`chansend`、`chanrecv`） |
| `src/runtime/asm_amd64.s` | go1.27.1 | 1668 | 现役 `gogo`/`systemstack`（无 `gosave`） |
| `src/runtime/lock_futex.go` | go1.27.1 | 163 | 现役 futex 封装（`notesleep`/`futexsleep`） |

---

## 2. 结构体逐字段（C 版，可直接翻译成 C++）

### 2.1 `Hchan`：三代对比

**go1.1（纯 C，`E:\CodexWorkspace\GoSrc\go11/src/pkg/runtime/chan.c:38-51`）**：

```c
struct	Hchan
{
	uintgo	qcount;			// total data in the q
	uintgo	dataqsiz;		// size of the circular q
	uint16	elemsize;
	bool	closed;
	uint8	elemalign;
	Alg*	elemalg;		// interface for element type
	uintgo	sendx;			// send index
	uintgo	recvx;			// receive index
	WaitQ	recvq;			// list of recv waiters
	WaitQ	sendq;			// list of send waiters
	Lock;					// 匿名嵌入：{ uintptr key; }
};
#define chanbuf(c, i) ((byte*)((c)+1)+(uintptr)(c)->elemsize*(i))
```

两个要点：
- `Alg* elemalg` 是**元素操作函数表**（copy/print/equal），C 版 chan 靠它做类型擦除：
  `c->elemalg->copy(c->elemsize, dst, src)`（`chan.c:212`）。这就是 C++ 里 `memcpy` / placement-new 的位置。
- buffer **紧跟 `Hchan` 之后**（`(c)+1`），一次 `mallocgc(sizeof(*c) + hint*elem->size)` 分配完。

**go1.3（`E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/chan.h:29-46`）**：

```c
// The garbage collector is assuming that Hchan can only contain pointers into the stack
// and cannot contain pointers into the heap.
struct	Hchan
{
	uintgo	qcount;			// total data in the q
	uintgo	dataqsiz;		// size of the circular q
	uint16	elemsize;
	uint16	pad;			// ensures proper alignment of the buffer that follows Hchan in memory
	bool	closed;
	Type*	elemtype;		// element type
	uintgo	sendx;			// send index
	uintgo	recvx;			// receive index
	WaitQ	recvq;			// list of recv waiters
	WaitQ	sendq;			// list of send waiters
	Lock;
};

// Buffer follows Hchan immediately in memory.
#define chanbuf(c, i) ((byte*)((c)+1)+(uintptr)(c)->elemsize*(i))
```

**go1.4（`E:\CodexWorkspace\GoSrc\go14/src/runtime/chan.h:17-30`，Go 版直接操作它）**：

```c
struct	Hchan
{
	uintgo	qcount;			// total data in the q
	uintgo	dataqsiz;		// size of the circular q
	byte*	buf;			// ← 改成显式指针（GC 要能看到它）
	uint16	elemsize;
	uint32	closed;			// ← bool → uint32（无锁快照读）
	Type*	elemtype;		// element type
	uintgo	sendx;			// send index
	uintgo	recvx;			// receive index
	WaitQ	recvq;			// list of recv waiters
	WaitQ	sendq;			// list of send waiters
	Mutex	lock;			// ← Lock → Mutex
};
```

**go1.27.1（`/usr/local/go/src/runtime/chan.go:34-55`）**：

```go
type hchan struct {
	qcount   uint           // total data in the queue
	dataqsiz uint           // size of the circular queue
	buf      unsafe.Pointer // points to an array of dataqsiz elements
	elemsize uint16
	closed   uint32
	timer    *timer // timer feeding this chan
	elemtype *_type // element type
	sendx    uint   // send index
	recvx    uint   // receive index
	recvq    waitq  // list of recv waiters
	sendq    waitq  // list of send waiters
	bubble   *synctestBubble

	lock mutex     // 放最后：读 qcount/sendx 的快路径不碰锁所在 cacheline
}
```

**逐字段语义（照抄时的注释）**：

| 字段 | 含义 | C++ 对应 | 不变量 |
|---|---|---|---|
| `qcount` | 缓冲中当前元素个数 | `m_count` | `0 <= qcount <= dataqsiz` |
| `dataqsiz` | 环形缓冲容量 | `Cap`（编译期） | `0` 即 rendezvous |
| `buf` | 环缓冲首地址 | `std::array<Slot,Cap>` | 1.3 之前内联在结构后 |
| `elemsize` | 元素大小（≤ 1<<16） | `sizeof(T)` | 编译期固定 |
| `closed` | 关闭标志 | `std::uint32_t m_closed` | 关闭后不可重开 |
| `elemtype` | 元素类型描述（含 `alg` 表） | `TypeDesc<T>` | C++ 可模板化省掉 |
| `sendx` | 下一个写入位置 | `m_tail` | `sendx < dataqsiz` |
| `recvx` | 下一个读取位置 | `m_head` | `recvx < dataqsiz` |
| `recvq` | 阻塞的接收者队列 | `m_receivers` | 与 sendq 至少一个为空 |
| `sendq` | 阻塞的发送者队列 | `m_senders` | 同上 |
| `lock` | 保护以上全部字段 | `ChannelSpinLock` | **持锁时绝不 park** |

### 2.2 `SudoG`（等待节点，FastChannel 的 `SendNode`/`RecvNode` 原型）

**go1.1（`E:\CodexWorkspace\GoSrc\go11/src/pkg/runtime/chan.c:21-28`）**：

```c
struct	SudoG
{
	G*	g;			// g and selgen constitute a weak pointer to g
	uint32	selgen;		// weak pointer 的代数（防 select 迟到唤醒）
	SudoG*	link;		// 单链表（enqueue/dequeue 只操作 next）
	int64	releasetime;	// 阻塞开始时间（block profiling）
	byte*	elem;		// 数据元素地址（指向等待者的栈！）
};
```

**go1.3（`E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/chan.h:12-19`）**：

```c
struct	SudoG
{
	G*	g;
	uint32*	selectdone;	// selgen 进化成 CAS 指针：select 抢唤醒权
	SudoG*	link;
	int64	releasetime;
	byte*	elem;		// data element
};
```

**go1.4（`E:\CodexWorkspace\GoSrc\go14/src/runtime/runtime.h:228` 起）**：加了 `next`/`prev`（双向链表）、`waitlink`
（挂到 `g.waiting`）、`isSelect`、`success` 字段；不再是纯单链表。

**go1.27.1（`/usr/local/go/src/runtime/runtime2.go:404-450`）**：

```go
type sudog struct {
	g *g
	next *sudog
	prev *sudog
	elem maybeTraceablePtr  // data element (may point to stack)
	acquiretime int64
	releasetime int64
	ticket      uint32
	isSelect bool
	success bool
	waiters uint16
	parent   *sudog
	waitlink *sudog
	waittail *sudog
	c        maybeTraceableChan
}
```

**要点**：
- `elem` 指向**等待者自己的栈**（`mysg.elem = ep`，`chan.goc:136`），这是直接交接（rendezvous）的核心：
  发送者**直接写接收者的栈**，不经过 buffer。
- `success`（1.4+）/`g->param`（1.3-）区分"被数据唤醒"还是"被 close 唤醒"；
  FastChannel 的 `node->ready` / `node->closed` 两个 bool 正是这个语义。

### 2.3 `WaitQ`

```c
struct	WaitQ
{
	SudoG*	first;
	SudoG*	last;
};
```
`enqueue` 尾插、`dequeue` 头取（`chan.goc:1098-1146`），**只有一把 hchan 锁保护**，所以不需要原子操作。

### 2.4 `Gobuf`（协程上下文，Fiber 切换的寄存器集合）

**go1.3（`E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/runtime.h:211-220`）**：

```c
struct	Gobuf
{
	// The offsets of sp, pc, and g are known to (hard-coded in) libmach.
	uintptr	sp;		// 栈指针
	uintptr	pc;		// 返回地址
	G*	g;		// 所属 goroutine
	void*	ctxt;		// 闭包上下文（DX 寄存器）
	uintreg	ret;		// 返回值
	uintptr	lr;		// ARM 的 link register
};
```

**go1.27.1**：`gobuf` 为 `sp/pc/g/ctxt/ret/lr/bp`（多了 frame pointer `bp`）。
`bp` 是 Go 1.7+ 为了 frame-pointer 栈展开加的，**C++ 协程不需要**（除非要 perf/backtrace 走 frame pointer）。

### 2.5 `G` / `M` 的关键字段（go1.3 `runtime.h:251-290` / `292-349`）

```c
struct	G {
	uintptr	stackguard0;	// 抢占标志（StackPreempt）
	uintptr	stackbase;
	Gobuf	sched;		// ← 上下文保存位置（gogo(&gp->sched)）
	void*	param;		// ← 被唤醒时由唤醒方写入（chan: sg / nil）
	int16	status;		// Grunnable/Grunning/Gwaiting/Gdead
	int64	goid;
	int8*	waitreason;
	G*	schedlink;	// runqueue 链
	bool	preempt;
	M*	m;
	M*	lockedm;
};

struct	M {
	G*	g0;		// 调度栈所属 g
	G*	curg;		// 当前运行的 g
	P*	p;
	int32	locks;		// 持锁计数（决定能否抢占）
	bool	(*waitunlockf)(G*, void*);	// ← park 时执行的回调
	void*	waitlock;
	Note	park;
};
```

`m->waitunlockf` + `m->waitlock` 是 `runtime·park()` 的机制：**在 g0 栈上执行 unlock 回调**，
这样"解锁 + 睡眠"是原子的（不会出现解锁后被抢占、别人抢先的窗口）。

### 2.6 `SemaRoot` / `SemaWaiter`（`E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/sema.goc:25-44`）

```c
struct SemaWaiter
{
	uint32 volatile*	addr;	// 被等待的信号量地址
	G*	g;
	int64	releasetime;
	int32	nrelease;	// -1 for acquire
	SemaWaiter*	prev;
	SemaWaiter*	next;
};

struct SemaRoot
{
	Lock;
	SemaWaiter*	head;
	SemaWaiter*	tail;
	uint32 volatile	nwait;	// Number of waiters. Read w/o the lock.
};

#define SEMTABLESZ 251	// Prime to not correlate with any user patterns.
struct semtable
{
	SemaRoot;
	uint8 pad[CacheLineSize-sizeof(SemaRoot)];	// 每项独占 cacheline
};
```

`semroot(addr) = &semtable[((uintptr)addr >> 3) % SEMTABLESZ]`（`sema.goc:57-61`）。
`SemaWaiter` 分配在**调用者栈上**（`SemaWaiter s;`，`sema.goc:106`）——因为持 `root` 锁期间不会被移动。
这是 C++ 里"等待节点放栈上"的合法先例（FastChannel 用池化节点更稳）。

---

## 3. `chan.goc`（Go 1.3）：send / recv / park 的完整 C 流程

### 3.1 `chansend`（`chan.goc:74-201`）逐段

```c
static bool
chansend(ChanType *t, Hchan *c, byte *ep, bool block, void *pc)
{
	SudoG *sg;
	SudoG mysg;        // ← 等待节点在栈上
	G* gp;
	int64 t0;

	// ① nil channel：阻塞则永久 park，否则直接失败
	if(c == nil) {
		USED(t);
		if(!block)
			return false;
		runtime·park(nil, nil, "chan send (nil chan)");
		return false;  // not reached
	}

	t0 = 0;
	mysg.releasetime = 0;
	if(runtime·blockprofilerate > 0) {
		t0 = runtime·cputicks();
		mysg.releasetime = -1;
	}

	// ② 加锁（hchan 的 Lock，就是 futex/mutex）
	runtime·lock(c);
	if(c->closed)
		goto closed;

	// ③ 有缓冲 → asynch 分支
	if(c->dataqsiz > 0)
		goto asynch;

	// ④ 无缓冲：有等待的接收者就直接交接（rendezvous）
	sg = dequeue(&c->recvq);
	if(sg != nil) {
		if(raceenabled)
			racesync(c, sg);
		runtime·unlock(c);              // ← 先解锁
		gp = sg->g;
		gp->param = sg;                 // ← 唤醒方把"凭证"写进 g->param
		if(sg->elem != nil)
			c->elemtype->alg->copy(c->elemsize, sg->elem, ep);  // 直接写接收者栈
		if(sg->releasetime)
			sg->releasetime = runtime·cputicks();
		runtime·ready(gp);              // ← 后唤醒
		return true;
	}

	// ⑤ 非阻塞：失败返回
	if(!block) {
		runtime·unlock(c);
		return false;
	}

	// ⑥ 阻塞：挂 sendq，park 并原子解锁
	mysg.elem = ep;
	mysg.g = g;
	mysg.selectdone = nil;
	g->param = nil;
	enqueue(&c->sendq, &mysg);
	runtime·parkunlock(c, "chan send");   // park + unlock 是一个原子动作

	// ⑦ 被唤醒：param==nil 说明是 close 唤醒
	if(g->param == nil) {
		runtime·lock(c);
		if(!c->closed)
			runtime·throw("chansend: spurious wakeup");
		goto closed;
	}

	if(mysg.releasetime > 0)
		runtime·blockevent(mysg.releasetime - t0, 2);
	return true;

asynch:
	// ⑧ 有缓冲：满了就 park，醒来 goto 回 asynch 重试
	if(c->closed)
		goto closed;

	if(c->qcount >= c->dataqsiz) {
		if(!block) {
			runtime·unlock(c);
			return false;
		}
		mysg.g = g;
		mysg.elem = nil;        // ← 缓冲满时 elem=nil：是"排队等空位"，不是直接交接
		mysg.selectdone = nil;
		enqueue(&c->sendq, &mysg);
		runtime·parkunlock(c, "chan send");

		runtime·lock(c);
		goto asynch;
	}

	// ⑨ 入环缓冲：sendx 前进，qcount++
	c->elemtype->alg->copy(c->elemsize, chanbuf(c, c->sendx), ep);
	if(++c->sendx == c->dataqsiz)
		c->sendx = 0;
	c->qcount++;

	// ⑩ 入队后若有人在等，唤醒一个（不直接给值，接收者自己来 buffer 取）
	sg = dequeue(&c->recvq);
	if(sg != nil) {
		gp = sg->g;
		runtime·unlock(c);
		if(sg->releasetime)
			sg->releasetime = runtime·cputicks();
		runtime·ready(gp);
	} else
		runtime·unlock(c);
	if(mysg.releasetime > 0)
		runtime·blockevent(mysg.releasetime - t0, 2);
	return true;

closed:
	runtime·unlock(c);
	runtime·panicstring("send on closed channel");
	return false;  // not reached
}
```

**十个动作的顺序就是本文件的全部价值**：
- ④ 直交时 **`unlock` 在 `ready` 之前**（唤醒方不持锁调调度器）；
- ⑧ 缓冲满时挂队用 `mysg.elem = nil`；接收者拉走一个 buffer 槽位后再 `ready` 发送者，
  发送者醒来 `goto asynch` 重新判断（"补位"语义，FastChannel 的 `RefillLocked` 对应其镜像）；
- ⑩ 入 buffer 后只 `ready` 不写值 —— 接收者醒来从 buffer 取，**不会出现"值既在 buffer 又在接收者栈"**。

### 3.2 `chanrecv`（`chan.goc:204-343`）逐段

```c
static bool
chanrecv(ChanType *t, Hchan* c, byte *ep, bool block, bool *received)
{
	SudoG *sg;
	SudoG mysg;
	G *gp;
	int64 t0;

	if(c == nil) {
		USED(t);
		if(!block)
			return false;
		runtime·park(nil, nil, "chan receive (nil chan)");
		return false;  // not reached
	}

	t0 = 0;
	mysg.releasetime = 0;
	if(runtime·blockprofilerate > 0) {
		t0 = runtime·cputicks();
		mysg.releasetime = -1;
	}

	runtime·lock(c);

	// ① 有缓冲 → asynch（接收侧先看 buffer，不看 sendq）
	if(c->dataqsiz > 0)
		goto asynch;

	// ② 无缓冲 + 已关闭 → closed
	if(c->closed)
		goto closed;

	// ③ 无缓冲：有等待的发送者就直接会合
	sg = dequeue(&c->sendq);
	if(sg != nil) {
		runtime·unlock(c);
		if(ep != nil)
			c->elemtype->alg->copy(c->elemsize, ep, sg->elem);  // 从发送者栈取
		gp = sg->g;
		gp->param = sg;
		if(sg->releasetime)
			sg->releasetime = runtime·cputicks();
		runtime·ready(gp);
		if(received != nil)
			*received = true;
		return true;
	}

	if(!block) {
		runtime·unlock(c);
		return false;
	}

	// ④ 阻塞：挂 recvq
	mysg.elem = ep;
	mysg.g = g;
	mysg.selectdone = nil;
	g->param = nil;
	enqueue(&c->recvq, &mysg);
	runtime·parkunlock(c, "chan receive");

	if(g->param == nil) {
		runtime·lock(c);
		if(!c->closed)
			runtime·throw("chanrecv: spurious wakeup");
		goto closed;
	}

	if(received != nil)
		*received = true;
	if(mysg.releasetime > 0)
		runtime·blockevent(mysg.releasetime - t0, 2);
	return true;

asynch:
	// ⑤ 缓冲空
	if(c->qcount <= 0) {
		if(c->closed)      // ← 空且已关：closed 分支（零值 + received=false）
			goto closed;

		if(!block) {
			runtime·unlock(c);
			if(received != nil)
				*received = false;
			return false;
		}
		mysg.g = g;
		mysg.elem = nil;
		mysg.selectdone = nil;
		enqueue(&c->recvq, &mysg);
		runtime·parkunlock(c, "chan receive");

		runtime·lock(c);
		goto asynch;
	}

	// ⑥ 从 buffer 取，recvx 前进，qcount--，槽位清零
	if(ep != nil)
		c->elemtype->alg->copy(c->elemsize, ep, chanbuf(c, c->recvx));
	c->elemtype->alg->copy(c->elemsize, chanbuf(c, c->recvx), nil);   // 清空槽位，防悬挂指针
	if(++c->recvx == c->dataqsiz)
		c->recvx = 0;
	c->qcount--;

	// ⑦ 取完若有等待的发送者（缓冲满时挂的），唤醒一个让它补位
	sg = dequeue(&c->sendq);
	if(sg != nil) {
		gp = sg->g;
		runtime·unlock(c);
		if(sg->releasetime)
			sg->releasetime = runtime·cputicks();
		runtime·ready(gp);
	} else
		runtime·unlock(c);

	if(received != nil)
		*received = true;
	if(mysg.releasetime > 0)
		runtime·blockevent(mysg.releasetime - t0, 2);
	return true;

closed:
	if(ep != nil)
		c->elemtype->alg->copy(c->elemsize, ep, nil);   // ← 写零值
	if(received != nil)
		*received = false;
	if(raceenabled)
		runtime·raceacquire(c);
	runtime·unlock(c);
	if(mysg.releasetime > 0)
		runtime·blockevent(mysg.releasetime - t0, 2);
	return true;
}
```

**必须照抄的三条语义**：
1. **close 后 buffer 里剩下的值还能读**：`asynch` 先判 `qcount > 0` 取数据，只有 `qcount <= 0` 才看 `closed`；
2. **close 唤醒的接收者写零值、`received=false`**（closed 分支）；
3. **⑦ 唤醒发送者让其补位**，发送者醒来走 `asynch` 把值写进刚空出的槽（不是直接交给接收者）。

### 3.3 `closechan`（`chan.goc:1035-1082`）

```c
static void
closechan(Hchan *c, void *pc)
{
	SudoG *sg;
	G* gp;

	if(c == nil)
		runtime·panicstring("close of nil channel");

	runtime·lock(c);
	if(c->closed) {
		runtime·unlock(c);
		runtime·panicstring("close of closed channel");
	}
	c->closed = true;

	// release all readers
	for(;;) {
		sg = dequeue(&c->recvq);
		if(sg == nil)
			break;
		gp = sg->g;
		gp->param = nil;          // ← nil 表示"不是数据唤醒，是 close"
		runtime·ready(gp);
	}

	// release all writers
	for(;;) {
		sg = dequeue(&c->sendq);
		if(sg == nil)
			break;
		gp = sg->g;
		gp->param = nil;
		runtime·ready(gp);
	}

	runtime·unlock(c);   // ← 全部 ready 完才解锁
}
```

`closechan` 是**唯一在锁内连续 ready 多个 g** 的地方；可以接受（ready 只是入 runqueue），
但会有"持锁期间别人排队"的延迟。

### 3.4 `dequeue` / `enqueue` / `dequeueg`（`chan.goc:1098-1146`）

```c
static SudoG*
dequeue(WaitQ *q)
{
	SudoG *sgp;

loop:
	sgp = q->first;
	if(sgp == nil)
		return nil;
	q->first = sgp->link;

	// if sgp participates in a select and is already signaled, ignore it
	if(sgp->selectdone != nil) {
		// claim the right to signal
		if(*sgp->selectdone != 0 || !runtime·cas(sgp->selectdone, 0, 1))
			goto loop;      // ← 已被别的 case 抢走，跳过
	}
	return sgp;
}

static void
enqueue(WaitQ *q, SudoG *sgp)
{
	sgp->link = nil;
	if(q->first == nil) {
		q->first = sgp;
		q->last = sgp;
		return;
	}
	q->last->link = sgp;
	q->last = sgp;
}
```

**FastChannel 没有 select，`selectdone` 那 5 行（1110-1114）不要抄**——照抄会让每次 dequeue 多一次可能失败的 CAS。

### 3.5 park 链路：`chansend` → `parkunlock` → `mcall` → `park0` → `schedule` → `execute` → `gogo`

```
chansend(阻塞分支)
  └─ runtime·parkunlock(c, "chan send")             proc.c:1383
       └─ runtime·park(parkunlock, lock, reason)    proc.c:1362
            ├─ m->waitlock = lock
            ├─ m->waitunlockf = parkunlock
            ├─ g->waitreason = reason
            └─ runtime·mcall(park0)                 asm_amd64.s:159
                 ├─ 保存 g->sched.pc/sp（在 g0 栈上运行）
                 └─ park0(gp)                       proc.c:1390
                      ├─ gp->status = Gwaiting
                      ├─ m->waitunlockf(gp, m->waitlock)   ← 在这里 unlock(c)！
                      ├─ m->curg = nil
                      └─ schedule()                 proc.c:1311
                           └─ execute(gp2)          proc.c:1128
                                └─ runtime·gogo(&gp2->sched)  asm_amd64.s:140
```

**这是 Go park 的精髓**：`unlock` 发生在 **g0 栈上的 park0 里**，而不是用户 g 的栈上。
因为用户 g 的栈在解锁那一刻可能被 GC 扫描/移动，而 g0 栈稳定。
我们的 Scheduler 没有 g0，但可以用"在调度器栈上执行 unlock 回调"等价实现（见 8.3）。

### 3.6 `ready`（`proc.c:374-389`）

```c
void
runtime·ready(G *gp)
{
	m->locks++;  // disable preemption because it can be holding p in a local var
	if(gp->status != Gwaiting) {
		runtime·printf("goroutine %D has status %d\n", gp->goid, gp->status);
		runtime·throw("bad g->status in ready");
	}
	gp->status = Grunnable;
	runqput(m->p, gp);                 // ← 进 P 的本地队列（无锁，P 私有）
	if(runtime·atomicload(&runtime·sched.npidle) != 0 &&
	   runtime·atomicload(&runtime·sched.nmspinning) == 0)
		wakep();                       // ← 有空闲 P 就踢一个 M 起来
	m->locks--;
}
```

`ready` 语义很轻：**改状态 + 入队 + 必要时踢醒一个 M**。它要求 `gp->status == Gwaiting`，
这就是 double wakeup 会 throw 的原因。

---

## 4. 调度器主干（`proc.c`，C 版）

### 4.1 `schedule`（`proc.c:1311-1357`）

```c
static void
schedule(void)
{
	G *gp;
	uint32 tick;

	if(m->locks)
		runtime·throw("schedule: holding locks");

top:
	if(runtime·sched.gcwaiting) {
		gcstopm();
		goto top;
	}

	gp = nil;
	// 每 61 个 tick 去全局队列拿一次，保证公平（防止两个 goroutine 互生互跑饿死别人）
	tick = m->p->schedtick;
	if(tick - (((uint64)tick*0x4325c53fu)>>36)*61 == 0 && runtime·sched.runqsize > 0) {
		runtime·lock(&runtime·sched);
		gp = globrunqget(m->p, 1);
		runtime·unlock(&runtime·sched);
		if(gp)
			resetspinning();
	}
	if(gp == nil) {
		gp = runqget(m->p);          // P 本地队列（无锁）
		if(gp && m->spinning)
			runtime·throw("schedule: spinning with local work");
	}
	if(gp == nil) {
		gp = findrunnable();          // 全局队列 / 偷别的 P / netpoll（阻塞直到有活）
		resetspinning();
	}

	if(gp->lockedm) {
		startlockedm(gp);             // LockOSThread 的 g：交回它绑定的 M
		goto top;
	}

	execute(gp);                      // 永不返回
}
```

`tick - (((uint64)tick*0x4325c53fu)>>36)*61 == 0` 是 `tick % 61 == 0` 的**无除法写法**
（两个 MUL 代替一个 DIV）；C++ 直接写 `tick % 61 == 0` 即可。

### 4.2 `execute`（`proc.c:1128-1150`）

```c
static void
execute(G *gp)
{
	if(gp->status != Grunnable)
		runtime·throw("execute: bad g status");
	gp->status = Grunning;
	gp->waitsince = 0;
	gp->preempt = false;
	gp->stackguard0 = gp->stackguard;
	m->p->schedtick++;
	m->curg = gp;
	gp->m = m;

	hz = runtime·sched.profilehz;
	if(m->profilehz != hz)
		runtime·resetcpuprofiler(hz);

	runtime·gogo(&gp->sched);         // ← 切到 gp 的栈，永不返回
}
```

### 4.3 与 Bento Scheduler 的对照

| Bento / Go2cpp 概念 | Go C 版对应 | 说明 |
|---|---|---|
| `Scheduler::Run()` | `schedule()` | 主循环，取下一个 runnable |
| `Scheduler::Park()` | `runtime·park()` | 保存上下文 + 调调度器 |
| `Scheduler::Wake()` | `runtime·ready()` | 改状态 + 入队 + 必要时唤醒 worker |
| per-worker 队列 | `m->p->runq` | 本地队列，仅 owner 可写 |
| 全局队列 | `runtime·sched.runq` | 需要 `sched` 锁 |
| work stealing | `findrunnable()` 的 `runqsteal` | 见 `proc.c:1154+` |
| Fiber 上下文 | `Gobuf` | `sp`/`pc`/`ctxt` 三件套 |
| `ChannelParkOnce` | `parkunlock` + `m->waitunlockf` | 调度器栈上 unlock |

**建议照抄的一条**：`park0` 里"先转 `Gwaiting` → 再调 `unlockf` → 再 `schedule()`"的顺序。
先 schedule 后 unlock 会在"已换出但锁没放"的窗口里死锁。

---

## 5. 汇编：`gosave` / `gogo` / `mcall`（上下文切换的全部）

### 5.1 Go 1.3（`asm_amd64.s:123-185`）——最干净的版本

```asm
// void gosave(Gobuf*)
// save state in Gobuf; setjmp
TEXT runtime·gosave(SB), NOSPLIT, $0-8
	MOVQ	8(SP), AX		// gobuf
	LEAQ	8(SP), BX		// caller's SP
	MOVQ	BX, gobuf_sp(AX)
	MOVQ	0(SP), BX		// caller's PC
	MOVQ	BX, gobuf_pc(AX)
	MOVQ	$0, gobuf_ret(AX)
	MOVQ	$0, gobuf_ctxt(AX)
	get_tls(CX)
	MOVQ	g(CX), BX
	MOVQ	BX, gobuf_g(AX)
	RET

// void gogo(Gobuf*)
// restore state from Gobuf; longjmp
TEXT runtime·gogo(SB), NOSPLIT, $0-8
	MOVQ	8(SP), BX		// gobuf
	MOVQ	gobuf_g(BX), DX
	MOVQ	0(DX), CX		// make sure g != nil
	get_tls(CX)
	MOVQ	DX, g(CX)
	MOVQ	gobuf_sp(BX), SP	// restore SP
	MOVQ	gobuf_ret(BX), AX
	MOVQ	gobuf_ctxt(BX), DX
	MOVQ	$0, gobuf_sp(BX)	// clear to help garbage collector
	MOVQ	$0, gobuf_ret(BX)
	MOVQ	$0, gobuf_ctxt(BX)
	MOVQ	gobuf_pc(BX), BX
	JMP	BX

// void mcall(void (*fn)(G*))
// Switch to m->g0's stack, call fn(g).
TEXT runtime·mcall(SB), NOSPLIT, $0-8
	MOVQ	fn+0(FP), DI
	get_tls(CX)
	MOVQ	g(CX), AX	// save state in g->sched
	MOVQ	0(SP), BX	// caller's PC
	MOVQ	BX, (g_sched+gobuf_pc)(AX)
	LEAQ	8(SP), BX	// caller's SP
	MOVQ	BX, (g_sched+gobuf_sp)(AX)
	MOVQ	AX, (g_sched+gobuf_g)(AX)

	MOVQ	m(CX), BX
	MOVQ	m_g0(BX), SI
	CMPQ	SI, AX	// if g == m->g0 call badmcall
	JNE	3(PC)
	MOVQ	$runtime·badmcall(SB), AX
	JMP	AX
	MOVQ	SI, g(CX)	// g = m->g0
	MOVQ	(g_sched+gobuf_sp)(SI), SP	// sp = m->g0->sched.sp
	PUSHQ	AX
	ARGSIZE(8)
	CALL	DI
	POPQ	AX
	MOVQ	$runtime·badmcall2(SB), AX
	JMP	AX
	RET
```

**Fiber 切换可以直接照搬的全部内容**：
- `gosave` 保存 **caller 的 SP/PC**（`8(SP)` 是返回地址，`LEAQ 8(SP)` 是 caller 栈顶），
  等价于 `getcontext` 只保存两个寄存器；
- `gogo` 恢复 SP 后直接 `JMP` 到保存的 PC —— **不是 `RET`**，因为 PC 是 caller 的下一条指令地址，
  SP 已指向 caller 栈帧，`JMP` 等价于"从 gosave 返回"；
- 寄存器约定：只需保存 SP/PC/ctxt(DX)/ret(AX)，**callee-saved 寄存器（RBX/RBP/R12-R15）
  由编译器在函数序言里保存到栈上**，SP 一恢复它们自然恢复。`fcontext`/`ucontext` 也是同一约定。

### 5.2 Go 1.27（`asm_amd64.s:402-419`）的变化

```asm
// func gogo(buf *gobuf)
// restore state from Gobuf; longjmp
TEXT runtime·gogo(SB), NOSPLIT, $0-8
	MOVQ	buf+0(FP), BX		// gobuf
	MOVQ	gobuf_g(BX), DX
	MOVQ	0(DX), CX		// make sure g != nil
	JMP	gogo<>(SB)

TEXT gogo<>(SB), NOSPLIT, $0
	get_tls(CX)
	MOVQ	DX, g(CX)
	MOVQ	DX, R14		// set the g register  ← 新增：g 常驻寄存器
	MOVQ	gobuf_sp(BX), SP	// restore SP
	MOVQ	gobuf_ctxt(BX), DX
	MOVQ	gobuf_bp(BX), BP	// ← 新增：frame pointer
	MOVQ	$0, gobuf_sp(BX)
	MOVQ	$0, gobuf_ctxt(BX)
	MOVQ	$0, gobuf_bp(BX)
	MOVQ	gobuf_pc(BX), BX
	JMP	BX
```

**`gosave` 在 1.4 之后被删掉了**：现代 Go 用 `mcall`/`systemstack` 里的 `save()`（`proc.c:1481`）代替，
只需要"保存 caller 的 PC/SP"一个场景。C++ 若要支持"从任意点切出再切回"，`gosave` 更通用，
**建议照抄 1.3 的 `gosave`**。

---

## 6. sema：`semacquire` / `semrelease`（`sema.goc:92-185`）

```c
static int32
cansemacquire(uint32 *addr)
{
	uint32 v;
	while((v = runtime·atomicload(addr)) > 0)
		if(runtime·cas(addr, v, v-1))
			return 1;
	return 0;
}

void
runtime·semacquire(uint32 volatile *addr, bool profile)
{
	SemaWaiter s;	// Needs to be allocated on stack, otherwise garbage collector could deallocate it
	SemaRoot *root;

	// Easy case.
	if(cansemacquire(addr))
		return;

	root = semroot(addr);
	for(;;) {
		runtime·lock(root);
		// Add ourselves to nwait to disable "easy case" in semrelease.
		runtime·xadd(&root->nwait, 1);
		// Check cansemacquire to avoid missed wakeup.
		if(cansemacquire(addr)) {
			runtime·xadd(&root->nwait, -1);
			runtime·unlock(root);
			return;
		}
		// Any semrelease after the cansemacquire knows we're waiting
		// (we set nwait above), so go to sleep.
		semqueue(root, addr, &s);
		runtime·parkunlock(root, "semacquire");
		if(cansemacquire(addr))
			return;
	}
}

void
runtime·semrelease(uint32 volatile *addr)
{
	SemaWaiter *s;
	SemaRoot *root;

	root = semroot(addr);
	runtime·xadd(addr, 1);

	// Easy case: no waiters?
	// This check must happen after the xadd, to avoid a missed wakeup
	if(runtime·atomicload(&root->nwait) == 0)
		return;

	runtime·lock(root);
	if(runtime·atomicload(&root->nwait) == 0) {
		runtime·unlock(root);
		return;
	}
	for(s = root->head; s; s = s->next) {
		if(s->addr == addr) {
			runtime·xadd(&root->nwait, -1);
			semdequeue(root, s);
			break;
		}
	}
	runtime·unlock(root);
	if(s) {
		if(s->releasetime)
			s->releasetime = runtime·cputicks();
		runtime·ready(s->g);
	}
}
```

**三条铁律**（FastChannel 的 `send`/`signal` 协议已在实践，值得写成注释）：
1. **先 `xadd(nwait, +1)` 再 `cansemacquire` 复检**：防"release 发生在检查之后、park 之前"的丢失唤醒；
2. **`semrelease` 先 `xadd(addr, 1)` 再读 `nwait`**：读不到 nwait 就不碰锁，快路径零争用；
3. **`semqueue` 的节点必须在等待者栈上（或池里）**，且**被唤醒前不能被释放** ——
   即 FastChannel `WaitNodePool::Acquire/Release` 必须遵守的生命周期。

---

## 7. 与 C++ 的映射表

### 7.1 类型

| Go C 类型 | 语义 | C++ 写法 | 注意 |
|---|---|---|---|
| `uintgo` | 机器字长无符号 | `std::uintptr_t` / `std::size_t` | 32 位平台要一致 |
| `byte*` | 原始字节指针 | `std::byte*` / `void*` | `elem` 指向等待者栈 |
| `Type*` | 类型描述（含 `alg`） | `template<class T> struct TypeDesc` | 模板可省掉运行时 `alg` 调用 |
| `Alg*` | copy/print/equal 函数表 | `T::copy(dst, src)` 或 `memcpy` | 非平凡类型必须 placement-new |
| `Lock`/`Mutex` | `{ uintptr key; }` | `FutexMutex`（`atomic<uint32_t>` + futex） | 1.3 用 M\* 链表，1.4+ 用 futex |
| `Note` | 一次性通知 | `Event`/`Note`（`Xchg` + `futex_wake`） | 双唤醒必须 throw |
| `Gobuf` | 协程上下文 | `struct FiberCtx { void* sp; void* pc; void* ctxt; }` | `bp` 可选 |
| `G` | 协程控制块 | `Fiber` | `status`/`sched`/`param` 三件套 |
| `M` | OS 线程 | `Worker` | `g0` → 调度器栈 |
| `P` | 处理器（本地队列） | `Worker::runq` | 无锁本地队列 |
| `SudoG` | 等待节点 | `SendNode`/`RecvNode` | `elem` 指针语义 |
| `SemaRoot`/`SemaWaiter` | 信号量 | `WaitQueue` + `WaitNode` | 251 桶哈希 |
| `Hchan` | 通道 | `FastChannel<T,Cap>` | 见第 8 节 |

### 7.2 函数

| Go C 函数 | 位置 | C++ 对应 | 备注 |
|---|---|---|---|
| `chansend` | `chan.goc:74` | `FastChannel::Send/TrySend` | 分支顺序要一致 |
| `chanrecv` | `chan.goc:204` | `FastChannel::Recv/TryRecv` | 同上 |
| `closechan` | `chan.goc:1035` | `FastChannel::Close` | 唤醒全部等待者 |
| `dequeue`/`enqueue` | `chan.goc:1098/1135` | `WaitQueue::pop_front/push_back` | 单链表，锁保护 |
| `parkunlock` | `proc.c:1383` | `Scheduler::ParkUnlock(lock)` | **在调度器栈上 unlock** |
| `park0` | `proc.c:1390` | `Scheduler::ParkOnSchedulerStack` | 状态转换 + schedule |
| `ready` | `proc.c:374` | `Scheduler::Wake` | 入 runq + 必要时 `wakep` |
| `schedule`/`execute` | `proc.c:1311/1128` | `Scheduler::Run/Execute` | 永不返回 |
| `gosave`/`gogo` | `asm_amd64.s:125/140` | `Fiber::Save/Restore`（内联汇编） | 见 5.1 |
| `mcall` | `asm_amd64.s:159` | `Scheduler::SwitchToSchedulerStack` | 保存 sched 后切 g0 |
| `semacquire`/`semrelease` | `sema.goc:104/150` | `WaitQueue::Wait/WakeOne` | 三条铁律 |

### 7.3 内存序（C 版没有 `atomic_*` 内存序参数，但语义明确）

| 位置 | Go 代码 | 需要的 C++ 内存序 |
|---|---|---|
| `c->closed` 读（`chansend` 入口） | 普通读，但在 `lock(c)` 内 | `relaxed` + 锁 acquire，或 `acquire` |
| `c->qcount`/`sendx` 读 | 持锁访问 | `relaxed`（锁已提供屏障） |
| `gp->param = sg`（唤醒方写） | 持锁/解锁前写 | `release`（配合被唤醒方 `acquire`） |
| `*received = true` | 栈上变量，无竞争 | 不需要原子 |
| `atomicload(&sched.npidle)` | 无锁快照 | `relaxed` |
| `notewakeup` 的 `Xchg` | 原子交换 | `acq_rel` |
| `semrelease` 的 `xadd(addr,1)` | 原子加 | `acq_rel`（唤醒方）/ `acquire`（等待方读） |
| `runqput` 发布 g | 需让别的 M 看到完整初始化 | `release` |
| `canSemacquire` 的 CAS | 原子 CAS | `acq_rel`/`seq_cst` |

**一条经验**：Go 的 C 版 chan **所有字段都在 hchan 锁内访问**，所以除 `closed` 的无锁快照和
`g->param` 的发布/消费外，其余全是 `relaxed`。FastChannel 用自旋锁（`ChannelSpinLock`），
锁本身提供 acquire/release，字段访问同样可以 relaxed。

---

## 8. 可以直接抄的代码片段（带来源）

### 8.1 抄 `chan.h` 的字段表 → FastChannel（来源：`E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/chan.h:29-42` + `E:\CodexWorkspace\GoSrc\go14/src/runtime/chan.h:17-30`；对应现状 `include/go2cpp/concurrency/fast_channel.hpp:517-531`）

```cpp
template <typename T, std::size_t Cap>
class FastChannel final {
  // hchan.qcount   → m_count
  // hchan.dataqsiz → Cap（编译期，省掉一个字段）
  // hchan.buf      → m_slots（std::array<Slot, Cap>，Go 1.3 内联在结构后，等价）
  // hchan.elemsize → sizeof(T)（编译期）
  // hchan.closed   → m_closed（建议改 uint32_t / atomic，见 9.2）
  // hchan.elemtype → TypeDesc<T>（C++ 可省）
  // hchan.sendx    → m_tail
  // hchan.recvx    → m_head
  // hchan.recvq    → m_receivers
  // hchan.sendq    → m_senders
  // hchan.lock     → m_lock（alignas(64) 独立 cacheline，比 Go 更抗伪共享，保留）
};
```

### 8.2 抄 `chansend` 的骨架（来源：`chan.goc:74-201`，C → C++）

```cpp
FastStatus Send(T value) {
  if (closed()) return FastStatus::kClosed;        // ①
  std::unique_lock lock(m_lock);                    // ②
  if (m_closed) { lock.unlock(); return FastStatus::kClosed; }

  if (!m_receivers.empty()) {                       // ④ 直交
    RecvNode* r = m_receivers.pop_front();
    r->value.emplace(std::move(value));
    r->ready = true;
    lock.unlock();                                  // ← 先解锁
    WakeNode(r);                                    // ← 后唤醒
    return FastStatus::kOk;
  }
  if constexpr (Cap > 0) {
    if (m_count < Cap) {                            // ⑨ 入环缓冲
      m_slots[m_tail].Construct(std::move(value));
      AdvanceTail(); ++m_count;
      lock.unlock();
      return FastStatus::kOk;
    }
  }
  // ⑥ park（节点从池里取，等价于栈上 SudoG + 1.5+ 的 sudog pool）
  SendNode* node = WaitNodePool<SendNode>::Acquire();
  node->value.emplace(std::move(value));
  m_senders.push_back(node);
  lock.unlock();
  const bool woken = WaitNode(node);                // parkunlock：已在锁外
  lock.lock();
  ...
}
```

### 8.3 抄 `park0` 的顺序（来源：`proc.c:1362-1411`）

```cpp
void Scheduler::ParkOnSchedulerStack(Fiber* f, UnlockFn unlock, void* lock) {
  f->status = FiberStatus::kWaiting;   // 先改状态
  f->m = nullptr;
  current_worker_->curg = nullptr;
  if (unlock) unlock(f, lock);         // ← 在调度器栈上解锁，绝不先解锁再改状态
  switch_to_scheduler();               // schedule()
}
```

### 8.4 抄 `semacquire` 的防丢唤醒三行（来源：`sema.goc:127-146`）

```cpp
for (;;) {
  std::lock_guard g(root->lock);
  root->nwait.fetch_add(1, std::memory_order_relaxed);   // ① 先登记
  if (canAcquire(addr)) {                                 // ② 再复检
    root->nwait.fetch_sub(1, std::memory_order_relaxed);
    return;
  }
  semQueue(root, addr, &s);                               // ③ 然后入队 park
  root->lock.ParkUnlock(scheduler);
  if (canAcquire(addr)) return;
}
```

### 8.5 抄 `gosave`/`gogo` 的汇编约定（x86-64，来源：`asm_amd64.s:125-153`）

```asm
# 配合 struct FiberCtx { void* sp, *pc, *ctxt; }
gosave:
    movq 8(%rsp), %rax      # gobuf
    leaq 8(%rsp), %rbx      # caller's SP（跳过 return address）
    movq %rbx, 0(%rax)      # gobuf->sp
    movq (%rsp), %rbx       # caller's PC（return address）
    movq %rbx, 8(%rax)      # gobuf->pc
    ret

gogo:
    movq 8(%rsp), %rbx      # gobuf
    movq 0(%rbx), %rsp      # restore SP
    movq 8(%rbx), %rax      # restore PC
    movq 16(%rbx), %rdx     # restore ctxt（可选）
    jmp *%rax               # 不是 ret
```

**注意**：C++ 里不能用裸 `jmp` 跳过栈帧而不处理 callee-saved 寄存器 —— 但 `gosave` 的设计是
"保存 caller 的 SP/PC"，callee-saved 寄存器由 caller 的栈帧负责（编译器已保存），
所以 SP 恢复后寄存器自然正确。这是 Go 的做法，也是 `fcontext`/`ucontext` 的做法。

---

## 9. 给 FastChannel 的实现建议（逐条，对照现状）

现状：`include/go2cpp/concurrency/fast_channel.hpp`，`PushLocked` 先看 `m_receivers` 再入 buffer
（对齐 `chansend` ④⑨），`PopLocked` 先出 buffer、`RefillLocked` 补位、再与 `m_senders` 会合
（对齐 `chanrecv` ⑥⑦③）。**分支顺序已与 Go 一致，方向正确。** 差距在细节：

### 9.1 把"锁外唤醒"落实到代码

- **Go**：`chan.goc:118-127` 直交路径是 `runtime·unlock(c); ...; runtime·ready(gp);`。
- **现状**：`fast_channel.hpp:425-434` 的 `PushLocked` 在**持锁**状态调 `detail::FastWakeNodeLocked(receiver)`。
- **建议**：锁内只做 `signal.store(kReady, release)`，把 futex/event wake 收集到局部变量，
  `lock.unlock()` 后再 wake。理由：持锁 wake 会让被唤醒线程立刻争用同一 cacheline
  （`receiver->signal`），表现为"唤醒延迟计入临界区"。

### 9.2 补 `closed` 的无锁快照快路径

- **Go 1.27**：`chan.go:213` `if !block && c.closed == 0 && full(c) { return false }`，
  **不加锁**就读 `closed` 与队列状态，靠"关闭单向不可逆"论证免锁安全。
- **现状**：`TrySend` 直接拿锁（`fast_channel.hpp:268-271`）。
- **建议**：`m_closed` 改 `std::atomic<std::uint32_t>`（与 Go 的 `uint32 closed` 一致），
  `TrySend` 先 `if (m_closed.load(relaxed)) return kClosed;` 再判满，最后才拿锁。
  把"关闭后的 TrySend"从一次 futex 竞争降到一次原子读。

### 9.3 `Recv` 在 `kClosed` 时写零值

- **Go**：`chan.goc:332-336` 的 `closed:` 分支 `alg->copy(elemsize, ep, nil)` —— **写零值**，
  `received=false`。Go 的 `v, ok := <-c` 里 `v` 永远是零值而不是旧值。
- **现状**：`Recv` 返回 `kClosed` 时 `out` 保持不变（`fast_channel.hpp:356-360`），注释也没写。
- **建议**：在 `kClosed` 分支 `out = T{}`（与 Go 一致，避免调用方读到脏数据），
  或至少在文档里明确"kClosed 时 out 不被修改"。**当前注释没写，容易踩坑。**

### 9.4 `Close` 后 `m_senders` 必须为空

- **Go**：`closechan` 把所有 `sendq` 的 g 唤醒（`param=nil`），醒来后 `chansend` 走 `closed` 分支 panic。
  所以 **close 返回后 sendq 一定是空的**。
- **现状**：`Close()`（`fast_channel.hpp:379-395`）逐个 pop 并 `signal=kClosed`，但没有断言。
- **建议**：`Close()` 结束时 `assert(m_senders.empty() && "close 后不应再有 sender")`（debug 构建），
  并把"关闭后 Send 一定返回 kClosed"写成不变量注释。

### 9.5 不要抄 `dequeue` 的 `selectdone` CAS

`chan.goc:1110-1114` 那 5 行是为 select 抢唤醒权服务的。FastChannel 无 select，
照抄会引入无意义的 CAS 和 `goto loop` 重试。**只抄 `q->first = sgp->link` 这一行。**

### 9.6 `elem` 指针语义 → `std::optional<T>`

- **Go**：`mysg.elem = ep` 指向等待者栈；发送者直接 `alg->copy(sg->elem, ep)`。
- **现状**：`node->value` 是 `std::optional<T>`（`fast_channel.hpp:292`），语义等价且更安全。
- **建议保持**，但注意 `emplace(std::move(value))` 后 `value` 处于 moved-from；
  park 失败要 `value = std::move(*node->value)` 取回（现状 `:295` 已这么做，✓）。

### 9.7 把 hchan 的不变量写成 debug 断言

- **Go 1.27**：`chan.go:9-18` 注释了三条不变量：
  ```
  At least one of c.sendq and c.recvq is empty,
  c.qcount > 0 implies that c.recvq is empty.
  c.qcount < c.dataqsiz implies that c.sendq is empty.
  ```
- **建议**：debug 构建在持锁处加：
  ```cpp
  assert(m_senders.empty() || m_receivers.empty());
  assert(m_count == 0 || m_receivers.empty());
  assert(m_count == Cap || m_senders.empty());
  ```
  这三条一旦被破坏，就是"值丢/值重/死等"三类 bug 的根因。

### 9.8 `m_lock` 的位置

Go 1.27 把 `lock` 放在 `hchan` **最后**（`chan.go:54`），让 `qcount/sendx/recvx` 这些快路径字段相邻；
FastChannel 把 `m_lock` 放最前并 `alignas(64)` 独立 cacheline（`fast_channel.hpp:517`）是
**更抗伪共享**的做法，**不要改**。

---

## 10. 复现验证命令

```bash
# 1. 拉取历史源码（gitee 镜像，本机已验证可用；WSL 内 github 解析为 ::1）
git clone --depth 1 --single-branch --branch go1.3 https://gitee.com/mirrors/go.git E:\CodexWorkspace\GoSrc\go13
git clone --depth 1 --single-branch --branch go1.4 https://gitee.com/mirrors/go.git E:\CodexWorkspace\GoSrc\go14
git clone --depth 1 --single-branch --branch go1.1 https://gitee.com/mirrors/go.git E:\CodexWorkspace\GoSrc\go11

# 2. 确认 goc.c 不存在、chan 实现在哪
ls E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/ | grep -E 'chan|goc|sema'

# 3. 看 C 结构
sed -n '29,46p' E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/chan.h      # Hchan
sed -n '12,19p' E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/chan.h      # SudoG

# 4. 看 C 版 send/recv
sed -n '74,201p'  E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/chan.goc
sed -n '204,343p' E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/chan.goc

# 5. 看汇编
sed -n '123,185p' E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/asm_amd64.s

# 6. 现役版对照
sed -n '34,55p'   /usr/local/go/src/runtime/chan.go     # hchan
sed -n '176,310p' /usr/local/go/src/runtime/chan.go     # chansend
sed -n '402,419p' /usr/local/go/src/runtime/asm_amd64.s # gogo
```

---

## 11. 一句话总结

**要抄就抄 Go 1.3 的 `chan.h` + `chan.goc` + `asm_amd64.s` 三份文件**：
它们用 100 行 C 讲清了 channel 的完整状态机，用 60 行汇编讲清了协程切换，
用 40 行 C 讲清了 park/unlock 的原子性。1.4 之后只是把同样逻辑换了门语言，
信息量反而被 GC/race/泛型稀释了。

---

## 附：引用位置速查

| 内容 | 文件:行 |
|---|---|
| Hchan（1.1 纯 C） | `E:\CodexWorkspace\GoSrc\go11/src/pkg/runtime/chan.c:38-51` |
| Hchan（1.3） | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/chan.h:29-46` |
| Hchan（1.4） | `E:\CodexWorkspace\GoSrc\go14/src/runtime/chan.h:17-30` |
| hchan（1.27） | `/usr/local/go/src/runtime/chan.go:34-55` |
| SudoG（1.1） | `E:\CodexWorkspace\GoSrc\go11/src/pkg/runtime/chan.c:21-28` |
| SudoG（1.3） | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/chan.h:12-19` |
| sudog（1.27） | `/usr/local/go/src/runtime/runtime2.go:404-450` |
| Gobuf（1.3） | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/runtime.h:211-220` |
| G（1.3） | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/runtime.h:251-290` |
| M（1.3） | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/runtime.h:292-349` |
| SemaRoot/SemaWaiter | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/sema.goc:25-44` |
| chansend | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/chan.goc:74-201` |
| chanrecv | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/chan.goc:204-343` |
| closechan | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/chan.goc:1035-1082` |
| dequeue/enqueue | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/chan.goc:1098-1146` |
| semacquire/semrelease | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/sema.goc:104-185` |
| park/parkunlock/park0 | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/proc.c:1362-1411` |
| ready | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/proc.c:374-389` |
| schedule | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/proc.c:1311-1357` |
| execute | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/proc.c:1128-1150` |
| gosave/gogo/mcall | `E:\CodexWorkspace\GoSrc\go13/src/pkg/runtime/asm_amd64.s:123-185` |
| gogo（1.27） | `/usr/local/go/src/runtime/asm_amd64.s:402-419` |
| hchan 不变量注释 | `/usr/local/go/src/runtime/chan.go:9-18` |
| FastChannel 现状 | `include/go2cpp/concurrency/fast_channel.hpp:246-532` |
