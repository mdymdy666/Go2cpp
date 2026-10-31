# Select 语义：Go 对齐 + 有值优先（D1 真正修复）

> 状态：已实现并通过回归/压测（2026-10-31）。
> 实现位置：`src/concurrency/channel.cpp` 的 `Select()` 探测阶段（step2）。
> 回归测试：`tests/concurrency/test_select_value_first.cpp`。
> 压测程序：`tests/concurrency/stress_select_value_first.cpp`。

## 1. Go 的 select 语义（基准）

Go 运行时 `selectgo` 的规则（`runtime/chan.go`）：

1. 先把所有 case 的 channel 按地址全序加锁（`sellock`），再逐个检查是否就绪；
2. **多个 case 同时就绪时，按 pollorder（随机顺序）选第一个**，因此是伪随机而不是固定优先级；
3. 没有 case 就绪且存在 `default` 时走 `default`；
4. 否则把当前 goroutine 挂到所有 case 的等待队列上 park，任一 channel 就绪时只允许一个赢家接管结果；
5. `close(chan)` 让所有等待该 channel 的收发者就绪（接收者拿到 `ok=false`）。

本库（go2cpp）实现同样的「随机起点探测 + 单一赢家等待节点 + default + 取消/超时」，
差别只在第 2 步的排序规则：**在随机之上，把「有值 / 可发送」排在「已关闭」之前**。

## 2. 为什么需要「有值优先」（审计 D1）

旧实现的探测阶段是「从随机起点开始逐个 `probe()`，第一个 `ready` 就返回」。
`probe()` 对 channel case 会真正调用 `TryRecv` / `TrySend`，因此：

- 命中「有值」的 recv case → 值被取走并返回，正确；
- 命中「已关闭且缓冲为空」的 recv case → 返回 `kClosed`，也「就绪」；
- 但如果同一个 select 里另一个通道还有缓冲值，**这次返回会把那个值留在原地**。
  调用方看到 `kClosed` 后按 Go 习惯 `break` 退出循环，那个缓冲值再也无人取走
  —— 这就是 D1「Close 与 Select 并发时丢 1 个缓冲值」的根因。

关键事实：**`Close` 本身已经不丢值**（关闭时先交付缓冲值、普通接收者先排空，
见 `Channel::Close()` 的 D1 注释），丢值发生在 `Select` 的选择顺序。

## 3. 本库的排序规则

### 3.1 探测阶段（无等待路径）

```
扫描所有非 default case（从线程本地随机起点轮转）：
  probe() 未就绪                 → 跳过
  probe() 就绪且 status==kReady  → 立刻返回（传输已发生，不能回滚）
  probe() 就绪且 status!=kReady  → 记为候选（closed / nil / invalid / cancelled）
扫描结束后仍有候选                 → 返回第一个候选
否则 → 取消检查 → default → 超时检查 → 注册等待（park）
```

由此得到确定的优先级：

| 优先级 | 结果类型 | 是否消耗传输 | 说明 |
|---|---|---|---|
| 1（最高） | recv **有值**（`status==kReady`） | 是（取走值） | 必须立刻返回，否则值丢 |
| 1（同档） | send **可完成**（`status==kReady`） | 是（发出值） | 必须立刻返回，否则值发出却丢弃结果 |
| 2 | `kClosed` / `kNil` / `kInvalid` / `kCancelled` | 否 | 暂存，等扫描确认没有可交付的值/可完成的发送 |
| 3 | `default` | 否 | 无任何 case 就绪时使用 |
| 4 | 取消 / 超时 | — | 在 default 之后、park 之前检查 |

要点：

- **有值 / 可发送 > 关闭**：这是 D1 的修复本身。关闭 case 不再遮蔽同一 select 中
  另一个通道的缓冲值。
- **关闭 > default**：只要有任意 case 处于 closed/nil 等就绪状态，就不走 default。
- **有值 vs 可发送**：二者都在扫描中「立刻返回」，谁先被探测到谁赢 —— 与 Go 的
  随机顺序一致，不额外规定优先级。原因是若把 send 成功的结果也延后，就必须
  回滚一次已经发生的发送（接收方可能已经拿到值），那是真正的丢值。
- **同一档内部仍随机**：每次 `Select` 用线程本地 xorshift 取起点，避免固定下标
  让后面的 case 饿死（Go 的 per-P fastrand 对应物）。

### 3.2 等待路径（已 arm）

`arm` 阶段按 `SelectLockKey` 全序一次性持有整组 channel 锁（对齐 Go `selectgo`
的 `sellock`），在锁内重新验证就绪并登记等待节点；`SelectWaitState` 保证只有
一个 case 能提交结果（`TrySelect` / `TrySelectPair`）。这条路径的语义未改变：

- 任一 channel 就绪（发送者直交 / 缓冲入队 / 关闭）都会唤醒等待者；
- 关闭时 `Close()` 对 select 接收者先交付缓冲值、缓冲为空才提交 `kClosed`；
- 取消 / 超时与就绪并发时，以 `SelectWaitState` 的单一赢家为准。

## 4. 与 Go 的差异（明确记录）

| 场景 | Go | 本库 |
|---|---|---|
| 多个 recv 有值 | 随机 | 随机（保留） |
| 有值 + 可发送同时就绪 | 随机 | 随机（探测顺序决定） |
| **有值 + closed 同时就绪** | 随机（可能先中 closed，靠调用方继续收） | **一定先返回值**（工程增强） |
| 可发送 + closed 同时就绪 | 随机 | **一定先完成发送** |
| closed + default | closed 优先 | closed 优先（一致） |

差异的理由：Go 的 channel 可以被 goroutine 长期持有，调用方通常按 `range` /
循环收完；本库的使用方（业务代码、`select` 循环）常在 `kClosed` 时直接退出，
因此「closed 遮蔽有值」是实际丢值路径（D1）。把 closed 延后不改变任何
「有值/可发送」的结果，只影响多个就绪结果之间的选择顺序，属于可接受的工程增强。

## 5. 一个被否决的方案（记录踩坑）

最初实现给 `SelectCase` 增加内建 `peek` 入口（无副作用预判 + 三档探测），
单测/压测全部通过，但**改变了公共结构体 `SelectCase` 的尺寸**。本工作树是
多 agent 共享、且存在多个构建目录（`.build-p0-wsl`、`.build-p0-asan2` 等），
「旧 `libgo2cpp_channel.so` + 新头文件编译的测试」在切换点会因结构体布局不一致
读到越界指针，表现为 `Select` 内 SEGFAULT。

因此最终方案**不动任何公共类型/头文件**，只在 `channel.cpp` 内利用「closed/nil
不产生传输」这一性质做延后返回，达到同样的「有值优先」效果。以后若确实需要
完整的三档预判，必须同时重建所有构建目录，或把预判信息放进独立的版本化接口。

## 6. 验证

```bash
# 回归/语义单测（含 D1 复现：修复前 T1/T2/T3 失败）
g++ -std=c++20 -O2 -Iinclude -Itests -pthread \
  tests/concurrency/test_select_value_first.cpp -o /tmp/test_select_value_first \
  -Wl,--no-as-needed -Wl,-rpath,$PWD/.build-bento-wsl -L.build-bento-wsl \
  -lgo2cpp_channel -lgo2cpp_context -lgo2cpp_scheduler -lgo2cpp_fiber \
  -lgo2cpp_error -lgo2cpp_sync -lgo2cpp_debug_event -lgo2cpp_log -ldl -lm
/tmp/test_select_value_first 10000000

# 独立压测（默认 4 场景 × 2500 万操作 = 1e8）
g++ -std=c++20 -O2 -Iinclude -Itests -pthread \
  tests/concurrency/stress_select_value_first.cpp -o /tmp/stress_select_value_first \
  -Wl,--no-as-needed -Wl,-rpath,$PWD/.build-bento-wsl -L.build-bento-wsl \
  -lgo2cpp_channel -lgo2cpp_context -lgo2cpp_scheduler -lgo2cpp_fiber \
  -lgo2cpp_error -lgo2cpp_sync -lgo2cpp_debug_event -lgo2cpp_log -ldl -lm
/tmp/stress_select_value_first all 25000000
```

压测断言：

- `close`：每通道序号严格 `0..N-1`（无丢/无重/有序），总数一致，
  `kClosed` 返回时所有已关闭通道缓冲为空；
- `random`：每通道 `count == produced` 且 `sum == produced*(produced-1)/2`；
- `fiber`：3 个受管 Fiber 用 `Select` 消费，序号严格递增；
- `cancel`：已取消 + 有值必须交付值，已取消 + 空返回 cancelled，0ms 超时返回
  `kTimedOut`，`AfterCase(0)` 立即就绪；
- 全部带看门狗，挂起即 `_Exit(124)`。

## 7. 已知限制

- 「有值 vs 可发送」同时就绪时不做确定性排序（见 §3.1），两者都不丢值；
- `DoneCase` 的 cancelled 结果与 closed 同属「无传输」档，谁先被扫描到谁返回
  （与旧实现的随机顺序相比只是去掉了「closed 必然优先」的偏差）；
- 延后返回的候选只保留扫描顺序中的第一个，不会为了找「更合适」的 closed
  再扫一轮（closed 之间没有语义差别）。
