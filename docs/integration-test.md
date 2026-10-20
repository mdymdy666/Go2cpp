# Go2Cpp 四线集成联调测试（Debug / 迁移 / 重放 / CLI）

> 执行日期：2026-10-20（Asia/Shanghai）
> 工作树：`/UserData/CodexWorkSpace/Go2Cpp-work`（WSL2 Ubuntu 24.04，未 commit）
> 结论：**四条端到端全部真实跑通**；联调中发现并修复 **2 个真实阻塞问题**（一个全量构建失败、一个 CLI 迁移魔数过时）。
> 集成脚本：`tools/cli/tests/integration_e2e.sh`（44 项断言，44 PASS / 0 FAIL）。

---

## 0. 结论速览

| 线 | 端到端路径 | 结果 | 关键证据 |
|---|---|---|---|
| Debug | 真实进程 → `/tmp/go2cpp-<pid>.sock` → `go2cpp-cli status/dump` | **通过** | `STATUS`/`JSON scheduler`/`JSON fiber`/`JSON channel` 与 CLI 输出逐字段一致 |
| 迁移 v2 | `migrate dump` → `migrate verify` → `migrate restore` | **通过** | 两进程栈地址不同（PIE/ASLR），Fiber 跨进程继续执行，`result=7` |
| 迁移 CLI | `go2cpp-cli migrate restore/dump` 走真实后端 | **通过** | CLI 识别 `G2CMIG02`/`version=2`；v1 与 `version=3` 均被拒绝（退出码 4） |
| 重放 | `go2cpp_replay_record` → `go2cpp_replay_replay` | **通过** | 96 次 rendezvous，`consistency=MATCH` |
| 重放 CLI | `go2cpp-cli replay play` 走真实离线 `replay` 后端 | **通过** | 报告 `status=ok`、`messages=96`、`checksum_ok=true` |
| 丢唤醒 | 注入丢唤醒 → 重放复现卡死 | **通过** | 重放 `status=stuck`、`consistency=MISMATCH`、退出码 1 |

回归结果（全部实际运行）：

| 测试 | 命令 | 结果 |
|---|---|---|
| 集成端到端 | `bash tools/cli/tests/integration_e2e.sh` | **44 PASS / 0 FAIL** |
| 全量 ctest | `ctest --test-dir build-integration --output-on-failure -j6` | **116/116 通过**，26.84 s |
| go2cpp-cli 自带验收 | `BIN=$PWD/build-integration/tools/cli/go2cpp-cli bash tools/cli/tests/run_all.sh` | **202 PASS / 0 FAIL**（含 500 组随机参数、200 次压测） |
| tools/migration 场景 | `bash tools/migration/tests/run_tests.sh build-migration` | **23/23 通过** |
| tools/replay 压测 | `bash tools/replay/scripts/stress_record_replay.sh build-replay 10` | **10/10 轮 MATCH** |
| tools/replay 故障注入 | `bash tools/replay/scripts/fault_injection.sh build-replay` | **3 个阶段通过（两种注入均判定 stuck）** |

---

## 1. 环境与构建

### 1.1 环境

```text
OS      : WSL2 / Ubuntu 24.04
CPU     : 12 vCPU
g++     : 13.3.0
cmake   : 3.28.3
```

### 1.2 构建命令（实际执行）

```bash
cd /UserData/CodexWorkSpace/Go2Cpp-work

# 主工程：Debug dump + CLI 同时开启
cmake -B build-integration \
      -DGO2CPP_DEBUG_DUMP=ON \
      -DGO2CPP_WITH_CLI=ON \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-integration -j12

# 迁移工具（独立工程）
cmake -S tools/migration -B build-migration
cmake --build build-migration -j12

# 重放工具（独立工程，链接主工程构建出的运行时库）
cmake -S tools/replay -B build-replay \
      -DGO2CPP_REPLAY=ON \
      -DGO2CPP_REPLAY_ROOT=$PWD \
      -DGO2CPP_REPLAY_LIB_DIR=$PWD/build-integration \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-replay -j12
```

说明：

- 原任务给出的主构建命令没有 `-DCMAKE_BUILD_TYPE=Release`。实际执行时追加了
  `Release`，目的是避免 `-O0` 下压测/超时测试失真；`ctest` 用例数仍是 116。
- **第一次执行上面的主构建命令时在 41% 失败**，根因与修复见 §5.1；修复后
  全部目标 100% 构建通过（`/tmp/integration-build.log` 末尾无错误）。
- 生成的 CLI：`build-integration/tools/cli/go2cpp-cli`。

### 1.3 一键复现

```bash
bash tools/cli/tests/integration_e2e.sh
```

脚本会：

1. 校验三套构建产物存在；
2. 编译并启动长跑 demo（`tools/cli/tests/debug_longrun_demo.cpp`）；
3. 跑 Debug、迁移、重放、CLI 四条端到端并逐项断言；
4. 证据写到 `/tmp/go2cpp-integration-evidence/`。

可选环境变量：`GO2CPP_BUILD_INTEGRATION`、`GO2CPP_BUILD_MIGRATION`、
`GO2CPP_BUILD_REPLAY`、`GO2CPP_INTEGRATION_EVIDENCE`。

---

## 2. Debug 端到端（真实进程 + 真实 Unix socket）

### 2.1 被测进程

`tools/cli/tests/debug_longrun_demo.cpp` 编译为 `/tmp/.../go2cpp_debug_demo`：

- 启动 2 P / 2 worker 的 `Scheduler`；
- 注册 `scheduler` 组件（`RegisterScheduler`，含 `include_fibers=true`）；
- 3 个 Fiber：2 个阻塞在无缓冲 `Channel<int>::Recv()`，1 个阻塞在
  `Channel<int>::Send()`；
- 分别注册为 `fiber`（第一个接收者）、`fiber-recv2`、`fiber-send`；
- 两个 channel 注册为 `channel`、`channel-send`；
- 最后调用 `DebugServer::Instance().Start()`，默认监听
  `/tmp/go2cpp-<pid>.sock`。

启动输出：

```text
READY pid=2040002 socket=/tmp/go2cpp-2040002.sock
```

### 2.2 `go2cpp-cli status --pid 2040002`

```text
$ ./build-integration/tools/cli/go2cpp-cli status --pid 2040002
目标进程: pid=2040002
协议: GO2CPP-DEBUG/1
DebugServer: true，uptime 7.4 ms，组件 8 个
路径: /tmp/go2cpp-2040002.sock
调度器: processors=2 workers=2 running=true
任务: 3 个（kWaiting=3）
runqueue: local=0 global=0 per_p=[0, 0]
调度指标: task_runs=3 completions=0 resume=407.5 us（avg 135.8 us/run） local_pops=0 steal=0
内存: (DebugServer 未提供内存统计；可用 `dump --sections runtime` 查看运行信息)
```

CLI 没有打印“有 N 处字段缺失”告警，说明 `STATUS` 的
`protocol/path/running/uptime_ns/components` 与 `JSON scheduler` 的
`processor_count/worker_count/tasks/runqueue/metrics` 全部命中。

### 2.3 `go2cpp-cli dump --pid 2040002 --sections scheduler,fiber,channel --format json`

完整输出在 `debug-dump.json`；下面是实际返回的 `channel`/`fiber` 两个组件全文，
`scheduler` 只摘关键字段：

```json
{
  "components": {
    "channel": {
      "capacity": 0,
      "change_waiters": 0,
      "closed": false,
      "generation": 0,
      "length": 0,
      "recv_waiters": 2,
      "schema": "go2cpp.dump.channel/1",
      "select_receivers": 0,
      "select_senders": 0,
      "send_waiters": 0,
      "version": 1
    },
    "fiber": {
      "active_parent_id": 0,
      "committed_begin": "0x7e1bb63eb000",
      "current_sp": "0x0",
      "growable": true,
      "has_stack": true,
      "id": 1,
      "machine_id": 2,
      "managed": true,
      "mapping_base": "0x7e1bb5bfd000",
      "mapping_top": "0x7e1bb63fd000",
      "nesting_depth": 1,
      "parent_id": 0,
      "processor_id": 1,
      "requested_bytes": 4096,
      "reserve_bytes": 8388608,
      "schema": "go2cpp.dump.fiber/1",
      "state": "kSuspended",
      "suspend_reason": "kPark",
      "task_id": 1,
      "usable_bytes": 73728,
      "version": 1
    },
    "scheduler": {
      "schema": "go2cpp.dump.scheduler/1",
      "version": 1,
      "running": true,
      "processor_count": 2,
      "worker_count": 2,
      "runqueue": {"global": 0, "local_total": 0},
      "metrics": {
        "task_runs": 3,
        "task_completions": 0,
        "fiber_resume_ns": 407499,
        "local_queue_pops": 0,
        "steal_pops": 0
      },
      "tasks": [
        {"id": 1, "state": "kWaiting", "started": true, "queued": false},
        {"id": 2, "state": "kWaiting", "started": true, "queued": false},
        {"id": 3, "state": "kWaiting", "started": true, "queued": false}
      ]
    }
  },
  "schema": "go2cpp-cli.dump.selection/1",
  "version": 1
}
```

字段含义与 `src/debug/debug_dump.cpp` 的实现一致：

- `channel.recv_waiters=2` 对应两个阻塞 `Recv` 的 Fiber；
- `fiber.state=kSuspended`、`suspend_reason=kPark`、`task_id=1`；
- `scheduler.tasks` 由 `RegisterScheduler` 的 `include_fibers=true` 填充，
  3 个任务全部 `kWaiting`（分别等在 channel 接收/发送上）。

### 2.4 独立 socket 探测（CLI 输出 vs DebugServer 原始响应）

`tools/cli/tests/debug_socket_probe.py` 绕过 CLI 直接连 socket，逐字段对比：

```text
$ python3 tools/cli/tests/debug_socket_probe.py 2040002 <evidence>/debug-dump.json
socket STATUS: {"components": "8", "debug_dump": "on", "path": "/tmp/go2cpp-2040002.sock",
 "pid": "2040002", "protocol": "GO2CPP-DEBUG/1", "requests": "8", "running": "true",
 "uptime_ns": "97273302"}
socket JSON scheduler: schema=go2cpp.dump.scheduler/1 processors=2 workers=2 tasks=3
socket JSON fiber: id=1 state=kSuspended reason=kPark task_id=1
socket JSON channel: capacity=0 recv_waiters=2 send_waiters=0
PROBE_OK: CLI dump 与 DebugServer 原始响应字段一致（socket=/tmp/go2cpp-2040002.sock）
```

断言内容：

- `STATUS`：`protocol=GO2CPP-DEBUG/1`、`debug_dump=on`、`running=true`、`path` 与 pid 一致；
- `JSON scheduler`：`processor_count/worker_count/running/runqueue/metrics.task_runs/tasks[]` 与 CLI 聚合结果一致；
- `JSON fiber`：`id/state/suspend_reason/task_id/nesting_depth/has_stack/growable/requested_bytes/usable_bytes/reserve_bytes/mapping_*/committed_begin` 全等；
- `JSON channel`：整个对象与 CLI 输出**完全相等**（无时间字段）；
- `JSON no-such-component` → `ERR unknown-component`。

### 2.5 错误路径

```text
$ ./build-integration/tools/cli/go2cpp-cli dump --pid 2040002 --sections io
go2cpp-cli: 目标进程未注册组件 'io'；DebugServer 的 LIST 命令可查看实际可用组件。
  程序中需要调用 go2cpp::debug::RegisterXxx("io", ...) 才会暴露该组件。
$ echo $?
3
```

`dump --sections all --format text` 返回 DebugServer 的聚合 dump：

```text
schema=go2cpp.dump.all/1
version=1
pid=2040002
component_count=8
unavailable=condition_variable,io,mutex,parking_condition

[component channel]
schema=go2cpp.dump.channel/1
...
```

---

## 3. 迁移 v2 端到端

### 3.1 `migrate dump`

```bash
./build-migration/migrate dump /tmp/go2cpp-integration-evidence/fiber.mig
```

实际输出：

```text
[A] 栈=[0x7d1367600000,0x7d1367800000) 初始 ctx=0x7d13677fffc8 入口=0x59d44b542e00
[A] fiber#1 启动，准备在通道 0x59d44b55f080 上接收（栈上 payload=65536 字节，校验和=8355840）
[A] Fiber 已挂起：ctx=0x7d13677eff08 waiter=0x7d13677eff70 channel.head=0x7d13677eff70
[A] 栈快照 65784 字节，符号 7 个，重定位 19 条，帧 5 个
[A] 指针审计 stack=5 symbol=14 code=8 heap=0 other=0 slots=8223
[A] 已写出快照：/tmp/go2cpp-integration-evidence/fiber.mig（65784 字节栈）
[A] dump 完成: /tmp/go2cpp-integration-evidence/fiber.mig
```

头部校验（实际读取）：

```text
magic=G2CMIG02 version=2 header_size=192 size=67784
```

### 3.2 `migrate verify`

```bash
./build-migration/migrate verify /tmp/go2cpp-integration-evidence/fiber.mig
```

尾部输出（完整内容为段表/符号/帧链清单）：

```text
  frame rbp_off=0x1eff40 ret_off=0x1eff48 sym_id=3
  frame rbp_off=0x1eff60 ret_off=0x1eff68 sym_id=3
  frame rbp_off=0x1effa0 ret_off=0x1effa8 sym_id=5
  frame rbp_off=0x1fffd0 ret_off=0x1fffd8 sym_id=6
  frame rbp_off=0x1ffff0 ret_off=0x1ffff8 sym_id=7
summary: 符号化协程快照：fiber#1，栈 65784 字节，7 个符号、19 条重定位、5 帧；指针审计 stack=5 symbol=14 code=8 heap=0
```

### 3.3 `migrate restore`（进程 B）

```bash
./build-migration/migrate restore /tmp/go2cpp-integration-evidence/fiber.mig
```

关键输出：

```text
[B] 旧栈 0x7d1367600000 → 新栈 0x7f0c3f200000；入口 BuiltinDemoBody+0 → 0x5b8d2f1fbe00
[B] 发送第 1 条消息 4242
[B] fiber#1 继续执行，收到消息 #1 = 4242
[B] fiber#1 栈上 payload 校验通过（8355840）
[B] 第 1 条消息已送达，Fiber 再次挂起于 state=2（2=Parked）
[B] 发送第 2 条消息 7
[B] fiber#1 继续执行，收到消息 #2 = 7，主体结束
[B] Fiber 已完成，result=7（期望 7）
[B] restore 完成: 消息 4242/7 已送达，Fiber 跨进程继续执行并正常结束
```

`旧栈 → 新栈`、`入口 → 新地址` 均随 ASLR 变化，证明 v2 的符号化恢复不依赖固定地址。

### 3.4 CLI 走真实迁移后端

```bash
GO2CPP_MIGRATE_TOOL=$PWD/build-migration/migrate \
  ./build-integration/tools/cli/go2cpp-cli migrate restore /tmp/.../fiber.mig
```

CLI 输出与上面一致，尾部：

```text
[B] Fiber 已完成，result=7（期望 7）
[B] restore 完成: 消息 4242/7 已送达，Fiber 跨进程继续执行并正常结束
```

`migrate dump`（CLI）：

```text
$ ./build-integration/tools/cli/go2cpp-cli migrate dump --pid $$ --gid 0 --out /tmp/.../cli-fiber.mig
go2cpp-cli: 注意：当前迁移后端（tools/migration demo）不支持按 pid/gid 抓取运行中的协程；
--pid=... --gid=0 通过 MIGRATION_PID/MIGRATION_GID 传递，待正式迁移接口对齐。
[A] dump 完成: /tmp/go2cpp-integration-evidence/cli-fiber.mig
迁移快照: /tmp/go2cpp-integration-evidence/cli-fiber.mig（66.2 KiB，迁移快照（G2CMIG02，版本 2））
```

### 3.5 版本/魔数负路径（CLI 修复后的行为）

v1 魔数：

```text
$ go2cpp-cli migrate restore /tmp/.../legacy-v1.mig
go2cpp-cli: 迁移文件格式不匹配：'/tmp/.../legacy-v1.mig'
  期望迁移快照（魔数 G2CMIG02，由 tools/migration v2 生成）；实际：无法识别的文件头：47 32 43 4d 49 47 30 31 ...
$ echo $?
4
```

`G2CMIG02` + `version=3`（魔数对、版本错）：

```text
$ go2cpp-cli migrate restore /tmp/.../version3.mig
go2cpp-cli: 迁移快照版本不支持：'/tmp/.../version3.mig' 的 version=3；
tools/migration v2 只接受 version=2（魔数 G2CMIG02）。
$ echo $?
4
```

---

## 4. 重放端到端

### 4.1 录制（3 producer + 1 consumer，96 次 rendezvous）

```bash
./build-replay/go2cpp_replay_record /tmp/go2cpp-integration-evidence/fanin.g2cr
```

```text
[record] path=/tmp/go2cpp-integration-evidence/fanin.g2cr
[record] file_bytes=20581 events=394
[record] consumer received=96 checksum=4129256551941548939
```

### 4.2 重放（逐位一致）

```bash
./build-replay/go2cpp_replay_replay /tmp/go2cpp-integration-evidence/fanin.g2cr
```

```text
[replay] path=/tmp/go2cpp-integration-evidence/fanin.g2cr
  status=ok complete=true checksum_ok=true file_size=20581 events=394 file_crc=3147636121
  demo="fanin-3x32"
  events: sends=96 recvs=96 parks=96 wakes=96 blocked=96 closes=0 selects=0
  messages=96 checksum=4129256551941548939 replayed_checksum=4129256551941548939
  replay: executed=true
  gid 0: sends=32 recvs=0 parks=0 wakes=0 exited=true
  gid 1: sends=32 recvs=0 parks=0 wakes=0 exited=true
  gid 2: sends=32 recvs=0 parks=0 wakes=0 exited=true
  gid 3: sends=0 recvs=96 parks=0 wakes=0 exited=true
[replay] consumer received=96 checksum=4129256551941548939
[replay] recorded: messages=96 checksum=4129256551941548939
[replay] replayed: checksum=4129256551941548939
[replay] consistency=MATCH
```

实际重复 10 轮（每轮真实调度交错不同）：

```text
[stress] round 1: MATCH events=394
...
[stress] round 10: MATCH events=394
[stress] all 10 rounds matched
```

### 4.3 CLI `replay play`

```bash
GO2CPP_REPLAY_TOOL=$PWD/build-replay/replay \
  ./build-integration/tools/cli/go2cpp-cli replay play /tmp/.../fanin.g2cr
```

CLI 把后端 JSON 报告原样输出。关键字段：

```json
{"schema": "go2cpp.replay.report/1", "status": "ok", "complete": true,
 "checksum_ok": true, "replay_executed": false, "file_size": 20581}
{"file_events": 394, "sends": 96, "recvs": 96, "parks": 96, "wakes": 96,
 "messages": 96, "message_checksum": 4129256551941548939,
 "replayed_message_checksum": 0}
```

> `replay_executed=false` 是预期行为：CLI 的 `replay play` 调用的是
> `tools/replay/replay`（离线静态重放/校验工具），不是执行 Fiber 的
> `go2cpp_replay_replay`。完整执行链路由 §4.2 覆盖。

### 4.4 丢唤醒复现

```bash
GO2CPP_REPLAY_DROP_WAKE=17 GO2CPP_REPLAY_WATCHDOG_SEC=1 \
  ./build-replay/go2cpp_replay_record /tmp/.../stuck.g2cr
./build-replay/go2cpp_replay_replay /tmp/.../stuck.g2cr
```

重放端实际输出（尾部）：

```text
  error: recording ends while gid 1 still has channel operations to perform
[replay] consumer received=15 checksum=15300253589118617522
[replay] recorded: messages=15 checksum=13218054778585603229
[replay] replayed: checksum=15300253589118617522
[replay] consistency=MISMATCH
[replay] status=stuck error=recording ends while gid 1 still has channel operations to perform
$ echo $?
1
```

`tools/replay/scripts/fault_injection.sh build-replay` 的两种注入（录制端丢唤醒、
正常记录 + 重放端丢唤醒）也都返回 1 并被判定 stuck。

---

## 5. 发现的问题与修复

### 5.1 [P0] `GO2CPP_DEBUG_DUMP=ON` 全量构建失败

**现象**（第一次构建，41%）

```text
/usr/bin/ld: libgo2cpp_scheduler.so.0.1.0: undefined reference to
  `go2cpp::debug::EventRing::Record(go2cpp::debug::SchedEventCode, unsigned long, unsigned long)'
collect2: error: ld returned 1 exit status
```

受影响的是只链接单个模块的测试目标（如 `go2cpp_config_registry_macro_test`、
`go2cpp_so_manifest_test`）。

**根因**

- `GO2CPP_DEBUG_DUMP=ON` 时 `src/concurrency/scheduler.cpp` 与
  `src/network/io.cpp` 直接调用 `EventRing::Record`；
- 但 `EventRing` 的实现 `src/debug/debug_event_ring.cpp` 被编进
  `go2cpp_debug`，而 `go2cpp_debug` 又依赖 `go2cpp_scheduler`/`go2cpp_io`；
- 让 scheduler/io 反向链接 `go2cpp_debug` 会形成目标依赖环，CMake 不允许；
- 结果：只链接 `go2cpp_scheduler` 的目标拿不到 `EventRing::Record` 定义。

**修复**（只改构建接线，未改 `src/**`）

`cmake/Go2CppModules.cmake`：

1. 新增零依赖库 `go2cpp_debug_event`，只编译
   `src/debug/debug_event_ring.cpp`；
2. `go2cpp_debug` 不再包含该源文件，改为 `PUBLIC` 链接 `go2cpp_debug_event`；
3. 仅当 `GO2CPP_DEBUG_DUMP=ON` 时：
   ```cmake
   target_link_libraries(go2cpp_scheduler PUBLIC go2cpp_debug_event)
   target_link_libraries(go2cpp_io PUBLIC go2cpp_debug_event)
   ```
4. `go2cpp_debug_event` 加入 `GO2CPP_MODULE_TARGETS`（继承 include/PIC/Threads/安装/导出）。

依赖方向变为：

```text
scheduler/io ──> debug_event ──> libc
debug       ──> scheduler/fiber/sync/io + debug_event
```

**验证**：主构建 100% 通过；`ctest` 116/116 通过；`GO2CPP_DEBUG_DUMP=OFF`
的独立构建（`/tmp/go2cpp-off-build`）也通过（见 §6）。

### 5.2 [P1] CLI 仍只认 `G2CMIG01`，与迁移 v2 的 `G2CMIG02` 不一致

**现象**

- `tools/cli/formats.hpp` 的 `kMigrationMagic` 是 `G2CMIG01`；
- 所有提示/帮助/文档都写 `G2CMIG01`，且只校验魔数、不校验 `version`；
- 实际 `tools/migration` 已升级 v2：魔数 `G2CMIG02`、`version=2`、
  `header_size=192`；
- 因此 CLI 会把真实 v2 快照判成“未知格式”，`migrate restore` 完全不可用。

**修复**（`tools/cli/**` + CLI 文档）

- `formats.hpp`/`formats.cpp`：魔数改为 `G2CMIG02`，探测详情输出 `G2CMIG02`；
- `commands.cpp`：
  - 新增 `RequireMigrationV2(probe)`，在 `migrate dump` 后校验与
    `migrate restore` 前校验 `version == 2`，否则退出码 4；
  - 更新错误文案与 `debug` 支持格式列表；
- `help.cpp`：帮助文本改为 `G2CMIG02`/v2，删除“必须 `-no-pie`”的过时前提；
- `tests/make_binaries.py`：fixture 改为 192 字节 v2 头，并新增 v1 反例；
- `tests/run_all.sh`：断言改为 `G2CMIG02`/`version=2`，新增 CLI 拒绝 v1 的用例；
- `docs/cli.md`：同步更新协议、头部大小、PIE/ASLR 说明。

**验证**

- 真实 v2 快照：`migrate restore` / `migrate dump` 均成功（§3.4）；
- v1 魔数 → 退出码 4、提示 `G2CMIG02`；
- `G2CMIG02 + version=3` → 退出码 4、提示 `version=3`；
- CLI 自带验收 202/202 通过。

### 5.3 联调确认、但不算缺陷的差异

1. **CLI `replay play` 用的是离线 `replay`**，不是执行 Fiber 的
   `go2cpp_replay_replay`；因此报告里 `replay_executed=false`。完整重放由
   `go2cpp_replay_replay` 覆盖。
2. **`migrate dump --pid/--gid` 仍是 demo 能力**：后端只写自己进程 A 的快照，
   CLI 会在 stderr 明确提示并通过 `MIGRATION_PID`/`MIGRATION_GID` 透传，
   不假装抓取成功。
3. **`--sections fiber,channel` 只能按组件名各取一个**：DebugServer 的组件名
   必须唯一，若进程注册了多个 Fiber/Channel，需要分别起名（如本 demo 的
   `fiber-recv2`/`fiber-send`）再用 `--sections` 列出，或 `all` 聚合。
4. **`dump --sections all` 的输出是 DebugServer 的聚合 schema
   `go2cpp.dump.all/1`**，不是 CLI 选择性聚合的
   `go2cpp-cli.dump.selection/1`（两者都由 `RunDumpImpl` 的真实分支产生）。

---

## 6. 回归验证

### 6.1 全量 ctest（`GO2CPP_DEBUG_DUMP=ON` + CLI 构建）

```text
$ ctest --test-dir build-integration --output-on-failure -j6
...
116/116 Test #1: go2cpp_tests ........................   Passed   26.83 sec

100% tests passed, 0 tests failed out of 116

Total Test time (real) =  26.84 sec
```

### 6.2 `GO2CPP_DEBUG_DUMP=OFF` 构建（确认默认路径未破坏）

```bash
cmake -S . -B /tmp/go2cpp-off-build \
      -DGO2CPP_DEBUG_DUMP=OFF -DGO2CPP_WITH_CLI=OFF \
      -DGO2CPP_BUILD_EXAMPLES=OFF -DGO2CPP_BUILD_TESTS=ON \
      -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/go2cpp-off-build --target go2cpp_tests -j12
```

结果：构建成功（见 §7 限制：本轮没有在 OFF 构建上重跑完整 116 项 ctest）。

### 6.3 各工具自带测试

```text
go2cpp-cli run_all.sh        : PASS=202 FAIL=0
tools/migration run_tests.sh : 通过 23 项，失败 0 项
replay stress (10 轮)        : all 10 rounds matched
replay fault_injection.sh    : 演示通过（两种注入均 stuck）
```

---

## 7. 限制与未覆盖项

1. **未 commit**：本轮所有改动仍在工作树中；`docs/integration-test.md` 与
   三个测试文件为新增，`cmake/Go2CppModules.cmake` 为修改。
2. **OFF 构建只验证了编译**：`/tmp/go2cpp-off-build` 构建了 `go2cpp_tests`，
   没有在 OFF 构建上重跑 116 项 ctest；ON 构建的 116/116 已完整通过。
3. **迁移 demo 的 `--pid/--gid` 能力缺口未修**：需要 DebugServer 导出运行中
   Fiber 栈/上下文的正式接口，超出本次“联调必需小修”的边界。
4. **Debug demo 固定 3 个阻塞 Fiber**：只覆盖 `Recv` 等待与 `Send` 等待，
   没有覆盖 Select/Timer/IO 等待下的 dump；这些组件由 Debug 模块自带单测覆盖，
   不在本次跨进程联调范围。
5. **重放压测规模**：10 轮 × 每轮 394 条事件，不是长稳压测；96 rendezvous
   的逐位一致与丢唤醒复现是本次验收目标。
6. **CLI `dump --sections` 的组件模型**：见 §5.3 第 3 条；这是协议约束，
   本次未改。

---

## 8. 变更文件清单（均未 commit）

本轮联调新增/修改：

| 文件 | 类型 | 说明 |
|---|---|---|
| `cmake/Go2CppModules.cmake` | 修改 | 拆分 `go2cpp_debug_event`，修复 ON 构建链接失败 |
| `tools/cli/formats.hpp` / `formats.cpp` | 修改 | 迁移魔数对齐 `G2CMIG02` |
| `tools/cli/commands.cpp` | 修改 | 增加 `version=2` 校验、更新文案 |
| `tools/cli/help.cpp` | 修改 | 帮助文本对齐 v2 |
| `tools/cli/tests/make_binaries.py` | 修改 | v2 fixture + v1 反例 |
| `tools/cli/tests/run_all.sh` | 修改 | 断言更新 + v1 拒绝用例 |
| `docs/cli.md` | 修改 | 协议/前提/测试清单同步 v2 |
| `codex.md` | 修改 | 追加第 20 节联调记录 |
| `tools/cli/tests/integration_e2e.sh` | 新增 | 四线联调脚本（44 断言） |
| `tools/cli/tests/debug_longrun_demo.cpp` | 新增 | Debug 联调用长跑进程 |
| `tools/cli/tests/debug_socket_probe.py` | 新增 | 直连 socket 字段对比探针 |
| `docs/integration-test.md` | 新增 | 本文 |

未修改 `src/**` 的任何核心实现。
