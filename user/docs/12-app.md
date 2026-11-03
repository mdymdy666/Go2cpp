# 应用层（交接文档）

> **定位**：把「一次游戏资源下发」和「一张 `k1=v1;…` 表」统一抽象成 **HTTP/2 风格的流表**，并给出可直接跑的传输适配与测试。
> **源码**：`include/go2cpp/app/`（13 个头）、`src/app/`（12 个实现）；构建开关 `-DGO2CPP_WITH_APP=ON`（默认 OFF）、测试 `-DGO2CPP_APP_TESTS=ON`
> **构建接入**：库目标 `go2cpp_app`；Bento 目标存在时追加帧桥与 Link 适配，`bento::HttpServer` 存在时追加 HTTP 隧道
> **测试目标**：`app_test_frame`、`app_test_stream_table`、`app_test_resource`、`app_test_kv`、`app_test_connection`、`app_test_load`、`app_test_bento`、`app_test_http`
> **设计全文**：[app-framework.md](../../docs/modules/app-framework.md)（协议表、状态机、WFQ、全部实测数字）、[bento-app.md](../../docs/modules/bento-app.md)（相邻层：fetch/zip/backup/capability/command/file_transfer）

## 1. 这个模块是什么

`go2cpp::app` 是**应用层协议框架**，不是业务逻辑。它解决三个具体问题：一条连接上跑多个有独立窗口/优先级/生命周期的逻辑流；一个 100 MB 资源怎么分块、校验、续传；一张大小不定的 k=v 表怎么发（小表一帧、大表自动分区、或每键一流）。

```
┌──────────────────────────────────────────────────────────────┐
  业务层  │ KvStream（小/大/每键一流）  ResourceTransfer（块/校验/续传）
        └──────────────┬───────────────────────┬───────────────┘
                       │ HEADERS / DATA
        ┌──────────────▼───────────────────────▼───────────────┐
  流表层 │ Connection：握手、帧分发、发送调度、事件、流控归还
        │ StreamTable：状态机 + 连接/流窗口 + 依赖树 WFQ
        └──────────────┬───────────────────────────────────────┘
                       │ 9 B 帧（length/type/flags/stream_id）
        ┌──────────────▼───────────────────────────────────────┐
  传输层 │ ITransport：Memory / Bento Link(STREAM) / HTTP 隧道
        └──────────────────────────────────────────────────────┘
```

**分层原则**：流表不知道字节从哪来，传输层不知道帧的含义，业务层不碰窗口与优先级。三者可分别测试。

### 1.1 与 HTTP/2 同构到什么程度

| 维度 | HTTP/2（RFC 7540） | 本框架 |
|---|---|---|
| 帧头 | 9 B：length(24)+type(8)+flags(8)+R(1)+stream_id(31) | **相同**，`R` 必须为 0 |
| 帧类型 | DATA/HEADERS/PRIORITY/RST_STREAM/SETTINGS/PING/GOAWAY/WINDOW_UPDATE | 同上 + **RESOURCE_INDEX**(0x0d)，去掉 PUSH_PROMISE/CONTINUATION |
| 头部编码 | HPACK | **显式 KV 二进制**（varint 长度前缀），无状态机、无压缩炸弹 |
| 流状态 | idle/reserved/open/half-closed/closed | idle/open/half_closed_local/half_closed_remote/closed（**无 reserved**） |
| 流控 | 连接窗口 + 流窗口 + WINDOW_UPDATE | **相同**（默认连接 1 MiB、流 64 KiB） |
| 优先级 | 依赖树 + 权重（RFC 9113 已弃用） | **严格父优先 + WFQ**（虚拟完成时间，整数运算，可单测） |
| 默认帧大小 | 16 KiB | **256 KiB**（上限 16 MiB）；默认载荷上限 1 MiB |

一句话：**帧头/状态机/窗口与 HTTP/2 同构，头部编码与调度按「资源分发」重新取舍**，业务语义作为 HEADERS 里的普通键值，不新增协议层。

### 1.2 与 `go2cpp::bento::app` 的区别（容易混）

| | `go2cpp::app`（本文） | `go2cpp::bento::app` |
|---|---|---|
| 目的 | 通用应用层**流协议** | 一组**具体运维能力**：fetch/zip/backup/capability |
| 依赖方向 | 只依赖 `hash`，可选 Bento | 依赖 `bento::os`/`hash`/`net` |
| 头文件 | `include/go2cpp/app/*.hpp` | `include/go2cpp/bento/app/*.hpp` |
| 何时看 | 要做资源下发 / KV 同步 | 要做文件下载、打包上传、环境探测 |

`bento::app` 的 `command` / `file_transfer` 帧自带 magic 与总长校验，**可跑在任意可靠信道上，不是本框架的依赖**。

## 2. 架构 / 数据流

### 2.1 握手

```
Client: "APP2\r\n\r\n" + SETTINGS         Server: "APP2\r\n\r\n" + SETTINGS
双方: 收到 SETTINGS → SETTINGS ACK        ready() = 收到对端 preface + SETTINGS
```

preface 固定 8 字节、双方都发：误接到别的端口会在前 8 字节失败，而不是把垃圾当帧继续解析。SETTINGS 协商并发上限、初始流窗口、单帧上限、HEADERS 上限、小 KV 阈值、资源块大小。

### 2.2 线程模型（交接重点）

- `Step()` / `ReadEvent()` 是 IO 驱动入口，**只能由一个 IO 线程/Fiber 调用**；
- `SendKv*` / `SendResource*` / `SendBytes` / `ResetStream` 可在**任意线程**调用（内部一把 `recursive_mutex` 串行化流表）；
- 事件回调在 `Step` 线程同步执行，可以**重入发送 API**（同一线程递归锁，`EventHandlerReentrancy` 覆盖）；
- 事件队列有独立锁，`ReadEvent` 等待时释放 IO 锁。

### 2.3 一次资源下发的数据流

```
发送端                                                     接收端
BuildResourceMeta（扫描 SHA-256/CRC/块数）   HEADERS: app.kind=resource, size, sha256, chunks…
  ├─ DATA: chunk_index(u32) | offset(u32) | crc32(u32) | data
  │    一块可跨多帧（受窗口/帧上限约束），offset 保证能检测不连续/重复
  ├─ …                                                     逐帧校验 index/offset 连续性
  └─ DATA(END_STREAM)                                      凑齐整块 → 先校验 CRC → 写 sink
                                                            增量更新 SHA-256
                                                            Finish 校验块数/大小/整资源 SHA-256
                                                            回空 DATA(END_STREAM) 作为关闭确认
```

内存边界：接收端只保留「当前块」缓冲（≤4 MiB）+ SHA-256 状态，**不缓存整资源**。

## 3. 对外 API（关键接口）

### 3.1 帧（`include/go2cpp/app/frame.hpp`）

```cpp
enum class FrameType : std::uint8_t {
    kData = 0x00, kHeaders = 0x01, kPriority = 0x02, kRstStream = 0x03, kSettings = 0x04,
    kPing = 0x06, kGoAway = 0x07, kWindowUpdate = 0x08, kResourceIndex = 0x0d,
};
struct Frame {
    std::uint32_t stream_id = 0;        // 0 = 连接级
    FrameType type = FrameType::kData;  std::uint8_t flags = kFlagNone;   // END_STREAM/ACK/END_HEADERS/RESUME
    std::vector<std::uint8_t> payload;  // 拥有时有效
    const std::uint8_t* view = nullptr; std::uint32_t view_bytes = 0;     // 借用视图
    std::size_t Size() const noexcept;  const std::uint8_t* Bytes() const noexcept;
};
// ErrorCode 0..7 对照 HTTP/2（kNoError..kCancel），8.. 是本框架扩展：
//   kChecksumError=8, kResourceNotFound=9, kResumeOutOfRange=10, kInvalidChunk=11
class FrameDecoder { /* 流式解码，处理粘包/半包，解码前按长度拒绝超限帧 */ };
```

### 3.2 连接（`include/go2cpp/app/connection.hpp`）

```cpp
enum class Role : std::uint8_t { kClient = 0, kServer = 1 };   // 决定流 id 奇偶
struct SendResult { ErrorCode error = ErrorCode::kNoError; std::uint32_t stream_id = 0;
                    std::string message; bool ok() const noexcept; };   // stream_id=0 表示失败
class Connection {
public:
    Connection(TransportPtr transport, Role role, ConnectionConfig config = {});
    bool Start(std::string* error = nullptr);              // preface + SETTINGS
    int  Step(std::uint32_t timeout_ms);                   // 返回处理帧数；-1 = 已关闭
    bool ReadEvent(Event* out, std::uint32_t timeout_ms);  // 内部会调用 Step
    void SetEventHandler(std::function<void(const Event&)> handler);   // Step 线程同步调用
    void Close(ErrorCode code = ErrorCode::kNoError, std::string message = {});

    SendResult SendKv(const KvTable& table, std::uint16_t weight = 0);      // 小/大自动选
    SendResult SendKvText(std::string_view text, std::uint16_t weight = 0); // k1=v1;k2=v2
    SendResult SendKvPerKey(const KvTable& table, std::vector<std::uint32_t>* stream_ids = nullptr, ...);
    SendResult SendResource(ResourceMeta meta, std::shared_ptr<ChunkSource> source, ...);
    SendResult SendBytes(std::vector<std::uint8_t> data, const KvTable& meta = {}, ...);
    SendResult SendResourceIndex(const ResourceIndex& index);
    SendResult ResetStream(std::uint32_t stream_id, ErrorCode code = ErrorCode::kCancel);
    SendResult Ping(const std::array<std::uint8_t, 8>& opaque = {});
    void SetResourceSinkFactory(std::function<std::unique_ptr<ResourceSink>(const ResourceMeta&)>);
    bool Acknowledge(std::uint32_t stream_id, std::size_t bytes);   // 手动归还窗口
    ConnectionStats stats() const;
};
// EventType: kHeaders, kData, kEndStream, kReset, kGoAway, kPing, kSettings,
//            kKvComplete, kResourceComplete, kResourceIndex, kClosed, kError, kWindowUpdate
```

`Event` 里 `headers` 在 `kKvComplete` 时是**合并后的完整业务表**（`app.*` 保留键已过滤）；`kResourceComplete` 带 `resource_meta` 与 `resource_stats`（含 `verified`）。

### 3.3 流表（`include/go2cpp/app/stream_table.hpp`）

```cpp
struct StreamTableConfig {
    bool client = true;                          // 本端流 id 用奇数
    std::uint32_t max_concurrent_streams = 256;  // 未关闭流上限（含 idle）
    std::uint32_t max_retired_streams = 256;     // 已关闭流保留数（迟到帧容忍期）
    std::int64_t initial_stream_window = 65535;
    std::int64_t connection_window = 1024 * 1024;
    std::uint32_t max_frame_payload = 256 * 1024;
    std::uint16_t default_weight = 16;
};
class StreamTable { /* 状态机 + 两级窗口 + 依赖树 WFQ；纯逻辑，不碰 IO */ };
```

WFQ：`finish += 已发字节 * 2^20 / weight`，每次选 `finish` 最小者；有祖先可发时子流本轮不参与。环检测拒绝把 A 挂到 A 的后代下；删父流后子流自动上浮到根。

### 3.4 KV 与资源

```cpp
enum class KvMode : std::uint8_t { kSingle = 0, kStream = 1, kPerKey = 2 };
class KvTable {
    std::string Get(std::string_view key, std::string_view fallback = {}) const;
    bool Set(std::string key, std::string value);
    static bool ParseText(std::string_view text, KvTable* out, ...);          // k1=v1;k2=v2
    static bool ParseBinary(const std::uint8_t* data, std::size_t size, KvTable* out, ...);
};
// 接收侧：KvAssembler（合并 HEADERS+DATA，默认上限 64 MiB）/ KvBatchCollector（每键一流的合并）

struct ResourceMeta {
    std::string name;  std::uint64_t size = 0;  std::string sha256_hex;   // 64 hex，空=不校验
    std::uint32_t chunk_size = kDefaultResourceChunkSize;  std::uint32_t chunk_count = 0;
    std::uint32_t crc32 = 0;  std::uint32_t version = 1;  std::uint32_t resume_chunk = 0;
    KvTable extra;
    KvTable ToKv() const;  static bool FromKv(const KvTable&, ResourceMeta*, std::string*);
    bool Validate(std::string* error = nullptr) const;
};
ResourceMeta BuildResourceMeta(std::string name, ChunkSource& source,
                               std::uint32_t chunk_size = kDefaultResourceChunkSize);
// ChunkSource 必须支持乱序/重复读取；实现：MemoryChunkSource / FileChunkSource / PatternChunkSource
// Sink 实现：MemoryResourceSink / FileResourceSink(path, truncate=false 用于续传)
```

续传：发送端 `SendResource(meta, source, start_chunk = N)` 自动在 HEADERS 带 `app.resume=N`；接收端按 `resume_chunk` 建接收器，文件 sink 用 `truncate=false` 保留已收前缀。

## 4. 最小用法（可编译示例）

```cpp
// app_demo.cpp —— 内存传输上跑一次 KV 同步
#include <iostream>
#include <utility>

#include "go2cpp/app/connection.hpp"
#include "go2cpp/app/transport.hpp"

int main() {
    using namespace go2cpp::app;

    auto pair = MemoryTransport::CreatePair();
    Connection client(std::move(pair.first), Role::kClient);
    Connection server(std::move(pair.second), Role::kServer);
    if (!client.Start() || !server.Start()) { std::cerr << "handshake failed\n"; return 1; }

    KvTable table;
    table.Set("k1", "v1");
    table.Set("k2", "v2");
    const SendResult sent = client.SendKv(table);
    if (!sent.ok() || sent.stream_id == 0) {
        std::cerr << "send failed: " << ErrorCodeName(sent.error) << '\n';
        return 1;
    }

    for (int i = 0; i < 200; ++i) {
        client.Step(1);          // 只能在同一线程调用
        server.Step(1);
        Event event;
        while (server.ReadEvent(&event, 0)) {
            if (event.type == EventType::kKvComplete) {
                std::cout << "k1=" << event.headers.Get("k1")
                          << " k2=" << event.headers.Get("k2") << '\n';
                return 0;
            }
            if (event.type == EventType::kError) { std::cerr << event.message << '\n'; return 1; }
        }
    }
    std::cerr << "timeout\n";
    return 1;
}
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGO2CPP_WITH_APP=ON
cmake --build build --target go2cpp_app -j
g++ -std=c++17 -O2 -Iinclude app_demo.cpp -Lbuild \
    -lgo2cpp_app -lgo2cpp_hash -lgo2cpp_error -lpthread -o app_demo && ./app_demo
```

换真实链路只改两行：把 `std::make_unique<BentoLinkTransport>(...)` 或 `HttpTunnelClientTransport` 传进 `Connection`；业务代码不动。

## 5. 源码位置

| 文件 | 行数 | 职责 |
|---|---:|---|
| `app/frame.hpp` / `src/app/frame.cpp` | 398 / 510 | 9 B 帧头、帧类型、流式解码、标准载荷、KV 文本/二进制 |
| `app/kv.hpp` / `src/app/kv.cpp` | 约 200 / 449 | 有序 KV 表、`k=v` 转义、二进制编码 |
| `app/stream_table.hpp` / `src/app/stream_table.cpp` | 约 330 / 1,064 | 状态机、两级窗口、依赖树 + WFQ |
| `app/transport.hpp` / `src/app/transport.cpp` | 约 160 / 176 | `ITransport` + 进程内配对传输（8 MiB 有界队列背压） |
| `app/connection.hpp` / `src/app/connection.cpp` | 391 / 1,663 | 握手、帧分发、发送调度、事件、资源/KV 发送 |
| `app/resource.hpp` / `src/app/resource.cpp` | 约 470 / 1,043 | 元数据、分块、CRC/SHA-256、续传、索引、各 Source/Sink |
| `app/kv_stream.hpp` / `src/app/kv_stream.cpp` | 约 230 / 298 | `KvProducer`、`KvAssembler`、`KvBatchCollector` |
| `app/bento_bridge.hpp` / `src/app/bento_bridge.cpp` | 约 80 / 211 | App 帧 ↔ Bento 20 B 通用帧（CRC32） |
| `app/bento_link_transport.hpp` / `src/app/bento_link_transport.cpp` | 约 70 / 149 | Bento Link STREAM 通道 → `ITransport` |
| `app/http_tunnel.hpp` / `src/app/http_tunnel.cpp` | 约 90 / 189 | 帧流 over HTTP（TCP）批式隧道 |
| `app/gateway.hpp` / `src/app/gateway.cpp` | 460 / 1,560 | 通用 HTTP 网关：注册式路由、中间件链、可插拔上游解析、JSON 配置 |
| `app/realtime_room.hpp` / `src/app/realtime_room.cpp` | 95 / 382 | 通用实时房间：房间聚合连接 + 每连接有界队列异步广播 |

`app/sha256_ni.hpp` 现在是 `hash::Sha256` 的薄转发，建议下一轮让 `resource.cpp` 直调 hash 后删除。

## 6. 性能数据

**无 asm 级实测**：`docs/reports/` 下没有 `asm-app*.md`，本模块没做过 objdump 指令级审计。只有 [app-framework.md](../../docs/modules/app-framework.md) §10 的端到端实测（WSL2、Release `-O3`、12 核、单线程驱动内存传输）：

| 指标 | 数值 | 备注 |
|---|---|---|
| 4 MiB 资源内存传输 | **21 ms / ≈190 MiB/s** | `app_test_resource`，含 SHA-256 |
| 1 GiB 资源内存传输 | **3992 ms / ≈256 MiB/s** | `app_test_load`；Pattern 源 + Discard sink，常驻 O(1)；元数据扫描 3348 ms |
| 1000 并发 KV 流 | **14 ms**，客户端 1002 帧 | 事件完整、流表清空 |
| 1 MiB 资源 over Bento Link（UDP 环回） | ≈7.7 MiB/s | 测试每 2 ms 驱动一次 `RunOnce`，属**驱动频率限制**，不是协议上限 |
| 256 KiB 资源 over HttpServer | 6 次 HTTP 请求 | 每次请求批量携带多帧 |

性能不是第一目标（**正确性/可测性优先**），这些数字用于发现回归，不作为吞吐承诺。1 GiB 那行的 3348 ms 是元数据扫描（`BuildResourceMeta` 遍历算 SHA-256），不是传输——这说明「大资源要预计算 meta」是硬性要求（§7 第 6 条）。

## 7. 边界与已知限制

1. **无 HPACK**：HEADERS 是显式 KV，重复头不压缩；资源元数据量小，收益低于复杂度。
2. **HEADERS 单帧**：不支持 CONTINUATION；超大 KV 表自动分区到 DATA，资源元数据过大则**拒绝发送**。
3. **HTTP 隧道是半双工批式**：不是长连接全双工，每次交换有 HTTP 开销；一个 `HttpTunnelServer` 绑定一个 `Connection`，不能并发处理多请求。
4. **续传的跨进程校验不完整**：新接收器只能对续传段做 SHA-256（`verified=true` 但属 partial）；要全量校验需保留原 receiver 或重新扫描已收前缀。
5. **事件队列有界（65536）**：业务必须及时消费，否则旧事件被丢弃（关闭/错误事件优先保留）；生产建议用 `SetEventHandler` 回调。
6. **`SendResource` 在 meta 无 `sha256` 时会扫描数据源**：1 GiB 源会在调用线程阻塞数秒（**持 IO 锁**）；大批量下发必须预计算 `ResourceMeta` 并复用。
7. **`ChunkSource` 读取失败会停止该资源流**：`ResourceProducer` 没有独立错误帧，读失败后生产者 done 但不发 END_STREAM；生产环境要给资源流加超时/`ResetStream`。
8. **Bento Link 适配器不自动重连**：Link 断开需业务调用 `Connection::Close`。
9. **`PRIORITY` 只作用于已存在的流**：`SetPriority` 要求流已创建，HEADERS 之前的 PRIORITY 被忽略；要提前定优先级先 `OpenStream` 再 `SendPriority`。
10. **事件里的 KV 表是拷贝**：1 MiB 的 KV 完成事件会持有 1 MiB 数据。
11. **未做 TLS**：加密/身份认证交给传输层（Bento 自带 PSK/X25519；HTTP 可部署在 TLS 网关后）。
12. **库目标未加入 `install(EXPORT)`**：`-DGO2CPP_WITH_APP=ON` 只构建库与测试，头文件随整个 `include/` 安装；需要导出时在 `Go2CppInstall.cmake` 的 `GO2CPP_MODULE_TARGETS` 追加 `go2cpp_app`。

## 8. 测试与验证方法

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DGO2CPP_WITH_APP=ON -DGO2CPP_APP_TESTS=ON
cmake --build build -j12
ctest --test-dir build -R 'app_' --output-on-failure
```

| 可执行 | 场景 | 覆盖重点 |
|---|---:|---|
| `app_test_frame` | 7 | 帧往返、半包/粘包、非法帧、KV 文本、KV 二进制 2000 轮随机 + 2000 轮 fuzz、Bento 帧桥 |
| `app_test_stream_table` | 12 | 状态机、10 流并发、WFQ 4:1、依赖树/环检测、两级窗口、并发上限、RST、GOAWAY、清理 |
| `app_test_resource` | 10 | 元数据、700 KiB 分块、坏 CRC、断点续传（单元 + 新连接）、8 资源并发、索引、空资源、8 KiB 帧、4 MiB 吞吐 |
| `app_test_kv` | 10 | 小表、文本 API、1 MiB 大值、混合、每键一流、转义/二进制、500 键分区、4 KiB 窗口流控、生产者/聚合器对拍 |
| `app_test_connection` | 11 | 握手、bytes 流、50 流、流控、取消、PING、GOAWAY、关闭、坏 preface、优先级、**回调重入** |
| `app_test_load` | 3 | 1000 并发 KV 流、1 GiB 资源（SHA-256 校验）、50 资源 + 200 KV 混合 |
| `app_test_bento` / `app_test_http` | 4 / 5 | 真实 UDP Link 与真实 TCP HttpServer 上的 KV / 资源 / 索引 / 坏 body |

自建最小闭环：① 跑 §4 的 `app_demo` 应打印 `k1=v1 k2=v2`；② 把 `MemoryTransport::CreatePair(1024)` 调到 1 KiB 以下验证背压（`WriteAll` 阻塞至超时）；③ 用 `PatternChunkSource(1ull << 30)` + `DiscardSink` 跑 1 GiB 核对 `stats.verified == true`；④ 故意改坏一块 CRC，接收端应收到 `kResourceComplete(ok=false)` 且发送端收到 `RST_STREAM(kChecksumError)`。

`gateway` / `realtime_room` 不注册 ctest，用独立脚本编译运行：`tests/app/build_and_run_gateway.sh`（路由/鉴权/透传/超时/502/中间件/Resolver/配置/压测）、`tests/app/build_and_run_realtime.sh`（同房间广播、房间隔离、慢连接踢出、Leave/CloseAll、100 连接并发广播）。

## 9. 交接注意

- **不要在 `Step` 之外的线程调 `Step`/`ReadEvent`**：这两个是单线程入口；要并发请开多条 `Connection`，不要让两个线程抢同一条。
- **`SendResult.stream_id == 0` 是失败**：调用方必须同时检查 `ok()` 与 `stream_id`，别把 0 当合法流。
- **改帧头或状态机必须同时改四处**：`frame.hpp`、`stream_table.cpp`、`connection.cpp`、[app-framework.md](../../docs/modules/app-framework.md) §3；9 B 帧头被 `FrameDecoder`、`bento_bridge`、`http_tunnel` 共同依赖。
- **加新 `ErrorCode` 要往 8 以上加**：0..7 已与 HTTP/2 对齐，改值会让日志/抓包工具误读。
- **`BuildResourceMeta` 是 O(size) 扫描**：1 GiB 会阻塞数秒且持有 IO 锁，批量下发要提前算好 meta 复用（既是性能也是正确性问题）。
- **`bento::app` 与本模块无关**：改 `bento/app/fetch` 或 `zip` 不影响 `go2cpp::app`，反之亦然；两边的「app」只是命名巧合。
- **未完成**：HPACK、CONTINUATION、TLS、`ChunkSource` 错误帧、`install(EXPORT)` 导出、`sha256_ni.hpp` 的删除（目前是薄转发）。
