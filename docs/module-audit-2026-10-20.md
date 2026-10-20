# 基础模块审计记录（2026-10-20）

> 方法：7 个并行审计 agent，只读源码 + `/tmp` 独立编译测试（不修改仓库源码/CMake、不 commit），
> 链接 `build-work` 的 Release 库；每份报告含缺陷行号、复现命令、期望/实际与压测数据。
> 原始报告：`/tmp/audit_{so_config,os,http2,http3,sse_tls_ws,channel_pc,locks}.md`。

## 1. 验收矩阵（7/7 达标）

| 模块 | 场景/断言 | 结论 | 报告 |
|---|---|---|---|
| SO 热重载 / 配置 | 18 场景 / 389 断言 / 2 失败 | 达标；2 万条 fuzz 无崩溃 | `audit_so_config.md` |
| OS（file/dir/config） | 8 场景 / 307 断言 | 达标；唯一失败为刻意暴露的契约缺陷 | `audit_os.md` |
| HTTP2 | 7 场景 / 50,244 断言 / 0 失败 | 达标；20 个一致性探针全部命中缺陷 | `audit_http2.md` |
| HTTP3 / QUIC | 构建 + 端到端 + 10 万 datagram | 达标；真集成、可用、不可生产 | `audit_http3.md` |
| SSE / TLS / WebSocket | 24 场景 / 2,615 断言 | 达标；唯一失败为 TLS 证书缺陷 | `audit_sse_tls_ws.md` |
| Channel / ParkingCondition | 100 万消息 + 随机 close | 达标；0 崩溃 0 死锁 | `audit_channel_pc.md` |
| 锁原语（Mutex/RWMutex/RecursiveMutex） | 2.24e8 操作 / 823 超时 | 达标；正确性 0 失败，2 个 P1 性能缺陷 | `audit_locks.md` |

## 2. P0 / P1 缺陷（按修复优先级）

### P0

1. **TLS 不在发布物**：`src/network/http2/tls_transport.cpp` 不在任何构建 target（`cmake/Go2CppModules.cmake:43-50`），`libgo2cpp_network.so` 0 符号；发布物没有 TLS。
2. **HTTP3 析构不发 CONNECTION_CLOSE**：`picoquic_connection.cpp:142-149`、`client.cpp:340-344`；300 次建连仅 256 成功，服务端槽位泄漏。
3. **HTTP3 5s 超时失效**：`client.cpp:614-628/673-717`、`picoquic_connection.cpp:285`；阻塞 Read 8s 实测 8.000s。
4. **HTTP3 空指针 SEGV**：`ReadStream/WriteStream` 未判空（`cpp:361/337`，ASan 确认）。
5. **HTTP3 verify_peer 无效**：`cpp:185-203`、`client.cpp:561-562`；自签名证书仍握手成功。

### P1

6. **Mutex 稳态 FIFO 锁死**（公共底座）：`sync.cpp:1097-1113/1281-1347`；8 线程从 250ns/op 退化到 **35µs/op（~1400×）**，同机 `std::mutex` 稳定 25ns。
7. **RWMutex 写路径**：`rwmutex.hpp:166-167/197-202`、`sync.cpp:1495-1506`；每次写插 `std::set` + 无条件 NotifyAll，4 线程 10000 次 **2.03ms/op、27.3 futex/op**。
8. **Select + DoneCase 取消竞态**：`channel.cpp:272-285/344-346/401-410`；8%~19% 返回 `selected=false` 而非 DoneCase。
9. **ParkingCondition notify_one 丢唤醒**：`parking_condition.cpp:135-163/341-371`；多 native waiter 共享 futex 代际字，~0.125%/轮，丢失者睡满 timeout。
10. **HPACK 扩表不同步**：`hpack.hpp:296`、`hpack.cpp:635-642`、`connection.cpp:243-247`；对端设 `HEADER_TABLE_SIZE=8192` 后第二块索引越界，现实互通会 `COMPRESSION_ERROR`。
11. **HTTP2 SETTINGS 忽略**：`connection.cpp:807-819`；非法值（`MAX_FRAME_SIZE=100`、`ENABLE_PUSH=2`）被接受，`MAX_CONCURRENT_STREAMS` 未执行。
12. **SSE Parser O(n²)**：`sse.cpp:373` 每行 `erase(0)`；4MB 整块 238.8ms vs 4KiB 分片 2.5ms（93.7×）。
13. **SO 深依赖链栈溢出**：`so_manifest.cpp:507-538/599-625`；20 万层 manifest → SIGSEGV（外部配置可 DoS）。
14. **OS NUL 截断**：`file.cpp:199/496/553/583/601/619`、`dir.cpp` 11 处；`Setenv("A\0B")` 实际设置 `A`。
15. **OS ReadFile 上限失效**：`file.cpp:507-516/523-538`；`max=64B` 时 `/proc/self/status` 返回 1484B。
16. **OS RemoveAll(".")/("..")**：`dir.cpp:236-310`；清空 cwd/父目录内容。

### P2/P3（详见各报告）

HTTP3 关闭码恒 0 / Poll 无 PTO / 非阻塞 Transport 100% CPU / 响应体无上限；HTTP2 `DecodeInteger` 未定义移位、帧长超限先缓冲、流 id 校验缺失；TLS 无主机名校验、`Duration::max()` 永久挂起、测试脚本悬空；WebSocket 无发送端分片（>16MiB 发不出）；RWMutex 三态返回值假设、RecursiveMutex owner 无 generation；配置 DumpIni 非往返；OS fstat 失败跳过校验、WriteAt+O_APPEND 等。

## 3. 正向压测数据（亮点）

| 模块 | 数据 |
|---|---|
| HTTP2 | 50,244 断言 0 失败；120,000 随机帧 0 崩溃；200,000 轮状态机（2.46M 转换）与参考模型 0 差异；64MiB 流控 325.6MB/s |
| HTTP3 | hq 握手 PASS；h3 GET 200/alpn=h3；32/32 流；1/8/64MB body 精确；ASan 0 错误；10 万 datagram 0 崩溃 |
| Channel | MPMC 100 万消息 0 丢/重/幽灵；close 唤醒 32 Recv/32 Send/32 Select 0~2ms；cap1024 107ns/msg |
| 锁（fiber 路径） | 64 fiber 90/10 = 12.47 Mops/s（80.2ns/op） |
| SSE | 110.5MB/10 万事件 ≈ 994MiB/s；1MiB 单事件；8 并发写者 |
| WebSocket | 11 场景全过（0~16MiB 帧、1009 超帧、12 类非法帧、400 轮 fuzz） |
| OS | 1GiB 写 473.6MiB/s、读 6868MiB/s；10 万文件 create 3.4s / list 28ms；并发 O_APPEND 4000 条无撕裂 |
| SO/配置 | 2 万条 manifest/config fuzz 无崩溃；100MB Fiber transport hash 相等（386MiB/s） |

## 4. 文档漂移（需订正）

- `docs/quic.md`：顶部已订正（2026-10-20）；但 `:37-58/86-102` 仍称 varint/包头/帧层与 94/94 测试存在，实际已删除。
- `docs/network.md:113` 称 TlsTransport 已实测，但当前构建无该 target。
- `so-reload-progress.md` 写 114、`NEXT-STEPS.md` 写 113，实测 ctest **116**。
- `parameter-tuning.md:19` fiber_stack 默认 0（源码 2048）、`:27` fiber_bin 32（源码 128）。
- `docs/design.md` 缺 TLS/WebSocket/SSE/OS 设计章节；`function-annotation-progress.md` 未列 so_manifest/so_reloader/fiber_transport。

## 5. 建议修复顺序

1. **安全/发布物**：TLS 接线（target + CA + hostname）、HTTP3 D01-D04；
2. **公共底座**：Mutex 两态（正常模式插队、>1ms 饥饿）、RWMutex 按角色唤醒 + 0 等待者短路 + 写快路径去分配；
3. **正确性**：Select cancel_probe、ParkingCondition per-node futex、HPACK 扩表同步、HTTP2 SETTINGS/流校验；
4. **资源/DoS**：HTTP3 响应体上限、SO 依赖深度上限、OS NUL 校验 + ReadFile 累计上限 + RemoveAll 拒绝 `.`/`..`；
5. **性能**：SSE 读游标（去 erase(0)）、Select thread-local 随机、notify_all 批量 FUTEX_WAKE(n)。

### 6.2 ParkingCondition D2：notify_one 丢唤醒（2026-10-20 12:42）

- **改动**：`WaitNode` 改为 **per-node futex**（`parking_condition.cpp:116-160` 增加
  `alignas(64) m_futex_seq` + `m_futex_waiters`）；`Wake()` 只推进/唤醒**选中节点**
  自己的代际字（`:142-152`）；`WaitNative` 全部改用节点字段（`:180-292`）。
  根因是所有 native waiter 共享 `m_futex_seq`，`FUTEX_WAKE` 会唤醒任意节点，
  被误唤醒者重新 park，选中节点已摘出队列只能睡到超时。
- **验证**：`ctest` **116/116**；30 轮 pc-lost（4 waiter × 600 轮）= **30/30 轮 lost=0**
  （修复前 4 次×600 轮累计 3 次丢失）；pc-fifo 20 轮 8 waiter mismatch/hung=0；
  pc-timeout 100 轮 max_abs_error 227µs；pc-fiber-notify-all 16/16、notify-one 8/8；
  `audit_channel_pc` 全场景 failures=0。
- **性能**：pc-rtt 23.8µs（修复前 18.9µs，WSL2 噪声内）；channel cap1024 仍 107.5ns/msg。

### 6.3 RecursiveMutex D4：owner token 用 G id 替代 Task 地址（2026-10-20 13:00）

- **改动**：`CurrentToken()` 从 `Task* | 1` 改为 `(Task::id() << 1) | 1`
  （`recursive_mutex.hpp:207-220`）。G id 全局单调不复用，地址复用导致的
  「旧 owner 未解锁退出 → 新 G 继承 owner」不再可能。
- **验证**：`ctest` **116/116**；30 轮 `audit_locks --extra`（递归深度 1-64、
  跨 G 解锁、取消/超时、混合读写）全部 `failures=0`；64 fiber 90/10 仍
  **12.29 Mops/s（81.4ns/op）**。
- **未修（记录）**：D3 RWMutex 的 `ConditionVariable::Wait` 重锁失败路径
  （`relocked=false` 时锁未持有，但 RWMutex 的等待队列清理也必须持锁）。
  安全修复需要把 writer 票据队列改成可在无锁状态清理的结构，触发条件极窄
  （调度器关闭 + 取消 + 重锁失败），留待性能改造时一并处理。


### 6.4 OS 模块缺陷修复（2026-10-20 14:28）

> 工作树 `/UserData/CodexWorkSpace/Go2Cpp-work`；只改 `src/os/**`、`include/go2cpp/os/**`、
> `tests/os/**` 与本文档；不 commit（由父 agent 统一提交）。详细报告：`/tmp/fix_os_final.md`。

**已修（D1-D6、D8-D11、D13）**

| 缺陷 | 修复 | 关键位置 |
|---|---|---|
| D1 NUL 截断 | 新增 `detail::ContainsNul`；全部路径/名字/值入口拒绝内嵌 NUL（`kInvalidArgument`+`EINVAL`；`Getenv` 返回 nullopt） | `file.hpp`、`file.cpp` Open/Getenv/Setenv/Unsetenv、`dir.cpp` 7 个入口 |
| D2 ReadFile 上限失效 | 读循环累计 `total`，超过 `os.max_file_size` 立即清空并返回 `kIoFailed`+`EFBIG`；不再依赖 `st_size>0` | `file.cpp` `ReadFile` |
| D3 OOM 抛异常 | `reserve`/buffer/`append` 包 try/catch，`bad_alloc`/`length_error` → `kIoFailed`+`ENOMEM` | `file.cpp` `ReadFile` |
| D4 RemoveAll(".")/("..") | 最后一段是 `.`/`..`（含 `a/.`、`a/..`）直接 `kInvalidArgument`+`EINVAL`，在删除任何内容之前拒绝 | `dir.cpp` `RemoveAll` |
| D5 WriteAt+O_APPEND | `File::m_append`（Open 按 flags 记录，Adopt 用 `F_GETFL` 只读探测），`WriteAt` 返回 -1/`EINVAL` | `file.hpp`、`file.cpp` |
| D6 SetTimes 文档与实现相反 | 文档改为「跟随符号链接」，补悬空链接 `ENOENT` 说明 | `dir.hpp`、`dir.cpp` |
| D8 fstat 失败跳过类型校验 | fstat 失败关闭 fd 并返回 `kOpenFailed`/errno（与 `Adopt` 对齐） | `file.cpp` `File::Open` |
| D9 MaxFileSize 注释失真 | 注释改为「无 clamp；配置是 int，上限 2 GiB-1」 | `config.cpp`、`os.hpp` |
| D10 O_NONBLOCK 未说明 | `Open` 文档注明 fd 带 `O_CLOEXEC\|O_NONBLOCK`，`Adopt` 不改标志 | `file.hpp` |
| D11 SIGXFSZ 未说明 | `Write`/`WriteAll` 文档注明默认终止进程与忽略/捕获建议 | `file.hpp` |
| D13 out==nullptr 无 errno | file 3 处 + dir 3 处统一 `EINVAL` | `file.cpp`、`dir.cpp` |
| 回归用例 | `test_os_file.cpp` 场景 12、`test_os_dir.cpp` 场景 9（沿用既有 target，不改 CMake） | `tests/os/` |

**验证**

- `ctest --output-on-failure -j4`：**116/116** 通过（`build-work`）。
- 专项断言（`/tmp/fix_os_d1.cpp`、`/tmp/fix_os_d2.cpp`、`/tmp/fix_os_d4.cpp`、
  `/tmp/fix_os_d5.cpp`、`/tmp/fix_os_d8d13.cpp`，独立编译并链接 `libgo2cpp_os.so`）：
  D1 31/31、D2+D3 19/19、D4 12/12、D5 22/22、D8-D13 19/19，共 **103 项 0 失败**。
- 30 轮重复五组专项：`d1/d2d3/d4/d5/d8d13` 失败数均为 **0**。
- 修复前后对比（同一探针对 HEAD 旧源码与修复后库）：

| 探针 | 修复前 | 修复后 |
|---|---|---|
| D1 `Open("base\0evil")` | ok，fd 有效（实际操作截断路径） | `kInvalidArgument`/EINVAL |
| D1 `ReadFile("base\0evil")` | ok，5 字节 `hello` | `kInvalidArgument`，空 |
| D1 `Setenv("A\0B")` | ok（实际设置了 `A`） | `kInvalidArgument`/EINVAL |
| D2 `ReadFile("/proc/self/status", max=64)` | ok，1498 字节 | `kIoFailed`/EFBIG，0 字节 |
| D3 1 GiB + `RLIMIT_AS=512MiB` | 子进程 SIGABRT（`bad_alloc` 穿透） | `kIoFailed`/ENOMEM，进程正常 |
| D4 `RemoveAll(".")` | `kIoFailed`/EINVAL，但子项已删（kept=0） | `kInvalidArgument`/EINVAL，内容完整（kept=1） |
| D5 `WriteAt` on `O_APPEND` | 返回 1，内容 `AAAZ`（追加） | 返回 -1/EINVAL，内容 `AAA` |
| D6 `SetTimes(link)` | 目标 mtime 改变、链接不变（跟随），文档却写「不跟随」 | 行为不变，文档已改为「跟随」 |
| D8 `fstat=EPERM`（seccomp 注入） | `Open` 成功、fd 有效（未校验） | `kOpenFailed`/EPERM，fd 无效 |
| D13 `Open(out=nullptr)` | `kInvalidArgument`，`errno=0` | `kInvalidArgument`，`errno=EINVAL` |

**无法修复 / 需要决策**

1. **D7 `File::Close()` 丢弃 `close(2)` 错误**（P2）：改成返回 `FileStatus` 会破坏现有调用方与 ABI，
   属接口决策。本次只在 `file.hpp` 明确记录「错误被丢弃、需先 `Sync()`」，并建议后续新增
   `FileStatus CloseChecked()`/`Flush()`（不改现有签名）。
2. **D12 目录 API 缺口**（P3）：`Rename`/`Chmod`/`Chdir`/`Getwd`/`MkdirTemp`/`TempDir` 未实现。
   这是接口面扩张（命名、Go 语义对齐、`MkdirTemp` 随机命名规则），不属于本次正确性修复边界；
   `ReadFile` 对目录的拒绝已由 `File::Open` 的 `S_ISREG` 白名单保证，文档已补说明。
3. **D14 文档缺口**（P3）：`docs/design.md` 无 os 设计章节、`docs/state-machines/` 无 Dir 状态机、
   `codex.md` 的 os 章节未覆盖目录/路径/配置。本次只更新本文档与代码内 Doxygen；
   其余章节留待文档重构，避免本次改动膨胀。
4. **环境限制**：无法注入 `ENOSPC/EDQUOT`（无 root/免密 sudo）与 `close(2)` 延迟错误；
   `d_type=DT_UNKNOWN` 不可构造（ext4/tmpfs 都填 `d_type`）。D7 仍缺运行时证据。

## 7. 第二轮 P0/P1 修复（2026-10-20 15:00）

> 基线 HEAD `bd99603`；本轮全部改动未提交（待用户确认）。

### 7.1 scheduler fiber 唤醒丢失（根治）

- 根因：`Task::try_mark_queued()` 持 `m_transition_mutex` 做 check-then-store，`try_mark_queued_from_worker()` 用无锁 CAS，二者线性化点不同 → 同一 G 两个队列节点 → 残留节点 claim 失败时 `clear_queued()` 误清新 reservation → G 永久 `kRunnable` 且无队列节点。
- 修复：`src/concurrency/scheduler.cpp:376-393` 共用 `m_transition_mutex`（仅一处，无 ABI 变化）。
- 验证：标准复现 12 路×30 轮 **7/12 → 0/24**（720 轮全 DONE）；隔离 6×500 / 6×1000 **0/6、0/6**；ctest 116；串行 `--extra` 30/30；性能 +0.7%/+1.7%（噪声内）。

### 7.2 RWMutex D3 + scheduler 残余

- D3：`ConditionVariable::Wait` 的 `relocked=false` 路径改为无 park `TryLock` 抢回（8192 预算），抢不回抛 `std::runtime_error`（明确 mutex NOT held）；RWMutex 四条等待路径捕获并安全清理，`m_waiting_readers` 原子化。
- scheduler 残余：三条 dequeue 路径 claim 失败改 `recover_unclaimed`（自愈），`enqueue` 补 `clear_queued`。
- 验证：ctest 116（两次）；hybrid 6 路×25 轮 0/150；D3 专项（自定义 WaitBackend）旧库 terminate → 新库异常+计数+可复用。
- 需决策：`AbortAfterLockLoss` 的 `recovered=false` 兜底分支保留写者票据（实际不可达；彻底移除会牺牲 D2 写路径零分配）。

### 7.3 OS 模块（11 项）

D1 NUL 截断→EINVAL、D2 ReadFile 上限（循环累计/EFBIG）、D3 OOM→FileStatus、D4 RemoveAll 拒绝 `.`/`..`、D5 WriteAt+O_APPEND→EINVAL、D6 SetTimes 文档、D8 fstat 失败关 fd、D9/D10/D11 文档、D13 `out==nullptr`→EINVAL。
验证：103 断言、fuzz 10 万、30 轮、3 个 OS ctest 全绿。

需决策：
- D7 `Close()` 丢弃 close(2) 错误 → 建议新增 `CloseChecked()`（API/ABI 变更）；
- D12 缺 `Rename/Chmod/Chdir/Getwd/MkdirTemp`（接口扩张，需产品决策）；
- D14 design.md/Dir 状态机/codex.md 文档缺口。

### 7.4 HTTP2（20/20 探针）

PUSH_PROMISE/ENABLE_PUSH、`:scheme` 强制、MAX_HEADER_LIST_SIZE、流表清理（`max_retired_streams=256`）、MAX_CONCURRENT_STREAMS 默认 true、帧长超限发 GOAWAY(FRAME_SIZE_ERROR)。
验证：20/20 探针、checks=50244 fails=0、30 轮 0 崩溃、ASan/UBSan 干净、ctest 116。

需决策：
- `:scheme` 默认仅对带 `:authority` 请求强制（`test_connection` 场景 8/case3 依赖宽松；`strict_scheme` 可完全严格但需改测试）；
- 客户端请求方向 header 上限默认关闭（case2 两端同 config 冲突），建议改测试后默认 true；
- 服务端收到客户端 `ENABLE_PUSH=1` 按 RFC 接受（字面报错会破坏互通）；
- 未自动发 431；迟到帧水位有极低误判；draining 仍用连接错误。

### 7.5 TLS / 配置

- close_notify：新增公开 `Close()`，析构先 `SSL_shutdown`（WANT_WRITE 重试，预算 min(timeout,500ms)），`m_shutdown_sent` 只发一次；实测服务端 `SHUTDOWN_STATE=0x2`；附带修 SIGPIPE。
- 错误查询：公开 `LastError()/LastErrorString()`（SSL_get_error + ERR 队列 + verify 原因）。
- 配置 D3：`\xHH` 双向转义（反斜杠/`#`/`;`/控制字符/首尾空格）；专项 44 项 + 1000 条随机 0 不一致。
- `verify_peer` 默认 false → **true**（安全默认）。

需决策：
- `verify_peer` 默认 true 为破坏性安全变更（依赖旧默认可连自签名服务者会 fail-closed），发布说明需写明；
- 手写值里的字面 `\xHH` 会被解码（可改 `%HH` 前缀）；
- 键名含 `=`/`#`/`;`/首尾空格仍不能往返；
- SIGPIPE 未覆盖 Read 内部写。

### 7.6 HTTP3 / QUIC

D09 复核 + 端到端（application_close(7)、stream_reset(0x1234)）；D15 host/port/SNI/authority 拆分（11 组 PASS）；D16 控制流（SETTINGS/GOAWAY/MAX_PUSH_ID、QPACK 7/11 流、push 拒绝、GOAWAY 后 Send 拒绝）；D18 回调 try/catch（bad_alloc 注入不 terminate）；D20 docs/quic.md 重写；D21 剩余项。
验证：29/29、30 轮 0 崩溃、300×3 建连、ctest 116、-Werror 零警告。

需决策：
- D19 仓库内无 http3/quic ctest（cmake/tests 不在 agent 边界，验收测试在 /tmp/fix_http3）；
- `docs/quic-lib-evaluation.md`、`quic-go-reading-guide.md` 过时；
- POST content-type 固定 text/plain（重复风险）；
- QPACK 动态表/push/0-RTT/迁移为设计边界。

### 7.7 RWMutex 死锁归因（结论）

- 逐行审查：等待-唤醒配对完整（写者三条、读者两条路径，写者>1 广播）。
- 当前版 6 路×30 + 4 CPU burner 300s → **0/180**；HEAD 原版同条件 0/180。
- 反证：HEAD 原版在 hybrid 复现器下 2/75、5/180 卡死（现场锁全空闲、scheduler 停摆）→ 属 HEAD 的 scheduler 唤醒丢失（已由 7.1 修复）。
- 结论：**无证据表明 RWMutex 改动引入死锁**。

### 7.8 验证汇总

- `ctest`：**116/116**（本轮修复后，含 go2cpp_connection_test / http2_stress / OS 测试）；
- 各模块专项/压测数据见 7.1-7.7。

### 7.9 未修/需决策总表

| 项 | 归属 | 建议 |
|---|---|---|
| `CloseChecked()` | OS | 新增 API，不破坏现有签名 |
| `Rename/Chmod/Chdir/Getwd/MkdirTemp` | OS | 产品决策后补 |
| `:scheme` 严格模式 | HTTP2 | 改 `test_connection` 场景 8/case3 后开 |
| 请求方向 header 上限 | HTTP2 | 改 case2 测试后默认 true |
| 431 自动发送 | HTTP2 | 上层决定 |
| verify_peer 默认 true 的发布说明 | TLS | 发布前写 breaking change |
| 字面 `\xHH` 兼容 | 配置 | 可改 `%HH` |
| 键名往返 | 配置 | 需要新的键转义规则 |
| http3/quic ctest | HTTP3 | 需放宽 cmake/tests 边界 |
| QPACK 动态表/push/0-RTT/迁移 | HTTP3 | 设计边界 |
| `AbortAfterLockLoss` 兜底分支 | 并发 | 实际不可达，保留 |
| fiber SIGSEGV handler 覆盖/async-signal-safe | Fiber | 未排期（P2） |
| `worker_loop` 门槛 `runnable>P` 注释不符 | scheduler | 性能项，未改 |

## 8. 发布注意事项（Breaking Changes 与行为变化）

> 本节面向升级者，列出第二轮修复中**默认行为发生变化**的项。

### 8.1 默认值/行为变更（升级需检查）

| 变更 | 影响 | 建议 |
|---|---|---|
| **TLS `verify_peer` 默认 false → true** | 依赖旧默认可连自签名/无证书服务端的代码会 fail-closed | 发布说明写明 breaking change；需要旧行为的调用显式传 `verify_peer=false` |
| **HTTP2 `MAX_CONCURRENT_STREAMS` 默认执行** | 超过对端通告值的流返回 `REFUSED_STREAM`（此前全部接受） | 依赖「只发不收」压测的代码需处理拒绝 |
| **HTTP2 `:scheme` 强制** | 带 `:authority` 的请求缺少/非法 `:scheme` 会被拒 | 客户端已自动补 `http`；自定义客户端需带 `:scheme` |
| **HTTP2 帧长超限先发 GOAWAY** | 超限帧不再直接断连 | 无需改代码，行为更符合 RFC |
| **配置值中的字面 `\xHH`** | 会被解析为对应字节（此前按字面保留） | 需要字面 `\x` 的值请用 `\\x` 转义 |
| **OS 路径/环境变量含 NUL** | 返回 `EINVAL`（此前静默截断） | 检查是否有依赖截断的调用 |
| **`RemoveAll(".")`/`("..")`** | 返回 `EINVAL`（此前清空 cwd/父目录） | 修正误用 |
| **`WriteAt` 在 O_APPEND 句柄** | 返回 `EINVAL`（此前忽略 offset 追加） | 改用 `Write` 或非 O_APPEND 句柄 |
| **`ReadFile` 超过 `max_file_size`** | 返回 `EFBIG`（此前 procfs 等可绕过） | 调大配置或分块读 |

### 8.2 待决策项（默认未启用/未改）

| 项 | 现状 | 启用条件 |
|---|---|---|
| HTTP2 `:scheme` 完全严格（`strict_scheme`） | 默认仅对带 `:authority` 的请求强制 | 修改 `tests/network/test_connection` 场景 8/case3 后可开 |
| HTTP2 请求方向 header 上限 | 默认关闭（服务端响应方向始终检查） | 修改 `test_http2_stress` case2（两端同 config=100）后可默认 true |
| HTTP2 431 自动发送 | 未自动发，由上层处理 | 上层 HTTP 服务器实现 |
| OS `CloseChecked()` | 未提供，`Close()` 仍丢弃 close(2) 错误 | 新增 API（不破坏现有签名） |
| OS `Rename/Chmod/Chdir/Getwd/MkdirTemp` | 未实现 | 产品决策 |
| http3/quic ctest | 仓库内无（验收测试在 `/tmp/fix_http3`） | 放宽 `cmake/`、`tests/` 边界后迁入 |

### 8.3 已知未修（不阻塞发布）

- `fiber.cpp` growable 栈覆盖进程 SIGSEGV handler、不保存旧 handler、handler 内 `mprotect` 非严格 async-signal-safe（P2）；
- `worker_loop` 本地快路径门槛 `runnable > P` 与注释不符（性能项）；
- `wake_io_batch` 入队失败静默、stale wake token（P3，`Await` 已容忍）；
- `AbortAfterLockLoss` 的 `recovered=false` 兜底分支保留写者票据（实际不可达）。

### 8.4 验证基线

- `ctest` **116/116**；
- scheduler 唤醒丢失：卡死 7/12 → **0/24**（720 轮）；
- HTTP2：20/20 探针、checks=50244；
- HTTP3：29/29 acceptance、300×3 建连；
- OS：103 断言、fuzz 10 万、30 轮；
- 并发性能：Mutex 两态 latch 8t 24~55µs→18.3~19ns、PC pc-rtt 32.1→3.4µs、Channel cap0 30.3→1.19µs、RWMutex alloc 3.00→0.02。
