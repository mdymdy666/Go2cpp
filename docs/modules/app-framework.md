# 应用层框架（app）：流表 / 资源传输 / KV 流

> 位置：`include/go2cpp/app/**`、`src/app/**`、`tests/app/**`
> 目标：把「一次游戏资源下发」和「一张 `k1=v1;...;k5=v5` 表」统一抽象成
> **HTTP/2 风格的流表**，并给出可直接跑的传输适配、测试与性能数据。
> 构建开关：`-DGO2CPP_WITH_APP=ON`（默认 OFF，不影响既有模块）。

---

## 1. 定位与总体结构

```
                ┌──────────────────────────────────────────────────────┐
  业务层         │ KvStream（小表/大表/每键一流）  ResourceTransfer（块/校验/续传）│
                └───────────────┬──────────────────────┬───────────────┘
                                │   HEADERS / DATA     │
                ┌───────────────▼──────────────────────▼───────────────┐
  流表层         │ Connection：握手、帧分发、发送调度、事件、流控归还        │
                │ StreamTable：状态机 + 连接/流窗口 + 依赖树 WFQ          │
                └───────────────┬──────────────────────────────────────┘
                                │ 9B 帧（length/type/flags/stream_id）
                ┌───────────────▼──────────────────────────────────────┐
  传输层         │ ITransport：Memory / Bento Link(STREAM) / HTTP 隧道     │
                └──────────────────────────────────────────────────────┘
```

**分层原则**：流表不知道字节从哪来，传输层不知道帧的含义，业务层不碰窗口与
优先级。三者可以分别测试：`StreamTable` 是纯逻辑，`Connection` 只依赖
`ITransport`，资源/KV 只依赖 `Connection` 的公开 API。

### 文件清单

| 文件 | 职责 |
|---|---|
| `include/go2cpp/app/frame.hpp` / `src/app/frame.cpp` | 9B 帧头、帧类型、流式解码、标准载荷 |
| `include/go2cpp/app/kv.hpp` / `src/app/kv.cpp` | 有序 KV 表、文本 `k=v` 与二进制编码 |
| `include/go2cpp/app/stream_table.hpp` / `.cpp` | 流状态机、两级窗口、依赖树 + WFQ 调度 |
| `include/go2cpp/app/transport.hpp` / `.cpp` | `ITransport` 抽象 + 进程内配对传输 |
| `include/go2cpp/app/connection.hpp` / `.cpp` | 握手、帧分发、发送调度、事件、资源/KV 发送 |
| `include/go2cpp/app/resource.hpp` / `.cpp` | 资源元数据、分块、SHA-256/CRC、续传、索引 |
| `include/go2cpp/app/kv_stream.hpp` / `.cpp` | KV 发送源、接收聚合、每键一流收集器 |
| `include/go2cpp/app/bento_bridge.hpp` / `.cpp` | App 帧 ↔ Bento 20B 通用帧（CRC32） |
| `include/go2cpp/app/bento_link_transport.hpp` / `.cpp` | Bento Link STREAM 通道 → `ITransport` |
| `include/go2cpp/app/http_tunnel.hpp` / `.cpp` | 帧流 over HTTP（TCP）批式隧道 |

---

## 2. 与 HTTP/2（RFC 7540）的对比

| 维度 | HTTP/2 | 本框架 | 说明 |
|---|---|---|---|
| 帧头 | 9B：length(24)+type(8)+flags(8)+R+stream_id(31) | **相同** | 直接复用 RFC 布局，便于有 HTTP/2 经验的读者 |
| 帧类型 | DATA/HEADERS/PRIORITY/RST_STREAM/SETTINGS/PUSH_PROMISE/PING/GOAWAY/WINDOW_UPDATE/CONTINUATION | DATA/HEADERS/PRIORITY/RST_STREAM/SETTINGS/PING/GOAWAY/WINDOW_UPDATE + **RESOURCE_INDEX** | 去掉推送与 CONTINUATION |
| 头部编码 | HPACK（静态/动态表 + Huffman） | **显式 KV 二进制**（varint 长度前缀） | 资源元数据是内部协议，不需要 HTTP 语义；省掉 HPACK 状态机与压缩炸弹风险 |
| 流状态 | idle/reserved/open/half-closed/closed | idle/open/half_closed_local/half_closed_remote/closed | 无 reserved（不支持推送） |
| 流控 | 连接窗口 + 流窗口，WINDOW_UPDATE | **相同** | 初始窗口/帧大小经 SETTINGS 协商 |
| 优先级 | 依赖树 + 权重（后来 RFC 9113 弃用） | **严格父优先 + WFQ**（依赖树 + 权重） | 保留依赖语义，但用虚拟完成时间做加权公平，行为可预测、可单测 |
| 头部续帧 | HEADERS + CONTINUATION | HEADERS 单帧；超限自动把条目分区到 DATA | 避免「header block 跨帧」状态机 |
| 服务端推送 | PUSH_PROMISE | 不支持 | 资源下发用普通流即可 |
| 双工 | 客户端/服务端都可发起流 | **相同**（client 奇数、server 偶数） | `SendKv` 双向均可 |
| 关闭确认 | 响应 END_STREAM | 接收完成回空 DATA(END_STREAM) | 让单向推送流能进入 closed 并清理 |
| 加密 | TLS（RFC 7540 要求） | 不内置；由传输负责 | Bento Link 自带 PSK/X25519；HTTP 走 TLS 由部署决定 |
| 默认帧大小 | 16 KiB | **256 KiB**（可配置，上限 16 MiB） | 资源块传输需要更大载荷；解码前按长度拒绝超限帧 |

**一句话**：帧头/状态机/窗口与 HTTP/2 同构，头部编码与优先级调度按「资源分发」
场景重新取舍，业务语义（资源块、KV 模式、续传）作为 HEADERS 里的普通键值，
不引入新的协议层。

---

## 3. 线格式

### 3.1 帧头

```
 0                   1                   2                   3
+-------------------------------+--------+--------+---------------+
|         length (24)           | type   | flags  |R| stream_id   |
+-------------------------------+--------+--------+  (31 bits)    |
|                          payload (length bytes)                |
+----------------------------------------------------------------+
```

- 所有整数**网络字节序（大端）**；
- `R` 位必须为 0，否则帧解码失败；
- 单帧载荷默认上限 1 MiB（`kDefaultMaxFramePayload`），协议上限 16 MiB；
- 连接层默认把发送帧限制在 256 KiB（`ConnectionConfig::max_frame_payload`）。

### 3.2 帧类型与标准载荷

| 类型 | 值 | 载荷 |
|---|---|---|
| DATA | 0x00 | 原始字节 / 资源块（12B 块头 + 数据） |
| HEADERS | 0x01 | `KvTable` 二进制编码（含 `app.kind` 等） |
| PRIORITY | 0x02 | `parent_id(4) + weight(2)` |
| RST_STREAM | 0x03 | `error_code(4)` |
| SETTINGS | 0x04 | `count(2) + N*(id(2), value(4))`；ACK 时为空 |
| PING | 0x06 | 8B opaque；ACK 置 `FLAG_ACK` |
| GOAWAY | 0x07 | `last_stream_id(4) + error(4) + debug(varint+bytes)` |
| WINDOW_UPDATE | 0x08 | `increment(4，31 位，非 0)` |
| RESOURCE_INDEX | 0x0d | `ResourceIndex` 的 KvTable 二进制（资源清单） |

标志位：`END_STREAM=0x01`、`ACK=0x02`、`END_HEADERS=0x04`、`RESUME=0x08`。

### 3.3 KV 载荷

`KvTable` 二进制：

```
varint count
count * { varint key_len, key, varint value_len, value }
```

文本形式 `k1=v1;k2=v2` 用反斜杠转义 `\`、`;`、`=`，供 `SendKvText` 与人工调试。

### 3.4 资源块（DATA 载荷）

```
+---------------+----------------+----------------+--------------------+
| chunk_index   | offset         | crc32          | data (N bytes)     |
| u32           | u32            | u32            |                    |
+---------------+----------------+----------------+--------------------+
```

- 一个块可以跨多个 DATA 帧（受流窗口/帧上限约束），`offset` 保证接收端能检测
  不连续/重复；
- `crc32` 是**整块**的 CRC-32/IEEE；接收端凑齐整块后先校验、再落盘；
- 块大小默认 512 KiB（64 KiB..4 MiB），由 `ResourceMeta::chunk_size` 决定。

---

## 4. 流表

### 4.1 状态机

```
 idle ──send/recv HEADERS──> open ──send ES──> half_closed_local
   │                           │                     │
   │                           └──recv ES──> half_closed_remote
   │                                                 │
   └──────────── RST_STREAM / 双向 END ─────────────> closed
```

- 非法转换**不修改状态**，返回 false（连接层据此 RST/GOAWAY）；
- `closed` 之后所有操作返回 false；
- 接收端完成 KV/资源后发一个**空 DATA(END_STREAM)** 作为关闭确认，发送端收到后
  进入 closed 并清理流表条目（避免单向推送流永久占用槽位）。

### 4.2 两级窗口

- **连接窗口**（stream 0）：所有流共享，默认 1 MiB；
- **流窗口**：每条流独立，默认 64 KiB；
- 发送 DATA 前必须满足 `min(连接窗口, 流窗口) >= 载荷长度`；
- 接收 DATA 时两级同时扣减；框架消费后调用 `AcknowledgeReceived` /
  `AcknowledgeConnection`，累计到窗口一半时生成 WINDOW_UPDATE；
- 窗口增量为 0 或让窗口超过 `2^31-1` 都是流控错误（GOAWAY/关闭）。

### 4.3 优先级：依赖树 + WFQ

每条流有 `parent_id` 和 `weight(1..256)`：

1. **严格父优先**：若某流的祖先当前也有可发送帧且窗口允许，子流本轮不参与
   调度；
2. **加权公平**：其余候选按虚拟完成时间选择——
   `finish += 已发字节 * 2^20 / weight`，每次选 `finish` 最小者。长期字节数
   之比收敛到权重之比（WFQ），全程整数运算；
3. **环检测**：把 A 挂到 A 的后代下会被拒绝；父不存在/未知流也拒绝；
4. 删除父流后子流自动上浮到根，不会悬挂。

> 与 HTTP/2 的差异：HTTP/2 的依赖树允许「父节点关闭后权重继承」等复杂规则，
> 且被 RFC 9113 标记为弃用。本实现保留依赖树与权重，但把调度简化成
> 「父优先 + WFQ」，可预测、可单测；`PRIORITY` 帧与 `SetPriority` 更新的是
> 同一份依赖关系。

### 4.4 并发与关闭

- `max_concurrent_streams` 限制**未关闭**的流数量（含刚创建未发 HEADERS 的
  idle 流），超过时 `CreateLocal` 失败；
- `max_retired_streams` 限制已关闭流的保留数量，超出后按关闭时间从旧到新
  清理（为迟到帧保留一段容忍期）；
- `GOAWAY` 后本端不再创建新流（`SendKv` 返回 `kRefusedStream`），已有流可继续
  完成；
- 收到 `RST_STREAM` 立即进入 closed，后续帧被忽略。

### 4.5 发送源（FrameProducer）

流表不保存待发数据，只保存一个生产者：

```cpp
class FrameProducer {
 public:
  virtual bool Produce(std::uint32_t max_payload, Frame* out) = 0;
  virtual bool done() const = 0;
};
```

- 首帧约定为 HEADERS（不占 DATA 窗口）；后续 DATA 按 `max_payload` 生成；
- 连接层在窗口允许时调用 `Produce`，把结果放进 `Stream::pending`；
- 1 GiB 资源不需要全部驻留内存，断点续传也可从任意块开始；
- 窗口不足时连接层**不生产**新帧（避免生产出永远发不出去的大帧）。

---

## 5. 连接层

### 5.1 握手

```
Client: "APP2\r\n\r\n" + SETTINGS
Server: "APP2\r\n\r\n" + SETTINGS
双方:   收到 SETTINGS -> SETTINGS ACK
```

- preface 固定 8 字节，双方都发；作用是在字节流上明确协议归属（误接到别的端口
  会在前 8 字节失败，而不是把垃圾当帧继续解析）；
- SETTINGS 协商：并发上限、初始流窗口、单帧上限、HEADERS 上限、小 KV 阈值、
  资源块大小；
- `ready()` = 收到对端 preface + SETTINGS。

### 5.2 线程模型

- `Step()` / `ReadEvent()` 是 IO 驱动入口，**只能由一个 IO 线程/Fiber 调用**；
- `SendKv*` / `SendResource*` / `SendBytes` / `ResetStream` 等发送 API 可在任意
  线程调用（内部一把 `recursive_mutex` 串行化流表）；
- 事件回调在 `Step` 线程同步执行，可以**重入发送 API**（同一线程递归锁），
  测试 `EventHandlerReentrancy` 覆盖；
- 事件队列有独立锁，`ReadEvent` 等待时释放 IO 锁。

### 5.3 发送调度

每次 `Step` 的发送阶段：

1. 先发挂起的 WINDOW_UPDATE（连接级/流级）；
2. 循环 `PickNext`：选流 → 取 pending 帧 → 检查窗口 → 写传输 → 扣窗口 →
   推进状态 → 扣 WFQ 信用；
3. 单次 Step 的发送预算 `max_bytes_per_flush`（默认 2 MiB），避免发送饿死
   接收与定时器；
4. 发送 `END_STREAM` 后若流已双向关闭则清理。

### 5.4 接收分发与事件

| 事件 | 触发 |
|---|---|
| `kKvComplete` | KV 流结束（`headers` 是合并后的业务表） |
| `kResourceComplete` | 资源流结束（`resource_stats.verified/bytes/sha256`） |
| `kResourceIndex` | 收到资源索引 |
| `kHeaders` / `kData` / `kEndStream` | `kBytes`/控制流的元数据/裸数据/结束 |
| `kReset` / `kGoAway` / `kPing` / `kSettings` / `kWindowUpdate` | 控制帧 |
| `kClosed` / `kError` | 关闭 / 协议错误 |

资源与 KV 的 DATA 由框架内部消费（缓冲/校验/落盘），不会逐帧投递事件；`kBytes`
流才逐帧交给业务。

---

## 6. 资源传输

### 6.1 元数据（HEADERS，k=v）

```
app.kind=resource
app.name=hero/body.v1.pak
app.size=104857600
app.sha256=<64 hex>
app.chunk_size=524288
app.chunks=200
app.crc32=3735928559
app.version=1
app.resume=0            # 断点续传起始块（0/缺省=从头）
x.asset_type=mesh       # 业务自定义键
```

`ResourceMeta::ToKv/FromKv` 负责编解码与校验（chunk 数必须与 size/chunk_size
一致、hash 必须是 64 hex、resume 不得越界）。

### 6.2 发送/接收

```cpp
MemoryChunkSource source(data);
ResourceMeta meta = BuildResourceMeta("hero.pak", source, 512 * 1024);
conn.SendResource(meta, std::make_shared<MemoryChunkSource>(data));

// 接收端（可选落盘）
server.SetResourceSinkFactory([](const ResourceMeta& meta) {
    return std::make_unique<FileResourceSink>("/data/" + meta.name);
});
```

- `BuildResourceMeta` 扫描一遍数据源计算 SHA-256、CRC-32、块数；也可以由调用方
  直接提供 meta（1 GiB 场景避免二次扫描）；
- 发送端按块读取、计算块 CRC、按 `max_payload-12` 切帧；最后一块置
  `END_STREAM`；
- 接收端逐帧校验 index/offset 连续性，凑齐整块后先校验 CRC 再写 sink，并增量
  更新 SHA-256；`Finish` 校验块数/大小/整资源 SHA-256；
- 失败：块 CRC 不符 → `RST_STREAM(kChecksumError)` + `kResourceComplete(ok=false)`；
  整资源 hash 不符 → `Finish` 失败，已写数据由业务决定去留；
- 内存边界：接收端只保留「当前块」缓冲（≤4 MiB）+ SHA-256 状态，不缓存整资源。

### 6.3 断点续传

- 接收端 `ResourceReceiver::completed_chunks()` 给出连续完成的块数；
- 发送端 `SendResource(meta, source, start_chunk=N)` 从第 N 块开始，HEADERS 里
  自动带 `app.resume=N`；
- 接收端从 `app.resume` 创建 `ResourceReceiver(meta, sink, start_chunk)`，文件
  sink 可用 `FileResourceSink(path, /*truncate=*/false)` 保留已收前缀；
- 续传时只对后半段做 SHA-256（`stats.verified=true` 但属于 partial 校验），
  块级 CRC 仍然逐块校验；同进程内保留原 receiver 则可得到完整全量校验。

### 6.4 资源索引

`ResourceIndex` = 多个 `ResourceMeta`，编码成一张 k=v 表（`count`、`r0.name`…），
用 `kResourceIndex` 帧在 stream 0 上发送。客户端先拿索引，再决定下载哪些资源。

---

## 7. KV 流

### 7.1 三种承载模式

| 模式 | 线格式 | 适用 |
|---|---|---|
| `single` | 单个 HEADERS + END_STREAM | ≤ 阈值（默认 4 KiB）的元数据/命令 |
| `stream` | HEADERS（小值）+ 若干 DATA（大值子表）+ END_STREAM | 大字段/清单/配置 |
| `perkey` | 每个 k=v 一条独立流 | 需要独立取消/优先级/并发的键 |

HEADERS 里带 `app.kind=kv`、`app.kv.mode`、`app.kv.keys`、`app.kv.partial`。

### 7.2 自动分区

- 值长度 ≤ `small_value_limit`（默认 4 KiB）进 HEADERS，更大的进 DATA；
- 若 HEADERS 二进制仍超过单帧上限，生产者按「值从大到小」继续把条目移入 DATA，
  并为保留键预留 256 B；
- 500 个键在 4 KiB 帧下会自动分区，业务代码不需要手动分块；
- 接收端 `KvAssembler` 合并 HEADERS 与 DATA 子表，过滤 `app.*` 保留键后投递
  `kKvComplete`。

### 7.3 每键一流

`SendKvPerKey` 返回每条流的 id；接收端每条流一个 `kKvComplete`，
`KvBatchCollector(expected)` 负责合并。适合「一个大表拆成可独立取消的多个键」
（例如资源清单里每个资源一个键）。

### 7.4 示例

```cpp
// 小表：单帧
conn.SendKvText("k1=v1;k2=v2;k3=v3;k4=v4;k5=v5");

// 大表：自动分区
KvTable table;
table.Set("name", "manifest");
table.Set("payload", std::string(1024 * 1024, 'x'));
conn.SendKv(table);

// 每键一流
std::vector<std::uint32_t> ids;
conn.SendKvPerKey(table, &ids);
```

---

## 8. 传输适配

### 8.1 MemoryTransport

一对带条件变量的内存管道，每方向默认 8 MiB 有界队列；`WriteAll` 在队列满时
背压等待，`Close` 幂等并唤醒所有等待者。用于全部确定性测试。

### 8.2 Bento Link（UDP 可靠+不可靠）

`BentoLinkTransport` 把 Link 的 STREAM 通道（可靠有序、自动分片）适配成
`ITransport`：

- `WriteAll` 按 32 KiB 切消息，发送窗口满时短暂退避重试；
- `Install()` 包装 `Link::on_stream`，把同通道数据追加到接收队列，并链式调用
  原有回调；
- 接收线程跑 `Link::RunOnce`，Connection 在自己的线程 `Step`，二者通过队列 +
  条件变量同步；
- 不接管 Link 生命周期；生产环境可在 `on_disconnected` 里关闭 Connection。

### 8.3 Bento 通用帧桥

`bento_bridge` 把 App 帧装进 Bento 的 20B 帧（magic/version/type/flags/len/
CRC32），`type=kAppFrameType`：

```cpp
auto bytes = bento_bridge::Encode(frame);   // 20B Bento 头 + App 帧
bento_bridge::Decoder decoder;              // 流式，处理粘包/半包
```

适合「复用 Bento 帧的消息边界 + CRC，但不需要 Link 会话」的场景。

### 8.4 HTTP 隧道（TCP）

`HttpTunnelClientTransport` + `HttpTunnelServer` 把帧流塞进 HTTP body：

- 客户端 `Read` 时若没有待处理响应，把当前待发帧 POST 出去；
- 服务端 handler 调 `HandleRequest(body)`，驱动服务端 `Connection` 处理并返回
  响应帧；
- 适合资源下发、KV 同步等请求-响应场景；半双工、有 HTTP 请求开销；
- 一个 `HttpTunnelServer` 绑定一个 Connection，不可并发处理多个请求。

---

## 9. API 速览

```cpp
using namespace go2cpp::app;

// 1) 建立连接（任意 ITransport）
auto pair = MemoryTransport::CreatePair();
Connection client(std::move(pair.first), Role::kClient);
Connection server(std::move(pair.second), Role::kServer,
                  ConnectionConfig{.table = {.initial_stream_window = 65535}});

client.Start();
server.Start();

// 2) 发送 KV（自动小表/大表）
client.SendKv(KvTable{{"k1", "v1"}, {"k2", "v2"}});

// 3) 发送资源
MemoryChunkSource source(data);
ResourceMeta meta = BuildResourceMeta("hero.pak", source);
client.SendResource(meta, std::make_shared<MemoryChunkSource>(data));

// 4) 驱动与接收
server.SetResourceSinkFactory([](const ResourceMeta& meta) {
    return std::make_unique<FileResourceSink>("/data/" + meta.name);
});
while (!done) {
    client.Step(1);
    server.Step(1);
    Event event;
    while (server.ReadEvent(&event, 0)) { /* kKvComplete / kResourceComplete ... */ }
}
```

发送 API 返回 `SendResult{error, stream_id, message}`；`stream_id=0` 表示失败路径
没有分配流。

---

## 10. 测试与实测数据

运行：

```bash
cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_APP=ON -DGO2CPP_APP_TESTS=ON
ninja -C .build-bento-wsl app_test_frame app_test_stream_table app_test_resource \
      app_test_kv app_test_connection app_test_load app_test_bento app_test_http
ctest --test-dir .build-bento-wsl -R 'app_' --output-on-failure
```

| 可执行 | 场景数 | 覆盖 |
|---|---|---|
| `app_test_frame` | 7 | 帧往返、流式半包/粘包、非法帧、标准载荷、KV 文本、KV 二进制 2000 轮随机 + 2000 轮 fuzz、Bento 帧桥 |
| `app_test_stream_table` | 12 | 状态机、10 流并发、WFQ 4:1、依赖树/环检测、流窗口、连接窗口、并发上限、RST、GOAWAY、初始窗口、清理、计数 |
| `app_test_resource` | 10 | 元数据、700 KiB 分块、坏 CRC、断点续传（单元 + 新连接）、8 资源并发、索引、空资源、8 KiB 帧、4 MiB 吞吐 |
| `app_test_kv` | 10 | 小表、文本 API、1 MiB 大值、混合、每键一流、转义/二进制、500 键分区、4 KiB 窗口流控、生产者/聚合器对拍、双向 |
| `app_test_connection` | 11 | 握手、bytes 流、50 流、流控、取消、PING、GOAWAY、关闭、坏 preface、优先级、回调重入 |
| `app_test_load` | 3 | 1000 并发 KV 流、1 GiB 资源（SHA-256 校验）、50 资源 + 200 KV 混合 |
| `app_test_bento` | 4 | 真实 UDP Link 上的 KV、1 MiB 资源、服务端反向 KV、Bento 帧桥 |
| `app_test_http` | 5 | 真实 TCP HttpServer 上的 KV、256 KiB 资源、索引、坏 body、300 KiB KV |

实测（WSL2，Release `-O3`，本机 12 核，单线程驱动内存传输）：

| 指标 | 数值 | 备注 |
|---|---|---|
| 4 MiB 资源内存传输 | **21 ms / ≈190 MiB/s** | `app_test_resource`（含 SHA-256） |
| 1 GiB 资源内存传输 | **3992 ms / ≈256 MiB/s** | `app_test_load`；Pattern 源 + Discard sink，常驻内存 O(1)；元数据扫描 3348 ms |
| 1000 并发 KV 流 | **14 ms**，客户端 1002 帧 | 事件完整、流表清空 |
| 1 MiB 资源 over Bento Link（UDP 环回） | ≈7.7 MiB/s | 测试每 2 ms 驱动一次 `RunOnce`，属驱动频率限制，不是协议上限 |
| 256 KiB 资源 over HttpServer | 6 次 HTTP 请求 | 每次请求批量携带多帧 |

> 性能不是本模块第一目标（正确性/可测性优先）；上面的数字用于发现回归，
> 不作为吞吐承诺。

---

## 11. 已知限制

1. **无 HPACK**：HEADERS 载荷是显式 KV，重复头不压缩；资源元数据量小，收益
   低于复杂度。
2. **HEADERS 单帧**：不支持 CONTINUATION；超大表会自动分区到 DATA（KV），资源
   元数据过大则拒绝发送。
3. **HTTP 隧道是半双工批式**：不是长连接全双工；每次交换有 HTTP 开销，不适合
   实时指令流。
4. **资源断点续传的跨进程校验**：新接收器只能对续传段做 SHA-256；要全量校验需
   保留原接收器或重新扫描已收前缀。
5. **事件队列有界**（65536）：业务必须及时消费，否则旧事件被丢弃（关闭/错误
   事件优先保留）。生产环境建议用 `SetEventHandler` 回调而非队列。
6. **Bento Link 适配器不自动重连**；Link 断开需业务调用 `Connection::Close`。
7. **HTTP 隧道实例非线程安全**：一个实例绑定一个 Connection，多 worker 并发
   请求需按会话创建多个实例。
8. **未做 TLS**：加密/身份认证交给传输层（Bento 自带 PSK/X25519；HTTP 可部署
   在 TLS 网关后）。
9. **PRIORITY 只作用于已存在的流**：HTTP/2 允许对 idle 流建立依赖；本实现的
   `SetPriority` 要求流已创建，HEADERS 之前收到的 PRIORITY 会被忽略（流仍以
   默认权重打开）。需要提前定优先级时，先 `OpenStream` 再 `SendPriority`。
10. **`ChunkSource` 读取失败会停止该资源流**：`ResourceProducer` 没有独立的
    错误帧，读失败后生产者 done 但不会发 END_STREAM；生产环境应给资源流加
    超时/`ResetStream`，或保证源可靠。
11. **`SendResource` 在 meta 无 `sha256` 时会扫描数据源**：1 GiB 源会在调用线程
    阻塞数秒（持 IO 锁）；大批量下发建议预计算 `ResourceMeta` 并复用。
12. **事件中的 KV 表是拷贝**：1 MiB 的 KV 完成事件会持有 1 MiB 数据；事件队列
    有界（65536），业务应及时消费或改用回调。
13. **库目标未加入 `install(EXPORT)`**：与 CLI/MCP 类似，`-DGO2CPP_WITH_APP=ON`
    只构建库与测试；头文件随整个 `include/` 安装。需要安装导出时可在
    `Go2CppInstall.cmake` 的 `GO2CPP_MODULE_TARGETS` 中追加 `go2cpp_app`。

---

## 12. 与既有模块的边界

- **不改** `include/go2cpp/network/http2/**`：现有 HTTP/2 实现保持原样；本模块
  是独立的应用层框架，不替代也不依赖它。
- 依赖 `go2cpp::hash`（SHA-256/CRC-32）；Bento 目标存在时追加帧桥/Link 适配，
  HttpServer 目标存在时追加 HTTP 隧道；核心库可单独构建。
- 安装面：默认不改变既有导出集合；头文件随整个 `include/` 目录安装。
