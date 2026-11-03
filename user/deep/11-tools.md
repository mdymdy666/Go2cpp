# 开发者工具链与 Go 兼容工具层 · 深入学习

> 面向「要接手改 `tools/` 或 `include/go2cpp/tools/` 的人」。文中每个 `文件:行` 都用
> `grep -n` / `sed -n` 在 WSL 源码树 `/UserData/CodexWorkSpace/Go2cpp` 复核过；
> 拿不准的地方标「未核实」，不写「达标」结论。
>
> **先读这条**：本篇与 `user/docs/11-tools.md` 同名同号，但**不是同一主题**。
> 浅层那篇讲的是 Go 标准库风格的工具库（hash / encoding / compress / reflect / container /
> math / path，外加 `tools/strconv|strings`）；本篇讲的是**开发者工具链**（`tools/` 下的
> CLI / 迁移 / 回放 / 持久化 / 启动开销 / 基准）与**Go 兼容层**（`include/go2cpp/tools/`
> 下 13 个头）。两者只在 `strconv`、`strings` 两个头文件上重叠，详见 §1.3。

---

## 第一部分 · 定位

### 1.1 这个模块解决什么问题

六个子目录各解决一个「开发/运维侧」问题，彼此**没有运行时依赖**，只在构建期和
文件格式上共享 replay 的编码器：

| 子目录 | 解决什么问题 | 产物 | 是否独立 CMake 工程 |
|---|---|---|---|
| `tools/cli/` | 一条命令做自检：dump / status / replay / migrate / debug / doctor / graph | `go2cpp-cli`（`tools/cli/CMakeLists.txt:11`） | 否，接入主构建 |
| `tools/migration/` | 协程快照跨进程恢复（进程 A 挂起 → 进程 B 继续） | `migrate` / `migrate_alt` / `migrate_conflict`（`tools/migration/CMakeLists.txt:58/65/72`） | 是 |
| `tools/replay/` | 记录事件流 → 静态重演 / 单线程重放 / 分片 / 检查点 | `replay` CLI + demo（`tools/replay/CMakeLists.txt:43`） | 是 |
| `tools/persistence/` | 高吞吐事件落盘（buffered / mpsc / segmented 三引擎） | `go2cpp_persistence` 静态库（`tools/persistence/CMakeLists.txt:21`） | 是 |
| `tools/startup_cost/` | 池化资源在启动期与首次使用的时间/内存开销 | `startup_cost` 可执行文件 | 是（`build.sh` 直编） |
| `tools/bench/` | 固化台账 M16 的 Go scheduler 基线源码 | `go_sched_bench.go` | 否（`go build`） |

### 1.2 `include/go2cpp/tools/` 的 13 个头

这层的共同点是 **header-only、语义对齐 Go 标准库**，被 `runtime`、`storage`、`app`
和测试直接 include。它**不是**可执行工具，与根 `tools/` 是两套东西：

| 头文件 | 对齐的 Go 能力 | 已核实入口 |
|---|---|---|
| `bytes.hpp` | `bytes` 包 | `Equal` `bytes.hpp:83`、`Buffer` `bytes.hpp:279`、`Reader` `bytes.hpp:550` |
| `strings.hpp` | `strings` + `unicode/utf8` | 未逐条核实行号（浅层文档已覆盖语义） |
| `strconv.hpp` | `strconv` | `ParseInt` `strconv.hpp:339`、`Atoi` `strconv.hpp:382`、`Itoa` `strconv.hpp:561` |
| `strconv_core.hpp` | `strconv` 的底层数字转换 | 未核实具体行号 |
| `sort.hpp` | `sort` | `Sort` `sort.hpp:87`、`Search` `sort.hpp:245` |
| `time.hpp` | `time` | `Duration` `time.hpp:102`、`Sleep` `time.hpp:488` |
| `fmt.hpp` | `fmt` 子集 | 未核实具体行号 |
| `error.hpp` | `errors` | `Error` `error.hpp:91`、`Wrap` `error.hpp:321`、`Unwrap` `error.hpp:336` |
| `control_flow.hpp` | `defer` / `panic` / `recover` | `defer` `control_flow.hpp:98`、`Recover` `control_flow.hpp:336` |
| `event.hpp` | `EventBatch`/`SelectLoop` | 未核实具体行号 |
| `go.hpp` | `go` / `after` / `every` | `go` `go.hpp:148`、`after` `go.hpp:182`、`every` `go.hpp:195` |
| `json.hpp` | nlohmann/json 的深度栅栏封装 | `ParseJson` `json.hpp:307` |
| `streams.hpp` | `io` 流接口族 | `Reader` `streams.hpp:197`、`ReadAll` `streams.hpp:344`、`Copy` `streams.hpp:430` |

> `streams.hpp` 顶部注释明确解释了「Go 的 `io` 名字已被本项目占用，所以这层叫
> streams」——这是又一处**命名撞车**，接手时别按 Go 的 `io` 去搜。

### 1.3 命名撞车：为什么容易混

这是本篇最需要先讲清楚的事，三处撞车叠在一起：

1. **文档名撞车**：`user/docs/11-tools.md` 与 `user/deep/11-tools.md` 同号同名。
   浅层那篇第 3 行自述源码范围是 `include/go2cpp/{hash,encoding,compress,reflect,
   container,math,path}/` 与 `include/go2cpp/tools/{strconv,strings}.hpp`（`user/docs/11-tools.md:3`），
   全文 347 行，主题是「标准库补齐」；本篇主题是「开发者工具 + 兼容层」。
   只读标题会以为两篇是「简版/详版」，实际覆盖面只有两个头文件相交。
2. **目录名撞车**：根 `tools/` 放可执行工具，`include/go2cpp/tools/` 放头文件库。
   CMake 目标也分两套——前者产出 `go2cpp_cli`、`migration_core`、`go2cpp_persistence`、
   `go2cpp_replay`，后者通常以源码/接口库形式被 include（其独立 CMake 目标名**未核实**）。
3. **`migrate` 语义撞车**：CLI 的 `migrate` 是**协程/进程快照迁移**（魔数 `G2CMIG03`），
   与 `storage::Migration::Up/Down` 的**数据库 schema 迁移**完全无关。搜 `migration`
   会同时命中两套代码，看注释前先看路径。

后果：写交叉引用或提交信息时，必须带路径限定词（`tools/` 还是 `include/go2cpp/tools/`），
否则下一个接手的人会点进错误的文档。

---

## 第二部分 · 架构

### 2.1 文件/类清单

| 模块 | 关键文件 | 角色 |
|---|---|---|
| CLI 入口 | `tools/cli/main.cpp` | 全局开关 + 7 个子命令分发 + 异常→退出码 |
| CLI 参数 | `tools/cli/args.hpp/.cpp` | `OptionSpec` 表驱动解析、位置参数校验、整数越界检查 |
| CLI 命令 | `tools/cli/commands.cpp` | dump/status/replay/migrate/debug 的实现与后端定位 |
| CLI 图 | `tools/cli/graph.cpp` | 在线抓 DebugServer 视图，渲染依赖图 |
| CLI 自检 | `tools/cli/doctor.cpp` | 环境/构建/后端自检 |
| CLI 传输 | `tools/cli/transport.cpp` | AF_UNIX 非阻塞连接 + `GO2CPP-DEBUG/1` 协议 |
| CLI 格式 | `tools/cli/formats.cpp` | 文件魔数探测与分类 |
| 迁移驱动 | `tools/migration/migrate_main.cpp` | dump/restore/verify/selftest 四模式 |
| 迁移格式 | `tools/migration/include/migration/format.hpp` | v3 快照布局、段类型、签名结构 |
| 迁移运行时 | `tools/migration/src/runtime.cpp` | `DumpDemoToFile` / `RestoreDemoFromFile` / `VerifySnapshotFile` |
| 回放引擎 | `tools/replay/src/replay_engine.cpp` | 文件加载、静态重演、检查点、分片 |
| 回放格式 | `tools/replay/include/replay/format.hpp` | 32B 文件头 + 40B 记录头 + CRC + 事件枚举 |
| 回放时钟 | `tools/replay/include/replay/clock.hpp` | live 真睡 / replay 按记录推进 |
| 持久化写 | `tools/persistence/src/writer.cpp` | 三引擎写入、fsync 策略、manifest |
| 持久化读 | `tools/persistence/src/reader.cpp` | mmap + 逐条 CRC + 段范围筛选 |
| 持久化编码 | `tools/persistence/include/persistence/encoder.hpp` | 定长快路径，布局必须与 replay 一致 |
| 启动开销 | `tools/startup_cost/main.cpp` | 父子进程探针、rdtsc 校准、报告 |
| 基准 | `tools/bench/go_sched_bench.go` | 台账 M16 的可复现源码 |

### 2.2 入口点

| 入口 | 文件:行 | 谁调用 | 触发什么 | 返回什么 |
|---|---|---|---|---|
| `main` | `tools/cli/main.cpp:75` | shell / ctest | 收集 argv，进 try 块分发 | 进程退出码 |
| `help` / `-h` / `--help` | `tools/cli/main.cpp:95-101` | `main` | 主帮助或 `PrintCommandHelp` | `0` / 用法错误 |
| 7 路命令分发 | `tools/cli/main.cpp:106-126` | `main` | 调 `RunDump`/`RunStatus`/`RunReplay`/`RunMigrate`/`RunDebug`/`RunDoctor`/`RunGraph` | 子命令退出码 |
| `ParseArgs` | `tools/cli/args.cpp:46` | 各 `Run*Impl` | 逐 token 匹配 `OptionSpec`，`-h` 置位，收位置参数 | `ParsedArgs`；用法错误抛 `CliError` |
| `ParseIntOption` | `tools/cli/args.cpp:140` | 各 `Run*Impl` | 十进制解析 + `[min,max]` 越界拒绝 | `int` |
| `RunDumpImpl` | `tools/cli/commands.cpp:650` | `RunDump` | 定位进程/探针文件，必要时连 DebugServer | JSON 文本或统计 |
| `RunStatusImpl` | `tools/cli/commands.cpp:958` | `RunStatus` | `STATUS` / `JSON scheduler` 查询 | 格式化状态 |
| `RunReplayRecord` | `tools/cli/commands.cpp:1084` | `RunReplay` | 通过后端录制 | 后端退出码 |
| `RunReplayPlay` | `tools/cli/commands.cpp:1231` | `RunReplay` | 通过后端回放 | 后端退出码 |
| `RunMigrateImpl` | `tools/cli/commands.cpp:1444` | `main` | 分派 dump/restore | 子命令退出码 |
| `RunMigrateDumpImpl` | `tools/cli/commands.cpp:1336` | `RunMigrateImpl` | 解析 `--pid/--gid/--out`，找 `migrate` 后端并 exec | 后端退出码；产物做格式校验 |
| `RunMigrateRestoreImpl` | `tools/cli/commands.cpp:1411` | `RunMigrateImpl` | 校验魔数/版本后 exec 后端 restore | 后端退出码 |
| `RunBackend` | `tools/cli/commands.cpp:449` | 各 `Run*Impl` | fork/exec 外部工具并注入环境变量 | 子进程退出码 |
| `FindBackend` | `tools/cli/commands.cpp:141` | 迁移/回放命令 | 按名字+环境变量在 PATH/自身目录找可执行文件 | 路径或空串 |
| `DebugClient::Call` | `tools/cli/transport.cpp:181` | `RunDumpImpl`(`:681`)、`RunStatusImpl`(`:976`)、`Graph`(`graph.cpp:663`) | 建 UDS、发一条命令、读头行+payload | `DebugReply` / 抛 `CliError` |
| `migrate main` | `tools/migration/migrate_main.cpp:83` | shell | 解析 mode（argv 或 `MIGRATION_MODE`） | 0/1/2 |
| `DumpDemoToFile` | `tools/migration/src/runtime.cpp:1157` | `main` 的 dump 分支 | 起 Fiber、挂起、序列化、签名、落盘 | bool + error |
| `RestoreDemoFromFile` | `tools/migration/src/runtime.cpp:1235` | `main` 的 restore 分支 | 校验、映射、恢复栈/寄存器、继续跑 | bool + error/trace |
| `VerifySnapshotFile` | `tools/migration/src/runtime.cpp:1255` | `main` 的 verify 分支 | 只校验格式/签名/符号/挂起点 | bool + 报告 |
| `replay main` | `tools/replay/replay.cpp:575` | shell | `info`/`play`/`step`/`split` 手写解析 | 进程退出码 |
| `RunPlay` | `tools/replay/replay.cpp:173` | `main` | 单线程完整重放（真调度器） | 状态码 |
| `RunSplit` | `tools/replay/replay.cpp:533` | `main` | 按事件数/虚拟毫秒切片 | 状态码 |
| `LoadFile` | `tools/replay/src/replay_engine.cpp:105` | `Analyze`/`RunPlay` | 全量读文件、解头、逐条解码、校验 kEnd CRC | `LoadedFile`（错误在结构体内） |
| `AnalyzeLoaded` | `tools/replay/src/replay_engine.cpp:521` | `Analyze` | 静态重演：时序、配对、校验和、卡死诊断 | `ReplayReport` |
| `EventWriter::Create` | `tools/persistence/include/persistence/writer.hpp:119` | 业务/bench | 按 `WriterOptions.engine` 造三种写端之一 | `unique_ptr<EventWriter>` |
| `BufferedWriter` | `tools/persistence/src/writer.cpp:251` | `Create`（`engine=kBuffered`） | 单缓冲 + 全局锁 + 同步 write | 写端对象 |
| `AsyncWriter` | `tools/persistence/src/writer.cpp:526` | `Create`（mpsc/segmented） | 每线程 batch → MPSC → 后台 write/fsync | 写端对象 |
| `WriteManifest` | `tools/persistence/src/writer.cpp:999` | AsyncWriter 收尾 | 写 `G2CSESSION 1` 文本 + 原子 rename | void（错误进 `m_error`） |
| `SessionReader::Scan` | `tools/persistence/src/reader.cpp:190` | `ScanAll`/范围读 | 逐段 mmap、逐条 CRC、seq/时间过滤 | `ReadStats` |

### 2.3 总架构图

```
                    ┌──────────────────────────────────────────────┐
   shell ──────────►│ tools/cli/main.cpp:75                        │
                    │  help/version → 7 子命令 → catch → 退出码     │
                    └───────┬───────────────┬──────────────────────┘
                            │               │
              args.cpp:46 ParseArgs     commands.cpp
                            │               ├─ RunDump/RunStatus ──► transport.cpp:181
                            │               │                         AF_UNIX + poll
                            │               │                         "GO2CPP-DEBUG/1 OK n\n"
                            │               │                              │
                            │               │                              ▼
                            │               │                        DebugServer（另一个进程）
                            │               ├─ RunReplay ──► 找 replay 后端 exec
                            │               └─ RunMigrate ─► 找 migrate 后端 exec
                            │                                      │
                            ▼                                      ▼
                  include/go2cpp/tools/13 个头           tools/migration（独立工程）
                  （header-only，被全项目 include）        G2CMIG03 快照
                                                          ▲
   tools/replay（独立工程）                                 │ 复用编码器
     format.hpp: 32B 头 + 40B 记录 + CRC             tools/persistence（独立工程）
     replay_engine.cpp:105 LoadFile                   writer.cpp:251/526
     replay_engine.cpp:521 AnalyzeLoaded               └─ EncodeEvent/EncodeFixed
     replay.cpp:575 CLI（info/play/step/split）              │
            ▲                                                ▼
            └──────────── tools/bench（M16 基线，Go 源码）
```

### 2.4 线程 / IO 边界

| 模块 | 线程模型 | IO 边界 | 关键约束 |
|---|---|---|---|
| CLI | 单线程 | 每个 DebugServer 调用**新建一条连接**，一连接一命令（`commands.cpp:21` 注释） | 非阻塞 + `poll` + 绝对 deadline；失败映射成 `kUsage/kUnavailable/kFailure` |
| replay 静态重演 | 单线程，纯内存扫描 | `fopen/fread` 全量读入 `std::vector` | 不执行用户代码；不做 wall-clock 等待 |
| replay 真重放 | 单线程调度器（`RunPlay`） | 读文件 → 驱动真实 `RECORD_FIBER_ENTRY` | 需要录制时的同一构建/入口 |
| persistence 写 | buffered：调用线程 + 全局锁；mpsc：每线程 batch + 单后台线程 | `write`/`fdatasync`/`sync_file_range` | `Finish()` 必须在所有生产者停止 `Append()` 后调用（`writer.hpp:22`） |
| persistence 读 | 单线程 | `mmap` 每个段，逐条解码 | 段按 manifest 范围跳过，不读无关段 |
| migration | 主控单线程；dump 在进程 A 挂起 Fiber | 普通文件读写 + 可选 `dlopen` demo SO | 生成方/恢复方须同一可执行文件（ELF build-id） |
| startup_cost | 父进程 fork/exec 子进程探针 | 读 `/proc/self/status`（VmRSS）、rdtsc | 结果与机器强相关 |

---

## 第三部分 · 关键实现

### 3.1 CLI：表驱动解析 + 手写分发

`main.cpp` 故意不引参数框架（要静态链接、零额外依赖）：

1. **收集**：`main.cpp:77-83` 把 `argv[1..]` 拷进 `std::vector<std::string>`，后续只处理这个 vector。
2. **全局开关**：空参数打印主帮助（`:88-90`）；`help/-h/--help` 可带命令名走 `PrintCommandHelp`（`:95-101`）；`--version` 直接返回（`:102-105`）。
3. **分发**：`:106-126` 是 7 路 if 链（dump/status/replay/migrate/debug/doctor/graph），每个分支把 `rest` 交给 `Run*`；没有 map/虚函数，加命令就是加一个 if。
4. **错误收敛**：`CliError` 带退出码，`:131-134` 打到 stderr 后原样返回；其它异常统一 `kFailure`（`:135-139`），子命令不用自己打印用法。
5. **子命令再解析**：`RunMigrateDumpImpl`（`commands.cpp:1336`）构造 `{--pid,--gid,--out}` 的 `OptionSpec` 表，`ParseArgs(args, specs, 0, 0)` 表示零位置参数；`--out` 缺失或为 `-` 时主动抛用法错（快照是二进制，不能写 stdout）。
6. **整数边界**：`ParseIntOption(..., 1, 2147483647)`（`args.cpp:140`）把非数字与越界都变成 `kUsage`，避免 0/负 pid 进 `/proc`。

解析器硬规则：重复选项报错、需要值的选项缺值报错、位置参数数量校验（`args.cpp:129-136`）、未知选项拒绝（`args.cpp:57` 抛的是 `kFailure` 而非 `kUsage`，按退出码分流时要注意）。

### 3.2 CLI → DebugServer：一连接一命令

`DebugClient::Call`（`transport.cpp:181`）是 CLI 唯一外部通信入口：

1. `socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC)`（`:187`；CLOEXEC 防 exec 后端时泄漏 fd）。
2. `fcntl(O_NONBLOCK)`（`:203`），后续读写全走 `poll` + deadline，不用 `SO_RCVTIMEO`。
3. `connect` 返回 `EINPROGRESS` 时 `poll(POLLOUT)` 再 `getsockopt(SO_ERROR)`（`:205-239`）；超时 `kUnavailable`，其它 `kFailure`。
4. 发 `command + "\n"`（`:241`），`MSG_NOSIGNAL` + EAGAIN 重 poll；命令不能含换行。
5. 读头行到 `'\n'`，上限 4096（`:56/277`）；必须以 `GO2CPP-DEBUG/1 ` 开头（`transport.hpp:64`、`transport.cpp:321`）；`ERR` → `DebugError`（`:334`），`OK <n>` 解析字节数（`:347`）。
6. 读 payload，上限 64MiB（`:55/360`）：先用头行后缓冲，再 `recv`；提前 EOF 报「期望 n 实际 m」。所有分支都 close fd。

一连接一命令换来实现简单：服务端读一行、回一帧、关连接；CLI 不用处理粘包。

### 3.3 migration：段表 + 符号化 + HMAC

`tools/migration` 是进程/协程迁移，不是数据库迁移。CLI 只做参数校验 + 找后端 + exec：

1. **CLI 侧**（`commands.cpp:1336`）：解析 `--pid/--gid/--out`，检查输出目录、`/proc/<pid>`（`:1364-1367`），`FindBackend` 找 `migrate` 或 `GO2CPP_MIGRATE_TOOL`，`RunBackend` 注入 `MIGRATION_PID/MIGRATION_GID`（`:1393`）。
2. **现实约束**：注释写明后端是 demo，「dump 只写它自己进程 A 的快照，不接受 pid/gid」（`:1383`），pid/gid 被忽略（`:1387-1388`）。`--pid` 抓不到目标进程。
3. **后端入口**（`migrate_main.cpp:83`）：mode 取 `argv[1]` 或 `MIGRATION_MODE`（`:88`），path 取 `argv[2]` 或 `MIGRATION_FILE`（`:93`）；dump `:104`、restore `:114`、verify `:124`、selftest `:70`。
4. **dump**（`runtime.cpp:1157`）：起 Fiber → 挂起点停住 → 收集栈/寄存器/句柄/符号 → 段 CRC + 整文件 CRC → HMAC-SHA256 → 落盘。
5. **restore**（`runtime.cpp:1235`）：校验格式/版本/签名/符号/挂起点 → 映射段 → 按 safepoint id 查锚点 → 重定位 → 恢复寄存器/栈 → 继续。
6. **verify**（`runtime.cpp:1255`）：只做校验不恢复，用于预检。
7. **格式**：magic `G2CMIG03`（`format.hpp:105`）、version 3（布局注释 `:35-36`）、段类型 1..13（`:143-155`）、48B 段入口（`:111`）、`SignatureState`（`:235`）；读端按 magic/version 拒绝（`:17` 注释）。
8. **关键设计**：挂起点用 safepoint id 而非指令偏移（`format.hpp:9`），SO 重编译仍可恢复；代价是编译器必须分配稳定 id。

### 3.4 replay：加载 → 静态重演 → 诊断

**第一层 LoadFile（`replay_engine.cpp:105`）**：

1. `fopen/fseek/ftell`（`:106-120`）。
2. 超 `kMaxFileSize=256MiB` 拒绝（`:28/129`）。
3. 一次 `fread` 进 `std::vector`（`:131-140`），全量驻留，无流式解码。
4. `DecodeFileHeader` 校验 magic/版本/头长（`:142`）。
5. 从 32B 头后循环 `DecodeEvent`（`:146`）；每条记录自带 CRC32（`format.cpp:321/370-372`），坏一条整体失败。
6. **gid 防御**：非资源事件 `gid>65536` 拒绝（`:154-166`）——稠密索引会 `bad_alloc`→abort。
7. **kEnd**：置 `complete`，用 `sched_type` 存滚动 CRC 并与 `Crc32(body)` 比对（`:176-183`）。
8. **截断容忍**：无 kEnd 只置 `complete=false`（`:190-194`），给被 kill 的录制留分析入口。

**第二层 AnalyzeLoaded（`:521`）**：

1. 初始化 4 个 FNV-1a（`:525-528`），offset basis 与 demo 侧一致。
2. 再守 gid 上限（`:543`），按 `max_gid+1` 分配统计数组（`:549-553`）。
3. 按顺序扫描（`:559-560`），`--until` 用 seq 截断（`:562-563`）。
4. 虚拟时钟：`max(vtime_ns)`；kSleep 再加 `timer_ns`（`:566` 起），不读真实时钟。
5. 逐类累计：send/recv 配对与值、select 结果、timer、park/wake（`:701/778`）。
6. 不符 → `kMismatch`（`:596` 例）；提前结束 → `stopped_early`（`:755`）；收尾写卡死诊断 `stuck_op_index`（`:798/818`）。

静态重演不执行用户代码；真正跑调度器是 `RunPlay`（`replay.cpp:173`），二者结论不能互推。

### 3.5 persistence：三引擎 + manifest + 逐条 CRC

**写端**（`writer.hpp:40` 三 Engine、`:47` 三 SyncPolicy）：

1. `EventWriter::Create`（`writer.hpp:119`，`writer.cpp:1090`）按 engine 选 `BufferedWriter`（同步对照）或 `AsyncWriter`（mpsc/segmented）。
2. Buffered（`writer.cpp:251`）：32B 文件头（`:266-272`）→ kMeta（`:283`）→ `Append`（`:310`）→ `FlushLocked`（`:442`）→ kEnd（`:380`）。
3. Async（`writer.cpp:526`）：每线程 batch（`:624-651`）→ MPSC → 单后台线程 `write`（`:852`）→ 按策略 `fdatasync`/`sync_file_range`（`:881`）。
4. segmented：段文件是裸记录流、无头/kEnd（`writer.hpp:23-24`），会话信息写 `<base>.manifest`：`G2CSESSION 1`、base、engine、records/bytes/checksum、segments，每段一行 `seg ...`（`writer.cpp:1001-1017`）；临时文件 + fsync + rename（`:1019-1036`）。
5. 编码器：`EncodeFixed`（`encoder.hpp:45`）快路径，否则 `EncodeGeneric`（`:52`）；必须与 `replay::EncodeEvent` 逐字节一致（`encoder.hpp:16`）。
6. CRC 位置：每条记录 CRC 由 `EncodeEvent` 算（`format.cpp:321`）；单文件整文件 CRC 在 kEnd（`replay_engine.cpp:179`）；manifest 的 checksum 是 `MixValue` 逻辑和（`writer.cpp:339/413`），不是整文件 CRC。

**读端**：

1. `Open`（`:105`）优先 manifest：校验 `G2CSESSION`（`:111`）、seg 字段（`:131-137`）、段文件存在且大小等于 bytes（`:146-158`）、段数（`:166`）。
2. 无 manifest 按单文件，段范围填 `0..UINT64_MAX`（`:172-185`）。
3. `Scan`（`:190`）先 `NeedSegment`（`:199`）再 mmap；单文件跳 32B 头（`:211-217`）。
4. 逐条 `DecodeEvent`（`:226`），坏 CRC 置 `crc_ok=false` 并记 offset（`:231-237`）。
5. 遇 kEnd 停（`:240-244`）；单文件缺 kEnd 报截断（`:269-271`）。
6. `ReadStats` 只统计，不自动比对 manifest 的 `expected_*`；后者通过 `reader.hpp:59-62` 暴露给调用方。

## 第四部分 · 数据与性能

按任务要求，这里只引用台账编号，不搬数字、不下结论。

| 子模块 | 台账关系 |
|---|---|
| `tools/bench/` | **M16**。README 首行即写「台账 M16 的可复现基准」（`tools/bench/README.md:1`）；M16 条目在 `docs/metrics-ledger.md:244`，汇总表 `:279`，状态行 `:89`。复现命令：`taskset -c 2,3 /tmp/go_sched_bench 100000 5 4`，参数为任务数/轮数/GOMAXPROCS，输出中位数与 min/max。台账明确要求「同轮同机同核位」比值，不跨轮拼接。 |
| `tools/persistence/` | 无独立台账。有 `bench/persist_bench.cpp` 与 `tools/persistence/bench`，但台账里没有对应编号（**未核实**是否有内部报告引用其数字）。 |
| `tools/cli/` | 无独立台账。`tools/verify_metrics.sh` 是台账校验脚本，不属于 CLI 本体。 |
| `tools/replay/` | 无独立台账。 |
| `tools/migration/` | 无独立台账。 |
| `tools/startup_cost/` | 无独立台账；方法论与数据在 `docs/modules/startup-cost.md`（README 指向），是否进台账**未核实**。 |

注意：`tools/startup_cost/` 用 rdtsc 校准 + 父子进程 + VmRSS 采样（`main.cpp:725`
主入口、`park_wake.cpp:889` 另一入口），结果对机器/负载敏感；引用时带环境，
不要把它当跨机结论。

---

## 第五部分 · 边界与陷阱

1. **文档同名不等于同主题**（最高频错误）。`user/docs/11-tools.md` 讲标准库风格
   工具库，`user/deep/11-tools.md`（本篇）讲开发者工具链 + 兼容层。浅层文档
   `:3` 的源码范围只包含 `include/go2cpp/tools/{strconv,strings}.hpp`，**不包含**
   其余 11 个头，更不包含根 `tools/`。交叉引用必须带路径。

2. **`migrate` 不是 schema migration**。CLI/`tools/migration` 处理的是协程快照
   （`G2CMIG03`），数据库迁移在 `storage::Migration`。两套代码的注释、错误信息都
   可能含 `migration`，搜到后先看路径再读。

3. **CLI 的 `migrate dump` 目前抓不到目标进程**。后端是 demo，pid/gid 只通过
   环境变量透传且被忽略（`commands.cpp:1383-1388`）。拿它做「抓取运行中进程」
   的验收会得到自己进程的快照。

4. **replay 的时间是虚拟时钟**。`AnalyzeLoaded` 只按 `vtime_ns`/`timer_ns` 推进，
   `ReplayClock::SleepFor` 在 replay 模式只查 `(gid, sleep_index)` 对应事件、
   不真睡（`clock.hpp:9/53`）。所以 replay 的「耗时」不能当墙钟性能数据；
   真睡只在 live 模式发生。

5. **replay 全量读入 + 256MiB 硬上限**。`LoadFile` 把整个文件 `fread` 进
   `std::vector`（`replay_engine.cpp:131-140`），超过 `kMaxFileSize` 直接拒绝
   （`:129`）。录制超过 256MiB 需要先分片（`RunSplit`），否则加载失败；
   分片后每片仍是全量驻留。

6. **缺 kEnd 的两种相反处理**。replay 引擎容忍截断（`complete=false`，
   `replay_engine.cpp:190-194`），reader 对单文件缺 kEnd 直接报错
   （`reader.cpp:269-271`）。同一份被 kill 的录制，`replay info` 可能出报告，
   `SessionReader::Scan` 却返回错误——不要据此判断「文件坏了」。

7. **格式版本兼容是单向的**。replay 写端 v5、读端接受 v1–v5
   （`format.hpp:66-68`），旧读端读不了新文件；persistence 的定长编码器要求
   与 `EncodeEvent` 逐字节一致（`encoder.hpp:16`），给 `Event` 加字段或改布局
   会同时破坏两处，必须同步改 `FixedRecordSize`/`EncodeFixed` 并跑
   `persistence_tests` 的往返用例。

8. **manifest 自身没有 CRC，checksum 也不强制比对**。manifest 是文本、写入用
   rename 保证原子，但内容没有校验和；`reader` 只校验段大小（`reader.cpp:146-158`），
   `checksum` 通过 `reader.hpp:59-62` 交给调用方比对。也就是说「能打开」不等于
   「内容完整」，需要显式比对 expected。

9. **migration 要求同一可执行文件**。CLI 错误信息明确写「v3/v2 都允许运行间
   PIE/ASLR 地址变化，但仍要求生成方与恢复方是同一可执行文件（ELF build-id 校验）；
   SO 版本可以不同」（`commands.cpp:1439`）。
   换构建、换编译选项后旧快照不能恢复；`migrate_alt`（`CMakeLists.txt:65`）
   就是专门造不同 build-id 的反例。

10. **签名密钥是硬约束**。`MIGRATION_SIGNING_KEY` / `MIGRATION_SIGNING_KEY_FILE`
    必需，`MIGRATION_INSECURE_NO_SIGNATURE=1` 仅测试用（`migrate_main.cpp:18-21`
    注释）。密钥丢失 = 旧快照永久不可恢复；密钥泄漏 = 可伪造快照，
    具体威胁模型**未核实**。

11. **CLI 只能单机**。传输是 `AF_UNIX`（`transport.cpp:187`），路径来自本机
    DebugServer；`--pid` 只查本机 `/proc/<pid>`（`commands.cpp:1364` 附近）。
    没有远程模式；跨机诊断需要自己转发 socket 或改 transport。

12. **独立工程默认不在主构建里**。replay/migration/persistence 都是独立 CMake
    工程（各自 `project(...)`），顶层构建不产出它们；ctest 包装脚本找不到产物时
    以 **77 退出 → SKIP**（`tests/tools/run_replay.sh`、`run_migration.sh` 头部注释）。
    看到 SKIP 不要当成「测试通过」。

13. **`args.cpp` 的未知选项返回 `kFailure`**（`args.cpp:57`），而缺位置参数返回
    `kUsage`（`args.cpp:129-131`）。如果外部脚本按退出码区分「命令行写错」和
    「运行失败」，未知选项会被归到后者——这是当前实现，不是文档承诺。

---

## 第六部分 · 测试

### 6.1 `tests/tools/`（Go 兼容层 + CLI/迁移/回放包装）

| 文件 | 覆盖 |
|---|---|
| `test_control_flow.cpp` | `defer`/`panic`/`recover` 状态机 |
| `test_error.cpp` / `test_errors_ns.cpp` | `Error`/`Wrap`/`Unwrap`/`Join`/命名空间 |
| `test_fmt.cpp` | 格式化 |
| `test_strconv.cpp` | `Atoi`/`ParseInt`/`Itoa`/边界 |
| `test_streams.cpp` | `Reader`/`Writer`/`Copy`/`ReadFull` |
| `test_strings.cpp` / `test_time.cpp` | `strings`、`time` |
| `run_cli.sh` | 调 `tools/cli/tests/run_all.sh`，213 项断言；找不到二进制 SKIP 77 |
| `run_migration.sh` | 调 `tools/migration/tests/run_tests.sh`，53 项场景 |
| `run_replay.sh` | 先跑 `go2cpp_replay_tests`，再跑 6 个端到端脚本 |

命令（在仓库根执行）：

```bash
# CLI：先构建，再指定二进制
cmake -S . -B build-cli -DGO2CPP_WITH_CLI=ON && cmake --build build-cli -j
GO2CPP_CLI_BIN=$PWD/build-cli/tools/cli/go2cpp-cli bash tests/tools/run_cli.sh

# migration / replay：独立工程先构建
cmake -S tools/migration -B build-migration && cmake --build build-migration -j
bash tests/tools/run_migration.sh

GO2CPP_REPLAY_BUILD_DIR=build-replay ./tools/replay/build.sh
bash tests/tools/run_replay.sh
```

> 三个脚本都在找不到产物时打印构建命令并以 77 退出；CI 侧要靠
> `SKIP_RETURN_CODE=77` 区分「跳过」与「失败」。

### 6.2 `tests/replay/`（并发原语回放）

13 个文件：`test_timer_replay.cpp`、`test_io_replay.cpp`、`test_cv_replay.cpp`、
`test_rwmutex_replay.cpp`、`test_waitgroup_replay.cpp`、`test_mixed_v5_replay.cpp`、
`test_resource_replay.cpp`、`test_resources_v5.cpp`、`replay_v5_stress_main.cpp`
及 3 个 `.sh` 场景脚本。重点验证 v5 资源事件（RWMutex/CV/WaitGroup/IO）的
录制→回放一致性与 seq 单调性。

```bash
bash tests/replay/run_resource_replay_demo.sh
bash tests/replay/run_resources_v5_demo.sh
bash tests/replay/run_history_vs_replay_demo.sh
```

（脚本内部依赖已构建的 demo 与 `.so`，具体前置条件见脚本头部；**未逐行核实**。）

### 6.3 `tools/persistence/tests/`

`persistence_tests.cpp` 覆盖：定长编码器与 `replay::DecodeEvent` 的逐字段往返
（含随机值）、三引擎写→读回、4/8 线程 seq 单调、段滚动与范围读、v4/v5 混合、
故障注入（截断/CRC 损坏/段大小不一致）、0/1 条边界、跨引擎 checksum 一致
（文件头注释 1-8 条）。`platform_probe.cpp` 只探测 io_uring/O_DIRECT 能力，
不参与压测（`CMakeLists.txt:38` 注释）。

```bash
cmake -S tools/persistence -B build-persistence -DPERSISTENCE_BUILD_TESTS=ON
cmake --build build-persistence -j
./build-persistence/persistence_tests

# 压测
./build-persistence/persist_bench
```

注意：该文件注释写「不接入顶部 ctest」，但独立工程的
`CMakeLists.txt:46-47` 在 `PERSISTENCE_BUILD_TESTS` 下注册了
`add_test(NAME go2cpp_persistence_tests ...)`——两句话不矛盾（不接入**顶部**），
但容易被读成「完全不注册」。

---

