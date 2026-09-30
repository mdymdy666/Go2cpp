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

新手也可以用一个调用完成首次加载、日志绑定和热更新：

```cpp
auto& config = go2cpp::config::Config::Instance();
std::string error;
if (!go2cpp::config::LoadAndWatch(config, "go2cpp.ini",
                                  std::chrono::milliseconds(500), &error)) {
    // error 中包含解析、类型转换或文件监听失败原因
}
```

配置变量的普通读取不再持有配置锁；刷新线程以不可变快照发布新值，
因此业务线程可以直接调用 `GetValue()`。监听器仍按注册顺序串行执行，
监听器中不应长时间阻塞或再次等待业务锁。

`Config::LoadFromFile` 会先校验所有已注册变量的类型，再统一提交变更；监听器只
在值真正改变后触发。`BindLoggingConfig` 将 `log.*` 变量绑定到 LoggerManager，
因此热加载会更新已经存在的 Logger。`BindRuntimeConfig` 可以把调度参数绑定到
一个 `RuntimeConfig` 对象；已经启动的 Scheduler 对 P/M 队列结构参数仍建议重新
创建，避免在线改变队列拓扑。

`RuntimeConfig` 由调用方持有时，必须保证它的生命周期覆盖 watcher 的整个运行期；
停止 watcher 后再销毁该对象。通常每个配置中心只调用一次 `BindLoggingConfig` 和
`BindRuntimeConfig`，避免重复注册监听器。不要在 watcher 自己的监听器回调中再次
调用 `StartWatcher` 或 `StopWatcher`，需要重启时应由其他管理线程执行。
`BindRuntimeConfig` 的监听器会更新目标对象字段；如果业务线程同时直接读取这些
公开字段，调用方必须用自己的互斥量或在停用 watcher 后读取。Scheduler 已启动后
不要在线修改队列拓扑、P 数量或 M 上限。
