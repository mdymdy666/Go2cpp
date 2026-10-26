# Bento Transport Protocol v0.1 — 最小实现报告

> **2026-10-26 P2 更新（重要）**：帧头已升级为 **16B 且协议版本为 2**
> （新增 `conn_id`），并增加 PMTUD/FEC/0-RTT。本文前 10 节描述的是 P0/P1
> 的 12B 线格式，已被 **§11 P2 变更** 取代；与旧实现的线格式不兼容。

> **2026-10-26 P0-A 更新**：下文 §2.5、§3.2、§4.5、§5 中「STREAM 不分片」
> 「固定 PTO / 所有消息都被重传导致 RTT 样本为 0」等描述已被本日变更取代，
> 以文末「8. P0-A 更新」为准。完整数据见 `/tmp/bento_p0a.md`。

> 日期：2026-10-26 · 工作树：`/mnt/e/CodexWorkspace/Go2Cpp`（WSL）
> 构建目录：`/tmp/build-bento` · 作者：DeepSeek agent（go2cpp 子任务）
> 变更范围：仅新增 `include/go2cpp/bento/`、`src/bento/`、`tests/bento/`、
> `cmake/Go2CppBento.cmake`，并在顶层 `CMakeLists.txt` 增加 Bento 入口；
> 未修改 base 模块源码，未 commit。

---

## 1. 结论

| 验收项 | 结果 | 证据 |
|---|---|---|
| `go2cpp_bento` 库构建 | **通过，0 警告** | `ninja -C /tmp/build-bento`；干净重建 13 个目标，`grep -ci warning` = 0 |
| 测试通过数 | **4 通过 / 0 失败 / 1 SKIP** | `ctest --test-dir /tmp/build-bento`，连续 5 轮结果一致 |
| 弱网 | **用户态代理 20% 丢包 + 200ms 延迟通过**；tc netem **SKIP** | `bento_weaknet_proxy` PASS；`bento_netem` SKIP（无 CAP_NET_ADMIN） |

- 编译：GCC 13.3.0，C++17，`-Wall -Wextra -Wpedantic -Werror`，0 warning / 0 error。
- 测试断言：`test_link_loopback` 866 checks、`test_unreliable_dropstale` 477 checks、
  `test_video_fragment` 37 checks，全部 PASS。
- 产物：`/tmp/build-bento/libgo2cpp_bento.a`（975,482 B）、4 个测试可执行文件。

### 1.1 环境阻塞说明（重要）

本工作树当前**缺少 base 模块源码**（`Go2CppModules.cmake` 已引用但文件不在 checkout）：
`src/config/so_manifest.cpp`、`src/math/`、`src/path/`、`src/container/`、
`src/compress/`、`src/encoding/`、`src/hash/`。直接执行任务给出的
`cmake -S . -B /tmp/build-bento ...` 会在 configure 阶段失败，与 Bento 改动无关。

为遵守「不改 base 模块」的边界，新增一个**默认 OFF** 的独立入口：

```bash
cmake -S . -B /tmp/build-bento -G Ninja \
      -DGO2CPP_WITH_BENTO=ON -DGO2CPP_BUILD_TESTS=OFF -DGO2CPP_BENTO_STANDALONE=ON
ninja -C /tmp/build-bento
ctest --test-dir /tmp/build-bento --output-on-failure
```

`GO2CPP_BENTO_STANDALONE=ON` 时顶层 CMake 在 `project()` 后直接 include
`cmake/Go2CppBento.cmake` 并 return，不触碰 base 模块清单；默认 OFF 时原有
构建流程完全不变。完整源码 checkout 上可去掉该开关。

---

## 2. 帧格式（wire，网络字节序/大端）

### 2.1 固定帧头（12B）

| 偏移 | 字段 | 类型 | 语义 |
|---|---|---|---|
| 0 | ver | u8 | 版本，当前 `0x01`；其他值整帧拒绝 |
| 1 | type | u8 | 帧类型（见 2.2） |
| 2 | chan | u8 | 通道 id：move=0 / click=1 / video=2，其余自定义 |
| 3 | flags | u8 | `0x01` ACK 段存在；`0x02` VIDEO KEY；`0x04` 重传标记 |
| 4 | seq | u32 | 通道内序号（可靠消息 seq / 不可靠包 seq / BARRIER 的屏障 seq） |
| 8 | dep_seq | u32 | 依赖的不可靠 seq；0 = 无依赖 |

seq 从 1 开始，`0` 保留为「无效」。32 位回绕由 `SeqGreater`（有符号差值）处理，
窗口远小于 2^31。

### 2.2 帧类型

| 值 | 类型 | 载荷 | 可靠性 |
|---|---|---|---|
| 0x01 | HELLO | 8B：`"BENT"` + session u32 | 握手重传 200ms→2s，最多 10 次 |
| 0x02 | HELLO_ACK | 8B：`"BENT"` + 回显 session | 同上，由监听方回复 |
| 0x03 | STREAM | 应用消息（≤1300B） | 可靠有序：ACK + 重传 + 去重 |
| 0x04 | DATAGRAM | 应用消息（≤1300B） | 不可靠：DropStale |
| 0x05 | VIDEO | 8B 子头 + 分片（每片 ≤1200B） | 不可靠分片，接收端重组 |
| 0x06 | ACK | 8B：ack_seq u32 + ack_bits u32 | 独立 ACK / 心跳 |
| 0x07 | BARRIER | 无载荷，屏障 seq 在 header.seq | 一次发送，作废旧不可靠 seq |

### 2.3 可选 ACK 段（8B，仅 flags bit0 = 1）

| 偏移 | 字段 | 语义 |
|---|---|---|
| +0 | ack_seq u32 | 下一个期望的 seq（所有 < ack_seq 的 STREAM 已收到） |
| +4 | ack_bits u32 | bit i = 1 表示 `ack_seq+i` 已收到（32 位 SACK） |

STREAM 帧总是捎带 ACK 段；ACK 帧的载荷是同构的 8B。`ACK/BARRIER` 不允许再带
ACK 段（避免两个 ack 来源）。

### 2.4 VIDEO 子头（8B）

`frame_id u32 | frag_idx u16 | frag_cnt u16`，后接分片数据。
非末片必须恰好 1200B，末片 0..1200B；`frag_cnt ∈ [1,256]`，单帧上限约 307KB。

### 2.5 尺寸上限

`kMaxFramePayload = 1300`，完整帧最大 `12 + 8 + 1300 = 1320B`，低于常见
UDP 安全 MTU，不依赖 IP 分片。STREAM/DATAGRAM 超过 1300B 直接拒绝，不分片
（分片只用于 VIDEO；这是与 yojimbo「整包分片」不同的取舍）。

---

## 3. 通道与状态机

### 3.1 Link 状态机

```
Idle ──Connect()──> Connecting ──HELLO_ACK──> Connected
  │                     │                        │
  │                     └─握手超时/10 次重试──> TimedOut
  └─监听方收到 HELLO──────────────────────────> Connected
Connected ──peer_timeout_ms 内无任何有效帧──> TimedOut
任意状态 ──Close()──> Closed
```

- 监听方（`peer_address` 为空）在 `Idle` 等 HELLO；客户端 `Open()` 后调 `Connect()`。
- 同一地址的新 session 视为对端重启：监听方重置可靠/不可靠通道与 Relation
  （视频重组不重置，旧 frame_id 按 stale 丢弃）。
- 心跳是纯 ACK 帧：每 `heartbeat_interval_ms`（默认 500ms）至少一帧，
  同时刷新对端活跃时间并补发 ACK；独立 ACK 最快每 20ms 一次。
- 默认 `peer_timeout_ms = 3000`；`netem_probe` 放宽到 10s 以吸收 20% 丢包。

### 3.2 ReliableChannel（STREAM）

- 发送：seq 分配 → 未发队列 → 发送窗口 256 → inflight（按 seq 升序 deque）。
  首次发送带 `ack_seq`/`ack_bits`（捎带接收状态）。
- ACK：累积确认 `seq < ack_seq`；SACK 位图确认空洞后的消息；Karn 算法只对
  未重传消息采样 RTT。
- 重传：每条消息独立 PTO = `min(200ms × 2^retransmits, 2000ms)`，无重试上限，
  由 Link 心跳超时兜底；重传帧置 `kFlagRetransmit`。
- 接收：窗口 512；`seq < expected` → stale；窗口内重复 → duplicate；
  乱序缓存，收到 `expected` 后连续交付（严格有序）。
- 因果：`dep_seq != 0` 且依赖未满足 → **不接收、不 ACK**，等发送端重传；
  这样可靠通道不会出现「跨过较小 seq 交付」。
- 去重：`m_rx_seen` 集合 + expected 推进；已交付 seq 不再入缓存。

### 3.3 UnreliableChannel（DATAGRAM）

- 每通道独立发送 seq，从 1 单调递增；接收端只接受 `seq > applied_seq`。
- `seq <= applied_seq` → `stale_dropped++`，永不采纳（DropStale）。
- 带 `dep_seq` 且依赖未满足 → 复制载荷放入 Relation pending（上限 128）。
- 依赖满足/ BARRIER 释放时，若该帧 seq 已被更新的帧越过，仍按 stale 丢弃。
  结果：交付序号**严格递增**，代价是部分晚到帧被丢弃。
- BARRIER：先把所有不可靠通道 applied 抬到 barrier_seq，再释放 Relation
  pending；barrier 之前的帧永久作废。

### 3.4 VideoChannel（VIDEO）

- 发送：按 1200B 切片，每片一个 VIDEO 帧（`frame_id/frag_idx/frag_cnt` + KEY）。
  seq 按片递增；不做重传。
- 接收：重组项最多 4 个，半成品 1s 超时丢弃；只接受比当前最大重组帧更大的
  frame_id（丢包后旧帧不补齐）；旧 frame_id / 重复片丢弃；非法分片计数丢弃。
- 全部片到齐后按 `(frag_cnt-1)*1200 + last_len` 交付整帧，清理 ≤ 交付帧的状态。

### 3.5 Relation（因果）

- `watermark`：已采纳的最大不可靠 seq；`IsReady(dep) = dep==0 || dep<=watermark`。
- pending：按 dep 升序的 multimap；满 128 丢最旧；`Flush()` 执行所有就绪回调。
- BARRIER 将 watermark 抬到 barrier 并 Flush。

---

## 4. 测试与实测数据

### 4.1 运行方式

```bash
cmake -S . -B /tmp/build-bento -G Ninja \
      -DGO2CPP_WITH_BENTO=ON -DGO2CPP_BUILD_TESTS=OFF -DGO2CPP_BENTO_STANDALONE=ON
ninja -C /tmp/build-bento
ctest --test-dir /tmp/build-bento --output-on-failure
```

### 4.2 `test_link_loopback`（真实 UDP 回环，双端）

- 场景：move=0 / click=1 双向各 100 条 STREAM（64B，含 seq + 校验模式），
  另 10 条 DATAGRAM 混跑。
- 结果：866 checks PASS；双向 100/100 按序、内容校验通过；10/10 DATAGRAM；
  `retransmits=0`、`stale=0`、`duplicate=0`、`inflight=0`、HELLO 重传 0；
  延迟 p50=1ms、p95=1~2ms、p99=2ms。
- 异常路径：1301B STREAM 被拒绝；坏帧/截断/坏版本/未知类型在单测中覆盖。

### 4.3 `test_unreliable_dropstale`（注入乱序/旧 seq）

- 注入顺序 `5,3,4,2,1,6`：只采纳 5、6；随后注入 100 与 99 个随机打乱的旧
  seq（1..99）+ 重复 100：**新增采纳 0 条**，累计 `stale_dropped=104`，
  交付序列严格递增 `[5,6,100]`，`0 乱序`。
- 因果：`10,20(dep=12),11,12` → 交付 `[10,11,12,20]`；依赖被 50 越过的
  30 被丢弃（不产生乱序）；BARRIER(150) 正确释放 `200(dep=150)`。
- 可靠通道：`3,3,1,2` → 交付 `[1,2,3]`，重复 3 → duplicate，seq 0 → invalid，
  seq 5000 超窗 → stale；STREAM dep 未满足时不 ACK，BARRIER 后重传可交付。
- 真实 Link：50 条 DATAGRAM 经 UDP 回环全部按序收到，`stale=0`。
- 477 checks PASS。

### 4.4 `test_video_fragment`

- 3000B 帧 → 3 片，按 `2,0,1` 乱序注入 → 完整交付、KEY 保留、内容一致。
- 旧 frame 5 → stale；frame 8 缺片 → 1s 超时丢弃，补齐后不交付；
  空帧 0B → 1 片交付；`frag_idx >= frag_cnt`、非末片非满片 → invalid；
  超过 256 片 → 发送拒绝。
- 37 checks PASS。

### 4.5 `bento_weaknet_proxy`（用户态 20% 丢包 + 200ms 单向延迟）

`bento_netem_probe --loss 20 --delay-ms 200 --messages 200 --payload 256`：
单进程双 Link，客户端经 UDP 代理连服务端；代理每个方向独立丢 20%、
延迟 200ms。

| 指标 | 实测（两次） |
|---|---|
| 可靠交付 | **200/200 按序，0 超时** |
| 重传次数 | 358 / 665 |
| 可靠延迟 p50 / p95 / p99 | 780 / 3193 / 3202 ms；5123 / 5194 / 5202 ms |
| 不可靠交付 | 154/200，**0 乱序**（stale=0，晚到旧包被 DropStale） |
| 代理转发/丢弃 | 687/173；960/241 |
| 总耗时 | 4.26s；约 5.1s（5 轮范围 1.86–6.15s） |

5 轮 `ctest`：每轮 4 PASS + 1 SKIP，0 FAIL。

### 4.6 `test_netem.sh`（tc netem）— SKIP

- 本机 `uid=1000`，`sudo -n true` 需要密码，`tc qdisc add` 返回
  `RTNETLINK answers: Operation not permitted`，无 `CAP_NET_ADMIN`。
- 脚本按设计输出 `SKIP: no root/CAP_NET_ADMIN ...` 并 `exit 77`；
  CTest 用 `SKIP_RETURN_CODE 77` 已识别为 **Skipped**（非失败）。
- 脚本实现：`prio` + `u32` 过滤，仅把目标/源端口 == 探针端口的 loopback
  流量放进 netem class，避免影响 127.0.0.1:56911 的 GUI；结束用 trap 恢复。
  有 root 的机器可直接跑：`bash tests/bento/test_netem.sh /tmp/build-bento/bento_netem_probe`。

### 4.7 覆盖矩阵（真实场景 ≥5）

| 场景 | 覆盖功能 | 测试 |
|---|---|---|
| 双人实时移动/点击双向流 | HELLO、STREAM、ACK、有序交付 | loopback |
| 弱网丢包 + 延迟 | PTO 重传、指数退避、心跳、去重 | weaknet_proxy |
| 状态同步乱序/旧包 | DropStale、dep pending | dropstale |
| 视频关键帧传输 | VIDEO 分片/重组/KEY/超时 | video_fragment |
| 输入→动作因果 + 屏障 | dep_seq、BARRIER、不可靠水位 | dropstale 因果段 |
| 异常输入 | 超长、截断、坏版本/类型、非法分片 | dropstale / video |

随机性：dropstale 用固定种子随机打乱 99 个旧 seq；弱网代理固定种子丢包；
压测：200 条 × 20% 丢包 + 200ms，统计 p50/p95/p99。

---

## 5. 已知限制

1. **无加密/认证**：HELLO 只校验 magic/session，任何能访问端口的进程都可
   加入；无防重放、无消息完整性。生产化需 Noise/TLS 类握手。
2. **工作树 base 模块缺失**：默认全量 configure 失败，Bento 走
   `GO2CPP_BENTO_STANDALONE=ON`；这是本 checkout 的环境问题，不是协议问题。
3. **tc netem 未实测**：无 root；替代方案用户态代理不模拟乱序/抖动，
   只模拟独立丢包与固定延迟。
4. **固定 PTO**：严格 200ms 起、指数退避、2s 上限。单向 200ms（RTT 400ms）
   时 200ms PTO 会过度重传（实测 358–665 次、p95 3.2–5.2s，RTT 样本为 0
   因为所有消息都被重传、Karn 丢弃）。下一步可用 SRTT + 抖动估计 PTO。
5. **STREAM/DATAGRAM 不分片**：单条 ≤1300B；大消息需上层拆分。
6. **BARRIER 不可靠**：只发一次；丢失则接收端水位不推进，需上层重发。
7. **单线程、IPv4 only、无 DNS、无自动重连**：一个 Link 对应一个 peer；
   不支持多客户端服务端。
8. **32 位 seq 回绕**：代码跳过 seq 0 并用有符号差值比较，但未做 2^32
   回绕的注入测试；接收窗口 512 远小于回绕半程。
9. **视频**：只接受更大的 frame_id，丢包后旧帧不会补齐；同时最多 4 个
   重组帧、1s 超时；无重传、无前向纠错。
10. **超时路径未覆盖**：Link peer timeout / HELLO 超时没有专门的注入测试；
    当前测试 0 超时。
11. **通道上限 8**：超过后新通道被拒绝（防内存 DoS），测试未覆盖。
12. **无拥塞控制/流控**：只有 256 的发送窗口；弱网下可能过度占用带宽。

## 6. 参考与原创性

- yojimbo `reliable.h`（314 行）/`reliable.c`（4292 行）：
  借鉴「包头携带 ack/ack_bits、序列回绕比较、PTO 重传、分片重组、去重窗口」
  的**设计思路**；本实现是独立的 C++17 类（32 位 seq、12B 头、显式通道对象、
  Relation 因果层），未复制任何代码。
- GameNetworkingSockets `steamnetworkingtypes.h` 的每消息
  `k_nSteamNetworkingSend_Reliable/Unreliable` 标志：启发了按消息选择
  STREAM/DATAGRAM 类型，而不是按通道绑定可靠性。
- 许可证：MIT（与项目一致）。

## 7. 文件清单

```
include/go2cpp/bento/net/protocol.hpp   160 行
include/go2cpp/bento/net/relation.hpp   102 行
include/go2cpp/bento/net/channel.hpp    307 行
include/go2cpp/bento/net/link.hpp       255 行
src/bento/net/protocol.cpp              258 行
src/bento/net/relation.cpp               53 行
src/bento/net/channel.cpp               532 行
src/bento/net/link.cpp                  722 行
tests/bento/test_util.hpp                95 行
tests/bento/test_link_loopback.cpp      225 行
tests/bento/test_unreliable_dropstale.cpp 330 行
tests/bento/test_video_fragment.cpp     146 行
tests/bento/netem_probe.cpp             304 行
tests/bento/udp_proxy.hpp               171 行
tests/bento/test_netem.sh                85 行
cmake/Go2CppBento.cmake                  87 行
CMakeLists.txt                           +12 行（standalone 分支 + include）
```

## 8. P0-A 更新（2026-10-26）：动态 PTO / 快速重传 / STREAM 分片

### 8.1 动态 SRTT/RTTVAR/PTO

- ACK 新确认消息取 `rtt = now - first_send_ms`；首样本 `srtt=rtt,
  rttvar=rtt/2`；之后 `rttvar = 3/4*rttvar + 1/4*|srtt-rtt|`、
  `srtt = 7/8*srtt + 1/8*rtt`。
- `PTO = srtt + 4*rttvar + max_ack_delay`，默认夹在 `[200ms, 2000ms]`，
  可通过 `LinkConfig::reliable.rtt` 配置；`max_ack_delay_ms` 默认 25ms。
- 首样本前用 RFC 9002 初始 `srtt=333ms / rttvar=166ms`（初始 PTO 1022ms），
  避免 400ms RTT 链路在首个 ACK 前必然超时、Karn 拿不到干净样本。
- 超时退避 `base * 2^n`，上限 2s；`fixed_pto_ms>0` 可复现旧固定 200ms 行为。
- Karn：重传消息不采样；被 SACK 空洞堵住的消息打 `hol_blocked`，跳过采样。

### 8.2 快速重传

- 3 次重复 ACK（不推进累积确认且无新 SACK）立即重传最小未确认 seq。
- SACK 新增证据 ≥3 时立即触发；32 位 SACK 窗口内的多个已知空洞可一次
  并行补发（上限 8 条），不再逐空洞等待 PTO。
- 恢复期内（`in_fast_recovery`）抑制重复触发，累积确认前进后复位。
- 重传帧置 `kFlagRetransmit`；新增 `fast_retransmits` 统计。

### 8.3 STREAM 分片/重组

- >1300B 消息自动分片：STREAM `flags` 置 `kFlagFragmented=0x08`，
  载荷开头 8B 子头 `frame_id u32 | frag_idx u16 | frag_cnt u16`，
  每片应用数据 ≤1292B，单条消息 ≤256 片 / 330752B。
- ≤1300B 消息保持 v0.1 原格式，不增加 8B 开销，与旧实现互通。
- 接收端按 `frame_id` 重组：乱序容忍、非末片必须满 1292B、重复/非法计数、
  半成品 10s 超时、最多 16 个并发重组，完整后一次性交付。
- 发送窗口按分片计；一条消息的所有分片必须一次放入窗口，否则背压拒绝。

### 8.4 新增测试（`ctest -R '^bento_'`）

| 测试 | 结果 |
|---|---|
| `bento_rtt_pto`（80 checks） | PASS；50/200/400ms 注入 PTO=200/237/453ms |
| `bento_fast_retransmit`（73 checks） | PASS；3 dupACK/多 SACK 空洞、<PTO |
| `bento_stream_fragment`（85 checks） | PASS；5000B 乱序重组/缺片/非法 |

### 8.5 弱网数据（20% 丢包 + 200ms 单向，5 seeds）

动态 PTO 平均重传 276.4 次，固定 200ms 平均 395.0 次，下降 30.0%（中位数 226 vs 361，下降 37.4%）；
200/200 按序交付、0 超时。代价是动态 PTO 的 p50/p95 延迟均值更高
（3163/3586ms vs 938/1100ms），详见 `/tmp/bento_p0a.md` §4。

## 9. P0-B 更新（2026-10-26）：PSK 认证加密 / 防重放 / 拥塞控制 + pacing

> 工作树 `/UserData/CodexWorkSpace/Go2cpp`，构建 `/tmp/build-bento-wsl`。
> 仅改 `include/go2cpp/bento/net/**`、`src/bento/net/**`、`tests/bento/**`
> 与 bento 专用 `cmake/Go2CppBento.cmake`；未改 base、未 commit。
> 完整数据见 `/tmp/bento_p0b.md`。

### 9.1 加密模式开关

- 默认明文，保持旧测试与线格式；配置 32B `LinkConfig::pre_shared_key`
  或调用 `Link::SetPreSharedKey()` 后启用 PSK 认证加密。
- 双方 PSK 不一致、或一方未配置时握手认证失败，链路**不会降级**为明文。
- 加密开启后，STREAM / DATAGRAM / VIDEO / ACK / BARRIER / 心跳全部加密。

### 9.2 加密帧格式（外层 12B 头 + 密文 + 16B tag）

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | ver=1 | 协议版本 |
| 1 | type | 内层帧类型 |
| 2 | chan | 内层通道 |
| 3 | flags | 内层 flags \| `kFlagEncrypted=0x10` |
| 4 | seq | 每方向递增的 wire seq（从 1 开始） |
| 8 | dep_seq | 外层保留 0（内层 dep_seq 在密文里） |
| 12 | ct | 内层完整帧（12B 头 + 可选 ACK 段 + 载荷）的 ChaCha20 密文 |
| 12+len(ct) | tag | Poly1305 16B |

- AEAD nonce = `direction u32 BE || wire seq u64 BE`（12B）；外层 12B 头为 AAD。
- 内层解密后再走原 `ParseFrame`/通道逻辑，可靠性语义零改动。
- 每帧开销 28B；STREAM 最大加密帧 1348B < 1472B（IPv4+UDP 安全上限）。
- 解密成功后才推进重放窗口；伪造高 seq 无法把窗口推高。

### 9.3 PSK 握手与密钥派生

- 握手密钥：`HChaCha20(PSK, "BENTO-HS-C2S/S2C-001")`，C2S/S2C 分离。
- HELLO / HELLO_ACK 明文体：magic `"BENT"` + version + session u32 +
  16B 随机 nonce + 8B 墙钟时间戳；用握手密钥 AEAD 加密。
- 会话方向密钥：
  `HChaCha20(HChaCha20(PSK, client_nonce), server_nonce)`，再按
  `"BENTO-DATA-C2S/S2C-01"` 派生两把独立密钥。
- 握手 wire seq 每次连接随机（32 位非零），重传复用同一密文；
  避免跨连接 `(key, nonce)` 重用。
- 监听方缓存最近 8 个 client nonce：当前会话重复 HELLO 幂等重发 ACK；
  旧 nonce 重放拒绝；时间戳超出 `handshake_replay_window_ms`（默认 30s）拒绝。
- 会话内数据 wire seq 从 1 递增；发送失败也推进 seq，绝不重用 nonce。

### 9.4 防重放窗口

- 每方向独立 64 位滑动窗口（IPsec 风格）：`bitmap` 的 bit i 表示
  `highest-i` 已收到，窗口外旧包一律拒绝；按有符号 32 位回绕差值比较。
- 统计（`Link::secure_stats()`）：
  `packets_sealed/opened`、`auth_failures`（AEAD 失败）、`replay_drops`、
  `malformed_frames`、`plaintext_rejected`、`handshakes`、`handshake_retries`、
  `encrypted_bytes_sent/received`。

### 9.5 拥塞控制（NewReno 简化）+ pacing

- **慢启动**：每确认 1 包 cwnd +1 MSS，到 ssthresh 转拥塞避免。
- **拥塞避免**：累计确认 cwnd×MSS 字节才 +1 MSS（每轮 +1）。
- **丢包**：`ssthresh=max(cwnd/2,2)`，`cwnd=ssthresh`；同一 SRTT 内
  最多降一次，避免连续 SACK 空洞把窗口瞬间打到最小。
- **PTO 超时**：`ssthresh=max(cwnd/2,2)`，`cwnd=2`，回慢启动。
- cwnd 上限 256（发送窗口）；默认 initial=32、ssthresh=64、MSS=1200B。
- **pacing**：令牌桶，速率 `cwnd×MSS/SRTT`，桶容量一个 cwnd；
  每包按 MSS 计费；初始桶满允许一个窗口突发，之后按速率补充。
- 配置：`ReliableChannel::Config::congestion`（enabled / pacing /
  initial_cwnd_packets / min_cwnd_packets / max_cwnd_packets /
  initial_ssthresh_packets / mss_bytes / initial_rtt_ms / min_rtt_ms）。
- 统计导出：`ReliableChannel::Stats` 的 `cwnd_packets`、`ssthresh_packets`、
  `effective_cwnd`、`pacing_rate_bps`、`pacing_deferrals`、`loss_events`、
  `timeout_loss_events`、`congestion_bytes_sent`、`slow_start`；
  或 `ReliableChannel::congestion_stats()` / `Link::congestion_stats(channel)`。

### 9.6 新增测试（ctest，4 个）

| 测试 | 覆盖 |
|---|---|
| `bento_crypto` | RFC 8439 §2.4.2 ChaCha20、RFC 7539 §2.5.2 Poly1305、RFC 8439 §2.8.2 AEAD 向量；篡改/截断/重放/错误密钥；64 位窗口边界；吞吐 ≥50MB/s |
| `bento_secure_link` | 正确 PSK 双向加密数据；错误 PSK 不连接；明文客户端被安全监听方拒绝；安全客户端被明文监听方拒绝；统计导出 |
| `bento_secure_injection` | 故障代理注入篡改/截断/重放/明文；服务端四类拒绝计数均 >0；可靠流 10/10 按序 |
| `bento_congestion` | 慢启动/拥塞避免/丢包减半/超时回慢启动/令牌桶速率/通道集成/Link 统计 |

### 9.7 弱网数据（20% 丢包 + 200ms，用户态代理）

200 条 256B 消息、40s 预算，一次实测：

- 可靠：**200/200 按序**，`retransmits=92`（`fast_retransmits=33`），
  `srtt=401ms`，`pto=442ms`，p50=11.8s / p95=32.0s / p99=33.5s。
- 不可靠：160/200 送达，**0 乱序**（`ordered=1`）。
- 拥塞：末值 `cwnd=3`、`ssthresh=2`、`loss_events=15`、`timeout_loss=33`、
  `pacing_bps=8977`、`pacing_deferrals=1513`、`bytes_sent=55200`。
- 链路：`timeouts=0`，握手 2 次，帧发送 992 / 接收 790，代理丢 201。

高丢包下 PTO 把 cwnd 压到 2~3，吞吐显著低于无丢包；正确性（100% 按序）
优先。`bento_netem` 仍因无 CAP_NET_ADMIN SKIP，用户态代理覆盖同等语义。

### 9.8 限制

- 仅 PSK，无 X25519 前向保密（任务允许先做 PSK 版）。
- 握手时间戳依赖双方墙钟大致同步（默认 ±30s）；可用
  `handshake_replay_window_ms=0` 关闭校验。
- 40 亿包后 wire seq 回绕：重放窗口按 32 位差值处理，但单连接实际不会
  持续到回绕；文档按限制记录。
- `cmake/Go2CppBento.cmake` 增加了 4 个新测试的注册与无构建类型时的
  `-O2`（性能验收需要）；这是 bento 专用构建文件，未触碰 base。

## 10. P1 更新（2026-10-26）：流控 / 带宽估算 + ABR / 统计导出 / IPv6 / 多线程

> 工作树 `/UserData/CodexWorkSpace/Go2cpp`，构建 `/tmp/build-bento-wsl`（Ninja）。
> 仅改 `include/go2cpp/bento/net/**`、`src/bento/net/**`、`tests/bento/**`
> 与 bento 专用 `cmake/Go2CppBento.cmake`；未改 base、未 commit。
> 完整数据见 `/tmp/bento_p1.md`。

### 10.1 流控（RFC 9000 MAX_DATA 语义）

- 新帧类型 `MAX_DATA = 0x08`：载荷 8B 大端 u64 绝对上限，`header.channel`
  指定通道。`ParseFrame` 校验载荷恰为 8B。
- 每可靠通道维护两个累计字节偏移：
  - `m_peer_max_data`：对端允许我发送的线上载荷字节（含 8B STREAM 分片子头）；
  - `m_local_max_data`：我承诺可接收的累计字节。
- 发送：`ReliableChannel::TrySend()` 在 `tx_bytes_total + wire > peer_max_data`
  时返回 `SendStatus::kWouldBlock`，消息不入队、不消耗 seq；调用方在收到
  `MAX_DATA` 后重试同一消息。`Send()` 保留 bool 语义（WouldBlock = false），
  `Link::TrySendStream()` 暴露三态。
- 接收：超窗帧丢弃并计 `flow_control_violations`（不 ACK）；收到新数据后
  若剩余窗口 < initial/2，把上限抬到 `rx_bytes_total + initial`，由 Link 的
  `Tick` 发送 MAX_DATA。通告丢失时按 `max_data_refresh_ms`（默认 200ms）
  幂等重发最新上限。
- 初始窗口 `ReliableChannel::Config::initial_max_data`（默认 1 MiB，双方必须
  一致；`peer_initial_max_data` 可单独覆盖对端初值）。
- 统计：`flow_control()`（local/peer/tx/rx/max_data_sent/received/blocked/
  violations）与 `ReliableChannel::Stats` 的 `flow_control_blocked`、
  `send_window_blocked`、`max_data_sent`、`max_data_received`。

### 10.2 带宽估算 + ABR

- `BandwidthEstimator`（`bandwidth.hpp` / `bandwidth.cpp`）：1s 滑动窗口记录
  每次 ACK 的应用层字节，`delivery_rate_bps = 窗口字节×8×1000 /
  max(now-oldest, 100ms)`；丢包率 = 累计丢失包 /（确认包 + 丢失包）。
- 可靠通道在 `Acknowledge()` 喂确认字节（分片消息扣除 8B 子头），在快速
  重传/PTO 事件喂丢包；`Link` 汇总所有可靠通道。
- `Link::BandwidthEstimate()`（bps）、`Link::LossRate()`（0..1）。
- ABR：`LinkConfig::abr_interval_ms`（默认 200ms）与
  `Link::on_bandwidth(std::uint64_t bitrate_bps)`；估算变化超过 ±10% 才回调，
  回调在 RunOnce 线程内执行，可安全调用 Send*。
- 实测估算器（1s 窗口、20ms 样本）：500k/2M/8Mbps 三档偏差 0.0%。

### 10.3 统计导出

- `LinkStats` 增加 `rtt_ms`、`srtt_ms`、`rttvar_ms`、`pto_ms`、`cwnd_packets`、
  `ssthresh_packets`、`inflight`、`loss_events`、`retransmits`、
  `bandwidth_bps`、`loss_rate`、`flow_control_blocked`、`max_data_sent`、
  `max_data_received`。
- `Link::Stats()` 聚合所有可靠通道：RTT/RTTVAR/PTO 取最大，cwnd/ssthresh/
  inflight/retrans/loss 求和；`Link::StatsJson()` / `LinkStatsToJson()` 输出
  单行 JSON，`loss` = 丢包降窗事件、`retrans` = 重传次数、
  `loss_rate` 为 4 位小数。
- 实测（20 条 128B 回环）：`rtt_ms=18`、`srtt_ms=18`、`pto_ms=200`、
  `cwnd_packets=52`、`ssthresh_packets=64`、`bytes_sent=2980`、
  `bandwidth_bps=204800`、`inflight=0`、`loss=0`、`retrans=0`；JSON 可被
  平坦解析器解析。

### 10.4 IPv6

- `UdpSocket` 从 IPv4-only 改为 `sockaddr_storage` + `getaddrinfo(AF_UNSPEC)`；
  `LinkConfig::remote` 优先于旧 `peer_address`，支持 IPv4/IPv6/主机名。
- 绑定 `::` 时关闭 `IPV6_V6ONLY`（双栈）；`SamePeer` 比较 family、端口、
  16B 地址与 scope id。
- 保留 `ResolveIPv4`、`SendTo(sockaddr_in)`、`RecvFrom(sockaddr_in*)`、
  `SamePeer(sockaddr_in, sockaddr_in)` 重载，旧测试/代理零改动。
- `test_ipv6`：`::1` 双向各 20 条 64B；`remote` 覆盖不可达的
  `peer_address=192.0.2.1` 仍成功。

### 10.5 多线程

- `Link` 增加 `mutable std::recursive_mutex m_mutex`；`Open/Connect/Close/
  RunOnce/SendStream/TrySendStream/SendDatagram/SendVideoFrame/SendBarrier/
  ConcurrentSend/SetPreSharedKey/Stats/StatsJson/BandwidthEstimate/LossRate/
  各 stats 查询` 全部加锁；`RunFor` 通过 `RunOnce` 间接加锁。
- 接收模型不变：单线程跑 `RunOnce`（持锁完成 poll + 分发 + Tick）；发送线程
  可任意并发调用 `ConcurrentSend`（与 `SendStream` 等价）。回调在接收线程
  锁内执行，可递归调用 Send*，但不要调 RunOnce。
- 已知代价：`RunOnce` 的 poll 持锁，最多阻塞发送线程一个 `timeout_ms`；
  生产建议 `RunOnce(0)` 或短超时。
- 实测：2 线程各 100 条（同 channel 0）-> 200/200 送达、0 重复、
  `duplicates_dropped=0`、`messages_sent=200`；TSan 构建（关闭 ASLR）无
  data race 报告。

### 10.6 新增测试（ctest，5 个）

| 测试 | checks | 覆盖 |
|---|---|---|
| `bento_flow_control` | 428 | 发送端窗口耗尽 -> WouldBlock、MAX_DATA 恢复、接收端过半推进、200 条端到端窗口刷新 |
| `bento_bandwidth` | 30 | 500k/2M/8Mbps 估算偏差 0.0%、丢包率 20%、ABR 回调与 Link 估算 |
| `bento_stats_json` | 76 | 20 条后字段非零、inflight 反映未确认、JSON 平坦解析与值一致性 |
| `bento_ipv6` | 175 | `::1` resolve/绑定/连接、`remote` 优先、双向 20 条按序 |
| `bento_concurrent_send` | 608 | 2 线程各 100 条并发 Send、0 重复、200/200 送达 |

全量 `ctest -R '^bento_'`：**16 PASS + 1 SKIP（netem 无 CAP_NET_ADMIN）**，
0 FAIL；干净重建 Bento 目标 `grep -ci warning = 0`。

### 10.7 弱网复测（20% 丢包 + 200ms，用户态代理）

`bento_netem_probe --loss 20 --delay-ms 200 --messages 200 --payload 256`：

| 指标 | 实测 |
|---|---|
| 可靠交付 | **200/200 按序**，`ordered=1`、`timeouts=0` |
| 重传 | 103（快速重传 61），`acks=200` |
| RTT/PTO | `srtt=402ms`、`rttvar=3ms`、`pto=439ms`、`rtt=405ms` |
| 延迟 | p50=13783ms / p95=35583ms / p99=36968ms（20% 丢包下 PTO 退避） |
| 不可靠 | 162/200 送达，**ordered=1、stale=0（0 乱序）** |
| 拥塞 | `cwnd=2`、`ssthresh=2`、`loss_events=23`、`timeout_loss=26` |
| 代理 | 转发 814、丢弃 208（20%） |

### 10.8 P1 限制

1. **流控初值无握手协商**：双方必须配置相同的 `initial_max_data`；对端若
   配置更小，发送端会超额发送并被丢弃。P2 可在 HELLO 中携带初始窗口。
2. **接收窗口按「已接收字节」前进**，不是应用已消费字节；应用长期不消费
   会限制吞吐，但内存仍受 `initial_max_data` 与 `kReceiveWindow`（512 帧）
   双重约束，不会无限增长。
3. **MAX_DATA 周期重发**：窗口推进后每 `max_data_refresh_ms`（200ms）重发
   一次最新上限（每通道约 5 帧/s），不是逐包确认；极端丢包下恢复延迟为
   一个 refresh 周期。
4. **发送窗口仍是 256 帧**，与字节流控独立；小消息场景先撞帧窗口。
5. **RunOnce 持锁 poll**：发送线程最坏被阻塞一个 `timeout_ms`；文档建议
   短超时，生产可改成 poll 锁外 + 处理锁内（需要额外处理 Close 竞态）。
6. **带宽估算无时间戳回显**：ACK 压缩/延迟确认时估算偏低；1s 窗口对短时
   突发不敏感。当前仅在本地回环与用户态弱网验证，未做真实公网测试。
7. **IPv6 双栈依赖系统**：`::` 绑定的 `IPV6_V6ONLY=0` 在部分平台受 sysctl
   限制；未做 Windows/macOS 适配（Bento 仅 Linux）。
8. **统计 JSON 为手写单层对象**：无嵌套、无 schema 版本；新增字段需同步
   `LinkStatsToJson` 与测试。

---

## 11. P2 变更（2026-10-26）

### 11.1 线格式

| 偏移 | 字段 | 类型 | 语义 |
|---|---|---|---|
| 0 | ver | u8 | **2**（P2 起；旧 1 不再接受） |
| 1 | type | u8 | 帧类型 |
| 2 | chan | u8 | 通道 id |
| 3 | flags | u8 | 见 11.2 |
| 4 | conn_id | u32 | HELLO 协商的连接标识；所有后续帧携带 |
| 8 | seq | u32 | 通道内 seq / 外层 wire seq |
| 12 | dep_seq | u32 | 依赖的不可靠 seq |

- HELLO/HELLO_ACK 载荷由 8B 扩为 12B：`"BENT" + session u32 + conn_id u32`。
- 加密外层头同样为 16B；`conn_id` 明文可见并作为 AEAD AAD 的一部分，
  `seq` 为每方向 wire seq。`kSecureOverheadBytes = 16 + 16 = 32`。

### 11.2 新增 flags / 帧类型

| 值 | 名称 | 语义 |
|---|---|---|
| 0x20 | `kFlagFec` | VIDEO XOR 校验片（仅 VIDEO） |
| 0x40 | `kFlagResume` | 0-RTT 恢复首帧；接收端只重置一次应用通道 |
| 0x80 | `kFlagResumeAck` | 监听方已接受恢复首帧；客户端据此停止置 Resume |
| 0x09 | `kPmtudProbe` | PMTUD 探针：token u32 + target u32 + 填充，最大帧 1500B |
| 0x0A | `kPmtudAck` | PMTUD 回显，最小 8B |

### 11.3 连接迁移

- 客户端 `Link::Migrate(new_addr, new_port)` 重绑本地 UDP socket，conn_id、
  wire seq、会话密钥与所有通道状态保留；下一个包从新地址发出。
- 接收端先按 conn_id 路由，只有（加密模式下）AEAD 校验通过的帧才更新
  对端地址；旧地址在 `migration_grace_ms` 内到达的帧计入
  `stale_peer_frames` 并丢弃。
- 新路径会重置 PMTUD 为保守 1200 并重新探测。

### 11.4 VIDEO XOR FEC

- `LinkConfig::video_fec_group_size = N`（0=关闭，2..8；推荐 4/8）。
- 每 N 个数据片生成 1 个校验片：`header.flags |= kFlagFec`，
  视频子头 `frag_idx = N`，`frag_cnt = 数据片总数`。
- 校验片载荷（`P` 为组内最大片长）：
  `8B 视频子头 | group_start u16 | group_count u16 | max_len u16 |
   group_count×u16 各片原始长度 | P 字节 XOR 数据`。
- 接收端仅在“某组恰好缺 1 片且校验片已到”时异或恢复；缺 ≥2 片绝不猜，
  由重组超时丢弃整帧。组内非末片恢复后仍必须是满 1200B。

### 11.5 PMTUD

- `LinkConfig::enable_pmtud`（默认 false，保持既有测试时序）开启后 socket
  设置 `IP_MTU_DISCOVER=DO`（DF）与 `IP_RECVERR`。
- 连上后从 1200B 起，每档 +100B 探到 `pmtud_max_mtu`（默认 1500，按 UDP
  载荷计）；对端回 `kPmtudAck` 才升档。
- 收到 ICMP `EMSGSIZE`（错误队列）或本地 sendto `EMSGSIZE` 时按报告值下降
  （夹在 576..max），并停止升档；无 ICMP 且探针无响应时保持当前值（初始
  1200）并计 `pmtud_blackholes`。
- `Link::PathMtu()` 导出当前估计；`NotifyPathMtuError(mtu)` 供无
  `IP_RECVERR` 平台或测试注入，只降不升。

### 11.6 0-RTT 会话票据

- `Link::ExportSessionTicket()`：PSK 模式且会话密钥就绪时导出 92B 票据：
  magic/ver/flags + conn_id + session + issued/expires + client/server nonce +
  两方向 next wire seq + 16B Poly1305 MAC。MAC 根密钥由 PSK 经 HChaCha20
  派生，一次一密输入为 (conn_id, session, issued)；票据不含密钥。
- `Link::Resume(ticket)`：校验 MAC/有效期/PSK 后派生会话密钥，直接进入
  Connected，不发送 HELLO；首个 `Send*` 即为 AEAD 加密帧。应用通道
  （stream/datagram/video seq 与重组状态）从新会话重新开始，wire seq
  从票据续接，防止 nonce 复用。
- 监听方在握手中把服务端会话快照（nonces/密钥可由 PSK 重派生、client
  方向的 replay window、server next wire seq）写入进程内缓存；收到
  未知 conn_id 的加密帧时先试解缓存中的会话，认证成功才提交状态。
  因此同一进程内监听方对象重建后仍能接受 0-RTT 首包。
- 票据过期、MAC 错误或 PSK 不符时 `Resume` 返回 false 并计
  `tickets_rejected`。

### 11.7 P2 限制

1. **0-RTT 跨进程不支持**：票据不含密钥，监听方需要进程内会话缓存才能
   在“服务端重启”后接受首包；跨进程需把票据放进首个控制帧或引入服务端
   持久化票据存储。
2. **明文模式迁移可被伪造**：conn_id 在明文模式下未认证；加密模式下只有
   AEAD 校验通过才迁移，但旧地址静默期仍可被伪造源地址用于抑制（DoS）。
3. **完整重握手不迁移**：带新 session 的 HELLO 仍要求源地址不变；跨地址
   重连走 0-RTT/数据面 conn_id 迁移。
4. **PMTUD 依赖 Linux 错误队列**：非 Linux/无 `IP_RECVERR` 平台只能靠
   ACK 探针与上层 `NotifyPathMtuError`；`PathMtu()` 是 UDP 载荷预算，
   不是 IP 层 MTU。
5. **FEC 只保护 VIDEO 且单组单校验片**：可纠 1 片擦除，不能纠 2 片；
   大帧按 N 分组独立校验，组间不共享。
6. **0-RTT 应用通道重置**：恢复后 stream/datagram 的 seq 从 1 重新开始，
   依赖旧通道 seq 的跨会话因果语义不保留。
