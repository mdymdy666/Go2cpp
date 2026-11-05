# 状态码细化 + StatusMessage（2026-11-05）

## 范围 / 盘点

只改 `include/go2cpp/concurrency/**`、`src/concurrency/**`、`tests/concurrency/**`；全仓
`enum class …Status` 共 **40** 个，concurrency 内 **5** 个全部加 Message。

| 枚举 | 状态数 | Message | 新增状态 |
|---|---:|---|---|
| `core::ParkingCondition::WaitStatus` | 6 | `WaitStatusMessage` | `kSpurious`/`kNotArmed`/`kInvalid`（末尾追加） |
| `ChannelStatus` | 10 | `ChannelStatusMessage` | 无 |
| `FutureStatus` | 7 | `FutureStatusMessage` | 无 |
| `FastStatus` | 4 | `FastStatusMessage` | 无 |
| `ContextRollback::Status` | 5 | `ContextRollback::StatusMessage` | 无 |
| 其余 35 个（tunnel/distributed/io/http2/bento…） | — | 不在允许目录，只盘点 | 无 |

## WaitStatus 细化（0/1/2 未变，新增值只追加末尾）

| 值 | 状态 | 语义 / 何时返回 | 调用方动作 |
|---:|---|---|---|
| 0 | `kNotified` | 谓词已满足（通知先到或等待期间被唤醒） | 正常继续，不重试 |
| 1 | `kTimedOut` | 超过 deadline 且谓词未满足 | 检查业务条件，重试或返回超时 |
| 2 | `kCancelled` | Task 取消在等待前/期间生效 | 立即传播取消，不重试 |
| 3 | `kSpurious` | 无关唤醒/信号打断、谓词未满足；当前实现内部重 park，不返回 | 若收到则重检谓词并重试 |
| 4 | `kNotArmed` | 手动 Fiber（无 Scheduler/Task）无法 park，未进入阻塞即返回 | 改非阻塞接口或先绑 Scheduler，不原地重试 |
| 5 | `kInvalid` | 等待返回时节点仍 `kWaiting`（状态机违约） | 停止重试、记录诊断、尽快失败 |

实现改动：① 手动 Fiber 由 `kCancelled` → `kNotArmed`（旧值仍留给真正的 Task 取消）；
② 节点返回仍为 `kWaiting` 时映射 `kInvalid`（原伪装成 `kCancelled`）；
③ `wait()` 由「仅 `kCancelled` 退出」改为「非 `kNotified` 即退出」，否则 `kNotArmed` 忙等；
④ debug 事件 `switch` 补 3 个 case，`-Wswitch` 零告警。

## StatusMessage 接口

```cpp
const char* WaitStatusMessage(ParkingCondition::WaitStatus) noexcept;  // go2cpp::core
const char* ChannelStatusMessage(ChannelStatus) noexcept;
const char* FutureStatusMessage(FutureStatus) noexcept;
const char* FastStatusMessage(FastStatus) noexcept;
static const char* ContextRollback::StatusMessage(Status) noexcept;
```

全部 `noexcept`、无堆分配、返回静态存储；内容是「发生了什么 + 调用方该怎么做」，
不重复枚举名；未知值返回 `"unknown …"`。与既有 `*StatusName`（稳定枚举名）分工并存。

## 测试

`tests/concurrency/test_status_message.cpp`（独立 main，未改 CMake）：5 个枚举 × 全部
32 个状态值均断言「非空、长度 ≥20、非枚举名、非 unknown」；`WaitStatus` 历史值
`static_assert`（0..5）；真实场景 6 个（20ms 超时、过去 deadline 立即返回、`notify_one`、
`notify_all` 4 等待者、通知不缓冲、手动 Fiber `wait()` 立即返回）。编译
`g++ -std=c++20 -O2 -Wall -Wextra -Werror` 通过，运行退出码 **0**；
`test_parking_condition.cpp` `-fsyntax-only` 通过（旧行为不变）。

## git status（相关文件）

`M channel.hpp / context.hpp / core/parking_condition.hpp / fast_channel.hpp / future.hpp`、
`M src/concurrency/parking_condition.cpp`（开始时已是 M，本次仅追加约 16 行）、
`?? tests/concurrency/test_status_message.cpp`。未 commit；大文件 diff 主要来自任务开始前
已有的未提交改动，未回退。
