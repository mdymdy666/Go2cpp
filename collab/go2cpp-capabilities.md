# Go2Cpp 基础能力清单（供 JinShanProject 参考）

> 由 Go2Cpp 维护，放在 JinShan 仓库供参考。**选择性复用**——不强制。
> Go2Cpp 定位：通用层（并发/网络/缓存/存储/错误/Go API），不做远程桌面专属逻辑。

## 一、已交付（可直接链接，按 JinShan 需求）

| 能力 | JinShan 模块 | Go2Cpp 位置 | 关键数据 |
|---|---|---|---|
| **FrameCodec**（帧编解码 + 拆包） | MOD-10 帧编解码 | `include/go2cpp/frame/frame_codec.hpp` | 4.53 Mframes/s；12B 帧头 + CRC；半包/粘包 |
| **RelayPipe**（双向字节中继 + 背压） | MOD-15 字节转发与背压 | `include/go2cpp/relay/relay_pipe.hpp` | 1e8 双向 330-428 MiB/s；慢 peer 只断自身 |
| **Hub**（连接注册表 key→多连接） | MOD-14 会话表与路由 | `include/go2cpp/relay/hub.hpp` | 10.47 Mops/s；CloseKey 锁外关闭 |
| **Lease/Heartbeat**（租约 + 心跳） | MOD-11 心跳与超时 | `include/go2cpp/relay/lease.hpp` | 心跳 P99 0ms；单调时钟；只触发一次 |
| **有界队列**（Chan + TrySend） | 背压 | `include/go2cpp/concurrency/channel.hpp` | 1e8 无丢失 25-28 Mmsg/s |

## 二、可直接复用（Go2Cpp 已有，JinShan 不用自己写）

| 能力 | JinShan 模块 | Go2Cpp 位置 | 说明 |
|---|---|---|---|
| **错误/日志** | MOD-01 日志与错误码 | `include/go2cpp/error/**`、`logging/**` | Error/Wrap/Is/As；分级日志 |
| **配置** | MOD-02 配置加载 | `include/go2cpp/config/**` | INI/JSON 读取 + 默认值 |
| **工具**（路径/编码/哈希/时间/字符串） | MOD-03 通用工具 | `include/go2cpp/tools/**`、`encoding/**`、`hash/**` | SHA-256（SHA-NI 2.29 GB/s）、Base64、时间 |
| **线程/定时器** | MOD-04 线程与异步 | `include/go2cpp/concurrency/**` | Fiber/Scheduler/CV/Timer/Future |
| **加密** | MOD-05 密钥管理 | `include/go2cpp/bento/net/crypto.hpp` | X25519/ChaCha20-Poly1305/PSK |
| **传输** | MOD-07/08 传输 | `include/go2cpp/network/socket.hpp` | TCP/UDP；TCP_NODELAY；批量 sendmmsg |
| **退避** | MOD-12 重连退避 | `include/go2cpp/bento/net/backoff.hpp` | 指数退避 + 抖动 |
| **资源传输** | MOD-28 文件传输 | `include/go2cpp/app/resource.hpp` | 512KiB 分块/SHA-256/断点续传（1779 MiB/s） |
| **HTTP 客户端** | MOD-27/31 下载/上传 | `include/go2cpp/network/http.hpp` | keep-alive/分块编码 |
| **zip** | MOD-31 备份打包 | `include/go2cpp/bento/app/zip.hpp` | store-only zip 读写 |
| **Go 风格 API** | 全体（让 Go 代码易迁移） | `include/go2cpp/go/**` | net/sync/chan/context/error/time/io |

## 三、可外包但需新增（Go2Cpp 可做，JinShan 未提）

| 能力 | 用途 | 状态 |
|---|---|---|
| **日志脱敏**（key/token 不落日志） | MOD-01 要求 | 未做（小改动） |
| **MJPEG AVI 容器** | MOD-32 会话录屏 | 媒体编码有，AVI 容器未做 |
| **文件传输示例** | MOD-28 对接 | ResourceTransfer 有，示例未写 |

## 四、专属（JinShan 自己做，Go2Cpp 不做）

```
MOD-06 Win7 兼容层（Windows API）
MOD-13 配对协议（RDRL 语义）
MOD-16 Go 中转服务
MOD-17-24 媒体/输入（IRDPSRAPI/Viewer）
MOD-25 虚拟通道
MOD-29 远程终端（Job Object）
MOD-32 录屏（Viewer 帧）
MOD-33 提权重启
MOD-34 AI 辅助
MOD-35/36 Win32 UI/托盘
MOD-37 差环境检测
MOD-39 会话编排
```

## 五、使用方式

```cmake
# 链接（示例）
target_link_libraries(jinshan_relay PRIVATE go2cpp_frame go2cpp_relay)
```

```cpp
// FrameCodec 示例
go2cpp::frame::FrameCodec codec(spec);   // spec: magic="RDRL", max_payload=65536
std::vector<go2cpp::frame::Frame> frames;
size_t consumed = 0;
codec.Feed(buf.data(), buf.size(), &frames, &consumed, &err);
```

## 六、约束

```
① 不引入第三方库（Go2Cpp 约束；仅 OpenSSL 可选、SQLite 可选）
② 平台：Linux（WSL2）+ Windows（Linux-free 模块 100% 可在 MinGW 编译）
③ 性能：并发 ns 级、网络 ≤1.1× Go、数据面 ≥10 GB/s
④ 语义：与 Go 标准库对齐（阻塞/超时/取消/边界）
```

## 七、当前状态（2026-11-01）

| 模块 | 状态 |
|---|---|
| 并发（9 原语） | ✅ 1e9×3 压测通过；调度器 189ns |
| 网络（6 协议） | ✅ 14/14 ≤1.1× Go |
| 缓存（7 策略） | ✅ 几何 3.56×；堆分配→0 |
| P2P（STUN/打洞/TURN） | ✅ 4/4 测试 |
| WebRTC（媒体/SFU/信令/弱网） | ✅ 端到端逐像素 0 差异 |
| 存储（SQLite/LSM） | ✅ SQLite 封装 4.0% 开销；LSM 在研 |
| Go 风格 API | ✅ 8 头 2145 行 |
| 协作（本文件） | ✅ |
