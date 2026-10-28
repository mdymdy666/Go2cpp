# 模块文档：日志（logging）

> 适用基线：`HEAD 27e892b`（2026-10-26），WSL 工作树 `/UserData/CodexWorkSpace/Go2cpp`。
> 源码：`include/go2cpp/logging/log.hpp`（855 行）、`src/logging/log.cpp`（1,057 行）；
> 公开入口别名 `go2cpp/log.hpp`（`go2cpp::Logger`/`LoggerManager`/`LogLevel`）。
> 引用约定：`log.hpp` 指 `include/go2cpp/logging/log.hpp`，`log.cpp` 指
> `src/logging/log.cpp`。
> 状态：**现行**。本文合并 `docs/logging.md`（旧文档顶部已标注合并去向，正文保留
> 为历史快照）。

## 1. 概述

日志模块是独立目标 `go2cpp_log`，只依赖 C++17 标准库，不绑定 spdlog/fmt。一次日志
按 **Filter → Formatter → Sink** 三段处理，`LogItemWorker` 把三段组装成一个可替换
的工作单元，`Logger` 持有多个 worker，`LoggerManager` 管进程级注册表与默认配置
（`log.hpp:3-5`、`16-20`）。

```
GO2CPP_LOG_INFO(logger) << ...
        │ LogLine（构造抓时间/线程/位置，析构提交）
        ▼
Logger::Log(record) ──► 逐个 LogItemWorker::Submit
                              │ 1) Filter::Accept（不过滤就不格式化）
                              │ 2) Formatter::Format
                              ▼ 3) Sink::Write
                        FileSink / StdoutSink / MemorySink / CallbackSink ...
```

默认行为（`log.cpp:836-854`、`config.hpp:676-680`）：级别 `Warn`、写
`log/go2cpp.log`、**不写 stdout**、格式
`{time} [{level}] {logger} ({file}:{line}) {message}\n`。

### 1.1 最小示例

```cpp
#include "go2cpp/log.hpp"

static auto g_log = GO2CPP_LOG_NAME("SchedulerModule");

void report() {
    GO2CPP_LOG_INFO(g_log) << "只有 log.level<=info 时才输出";
    GO2CPP_LOG_WARN(g_log) << "默认会写入 log/go2cpp.log";
}
```

要立刻看到 stdout 并提高详细度：

```cpp
go2cpp::LoggerManager::Instance().Configure(
    go2cpp::LogLevel::Debug, /*stdout_enabled=*/true,
    "log", "go2cpp.log");
```

## 2. 级别与记录

### 2.1 级别

`Level` 是 `uint8_t` 枚举，数值从详细到严重递增（`log.hpp:61-73`）：

| 级别 | 数值 | 文本 |
|---|---:|---|
| `Trace` | 0 | `TRACE` |
| `Debug` | 1 | `DEBUG` |
| `Info` | 2 | `INFO` |
| `Notice` | 3 | `NOTICE` |
| `Warn` | 4 | `WARN` |
| `Error` | 5 | `ERROR` |
| `Crit` | 6 | `CRIT` |
| `Alert` | 7 | `ALERT` |
| `Critical` | 8 | `CRITICAL` |
| `Off` | 9 | `OFF`（拒绝一切） |
| `Fatal` | = `Critical` | 兼容别名 |

`ToString` 输出大写名、未知值返回 `UNKNOWN`（`log.cpp:335-349`）；`ParseLevel`
大小写不敏感，接受 `warning`→Warn、`fatal`→Critical 等别名（`log.cpp:407-425`）。
`MinimumLevelFilter::Accept` 的规则是 `record.level >= minimum && minimum != Off`
（`log.cpp:427-430`）。

### 2.2 LogRecord

`LogRecord` 是四层之间唯一的数据契约（`log.hpp:94-106`）：

| 字段 | 语义 | 谁写 |
|---|---|---|
| `level` / `logger` | 级别与记录器名 | `LogLine` 构造时 |
| `message` | 消息正文 | `LogLine` 析构时从 ostringstream 取出 |
| `file` / `function` / `line` | 调用点 | `GO2CPP_LOG_*` 宏传入 |
| `timestamp_ms` | Unix epoch 毫秒 | `LogLine` 构造时 |
| `thread_id` | 线程 id 哈希 | `LogLine` 构造时 |
| `fiber_id` | Fiber id；未填为 0 | 调用方 |
| `elapse_ms` / `thread_name` | 业务可选 | 调用方 |

所有字符串字段都是**自有拷贝**，不引用调用方临时内存（`log.hpp:89-92`），因此
异步/批量 Sink 可以安全持有记录副本。

## 3. 处理流水线

### 3.1 LogItemWorker

`Submit` 的热路径只做一次 `atomic_load` 读配置快照，然后依次过滤、格式化、写 Sink；
过滤失败直接返回，不构造格式化字符串（`log.cpp:590-614`）。用户 Formatter/Sink 在
**不持任何 Worker 锁**的情况下被调用，因此自定义 Sink 内部再次打日志不会递归锁死
（`log.cpp:599-606`）。

配置变更走「复制-替换」：`SetFilter`/`SetFormatter`/`SetSink` 在 `m_mutex` 内复制
当前 `Snapshot`、替换一个字段、再原子发布新快照（`log.cpp:616-623`）。因此日志线程
永远看到一致的三元组，不会出现「新 Filter + 旧 Sink」的中间态。

### 3.2 Logger 与传播

`Logger` 持有默认 worker 与自定义 worker 两套列表，日志热路径读的是原子快照
（`log.hpp:738-746`）。`ShouldLog` 只读 `m_level` 原子量（`log.cpp:671-679`）；
`Log` 先按级别分发，再按 `m_propagate` 与父记录器 `weak_ptr` 决定是否上抛
（`log.cpp:769-781`、`log.hpp:689-693`）。默认只有 `root` 之外的新 Logger 以 root
为父，且 `propagate` 默认关闭（`log.cpp:814-815`、`log.hpp:743`）。

`LoggerManager` 是进程级单例（`log.cpp:782-785`）：

- `Get(name)` 查找或创建；新 Logger 继承当前默认级别、共享一个
  `PatternFormatter`、建 FileSink（可选再加 StdoutSink）（`log.cpp:787-819`）；
- `Configure(level, stdout, directory, file, pattern)` 遍历**所有** Logger，同步级别
  并整体替换默认 worker；自定义 worker 保留（`log.cpp:823-854`）。代价是每次
  Configure 都重新打开文件，热更新频率应远低于写日志频率；
- `Remove("root")` 被忽略；`Flush()` 遍历所有 Logger（`log.cpp:864-871`）。

### 3.3 LogLine 与宏

`GO2CPP_LOG_INFO(logger) << ...` 展开成 `LogLine(...).stream()`（`log.hpp:846-855`）。
构造时判断级别是否通过；未通过则 `m_enabled=false`，流仍可写但析构不提交
（`log.cpp:879-886`）。析构是 `noexcept` 且整体 `try/catch(...)`，日志失败绝不影响
业务（`log.cpp:888-898`、`log.hpp:810-813`）；这也意味着**不要**在日志表达式里做
有副作用且可能抛异常的重要操作。

## 4. 输出层（Sink）

### 4.1 内置 Sink

| Sink | 行为 | 引用 |
|---|---|---|
| `FileSink` | 一把 `m_mutex` 串行化同一文件；锁内只做一次 `write`；`Warn` 及以上立即 `flush`，低级别留在流缓冲 | `log.hpp:478-498`、`log.cpp:461-476` |
| `RotatingFileSink` | 超 `max_bytes` 后关闭流、从高到低重命名备份、重建文件；`m_bytes` 构造时用 `file_size` 初始化 | `log.hpp:513-533`、`log.cpp:529-554` |
| `StdoutSink` | 实例内互斥；`std::cout` 跨实例仍可能交错 | `log.hpp:545-553` |
| `StderrSink` | 同 Stdout，用于错误分流 | `log.hpp:564-572` |
| `MemorySink` | 保存格式化文本供测试断言；丢结构化字段 | `log.hpp:584-596` |
| `CallbackSink` | 转发给 spdlog/fmt 或测试回调；回调线程安全自负 | `log.hpp:609-620` |
| `BufferedSink` | 装饰器：把 N 条合并成一次批量写 | `log.hpp:404-462` |

`LogSink` 基类**不承诺**线程安全；`WriteRaw` 默认返回 false，表示不支持批量，
缓冲层会退化成逐条 `Write`（`log.hpp:326-351`）。

### 4.2 BufferedSink 的刷新策略

| 策略 | 触发条件 | 备注 |
|---|---|---|
| `kImmediate` | 每条直接写并 flush | 等价不缓冲，便于对照测试 |
| `kBySize` | 缓冲 ≥ `capacity_bytes`（默认 64 KiB） | 高频场景主用 |
| `kByTime` | 距上次刷新 ≥ `flush_interval`（默认 1000 ms） | **惰性检查**：没有新日志就不会刷 |
| `kBySizeOrTime` | 任一满足 | — |

`bypass_threshold > 0` 时，单条超过阈值的记录绕过缓冲直接写并 flush，避免撑爆或
长期滞留（`log.cpp:968-989`）。缓冲 `Write` 的常见路径是：锁内 append（补换行）、
判断是否该刷、把缓冲 `swap` 到局部变量、**释放锁后**才做 IO（`log.cpp:991-1009`）。
`m_mutex` 与 `m_io_mutex` **不嵌套**：先释放 `m_mutex` 再取 `m_io_mutex`，因此不
存在锁序死锁；多线程同时触发刷新时各自持有自己的批次依次写，顺序不保证但不会丢、
不会写出半条记录（`log.hpp:372-388`、`log.cpp:1012-1040`）。

批量写时 `WriteRaw` 收到的 `LogRecord` 上下文是**最后一条**记录；依赖每条 record
字段的 Sink 应覆写 `WriteRaw` 或不要包缓冲（`log.hpp:384-388`）。

## 5. 格式化

`PatternFormatter` 在构造时把模式解析成 Item 链，热路径只顺序调用
`Item::Format`，不再解析字符串、不再访问注册表（`log.hpp:282-311`、
`log.cpp:432-442`）。

内置占位符：`{time}`、`{level}`、`{logger}`、`{thread}`、`{fiber}`、`{file}`、
`{line}`、`{function}`、`{message}`（`log.hpp:279-280`，注册表在
`log.cpp:173-217`）。同时兼容 Sylar 风格的 `%d{%Y-%m-%d %H:%M:%S}%T...%n`
（旧文档 §“格式器兼容”），未知/未闭合占位符不抛异常，而是原样保留并把 `HasError()` 置 true，便于
配置加载阶段拒绝拼写错误（`log.hpp:301-305`、`log.cpp:232-259`）。

### 5.1 自定义格式项

`LogFormatter::Item` 是插件边界：派生类覆写 `Format` 或 Sylar 风格的小写 `format`
之一即可（`log.hpp:190-210`）。注册接口有四种形态（带选项/无选项/模板/小写别名），
都落到同一张注册表；重复 key 返回 false，不会静默覆盖（`log.hpp:215-273`、
`log.cpp:360-376`）。工厂在释放注册表锁后执行，因此工厂内部可以安全加载其它插件；
`PatternFormatter` 构造后固定 Item 链，注销格式项不影响已创建实例
（旧文档 §“自定义格式项”、`log.hpp:298-310`）。

## 6. 动态配置

`BindLoggingConfig` 把 `log.level`/`log.stdout`/`log.directory`/`log.file`/
`log.format` 注册为 `ConfigVar`，并用 `weak_ptr` 捕获避免
`ConfigVar → Listener → ConfigVar` 引用环；任一项变化都读取全部五项后调用一次
`LoggerManager::Configure`，避免「新级别 + 旧格式」的中间态
（`config.cpp:860-900`）。配置文件热加载与编辑锁协议见
[config.md](config.md) §6。

`RuntimeConfig` 里对应的默认值是 `log_level=Warn`、`log_stdout=false`、
`log_directory="log"`、`log_file="go2cpp.log"`、`log_format` 为默认模式
（`config.hpp:676-680`）；`ApplyLogging()` 只改全局 LoggerManager，不触碰调度器
（`config.cpp:562-567`）。

## 7. API 一览

| API | 位置 | 说明 |
|---|---|---|
| `Level` / `ToString` / `ParseLevel` | `log.hpp:61-78`；`log.cpp:335-349`、`407-425` | 级别与稳定文本 |
| `LogFilter::Accept` / `MinimumLevelFilter` | `log.hpp:121-152`；`log.cpp:427-430` | 过滤在格式化之前 |
| `LogFormatter` / `Item` / `PatternFormatter` | `log.hpp:167-311` | 占位符模式 + 插件项 |
| `LogFormatter::AddFormat/RemoveFormat/HasFormat/Formats` | `log.hpp:215-273`；`log.cpp:360-405` | 全局格式项注册表 |
| `LogSink` / `Write` / `WriteRaw` / `Flush` / `Reopen` | `log.hpp:332-352` | 输出抽象；默认不批量 |
| `BufferedSink(inner, Config)` / `FlushCount` / `BufferedBytes` | `log.hpp:404-462`；`log.cpp:910-1053` | 批量装饰器与诊断计数 |
| `FileSink(path)` / `RotatingFileSink(path, max_bytes, max_files)` | `log.hpp:478-533` | 文件与滚动 |
| `StdoutSink` / `StderrSink` / `MemorySink` / `CallbackSink` | `log.hpp:545-620` | 其余内置输出 |
| `LogItemWorker(filter, formatter, sink)` / `Submit` / `Set*` | `log.hpp:637-678`；`log.cpp:585-655` | 一次流水线 |
| `Logger(name)` / `SetLevel` / `ShouldLog` / `Log` / `AddWorker` / `Flush` | `log.hpp:694-747`；`log.cpp:658-780` | 记录器与传播 |
| `LoggerManager::Instance/Get/Root/Configure/ConfigureLogger/Remove/List/Flush` | `log.hpp:762-799`；`log.cpp:782-871` | 进程级注册表 |
| `LogLine(logger, level, file, line, function)` | `log.hpp:815-833`；`log.cpp:879-898` | 流式临时对象 |
| `GO2CPP_LOG_NAME/TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL/FATAL` | `log.hpp:846-855` | 宏入口 |

## 8. 设计说明

1. **为什么分四层**：Filter 在格式化前拒绝可以省掉整条字符串构造；Formatter 与 Sink
   分离使「同一格式、多个输出」共享一个 Formatter；Worker 把组合固定成不可变快照，
   热路径不加锁。
2. **为什么用原子 snapshot 而不是读写锁**：日志热路径的读远多于配置写；
   `atomic_load(shared_ptr)` 一次读即可拿到一致三元组，配置写频率极低，复制成本可忽略
   （`log.hpp:659-677`、`log.cpp:602-614`）。
3. **为什么 Warn 及以上立即 flush**：错误日志需要被外部收集器马上看到；Info/Debug
   留在流缓冲，避免高吞吐退化成每条一次 `write(2)`。确定性落盘由显式 `Flush()` 负责
   （`log.cpp:448-460`）。
4. **为什么缓冲层用两把锁**：锁内 append + swap 让持锁时间与批量大小无关；IO 在锁外，
   慢磁盘不会阻塞其它线程的 append；两把锁不嵌套，消除锁序死锁（`log.hpp:372-388`）。
5. **为什么日志异常必须吞掉**：日志是旁路设施，不能让磁盘满/回调抛异常把业务打崩；
   `LogLine` 析构与监听器回调都在边界处 catch（`log.cpp:888-898`）。

## 9. 边界与陷阱

- **不自动 Flush**：`BufferedSink` 与 `bufio::Writer` 语义一致，只在容量/时间条件或
  显式 `Flush()` 时刷盘；进程崩溃前必须 `LoggerManager::Flush()`
  （`log.hpp:27-37`）；
- **不要在持锁期间打日志**：`fprintf`/`std::ostream` 自身会加锁并可能触发刷盘，
  在业务锁内打日志会把「业务锁 → stdio 锁 → 磁盘 IO」串成一条链；Sink 在锁内只做
  `write`，刷盘放到锁外（`log.hpp:31-34`）；
- **`kByTime` 是惰性检查**：长时间没有日志时缓冲不会自动刷出
  （`log.hpp:363-370`）；
- **批量写丢 record 上下文**：`WriteRaw` 的 `LogRecord` 只保留最后一条
  （`log.hpp:384-388`）；
- **stdout 跨实例可能交错**：每个 `StdoutSink` 只有自己的锁，`std::cout` 是进程级
  共享资源（`log.hpp:541-543`）；
- **日志表达式不要放关键副作用**：`LogLine` 析构吞掉所有异常
  （`log.hpp:810-813`）；
- **Configure 会重开文件**：热更新频率过高会放大 `open` 成本（`log.cpp:832-834`）。

## 10. 性能特征

模块没有随代码提交的独立微基准；可复现的成本结构如下：

- 未通过级别检查时，热路径只有一次 `ShouldLog` 原子读 + 一次 `LogLine` 构造，
  不构造格式化字符串、不碰 Sink（`log.cpp:879-886`）；
- 通过检查后每条日志一次 `atomic_load` 快照 + 一次 `Formatter::Format`
  （`ostringstream` 拼接）+ 一次 `Sink::Write`；`FileSink` 在锁内一次 `write`，
  Warn+ 额外一次 `flush`（`log.cpp:461-476`）；
- `BufferedSink` 把 N 条合并为一次 `WriteRaw`/`Write`，系统调用次数降为 1/N，
  代价是最多一个 `capacity_bytes` 的延迟与内存（`log.hpp:354-388`）；
- 格式项注册与模式解析只发生在 `PatternFormatter` 构造时，日志热路径不访问注册表
  （`log.hpp:186-189`）。

需要具体数字时用 `tests/logging/test_log_buffer.cpp` 的缓冲行为测试扩展成基准，
不要引用旧文档里没有出处的数字。

## 11. 测试（对应 ctest）

| ctest | 覆盖 | 源码 |
|---|---|---|
| `go2cpp_log_buffer_test` | `BufferedSink` 三种策略、bypass、`FlushCount`/`BufferedBytes`、并发刷新不丢不撕裂 | `tests/logging/test_log_buffer.cpp`；注册在 `cmake/Go2CppTests.cmake:579-586` |
| `go2cpp_tests`（filter `log`） | 格式化、并发写入、动态配置热加载、级别边界 | `tests/logging/test_log_config.cpp`；`tests/test_main.cpp:99` |
| `go2cpp_config_registry_macro_test` | 配置注册宏与日志字段联动 | `tests/config/test_config_registry_macro.cpp`；`cmake/Go2CppTests.cmake:647-654` |

```bash
ctest --test-dir /tmp/build-ci-sim -R 'log_buffer' --output-on-failure
GO2CPP_TEST_FILTER=log /tmp/build-ci-sim/go2cpp_tests
```

## 12. 合并说明与变更记录

- 合并 `docs/logging.md`：默认级别/路径、最短用法、PatternFormatter 占位符、
  Sylar 兼容、自定义 Item 的两种写法、动态配置示例；正文保留为历史快照。
- 2026-10-26：按 `HEAD 27e892b` 核对行号；补 `BufferedSink` 两把锁与惰性刷新、
  `LogItemWorker` 的不可变快照、`LoggerManager::Configure` 的重建代价、边界清单。
