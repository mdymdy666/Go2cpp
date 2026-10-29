# Bento 路线图（2026-10-30 起，二次修订）

> 2026-10-29 晚二次修订：明天**不追求 NetServer 完整开发**（开发不完）。
> 明天聚焦：**并发/网络基础模块检查 → TcpSocket/UdpSocket Debug 模式 → MCP 插件**。
> NetServer（万能网络适配层）推迟到基础夯实后。

## 明天（2026-10-30）任务

### 1. 并发 + 网络基础模块检查（主）

| 模块 | 检查内容 |
|---|---|
| `concurrency/scheduler` | 压测（10 万 G 调度）、随机（取消/超时/关闭）、5 场景（长稳/高并发/嵌套）、解耦 |
| `concurrency/fiber` | 栈增长/切换开销/泄漏、上下文丢失边界 |
| `concurrency/channel` | 10 万消息、随机 close/send 竞态、select 公平性 |
| `concurrency/sync` | Mutex/CV/WaitGroup/RWMutex 压测、竞态（TSan） |
| `concurrency/timer` | 10 万定时器、精度/漂移、取消竞态 |
| `network/tcp_socket` | 连接/读写压测、超时/错误映射、fd 泄漏 |
| `network/udp_socket` | 收发压测、数据报边界、端口复用 |
| `network/io`（epoll） | 多路复用压测、边缘触发边界 |

> 基础模块是用户后续要自己审的——明天只做**压测 + Debug 模式接入**（不深挖重构）。

### 2. TcpSocket / UdpSocket Debug 模式（次主）

目标：给 socket 加「可观测」能力，接入现有 debug 模块：

```
SocketDebug {
  Enable(true)                  // 打开调试（计数/事件/采样）
  Snapshot() -> SocketStats     // 连接数/收发字节/错误/延迟分布
  Events() -> 事件流            // connect/accept/read/write/close/timeout
  DumpTo(debug::View&)          // 接入 debug 视图（10 views 之一）
}
```

- 统计：连接/字节/错误/超时/RTT（可选）；
- 事件：结构化事件（接入 debug 的 event ring）；
- 开销：Debug 关闭时近零（条件分支/原子计数）；
- 测试：压测对比（Debug on/off 开销）、事件完整性。

### 3. MCP 插件（让 AI 掌握底层）

- MCP 工具（基于 debug 模式）：
  - `socket_list()` → 所有 socket 状态；
  - `socket_stats(fd)` → 统计；
  - `socket_events(fd, n)` → 最近事件；
  - `socket_close(fd)` → 关闭（受审计）；
- 白名单 + 审计（AI 操作可追溯）；
- 测试：MCP 工具端到端（起 socket → 查询 → 关闭）。

## 推迟（不做）

- **NetServer 万Network适配层**（协议嗅探/路由/三并发模型）——基础夯实后再做；
- QoS 抽象层；
- WS/HTTP/SSE/DDS/Raft 适配；
- JinShan token/RDRL（记录在 `jinshan-protocol-gaps.md`）。

## 规则

- 每个模块：压测/随机/5 场景 → audit 文档 → 三遍审查；
- 0 警告 + ctest 全过；不引入第三方库。
