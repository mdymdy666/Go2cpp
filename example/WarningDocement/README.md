# example 使用限制索引（WarningDocement）

> 逐条对照 `example/` 代码提炼的**使用限制**，每条附 file:line 证据。
> 只写「违反会坏」的约束，不写 API 教程与设计长文。生成日期：2026-10-29。

## 模块文档

| 模块 | 文档 | 限制数 | 覆盖重点 |
| --- | --- | --- | --- |
| concurrency | [concurrency.md](concurrency.md) | 11 | go() 可复制回调、Fiber 栈上限、SpinMutex 禁 park、Lock 可失败、取消回调禁阻塞、Timer 句柄、TLS 策略、BlockingRegion、FiberLocal、shutdown 顺序、异步语义 |
| network | [network.md](network.md) | 10 | Hook 仅 Linux、多 fd 等待仅 Fiber、WaitManyFor 非屏障、Hook 开关禁锁、WebSocket 无超时、Framer/SSE 边界、FdManager 不拥有 fd、IOBackend 注册表、关闭顺序 |
| config | [config.md](config.md) | 9 | IniFile 不校验、Config 单例+事务锁、监听器/校验器异常语义、校验拒绝保留旧值、热重载、FromIni 事务性、[timer] 段缺陷、求值顺序、ApplyLogging 副作用 |
| logging | [logging.md](logging.md) | 7 | 格式项单字符、内置键占用、注册顺序、非法模式不报错、全局 LoggerManager、配置缺失、Flush |
| debug | [debug.md](debug.md) | 7 | dump CLI argc/argv 契约、--follow 上限、SIGUSR1 只置位、include_fibers、只读快照、ctest 约束 |
| tools | [tools.md](tools.md) | 10 | panic 不展开栈、recover 边界、defer 异常、run_now、Select 可复制、errors 身份比较、MultiReader、ReadFull、Debug 计时 |

**合计 54 条限制。**

## 专题设计文档（保留）

| 文档 | 主题 | 关联条目 |
| --- | --- | --- |
| [waitnode-lock-design.md](waitnode-lock-design.md) | WaitNode 内部锁为何必须是 FutexMutex（跨线程解锁 UB / 持锁 park） | concurrency #4、#5 |
| [context-callback-design.md](context-callback-design.md) | Context 取消回调禁止 park/阻塞的机制与约定 | concurrency #5 |

## 读法

1. 先看本模块文档的标题清单，判断自己是否用到对应 API；
2. 点开 file:line 核对原始代码；
3. 违反后果一栏写的是**实际症状**（崩溃 / 自死锁 / 静默丢数据 / 永不触发），便于搜 issue。

## 维护约定

- 新增示例若引入新约束，在对应模块文档追加条目并更新本表计数；
- 每条限制必须能指到 example 或 src/include 的具体行；
- 不修改 `example/` 源码，本目录只承载文档。

## 补充索引（2026-10-30 晚）

- [audit-mcp-socket](audit-mcp-socket.md)：MCP Socket 工具（33 工具，14.5k ops/s）
- [network-p1-fix](network-p1-fix.md)：网络 P1 修复（Fiber errno / HOOK=OFF）
- [audit-base-network](audit-base-network.md)：网络基础审计（TCP 28.7k conn/s / UDP 41.9k pkt/s）
