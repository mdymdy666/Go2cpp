# 03 log 模块问题文档

> 工作目录：`/UserData/CodexWorkSpace/Go2Cpp`。关键文件：`include/go2cpp/logging/log.hpp`（855 行）、
> `src/logging/log.cpp`（1058 行）。
> 阅读状态：已答（首轮）

## 1. 模块速览

流水线：`Filter → Formatter → Sink`，由 `LogItemWorker` 组合；`Logger` 管级别/worker/父链，
`LoggerManager` 管注册表与默认配置，`LogLine` 是宏背后的 RAII 临时对象。

| 层 | 入口 | 说明 |
|---|---|---|
| 级别/记录 | `log.hpp:61` Level、`:94` LogRecord | 时间/线程/文件行/消息 |
| 过滤 | `log.hpp:121` LogFilter、`:139` MinimumLevelFilter | 两级过滤：Logger 与 worker 各一层 |
| 格式化 | `log.hpp:167` LogFormatter、`:291` PatternFormatter | `{time} [{level}] ...` 模式，Item 链 |
| 输出 | `log.hpp:332` LogSink 及 File/Stdout/Stderr/Memory/Callback/Buffered | `Write`/`Flush`/`Reopen` |
| 工作单元 | `log.hpp:637` LogItemWorker | `Submit()` 同步执行过滤→格式化→写 |
| 记录器 | `log.hpp:694` Logger、`:762` LoggerManager | 原子级别 + 原子 worker 快照 |
| 宏入口 | `log.hpp:815` LogLine、`:846-855` `GO2CPP_LOG_*` | 构造抓上下文，析构提交 |

默认值（`log.hpp:791-798`）：level=`Warn`、stdout=`false`、directory=`log`、file=`go2cpp.log`。
即默认只把 Warn 及以上写入 `log/go2cpp.log`，不打印到 stdout。

## 2. 问题记录

### Q-001 一条日志从宏到落盘的完整路径？

- 结论：
  1. `GO2CPP_LOG_INFO(logger) << ...` 构造临时 `LogLine`（`log.cpp:879-886`）；
  2. 构造时 `ShouldLog(level)` 判定，未通过则 `m_enabled=false`，析构不提交；
  3. 表达式结束析构 `LogLine`（`log.cpp:895-898`）：`m_stream.str()` → `Logger::Log(record)`；
  4. `Logger::Log`（`log.cpp:769-779`）：再判级别，原子读 worker 快照，逐个 `Submit`；
  5. `LogItemWorker::Submit` 同步执行 Filter → Formatter → `Sink::Write`；
  6. `FileSink::Write`（`log.cpp:461-472`）锁内一次 `write`，Warn+ 立即 `flush`。
- 证据：
  - 宏展开：`log.hpp:847-848`；级别宏：`:849-855`
  - `LogLine` 构造/析构：`log.cpp:879-898`
  - `Logger::Log`：`log.cpp:769-779`
  - `FileSink::Write/Reopen`：`log.cpp:461-499`（`app|out` 追加打开）
- 状态：已答

### Q-002 线程模型与性能：是不是异步日志？

- 结论：**不是异步**。`LogItemWorker::Submit` 在调用线程同步完成格式化与 Sink 写入；
  `Logger`/`LogItemWorker` 的热路径用原子 `shared_ptr` 快照，不加配置锁；`FileSink`
  每条日志一次 `write`，Warn+ 额外 `flush`。高并发写日志会直接占用业务线程。
- 证据：
  - worker 快照：`log.hpp:670-678`，`Submit` 不在锁内调 Formatter/Sink（`:631-635`）
  - `Logger::Log` 原子读快照：`log.cpp:773-774`
  - FileSink 锁内只写、不额外 IO：`log.cpp:461-472`
  - 可选 `BufferedSink`（默认不启用）：`log.hpp:404-462`，批量写 + 两把不嵌套的锁
- 风险/建议：低延迟业务可把 Sink 包 `BufferedSink`（kBySize/kByTime），或自行接异步队列；
  `kByTime` 是惰性刷，长时间无日志不会自动刷，必须显式 `Flush()`。
- 状态：已答

### Q-003 父子 Logger 传播默认到底开不开？

- 结论：**默认关闭**。`Logger` 的 `m_propagate{false}`（`log.hpp:743`）；`LoggerManager::Get`
  只给非 root Logger `SetParent(m_root)`（`log.cpp:815`），**不**自动 `SetPropagate(true)`。
  因此 README 说的「Logger 父级传播」需要显式开启（`SetPropagate(true)` 或
  `LoggerManager::ConfigureLogger(name, level, propagate, workers)`，`log.hpp:774`）。
- 证据：
  - `m_propagate{false}`：`log.hpp:743`
  - `Log()` 仅在 `propagate` 为真时递归父级：`log.cpp:775-778`
  - `SetParent` 只写父链并做环检查：`log.cpp:733-757`
  - `LoggerManager::Get` 只 `SetParent`：`log.cpp:815`
- 风险/建议：文档/示例应明确「只 SetParent 不会传播」；否则用户会以为日志同时进了父文件。
  另注意传播会按父链每级各写一次（每级有自己的 worker），开启前确认不会重复落同一文件。
- 状态：已答

### Q-004 多个 Logger 写同一个 `log/go2cpp.log`，安全吗？

- 结论：默认 `FileSink` 用 `app|out` 追加打开（`log.cpp:494`），多个实例各自持 fd，
  依赖内核 `O_APPEND` 保证单次 `write` 不覆盖；单条记录是完整字符串，正常不会半条交错。
  但 **`RotatingFileSink` 多实例没有跨实例协调**：各自维护 `m_bytes`、各自 `rename`，
  滚动时其他实例的 fd 仍指向旧 inode，可能写到已改名的备份文件或丢日志。
- 证据：
  - `FileSink` 追加打开：`log.cpp:487-499`
  - 每个 Logger 独立默认 FileSink：`log.cpp:809-811`
  - `RotatingFileSink` 仅继承 FileSink 锁、无进程级滚动锁：`log.hpp:500-533`
- 风险/建议：生产若要多 Logger + 滚动，建议只给 root 配滚动 Sink、子 Logger 不配文件
  worker（或共享同一个 Sink 实例），不要在多个 Logger 上各建 `RotatingFileSink` 指向同一路径。
- 状态：已答

### Q-005 其他坑

1. **`LoggerManager::Get` 持 `m_mutex` 构造 FileSink**（`log.cpp:799-819`）：`create_directories`
   + `open` 都在锁内，首次获取某 Logger 会阻塞其他线程的日志调用；可在启动期预热。
2. **`FileSink::Reopen` 失败后静默丢日志**：`m_stream` 为空时 `Write` 直接返回（`log.cpp:463`），
   没有计数器/告警；磁盘满或权限问题时表现为「日志突然没了」。
3. **`LogLine` 析构吞所有异常**（`log.cpp:897`）：日志表达式里的重要副作用若抛异常也会被吞，
   不要把业务逻辑塞进 `LOG << ...`。
4. **时间源**：`LogRecord.timestamp_ms` 用 `system_clock`（`log.hpp:100`），NTP 回拨会让时间倒退；
   跨线程顺序不保证。
5. **双重级别过滤**：`Logger::ShouldLog` 与 worker 的 `MinimumLevelFilter` 各一层；
   `SetLevel` 会同步更新快照里的 worker（`log.cpp:661-668`），但自定义非 MinimumLevelFilter 不会被改。
6. **`FileSink::Write` 依赖 `dynamic_cast<std::ofstream*>`**（`log.cpp:465`）：换成其他 ostream
   实现会静默不写；目前构造只产生 ofstream，暂无触发。

## 3. 已确认缺陷

| ID | 严重度 | 位置 | 触发 | 期望/实际 | 状态 |
|---|---|---|---|---|---|
| L-01 | P2 | `log.hpp:743`、`log.cpp:815` | 按 README 配父子 Logger | 期望自动传播；实际需显式 `SetPropagate(true)` | 未修（文档/默认值） |
| L-02 | P2 | `log.hpp:513-533` | 多个 Logger 各配 `RotatingFileSink` 同路径 | 期望协调滚动；实际 rename 竞争/旧 fd 写备份 | 未修 |
| L-03 | P3 | `log.cpp:799-819` | 首次 `Get(name)` | 期望不阻塞日志；实际持注册表锁做文件 IO | 未修 |
| L-04 | P3 | `log.cpp:463`、`:487-499` | 磁盘满/权限失败 | 期望可观测；实际静默丢弃 | 未修 |

## 4. 待读 / 待确认

- [ ] `PatternFormatter` 的模式解析缓存与自定义 Item 注册（`log.hpp:291`、`log.cpp:44-117`）
- [ ] `LoggerManager::Configure` 热更新时，旧 FileSink 关闭与新 sink 打开的并发窗口
- [ ] `BufferedSink::WriteRaw` 的 record 上下文丢失（`log.hpp:384-388`）对自定义 Sink 的影响
- [ ] `GO2CPP_LOG_*` 在 noexcept/析构路径中的使用约束

## 5. 关联

- 导读：`docs/code-review-guide.md` §2.1
- 文档：`docs/logging.md`
- 配置：`BindLoggingConfig`（`src/config/config.cpp:792`）→ `LoggerManager::Configure`（`:488`/`:814`）

## 6. 并发分层（core / concurrent，待拆分）

> 决策（2026-10-20）：log 拆成「不依赖 sync 的 core」与「依赖 sync 的 concurrent 层」，
> 以解决 `log → sync → scheduler → log` 的依赖环。详见
> `00-concurrency-policy.md` §4 方案 A。

### 6.1 目标结构

| 层 | 内容 | 依赖 |
|---|---|---|
| `go2cpp_log`（core） | Level/Record/Filter/Formatter/Sink/Logger/LoggerManager/`GO2CPP_LOG_*` | Threads；**不依赖 sync/scheduler** |
| `go2cpp_log_concurrent`（新） | `AsyncSink`、`FiberSink`、`Dispatcher`、可 park 的 `Flush` | `go2cpp_log` + `go2cpp_sync` |

scheduler 只依赖 core；config 可依赖 core 或 `log_concurrent`；`log_concurrent` 不回指
scheduler/config，因此无环。

### 6.2 边界

- core 读路径已经无锁（`Logger::m_worker_snapshot`、`LogItemWorker::m_snapshot` 原子快照）；
- core 写路径（`AddWorker/SetLevel/Configure`）保留 `std::mutex` 或声明单写者，**不得 park**；
- `FileSink::Write` 的锁无法去掉（多线程写同一 fd），但不在锁内 park；Sink 契约要求本地/非阻塞；
- concurrent 层负责：业务线程只入队（`AsyncSink`），后台单写线程落盘；`Flush` 用
  `sync::ConditionVariable` 等待，允许 Fiber park；`Dispatcher` 把配置回调/热更新投递到专用线程。

### 6.3 落地步骤

1. 拆 CMake target 与头文件（`logging/async.hpp`），`go2cpp_log` 的 link interface 去掉 sync；
2. `LoggerManager::Configure` 把 `FileSink` 构造移到锁外，锁内只替换 worker 快照；
3. config 的 `BindLoggingConfig` 监听器改用 concurrent 层的 `Configure`/`Dispatcher`；
4. 验收：`cmake --graphviz` 无环、core 不链接 sync/scheduler、回调 park 压测不卡 worker。
