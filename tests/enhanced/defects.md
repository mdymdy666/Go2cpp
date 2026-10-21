# Go2Cpp 五能力增强测试：缺陷清单

> 基线：工作树 `Go2Cpp-work`，2026-10-20 18:5x（源码在测试期间仍有更新；
> 最终二进制由当前源码重建）。所有复现均只读核心源码，未修改被测代码。
> 环境限制：当前 uid=1000，不能 `mount -t tmpfs`，磁盘满用 `/dev/full` /
> `RLIMIT_FSIZE` 替代；已在报告中标注。
> 
> **状态复核（2026-10-21）**：D1 已修复；D2 **部分修复**（全量 dump 已过滤终态 G，
> RSS 回收仍滞后）；D3/D4 已修复。逐条证据见各条「验证」小节。tools 三套测试已
> 注册进 ctest（`tools-migration` / `tools-cli` /
> `tools-replay`，见 `cmake/Go2CppTests.cmake`）。

## D1（已修复，2026-10-20）CLI 已支持 G2CMIG03/v3

- **状态**：已修复。2026-10-20 起 `go2cpp-cli` 同时识别 v3（`G2CMIG03`，当前写版本）
  与 v2（`G2CMIG02`，兼容读）；v1（`G2CMIG01`）仍明确拒绝。
- **修复位置**：
  - `tools/cli/formats.hpp:38-49`：`kMigrationMagic` 为 `G2CMIG03`，新增
    `kMigrationMagicV2 = "G2CMIG02"`（仅兼容读）；两种魔数都归入 `kMigrationBinary`。
  - `tools/cli/formats.cpp:190-200`：按前 8 字节区分 `G2CMIG03`/`G2CMIG02` 并打印版本。
  - `tools/cli/commands.cpp:1320-1332`：`RequireMigrationSupported()` 接受
    `version == 3 || version == 2`，错误信息明确「CLI 接受 v3（魔数 G2CMIG03，
    当前写版本）与 v2（G2CMIG02，兼容读）」。
- **验证（2026-10-21 复测）**：
  - `tests/enhanced/migration/test_migration.sh`：**34/34 PASS**，其中
    `cli-migrate-dump`、`cli-debug`、`cli-migrate-restore` 三项使用真实 G2CMIG03 快照；
  - `tools/cli/tests/run_all.sh`：**213/213 PASS**。
- **修复前的历史记录（2026-10-20 18:5x）**：
  - 原声明：CLI 只认 `G2CMIG02/v2`；`tools/cli/formats.hpp:38` 的 `kMigrationMagic`
    为 `G2CMIG02`，`tools/cli/commands.cpp:1240-1252` 的 `RequireMigrationV2()` 要求
    `version == 2`。
  - 原现象：`migrate restore`/`debug` 对真实 G2CMIG03 返回 4；`migrate dump` 后端已
    成功写出 G2CMIG03，但 CLI 的 `ProbeFile` 判为未知格式后返回 1。
  - 原影响：CLI 与 tools/migration v3 能力脱节，202 条既有断言只覆盖 G2CMIG02 fixture。
  - 原矩阵失败项：`cli-migrate-dump:真实 G2CMIG03 应成功`、
    `cli-debug:真实 G2CMIG03 应可分析`、`cli-migrate-restore:真实 G2CMIG03 应成功`。
- 说明：D1 编号保留便于与历史报告对照；本条不再计入未修复缺陷。

## D2（高，已部分修复 2026-10-21）全量 scheduler dump 携带大量已完成 G，内存与 dump 体积线性增长

- **状态**：**部分修复**。
  - 已修：全量 dump 不再枚举终态 G，dump 体积有界。
  - 未修：终态 G 在 registry 中的回收仍滞后，进程 RSS 随历史任务数增长。
- **已修部分（2026-10-21）**：
  - 位置：`src/concurrency/scheduler.cpp:3524-3557` 的
    `Scheduler::DebugSnapshotTasks()`：分片遍历时执行
    `if (!task || task->terminal()) continue;`，只把**非终态** G
    放进快照；注释写明「未回收的终态 G 属于调度器内部回收滞后，不应出现在
    对外观测快照里」。
  - 同文件 `IsTaskAlive()`（3559-3578）同样用 `terminal()`
    判定存活，避免把滞留终态 G 误判为活。
  - 修复提交：`4378bc3`（2026-10-20，
    feat(diagnostics): adopt API + Timer Debug/回放 + 测试缺陷修复 D1-D4 + 测试矩阵）。
  - **验证（2026-10-21，按当前源码重建 build-debug）**：
    ```bash
    build-debug/go2cpp_debug_subscribe_demo /tmp/d2check.sock 90 &
    sleep 2   # 以及 10s / 30s / 50s
    GO2CPP_DEBUG_SOCKET=/tmp/d2check.sock \
    build-cli/tools/cli/go2cpp-cli dump --pid PID \
    --sections scheduler --out /tmp/sched.json
    grep -c kDead /tmp/sched.json
    ```
    实测 t=2/10/30/50s：dump **2227/2228/2226/2226 B**、`kDead=0`、
    tasks=6（全部为存活 G），大小与任务数全程稳定；修复前同夹具 48s 为
    238253–241869 个 kDead、61.1–62.1 MB。
- **未修部分（RSS 回收滞后）**：
  - 位置：`src/concurrency/scheduler.cpp:1611-1647` 的
    `collect_terminal_locked()` 仍只在收尾路径调用（调用点 1807、2092、
    2135、4248、4288，均为 shutdown/cancel 类路径）；常规 worker 主循环不回收
    已终态 G，registry 会一直强持有它们。
  - 实测（同一夹具）：RSS **23.7 MB(2s) → 65.3 MB(10s) → 106.1 MB(30s) →
    143.0 MB(50s)**，与 dump 大小脱钩，随运行时间近乎线性增长。
  - 影响：dump 可用性已恢复（不再撑爆 CLI 的 64MiB 传输上限），但长跑进程仍有
    内存增长；这是回收路径问题，不是 dump 枚举问题。
- **修复前历史记录（2026-10-20）**：
  - 位置（修复前）：`src/concurrency/scheduler.cpp:3457-3483` 的
    `DebugSnapshotTasks()` 无条件遍历 `task_registries`，
    **不过滤终态**（kDead/kFailed/kCancelled）；
    `src/debug/debug_dump.cpp:189+`、`src/debug/debug_server.cpp:814+`
    的全量 JSON 直接拼接所有 tasks，没有上限/过滤；
    `tests/debug/debug_subscribe_demo.cpp:108-115` 每轮 send 都
    `scheduler.spawn` 一个 Task，30s 内可产生十几万个已完成的 G
    （放大负载，不是根因）。
  - 复现（历史）：
    ```bash
    build-debug/go2cpp_debug_subscribe_demo /tmp/dbg.sock 120 &
    sleep 2;  # 以及 10s、30s 重复
    GO2CPP_DEBUG_SOCKET=/tmp/dbg.sock go2cpp-cli dump --pid $! \
    --sections scheduler --out /tmp/sched.json
    grep -o kDead /tmp/sched.json | wc -l
    stat -c%s /tmp/sched.json
    ```
  - 实测（历史）：
    - 2s：10159 tasks，2.59 MB；
    - 5s：24831 tasks，其中 **24829 个 kDead**，仅 2 个 kWaiting；
    - 10s：51007 tasks，13.0 MB；
    - 30s：155199 tasks，39.8 MB；RSS 29.9 MB → 97 MB → 277 MB；
    - 48s（`stress_debug.sh` 的 C 段，两次运行）：**238253–241869 个
      kDead、61.1–62.1 MB**；
    - 10000 条随机协议帧后 RSS 12.8 MB → 390 MB（服务存活，但内存不回退）。
  - 期望（历史）：`DebugSnapshotTasks` 只返回仍在注册表且存活的 G；
    RSS 与 dump 大小随负载稳定，不随运行时间线性增长。
  - 实际（历史）：终态 G 一直保留在 registry 并被 dump 枚举；48s 已 59MiB，再持续
    约数秒即超过 CLI 的 64MiB 传输上限（`tools/cli/transport.cpp:55`），
    全量 dump 不可用。
  - 影响（历史）：Debug 的「全量 dump」在长跑进程中退化为不可用；同时是明确的
    内存泄漏。父 Fiber park 本体的可见性/状态不受影响（见 P2 与父 park 结论）。

## D3（中，已修复 2026-10-21）`go2cpp_replay_record /dev/full` 写盘失败仍返回 0

- **状态**：已修复。
- **修复位置**：
  - `tools/replay/demo/record_demo.cpp:164-172`：
    `recorder.Finalize("completed")` 之后读取 `recorder.error()`；
    非空时打印 `[record] 写入记录文件失败（<path>）：<原因>` 并
    `return 2`。
  - `tools/replay/src/recorder.cpp:180-195` 的
    `Recorder::WriteRaw()` 把 short write / flush 失败写入
    `m_error`（本次未改，作为错误来源被 `error()` 读出）。
  - 修复提交：`4378bc3`（2026-10-20）。
- **验证（2026-10-21，重建 build-replay 后）**：
  ```bash
  build-replay/go2cpp_replay_record /dev/full; echo $?
  # [record] path=/dev/full
  # [record] file_bytes=0 events=389
  # [record] 写入记录文件失败（/dev/full）：short write (4/56)
  # exit=2
  ```
  正常路径 `build-replay/go2cpp_replay_record /tmp/fanin.g2cr` 仍
  exit=0（file_bytes=24429、events=394）。
- **注意（复测踩到的点）**：本工作树 2026-10-20 构建的旧
  `build-replay/go2cpp_replay_record` 仍返回 0；必须重建 tools/replay
  后修复才生效（本次用
  `GO2CPP_REPLAY_BUILD_DIR=build-replay ./tools/replay/build.sh` 重建）。
- **修复前历史记录（2026-10-20）**：
  - 位置：`tools/replay/demo/record_demo.cpp:150-164`：
    `recorder.Finalize("completed")` 后只检查 `received.size()`，
    **没有检查** `recorder.error()` / 写入失败；
    `Recorder::WriteRaw()` 在 short write 时只设置 `m_error`
    （`tools/replay/src/recorder.cpp:180-195`）。
  - 复现：
    ```bash
    /tmp/go2cpp-replay-build/go2cpp_replay_record /dev/full; echo $?
    # 输出 [record] file_bytes=0 events=389 ...
    ```
  - 期望：落盘失败返回非 0，并打印写入错误；不能「成功退出但文件为 0 字节」。
  - 实际：exit=0，`file_bytes=0`，events 仍显示 389。
  - 影响：磁盘满/坏盘时故障被静默吞掉，自动化会把空记录当成功。

## D4（中，已修复 2026-10-21）CLI `replay play` 未透传 `GO2CPP_REPLAY_DROP_WAKE`，故障注入失效

- **状态**：已修复。
- **修复位置**：
  - `tools/replay/replay.cpp:174-183`：`RunPlay()` 构造
    `ReplayOptions` 时新增
    `options.drop_wake_at = EnvU32("GO2CPP_REPLAY_DROP_WAKE", 0);`，
    与 demo 入口一致；注释说明 CLI 通过 fork/exec 启动本工具，环境变量天然透传。
  - `tools/cli/commands.cpp:1256-1263`：`RunReplayPlay()`
    不过滤/清空环境，直接
    `RunBackendCapture(backend, {"play", AbsolutePath(probe.path)})`，
    故障注入变量原样继承给后端。
  - 修复提交：`4378bc3`（2026-10-20）。
- **验证（2026-10-21，重建 build-replay 后）**：
  ```bash
  FANIN=/tmp/fanin_d4.g2cr
  GO2CPP_REPLAY_TOOL=$PWD/build-replay/replay \
  GO2CPP_REPLAY_DROP_WAKE=20 \
  build-cli/tools/cli/go2cpp-cli replay play "$FANIN"
  # status=stuck, exit=1（gid 3 is still parked at exit event）
  # 对照（不带 DROP_WAKE）：status=ok, exit=0
  ```
- **修复前历史记录（2026-10-20）**：
  - 位置：`tools/cli/commands.cpp:1151-1185` 只把
    `play <file>` 传给后端，未提供 `drop_wake` 选项或环境透传；
    `tools/replay/replay.cpp:142-149` 的 `RunPlay` 不读取
    `GO2CPP_REPLAY_DROP_WAKE`，该变量只在 fanin demo 的重放入口里被解析。
  - 复现：
    ```bash
    GO2CPP_REPLAY_DROP_WAKE=20 go2cpp-cli replay play /tmp/fanin.g2cr
    # 期望 status=stuck/exit=1；实际 status=ok/exit=0
    GO2CPP_REPLAY_DROP_WAKE=20 /tmp/go2cpp-replay-build/go2cpp_replay_replay /tmp/fanin.g2cr
    # demo 路径：status=stuck/exit=1（注入本身有效）
    ```
  - 期望：CLI 的 `replay play` 与 demo 一样支持环境变量注入
    （README 的环境变量表已声明该能力）。
  - 实际：CLI 路径完全忽略该变量，自动化无法用 CLI 做丢唤醒回归。
  - 影响：故障注入只在上游 demo 可用，CLI 用户无法复现 stuck 场景。

## P2-1（能力缺口）`DumpScheduler.tasks` 不带 parent_id / 栈信息

- 位置：`include/go2cpp/concurrency/scheduler/scheduler.hpp:306-318` 的 `TaskSnapshot`
  只有 id/state/queued/started/last_processor_id/task_class/failed/cancellation_requested；
  `src/debug/debug_dump.cpp:189+` 的 `DumpScheduler` 按此输出。
- 复现：`tests/enhanced/debug/test_parent_park.sh`（父 G spawn 子 G 后 park）。
- 实测：父/子都在 tasks 中，父 state=`kWaiting`（不是 kDead/kCompleted），
  但 `tasks_have_parent_id=0`、`tasks_have_stack=0`。
- 期望：全量 dump 至少能指出父 gid、子 gid 与栈范围；实际只有扁平任务表。
- 影响：父链与栈需要额外机制才能看到（P2，不阻塞 park 可见性）。

## P2-2（能力缺口）没有按 gid 查询 `DumpFiber` 的入口

- 位置：
  - `src/debug/debug_dump.cpp:335+` 的 `DumpFiber(const Fiber&)` 含 parent_id/
    active_parent_id/nesting_depth/stack/current_sp 等字段；
  - 但 `src/debug/debug_server.cpp:1207` 只提供 `RegisterFiber(name, Fiber&)`，
    没有 gid → Fiber 的查询接口；CLI 也没有 `--gid`/`--fiber` 选项。
- 复现：父 park 夹具上 `go2cpp-cli dump --pid <pid> --sections fiber`。
- 实测：exit 3，`目标进程未注册组件 'fiber'`。
- 期望：按父 gid 拉取 `DumpFiber`（或 scheduler tasks 内嵌 fiber 字段）。
- 影响：看到父 G 在 tasks 后，无法继续拉取它的栈/父链（P2）。

## 父 Fiber park 的 P0 结论（无 P0）

- `tests/enhanced/debug/test_parent_park.sh` 实测：
  - 父 G 在 tasks 中：`parent_present=1`；
  - 父 G 状态 `kWaiting`（GState 的 park 状态；**不是** kDead/kCompleted）；
  - 子 G 同样在 tasks 中且 `kWaiting`；
  - 因此“父 park 被误标完成/从 tasks 消失”的 P0 **未复现**。
- 唯一问题是 tasks 不带父链/栈、且无按 gid DumpFiber（P2-1/P2-2）。

## 旧构建观察（当前源码已修复，不计入最终缺陷数）

以下是测试早期在**旧二进制**上复现、但在按当前源码重建后不再复现的问题，
保留记录用于说明测试有效性：

1. CLI `replay play` 只执行静态 info（`replay_executed:false`）。
   - 旧二进制 `/tmp/go2cpp-cli-build/.../go2cpp-cli` 构建于 16:24/16:39，
     早于 `tools/cli/commands.cpp:1179-1181` 的 `play` 子命令修复。
   - 当前源码已改为 `RunBackendCapture(backend, {"play", <file>})`，重建后
     `tests/enhanced/replay/test_replay.sh` 的 `cli-replay-play:应完整重放` 已 PASS。
2. DebugServer 慢客户端阻塞后续命令约 2.8-3.3s。
   - 旧 `build-debug/go2cpp_debug_subscribe_demo` 构建于 17:57，早于
     `src/debug/debug_server.cpp:510-525` 的首字节超时修复。
   - 当前源码使用 `kRequestFirstByteTimeoutMs` 限制首字节等待；重建后
     `slow-client:慢连接不阻塞 STATUS` 已 PASS（36/36）。
