# logging 使用限制

> 依据：`example/logging/` 示例 + 实际头/实现；每条标题末尾给出 file:line。

## 1. 自定义格式项的键必须是单字符 —— `example/logging/log_full.cpp:140-142`
`%x` 只消费一个字符；写成 `"zz"` 会被解析成 `%z` + 字面 `z`。
```cpp
LogFormatter::AddFormat("g", factory);   // 不能是 "zz"
```
**后果**：渲染结果原样输出，看起来像「格式项没生效」。
## 2. 多数单字母键已被内置项占用 —— `example/logging/log_full.cpp:137-139`
实测 `T` 是制表符、`F` 是 fiber_id；对这些键 `AddFormat` 因重名失败。
```cpp
if (LogFormatter::HasFormat(k)) continue;   // 先探测空闲键
```
**后果**：硬编码键名注册失败，格式里出现字面 `%T`。
## 3. 必须先注册格式项，再构造 Formatter —— `example/logging/log_full.cpp:178`、`example/logging/log_config_demo.cpp:35`
`PatternFormatter` 构造时保存格式项快照，之后再注册不会生效。
```cpp
LogFormatter::AddFormat("g", factory);
PatternFormatter fmt("%g %m%n");   // 顺序反了看不到 g
```
**后果**：运行期注册无效，只能重建 Formatter。
## 4. 非法模式不报错 —— `example/logging/log_full.cpp:123-124`
实测 `PatternFormatter("%")` 的 `HasError()` 为假，`%` 当字面量处理。
```cpp
PatternFormatter p("%");   // HasError() 仍为 false
```
**后果**：拼错模式不会失败，只会输出意外内容。
## 5. `LoggerManager` 是全局单例，默认不输出 Info —— `example/logging/log_config_demo.cpp:44-45,77`
配置缺失时用默认安全值；声明式 `GO2CPP_LOG_INFO` 默认看不到。
```cpp
LoggerManager::Instance().Configure(LogLevel::Info, true, "log", "app.log");
```
**后果**：以为日志没生效，其实是级别/输出目标没开。
## 6. 配置文件缺失不是崩溃 —— `example/logging/log_config_demo.cpp:38-47`
`LoadRuntimeConfig` 失败只代表没有可选配置，不应终止进程。
```cpp
if (!LoadRuntimeConfig("go2cpp.ini", &cfg, &error)) { /* 用默认值 */ }
```
**后果**：把「没配置文件」当致命错误 → 误报。
## 7. 退出前必须 Flush —— `example/logging/log_config_demo.cpp:78`
异步 sink 有缓冲，进程直接退出会丢尾部日志。
```cpp
LoggerManager::Instance().Flush();
```
**后果**：崩溃前最后几条日志缺失。
