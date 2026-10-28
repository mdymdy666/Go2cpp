# Bento P1 三模块测试充分性审查（session_control / peer_hub secure / log_hook）

> 基线：`1e08723`（`feat(bento): 场景缺口补齐（控制权仲裁/多 peer 加密/日志钩子）`）
> 审查对象（只读，不改代码）：
> - `tests/bento/test_session_control.cpp`（417 行）↔ `include/go2cpp/bento/net/session_control.hpp`（228 行）+ `src/bento/net/session_control.cpp`（468 行）
> - `tests/bento/test_peer_hub_secure.cpp`（627 行）↔ `include/go2cpp/bento/net/peer_hub.hpp`（338 行）+ `src/bento/net/peer_hub.cpp`（1603 行）
> - `tests/bento/test_log_hook.cpp`（369 行）↔ `include/go2cpp/bento/net/log_hook.hpp`（275 行）
> 工作树状态：`src/bento/net/peer_hub.cpp` 相对基线仅文件模式变化（`git diff --stat` 为 `0 insertions / 0 deletions`），内容与基线一致；三份测试文件均无未提交改动。
> 环境：WSL2 Ubuntu 24.04 / g++ 13.3.0 / CMake 3.28.3 / Ninja。
> 审查方式：逐公开 API 对照用例 + 只读实现核对 + 实跑三轮 ctest + 3 个变异实验（在 `/tmp` 副本进行，工作树零改动）。

## 0. 结论摘要

三份测试都不是冒烟测试：断言密度分别为 **14816 / 506 / 124** 次检查，大量断言校验消息内容三元组、事件字段（from/to/reason/generation）、精确计数与交付顺序，不是「只断言返回 OK」。实跑三轮全绿，耗时稳定（0.16 s / 3.78 s / 4.01 s），无 SKIP 逃逸。

但覆盖仍有结构性空洞，统计为 **P0×2 / P1×5 / P2×9 / P3×1**：

| 模块 | 用例 / 检查数 | 功能覆盖 | 主要空洞 |
|---|---|---|---|
| session_control | 8 / 14816 | 公开 API 20/24 有至少一条用例 | `Tick(now_ms)` 零调用、租约边界未测、并发只跑一次抢控 |
| peer_hub secure | 6 / 506 | secure 相关 API 覆盖较全，基础 API 依赖 `test_peer_hub.cpp` | 重放/篡改零覆盖、加密分片零覆盖、无并发发送、握手幂等与重连未测 |
| log_hook | 7 / 124 | 公开 API 11/14 覆盖，计数/并发断言强 | 越界枚举钳位未测、PeerHub 事件接入点 8/14 未验证、sink 重入未测 |

最严重的三条（建议优先补）：**PH-01 加密数据帧重放/篡改**、**PH-02 加密模式下 STREAM 分片/大载荷**、**SC-01 `Tick(now_ms)` 注入时钟与租约边界**。

## 1. 运行与变异证据

### 1.1 构建与实跑

```bash
cmake -S . -B build-review -G Ninja -DGO2CPP_WITH_BENTO=ON -DGO2CPP_BUILD_TESTS=OFF
cmake --build build-review --target bento_test_session_control bento_test_peer_hub_secure bento_test_log_hook
ctest --test-dir build-review -R 'bento_(session_control|peer_hub_secure|log_hook)' --output-on-failure
```

三轮结果（`build-review`，测试编号 36–38）：

| 轮次 | session_control | peer_hub_secure | log_hook | 总计 |
|---|---:|---:|---:|---:|
| 1 | 0.16 s | 3.78 s | 4.01 s | 7.96 s |
| 2 | 0.16 s | 3.79 s | 4.01 s | 7.95 s |
| 3 | 0.16 s | 3.78 s | 4.01 s | 7.94 s |

基线数字（直接运行二进制的 `[INFO]` 输出，可作为后续回归对照）：

- session_control：`concurrent: peers=16 winners=1 denials=15`；`randomized: steps=4000 grants=526 requests=87`；`[PASS] 14816 checks`。
- peer_hub_secure：`secure_3peers: peers=3 stream=90 dgram=60 opened=153 sealed=33`；`wrong_psk: auth_failures=5 client_state=timed_out`；`plaintext_hello_rejected=2`；`per_peer_psk: a/b handshakes=1 rx=10/10`；`pto-window: retransmits=32 sealed=75`；`secure_retransmit: retransmits=32 sealed=245 opened=245 rx=200`；`[PASS] 506 checks`。
- log_hook：`concurrent_emit: emitted=4000 delivered=4000`；`handshake_retry: count=7`；`retransmit_events: retransmit=5 loss=1 rx=20`；`timeout_auth_events: timeout=1 auth_failure=7`；`[PASS] 124 checks`。

### 1.2 变异实验（验证断言是否真能抓 bug）

在 `/tmp/mut_*` 副本上改实现、重编译、跑原测试；工作树未改动。

| 编号 | 变异 | 预期 | 实际 | 结论 |
|---|---|---|---|---|
| A | `Tick` 租约判断 `now_ms < deadline` 改为 `now_ms <= deadline`（恰好到期不回收） | 测试应失败 | **仍 PASS（14816 checks）** | 租约边界 `now == deadline` 未被任何用例覆盖（SC-01） |
| D | `max_participants` 越限判断 `>=` 改为 `>` | 测试应失败 | FAIL 2 条（`test_session_control.cpp:311-312`） | 上限断言有效 |
| E | `LogHook::LevelIndex/EventIndex` 去掉越界钳位（`return raw`），加 ASAN | 测试应失败或 ASAN 报错 | **仍 PASS（124 checks）** | 非法枚举钳位零覆盖（LH-01） |

三个变异说明：核心断言（上限、唯一控制器、内容校验）确实能抓 bug；但**边界值与防御性分支**存在「改了也不报警」的盲区。

## 2. 覆盖矩阵

图例：✅ 有调用且有能抓 bug 的断言；⚠️ 有调用但断言不足/只覆盖部分分支；❌ 零用例。

### 2.1 SessionControl（`test_session_control.cpp`）

| 公开 API / 分支 | 用例位置 | 边界与错误输入 | 判定 |
|---|---|---|---|
| `ControlModeName` / `ControlChangeReasonName` | `:334-337` | 只测 `view`、`lease_expired`；其余 11 个枚举值与 `unknown` 分支未测 | ⚠️ |
| `ControlSnapshot::ToJson` | `:254-258` | 只查 4 个字段子串；空 `participants`、`lease_remaining_ms`、`interact` 字符串未测 | ⚠️ |
| 构造器配置归一（`max_participants=0→1`、`default_mode=kNone→kView`） | 无 | 实现 `session_control.cpp:75-84` 的两个钳位分支零覆盖 | ❌ |
| `Join` | `:73-75, 281-283, 306-312, 318-321` | `0`/重复/上限已测；上限拒绝后 `generation`/`stats` 不变未断言 | ✅ |
| `Leave` | `:287`（仅不存在）、`:360`（随机） | 未断言 `kLeft` 事件、控制器离场时 `from=kInteract→to=kNone`、`controller=0` | ⚠️ |
| `OnPeerDisconnected` | `:112-137` | 事件 reason/from/to、`disconnects` 计数均已断言 | ✅ |
| `Grant(peer)` | `:160, 295-296, 303` | 未断言 `kGranted` 事件与 `grants` 计数，也未测解除 Revoke 的事件语义 | ⚠️ |
| `Grant(peer, mode)` | `:297-303, 378-380` | `kInteract` 抢占路径无事件顺序断言；`kView` 显式授予无变更事件断言；`Grant(0)` 未测 | ⚠️ |
| `Revoke()` | `:166-170` | 已测撤销当前控制器、无控制器返回 false、被撤销者不能再请求 | ✅ |
| `Revoke(peer)` | `:147-159, 289` | 幂等（第二次调用返回 true 且不重复回调/计数）未测；对非控制器 peer 置 `revoked=true` 未测；`Revoke(0)` 未测 | ⚠️ |
| `RequestControl` | `:87-101, 117, 133, 144, 152, 162, 169, 290-291, 368` | 幂等请求（同一控制器再次请求 = 续租、不产生事件）未测；`requests` 对未加入/被拒请求也计数（实现 `:304` 先加后查）未断言 | ⚠️ |
| `Renew` | `:292, 299, 385` | 只断言返回值；未用注入时钟验证租约真的被延长；`lease_ms=0` 时返回 false 未测 | ⚠️ |
| `mode` / `is_controller` / `revoked` / `controller` | 多处 | `mode(99)`、`is_controller(99)`、`revoked(1)` 已测；`is_controller(0)`、`revoked(99)` 未测 | ✅ |
| `has_controller` | 无 | 整个测试文件零调用 | ❌ |
| `participants` | `:126, 395` | 只断言数量/上限，未断言按 `peer_id` 升序 | ⚠️ |
| `Snapshot` | `:244-253` | 只测无控制器、单参与者、revoked 场景；`lease_remaining_ms` 非零、多参与者排序、`controller=true` 标志未测 | ⚠️ |
| `generation` / `stats` | `:260-265` | 固定 3 步路径精确断言；未断言 `generation == stats.changes` 守恒（随机 4000 步路径无此校验） | ⚠️ |
| `Tick()` | `:184-190, 330` | 已测过期回收、未过期不回收、`lease_ms=0` 不回收；边界 `now == deadline`、无控制器分支未测 | ⚠️ |
| `Tick(now_ms)`（注入时钟） | **无** | 公开 API 零调用；变异 A 证明边界漏检 | ❌ |
| `on_change` 回调 | `:120-121, 204-205, 229-230, 268-275` | 已测锁外重入查询不死锁；回调内执行写操作（如 `Join`）、回调抛异常未测 | ✅ |
| 并发 16 线程抢控 | `:70-109` | 单次起跑屏障；只调用 `RequestControl`，无 `Join/Leave/Grant/Tick` 混合并发 | ⚠️ |
| 随机操作 4000 步 | `:341-403` | 单线程、单种子；每步检查不变式，但无统计守恒、无并发随机 | ✅ |

### 2.2 PeerHub（PSK 加密部分，`test_peer_hub_secure.cpp`）

> 说明：非加密基础路径（`AddPeer` 参数校验、`RemovePeer`、伪造 `conn_id`、peer 超时隔离、吞吐）由 `test_peer_hub.cpp` 覆盖，本表只标注 secure 测试是否触及；未触及的 API 在本文件中仍算缺口（回归时 secure 模式可能破坏这些路径）。

| 公开 API / 分支 | 用例位置 | 边界与错误输入 | 判定 |
|---|---|---|---|
| `Open`（PSK 非法长度） | `:557-562` | 31B 被拒且错误码含 `"32"`；`pre_shared_key` 为空的明文模式已测 | ✅ |
| `Close` | 无 | 幂等、加密 peer 清理、`secure_ready` 复位均未测（仅析构隐式调用） | ❌ |
| `AddPeer`（加密） | `:153, 357, 383, 394, 450-451, 502, 583, 605` | 正常路径；未测加密 peer 在握手完成前 `SendStream` 必须失败（`peer_hub.cpp:400-414`） | ⚠️ |
| `Connect`（重连） | 无 | 重连换 `conn_id`、重置 nonce/密钥/接收窗口（`peer_hub.cpp:518-538`）零覆盖 | ❌ |
| `RemovePeer`（加密） | 无 | 移除后 `peer_state == kClosed`、`peer_secure_ready=false`、`Peers()` 清空未测 | ❌ |
| `HasPeer` / `peer_state` / `peer_connected` | `:365, 253, 401-402, 470` | `peer_state` 只在错误 PSK 下测 `kTimedOut`；`HasPeer` 零调用 | ⚠️ |
| `Peers` / `PeerIds` | `:261-266, 364, 405` | 已断言 `secure` / `secure_ready` 字段；`peer.secure` 与 `secure_stats` 一致性好 | ✅ |
| `RunOnce` / `RunFor` | 所有场景通过 `PumpHubs/PumpFor` 间接触发 | `RunFor` 未直接调用；`RunOnce` 的 `kMaxFramesPerRun=256` 上限未测 | ✅ |
| `SendStream` | `:278, 434, 480-481, 530, 612` | 全部 48 B/32 B 小包；大包分片、空包、超限包未测 | ⚠️ |
| `SendDatagram` | `:285` | 20 条/peer、48 B；大包/空包未测 | ⚠️ |
| `TrySendStream` | 无 | 加密模式下 `kWouldBlock` / `kError` 分支零覆盖 | ❌ |
| `SendBarrier` | 无 | 加密模式因果屏障零覆盖 | ❌ |
| `SetPreSharedKey` | `:564-570, 598-610` | `nullptr`、31B、32B、已连接明文 peer 不热升级均已测；`bytes=0`、未连接 peer 被重启握手未测 | ✅ |
| `SetPeerPreSharedKey` | `:452-453, 572-573, 586` | 不存在 peer、16B、已建立会话返回 false、正常设置已测；`nullptr`、未连接 peer 设置成功率未测 | ✅ |
| `secure` / `peer_secure` / `peer_secure_ready` | `:145, 152, 270, 366, 473-474, 565, 576, 593-594` | 已测配置前/后、连接前后；`peer_secure(99)` / `peer_secure_ready(99)` 未测 | ✅ |
| `secure_stats` | `:319-325, 360-367, 475-476, 535-552` | 只验证 `handshakes/packets_opened/sealed/bytes/auth_failures/plaintext_rejected`；`replay_drops`、`malformed_frames`、`handshake_retries` **从未断言为非零** | ⚠️ |
| `SetLogHook` / `log_hook` | `test_log_hook.cpp:190-208` | 注入、指针回读、`nullptr` 卸载均测 | ✅ |
| `reliable_stats` / `unreliable_stats` | `:534, 550` | 只读 `retransmits`；不存在 peer 返回默认值、`unreliable_stats` 零调用 | ⚠️ |
| `stats` | `:249, 328-329, 363-364, 401, 468-470, 522` | `peers_active/handshakes/invalid_frames` 有断言；`unknown_peer_frames`、`conn_id_conflicts`、`peers_rejected` 未测 | ⚠️ |
| `local_port` / `is_open` | `local_port` 多处使用；`is_open` 无 | `is_open` 在 secure 测试零断言 | ⚠️ |
| 回调 `on_stream` / `on_datagram` | `:169-226, 396, 459-464, 507-520, 603` | 内容、通道、peer 隔离、顺序均校验 | ✅ |
| 回调 `on_peer_connected` / `on_peer_disconnected` | 无 | secure 连接建立/断开事件零覆盖 | ❌ |
| 握手失败/错误 PSK | `:340-371` | `auth_failures>0`、`handshakes=0`、`peers_active=0`、客户端 `kTimedOut` 已测 | ✅ |
| 明文降级 | `:374-436` | 明文 HELLO 被拒、明文 STREAM 注入被拒、合法加密流量仍可用，均已测 | ✅ |
| per-peer 密钥 | `:439-492` | 两个不同 PSK 服务端、双向各 10 条已测 | ✅ |
| 重传恢复 | `:495-554` | PTO 重传、200 条按序全量、`packets_sealed>200` 已测 | ✅ |
| 重放 / 篡改 | 无 | `replay_drops`、数据帧 `auth_failures` 分支（`peer_hub.cpp:1152-1175`）零用例 | ❌ |
| 时间戳窗口 | 无 | `handshake_replay_window_ms` 与 `TimestampAcceptable`（`peer_hub.cpp:773-780`）零用例 | ❌ |
| HELLO 幂等重发 / 缓存 ACK | 无 | `handshake_retries` 与 `cached_hello_ack` 重发（`peer_hub.cpp:1311-1320, 1386-1407`）零用例 | ❌ |
| 大数据 / 分片加密 | 无 | 所有载荷 48 B，未跨越 1200 B 分片阈值（`channel.hpp:547`） | ❌ |
| 并发发送 | 无 | 4 线程以上并发 `SendStream`（加密 tx seq/窗口）零覆盖 | ❌ |

### 2.3 LogHook（`test_log_hook.cpp`）

| 公开 API / 分支 | 用例位置 | 边界与错误输入 | 判定 |
|---|---|---|---|
| `LogLevelName` / `LogEventName` | `:335-356` | 遍历全部 5 级别 + 14 事件，断言非空、非 `unknown`、去重后数量不变 | ✅ |
| 名称的 `unknown` 分支 | 无 | 传入越界枚举未测（与 LH-01 同根） | ❌ |
| `LogHook()` 默认构造 | 所有用例 | 默认 `min_level == kInfo` 已断言 | ✅ |
| `LogHook(Sink, min_level)` | 无 | 带初始 sink/级别的构造函数零覆盖 | ❌ |
| `SetSink` / `has_sink` | `:90-94, 129-130` | 注入、卸载、指针语义已测 | ✅ |
| `SetMinLevel` / `min_level` | `:95, 127` | 只测 `kWarn` 阈值；`kTrace`/`kDebug`/`kError` 作为阈值未测 | ⚠️ |
| `Emit` | `:84, 96-98, 151` | 无 sink、低于阈值、`value`、`message`、`timestamp>0` 已测 | ✅ |
| `Emit` 的 `message == nullptr` | 无 | 文档明确允许 `nullptr`（`log_hook.hpp:139`），零用例 | ❌ |
| `Trace` / `Debug` | `Debug` 经 PeerHub（`:234, 253`）；`Trace` 无 | `Trace` 便捷入口零调用 | ⚠️ |
| `Info` / `Warn` / `Error` | `:96-98, 131, 151` | 已测（含过滤与回调） | ✅ |
| `Stats` / `Count(level)` / `Count(event)` | `:111-120` | 精确计数、按级别/事件分桶均测 | ✅ |
| `ResetStats` | `:122-127` | 清零且不影响 sink/级别已测 | ✅ |
| 越界枚举钳位（`LevelIndex`/`EventIndex`） | 无 | 变异 E：去掉钳位 + ASAN 仍 PASS，说明防御分支零覆盖 | ❌ |
| sink 内重入 `Emit` | 无 | 文档承诺「sink 里可以再次 Emit」（`log_hook.hpp:18-21`），零用例 | ❌ |
| 并发 `Emit` | `:136-167` | 8 线程 × 500 条，`emitted` 与 `delivered` 精确 4000 | ✅ |
| 并发 `SetSink` / `SetMinLevel` | 无 | 运行期改配置与 Emit 竞争未测 | ❌ |
| PeerHub 事件接入：`kPeerConnected` | `:170-211` | 双端各 1 次，`peer_id` 校验 | ✅ |
| `kHandshakeRetry` | `:214-243` | 死端口重传，`>0` | ✅ |
| `kRetransmit` / `kPacketLost` | `:246-285` | PTO 场景下均 `>0`，并验证恢复后全量 | ✅ |
| `kPeerTimeout` / `kAuthFailure` | `:288-332` | 超时、错误 PSK 均 `>0` | ✅ |
| `kPeerDisconnected` | 无 | `DropPeer`/`RemovePeer` 的断开事件未验证 | ❌ |
| `kHandshakeFailed` | 无 | 握手超上限/参数非法分支未验证 | ❌ |
| `kPlaintextRejected` / `kReplayDrop` / `kFrameRejected` | 无 | 三者在 `peer_hub_secure` 里只验证了计数，未验证日志事件接入 | ❌ |
| `kSendError` / `kRecvError` | 无 | 只在单元层验证了 `LogHook` 计数，未验证 PeerHub 接入点 | ❌ |

### 2.4 覆盖统计

| 模块 | ✅ | ⚠️ | ❌ |
|---|---:|---:|---:|
| SessionControl（24 项） | 8 | 12 | 4 |
| PeerHub secure（27 项） | 11 | 9 | 7 |
| LogHook（22 项） | 10 | 2 | 10 |

## 3. 缺口清单

| ID | 级别 | 模块 | 缺失场景 | 建议补的用例 |
|---|---|---|---|---|
| PH-01 | P0 | peer_hub_secure | 加密数据帧**重放**与**篡改**零用例：`replay_drops`、数据帧路径的 `auth_failures` 永远为 0，AEAD 重放窗口与篡改检测实际未被测试保护（对应 `peer_hub.cpp:1152-1175`） | 新增 `TestSecureReplayAndTamper`：建立加密会话后用原始 UDP socket 复制一帧密文原样重发 → 第二次 `replay_drops>=1` 且 `on_stream` 不重复交付；再翻转密文中间 1 字节重发 → `auth_failures>=1`、`packets_opened` 不增、后续合法帧仍可达（连接不被误杀）；最后断言 `secure_stats` 各分桶互不污染 |
| PH-02 | P0 | peer_hub_secure | 加密模式下的 **STREAM 分片/重组**与**大载荷**零覆盖：所有载荷 48 B，远低于 `kStreamFragmentBytes≈1200`（`channel.hpp:547`），加密外层多 32 B 后的分片预算、`fragments_*` 统计、乱序分片重组都未被验证 | 新增 `TestSecureFragmentedStream`：依次发 1200 B（边界）、1201 B（跨分片）、4 KB、120 KB（接近 `kMaxStreamFragments`）四档；断言接收端按序内容一致、`fragments_sent/received>0`、`fragments_invalid==0`、`duplicates_dropped==0`；再故意丢弃首片验证缺口不误交付；最后断言 `packets_sealed` 与分片数对齐 |
| SC-01 | P1 | session_control | `Tick(now_ms)` 公开 API 零调用；租约边界 `now == deadline` 未测（变异 A：改成 `<=` 仍 PASS）；`TestLeaseExpiry` 依赖 `sleep_for(150ms)`，既有固定时延又有时序依赖 | 新增 `TestLeaseBoundaryInjectedClock`：用 `Tick(now)` 注入——`deadline-1` 不回收、`deadline` 恰好回收、再次 `Tick(deadline)` 返回 false；断言事件 `reason=kLeaseExpired`、`from=kInteract`、`to=kView`、`controller=0`、`generation+1`、`lease_expirations=1`；同时断言 `Snapshot().lease_remaining_ms` 在 `deadline-10` 时精确为 10 |
| SC-02 | P1 | session_control | 并发只覆盖「16 线程同时 `RequestControl` 一次」；`Join/Leave/Grant/Revoke/Tick/OnPeerDisconnected` 的并发混合、回调并发执行零覆盖 | 新增 `TestConcurrentMixedOperations`：8 线程 × 2000 轮、每线程固定 peer 段，随机执行全部 8 类操作；每轮用一次 `Snapshot()`（原子快照，见 Q4）检查「最多一个 `kInteract`」；结束断言 `generation == stats.changes`、`participants().size() <= max`、无 peer 同时 `controller` 且 `mode!=kInteract`；该用例在 TSAN 构建下再跑一次 |
| PH-03 | P1 | peer_hub_secure | HELLO_ACK 丢失后的**幂等重传**（`handshake_retries`、`cached_hello_ack`）与**时间戳窗口**（`TimestampAcceptable`）零覆盖 | 新增 `TestSecureHandshakeRetryAndTimestamp`：用 `fault_proxy.hpp` 或原始 socket 丢掉服务端首个 HELLO_ACK，客户端 PTO 重发 HELLO；断言 `handshake_retries>=1`、`secure_stats.handshakes==1`、`on_peer_connected` 只触发一次；再以 `handshake_replay_window_ms=100` 重放旧 HELLO，断言 `replay_drops>=1` 且不新建 peer；补 `=0` 时接受（记录为已知弱化） |
| PH-04 | P1 | peer_hub_secure | `Connect` 重连、`RemovePeer`、`Close` 在加密会话下的密钥/窗口清理零覆盖（对应 `ResetSecureSession`、`StartHandshake` 新 nonce） | 新增 `TestSecureReconnectAndTeardown`：建连后 `RemovePeer` → `peer_state==kClosed`、`peer_secure_ready=false`、`Peers()` 空；重新 `AddPeer` 建连 → `handshakes` 递增且旧密文重放得到 `replay_drops`（证明 nonce/窗口已换）；`Close()` 连调两次幂等且无 UAF（ASAN 下跑） |
| LH-01 | P1 | log_hook | `LevelIndex`/`EventIndex` 的越界钳位零覆盖（变异 E：去掉钳位 + ASAN 仍 PASS）；一旦实现回归会越界写数组 | 新增 `TestInvalidEnumClamping`：`Emit(static_cast<LogLevel>(99), static_cast<LogEvent>(99), ...)` 与 `Count(...)` 不崩溃，且计入 `per_level[kLogLevelCount-1]`/`per_event[kLogEventCount-1]`；`LogLevelName(static_cast<LogLevel>(99))=="unknown"`；用例在 ASAN 构建下运行 |
| SC-03 | P2 | session_control | 事件与统计盲区：`Leave` 的 `kLeft`、`Grant` 的 `kGranted`、`Revoke(peer)` 幂等、`RequestControl` 幂等（续租不产事件）、`Grant(kInteract)` 抢占的「先 `kPreempted` 后 `kGranted`」都未断言 | 按分支补 5 个短用例：Leave 控制器断言单条 `kLeft`、`from=kInteract→kNone`、`disconnects` 不变；Grant 断言 `kGranted` 与 `grants`；`Revoke(peer)` 连调两次断言第二次返回 true 且 `changes`/`revocations` 不增；同一控制器再次 `Request` 断言返回 true、`changes` 不增、`requests+1`，并用 `Tick(now)` 证明 deadline 被延长；`Grant(kInteract)` 抢占断言两条事件顺序与 `preemptions=1` |
| SC-04 | P2 | session_control | 构造器钳位（`max_participants=0→1`、`default_mode=kNone→kView`）与 0/非法 id 边界（`Grant(0)`、`Revoke(0)`、`is_controller(0)`、`revoked(99)`、`has_controller`）未测 | 补 `TestConfigClampAndZeroIds`：`max_participants=0` 时 `Join(1)` 成功、`Join(2)` 失败；`default_mode=kNone` 时 `Join` 后 `mode==kView`；逐一断言上述 0/99 返回 false；`has_controller()` 在 `RequestControl` 前后分别为 false/true |
| SC-05 | P2 | session_control | `Snapshot`/`ToJson` 的多参与者排序、`lease_remaining_ms` 非零、空参与者数组、`interact` 模式字符串未测；名称映射只覆盖 2/13 | 补 `TestSnapshotFullFields`：3 个参与者断言 `peer_id` 升序、`controller=true` 唯一、`mode` 字符串为 `interact/view`；有控制器时 `lease_remaining_ms>0`；空会话 `ToJson()` 含 `"participants":[]` 且 `"controller":0`；遍历全部 `ControlModeName`/`ControlChangeReasonName` 断言非空且去重 |
| SC-06 | P2 | session_control | 随机测试只有单线程单种子，缺统计守恒断言；随机路径无法发现「`generation` 与 `changes` 脱钩」「`grants` 与事件数不一致」类 bug | 扩展 `TestRandomizedOperations`：每 100 步断言 `generation()==stats().changes`、`preemptions<=grants`；用 3 个种子各 4000 步；再补 2 线程随机混合（配合 SC-02） |
| PH-05 | P2 | peer_hub_secure | 加密模式下并发 `SendStream`、`TrySendStream`（`kWouldBlock` 重试）、`SendBarrier` 因果顺序零覆盖 | 新增 `TestSecureConcurrentSend`：4 线程 × 500 条向同一 peer 发送，接收端按序全量、`packets_sealed` 与发送量对账；另测 `TrySendStream` 遇 `kWouldBlock` 后重试最终全量；`SendBarrier` 后断言先前 datagram 已交付（因果屏障）；TSAN 构建下运行 |
| PH-06 | P2 | peer_hub_secure | 非法/边界输入：空载荷（`nullptr`/`bytes=0`）、超过 `kMaxStreamFragments*kStreamFragmentBytes` 的超限包、`peer_secure(99)`/`peer_secure_ready(99)`、`SetPreSharedKey(key,0)`、`SetPeerPreSharedKey(1,nullptr,32)`、加密 peer 未握手时 `SendStream` 失败且 `send_errors` 增 | 补 `TestSecureInvalidInputs`：逐项断言返回值与计数；超限包断言 `SendStream` 返回 false 且不产生半帧；未握手发送断言 `send_errors` 增 1 且 `packets_sealed` 不变 |
| PH-07 | P2 | peer_hub_secure | 无随机/故障注入；5 处固定 `PumpFor`（600/1200/1500/200/1500 ms）用时序等待而非条件轮询，弱网机器上可能假失败或白等 | 新增 `TestSecureRandomFaultInjection`：用 `fault_proxy.hpp` 以 1%~5% 概率对密文丢包/重排/重复/篡改，3 个种子 × 200 条；断言不崩溃、无认证绕过、合法流量最终全量、坏包被对应计数；把固定 `PumpFor` 改为「条件轮询 + 超时」（`PumpHubs` 已有，统一用它） |
| LH-02 | P2 | log_hook | PeerHub 事件接入点 8/14 未验证：`kPeerDisconnected`、`kHandshakeFailed`、`kPlaintextRejected`、`kReplayDrop`、`kFrameRejected`、`kSendError`、`kRecvError`、`kControlChange`（后者当前无接入点） | 复用 secure 场景补事件断言：明文注入 → `kPlaintextRejected` 且 `peer_id` 正确；重放 → `kReplayDrop`；非法帧 → `kFrameRejected`；`RemovePeer` → `kPeerDisconnected`；`hello_max_attempts` 耗尽 → `kHandshakeFailed`；对未连接 peer 发送 → `kSendError` |
| LH-03 | P2 | log_hook | 构造带 `min_level`、`Trace` 便捷入口、`message==nullptr`、sink 内重入 `Emit`、并发 `SetSink`/`SetMinLevel` 未测 | 补 `TestHookConstructionAndReentry`：`LogHook(sink, kError)` 下 Info/Warn 不回调、Error 回调；`Emit(..., nullptr)` sink 收到 `message==nullptr`；sink 内再次 `Emit` 不死锁且计数正确；8 线程 `Emit` + 2 线程切换 `SetMinLevel/SetSink` 不崩溃、计数守恒 |
| SC-07 | P3 | session_control | 名称映射覆盖 2/13；`ToJson` 只做子串匹配，无法发现字段顺序/逗号错误 | 用解析器或严格整串比较导出 JSON（空表、单参与者、多参与者三组固定期望值）；名称映射与 `control_change` 全枚举覆盖 |

## 4. 断言质量问题

| 编号 | 位置 | 问题 | 证据/建议 |
|---|---|---|---|
| Q1 | `test_session_control.cpp:153`、`test_peer_hub_secure.cpp:322-325, 546-548` | **下界断言过弱**：`denials >= 1`、`packets_opened > 0`、`encrypted_bytes_* > 0`、`packets_sealed > kTotal` 只能证明「发生了」，不能证明「数值正确」；实现把每帧封装两次、或把字节数算错，这些断言仍通过 | `denials` 应为精确值（场景 3 可算）；`packets_opened` 应与「消息数 + ACK/心跳数」对账；`encrypted_bytes_sent` 应等于 `packets_sealed × 帧长` 的累计（允许 ±16B tag 的固定开销） |
| Q2 | `test_session_control.cpp:175-195` | **租约边界盲区**：变异 A 把「恰好到期」改成不回收，整份测试仍 PASS；说明 sleep 只覆盖了「明显过期」，没覆盖等号边界 | 见 SC-01：改用 `Tick(now)` 注入，断言 `deadline-1` / `deadline` 两点 |
| Q3 | `test_log_hook.cpp:335-356` | **防御分支盲区**：变异 E 去掉 `LevelIndex/EventIndex` 钳位并开 ASAN，测试仍 PASS；越界写数组的回归不会被发现 | 见 LH-01：显式传非法枚举值 |
| Q4 | `test_session_control.cpp:43-51`（`CountInteract`） | **不变式采样非原子**：逐 `peer_id` 调 `mode()`，每次独立加锁，跨调用之间状态可变；若实现存在「瞬间两个 `kInteract`」的窗口，采样可能漏掉（并发测试虽在 join 后调用，但 `TestRandomizedOperations` 的每步检查同样是非原子采样） | 改用一次 `Snapshot()` 拿全量参与者再统计；`Snapshot` 内部单锁，语义是原子快照 |
| Q5 | `test_peer_hub_secure.cpp:534-548` | **统计对账缺失**：`retransmits>0`、`sealed>kTotal` 都只是弱证据；无法发现「重传帧未重新封装」「wire seq 复用」「每帧重复发送」类 bug | 断言 `received == 200` 已有；补 `packets_sealed - packets_opened <= 丢包数`、`secure_stats.handshakes == 1`、`retransmits` 与 `reliable_stats` 的 `retransmits` 一致 |
| Q6 | `test_session_control.cpp:341-403` | **随机路径缺守恒断言**：4000 步只查「最多一个 Interact」；`generation`、`stats.changes`、`requests/grants/denials` 之间的守恒关系未验证（固定 3 步的 `TestCallbacksAndSnapshot` 只覆盖极短路径） | 每步或每 100 步断言 `generation()==stats().changes`；明确「`requests` 是否包含失败请求」并断言 |
| Q7 | 三个测试的 `main` | **失败不中断导致级联误报**：`BENTO_CHECK` 只计数不 return，`PumpHubs` 超时后仍继续跑后续断言，首个根因会被大量级联 `[FAIL]` 淹没 | 对「前置条件」（`Open`/`connected`/`PumpHubs` 返回值）用 `if (!ok) return;` 早退，或增加 `BENTO_REQUIRE` 宏；保留 `BENTO_CHECK` 用于独立断言 |
| Q8 | `test_session_control.cpp:262` vs `src/bento/net/session_control.cpp:304` | **统计语义未测**：`RequestControl` 在 peer 未加入/被撤销时也先 `++requests`；测试只在成功路径断言 `requests==1`，未锁定「失败请求是否计数」的契约，后续重构可能静默改变指标含义 | 补断言：未加入请求后 `requests` 与 `denials` 各 +1；被撤销请求后 `denials` +1、`requests` +1；并在头文件注释写清语义 |

另有两处「合理但需注意」的弱断言，不计入问题：错误 PSK/明文降级的 `auth_failures>0`、`plaintext_rejected>0` 依赖重传次数，用下界是正确选择；`handshake_retry count>0` 同理。

## 5. 七维结论

| 维度 | 结论 | 依据 |
|---|---|---|
| 1. 功能覆盖 | **中上**：核心 happy path 全覆盖；SC 20/24、LH 11/14 有调用，PH secure 关键分支覆盖较全但基础 API 依赖另一文件 | 覆盖矩阵 §2；三份测试共 21 个用例 |
| 2. 边界（空/满/超时/错误/重复/非法） | **不足**：满（SC 上限 ✅、PH `max_peers` ❌）、超时（握手 ✅、租约边界 ❌）、错误（PSK/明文 ✅）、重复（幂等 ❌）、非法（越界枚举 ❌）、空（空载荷 ❌） | SC-01/SC-03/SC-04、PH-06、LH-01 |
| 3. 并发/压力 | **不均衡**：LH 有 8×500 并发、SC 有 16 线程单次抢控；PH **无任何并发发送测试**，SC 无混合并发；三份测试均未在 TSAN 下验证 | SC-02、PH-05 |
| 4. 随机/真实场景 ≥5 | **场景数达标、随机注入不足**：SC 8 场景 + 1 个 4000 步随机；PH 6 场景、LH 7 场景，但 PH/LH 无随机或故障注入，PH 每 peer 仅 30 条 | PH-07、SC-06 |
| 5. 断言有效性 | **总体强、局部弱**：内容级校验 + 事件字段 + 精确计数；变异 D 证明上限断言有效；但 Q1/Q2/Q3 证明边界与下界断言存在盲区 | §1.2、§4 |
| 6. SKIP 逃逸 | **无**：三个文件无 `SKIP`/`exit(77)`/环境分支；`cmake/Go2CppBento.cmake:165-167` 注册为普通测试，`TIMEOUT 90`；`SKIP_RETURN_CODE` 只属于 `bento_netem`（不在本次范围） | `grep SKIP` 无命中 |
| 7. 可复现性 | **好，但有时序依赖**：端口全用 `bind_port=0`，无共享固定路径；随机种子固定；三轮耗时波动 <0.02 s。风险点是 1 处 `sleep` 与 5 处固定 `PumpFor` | §6 |

## 6. 可复现性与 flaky 风险

| 位置 | 依赖 | 风险 | 建议 |
|---|---|---|---|
| `test_session_control.cpp:186` | `sleep_for(150ms)` 后 `Tick()` 必须回收（租约 100 ms） | 若 CI 在 `RequestControl` 与首次 `Tick()` 之间发生 >100 ms 调度暂停，首次 `!Tick()` 会假失败；慢机只增加耗时 | 改 `Tick(now)` 注入（SC-01），彻底去掉 sleep |
| `test_peer_hub_secure.cpp:359` | `PumpFor(1500)` 等客户端进入 `kTimedOut` | 握手超时 500 ms，1.5 s 余量足够；机器极慢时只会更晚超时，风险低 | 改为 `PumpHubs` 条件轮询 |
| `test_peer_hub_secure.cpp:384, 428` | `PumpFor(600)` / `200` 等明文拒绝计数 | 首次 HELLO 立即发送，拒绝是同步的；余量足够 | 可保留，但统一改条件轮询更稳 |
| `test_peer_hub_secure.cpp:533` | `PumpFor(client, 1200)` 制造 PTO 窗口 | 初始 PTO 200 ms，1.2 s 内重传稳定；依赖 cwnd 初值 | 改为轮询 `retransmits>0` 且加上限 |
| `test_peer_hub_secure.cpp:295-308` | 20 s 交付窗口 | 实测整测试 3.8 s，余量 5×；重负载下可能接近，但 `TIMEOUT 90` 兜底 | 保留 |
| `test_log_hook.cpp:238, 275, 304, 327` | 固定 `PumpFor(700/1500/900/900)` | 事件由首次重传/心跳触发，余量足够；不会假通过（断言 `>0`） | 可改条件轮询，减少固定等待 |

不存在：固定端口、固定临时文件、跨测试共享状态、随机种子漂移、依赖外部服务（`bento_netem` 的 root 依赖不在这三个测试）。因此**当前无已观测 flaky**；上述均为理论风险，按 SC-01 改造可消除最主要的一处。

## 7. 限制与说明

- 本报告只审查三份测试文件与对应实现；`test_peer_hub.cpp`（明文基础路径）、`test_crypto.cpp`、`test_secure_link.cpp` 仅用于判定覆盖归属，未逐条审查其断言质量。
- 变异实验在 `/tmp/mut_a`、`/tmp/mut_d`、`/tmp/mut_e` 的副本上进行，工作树三份测试与实现零改动；`build-review/` 被 `.gitignore` 的 `/build-*/` 忽略。
- 未做全量 TSAN/ASAN：仅 log_hook 变异用 ASAN 跑过一次；并发断言（SC-02、PH-05）建议在 CI 增加一次 TSAN 任务，但本次未执行。
- 覆盖率百分比未使用 `gcov/lcov`，矩阵判定基于「用例是否调用 + 断言是否能区分正确/错误实现」，属于静态判定；建议后续加一次 `--coverage` 实测作为补充。
- 未验证 `ctest -j` 并行下的端口/时序冲突；三份测试均绑定 0 端口，理论上可并行，但未实测。
