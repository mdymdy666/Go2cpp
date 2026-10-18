# 第二轮推进记录（2026-10-18，18:00 截止）

> 角色：总技术管理 —— 技术走向、项目规范、子 agent 成果复查
> 定时任务：每 15 分钟一轮（`schedule-c7b639c2`）

## 路线图（6 小时）

| 时段 | 重点 | 状态 |
|---|---|---|
**11:58~13:00** | 新模块第一批（math + rand）+ 状态机审查 + 文档盘点 | ✅ **完成** |
13:00~14:30 | 新模块第二批（bufio / container / path）| 待 |
14:30~16:00 | 新模块第三批（crypto hash / compress）| 待 |
16:00~17:00 | 旧代码审查第二轮（IO / 网络 / 调度器）| 待 |
17:00~18:00 | 收口：全量验证 + 提交 + 总结 | 待 |

---

## 第 1 轮（11:58）

**项目盘点**：工作树干净、99 个测试、40+ 模块。

**起的 3 条流**：
| agent | 任务 |
|---|---|
`d807c608` | math + math/rand 新模块 |
`feefb873` | 状态机穷举 + 并发审查 |
`62723516` | 文档盘点 + 风险评定 |

---

## 第 2 轮（12:13）—— 三条流全部完成

### agent3（文档盘点）✅

**最重要的贡献：纠正了我的误判。**

`go2cpp_fiber_stack_limit_full` **不是 flaky，是确定性失败**（5/5 次 ctest + 直接跑 3/3 都失败）。
根因：`example/concurrency/fiber_stack_limit_full.cpp:521` 的断言

```cpp
copy_limit < dump_limit    // 恒假：实测 dump=1162 < copy=1631
```

**我当时把「单独跑通过」当成了"ctest 环境差异"的证据 —— 这是错的。**
单独跑和 ctest 跑的是**同一个二进制**，如果断言恒假，两次都该失败。

**修正 4 处文档冲突**：`modules.md`（os 待接入过时）、`quic.md`（§一~§四 描述已删代码）、
`G11`（称 QUIC 零实现）、`NEXT-STEPS`（flaky 定性错误）。新增 `docs/os.md`(72 行)。

**它列的最高风险**：TLS `verify_peer` 默认 false、HybridGate 无界自旋仍在 3 个生产文件、
`sync::Mutex` 4 线程 29.8 µs/op。

### agent2（状态机审查）✅

**14 组真状态机 + 8 组结果码**盘点。新增 3 个测试：

| 测试 | 规模 | 结果 |
|---|---|---|
`test_state_machine_matrix.cpp` | GState 8×17=136 格 + Fiber/Timer/Channel/Future | **1844 断言，0 失败** |
`test_http2_state_matrix.cpp` | StreamState 7×12=84 格（对照 RFC 9113 §5.1）+ 随机 2 万步 | **20567 断言，0 失败** |
`test_http2_state_desync_repro.cpp` | 5 个真缺陷复现 | **7 断言稳定失败**（DISABLED）|

**关键结论：纯状态机逻辑是干净的**（终态不可复活，非法转移被明确拒绝）。
问题都在**连接层的校验顺序**：

| 严重性 | 缺陷 | 位置 |
|---|---|---|
**高** | `WriteData` 先推进状态机再校验 → 被拒写入仍吃 END_STREAM | `connection.cpp:269-291` |
**中** | 连接无 GOAWAY/draining/closed 状态 | `connection.cpp:434-444,620-629` |
**中** | 接收侧流控不校验（RFC 要求 FLOW_CONTROL_ERROR）| `connection.cpp:541-575` |
**中** | `StreamStateMachine` 无并发保护却写"任意线程可用"（TSan 实锤）| `stream_state.hpp:81` |
**中** | `max_concurrent_streams` 只写 SETTINGS 从不校验 | 全仓 2 处 |

### agentV（math/rand）✅

**2699 行**（955 实现 + 1744 测试）。`math` 委托 libm，`rand` 用 **xoshiro256\*\* + splitmix64**。

**实测**：
- 21 个一元函数 × 10 万随机输入 + 32 特殊值 → **max_ulp=0, mismatches=0**
- 压测：`sqrt` 429.69 Mops/s、`exp` 255.30、`log` 275.72
- 全局 `Intn(1000)` **8.84 ns/op**（mutex），实例版 4.56 ns/op
- 真实场景 math 8 个 + rand 7 个（骰子/牌堆/掉宝/负载均衡/确定性重放）

**非显然发现**：Go `Rand.Read` 每个 `Int63()` 只产出 **7 个字节**（不是 8），用 Go 1.27 实测确认。

### 我这一轮做的事

1. **修 `fiber_stack_limit_full` 的恒假断言**（`05e4f37`）—— 全量 ctest 从 98/99 → **103/103**
2. **提交三条流产出**（`e5a18f7`，已推送）

---

## 第 3 轮（12:23）—— 修复高危缺陷

起 `f3aa1332` 修：
- **R1**：`WriteData` 先校验后推进
- **R13**：接收侧流控校验
- **R2**：连接 GOAWAY/draining/closed 状态
- **R3**：只改文档（非线程安全）

**关键要求**：修完把 `test_http2_state_desync_repro.cpp` 的 **DISABLED 去掉转正** ——
这是检验修复真的生效的唯一证据。

---

## 当前基线

```
远端 HEAD: e5a18f7
全量 ctest: 103 通过 / 0 失败 / 104 注册（1 个 DISABLED 是缺陷复现）
工作树: 干净（agent4 正在改）
```
