> 已合并至 docs/modules/tools.md（模块文档体系整理，2026-10-26）；本文保留作历史细节。

# go2cpp-cli：统一命令行入口

> 状态：v0.1.0，已与同期交付的三个模块**完成协议对齐**（2026-10-20 联调版，迁移对齐 v3 `G2CMIG03`，兼容读 v2）：
>
> - DebugServer：`GO2CPP-DEBUG/1` 文本协议（`include/go2cpp/debug/debug_server.hpp`）；
> - 重放：二进制 `G2CRPLY` 记录 + `replay` 工具（`tools/replay`）；
> - 迁移：二进制 `G2CMIG03`（v3：safepoint id + 句柄 + 签名）快照 + `migrate` 工具（`tools/migration`）。
>
> 仍存在两处**后端能力缺口**（不是 CLI 缺陷），见第 5 节「待对齐」：
> 通用 `GO2CPP_REPLAY` 记录注入、`migrate` 按 pid/gid 抓取运行中协程。
>
> 适用范围：Linux（POSIX）。CLI 只依赖 libc 与项目自带的 nlohmann/json，
> **不链接任何 go2cpp 运行时库**。

---

## 1. 定位

`go2cpp-cli` 是 Go2Cpp 新能力（Debug dump、协程迁移、事件重放）的统一命令入口。
它本身**不实现**这些能力，只负责：

1. 解析命令行参数并给出稳定的退出码；
2. 通过 Unix socket 与目标进程的 DebugServer 通信（dump / status）；
3. 以约定的环境变量启动程序（replay record），或以约定的参数调用外部工具
   （replay play / migrate dump / migrate restore）；
4. 离线分析 dump / trace / 二进制记录（debug）。

与 `include/go2cpp/debug/debug_cli.hpp` 的区别：后者是**进程内**的 dump 参数解析
（调用方把自己的 `Scheduler&` 交进去），不能跨进程；`go2cpp-cli` 是**跨进程**入口，
通过 DebugServer 读取另一个正在运行的进程。两者互补，不互相替代。

---

## 2. 构建

CLI 是可选 target，默认 **OFF**，不影响库、测试、示例的默认构建：

```bash
cmake -S . -B build-cli \
  -DGO2CPP_WITH_CLI=ON \
  -DGO2CPP_BUILD_TESTS=OFF \
  -DGO2CPP_BUILD_EXAMPLES=OFF
cmake --build build-cli --target go2cpp_cli -j"$(nproc)"

./build-cli/tools/cli/go2cpp-cli --help
```

默认配置（`GO2CPP_WITH_CLI=OFF`）下不存在 `go2cpp_cli` target，
`ctest` 的用例数量与内容不变（116 个）。

CMake 侧只做两件事：顶层 `CMakeLists.txt` 增加 `option(GO2CPP_WITH_CLI ...)` 与
`if (GO2CPP_WITH_CLI) add_subdirectory(tools/cli) endif()`；`tools/cli/CMakeLists.txt`
定义可执行文件。CLI 不进入 `install(EXPORT)`，不改变任何库目标。

### 2.1 配套工具（可选，但 dump/status 之外的功能需要）

```bash
# DebugServer 在目标程序内：用 GO2CPP_DEBUG_DUMP=ON 构建，并调用 Start()
cmake -S . -B build-debug -DGO2CPP_DEBUG_DUMP=ON
cmake --build build-debug -j

# 重放工具（独立工程，不并入主构建）
cmake -S tools/replay -B build-replay
cmake --build build-replay -j          # 产出 build-replay/replay

# 迁移工具（独立工程，必须 -no-pie）
cmake -S tools/migration -B build-migration
cmake --build build-migration -j       # 产出 build-migration/migrate
```

CLI 按以下顺序查找后端：`GO2CPP_*_TOOL` > 与 `go2cpp-cli` 同目录 > `PATH`。
重放候选名：`go2cpp-replay`、`go2cpp_replay`、`replay`；
迁移候选名：`go2cpp-migrate`、`go2cpp_migrate`、`migrate`。

---

## 3. 退出码与环境变量

### 3.1 退出码（稳定契约）

| 值 | 含义 | 典型场景 |
|---|---|---|
| 0 | 成功 | 正常输出 / 子进程退出码 0 |
| 1 | 一般失败 | 文件不存在、IO 错误、协议内容异常、程序无法启动 |
| 2 | 用法错误 | 参数缺失、未知选项、数值越界 |
| 3 | 后端不可用 | DebugServer 未启用、连接/响应超时、迁移/重放后端缺失 |
| 4 | 文件格式不匹配 | 空文件、魔数不符、JSON 语法错 |
| 5 | 目标进程返回失败 | 协议 `ERR <code>`（例如 `unknown-component`、`internal`） |

被信号杀死的子进程按 shell 惯例返回 `128 + signo`（SIGTERM → 143）。
`replay record` 的目标程序退出码原样透传。

### 3.2 环境变量

| 变量 | 作用 |
|---|---|
| `GO2CPP_DEBUG_SOCKET` | 覆盖 DebugServer socket 路径；默认 `/tmp/go2cpp-<pid>.sock`。设置后不再检查 `/proc/<pid>`（便于测试与自定义部署）。 |
| `GO2CPP_DOCTOR_SOCKET` | 覆盖 Doctor socket 路径；默认 `/tmp/go2cpp-<pid>-doctor.sock`。仅 `doctor` 子命令使用。 |
| `GO2CPP_CLI_TRACE=1` | 把协议请求/响应打到 stderr，用于排查协议问题。 |
| `GO2CPP_REPLAY_TOOL` | 指定 `replay play` 的后端（`tools/replay` 的 `replay`）。 |
| `GO2CPP_REPLAY_DROP_WAKE=N` | 丢弃第 N 次唤醒（故障注入）。CLI 不解析、原样继承给 `replay` 后端，用于复现 stuck。 |
| `GO2CPP_MIGRATE_TOOL` | 指定 `migrate` 子命令的后端（`tools/migration` 的 `migrate`）。 |

---

## 4. 命令参考

### 4.1 `dump`

从目标进程读取 dump。

```text
go2cpp-cli dump --pid <pid> [--out <file>] [--sections <list>]
                [--format json|text] [--timeout-ms <n>]
```

| 参数 | 必需 | 说明 |
|---|---|---|
| `--pid <PID>` | 是 | 目标进程 pid（1..2147483647）。 |
| `--out <FILE>` | 否 | 输出文件；默认 `-`（stdout）。同目录临时文件 + `rename`，失败不留半截文件。 |
| `--sections <LIST>` | 否 | `all`（默认）、`goroutines`、`scheduler`、`locks`，或稳定组件名（`scheduler`/`fiber`/`channel`/`parking_condition`/`mutex`/`condition_variable`/`io`/`events`/`runtime`）。 |
| `--format <FMT>` | 否 | `json`（默认）或 `text`。 |
| `--timeout-ms <N>` | 否 | 连接 + 发送 + 等待响应的总超时，默认 5000。 |

协议行为：

- `all`：发 `JSON ALL`（或 `DUMP ALL`），原样输出服务端聚合结果；
- 指定 sections：发 `JSON <component>` / `DUMP <component>`，多个组件合并为
  `{"schema":"go2cpp-cli.dump.selection/1","components":{...}}`（JSON）或
  `[component name]` 段（text）；
- 目标进程未注册某组件时，服务端返回 `ERR unknown-component`，CLI 退出 3 并说明
  需要调用哪个 `RegisterXxx`，**不会静默省略**。

`sections` 映射：

| 语义名 | 实际组件 |
|---|---|
| `goroutines` | `scheduler` + `fiber` |
| `scheduler` | `scheduler` |
| `locks` | `mutex` + `channel` + `parking_condition` |

示例：

```bash
go2cpp-cli dump --pid 1234
go2cpp-cli dump --pid 1234 --sections goroutines,locks --out /tmp/dump.json
go2cpp-cli dump --pid 1234 --sections runtime --format text
```

### 4.2 `status`

读取运行时统计。

```text
go2cpp-cli status --pid <pid> [--json] [--timeout-ms <n>]
```

| 参数 | 必需 | 说明 |
|---|---|---|
| `--pid <PID>` | 是 | 目标进程 pid。 |
| `--json` | 否 | 输出 `{status: {...}, scheduler: {...}|null}`。 |
| `--timeout-ms <N>` | 否 | 总超时，默认 5000。 |

CLI 发两条命令：`STATUS`（服务信息）与 `JSON scheduler`（调度统计）。
默认输出示例：

```text
目标进程: pid=1234
协议: GO2CPP-DEBUG/1
DebugServer: true，uptime 1.23 s，组件 3 个
路径: /tmp/go2cpp-1234.sock
调度器: processors=4 workers=6 running=true
任务: 3 个（kRunning=1, kWaiting=2）
runqueue: local=9 global=3 per_p=[3, 0, 2, 4]
调度指标: task_runs=100000 completions=99900 resume=880.0 ms（avg 8.8 us/run） local_pops=90000 steal=400
内存: (DebugServer 未提供内存统计；可用 `dump --sections runtime` 查看运行信息)
```

- 目标进程未注册 `scheduler` 组件时，仍显示 STATUS 信息，并明确提示需要调用
  `go2cpp::debug::RegisterScheduler(...)`；
- 字段缺失显示 `?` / `(响应缺少 ...)`，末尾统计缺失数量，**不会显示成 0**；
- DebugServer 当前不提供内存统计，CLI 如实标注。

### 4.3 `replay record`

以 `GO2CPP_REPLAY=ON` 启动程序并记录。

```text
go2cpp-cli replay record <exe> [args...] --out <file>
go2cpp-cli replay record --out <file> -- <exe> [args...]
```

- `--out <FILE>`：必需，记录文件路径（不能是 `-`）。可写在任意位置；
  目标文件已存在或为目录时**直接拒绝**（exit 2），不删除任何现有文件——
  避免误删目标程序或旧记录。
- `<exe>` 之后的参数原样透传，包括以 `-` 开头的参数；要把 `--out` 传给目标程序，
  写在 `--` 之后。
- 子进程环境：`GO2CPP_REPLAY=ON`、`GO2CPP_REPLAY_RECORD=<绝对路径>`。
- 退出码：与 `<exe>` 相同；被信号杀死返回 `128+signo`；`<exe>` 成功退出但
  没有生成记录文件时返回 3。
- 输出摘要：`程序退出码: N`、`记录文件: <path>（<size>，<N> 行 / 二进制）`。

```bash
go2cpp-cli replay record ./build/my_server --out /tmp/run.g2cr
go2cpp-cli replay record --out /tmp/run.g2cr -- ./build/my_server -v
```

> **能力缺口**：`tools/replay` 当前提供的是 demo 记录器
> `go2cpp_replay_record <file>`（固定场景，自行驱动 channel/fiber），
> 尚未实现「任意程序在 `GO2CPP_REPLAY=ON` 时自动记录」的注入。
> `replay record` 已按任务约定实现环境变量与生命周期，待后端补齐后即可产出
> `G2CRPLY` 记录。

### 4.4 `replay play`

离线重放并输出一致性结果。

```text
go2cpp-cli replay play <file>
```

流程：

1. 探测并校验记录文件：魔数 `G2CRPLY`（`tools/replay/include/replay/format.hpp`）；
2. 查找后端 `replay`（`GO2CPP_REPLAY_TOOL` > 同目录 > PATH）；
3. 找到则执行 `replay play <绝对路径>`，**捕获 stdout 解析 JSON 报告**的
   `replay_executed`；未真正重放时以 exit 3 拒绝；退出码透传
   （0=记录自洽，1=stuck/不一致，2=用法错误）；
4. 找不到退出 3，并给出 `tools/replay` 的构建命令。

故障注入：`GO2CPP_REPLAY_DROP_WAKE=N` 由 CLI 原样继承给后端（`fork`/`exec`
不清理环境），`replay` 端在 `RunPlay` 中解析并丢弃第 N 次唤醒，从而复现
`status=stuck`（退出码 1）。其它 `GO2CPP_REPLAY_*` 同样透传。

```bash
go2cpp-cli replay play /tmp/run.g2cr
GO2CPP_REPLAY_DROP_WAKE=20 go2cpp-cli replay play /tmp/run.g2cr   # 期望 stuck/exit 1
```

### 4.5 `migrate dump`

导出可迁移协程快照。

```text
go2cpp-cli migrate dump --pid <pid> --gid <gid> --out <file>
```

| 参数 | 必需 | 说明 |
|---|---|---|
| `--pid <PID>` | 是 | 目标进程 pid（CLI 校验 `/proc/<pid>` 存在）。 |
| `--gid <GID>` | 是 | 要导出的协程 id（≥0）。 |
| `--out <FILE>` | 是 | 快照文件（二进制，不能是 `-`）。 |

行为：调用 `migrate dump <file>`（`tools/migration`），并把 `MIGRATION_PID` /
`MIGRATION_GID` 传给后端；成功后校验输出文件魔数 `G2CMIG03` 与 `version=3`（兼容 `G2CMIG02`/v2）并打印摘要。

> **能力缺口**：`tools/migration` 当前是 demo —— `dump` 只写它自己进程 A 的快照，
> **不支持按 pid/gid 抓取运行中的协程**。CLI 会在 stderr 明确提示这一点，
> 而不是假装成功。正式迁移接口（DebugServer 导出 Fiber 栈与上下文）待补齐。

### 4.6 `migrate restore`

恢复快照。

```text
go2cpp-cli migrate restore <file>
```

流程：

1. 校验快照魔数 `G2CMIG03` 且 `version=3`；v2（`G2CMIG02`/`version=2`）仍可读（`tools/migration/include/migration/format.hpp`）；
2. 查找后端 `migrate`（`GO2CPP_MIGRATE_TOOL` > 同目录 > PATH）；
3. 找到则执行 `migrate restore <绝对路径>`，退出码透传；
4. 找不到退出 3，并给出构建命令。

注意：v3/v2 快照允许 PIE/ASLR 导致的运行间地址变化，但仍要求生成方与恢复方是
**同一可执行文件**（ELF build-id 校验）；Fiber 主体所在 SO 可以换版本。

```bash
go2cpp-cli migrate restore /tmp/fiber.mig
```

### 4.7 `debug`

离线分析（不需要目标进程）。

```text
go2cpp-cli debug <file> [--top <n>] [--json]
```

| 参数 | 说明 |
|---|---|
| `<file>` | JSON dump、`# go2cpp-trace` 文本、ini dump、`G2CRPLY` 记录、`G2CMIG03`/`G2CMIG02` 快照。 |
| `--top <N>` | 时间线显示前 N 条，默认 20；0 表示全部。 |
| `--json` | 以 JSON 输出统计。 |

- JSON dump：`schema`、顶层字段、`fibers`/`tasks`/`goroutines` 数量与状态分布、
  `suspend_reason` 分布、`events`/`trace` 数量与类型分布、时间线；
- trace 文本：记录数、INPUT/OUTPUT 计数、事件类型分布、时间线；
- ini：节数、键值行数、节名列表；
- `G2CRPLY`：格式版本、header_size、producer_id；事件级报告交给 `replay` 工具；
- `G2CMIG03` / `G2CMIG02`：版本、段数、fiber_id、栈大小；恢复/只读校验交给 `migrate`。

```bash
go2cpp-cli debug /tmp/dump.json
go2cpp-cli debug /tmp/run.trace --top 5
go2cpp-cli debug /tmp/run.g2cr --json
```

`--json` 输出结构（JSON dump）：

```json
{
  "file": "/tmp/dump.json",
  "size_bytes": 1234,
  "format": "json",
  "schema": "go2cpp.dump.scheduler/1",
  "top_level_keys": ["processor_count", "tasks"],
  "fiber_count": 3,
  "fiber_states": {"kRunning": 1, "kWaiting": 2},
  "suspend_reasons": {},
  "event_count": 2,
  "event_types": {"PARK": 1, "SPAWN": 1},
  "timeline": [ ... ]
}
```

---

### 4.8 `doctor`

在线诊断 + 受控唤醒。连接目标进程的 `DiagnosticAgent`（`G2CD/1` 二进制帧，
不是 `GO2CPP-DEBUG/1` 文本协议）。完整协议、白名单、QUIC 承载与限制见
[docs/diagnostic-channel.md](diagnostic-channel.md)。

```text
go2cpp-cli doctor [--pid <pid> | --socket <path>] [--transport unix|quic]
                  [--timeout-ms <n>] [--json] [--concurrency <n>] [command]
```

`--transport quic` 在当前版本明确返回「尚未实现」并以退出码 3 结束；协议与
stream 映射已定稿，picoquic 适配见 [diagnostic-channel.md §9](diagnostic-channel.md)。

| 命令 | 作用 |
|---|---|
| `status [json]` | 协程数、M/P、runqueue、锁等待、模块基址 + build-id |
| `dump [sections=a,b]` | 复用 DebugServer 的组件 dump；响应带 `duration_us` |
| `force-wake scope=<scope>` | `scheduler-idle` / `parking-condition` / `channel` / `all`；`mutex` 明确 refused |
| `deadlock-report [json]` | 「谁在等谁」的等待图 + deadlock 判定 |
| `mutex-unlock name=<name>` | 受控解锁：仅 owner 已终止时执行 |
| `inject-fiber fn=<name>` | 在诊断 Fiber 的 GMP 上下文里执行（内置 `traverse`） |
| `ping` / `stats` / `help` | 存活 / Doctor 自身统计 / 命令列表 |

示例：

```bash
go2cpp-cli doctor --pid 1234 status
go2cpp-cli doctor --socket /tmp/demo.sock deadlock-report
go2cpp-cli doctor --socket /tmp/demo.sock "force-wake scope=all"
go2cpp-cli doctor --socket /tmp/demo.sock --concurrency 10 --command status
```

退出码沿用第 3.1 节：连接不上返回 3；目标返回 error 帧或命令
`verdict=refused` 返回 5。`--concurrency` 只证明「传输并发 + 执行串行 +
按 stream_id 匹配」，响应里的 `doctor.concurrent_peak` 恒为 1。

前置条件：目标进程以 `-DGO2CPP_DIAGNOSTIC=ON` 构建，并在启动后调用
`go2cpp::debug::DiagnosticAgent::Instance().Start()`。

---

### 4.9 `graph`

把并发运行时的现场渲染成状态图：Mermaid（GitHub 直接渲染）、Graphviz DOT、
JSON（前端可渲染）。数据来自 Doctor（`G2CD/1`）与 DebugServer（`GO2CPP-DEBUG/1`
的 `VIEW`），或离线的 `--replay` 文件；不链接任何运行时库，纯字符串生成。

```text
go2cpp-cli graph --pid <pid> --type <type> [--format mermaid|dot|json] [options]
go2cpp-cli graph --replay <file> --type <type> [--format ...] [options]
```

| 图（`--type`） | 内容 | 数据源 |
|---|---|---|
| `wait-for` | G → 资源 → 持有者；死锁环标红 | Doctor `deadlock-report json=1` 的 edges（owner/waiters）+ DebugServer `VIEW history`（或 `VIEW primitives`）的等待者 gid |
| `resource` | 资源节点（Mutex/Channel/PC）+ 持有/等待边 + holds/waits/wakes | `VIEW resources` + 每个资源的 `VIEW history <id>` |
| `fiber-state` | 每个 G 的状态迁移（created→running→parked→ready→completed） | `VIEW sched` 的 flow（事件环）+ `VIEW gmp` 的当前状态快照 |
| `gmp` | P/M/G 拓扑 + 全局/本地 runqueue + blocked G | `VIEW gmp`；缺失时退回 Doctor `status` 的 machines/processors/runqueue |
| `sequence` | 资源操作时序（Mermaid `sequenceDiagram`） | 每个资源的 `VIEW history`；窗口被冲刷时退回 `primitives` 的 Mutex/Channel 事件 |

| 参数 | 说明 |
|---|---|
| `--pid <PID>` | 在线模式：目标进程 pid（与 `--replay` 二选一）。 |
| `--replay <FILE>` | 离线模式：JSON dump / `VIEW` 输出 / `# go2cpp-trace`。G2CRPLY 二进制请先用 `replay` 工具导出。 |
| `--type <TYPE>` | 必填：`wait-for` / `resource` / `fiber-state` / `gmp` / `sequence`。 |
| `--format <FMT>` | `mermaid`（默认）/ `dot` / `json`。 |
| `--out <FILE>` | 写入文件；默认 `-`（stdout），临时文件 + rename。 |
| `--debug-socket <PATH>` | 覆盖 DebugServer socket（默认 `/tmp/go2cpp-<pid>.sock`）。 |
| `--doctor-socket <PATH>` | 覆盖 Doctor socket（默认 `/tmp/go2cpp-<pid>-doctor.sock`）。 |
| `--timeout-ms <N>` | 单次请求超时，默认 5000。 |
| `--max-g <N>` | 最多显示多少个 G，默认 16。 |
| `--max-resources <N>` | 最多显示多少个资源，默认 48。 |
| `--max-events <N>` | sequence 最多多少条消息，默认 400。 |

示例：

```bash
go2cpp-cli graph --pid 1234 --type wait-for --format mermaid
go2cpp-cli graph --pid 1234 --type resource --format dot > /tmp/res.dot
go2cpp-cli graph --pid 1234 --type fiber-state --format mermaid
go2cpp-cli graph --pid 1234 --type gmp --format dot
go2cpp-cli graph --replay /tmp/history.json --type sequence --format mermaid
go2cpp-cli graph --pid 1234 --type wait-for --format json --out /tmp/wf.json
```

**死锁环**：在 G/资源二部图上做有界 DFS，找到的简单环同时写入
`deadlock_cycles`，并标记环上的节点与边。Mermaid 用
`classDef deadlock` + `linkStyle ... stroke:#c00000`，DOT 用
`color="#c00000" penwidth=2`，JSON 用 `"deadlock": true`。

**输出结构**（JSON）：

```json
{
  "schema": "go2cpp.graph.wait-for/1",
  "version": 1,
  "type": "wait-for",
  "title": "wait-for @ pid 1234",
  "ok": true,
  "notes": ["事件窗口已截断（window.truncated=true）：等待者可能不完整"],
  "source": {"pid": 1234, "sources": {"debugserver": true, "doctor": true}},
  "deadlock_cycles": [["res-a", "G11", "res-b", "G12", "res-a"]],
  "nodes": [{"id": "res-a", "label": "res-a\nkMutex#1\nMutex\nowner=G11", "kind": "resource", "deadlock": true}],
  "edges": [{"from": "res-a", "to": "G11", "label": "held", "kind": "held", "deadlock": true}]
}
```

sequence 额外有 `sequence.participants[]` 与 `sequence.messages[]`（含
`count`：连续重复通知折叠后的次数）；DOT 版本把时序展开为按时间连接的
事件节点（`m0 → m1 → ...`）。

**降级契约**（不是静默省略）：

- Doctor 不可用：owner 改由 `VIEW history` 的事件推导，notes 记录缺口；
- DebugServer 不可用：只画 Doctor 给出的「资源 → owner」与 waiters 计数，
  不伪造等待者 gid；
- 两者都不可用：输出只含 note 节点的图，**退出码 3**（提示需要
  `-DGO2CPP_DEBUG_DUMP=ON` / `-DGO2CPP_DIAGNOSTIC=ON`）；
- 部分降级（至少一个数据源可用）：退出码 0，notes 写明来源与缺口。

**已知限制**：

- 事件环按线程有界。死锁现场若在建立后较久才采集，park/mutex 事件可能已被
  高频 timer/PC 事件冲刷；此时 `VIEW history` 为空，图只能展示 Doctor 的
  owner/waiters 计数。`window.truncated` 会写入 notes，绝不把「窗口内没看到」
  当成「不存在」。
- Doctor 的注册名与 DebugServer `resource_id` 没有地址字段可直接对应：
  实现按 `owner_gid` 匹配；对不上的注册名以独立节点展示，不猜测顺序映射。
- `--replay` 只接受 JSON 与 `# go2cpp-trace`；`G2CRPLY` 二进制需要先用
  `replay`/`debug` 工具导出事件。

---

## 5. 与三个模块的对接点

### 5.1 DebugServer（`6192f825`）— 已对齐 `GO2CPP-DEBUG/1`

**传输**：`AF_UNIX` / `SOCK_STREAM`，默认 `/tmp/go2cpp-<pid>.sock`，
服务端把 socket 设为 `0600`；可用 `GO2CPP_DEBUG_SOCKET` 覆盖。

**请求**：一行 UTF-8 命令，`\n` 结尾（服务端上限 4096 字节）：

```text
PING | LIST | STATUS | DUMP <component>|ALL | TEXT ... | JSON ... | EVENTS [limit] | HELP
```

**响应**：

```text
GO2CPP-DEBUG/1 OK <payload_bytes>\n<payload>
GO2CPP-DEBUG/1 ERR <code> <message>\n
```

`payload_bytes` 是 payload 的精确字节数。错误码固定：
`bad-command` / `unknown-component` / `disabled` / `internal`。

**CLI 实际使用的命令**：

| 子命令 | 命令 |
|---|---|
| `dump`（默认） | `JSON ALL` |
| `dump --format text` | `DUMP ALL` |
| `dump --sections X` | `JSON X` / `DUMP X`（逐组件） |
| `status` | `STATUS` + `JSON scheduler` |

**连接模型**：服务端一个连接只处理一条命令（读一行、响应、关闭），
CLI 每次 `Call` 新建连接；`status` 会开两个连接。

**status 字段来源**（scheduler JSON，schema 见 `debug_dump.hpp`）：
`processor_count`、`worker_count`、`running`、`tasks[]`、`runqueue.local_total`、
`runqueue.global`、`processors[].queued`、`metrics.{task_runs,task_completions,
fiber_resume_ns,local_queue_pops,steal_pops}`。

**待对齐/已知缺口**：

1. DebugServer 不提供内存统计（CLI 如实标注，不显示 0）；
2. 调度延迟只有 `metrics.fiber_resume_ns` 累计值，CLI 用「总量 / task_runs」算平均，
   没有 p50/p95/p99 —— 若后端增加直方图，CLI 增加对应渲染即可；
3. 若协议新增字段或改动错误码，只需改 `tools/cli/transport.cpp`（协议）与
   `tools/cli/commands.cpp` 的两处请求构造/字段读取。

**验证方式**：`tools/cli/tests/fake_debug_server.py` 实现同一协议，覆盖正常响应、
`ERR`、协议头不匹配、payload 截断、超时、组件未注册、字段缺失等路径。

### 5.2 可迁移协程（`b0888d42`）— 已对齐文件格式，能力待补齐

- 快照格式：`G2CMIG03` + 224 字节头（version/header_size/段表/build-id/CRC/签名，
  v3 用 safepoint id + 句柄；`G2CMIG02` + 192 字节头的 v2 快照仍可读），
  见 `tools/migration/include/migration/format.hpp`；
- CLI 的 `migrate dump` 调用 `migrate dump <file>`，`migrate restore` 调用
  `migrate restore <file>`；
- `migrate dump` 会把 `MIGRATION_PID` / `MIGRATION_GID` 放进子进程环境。

**待对齐**：

1. `migrate dump` 目前不按 pid/gid 抓取运行中的协程（demo 只写自己的进程 A），
   需要后端提供「从目标进程导出指定 Fiber」的接口；届时 CLI 只需改
   `RunMigrateDumpImpl` 的参数传递；
2. 快照要求恢复方与生成方同一二进制（build-id + 签名），CLI 只做格式校验，
   跨二进制恢复由后端拒绝；
3. 若快照格式版本升级，`formats.cpp` 的魔数/版本读取与文档需同步。

### 5.3 重放服务（`2c3fb6f2`）— 已对齐文件格式，记录注入待补齐

- 记录格式：`G2CRPLY` + 32 字节头，记录 40 字节头 + 变长 payload + CRC32，
  `kEnd` 收尾，见 `tools/replay/include/replay/format.hpp`；
- CLI 的 `replay play` 调用 `replay play <file>`（stdout 输出 JSON 报告，
  exit 0/1/2）；
- `replay record` 已实现环境变量约定（`GO2CPP_REPLAY=ON` /
  `GO2CPP_REPLAY_RECORD`）与子进程生命周期。

**待对齐**：

1. 通用「任意程序在 `GO2CPP_REPLAY=ON` 时记录」尚未实现：当前只有 demo 记录器
   `go2cpp_replay_record`（固定 3 producer + 1 consumer 场景）。CLI 在未生成记录
   文件时会明确提示这一点；
2. 记录端是否还需要 seed / 输出目录 / 采样参数等环境变量，待后端定稿；
3. `replay` 的一致性报告是 JSON，CLI 原样透传；若需要结构化摘要（例如提取
   `status` 字段并入 CLI 输出），再增加解析。

---

## 6. 测试

验收脚本在 `tools/cli/tests/`（不注册到 ctest，不影响 116 个既有用例）：

```bash
# 先构建 CLI（见第 2 节），然后：
BIN=./build-cli/tools/cli/go2cpp-cli bash tools/cli/tests/run_all.sh
```

覆盖内容（断言数以 `run_all.sh` 的 PASS 汇总为准）：

- 全部子命令 `--help`、主帮助、`help <cmd>`、版本、未知命令；
- 参数错误（缺失/未知/重复/越界/多余位置参数/`--out -`）；
- DebugServer 未启用、进程不存在、socket 覆盖；
- 假 DebugServer（真实 `GO2CPP-DEBUG/1` 协议）：`STATUS`、`JSON scheduler`、
  `JSON ALL`、`DUMP ALL`、逐组件请求、未注册组件、`ERR`、协议头不匹配、
  payload 截断、响应超时、字段缺失降级；
- `replay record` 的环境变量、参数透传、退出码透传、信号、旧文件替换、
  二进制记录、未生成记录；
- `replay play`：无后端、后端参数与退出码、文件不存在、空文件、非 `G2CRPLY` 格式；
- `migrate dump/restore`：后端成功与退出码、pid 不存在、`--out -`、无后端、
  非迁移格式、v1 旧魔数、不支持的版本、空文件；
- `debug`：JSON / trace / ini / `G2CRPLY` / `G2CMIG03` / `G2CMIG02` / 未知格式 / `--json`；
- 随机参数 500 组（退出码必须在 0..5，无超时）；
- 压测 200 次启动与 200 次 `status` 请求的 P50/P95/P99；
- `graph`（`tools/cli/tests/run_graph.sh`，由 `run_all.sh` 调用，91 项断言）：
  五种图 × Mermaid/DOT/JSON、wait-for 环与死锁高亮、DOT `dot -Tsvg` 渲染、
  `--replay`（sched/history/gmp/resources/trace）、二进制记录拒绝、
  仅 Doctor / 仅 DebugServer / 无数据源三种降级、参数错误与 `--out`。

测试依赖：`bash`、`python3`（假 server 与二进制 fixture，不进入构建依赖）。
`run_graph.sh` 使用独立的假数据源 `graph_fake_server.py`（同时实现
`GO2CPP-DEBUG/1` 与 `G2CD/1`），不依赖真实进程；真实进程联调见
`tests/scenarios/deadlock_routes`。

---

## 7. 已知限制与未覆盖项

1. **仅 Linux/POSIX**：`transport.cpp` 使用 Unix socket 与 `poll`，非 POSIX 平台
   编译期 `#error`；`SelfDirectory()` 依赖 `/proc/self/exe`。
2. **`dump`/`status` 未与真实 DebugServer 进程联调**：协议按头文件实现，用同协议的假
   server 做了端到端验证；真实联调需要目标程序调用 `DebugServer::Start()`。
   （`graph` 已在 `tests/scenarios/deadlock_routes` 的真实进程上联调，见 §4.9/§6。）
3. **迁移/重放走外部可执行**：CLI 不链接 `tools/migration`、`tools/replay` 的库。
   若对方改为只提供库，需要新增链接或包装可执行文件。
4. **通用记录注入缺失**（见 5.3），`replay record` 目前只能验证生命周期与参数约定。
5. **`migrate dump` 的 pid/gid 尚未真正生效**（见 5.2）。
6. **JSON 上限 64 MiB、payload 上限 16 MiB**；超过即报错，不做截断分析。
7. **未做权限模型**：只依赖 socket 权限（服务端 0600），不检查属主。
8. **`debug` 的字段统计是启发式的**：`fibers`/`tasks`/`goroutines`、`events`/`trace`
   按常见命名探测；若 dump schema 定稿后可改为严格字段。
9. **`graph` 依赖事件窗口**：事件环按线程有界，死锁建立较久后 park/mutex 事件
   可能已被冲刷；此时 wait-for 只能显示 Doctor 的 owner/waiters 计数，sequence
   退回 `primitives` 的 Mutex/Channel 事件。`window.truncated` 会写进 notes。
10. **`graph` 的 Doctor/DebugServer 名称映射**：两者之间没有地址字段，注册名
    （`res-a`）与 `resource_id`（`kMutex#1`）只能按 `owner_gid` 匹配；对不上的
    注册名以独立节点展示，不做顺序猜测。
11. **`graph --replay` 不支持 `G2CRPLY` 二进制**：格式解析属于 `tools/replay`，
    CLI 不链接它；需要先用 replay/debug 工具导出 JSON 事件。
12. **`graph` 的输出规模有上限**（默认 16 G / 48 资源 / 400 消息）：超大进程
    需要显式调大 `--max-*`，但 Mermaid 在数百节点后会难以阅读，建议按 G 或
    资源过滤后再渲染。
