# Go2Cpp 配置

根目录的 `go2cpp.ini` 是可复制的默认模板。解析器支持 `[section]`、`key = value`、
`#` 和 `;` 注释，不依赖 YAML。调用 `LoadRuntimeConfig` 后必须通过统一校验，校验
失败应记录错误并停止启动调度器。

调度器配置重点：`processor_count=0` 使用硬件并发数；`min_workers=0` 和
`max_workers=0` 使用调度器自身的按需策略；显式 `max_workers` 不能超过 32，
`min_workers` 不能小于 1（0 表示自动）。`local_queue_limit` 必须大于 0。

日志配置重点：`level` 默认 `warn`，`stdout` 默认 `false`，`directory` 默认 `log`，
`file` 默认 `go2cpp.log`。程序中的典型启动顺序是：

```cpp
go2cpp::config::RuntimeConfig config;
std::string error;
if (!go2cpp::config::LoadRuntimeConfig("go2cpp.ini", &config, &error) ||
    !config.ApplyLogging(&error)) {
    // 配置错误时不要启动业务 Scheduler
}
```

配置中心提供类似 Sylar `ConfigVar` 的强类型变量和监听器：

```cpp
auto& registry = go2cpp::config::Config::Instance();
auto workers = registry.Lookup<std::size_t>("scheduler.max_workers", 0,
                                             "最大 M 数量");
workers->AddListener([](const auto&, const auto& current) {
    // 在线刷新业务侧参数
});
registry.StartWatcher("go2cpp.ini", std::chrono::milliseconds(500));
```

`Config::LoadFromFile` 会先校验所有已注册变量的类型，再统一提交变更；监听器只
在值真正改变后触发。`BindLoggingConfig` 将 `log.*` 变量绑定到 LoggerManager，
因此热加载会更新已经存在的 Logger。`BindRuntimeConfig` 可以把调度参数绑定到
一个 `RuntimeConfig` 对象；已经启动的 Scheduler 对 P/M 队列结构参数仍建议重新
创建，避免在线改变队列拓扑。
