# Bento 路线图（2026-10-30 起，三次修订）

> 2026-10-29 晚三次修订：明天任务分工明确——
> **任务 1 = 检查**（并发/网络基础模块）、**任务 2 = 调研**（Socket Debug 模式方案）、
> **任务 3 = 开发**（MCP 插件）。

## 明天（2026-10-30）任务

### 任务 1：并发 + 网络基础模块检查（检查）

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

> 基础模块是用户后续要自己审的——本任务只做**压测 + 结论**（不深挖重构）。

### 任务 2：TcpSocket / UdpSocket Debug 模式（调研，不写实现）

**目标**：输出「Socket Debug 模式」调研报告（`docs/research/socket-debug-mode.md`），包含：

1. **现状调研**：
   - base debug 模块能力（10 视图/事件环/trace/replay）；
   - tcp_socket/udp_socket 现有可观测点（统计/错误码/内部状态）；
   - 已有 socket 调试手段（strace/eBPF/现有 monitor）。
2. **方案对比**（≥3 个）：
   - A. 自研 SocketDebug（计数/事件/快照）——开销/侵入性；
   - B. 复用 debug 事件环（socket 事件注入）——统一视图；
   - C. eBPF 无侵入（monitor 已有）——零侵入但依赖内核；
   - D. 混合（A+B）。
3. **推荐方案**：接口草案（`SocketDebug::Enable/Snapshot/Events/DumpTo`）、开销目标（Debug 关闭近零）、风险。
4. **验证计划**（若采纳）：压测对比/事件完整性/长稳。

> 本任务**只产出调研报告**，不写实现代码（实现留给后续开发任务）。

### 任务 3：MCP 插件（开发）

- 目标：把「Socket 可观测」暴露为 MCP 工具，让 AI 掌握底层网络；
- 工具（基于任务 2 调研结论，若 Debug 模式不可用则用现有 monitor/统计）：
  - `socket_list()` / `socket_stats(fd)` / `socket_events(fd, n)` / `socket_close(fd)`；
- 白名单 + 审计（AI 操作可追溯）；
- 测试：MCP 工具端到端（起 socket → 查询 → 关闭）。

## 推迟（不做）

- **NetServer 万能网络适配层**（协议嗅探/路由/三并发模型）——基础夯实后再做；
- QoS 抽象层；WS/HTTP/SSE/DDS/Raft 适配；JinShan token/RDRL。

## 规则

- 任务 1/2 产出文档；任务 3 产出代码（压测/随机/5 场景 + audit 文档 + 三遍审查）；
- 0 警告 + ctest 全过；不引入第三方库。
