# G6 · 网络基础 —— 一图看懂

> 覆盖：`Address` 体系（`IPAddress`/`IPv4Address`/`IPv6Address`/`UnixAddress`/`UnknownAddress`）、
> `Socket`/`SocketResult`/`SocketType`/`SocketWaiter`、`SocketStream`、
> `Framer`（分帧）、`ByteArray`（字节缓冲）、`endian`（字节序）。
> 源码：`include/go2cpp/network/address.hpp`、`socket.hpp`、`framing.hpp`、`byte_array.hpp`、`endian.hpp`。

## ① 类架构：每个类用的底层能力 + 主要职责

```mermaid
classDiagram
    direction TB

    class Address {
        <<地址抽象>>
        +Network() string
        +String() string
    }
    class IPAddress {
        <<IP 基类>>
    }
    class IPv4Address {
        <<点分十进制>>
        +Bytes() / Port()
    }
    class IPv6Address {
        <<带 scope id>>
    }
    class UnixAddress {
        <<文件系统路径>>
    }
    class UnknownAddress {
        <<无法识别时占位>>
    }

    class SocketType {
        <<枚举>>
        Tcp
        Udp
        UnixStream
        UnixDatagram
    }
    class SocketResult {
        <<结果 + 错误>>
    }
    class SocketWaiter {
        <<可等待的 socket 操作>>
        +Wait(ctx) bool
        +WaitFor(timeout, ctx) bool
        +Result() SocketResult
    }
    class Socket {
        <<一个连接/监听>>
        +Bind / Listen / Accept
        +Connect
        +Read / Write
        +Close
        +SetDeadline(receive, send)
        +LocalAddr / RemoteAddr
    }
    class SocketStream {
        <<带缓冲的读写视图>>
        +ReadFull(buf) 
        +ReadLine()
        +WriteAll(data)
    }

    class ByteArray {
        <<字节缓冲>>
        +Append / Resize
        +Bytes()
    }
    class Framer {
        <<分帧器>>
        +Append(bytes)
        +NextFrame(out) bool
        +SetMaxFrameSize(n)
    }
    class EndianCodec {
        <<字节序编解码>>
        +ReadU16/U32/U64
        +WriteU16/U32/U64
    }

    class IOBackend {
        <<G5 事件循环>>
    }
    class ParkingCondition {
        <<G1 等待原语>>
    }

    Address <|-- IPAddress
    IPAddress <|-- IPv4Address
    IPAddress <|-- IPv6Address
    Address <|-- UnixAddress
    Address <|-- UnknownAddress
    Socket ..> Address : 本地/远端地址
    Socket ..> IOBackend : 读写走事件循环（G5）
    Socket ..> ParkingCondition : 等待时挂起 G
    SocketWaiter ..> Socket
    SocketWaiter ..> ParkingCondition
    SocketStream *-- Socket
    SocketStream ..> ByteArray : 内部缓冲
    Framer ..> ByteArray : 拼包/拆包
    Framer ..> EndianCodec : 读长度前缀
```

| 类 | 主要职责 | 用的底层能力 |
|---|---|---|
| `Address` 体系 | 地址抽象：TCP/UDP/Unix，`String()` 与解析 | 系统 `sockaddr` 转换 |
| `Socket` | 一个 fd 的面向对象封装：bind/listen/accept/connect/read/write/close | **G5 的 hook + IOBackend**、`ParkingCondition` |
| `SocketWaiter` | 把"一次可能阻塞的操作"做成**可 `Wait(ctx)`/`WaitFor(timeout)`** 的对象 | `ParkingCondition` |
| `SocketStream` | 在 `Socket` 上提供 `ReadFull`/`ReadLine`/`WriteAll` 这类便利方法 | 内部 `ByteArray` |
| `Framer` | **分帧**：把字节流切成一条条消息（长度前缀 / 分隔符，含 `max_frame_size` 保护） | `ByteArray`、`EndianCodec` |
| `ByteArray` | 可增长的字节缓冲 | `std::vector<char>` 类设施 |
| `EndianCodec` | 大小端读写 | 位运算 |

## ② 重要函数：功能与实现要点

```mermaid
flowchart TB
    subgraph ACC["Socket::Accept"]
        A1["先直接 accept 一次"] --> A2{"EAGAIN?"}
        A2 -- 否 --> A3["返回新 Socket（并登记到 FdManager）"]
        A2 -- 是 --> A4["注册 fd 可读 -> park 当前 G"]
        A4 --> A5["就绪后重试 accept（循环）"]
        A5 --> A3
    end

    subgraph FRAME["Framer 的拆包流程"]
        F1["Append(新收到的字节)"] --> F2["检查缓冲里是否够一个长度前缀"]
        F2 -- 不够 --> F3["返回，等更多数据"]
        F2 -- 够 --> F4["读长度 L（按设定字节序）"]
        F4 --> F5{"L > max_frame_size?"}
        F5 -- 是 --> F6["**拒绝并报错**<br/>（防恶意超长帧耗尽内存）"]
        F5 -- 否 --> F7{"缓冲里够 L 字节?"}
        F7 -- 否 --> F3
        F7 -- 是 --> F8["NextFrame 取出一条完整消息<br/>并从缓冲消费掉"]
        F8 --> F2
    end

    subgraph RD["SocketStream::ReadFull / ReadLine"]
        R1["循环调用底层 Socket::Read<br/>（每次可能 park）"] --> R2{"已读够?"}
        R2 -- 否 --> R1
        R2 -- 是 --> R3["返回"]
        R1 --> R4{"对端关闭?"}
        R4 -- 是 --> R5["ReadFull 返回短读；ReadLine 返回已有内容"]
    end

    subgraph DL["SetDeadline"]
        D1["SetDeadline(receive, send)"] --> D2["写入 FdContext（G5）"]
        D2 --> D3["之后每个等待都把该 deadline 传给 park"]
        D3 --> D4["超时 -> 返回超时错误，**不关闭** socket"]
    end

    ACC --> FRAME
    FRAME --> RD
    RD --> DL
```

**要点**

- `Accept`/`Read`/`Write` 都是 **"先试一次，EAGAIN 就注册事件 + park，就绪后重试"** 的循环 —— 这是 hook 的标准形态；
- **`Framer` 的 `max_frame_size` 是安全边界**：没有它，一个声明了 4GB 长度的帧就能把内存吃光；
- `SetDeadline` **只影响等待**，超时**不关闭** socket（与 Go 的 `SetDeadline` 语义一致）；
- `SocketStream` 的 `ReadFull` 在**对端提前关闭**时返回**短读**，而不是无限循环等；
- 分帧的**长度字段字节序可配置**（`EndianCodec`），跨语言协议要显式约定。

## ③ 向上层提供了什么

```mermaid
graph LR
    subgraph G6API["网络基础能力"]
        B1["Address 解析/格式化（TCP/UDP/Unix）"]
        B2["Socket：bind/listen/accept/connect/read/write/close/deadline"]
        B3["SocketWaiter：可 Wait(ctx)/WaitFor(timeout) 的操作"]
        B4["SocketStream：ReadFull/ReadLine/WriteAll"]
        B5["Framer：长度前缀分帧 + 超长保护"]
    end
    subgraph USE["谁在用"]
        V1["http / websocket / sse：<br/>全部建立在 Socket + Framer 之上"]
        V2["DialTCP / ListenTCP 等顶层便捷函数"]
        V3["调试模块：不直接用；<br/>但 socket 等待会体现为 G 的 kWaiting"]
    end
    B1 --> V2
    B2 --> V1
    B3 --> V1
    B4 --> V1
    B5 --> V1
```

## ④ 状态流转

### 一个 Socket 的生命周期

```mermaid
stateDiagram-v2
    [*] --> kCreated : socket() 创建 fd
    kCreated --> kBound : Bind(addr)
    kBound --> kListening : Listen(backlog)
    kListening --> kAccepting : Accept（可 park）
    kAccepting --> kListening : 拿到新连接
    kCreated --> kConnecting : Connect（可 park）
    kConnecting --> kConnected : 成功
    kConnected --> kReading : Read（EAGAIN 则 park）
    kConnected --> kWriting : Write（EAGAIN 则 park）
    kReading --> kConnected : 读到数据 / 短读
    kWriting --> kConnected : 写完 / 短写
    kConnected --> kClosed : Close()
    kListening --> kClosed : Close()
    kReading --> kClosed : 对端关闭（读到 EOF）
    kClosed --> [*]
```

### Framer 的缓冲状态

```mermaid
stateDiagram-v2
    [*] --> kEmpty : 新建
    kEmpty --> kPartialPrefix : Append 但不足一个长度前缀
    kPartialPrefix --> kPartialBody : 前缀够了但 body 不足
    kEmpty --> kPartialBody : 前缀与部分 body 都到了
    kPartialBody --> kPartialBody : 继续 Append
    kPartialBody --> kEmpty : NextFrame 取走一条且无剩余
    kPartialBody --> kPartialBody : NextFrame 取走一条但还有剩余（粘包）
    kPartialPrefix --> kError : 声明长度 > max_frame_size
    kPartialBody --> kError : 同上
    kError --> [*]
```

## 与 Go 的差异

| 项 | Go | Go2Cpp |
|---|---|---|
| socket 抽象 | `net.Conn` 接口 | 具体类 `Socket` + `SocketStream` |
| 分帧 | 需第三方或手写 | 内置 `Framer`（长度前缀 + `max_frame_size` 保护） |
| 超时 | `SetDeadline/SetReadDeadline` | `Socket::SetDeadline(receive, send)`（记在 `FdContext`） |
| 地址 | `net.Addr` 接口 | `Address` 继承体系（`IPv4`/`IPv6`/`Unix`/`Unknown`）|
| 字节序 | `encoding/binary` | `EndianCodec`（大小端读写） |
