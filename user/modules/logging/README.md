# 日志（logging）

> **一句话定位**：四层可组合的日志。
> 本文是「模块 → 能力 → 异常」手册；[可观测性总览（日志/脱敏/轮转）](../../deep/10-observability.md)


> 日志模块的示例：级别过滤、模式化格式化、内存收集、文件轮转、INI 热配置。

## 能力清单

一个**零第三方依赖**（只用 C++17 标准库）的日志实现，按四层组合：

| 层 | 类型 | 职责 |
| --- | --- | --- |
| 过滤 | `LogFilter` / `MinimumLevelFilter` | 决定一条记录是否放行 |
| 格式化 | `LogFormatter` / `PatternFormatter` / `Item` | 把 `LogRecord` 渲染成一行（`%m` 消息、`%n` 换行、`%l` 级别…） |
| 投递 | `LogItemWorker` | 异步队列，热路径只做一次原子读的三字段快照 |
| 输出 | `LogSink` 家族 | `FileSink` / `RotatingFileSink` / `StdoutSink` / `StderrSink` / `MemorySink` / `CallbackSink` / `BufferedSink` |

入口是 `LoggerManager`（进程级注册表）、`Logger`（命名记录器，支持父子传播）
和 `GO2CPP_LOG_*` 宏。

## 底层架构

```
 业务代码
   GO2CPP_LOG_INFO(logger) << "..."
            │
            ▼
   ┌──────────────┐   级别过滤   ┌──────────────┐
   │ Logger::Log  │ ──────────► │ LogFilter    │  不通过 → 直接丢弃
   └──────┬───────┘              └──────────────┘
          │ 构造 LogRecord（logger/level/message/时间/线程）
          ▼
   ┌──────────────┐  异步队列   ┌──────────────┐
   │ LogItemWorker│ ──────────► │ LogFormatter │  渲染成一行
   │  Submit()    │             └──────┬───────┘
   └──────────────┘                    ▼
                                ┌──────────────┐
                                │ LogSink      │ 文件 / 轮转 / stdout / 内存 / 回调
                                └──────────────┘
```

**关键设计**：

- **热路径极简**：`LogItemWorker::Submit()` 只读一次原子快照（worker 的三个字段），
  不做锁、不做格式化；真正的渲染在 worker 线程做；
- **两条锁不嵌套**：队列锁与 sink 锁分开获取，避免「写日志时持锁导致死锁」；
- **`BufferedSink` 批量刷盘**：按条数/时间触发 flush，减少 `write(2)` 次数；
- **`CallbackSink` 是逃生口**：要把日志转给 spdlog/fmt 时不用改框架，挂一个回调即可；
- **默认安全值**：`warning`/`error` 写入 `log/go2cpp.log`，**stdout 默认关闭**，
  避免库把日志喷到调用方的终端（要开就 `Configure(..., stdout_enabled=true, ...)`）。

## 最小示例

```cpp
#include "go2cpp/log.hpp"

int main() {
    using namespace go2cpp::log;

    // ① 纯内存演示：级别解析 → 模式格式化 → 内存 sink 收集
    Level level = Level::Off;
    (void)ParseLevel("warn", &level);           // → Level::Warn

    PatternFormatter formatter("%m%n");
    LogRecord record;
    record.level = Level::Error;
    record.logger = "Demo";
    record.message = "something failed";

    MemorySink sink;
    sink.Write(record, formatter.Format(record));
    const auto lines = sink.Snapshot();          // {"something failed\n"}

    // ② 真正打日志：配置进程级 LoggerManager 后按名字取 Logger
    LoggerManager::Instance().Configure(Level::Info, /*stdout=*/true, "log", "demo.log");
    const auto logger = GO2CPP_LOG_NAME("Demo");
    GO2CPP_LOG_INFO(logger) << "hello";
    return lines.size() == 1 ? 0 : 1;
}
```

自定义格式项（`log_config_demo.cpp` 的写法）：

```cpp
class MyItem final : public go2cpp::log::LogFormatter::Item {
public:
    void format(std::ostream& os, const go2cpp::log::LogRecord& r) const override {
        os << "module=" << r.logger;
    }
};
go2cpp::log::LogFormatter::AddFormat("g", [](const std::string&) {
    return std::make_shared<MyItem>();
});
go2cpp::log::PatternFormatter fmt("%g %m%n");   // 必须先注册再构造 Formatter
```

## 3 个常见异常

### 1. 日志「没有输出」
- **现象**：程序跑完终端空白，以为日志没生效。
- **原因**：默认 `stdout_enabled=false`，只把 warning/error 写入 `log/go2cpp.log`。
- **处理**：`LoggerManager::Configure(..., stdout_enabled=true, ...)`，或直接查看日志文件。

### 2. 自定义格式项 `%g` 不生效
- **现象**：输出里原样打印 `%g`。
- **原因**：`PatternFormatter` **构造时快照**已注册的格式项；先建 Formatter 再 `AddFormat` 无效。
- **处理**：先 `LogFormatter::AddFormat("g", ...)`，再构造 `PatternFormatter`。

### 3. 同一条日志重复输出
- **现象**：一行日志出现两三次。
- **原因**：父子 Logger 都挂了 sink，记录沿父子链传播时被多次落盘。
- **处理**：只在根 logger 挂 sink；子 logger 只设级别与字段。

## 示例清单

| 示例 | 演示什么 |
| --- | --- |
| `log_demo` | 简单版：`ParseLevel`、`PatternFormatter`、`MemorySink`（全部在内存，不写文件） |
| `log_full` | 7 个场景：级别过滤、格式化模式、多 sink、轮转、父子 logger 传播、异常隔离、批量刷盘 |
| `log_config_demo` | 从 `go2cpp.ini` 加载运行时日志配置、注册自定义格式项 `%g`、按级别输出 |

```sh
cmake --build /tmp/build-user --target go2cpp_log_demo -j && /tmp/build-user/go2cpp_log_demo
```

## 源码位置

| 能力 | 位置 |
| --- | --- |
| 全部公开接口（Filter/Formatter/Worker/Sink/Logger/宏） | `include/go2cpp/logging/log.hpp` |
| 实现 | `src/logging/log.cpp` |
| 敏感字段脱敏 | `include/go2cpp/logging/redact.hpp` |
| 文件轮转 | `include/go2cpp/logging/rotate.hpp` |
| 顶层转发头 | `include/go2cpp/log.hpp` |

日志格式说明与配置项见根目录 `go2cpp.ini` 的 `[log]` 段与 `docs/HANDOFF.md`。

## 注意事项

- **stdout 默认关闭**：不加 `stdout_enabled=true` 时只能看到 `log/go2cpp.log`
  （`warning` 及以上）。示例里为了让读者看到输出，通常显式打开。
- **`PatternFormatter` 构造时快照格式项**：必须**先** `AddFormat`，**再**建 Formatter；
  顺序反了 `%g` 不会被替换。
- **sink 必须线程安全**：worker 线程会并发调用 `Write`；自己实现 sink 时别省锁。
- **日志里不要放大对象**：`LogLine` 的流式拼接在调用线程完成，超大字符串会拖慢热路径；
  需要序列化大对象时先判断级别。
- **`Logger` 有父子传播**：没配 sink 的子 logger 会把记录交给父 logger；
  重复挂 sink 会导致同一条日志输出多次。

## 深入阅读

- [可观测性总览（日志/脱敏/轮转）](../../deep/10-observability.md)
- [交接摘要（docs）](../../docs/10-observability.md)
- 源码位置：`include/go2cpp/logging/`、`src/logging/`、转发头 `include/go2cpp/log.hpp`
- 示例目标名 = `go2cpp_<文件名>`；新增示例需同时登记 `cmake/Go2CppTests.cmake` 与本文清单。
