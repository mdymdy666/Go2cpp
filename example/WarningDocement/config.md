# config 使用限制

> 依据：`example/config/` 示例 + 实际头/实现；每条标题末尾给出 file:line。

## 1. `IniFile` 只做文本解析 —— `include/go2cpp/config/config.hpp:69`
不负责类型转换、范围校验，也不负责线程同步。
```cpp
const std::string raw = ini.Get("s", "port", "0");   // 仍是字符串
```
**后果**：直接信任内容 → 类型错误/越界值进业务。
## 2. `Config` 是进程级单例 + 事务锁 —— `include/go2cpp/config/config.hpp:503-528`
`Instance()` 是函数内静态对象；整批发布由可重入事务锁串行化，发布期间其他线程提交被阻塞。
```cpp
auto& cfg = Config::Instance();   // 全进程共享，示例键加 "demo." 前缀
```
**后果**：在监听器里再提交同批配置会拖住其他线程；跨测试复用会串值。
## 3. 监听器异常被吞，校验器异常会外传 —— `include/go2cpp/config/config.hpp:301,435,471`
提交/回滚路径捕获监听器异常；但 `Validator` 调用点没有 try/catch，校验器抛异常会直穿 `SetValue`/`LoadFromIni`。
```cpp
if (validator && !validator(value, error)) return false;   // 抛异常不在此兜底
```
**后果**：监听器失败被静默吞掉（只能靠日志）；校验器抛异常则绕过 false 语义。
## 4. 校验器拒绝后旧值不变 —— `example/config/config_center_full.cpp:192-193`
`LoadFromIni` 返回 false 且当前值保持原样，这是整批事务语义。
```cpp
if (!config.LoadFromIni(ini, &error)) { /* 值仍是旧值，不要手动回滚 */ }
```
**后果**：手动再"恢复"会与并发提交竞争，可能覆盖他人写入。
## 5. 热重载依赖文件存在 + 必须 StopWatcher —— `example/config/config_center_full.cpp:218-232`、`include/go2cpp/config/config.hpp:618`
空文件/不存在才读取，失败不启动线程；`Watching()` 可查询，析构会 Stop。
```cpp
config.StartWatcher("/tmp/app.ini", Ms(50), &error);
config.StopWatcher();   // 退出前显式停
```
**后果**：路径写错只会静默不重载；不停会留下后台线程。
## 6. `RuntimeConfig::FromIni` 是事务性的 —— `example/config/runtime_config_full.cpp:10`
失败时不修改调用方 config；空 INI 得到全默认快照。
```cpp
RuntimeConfig cfg;
if (!RuntimeConfig::FromIni(ini, &cfg, &error)) { /* cfg 保持原值 */ }
```
**后果**：误以为部分字段已生效。
## 7. 已知缺陷：`[timer]` 段读不到 —— `example/config/runtime_config_full.cpp:13-15,205-228`
`[timer]` 用 `ReadSize` 读取，而 ReadSize 对非 scheduler 段必定 false。
```cpp
// 任何含 [timer] 的 INI 都可能被 FromIni 拒绝
```
**后果**：带 `[timer]` 的配置整体加载失败（回归验证见该文件场景 7）。
## 8. 求值顺序陷阱 —— `example/config/config_demo.cpp:52-54`
同一 `<<` 链里先 `GetValue()` 再 `FromString()` 会打印改后值。
```cpp
const int before = workers.GetValue();   // 先取值，再改
const bool ok = workers.FromString("16");
```
**后果**：日志/断言看到的是新值，误判往返正确。
## 9. `ApplyLogging` 有全局副作用 —— `example/config/runtime_config_full.cpp:17-19`
它改全局 LoggerManager 并可能创建日志目录，不适合放进可反复运行的示例。
```cpp
// 示例只用 FromIni + Validate；ApplyLogging 留给真正启动流程
```
**后果**：测试间互相污染日志配置与磁盘。
