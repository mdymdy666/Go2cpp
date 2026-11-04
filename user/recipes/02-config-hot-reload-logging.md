# 配置热更新驱动日志级别

> 组合：`config`（INI + ConfigVar + 配置中心）+ `logging`（LoggerManager 重新配置）。
> 目标：改 `go2cpp.ini` 后不重启进程，日志级别/输出目标在秒级生效，且不出现「改一半」。

## 场景

`config_demo` 演示解析与强类型读写，`log_config_demo` 演示从 INI 加载日志配置——但两者
各自跑一次。真实服务需要在运行中把「配置变更」翻译成「logger 重配」，并且保证：
失败时保留旧配置、通知时不重入、监听者读到的是完整快照。

## 组合方式

```
go2cpp.ini ──watch 线程──► Config::LoadFromIni
                               │ 两阶段提交（预检 → 写入 → 通知）
                               ▼
                    ConfigVar<log.level> 变更回调
                               │ 只投递（不直接改 logger）
                               ▼
                     主循环 / 专用线程：LoggerManager::Configure(...)
                               │
                               ▼
                    新的 Filter/Sink 生效（旧 logger 复用）
```

| 模块 | 负责 | 不负责 |
| --- | --- | --- |
| `config` | 解析、类型校验、事务性写入、变更通知 | 如何应用日志配置 |
| `logging` | 级别过滤、sink 生命周期、线程安全 | 读文件、监听变更 |

## 最小实现

参考 [`../modules/config/config_center_demo.cpp`](../modules/config/config_center_demo.cpp)
（注册默认值 + INI 覆盖 + 两阶段提交）与
[`../modules/logging/log_config_demo.cpp`](../modules/logging/log_config_demo.cpp)
（从 `go2cpp.ini` 加载日志配置）。

关键代码（回调只投递，应用在主循环）：

```cpp
auto& center = go2cpp::config::Config::Instance();
auto level = center.Lookup<std::string>("log.level", "info", "日志级别");
level->AddListener([&](const std::string&) {
    reload_requested.store(true);          // 只置标志，不在监听线程里改 logger
});

// 主循环里：
if (reload_requested.exchange(false)) {
    go2cpp::log::LoggerManager::Instance().Configure(/* 用最新快照重建 */);
}
```

> 片段是骨架；`LoggerManager::Configure` 的参数以 `log_config_demo.cpp` 的实际调用为准。

## 验证

```sh
# 1) 启动服务，确认当前级别
./build/user/app/go2cpp_web_server_simple 28080 &

# 2) 改 ini 里的 log.level = debug，保存
# 3) 观察：不重启进程，debug 日志开始出现（或停止）
# 4) 改成一个非法值（如 log.level = verbose），确认旧级别保持不变并打印告警
```

失败路径重点验证「非法值不生效、旧配置不丢」——这是两阶段提交的核心保证。

## 常见坑

### 1. 在监听回调里直接改 logger
- **现象**：偶发死锁或日志系统短暂静默。
- **原因**：通知在**监听线程**上回调，`LoggerManager::Configure` 会拿内部锁，和正在写日志的
  worker 线程形成锁顺序反转。
- **处理**：回调只置标志/入队，应用配置放到主循环或专用线程。

### 2. 缓存了旧的 `RuntimeConfig` / 旧 `Logger` 引用
- **现象**：配置显示已更新，实际输出没变。
- **原因**：`RuntimeConfig` 是构造时快照；`Logger` 句柄在重配后可能已被替换。
- **处理**：每次重载重新构造快照；logger 通过 `LoggerManager` 按名字取，不长期持有裸指针。

### 3. 改一半的配置被应用
- **现象**：一批配置里部分生效、部分没生效。
- **原因**：逐项 `SetValue` 而不是走 `LoadFromIni` 的两阶段提交，中途失败留下中间态。
- **处理**：批量变更统一用 `Config::LoadFromIni`；失败时整批回滚，监听者按最终快照重建。

## 相关文档

- [config 模块手册](../modules/config/README.md) · [deep/10-observability.md](../deep/10-observability.md)
- [logging 模块手册](../modules/logging/README.md) · [deep/10-observability.md](../deep/10-observability.md)
