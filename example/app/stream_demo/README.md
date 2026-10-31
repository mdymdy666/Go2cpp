# stream_demo —— 流表框架：传一个文件 + 一张 k=v 表

用 `MemoryTransport` 把两个 `Connection` 背靠背连起来，**不需要端口**，但
帧编解码、握手、流状态机、连接/流两级窗口、WFQ 优先级调度、资源分块与
SHA-256 校验全部走真实路径：

```
client Connection ──► ITransport（内存管道）──► server Connection
   SendResource(权重 32)                          FileResourceSink
   SendKv(大表, 16)                               KvAssembler
   SendKv(小表, 8)
```

## 演示点

1. **ResourceTransfer**：`FileChunkSource` 按 128 KiB 分块，
   `BuildResourceMeta` 计算整资源 SHA-256 与块数；服务端用
   `FileResourceSink` 落盘，`ResourceReceiver` 校验每块 CRC-32 与整资源
   SHA-256；
2. **KvStream**：一张 64 KiB 的大表（`payload` 超过 4 KiB 阈值）自动走
   「HEADERS + DATA」流式模式；一张 5 对的小表走单 HEADERS 模式；
3. **流表**：同一连接上 3 条流（resource 32 / kv 16 / kv 8）由流表按
   依赖树 + 权重调度；资源块大于初始流窗口（65535），因此每个块被切成
   3 个 DATA 帧，靠服务端的 WINDOW_UPDATE 续传；
4. **校验闭环**：传输结束后示例自己读回落盘文件，重新计算 SHA-256 与
   元数据比对，任何不一致都以非零退出码结束。

## 构建与运行

```sh
# 仓库根目录
cmake -S . -B .build-bento-wsl -DGO2CPP_BUILD_EXAMPLES=ON
cmake --build .build-bento-wsl --target go2cpp_stream_demo -j8

# 自动生成 1 MiB 输入文件并传输
./.build-bento-wsl/example/app/go2cpp_stream_demo

# 传自己的文件（任意大小）
./.build-bento-wsl/example/app/go2cpp_stream_demo --file /path/to/asset.pak

# 调整参数
./.build-bento-wsl/example/app/go2cpp_stream_demo \
    --workdir /tmp/go2cpp_stream_demo --kv-bytes 262144 --chunk-size 262144
```

`go2cpp_stream_demo` 只在 `-DGO2CPP_WITH_APP=ON` 时构建（该模块默认 OFF）。

## 预期输出（2026-10-31 实测）

```text
[stream_demo] 已生成输入文件 /tmp/go2cpp_stream_demo/input.bin（1 MiB）
[stream_demo] 工作目录 /tmp/go2cpp_stream_demo
[stream_demo] 输入 /tmp/go2cpp_stream_demo/input.bin（1048576 字节）
[stream_demo] 资源元数据：chunk_size=131072 chunks=8 sha256=b9a56eb486a3f0de...
[stream_demo] 已发起：资源流 #1、大 KV 流 #3、小 KV 流 #5
[server] KV 流 #5 完成：keys=5（小表，单 HEADERS 帧）
[server] KV 流 #3 完成：keys=4 name=stream_demo payload=65536 字节
[server] 资源流 #1 完成：ok=1 bytes=1048576 chunks=8 sha256=b9a56eb486a3f0de... 吞吐=55.6 MiB/s
[client] 统计：帧=31/23 载荷字节=1114864/114 flush=29 流控阻塞=0 WINDOW_UPDATE=0 资源流=1 KV流=2
[client] 流表：
  #1   resource  状态=half_closed_local 权重=32 发送=1048864 字节/24 帧
[server] 连接关闭
[stream_demo] 输出 /tmp/go2cpp_stream_demo/output.bin（1048576 字节）sha256=b9a56eb486a3f0de...
[stream_demo] 摘要：资源 1048576 字节 / 8 块，KV 大表 65536 字节 + 小表 5 键，总耗时 20 ms，接收吞吐 55.6 MiB/s
STREAM_OK
```

读法：

- **24 帧 / 8 块**：128 KiB 的块 > 初始流窗口 65535，每块被切成 3 个 DATA
  帧（块头 12B + 数据），共 24 帧；服务端 `auto_window_update` 在消费后
  立即归还窗口，所以本次没有出现「窗口不足阻塞」（`流控阻塞=0`）；
- **1 048 864 字节** = 1 MiB 数据 + 8 块 × 12 B 块头；KV 的 65 536 字节
  `payload` 走 DATA 帧，不占 HEADERS；
- **小 KV 先完成**：它只有一帧，虽然权重最低（8），但一帧就能发完；资源
  流权重最高（32），在剩余帧的调度中占优；
- **`#1` 之外看不到 #3/#5**：KV 流完成即被连接清理，流表只保留资源流
  （`half_closed_local` 表示本端已发 END_STREAM，等对端确认后关闭）；
- 输出文件与输入文件的 SHA-256 相同，`STREAM_OK` 表示校验通过。

## 参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--file` | 空 | 要传的文件；空则自动生成 1 MiB 确定性文件 |
| `--workdir` | `/tmp/go2cpp_stream_demo` | 输入/输出文件目录（会自动创建） |
| `--kv-bytes` | `65536` | 大 KV 值的字节数；> 4096 触发流式模式 |
| `--chunk-size` | `131072` | 资源块大小，范围 64 KiB..4 MiB |

## 边界与取舍

- 传输用 `MemoryTransport`（进程内管道），便于无端口自检；真实部署把
  `ITransport` 换成 `bento_link_transport`（UDP 可靠+不可靠）或
  `http_tunnel`（TCP），`Connection` 代码不变；
- 只演示「一个资源 + 两张 KV 表」；断点续传 API（`start_chunk` /
  `completed_chunks()`）与资源索引（`ResourceIndex`）没有在示例里展开；
- 单线程泵（交替 `Step(0)`）是为了让事件顺序确定；生产用独立 IO 线程；
- 事件回调在 `Step` 线程同步执行，示例用无锁的局部状态收集结果；
- 不做压缩/加密，SHA-256 只用于完整性校验。
