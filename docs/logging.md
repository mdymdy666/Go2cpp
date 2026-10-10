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

## 自定义格式项

`LogFormatter::Item` 是格式器的插件边界。业务模块可以继承 `Item`，只负责把一个
字段追加到输出流，再用 `AddFormat`（或兼容 Sylar 的 `addFormat`）注册工厂：

```cpp
class UserItem final : public GO2CPP::log::LogFormatter::Item {
public:
    // 也可以覆写大写 Format；覆写小写 format 可兼容 Sylar 风格代码。
    void format(std::ostream& stream,
                const GO2CPP::log::LogRecord& record) const override {
        stream << "request=" << record.logger;
    }
};

GO2CPP::log::LogFormatter::AddFormat(
    "g", [](const std::string&) {
        return std::make_shared<UserItem>();
    });

GO2CPP::log::PatternFormatter formatter("%g %m%n");
```

若自定义 Item 有 `Item(const std::string&)` 构造函数，也可以使用更短的模板接口：

```cpp
struct TagItem final : GO2CPP::log::LogFormatter::Item {
    explicit TagItem(std::string option) : option_(std::move(option)) {}
    void Format(std::ostream& out, const GO2CPP::log::LogRecord&) const override {
        out << "tag=" << option_;
    }
    std::string option_;
};
GO2CPP::log::LogFormatter::addFormat<TagItem>("tag");
GO2CPP::log::PatternFormatter formatter2("{tag}");
```

`AddFormat`、`RemoveFormat` 和 `HasFormat` 都受同一注册表互斥保护；格式工厂会在
释放注册表锁后执行，因此工厂内部可以安全地加载其他插件。一个 Formatter 在构造
时固定 Item 链，之后注销格式项不会影响已经创建的 Formatter。未知格式项会原样
保留并将 `HasError()` 置为 `true`，便于配置加载阶段拒绝拼写错误。

只需要输出固定内容时，可以注册无参数工厂：

```cpp
GO2CPP::log::LogFormatter::addFormat("g", [] {
    return std::make_shared<UserItem>();
});
```

`%d{...}` 是内置日期格式项，不能被业务注册覆盖；业务格式项应选择未占用的单字符
或命名键。带选项的插件使用 `ItemFactory` 接收 `%g{option}` 中的 `option` 文本。

注册表按名称区分格式项，重复注册会返回 `false`，不会静默覆盖其他模块的实现。
格式项在 `PatternFormatter` 构造时实例化，后续每条日志只执行 `Item::Format`，不会
在日志热路径再次访问注册表。使用完毕可调用 `RemoveFormat("g")`；已创建的
`PatternFormatter` 保留自己的 Item 快照，不会被注销操作破坏。

动态配置示例：

```cpp
auto& config = go2cpp::config::Config::Instance();
go2cpp::config::BindLoggingConfig(config);
config.StartWatcher("go2cpp.ini", std::chrono::milliseconds(500));
```

修改配置文件后，已创建的 Logger 会即时更新日志级别、格式、文件和 stdout 输出。
