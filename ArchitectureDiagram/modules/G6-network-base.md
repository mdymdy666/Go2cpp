# G6 · 网络基础：`socket` / `address` / `bytearray` / `framing` / `endian` / `socket_stream`

> 依赖：`socket` → `io`（EAGAIN 时 park）+ `context`（取消）+ `core::ParkingCondition`（close 等待在途操作）；
> `address` / `bytearray` / `framing` / `endian` **不依赖调度器**，可在线程环境单独使用。
> 规模（实测）：`network/socket.hpp` 225 + `src/network/socket.cpp` 568；
> `network/address.hpp` 153 + `src/network/address.cpp` 280；
> `network/byte_array.hpp` 94 + `src/network/byte_array.cpp` 196；
> `network/framing.hpp` 129（header-only）；`network/endian.hpp` 80（header-only）。
> **转发头事实**：根路径的 `address.hpp`/`socket.hpp`/`socket_stream.hpp`/`bytearray.hpp`/`myendian.hpp`
> 都是 3 行转发头（`#include "go2cpp/network/..."`），真身在 `network/` 下
> —— 其中 `myendian.hpp → network/endian.hpp`、`socket_stream.hpp → network/socket.hpp`
> （`SocketStream` 类就定义在 `socket.hpp` 里）。

---

## 0. 一句话定位

这一层是「字节与连接」的封装：`Socket` 是 RAII 的 fd 所有者（带 in-flight 保护与延迟关闭），
`Address` 是 `sockaddr` 的多态包装（保留 `getaddrinfo` 顺序），
`ByteArray`/`Framer`/`endian` 是协议编解码与分帧工具（**都不依赖调度器**）。

---

## 1. `Socket`（RAII fd 所有者）

### 1.1 公开 API

```cpp
enum class SocketType { Tcp = SOCK_STREAM, Udp = SOCK_DGRAM, UnixStream = SOCK_STREAM, UnixDatagram = SOCK_DGRAM };
struct SocketResult { ssize_t transferred{-1}; int error{0}; bool ok() const noexcept; };   // error 为 errno

class SocketWaiter {                       // 可选等待后端（默认走当前 IOManager / poll）
  virtual int wait(int fd, io::IOEvent event, std::chrono::milliseconds timeout) = 0;
};

class Socket final : public std::enable_shared_from_this<Socket> {
  using Ptr = std::shared_ptr<Socket>;  using Duration = std::chrono::milliseconds;
  static Ptr CreateTCP(int family = AF_INET);   static Ptr CreateUDP(int family = AF_INET);
  static Ptr CreateUnix(bool datagram = false);
  static Ptr FromFd(int fd, int family, int type, int protocol = 0, bool owner = true);

  int fd() const noexcept;            // ★ 瞬时值：不进入 in-flight 保护（ABA）
  int family/type/protocol() const noexcept;  bool valid/connected() const noexcept;  int last_error() const noexcept;
  int release() noexcept;             // ★ 有在途操作时返回 -1 并转为「延迟关闭」
  void set_waiter(std::shared_ptr<SocketWaiter>) noexcept;

  bool bind(const Address&);  bool listen(int backlog = SOMAXCONN);
  Ptr accept(Duration timeout = Duration::max());
  bool connect(const Address&, Duration timeout = Duration::max());
  bool reconnect(Duration timeout = Duration::max());
  bool close() noexcept;      bool shutdown(int how) noexcept;

  SocketResult send(const void*, std::size_t, int flags = 0, Duration timeout = Duration::max());
  SocketResult recv(void*, std::size_t, int flags = 0, Duration timeout = Duration::max());
  SocketResult sendv(const iovec*, std::size_t, int flags = 0, Duration = max());
  SocketResult recvv(iovec*, std::size_t, int flags = 0, Duration = max());
  SocketResult send_to(const void*, std::size_t, const Address& to, int flags = 0, Duration = max());
  SocketResult recv_from(void*, std::size_t, AddressPtr& from, int flags = 0, Duration = max());

  AddressPtr local_address() const;  AddressPtr remote_address() const;
  bool set_recv_timeout(Duration);   bool set_send_timeout(Duration);
  Duration recv_timeout() const noexcept;  Duration send_timeout() const noexcept;
  bool set_option(int level, int option, const void*, socklen_t);
  bool get_option(int level, int option, void*, socklen_t*) const;
  bool cancel_read() noexcept;       bool cancel_write() noexcept;   // 跨线程取消当前等待
};
// Go 风格便捷入口
AddressPtr ResolveTCPAddr/ResolveUDPAddr(host, port, family = AF_UNSPEC);
Socket::Ptr DialTCP/DialUDP(host, port, timeout = max);
Socket::Ptr ListenTCP(host, port, backlog = SOMAXCONN);  Socket::Ptr ListenUDP(host, port);
```

### 1.2 两个关键机制（本模块的核心）

**① in-flight 操作保护 + 延迟关闭**

```text
begin_operation(fd)  → 锁定并固定 fd 与代际（m_generation），m_inflight++，
                        同时把 this 存进 m_inflight_keepalive（自持有，防最后一个外部 shared_ptr 释放导致析构）
真实 syscall
end_operation()      → m_inflight--，若 m_close_pending 且归零 → finalize_deferred_close()（唯一最终关闭者）
close()              → 若当前上下文无法 park：置 m_close_pending = true 并返回 false，
                        把最终关闭交给最后一个在途操作（避免 fd 泄漏）
                       否则 wait_close_state() 等到在途归零后再关
```

- 等待用 `core::ParkingCondition m_operation_condition`，**复用 `m_mutex` 保护的谓词** ——
  普通线程阻塞原生 cv、受管 Fiber park，**close 等待在途操作时不会占住 M，也不会丢通知**。
- `m_read_cancel_generation` / `m_write_cancel_generation`（`atomic<uint64_t>`）：
  `cancel_read()`/`cancel_write()` 发布一个**取消代际**，`wait_ready()`/`wait_or_retry()`
  按代际判断是否是"我的"取消（避免误伤后续操作）。
- `wait_ready()` 把 timeout **切成 ≤100 ms 的片**轮询，并有原生 poll 回退
  （即：即使有 IOManager，也不会无限 park 在一次等待里）。

**② `fd()` 是瞬时值**

头文件 `@note` 明写：返回值**不进入 `begin_operation` 保护**，拿到之后若发生 `close()` 或 fd 复用即失效（ABA）；
库内部所有操作都会自行固定 fd；**调用方不应跨操作持有该返回值**。

### 1.3 `SocketStream`

```cpp
class SocketStream {          // 自动处理 partial I/O 的字节流接口
  explicit SocketStream(Socket::Ptr, bool owner = true);
  ssize_t read(void*, std::size_t);   ssize_t write(const void*, std::size_t);
  ssize_t readv(iovec*, std::size_t); ssize_t writev(const iovec*, std::size_t);
  bool write_all(const void*, std::size_t);   bool close() noexcept;   bool connected() const noexcept;
};
```

### 1.4 坑点

1. `fd()` 是瞬时值（见上），**不要**把它当作稳定句柄缓存。
2. `release()` 在有在途操作时返回 **-1**（不是抛异常）；此时 fd 会在最后一个操作结束时关闭。
3. `close()` 在无法 park 的上下文（例如普通线程已关闭、调度器 draining）返回 `false`，
   但**关闭仍会发生**（延迟关闭路径）——不能把 `false` 理解为"没关"。
4. 接收/发送超时默认是 `Duration::max()`（无限）；要超时必须显式传或 `set_*_timeout`。
5. `accept()` 返回的 `Ptr` 是新的拥有者；原监听 socket 仍然有效。

---

## 2. `Address`（`sockaddr` 多态包装）

```cpp
class Address {                                   // 只拥有地址值，不拥有 Socket
  static Ptr Create(const sockaddr*, socklen_t);          // 按 family 复制出正确子类
  static bool Lookup(std::vector<Ptr>& out, const std::string& host,
                     int family = AF_UNSPEC, int type = 0, int protocol = 0);   // ★ 保留全部候选与顺序
  static Ptr LookupAny(host, family, type, protocol);
  static std::shared_ptr<IPAddress> LookupAnyIPAddress(host, family, type, protocol);
  virtual int family() const noexcept;  virtual const sockaddr* addr() const noexcept;
  virtual sockaddr* addr() noexcept;    virtual socklen_t addr_len() const noexcept;
  virtual std::string to_string() const;
  bool operator<(const Address&) const noexcept;   // 按二进制内容比较（排序/测试）
  bool operator==(const Address&) const noexcept;
};
class IPAddress : public Address {                // 带端口的 IP 地址通用接口
  virtual std::uint16_t port() const noexcept;  virtual void set_port(std::uint16_t) noexcept;
  virtual Ptr broadcast_address(std::uint32_t prefix_len) const;
  virtual Ptr network_address(std::uint32_t prefix_len) const;
  virtual Ptr subnet_mask(std::uint32_t prefix_len) const;
  static Ptr Create(const char* address, std::uint16_t port = 0);
};
class IPv4Address final : public IPAddress;   // prefix_len ∈ [0, 32]；内部 sockaddr_in
class IPv6Address final : public IPAddress;   // 保留 scope id（链路本地）；内部 sockaddr_in6
class UnixAddress final : public Address;     // 支持抽象命名空间；path()
class UnknownAddress final : public Address;  // sockaddr_storage 保留系统扩展地址族
```

坑点：

- ★ **IPv6 的 `broadcast_address()` 语义不是广播**：IPv6 没有广播（RFC 4291 用 multicast 取代），
  实现实际返回"host 位全 1 = 子网最后一个地址"，与 IPv4 版本（`host | ~mask`）**不同质**
  （台账 X-28；头文件与实现均未说明，命名有误导性）。
- `Lookup` 用 `getaddrinfo` + `AI_ADDRCONFIG`，**保留候选顺序**，调用方自行决定 IPv4/IPv6 回退策略。

---

## 3. `ByteArray`（协议编解码缓冲）

```cpp
enum class ByteOrder { Little, Big };
class ByteArray final {
  explicit ByteArray(std::size_t base_size = 4096);
  std::size_t position() const noexcept;  void set_position(std::size_t);
  std::size_t size() const noexcept;  std::size_t capacity() const noexcept;  std::size_t readable_size() const noexcept;
  void clear() noexcept;  ByteOrder order() const noexcept;  void set_little_endian(bool) noexcept;
  void write(const void*, std::size_t);  std::size_t read(void*, std::size_t);
  std::string to_string() const;         std::string to_hex_string() const;

  // 定长（f = fixed）
  void write_fuint8/fint8/fuint16/fint16/fuint32/fint32/fuint64/fint64(...);  void write_float/write_double(...);
  // 变长（无 f 前缀）
  void write_uint32/uint64/int32/int64(...);   void write_varuint32/64(...);   void write_varint32/64(...);
  // 字符串：16/32 位长度前缀、varint 长度前缀、无长度
  void write_string_f16/f32/vint/without_length(const std::string&);
  std::string read_string_f16/f32/vint/without_length(std::size_t length);

  std::size_t get_read_buffers(std::vector<iovec>&, std::size_t max_count = 16) const;   // 从 position 起的只读 iovec
  std::size_t get_write_buffers(std::vector<iovec>&, std::size_t length, std::size_t max_count = 16);
};
```

- ★ **默认小端**（`ByteOrder::Little`）—— 头文件明确：「本地序列化用小端；**网络协议应显式
  `set_little_endian(false)`**」。这是一个容易静默出错的默认值。
- **单所有者对象**：读写游标 `m_position` 不能被多个线程同时修改（头文件明写约束）。
- 提供 `iovec` 出口（`get_read_buffers`/`get_write_buffers`），可直接对接 `Socket::sendv/recvv`。
- 越界读取抛 `std::out_of_range`；超长字符串（>64 MB）走 `length_error`（子代理实测结论）。

---

## 4. `Framer`（TCP 分帧，header-only）

```cpp
class Framer final {
  enum class Mode { Delimiter, Fixed, BigEndian32Length };
  enum class FeedError { None, FrameTooLarge, ResourceExhausted, Failed };
  static Framer delimiter(std::string delimiter, std::size_t max_frame = 16 * 1024 * 1024);
  static Framer fixed(std::size_t frame_size);
  static Framer big_endian32(std::size_t max_frame = 16 * 1024 * 1024);

  FeedError try_feed(std::string_view data) noexcept;   // ★ 不向调用方抛分配异常
  void feed(std::string_view data);                     // 兼容接口：会抛 length_error/bad_alloc/runtime_error
  std::vector<std::string> take_frames();               // swap 出全部完整帧
  std::size_t buffered_size() const noexcept;
  void reset() noexcept;                                // 清缓冲 + 失败态，供丢弃损坏连接后复用
};
```

语义要点（逐行读证）：

- **失败即清空**：`fail()` 置 `m_failed=true` 并 `swap` 清空 `m_buffer` 与 `m_frames`
  —— 避免调用方把部分处理结果误认为完整批次；失败后进行任何 `try_feed` 都返回 `Failed`，直到 `reset()`。
- 分隔符模式：缓冲区里没有分隔符且 `size() > max_frame` → `FrameTooLarge`；
  分隔符位置 `> max_frame` → `FrameTooLarge`。
- 大端 32 位长度前缀：先检查 `length > max_frame || length > SIZE_MAX - 4`（**防整数溢出**）再等数据。
- 异常映射：`length_error → FrameTooLarge`、`bad_alloc → ResourceExhausted`、其它 → `ResourceExhausted`。
- 构造期校验：空分隔符 / 零帧长 → `std::invalid_argument`。
- 坑：**单所有者、无锁**，头文件注释**没有声明并发约束**（台账 X-29：与 `sse::Parser`、`ByteArray`
  的对称约束缺失）；`feed()` 是"会抛异常"的兼容接口，`try_feed()` 才是推荐用法。

---

## 5. `endian`（无状态工具，header-only）

```cpp
constexpr std::uint16_t byteswap16(std::uint16_t) noexcept;
constexpr std::uint32_t byteswap32(std::uint32_t) noexcept;
constexpr std::uint64_t byteswap64(std::uint64_t) noexcept;
constexpr std::uint16_t host_to_be16/32/64(...) noexcept;   // 小端主机才交换（#if __BYTE_ORDER__）
constexpr std::uint16_t be_to_host16/32/64(...) noexcept;
```

不依赖 Socket 或调度器；协议层可以替换为自己的编码策略。根路径 `go2cpp/myendian.hpp` 是它的转发头。

---

## 6. 与 Go 的对应与差异

| Go | 本项目 | 差异 |
|---|---|---|
| `net.Conn`（Read/Write 阻塞语义） | `Socket` + `SocketResult{transferred, error}` | 返回 errno，调用方需自己重试 EAGAIN 循环 |
| `net.Dial`/`net.Listen` | `DialTCP/DialUDP/ListenTCP/ListenUDP` | 无 `net.Dialer` 的超时/KeepAlive/控制回调 |
| `net.Addr` | `Address`/`IPAddress`/`IPv4Address`/`IPv6Address`/`UnixAddress`/`UnknownAddress` | 多出子网计算（`broadcast/network/subnet_mask`） |
| `net.IPNet` | `IPAddress::network_address/subnet_mask(prefix_len)` | — |
| `bytes.Buffer` | `ByteArray` | 多出定长/变长/长度前缀字符串与 iovec 出口；**默认小端** |
| `bufio.Scanner` + 自定义分割 | `Framer`（分隔符/定长/大端 32 位长度） | 内置 max_frame 防恶意长度；失败即清空 |
| `encoding/binary` | `endian` 工具 | 只有字节序转换，无反射式编解码 |

---

## 7. 对架构图的贡献

**节点**

- `Socket`（fd 所有者 + `m_inflight` 保护 + 延迟关闭 + 读/写取消代际）
- `SocketWaiter`（可替换等待后端）
- `Address` 族（`IPv4/IPv6/Unix/Unknown`；`getaddrinfo` 候选保留）
- `ByteArray`（定长/变长/长度前缀 + iovec 出口；默认小端）
- `Framer`（分隔符 / 定长 / 大端 32 位长度；失败即清空）
- `endian`（constexpr 字节序工具）、`SocketStream`（partial I/O 处理）

**连线**

- `Socket → IOManager / SocketWaiter`：「EAGAIN → wait_ready（≤100 ms 切片 + poll 回退）」
- `Socket → ParkingCondition`：「close/release 等待在途操作归零（Fiber park，不占 M）」
- `Socket → FdManager`：「用户/系统非阻塞标志与收发超时元数据」
- `Address → Socket`：「bind/connect/accept 的 sockaddr」
- `Framer → SocketStream`：「字节流 → 消息帧；take_frames 交付完整帧」
- `ByteArray ⇄ Socket`：「get_read_buffers/get_write_buffers 直接对接 sendv/recvv」
- `Hook → Socket`：「被拦截的 libc 调用最终落到 Socket 的等待策略」

**图内文字**

> 网络基础层是「字节与连接」：`Socket` 拥有 fd，用 **in-flight 计数**保证 close 不会打断正在进行的
> 系统调用（无法 park 时转「延迟关闭」，由最后一个操作关 fd）。`fd()` 是**瞬时值**，不能跨操作持有。
> `Address` 保留 `getaddrinfo` 的候选顺序；`ByteArray` **默认小端**（网络协议必须显式改大端）；
> `Framer` 用 `max_frame` 挡恶意长度，一旦失败就**清空缓冲与已取帧**，必须 `reset()` 才能复用。
> 本层除 `Socket` 外都不依赖调度器，可在线程环境中单独使用。
