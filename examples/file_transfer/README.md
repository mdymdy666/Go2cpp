# file_transfer：资源传输示例（JinShan MOD-28 对接）

用 `include/go2cpp/app/resource.hpp` 的 `ResourceTransfer` 部件演示**真实的文件
传输**：512 KiB 分块、SHA-256 内容校验、断点续传、进度与吞吐统计。

```
源文件 ──> FileChunkSource ──> ResourceProducer ──> 帧编码 ──> 通道
                                                             │
     目标文件 <── FileResourceSink <── ResourceReceiver <── 帧解码 <──┘
```

## 构建与运行

```bash
# 先确保主工程已构建（app 框架 ON）
cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_APP=ON
cmake --build .build-bento-wsl -j

# 方式一：一键脚本（g++ 直接编译，最快）
examples/file_transfer/build_and_run.sh --suite

# 方式二：独立 CMake 工程
cmake -S examples/file_transfer -B .build-file-transfer -G Ninja
cmake --build .build-file-transfer -j
./.build-file-transfer/file_transfer_demo --suite
```

常用参数：

| 参数 | 说明 |
|------|------|
| `--transport memory\|tcp` | 进程内有界字节管道 / 127.0.0.1 回环 TCP（默认 memory） |
| `--size 1G` | 载荷大小，支持 K/M/G 后缀（默认 1 GiB） |
| `--chunk-size 512K` | 分块大小（默认 512 KiB；范围 64 KiB..4 MiB） |
| `--interrupt-after N` | 发完第 N 块后关闭通道，再从第 N 块续传 |
| `--suite` | 跑 6 个边界尺寸：0 / 1 B / 511 KiB / 512 KiB / 1 MiB+7 / 8 MiB+3 |
| `--reuse` | 复用已存在且大小一致的源文件（重跑更快） |

工作目录固定为 `/tmp/go2cpp_file_transfer`（源文件与目标文件都留在这里便于复查）。

## 每次运行都验证什么

1. 目标文件大小 == 源文件大小；
2. 目标文件 SHA-256 == `ResourceMeta::sha256_hex`（源文件哈希）；
3. **逐字节比较**（4 MiB 缓冲、`pread` + `memcmp`，不是抽样）；
4. 打印发送端/接收端吞吐（MiB/s）与总用时。

## 实测数据（WSL2，Release，本机）

| 场景 | 载荷 | 吞吐（接收端） | 用时 |
|------|------|----------------|------|
| memory | 1 GiB | 530.7 MiB/s | 1.93 s |
| tcp | 1 GiB | 527.6 MiB/s | 1.94 s |
| memory + 中断续传（640/2048 块） | 1 GiB | 599.9 MiB/s（两段合计） | 1.71 s |
| tcp + 中断续传（700/2048 块） | 1 GiB | 544.1 MiB/s（两段合计） | 1.88 s |

续传场景下两段都通过 SHA-256 与逐字节比较；第一段 320 MiB / 350 MiB 后关闭通道，
第二段从断点继续且目标文件不截断。

瓶颈在接收端的块 CRC-32（`Crc32Of` 软件表驱动，约 400~600 MiB/s）与文件落盘；
发送端的 `FileChunkSource` 用 `pread`，不整体加载 1 GiB。

## 断点续传的语义（重要）

- 发送端：`ResourceProducer(meta, source, start_chunk)`，被跳过的块不读、不发；
- 接收端：`ResourceReceiver(meta, sink, start_chunk)`，`completed_chunks()` 是
  **累计**连续完成块数（构造时初始化为 `start_chunk`），可直接作为下次续传起点；
- 目标文件：`FileResourceSink(path, false)` 不截断，`pwrite` 按绝对偏移写入；
- **续传时框架跳过整资源 SHA-256 比较**（`m_partial=true`，因为前段哈希状态
  已丢失）。所以示例在续传后自己重算目标文件 SHA-256 + 逐字节比较——真实客户端
  也应如此：框架保证块级 CRC，应用层负责整文件一致性；
- 块头 `chunk_index + offset + crc32` 让接收端能拒绝跳块/重复块/重叠块（
  `OnData` 返回 false，连接层应回 `RST_STREAM(kChecksumError)`）。

## 两种传输层为什么共用一套帧

内存管道与 TCP 都只提供**字节流**语义（有界、可能半包/粘包），发送端统一
`AppendEncodedFrame`，接收端统一 `FrameDecoder`。因此：

- 线格式（9B 帧头 + 载荷）在两种模式下完全一致；
- 内存模式也能复现半包/粘包/背压问题（有界 64 MiB 队列）；
- 换成 Bento STREAM 通道或 HTTP body 时只需替换 `ByteChannel` 实现。

## 与 MOD-28 的对接点

JinShan 的 MOD-28 可以直接复用：

- `BuildResourceMeta`：一次扫描得到 size/chunks/SHA-256/CRC-32；
- `ResourceProducer` / `ResourceReceiver`：分块、块头、断点续传；
- `ResourceIndex`：多资源清单（`app.resource_index` 帧）下发；
- `ResourceMeta::ToKv/FromKv`：HEADERS 帧的元数据编解码。

需要业务自己实现的只有：连接管理、鉴权、并发调度（`stream_table` 的窗口/WFQ
已提供）和整文件级校验策略。
