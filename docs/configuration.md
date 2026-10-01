# Go2Cpp 配置与热更新

根目录的 `go2cpp.ini` 是可复制的默认模板。解析器支持 `[section]`、`key = value`、
`#` 和 `;` 注释，不依赖 YAML。调用 `LoadRuntimeConfig` 后必须通过统一校验，校验
失败应记录错误并停止启动调度器。

配置文件使用简单的 `section`、`key = value` 格式。值在启动加载和热更新时都会
进行类型与范围校验；被拒绝的变量保留旧值，不会将非法参数传给调度器。配置文件
中其他已经通过校验的变量仍可能先完成更新，业务需要跨变量原子切换时应先停用
配置监听，在业务锁内调用自有配置事务。

调度器参数的正式范围如下：

| 参数 | 默认值 | 允许范围 | 说明 |
| --- | ---: | --- | --- |
| `processor_count` | `0` | `0` 或 `1..32` | P 数量；0 使用硬件并发数。 |
| `min_workers` | `0` | `0` 或 `1..32` | 最小 M 数量；0 使用运行时默认值 1。 |
| `max_workers` | `0` | `0` 或 `1..32` | 最大 M 数量；0 按 P 和阻塞区策略计算。 |
| `local_queue_limit` | `256` | `1..1048576` | 每个 P 的本地队列容量。 |
| `fiber_stack_size` | `0` | `0` 或 `16384..67108864` 字节 | Fiber 初始栈；0 使用后端默认值。 |
| `task_affinity_budget` | `4` | `0..1048576` | 同类任务优先留在最近 P 的次数预算。 |
| `fiber_bin_capacity` | `32` | `0..4096` | 每个 M 缓存的已完成 Fiber 数量。 |
| `idle_wait_ms` | `10` | `1..86400000` | 空闲 M 等待新任务的周期。 |
| `idle_worker_timeout_ms` | `250` | `1..86400000` | 空闲 M 回收等待时间。 |
| `sysmon_interval_ms` | `10` | `1..86400000` | sysmon 检查周期。 |
| `long_syscall_threshold_ms` | `50` | `1..86400000` | 已声明阻塞区的长调用阈值。 |

`allow_worker_oversubscription`、`enable_sysmon`、`pin_workers_to_cpu` 和
`collect_metrics` 接受 `true/false`、`yes/no`、`on/off` 或 `1/0`。其中
`collect_metrics=false` 只关闭累计指标，不改变调度语义；`fiber_bin_capacity`
只影响对象复用，不改变 Fiber 生命周期和执行顺序。

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

`RuntimeConfig` 由调用方持有时，必须保证它的生命周期覆盖配置监听的整个运行期；
停止配置监听后再销毁该对象。通常每个配置中心只调用一次 `BindLoggingConfig` 和
`BindRuntimeConfig`，避免重复注册监听器。不要在配置监听自己的回调中再次
调用 `StartWatcher` 或 `StopWatcher`，需要重启时应由其他管理线程执行。
`BindRuntimeConfig` 的监听器会更新目标对象字段；如果业务线程同时直接读取这些
公开字段，调用方必须用自己的互斥量或在停用 watcher 后读取。Scheduler 已启动后
不要在线修改队列拓扑、P 数量或 M 上限。
