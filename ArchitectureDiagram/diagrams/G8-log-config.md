# G8 · 横切设施（日志 / 配置）—— 一图看懂

> 覆盖：`log`（`LogSink`/`LogFilter`/`LogFormatter` 三个**基类**及其派生：`FileSink`/`RotatingFileSink`/
> `StdoutSink`/`StderrSink`/`MemorySink`/`CallbackSink`/`MinimumLevelFilter`/`PatternFormatter`，
> 以及 `Logger`/`LoggerManager`/`LogRecord`/`LogLine`）、
> `config`（`IniFile`/`ValueCodec<T>`/`ConfigVarBase`/`ConfigVar<T>`/`Config`/`RuntimeConfig`）。
> 源码：`include/go2cpp/logging/log.hpp`、`config/config.hpp`。

**为什么放在一起**：两者都是**被几乎所有模块使用**的横切设施，而且都靠
"**抽象基类 + 可替换实现**"来避免上层依赖具体后端。

## ① 类架构：每个类用的底层能力 + 主要职责

```mermaid
classDiagram
    direction TB

    class LogSink {
        <<输出目标抽象>>
        +Write(record, formatted)*
        +Flush()
        +Reopen() bool
    }
    class FileSink {
        <<文件输出·互斥锁保护流>>
    }
    class RotatingFileSink {
        <<按大小轮转>>
    }
    class StdoutSink
    class StderrSink
    class MemorySink {
        <<内存缓冲·测试用>>
    }
    class CallbackSink {
        <<转发到用户回调>>
    }

    class LogFilter {
        <<过滤抽象>>
        +ShouldLog(record)* bool
    }
    class MinimumLevelFilter {
        <<低于阈值丢弃>>
    }
    class LogFormatter {
        <<格式化抽象>>
        +Format(record)* string
    }
    class PatternFormatter {
        <<按 pattern 展开>>
    }

    class LogRecord {
        <<一条日志的原始字段>>
        +level / time / file / line / thread
        +message
    }
    class LogLine {
        <<便于链式写日志>>
        +operator<< 追加
        +完成后提交给 Logger
    }
    class Logger {
        <<一个命名 logger>>
        +Log(level, message)
        +SetSink / SetFilter / SetFormatter
    }
    class LoggerManager {
        <<全局注册表>>
        +GetOrCreate(name) Logger
    }

    class IniFile {
        <<INI 解析>>
        +Load(text) bool
        +Get(section, key) string
    }
    class ValueCodec {
        <<字符串 <-> 强类型>>
        +Encode / Decode (特化)
    }
    class ConfigVarBase {
        <<配置项抽象>>
        +Name() / Description()
        +FromString(s) bool
        +ToString() string
    }
    class ConfigVar {
        <<模板配置项>>
        +GetValue() T
        +SetValue(T)
        +AddListener(cb)
        ← 值变化时通知监听者
    }
    class Config {
        <<配置集合>>
        +Lookup(name) ConfigVarBase
        +LoadFromIni(IniFile)
    }
    class RuntimeConfig {
        <<运行时配置快照>>
    }

    FileSink --|> LogSink
    RotatingFileSink --|> FileSink
    StdoutSink --|> LogSink
    StderrSink --|> LogSink
    MemorySink --|> LogSink
    CallbackSink --|> LogSink
    MinimumLevelFilter --|> LogFilter
    PatternFormatter --|> LogFormatter
    Logger ..> LogSink : 持有
    Logger ..> LogFilter
    Logger ..> LogFormatter
    LoggerManager *-- Logger
    LogLine ..> LogRecord
    LogLine ..> Logger
    ConfigVar --|> ConfigVarBase
    Config *-- ConfigVarBase
    ConfigVar ..> ValueCodec : 读/写用 codec
    Config ..> IniFile : 从 INI 装载
    Config ..> RuntimeConfig : 生成运行时快照
```

| 类 | 主要职责 | 用的底层能力 |
|---|---|---|
| `LogSink` | **输出目标抽象**（写/刷新/重开） | 纯虚接口 |
| `FileSink` / `RotatingFileSink` | 写文件 / 按大小轮转 | `std::ofstream` + **互斥锁**（多线程安全） |
| `StdoutSink` / `StderrSink` | 标准输出/错误 | libc 流 |
| `MemorySink` | 内存里留最近 N 条（**测试与自检用**） | 环形缓冲 |
| `CallbackSink` | 转发给用户回调（接外部系统） | `std::function` |
| `LogFilter` / `MinimumLevelFilter` | 过滤（低于阈值丢弃） | 级别比较 |
| `LogFormatter` / `PatternFormatter` | 格式化（按 pattern 展开字段） | 字符串替换 |
| `Logger` / `LoggerManager` | 命名 logger 与全局注册表 | 互斥锁 + map |
| `ConfigVar<T>` | 类型安全的配置项，**值变化通知监听者** | `ValueCodec<T>` 特化 |
| `Config` / `IniFile` | 从 INI 装载、按名字查找 | 解析器 |

## ② 重要函数：功能与实现要点

```mermaid
flowchart TB
    subgraph LOG["一次 Logger::Log 的路径"]
        L1["LogLine 累积字段<br/>（level/time/file/line/thread/message）"] --> L2["提交给 Logger"]
        L2 --> L3{"Filter.ShouldLog(record)?"}
        L3 -- 否 --> L4["直接丢弃（**最便宜的路径**）"]
        L3 -- 是 --> L5["Formatter.Format(record) -> string"]
        L5 --> L6["Sink.Write(record, formatted)"]
        L6 --> L7{"sink 是 FileSink?"}
        L7 -- 是 --> L8["持**该 sink 自己的**互斥锁写流"]
        L7 -- 否 --> L9["各自实现（内存/回调/标准流）"]
        L8 --> L10{"需要 Flush?"}
        L10 -- 是 --> L11["Sink.Flush()"]
        L10 -- 否 --> L12["返回（缓冲由 sink 自己决定）"]
    end

    subgraph ROT["RotatingFileSink 的轮转"]
        R1["写之前检查当前文件大小"] --> R2{"超过阈值?"}
        R2 -- 否 --> R3["直接写"]
        R2 -- 是 --> R4["关闭当前文件 -> 重命名为 .1/.2/..."]
        R4 --> R5["打开新文件继续写"]
        R5 --> R3
    end

    subgraph CFG["配置装载与生效"]
        C1["IniFile 解析文本 -> (section,key,value)"] --> C2["Config 按名字找到 ConfigVarBase"]
        C2 --> C3["ConfigVar::FromString(value)<br/>内部用 ValueCodec 解码"]
        C3 --> C4{"解码成功?"}
        C4 -- 否 --> C5["保留旧值并报错（**不静默丢配置**）"]
        C4 -- 是 --> C6["更新值并**通知所有监听者**"]
        C6 --> C7["监听者通常是各模块的「重新配置」钩子"]
    end

    LOG --> ROT
    LOG --> CFG
```

**要点**

- **过滤在格式化之前**：被过滤掉的日志**不做字符串格式化**，这是最主要的热路径优化；
- 每个 `FileSink` 用**自己的互斥锁**保护流对象，所以多个不同的 sink 之间不互相阻塞；
- `RotatingFileSink` 在**写之前**检查大小（而不是写完再检查），保证单个文件不超阈值太多；
- `ConfigVar` 的值变化会**通知监听者**，所以"改了配置立即生效"不需要轮询；
- **解码失败保留旧值并报错**，绝不静默丢弃配置 —— 这类"静默降级"是配置系统最常见的事故来源。

## ③ 向上层提供了什么

```mermaid
graph LR
    subgraph G8API["横切能力"]
        A1["log：Logger/LoggerManager + 可插拔 Sink/Filter/Formatter"]
        A2["config：IniFile + 类型安全 ConfigVar + 监听器 + 运行时快照"]
    end
    subgraph USE["谁在用"]
        U1["几乎所有模块：打日志"]
        U2["启动期：从 INI 装载配置"]
        U3["运行期：改配置 -> 监听者立即生效"]
        U4["调试模块：示例与测试用 MemorySink 做断言；<br/>不依赖具体 sink"]
    end
    A1 --> U1
    A1 --> U4
    A2 --> U2
    A2 --> U3
```

## ④ 状态流转

### 一个 Logger 的配置状态

```mermaid
stateDiagram-v2
    [*] --> kUnconfigured : LoggerManager 创建
    kUnconfigured --> kReady : 设置 sink/filter/formatter
    kReady --> kReady : SetSink / SetFilter / SetFormatter（运行期替换）
    kReady --> kSwitching : 正在替换 sink（持锁）
    kSwitching --> kReady : 替换完成（旧 sink 引用释放）
    kReady --> [*] : 进程退出（LoggerManager 析构）

    note right of kUnconfigured
        未配置时通常落到一个默认 sink
        （否则日志会静默丢失）
    end note
```

### ConfigVar 的生命周期

```mermaid
stateDiagram-v2
    [*] --> kDefault : 构造（有默认值）
    kDefault --> kLoaded : 从 INI 成功解码
    kDefault --> kLoadFailed : 解码失败（**保留默认值并报错**）
    kLoaded --> kLoaded : 再次装载 / 监听者触发重新配置
    kLoadFailed --> kLoaded : 修正后重新装载
    kLoaded --> [*]
    kLoadFailed --> [*]
```

## 与 Go 的差异

| 项 | Go | Go2Cpp |
|---|---|---|
| 日志 | `log` / `slog`（Handler 接口） | `LogSink`/`LogFilter`/`LogFormatter` **三个独立抽象** |
| 轮转 | 第三方（`lumberjack`） | **内置** `RotatingFileSink` |
| 测试用 sink | 需自己实现 | **内置** `MemorySink` |
| 配置 | 第三方（viper 等） | **内置** `IniFile` + 类型安全 `ConfigVar<T>` + 监听器 |
| 配置错误 | 由库决定 | **保留旧值并报错，不静默降级** |
