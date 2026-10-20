# tools/replay — 语义级 record/replay（v2）

Go2Cpp 调度事件记录 + 离线重放。**不修改核心库**、**不接入顶层 CMake**：
默认构建行为完全不变，只有显式 `-DGO2CPP_REPLAY=ON` 时才编译 demo。

v2 在 v1（i64 + send/recv/close）之上增加：

- **select**：多 channel 竞争、default、timeout、Context 取消；记录最终决策
  （`chosen`、`decision`、值、case 表），重放按记录返回；
- **Context / 超时 / 虚拟时钟**：`ReplayContext` 的取消与 deadline 记录为
  `kCtx`/`kTimer`；`ReplayClock::SleepFor()` 在重放端瞬间推进虚拟时间；
- **值类型**：`SyncChannel<T>` 的 `ValueCodec` 序列化钩子，支持 `std::string`、
  bytes、以及带 `Serialize()/Deserialize()` 的结构体；i64 保持兼容；
- **自动注册入口**：`RECORD_FIBER_ENTRY(fn)` + `ReplayFileAuto()`，CLI
  `replay play` 可直接重跑；
- **检查点分片**：`replay split` 产生可独立重放的前缀分片，
  `replay play --until=SEQ` 只重放到出事点。

详细设计（事件模型、磁盘格式、replay 语义、限制、与可迁移协程的关系）见
[`docs/replay-service.md`](../../docs/replay-service.md)。

## 目录

```
tools/replay/
├── include/replay/
│   ├── format.hpp          # 稳定二进制格式 v2（v1 兼容）
│   ├── recorder.hpp        # 录制端：流式写、seq、虚拟时间、丢唤醒注入
│   ├── replay_source.hpp   # replay 侧窄接口（查表 / park / 时钟 / ctx）
│   ├── sync_channel.hpp    # 可记录/可重放的 channel（值 codec + 组锁）
│   ├── value_codec.hpp     # 值序列化钩子（i64/string/bytes/结构体）
│   ├── select.hpp          # select：多路竞争 / default / timeout / ctx
│   ├── select_state.hpp    # select 共享状态与组锁
│   ├── context.hpp         # ReplayContext：Cancel / deadline
│   ├── clock.hpp           # 虚拟时钟与 sleep
│   ├── gid.hpp             # 逻辑 gid 解析（Task::id 反查）
│   ├── auto_entry.hpp      # RECORD_FIBER_ENTRY / ReplayFileAuto
│   ├── split.hpp           # 前缀分片
│   ├── ucx.hpp             # 单线程 ucontext 协程（replay 专用）
│   └── replay_engine.hpp   # 静态重演 + 完整重放 + JSON
├── src/                    # format/recorder/replay_engine/auto_entry/split
├── demo/
│   ├── scenario.hpp / scenario_entry.cpp   # 3 producer → 1 consumer（96 交接）
│   ├── record_demo.cpp / replay_demo.cpp   # fanin 录制/重放
│   ├── select_demo.cpp                     # select 多 channel 竞争
│   ├── context_demo.cpp                    # Context 超时 + 虚拟时钟
│   └── values_demo.cpp                     # string / 结构体值
├── replay.cpp              # CLI：info / play / split
├── tests/                  # 独立单测（ctest，不影响顶层）
├── CMakeLists.txt          # 独立工程
├── build.sh                # 一键构建（产物在 /tmp/go2cpp-replay-build）
└── scripts/
    ├── run_all.sh                  # 一键跑全部场景
    ├── stress_record_replay.sh     # fanin 10 轮录制+重放
    ├── select_record_replay.sh     # select 5 轮
    ├── context_timeout_replay.sh   # 虚拟时钟 3 轮
    ├── values_record_replay.sh     # string/struct 3 轮
    ├── chunked_replay.sh           # 分片 + until
    └── fault_injection.sh          # 丢唤醒 → stuck
```

## 构建

前置：工作树根目录已有 `build-work/libgo2cpp_*.so`（常规构建产物）。

```bash
./tools/replay/build.sh
# 或手动：
cmake -S tools/replay -B /tmp/go2cpp-replay-build \
      -DGO2CPP_REPLAY=ON \
      -DGO2CPP_REPLAY_ROOT="$PWD" \
      -DGO2CPP_REPLAY_LIB_DIR="$PWD/build-work"
cmake --build /tmp/go2cpp-replay-build -j"$(nproc)"
```

产物：

| 文件 | 作用 |
|---|---|
| `replay` | CLI：`info` / `play` / `split`（ON 构建内置 fanin 入口） |
| `go2cpp_replay_record` | fanin 录制 demo（96 次无缓冲交接） |
| `go2cpp_replay_replay` | fanin 重放 demo（`RECORD_FIBER_ENTRY` 自动入口） |
| `go2cpp_replay_select` | select 多 channel 竞争（record/replay） |
| `go2cpp_replay_context` | Context 超时 + 虚拟时钟（record/replay） |
| `go2cpp_replay_values` | string / 结构体值（record/replay） |
| `go2cpp_replay_tests` | 格式/分片/注册表单测（`ctest`） |

## 跑一遍

```bash
# 一键跑全部验收场景
./tools/replay/scripts/run_all.sh /tmp/go2cpp-replay-build

# 单独跑
/tmp/go2cpp-replay-build/go2cpp_replay_record /tmp/fanin.g2cr
/tmp/go2cpp-replay-build/go2cpp_replay_replay /tmp/fanin.g2cr
/tmp/go2cpp-replay-build/go2cpp_replay_select record /tmp/select.g2cr
/tmp/go2cpp-replay-build/go2cpp_replay_select replay /tmp/select.g2cr
/tmp/go2cpp-replay-build/go2cpp_replay_context record /tmp/context.g2cr
/tmp/go2cpp-replay-build/go2cpp_replay_context replay /tmp/context.g2cr
/tmp/go2cpp-replay-build/go2cpp_replay_values record /tmp/values.g2cr
/tmp/go2cpp-replay-build/go2cpp_replay_values replay /tmp/values.g2cr

# CLI
/tmp/go2cpp-replay-build/replay /tmp/fanin.g2cr                 # 静态 JSON
/tmp/go2cpp-replay-build/replay play /tmp/fanin.g2cr            # 自动完整重放
/tmp/go2cpp-replay-build/replay play /tmp/fanin.g2cr --until=200
/tmp/go2cpp-replay-build/replay split /tmp/fanin.g2cr \
      --every-events=100 --out-dir=/tmp/parts
/tmp/go2cpp-replay-build/replay play /tmp/parts/fanin.part0001.g2cr
```

`ctest`（独立工程，不影响顶层 116 个测试）：

```bash
ctest --test-dir /tmp/go2cpp-replay-build --output-on-failure
```

环境变量：

| 变量 | 作用 |
|---|---|
| `GO2CPP_REPLAY_DROP_WAKE=N` | 丢弃第 N 次唤醒（录制端=真实卡死；重放端=重演卡死） |
| `GO2CPP_REPLAY_WATCHDOG_SEC=S` | 录制端看门狗秒数（默认 5） |

## CLI

```
replay <recording.g2cr>                      静态校验，stdout JSON
replay info <recording.g2cr>                 同上
replay play <recording.g2cr> [options]       自动完整重放（RECORD_FIBER_ENTRY）
replay split <recording.g2cr> [options]      前缀分片
options:
  --until=SEQ           只重放到 seq（play/info）
  --every-events=N      每 N 条事件一个检查点
  --every-ms=M          每 M 虚拟毫秒一个检查点
  --out-dir=DIR         分片输出目录
  --prefix=NAME         分片文件名前缀
  --json                play 时保留 timeline
```

退出码：0=自洽/一致/partial；1=stuck 或不一致；2=用法/格式错误；
3=`play` 时进程内没有注册入口。

## 库接口

```cpp
#include "replay/auto_entry.hpp"
#include "replay/replay_engine.hpp"

// 显式 factory
auto report = go2cpp::replay::ReplayFileWith(path, factory, options);

// 编译期注册入口（业务 .cpp 顶层）
void MyEntry(std::uint32_t gid, go2cpp::replay::ReplaySource& source);
RECORD_FIBER_ENTRY(MyEntry);
auto report = go2cpp::replay::ReplayFileAuto(path, options);

// 静态重演
auto report = go2cpp::replay::ReplayFile(path, options);
std::string json = go2cpp::replay::ReplayReportJson(report);
```

JSON schema `go2cpp.replay.report/1` 新增字段：`virtual_ns`、`stopped_early`、
`stopped_at_seq`、`auto_entry`、`entry_name`、`checkpoints[]`、
`stats.selects/timers/ctx_events/bytes_messages/select_checksum/
replayed_select_checksum/select_choices/replayed_select_choices`，以及
`timeline[].vtime_ns/chosen/data_hex`。
