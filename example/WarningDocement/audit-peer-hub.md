# peer_hub 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/net/peer_hub.hpp`、`src/bento/net/peer_hub.cpp`、
  `tests/bento/test_peer_hub.cpp`、`tests/bento/test_peer_hub_secure.cpp`
- 压测工具：`tests/bento/audit_peer_hub_stress.cpp`（独立程序，不注册 CTest）
- 构建：`.build-bento-wsl`（Ninja，`GO2CPP_WITH_BENTO=ON`，Release，
  `-Wall -Wextra -Wpedantic -Werror`）
- 环境：WSL2 Ubuntu 24.04，12 vCPU，19 GB 内存，GCC 13.3，fd 上限 10240
- 结论：**无 P0**。修复 **2 个 P1**（明文 HELLO 重传的 conn_id 抖动导致千 peer
  「假连接/误删」；`RemovePeer` 回调内 `Connect` 的假重连）与 **4 个 P2**；
  3 项性能增强；P2 记录 6 项。`ninja` 0 警告，`ctest -R bento` **48/48 通过**
  （`bento_netem` 因无 CAP_NET_ADMIN 按设计 SKIP）。

## 1. 压测数据

### 1.1 1000 peer 同时连接

| 指标 | 结果 |
|---|---|
| 同时在线 | **1000/1000**（`server_active=1000`、`client_connected=1000`） |
| 建连耗时 | **0.36 s**（分批接入，每批 50 个） |
| 服务端 peer 表 / fd | `PeerIds=1000`；进程 fd **1004**（1000 客户端 socket + 服务端 + 标准 fd） |
| 内存 | 建连前 3.7 MiB → 1000 peer 后 **69.3 MiB**（客户端 Hub 的 64 KiB 收包缓冲占大头） |

### 1.2 10 万帧可靠转发（STREAM，64 B 载荷，100 帧/peer）

| 指标 | 结果 |
|---|---|
| 交付 | **100000/100000**，按 peer 独立按序，0 串流、0 重排、0 坏载荷 |
| 耗时 / 吞吐 | **3.38 s，29,602 frames/s，1.81 MiB/s** |
| 延迟（含发送端排队） | P50 **617 ms**、P95 2.15 s、P99 2.51 s |
| 错误计数 | invalid=0、unknown=0、timeouts=0、send_errors=0 |

延迟解释：1000 条独立可靠流各有 cwnd/PTO/ACK 节奏（ACK 间隔 20 ms），客户端
每 peer 每轮只入队 1 帧，P50 主要是**发送端排队**而非链路时延。这是 1000 条
小窗口流的聚合结果，不代表单流 RTT（同进程 100 peer 时 P50 为 0.28 ms）。

### 1.3 10 万帧 DATAGRAM 纯转发（无窗口/ACK 限制）

| 指标 | 结果 |
|---|---|
| 交付 | **100000/100000**（本地回环 0 丢包） |
| 耗时 / 吞吐 | **0.84 s，118,551 frames/s，7.24 MiB/s** |
| 错误计数 | bad=0、owner_mismatch=0 |

### 1.4 空闲 Tick 成本（1000 peer connected）

| 指标 | 结果 |
|---|---|
| 服务端 `RunOnce(0)` | **91.5–109.7 µs**（Tick 遍历 1000 peer + 心跳/ACK + 通道 OnTimer） |
| 客户端单 Hub `RunOnce(0)` | **1.2–1.7 µs**（1 个 peer） |

### 1.5 泄漏

| 项目 | 结果 |
|---|---|
| 1000 peer 压测前后 RSS | 71.0 → 90.6 MiB（客户端/服务端通道与统计，进程退出回收） |
| 50 轮 × 16 peer「建连→收发→RemovePeer/Close→销毁」 | RSS 0 增长（5376→5376 KiB），fd 0 增长 |
| ASan+UBSan（leaks 8 轮 / random 1500 ops / 5 场景 / bench 200 peer） | 0 报告、0 失败 |
| valgrind（leaks 4 轮 6 peer；random 120 ops） | 917 allocs = 917 frees / 671 = 671，`0 bytes in 0 blocks`，0 错误 |

### 1.6 覆盖率（gcov，`-O0 --coverage`）

| 测试 | 行覆盖 | 分支执行 / 至少走一次 | 调用覆盖 |
|---|---|---|---|
| `test_peer_hub`（502 断言） | **54.65%**（682/1248） | 62.53% / 37.21% | 63.16% |
| `test_peer_hub_secure`（825 断言） | **66.03%**（824/1248） | 73.30% / 42.20% | 68.93% |

未覆盖集中在：IPv6 分支（`FormatAddress`/`PortOf` 的 AF_INET6）、MAX_DATA/流控
（`WriteU64/ReadU64`、`SendMaxDataFrame`）、PMTUD/视频帧拒绝分支、部分错误返回。
与第 6 节「覆盖缺口」一致。

## 2. 随机测试

`--mode=random --ops=1500`：1500 次随机操作（AddPeer/RemovePeer/Connect/
SendStream/SendDatagram/Close/Open/驱动），回调内以 1/8~1/32 概率删除 peer 或
关闭 Hub，每 25 步做一次全量不变量检查（`PeerIds`/`Peers`/`peer_state`/
`stats().peers_active`/非法 id 发送失败）。**7148 断言 0 失败**，RSS 3.7→4.7 MiB，
fd 4→4。

## 3. 真实场景（5 个，覆盖全部待验证功能）

| # | 场景 | 结果 |
|---|---|---|
| 1 | 每 peer 独立 PSK：1 个 Hub 连 4 个不同 PSK 服务端 + 第 5 个错误 PSK | 4 条加密会话各自握手、双向 100 帧隔离；错误 PSK 只影响第 5 条（auth_failures=8），其余 4 条继续可用 |
| 2 | 超时回收释放 `max_peers` | 32/32 满员；第 33 个被拒（peers_rejected+4）；32 个静默后 `PeerIds` 清空、timeouts+32；`Connect` 重连成功且不再计 rejected |
| 3 | 回调内 RemovePeer/Close/二次删除/回调内重连（P0-01 不回归） | 120×RemovePeer、60×Close、60×on_peer_connected 内删除、超时回调内软回收重连，全部不崩、状态一致 |
| 4 | 重连 session/conn_id 同步 | 明文 10 次 + 加密 6 次：每次 `Connect` 换新 conn_id，服务端 conn_id 与客户端一致、`stale_dropped` 不增、双向数据恢复 |
| 5 | 60 s 长稳（随机 peer 出入） | 546 次 AddPeer / 540 次 RemovePeer / 4952 帧；RSS 4736→4736 KiB、fd 13→13、timeouts=0、unknown=0、invalid=0 |

## 4. 解耦与头文件

- `peer_hub.hpp` 只 include `channel/crypto/link/log_hook/protocol/relation` +
  标准库/POSIX，**不依赖 app 层**（静态检查 0 处 `app/` 引用）。
- 头文件自包含：单独 `#include "go2cpp/bento/net/peer_hub.hpp"` + `-fsyntax-only`
  `-Wall -Wextra -Wpedantic -Werror` 通过。
- 依赖方向：`peer_hub → {link(UdpSocket/NowMs), crypto, channel, relation,
  protocol, log_hook}`，无反向依赖；app 层不进入本模块。

## 5. 问题清单

### P1-1 明文 HELLO 重传在 conn_id 冲突后被当成新会话（千 peer 假连接）

- **现象**：1000 peer 压测中约 1.8% 客户端「收到 ACK、开始发心跳」后 15–20 s
  被双向判超时；服务端 `unknown_peer_frames` 持续增长（约 150 帧/个）。
- **根因**：`HandleHello` 对「同 session、`proposed_conn_id != peer->conn_id`」
  的 HELLO 一律 `ResetChannels` + 换 conn_id。服务端曾因冲突改过 conn_id 时，
  客户端 ACK 丢失后的**原包重传**会再次触发重置：客户端按旧 ACK 的 conn_id
  发心跳，服务端路由表已换新值 → 计入 unknown、不刷新 `last_rx` → 15 s 后
  `peer_timeout` 误删 peer，客户端也超时。
- **修复**：`Peer` 新增 `client_conn_id` 记录客户端本代提议值；同值重传走
  **幂等 ACK**（不重置、不换 conn_id），提议值变化才视为「未换 session 的重连」。
- **证据**：新增回归 `TestHelloRetransmitAfterConnIdConflict`（两个 raw socket
  制造 conn_id 冲突，断言两次 ACK 的 conn_id 相同、conflicts 不增、重传后数据
  仍交付）。**回滚修复后该测试 3 处失败**（`ack_b2 != ack_b1`、conflicts+1、
  数据帧不交付），恢复后通过。1000 peer 连接从 982/1000 提升到 1000/1000。

### P1-2 `RemovePeer` 回调内 `Connect` 的假重连

- **现象**：`RemovePeer` 触发 `on_peer_disconnected("removed")` 时，回调里调用
  `Connect` 返回 `true`，但 `RemovePeer` 返回后 peer 被硬删除队列销毁，应用
  以为在重连、实际已无记录。
- **根因**：先回调、后 `ScheduleCleanup(peer, true)`；`Connect` 的 pending 检查
  看不到删除意图。
- **修复**：先清 conn_id + 登记硬删除，再触发回调；`Connect` 命中 pending 时
  返回 false（与「RemovePeer 后不可复活」语义一致）。
- **证据**：新增回归 `TestRemovePeerRejectsReconnectInCallback`；旧实现该断言
  失败（Connect 返回 true）。

### P2（已修）

- **P2-1** 显式 `RemovePeer` 被计入 `stats().timeouts`（统计语义错误）→ 不再计数；
  回归 `TestRemovePeerDoesNotCountAsTimeout`。
- **P2-2** 延迟销毁中的 `kClosed` peer 可被同地址 HELLO 复活（假连接）→
  `HandleHello`/`HandleSecureHello` 入口拒绝并计 `unknown_peer_frames`。
- **P2-3** `stats()` 每次 O(peers) 遍历 → 改为 `m_active_peers` 增量维护（O(1)）；
  回归 `TestPeersActiveCountMatchesSnapshot` 在随机/场景中持续校验。
- **P2-4** 热路径每帧堆分配：发送戳 conn_id、加密 seal/open 的临时 vector →
  改为成员 scratch 复用（1000 peer × 10 万帧少 20–30 万次 malloc/free）。
- **P2-5** `Tick` 先发心跳/ACK 再判超时 → 先判超时并 `continue`（失联 peer 不再
  占用发送与重传）。

### P2（记录，未改）

- **P2-6（base 层）** `UdpSocket::Open` 无条件 `SO_REUSEADDR`：同进程 1000 个
  `bind(0)` 的 UDP socket 实测出现 **18 对本地端口复用**（生日碰撞
  ~N²/2R，R≈28k）。复用时内核只把包投给其中一个 socket，另一个 PeerHub
  「假连接」后超时。压测侧已改用不同回环 IP（127.1.x.y）规避；根因在
  `src/bento/net/link.cpp`，本任务边界外，建议「仅显式指定端口时才设
  SO_REUSEADDR」。
- **P2-7** 监听端不支持 per-source PSK：只能 Hub 默认 PSK，或对已注册的主动
  peer 用 `SetPeerPreSharedKey`。1 被控 N 主控若要每主控独立密钥需扩展配置。
- **P2-8** `FindByAddress` 是 O(peers) 线性扫描：1000 peer 握手重传期最坏
  O(N²)（实测连接阶段仍 <0.4 s，未成瓶颈）。地址索引需同步 NAT 迁移的 4 个
  更新点，风险大于收益，记录。
- **P2-9** `SendControl` 每帧 `BuildFrame` 分配（心跳 1000 peer × 2/s）；控制帧
  小、频率低，未改。
- **P2-10** 回调内 `Close()` 后同一 `RunOnce` 内再 `Open()`：`m_close_deferred`
  会在本轮末尾清空刚建的 peer 表（极端用法，文档未承诺支持）。
- **P2-11** `PeerSecureStats` 的 `packets_opened` 在 AEAD 通过后才计；但重放窗口
  在 `OpenFrame` 内先 Accept 再解析，坏包会推进窗口（正常，记录）。

## 6. 测试复查

- 断言：`test_peer_hub` **458 → 502**（+44）、`test_peer_hub_secure` 825，
  合计 1327；新增 4 个回归场景（P1-1、P1-2、P2-1、P2-3）。
- 压测程序全量 `--mode=all`：**10,565 断言 0 失败，97.81 s**（bench + 1500 随机
  + 5 场景 + 60 s 长稳，同一进程连续运行）。
- 有效性验证：临时回滚 P1-1 修复 → 新测试 **3 处失败**；恢复后全过，说明断言
  不是「恒真」。
- SKIP 逃逸：两份测试均无 `Skip(` 调用；`cmake/Go2CppBento.cmake` 未给
  `bento_peer_hub*` 配 `SKIP_RETURN_CODE`，即使将来调用 `Skip` 也会被判失败，
  不会静默通过。
- 覆盖缺口（未补，记录）：IPv6 路径（`UdpSocket` 支持，peer_hub 无测试）；
  `kClosed` 延迟销毁窗口内 HELLO 拒绝（已修，仅由 audit 随机/场景间接覆盖）；
  监听端 per-source PSK；PMTUD/视频帧（`PeerHub` 设计上不实现，`DispatchFrame`
  计入 `unknown_peer_frames`）。

## 7. 复现命令

```bash
# 全量（bench + random + 5 场景 + 60s 长稳）
.build-bento-wsl/audit/audit_peer_hub_stress --mode=all --peers=1000 --frames=100

# 单项
.build-bento-wsl/audit/audit_peer_hub_stress --mode=bench    --peers=1000 --frames=100
.build-bento-wsl/audit/audit_peer_hub_stress --mode=random   --ops=1500
.build-bento-wsl/audit/audit_peer_hub_stress --mode=scenarios
.build-bento-wsl/audit/audit_peer_hub_stress --mode=soak     --seconds=60
.build-bento-wsl/audit/audit_peer_hub_stress --mode=leaks    --rounds=50 --peers=16

# 回归
cd .build-bento-wsl && ninja && ctest -R bento -j4
```

编译压测程序（不走 CMake，保持与既有 audit_* 一致）：

```bash
g++ -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror -Iinclude -Itests/bento \
    tests/bento/audit_peer_hub_stress.cpp .build-bento-wsl/libgo2cpp_bento.a \
    -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl -lgo2cpp_hash -lpthread \
    -o .build-bento-wsl/audit/audit_peer_hub_stress
```

## 8. 改动文件

- `include/go2cpp/bento/net/peer_hub.hpp`（+9：`m_active_peers`、3 个热路径 scratch）
- `src/bento/net/peer_hub.cpp`（+77/−41：P1-1/P1-2/P2-1~5）
- `tests/bento/test_peer_hub.cpp`（+190：4 个回归场景）
- `tests/bento/audit_peer_hub_stress.cpp`（新增，压测/随机/场景/长稳/泄漏）
