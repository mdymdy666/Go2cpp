# Bento 路线图（2026-10-30 起，修订）

> 2026-10-29 晚修订：**暂不做** QoS/多协议适配（网络传输零散、价值分散）。
> 明天改为：**需求文档 + 多模型套接字框架**（复用 base TcpSocket/UdpSocket，
> 并发层决定 fd 走 GMP 还是阻塞），完成后可做成 MCP 插件。

## 阶段 A：需求文档（明天上午）

`docs/requirements/socket-framework.md`：
- 目标：一套 Socket API，三种并发模型（GMP / 阻塞 / epoll）；
- 约束：复用 base `TcpSocket`/`UdpSocket`；不引入第三方；Linux；
- 对 AI 友好：MCP 暴露底层（connect/read/write/close/多路复用）。

## 阶段 B：多模型套接字框架（明天主任务）

### 抽象

```
bento::net::socket::
  Socket          // 统一句柄（TCP/UDP），内部 fd
  ConcurrencyMode // Gmp | Blocking | Poll
  SocketConfig { mode, timeout, buffer, reuse_addr, nodelay }

  Listen/Accept/Connect/Read/Write/Close
  // 同一 API 在三种模式下行为一致（阻塞语义由运行时实现）
```

### 三种模型

| 模型 | fd 归属 | 阻塞语义 |
|---|---|---|
| **Gmp**（Fiber） | 注册到 epoll（IO 等待）→ Fiber park；写就绪回调唤醒 | 调用看起来阻塞，实际 park Fiber，M 去跑别的 G |
| **Blocking**（线程） | 直接阻塞 fd（fcntl 阻塞模式） | 线程真阻塞（简单/兼容） |
| **Poll**（显式） | 用户调用 Poll/Select 等待就绪，再 Read/Write | 非阻塞 + 用户循环 |

### 运行时选择

- `SocketConfig.mode` 显式指定；
- 或 `Auto`：检测当前上下文（是否在受管 Fiber 内）→ Gmp；否则 Blocking；
- 切换成本：fd 的 O_NONBLOCK 标志切换（Gmp/Poll 需要非阻塞，Blocking 需要阻塞）。

### 复用

- base `TcpSocket`/`UdpSocket`（`include/go2cpp/network/`）——封装其 fd；
- base `IOManager`（epoll）——Gmp 模式的 IO 等待；
- base `Fiber`/`Scheduler`——Gmp 模式的 park/wake。

## 阶段 C：MCP 插件（明天下午/后续）

- 把 Socket 框架暴露为 MCP 工具：
  - `socket_connect/listen/accept/read/write/close`；
  - `socket_poll`（多路复用）；
  - `socket_set_mode`（切换 GMP/Blocking）；
- AI 通过 MCP 直接操作底层网络（「AI 绝对掌握底层」）；
- 风险：权限/沙箱/审计——工具只允许白名单地址/端口。

## 不做（本次）

- QoS 抽象层（QosProfile/QosManager）——推迟；
- WS/HTTP/SSE/DDS/Raft 适配——推迟；
- JinShan 协议（token/RDRL/RDCH）——记录在 `jinshan-protocol-gaps.md`，等合并时再做。

## 规则

- 每个模块：实现 → 压测/随机/5 场景 → audit 文档 → 三遍审查；
- 0 警告 + ctest 全过；不引入第三方库；基础模块不动（留给用户）。
