# 公网穿透 Tunnel（交接文档）

> **定位**：把 NAT 后的本地 HTTP 端口发布成公网 URL，服务端/客户端都是本仓库构建的原生可执行文件。
> **源码**：`include/go2cpp/tunnel/`、`src/tunnel/`　**构建**：`cmake/Go2CppTunnel.cmake`（默认 ON，仅 POSIX）
> **测试**：`tests/tunnel/test_tunnel.cpp`（目标 `go2cpp_tunnel_test`）
> **设计全文**：[docs/modules/tunnel.md](../../docs/modules/tunnel.md)（协议表、部署、对照 frp/rathole）

## 1. 这个模块是什么

内网机器通常没有公网 IP，手机/外部浏览器无法直接访问它上面的 HTTP 服务。
Tunnel 在公网 VPS 上跑 `tunnel-server`、在内网机器上跑 `tunnel-client`，
两者之间用一条（或多条）TCP 长连接做**流级多路复用**，把公网请求搬运到本地端口。

它解决的是「演示 / 联调 / 临时对外」场景：**一条命令换一个公网 URL**，不装
Tailscale、不用 cloudflared 账号、不配 DNS。两种 URL 形态：

- **路径模式**（零配置）：`http://<vps-ip>:8443/shop/`；
- **子域名模式**（生产推荐）：`http://shop.tunnel.example.com:8443/`，需要 `*.tunnel.example.com` 泛解析。

能力边界一句话：**只做 HTTP 路由 + TCP 字节流搬运**，不做 TLS 终结、不做 UDP 转发、
不做多租户配额（见 §7）。

## 2. 架构 / 数据流

```
   手机 / 浏览器（任意设备，无需装任何东西）
        │  HTTP
        ▼
   ┌──────────────────────── VPS（公网）────────────────────────┐
   │ tunnel-server                                              │
   │   :8443 公网端口 ─┐                                         │
   │                   ├─ 路由：路径前缀 /shop/ 或 Host: shop.*   │
   │   :7443 控制端口 ─┘   → 隧道表 → OpenStream(stream_id)      │
   │                                                            │
   │   每条浏览器连接 = 一条 MuxStream；字节经 RelayPipe 双向搬运 │
   └──────────────────────────┬─────────────────────────────────┘
                              │ 一条（或 1+N 条）TCP
                              │ 帧：HELLO/CHALLENGE/AUTH/OPEN/DATA/CLOSE/PING/GOAWAY
                              ▼
   ┌──────────────────── NAT 后的本地机器 ───────────────────────┐
   │ tunnel-client                                              │
   │   收到 OPEN → 连 127.0.0.1:8080 → RelayPipe 双向搬运        │
   │   控制连接断开 → bento::Backoff 指数退避重连                │
   └──────────────────────────┬─────────────────────────────────┘
                              ▼
                      本地 HTTP 服务 :8080
```

### 2.1 控制面与数据面

默认**单连接 + 流多路复用**（与 rathole 默认模式同构）。`--data-conns N` 会额外
建立 N 条只承载 `OPEN/DATA/CLOSE` 的数据连接，控制连接只负责注册、心跳与
`GOAWAY`；服务端按轮询把新流分配到数据连接。这对应 frp 的 work connection。

| 模式 | 适用 | 代价 |
|---|---|---|
| 单连接（默认） | 绝大多数场景 | 控制帧与数据帧共享同一条 TCP 的发送缓冲 |
| `--data-conns N` | 高并发/大流量，控制面要与数据面隔离 | 多 N 条 TCP + N 个心跳 |

### 2.2 线程模型（交接重点）

- **服务端**：2 条 accept 循环（控制口 + 公网口）；每条控制连接 1 条等待线程；
  每条公网连接 1 条 RelayPipe（外加 MuxSession 自己的 2 条）。
  **线程数随在线隧道数与并发请求数增长**，不是固定线程池。
- **客户端**：1 条主循环（连接 → 注册 → 断开 → 退避重连）；`data_conns` 条数据
  连接线程；每条流一个转发线程（`HandleOpen` 里起，立即返回）。
- **心跳**：服务端 15 s PING、90 s 无入站帧判定死亡；客户端同样 15 s/90 s。
- **drain**：`SIGTERM` → `BeginDrain()` 发 `GOAWAY` → 等在途流结束（默认 5 s）→ 断开。

### 2.3 复用了什么（不重复造轮子）

| 能力 | 来源 | 用途 |
|---|---|---|
| 帧编解码（12 B 头 + 半包/粘包） | `go2cpp::frame::FrameCodec` | 控制连接的 `TUNL` 帧 |
| 双向中继 + 背压 + 写 deadline | `go2cpp::relay::RelayPipe` | 每条流的字节搬运 |
| PSK / ChaCha20-Poly1305 | `bento::net::crypto` | CHALLENGE/AUTH |
| 指数退避 + 抖动 | `bento::Backoff` | 客户端重连 |
| 指标注册表 | `go2cpp::metrics::Registry` | 连接/流/鉴权失败/字节数 |

注意：tunnel **是独立守护进程**，不链接 `go2cpp_network`（避免 Fiber/hook 耦合），
也不链接 `go2cpp_bento`（默认 OFF）。它把 `src/bento/net/{crypto,protocol,x25519,backoff}.cpp`
与 `src/metrics/metrics.cpp` 直接编进 `go2cpp_tunnel` 目标——**同一份源码，不是拷贝**。

## 3. 对外 API（关键接口）

### 3.1 服务端

```cpp
struct TunnelServerConfig {
    std::string control_host = "0.0.0.0";   // 控制口监听地址
    std::uint16_t control_port = 7443;      // 0 = 内核分配（测试用）
    std::string public_host = "0.0.0.0";
    std::uint16_t public_port = 8443;       // 0 = 内核分配
    std::string display_host;               // URL 里展示的 host；空 = 自动探测
    std::string domain;                     // 泛域名后缀；空 = 只用路径前缀
    std::string token;                      // 空 = 不鉴权
    std::size_t max_streams_per_tunnel = 256;
    std::size_t recv_queue_cap = 8U * 1024U * 1024U;
    int idle_timeout_ms = 90000;
    int ping_interval_ms = 15000;
    bool strip_prefix = true;               // 路径模式是否剥掉前缀
    bool with_checksum = false;             // 帧尾 CRC（两端一致）
    int accept_timeout_ms = 500;
    int drain_timeout_ms = 5000;
};

class TunnelServer {
public:
    bool Start(ErrorPtr* err);              // 绑定两个端口 + 起 accept 线程
    void Stop();                            // 断开所有隧道，join 全部线程
    std::uint16_t control_port() const;     // 实际端口（port=0 时有用）
    std::uint16_t public_port() const;
    std::vector<TunnelSnapshot> tunnels() const;
    std::string PublicUrlFor(const std::string& subdomain) const;
    void BeginDrain();                      // 优雅关闭：GOAWAY + 等在途流
    bool draining() const;
    bool auth_required() const;
    std::uint64_t connections_total() const;
    static std::string DetectLocalAddress(); // UDP connect 取源地址（不发包）
    void set_event_handler(EventHandler);    // 上线/下线/错误；在任意工作线程调用
};
```

### 3.2 客户端

```cpp
struct TunnelClientConfig {
    std::string server_host = "127.0.0.1";
    std::uint16_t server_port = 7443;
    std::string local_host = "127.0.0.1";
    std::uint16_t local_port = 8080;
    std::string subdomain;                  // 空 = 服务端随机分配
    std::string token;
    int dial_timeout_ms = 5000;
    int reconnect_min_ms = 500;             // 退避下限
    int reconnect_max_ms = 15000;           // 退避上限
    std::size_t recv_queue_cap = 8U * 1024U * 1024U;
    int idle_timeout_ms = 90000;
    int ping_interval_ms = 15000;
    std::size_t max_local_streams = 128;    // 超出直接回 503
    bool with_checksum = false;
    int hello_timeout_ms = 10000;
    int data_conns = 0;                     // >0 时额外建 N 条数据连接
    double reconnect_jitter = 0.2;
};

class TunnelClient {
public:
    bool Start(ErrorPtr* err);              // 起后台连接线程，立即返回
    void Stop();                            // 置停 + join；返回后无线程访问本对象
    bool WaitOnline(int timeout_ms);        // 等首次注册成功
    bool online() const;
    std::string public_url() const;         // 未上线为空
    std::string subdomain() const;
    TunnelClientStats Stats() const;        // reconnects/streams/bytes/url
    bool server_draining() const;
    int data_connections() const;
    void set_event_handler(EventHandler);
};
```

### 3.3 协议帧（12 字节小端头）

```
 0   4  magic "TUNL"   4   1  version(1)   5   1  type
 6   2  flags          8   4  length       12  N  payload
```

`type`：1 HELLO / 2 HELLO_ACK / 3 OPEN / 4 DATA（body ≤ 65532）/ 5 CLOSE（code 0=全关 1=半关）
/ 6 PING / 7 PONG / 8 CHALLENGE（16 B 随机数，全零=服务端未启用鉴权）/ 9 AUTH（16 B AEAD tag）
/ 10 GOAWAY（reason + retry_after_ms）。

**握手顺序**：`S→C CHALLENGE` → `C→S AUTH` → `C→S HELLO` → `S→C HELLO_ACK`。
PSK = `SHA-256(token)`；tag = `Seal(KDF(PSK), nonce=challenge[0..12), aad=challenge)`。
token 明文从不上线，挑战每次随机所以无法重放。

## 4. 最小用法

### 4.1 编译

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tunnel-server tunnel-client -j
# 产物固定：build/tunnel-server、build/tunnel-client
```

### 4.2 两个终端跑起来

```bash
# 终端 1（公网 VPS 或本机）：服务端
./build/tunnel-server --public-port 8443 --control-port 7443 --token 'please-change-me'

# 终端 2（本地）：客户端，发布本地 8080
./build/tunnel-client --server 127.0.0.1:7443 --local 8080 --subdomain shop \
                      --token 'please-change-me'
# 输出 "公网 URL: http://127.0.0.1:8443/shop/"

# 终端 3：验证
curl -v http://127.0.0.1:8443/shop/
```

### 4.3 嵌入式 C++ 示例（客户端）

```cpp
#include "go2cpp/tunnel/client.hpp"

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    go2cpp::tunnel::TunnelClientConfig cfg;
    cfg.server_host = "203.0.113.7";   // VPS 公网 IP
    cfg.server_port = 7443;
    cfg.local_port  = 8080;
    cfg.subdomain   = "shop";
    cfg.token       = "please-change-me";

    go2cpp::tunnel::TunnelClient client(cfg);
    go2cpp::ErrorPtr err;
    if (!client.Start(&err)) {
        std::cerr << "start failed: " << (err ? err->Message() : "unknown") << "\n";
        return 1;
    }
    if (!client.WaitOnline(5000)) {
        std::cerr << "not online (token/subdomain?)\n";
        client.Stop();
        return 1;
    }
    std::cout << "public url: " << client.public_url() << "\n";
    std::this_thread::sleep_for(std::chrono::seconds(60));
    client.Stop();   // 返回后不会再有工作线程访问 client
    return 0;
}
```

链接：`target_link_libraries(app PRIVATE go2cpp::tunnel)`；头文件路径 `include/` 已由目标导出。

## 5. 源码位置

| 文件 | 行数 | 职责 |
|---|---:|---|
| `include/go2cpp/tunnel/protocol.hpp` / `src/tunnel/protocol.cpp` | 310 / 650 | 帧类型、载荷编解码、PSK/挑战应答、子域名校验 |
| `include/go2cpp/tunnel/net.hpp` / `src/tunnel/net.cpp` | 264 / 625 | TCP 封装（非阻塞 + poll）、`relay::Stream` 适配、前缀/EOF 包装 |
| `include/go2cpp/tunnel/mux.hpp` / `src/tunnel/mux.cpp` | 320 / 639 | 一条 TCP 上的多路复用、流表、心跳、GOAWAY、`FrameReader` |
| `include/go2cpp/tunnel/server.hpp` / `src/tunnel/server.cpp` | 269 / 1365 | 双端口、HTTP 路由、响应完成检测、drain、状态页、metrics |
| `include/go2cpp/tunnel/client.hpp` / `src/tunnel/client.cpp` | 216 / 522 | 握手、重连退避、数据连接、本地转发 |
| `src/tunnel/server_main.cpp` / `client_main.cpp` | 214 / 247 | 两个命令行入口（参数解析、信号处理） |
| `tests/tunnel/test_tunnel.cpp` | — | 协议/子域名/TCP 单元 + 端到端/鉴权/数据连接/重连/并发/性能 |

构建脚本 `cmake/Go2CppTunnel.cmake` 定义了 `go2cpp_tunnel` 静态库、
`tunnel-server`/`tunnel-client` 两个可执行与 `go2cpp_tunnel_test`。

## 6. 性能数据

实测（WSL2 本机回环，8 核），来源 [docs/modules/tunnel.md](../../docs/modules/tunnel.md) §6：

| 场景 | 直连 | 隧道 | 比值 |
|---|---|---|---|
| 短连接延迟 p50（每请求新建 TCP，300 次） | 0.685 ms | 1.344 ms | **1.96×** |
| 短连接延迟 p95 | 0.988 ms | 1.751 ms | 1.77× |
| keep-alive 延迟 p50 | 0.291 ms | 0.869 ms | 2.99× |
| 8 MiB 下载吞吐（3 轮中位） | 1312 MiB/s | 488 MiB/s | 0.37× |
| 并发 32 | — | 32/32 成功，1058 ms | — |

解读：固定开销主要来自**两端各 5 条 RelayPipe 线程的创建**与 4 次跨线程队列唤醒
（约 0.6 ms）。公网 RTT ≥ 10 ms 时占比 < 6%。吞吐 488 MiB/s 已高于 1 Gbps 网卡上限
（119 MiB/s）。本模块**没有做 objdump/汇编级审计**，因为瓶颈在系统调用与线程调度，
不在指令数。

## 7. 边界与已知限制

1. **路径前缀模式强制 `Connection: close`**：服务端只解析第一个请求头，keep-alive
   上的后续请求无法再按前缀路由。子域名模式无此限制。
2. **路径前缀不重写响应里的绝对路径**：后端返回 `/static/a.css` 会 404；用相对路径
   或 `<base href="/shop/">`，正式部署推荐子域名模式。
3. **每流 5 条线程**：32 并发约 160 线程；建议单隧道并发 ≤ 256，更高并发需要给
   RelayPipe 加线程池（属于 relay 模块，未做）。
4. **单流接收队列 8 MiB**：下游极慢且持续超过上限时该流被关闭（保护整条连接），
   日志打印 `recv queue overflow`。
5. **响应完成检测只认 `Content-Length`**：chunked 响应依赖客户端关连接收尾；
   路径模式下无 Content-Length 的响应会走到 90 s 心跳超时。
6. **无 TLS**：隧道里是明文 HTTP；敏感场景在 VPS 前置 nginx/caddy 终结 TLS。
   设计理由：自签证书在浏览器端仍要装证书，会破坏「任何设备能开」。
7. **子域名可抢占**：知道 token 的人能用同一子域名顶掉现有隧道（便于客户端重启恢复）。
   需要隔离时要 per-token ACL（未实现）。
8. **单进程单实例**：隧道表在内存，重启即丢失；不支持多 VPS 集群共享路由。
9. **仅 POSIX**：Windows 上 CMake 直接跳过（socket/poll 语义不同）。

## 8. 测试与验证方法

```bash
# 单测（协议/单元/端到端/性能，153 项检查）
ctest --test-dir build -R tunnel --output-on-failure
./build/go2cpp_tunnel_test

# 只验协议层
./build/go2cpp_tunnel_test --filter protocol
```

自建最小闭环（不依赖公网）：

1. `python3 -m http.server 8080` 起本地服务；
2. `./build/tunnel-server --public-port 18443 --control-port 17443 --token t`；
3. `./build/tunnel-client --server 127.0.0.1:17443 --local 8080 --subdomain shop --token t`；
4. `curl -v http://127.0.0.1:18443/shop/` 应返回目录列表；
5. 错误 token 的客户端应 `WaitOnline` 超时，公网口返回 404（测试里已覆盖）。

观测：`curl http://127.0.0.1:18443/__tunnel__/status` 返回隧道列表/流数/字节数/数据连接数/draining。

## 9. 交接注意

- **先看协议再改代码**：帧头 12 B 与握手顺序被 `frame::FrameCodec`、`mux`、`server`、
  `client` 四处共同依赖；改 type 必须同时改四端与 `docs/modules/tunnel.md`。
- **错误 token 不重试**：服务端明确拒绝（token 错/版本错）时客户端不进入退避重试，
  避免刷日志；改重连逻辑时别把这条丢掉。
- **Stop() 的时序**：`TunnelClient::Stop()` 返回后保证没有工作线程再访问对象；
  在回调里调 `Stop()` 会自 join 死锁——回调必须快速返回。
- **`with_checksum` 两端必须一致**，默认关；打开会多 4 B/帧。
- **端口 0 是测试入口**：`control_port/public_port = 0` 让内核分配，`Start()` 后用
  `control_port()` 取实际值——写集成测试时用这个，别硬编码端口。
- **不要链 `go2cpp_bento`**：tunnel 特意只编入 bento 的零依赖 .cpp；新增依赖会破坏
  「独立守护进程」定位，也会把 Fiber/hook 带进来。
- **未完成事项**：TLS 终结、per-token 配额、UDP 转发、多 VPS 路由共享、按流 credit 窗口。
  这些在 [docs/modules/tunnel.md](../../docs/modules/tunnel.md) §5 有逐条对照。
