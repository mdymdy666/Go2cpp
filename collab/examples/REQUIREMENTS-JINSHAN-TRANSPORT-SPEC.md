# Go2Cpp 外包需求：通用传输层 + 协议注入入口

| 项 | 内容 |
|---|---|
| 文档编号 | REF-GO2CPP-01 |
| 版本 | v1.0（2026-11-02） |
| 状态 | **生效契约**。乙方按本文交付；本文未写的，乙方不得自行扩展 |
| 甲方 | JinShanProject（下文「我方」） |
| 乙方 | Go2Cpp 团队（下文「Go2Cpp」） |
| 对齐会 | 2026-11-03，议程见 §8 |
| 依据材料 | `RDP-UDP-RESEARCH.md`（自研 RDT/UDP v1 规范，§6 为交付级契约）、`RDP-REAL-CAPTURE.md`（Windows 实测 19/462/529 字节报文）、`product/common/src/net/*`（现有 `rd::net` 实现）、`product/media/src/tcp_bridge.cpp`（待通用化场景）、`docs/ACCEPTANCE-STATUS.md`（当前验收状态）、ADR-12（通信/并发走 Go2Cpp） |
| 语言/标准 | C++17；MSVC `/W4 /WX`、GCC/Clang `-Wall -Wextra -Wpedantic -Werror`；运行库 `/MT`（Windows） |
| 约束 | 不引入第三方库；不新增线程模型；不改动我方 `product/` 下既有业务代码的语义 |

> 本文出现的 `rd::proto::rdp::*`、`rd::proto::rdt::*` 是**本契约为了划界而定义的甲方命名**，用于明确「这些符号永远不出现在 Go2Cpp 交付物里」；这些模块由我方实现，不属于本次外包范围。

---

## 0. 一页纸摘要

**一句话边界：Go2Cpp 只做「通用传输层」和「协议注入入口」，不实现任何 RDP / 自定义 UDP 协议的业务层。**

| 谁 | 交付什么 |
|---|---|
| Go2Cpp | ① UDP/TCP socket 抽象（含 bind/listen、报文边界、截断可观测）② 事件循环 + 毫秒定时器（单线程多路复用，禁止每连接一线程）③ 缓冲区所有权与生命周期模型 ④ 协议插件接口（`Protocol` / `StreamProtocol` / `ProtocolContext`）⑤ 可靠传输**通用骨架**（重传队列、RTT/RTO 估算器、拥塞控制插入点、序号比较原语）⑥ 通用流中继 `StreamRelay`（`tcp_bridge` 的通用化替身） |
| 我方 | ① RDP 报文解析（TPKT / X.224 / MCS / Security / 快慢路径）② RDT/UDP v1 全部报文格式与状态机（24 字节头、握手、ACK 位图语义、重传触发策略、分片与重组、心跳、错误码）③ RDP 邀请函改写 ④ 会话表/路由 ⑤ 媒体与业务逻辑 |

**交付门禁（未过门禁不算交付）**

| 门禁 | 判据 | 章节 |
|---|---|---|
| G1 编译 | 双平台零告警；C++17；公开头不 leak 平台类型 | §7.2 |
| G2 单测 | 20 个用例全绿（含故障注入与边界） | §7.3 |
| G3 注入 | 我方**不改 Go2Cpp 任何源文件**即可挂入自定义协议，`git diff` 为空 | §7.4 |
| G4 性能 | 双平台量化基线达标（见分级指标） | §7.5 |
| G5 一致 | Windows/Linux 同一用例行为与错误码逐条一致 | §7.6 |
| G6 边界 | 交付代码内零协议符号（grep 判据） | §7.7 |

---

## 1. 边界声明（最重要）

### 1.1 唯一的判断规则

> **凡是可以由「上层协议规范」决定的东西，一律不进 Go2Cpp。Go2Cpp 只提供「让上层协议能挂进去」的机制。**

把一段逻辑放进 Go2Cpp 之前，用下面三个探针自测，任一探针答「要改 Go2Cpp」即说明该逻辑越界：

| 探针 | 提问 | 正确结论 |
|---|---|---|
| A 换协议 | 如果上层从 RDT/UDP v1 换成 QUIC，Go2Cpp 要不要改？ | **不该改**。Go2Cpp 只搬字节，协议换了它不知道 |
| B 换字段宽度 | 如果把 `ack_bits` 从 32 位改成 64 位，Go2Cpp 要不要改？ | **不该改**。说明 ACK 位图语义不属于它 |
| C 换 MTU | 如果把 MTU 从 1200 改成 9000，Go2Cpp 要不要改？ | **不该改**。说明分片/重组不属于它 |

反向探针（用来确认某个能力**确实**属于 Go2Cpp）：如果去掉这段逻辑，上层每个协议都要重写一遍几乎相同的代码（例如「非阻塞等待可读」「socket 生命周期」「到期的包该重传了」的容器与定时器），那它才是通用层。

### 1.2 「Go2Cpp 做 / 我方做」对照表（逐行到符号）

签字方式：对齐会逐行确认，结论只允许三种 —— `Go2Cpp`（乙方实现）、`我方`（甲方实现，乙方不得实现）、`Go2Cpp 改造`（乙方在既有代码上改造）。

| # | 事项 | 归属 | 具体到符号 | 判定依据 |
|---|---|---|---|---|
| 1 | socket 生命周期（创建/绑定/监听/连接/关闭） | Go2Cpp | `go2cpp::platform::SocketHandle::{Create,Bind,Listen,Accept,Connect,Close,ShutdownBoth}` | 与协议无关 |
| 2 | 非阻塞收发 | Go2Cpp | `SocketHandle::{SetNonBlocking,Send,Recv}` | 只搬字节 |
| 3 | 就绪等待（多路复用） | Go2Cpp | `go2cpp::platform::Poller::{Add,Modify,Remove,Wait,Wake}` | 只报告「哪个 fd 就绪」，不解释内容 |
| 4 | 事件循环（回调分发 + 定时器 + 跨线程投递） | Go2Cpp（新增） | `rd::net::EventLoop::{Create,AttachDatagram,AttachStream,Detach,ScheduleAfter,CancelTimer,Post,Run,Stop}` | 调度，不解析 |
| 5 | 毫秒级定时器 | Go2Cpp（新增 + 复用） | `rd::net::EventLoop::ScheduleAfter`、`go2cpp::core::TimerService` | 只给时间点，不给协议语义 |
| 6 | 缓冲区所有权与池化 | Go2Cpp（新增） | `rd::net::Buffer`、`rd::net::BufferPool::{Create,Acquire,Release}` | 只管内存生命周期 |
| 7 | 协议注入接口 | Go2Cpp（新增） | `rd::net::Protocol`、`rd::net::StreamProtocol`、`rd::net::ProtocolContext` | 只定义调用时机与签名 |
| 8 | UDP 报文边界与截断可观测 | Go2Cpp（新增） | `rd::net::DatagramSocket::{RecvFrom,SendTo}` → `IoStatus::kTruncated` | 内核语义，与协议无关 |
| 9 | 重传队列（到期排序容器） | Go2Cpp（新增） | `rd::net::RetransmitQueue::{Push,PopDue,Remove,Find,size}` | 容器 + 排序，不含触发策略 |
| 10 | RTT/RTO 估算 | Go2Cpp（新增） | `rd::net::RttEstimator::{OnSample,srtt,rttvar,rto}`，参数由 `RttParams` 传入 | 公式是通用统计，采样时机由上层决定 |
| 11 | 拥塞控制插入点 | Go2Cpp（新增） | `rd::net::CongestionController`（纯虚，无默认实现） | 只给接口，不给算法 |
| 12 | 序号比较原语（回绕安全） | Go2Cpp（新增） | `rd::net::SeqNum<Seq>::Less/Next/Distance` | 纯算术 |
| 13 | 通用流中继（accept→连上游→双向搬运→重连→半关闭） | Go2Cpp 改造 | `rd::net::StreamRelay::{Start,Stop,stats}`（替身：`product/media/src/tcp_bridge.cpp`） | 字节搬运，不看内容 |
| 14 | RDP TPKT 分帧（`03 00 len`，大端） | 我方 | `rd::proto::rdp::TpktFramer` | RDP 规范（RFC 1006 / T.123） |
| 15 | X.224 CR/CC/DT 编解码 | 我方 | `rd::proto::rdp::X224Codec` | RDP 规范（X.224 Class 0） |
| 16 | MCS BER/PER（Connect Initial 416/462 字节、SDIN） | 我方 | `rd::proto::rdp::McsCodec` | RDP 规范（T.125） |
| 17 | RDP Security 头 / TLS / CredSSP 选择 | 我方 | `rd::proto::rdp::SecurityLayer` | RDP 规范（MS-RDPBCGR §5.3/5.4） |
| 18 | 快路径 / 慢路径解析 | 我方 | `rd::proto::rdp::FastPathCodec`、`rd::proto::rdp::SlowPathCodec` | RDP 规范 |
| 19 | RDT/UDP v1 24 字节定长头 | 我方 | `rd::proto::rdt::Header`（magic `0x4A`、version `0x01`、LE） | 自研规范 §6.2 |
| 20 | RDT 1-RTT 握手（SYN/SYNACK/ACK + nonce 回显） | 我方 | `rd::proto::rdt::Handshake` | 自研规范 §6.4 |
| 21 | ACK 位图语义（`ack_num` + 32 位 `ack_bits`） | 我方 | `rd::proto::rdt::AckBitmap` | 自研规范 §6.5.1 |
| 22 | 重传触发策略（RTO 到期 / 3 次重复 ACK / 8 次上限） | 我方 | `rd::proto::rdt::RetransmitPolicy` | 自研规范 §6.5.2 |
| 23 | 分片与重组（8 字节分片头、`frame_id`、121 片上限、1 MiB 上限） | 我方 | `rd::proto::rdt::Fragmenter`、`rd::proto::rdt::Reassembler` | 自研规范 §6.7 |
| 24 | 心跳与判死（PING/PONG、3×heartbeat、RST 错误码） | 我方 | `rd::proto::rdt::Heartbeat` | 自研规范 §6.6 / §6.11 |
| 25 | 协议错误码语义（`0x0001`–`0x0009`） | 我方 | `rd::proto::rdt::ErrorCode` | 自研规范 §6.11 |
| 26 | RDP 邀请函改写 | 我方 | `rd::media::InvitationRewrite` | 产品逻辑（ADR-03） |
| 27 | 会话表 / 路由 / relay 字节透明转发 | 我方 | `rd::relay::SessionTable`、Go `relayd` | 产品逻辑（ADR-01） |

### 1.3 三个高频分歧的预先裁决

对齐会上最容易扯皮的是「这个算不算通用」。逐条给结论，**不再讨论**：

| 分歧点 | 常见主张 | 裁决 | 理由 |
|---|---|---|---|
| 「ACK 位图是通用可靠传输的一部分」 | Go2Cpp 提供 `ack_bits` 字段与置位/查询 | **驳回**。Go2Cpp 只提供 `SeqNum<Seq>::Less` 与 `RetransmitQueue`；位图由我方解析后翻译成「这些 seq 被确认」再交给队列 | 位图宽度、位序、累积语义都由 RDT/UDP v1 §6.5.1 规定；换成 SACK 或 QUIC 的 ACK ranges 就全变了 |
| 「分片重组是通用能力」 | Go2Cpp 提供 `Reassembler`，按 `frame_id` 拼包 | **驳回**。Go2Cpp 只提供 `BufferPool` 与 `Buffer`；重组状态机由我方实现 | `frame_id`/`frag_index`/`total_len` 是自研规范 §6.7.1 定义的字段；RDP 的 VC 分片又是另一套 |
| 「握手是连接建立，应该通用」 | Go2Cpp 提供 `Connect()` 完成 SYN/SYNACK | **驳回**。Go2Cpp 的 `Connect()` 只做 UDP socket `connect()` 或 TCP 三次握手；RDT 的 1-RTT 握手（nonce 回显、MTU/window 协商、SYN 重传 500ms×5）全部由我方在 `Protocol` 回调里实现 | 握手报文格式、重传节奏、失败错误码都是我方的协议契约 |

### 1.4 边界的机器化判据（G6 的判据来源）

验收时对 Go2Cpp 交付目录跑以下检查，**任意一条命中即判边界违规**：

```bash
# 判定范围：本次交付物（§7.1 清单）。不包含 include/go2cpp/frame、/relay 等既有通用模块。
TARGETS="include/go2cpp/platform/error.hpp include/go2cpp/platform/io_poller.hpp
         src/platform/linux/io_poller.cpp src/platform/windows/io_poller.cpp
         product/common/include/rd/net product/common/src/net"

# 1) 协议魔数/字段名不得出现在交付物中
grep -rnE '0x4[Aa]|"RDRL"|TPKT|X\.224|T\.125|ack_bits|ack_num|session_id|frame_id|frag_index|total_len|nonce' \
  $TARGETS

# 2) 不得出现按内容分支的解析逻辑（首字节判断类型、长度字段解包等）
grep -rnE 'payload\[0\]|data\[0\] ==|switch *\(.*\[0\]\)' \
  $TARGETS

# 3) 不得出现我方协议模块
grep -rn 'rd::proto' $TARGETS
```

唯一允许出现的例外：`§7.4` 注入验证用的 `tests/injection/` 下由**我方**编写的测试协议文件（不属于交付物，不参与 G6 判定）。

---

## 2. 需求一：通用传输抽象

> **边界提醒**：本章只定义「socket 怎么用、内存谁负责、错误怎么表达」。任何「这个字节是什么意思」的判断都不在本章。

### 2.1 我方现有基线（乙方必须以此为实现起点，不得另起一套）

| 现状 | 证据（我方实测） | 缺口 |
|---|---|---|
| `rd::net::Dial("tcp"/"udp")` 可用；`Listen` 仅支持 TCP | `product/common/src/net/go2cpp_transport.cpp:240-248`、`winsock_transport.cpp:197-205` | **UDP 无法 bind/listen**，两个后端都返回 `kNotImplemented`，且 `tests/test_net.cpp:112,133` 已把该行为固化为契约 |
| UDP 只能 `Dial` 后 `Write`，读侧无报文边界 | `go2cpp_transport.cpp:82-96` 直接透传 `Recv` | 报文大于 buffer 时：Linux 静默截断、Windows 报错并被映射为 `kIoError`；**两条平台行为不一致且都不可观测** |
| 无事件循环 | `rd::net` 公共头只有同步 `Read/Write` | 只能用「每连接一线程 + 相对超时」模型，正是 `tcp_bridge.cpp` 现状（每连接 2 线程） |
| `Poller` 已有但 Windows 后端有硬上限 | `src/platform/windows/io_poller.cpp:98-101`：`m_entries.size() + 1 >= FD_SETSIZE` 即返回 `kUnsupported`，`FD_SETSIZE`=64 | Windows 单 loop 最多 63 个 socket，且**未验证**（源码注释自标 `[未验证]`） |
| 定时器已有 | `go2cpp::core::TimerService::{Schedule,ScheduleAfter,ScheduleEvery}` | 未与事件循环集成，回调在独立 worker 线程，需要在 `EventLoop` 内归一 |
| 错误模型过窄 | `rd::base::error.h:23-43` 只有 `kTimeout/kIoError/kCancelled` | **无法区分「对端有序关闭」「RST」「超时」**，只能靠 `Read` 返回 0 猜 |

### 2.2 R1-1 统一 socket 抽象

#### 2.2.1 错误与结果类型（先定这个，其余接口都依赖它）

```cpp
// product/common/include/rd/net/io_status.h
#pragma once
#include <cstddef>
#include <cstdint>

namespace rd::net {

/// IO 结果状态。禁止用 bool 表示「成功/失败」——五类故障必须可区分。
enum class IoStatus : std::uint8_t {
    kOk = 0,          ///< 成功；bytes 有效
    kWouldBlock = 1,  ///< 非阻塞下暂无数据/暂不可写；可重试
    kTimeout = 2,     ///< 等待超时；连接仍可用
    kClosed = 3,      ///< 对端有序关闭（TCP FIN / 已收到半关闭）；本端仍可写
    kReset = 4,       ///< 对端异常重置（TCP RST / ICMP port unreachable）；连接不可用
    kTruncated = 5,   ///< 数据报被截断：报文超过 buffer，剩余部分已丢弃
    kBackpressure = 6,///< 发送队列满；本端未发出任何字节
    kError = 7,       ///< 其它系统错误；见 os_error
};

struct IoResult {
    IoStatus status{IoStatus::kError};
    std::size_t bytes{0};   ///< 已读/已写字节数；失败时语义见各接口注释
    std::int32_t os_error{0}; ///< errno / WSA error；0 表示无
    bool ok() const noexcept { return status == IoStatus::kOk; }
    bool retryable() const noexcept {
        return status == IoStatus::kWouldBlock || status == IoStatus::kBackpressure;
    }
};

/// 端点。host 必须是数字地址（IPv4 点分 / IPv6 冒号形式），不做 DNS。
struct Endpoint {
    std::string host;
    std::uint16_t port{0};
    std::uint8_t family{0};  ///< 4 / 6 / 0=未知
};

}  // namespace rd::net
```

**硬性要求**

| 编号 | 要求 |
|---|---|
| E1 | `kClosed` 与 `kReset` 必须能区分：对端 `FIN`/`shutdown(SD_SEND)` → `kClosed`；收到 `RST` 或 `ECONNRESET`/`WSAECONNRESET` → `kReset` |
| E2 | 超时只能映射为 `kTimeout`，不得映射为 `kIoError`，也不得映射为 `kClosed` |
| E3 | `os_error` 必须填原始 `errno`/`WSA` 码，便于我方日志定位 |
| E4 | 我方 `rd::ErrorCode` 只做**追加**（`kClosed=19`、`kReset=20`、`kTruncated=21`、`kWouldBlock=22`、`kBackpressure=23`），不改既有数值（现有最大值为 `kNotImplemented=18`，见 `product/base/include/rd/base/error.h:42`）；Go2Cpp 在自己的 `ErrorCode` 追加 `kPeerClosed/kReset/kTruncated` 三个值，映射表见 §7.6 |

#### 2.2.2 `DatagramSocket`（UDP，保留报文边界）

```cpp
// product/common/include/rd/net/datagram_socket.h
namespace rd::net {

struct DatagramOptions {
    bool reuse_address{false};
    std::size_t receive_buffer_bytes{0};  ///< 0 = 系统默认
    std::size_t send_buffer_bytes{0};     ///< 0 = 系统默认
};

/// UDP socket。内部一律非阻塞；同步等待是可选的便利方法，不用于事件循环路径。
class DatagramSocket {
   public:
    /// 绑定本地地址（host 可为空表示通配）。UDP 必须能 Bind —— 这是当前缺口。
    static Result<DatagramSocket> Bind(std::string_view address,
                                       const DatagramOptions& options = {});
    /// 连接远端（仅设置默认对端，不做握手）。
    static Result<DatagramSocket> Connect(std::string_view address,
                                          const DatagramOptions& options = {});

    DatagramSocket() noexcept;
    DatagramSocket(DatagramSocket&&) noexcept;
    DatagramSocket& operator=(DatagramSocket&&) noexcept;
    ~DatagramSocket();
    DatagramSocket(const DatagramSocket&) = delete;
    DatagramSocket& operator=(const DatagramSocket&) = delete;

    bool valid() const noexcept;

    /// 恰好消费一个数据报，保留报文边界。
    /// - 成功：status=kOk，bytes=报文长度
    /// - buffer 小于报文：status=kTruncated，bytes=buffer.size()，报文剩余丢弃，from 仍填充
    /// - 非阻塞且无报文：status=kWouldBlock
    /// - 已 connect 且收到 ICMP unreachable：status=kReset
    IoResult RecvFrom(MutableByteView buffer, Endpoint* from);

    /// 发送一个数据报。要么整包发出，要么返回失败；不存在部分发送。
    IoResult SendTo(ByteView payload, const Endpoint& to);
    IoResult Recv(MutableByteView buffer);
    IoResult Send(ByteView payload);

    /// 便利等待（内部用 Poller，只用于单测/CLI，不得在 EventLoop 回调内调用）。
    IoResult WaitReadable(std::chrono::milliseconds timeout);
    IoResult WaitWritable(std::chrono::milliseconds timeout);

    Status SetNonBlocking(bool enable);
    std::string LocalAddr() const;
    std::string PeerAddr() const;
    std::intptr_t NativeHandle() const noexcept;
};

}  // namespace rd::net
```

**硬性要求**

| 编号 | 要求 | 理由 |
|---|---|---|
| D1 | `RecvFrom` 一次调用只消费一个报文，绝不做「凑够 N 字节」的读取 | 报文边界是 UDP 唯一语义；RDT/UDP v1 的 24 字节头按包解析 |
| D2 | buffer 不足必须返回 `kTruncated`，**禁止静默截断** | 静默截断会让 `payload_len` 校验失效（规范 §6.2 约束 1），表现为「偶发丢包」且无法定位 |
| D3 | `SendTo` 禁止部分发送；`bytes` 只能等于 `payload.size()` 或 0 | UDP 无部分发送语义，部分发送只能是实现 bug |
| D4 | `Bind` 支持 `host` 为空（通配）与 `port=0`（系统分配），并且 `LocalAddr()` 能取回真实端口 | 我方测试与 relay 都需要 |
| D5 | 单包上限由调用方控制；Go2Cpp 不得按 MTU 自动切片 | 切片属于协议层（规范 §6.7.2） |

#### 2.2.3 `StreamSocket`（TCP，字节流）

```cpp
// product/common/include/rd/net/stream_socket.h
namespace rd::net {

class StreamSocket {
   public:
    static Result<StreamSocket> Connect(std::string_view address,
                                        const DialOptions& options = {});
    /// 由 Listener 内部构造；对外不暴露「从裸句柄接管」的入口。
    StreamSocket() noexcept;
    StreamSocket(StreamSocket&&) noexcept;
    StreamSocket& operator=(StreamSocket&&) noexcept;
    ~StreamSocket();

    bool valid() const noexcept;

    /// 读一段字节。对端有序关闭：status=kClosed、bytes=0（不是错误）。
    IoResult Read(MutableByteView buffer);
    /// 写完全部字节后返回；内部循环处理部分写。
    /// 仅允许在非事件循环线程调用；事件循环内必须走 ProtocolContext::SendStream。
    IoResult Write(ByteView data);
    /// 半关闭写方向（保留读方向），用于 StreamRelay 的优雅收尾。
    Status ShutdownWrite();
    Status Close();

    IoResult WaitReadable(std::chrono::milliseconds timeout);
    IoResult WaitWritable(std::chrono::milliseconds timeout);

    std::string LocalAddr() const;
    std::string RemoteAddr() const;
    std::intptr_t NativeHandle() const noexcept;
};

}  // namespace rd::net
```

**硬性要求**

| 编号 | 要求 |
|---|---|
| S1 | `Read` 返回 `kClosed` 而不是「`kOk` + bytes=0」；`kOk` 时 `bytes` 必须 > 0 |
| S2 | `Write` 语义是「全写或失败」；失败时 `bytes` 表示已写字节数，调用方据此判断是否需要重发 |
| S3 | `ShutdownWrite` 必须真的发 FIN（`shutdown(SD_SEND)`），不影响读方向 |
| S4 | 公开头不得出现 `SOCKET`/`int fd`/`<winsock2.h>`/`<sys/socket.h>`；平台类型只允许出现在 `std::intptr_t NativeHandle()` 返回值 |

### 2.3 R1-2 非阻塞 + 事件驱动（禁止每连接一线程）

#### 2.3.1 `EventLoop`

```cpp
// product/common/include/rd/net/event_loop.h
#include "rd/net/protocol.h"  // DetachReason / CloseReason / ProtocolHandle / Protocol / StreamProtocol

namespace rd::net {

struct EventLoopOptions {
    std::size_t max_sockets{4096};                  ///< 注册上限；超限 Attach 返回 kResourceExhausted
    std::size_t send_queue_bytes{8u * 1024 * 1024}; ///< 每注册项发送队列上限
    std::chrono::milliseconds timer_tick{1};        ///< 定时器最小推进粒度
};

class EventLoop {
   public:
    static Result<std::unique_ptr<EventLoop>> Create(const EventLoopOptions& options = {});
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    Result<ProtocolHandle> AttachDatagram(DatagramSocket socket, Protocol& protocol);
    Result<ProtocolHandle> AttachStream(StreamSocket socket, StreamProtocol& protocol);
    Status Detach(ProtocolHandle handle, CloseReason reason);

    /// 毫秒定时器；回调在 loop 线程执行。
    Result<TimerId> ScheduleAfter(std::chrono::milliseconds delay,
                                  std::function<void()> callback);
    Status CancelTimer(TimerId id);

    /// 跨线程投递；唯一线程安全的入口之一（另一个是 Stop）。
    Status Post(std::function<void()> task);

    /// 阻塞运行，直到 Stop()。只允许一个线程调用。
    Status Run();
    void Stop();

    bool running() const noexcept;
    std::uint64_t owner_thread_id() const noexcept;
    const EventLoopStats& stats() const noexcept;  ///< 含 pps/丢包/队列水位/定时器延迟
};

}  // namespace rd::net
```

**硬性要求**

| 编号 | 要求 | 验收方式 |
|---|---|---|
| L1 | 一个 `EventLoop` 只绑一个线程；`Run()` 之外不得有任何回调执行 | 单测：在回调内断言 `owner_thread_id() == 当前线程 id` |
| L2 | 连接数增长不得导致线程数增长 | 单测：注册 100 个 UDP socket 前后 `thread_count` 不变（Windows `GetProcessInformation`/Linux `/proc/self/status`） |
| L3 | 回调中禁止阻塞；`EventLoop` 必须提供 `Post` 供其它线程投递 | 单测：回调内 `Post` 一个任务，第二个 tick 执行 |
| L4 | 定时器精度：空闲 loop 下 `ScheduleAfter(10ms)` 的实际触发延迟 **P99 ≤ 2 ms、最大 ≤ 10 ms** | 压测：10 万次定时器，统计偏差直方图 |
| L5 | `max_sockets` 超限必须返回 `kResourceExhausted`，**不得静默丢弃** | 单测：`Options.max_sockets=4`，第 5 次 Attach 断言错误码 |
| L6 | Windows 后端真实容量必须由 `QueryCapabilities().max_sockets_per_loop` 如实报告（当前 select 后端为 63）；谎报判 G5 失败 | 单测：Windows 上填 4096 后 Attach 第 64 个，断言返回 `kUnsupported` 且能力值 = 63 |

#### 2.3.2 后端能力查询（`Backend::kAuto` 的语义基础）

```cpp
// product/common/include/rd/net/net.h（追加，不改既有声明）
namespace rd::net {

struct BackendCapabilities {
    bool datagram_bind{false};
    bool datagram_recvfrom{false};   ///< 且能报告 kTruncated
    bool event_loop{false};
    bool timer_ms{false};
    bool zero_copy_send{false};      ///< SendDatagram(ByteView) 不拷贝
    std::size_t max_sockets_per_loop{0};
};

/// 查询后端真实能力；kAuto 时返回当前实际会选中的后端的能力。
Result<BackendCapabilities> QueryCapabilities(Backend backend);

}  // namespace rd::net
```

**硬性要求**

| 编号 | 要求 |
|---|---|
| B1 | `QueryCapabilities` 的每个字段必须与实现一致；§7.6 会逐字段构造反例验证（例如谎报 `datagram_bind=true` 但实际返回 `kNotImplemented`） |
| B2 | `Backend::kAuto` 的选择在**进程内首次调用时锁定**并记录到日志；同一进程内不允许 TCP 走一个后端、UDP 走另一个后端 |
| B3 | `kAuto` 遇到 `kNotImplemented`/`kUnsupported` 才回退（保持现有语义，`net.cpp:16-18`）；真实 IO 错误不得触发回退 |

### 2.4 R1-3 缓冲区所有权（谁分配、谁释放、零拷贝到什么程度）

#### 2.4.1 三种模式，只有三种

| 模式 | 用于 | 谁分配 | 谁释放 | 生命周期 |
|---|---|---|---|---|
| **借用（borrowed）** | `OnDatagram(ByteView)` 的入参、`SendDatagram(ByteView)` 的入参 | Go2Cpp（收）/ 上层（发） | Go2Cpp（收）/ 上层（发） | 回调返回即失效（收）；`OnSendComplete` 返回即失效（发） |
| **移交（owned）** | `SendDatagram(BufferPtr)`、`BufferPool::Acquire` | 上层从池取 | 引用计数归零时自动归还池 | 直到最后一个 `BufferPtr` 释放 |
| **池化（pooled）** | 收侧大流量路径 | `BufferPool` | `BufferPool` | 同 owned |

```cpp
// product/common/include/rd/net/buffer.h
namespace rd::net {

/// 固定容量缓冲；不可拷贝。数据长度与容量分离（size <= capacity）。
class Buffer {
   public:
    char* data() noexcept;
    const char* data() const noexcept;
    std::size_t capacity() const noexcept;
    std::size_t size() const noexcept;
    void set_size(std::size_t size) noexcept;  ///< size 不得 > capacity
};
using BufferPtr = std::shared_ptr<Buffer>;

class BufferPool {
   public:
    /// buffer_size 为单块容量；capacity 为池内块数上限。
    static Result<std::unique_ptr<BufferPool>> Create(std::size_t buffer_size,
                                                      std::size_t capacity);
    /// 池空返回 kResourceExhausted（不阻塞、不分配新块）。
    Result<BufferPtr> Acquire();
    std::size_t available() const noexcept;
    std::size_t buffer_size() const noexcept;
};

}  // namespace rd::net
```

**硬性要求**

| 编号 | 要求 | 验收方式 |
|---|---|---|
| M1 | 收侧从内核到 `OnDatagram` 之间 **0 次额外拷贝**（内核 → Go2Cpp 缓冲 1 次不可避免，之后到回调不得再拷贝） | 用 `memcpy` 计数钩子（`LD_PRELOAD`/MSVC `/Gh` 或 Go2Cpp 自带 `rd_net_memcpy_count` 计数器）统计：每包 ≤ 1 次 |
| M2 | `SendDatagram(ByteView)` 不拷贝；实现方必须在 `OnSendComplete` 之后才允许引用该内存 | 单测：`SendDatagram` 返回后立刻把源缓冲 `memset` 成 `0xCC`。**对端收到 `0xCC` = 未拷贝（合规）；收到原值 = 实现偷偷拷贝了（判 bug）** |
| M3 | `BufferPool` 不得在热路径 `malloc`；池耗尽必须返回错误而不是扩容 | 压测：池 `capacity=64`，并发取 65 次，第 65 次断言 `kResourceExhausted`；`malloc` 计数为 0 |
| M4 | `Buffer` 的 `set_size` 越界必须 `assert`（Debug）并截断为 capacity（Release），不得越界写 | 单测：`set_size(capacity+1)` |

### 2.5 本章验收

| 项 | 方式 | 判据 |
|---|---|---|
| R1-1 | 单测 `test_datagram_socket.cpp` / `test_stream_socket.cpp` | 覆盖 D1–D5、S1–S4；含「报文 1 字节」「报文 = buffer」「报文 = buffer+1」三边界 |
| R1-2 | 单测 `test_event_loop.cpp` | L1–L6 逐条断言；线程数不随连接数增长 |
| R1-3 | 单测 + `memcpy` 计数 | M1–M4 逐条断言 |

---

## 3. 需求二：协议注入入口（核心）

> **边界提醒**：本章是整份契约的重心。Go2Cpp 在这里只做一件事 —— **把字节搬进搬出 + 调度**。它不知道字节里是什么，也不允许去猜。

### 3.1 接口设计的四个决定（先定，再写签名）

| 决定 | 结论 | 理由 |
|---|---|---|
| 数据怎么交给上层 | `ByteView`（= `std::string_view`，二进制安全）**借用**语义 + `BufferPtr` **移交**语义，两条路都给 | 借用满足零拷贝；移交满足「回调里不能长时间持有」的场景（重组缓冲） |
| 上层怎么请求发送 | **一律入队**，返回 `SendResult{status, token}`；不提供「同步 send」 | 同步 send 会在回调里阻塞整个 loop，等于回到每连接一线程 |
| 定时器怎么给 | `ProtocolContext::ScheduleAfter(毫秒)` + `OnTimer(TimerId)`，回调在 loop 线程 | RTO 需要毫秒级；跨线程定时器会让协议状态机加锁 |
| 生命周期怎么保证 | `Protocol` 由上层持有；`Attach`/`Detach` 显式；`OnDetached` 返回后不再有任何回调 | 避免「回调里析构自己」的悬空 |

### 3.2 回调点清单（Go2Cpp 只在这些时刻调用上层）

| 回调 | 何时调用 | 线程 | 允许做什么 | 禁止做什么 |
|---|---|---|---|---|
| `OnAttached(ctx)` | `AttachDatagram/AttachStream` 成功后**下一个 loop tick** | loop | 读 `local_endpoint()`、注册定时器、发首包 | 阻塞、析构 loop、`Detach` 自己 |
| `OnDatagram(ctx, payload, from)` | 每收到一个 UDP 报文 | loop | 解析、改状态、`SendDatagram`、`ScheduleAfter` | 持有 `payload` 超过返回；阻塞；`delete this` |
| `OnBytes(ctx, data)` | TCP 有可读字节（**不保证消息边界**） | loop | 追加到自己的分帧缓冲、`SendStream` | 假设「一次回调 = 一条消息」 |
| `OnTimer(ctx, id)` | 注册的定时器到期 | loop | 改状态、发重传、再次 `ScheduleAfter` | 在回调内 `CancelTimer(自己)` 后仍期待不再触发（允许，但无意义） |
| `OnSendComplete(ctx, token, status)` | 一个入队项被发出/丢弃 | loop | 回收发送缓冲、统计 | 在此回调里同步阻塞等待对端 |
| `OnDetached(ctx, reason)` | `Detach` 完成、对端关闭、loop 停止 | loop | 释放资源、上报 | 再调用 `ctx` 的任何方法（已失效） |

### 3.3 C++ 接口原型（逐字符确认，签字后不得变更）

```cpp
// product/common/include/rd/net/protocol.h
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

#include "rd/net/buffer.h"        // BufferPtr
#include "rd/net/datagram_socket.h"  // ByteView / MutableByteView / Endpoint
#include "rd/net/io_status.h"

namespace rd::net {

/// 断链原因（OnDetached 的入参）。
enum class DetachReason : std::uint8_t {
    kClosed = 0,  ///< 对端有序关闭（收到 FIN / 已收到半关闭）
    kReset = 1,   ///< 对端重置（收到 RST / ICMP unreachable）
    kTimeout = 2, ///< 本端请求的超时关闭
    kStopped = 3, ///< 上层 Detach 或 loop 停止
    kError = 4,   ///< 其它系统错误
};

/// 主动关闭原因（ProtocolContext::Close 的入参）。
enum class CloseReason : std::uint8_t { kNormal = 0, kAbort = 1, kError = 2 };

/// 只读字节视图。二进制安全：不假设 UTF-8、不含 NUL 终止。
using ByteView = std::string_view;

/// 可写字节视图（收侧写入用）。
struct MutableByteView {
    char* data{nullptr};
    std::size_t size{0};
};

using TimerId = std::uint64_t;        ///< 非 0；在同一 ProtocolContext 内唯一
using SendToken = std::uint64_t;      ///< 非 0；由 SendDatagram/SendStream 返回
using ProtocolHandle = std::uint64_t; ///< 非 0；由 EventLoop::Attach* 返回

struct SendResult {
    IoStatus status{IoStatus::kError};
    SendToken token{0};  ///< status==kOk 时有效
    bool ok() const noexcept { return status == IoStatus::kOk; }
};

/// 上层协议在 loop 线程内操作 socket/定时器的唯一入口。
/// 生命周期：OnAttached 传入，OnDetached 返回后失效；不得拷贝或跨线程保存。
class ProtocolContext {
   public:
    ProtocolContext(const ProtocolContext&) = delete;
    ProtocolContext& operator=(const ProtocolContext&) = delete;
    virtual ~ProtocolContext() = default;

    // ---- 发送（全部入队，不阻塞）----

    /// 发送一个 UDP 报文。不拷贝：payload 必须活到对应 OnSendComplete。
    /// 队列满：status=kBackpressure，token=0，未发出任何字节。
    /// 已关闭：status=kClosed。
    virtual SendResult SendDatagram(ByteView payload, const Endpoint& to) = 0;

    /// 发送一个 UDP 报文，所有权移交给 loop（发出或丢弃后由 loop 释放 BufferPtr）。
    virtual SendResult SendDatagram(BufferPtr payload, const Endpoint& to) = 0;

    /// 发送 TCP 字节。不拷贝：payload 必须活到对应 OnSendComplete。
    virtual SendResult SendStream(ByteView payload) = 0;
    virtual SendResult SendStream(BufferPtr payload) = 0;

    // ---- 定时器（毫秒）----

    /// 注册一次性定时器；返回非 0 id。delay<=0 表示下一个 tick 触发。
    /// loop 已停止或队列失效时返回 0（0 恒为无效 id）。
    virtual TimerId ScheduleAfter(std::chrono::milliseconds delay) = 0;
    /// 取消；已进入待执行队列的回调会被跳过（不会再收到 OnTimer）。
    virtual void CancelTimer(TimerId id) = 0;

    // ---- 连接控制 ----

    /// 请求关闭；loop 会在当前回调返回后停止该注册项并回调 OnDetached。
    virtual void Close(CloseReason reason) = 0;
    /// 仅流：半关闭写方向，读方向继续。
    virtual void ShutdownWrite() = 0;

    // ---- 只读信息 ----

    virtual Endpoint local_endpoint() const = 0;
    virtual Endpoint remote_endpoint() const = 0;
    virtual std::size_t send_queue_bytes() const noexcept = 0;
    virtual std::size_t send_queue_datagrams() const noexcept = 0;
};

/// UDP 协议插件。
class Protocol {
   public:
    virtual ~Protocol() = default;

    virtual void OnAttached(ProtocolContext& ctx) = 0;
    virtual void OnDatagram(ProtocolContext& ctx, ByteView payload,
                            const Endpoint& from) = 0;
    virtual void OnTimer(ProtocolContext& ctx, TimerId id) = 0;
    virtual void OnSendComplete(ProtocolContext& ctx, SendToken token,
                                IoStatus status) = 0;
    virtual void OnDetached(ProtocolContext& ctx, DetachReason reason) = 0;
};

/// TCP 协议插件。注意：OnBytes 不保证消息边界，分帧是上层的事。
class StreamProtocol {
   public:
    virtual ~StreamProtocol() = default;

    virtual void OnAttached(ProtocolContext& ctx) = 0;
    virtual void OnBytes(ProtocolContext& ctx, ByteView data) = 0;
    virtual void OnTimer(ProtocolContext& ctx, TimerId id) = 0;
    virtual void OnSendComplete(ProtocolContext& ctx, SendToken token,
                                IoStatus status) = 0;
    virtual void OnDetached(ProtocolContext& ctx, DetachReason reason) = 0;
};

}  // namespace rd::net
```

**接口约束（乙方必须实现，甲方按此验收）**

| 编号 | 约束 |
|---|---|
| P1 | 所有回调都在 `EventLoop::owner_thread_id()` 线程执行；协议实现不需要任何锁 |
| P2 | `SendDatagram`/`SendStream` 必须在**同一 tick 内**尝试发出（不允许「攒 100 个再发」）；无法立即发出时入队并注册可写兴趣 |
| P3 | 回调内调用 `Send*` 不得递归回调 `OnSendComplete`（必须延后到本次回调返回之后），避免重入 |
| P4 | `OnDatagram` 的 `payload` 在回调返回后立即失效；`OnBytes` 的 `data` 同理 |
| P5 | `CancelTimer` 之后该 `id` 永不再触发；`id` 不复用（哪怕回绕也要跳过 0） |
| P6 | `OnDetached` 返回后，该 `ProtocolContext` 的所有方法返回失败（`kClosed`）或 `noexcept` 无操作；不得崩溃 |
| P7 | `Detach` 与 `OnDetached` 之间不得再有任何 `OnDatagram/OnBytes/OnTimer` |
| P8 | 发送队列有上限（`EventLoopOptions::send_queue_bytes`），超限返回 `kBackpressure` 并保证「未发出任何字节」 |
| P9 | 单 tick 内每个 socket 最多处理 **64 个数据报**，剩余留到下一 tick；防止一个 socket 饿死其它 socket |
| P10 | 单 tick 内的回调顺序固定（见下），不得因平台不同而改变 |

**头文件依赖（单向，无循环；乙方按此组织，否则实现会陷入循环 include）**

```text
io_status.h           ← 无依赖
buffer.h              ← 无依赖
datagram_socket.h     ← io_status.h
stream_socket.h       ← io_status.h, net.h(DialOptions)
protocol.h            ← buffer.h, datagram_socket.h, io_status.h
event_loop.h          ← protocol.h, datagram_socket.h, stream_socket.h
stream_relay.h        ← event_loop.h
```

规则：`protocol.h` **不得** include `event_loop.h`（否则循环）；`EventLoop` 只通过 `protocol.h` 里的类型与协议交互。

**`CloseReason` → `OnDetached(DetachReason)` 映射（必须一一对应）**

| 触发 | `OnDetached` 收到 |
|---|---|
| `ctx.Close(CloseReason::kNormal)` | `kStopped` |
| `ctx.Close(CloseReason::kAbort)` | `kReset` |
| `ctx.Close(CloseReason::kError)` | `kError` |
| 未调 `Close`，对端 FIN | `kClosed` |
| 未调 `Close`，对端 RST | `kReset` |
| `EventLoop::Stop()` | `kStopped` |

**单 tick 内的回调顺序（P10 展开）**

| 序 | 阶段 | 动作 |
|---|---|---|
| 1 | 读就绪 | 对每个就绪 socket 调 `OnDatagram` / `OnBytes`，每 socket 至多 64 次（P9） |
| 2 | 写就绪 | 推进发送队列，发出后调 `OnSendComplete` |
| 3 | 定时器 | 按 `due` 升序调 `OnTimer` |
| 4 | 投递队列 | 执行 `Post` 进来的任务 |
| 5 | 生效关闭 | 调 `OnDetached`，释放注册项 |

### 3.4 我方 RDT/UDP v1 如何挂进去（证明接口够用的最小样例）

下面是**我方会写的真实代码形态**（不是伪代码）。它证明：24 字节头解析、握手、ACK 位图、分片全部在我方，Go2Cpp 一行都不用改。

```cpp
// 我方：product/common/src/proto/rdt/rdt_udp_protocol.cpp
namespace rd::proto::rdt {

class RdtUdpProtocol final : public rd::net::Protocol {
   public:
    explicit RdtUdpProtocol(RdtConfig config) : m_config(config) {}

    void OnAttached(rd::net::ProtocolContext& ctx) override {
        m_rto = m_config.rto_init;                 // 200ms（我方常量，Go2Cpp 不知情）
        m_retransmit_timer = ctx.ScheduleAfter(m_rto);
    }

    void OnDatagram(rd::net::ProtocolContext& ctx, rd::net::ByteView payload,
                    const rd::net::Endpoint& from) override {
        if (payload.size() < kHeaderLen) return;   // 24 字节头是我方规范 §6.2
        const Header h = ParseHeader(payload);     // 我方解析 magic/version/type/flags
        if (!h.valid) return;

        switch (h.type) {
            case PacketType::kSyn:      SendSynAck(ctx, h); break;
            case PacketType::kData:
                m_reassembly.Accept(h, payload.substr(kHeaderLen));  // 我方分片重组
                m_ack_bitmap.OnReceived(h.seq);                      // 我方位图语义
                SendAckIfDue(ctx);                                   // 我方 ACK 策略（10ms/2 包）
                break;
            case PacketType::kAck:
                // 只把「哪些 seq 被确认」告诉 Go2Cpp 的容器，位图解释权在我方
                for (const auto seq : m_ack_bitmap.Expand(h.ack_num, h.ack_bits)) {
                    m_retransmit.Remove(seq);
                }
                break;
            default: break;
        }
    }

    void OnTimer(rd::net::ProtocolContext& ctx, rd::net::TimerId id) override {
        if (id == m_retransmit_timer) {
            // Go2Cpp 只提供「到期了」这个事件；重传什么、几次、退避多少由我方决定
            for (const auto& entry : m_retransmit.PopDue(Now())) {
                if (entry.retransmits >= m_config.max_retransmit) {
                    SendRst(ctx, ErrorCode::kTimeout);
                    ctx.Close(rd::net::CloseReason::kError);
                    return;
                }
                ResendFragment(ctx, entry);        // 重发分片：frame_id 不变，seq 变
            }
            m_retransmit_timer = ctx.ScheduleAfter(m_config.rto_max_backoff);
        }
    }

    void OnSendComplete(rd::net::ProtocolContext&, rd::net::SendToken, rd::net::IoStatus) override {}
    void OnDetached(rd::net::ProtocolContext&, rd::net::DetachReason) override {}

   private:
    RdtConfig m_config;
    rd::net::RetransmitQueue m_retransmit;   // Go2Cpp 的容器
    Reassembler m_reassembly;                // 我方的重组状态机
    AckBitmap m_ack_bitmap;                  // 我方位图
    rd::net::TimerId m_retransmit_timer{0};
    std::chrono::milliseconds m_rto{0};
};

}  // namespace rd::proto::rdt
```

**这段样例的意义**：它同时是 §7.4 注入验收的模板。如果乙方交付的接口让这段代码需要 `#include <go2cpp/...>` 或需要改 Go2Cpp 源码才能编译，即判 G3 失败。

### 3.5 Go2Cpp 明确不做的事（写给实现者，防止「顺手帮忙」）

| 不做 | 具体含义 | 违规示例 |
|---|---|---|
| 不解析首字节 | 不得用 `payload[0]` 判断包类型 | `if (p[0] == 0x4A) ...` |
| 不校验字段 | 不得校验 `magic`/`version`/`payload_len` 自洽性 | `if (hdr.len != size - 24) drop();` |
| 不做分帧 | TCP 上不得按长度前缀切消息 | 内置「4 字节长度 + body」的 framer |
| 不做重排 | 不得按序号缓存乱序包 | 内置 jitter buffer |
| 不做重传决策 | 不得决定「何时重传」 | 内置「3 次重复 ACK 触发重传」 |
| 不做握手 | 不得内置任何 SYN 语义 | `Connect()` 里有 `SYN` 状态机 |
| 不认 magic | 不得对 payload 做任何魔数匹配 | `memcmp(p, "RDRL", 4)` |
| 不做统计口径 | 不得统计「协议层丢包」 | `protocol_packets_lost` 计数器 |

> 允许的统计只有传输层口径：`bytes_sent/bytes_recv/datagrams_sent/datagrams_recv/queue_bytes/timer_delay_us`。

### 3.6 本章验收

| 项 | 方式 | 判据 |
|---|---|---|
| R2-1 接口完整性 | 逐行比对 §3.3 的 8 个方法签名 | 签名、const、noexcept、返回类型**逐字符一致**；少一个即不通过 |
| R2-2 回调线程 | 单测在回调内断言线程 id | 100% 命中 loop 线程 |
| R2-3 零拷贝 | `memcpy` 计数 + M2 的 `memset` 反证 | 每包 ≤ 1 次；`memset` 反证通过 |
| R2-4 背压 | 单测填满发送队列 | 第 N+1 次返回 `kBackpressure` 且线上未新增字节 |
| R2-5 注入 | §7.4 | **不改 Go2Cpp 代码**编译并跑通我方测试协议 |
| R2-6 协议无关 | §1.4 的 grep | 零命中 |

---

## 4. 需求三：可靠传输的「通用骨架」

> **边界提醒**：本章交付的是**骨架**（容器 + 定时器驱动 + 统计公式 + 接口），不是**策略**（何时 ACK、重传几次、窗口怎么涨）。RDT/UDP v1 的策略全在我方规范 §6.5，Go2Cpp 不得实现其中任何一条。

### 4.1 骨架与策略的分界

| 能力 | 归属 | 说明 |
|---|---|---|
| 重传队列（按到期时间排序的容器） | Go2Cpp | `Push/PopDue/Remove/Find`；不含「何时 Push」 |
| 定时器驱动（下一次到期时间） | Go2Cpp | `NextDeadline()` 供 loop 计算 sleep 时长 |
| RTT 采样统计（SRTT/RTTVAR/RTO 公式） | Go2Cpp | 参数（alpha/beta/K/min/max）由上层传；不含「哪些包可采样」（Karn 算法在上层） |
| 拥塞控制插入点 | Go2Cpp | 纯虚接口 + 空实现；算法由上层注入 |
| 序号比较（回绕安全） | Go2Cpp | 纯算术，模板化 `Seq` |
| 何时 ACK、ACK 位图宽度与位序 | 我方 | RDT 规范 §6.5.1 |
| 快速重传（3 次重复 ACK） | 我方 | 规范 §6.5.2 |
| 重传上限（8 次）与断连 | 我方 | 规范 §6.5.2 / §6.11 |
| 拥塞算法（慢启动/拥塞避免/AIMD 参数） | 我方 | 规范 §6.5.5 |
| 握手、分片、重组、心跳 | 我方 | 规范 §6.4 / §6.7 / §6.6 |

### 4.2 序号空间：必须允许上层自定义语义

**这是本需求最容易做错的地方。** 我方分片方案是「**每个 UDP 包占一个 seq**」（规范 §6.7.4：分片各自占用 `seq_num`，与 `frag_index` 无固定映射）。因此 Go2Cpp 的骨架**不得假设**：

| 禁止的假设 | 为什么错 |
|---|---|
| 「seq 是字节偏移」 | 我方的 seq 是包序号，与字节数无关 |
| 「seq 是帧号」 | 一个帧可能占多个 seq；重传会改变 seq 与 frag_index 的对应 |
| 「seq 单调递增且不回绕」 | 32 位序号会回绕，必须用 `(int32)(a-b) > 0` 语义 |
| 「seq 从 0 开始」 | 骨架必须支持任意起点 |
| 「一个 seq 对应一次发送」 | 重传会让同一逻辑内容出现在新 seq 上 |

```cpp
// product/common/include/rd/net/seq_num.h
namespace rd::net {

/// 回绕安全的序号运算。Seq 必须是 32 位或 64 位无符号整型。
template <typename Seq>
struct SeqNum {
    static_assert(std::is_unsigned_v<Seq>, "Seq must be unsigned");

    /// a 是否在 b 之前（回绕安全）。
    static bool Less(Seq a, Seq b) noexcept {
        return static_cast<std::make_signed_t<Seq>>(a - b) < 0;
    }
    static Seq Next(Seq v) noexcept { return static_cast<Seq>(v + 1); }
    /// from 到 to 的无符号距离（用于窗口判定，不做回绕判断）。
    static Seq Distance(Seq from, Seq to) noexcept {
        return static_cast<Seq>(to - from);
    }
    /// a 与 b 的差是否在 [0, window) 内。
    static bool InWindow(Seq a, Seq b, Seq window) noexcept {
        return Distance(b, a) < window;
    }
};

}  // namespace rd::net
```

| 编号 | 要求 | 验收 |
|---|---|---|
| Q1 | `SeqNum` 必须对 `uint32_t` 与 `uint64_t` 都可用 | 单测两种类型各 3 组回绕用例（`0xFFFFFFFF`→`0`、`0xFFFFFFFE`→`1`） |
| Q2 | 骨架任何地方不得对 `Seq` 做 `<`/`>` 直接比较，只能用 `SeqNum<Seq>::Less` | 代码审查 + 单测：注入 `Seq=uint32_t`、初值 `0xFFFFFFF0`，全流程正确 |

### 4.3 重传队列

```cpp
// product/common/include/rd/net/retransmit_queue.h
namespace rd::net {

/// 一个已发出、尚未被确认的条目。seq 的语义完全由上层解释；骨架只按 due 排序。
struct RetransmitEntry {
    std::uint64_t seq{0};                          ///< 上层序号（窄化由上层负责）
    std::chrono::steady_clock::time_point due{};   ///< 下次到期时间
    std::uint32_t retransmits{0};                  ///< 已重传次数（骨架只累加，不判定上限）
    SendToken token{0};                            ///< 关联的发送凭证（可为 0）
};

class RetransmitQueue {
   public:
    /// 插入或覆盖同一 seq 的项。
    void Push(const RetransmitEntry& entry);

    /// 取出所有 due <= now 的项（按 due 升序）；不影响队列内其余项。
    std::vector<RetransmitEntry> PopDue(std::chrono::steady_clock::time_point now);

    /// 按 seq 删除（收到 ACK）。返回是否删除。
    bool Remove(std::uint64_t seq);
    /// 按 seq 查到并累加 retransmits，返回累加后的次数；不存在返回 0。
    std::uint32_t BumpRetransmit(std::uint64_t seq,
                                 std::chrono::steady_clock::time_point next_due);

    bool Find(std::uint64_t seq, RetransmitEntry* out) const;
    /// 最早到期时间；空队列返回 time_point::max()。
    std::chrono::steady_clock::time_point NextDeadline() const;
    std::size_t size() const noexcept;
    void Clear() noexcept;

   private:
    // 实现要求：按 due 有序（最小堆或有序 map 均可），Push/Remove/PopDue 均摊 O(log n)
};

}  // namespace rd::net
```

| 编号 | 要求 | 验收 |
|---|---|---|
| RQ1 | `Push`/`Remove`/`PopDue` 均摊 O(log n)；10 万项下单次操作 ≤ 1 µs | 微基准：10 万项随机 Push/Remove 混合，P99 ≤ 1 µs |
| RQ2 | `PopDue` 返回顺序严格按 `due` 升序；相同 `due` 按插入顺序 | 单测：100 项同 `due`，断言顺序稳定 |
| RQ3 | 骨架**不得**内置重传次数上限；`BumpRetransmit` 只累加 | 单测：连续 Bump 1000 次仍返回 1000，不自动删除 |
| RQ4 | `NextDeadline()` 空队列必须返回 `time_point::max()`，供 loop 计算无限 sleep | 单测 |

### 4.4 RTT / RTO 估算器（公式是通用统计，参数是策略）

```cpp
// product/common/include/rd/net/rtt_estimator.h
namespace rd::net {

struct RttParams {
    double srtt_alpha{0.125};   ///< RFC 6298: 1/8
    double rttvar_beta{0.25};   ///< RFC 6298: 1/4
    double rto_k{4.0};          ///< RFC 6298: 4
    std::chrono::milliseconds rto_min{100};
    std::chrono::milliseconds rto_max{3000};
};

class RttEstimator {
   public:
    explicit RttEstimator(const RttParams& params = {});
    /// 喂一个 RTT 样本；首个样本按 RFC 6298 §2.2 初始化（srtt=sample, rttvar=sample/2）。
    /// 是否采样（Karn 算法）由上层决定，估算器不做判断。
    void OnSample(std::chrono::milliseconds sample);
    std::chrono::milliseconds srtt() const noexcept;
    std::chrono::milliseconds rttvar() const noexcept;
    /// clamp(srtt + rto_k * rttvar, rto_min, rto_max)。
    std::chrono::milliseconds rto() const noexcept;
    /// 无样本时返回 rto_min（上层可自行使用 rto_init）。
    bool has_sample() const noexcept;
};

}  // namespace rd::net
```

| 编号 | 要求 | 验收 |
|---|---|---|
| RT1 | 首样本、后续样本、`srtt+4*rttvar` 三条公式必须与 RFC 6298 §2.2/2.3 **逐位一致**（整型毫秒运算，误差 0） | 单测：给定样本序列 `{100,120,80}`，断言 `srtt=... ` 精确等于手算值 |
| RT2 | `rto()` 必须 clamp；`rto_min=rto_max` 时的退化也要正确 | 单测 |
| RT3 | 估算器**不得**内置「重传后 RTO 翻倍」；退避由上层做 | 代码审查 + 单测：连续 `OnSample` 不改变任何「退避状态」（无此状态） |
| RT4 | 无样本时 `rto()` 返回 `rto_min`，不得返回 0 或 max | 单测 |

> 注意：我方规范 §6.5.3 的 `RTO_INIT=200ms`、`RTO_MIN=100ms`、`RTO_MAX=3000ms`、`RTO_MAX_BACKOFF=6000ms` 由我方在 `RdtConfig` 里传入，**不写进 Go2Cpp 默认值**。Go2Cpp 的默认值就是 RFC 6298 的 100/3000，避免「默认值即协议」。

### 4.5 拥塞控制插入点

```cpp
// product/common/include/rd/net/congestion.h
namespace rd::net {

enum class LossKind : std::uint8_t {
    kRto = 0,          ///< RTO 超时
    kFastRetransmit,   ///< 快速重传
    kCongestionEvent,  ///< 显式拥塞通知（如未来 ECN）
};

/// 拥塞控制策略接口。Go2Cpp 只提供接口与调用点，不提供实现。
class CongestionController {
   public:
    virtual ~CongestionController() = default;
    /// 新确认了 n 个包。
    virtual void OnAck(std::uint32_t newly_acked) = 0;
    /// 发生一次丢包事件。
    virtual void OnLoss(LossKind kind) = 0;
    /// 当前可用窗口（单位：包）。0 表示暂不可发。
    virtual std::uint32_t Window() const noexcept = 0;
    /// 上层设置对端通告窗口后，实际可用窗口 = min(Window(), peer_window)。
    virtual void SetPeerWindow(std::uint32_t peer_window) = 0;
};

}  // namespace rd::net
```

| 编号 | 要求 | 验收 |
|---|---|---|
| C1 | Go2Cpp **不得**提供任何 `CongestionController` 的默认实现（连 `NewReno` 也不行） | `grep -rn "class .*Congestion" go2cpp/` 只命中接口 |
| C2 | `EventLoop`/`RetransmitQueue` 不得自行调用 `OnAck/OnLoss`；调用点必须由上层代码触发 | 代码审查：`OnAck(` 的调用者只能是上层 |
| C3 | 骨架不得缓存 `Window()` 结果；每次发送判定都重新取 | 单测：注入一个「窗口每秒变化」的控制器，断言发送数随之变化 |

### 4.6 明确不做（本章）

| 不做 | 归属 |
|---|---|
| RDT/UDP v1 的 24 字节头、握手、ACK 位图、心跳、错误码 | 我方（规范 §6.2–§6.6、§6.11） |
| 应用层分片与重组（8 字节分片头、`frame_id`、1 MiB 上限） | 我方（规范 §6.7） |
| 「3 次重复 ACK 触发快速重传」 | 我方（规范 §6.5.2） |
| 「8 次重传上限即断连」 | 我方（规范 §6.5.2 / §6.11） |
| 慢启动/拥塞避免的具体参数（32/128/16/256） | 我方（规范 §6.5.5） |
| Karn 算法（哪些样本可采样） | 我方 |

### 4.7 本章验收

| 项 | 方式 | 判据 |
|---|---|---|
| R3-1 | 单测 `test_seq_num.cpp`、`test_retransmit_queue.cpp`、`test_rtt_estimator.cpp` | Q1–Q2、RQ1–RQ4、RT1–RT4 逐条断言 |
| R3-2 | 骨架不得含策略 | §1.4 grep + C1/C2 审查 |
| R3-3 | 我方注入 RDT 后行为不变 | 我方 7 个 golden vector（`RDP-UDP-RESEARCH.md` §6.8 的 GV-1…GV-7）在**我方仓库**跑通；Go2Cpp 仓库不出现这些向量 |

---

## 5. 需求四：与现有产品的集成路径

> **边界提醒**：本章规定「新能力怎么接进现有产品」，不改变 §1 的归属。集成过程中任何「为了让链路跑通而顺手加个协议解析」的行为都判越界。

### 5.1 `rd::net` 的演进

#### 5.1.1 新增 vs 保留

| 现有符号 | 处置 | 说明 |
|---|---|---|
| `rd::net::Dial/Listen` | **保留，语义不变** | 现网 `rdrl` 依赖；`product/common/rdrl/src/net.cpp:37` 直接调用 |
| `rd::net::Conn` | **保留，语义不变** | 阻塞式读写；`Read` 返回 0 表示 EOF 的既有约定不改（`conn.h:7`） |
| `rd::net::Backend` | **保留 + 收紧语义** | 见 §5.1.2 |
| `rd::net::EventLoop` | **新增** | §2.3 |
| `rd::net::DatagramSocket` / `StreamSocket` | **新增** | §2.2 |
| `rd::net::Protocol` / `StreamProtocol` / `ProtocolContext` | **新增** | §3.3 |
| `rd::net::Buffer` / `BufferPool` | **新增** | §2.4 |
| `rd::net::RetransmitQueue` / `RttEstimator` / `CongestionController` / `SeqNum` | **新增** | §4 |
| `rd::net::StreamRelay` | **新增（替代 `rd::media::TcpBridge`）** | §5.2 |

**硬性要求 N1**：`Conn`/`Listener` 的公开头**不得**被修改（除 §2.2.1 的 `ErrorCode` 追加）。理由：`rdrl` 的 28 个单测与 A1 e2e 已锁定这些语义，改动会连带回归。

#### 5.1.2 `Backend::kAuto` 的语义（收紧，写入契约）

现状（`product/common/src/net/net.cpp:22-59`）：

```text
kAuto = 先试 Go2Cpp；仅当返回 kNotImplemented 时回退 Winsock；其它错误直接上抛。
```

问题：`kAuto` 是**每个调用点**独立决策的。同一进程里 TCP 可能走 Go2Cpp、UDP 可能回退 Winsock，两条链路的内核语义（超时、RST、截断）不一致，故障定位困难。

新语义（三条，必须实现）：

| 编号 | 规则 |
|---|---|
| A1 | `kAuto` 在**进程内首次网络调用时**锁定一个后端，并把选择结果写入日志（`backend=go2cpp reason=ok` / `backend=winsock reason=go2cpp_unsupported:udp_bind`） |
| A2 | 锁定后所有 `Dial/Listen/DatagramSocket/StreamSocket/EventLoop` 都用同一后端 |
| A3 | 锁定时若 `QueryCapabilities()` 显示当前后端缺少**本次要用**的能力，返回 `kNotImplemented` 并把缺失项写进错误消息；**不得**静默回退到另一后端 |

`Backend::kWinsock` / `kGo2Cpp` 的强制语义不变（用于对照测试与排障，`net.h:38-43`）。

**验收**：

| 项 | 方式 | 判据 |
|---|---|---|
| N2 | 单测 `backend_auto_locks` | 首次 `Dial("tcp")` 后 `CurrentBackendEffective()`（新增诊断函数）保持不变；构造 `Listen("udp")` 失败时错误消息含 `udp_bind` |
| N3 | 单测 `backend_no_mixed` | 同一进程内 TCP 与 UDP 的实际后端名相同 |

#### 5.1.3 `QueryCapabilities` 必须如实

见 §2.3.2 的 B1。特别强调：Windows `select` 后端的 `max_sockets_per_loop` **必须填 63**，不得填 `EventLoopOptions::max_sockets`（4096）。这是 G5 的强制断言项。

### 5.2 `tcp_bridge` 的通用化（`StreamRelay`）

#### 5.2.1 现状问题（逐条，带行号）

`product/media/src/tcp_bridge.cpp` 现在是「媒体专用 + Winsock 专用」：

| 问题 | 证据 | 后果 |
|---|---|---|
| 直接 `#include <winsock2.h>` | 第 7–8 行 | 只能 Windows 编译；与「跨平台一致」冲突 |
| 每连接 2 个 `std::thread` 做双向 pump | `ForwardBoth` 第 300–303 行；线程创建在 `AcceptLoop` 第 226–227 行 | N 个 viewer = 2N 线程；100 连接 = 200 线程 |
| `select` + 100 ms 超时轮询 | 第 246–259 行 | 空闲连接每 100 ms 醒一次；延迟下限 100 ms 级 |
| 重放缓存硬编码 256 KiB | 第 123 行 `kReplayLimitBytes` | 无法按链路调整 |
| 重连策略硬编码 3 次 / 200 ms 递增 | 第 119–121 行 | 无法配置 |
| 日志裸 `printf` + `fflush` | 第 220、261–279、324–417 行 | 无法接入统一日志，且热路径 flush 拖慢 |
| socket 句柄用 `void*` 传 | `tcp_bridge.h:61,66`（声明）、`tcp_bridge.cpp:231`（定义） | 类型不安全，跨平台无法复用 |

#### 5.2.2 `StreamRelay` 接口

```cpp
// product/common/include/rd/net/stream_relay.h
namespace rd::net {

struct RelayOptions {
    std::size_t pump_buffer_bytes{64 * 1024};      ///< 单次搬运缓冲
    std::size_t replay_limit_bytes{256 * 1024};    ///< 上游未回数据前的重放缓存上限
    int max_upstream_retries{3};                   ///< 上游「0 字节即断」的重试次数
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds retry_base{200};     ///< 第 n 次重试间隔 = retry_base * n
    bool preserve_viewer_connection{true};         ///< 重连时只断上游，不断下游
};

struct RelayStats {
    std::uint64_t accepted{0};
    std::uint64_t upstream_ok{0};
    std::uint64_t upstream_retries{0};
    std::uint64_t upstream_failed{0};
    std::uint64_t bytes_up{0};
    std::uint64_t bytes_down{0};
    std::uint64_t replayed_bytes{0};
};

/// 通用 TCP 字节中继：监听 → 每连接连上游 → 双向搬运 → 上游异常时重放并重连。
/// 语义与 product/media/src/tcp_bridge.cpp 等价，但不含任何媒体/RDP 逻辑。
class StreamRelay {
   public:
    static Result<std::unique_ptr<StreamRelay>> Create(EventLoop& loop,
                                                       const RelayOptions& options = {});
    ~StreamRelay();
    StreamRelay(const StreamRelay&) = delete;
    StreamRelay& operator=(const StreamRelay&) = delete;

    /// 开始监听；listen_port 传 0 表示系统分配，用 listen_port() 取回。
    Status Start(const Endpoint& listen, const Endpoint& upstream);
    Status Stop();                       ///< 幂等；停止后所有连接关闭
    std::uint16_t listen_port() const noexcept;
    RelayStats stats() const noexcept;
};

}  // namespace rd::net
```

#### 5.2.3 必须保留的语义（逐条，来自 `tcp_bridge` 的实测行为）

| 编号 | 语义 | 现状依据 |
|---|---|---|
| SR1 | 上游在**没回过任何数据**时断开 → 重连并**重放**已缓存的 viewer 字节 | `tcp_bridge.cpp:354-370`、`380-398` |
| SR2 | 上游**回过数据**后断开 → 视为会话正常结束，**绝不重连** | `tcp_bridge.cpp:380-387` |
| SR3 | 重连期间**不关闭 viewer socket**（否则 viewer 会重连，用户可见黑屏） | `tcp_bridge.cpp:296-297`、`410-411` |
| SR4 | 重试耗尽 → 关闭 viewer，让失败显性化（不静默挂起） | `tcp_bridge.cpp:400-411` |
| SR5 | 重试间隔线性递增（200/400/600 ms） | `tcp_bridge.cpp:321-328` |

**硬性要求 N4**：`StreamRelay` 不得为每个连接创建线程；必须用 `EventLoop` + 非阻塞 `connect`。验收：注册 100 个 relay 连接，进程线程数不增长（与 L2 同一断言）。

**硬性要求 N5**：`StreamRelay` 不得解析任何字节（包括 RDP 邀请函、TPKT 头）。它只允许统计字节数与连接数。G6 grep 判据同样适用。

### 5.3 迁移顺序（先 TCP，后 UDP，可靠骨架最后）

#### 5.3.1 为什么不是并行

| 顺序 | 理由 |
|---|---|
| TCP 先行 | TCP 链路现网在用（`rdrl` 的 CONTROL/DATA），且**协议不变**（RDRL 帧原样承载）。这让我们能在零协议风险下验证事件循环、缓冲所有权、半关闭语义三件事 |
| UDP 其次 | UDP 一旦引入，故障可能来自「传输层」或「我方 RDT 实现」。若事件循环还没被验证过，两类错误会混在一起，定位成本翻倍 |
| 骨架最后 | `RetransmitQueue`/`RttEstimator` 只有在 UDP socket 与注入接口稳定后才有意义；提前做会因接口变动返工 |

#### 5.3.2 排期（承诺值）

| 阶段 | 日期 | 交付 | 出口判据 |
|---|---|---|---|
| **P0 接口冻结** | 2026-11-03 ~ 11-06 | 本文签署版；`§3.3` 签名逐字确认 | 双方签字；此后签名变更走 §8.6 |
| **P1 TCP** | 11-09 ~ 11-20 | `EventLoop`、`StreamSocket`、`Buffer/BufferPool`、`StreamRelay` | G1/G2（TCP 部分 12 用例）+ N4 + 我方 `tcp_bridge` 等价性测试通过 |
| **P2 UDP + 注入** | 11-23 ~ 12-04 | `DatagramSocket`、`Protocol`/`StreamProtocol`/`ProtocolContext`、`QueryCapabilities` | G1/G2（全部 20 用例）+ **G3 注入通过** + G6 |
| **P3 可靠骨架** | 12-07 ~ 12-18 | `SeqNum`、`RetransmitQueue`、`RttEstimator`、`CongestionController` | G2（骨架 8 用例）+ R3-2 + R3-3 |
| **P4 集成验收** | 12-21 ~ 12-25 | 双平台回归 + 性能报告 + 交付报告 | G4 + G5 + 全部门禁 |

#### 5.3.3 我方在 P1/P2 期间同步做的事（不占乙方工期）

| 我方任务 | 依赖乙方的哪个交付 | 说明 |
|---|---|---|
| `rd::proto::rdp::TpktFramer` + `X224Codec` | P1 的 `StreamProtocol` | 用真实 Windows 抓包的 19/462/529 字节报文（`RDP-REAL-CAPTURE.md` 块 485/487/488）做逐字节单测 |
| `rd::proto::rdt::Header` + golden vector GV-1…GV-7 | P2 的 `DatagramSocket` | 纯编解码单测，不需要网络 |
| `rd::proto::rdt::Handshake/Reassembler` | P2 的 `Protocol` | 用内存环回注入 |
| `tools/netbench` 压测工具 | P1 的 `EventLoop` | 见 §7.5 |

### 5.4 本章验收

| 项 | 方式 | 判据 |
|---|---|---|
| R4-1 后端语义 | 单测 `backend_auto_locks`、`backend_no_mixed` | N2/N3 |
| R4-2 relay 等价 | 我方 `tests/relay_equiv.cpp`：同一脚本驱动 `TcpBridge` 与 `StreamRelay`，对比字节数、重连次数、重放字节 | 五项统计逐项相等（`bytes_up/bytes_down/retries/replayed/gave_up`） |
| R4-3 relay 无线程增长 | 压测 100 连接 | 线程数不变；N4 |
| R4-4 迁移不回归 | `ctest` 全量（`rdrl` 28 项 + `rd_net_tests` 8 项 + 新增 20 项） | 全绿，无 skip |

---

## 6. 明确不做的事（清单）

> 这一章是给外包方的**负向需求**。做了任何一条，即使「功能更好」也判为范围外，不计工作量、不予验收。

### 6.1 协议解析类（一条都不做）

| # | 不做 | 归属 / 依据 | 若做了会怎样 |
|---|---|---|---|
| N-1 | 不实现 RDP 的 TPKT 分帧（`03 00 len` 大端 4 字节头） | 我方 `TpktFramer`；RFC 1006 / T.123 | TCP 流被 Go2Cpp 切成「消息」，`StreamProtocol` 语义被破坏；换 RDRL 承载时要重写 |
| N-2 | 不实现 X.224 CR/CC/DT（`e0/d0/f0`、`LI`、`EOT`） | 我方 `X224Codec`；X.224 Class 0 | 连接建立语义进 Go2Cpp，Redirection/协商分支无法扩展 |
| N-3 | 不实现 MCS BER/PER（Connect Initial 416/462 字节、SDIN `0x64`） | 我方 `McsCodec`；T.125 | 需要引入 BER 解析器；与「不引入第三方库」冲突 |
| N-4 | 不实现 RDP Security 头（`TS_SECURITY_HEADER`）与 TLS/CredSSP 选择 | 我方 `SecurityLayer`；MS-RDPBCGR §5.3/5.4 | 加密策略进传输层，违反分层 |
| N-5 | 不实现 RDP 快路径/慢路径（`fpOutputHeader`、`TS_SHAREDATAHEADER`） | 我方 `FastPathCodec`/`SlowPathCodec` | 同 N-3 |
| N-6 | 不实现 RDT/UDP v1 的 24 字节头（magic `0x4A`、`version`、`type`、`flags`、`session_id`、`seq_num`、`ack_num`、`ack_bits`、`payload_len`、`reserved`） | 我方 `rdt::Header`；规范 §6.2 | 边界立即失守：Go2Cpp 变成「半个 RDT 实现」，换协议要重写 |
| N-7 | 不实现 RDT 握手（SYN/SYNACK/ACK、nonce 回显、MTU/window 协商、500 ms×5 重传） | 我方 `rdt::Handshake`；规范 §6.4 | 同上 |
| N-8 | 不实现 RDT ACK 位图语义（`ack_num` + 32 位 `ack_bits`、bit i ↔ `ack_num-1-i`） | 我方 `rdt::AckBitmap`；规范 §6.5.1 | 位图宽度/位序是协议字段，不是通用能力 |
| N-9 | 不实现 RDT 重传触发（RTO 到期、3 次重复 ACK、8 次上限） | 我方 `rdt::RetransmitPolicy`；规范 §6.5.2 | 策略进骨架，我方无法按弱网调参 |
| N-10 | 不实现 RDT 心跳/判死（PING/PONG、3×heartbeat、RST 错误码 `0x0001`–`0x0009`） | 我方 `rdt::Heartbeat`；规范 §6.6 / §6.11 | 错误码语义双份，必然漂移 |
| N-11 | 不实现 RDP 邀请函改写 | 我方 `rd::media::InvitationRewrite`；ADR-03 | 产品逻辑进通用层 |

### 6.2 协议语义类（容易「顺手做」的）

| # | 不做 | 归属 / 依据 | 若做了会怎样 |
|---|---|---|---|
| N-12 | 不做应用层分片（8 字节分片头、`frame_id`/`frag_index`/`frag_count`/`total_len`） | 我方 `rdt::Fragmenter`；规范 §6.7.1 | 分片参数（MTU−32、121 片）是协议常量，换 MTU 就要改 Go2Cpp，违反探针 C |
| N-13 | 不做应用层重组（乱序缓存、`REASM_TIMEOUT=6000ms`、1 MiB 上限、同 `frame_id` 冲突处理） | 我方 `rdt::Reassembler`；规范 §6.7.3 | 违反探针 B；且重组是内存攻击面，必须在协议层做限额 |
| N-14 | 不做「按序号重排后交付」 | 我方（在重组完成后整帧上交）；规范 §6.5.4 | Go2Cpp 一旦重排，协议层的 `ack_num` 语义就与交付顺序脱钩，调试地狱 |
| N-15 | 不做「重复包检测」与「过期包丢弃」 | 我方；规范 §6.5.4 | 属于序号语义 |
| N-16 | 不做「窗口通告」与「流控」 | 我方；规范 §6.5.5 | 窗口是握手协商字段 |
| N-17 | 不做「连接建立（`Connect()`）即完成 RDT 握手」 | Go2Cpp 的 `Connect` 只做 socket `connect()`；握手在我方 `OnDatagram` | 一个函数两种语义，必然误解 |

### 6.3 通用层自身的越界（「为了通用而通用」）

| # | 不做 | 理由 |
|---|---|---|
| N-18 | 不内置任何具体可靠传输算法（KCP/QUIC/newreno 的完整实现） | 我方的可靠层是 RDT/UDP v1（规范 §6），不是 KCP；内置算法会与我方规范冲突并翻倍维护 |
| N-19 | 不做 FEC / 前向纠错 / 冗余编码 | 我方 v1 明确不做（规范 §6.1「不做 FEC」）；Go2Cpp 做了也没人调用 |
| N-20 | 不做「协议注册表」（按 magic 分发到不同 `Protocol`） | 分发即解析。一个 socket 一个协议，由 `Attach*` 显式绑定 |
| N-21 | 不做多路复用（一条 UDP 会话里分 channel） | 我方 v1 一条 `session_id` 一条流（规范 §6.1）；多通道是 v2，届时由我方在协议层加 `channel_id` |
| N-22 | 不做 0-RTT / 连接迁移 / 路径 MTU 发现 | 规范 §6.1 明确不做；且都需要协议头字段参与 |
| N-23 | 不改 `rd::net::Conn` 的既有语义（`Read` 返 0 = EOF） | N1；`rdrl` 28 项单测锁定 |
| N-24 | 不引入 OpenSSL / picoquic / picotls / KCP 任何第三方源码 | 项目约束（`go2cpp-capabilities.md` §6 ①）；且与「不实现协议」冲突 |

---

## 7. 验收标准（Go2Cpp 交付物怎么算合格）

> 判定原则：**只认可执行证据**。任何「已实现」「已测试」的口头结论不计入验收；每条判据必须能被我方在干净环境复跑。

### 7.1 交付物清单（文件级）

| # | 交付物 | 路径（相对 Go2Cpp 仓库） | 说明 |
|---|---|---|---|
| 1 | 平台层错误码扩展 | `include/go2cpp/platform/error.hpp`（追加 3 个枚举值） | `kPeerClosed=13`、`kReset=14`、`kTruncated=15`；既有值不得改 |
| 2 | 平台层 poller 增强 | `include/go2cpp/platform/io_poller.hpp`、`src/platform/{linux,windows}/io_poller.cpp` | Windows 后端必须满足 §7.5 的 pps 指标；后端选型自由 |
| 3 | `rd::net` 新增公共头 | `product/common/include/rd/net/{io_status,datagram_socket,stream_socket,event_loop,protocol,buffer,seq_num,retransmit_queue,rtt_estimator,congestion,stream_relay}.h` | 头文件内容与 §2–§5 逐字对应 |
| 4 | `rd::net` 新增实现 | `product/common/src/net/*.cpp` | 不得 include `go2cpp/*` 到公共头 |
| 5 | 单测 | `product/common/tests/test_{datagram_socket,stream_socket,event_loop,protocol,buffer,seq_num,retransmit_queue,rtt_estimator,stream_relay}.cpp` | 20 个用例见 §7.3 |
| 6 | 微基准 | `product/common/tests/bench_net.cpp` | 输出 JSON（字段见 §7.5.3） |
| 7 | 交付报告 | `docs/delivery/GO2CPP-NET-REPORT.md` | 含双平台实测数据、未达标项、已知限制 |

**不接受的交付形态**：只给设计文档不给代码；只给代码不给单测；只在一个平台编译通过；把实现塞进 `product/media` 或 `product/common/rdrl`。

### 7.2 G1：编译

| 项 | 判据 | 命令 |
|---|---|---|
| G1-1 | C++17；MSVC `/W4 /WX /permissive- /utf-8`；GCC/Clang `-Wall -Wextra -Wpedantic -Werror` | 见 `product/common/CMakeLists.txt:59-63` 的门槛，新增 target 沿用 |
| G1-2 | 公开头不泄漏平台类型 | `grep -rnE 'SOCKET|winsock2|sys/socket|<unistd' product/common/include/rd/net/` 为空 |
| G1-3 | 公共头不 include `go2cpp/*` | `grep -rn 'go2cpp' product/common/include/rd/net/` 为空 |
| G1-4 | 双平台编译 | Windows（MSVC 19.3x）+ Linux（GCC 13 / Clang 17）各一次干净构建 |
| G1-5 | `/MT` 运行库（Windows） | `MSVC_RUNTIME_LIBRARY "MultiThreaded"`，与 `rd_net` 一致 |

### 7.3 G2：单测（20 个用例，逐条必须存在且通过）

| # | 用例 | 覆盖条款 | 关键断言 |
|---|---|---|---|
| 1 | `datagram_bind_recvfrom_boundary` | D1/D2/D4 | 报文 1B、= buffer、= buffer+1 三种；第三种断言 `kTruncated` 且 `bytes==buffer.size()` |
| 2 | `datagram_sendto_atomic` | D3 | 故意让 socket 无效 → `kError`、`bytes==0`；线上无字节 |
| 3 | `datagram_icmp_unreachable` | E1 | `Connect` 到关闭端口后 `Send`+`Recv` → `kReset`（Linux `ECONNREFUSED`，Windows `WSAECONNRESET`） |
| 4 | `stream_read_closed_vs_reset` | E1/S1 | FIN → `kClosed`；RST → `kReset`；两者错误码不同 |
| 5 | `stream_write_all_or_error` | S2 | 写 1 MiB 中途对端关闭 → 返回 `bytes>0` 且 `status!=kOk` |
| 6 | `event_loop_callback_thread` | P1/L1 | 回调内 `owner_thread_id()==this_thread`，1000 次全中 |
| 7 | `event_loop_no_thread_growth` | L2 | 注册 100 个 UDP socket，线程数不变 |
| 8 | `event_loop_timer_precision` | L4 | 10 万次 10 ms 定时器，P99 ≤ 2 ms、max ≤ 10 ms |
| 9 | `event_loop_max_sockets` | L5/L6 | `max_sockets=4` 第 5 次 `kResourceExhausted`；Windows 第 64 个 `kUnsupported` 且 `max_sockets_per_loop==63` |
| 10 | `event_loop_detach_silence` | P6/P7 | `Detach` 后 10 万字节注入，`OnDatagram` 计数为 0 |
| 11 | `protocol_send_backpressure` | P8 | 队列满 → `kBackpressure`、`token==0`、线上字节数不变 |
| 12 | `protocol_zero_copy_send` | M2 | `SendDatagram` 返回后立刻 `memset` 源缓冲为 `0xCC`；对端必须收到 `0xCC`（收到原值即证明发生了拷贝，判失败） |
| 13 | `protocol_recv_zero_copy` | M1 | `memcpy` 计数：1000 包 ≤ 1000 次（允许内核 1 次/包） |
| 14 | `buffer_pool_exhausted` | M3 | `capacity=64`，第 65 次 `kResourceExhausted`；`malloc` 计数 0 |
| 15 | `seq_num_wraparound` | Q1/Q2 | `uint32_t`/`uint64_t` 各 3 组回绕；`Less` 全对 |
| 16 | `retransmit_queue_order` | RQ1/RQ2/RQ3/RQ4 | 10 万项 P99 ≤ 1 µs；同 `due` 稳定序；`Bump` 1000 次不删；空队列 `NextDeadline()==max()` |
| 17 | `rtt_estimator_rfc6298` | RT1/RT2/RT4 | 样本 `{100,120,80}` 的 `srtt/srttvar/rto` 与手算值**精确相等**（整型毫秒） |
| 18 | `congestion_controller_injection` | C1/C3 | 注入窗口每秒变化的假控制器，发送数随之变化；`go2cpp/` 内无 `CongestionController` 派生类 |
| 19 | `stream_relay_replay_retry` | SR1/SR2/SR5 | 上游 0 字节断开 → 重放并重连 3 次；上游回过数据 → 不重连 |
| 20 | `stream_relay_half_close` | SR3 | 重连期间 viewer socket 未关闭（`SHUTDOWN` 计数 0） |

### 7.4 G3：协议注入验收（**最关键的一条**）

**定义**：我方在 `product/common/tests/injection/` 下写一个**与 RDT 无关**的测试协议（4 字节大端长度前缀 + 1 字节 XOR 校验 + 回显），只使用 `rd::net` 公开头，实现 `Protocol`，挂到 `EventLoop`，跑 1000 次往返。

| 项 | 判据 |
|---|---|
| G3-1 | 该测试协议可编译、可跑通，**且 Go2Cpp 交付目录的 `git diff --stat` 为空**（验证期间不得改一行） |
| G3-2 | 该测试协议**不得** include 任何 `go2cpp/*` 头（`grep` 为空） |
| G3-3 | 把测试协议换成我方的 `RdtUdpProtocol`（§3.4），只增加我方文件，仍不改 Go2Cpp |
| G3-4 | 反向验证：故意让测试协议的**首字节**取非常规值 `0xFF`，Go2Cpp 必须原样搬运、不得丢弃、不得报错 |

G3-4 是**防「暗中解析」**的探针：如果乙方偷偷按首字节过滤，这一条会失败。

### 7.5 G4：性能基线

#### 7.5.1 环境与测法

| 项 | 规定 |
|---|---|
| 拓扑 | 同机回环 `127.0.0.1`，发送/接收为**两个独立进程**（避免同进程调度干扰） |
| 绑核 | loop 线程绑单核（Windows `SetThreadAffinityMask`；Linux `sched_setaffinity`） |
| 工具 | 我方 `tools/netbench`（乙方必须让其跑通；工具不进 Go2Cpp 仓库） |
| 时长 | 每场景 60 s 预热 + 600 s 测量 |
| 报文 | UDP 64 B 与 1200 B；TCP 64 KiB 块 |
| 统计 | pps、Gbps、p50/p99/p999 单向延迟、丢包数、RSS、CPU |

#### 7.5.2 必达指标（双平台，未达标即 G4 失败）

| 场景 | Linux（epoll 后端） | Windows（乙方选定后端） |
|---|---|---|
| UDP 64 B，单 loop | ≥ 150,000 pps | ≥ 100,000 pps |
| UDP 1200 B，单流 | ≥ 1.0 Gbps | ≥ 0.9 Gbps |
| UDP 1200 B，4 流聚合 | ≥ 1.5 Gbps | ≥ 1.0 Gbps |
| UDP 单向延迟 p99（1200 B） | ≤ 200 µs | ≤ 500 µs |
| 10 分钟压测丢包 | 0 | 0 |
| TCP 中继（64 KiB 块，单流） | ≥ 800 Mbps | ≥ 600 Mbps |
| 稳态 RSS 增长 | ≤ 1 MiB / 万包 | 同 |
| loop 线程 CPU | ≤ 1 核 | 同 |

> 说明：`1200 B × 100,000 pps ≈ 960 Mbps`，与「1 Gbps 回环」是同一量级的两种表述。Windows 若 `select` 后端无法达到 100k pps，**后端选型由乙方负责更换（WSAPoll / IOCP 均可）**，指标不因后端而放宽。

#### 7.5.3 基准输出格式（JSON，字段固定）

```json
{
  "platform": "windows|linux",
  "backend": "epoll|select|wsapoll|iocp",
  "scenario": "udp_64b|udp_1200b|udp_4stream|tcp_relay",
  "pps": 0, "gbps": 0.0,
  "latency_us": {"p50": 0, "p99": 0, "p999": 0},
  "dropped": 0, "rss_kib": 0, "cpu_percent": 0.0,
  "duration_s": 600
}
```

### 7.6 G5：跨平台行为一致

#### 7.6.1 错误码映射表（逐条断言，缺一行即失败）

| Go2Cpp `platform::ErrorCode` | 触发条件 | `rd::net::IoStatus` | `rd::ErrorCode`（追加后） |
|---|---|---|---|
| `kNone` | 成功 | `kOk` | `kOk` |
| `kWouldBlock` | 非阻塞暂无数据 | `kWouldBlock` | `kWouldBlock=22`（**新增**） |
| `kInterrupted` | `EINTR`/`WSAEINTR` | 内部重试，不对外暴露 | 不映射 |
| `kClosed` | 句柄已关闭（`EBADF`/`WSAENOTSOCK`） | `kError` | `kInvalidArgument` |
| `kPeerClosed`（**新增**） | 对端 FIN / 有序关闭 | `kClosed` | `kClosed=19`（**新增**） |
| `kReset`（**新增**） | `ECONNRESET`/`WSAECONNRESET`/ICMP unreachable | `kReset` | `kReset=20`（**新增**） |
| `kTruncated`（**新增**） | `WSAEMSGSIZE`/`MSG_TRUNC` | `kTruncated` | `kTruncated=21`（**新增**） |
| `kUnsupported` | 后端无此能力 | `kError` | `kNotImplemented` |
| `kPermission` | `EACCES` | `kError` | `kPermissionDenied` |
| `kNotFound` | 解析失败 | `kError` | `kNotFound` |
| `kSystem` | 其它 | `kError` | `kIoError` |
| （不来自 platform 错误） | 发送队列满 | `kBackpressure` | `kBackpressure=23`（**新增**） |

**硬性要求 E5**：`kWouldBlock` 与 `kTimeout` 必须分开。现有实现把 `kWouldBlock` 映射成 `kTimeout`（`go2cpp_transport.cpp:52-53`），那是**阻塞式超时**的正确做法；新的非阻塞 API 必须保留 `kWouldBlock`，否则我方无法区分「没数据」与「等超时了」。

#### 7.6.2 一致性判据

| 项 | 判据 |
|---|---|
| G5-1 | §7.3 的 20 个用例在 Windows 与 Linux 上**用例名、断言、结果完全一致**；不允许 `#ifdef` 跳过（平台差异只允许出现在期望值表里，且必须写明理由） |
| G5-2 | §7.6.1 映射表每一行都有单测；两平台跑同一份用例 |
| G5-3 | `QueryCapabilities()` 在 Windows 返回 `max_sockets_per_loop==63`（或乙方改造后的真实值），Linux 返回实际值；两者都必须与实测一致 |
| G5-4 | 同一场景的 pps/Gbps 差异不要求一致，但**必须都在 §7.5.2 的区间内** |

### 7.7 G6：边界合规

跑 §1.4 的三条 `grep`，**零命中**。允许的白名单只有 `tests/injection/`（我方文件）。

附加判据：

| 项 | 判据 |
|---|---|
| G6-1 | `go2cpp/` 与 `product/common/src/net/` 内不出现 `rd::proto` |
| G6-2 | 不出现按 payload 首字节分支的代码（人工审查 + `grep 'payload\[0\]'`） |
| G6-3 | 不出现协议常量：`24`（作为头长）、`0x4A`、`65552`、`121`、`6000`（作为重组超时）等魔法数；允许出现的是传输层常量（如 `FD_SETSIZE`） |

### 7.8 不接受的做法（反作弊条款）

| # | 不接受 | 检测方式 |
|---|---|---|
| 1 | 「跑通即交付」——没有单测或单测只测 happy path | G2 逐条核对用例名与断言内容 |
| 2 | 用 `sleep`/轮询冒充事件驱动 | G2 #7 线程数 + #8 定时器精度；代码审查 `Sleep(`/`sleep(`/`usleep(` |
| 3 | 热路径 `malloc`（收发包、定时器、队列） | G2 #14 的 `malloc` 计数；`bench` 的 RSS 曲线 |
| 4 | 在公开头里 `#include <winsock2.h>` 或暴露 `SOCKET` | G1-2 |
| 5 | 把协议常量写进 Go2Cpp 默认值（如 `rto_min=100` 当成「RDT 的默认值」） | G6-3；审查 `RttParams` 默认值必须是 RFC 6298 而非我方规范 |
| 6 | 用 `#ifdef _WIN32` 让某个用例在 Windows 上跳过 | G5-1 |
| 7 | 谎报 `QueryCapabilities` | G5-3 会构造反例（注册到上限） |
| 8 | 把 `StreamRelay` 写成「只支持一个连接」的简化版 | G2 #19/#20 + N4 的 100 连接压测 |
| 9 | 交付报告只写「通过」不写数据 | §7.5.3 的 JSON 必须附在报告里 |
| 10 | 验收期间临时改代码让测试通过 | G3-1 的 `git diff` 为空；验收前后各取一次 commit hash |

---

## 8. 明天（2026-11-03）对齐会议程

### 8.1 会议信息

| 项 | 内容 |
|---|---|
| 时间 | 2026-11-03（周二）14:00–15:30（90 分钟，硬截止） |
| 参会 | 我方：架构 + 传输层实现 + 测试；Go2Cpp：负责人 + 实现 + 测试 |
| 材料 | 本文打印版（带签字栏）、`RDP-UDP-RESEARCH.md` §6 节选、`product/common/src/net/` 现状截图 |
| 产出 | ① 签署版 §1.2 对照表 ② 冻结版 §3.3 接口签名（带版本号）③ 排期确认表 ④ 遗留项登记表 |

### 8.2 逐段时间盒

| 时间 | 议程 | 目标 | 材料 | 主持 | 超时处理 |
|---|---|---|---|---|---|
| 00:00–00:05 | **议程 0：边界复述** | 双方**各自用自己的话**复述「Go2Cpp 只做通用传输层 + 协议注入入口」，不一致当场纠正 | 本文 §0 | 我方 | 直接进入议程 1，用对照表纠正 |
| 00:05–00:30 | **议程 1：逐行过 §1.2 对照表（27 行）** | 每行落一个结论：`Go2Cpp` / `我方` / `Go2Cpp 改造`；有争议的行标红进入议程 5 | 本文 §1.2 | 我方逐行念，Go2Cpp 逐行答 | 单行超 60 秒即标红跳过；红行不超过 5 行 |
| 00:30–00:55 | **议程 2：逐字确认 §3.3 接口签名** | 8 个方法 + 6 个回调逐个确认；确认 `ByteView`/`BufferPtr`/`SendResult`/`TimerId` 的类型选择 | 本文 §3.3 | 我方 | 签名有异议当场改并记版本号；不讨论实现细节 |
| 00:55–01:10 | **议程 3：验收与性能基线** | 确认 §7.3 的 20 个用例、§7.5.2 的指标、§7.6.1 的映射表；Go2Cpp 明确回答「哪些指标当前架构达不到」 | 本文 §7 | 我方测试 | 有异议只允许两种结论：接受 / 提出替代数值并给出依据 |
| 01:10–01:25 | **议程 4：时间节点与人力** | 确认 §5.3.2 五个阶段的日期与人力；确认 P1 出口判据 | 本文 §5.3.2 | 双方 | 有冲突当场改日期，不接受模糊的工期承诺 |
| 01:25–01:30 | **议程 5：遗留项与升级规则** | 红行逐条登记：谁、何时、给什么结论；确认 §8.5 升级路径 | 本文 §8.5 | 我方 | 5 分钟内必须全部登记完 |

### 8.3 议程 1 的过表方法（避免开成技术讨论会）

对每一行只问三个问题，按顺序：

1. **这行归谁？**（答 `Go2Cpp` / `我方` / `改造`）
2. **符号名对不对？**（对着本文的符号念一遍）
3. **有没有「顺手做了」的风险？**（Go2Cpp 主动说，不说视为默认不做）

三问都过 → 打勾；任一有异议 → 标红，写进遗留表，**不在会上展开技术方案**。

### 8.4 议程 2 的签字方法

对我方最关键的 6 个签名，逐个念出来并让 Go2Cpp 复述语义：

| # | 签名 | 必须复述出的语义 |
|---|---|---|
| 1 | `SendResult SendDatagram(ByteView payload, const Endpoint& to)` | 「不拷贝，payload 要活到 `OnSendComplete`；队列满返回 `kBackpressure` 且一个字节都没发」 |
| 2 | `IoResult RecvFrom(MutableByteView buffer, Endpoint* from)` | 「一次一个报文；超 buffer 返回 `kTruncated` 不静默截断」 |
| 3 | `TimerId ScheduleAfter(std::chrono::milliseconds delay)` | 「毫秒级，回调在 loop 线程，`CancelTimer` 后不再触发」 |
| 4 | `void OnDatagram(ProtocolContext&, ByteView, const Endpoint&)` | 「payload 回调返回即失效；Go2Cpp 不解析内容」 |
| 5 | `void OnBytes(ProtocolContext&, ByteView)` | 「不保证消息边界，分帧是上层的事」 |
| 6 | `Result<ProtocolHandle> AttachDatagram(DatagramSocket, Protocol&)` | 「socket 所有权转移给 loop；protocol 生命周期由调用方保证」 |

复述错误 → 当场澄清并记入会议纪要（这类误解是后期返工的主因）。

### 8.5 分歧升级规则（写进合同）

| 级别 | 情形 | 处理 | 时限 |
|---|---|---|---|
| L1 | 对 §1.2 某一行的归属有异议 | 会上标红；会后 1 个工作日内由双方技术负责人给出书面结论 | 2026-11-04 18:00 前 |
| L2 | 对接口签名有异议 | 会上改；改后版本号 +0.1；旧签名作废 | 会上 |
| L3 | 对性能指标有异议 | 只允许「接受」或「提出替代数值 + 实测/理论依据」；不接受模糊承诺 | 会上 |
| L4 | 对工期有异议 | 当场改日期；改后写入 §5.3.2 | 会上 |
| L5 | 实现中发现契约缺口 | 提交 RFC 式变更单（问题/方案/影响面/验收），双方签字后生效 | 5 个工作日内 |

**禁止**：以「先做着看」「到时候再说」「按经验应该是」推进任何未签字项。

### 8.6 契约变更规则

| 项 | 规定 |
|---|---|
| 可变更 | 接口新增（不改既有签名）、性能指标上调、排期顺延（需双方签字） |
| 不可变更 | §1.2 的归属结论（除非重新走 L1）、§6 的不做清单、§7.4 的注入验收方式 |
| 版本号 | 每次签字变更 +0.1；重大归属变更 +1.0 |
| 生效 | 双方在本文末尾签字后生效；口头承诺无效 |

---

## 附录 A：术语表

| 术语 | 含义 |
|---|---|
| 通用层 | 与具体协议无关的传输能力：socket、事件循环、定时器、缓冲、注入接口、可靠骨架 |
| 协议注入 | 上层把自己的协议实现挂到 Go2Cpp 的事件循环上，Go2Cpp 只负责调用与搬运 |
| 骨架 / 策略 | 骨架 = 容器 + 定时器驱动 + 统计公式；策略 = 何时做、做几次、参数多少 |
| RDT/UDP v1 | 我方自研 UDP 可靠传输协议（`RDP-UDP-RESEARCH.md` §6），24 字节头、1-RTT 握手、ACK 位图、包级序号、分片重组 |
| 包级序号 | 每个 UDP 包占一个 `seq_num`，与 `frag_index` 无固定映射（规范 §6.7.4） |
| 借用 / 移交 | 借用 = 回调期间有效、不拷贝；移交 = 所有权转交、引用计数管理 |
| G1–G6 | 六道交付门禁（§7） |

## 附录 B：Go2Cpp 现有能力核对表（乙方**不需要**重复实现）

以下符号已在我方仓库中实际存在并被调用，乙方只需在其上扩展，不得重写：

| 能力 | 符号 | 位置（我方仓库副本） | 状态 |
|---|---|---|---|
| socket 生命周期 | `go2cpp::platform::SocketHandle` | `include/go2cpp/platform/socket.hpp:74-136` | 已用；需补 `RecvFrom` 的截断语义 |
| 就绪等待 | `go2cpp::platform::Poller` | `include/go2cpp/platform/io_poller.hpp:46-84` | 已用；Windows 后端需满足 G4 |
| 地址解析 | `go2cpp::platform::Resolve` | `socket.hpp:65-67` | 已用 |
| 单调时钟 | `go2cpp::platform::Now/MonotonicNanos` | `time.hpp:23-26` | 已用 |
| 定时器 | `go2cpp::core::TimerService` | `include/go2cpp/concurrency/core/timer.hpp:362-419` | 可复用；需与 `EventLoop` 归一（回调在 loop 线程） |
| 错误对象 | `go2cpp::platform::Error` | `error.hpp:51-60` | 已用；需追加 3 个枚举值 |
| 能力清单 | — | `docs/ref/go2cpp-capabilities.md` | 仅用于了解现状；验收一律以本文 §7 为准 |

**核对结论**：Go2Cpp 已具备 socket/等待/时钟/定时器/错误对象，**缺**的是「事件循环（回调分发 + 定时器归一 + 跨线程投递）」「UDP bind/listen + 报文边界」「协议注入接口」「可靠骨架」「通用流中继」。这正是本次外包的增量，不含任何协议实现。

## 附录 C：RDT/UDP v1 字段级责任映射

用于对齐会快速回答「这个字段谁管」：

| 字段（规范 §6.2） | 谁解析/生成 | Go2Cpp 是否可见 |
|---|---|---|
| `magic`(0) `version`(1) `type`(2) `flags`(3) | 我方 | **不可见**（只当字节） |
| `session_id`(4–7) | 我方 | 不可见 |
| `seq_num`(8–11) | 我方；值存入 `RetransmitEntry::seq` | 只见到 `uint64_t seq`，不知含义 |
| `ack_num`/`ack_bits`(12–19) | 我方 | 不可见；我方展开成 `Remove(seq)` 调用 |
| `payload_len`(20–21) `reserved`(22–23) | 我方 | 不可见 |
| 分片头 `frame_id`/`frag_index`/`frag_count`/`total_len` | 我方 | 不可见 |
| PING/PONG 时间戳 | 我方 | 不可见 |
| RST 错误码 | 我方 | 不可见 |

Go2Cpp 可见的全部字段只有：`Endpoint`（对端地址）、字节数、发送 token、定时器 id。

## 附录 D：变更记录

| 版本 | 日期 | 变更 | 作者 |
|---|---|---|---|
| v1.0 | 2026-11-02 | 首版：边界声明、四项需求、不做清单（24 条）、六道门禁（G1–G6）、对齐会议程 | JinShanProject 文档 agent |

---

## 签字栏（对齐会现场填写）

| 角色 | 姓名 | 结论（接受 / 有条件接受 / 不接受） | 签字 | 日期 |
|---|---|---|---|---|
| 我方 架构负责人 |  |  |  | 2026-11-03 |
| 我方 传输层实现 |  |  |  | 2026-11-03 |
| 我方 测试负责人 |  |  |  | 2026-11-03 |
| Go2Cpp 负责人 |  |  |  | 2026-11-03 |
| Go2Cpp 实现 |  |  |  | 2026-11-03 |

> 签字即表示：接受 §1.2 的 27 行归属结论、§3.3 的接口签名、§6 的 24 条不做清单、§7 的六道门禁。

