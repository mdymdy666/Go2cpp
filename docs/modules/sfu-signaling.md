# 模块文档：SFU 与 SDP 信令（sfu / signaling）

> **状态：现行（2026-11-01）。** 对应 WebRTC 媒体链路中「一对多转发」与
> 「会话协商」两段能力。本文解释设计取舍、API 契约、验收数据与已知限制；
> 代码与注释是事实来源，本文与代码冲突时以代码为准。
>
> 工作树：`/UserData/CodexWorkSpace/Go2cpp`；构建目录：`.build-bento-wsl`
> （Release，Ninja，g++ 13.3.0，WSL2 / Ubuntu 24.04，Ryzen 9 8945HX 12 vCPU）。

---

## 1. 概述与边界

`go2cpp::media` 已有「采集 → 编码 → VideoChannel 1200B 分片 → UDP → 重组 →
解码 → 渲染」的端到端链路（`docs/modules/webrtc-media.md`）。本模块补上另外两块：

| 模块 | 文件 | 解决什么 |
|---|---|---|
| SFU | `include/go2cpp/media/sfu.hpp`、`src/media/sfu.cpp` | 多房间、多 peer；发布者只发一份，SFU 按订阅关系**选择性转发分片** |
| 信令 | `include/go2cpp/media/signaling.hpp`、`src/media/signaling.cpp` | SDP 子集的 offer/answer、ICE 候选交换、`idle→offering→answering→connected` 状态机 |

**做**：

- 房间/参与者/流/订阅的增删查，peer 离开时级联清理；
- 以**分片**为单位的转发：不解码、不重编码，内存与帧大小无关；
- 中途订阅从下一个关键帧首片开始（差分编码必须）；
- 回调内可安全退订/离开（延迟回收，见 §3.4）；
- 两个 SFU 单向级联 + hop 上限防环；
- SDP 文本序列化/解析、host/srflx/prflx/relay 候选、`a=setup` 角色协商；
- 与 `p2p::Candidate` 双向转换，信令消息可跑在 `p2p::LoopbackSignaling` 上。

**不做（明确留给上层或后续）**：

- RTCP / NACK / PLI / FIR、拥塞控制、带宽估计、simulcast/SVC 选层；
- 实际 DTLS 握手与 SRTP 加密（只透传 `a=fingerprint` 字段）；
- 房间鉴权、候选合法性防伪造（依赖上层 Token/ACL 与 DTLS）；
- 信令传输通道本身（只定义消息文本格式，IM/WebSocket/自建 TCP 均可承载）；
- 重协商/rollback（一份会话一次 offer/answer；完成后只能关闭）；
- 多线程/锁（与 `VideoChannel`、`UdpVideoReceiver` 一样是单线程 pump 模型）。

---

## 2. 文件布局

| 路径 | 内容 | 规模 |
|---|---|---|
| `include/go2cpp/media/sfu.hpp` | `SfuRouter` / `SfuPublisher` / `SfuSubscriber` / `SfuCascade`、统计结构 | 336 行 |
| `src/media/sfu.cpp` | 房间/流索引、选择性转发、延迟回收、级联实现 | 943 行 |
| `include/go2cpp/media/signaling.hpp` | `IceCandidate`、`SessionDescription`、`SignalingMessage`、`SignalingSession` | 454 行 |
| `src/media/signaling.cpp` | SDP 行状态机、候选编解码、状态机、P2P 互转 | 1789 行 |
| `tests/media/test_sfu.cpp` | 7 组用例（含 3000 步随机拓扑与 1e6 帧吞吐） | 884 行 |
| `tests/media/test_signaling.cpp` | 5 组用例 + 6 个真实协商场景 + 随机 fuzz | 891 行 |
| `cmake/Go2CppMedia.cmake` | 追加 SFU/信令源文件、P2P 依赖与 2 个 ctest 目标 | 追加 23 行 |
| `docs/reports/asm-sfu.md` | SFU 转发热路径反汇编、A/B 数据与下限分析 | 新增 |
| `docs/reports/asm-signaling.md` | SDP 解析反汇编、分配瓶颈与下限分析 | 新增 |

构建开关沿用 `GO2CPP_WITH_MEDIA=ON`。若 `GO2CPP_WITH_P2P=OFF`，媒体库仍会
构建，只是 `GO2CPP_MEDIA_HAVE_P2P` 不定义、不提供候选互转函数（其余 SDP/候选
逻辑不依赖 P2P）。

---

## 3. SFU 设计

### 3.1 数据模型

```
Room(房间名)
 ├─ peers: peer_id -> Peer { published: set<stream_id>, subscriptions 计数 }
 └─ streams: (peer_id<<32 | stream_id) -> Stream {
        publisher, std::deque<Subscription>, stats
    }
Subscription { subscriber, sink, active, waiting_key }
```

两条索引分别回答两个问题：

- 某 peer 发布了哪些流？→ `Peer::published`；
- 某条流该转发给谁？→ `Stream::subscriptions`。

`streams` 用扁平 64 位键（`publisher << 32 | stream`）而不是 `std::pair`
或字符串，避免每个分片构造临时键；房间仍用 `std::string`（建房/加入不是热路径）。

### 3.2 选择性转发路径

`ForwardFragment(room, publisher, stream, data, bytes, hops)`：

1. 校验 `hops <= max_cascade_hops`；
2. 用 `DecodeHeader` + `DecodeVideoHeader` 做最小解码：要求
   `type == kVideo`、`channel == kChannelVideo`、无 `kFlagAck`，再校验
   `frag_index < frag_count`、数据片 ≤1200B。这里刻意不用通用的
   `ParseFrame`——它为 ACK/FEC/长度准备了多条热路径分支，实测会拖慢 1.7×
   （见 `docs/reports/asm-sfu.md`）；
3. 查房间与流索引；没有订阅者直接 `fragments_no_subscriber++` 返回；
4. 只遍历**这条流**的订阅者，构造一个栈上的 `SfuFragment`（含解析好的
   `FrameHeader`/payload 视图），逐个调用 `sink`；
5. 更新全局/单流统计（`fragments_forwarded` 按订阅者累计，`frames_forwarded`
   只在 `frag_index==0` 且至少投递一次时 +1）。

因此转发成本 = 一次解析 + 一次 map 查找 + N 次 `std::function` 调用，
与房间内 peer 数无关；不做全广播。

### 3.3 关键帧门控

差分编码（`kDeltaI420`）从 P 帧开始解码会花屏，所以新订阅默认
`waiting_key = true`：只有**关键帧且 `frag_index == 0`** 的分片才放行，之后
清位。被挡住的分片计入 `fragments_keyframe_gated`（按订阅者累计）。
`SfuConfig::require_keyframe_start=false` 可关闭（测试吞吐路径使用）。

### 3.4 回调内退订：延迟回收

sink 完全可能在自己的回调里 `Unsubscribe` 或 `Leave` 发布者。实现约定：

- 失活订阅时**立即**扣减 `Peer::subscriptions` 与统计，查询接口结果一致；
- 转发期间只做 `active=false` / `dead=true` 标记，不 erase 容器；
- 每层 `ForwardFragment` 维护 `forward_depth`，归零后才执行 `Cleanup()`，
  统一删除 dead 房间/流/peer 与失活订阅；
- 订阅列表用 `std::deque<unique_ptr<Subscription>>`：回调里新增订阅不会让
  已有元素的地址失效，转发按索引遍历。

这条规则由 `test_sfu` 的「回调里让发布者离开」用例覆盖。

### 3.5 级联与 hop 上限

`SfuCascade` 表示一条**单向**链路：下游房间的 `(publisher, stream)` 被桥接
peer 订阅，回调里以 `hops+1` 转发到上游 SFU 的同名流。`Start()` 幂等地准备
上游房间/桥接 peer/桥接流，`Stop()` 退订。

级联必须配置成无环；`SfuConfig::max_cascade_hops`（默认 3）是最后保险：
`hops > max` 的分片直接丢弃并计入 `fragments_hop_limit`。级联不做健康检查/
带宽协商，上游断连需上层重建。

---

## 4. SFU API

### 4.1 `SfuRouter`（`sfu.hpp:158`）

| 分组 | 方法 | 说明 |
|---|---|---|
| 房间 | `CreateRoom` / `RemoveRoom` / `HasRoom` / `room_count` / `rooms` | 删除房间会连带清理 peer/流/订阅 |
| 参与者 | `Join` / `Leave` / `HasPeer` / `peer_count` / `peers` | `Leave` 同时清理自己发布的流和作为订阅者的订阅 |
| 发布 | `Publish` / `Unpublish` / `HasStream` / `stream_count` / `streams` / `stream_stats` | 同一 peer 可发布多条流 |
| 订阅 | `Subscribe` / `Unsubscribe` / `subscriber_count` / `subscription_count` | 订阅者必须是房间成员，不能订阅自己 |
| 转发 | `ForwardFragment`（`sfu.hpp:209`） | 返回实际投递的订阅者数；非法输入返回 0 并计数 |

所有失败都返回 `SfuStatus`（`kRoomNotFound`/`kPeerExists`/`kStreamLimitReached`/
`kSelfSubscription` 等），不抛异常。`SfuStatusName()` 供日志使用。

### 4.2 发布端与订阅端

```cpp
SfuRouter router;
router.CreateRoom("room-1");
router.Join("room-1", /*peer=*/1);
router.Join("room-1", /*peer=*/3);

SfuPublisher publisher(router, "room-1", 1, 100);
publisher.Start();                       // Publish
publisher.SendFrame(frame_id, key, encoded.data(), encoded.size());

SfuSubscriber subscriber(router, "room-1", 3, 1, 100,
                         [](uint32_t id, bool key, const uint8_t* data, size_t bytes) {
                             // 已由 VideoChannel 重组好的完整帧
                         });
subscriber.Start();                      // Subscribe
subscriber.OnTimer(now_ms);              // 驱动缺片超时（可选）
```

- `SfuPublisher` 内部持有一个 `VideoChannel`，`SendFrame` 直接复用其 1200B
  分片逻辑；sink 回调把每个分片交给 `SfuRouter::ForwardFragment`。
- `SfuSubscriber` 内部也持有一个 `VideoChannel`，把收到的分片交给
  `OnFrame` 重组；一个实例只对应一条 `(publisher, stream)`，避免不同发布者
  的 `frame_id` 混在一个重组器里。
- 两个 helper 都必须在 `SfuRouter` 之前析构；`SfuSubscriber` 析构会自动退订。

### 4.3 统计口径

| 字段 | 含义 |
|---|---|
| `fragments_received` / `bytes_received` | 通过校验、进入转发的分片 |
| `fragments_forwarded` / `bytes_forwarded` | **按订阅者累计**（2 个订阅者 = 2 次） |
| `frames_received` | `frag_index==0` 的帧数（含无人订阅的帧） |
| `frames_forwarded` | 至少投递给一个订阅者的帧数 |
| `fragments_no_subscriber` | 流存在但当前无 active 订阅 |
| `fragments_unknown_stream` | 房间/流不存在 |
| `fragments_malformed` | 空指针、非 Bento 视频帧、视频子头非法 |
| `fragments_keyframe_gated` | 等关键帧被挡下的分片（按订阅者累计） |
| `fragments_hop_limit` | 级联跳数超限 |

`SfuStreamStats` 是单流版本（`frames_forwarded`/`fragments_forwarded`/
`bytes_forwarded`），用于验证房间隔离。

---

## 5. 信令设计

### 5.1 SDP 子集

序列化按固定顺序输出（可逐字节断言）；解析是严格行状态机：

```
v=0
o=- <session-id> <version> IN IP4 127.0.0.1
s=go2cpp-media
t=0 0
a=group:BUNDLE 0 1
m=video 9 UDP/TLS/RTP/SAVPF 96
c=IN IP4 0.0.0.0
a=mid:0
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1
a=sendrecv
a=ice-ufrag:uF1x
a=ice-pwd:0123456789abcdef01234567
a=ice-options:trickle
a=fingerprint:sha-256 AA:BB:...
a=setup:actpass
a=candidate:oh 1 udp 2130706431 192.168.1.10 5000 typ host
...
a=end-of-candidates
```

- `m=` 支持 video/audio/application；application 输出 `a=sctp-port:5000`；
- 只支持 `v=0`；`o=` 必须存在（会话 id 是后续统计/日志的键）；
- 未知 `a=` 属性（`rtcp-mux`、`extmap` 等）忽略，已知但非法（坏 candidate、
  坏 rtpmap、坏 setup、重复 mid、BUNDLE 引用未知 mid）整体失败并写 error；
- 每个媒体段必须带 `a=mid` 与 ICE 凭据（ufrag 4..256，pwd 22..256）；
- 一份描述是 `offer` 还是 `answer` 由 `SignalingMessage` 的类型字段标记，
  SDP 文本本身不携带。

### 5.2 候选与优先级

`IceCandidate` 与 RFC 8839 对齐：`candidate:<foundation> <component> <transport>
<priority> <address> <port> typ <type> [raddr/rport] [generation]`。
优先级按 RFC 8445 §5.1.2.1：
`2^24 × type_pref + 2^8 × local_pref + (256 - component)`，
type_pref：host=126、prflx=110、srflx=100、relay=0，因此排序天然是
**host > prflx > srflx > relay**（默认 local_pref=65535、component=1 时分别为
2130706431 / 1845493759 / 1694498815 / 16777215）。

`ToP2PCandidate()` / `FromP2PCandidate()` 与现有 `go2cpp::p2p::Candidate`
双向映射（类型、foundation、priority、endpoint、base 地址），媒体目标链接
`go2cpp_p2p` 时自动启用；`p2p::ComputeCandidatePriority` 与本模块公式一致
（测试逐类型比对）。

### 5.3 信令消息

```
g2p-signal/1 <type>
<body>
```

`type ∈ {offer, answer, candidate, end-of-candidates, bye}`；offer/answer 的
body 是整段 SDP，candidate 的 body 是一行 `candidate:...`，其余可为空。
`ToText()` / `Parse()` 负责打包与拆包，不依赖 JSON 库。实际部署把它塞进
IM 私聊、WebSocket 或 `p2p::Signaling` 的 payload 即可。

### 5.4 状态机（`signaling.hpp:299`）

```
offerer : idle --CreateOffer--> offering --SetRemoteAnswer--> answering --OnIceConnected--> connected
answerer: idle --SetRemoteOffer--> offering --CreateAnswer--> answering --OnIceConnected--> connected
```

- `Tick()` 用可注入时钟检查 `negotiation_timeout_ms`（默认 30s），超时 →
  `failed`；`OnIceFailed()` 直接置 `failed`；`Close()` 置 `closed`；
- 非法迁移返回 `kInvalidState`/`kInvalidRole` 且状态不变（例：answer 之前
  不能 `OnIceConnected`；带非法 mid/setup/媒体数量的 answer 返回
  `kMediaMismatch`）；
- `CreateAnswer()` 的方向 = 对端方向取反（sendonly→recvonly），DTLS 角色按
  `NegotiateAnswerSetup` 纠正（offer actpass + 首选 active → active；
  offer active → 强制 passive）；
- `OnIceConnected(local, remote)` 由 ICE 层在选对成功后调用，记录选中的候选对；
  SDP 中没有的 prflx 候选会自动补录。

### 5.5 与 P2P / ICE 的分工

| 层 | 负责 |
|---|---|
| `go2cpp::p2p` | 候选收集优先级、STUN Binding 检查、打洞、TURN 中继、`P2PChannel` 状态 |
| `go2cpp::media::signaling` | 把候选/媒体/fingerprint 编成 SDP，驱动 offer/answer 与状态机 |
| 调用方 | 把 `p2p::P2PChannel` 的连接事件映射到 `OnIceConnected/OnIceFailed` |

`p2p::LoopbackSignaling` 在测试中被用作真实信令通道，验证 offer/answer 与
候选 trickle 的文本往返。

---

## 6. 与 VideoChannel 的分片契约

SFU 转发的 `data/bytes` 是**完整 Bento 分片**（16B 帧头 + 8B 视频子头 + 数据），
不是裸载荷。原因：

1. 订阅端 `SfuSubscriber` 必须能看到 `frame_id/frag_index/frag_count/key` 才能
   重组与判关键帧；
2. FEC、`seq`、`conn_id` 等字段原样保留，后续开 FEC 不需要改 SFU；
3. SFU 只校验不重写，因此同一份字节既能走 UDP，也能走进程内直连/级联。

`SfuFragment` 额外携带已解析的 `FrameHeader` 与 payload 视图，订阅端可以直接
`channel.OnFrame(fragment.header, fragment.payload, fragment.payload_bytes)`，
避免二次解析。

---

## 7. 复用与取舍（为什么自己写）

实现前对比过主流 SFU：

| 候选 | 语言/许可证 | 不采用的原因 |
|---|---|---|
| [libwebrtc](https://webrtc.googlesource.com/src) | C++ / BSD-3 | 完整 ICE/DTLS/SRTP/拥塞控制，但代码与构建体量巨大（GN + 平台依赖），会把现有零依赖媒体层整个替换掉 |
| [mediasoup](https://github.com/versatica/mediasoup) | C++ / ISC（以仓库 LICENSE 为准） | 依赖 libuv、OpenSSL、libsrtp、flatbuffers，且自带完整 ICE/DTLS；与本项目已有的 P2P 模块重复 |
| [Janus](https://github.com/meetecho/janus-gateway) | C / GPLv3 | GPLv3 与本仓库 MIT 不兼容，插件式架构改造成本高 |
| [Pion / ion-sfu](https://github.com/pion/webrtc) | Go / MIT | 语言栈不一致，接入需要跨语言进程与序列化 |
| 本实现 | C++17 / MIT | 复用仓内 `VideoChannel`（分片/重组）与 `p2p`（候选/ICESignaling），只补 SDP 子集与转发层；无新第三方依赖 |

SDP 格式与候选优先级均直接按 RFC 实现（RFC 4566 / 8445 / 8839 / 5763），
没有引入 SDP 解析库；信令消息格式是自定义的极简封装（`g2p-signal/1`）。

---

## 8. 测试与验收数据

### 8.1 构建与运行

```bash
cmake -S . -B .build-bento-wsl \
  -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON -DGO2CPP_WITH_P2P=ON
cmake --build .build-bento-wsl --target media_test_sfu media_test_signaling -j 8
ctest --test-dir .build-bento-wsl -R "media_" --output-on-failure
# 或直接跑：
.build-bento-wsl/media_test_sfu
.build-bento-wsl/media_test_signaling
```

### 8.2 结果（2026-11-01，Release）

| 项 | 结果 |
|---|---|
| `media_test_sfu` | **OK，31800 checks，0 failures** |
| `media_test_signaling` | **OK，3998 checks，0 failures** |
| `ctest -R media_` | **9/9 通过**（含既有 7 个媒体测试，总耗时 1.99 s） |
| 编译 | `-Wall -Wextra -Wpedantic -Werror` 零警告；头文件单独 include 可编译 |

### 8.3 用例如实清单

**SFU（7 组）**

1. 单房间 4 peer（2 发布 2 订阅）：20+12 帧、53 个分片，逐帧内容比对；
   跨流零投递，`fragments_forwarded == 53`。
2. 多房间隔离：相同 peer id/stream id 在 room-a/room-b 各自转发，离开 A
   不影响 B。
3. peer 离开清理：发布者离开 → 流删除、其订阅者订阅数递减；订阅者离开 →
   订阅清零；`RemoveRoom` 后计数归零并可重建。
4. 可重入：订阅回调里 `Leave` 发布者，无崩溃、容器一致（延迟回收）。
5. 关键帧门控/重复订阅/上限/退订重订：P 帧被挡、关键帧放行、上限逐个命中。
6. 级联：下游发布 → 上游订阅者收到 10 帧；Stop 后停止；自环/缺房间/hop 超限
   全部拒绝。
7. 随机拓扑：3000 步随机 join/leave/publish/subscribe/forward，与模型逐步
   对账 peer/stream/subscription 数量与投递集合；全局账本（joined-left 等）一致。
8. 异常输入：空指针、非视频 Bento 帧、未知房间/流、无人订阅、状态名映射。

**信令（5 组 + 6 场景）**

1. offer/answer 往返：SDP 文本 → `SignalingMessage` → 解析 → 应用；
   校验 2 路媒体、mid、rtpmap、setup 协商、候选数、统计、逐字节 SDP 往返。
2. 候选交换：`BestRemoteCandidate` 选 host、去重/非法拒绝、排序、connected、
   重复 connected 返回 `kAlreadyConnected`。
3. 状态机：角色错误、重复 offer/answer、超时、ICE 失败、关闭后操作全部拒绝。
4. SDP 格式/容错：20 个必需片段、未知属性忽略、7 类损坏输入拒绝、
   候选 host/srflx/relay/prflx 往返。
5. P2P 集成：4 种候选与 `p2p::Candidate` 互转、优先级一致性、
   `LoopbackSignaling` 承载 offer/answer + 候选 trickle。
6. 随机：500 次 SDP 字节变异（189 次仍合法、311 次被拒且 error 非空）、
   1000 个随机候选往返、300 段随机字节串喂三个解析器不崩溃。

**真实场景（6 个，覆盖全部待验证功能）**

| 场景 | 模拟的真实用法 | 期望 | 实际 |
|---|---|---|---|
| 1v1 通话 | 两端各 3 候选，完整 offer/answer | connected，host 选对 | 通过 |
| 3 方经 SFU | 3 个 client 各与 SFU 会话协商 | 3 个会话 connected，ufrag 互异 | 通过 |
| trickle | 先换描述、后补候选 | 补候选后 connected | 通过 |
| relay 降级 | 只有 relay 候选 | 选中 relay 仍能 connected | 通过 |
| 协商超时 | 假时钟推进 2s+ | failed；新会话可重建 | 通过 |
| answer 校验 | 篡改 mid / setup / 媒体数 | 三类均 `kMediaMismatch`，状态不变 | 通过 |
| 4 人会议 | 2 发布 2 订阅 | 选择性转发、逐帧一致 | 通过（SFU 用例 1） |
| 中途加入 | 订阅者晚于发布者加入 | 等关键帧再开始 | 通过（SFU 用例 4） |

### 8.4 性能（1e6 帧）

条件：WSL2 Ubuntu 24.04，Ryzen 9 8945HX（12 vCPU），Release `-O3`，
单线程；分片 924B（16B Bento 头 + 8B 视频头 + 900B 数据），2 个订阅者。

| 路径 | 样本 | 结果 |
|---|---|---|
| `SfuRouter::ForwardFragment` 裸转发（优化后） | 1,000,000 帧 × 5 次 | **0.0165–0.0167 s，59.7–60.7 M frames/s**（中位 60.5 M） |
| 同上（优化前，通用 `ParseFrame`） | 1,000,000 帧 × 5 次 | 0.0278–0.0288 s，34.7–36.0 M frames/s（中位 35.6 M） |
| `SfuPublisher → SFU → SfuSubscriber`（真实 VideoChannel） | 20,000 帧 × 5 次 | **0.003 s，6.15–6.48 M frames/s** |

裸转发路径每帧 2 次订阅回调，1e6 帧共 2e6 次投递；测试断言
`fragments_forwarded == 2,001,000`（含 1000 次预热）、`frames_forwarded ==
1,001,000`。该数据只代表内存内转发开销，不含 UDP/网络。

热路径**零拷贝**：924B 分片只读 24B 头，转发给订阅者的是指针。按「拷贝给
2 个订阅者」折算，60.5 M frames/s 相当于 111.8 GB/s 的搬运量，已超过单核
`memcpy` 的物理上限——这正是分片指针转发（而不是整帧复制）的原因。

### 8.5 汇编级审查（`docs/reports/asm-sfu.md`、`asm-signaling.md`）

| 指标 | SFU 转发（优化后） | SDP 解析 |
|---|---:|---:|
| 热函数静态指令 | 347 | 1,609 |
| 每帧/次动态指令（自身） | ≈ 263 | ≈ 8,050 |
| 每帧/次动态指令（含子调用） | — | ≈ 134,600 |
| `lock` / 原子 | 0 | 0 |
| 每帧堆分配 | 0 | 0（库内） |
| 实测 | 16.5 ns/帧（2 订阅者） | 6.66 µs/次 |
| 估算下限 | 42–62 cycles/帧 | 1.5–3 µs/次（分配主导） |
| 与下限差距 | 约 1.2–1.8×（按 4.5 GHz 估算） | 约 2.2–4.4×（控制面，可接受） |

SFU 优化前后静态指令几乎不变（345→347），但动态分支减少、IPC 从约 2.2 升到
约 3.5，吞吐提升 1.70×；详见 `docs/reports/asm-sfu.md` §3–§4。

---

## 9. 已知限制与后续

1. **无背压**：`SfuFragmentSink` 是同步回调，订阅者处理慢会阻塞发布者；
   生产环境应在 sink 前面加有界队列/丢帧策略（可复用 Bento 的 congestion/
   bandwidth 模块）。
2. **无 RTCP 反馈**：丢包不会触发 PLI/FIR 请求关键帧；当前靠发布端固定 GOP
   （`MediaConfig::keyframe_interval`）自愈。
3. **无 simulcast/SVC**：一条流只有一份分辨率/码率，不能按订阅者带宽选层。
4. **级联单向**：不支持双向级联/动态选路；hop 上限只是防环保险，不是路由。
5. **SDP 子集**：不支持重协商、rollback、`a=rid`/simulcast、TCP 候选的实际
   连接（候选文本可携带 tcp，但数据面仍走 UDP/Bento）；`a=group:BUNDLE` 只做
   声明与 mid 校验。
6. **安全**：房间加入/发布/订阅无鉴权；`a=fingerprint` 只透传，不做 DTLS
   握手/SRTP 加密。生产部署必须在上层加 Token/ACL，并在媒体面接入 DTLS-SRTP。
7. **单线程**：`SfuRouter` 与 `SignalingSession` 都不加锁，跨线程调用会数据竞争。
8. **只转发视频数据分片**：带 `kFlagAck` 的视频帧与 FEC 校验片不转发
   （`VideoChannel` 的数据分片不会带 ACK；FEC 跨 SFU 的丢包恢复当前未实现，
   需要在 SFU 边界重新做 FEC 或改用 NACK/重传）。

---

## 10. 参考

- RFC 4566（SDP）：<https://www.rfc-editor.org/rfc/rfc4566>
- RFC 8445（ICE）：<https://www.rfc-editor.org/rfc/rfc8445>
- RFC 8839（SDP 中的 ICE 属性）：<https://www.rfc-editor.org/rfc/rfc8839>
- RFC 5763（DTLS-SRTP 的 `a=setup`）：<https://www.rfc-editor.org/rfc/rfc5763>
- 项目内：`docs/modules/webrtc-media.md`（媒体端到端）、
  `docs/modules/p2p-nat-traversal.md`（STUN/ICE/TURN）、
  `include/go2cpp/bento/net/channel.hpp`（VideoChannel 分片/重组契约）。
