# Bento Transport Protocol v0.1 — 最小实现报告

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
