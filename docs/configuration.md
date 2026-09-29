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
