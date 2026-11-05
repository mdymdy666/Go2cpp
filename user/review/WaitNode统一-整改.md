# WaitNode：只做正确性检查（不做统一解耦）

> 来源：2026-11-05 用户审查提出「三套 WaitNode 同构」；同日用户决定：
>
> **「之前说的 WaitNode 只做正确性检查，不做解耦，重复就重复吧」**
>
> 即：**取消「抽公共基类」的整改**，三套实现各自保留；只对每套做**正确性检查**。

---

## 一、决定（2026-11-05）

| 项 | 决定 |
|---|---|
| 抽公共 `core/wait_node_base.hpp` | ❌ **不做**（解耦取消） |
| 三套节点归一 | ❌ **不做** |
| 重复代码 | ✅ **接受**（「重复就重复吧」） |
| 正确性检查 | ✅ **要做**（三套各自验证） |

**理由（用户判断）**：解耦会改变三处行为，风险大于收益；正确性可以分别验证。

## 二、三套 WaitNode（保留各自实现）

| # | 位置 | 特化 |
|---|---|---|
| 1 | `include/go2cpp/concurrency/channel.hpp:933` **`WaitNodeBase`** | `select_state`/`select_index` |
| 2 | `include/go2cpp/concurrency/core/parking_condition.hpp:242` **`ParkingCondition::WaitNode`** | `TimeoutFromTimer`/`Disarm` |
| 3 | `src/concurrency/sync.cpp:340` **`sync::WaitNode`** | `granted`（饥饿交接）/`backend` |

## 三、正确性检查清单（每套都过一遍）

### 3.1 ParkingCondition::WaitNode

- [ ] 4 状态（kWaiting/kNotified/kTimedOut/kCancelled）的 CAS 唯一赢家：`Finish` 只在 `kWaiting` 时成功；
- [ ] `futex_seq` 的「先 Finish 后推 seq」顺序（防睡眠前唤醒丢失）；
- [ ] `WaitNative` 的三个关键点：每轮先判终态、睡眠前 acquire 重读、区分 EAGAIN/ETIMEDOUT；
- [ ] `WaitManaged` 的 spurious 重试（`park_wait` 返回后重判）；
- [ ] `TimeoutFromTimer` 的 generation 校验（防节点复用误杀）；
- [ ] `Reset` 在 `m_wake_mutex` 内、`m_active=true` 在入队前；
- [ ] `notify_all` 的「锁内摘链+认领、锁外唤醒」+ `t_wake_list` 局部容器；
- [ ] TLS 池析构移交 orphan（防 TSan 的 `__call_tls_dtors` UAF）。

### 3.2 sync::WaitNode

- [ ] `TryFinish`/`TryFinishAndGrant` 的 CAS 唯一赢家；
- [ ] `WakeFast`（独占槽）与 `Wake`（队列锁外）的分工；
- [ ] `Disarm` 的 native 无锁路径（只清 `m_active`/`m_scheduler`，不碰 task）；
- [ ] `TryCancelAndWake` 的 generation 校验 + `m_active || !managed` 判断；
- [ ] `WakeNativeLocked`/`WakeManagedLocked` 的锁内调用约束；
- [ ] `Reset` 持 `m_wake_mutex`（与在途 Wake 互斥）。

### 3.3 channel::WaitNodeBase

- [ ] `signal`/`seq`/`parked` 三原子字的唤醒协议（CAS 0→kReady/kClosed/kCancelled）；
- [ ] `generation` 取消回调定位；
- [ ] `ResetBase` 的字段清零（池复用前）；
- [ ] select 节点的 `select_state`/`select_index` 生命周期。

## 四、方法（按丢唤醒专项的 4 步法）

1. **静态红旗**：`if` 而非 `while`、谓词非 atomic、锁外 notify、无超时无 cancel；
2. **TSan**：`setarch x86_64 -R`，≥10 次；
3. **注入延迟**：`notify` 前插 1µs（放大窗口）；
4. **看门狗**：等待 >2s 打印调用栈。

## 五、输出

- 每套一份检查结论（通过项 / 问题项 / 修复建议），记入本文件或各模块 review；
- **不写代码**（本对话边界：只审查记录）。

## 六、状态

**待审查**（三套各自做一遍正确性检查）。
