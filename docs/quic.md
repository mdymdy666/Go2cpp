# QUIC 地基模块（能力边界如实说明）

> **这不是一个 QUIC 实现。** 它只覆盖 QUIC 的**最底层结构**：可变长整数与包头解析。
> 握手、丢包恢复、拥塞控制、QPACK、流复用**全部没有**。
>
> 之所以先做这一层：QUIC 所有上层字段都建立在 varint 与包头之上，
> 这里错一个边界，后面全部错位，而且表现为"偶发解析失败"而非立即崩溃，极难定位。
> 把它做成可独立验证的组件，才有继续往上叠的可能。

## 一、已实现并验证

### 1. 可变长整数（RFC 9000 §16）

`include/go2cpp/network/quic/varint.hpp` + `src/network/quic/varint.cpp`

| 能力 | 说明 |
|---|---|
4 档长度前缀 | `00`/`01`/`10`/`11` → 1/2/4/8 字节 |
值上限 | 2^62 − 1 |
编码 | 总是选能装下的**最短**长度 |
解码 | **接受非最短编码**（RFC 不要求发送方用最短，接收方必须宽容） |
增量友好 | 不足时返回 `kNeedMore` 且 `consumed == 0`，调用方可从原位重试 |

### 2. 包头解析（RFC 9000 §17）

`include/go2cpp/network/quic/packet.hpp` + `src/network/quic/packet.cpp`

| 包类型 | 解析出的字段 |
|---|---|
**Initial** | version、DCID、SCID、Token、Length、PN 偏移 |
**0-RTT** | version、DCID、SCID、Length、PN 偏移 |
**Handshake** | 同上 |
**Retry** | version、DCID、SCID、Retry Token（长度由包总长推得）、16 字节完整性标签 |
**Version Negotiation** | version==0、DCID、SCID、可用版本列表（校验是 4 的倍数） |
**1-RTT（short）** | DCID（**长度由调用方提供**）、PN 偏移 |

## 二、**没有**实现的部分（逐项列出，避免误解）

| 缺失 | 属于哪一层 |
|---|---|
**TLS 1.3 握手集成** | RFC 9001。QUIC 把 TLS 当"提供密钥的握手"，需要 CRYPTO 帧承载 |
**Initial 包的密钥派生与加解密** | RFC 9001 §5.2（HKDF-Expand-Label + AEAD）|
**Header Protection 的解保护/加保护** | RFC 9001 §5.4 |
**Packet Number 的真实值与编码长度** | 见下方"为什么拿不到" |
**帧层**（STREAM/ACK/CRYPTO/MAX_DATA…） | RFC 9000 §19 |
**丢包检测与 PTO** | RFC 9002 |
**拥塞控制** | RFC 9002（NewReno / CUBIC）|
**流复用与流控** | RFC 9000 §4 |
**QPACK（HTTP/3 头部压缩）** | RFC 9204 |
**连接迁移 / 0-RTT 重放防护** | RFC 9000 §9 |

## 三、为什么拿不到 Packet Number

RFC 9001 §5.4 的 header protection 会加密**首字节的低 4 位**和**整个 packet number 字段**。
没有 header protection 密钥，那几位是伪随机的 —— 所以：

- 本模块只给出 `packet_number_offset`（PN 字段的起始偏移）；
- **不提供** `packet_number_length`，因为那 2 位本身就在受保护区域内（long header 的
  type-specific 低 2 位 / short header 的低 2 位）。

这是刻意的：**与其给一个在真实流量上必然错的字段，不如明确不给**。

## 四、验证数据

测试：`tests/network/test_quic_packet.cpp`（424 行，已进 ctest）

| 场景 | 内容 | 结果 |
|---|---|---|
1 | varint 4 档下/上边界（63/64、16383/16384、2^30−1/2^30、2^62−1）+ 非最短编码 + 截断 | ✅ |
2 | varint 随机往返 **200,000 次** | ✅ |
3 | Initial（含 token、Length=1200）逐字段比对 | ✅ |
4 | Handshake / 0-RTT / Retry（token + 16 字节标签） | ✅ |
5 | Version Negotiation（含"版本列表非 4 倍数应非法"） | ✅ |
6 | Short header（DCID 长度 0/4/8/20；0 与 21 的合法性边界） | ✅ |
7 | 畸形输入（fixed bit=0、DCID 声明 21）+ **任意前缀截断必须 kNeedMore** | ✅ |
8 | **随机变异 fuzz 100,000 次**（ok=22283 / need_more=7712 / invalid=70005 / 不崩溃） | ✅ |
9 | 包头随机往返 **20,000 次**（4 种 long 类型 × DCID/SCID 长度 0~20） | ✅ |

全量 ctest：**94/94 通过**。

## 五、库集成评估（为什么这一轮没接 picoquic）

按指示先在 Windows 侧试 clone，两个都成功：

| 库 | 规模 | 结论 |
|---|---|---|
`picoquic` | 244 个 C/H，约 **176,143 行** | **本轮不可集成** |
`ngtcp2` | 250 个 C/H，约 **110,292 行** | 未评估到可集成程度 |

**picoquic 的阻断点**：它的 TLS 层依赖 `picotls`，而仓库里 **`picotls/` 目录是空的**、
`.gitmodules` 也不存在。`CMakeLists.txt` 里 `PICOQUIC_FETCH_PTLS` 默认 **OFF**，
开启后用 `FetchContent` 从 `https://github.com/h2o/picotls.git` 拉取再编译。

也就是说接入 picoquic 需要：拉 picotls（网络）→ 编译 picotls → 编译 picoquic →
适配它的事件循环到本项目的 Fiber/IOManager → 才能跑第一个握手。**60 分钟预算内风险过高**，
而且当时 loadavg 已到 2.3~2.5（多个编译/测试并发），编译大库的时间不可控。

**结论**：库路线**可行但要单独排期**（预估半天：先编通 picoquic 自带的 client/server demo，
再写适配层）。本轮选择做地基，保证交付的是**能验证的真东西**而不是接了一半的烂摊子。

## 六、如果要继续往上做

建议顺序（每一步都能独立验证）：

1. **帧层**（RFC 9000 §19）：STREAM/ACK/CRYPTO/PADDING/PING/CONNECTION_CLOSE 等，
   纯逻辑、无密钥，可以直接对照 RFC 写测试；
2. **Header Protection**（RFC 9001 §5.4）：AES-ECB 或 ChaCha20 的掩码，
   有了它才能拿到真实 packet number；
3. **Initial 密钥派生 + AEAD**（RFC 9001 §5.2）：可以先只做
   "用固定 DCID 派生 initial secrets" → 解出第一包；
4. TLS 集成 —— 这一步要么接 OpenSSL 的 QUIC API（3.2+ 才有完整支持），
   要么接 picotls，是本模块之外的最大工程。
