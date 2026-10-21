# 线程版 Channel 挂起调查（2026-10-21）

## 背景

基线压测（`docs/concurrency-baseline.md`）曾报告「线程版 Channel 高频混合场景挂起一次」，
未复现、未定位；当时 mixed 场景改用 Fiber 路径规避。为死磕正确性，本调查专门复现。

## 装置

`tests/concurrency/test_channel_thread_stress.cpp`（纯 `std::thread`，不启动 Scheduler）：

| 场景 | 说明 |
|---|---|
| `mpmc64` / `mpmc1` / `mpmc0` | 4 生产者 + 4 消费者，cap=64/1/0 |
| `mpmc8_64` / `mpmc8_0` | 8+8，cap=64/0 |
| `rtt0` | 双向无缓冲 ping-pong |
| `mixed` | 2 worker（`sync::Mutex` → `Channel::Send` → `PC notify`）+ consumer + helper |

- 每轮每场景 10 万消息；
- watchdog：30s 无进展 → dump 线程 stage + `Channel::DebugSnapshot`（cap/len/send_waiters/recv_waiters/gen）+ marker + `gdb thread apply all bt`；
- 抓现场链路用 `--inject-hang` 注入自检验证（bt 显示发送线程停在 `ParkingCondition::WaitNative ← Channel<int>::SendUntil`，`send_waiters=1`）。

## 结果（未复现）

| 配置 | 规模 | 结果 |
|---|---|---|
| 4 进程 × 100 轮 × 5 场景 | 2.0 亿条 | 0 挂起 |
| 8 进程 × ~150 轮 × 7 场景（load 26 / 12 核超订 2.2×） | 8.58 亿条 | 0 挂起 |
| `DEBUG_DUMP=ON` 4 进程 × 20 轮 × 7 场景 | 0.56 亿条 | 0 挂起 |
| **合计** | **≈11.14 亿条（≈22 亿次原语）** | **0 挂起、0 校验失败** |
| TSAN（ON 插桩）1 轮 × 2000 消息 | — | 无数据竞争 |

## 审计（未发现缺陷）

逐点审计线程路径：
- Send/Recv 谓词重检闭合（park 前/唤醒后）；
- cap<=1 的 spin/parked/signaled 协议；
- `ParkingCondition` per-node futex（generation 匹配）；
- `sync::Mutex` 令牌交接（kLocked/kWaiter/kStarving/kWoken）；
- Close 唤醒路径；
- 锁序（Send/Recv/Close 的获取顺序）。

**未发现**丢唤醒、锁序环、无界自旋。

## 结论

**未能复现**（11 亿条 / 1300 轮）；无法给出具体根因。

判定口径（后续若再遇到）：
1. 先确认是**产品 bug**（可复现 + 现场明确指向丢唤醒/锁序）还是**测试设计**（混合场景锁顺序、未处理关闭、watchdog 误报）；
2. 复现时用本装置（`--inject-hang` 自检 + `DebugSnapshot` + gdb）抓现场；
3. 长跑建议：`test_channel_thread_stress` + TSAN + 事件环（ON 构建）。

## 复现

```bash
# 编译（需 build-work）
g++ -std=c++17 -O2 -I include tests/concurrency/test_channel_thread_stress.cpp \
  -L build-work -lgo2cpp_channel -lgo2cpp_sync -lgo2cpp_parking_condition ... \
  -Wl,-rpath,$PWD/build-work -o /tmp/channel_stress

# 跑（每轮 10 万消息，7 场景）
/tmp/channel_stress --rounds=100 --scenario=mixed
/tmp/channel_stress --inject-hang   # 自检抓现场链路
```
