# Go2Cpp 五能力对抗性缺陷报告（2026-10-20）

- 审查对象：DebugServer、事件环、迁移（tools/migration）、回放（tools/replay）、CLI（tools/cli）、Doctor（diagnostic_agent）
- 工作树：`/UserData/CodexWorkSpace/Go2Cpp-work`（只读；测试产物全部在 `/tmp/bughunt`）
- 方法：源码逐行审查 + `/tmp` 独立构建/探针 + 真实 demo 进程 + 畸形文件/帧注入 + RLIMIT 资源限制
- 结论口径：**真实缺陷**（可复现且违反文档/契约）、**设计边界**（代码/文档已声明或属于既定取舍）、**误报**（经复测排除）

## 0. 总览

| 级别 | 数量 | 摘要 |
|---|---:|---|
| P0 | 0 | 未发现默认配置下可直接远程利用、破坏内存安全的问题 |
| P1 | 5 | Doctor 误判 owner 存活并解锁；CLI replay play 未执行却成功；CLI 迁移与 v3 完全不兼容；replay 超大 gid 崩溃；migration 超大文件崩溃 |
| P2 | 10 | DebugServer 慢连接停摆/超长帧 RST/每订阅一线程/响应无界；CLI 删除目标文件、吞参数；migration 溢出/段重叠/半写；Doctor backlog 耗尽 |
| P3 | 12 | 负数参数、路径遍历、尾随垃圾、未知枚举、虚拟时钟回绕、CLI doctor 参数不一致等 |
| 设计边界 | 8 | 事件环线程退出丢事件、Reset 并发、关键区覆盖、128 线程上限、未鉴权本地接口等 |
| 误报 | 7 | 退出码 0（宿主 shell 变量展开导致）、订阅线程泄漏、深嵌套崩溃、签名绕过等 |

完整的原始输出与可复现脚本见 `/tmp/bughunt/`；报告副本 `/tmp/bug_hunt.md`。

## 1. 真实缺陷（可复现）

### P1-1 Doctor `mutex-unlock` 把“未注册 Scheduler 中的存活 owner”误判为已死并强行解锁

- 位置：`src/debug/diagnostic_agent.cpp:2536-2565`（owner 只在该文件 `:2544` 遍历 `m_schedulers` 的任务表）、`:2574-2587`（判定后直接 `mutex->Unlock()`）。
- 期望：文档与代码注释承诺“只有 owner 已不可能再访问临界区”才解锁（`:17-19`、`:2495-2497`）。
- 实际：owner 存活但所属 Scheduler **未注册**到 Doctor 时，`owner_alive=false`，返回 `verdict=ok reason=owner-gone` 并真的解锁；若 owner 之后继续执行临界区，互斥被破坏。
- 复现：
  1. `/tmp/bughunt/mutex_probe /tmp/bughunt-mutex.sock`（G1 持锁后 park，G2 排队；程序调用 `SetHeldLockParkDetection(true)`），输出 `SETUP held=1 waiters=1 owner_gid=1 owner_known=1`、`registered_scheduler=0`。
  2. `python3 /tmp/bughunt/g2cd_client.py /tmp/bughunt-mutex.sock "mutex-unlock name=held-mutex"`。
  3. 实际输出：`verdict=ok`、`reason=owner-gone`、`waiters_before=1`、`waiters_after=0`、`held_after=false`。
  4. 对照：以 `... /tmp/bughunt-mutex.sock registered` 启动（注册同一 Scheduler），同命令返回 `refused: owner-alive ... state=kWaiting`。
- 影响：错误解锁后两个执行流同时进入临界区，可能造成数据竞争/内存破坏；也违反“诊断只读、干预必须可证明安全”的核心约束。触发条件：Debug 构建（`NDEBUG` 未定义时持锁检测默认开）或设置 `GO2CPP_DETECT_HELD_PARK=1`，且存在未注册 Scheduler（部分注册是常见用法）。
- 建议：不依赖注册表判断存活；在 Mutex 中记录 owner 的 task 生命周期/弱引用，或对未注册 Scheduler 一律 `refused owner-unregistered`。

### P1-2 CLI `replay play` 从未执行重放，却返回成功

- 位置：`tools/cli/commands.cpp:1040-1041`（`RunBackend(backend, {AbsolutePath(probe.path)})`，没有把 `play` 子命令传给后端）。
- 期望：`go2cpp-cli replay play rec.g2cr` 真正驱动重放；后端 `replay play` 未注册入口时退出码 3。
- 实际：后端按 `replay <file>` 执行 **info/静态分析**，CLI 直接返回退出码 0；输出 JSON 中 `replay_executed=false`，但 CLI 不检查。
- 复现：`GO2CPP_REPLAY_TOOL=/tmp/replay-build/replay /tmp/go2cpp-cli replay play /tmp/bughunt/replay/minimal.g2cr; echo $?` → `exit=0`，JSON `"status":"ok","replay_executed":false`。
- 影响：CI/脚本会把“未重放”当成功；语义重放保障完全失效。
- 建议：传 `{"play", path}`，并校验 `replay_executed`；未执行时返回 3。

### P1-3 CLI 迁移只认 `G2CMIG02/v2`，当前后端产出 `G2CMIG03/v3`，新能力 100% 阻断

- 位置：`tools/cli/formats.hpp:37-38`（`kMigrationMagic` 硬编码 `G2CMIG02`）、`tools/cli/formats.cpp:190-193`、`tools/cli/commands.cpp:1065-1078`（`RequireMigrationV2`）、`:1162-1169`；后端格式为 `tools/migration/include/migration/format.hpp`（`G2CMIG03`、version 3）。
- 期望：CLI 的 dump/restore 与同工作树 migration 后端互通。
- 实际：
  - `migrate restore /tmp/snap_v3.g2c` → `exit=4`，`迁移文件格式不匹配 ... 期望魔数 G2CMIG02`。
  - `migrate dump --pid 1 --gid 1 --out /tmp/x.g2c` → 后端成功写出 v3，但 CLI 随后 `ProbeFile` 报“不是有效快照”，`exit=1`。
- 复现：`MIGRATION_SIGNING_KEY=... GO2CPP_MIGRATE_TOOL=.../build-migration/migrate /tmp/go2cpp-cli migrate restore /tmp/snap_v3.g2c`（快照生成见 `/tmp/bughunt` 记录）。
- 影响：CLI 的迁移子命令对当前版本完全不可用；文档若仍写 v2 会误导。
- 建议：CLI 支持 `G2CMIG03`/v3，或直接调用后端 `migrate verify` 做版本判定，避免重复维护格式常量。

### P1-4 回放解析对超大 `gid` 无上限，`std::bad_alloc` 未捕获导致进程 abort

- 位置：`tools/replay/src/replay_engine.cpp:252-257`（`per_gid.resize(max_gid+1)`）、`:677-696`（`BuildIndexes` 按 `max_gid+1` 分配 11 个 vector）。
- 期望：畸形记录返回 `format-error`/`mismatch`。
- 实际：一条 `kSpawn`（payload 为空）把 gid 设为 `0x7FFFFFFF` 或 `0xFFFFFFFF` 即可：
  ```
  /tmp/replay-build/replay info /tmp/bughunt/replay/gidmax.g2cr
  terminate called after throwing an instance of 'std::bad_alloc'
  # exit -6 (SIGABRT)
  ```
  `gidmax2.g2cr`（`0x7FFFFFFF`）同样 abort。CLI `replay play gidmax.g2cr` 也返回 134。
- 影响：任意可写记录文件即可让 CLI/回放工具崩溃；若接入常驻服务会被一条输入打死。
- 建议：限制 gid 上界（例如 1<<20）或用稀疏 `map`；在 `ReplayFile*` 外层捕获 `bad_alloc` 转 `format-error`。

### P1-5 迁移读取超大稀疏快照无大小上限，`bad_alloc` 未捕获导致 abort

- 位置：`tools/migration/src/format.cpp:908-932`（`ftell` 后 `std::vector<uint8_t> data(size)`，无上限、无 try/catch）。
- 期望：拒绝异常大的输入并返回结构化错误。
- 实际：`truncate -s 8G /tmp/big.g2c`，在 `RLIMIT_AS=400MiB` 下 `migrate verify` → `terminate called after throwing an instance of 'std::bad_alloc'`，`exit=-6`。无 RLIMIT 时则可能耗尽内存。
- 复现：`python3 /tmp/bughunt/mig_negative.py` 最后一段（已内置 `RLIMIT_AS`，输出 `huge verify exit=-6`）。
- 影响：误操作/恶意文件导致工具崩溃；服务化后为拒绝服务。
- 建议：读取前限制 `file_size`（例如 256MiB，与 replay 一致），并用临时缓冲按块校验。

### P2-1 DebugServer 单线程 accept 被慢连接阻塞 5 秒以上

- 位置：`src/debug/debug_server.cpp:453-469`（`Run` 串行 `HandleClient`）、`:471-491`（读请求循环，`SO_RCVTIMEO=5s` 在 `:472-478`）。
- 期望：一个客户端发半行不应影响其他 DUMP/STATUS。
- 实际：连接后只发 `P`（不换行、不关闭）→ 随后另一条连接 `PING` 等待 **5000.2ms** 才收到；`PING` 本身正常 0.5ms。每 4.9s 续一个字节可无限延长。
- 复现：`python3 /tmp/bughunt/test_debug.py /tmp/bughunt-debug.sock` 的 `ping-while-slow`（输出 5000ms）。
- 影响：任何一个异常/恶意客户端让整个诊断接口停顿；订阅线程虽独立，但 accept 循环被独占。
- 建议：请求读取也走非阻塞 + deadline，或每连接独立线程/事件驱动；为半包设置更短超时。

### P2-2 超长命令导致连接 RST，客户端收不到任何错误响应

- 位置：`src/debug/debug_server.cpp:482-495`（4096 上限后不再读取剩余字节，也不报错）；`:466-467` 关闭连接时未读数据触发 RST。
- 期望：超长帧返回 `ERR bad-command frame-too-long` 并保持/规范关闭。
- 实际：发送 5000 字节（即使以 `\n` 结尾、前缀是 `PING`）→ 客户端 `ConnectionResetError`；连 `PING` 的正常响应也收不到。
- 复现：`test_debug.py` 的 `long-5000`、`long-with-newline`、`json-deep-1800`（均 RST）。
- 影响：协议错误不可诊断；可被用来覆盖/丢失正常响应。
- 建议：先 `shutdown(SHUT_WR)` 并读完（或 drain）再关闭；超长时返回明确错误。

### P2-3 每个 SUBSCRIBE 连接创建一个 OS 线程，无上限

- 位置：`src/debug/debug_server.cpp:506-528`（`std::thread(...).detach()`），`ServeSubscription` 每连接一个线程。
- 期望：订阅数有上限或使用事件驱动。
- 实际：100 个订阅连接 → 服务进程线程数 107（基线 7）、fd 107；全部成功。1000 个会继续创建。
- 复现：`python3 /tmp/bughunt/test_backpressure.py /tmp/bughunt-debug.sock <pid>`（输出 `100 subscriptions: subscribers=100 threads=107`）。
- 影响：同用户可耗尽线程/虚拟内存（每线程默认 8MiB 栈），并触发 pthread_create 失败路径。
- 建议：限制并发订阅数（例如 16/64），或改为单线程 poll 多连接。

### P2-4 DebugServer 响应无上限（实测 46.5MiB），与 CLI 16MiB 上限冲突导致 status/scheduler dump 不可用

- 位置：`src/debug/debug_dump.cpp:309-314`（`tasks` 数组全量输出，无条数上限）；`src/debug/debug_server.cpp:884-885`（完整 payload 一次性返回）；`tools/cli/transport.cpp:51`、`:356`（16MiB 硬上限）。
- 期望：大型进程的 status/dump 可用，或明确分页/截断并标注。
- 实际：demo 进程 `JSON scheduler` 响应头 `GO2CPP-DEBUG/1 OK 48727096`（46.5MiB）；CLI `status`/`dump --sections scheduler` 直接报 `payload 超过上限 16.0 MiB`，`exit=1`。`DUMP scheduler` text 也超限。
- 复现：`python3 /tmp/bughunt/test_cli2.py` 的 `dump-scheduler-text`；或直接读响应头（报告附录命令）。
- 影响：任务数多的真实进程上，CLI 的核心 `status` 失效；单次请求还会在服务端构造数十 MB 字符串并长时间占用 accept 线程。
- 建议：DebugServer 侧限制 `tasks` 条数/分页并声明截断；CLI 侧支持分页或提高上限。

### P2-5 CLI `replay record` 先删除 `--out` 已存在文件，包含目标程序本身

- 位置：`tools/cli/commands.cpp:892-904`（在 fork/exec 之前 `std::filesystem::remove(out_absolute)`）。
- 期望：拒绝覆盖非记录文件/目标程序，或用临时文件+原子替换。
- 实际：
  - 先写 `/tmp/bughunt/cli-victim.txt`，执行 `replay record /bin/true --out /tmp/bughunt/cli-victim.txt` → 文件被删除，后端未生成记录，CLI 报错退出 3，数据不可恢复。
  - 复制 `/bin/true` 到 `/tmp/bughunt/cli-target`，执行 `replay record /tmp/bughunt/cli-target --out /tmp/bughunt/cli-target` → 目标被删除，`execv` 失败，`exit=1`。
- 复现：`python3 /tmp/bughunt/test_cli1.py` 的“replay record 破坏性”段。
- 影响：误指 `--out` 即造成用户文件/可执行文件丢失；脚本化使用时风险更高。
- 建议：先校验 `out` 不是目录/不是被记录程序；仅在子进程成功后再替换；使用临时文件。

### P2-6 CLI `replay record` 吞掉被记录程序自己的 `--out` 参数

- 位置：`tools/cli/commands.cpp:859-865`（已确定 exe 后仍把 `--out` 当 CLI 选项）。
- 期望：`record prog args... --out file` 中，`args...` 原样传给 prog；只有 CLI 自身的 `--out` 被消费（通常应要求 `--out` 在 exe 前或使用 `--`）。
- 实际：`replay record /bin/echo hello --out /tmp/f` → echo 只收到 `hello`（`--out /tmp/f` 被 CLI 吃掉，且 f 被删除/未生成）。
- 复现：`test_cli1.py` 的 `echo-out-eaten`（输出 `hello`，`exists=False`）。
- 影响：被记录程序若依赖 `--out` 等选项，记录场景会被静默改变；用户很难发现。
- 建议：exe 之后一律透传，CLI 选项必须在 exe 之前；或强制 `--`。

### P2-7 迁移快照 safepoint `slot_offset + 8` 整数溢出绕过越界校验

- 位置：`tools/migration/src/format.cpp:1335`（`slot.slot_offset + 8 > snap.stack_size`）。
- 期望：`slot_offset` 接近 `UINT64_MAX` 的挂起点必须被拒绝（文档声称“offset 越界都明确失败”）。
- 实际：把 `slot_offset` 改为 `0xFFFFFFFFFFFFFFF8`，重算段 CRC/整文件 CRC/HMAC（持有密钥），`migrate verify` 返回 0 且 `signature verified=yes`；`+8` 回绕为 0，检查通过。
- 复现：`python3 /tmp/bughunt/tamper_migration.py /tmp/snap_v3.g2c /tmp/bughunt/mig && python3 /tmp/bughunt/run_mig.py /tmp/bughunt/mig/safepoint_overflow.g2c`（exit 0）。
- 影响：恢复侧用 `new_base + slot_offset` 计算挂起点，回绕后可能指向栈外/任意地址；需要密钥才能构造，属纵深防御缺口。
- 建议：改用 `slot.slot_offset > stack_size || stack_size - slot.slot_offset < 8`。

### P2-8 迁移段表不校验重叠/布局，段可指向文件头

- 位置：`tools/migration/src/format.cpp:988-1019`（仅检查 `offset/size` 在文件内与 CRC，无“段不得与 header/段表/其它段重叠”检查）。
- 期望：段布局非法时拒绝。
- 实际：把 `registers` 段 `offset` 改为 0（指向 magic/header），重算 CRC/HMAC 后 `migrate verify` 返回 0，`DecodeRegisters` 从文件头读出 7 个 u64 并被当作寄存器。
- 复现：`tamper_migration.py` 生成的 `section_overlap.g2c`，`run_mig.py` 显示 exit 0。
- 影响：有密钥的构造者可让不同段互相覆盖/伪装，绕过“快照自洽”预期；恢复可能使用错误寄存器。
- 建议：要求 `offset >= kHeaderSize + section_count*48`、段间不重叠、必需段唯一，且签名段在末尾。

### P2-9 迁移写快照非原子：信号/磁盘限制时留下半写文件

- 位置：`tools/migration/src/format.cpp:893-905`（`fopen(path, "wb")` 直接截断目标文件并 `fwrite`；无临时文件、无 SIGXFSZ/SIGTERM 处理）。
- 期望：写入失败/进程被杀时不破坏既有快照（至少留下可识别的临时文件）。
- 实际：`RLIMIT_FSIZE=20000` 下 `migrate dump /tmp/.../limited.g2c` → 进程被 `SIGXFSZ(25)` 杀死，留下 **20000 字节半写文件**；若目标路径原有有效快照，已被 `"wb"` 截断。
- 复现：`python3 /tmp/bughunt/mig_negative.py`（`limit dump exit=-25 size=20000`）。
- 影响：磁盘满/配额/信号下产生损坏文件且无清理；覆盖同名有效快照时数据丢失。
- 建议：写 `path.tmp` → fsync → rename；必要时临时忽略 SIGXFSZ 并检查 `fwrite` 返回值。

### P2-10 Doctor 半帧连接无超时、无连接上限，耗尽 listen backlog

- 位置：`src/debug/diagnostic_agent.cpp:590`（`listen(..., 32)`）、`:671-699`（`TryReadFrame` 只读可用字节，不设每连接 deadline）、`:818-845`（accept 循环无连接数限制）；`include/go2cpp/debug/diagnostic_agent.hpp` 的 `Connection` 无超时字段。
- 期望：半帧空闲连接被回收，或至少不阻塞新客户端。
- 实际：开 91 个只发 24 字节帧头的连接后，新客户端 `connect` 报 `BlockingIOError: [Errno 11] Resource temporarily unavailable`（backlog 满）；这些连接会一直保留。`ping` 在已建立连接上仍可用（Receiver 是多路 poll），但新连接被拒。
- 复现：`python3 /tmp/bughunt/test_doctor_frames.py /tmp/bughunt-doctor.sock <pid>`（`half-frame connect failed at 91`）。
- 影响：同用户可让 Doctor 无法接受新命令（诊断通道 DoS）；无空闲超时意味着攻击者可以低成本长期占用。
- 建议：连接空闲超时（例如 5s）、最大连接数、backlog 满时明确拒绝新连接并记录；解析器对“只收到部分帧”的连接设置 deadline。

## 2. P3 / 低风险真实缺陷

1. **DebugServer `EVENTS -1` 与 `SUBSCRIBE events -1` 接受负数**：`src/debug/debug_server.cpp:858-868`、`:115-143` 使用 `std::stoull`，负号回绕为 `ULLONG_MAX`，`EVENTS -1` 等同“全量”（84,978 字节响应），`SUBSCRIBE events -1` 的 interval 变成 5000ms 而非报错。期望负数字段明确拒绝。
2. **DebugServer 合法 JSON 约 >50 层被拒**：`src/debug/debug_server.cpp:890-911` 调用 `ParseJson`；深度 50 接受、100/200/500 返回 `invalid JSON request`，错误信息不区分“深度超限”与语法错。属防御性限制，但未文档化。
3. **CLI `doctor` 与统一参数解析不一致**：`tools/cli/doctor.cpp:367-392` 手写解析，`--pid=1` 报“未知选项”（dump/status 支持 `--name=value`）；`--command ""` 被忽略后默认执行 `status`（`:415-416`）；重复 `--command` 后者静默覆盖；`--concurrency 0/257` 只返回 2 而无错误文案。
4. **回放 `kEnd` 之后尾随垃圾被接受**：`tools/replay/src/replay_engine.cpp:147-164` 遇到 `kEnd` 立即 break，`tail_garbage.g2cr`（kEnd 后 32 字节）`status=ok`。数据完整性上应拒绝或至少报告尾部字节。
5. **`select` 的 `cases` 为空时不校验 `chosen`**：`tools/replay/src/replay_engine.cpp:333-339` 只在 `!event.cases.empty()` 时检查越界；`select_zero_cases.g2cr`（chosen=99、cases=[]）`status=ok`。
6. **虚拟时钟加法回绕**：`tools/replay/src/replay_engine.cpp:271-277`、`:891-895` 的 `event.vtime_ns + event.timer_ns` 为 u64；`timer_wrap.g2cr`（vtime=200，timer_ns=UINT64_MAX-99）报告 `virtual_ns=200`，而不是接近 `UINT64_MAX`，时间线被严重低估。
7. **`replay split --prefix=../evil` 写出 out-dir**：`tools/replay/src/split.cpp` 拼接 `out_dir / (prefix + ".partNNNN.g2cr")`，实测生成 `/tmp/bughunt/split/../evil.part0001.g2cr`（`escaped=True`）。路径由用户控制，属本地安全边界，但会静默越过输出目录。
8. **迁移 `wait_kind=3`（枚举外）被接受**：`tools/migration/src/format.cpp:1117` 直接赋值，`:1226` 只要求“非 0 时有 channel 段”；`wait_kind_3.g2c` 重算签名后 verify 返回 0，`Describe` 显示 `wait_kind=3`。恢复侧对未知枚举没有已知拒绝路径。
9. **CLI `dump --out <目录>` 先写临时文件再 rename 失败**：`tools/cli/commands.cpp:186-198` + `formats.cpp:275`；错误文案清晰，但临时文件命名/清理依赖实现（本次未残留）。
10. **迁移 `socket/key` 环境变量可被任意覆盖**：`MIGRATION_SIGNING_KEY` 优先于文件；密钥长度校验 ≥16 字节但不检查熵；属部署边界。
11. **符号 id 唯一性未在格式层校验**：`tools/migration/src/format.cpp` 的 `DecodeSymbols`/verify 不检查 `SymbolRecord::id` 重复（句柄 id 有检查 `:1348`）。仅代码审查发现，未构造验证（需重排段）；同名符号冲突在运行期能拒绝（见覆盖）。
12. **迁移 `MIGRATION_INSECURE_NO_SIGNATURE=1` 生成的未签名快照无法被读端接受**：`format.cpp` 无条件拒绝未签名快照。这是有意的负例开关，但语义上“只写不读”，文档应明确。

## 3. 设计边界 / 已知限制（不计为缺陷）

1. **事件环只保留仍存活线程的 ring**：`src/debug/debug_event_ring.cpp:93-116` 在线程退出时 `RemoveRing`，`Snapshot` 只遍历表中 ring（`:339-363`）。实测（`/tmp/bughunt/ring_exit`）：两个线程各记录 100 条 `kThreadStop` 后 `join`，`after_join=0`；同样记录但保持线程存活时 `while_alive=200`。因此 `kThreadStop` 这类“记录后线程立刻退出”的事件在事后 dump 中不可见。头文件已声明“线程退出时自动从全局表摘除”，但与“线程生命周期可观测”目标冲突。
2. **`Reset()` 仅适用于无并发写入**：`debug_event_ring.cpp:381-402`；并发 Record 时 `g_publish_seq` 回退，实测 200 次 Reset 中有 90 次发布序号回退，seq 会重复。注释已声明“仅测试专用”，但公开 API 无运行时保护。
3. **关键区满时覆盖旧关键事件且无 `critical_dropped` 计数**：`kCriticalCapacity=64`；记录 100 条 `kWakeRequest` 后 `critical_event_count=64`（丢最早 36 条），Dump 文本没有专门计数，只能靠 seq 缺口推断。头文件已说明“更旧的关键事件会被覆盖”。
4. **`kMaxThreads=128`**：150 个并发记录线程时 22 个未注册，`dropped_threads=22`，其事件永久不可见；已有计数但无法知道具体丢失事件。
5. **DebugServer/Doctor 本地接口无命令级鉴权**：socket 0600（实测 `srw-------`），同用户/root 可 `force-wake`、`mutex-unlock`、`inject-fiber`；`DiagnosticAgentConfig::socket_mode` 可配 0666（API 允许），此时任意本地用户可干预。属“本机同用户信任”设计，但应在文档中显式声明。
6. **迁移 `mutex-unlock` 依赖持锁检测开关**：Release 构建默认 `DetectHeldParkEnabled()==false`（`src/concurrency/sync.cpp:111-126`），此时 `owner_known=false`，`mutex-unlock` 一律 `refused owner-unknown`（安全但功能不可用）；Debug/`GO2CPP_DETECT_HELD_PARK=1` 才进入 P1-1 的判定路径。
7. **回放记录允许截断进入分析**：`LoadFile` 在无 `kEnd` 时仍返回事件并标 `complete=false`（设计为诊断被 kill 的进程）；`replay info` 报 `stuck`，属预期。
8. **订阅/事件环容量与高频事件**：慢读订阅实测 `dropped_gap` 快速增长（3 秒 154,673）且 `dropped_backpressure` 如实上报；这是固定容量的固有限制，帧边界完整（leftover=0）。

## 4. 误报（经复测排除）

1. **“CLI 错误也退出 0”**：宿主 shell 对 `$?` 的展开干扰了早期观察；`strace` 显示 `exit_group(3)`，完整矩阵退出码为 2/3/4/5，符合契约。
2. **“订阅者计数/线程泄漏”**：最小复现关闭后 `subscribers=0`、线程回 7；100 个订阅关闭后 1 秒内归零（首次 0.2s 读数是关闭检测窗口）。
3. **“深嵌套 JSON 触发栈溢出”**：1800 层请求因超过 4096 字节被 RST（P2-2），未到达解析器；≤4096 字节的 50 层接受、100 层拒绝，无崩溃。
4. **“事件环线程退出并发快照 UAF”**：ASan 下 6 秒 514,469 轮 Snapshot/DumpText + 持续短命线程，未报告 UAF。代码审查仍认为存在“读者加载 ring 指针后线程析构 thread_local”的窗口，但未复现，**不作为真实缺陷**。
5. **“签名可绕过/CRC 修复即可伪造”**：翻转字节、重算 CRC、未签名、magic/version/截断全部被拒（见覆盖）。
6. **“Doctor 并发命令串行会丢响应”**：10/50/100/300 并发 `ping` 全部成功（300 并发 300/300，164.8ms），单线程执行 `concurrent_peak=1`。
7. **“迁移并发 dump 互相干扰”**：两个进程同时 dump 到不同文件均成功且 verify 通过。

## 5. 攻击面覆盖矩阵（每面 ≥5 输入）

### A. 协议解析（DebugServer 文本/JSON、Doctor G2CD、CLI 客户端、迁移/回放文件头）
- DebugServer 27 种输入：PING/空行/空白/小写/未知/EOF 无换行/半包 EOF/半包超时/双命令/多余参数/未知组件/负数/溢出数字/JSON 数组/浮点 id/未知 cmd/非法 sections/坏 JSON/5000 字节/CRLF/4095 边界/非法 UTF-8/1800 层 JSON 等。
- Doctor 20 种帧：bad magic/version/kind 0/4/flags bit3/all/checksum/plen>16MiB/16MiB 截断/空 payload/stream 0/seq max/chunked 请求/JSON flag/NUL/5000 字节/未知命令/双请求等。
- CLI 假服务器 8 模式：default/bad-protocol/truncate/error/delay 2000/no-scheduler/partial/max-conn。
- 迁移 8 个负例（magic/version/截断/CRC/重算 CRC 不重签/未签名）+ 5 个重签变体。
- 回放 12 个记录：gid max/2^31、timer 极值/回绕、select 越界/空 cases、尾随垃圾、截断、CRC 错、大 seq。
- **未发现**：DebugServer 深嵌套崩溃、Doctor 坏帧崩溃、CLI 协议错误误判。

### B. 资源（小内存、fd、磁盘、信号）
- `RLIMIT_AS=400MiB` + 8GiB 稀疏迁移快照 → bad_alloc abort（P1-5）。
- `RLIMIT_FSIZE=20000` 迁移 dump → SIGXFSZ + 20000 字节半写（P2-9）。
- 200 个 Doctor 半帧连接 → 91 个后 backlog 满、新 connect EAGAIN（P2-10）。
- 100 个 DebugServer SUBSCRIBE → 107 线程（P2-3）。
- 46.5MiB `JSON scheduler` 响应（P2-4）；CLI 16MiB 拒绝。
- 150 个事件环线程 → 128 上限、dropped_threads=22（边界）。
- 300 并发 Doctor 命令 → 全部成功（未触发 queue-full）。
- **未测**：真正 tmpfs 满、SIGSEGV 落盘、服务端 RLIMIT_NOFILE=小值。

### C. 并发
- Doctor 10/50/100/300 并发 ping：全部成功；`concurrent_peak=1`。
- 50 并发 DebugServer PING：50/50，max 4.5ms（慢连接场景除外）。
- 100 并发订阅、关闭后 1s 内线程/计数回落。
- 事件环：150 线程、200 次 Reset、6 秒 514k 轮 Snapshot、双线程 Record。
- 迁移：两个 dump 进程并发，均成功并 verify。
- **未发现**：订阅计数泄漏、Doctor 响应串线、迁移并发互相破坏。

### D. 数据完整性（CRC/签名/段越界/符号/类型）
- 迁移：CRC 位翻转拒绝、重算 CRC 不重签拒绝、未签名拒绝、magic/version 拒绝、截断拒绝、build-id 不匹配拒绝（后端内置）；safepoint 溢出、段重叠、wait_kind=3 被接受（P2-7/P2-8/P3）；符号名冲突在 dump 阶段拒绝（`migrate_conflict`）。
- 回放：CRC 错拒绝、截断报 stuck、gid 崩溃、尾随垃圾接受、select cases 空接受、虚拟时钟回绕。
- 事件环：seq 回退（Reset）、关键区覆盖、线程退出丢事件。
- **未构造**：符号 id 重复、句柄类型不匹配（需密钥+段重排）。

### E. 安全（socket 权限、路径遍历、命令注入、未鉴权）
- DebugServer/Doctor socket 均为 `srw-------`（0600），路径 `/tmp`；同用户可连接。
- `replay record --out ../../tmp/...`：相对路径按 CWD 解析，可写任意用户可写位置；`replay split --prefix=../evil` 实际写出 out-dir（P3）。
- `replay record --out <已存在文件/目标程序>`：先删除（P2-5）。
- 未鉴权 `force-wake scope=all` 成功唤醒 3 个 target；`scope=mutex` 被拒；`mutex-unlock` 在 owner 存活但未注册时被放行（P1-1）。
- 命令注入：Doctor 命令名固定白名单，参数 `name=`/`scope=` 仅查注册表/枚举；未发现 shell/注入路径。

### F. 语义（force-wake、mutex-unlock、事件覆盖、虚拟时钟）
- `force-wake scope=mutex` → `refused mutex-waiters-are-not-retryable`（正确）。
- `mutex-unlock`：注册 Scheduler 时 `refused owner-alive`（正确）；未注册时错误放行（P1-1）；未持有时 `refused not-locked`；无等待者 `refused no-waiters`。
- 事件环：ReadSince 最旧 N 条、升序、cursor 后为空；Snapshot 最新 N 条；pid 绑定/解绑正确。
- 回放：select 越界（有 cases）报 mismatch；空 cases 放行；timer 回绕。
- **未发现**：force-wake 对持锁等待误唤醒（scope 白名单正确）、事件环游标错乱。

## 6. 复现资产与命令索引

| 资产 | 用途 |
|---|---|
| `/tmp/bughunt/test_debug.py` | DebugServer 27 种协议输入（含慢连接 5s） |
| `/tmp/bughunt/test_debug2.py` / `test_sub_min.py` / `test_backpressure.py` | 订阅参数、背压、100 连接线程数 |
| `/tmp/bughunt/test_doctor.py` / `test_doctor_frames.py` / `g2cd_client.py` | Doctor 命令矩阵、300 并发、20 种坏帧、91 半帧 backlog |
| `/tmp/bughunt/mutex_probe.cpp` / `mutex_probe` | P1-1 的 G1 持锁 + G2 排队 + 未注册 Scheduler 复现 |
| `/tmp/bughunt/make_replay.py` / `run_replay.py` | 12 个 G2CRPLY 畸形记录与批量解析 |
| `/tmp/bughunt/tamper_migration.py` / `run_mig.py` / `mig_negative.py` | 重签篡改、未签名/CRC/截断、RLIMIT 测试 |
| `/tmp/bughunt/ring_test.cpp` / `run_ring.py` / `ring_exit.cpp` | 事件环 Reset/关键区/线程上限/游标/pid；线程退出后事件消失（`after_join=0 while_alive=200`） |
| `/tmp/bughunt/test_cli1.py` / `test_cli2.py` / `test_extra.py` | CLI 参数、协议客户端、迁移版本、replay play、record 破坏、split、超时、并发迁移 |

关键最小命令：

```bash
# 启动真实 demo
cd /UserData/CodexWorkSpace/Go2Cpp-work
setsid ./build-diag/go2cpp_debug_subscribe_demo /tmp/bughunt-debug.sock 900 \
  >/tmp/bughunt/debug-demo.log 2>&1 </dev/null &
setsid ./build-diag/go2cpp_diagnostic_demo /tmp/bughunt-doctor.sock \
  >/tmp/bughunt/doctor-demo.log 2>&1 </dev/null &

# P1-1：未注册 Scheduler 时错误解锁
setsid /tmp/bughunt/mutex_probe /tmp/bughunt-mutex.sock \
  >/tmp/bughunt/mutex-unreg.log 2>&1 </dev/null &
python3 /tmp/bughunt/g2cd_client.py /tmp/bughunt-mutex.sock "mutex-unlock name=held-mutex"

# P1-2 / P1-3 / P1-4
GO2CPP_REPLAY_TOOL=/tmp/replay-build/replay /tmp/go2cpp-cli replay play \
  /tmp/bughunt/replay/minimal.g2cr; echo $?
GO2CPP_MIGRATE_TOOL=$PWD/build-migration/migrate /tmp/go2cpp-cli migrate restore \
  /tmp/snap_v3.g2c; echo $?
/tmp/replay-build/replay info /tmp/bughunt/replay/gidmax.g2cr; echo $?

# P2-1 / P2-2
python3 /tmp/bughunt/test_debug.py /tmp/bughunt-debug.sock

# P2-7 / P2-8 / P3-8
python3 /tmp/bughunt/tamper_migration.py /tmp/snap_v3.g2c /tmp/bughunt/mig
python3 /tmp/bughunt/run_mig.py /tmp/bughunt/mig/*.g2c

# P1-5 / P2-9
python3 /tmp/bughunt/mig_negative.py
```

## 7. 结论与建议优先级

1. 先修 P1-1（错误解锁）与 P1-2/P1-3（CLI 语义/版本）——它们直接让“安全干预”和“新能力入口”不可信。
2. 给两个解析器加**输入规模上限 + 异常兜底**（P1-4/P1-5），把崩溃转成结构化错误。
3. DebugServer/Doctor 的传输层需要连接/请求上限与超时（P2-1/P2-3/P2-10），否则诊断接口本身是稳定性弱点。
4. CLI 的 `--out` 生命周期改成“临时文件 + 成功后原子替换”，并停止在 exe 之后消费 `--out`（P2-5/P2-6）。
5. 迁移写端改原子写；读端补段布局/整数溢出校验（P2-7/P2-8/P2-9）。
6. 事件环的“线程退出丢事件/Reset 回退/关键区无 dropped 计数”建议在文档中显式列为限制，或增加持久化/汇总计数。

---

附：本次审查未修改工作树源码；仅新增本报告文件。测试进程已停止，测试产物保留在 `/tmp/bughunt/` 供复核。
