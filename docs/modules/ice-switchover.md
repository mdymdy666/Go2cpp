# 候选对热切换（连接迁移）

> 文件：`include/go2cpp/p2p/ice_pair.hpp`、`src/p2p/ice_pair.cpp`、
> `include/go2cpp/p2p/p2p_channel.hpp`、`src/p2p/p2p_channel.cpp`
> 测试：`tests/p2p/test_ice_switchover.cpp`
> 目标：**网络切换（WiFi→4G、NAT 重绑定、端口映射过期）时换底层路径，而不是断连重连**。

---

## 1. 一句话契约

连接建立后，`P2PChannel` 会持续探测所有候选对；当**当前选定对**超过
`no_receive_timeout` 没有收到**任何**包时，切到**优先级最高的可写对**：

- `state()` 仍是 `kConnected`，**不经过** `kConnecting`/`kFailed`；
- `SessionId()` **不变**（上层 mux / WebSocket / RPC 流感知不到迁移）；
- `Send()` / `Recv()` 签名与语义不变，只换 `PeerEndpoint()`；
- 发送序列号**不重置**（对端不会看到假缺口）。

换句话说：**热切换改变的是「发给谁」，不是「有没有连接」**。

---

## 2. 能力与状态

### 2.1 两个状态（对齐 WebRTC 的 `Connection::writable()` / `receiving()`）

| 状态 | 判定 | 含义 | 用途 |
|------|------|------|------|
| `writable` | 该对**曾收到过**成功的 STUN 检查响应，或收到对端的检查请求（收到请求即证明双向可达，回 Success 后成立） | 发出的包能到达对端 | 有资格被选为切换目标 |
| `receiving` | 最近 `no_receive_timeout` 内收到过该对的**任意**包（数据/keepalive/STUN） | 对端的包还能回来 | 判定「当前对已死」 |

`receiving` 的起点取 `max(最近收包时刻, 被选定时刻)`：刚切过去的新对还没有收包
样本，用「被选定时刻」兜底一个宽限期，否则它会立刻被判死、来回切换。

### 2.2 切换判定（`IcePairController::Evaluate`）

| # | 条件 | 结论 |
|---|------|------|
| 1 | 功能关闭 / 没有选定对 | `kNone` |
| 2 | 当前对 `receiving == true` | `kNone`（默认不回切） |
| 3 | 当前对 `receiving == true` 且允许回切、存在「可写 + 在收包 + 优先级**严格更高**」的旧对、冷却期已过 | `kSwitchBack` |
| 4 | 当前对收包超时，且**没有**可写备用对 | `kNoCandidate`（保持当前对，**不断链**） |
| 5 | 当前对收包超时，有可写备用对，但冷却期未过 | `kCooldownBlocked`（选定对**不**改变） |
| 6 | 当前对收包超时，有可写备用对，冷却期已过 | `kSwitch`（切到优先级最高的可写对） |

「最优可写对」按组合优先级 `2^32×MIN + 2×MAX + (G>D)`（RFC 8445 §6.1.2.3）取最大，
同优先级取更近收过包的那条。**不要求目标对已经 `receiving`**：网络刚切过去时它
只可能「可写」，强迫它先收到包才允许切换等于永远不切。

### 2.3 可观测点

| API | 语义 |
|-----|------|
| `SessionId()` | 会话 ID：**热切换不变**，只有新建 `P2PChannel` 才变 |
| `SwitchGeneration()` | 切换代数：每次真正换选定对 +1（回切也算） |
| `CandidatePairs()` | 全部候选对快照（`writable`/`receiving`/`priority`/`rtt_ms`/计数） |
| `SelectedPairRemote()` | 当前选定对的远端（未连接/中继返回无效端点） |
| `EvaluateIceSwitchover()` | 外部驱动一次评估（无收发流量但仍需及时迁移的事件循环用） |
| `P2PStats::ice_probes_sent` / `ice_pair_switches` | 探测包数 / 切换次数 |

---

## 3. 参数

`P2PConfig::ice_switchover`（类型 `IceSwitchoverConfig`）：

| 参数 | 默认 | 含义 | 调参建议 |
|------|------|------|----------|
| `enabled` | `true` | 总开关。关闭后不注册候选对、不发探测包、`Recv()` 不走备用对判定 | 单对场景自动无效，不必手动关 |
| `no_receive_timeout` | `1000ms` | 「不再接收」阈值；`0` 表示禁用该判定（永不因超时切换） | 移动网络 1–2 s；稳定 WiFi 可降到 500 ms |
| `allow_switch_back` | `false` | 原对恢复后是否回切（仅在优先级严格更高、且在收包时） | 默认关闭以避免抖动；路径质量差异大且能容忍抖动时再开 |
| `switch_cooldown` | `2000ms` | 两次切换的最小间隔；`0` 表示不限制 | 建议 ≥ 2 × `no_receive_timeout` |
| `probe_interval` | `500ms` | 主动探测间隔 | 越小迁移越快、包越多；500 ms 与 WebRTC ping 周期同量级 |
| `max_check_failures` | `3` | 从未成功过的对连续失败多少次后标记 `kFailed`（探测降频） | |
| `failed_probe_backoff` | `2` | `kFailed` 对的探测间隔倍数 | 防黑洞地址探测风暴 |
| `max_pairs` | `32` | 候选对上限 | 超出后新对注册失败（不影响已注册对） |

**默认值的理由**：`no_receive_timeout=1s` 与 WebRTC 的 `receiving_timeout`
量级一致；`switch_cooldown=2s` 保证一次迁移后至少两个探测周期内不再动，
避免「切过去→新对还没收包→又切回来」的振荡；`allow_switch_back=false`
是因为弱网下「时通时不通」的旧路径更容易让上层看到地址反复变化。

---

## 4. 实现要点

1. **注册时机**：`Connect()` 算出直连目标后、打洞之前，把
   「本地候选 × 远端直连候选」注册进 `IcePairController`。中继候选不注册。
2. **按远端去重**：本模块所有本地候选共享**同一个 socket**（`SendTo` 的源地址
   由内核路由决定），「同一远端 + 不同本地候选」在物理上是同一条路径。逐对
   注册会出现「代数涨了但 `peer` 没变」的假切换，因此每个远端只保留本地
   优先级最高的那条。
3. **门控**：`m_ice_active` 只在候选对 ≥ 2 时为 `true`；单对场景下 `Recv()` 的
   收发热路径不碰控制器、不发探测包，行为与未引入该功能时一致。
4. **探测**：`Recv()` 循环里按 `probe_interval` 向所有对发 STUN Binding Request
   （`DueProbeTargets` 决定这一轮发给谁）；对端的成功响应或主动请求把该对置为
   `writable`。探测包同时兼任 NAT 映射保活。
5. **来源校验**：业务包只接受「选定对端」或「已确认可写的备用对」的来源。
   候选对来自信令交换，第三方地址不在集合里——这条约束替代了原来「只认
   `peer` 一个地址」的校验，不会被放宽成任意源可注入。
6. **提交点**：`EvaluateSwitchover()` 是唯一的切换提交处。它只做
   `PublishStateIfOpen(kConnected, new_peer)`，**不**改状态、**不**重置
   `m_last_data_sequence`，并重置收包/keepalive 计时到 `now`。

---

## 5. 与 WebRTC 的差异

对照 `p2p/base/basic_ice_controller.cc`（`SelectConnection`/
`ShouldSwitchConnection`）、`p2p/base/p2p_transport_channel.cc`
（`SwitchSelectedConnectionInternal`）、`p2p/base/connection.h`
（`writable()`/`receiving()`）。

| 维度 | WebRTC | 本实现 | 影响 / 说明 |
|------|--------|--------|-------------|
| 候选对与 socket 的关系 | 每个网络（`Port`）有独立 socket，local candidate 对应独立收发端口 | 单 socket，多本地候选共享；按远端去重 | **本实现不能覆盖「本地地址变化」**（WiFi→4G 换网卡），只能换「发往对端的哪个远端地址」；本地绑定变化仍需重建通道 |
| 提名（nomination） | regular/aggressive nomination，`useCandidate` 位 | 无提名；`Connect()` 第一个成功的对即选定 | 初始选对可能不是全局最优（只看谁先回包） |
| 切换判据 | `IceControllerEvent`（`REMOTE_CANDIDATE_ADDED` / `NETWORK_CHANGE` / `SELECTED_CONNECTION_NOT_RECEIVING` / `REMOTE_CANDIDATE_GENERATION_CHANGE`…）+ 综合比较 `writable`/`receiving`/RTT/优先级 | 仅「收包超时 → 最高优先级可写对」 | 不主动切到 RTT 更好的路径；弱网抖动时可能选到 RTT 稍差的路径 |
| receiving 阈值 | `Connection::receiving_timeout_`，由 ping 周期推导（量级 1 s） | 固定 `1000ms`，可配 | 语义对齐；**具体上游常量值未在本地源码逐行核对**，本实现取同量级 |
| ICE restart | 应用层 `SetIceParameters` 换 ufrag/pwd，旧凭据保留，generation 排序 | 无凭据层，无 restart | 不做凭据轮换；NAT 映射彻底过期后仍需上层重建 |
| 旧路径生命周期 | 切换后旧 `Connection` **不销毁**，继续探测，可回切；`PresumedWritable()` 让 TURN-TURN 在 permission 未建好时也报可写 | 旧对不销毁、继续探测，但业务包**单发**只走选定对 | 无「双写」；切换瞬间在途包会丢，靠上层重传 |
| 抖动抑制 | `initial_select_dampening`、receiving 宽限、network 比较 | 冷却期 + 新对宽限期（`selected_since` 兜底） | 机制不同、目标一致 |
| 中继 | TURN 对参与 pair 比较与切换 | relay 不参与（`m_ice_active` 只在直连时成立） | 中继态网络切换仍需重连 |
| 多路径 | 无（WebRTC 也不做吞吐聚合） | 无 | 两边一致 |

---

## 6. 不适用范围

1. **中继连接（`IsRelayed() == true`）**：不注册候选对、不切换；中继态断线只能重连。
2. **本地地址变化**：本实现不重建 socket。WiFi→4G 换网卡/IP 后旧 socket 仍绑定
   旧地址，通常表现为「对端收不到」，需要上层重建 `P2PChannel`（未来 ICE restart
   才会覆盖）。**能覆盖的是**：对端地址变化（NAT 重绑定、多宿主、端口映射过期后
   对端换端口）、对端多候选中的某条路径中断。
3. **不提供可靠传输**：切换期间的丢包由上层负责（RTP/QUIC/RPC 自身的重传）。
4. **对端必须是本模块**：对端若只接受固定源地址且不跑候选对探测，切换后的
   业务包会被它丢弃。
5. **不做 NAT 映射保活之外的保活**：映射彻底过期（例如长时间休眠）后不保证
   仍能直连，需要重新收集候选。
6. **不适用于多路径并发聚合**（MPTCP / QUIC multipath 式吞吐叠加）。
7. **单候选对场景不启用**：没有可切换目标，功能自动关闭（零额外开销）。

---

## 7. 测试与验证

`tests/p2p/test_ice_switchover.cpp`（无外部框架，`P2P_CHECK` + `Summary`）：

| # | 场景 | 断言 |
|---|------|------|
| 1 | 当前对停止收包（注入时钟推进 1.5 s） | `kSwitch`、`event.from/to` 正确、`generation()==1`、选定对改变 |
| 2 | 原对恢复（更高优先级、且 writable+receiving） | `kNone`，选定对**仍是**当前对，代数不变 |
| 3 | 打开 `allow_switch_back` 后原对恢复 | `kSwitchBack`、切回、代数 +1 |
| 4 | 冷却期 | 冷期内 `kCooldownBlocked` 且选定对不变；冷期过后可切 |
| 5 | 无可用备用对 | `kNoCandidate`，保持当前对（不断链） |
| 6 | 探测调度 | 首轮全放行、`probe_interval` 生效、`kFailed` 对探测间隔翻倍 |
| 7 | 快照/查询 API | 排序、`writable`/`receiving`、RTT EWMA、`Clear()` 语义 |
| 8 | 真实双通道在线验证 | 切断 A 的当前路径入向后触发热切换；`SessionId()` 不变、`state()==kConnected`、`SelectedPairRemote()` 改变、切换后出向仍可达 |

实测（Linux 6.x，12 核，2026-11-08）：

- `p2p_test_ice_switchover`：`checks=112 failures=0 skips=0`
- 在线用例实测：5 条候选对，注入前 `selected=127.0.0.1:57202`，注入后
  `selected=10.255.255.254:57202`，`代数=1`、`session` 不变、探测包 24 个；
  切换在 8 s 预算内完成；该测试用例总耗时 0.55 s（含全部 8 个场景）。
- `ctest -R p2p`：6/6 通过（基线 HEAD `7bc4208` 为 5/5，无新增失败）。

---

## 8. 后续可做

1. **ICE restart / 凭据 generation**：NAT 映射过期后重新收集候选并让新候选优先，
   不动连接。
2. **本地 socket 重建**：覆盖 WiFi→4G 的本地地址变化（这是当前最大的能力缺口）。
3. **切换双写窗口**：切换后 3–5 s 内双发，验证新路径可用后再摘旧路径
   （见 `docs/research/webrtc-public-network.md` §5「新旧并存」建议）。
4. **按 RTT 选优**：把 `rtt_ms` 纳入 `BestWritable` 的比较，而不只看静态优先级。
