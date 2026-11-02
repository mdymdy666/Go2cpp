# Relay / JinShan 基础件（交接文档）

> **定位**：JinShan relay 替换所需的四个可复用基础件：通用帧编解码 `frame::FrameCodec`、
> 双向字节中继 `relay::RelayPipe`、连接注册表 `relay::Hub`、租约与心跳 `relay::Lease`/`Heartbeat`，
> 外加贯穿各层的背压工具「有界队列」（`Channel`/`FastChannel` 的 `TrySend`）。
> **源码**：`include/go2cpp/frame/`、`include/go2cpp/relay/`、`src/frame/`、`src/relay/`
> **构建**：`cmake/Go2CppFrame.cmake`、`cmake/Go2CppRelay.cmake`、`cmake/Go2CppRelayHubLease.cmake`
> **测试**：`tests/frame/test_frame_codec.cpp`、`tests/relay/`（含 `go2cpp_relay_bench`）
> **设计全文**：[frame-codec](../../docs/modules/frame-codec.md)、[relay-pipe](../../docs/modules/relay-pipe.md)、
> [hub-lease](../../docs/modules/hub-lease.md)、[bounded-queue](../../docs/modules/bounded-queue.md)

## 1. 这个模块是什么

JinShan 原 relay 是 Go 写的「控制连接 + 数据连接 + 会话表」服务。把它迁到 C++ 时，
真正需要重写的是四个**与业务无关的底座**：

- **FrameCodec**：RDRL/RDCH 的线上帧格式（magic + version + type + flags + length + 可选 CRC），
  负责半包/粘包/坏帧的边界判定；
- **RelayPipe**：两条字节流之间的双向搬运，带**有界队列背压**、写 deadline、慢 peer 隔离、
  Context 取消；
- **Hub**：`key → [连接]` 的注册表，支持一个 key 多条连接、`CloseKey` 一次性关闭；
- **Lease / Heartbeat**：单调时钟上的租约（到期回调一次）与心跳节拍（该发心跳/是否超时），
  不碰 socket、不建线程池。

它们合起来解决：**在不可信网络上安全地搬字节、管理连接生命周期、判断对端是否还活着、
在慢客户端面前不被拖垮**。业务侧的 token、路由、协议语义不在本模块。

## 2. 架构 / 数据流

```
        CONTROL 连接（帧）                          DATA 连接（字节流）
             │                                            │
             ▼                                            ▼
   ┌──────────────────┐                        ┌────────────────────┐
   │ frame::FrameCodec│  半包/粘包/CRC          │ relay::RelayPipe   │
   │ Encode / Feed    │                        │  reader A→B ─┐     │
   └────────┬─────────┘                        │  writer A→B ◄┘     │
            │ Frame                            │  reader B→A ─┐     │
            ▼                                  │  writer B→A ◄┘     │
   ┌──────────────────┐    Register/Lookup    │  + 看门狗(5ms)      │
   │ relay::Hub       │ ◄─────────────────────┤  有界队列(字节)     │
   │ key → [Closer]   │                       └─────────┬──────────┘
   └────────┬─────────┘                                 │
            │ CloseKey / Unregister                     │ Stats()
            ▼                                           ▼
   ┌──────────────────┐                        ┌────────────────────┐
   │ relay::Lease     │  90s 租约到期 → 关闭    │ 指标 / 日志 / 审计   │
   │ relay::Heartbeat │  30s 节拍 / 90s 判死    └────────────────────┘
   └──────────────────┘
```

### 2.1 RelayPipe 线程模型（交接重点）

```
A --Read--> [reader A→B] --> bounded queue --> [writer A→B] --Write--> B
A <--Write-- [writer B→A] <-- bounded queue <-- [reader B→A] <--Read-- B
```

- 每个方向 2 条工作线程（读 + 写），共 **4 条**；另 1 条 **5 ms 轮询的看门狗**；
- 队列按**字节**计容量（`queue_cap`，默认 256 KiB），满则阻塞读线程 → 背压；
- 方向之间只共享原子统计与取消标志，一条方向阻塞不影响另一条；
- `Run` 返回前 join 全部线程；`Run` 只能调用一次（重复调用返回 `relay: Run already called`）。

### 2.2 慢 peer 判定（不是「单次写超时」）

判据是**「队列持续满 + 一个队列容量在预算内没有排空」**：

- 首次发现队列满时记录 `stall_start`；后续 Push 等待沿用同一起点；
- 期间消费者每 Pop 一个 chunk 累计 `drained_since_stall`；
- 已排空 ≥ `queue_cap` → 说明链路仍在排水，重置计时（健康的低速链路不误杀）；
- 持续满 ≥ `write_timeout`（默认 30 s）且排空不足一个容量 → `kStalled`：关闭**目的端**、
  该方向失败、`timeouts+1`；
- 写 deadline 是「每个 chunk 一个绝对 deadline」，chunk 内多次短写共用，避免慢 drip 写无限延长。

### 2.3 帧格式（FrameCodec）

```text
 偏移  长度   字段        说明
 0     4     magic       4 字节魔数（"RDRL"/"RDCH"/自定义），逐字节相等
 4     1     version     必须等于 FrameSpec::version，不协商降级
 5     1     type        上层消息类型，本层不解释
 6     2     flags       上层标志，小端，本层不解释
 8     4     length      载荷字节数，小端，≤ max_payload
 12    h-12  reserved    扩展区（header_len > 12 时），编码写 0、解码跳过、参与校验
 h     N     payload
 ...   4     checksum    CRC-32/IEEE，小端；覆盖「帧头 + payload」，不含自身
```

校验和放**帧尾**，这样 12 字节帧头与 RDRL/RDCH 现有布局逐字节一致；`length` 纳入校验，
篡改长度会被发现而不是悄悄改变帧边界。

### 2.4 Hub / Lease 并发模型

- **Hub**：单 `std::mutex` 保护整张表；临界区只做容器操作；`CloseKey` 在**锁外**逐条
  `Close()`（JinShan 会在 `Close()` 里回调 `Hub::Unregister`，持锁调用必自锁）。
- **Lease**：内部一条定时线程（`steady_clock`）；`Renew()` 重置窗口，到期回调**只触发一次**；
  `Stop()` 幂等，回调执行中不中断但不再触发第二次。
- **Heartbeat**：**没有后台线程**，`Poll()` 是唯一驱动点；用单调时钟，测试可注入假时钟；
  `Poll()` 返回 true 表示该发心跳，`IsAlive()` 判断对端是否超时。

## 3. 对外 API（关键接口）

### 3.1 FrameCodec

```cpp
namespace go2cpp::frame {

struct Frame { std::uint8_t type = 0; std::uint16_t flags = 0;
               std::vector<std::uint8_t> payload; };

struct FrameSpec {
    std::array<std::uint8_t, 4> magic{{0,0,0,0}};  // 必须显式设置
    std::uint8_t version = 1;
    int header_len = 12;          // [12, 4096]
    int max_payload = 65536;
    std::function<std::uint32_t(const std::uint8_t*, std::size_t)> checksum =
        &Crc32IeeeFrameChecksum;  // 置空 = 不校验
    bool HasChecksum() const noexcept;
    bool Valid() const noexcept;
    std::size_t MaxFrameBytes() const noexcept;   // 有界缓冲依据
};

constexpr std::array<std::uint8_t, 4> MakeMagic(const char (&text)[5]) noexcept;
FrameSpec MakeFrameSpec(std::array<std::uint8_t, 4> magic, std::uint8_t version = 1,
                        int max_payload = 65536) noexcept;

class FrameCodec {
public:
    explicit FrameCodec(FrameSpec spec);
    bool Encode(const Frame& frame, std::vector<std::uint8_t>* out, Error* err) const;
    bool Feed(const std::uint8_t* data, std::size_t len, std::vector<Frame>* frames,
              std::size_t* consumed, Error* err);
    void Reset() noexcept;
    std::size_t BufferedBytes() const noexcept;
};
}
```

`Feed` 语义：追加解出的帧；半包时返回 true、`frames` 为空、未成帧字节留内部缓冲；
粘包一次解完；出错返回 false 并**清空内部缓冲**（帧边界已不可信，应断开连接）。
`Encode` 的 `out` 先清空，载荷超限返回 `kPayloadTooLarge`，不截断。

### 3.2 RelayPipe

```cpp
namespace go2cpp::relay {

class Stream : public streams::Reader, public streams::Writer, public streams::Closer {};

struct PipeConfig {
    std::size_t copy_buf = 32768;       // 单次读缓冲 / chunk 上限
    std::size_t queue_cap = 262144;     // 队列字节容量（背压上界）
    int write_timeout = 30000;          // 单 chunk 写 deadline + 队列持续满预算
};

struct PipeStats { std::uint64_t a_to_b, b_to_a, dropped, timeouts; };

class RelayPipe {
public:
    RelayPipe(Stream* a, Stream* b, PipeConfig cfg);      // 不启动线程
    bool Run(const ContextPtr& ctx, ErrorPtr* err);       // 阻塞转发
    bool Run(ErrorPtr* err);                              // 用 Background
    PipeStats Stats() const;
    const PipeConfig& Config() const;
};
}
```

`Run` 返回 true 仅当**两个方向都干净 EOF**；任一失败/超时/取消返回 false，`*err` 是第一条错误。
超时用哨兵 `ErrTimeout()` + `Is(err, ErrTimeout())` 判断，不要比较文本。

### 3.3 Hub / Lease / Heartbeat

```cpp
class Hub final {
public:
    using Stream = ::go2cpp::streams::Closer;
    bool Register(std::string_view key, std::shared_ptr<Stream> conn);
    std::vector<std::shared_ptr<Stream>> Lookup(std::string_view key) const;
    void Unregister(std::string_view key, const std::shared_ptr<Stream>& conn);
    bool CloseKey(std::string_view key);    // 锁外逐条 Close；第二次返回 false
    std::vector<std::string> Keys() const;  // 字典序
};

class Lease final {
public:
    Lease(std::chrono::milliseconds timeout, std::function<void()> on_expire);
    ~Lease();          // Stop + 等线程退出
    void Renew();      // 重置为「现在 + timeout」
    void Stop();       // 幂等；到期后 Renew 不复活
};

class Heartbeat {      // 无后台线程
public:
    Heartbeat(std::uint32_t interval_ms = 30000, std::uint32_t timeout_ms = 90000,
              NowFn now = nullptr, TimeoutHandler on_timeout = nullptr);
    void Start();  void Stop() noexcept;
    bool Poll() noexcept;                 // true = 该发心跳
    bool IsAlive() const noexcept;
    void OnReceive() noexcept;            // 任意消息都算活动
};
```

### 3.4 有界队列（背压）

`Channel<T>` 与 `FastChannel<T, N>` 都提供 **`TrySend` 非阻塞发送**：成功 `kOk`、满
`kWouldBlock`、关闭 `kClosed`；`Send`/`SendFor` 是阻塞版。三种标准用法：

- **丢弃**（可丢的实时数据）：`TrySend` 返回 `kWouldBlock` 就丢，计入 dropped；
- **退避重试**：`kWouldBlock` 后等一个短周期再试，或换 `SendFor(50ms)`；
- **阻塞背压**（不能丢）：直接用 `Send`，让生产者停在调度器上。

`FastChannel` 是固定容量的 MPMC 无锁环，满/空返回状态而不是阻塞；选型判据见
[bounded-queue.md](../../docs/modules/bounded-queue.md) §3。

## 4. 最小用法（可编译）

### 4.1 帧编解码（半包 + 粘包）

```cpp
#include "go2cpp/frame/frame_codec.hpp"

#include <cstdio>
#include <vector>

int main() {
    using namespace go2cpp::frame;
    FrameCodec codec(MakeFrameSpec(MakeMagic("RDRL"), /*version=*/1, /*max_payload=*/65536));

    Frame frame;
    frame.type = 0x21;
    frame.payload = {1, 2, 3, 4};
    std::vector<std::uint8_t> wire;
    Error err;
    if (!codec.Encode(frame, &wire, &err)) return 1;

    std::vector<Frame> out;
    std::size_t consumed = 0;
    // 先喂 7 字节：半包，返回 true 但 out 为空
    if (!codec.Feed(wire.data(), 7, &out, &consumed, &err) || !out.empty()) return 2;
    // 再喂剩余：解出一帧
    if (!codec.Feed(wire.data() + 7, wire.size() - 7, &out, &consumed, &err)) return 3;

    std::printf("decoded=%zu type=0x%02x bytes=%zu\n", out.size(), out[0].type,
                out[0].payload.size());
    return out.size() == 1 && out[0].payload.size() == 4 ? 0 : 4;
}
```

构建：`g++ -std=c++17 -Iinclude demo_frame.cpp -Lbuild -lgo2cpp_frame -lgo2cpp_hash -o demo_frame`
（`build` 为已构建目录；`go2cpp_frame` 只依赖 `go2cpp_hash`）。

### 4.2 RelayPipe（结构示意，需自备 Stream）

```cpp
// Stream 必须满足三条契约：Read 阻塞到 ≥1 字节/EOF/错误；Write 允许短写；
// Close 幂等、可跨线程、能让阻塞中的 Read/Write 尽快返回。
go2cpp::relay::RelayPipe pipe(stream_a, stream_b,
                              go2cpp::relay::PipeConfig{/*copy_buf=*/32768,
                                                        /*queue_cap=*/262144,
                                                        /*write_timeout=*/30000});
go2cpp::ErrorPtr err;
if (!pipe.Run(go2cpp::Context::Background(), &err)) {
    std::fprintf(stderr, "relay failed: %s\n", err ? err->Message().c_str() : "?");
}
auto stats = pipe.Stats();   // a_to_b / b_to_a / dropped / timeouts
```

Socket 适配见 [relay-pipe.md](../../docs/modules/relay-pipe.md) §5（用 `socket_stream` 包装）。

## 5. 源码位置

| 文件 | 行数 | 职责 |
|---|---:|---|
| `include/go2cpp/frame/frame_codec.hpp` / `src/frame/frame_codec.cpp` | 360 / 369 | FrameSpec/FrameCodec、半包粘包、CRC32、错误码 |
| `include/go2cpp/relay/relay_pipe.hpp` / `src/relay/relay_pipe.cpp` | 244 / 850 | Stream 适配、有界队列、4 线程 + 看门狗、背压与超时 |
| `include/go2cpp/relay/hub.hpp` / `src/relay/hub.cpp` | 157 / 112 | key → [Closer] 注册表；锁外 CloseKey |
| `include/go2cpp/relay/lease.hpp` / `src/relay/lease.cpp` | 210 / 278 | Lease 到期回调、Heartbeat 节拍与存活判定 |
| `include/go2cpp/concurrency/channel.hpp`（`TrySend`/`SendFor`） | 3700+ | `Channel<T>` 有界队列（条数容量）与阻塞/非阻塞语义 |
| `include/go2cpp/concurrency/fast_channel.hpp` | 1600+ | `FastChannel<T,N>` 固定容量 MPMC 无锁环 |
| `tests/frame/test_frame_codec.cpp` | — | 编解码 + 1e6 帧压测 + ASan/UBSan |
| `tests/relay/` | — | relay_pipe / hub / lease / heartbeat / 场景 / bench |

构建目标：`go2cpp_frame`、`go2cpp_relay`（Hub+Lease）、`go2cpp_relay_pipe`（独立，避免重复 include）。

## 6. 性能数据

都是 2026-11-01 在 WSL2 上的实测，来源见各模块文档：

| 组件 | 数字 | 条件 / 来源 |
|---|---|---|
| FrameCodec | 1e6 帧 encode→随机切分 Feed→逐帧校验 **0.218 s ≈ 4.59 Mframes/s**，VmHWM 8.58 MB | Release -O3，单线程，载荷 0–119 B；[frame-codec.md](../../docs/modules/frame-codec.md) §6 |
| RelayPipe | 双向各 1e7 字节 **49 ms ≈ 389.3 MiB/s**；1e8 双向 525 ms ≈ 363.3 MiB/s | 12 vCPU，load≈2–4；[relay-pipe.md](../../docs/modules/relay-pipe.md) §7.3 |
| RelayPipe RSS | 6.2–6.8 MiB，队列内存不超过 `queue_cap` | `/usr/bin/time -v` |
| Hub（单线程） | Register+Lookup+Unregister 1e6 次 **95.5 ns/迭代，10.47 Mops/s** | load≈3.8；[hub-lease.md](../../docs/modules/hub-lease.md) §5.4 |
| Hub（8 线程） | 独立 key 2.19 Mops/s、共享 1 key 1.60 Mops/s、只读 Lookup 3.82 Mops/s | 单锁 + 分配器竞争 |
| Lease | `Renew` **80.5 ns/次，12.43 Mops/s**；200 个 50ms 租约迟到 P99=10 ms | 同上 |
| 有界队列 | 1e8 条 4 流水线：空载 **2.15 s / 46.57 Mmsg/s**，load≈4.3 时 3.88 s / 25.77 Mmsg/s；RSS 增长 0.00 MiB | [bounded-queue.md](../../docs/modules/bounded-queue.md) §5.3 |

解读：RelayPipe 的 389 MiB/s 已经接近单核内存拷贝 + SHA-256 标量实现（约 400 MiB/s）的
上限，**瓶颈不在中继逻辑**；Hub 是房间级注册表（不是逐字节热路径），95 ns/迭代合理，
不打算分片。

## 7. 边界与已知限制

1. **RelayPipe 每条管道 5 条线程**（2 读 + 2 写 + 看门狗）：高并发流场景线程数线性增长，
   需要线程池化（未做）。
2. **`Stream` 没有半关闭**：只实现完整 `Close`；慢 peer 场景下若另一方向正从被关闭的目的端读，
   会因 Read 失败而结束。接口层无法避免，RelayPipe 不主动关闭健康方向。
3. **`Run` 只能调一次**；析构时不得有 `Run` 未返回；`Stream` 生命周期由调用方保证。
4. **Hub 单锁、不分片**：8 线程下吞吐下降明显；设计定位是「房间级注册表」，若成为瓶颈再分片。
5. **Hub 析构不关闭连接**：析构期做 I/O/回调会延长生命周期并可能重入，需显式 `CloseKey`。
6. **Lease 不取消业务操作**：它只维护时间窗口，不生成 Context；业务 deadline 由调用方自己管。
7. **Heartbeat 无后台线程**：不调 `Poll()` 就不会有任何超时判定——这是最容易踩的坑。
8. **FrameCodec 出错即清空缓冲**：帧边界不可信，调用方应断开连接；没有「跳过坏帧继续」模式。
9. **有界队列的两种容量语义**：`Channel<T>` 按**条数**，RelayPipe 的 `ChunkQueue` 按**字节**，
   不要混用；`FastChannel` 容量是编译期常量。

## 8. 测试与验证方法

```bash
# 帧编解码（含 1e6 帧压测）
ctest --test-dir build -R frame --output-on-failure
./build/go2cpp_frame_codec_test

# RelayPipe / Hub / Lease / Heartbeat / 场景
ctest --test-dir build -R 'relay|hub|lease|heartbeat' --output-on-failure

# 压测（Hub/Lease/Heartbeat）
./build/go2cpp_relay_bench
```

- `tests/relay/` 覆盖：双向 1e7/1e8 字节 byte-exact、慢 peer 只断该端、写 deadline、
  4 管道并发不串流、40 轮随机分块、Context 取消、API 边界、Hub 重入 CloseKey、
  Lease 90s 到期与时钟跳变、Heartbeat 假时钟。
- ASan/TSan：`tests/relay/test_relay_pipe.cpp` 可按 `-fsanitize=address/thread` 独立编译，
  TSan 需 `setarch --addr-no-randomize`（见 relay-pipe.md §7.3）。
- 判定标准：RelayPipe 要求 byte-exact + 队列内存 ≤ `queue_cap`；有界队列要求
  `sent == received == 1e8` 且 RSS 增长 0。

## 9. 交接注意

- **先看 `Stream` 三条契约再写适配层**：Read 阻塞语义、Write 短写、Close 幂等可跨线程；
  违反任何一条都会让超时/取消收不回线程。
- **超时判据是「队列持续满 + 排空不足」，不是单次写超时**：改这个逻辑会让「慢但健康」的
  peer 被误杀（100 Mbps 长传也会让队列长期满）。
- **Hub 的 `CloseKey` 必须在锁外调用 `Close()`**：JinShan 会在 `Close()` 里回调
  `Hub::Unregister`，持锁调用必自锁死。
- **`FrameCodec::Feed` 出错清缓冲**：不要在坏帧后复用同一个 codec 继续读同一条连接。
- **`MakeMagic` 是编译期检查 4 字符**；`FrameSpec` 的 magic 必须显式设置，默认全 0 不是合法协议。
- **与 Bento 的边界**：本模块的 `FrameCodec` 是**通用 TLV 帧**；Bento 的 `bento::net::Frame`
  是另一套 20B 头 + CRC32 协议（见 [bento-transport.md](../../docs/modules/bento-transport.md)），
  两者不共用，不要混用 magic。
- **未完成事项**：RelayPipe 线程池化、Hub 分片、半关闭接口、按流 credit 窗口。
