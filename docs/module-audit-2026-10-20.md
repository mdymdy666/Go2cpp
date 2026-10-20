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
