# 重审：三套 WaitNode 同构（潦草实现的整改）

> 来源：2026-11-05 用户指出「`ParkingCondition::WaitNode` 和 `sync::WaitNode` 基本同构……这些任务完成的很潦草，重新再完成一遍」。

---

## 一、问题确认（实测三套）

| # | 位置 | 名字 | 字段/方法 |
|---|---|---|---|
| 1 | `include/go2cpp/concurrency/channel.hpp:933` | **`WaitNodeBase`** | `signal`/`seq`/`parked`/`generation`/`next`/`prev`/`linked`/`managed`/`scheduler`/`task`/`select_state`/`select_index` + `ResetBase` |
| 2 | `include/go2cpp/concurrency/core/parking_condition.hpp:242`（实现在 `.cpp`） | **`ParkingCondition::WaitNode`** | `result`/`futex_seq`/`futex_waiters`/`generation`/`next`/`prev`/`linked`/`managed`/`scheduler`/`task` + `Acquire/Release/Finish/Wake/Disarm/TimeoutFromTimer` |
| 3 | `src/concurrency/sync.cpp:340` | **`sync::WaitNode`** | `result`/`futex_seq`/`futex_waiters`/`granted`/`generation`/`next`/`prev`/`linked`/`managed`/`scheduler`/`task`/`backend` + `Arm/Disarm/TryFinish/TryCancelAndWake/Wake/WakeFast` |

**重复的部分**（三处各写一遍）：

| 逻辑 | 重复次数 | 风险 |
|---|---|---|
| 侵入式双向链表 `LinkTail/Unlink/PopFront` | **3** | 今天修 RWMutex 丢唤醒时，链表逻辑要改三处 |
| `generation` 代际校验（防节点复用误杀） | **3** | 同上 |
| 唤醒协议（`result` CAS → 唤醒 futex / Scheduler） | **3** | **今天 3 例丢唤醒正是在这三处各修一遍** |
| TLS 池 / orphan 池（`Acquire/Release`） | 2（parking + sync 各一套） | 内存管理逻辑重复 |
| `managed` 判定（Scheduler + Task） | **3** | 语义漂移风险 |

**已经证明这个重复有害**：
- `c5b888f`（RWMutex 丢唤醒）——修在 `rwmutex.hpp` 的私有实现；
- `b176c7f`（MuxStream 丢唤醒）——修在 `mux.cpp`；
- `b662f70`（storage flush 失败态）——又是一处；
- **同一类 bug 修 N 遍，因为「等待节点」有 N 套实现。**

## 二、根因

> **没有抽公共的「等待节点基类」**——`channel.hpp:933` 已经有 `WaitNodeBase`，但 `ParkingCondition` 和 `sync` 各自从零写了一套，没有继承它。

## 三、整改方案

### 目标结构

```
include/go2cpp/concurrency/core/wait_node_base.hpp   ← 新建：唯一的公共实现
├── WaitNodeBase（字段：generation / next / prev / linked / managed / scheduler / task）
├── 链表操作：LinkTail / Unlink / PopFront（静态，持队列锁）
├── 代际校验：GenerationMatches(g)
├── 唤醒协议骨架：TryFinish / WakeNative / WakeManaged
└── 池：TLS 池 + orphan 池（Acquire / Release）
        ↑ 继承/组合
├── channel::WaitNode  （+ select_state / select_index）
├── sync::WaitNode     （+ granted / backend）
└── ParkingCondition::WaitNode（+ timeout 回调）
```

### 分步（每步独立可验证）

| 步 | 内容 | 验收 |
|---|---|---|
| **① 抽基类** | 新建 `core/wait_node_base.hpp`，把三处**完全相同**的字段与链表操作搬进去（不改行为） | 三处改为 `: public WaitNodeBase`，`ctest` 全过 |
| **② 统一池** | 把 TLS 池 + orphan 池也搬进基类（sync 与 parking 共用） | 泄漏测试（ASan）0 报告 |
| **③ 统一唤醒协议** | `TryFinish` / `WakeNative` / `WakeManaged` 的骨架统一，各模块只留特化（channel 的 select、sync 的 granted、parking 的 timeout） | 丢唤醒专项的 TSan 复测 |
| **④ 删除重复** | 三处的链表/代际/池代码删除 | 代码行数下降，行为不变 |

### 不能动的（语义差异）

| 模块 | 特有语义 |
|---|---|
| channel | `select_state`/`select_index`（select 多路等待） |
| sync | `granted`（饥饿模式的锁所有权直接交接）、`backend`（Future 等待后端） |
| parking | `TimeoutFromTimer`（定时器回调落点）、`Disarm` 门 |

## 四、验收

1. 三处只保留「特化字段 + 特化方法」，公共逻辑零重复；
2. `ctest -R 'channel|sync|timer|concurrency'` 全过；
3. ASan/UBSan/TSan 三套 0 报告（尤其 TSan 的 `__call_tls_dtors` UAF）；
4. 丢唤醒专项的 3 个历史用例（RWMutex/MuxStream/tunnel Direction）仍全过；
5. 代码行数对比（整改前 vs 后）。

## 五、记录

- 本条整改**纳入丢唤醒专项**（`docs/plans/lost-wakeup-plan.md` 的「防复发」章节）——统一节点基类是从根上减少「同类 bug 修 N 遍」。
- 状态：**待开工**（用户确认后按 ①→④ 分步做）。
