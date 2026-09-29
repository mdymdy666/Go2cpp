# Go2Cpp 日志模块

日志模块是独立的 C++17 目标 `go2cpp_log`，默认不依赖 spdlog、fmt 或其他
第三方库。它把一次日志按 `LogFilter -> LogItemWorker -> LogFormatter ->
LogSink` 处理，调用方可以替换任一层。

发布默认级别是 `warn`，默认写入当前进程工作目录下的 `log/go2cpp.log`，不写
stdout。将 `[log] stdout = true` 写入配置后才会增加 stdout 输出；将级别改为
`info` 或 `debug` 才会放行对应消息。

最短用法如下：

```cpp
#include "go2cpp/log.hpp"

static GO2CPP::Logger::ptr g_logger = GO2CPP_LOG_NAME("SchedulerModule");

void report() {
    GO2CPP_LOG_INFO(g_logger) << "只有 log.level=info 时才输出";
    GO2CPP_LOG_WARN(g_logger) << "发布默认会写入 log/go2cpp.log";
}
```

`PatternFormatter` 支持 `{time}`、`{level}`、`{logger}`、`{thread}`、`{fiber}`、
`{file}`、`{line}`、`{function}`、`{message}`。接入 spdlog/fmt 时使用
`CallbackSink`，在回调中调用第三方库即可；Go2Cpp 不会替用户绑定第三方 ABI。

配置应在第一次调用 `GO2CPP_LOG_NAME` 前加载，这样新建 Logger 会使用新的目录、
文件和格式。已经创建的 Logger 会立即更新等级；如需切换输出目标，建议重新配置
后再创建模块 Logger。

格式器同时兼容 Sylar 常用的 `%d{%Y-%m-%d %H:%M:%S}%T%N%T[%p]%T%f:%l-%T%m%n`
标记。模块还提供 `RotatingFileSink`、`StderrSink` 和 `MemorySink`；一个 Logger 可
挂载多个 Worker，每个 Worker 独立设置 Filter、Formatter、Sink，并可将记录传播到
父 Logger。

动态配置示例：

```cpp
auto& config = go2cpp::config::Config::Instance();
go2cpp::config::BindLoggingConfig(config);
config.StartWatcher("go2cpp.ini", std::chrono::milliseconds(500));
```

修改配置文件后，已创建的 Logger 会即时更新日志级别、格式、文件和 stdout 输出。
