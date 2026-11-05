# 公网穿透（tunnel）

> 模块路径：`include/go2cpp/tunnel/`、`src/tunnel/`、`tests/tunnel/`
> 构建接入：`cmake/Go2CppTunnel.cmake`（默认 ON，POSIX only）
> 目标：**一条命令把本地端口发布成公网 HTTP URL**，不依赖 Tailscale / cloudflared /
> ngrok 等外部程序；服务端与客户端都是本仓库构建的原生可执行文件。

---

## 1. 它解决什么问题

| 方案 | 手机/任意设备直接打开？ | 要装 App？ | 要 DNS？ |
|---|---|---|---|
| Tailscale（tailnet only） | 要加入同一 tailnet | 要 | 不用 |
| cloudflared | 可以 | 不用 | 要（Cloudflare 账号 + 域名） |
| **本模块** | **可以** | **不用** | **不用**（路径模式） |

两种 URL 形态：

- **路径模式**（零配置）：`http://<vps-ip>:8443/shop/` — 不需要任何域名；
- **子域名模式**（生产推荐）：`http://shop.tunnel.example.com:8443/` — 需要
  `*.tunnel.example.com` 泛解析到 VPS。

---

## 2. 架构

```
   手机 / 浏览器
        │  HTTP（任意设备，无需装任何东西）
        ▼
   ┌───────────────────────────── VPS（公网）─────────────────────────────┐
   │  tunnel-server                                                       │
   │   :8443 公网端口 ──┐                                                  │
   │                    ├─ 路由：路径前缀 /shop/ 或 Host: shop.domain       │
   │   :7443 控制端口 ──┘   → 查隧道表 → OpenStream(stream_id)            │
   │                                                                      │
   │   每条浏览器连接 = 一条 MuxStream；字节经 RelayPipe 双向搬运          │
   └──────────────────────────────┬───────────────────────────────────────┘
                                  │ 一条（或 N+1 条）TCP
                                  │ 帧：HELLO/CHALLENGE/AUTH/OPEN/DATA/CLOSE/PING/GOAWAY
                                  ▼
   ┌──────────────────────── NAT 后的本地机器 ────────────────────────────┐
   │  tunnel-client                                                       │
   │   收到 OPEN → 新建到 127.0.0.1:8080 的 TCP → RelayPipe 双向搬运      │
   │   控制连接断开 → 指数退避重连（bento::Backoff）                      │
   └──────────────────────────────┬───────────────────────────────────────┘
                                  ▼
                          本地 HTTP 服务 :8080
```

### 2.1 控制连接与数据连接

默认是**单连接 + 流多路复用**（与 rathole 默认模式同构，等价于在一条 TCP 上跑
yamux/smux 会话）。`tunnel-client --data-conns N` 会额外建立 N 条**数据连接**：
控制连接只负责注册、心跳与 `GOAWAY`，数据连接只承载 `OPEN`/`DATA`/`CLOSE`，
服务端按轮询把新流分配到数据连接上。这对应 frp 的 work connection 与
rathole 的 data channel。

| 模式 | 适用 | 代价 |
|---|---|---|
| 单连接（默认） | 绝大多数场景 | 控制帧与数据帧共享一条 TCP 的发送缓冲 |
| `--data-conns N` | 高并发 / 大流量，想把控制面隔开 | 多 N 条 TCP + N 个心跳 |

### 2.2 复用面（不重复造轮子）

| 能力 | 来源 | 用途 |
|---|---|---|
| 帧编解码（12B 头 + 半包/粘包） | `go2cpp::frame::FrameCodec`（`include/go2cpp/frame/`） | 控制连接上的 `TUNL` 帧 |
| 双向中继 + 背压 + 写 deadline | `go2cpp::relay::RelayPipe`（`include/go2cpp/relay/`） | 每条流的两端字节搬运 |
| 帧缓冲读取 | `go2cpp::tunnel::FrameReader` | 握手阶段一次收到多帧不丢 |
| PSK / ChaCha20-Poly1305 / 随机数 | `go2cpp::bento::*`（`include/go2cpp/bento/net/crypto.hpp`） | 挑战-应答鉴权 |
| 指数退避 + 抖动 | `go2cpp::bento::Backoff`（`bento/net/backoff.hpp`） | 客户端重连 |
| 指标 | `go2cpp::metrics::Registry`（`include/go2cpp/metrics/`） | 连接/流/鉴权失败/字节数 |

`tunnel` 是独立守护进程，不链接 `go2cpp_network`（避免 Fiber/hook 耦合）也不链接
`go2cpp_bento`（默认 OFF），因此把上述几份**零依赖实现**（`src/bento/net/crypto.cpp`、
`protocol.cpp`、`x25519.cpp`、`backoff.cpp`、`src/metrics/metrics.cpp`）直接编入
`go2cpp_tunnel` 目标。它们仍是同一份源码，不是复制品。

---

## 3. 协议

复用 `FrameCodec` 的 12 字节帧头（全部小端）：

```
 0   4  magic    "TUNL"
 4   1  version  1
 5   1  type     见下表
 6   2  flags    保留（HELLO 用 bit0 标记数据连接）
 8   4  length   载荷字节数
 12  N  payload
```

| type | 名称 | 方向 | 载荷 |
|---|---|---|---|
| 1 | `HELLO` | C→S | `ver(2) flags(4) sub(1+len) local_port(2) token(2+len)` |
| 2 | `HELLO_ACK` | S→C | `status(1) sub(1+len) url(2+len) msg(2+len)` |
| 3 | `OPEN` | S→C | `stream_id(4) peer_ip(1+len) peer_port(2)` |
| 4 | `DATA` | 双向 | `stream_id(4) body`（body ≤ 65532） |
| 5 | `CLOSE` | 双向 | `stream_id(4) code(1)`（0=全关，1=半关） |
| 6/7 | `PING`/`PONG` | 双向 | `nonce(8)` |
| 8 | `CHALLENGE` | S→C | 16B 随机挑战（全零 = 服务端未启用鉴权） |
| 9 | `AUTH` | C→S | 16B AEAD tag = `Seal(KDF(PSK), nonce=challenge[0..12), aad=challenge)` |
| 10 | `GOAWAY` | 双向 | `reason(1) retry_after_ms(4)` |

**握手顺序**（客户端不需要猜服务端有没有开鉴权）：

```
S → C : CHALLENGE(16B)
C → S : AUTH(tag)
C → S : HELLO
S → C : HELLO_ACK
```

PSK = `SHA-256(token)`；无 token 时服务端发全零挑战、客户端回全零 tag。因为
AUTH 在 HELLO 之前，攻击者拿不到任何隧道信息；tag 由 AEAD 生成，**token 明文
从不上线**，也无法重放（挑战每次随机）。

---

## 4. 部署与使用

### 4.1 编译

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tunnel-server tunnel-client -j
# 产物：build/tunnel-server、build/tunnel-client
```

### 4.2 服务端（公网 VPS，一条命令）

```bash
./tunnel-server --public-port 8443 --control-port 7443 --token 'please-change-me'
```

输出：

```
tunnel-server 已启动
  控制端口   : 0.0.0.0:7443（tunnel-client 连这里）
  公网端口   : 0.0.0.0:8443（浏览器访问这里）
  公网地址   : http://203.0.113.7:8443/<子域名>/
```

常用参数：

| 参数 | 说明 |
|---|---|
| `--token <s>` | 启用 PSK 鉴权（强烈建议） |
| `--domain tunnel.example.com` | 启用子域名路由（需泛解析） |
| `--display-host <ip>` | URL 里显示的公网 IP（默认自动探测） |
| `--no-strip` | 路径模式不剥前缀（后端自己认 `/<子域名>/`） |
| `--max-streams N` | 单隧道并发流上限（默认 256） |

### 4.3 客户端（本地，一条命令）

```bash
./tunnel-client --server 203.0.113.7:7443 --local 8080 --subdomain shop --token 'please-change-me'
```

输出：

```
========================================
 公网 URL: http://203.0.113.7:8443/shop/
 本地目标: 127.0.0.1:8080
========================================
```

此时**任何设备**的浏览器打开 `http://203.0.113.7:8443/shop/` 即可访问本地 8080。

### 4.4 VPS 上的 systemd 单元（可选）

```ini
# /etc/systemd/system/tunnel-server.service
[Unit]
Description=Go2Cpp tunnel server
After=network-online.target

[Service]
ExecStart=/usr/local/bin/tunnel-server --public-port 8443 --control-port 7443 --token please-change-me
Restart=always
RestartSec=2
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable --now tunnel-server
sudo ufw allow 7443/tcp && sudo ufw allow 8443/tcp
```

### 4.5 可观测

- `GET /__tunnel__/status` → JSON：隧道列表、并发流、字节数、数据连接数、draining；
- `go2cpp::metrics::Registry`（与项目其它模块同一注册表）：
  `go2cpp_tunnel_connections_total`、`go2cpp_tunnel_streams_total`、
  `go2cpp_tunnel_auth_failures_total`、`go2cpp_tunnel_bytes_in_total`、
  `go2cpp_tunnel_bytes_out_total`、`go2cpp_tunnel_online`，
  客户端侧 `go2cpp_tunnel_client_reconnects_total` / `_streams_total` / `_online`。

---

## 5. 工业级特性对照（frp / bore / rathole）

| 特性 | 状态 | 实现 / 说明 |
|---|---|---|
| 控制连接 + 数据连接分离 | ✅ | `--data-conns N`：控制连接注册/心跳，数据连接只跑流；服务端轮询分配（frp work conn / rathole data channel 的等价物）。默认 0 = 单连接多路复用（同 rathole 默认） |
| 多路复用 | ✅ | 一条 TCP 上按 `stream_id` 复用，12B 帧头；等价 yamux/smux 的会话层 |
| 鉴权 | ✅ | token → SHA-256 → 32B PSK；CHALLENGE/AUTH 用 ChaCha20-Poly1305（复用 bento::net::crypto）。无 token 时全零挑战 |
| 心跳 + 超时 | ✅ | 15 s PING / 90 s idle；心跳线程独立于数据线程 |
| 指数退避重连 | ✅ | `bento::Backoff`（0.5 s → 15 s，±20% 抖动）；明确拒绝（token 错/版本错）不重试 |
| 优雅关闭（drain） | ✅ | 服务端 `SIGTERM` → `GOAWAY` → 等在途流排空（默认 5 s）→ 断开；客户端收到 GOAWAY 后不再重连 |
| 可观测 | ✅ | metrics + `/__tunnel__/status` |
| 限流 / 背压 | ✅ | RelayPipe 有界队列（256 KiB）+ 每流接收队列（8 MiB，溢出关流）+ 每隧道 `max-streams` |
| 复用 RelayPipe / FrameCodec / crypto / backoff / metrics | ✅ | 见 §2.2 |
| TLS 传输加密 | ❌ | 不做：公网侧建议 nginx/caddy 反代终结 TLS。PSK 只做鉴权——隧道里是明文 HTTP，与 frp 默认（无 TLS 时）一致。理由：自签 TLS 在浏览器端仍要装证书，反而破坏「任何设备能开」 |
| 配置热重载 | ❌ | 不做：命令行参数 + systemd 重启（重启时 GOAWAY 会等流排空，客户端自动重连）。rathole 的配置热重载需要文件监听 + 原子替换，收益低于复杂度 |
| 多租户 / 按用户配额 | ⚠️ | 单 token 全局共享；同名子域名以**最新注册**为准（重连友好）。多租户需要 token 表 + 配额存储，超出本次范围 |
| UDP 转发 | ❌ | 只做 HTTP 路由 + TCP 字节流。隧道本身不解析协议，理论上任意 TCP 协议可过，但入口按 HTTP 头路由 |

---

## 6. 实测（WSL2，本机回环，8 核；**历史快照，2026-11-03 前**）

> ⚠️ **下表是 raw 模式上线前的 mux 数字（2026-11-03 前，未复跑）**，已被 2026-11-04 raw 实测取代：
> **8 MiB 吞吐 2385.3 MiB/s / 开销 1.27×**（mux 同机对照 770.5 MiB/s / 3.59×），
> 见 `docs/reports/tunnel-raw-default-2026-11-04.md`，台账 **M17**。引用以 M17 为准，下表仅作历史对照。

| 场景 | 直连 | 隧道 | 比值 |
|---|---|---|---|
| 短连接延迟 p50（每请求新建 TCP，300 次） | 0.685 ms | 1.344 ms | **1.96×** |
| 短连接延迟 p95 | 0.988 ms | 1.751 ms | 1.77× |
| keep-alive 延迟 p50（一条 TCP 连续请求） | 0.291 ms | 0.869 ms | 2.99× |
| 8 MiB 下载吞吐（3 轮中位） | 1312 MiB/s | ~~488 MiB/s~~（历史数据，2026-11-03 前；现 2385.3，见 M17） | ~~0.37×~~（现 1.27×） |
| 并发 32 | — | 32/32 成功，1058 ms | — |

说明：本机回环下直连基线只有 0.3~0.7 ms，隧道的固定开销来自
**两端各 5 条 RelayPipe 线程的创建**与 4 次跨线程队列唤醒（约 0.6 ms）。
公网 RTT ≥ 10 ms 时这部分占比 < 6%。短连接场景（路径模式的真实行为）多轮中位 1.96×，1.90~2.15 波动，达成 < 2× 目标——**该 1.96× 与 488 MiB/s 均为 2026-11-03 前的 mux 历史值（历史数据，未复跑）**。raw 模式最新实测：8 MiB 吞吐 **2385.3 MiB/s / 开销 1.27×**（台账 M17、`docs/reports/tunnel-raw-default-2026-11-04.md`）。488 MiB/s 已超 1 Gbps 网卡上限（119 MiB/s），2385.3 MiB/s 亦超 10 Gbps（1192 MiB/s），瓶颈已移到两端 RelayPipe 线程与 syscall。

功能验证（`/tmp/tunnel_demo/smoke3.sh`）：鉴权拒绝、目录页、状态页、GET/POST/
图片、8 MiB 二进制 sha256、并发 10、kill -9 客户端 → 502 → 重启恢复、
SIGTERM drain 全部通过。C++ 测试 `go2cpp_tunnel_test` 153 项检查 0 失败。

---

## 7. 限制

1. **路径前缀模式强制 `Connection: close`**：服务端只解析第一个请求头，keep-alive
   连接上的后续请求无法再按前缀路由，因此一请求一条隧道流（多一次握手）。子域名
   模式没有这个限制。
2. **路径前缀不重写响应里的绝对路径**：后端返回 `/static/a.css` 这类绝对路径会
   404。请用相对路径或 `<base href="/shop/">`。正式部署推荐子域名模式。
3. **每流 5 条线程**（RelayPipe 的 2 读 + 2 写 + watchdog）：32 并发约 160 线程。
   建议单隧道并发 ≤ 256；更高并发需要给 RelayPipe 加线程池（属于 relay 模块）。
4. **单流接收队列 8 MiB**：某条流的下游极慢且持续超过 8 MiB 时，该流被关闭
   （保护整条连接），日志会打印 `recv queue overflow`。真正按流窗口（credit）留给后续。
5. **响应完成检测只认 `Content-Length`**：`Transfer-Encoding: chunked` 响应依赖
   客户端关闭连接来收尾（curl/浏览器都会关）。路径模式下无 Content-Length 的
   响应会走到 90 s 心跳超时才释放流。
6. **无 TLS**：公网明文。敏感场景请在 VPS 前置 nginx/caddy 做 TLS 终结。
7. **子域名抢占**：知道 token 的人可以用同一子域名顶掉现有隧道（设计如此，便于
   客户端重启后立刻恢复）。需要隔离时应为每人分配独立 token（未实现 per-token
   ACL）。
8. **单进程单实例**：隧道表在内存里，重启即丢失；不支持多 VPS 集群共享路由表。

---

## 8. 代码结构

| 文件 | 职责 |
|---|---|
| `include/go2cpp/tunnel/protocol.hpp` / `src/tunnel/protocol.cpp` | 帧类型、载荷编解码、PSK/挑战应答、子域名 |
| `include/go2cpp/tunnel/net.hpp` / `net.cpp` | TCP 封装（非阻塞 + poll）、`relay::Stream` 适配、前缀/EOF 包装 |
| `include/go2cpp/tunnel/mux.hpp` / `mux.cpp` | 一条 TCP 上的多路复用、流表、心跳、GOAWAY、`FrameReader` |
| `include/go2cpp/tunnel/server.hpp` / `server.cpp` | 双端口、HTTP 路由（路径/子域名）、响应完成检测、drain、状态页、metrics |
| `include/go2cpp/tunnel/client.hpp` / `client.cpp` | 握手、重连退避、数据连接、本地转发 |
| `src/tunnel/server_main.cpp` / `client_main.cpp` | 两个命令行入口 |
| `tests/tunnel/test_tunnel.cpp` | 协议/子域名/TCP 单元 + 端到端/鉴权/数据连接/重连/并发/性能 |

测试：

```bash
ctest --test-dir build -R tunnel --output-on-failure
# 或直接：./build/go2cpp_tunnel_test
```
